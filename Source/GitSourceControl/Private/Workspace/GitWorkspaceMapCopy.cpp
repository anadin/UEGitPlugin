// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#include "GitWorkspaceSaveFlow.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/World.h"
#include "Engine/Level.h"
#include "Engine/MapBuildDataRegistry.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "UObject/Package.h"
#include "UObject/PackageFileSummary.h"
#include "UObject/StrongObjectPtr.h"
#if PLATFORM_MAC
#include <sys/stat.h>
#endif

namespace GitWorkspaceSave
{
namespace
{
FString Hash(const FString& File)
{ const auto H = FMD5Hash::HashFile(*File); return H.IsValid() ? LexToString(H) : FString(); }
bool LiteralPath(const FString& File)
{
#if PLATFORM_MAC
    for (FString Path = File; !Path.IsEmpty(); Path = FPaths::GetPath(Path))
        if (IFileManager::Get().FileExists(*Path) || IFileManager::Get().DirectoryExists(*Path))
            return GitWorkspaceSession::CanonicalPath(Path) == Path;
    return false;
#else
    return true;
#endif
}
uint32 FileMode(const FString& File)
{
#if PLATFORM_MAC
    struct stat Info; return lstat(TCHAR_TO_UTF8(*File), &Info) == 0 && S_ISREG(Info.st_mode) ? Info.st_mode : 0;
#else
    return IFileManager::Get().IsReadOnly(*File) ? 1 : 2;
#endif
}
void ActorFiles(const TArray<FString>& Folders, TArray<FString>& Files)
{
    for (const auto& Folder : Folders) IFileManager::Get().FindFilesRecursive(Files, *Folder, TEXT("*.uasset"), true, false);
    Files.Sort();
}
FString ObjectFiles(const TArray<FString>& Folders)
{
    for (const auto& Folder : Folders)
    {
        TArray<FString> Files; IFileManager::Get().FindFilesRecursive(Files, *Folder, TEXT("*"), true, false);
        if (!LiteralPath(Folder) || !Files.IsEmpty()) return TEXT("Saved external objects, folder packages or changed paths need a separate map-copy adapter.");
    }
    return FString();
}
}
FString ReviewOFPACopySource(UWorld* World)
{
    if (!World || World->GetWorldPartition())
        return TEXT("World Partition Save As needs unloaded-actor and companion remapping. Use Save for the current map; WP copies are not supported yet.");
    const FString Error = ReviewExternalFirstMapSource(World, false);
    if (!Error.IsEmpty()) return TEXT("OFPA copy requires a fully loaded persistent map with canonical main actors and modern build data.\n") + Error;
    if (FPackageName::IsTempPackage(World->GetPackage()->GetName()) || World->GetPackage()->HasAnyPackageFlags(PKG_NewlyCreated) ||
        World->GetName() != FPackageName::GetShortName(World->GetPackage()->GetName()))
        return TEXT("Save and name this OFPA map before making a copy.");
    return FString();
}
FString CaptureOFPACopySource(UWorld* World, const FString& Root, const FString& Content, FMapSaveDestination& Destination)
{
    const FString Error = ReviewOFPACopySource(World); if (!Error.IsEmpty()) return Error;
    // Providers may call StaticFindObject (even for OFPA). Resolve them only
    // outside serialization, then inspect these exact folders in late checks.
    const auto ActorPaths = ULevel::GetExternalActorsPaths(World->GetPackage()->GetName());
    auto ObjectPaths = ULevel::GetExternalObjectsPaths(World->GetPackage()->GetName());
    for (const auto& Path : ActorPaths) ObjectPaths.Remove(Path);
    for (const auto& Paths : {ActorPaths, ObjectPaths}) for (const auto& Path : Paths)
    {
        FString Folder;
        if (!FPackageName::TryConvertLongPackageNameToFilename(Path, Folder)) return TEXT("Cannot resolve external package directories.");
        Folder = FPaths::ConvertRelativePathToFull(Folder);
        if (!FPaths::IsUnderDirectory(Folder, Root) || !FPaths::IsUnderDirectory(Folder, Content) || !LiteralPath(Folder))
            return TEXT("Source external directories must be literal paths inside this checkout's game Content.");
        (ActorPaths.Contains(Path) ? Destination.SourceActorFolders : Destination.SourceObjectFolders).AddUnique(Folder);
    }
    const FString ObjectError = ObjectFiles(Destination.SourceObjectFolders); if (!ObjectError.IsEmpty()) return ObjectError;
    TArray<UPackage*> Packages{World->GetPackage()};
    if (auto* Data = World->PersistentLevel->MapBuildData.Get()) Packages.Add(Data->GetPackage());
    for (AActor* Actor : World->PersistentLevel->Actors)
    {
        Destination.SourceActors.Add(Actor);
        if (Actor && Actor->IsPackageExternal()) Packages.AddUnique(Actor->GetExternalPackage());
    }
    Packages.Sort([](const auto& A, const auto& B) { return A.GetName() < B.GetName(); });
    TArray<FString> ExpectedActors;
    for (UPackage* Package : Packages)
    {
        FMapSaveDestination::FSourceFile Entry; Entry.Package = Package; Entry.Name = Package->GetName(); Entry.bDirty = Package->IsDirty();
        if (!FPackageName::TryConvertLongPackageNameToFilename(Entry.Name, Entry.Filename, Package == World->GetPackage() ? TEXT(".umap") : TEXT(".uasset")))
            return TEXT("Cannot resolve a source package for OFPA copy.");
        Entry.Filename = FPaths::ConvertRelativePathToFull(Entry.Filename);
        if (!FPaths::IsUnderDirectory(Entry.Filename, Root) || !FPaths::IsUnderDirectory(Entry.Filename, Content))
            return TEXT("Every OFPA source package must be inside this checkout's game Content.");
        if (!LiteralPath(Entry.Filename))
            return TEXT("Symlinked or unresolved OFPA source packages cannot be copied.");
        for (FString Parent = FPaths::GetPath(Entry.Filename); Parent != Root && !Parent.IsEmpty(); Parent = FPaths::GetPath(Parent))
            if (IFileManager::Get().DirectoryExists(*(Parent / TEXT(".git"))) || IFileManager::Get().FileExists(*(Parent / TEXT(".git"))))
                return TEXT("An OFPA source package belongs to a nested checkout.");
        Entry.bExists = IFileManager::Get().FileExists(*Entry.Filename);
        if (!Entry.bExists)
        {
            if (Package == World->GetPackage() || !Package->HasAnyPackageFlags(PKG_NewlyCreated) || IFileManager::Get().DirectoryExists(*Entry.Filename))
                return TEXT("Restore/hydrate the missing saved source package before making an OFPA copy: ") + Entry.Name;
        }
        else
        {
            if (Package->HasAnyPackageFlags(PKG_NewlyCreated)) return TEXT("A new source package has an occupied destination: ") + Entry.Name;
            TUniquePtr<FArchive> Reader(IFileManager::Get().CreateFileReader(*Entry.Filename)); uint32 Tag = 0;
            if (Reader && Reader->TotalSize() >= sizeof(Tag)) *Reader << Tag;
            if (!Reader || Reader->IsError() || Tag != PACKAGE_FILE_TAG) return TEXT("Hydrate the source Unreal package before copying: ") + Entry.Name;
            Entry.Hash = Hash(Entry.Filename); if (Entry.Hash.IsEmpty()) return TEXT("Cannot read the source package: ") + Entry.Name;
            Entry.bReadOnly = IFileManager::Get().IsReadOnly(*Entry.Filename);
            Entry.Mode = FileMode(Entry.Filename); if (!Entry.Mode) return TEXT("The source is not a regular package file.");
            if (Entry.Name.Contains(TEXT("/__ExternalActors__/"))) ExpectedActors.Add(Entry.Filename);
        }
        Destination.SourceFiles.Add(MoveTemp(Entry));
    }
    TArray<FString> ActualActors; ActorFiles(Destination.SourceActorFolders, ActualActors);
    ExpectedActors.Sort();
    if (ActualActors != ExpectedActors) return TEXT("This OFPA map has unloaded, deleted or orphan actor files. Load/reconcile the complete map before copying.");
    return FString();
}
FString ValidateOFPACopySource(UWorld* World, const FMapSaveDestination& Destination)
{
    // Also called inside core serialization: inspect held objects and files,
    // never FindPackage/StaticFindObject, load objects or resolve descriptors.
    if (!World || World->GetPackage()->GetName() != Destination.SourcePackage ||
        World->PersistentLevel != Destination.SourceLevel.Get() || World->GetWorldPartition() ||
        World->PersistentLevel->MapBuildData.Get() != Destination.SourceBuildData.Get())
        return TEXT("The OFPA copy's source world or build-data relationship changed. Review again.");
    const FString Error = ReviewOFPACopySource(World); if (!Error.IsEmpty()) return Error;
    const FString ObjectError = ObjectFiles(Destination.SourceObjectFolders); if (!ObjectError.IsEmpty()) return ObjectError;
    for (const auto& Folder : Destination.SourceActorFolders) if (!LiteralPath(Folder)) return TEXT("Source actor directory changed during OFPA copy.");
    if (World->PersistentLevel->Actors.Num() != Destination.SourceActors.Num()) return TEXT("The source actor set changed during OFPA copy. Review again.");
    for (int32 I = 0; I < Destination.SourceActors.Num(); ++I)
        if (World->PersistentLevel->Actors[I] != Destination.SourceActors[I].Get()) return TEXT("A source actor was replaced during OFPA copy. Review again.");
    for (const auto& Actor : Destination.Actors)
        if (!Actor.Actor.IsValid() || Actor.Actor->GetActorGuid() != Actor.Guid || Actor.Actor->GetPathName() != Actor.SourcePath ||
            Actor.Actor->GetActorLabel() != Actor.Label || !Actor.Actor->GetExternalPackage() || Actor.Actor->GetExternalPackage()->GetName() != Actor.SourcePackage)
            return TEXT("A source actor identity or binding changed during OFPA copy. Review again.");
    TArray<FString> ExpectedActors;
    for (const auto& File : Destination.SourceFiles)
    {
        if (!File.Package.IsValid() || File.Package->GetName() != File.Name || File.Package->IsDirty() != File.bDirty ||
            IFileManager::Get().FileExists(*File.Filename) != File.bExists || IFileManager::Get().DirectoryExists(*File.Filename) ||
            (File.bExists && (Hash(File.Filename) != File.Hash || FileMode(File.Filename) != File.Mode || IFileManager::Get().IsReadOnly(*File.Filename) != File.bReadOnly)))
            return TEXT("Source bytes, permissions or package state changed during OFPA copy: ") + File.Name + TEXT(". Review again.");
        if (!LiteralPath(File.Filename)) return TEXT("Source path changed during OFPA copy.");
        if (File.bExists && File.Name.Contains(TEXT("/__ExternalActors__/"))) ExpectedActors.Add(File.Filename);
    }
    TArray<FString> ActualActors; ActorFiles(Destination.SourceActorFolders, ActualActors);
    ExpectedActors.Sort();
    return ActualActors == ExpectedActors ? FString() : TEXT("The source actor files changed during OFPA copy. Review again.");
}
#if PLATFORM_MAC
GitWorkspace::FResult WriteOFPACopyDestination(UWorld* Source, const FMapSaveDestination& Destination, GitWorkspace::FRepository& Repository,
    const GitWorkspace::FAssetSavePermit& Permit, const GitWorkspaceSession::FLease& Lease, const FString& Content, UWorld*& OutWorld)
{
    check(IsInGameThread()); OutWorld = nullptr;
    auto Fail = [](const FString& Error) { GitWorkspace::FResult R; R.Error = Error + TEXT("\nThe source stays open. Completed copy files, unsaved copy packages and destination locks remain; review Save All to retry remaining copy packages. Nothing was staged or unlocked."); return R; };
    TStrongObjectPtr<UWorld> HoldSource(Source);
    auto CheckSource = [&] { return ValidateOFPACopySource(Source, Destination); };
    FString Error = CheckSource(); if (!Error.IsEmpty()) return Fail(Error);
    UPackage* Package = CreatePackage(*Destination.Map.PackageName); UWorld* Copy = nullptr;
    TMap<UObject*, UObject*> Created;
    {
        // Native ULevel::PreDuplicate seeds canonical external packages and
        // remaps actor references. No FileHelpers cleanup or source rename.
        FPreparedScope Prepared(Repository, Permit, Lease, Repository.Refresh().Root);
        auto Params = InitStaticDuplicateObjectParams(Source, Package, *FPackageName::GetShortName(Destination.Map.PackageName), RF_AllFlags, nullptr, EDuplicateMode::World);
        Params.CreatedObjects = &Created; Copy = Cast<UWorld>(StaticDuplicateObjectEx(Params));
    }
    if (!Copy) return Fail(TEXT("Unreal could not duplicate this OFPA map."));
    TStrongObjectPtr<UWorld> HoldCopy(Copy); OutWorld = Copy;
    Error = CheckSource(); if (!Error.IsEmpty()) return Fail(Error);
    if (Copy == Source || Copy->PersistentLevel == Source->PersistentLevel || Copy->GetWorldPartition() || !Copy->PersistentLevel->IsUsingExternalActors())
        return Fail(TEXT("The duplicated world has unexpected ownership or packaging."));
    TSet<FGuid> Guids;
    for (const auto& Entry : Destination.Actors)
    {
        AActor* Actor = Cast<AActor>(Created.FindRef(Entry.Actor.Get()));
        if (!Actor || Actor == Entry.Actor.Get() || Actor->GetOuter() != Copy->PersistentLevel || !Actor->IsMainPackageActor() ||
            !Actor->GetExternalPackage() || Actor->GetExternalPackage()->GetName() != Entry.Target.PackageName ||
            !Actor->GetActorGuid().IsValid() || Actor->GetActorGuid() == Entry.Guid || Guids.Contains(Actor->GetActorGuid()))
            return Fail(TEXT("Unreal produced an actor identity/package outside the reviewed copy destinations. No package was saved."));
        Guids.Add(Actor->GetActorGuid()); Actor->MarkPackageDirty();
    }
    if (Destination.SourceBuildData.IsValid() && !Copy->PersistentLevel->MapBuildData)
    {
        FPreparedScope Prepared(Repository, Permit, Lease, Repository.Refresh().Root);
        UPackage* DataPackage = CreatePackage(*Destination.BuildData.PackageName); DataPackage->SetPackageFlags(PKG_ContainsMapData);
        Copy->PersistentLevel->MapBuildData = Cast<UMapBuildDataRegistry>(StaticDuplicateObject(Destination.SourceBuildData.Get(), DataPackage, *FPackageName::GetShortName(Destination.BuildData.PackageName)));
    }
    auto* Data = Copy->PersistentLevel->MapBuildData.Get();
    if ((Data != nullptr) != !Destination.BuildData.Path.IsEmpty() || (Data && (Data == Destination.SourceBuildData.Get() || Data->GetPackage()->GetName() != Destination.BuildData.PackageName)))
        return Fail(TEXT("Unreal produced unexpected or shared build data. No package was saved."));
    Package->ThisContainsMap(); Package->MarkAsFullyLoaded(); Package->SetAssetAccessSpecifier(Source->GetPackage()->GetAssetAccessSpecifier());
    if (Source->GetPackage()->HasAnyPackageFlags(PKG_DisallowExport)) Package->SetPackageFlags(PKG_DisallowExport);
    Copy->ClearFlags(RF_Transient); Copy->SetFlags(RF_Public | RF_Standalone); Copy->MarkPackageDirty(); FAssetRegistryModule::AssetCreated(Copy);
    if (Data) { Data->MarkPackageDirty(); FAssetRegistryModule::AssetCreated(Data); }
    const auto Plan = GatherPackageSavePaths({Package}, Repository.Refresh().Root, Content);
    if (!Plan.Error.IsEmpty() || Plan.Paths != Destination.Paths() || Plan.NewPaths != Destination.Paths() || Plan.ExternalActorPaths != Destination.ExternalPaths())
        return Fail(TEXT("The copy package set differs from the reviewed destinations. No package was saved.\n") + Plan.Error);
    const auto Result = WriteExternalActorSave(Plan, Repository, Permit, Lease, Content, CheckSource);
    Error = CheckSource(); if (!Error.IsEmpty()) return Fail(Error);
    return Result.Ok() ? Result : Fail(Result.Error);
}
#endif
}
