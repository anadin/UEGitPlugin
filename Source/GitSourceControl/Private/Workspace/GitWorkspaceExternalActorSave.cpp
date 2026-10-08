// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#include "GitWorkspaceSaveFlow.h"
#if PLATFORM_MAC
#include "GitWorkspaceFileGuard.h"
#endif
#include "Editor.h"
#include "DeletedObjectPlaceholder.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "WorldPartition/WorldPartitionActorDescUtils.h"
#include "Async/Async.h"
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
#include "UObject/ObjectSaveContext.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectIterator.h"

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
UWorld* BuildDataWorld(UPackage* Package)
{
    for (TObjectIterator<UWorld> It; It; ++It)
        if (ExternalWorld(*It) && It->PersistentLevel->MapBuildData && It->PersistentLevel->MapBuildData->GetPackage() == Package) return *It;
    return nullptr;
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
    if (FPackageName::IsTempPackage(Map->GetName()))
        return TEXT("Name this WP/OFPA map using Save Current Level first, then retry Save All. Save As copies of named external maps are not supported yet.");
    const bool bExists = FPackageName::DoesPackageExist(Map->GetName(), &Filename);
    const bool bFirst = Map->HasAnyPackageFlags(PKG_NewlyCreated) && !bExists;
    if (bFirst)
    {
        const FString Error = ReviewExternalFirstMapSource(World, false); if (!Error.IsEmpty()) return Error;
        if (!FPackageName::TryConvertLongPackageNameToFilename(Map->GetName(), Filename, TEXT(".umap")))
            return TEXT("Cannot resolve this named map's first-save destination.");
        if (IFileManager::Get().FileExists(*Filename) || IFileManager::Get().DirectoryExists(*Filename))
            return TEXT("The never-saved map destination is occupied; choose another name without overwriting it.");
    }
    else if (!bExists || Map->HasAnyPackageFlags(PKG_NewlyCreated))
        return TEXT("The owning map is missing or its first-save destination is occupied. Restore/hydrate a saved map; it cannot be treated as a new map.");
    Filename = FPaths::ConvertRelativePathToFull(Filename);
    if (!FPaths::IsUnderDirectory(Filename, Content) || !FPaths::IsUnderDirectory(Filename, Root))
        return TEXT("The owning map must be an existing map in this repository's game Content folder.");
    if (!bFirst)
    {
        TUniquePtr<FArchive> Reader(IFileManager::Get().CreateFileReader(*Filename)); uint32 Tag = 0;
        if (Reader && Reader->TotalSize() >= sizeof(Tag)) *Reader << Tag;
        if (!Reader || Reader->IsError() || Tag != PACKAGE_FILE_TAG)
            return TEXT("Hydrate the owning map before saving its actors. Its saved file is not a readable Unreal package.");
    }
    if (World->PersistentLevel->OwningWorld != World)
        return TEXT("The persistent level no longer belongs to this exact map.");
    if (UMapBuildDataRegistry* Data = World->PersistentLevel->MapBuildData)
        if (Data->GetClass() != UMapBuildDataRegistry::StaticClass() || Data->IsLegacyBuildData() ||
            Data->GetPackage()->GetName() != Map->GetName() + TEXT("_BuiltData") || !Data->GetPackage()->GetExternalPackages().IsEmpty())
            return TEXT("Legacy, shared, embedded or custom build data requires a separate map save adapter.");
    return FString();
}
FString DeletedActorDescription(UPackage* Package, UWorld* World, FPackageSavePaths::FEntry& Entry)
{
    if (!Package || !World || !World->PersistentLevel || !UPackage::IsEmptyPackage(Package) ||
        Package->HasAnyPackageFlags(PKG_NewlyCreated) || !Package->IsDirty())
        return TEXT("Only dirty, previously saved empty actor packages can be deleted. Never-saved actors and conversions need separate handling.");
    FString Filename;
    if (!FPackageName::DoesPackageExist(Package->GetName(), &Filename)) return TEXT("The deleted actor's saved file is missing; restore it before reviewing deletion.");
    TUniquePtr<FArchive> Reader(IFileManager::Get().CreateFileReader(*Filename)); uint32 Tag = 0;
    if (Reader && Reader->TotalSize() >= sizeof(Tag)) *Reader << Tag;
    if (!Reader || Reader->IsError() || Tag != PACKAGE_FILE_TAG) return TEXT("Hydrate the saved actor before reviewing deletion.");
    Reader.Reset();
    auto& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
    // Bind to saved metadata rather than a transient/deleted in-memory actor.
    // Prime before entering core save scope; its late validator only reads it.
    if (!UE::IsSavingPackage()) Registry.ScanFilesSynchronous({Filename}, true);
    TArray<FAssetData> Assets; Registry.GetAssetsByPackageName(FName(*Package->GetName()), Assets, true);
    if (Assets.Num() != 1) return TEXT("Cannot identify one saved main actor in this empty package. Restore/hydrate it before reviewing deletion.");
    const auto Desc = FWorldPartitionActorDescUtils::GetActorDescriptorFromAssetData(Assets[0]);
    if (!Desc || !Desc->GetGuid().IsValid() || Desc->IsChildContainerInstance() || Desc->GetExternalDataLayerAsset().IsValid() ||
        Desc->GetActorPackage() != FName(*Package->GetName()))
        return TEXT("Deletion supports main persistent-map actors only; external data layers and nested containers need separate adapters.");
    UClass* Class = nullptr;
    // Also runs during late package validation: do not resolve/load classes here.
    for (TObjectIterator<UClass> It; It; ++It) if (It->GetClassPathName() == Desc->GetNativeClass()) { Class = *It; break; }
    if (!Class || !Class->IsChildOf(AActor::StaticClass()) || Class->IsChildOf(ALODActor::StaticClass()) || Class->IsChildOf(AWorldPartitionHLOD::StaticClass()))
        return TEXT("The saved actor class is unavailable or requires a separate deletion adapter.");
    Entry.ActorPath = Desc->GetActorSoftPath().ToString(); Entry.ActorGuid = Desc->GetGuid(); Entry.ActorLabel = Desc->GetActorLabelString();
    if (Entry.ActorPath != World->PersistentLevel->GetPathName() + TEXT(".") + Desc->GetActorNameString() ||
        ULevel::GetActorPackageName(World->GetPackage(), World->PersistentLevel->GetActorPackagingScheme(), Entry.ActorPath, World->PersistentLevel) != Package->GetName())
        return TEXT("The empty actor package does not belong to this exact map's main actor namespace.");
    for (AActor* Actor : World->PersistentLevel->Actors)
        if (IsValid(Actor) && (Actor->GetActorGuid() == Entry.ActorGuid || Actor->GetPathName() == Entry.ActorPath))
            return TEXT("The actor still exists in this level. Conversion/move cleanup cannot use actor deletion.");
    return FString();
}
FString RelativeFile(const FString& File, const FString& Root)
{ FString Path = File; FPaths::MakePathRelativeTo(Path, *(Root + TEXT("/"))); return Path; }
TArray<UPackage*> Sources(const FPackageSavePaths& Plan)
{ TArray<UPackage*> Out; for (const auto& Package : Plan.Sources) { if (!Package.IsValid()) return {}; Out.Add(Package.Get()); } return Out; }
FString PlanDrift(const FPackageSavePaths& Plan, const FPackageSavePaths& Current, const TSet<FString>& Saved, const TMap<FString, FString>& SavedMaps)
{
    if (!Current.Error.IsEmpty()) return Current.Error;
    if (!Current.bCoordinatedActors) return TEXT("The save destination set changed after review.");
    if (Plan.Owners.Num() != Current.Owners.Num()) return TEXT("The owning map set changed after review.");
    for (const auto& Owner : Plan.Owners)
    {
        const auto* Now = Current.Owners.FindByPredicate([&](const auto& O) { return O.World == Owner.World; });
        const FString* SavedHash = SavedMaps.Find(Owner.Path);
        if (!Now || Now->Name != Owner.Name || Now->Filename != Owner.Filename || Now->Path != Owner.Path ||
            Now->Hash != (SavedHash ? *SavedHash : Owner.Hash) || Now->BuildData != Owner.BuildData || Now->BuildDataName != Owner.BuildDataName)
            return TEXT("The owning map or its build-data association changed after review: ") + Owner.Name;
    }
    TArray<FString> ExpectedNew, ActualNew;
    for (const auto& Path : Plan.NewPaths) if (!Saved.Contains(Path)) ExpectedNew.Add(Path);
    for (const auto& Path : Current.NewPaths) if (!Saved.Contains(Path)) ActualNew.Add(Path);
    TArray<FString> ExpectedDelete, ActualDelete;
    for (const auto& Path : Plan.DeletePaths) if (!Saved.Contains(Path)) ExpectedDelete.Add(Path);
    for (const auto& Path : Current.DeletePaths) if (!Saved.Contains(Path)) ActualDelete.Add(Path);
    if (ExpectedDelete != ActualDelete) return TEXT("The actor deletion set changed after review.");
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
            Now->WorldFilename != Entry.WorldFilename || Now->WorldHash != Entry.WorldHash || Now->Kind != Entry.Kind ||
            Now->ActorPath != Entry.ActorPath || Now->ActorLabel != Entry.ActorLabel)
            return TEXT("A reviewed package or its owning map changed before writing: ") + Entry.Path;
    }
    return FString();
}
}
bool NeedsCoordinatedWorldSave(UPackage* Package)
{ return Package && (ExternalWorld(UWorld::FindWorldInPackage(Package)) || BuildDataWorld(Package)); }
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
            UWorld* Owner = Actor && Actor->GetLevel() ? Actor->GetLevel()->GetWorld() : nullptr;
            if (!Owner && Package->GetName().Contains(TEXT("/__ExternalActors__/")))
                for (TObjectIterator<UWorld> It; It; ++It)
                    if (ExternalWorld(*It) && It->PersistentLevel->GetLoadedExternalObjectPackages().Contains(Package)) { Owner = *It; break; }
            if (!Owner) { Out.Error = TEXT("This external package has no exact loaded persistent-map owner."); return Out; }
            Worlds.AddUnique(Owner);
        }
        else if (UWorld* DataOwner = BuildDataWorld(Package)) Worlds.AddUnique(DataOwner);
        else Ordinary.AddUnique(Package);
    }
    if (Worlds.IsEmpty()) return GatherOrdinaryPackageSavePaths(Ordinary, Root, Content);
    Out.bCoordinatedActors = true;
    for (UPackage* Package : Ordinary) Out.Sources.Add(Package);
    for (UWorld* World : Worlds)
    {
        FString MapFile; Out.Error = ExistingWorldError(World, Root, Content, MapFile); if (!Out.Error.IsEmpty()) return Out;
        const bool bFirstMap = World->GetPackage()->HasAnyPackageFlags(PKG_NewlyCreated);
        const FString Hash = FileHash(MapFile);
        if (Hash.IsEmpty() && !bFirstMap) { Out.Error = TEXT("Cannot read the owning map before reviewing actor saves."); return Out; }
        Out.Sources.Add(World->GetPackage());
        FPackageSavePaths::FOwner Owner; Owner.World = World; Owner.Name = World->GetPackage()->GetName(); Owner.Filename = MapFile;
        Owner.Path = RelativeFile(MapFile, Root); Owner.Hash = Hash; Owner.BuildData = World->PersistentLevel->MapBuildData.Get();
        if (Owner.BuildData.IsValid()) Owner.BuildDataName = Owner.BuildData->GetPackage()->GetName();
        Out.Owners.Add(Owner);
        bool bSaveMap = bFirstMap || World->GetPackage()->IsDirty();
        if (UMapBuildDataRegistry* Data = World->PersistentLevel->MapBuildData)
        {
            UPackage* Package = Data->GetPackage(); FString Filename;
            const bool bExists = FPackageName::DoesPackageExist(Package->GetName(), &Filename);
            const bool bNew = !bExists || Package->HasAnyPackageFlags(PKG_NewlyCreated);
            if (bNew || Package->IsDirty())
            {
                if (!bExists && !FPackageName::TryConvertLongPackageNameToFilename(Package->GetName(), Filename, TEXT(".uasset")))
                { Out.Error = TEXT("Cannot resolve the reviewed build-data destination."); return Out; }
                Filename = FPaths::ConvertRelativePathToFull(Filename);
                if (!FPaths::IsUnderDirectory(Filename, Content) || !FPaths::IsUnderDirectory(Filename, Root))
                { Out.Error = TEXT("Build data must be in this repository's game Content folder."); return Out; }
                FPackageSavePaths::FEntry Entry; Entry.Package = Package; Entry.PackageName = Package->GetName(); Entry.Filename = Filename; Entry.Path = RelativeFile(Filename, Root);
                Entry.World = World; Entry.WorldName = Owner.Name; Entry.WorldFilename = MapFile; Entry.WorldHash = Hash; Entry.Kind = FPackageSavePaths::EKind::BuildData;
                Out.Entries.Add(Entry); Out.Paths.AddUnique(Entry.Path); if (bNew) Out.NewPaths.AddUnique(Entry.Path);
                bSaveMap |= bNew; // The map must persist the new companion relationship.
            }
        }
        if (bSaveMap)
        {
            FPackageSavePaths::FEntry Entry; Entry.Package = World->GetPackage(); Entry.PackageName = Owner.Name; Entry.Filename = MapFile; Entry.Path = Owner.Path;
            Entry.World = World; Entry.WorldName = Owner.Name; Entry.WorldFilename = MapFile; Entry.WorldHash = Hash; Entry.Kind = FPackageSavePaths::EKind::Map;
            Out.Entries.Add(Entry); Out.Paths.AddUnique(Entry.Path);
            if (bFirstMap) Out.NewPaths.AddUnique(Entry.Path);
        }
        // This list includes deleted/empty packages. Never pass it to FileHelpers,
        // which can delete them without consulting IsPackageOKToSaveDelegate.
        for (UPackage* Package : World->PersistentLevel->GetLoadedExternalObjectPackages())
        {
            if (!Package || (!Package->IsDirty() && !Package->HasAnyPackageFlags(PKG_NewlyCreated) && !UPackage::IsEmptyPackage(Package))) continue;
            FString Filename;
            const bool bDelete = UPackage::IsEmptyPackage(Package);
            if (bDelete && !Package->IsDirty() &&
                FPackageName::TryConvertLongPackageNameToFilename(Package->GetName(), Filename, TEXT(".uasset")) &&
                !IFileManager::Get().FileExists(*Filename) && !IFileManager::Get().DirectoryExists(*Filename)) continue; // Completed deletion; no disk target.
            const bool bNew = Package->HasAnyPackageFlags(PKG_NewlyCreated);
            if (bNew)
            {
                if (!FPackageName::TryConvertLongPackageNameToFilename(Package->GetName(), Filename, TEXT(".uasset")))
                { Out.Error = TEXT("Cannot resolve the new actor destination."); return Out; }
                if (IFileManager::Get().FileExists(*Filename) || IFileManager::Get().DirectoryExists(*Filename))
                { Out.Error = TEXT("A new actor destination is already occupied. No file was overwritten: ") + Package->GetName(); return Out; }
            }
            else if (!FPackageName::DoesPackageExist(Package->GetName(), &Filename))
            { Out.Error = TEXT("An existing actor file is missing. Restore or hydrate it before saving; it cannot be treated as a new actor: ") + Package->GetName(); return Out; }
            Filename = FPaths::ConvertRelativePathToFull(Filename);
            if (!FPaths::IsUnderDirectory(Filename, Content) || !FPaths::IsUnderDirectory(Filename, Root))
            { Out.Error = TEXT("An external package is outside the owning repository's Content folder."); return Out; }
            FPackageSavePaths::FEntry Entry;
            Entry.Package = Package; Entry.PackageName = Package->GetName(); Entry.Path = RelativeFile(Filename, Root); Entry.Filename = Filename;
            Entry.World = World; Entry.WorldName = World->GetPackage()->GetName(); Entry.WorldFilename = MapFile; Entry.WorldHash = Hash;
            Entry.Actor = AActor::FindActorInPackage(Package); if (Entry.Actor.IsValid()) Entry.ActorGuid = Entry.Actor->GetActorGuid();
            Entry.Kind = bDelete ? FPackageSavePaths::EKind::DeleteActor : FPackageSavePaths::EKind::Actor;
            if (bDelete)
            { Out.Error = DeletedActorDescription(Package, World, Entry); if (!Out.Error.IsEmpty()) return Out; Out.DeletePaths.AddUnique(Entry.Path); }
            Out.Entries.Add(Entry); Out.Paths.AddUnique(Entry.Path); Out.ExternalActorPaths.AddUnique(Entry.Path);
            if (bNew) Out.NewPaths.AddUnique(Entry.Path);
            Out.Error = ValidateExternalActorBinding(Out, Entry.Path, Package); if (!Out.Error.IsEmpty()) return Out;
        }
    }
    // Save All may also contain ordinary assets. Keep those in the exact write
    // set; their native command must not rediscover external deletions afterward.
    const auto Assets = GatherOrdinaryPackageSavePaths(Ordinary, Root, Content);
    if (!Assets.Error.IsEmpty()) { Out.Error = Assets.Error; return Out; }
    for (const auto& Path : Assets.NewPaths) Out.NewPaths.AddUnique(Path);
    for (const FString& Path : Assets.Paths)
    {
        const FString Filename = FPaths::Combine(Root, Path); FString Name;
        // No StaticFindObject/FindPackage: this gather also runs in the core
        // import validator while Unreal holds its saving scope.
        UPackage* Package = nullptr;
        if (FPackageName::TryConvertFilenameToLongPackageName(Filename, Name))
            if (auto* Found = Ordinary.FindByPredicate([&](UPackage* P) { return P->GetName() == Name; })) Package = *Found;
        if (!Package || UWorld::FindWorldInPackage(Package) || UPackage::IsEmptyPackage(Package) || !Package->GetExternalPackages().IsEmpty())
        { Out.Error = TEXT("Save ordinary maps separately before an external-actor batch. Empty packages and assets with external dependencies need separate adapters."); return Out; }
        FPackageSavePaths::FEntry Entry; Entry.Package = Package; Entry.PackageName = Name; Entry.Path = Path; Entry.Filename = Filename;
        Out.Entries.Add(Entry); Out.Paths.AddUnique(Path);
    }
    Out.Paths.Sort(); Out.NewPaths.Sort(); Out.ExternalActorPaths.Sort(); Out.DeletePaths.Sort();
    Out.Entries.Sort([](const FPackageSavePaths::FEntry& A, const FPackageSavePaths::FEntry& B)
    {
        auto Rank = [](FPackageSavePaths::EKind Kind) { return Kind == FPackageSavePaths::EKind::DeleteActor ? -1 : Kind == FPackageSavePaths::EKind::BuildData ? 0 : Kind == FPackageSavePaths::EKind::Actor ? 1 : Kind == FPackageSavePaths::EKind::Asset ? 2 : 3; };
        return Rank(A.Kind) == Rank(B.Kind) ? A.Path < B.Path : Rank(A.Kind) < Rank(B.Kind);
    });
    return Out;
}
FString ValidateExternalActorBinding(const FPackageSavePaths& Plan, const FString& Path, UPackage* Package)
{
    check(IsInGameThread());
    const auto* Entry = Plan.Entries.FindByPredicate([&](const FPackageSavePaths::FEntry& E) { return E.Path == Path; });
    if (Entry && Entry->Kind == FPackageSavePaths::EKind::DeleteActor)
    {
        if (!Plan.DeletePaths.Contains(Path) || !Plan.ExternalActorPaths.Contains(Path) || !Package || Entry->Package.Get() != Package ||
            Package->GetName() != Entry->PackageName || !Entry->World.IsValid()) return TEXT("The deleted actor package changed after review.");
        FPackageSavePaths::FEntry Now; const FString Error = DeletedActorDescription(Package, Entry->World.Get(), Now);
        FString Filename;
        if (!Error.IsEmpty()) return Error;
        if (Now.ActorPath != Entry->ActorPath || Now.ActorGuid != Entry->ActorGuid || Now.ActorLabel != Entry->ActorLabel ||
            !FPackageName::DoesPackageExist(Package->GetName(), &Filename) || FPaths::ConvertRelativePathToFull(Filename) != Entry->Filename ||
            FileHash(Entry->WorldFilename) != Entry->WorldHash) return TEXT("The deleted actor identity or owning map changed after review.");
        return FString();
    }
    const bool bNew = Plan.NewPaths.Contains(Path);
    if (!Plan.ExternalActorPaths.Contains(Path) || !Entry || !Package || Entry->Package.Get() != Package || Package->GetName() != Entry->PackageName ||
        !Entry->World.IsValid() || !Entry->Actor.IsValid() || UPackage::IsEmptyPackage(Package) || Package->HasAnyPackageFlags(PKG_NewlyCreated) != bNew)
        return TEXT("The external actor/package no longer matches this prepared save.");
    UWorld* World = Entry->World.Get(); AActor* Actor = Entry->Actor.Get();
    if (!World->PersistentLevel || World->GetPackage()->GetName() != Entry->WorldName ||
        Actor->GetLevel() != World->PersistentLevel || Actor->GetOuter() != World->PersistentLevel ||
        World->PersistentLevel->GetPackage() != World->GetPackage() || Actor->GetExternalPackage() != Package ||
        !Actor->IsMainPackageActor() || Actor->HasAnyFlags(RF_Transient | RF_ClassDefaultObject | RF_ArchetypeObject) ||
        Actor->IsA<ALODActor>() || Actor->IsA<AWorldPartitionHLOD>() || !Entry->ActorGuid.IsValid() || Actor->GetActorGuid() != Entry->ActorGuid ||
        !World->PersistentLevel->Actors.Contains(Actor) || !Package->GetName().Contains(TEXT("/__ExternalActors__/")) ||
        ULevel::GetActorPackageName(World->GetPackage(), World->PersistentLevel->GetActorPackagingScheme(), Actor->GetPathName(), Actor) != Package->GetName())
        return TEXT("Only main actors belonging to this exact persistent map can be saved. External objects, HLOD and nested-container paths need separate adapters.");
    FString Filename;
    const bool bResolved = bNew ? FPackageName::TryConvertLongPackageNameToFilename(Package->GetName(), Filename, TEXT(".uasset")) : FPackageName::DoesPackageExist(Package->GetName(), &Filename);
    if (!bResolved || FPaths::ConvertRelativePathToFull(Filename) != Entry->Filename ||
        (bNew && (IFileManager::Get().FileExists(*Entry->Filename) || IFileManager::Get().DirectoryExists(*Entry->Filename))) ||
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
    TMap<FString, FString> SavedMaps;
    TArray<FString> RecoveryFolders;
    auto Fail = [&](const FString& Message)
    {
        GitWorkspace::FResult R; R.Error = Message + TEXT("\nLocks remain held. Completed writes remain on disk; nothing was staged or unlocked.");
        for (const auto& Entry : Plan.Entries) R.Error += TEXT("\n") + FString(Saved.Contains(Entry.Path) ? (Entry.Kind == FPackageSavePaths::EKind::DeleteActor ? TEXT("Deleted with recovery: ") : TEXT("Saved: ")) : TEXT("Not completed: ")) + Entry.Path;
        for (const auto& Folder : RecoveryFolders) R.Error += TEXT("\nRecovery backup: ") + Folder;
        return R;
    };
    if (!Plan.Error.IsEmpty() || !Plan.bCoordinatedActors || Plan.Owners.IsEmpty() || Plan.Entries.IsEmpty()) return Fail(TEXT("No valid coordinated map save was reviewed."));
    const auto Input = Sources(Plan); if (Input.IsEmpty()) return Fail(TEXT("The owning package set is no longer loaded."));
    TArray<TStrongObjectPtr<UPackage>> Packages; TArray<TStrongObjectPtr<UWorld>> Worlds; TArray<TStrongObjectPtr<AActor>> Actors;
    for (const auto& Entry : Plan.Entries)
    {
        if (!Entry.Package.IsValid() || !Permit.ContainsPath(Entry.Path)) return Fail(TEXT("Every package in an actor batch needs a prepared LFS save permit: ") + Entry.Path);
        Packages.Emplace(Entry.Package.Get());
        if (Entry.Actor.IsValid()) Actors.Emplace(Entry.Actor.Get());
    }
    for (const auto& Owner : Plan.Owners) if (Owner.World.IsValid()) Worlds.Emplace(Owner.World.Get()); else return Fail(TEXT("An owning world is no longer loaded."));
    auto CheckBatch = [&]() -> FString
    {
        for (const auto& Entry : Plan.Entries)
            if (Saved.Contains(Entry.Path) && Entry.Package.IsValid() && Entry.Package->IsDirty())
                return TEXT("A completed package has new unsaved edits from a save callback: ") + Entry.Path;
        return PlanDrift(Plan, GatherPackageSavePaths(Input, Root, Content), Saved, SavedMaps);
    };
    auto CheckWrite = [&](const FString& Path, UPackage* Package) -> FString
    {
        const auto* Entry = Plan.Entries.FindByPredicate([&](const auto& E) { return E.Path == Path; });
        if (!Entry || !Package || Entry->Package.Get() != Package || Package->GetName() != Entry->PackageName || Saved.Contains(Path))
            return TEXT("The package object/destination no longer matches the reviewed map save.");
        if (Entry->Kind == FPackageSavePaths::EKind::Actor || Entry->Kind == FPackageSavePaths::EKind::DeleteActor)
        { const FString Error = ValidateExternalActorBinding(Plan, Path, Package); if (!Error.IsEmpty()) return Error; }
        return CheckBatch();
    };
    FString Error = CheckBatch(); if (!Error.IsEmpty()) return Fail(Error);
    for (const auto& Entry : Plan.Entries) { const auto Ready = Repository.ValidateAssetSave(Permit, Entry.Path, Lease); if (!Ready.Ok()) return Fail(Ready.Error); }
    FPreparedScope Prepared(Repository, Permit, Lease, Root, &Plan, CheckWrite);
    struct FWorldCallback
    {
        UWorld* World; FObjectSaveContextData Context;
        bool bInitialized = false, bForceInitialized = false, bPosted = false;
        FWorldCallback(UWorld* InWorld, const FString& Filename) : World(InWorld), Context(InWorld->GetPackage(), nullptr, *Filename, SAVE_NoError) {}
    };
    TArray<TUniquePtr<FWorldCallback>> Callbacks;
    bool bExternalPosted = false;
    auto PostExternal = [&]() { bExternalPosted = true; for (const auto& World : Worlds) FEditorDelegates::PostSaveExternalActors.Broadcast(World.Get()); };
    ON_SCOPE_EXIT
    {
        Prepared.ExpectActorBatchWrite(FString());
        for (const auto& C : Callbacks)
        {
            if (!C->bPosted) { C->Context.bSaveSucceeded = false; GEditor->OnPostSaveWorld(C->World, FObjectPostSaveContext(C->Context)); }
            if (!Saved.Contains(RelativeFile(C->Context.TargetFilename, Root))) C->World->GetPackage()->SetDirtyFlag(true);
            if (C->bInitialized) GEditor->CleanupPhysicsSceneThatWasInitializedForSave(C->World, C->bForceInitialized);
        }
        if (!bExternalPosted) PostExternal();
    };
    for (const auto& World : Worlds) FEditorDelegates::PreSaveExternalActors.Broadcast(World.Get());
    for (const auto& Entry : Plan.Entries) Entry.Package->FullyLoad();
    // Native editor preparation may change the dirty set. Run it before any
    // writes and reject any new destinations rather than acquiring extra locks.
    for (const auto& Entry : Plan.Entries) if (Entry.Kind == FPackageSavePaths::EKind::Map)
    {
        if (!GEditor) return Fail(TEXT("Editor world-save callbacks are unavailable."));
        UWorld* World = Entry.World.Get(); World->GetPackage()->SetDirtyFlag(true);
        auto C = MakeUnique<FWorldCallback>(World, Entry.Filename);
        C->bInitialized = GEditor->InitializePhysicsSceneForSaveIfNecessary(World, C->bForceInitialized);
        GEditor->OnPreSaveWorld(World, FObjectPreSaveContext(C->Context)); Callbacks.Add(MoveTemp(C));
    }
    for (const auto& Entry : Plan.Entries)
    {
        if (Entry.Kind == FPackageSavePaths::EKind::DeleteActor) continue;
        FObjectSaveContextData Context(Entry.Package.Get(), nullptr, *Entry.Filename, SAVE_NoError);
        UPackage::PreSavePackageWithContextEvent.Broadcast(Entry.Package.Get(), FObjectPreSaveContext(Context));
    }
    Error = CheckBatch(); if (!Error.IsEmpty()) return Fail(Error);
    // Native external saves use RF_Standalone and no asset root. SavePackage
    // includes the package's external exports and serializes actor descriptors.
    FSavePackageArgs Args; Args.TopLevelFlags = RF_Standalone; Args.SaveFlags = SAVE_NoError;
    for (const auto& Entry : Plan.Entries)
    {
        Error = CheckBatch(); if (!Error.IsEmpty()) return Fail(Error);
        if (Entry.Kind == FPackageSavePaths::EKind::DeleteActor)
        {
            Error = CheckWrite(Entry.Path, Entry.Package.Get()); if (!Error.IsEmpty()) return Fail(Error);
            FActorDeletionRecovery Recovery;
            const auto Removed = RemoveExternalActorFile(Plan, Entry, Repository, Permit, Lease, Recovery);
            if (!Recovery.Folder.IsEmpty()) RecoveryFolders.Add(Recovery.Folder);
            if (!Removed.Ok()) return Fail(Removed.Error);
            // Notify only after the reviewed file is durably preserved and absent.
            // No ObjectTools cleanup, unloading, collection or source-control revert.
            FAssetRegistryModule::PackageDeleted(Entry.Package.Get());
            FEditorDelegates::OnPackageDeleted.Broadcast(Entry.Package.Get());
            UDeletedObjectPlaceholder::RemoveFromPackage(Entry.Package.Get());
            Entry.Package->SetDirtyFlag(false); Entry.Package->MarkAsNewlyCreated(); Saved.Add(Entry.Path);
            const auto Complete = CompleteExternalActorDeletion(Recovery, Repository, Lease);
            if (!Complete.Ok()) return Fail(Complete.Error);
            continue;
        }
        FString LateError;
        auto Settings = FSavePackageSettings::GetDefaultSettings();
        // Core invokes import validation after PreSave/harvest, before creating
        // its output linker. No destination bytes change if this check fails.
        Settings.AddExternalImportValidation([&](const FImportsValidationContext& Context)
        {
            LateError = CheckWrite(Entry.Path, const_cast<UPackage*>(Context.Package));
            if (LateError.IsEmpty())
            {
                const auto Ready = Async(EAsyncExecution::ThreadPool, [&] { return Repository.ValidateAssetSave(Permit, Entry.Path, Lease); }).Get();
                if (!Ready.Ok()) LateError = Ready.Error;
            }
            if (LateError.IsEmpty() && Permit.ContainsExternalActorPath(Entry.Path))
                GitWorkspaceSession::AllowVerifiedExternalReplacement(Entry.Filename);
            return LateError.IsEmpty() ? ESavePackageResult::Success : ESavePackageResult::Error;
        });
        FSavePackageContext Context(nullptr, nullptr, MoveTemp(Settings)); Args.SavePackageContext = &Context;
        Prepared.ExpectActorBatchWrite(Entry.Path);
        UObject* Asset = Entry.Kind == FPackageSavePaths::EKind::Map ? static_cast<UObject*>(Entry.World.Get()) : nullptr;
        if (!UPackage::SavePackage(Entry.Package.Get(), Asset, *Entry.Filename, Args))
        { Entry.Package->SetDirtyFlag(true); return Fail(TEXT("Package write failed or was refused: ") + Entry.Path + (LateError.IsEmpty() ? FString() : TEXT("\n") + LateError)); }
        UPackage::WaitForAsyncFileWrites(); Prepared.ExpectActorBatchWrite(FString()); Saved.Add(Entry.Path);
        if (Entry.Kind == FPackageSavePaths::EKind::Map)
        {
            SavedMaps.Add(Entry.Path, FileHash(Entry.Filename));
            for (const auto& C : Callbacks) if (C->World == Entry.World.Get())
            { C->Context.bSaveSucceeded = true; C->bPosted = true; GEditor->OnPostSaveWorld(C->World, FObjectPostSaveContext(C->Context)); }
        }
    }
    PostExternal(); Error = CheckBatch(); if (!Error.IsEmpty()) return Fail(Error);
    GitWorkspace::FResult Result; Result.Code = 0;
    FString Report;
    for (const auto& Folder : RecoveryFolders) Report += TEXT("Recovery backup: ") + Folder + TEXT("\n");
    if (!Report.IsEmpty()) Report += TEXT("With the editor closed, copy payload.uasset to the exact path in manifest.json to restore saved working bytes. Reopen to reload the actor. Staging and locks are unchanged. Keep backups until no longer needed.\n");
    FTCHARToUTF8 Bytes(*Report); Result.Out.Append(reinterpret_cast<const uint8*>(Bytes.Get()), Bytes.Length()); return Result;
}
#endif
}
