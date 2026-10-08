// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#include "GitWorkspaceSaveFlow.h"
#include "Editor.h"
#include "Engine/World.h"
#include "Engine/Level.h"
#include "Engine/MapBuildDataRegistry.h"
#include "GameFramework/Actor.h"
#include "Engine/LODActor.h"
#include "WorldPartition/WorldPartition.h"
#include "Interfaces/IMainFrameModule.h"
#include "Modules/ModuleManager.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "HAL/FileManager.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/StrongObjectPtr.h"
#include "Misc/RedirectCollector.h"

namespace GitWorkspaceSave
{
FString ReviewMapSource(UWorld* World)
{
    if (!World || World->GetClass() != UWorld::StaticClass() || !World->PersistentLevel ||
        (World->WorldType != EWorldType::Editor && World->WorldType != EWorldType::Inactive))
        return TEXT("Choose an ordinary editor map and stop Play/Simulate before saving.");
    ULevel* Level = World->PersistentLevel;
    if (World->GetWorldPartition() || Level->IsUsingExternalActors() || Level->IsUsingExternalObjects() || !World->GetPackage()->GetExternalPackages().IsEmpty())
    {
        if (FPackageName::IsTempPackage(World->GetPackage()->GetName())) return ReviewExternalFirstMapSource(World, true);
        return ReviewExternalMapCopySource(World);
    }
    if (World->GetCurrentLevel() != Level || World->GetLevels().Num() != 1 || !World->GetStreamingLevels().IsEmpty() || World->WorldComposition)
        return TEXT("Streaming, sublevel and World Composition saves need a separate map workflow. Save an ordinary persistent map here.");
    for (AActor* Actor : Level->Actors)
        if (Actor && (Actor->IsPackageExternal() || Actor->IsA<ALODActor>()))
            return TEXT("External actors and HLOD proxy packages require a separate coordinated map save.");
    if (UMapBuildDataRegistry* Data = Level->MapBuildData)
        if (Data->GetClass() != UMapBuildDataRegistry::StaticClass() || Data->IsLegacyBuildData() ||
            Data->GetPackage()->GetName() != World->GetPackage()->GetName() + TEXT("_BuiltData") || !Data->GetPackage()->GetExternalPackages().IsEmpty())
            return TEXT("This map's build data is legacy, shared or custom. Its package relationship needs a separate save adapter.");
    return FString();
}
FMapSaveDestination ReviewMapDestination(UWorld* World, const FString& PackageName, const FString& Root, const FString& Content)
{
    FMapSaveDestination Out; Out.Error = ReviewMapSource(World); if (!Out.Error.IsEmpty()) return Out;
    Out.SourcePackage = World->GetPackage()->GetName();
    Out.SourceBuildData = World->PersistentLevel->MapBuildData.Get();
    Out.SourceLevel = World->PersistentLevel; Out.SourcePartition = World->GetWorldPartition();
    Out.bNameCurrent = FPackageName::IsTempPackage(Out.SourcePackage);
    Out.bExternalFirstSave = Out.bNameCurrent && World->PersistentLevel->IsUsingExternalActors();
    Out.bExternalCopy = !Out.bNameCurrent && World->PersistentLevel->IsUsingExternalActors();
    if (Out.bExternalCopy)
    { Out.Error = CaptureExternalMapCopySource(World, Root, Content, Out); if (!Out.Error.IsEmpty()) return Out; }
    Out.Map = ReviewAbsentDestination(PackageName, Out.SourcePackage, Root, Content, true);
    if (!Out.Map.Error.IsEmpty()) { Out.Error = Out.Map.Error; return Out; }
    // Refuse orphan companion data even when the source has none. Never delete
    // or overwrite destination packages through Unreal's inherited Save As.
    auto Data = ReviewAbsentDestination(PackageName + TEXT("_BuiltData"), Out.SourcePackage, Root, Content, false);
    if (!Data.Error.IsEmpty()) { Out.Error = TEXT("Build-data destination: ") + Data.Error; return Out; }
    if (Out.SourceBuildData.IsValid()) Out.BuildData = Data;
    TArray<FString> ExternalPaths = ULevel::GetExternalActorsPaths(PackageName);
    ExternalPaths.Append(ULevel::GetExternalObjectsPaths(PackageName));
    for (const FString& Path : ExternalPaths)
    {
        FString Filename;
        if (!FPackageName::TryConvertLongPackageNameToFilename(Path, Filename) || IFileManager::Get().DirectoryExists(*Filename) || IFileManager::Get().FileExists(*Filename))
        { Out.Error = TEXT("The destination has an external-package directory or cannot be resolved. Choose a fresh map destination."); return Out; }
    }
    if (Out.bExternalFirstSave || Out.bExternalCopy) ReviewExternalFirstMapActors(World, Root, Content, Out);
    if (Out.Error.IsEmpty() && Out.bExternalFirstSave) ReviewHLODCompanions(World, Root, Content, Out);
    return Out;
}
#if PLATFORM_MAC
GitWorkspace::FResult WriteMapDestination(UWorld* Source, const FMapSaveDestination& Destination, GitWorkspace::FRepository& Repository,
    const GitWorkspace::FAssetSavePermit& Permit, const GitWorkspaceSession::FLease& Lease, const FString& Content, UWorld*& OutWorld, TFunction<FString()> ValidateContext)
{
    check(IsInGameThread()); OutWorld = nullptr;
    auto Fail = [](const FString& Error) { GitWorkspace::FResult R; R.Error = Error; return R; };
    if (ValidateContext) { const FString Error = ValidateContext(); if (!Error.IsEmpty()) return Fail(Error); }
    const FString Root = Repository.Refresh().Root;
    const auto Current = ReviewMapDestination(Source, Destination.Map.PackageName, Root, Content);
    if (!Destination.Error.IsEmpty() || !Current.Error.IsEmpty()) return Fail(Current.Error.IsEmpty() ? Destination.Error : Current.Error);
    if (Current.SourcePackage != Destination.SourcePackage || Current.SourceBuildData != Destination.SourceBuildData ||
        Current.SourceLevel != Destination.SourceLevel || Current.SourcePartition != Destination.SourcePartition ||
        Current.bNameCurrent != Destination.bNameCurrent || Current.bExternalFirstSave != Destination.bExternalFirstSave ||
        Current.bExternalCopy != Destination.bExternalCopy ||
        Current.SourceActorFolders != Destination.SourceActorFolders || Current.SourceObjectFolders != Destination.SourceObjectFolders ||
        Current.SourceContainer != Destination.SourceContainer || Current.bSourcePartitionInitialized != Destination.bSourcePartitionInitialized ||
        Current.SourceDescriptors != Destination.SourceDescriptors ||
        Current.Map.Filename != Destination.Map.Filename || Current.Paths() != Destination.Paths() || Current.Actors.Num() != Destination.Actors.Num() ||
        Current.HLODCompanions.Num() != Destination.HLODCompanions.Num())
        return Fail(TEXT("The map or its build-data package changed after review. Review the complete package set again."));
    for (int32 I = 0; I < Current.Actors.Num(); ++I)
    {
        const auto& A = Current.Actors[I]; const auto& B = Destination.Actors[I];
        if (A.Actor != B.Actor || A.Guid != B.Guid || A.SourcePackage != B.SourcePackage || A.SourcePath != B.SourcePath || A.Label != B.Label || A.HLODLayer != B.HLODLayer || A.Target.PackageName != B.Target.PackageName)
            return Fail(TEXT("An actor or its package identity changed after map naming review. Review all destinations again."));
        if (!Permit.ContainsExternalActorPath(A.Target.Path)) return Fail(TEXT("Actor destinations require an explicit coordinated first-save permit."));
    }
    for (int32 I = 0; I < Current.HLODCompanions.Num(); ++I)
    {
        const auto& A = Current.HLODCompanions[I]; const auto& B = Destination.HLODCompanions[I];
        if (!(A.Source == B.Source) || A.Target.PackageName != B.Target.PackageName || A.ObjectName != B.ObjectName)
            return Fail(TEXT("The default/parent HLOD companion chain changed after review."));
    }
    for (const FString& Path : Current.Paths())
    {
        if (!Permit.ContainsNewPath(Path)) return Fail(TEXT("The map destination set does not match the prepared first-save permit."));
        const auto Ready = Repository.ValidateAssetSave(Permit, Path, Lease); if (!Ready.Ok()) return Ready;
    }
    if (Current.bExternalFirstSave) return WriteExternalFirstMapDestination(Source, Current, Repository, Permit, Lease, Content, OutWorld);
    if (Current.bExternalCopy)
    {
        const FString Error = ValidateExternalMapCopySource(Source, Destination); if (!Error.IsEmpty()) return Fail(Error);
        return WriteExternalMapCopyDestination(Source, Destination, Repository, Permit, Lease, Content, OutWorld, MoveTemp(ValidateContext));
    }
    TStrongObjectPtr<UWorld> HoldSource(Source);
    FPreparedScope Prepared(Repository, Permit, Lease, Root);
    UPackage* Package = CreatePackage(*Current.Map.PackageName);
    UWorld* World = nullptr;
    const FSoftObjectPath OldPath(Source);
    const bool bDisallowExport = Source->GetPackage()->HasAnyPackageFlags(PKG_DisallowExport);
    const auto AccessSpecifier = Source->GetPackage()->GetAssetAccessSpecifier();
    if (Current.bNameCurrent)
    {
        // Only unnamed /Temp worlds move. Moving a saved world would discard the
        // original editor context; Save As instead duplicates it below.
        const ERenameFlags Flags = REN_NonTransactional | REN_DontCreateRedirectors | REN_AllowPackageLinkerMismatch;
        if (!Source->Rename(*FPackageName::GetLongPackageAssetName(Current.Map.PackageName), Package, Flags | REN_Test) ||
            !Source->Rename(*FPackageName::GetLongPackageAssetName(Current.Map.PackageName), Package, Flags))
            return Fail(TEXT("Could not name the current map. Unsaved edits and acquired locks remain; inspect its current package name before retrying."));
        World = Source;
    }
    else World = Cast<UWorld>(StaticDuplicateObject(Source, Package, *FPackageName::GetLongPackageAssetName(Current.Map.PackageName), RF_AllFlags, nullptr, EDuplicateMode::World));
    if (!World) return Fail(TEXT("Could not copy the map. Acquired destination locks remain held."));
    TStrongObjectPtr<UWorld> HoldWorld(World); OutWorld = World;
    // UE 5.8 marks Level.MapBuildData NonPIEDuplicateTransient. A normal map
    // duplicate can therefore have no registry when PostDuplicate runs. Copy
    // the exact reviewed modern registry explicitly; never share the source.
    if (!Current.bNameCurrent && Current.SourceBuildData.IsValid() && !World->PersistentLevel->MapBuildData)
    {
        UPackage* DataPackage = CreatePackage(*Current.BuildData.PackageName); DataPackage->SetPackageFlags(PKG_ContainsMapData);
        World->PersistentLevel->MapBuildData = Cast<UMapBuildDataRegistry>(StaticDuplicateObject(Current.SourceBuildData.Get(), DataPackage, *FPackageName::GetLongPackageAssetName(Current.BuildData.PackageName)));
    }
    Package->ThisContainsMap(); Package->MarkAsFullyLoaded();
    World->ClearFlags(RF_Transient); World->SetFlags(RF_Public | RF_Standalone); World->MarkPackageDirty();
    if (bDisallowExport) Package->SetPackageFlags(PKG_DisallowExport);
    Package->SetAssetAccessSpecifier(AccessSpecifier);
    FAssetRegistryModule::AssetCreated(World);
    if (Current.bNameCurrent) GRedirectCollector.AddAssetPathRedirection(OldPath.GetWithoutSubPath(), FSoftObjectPath(World).GetWithoutSubPath());
    ON_SCOPE_EXIT { if (Current.bNameCurrent) GRedirectCollector.RemoveAssetPathRedirection(OldPath.GetWithoutSubPath()); };
    UMapBuildDataRegistry* Data = World->PersistentLevel->MapBuildData;
    if ((Data != nullptr) != !Current.BuildData.Path.IsEmpty() || (Data && Data->GetPackage()->GetName() != Current.BuildData.PackageName))
        return Fail(TEXT("The named/copied map produced unexpected build data: ") + (Data ? Data->GetPathName() : TEXT("none")) + TEXT("; expected ") + Current.BuildData.PackageName + TEXT(". Nothing was saved; its unsaved packages and locks remain."));
    bool bForceInitialized = false;
    const bool bInitialized = GEditor && GEditor->InitializePhysicsSceneForSaveIfNecessary(World, bForceInitialized);
    ON_SCOPE_EXIT { if (bInitialized) GEditor->CleanupPhysicsSceneThatWasInitializedForSave(World, bForceInitialized); };
    FSavePackageArgs Args; Args.TopLevelFlags = RF_Public | RF_Standalone; Args.SaveFlags = SAVE_NoError;
    // Save the dependency first. Failure never writes a map that points at a
    // missing build-data file. Completed files/locks are retained on partial failure.
    if (Data)
    {
        FAssetRegistryModule::AssetCreated(Data); Data->MarkPackageDirty();
        if (!UPackage::SavePackage(Data->GetPackage(), Data, *Current.BuildData.Filename, Args))
            return Fail(TEXT("Build data could not be saved. The map is still unsaved; its packages and destination locks remain. Retry Save on the current named map, or review Save All / Git Workspace Save assets for an unsaved copy."));
    }
    if (!UPackage::SavePackage(Package, World, *Current.Map.Filename, Args))
    {
        World->MarkPackageDirty();
        return Fail(TEXT("Map could not be saved. Any completed build-data file and both locks remain. Retry Save on the current named map, or use Save All / Git Workspace Save assets to review and retry an unsaved copy."));
    }
    UPackage::WaitForAsyncFileWrites();
    // FileHelpers resolves a valid named package directly on the next Save;
    // its private filename-cache helper is neither needed nor exported.
    if (Current.bNameCurrent && GEditor && GEditor->GetEditorWorldContext().World() == World)
        if (auto* MainFrame = FModuleManager::GetModulePtr<IMainFrameModule>(TEXT("MainFrame"))) MainFrame->SetLevelNameForWindowTitle(Current.Map.Filename);
    GitWorkspace::FResult Result; Result.Code = 0;
    return Result;
}
#endif
}
