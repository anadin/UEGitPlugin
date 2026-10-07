// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#include "GitWorkspaceSaveFlow.h"
#include "Editor.h"
#include "Engine/World.h"
#include "Engine/Level.h"
#include "Engine/MapBuildDataRegistry.h"
#include "Engine/LODActor.h"
#include "WorldPartition/HLOD/HLODActor.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Misc/SecureHash.h"
#include "UObject/Package.h"
#include "UObject/PackageFileSummary.h"
#include "UObject/SavePackage.h"
#include "UObject/StrongObjectPtr.h"

namespace GitWorkspaceSave
{
namespace
{
bool ExternalName(const FString& Name)
{ return Name.Contains(TEXT("/__ExternalActors__/")) || Name.Contains(TEXT("/__ExternalObjects__/")); }
bool ExternalWorld(UWorld* World)
{
    return World && World->PersistentLevel && (World->GetWorldPartition() || World->PersistentLevel->IsUsingExternalActors() ||
        World->PersistentLevel->IsUsingExternalObjects() || !World->GetPackage()->GetExternalPackages().IsEmpty());
}
FString FileHash(const FString& Filename)
{
    const auto Hash = FMD5Hash::HashFile(*Filename);
    return Hash.IsValid() ? LexToString(Hash) : FString();
}
FString ExistingWorldError(UWorld* World, const FString& Root, const FString& Content, FString& Filename)
{
    if (!World || World->GetClass() != UWorld::StaticClass() || !World->PersistentLevel ||
        (World->WorldType != EWorldType::Editor && World->WorldType != EWorldType::Inactive) ||
        World->GetCurrentLevel() != World->PersistentLevel || World->GetLevels().Num() != 1 ||
        !World->GetStreamingLevels().IsEmpty() || World->WorldComposition)
        return TEXT("External actor saves currently support one persistent editor map. Stop Play/Simulate; sublevels and level instances need a separate save adapter.");
    UPackage* Map = World->GetPackage();
    if (FPackageName::IsTempPackage(Map->GetName()) || Map->HasAnyPackageFlags(PKG_NewlyCreated) ||
        !FPackageName::DoesPackageExist(Map->GetName(), &Filename))
        return TEXT("First saves and Save As for World Partition / OFPA maps are not supported yet. Existing actor edits require an already saved map.");
    Filename = FPaths::ConvertRelativePathToFull(Filename);
    if (!FPaths::IsUnderDirectory(Filename, Content) || !FPaths::IsUnderDirectory(Filename, Root))
        return TEXT("The owning map must be an existing map in this repository's game Content folder.");
    TUniquePtr<FArchive> Reader(IFileManager::Get().CreateFileReader(*Filename)); uint32 Tag = 0;
    if (Reader && Reader->TotalSize() >= sizeof(Tag)) *Reader << Tag;
    if (!Reader || Reader->IsError() || Tag != PACKAGE_FILE_TAG)
        return TEXT("Hydrate the owning map before saving its actors. Its saved file is not a readable Unreal package.");
    if (Map->IsDirty() || (World->PersistentLevel->MapBuildData && World->PersistentLevel->MapBuildData->GetPackage()->IsDirty()))
        return TEXT("This save supports existing actor edits with a clean owning map and clean build data. Dirty map/build-data writes need the next coordinated save adapter. Unsaved edits remain in the editor.");
    return FString();
}
FString RelativeFile(const FString& File, const FString& Root)
{ FString Path = File; FPaths::MakePathRelativeTo(Path, *(Root + TEXT("/"))); return Path; }
TArray<UPackage*> Sources(const FPackageSavePaths& Plan)
{ TArray<UPackage*> Out; for (const auto& Package : Plan.Sources) { if (!Package.IsValid()) return {}; Out.Add(Package.Get()); } return Out; }
FString PlanDrift(const FPackageSavePaths& Plan, const FPackageSavePaths& Current, const TSet<FString>& Saved)
{
    if (!Current.Error.IsEmpty()) return Current.Error;
    if (!Current.bCoordinatedActors) return TEXT("The save destination set changed after review.");
    TArray<FString> ExpectedNew, ActualNew;
    for (const auto& Path : Plan.NewPaths) if (!Saved.Contains(Path)) ExpectedNew.Add(Path);
    for (const auto& Path : Current.NewPaths) if (!Saved.Contains(Path)) ActualNew.Add(Path);
    if (ExpectedNew != ActualNew) return TEXT("The first-save destination set changed after review.");
    TArray<FString> Expected, Actual;
    for (const auto& Path : Plan.Paths) if (!Saved.Contains(Path)) Expected.Add(Path);
    for (const auto& Path : Current.Paths) if (!Saved.Contains(Path)) Actual.Add(Path);
    if (Actual != Expected) return TEXT("Dirty packages changed after review. Review the complete actor save again.");
    for (const auto& Entry : Plan.Entries)
    {
        if (Saved.Contains(Entry.Path)) continue;
        const auto* Now = Current.Entries.FindByPredicate([&](const FPackageSavePaths::FEntry& E) { return E.Path == Entry.Path; });
        if (!Now || Now->Package != Entry.Package || Now->PackageName != Entry.PackageName || Now->Filename != Entry.Filename ||
            Now->Actor != Entry.Actor || Now->ActorGuid != Entry.ActorGuid || Now->World != Entry.World || Now->WorldName != Entry.WorldName ||
            Now->WorldFilename != Entry.WorldFilename || Now->WorldHash != Entry.WorldHash)
            return TEXT("A reviewed package or its owning map changed before writing: ") + Entry.Path;
    }
    return FString();
}
}
FPackageSavePaths GatherPackageSavePaths(const TArray<UPackage*>& Packages, const FString& Root, const FString& Content)
{
    check(IsInGameThread());
    TArray<UWorld*> Worlds; TArray<UPackage*> Ordinary;
    FPackageSavePaths Out;
    for (UPackage* Package : Packages)
    {
        if (!Package) continue;
        if (UWorld* World = UWorld::FindWorldInPackage(Package); ExternalWorld(World)) Worlds.AddUnique(World);
        else if (ExternalName(Package->GetName()))
        {
            AActor* Actor = AActor::FindActorInPackage(Package);
            if (!Actor || !Actor->GetLevel())
            { Out.Error = TEXT("External-object and deleted/empty actor packages are not supported yet. No files were written or deleted."); return Out; }
            Worlds.AddUnique(Actor->GetLevel()->GetWorld());
        }
        else Ordinary.AddUnique(Package);
    }
    if (Worlds.IsEmpty()) return GatherOrdinaryPackageSavePaths(Ordinary, Root, Content);
    Out.bCoordinatedActors = true;
    for (UPackage* Package : Ordinary) Out.Sources.Add(Package);
    for (UWorld* World : Worlds)
    {
        FString MapFile; Out.Error = ExistingWorldError(World, Root, Content, MapFile); if (!Out.Error.IsEmpty()) return Out;
        const FString Hash = FileHash(MapFile);
        if (Hash.IsEmpty()) { Out.Error = TEXT("Cannot read the owning map before reviewing actor saves."); return Out; }
        Out.Sources.Add(World->GetPackage());
        // This list includes deleted/empty packages. Never pass it to FileHelpers,
        // which can delete them without consulting IsPackageOKToSaveDelegate.
        for (UPackage* Package : World->PersistentLevel->GetLoadedExternalObjectPackages())
        {
            if (!Package || (!Package->IsDirty() && !UPackage::IsEmptyPackage(Package))) continue;
            FString Filename;
            if (Package->HasAnyPackageFlags(PKG_NewlyCreated) || UPackage::IsEmptyPackage(Package) ||
                !FPackageName::DoesPackageExist(Package->GetName(), &Filename))
            { Out.Error = TEXT("New or deleted/empty external packages require first-save/deletion recovery support. No packages were saved or deleted: ") + Package->GetName(); return Out; }
            Filename = FPaths::ConvertRelativePathToFull(Filename);
            if (!FPaths::IsUnderDirectory(Filename, Content) || !FPaths::IsUnderDirectory(Filename, Root))
            { Out.Error = TEXT("An external package is outside the owning repository's Content folder."); return Out; }
            FPackageSavePaths::FEntry Entry;
            Entry.Package = Package; Entry.PackageName = Package->GetName(); Entry.Path = RelativeFile(Filename, Root); Entry.Filename = Filename;
            Entry.World = World; Entry.WorldName = World->GetPackage()->GetName(); Entry.WorldFilename = MapFile; Entry.WorldHash = Hash;
            Entry.Actor = AActor::FindActorInPackage(Package); if (Entry.Actor.IsValid()) Entry.ActorGuid = Entry.Actor->GetActorGuid();
            Out.Entries.Add(Entry); Out.Paths.AddUnique(Entry.Path); Out.ExternalActorPaths.AddUnique(Entry.Path);
            Out.Error = ValidateExternalActorBinding(Out, Entry.Path, Package); if (!Out.Error.IsEmpty()) return Out;
        }
    }
    // Save All may also contain ordinary assets. Keep those in the exact write
    // set; their native command must not rediscover external deletions afterward.
    const auto Assets = GatherOrdinaryPackageSavePaths(Ordinary, Root, Content);
    if (!Assets.Error.IsEmpty()) { Out.Error = Assets.Error; return Out; }
    Out.NewPaths = Assets.NewPaths;
    for (const FString& Path : Assets.Paths)
    {
        const FString Filename = FPaths::Combine(Root, Path); FString Name;
        UPackage* Package = FPackageName::TryConvertFilenameToLongPackageName(Filename, Name) ? FindPackage(nullptr, *Name) : nullptr;
        if (!Package || UWorld::FindWorldInPackage(Package) || UPackage::IsEmptyPackage(Package) || !Package->GetExternalPackages().IsEmpty())
        { Out.Error = TEXT("Save ordinary maps separately before an external-actor batch. Empty packages and assets with external dependencies need separate adapters."); return Out; }
        FPackageSavePaths::FEntry Entry; Entry.Package = Package; Entry.PackageName = Name; Entry.Path = Path; Entry.Filename = Filename;
        Out.Entries.Add(Entry); Out.Paths.AddUnique(Path);
    }
    Out.Paths.Sort(); Out.NewPaths.Sort(); Out.ExternalActorPaths.Sort();
    Out.Entries.Sort([](const FPackageSavePaths::FEntry& A, const FPackageSavePaths::FEntry& B) { return A.Path < B.Path; });
    return Out;
}
FString ValidateExternalActorBinding(const FPackageSavePaths& Plan, const FString& Path, UPackage* Package)
{
    check(IsInGameThread());
    const auto* Entry = Plan.Entries.FindByPredicate([&](const FPackageSavePaths::FEntry& E) { return E.Path == Path; });
    if (!Plan.ExternalActorPaths.Contains(Path) || !Entry || !Package || Entry->Package.Get() != Package || Package->GetName() != Entry->PackageName ||
        !Entry->World.IsValid() || !Entry->Actor.IsValid() || UPackage::IsEmptyPackage(Package) || Package->HasAnyPackageFlags(PKG_NewlyCreated))
        return TEXT("The external actor/package no longer matches this prepared save.");
    UWorld* World = Entry->World.Get(); AActor* Actor = Entry->Actor.Get();
    if (!World->PersistentLevel || World->GetPackage()->GetName() != Entry->WorldName || World->GetPackage()->IsDirty() ||
        (World->PersistentLevel->MapBuildData && World->PersistentLevel->MapBuildData->GetPackage()->IsDirty()) ||
        Actor->GetLevel() != World->PersistentLevel || Actor->GetOuter() != World->PersistentLevel ||
        World->PersistentLevel->GetPackage() != World->GetPackage() || Actor->GetExternalPackage() != Package ||
        !Actor->IsMainPackageActor() || Actor->HasAnyFlags(RF_Transient | RF_ClassDefaultObject | RF_ArchetypeObject) ||
        Actor->IsA<ALODActor>() || Actor->IsA<AWorldPartitionHLOD>() || !Entry->ActorGuid.IsValid() || Actor->GetActorGuid() != Entry->ActorGuid ||
        !World->PersistentLevel->Actors.Contains(Actor) || !Package->GetName().Contains(TEXT("/__ExternalActors__/")) ||
        ULevel::GetActorPackageName(World->GetPackage(), World->PersistentLevel->GetActorPackagingScheme(), Actor->GetPathName(), Actor) != Package->GetName())
        return TEXT("Only existing main actors belonging to this exact persistent map can be saved. External objects, HLOD and nested-container paths need separate adapters.");
    FString Filename;
    if (!FPackageName::DoesPackageExist(Package->GetName(), &Filename) || FPaths::ConvertRelativePathToFull(Filename) != Entry->Filename ||
        FileHash(Entry->WorldFilename) != Entry->WorldHash)
        return TEXT("The actor destination or owning map's saved bytes changed after review.");
    return FString();
}
#if PLATFORM_MAC
GitWorkspace::FResult WriteExternalActorSave(const FPackageSavePaths& Plan, GitWorkspace::FRepository& Repository,
    const GitWorkspace::FAssetSavePermit& Permit, const GitWorkspaceSession::FLease& Lease, const FString& Content)
{
    check(IsInGameThread());
    const FString Root = Repository.Refresh().Root;
    TSet<FString> Saved;
    auto Fail = [&](const FString& Message)
    {
        GitWorkspace::FResult R; R.Error = Message + TEXT("\nLocks remain held. Completed writes remain on disk; nothing was staged, deleted or unlocked.");
        for (const auto& Entry : Plan.Entries) R.Error += TEXT("\n") + FString(Saved.Contains(Entry.Path) ? TEXT("Saved: ") : TEXT("Not completed: ")) + Entry.Path;
        return R;
    };
    if (!Plan.Error.IsEmpty() || !Plan.bCoordinatedActors || Plan.ExternalActorPaths.IsEmpty()) return Fail(TEXT("No valid existing-actor save was reviewed."));
    const auto Input = Sources(Plan); if (Input.IsEmpty()) return Fail(TEXT("The owning package set is no longer loaded."));
    TArray<TStrongObjectPtr<UPackage>> Packages; TArray<TStrongObjectPtr<UWorld>> Worlds; TArray<TStrongObjectPtr<AActor>> Actors;
    for (const auto& Entry : Plan.Entries)
    {
        if (!Entry.Package.IsValid() || !Permit.ContainsPath(Entry.Path)) return Fail(TEXT("Every package in an actor batch needs a prepared LFS save permit: ") + Entry.Path);
        Packages.Emplace(Entry.Package.Get());
        if (Entry.Actor.IsValid()) Actors.Emplace(Entry.Actor.Get());
        if (Entry.World.IsValid() && !Worlds.ContainsByPredicate([&](const auto& W) { return W.Get() == Entry.World.Get(); })) Worlds.Emplace(Entry.World.Get());
    }
    FString Error = PlanDrift(Plan, GatherPackageSavePaths(Input, Root, Content), Saved); if (!Error.IsEmpty()) return Fail(Error);
    for (const auto& Entry : Plan.Entries) { const auto Ready = Repository.ValidateAssetSave(Permit, Entry.Path, Lease); if (!Ready.Ok()) return Fail(Ready.Error); }
    FPreparedScope Prepared(Repository, Permit, Lease, Root, &Plan);
    ON_SCOPE_EXIT { Prepared.ExpectActorBatchWrite(FString()); for (const auto& World : Worlds) FEditorDelegates::PostSaveExternalActors.Broadcast(World.Get()); };
    for (const auto& World : Worlds) FEditorDelegates::PreSaveExternalActors.Broadcast(World.Get());
    // Native external saves use RF_Standalone and no asset root. SavePackage
    // includes the package's external exports and serializes actor descriptors.
    FSavePackageArgs Args; Args.TopLevelFlags = RF_Standalone; Args.SaveFlags = SAVE_NoError;
    for (const auto& Entry : Plan.Entries)
    {
        Error = PlanDrift(Plan, GatherPackageSavePaths(Input, Root, Content), Saved); if (!Error.IsEmpty()) return Fail(Error);
        Prepared.ExpectActorBatchWrite(Entry.Path);
        if (!UPackage::SavePackage(Entry.Package.Get(), nullptr, *Entry.Filename, Args))
        { Entry.Package->SetDirtyFlag(true); return Fail(TEXT("Package write failed or was refused: ") + Entry.Path); }
        Prepared.ExpectActorBatchWrite(FString());
        UPackage::WaitForAsyncFileWrites(); Saved.Add(Entry.Path);
    }
    GitWorkspace::FResult Result; Result.Code = 0; return Result;
}
#endif
}
