// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#pragma once
#include "GitWorkspaceRepository.h"
#include "Framework/Commands/UIAction.h"
class UPackage;
class FUICommandList;
class FUICommandInfo;
class ISourceControlProvider;
class UObject;
class FAssetEditorToolkit;
class UWorld;
class AActor;
class UDeletedObjectPlaceholder;

namespace GitWorkspaceSave
{
struct FHLODLayerBinding
{
    TWeakObjectPtr<UObject> Layer, Parent;
    FString PackageName, ObjectName;
    bool operator==(const FHLODLayerBinding& Other) const
    { return Layer == Other.Layer && Parent == Other.Parent && PackageName == Other.PackageName && ObjectName == Other.ObjectName; }
};
FString CaptureHLODLayerChain(UWorld* World, TArray<FHLODLayerBinding>& Out);
bool HandlesProvider(const ISourceControlProvider& Provider);
void Register();
void Unregister();
void SaveDirtyAssets();
FString LockRemote();
void SetLockRemote(const FString& Remote);
// Public in this private module for fixture acceptance tests and lifecycle checks.
void InstallGuard();
void RemoveGuard();
void ShowCleanupNotices();
struct FPackageSavePaths
{
    enum class EKind : uint8 { Asset, Actor, BuildData, HLODLayer, Map, DeleteActor };
    struct FEntry
    {
        TWeakObjectPtr<UPackage> Package;
        TWeakObjectPtr<UWorld> World;
        TWeakObjectPtr<AActor> Actor;
        FString PackageName, Path, Filename, WorldName, WorldFilename, WorldHash;
        FGuid ActorGuid;
        TWeakObjectPtr<UObject> ActorHLODLayer;
        int32 CompanionDepth = 0;
        FString ActorPath, ActorLabel;
        EKind Kind = EKind::Asset;
    };
    struct FOwner
    {
        TWeakObjectPtr<UWorld> World;
        TWeakObjectPtr<UObject> BuildData;
        FString Name, Filename, Path, Hash, BuildDataName;
        TArray<FHLODLayerBinding> HLODLayers;
    };
    TArray<FString> Paths, NewPaths, ExternalActorPaths, DeletePaths;
    TArray<FEntry> Entries;
    TArray<FOwner> Owners;
    TArray<TWeakObjectPtr<UPackage>> Sources;
    FString Error;
    bool bCoordinatedActors = false;
};
FPackageSavePaths GatherPackageSavePaths(const TArray<UPackage*>& Packages, const FString& Root, const FString& Content);
FPackageSavePaths GatherOrdinaryPackageSavePaths(const TArray<UPackage*>& Packages, const FString& Root, const FString& Content);
FString ValidateExternalActorBinding(const FPackageSavePaths& Plan, const FString& Path, UPackage* Package);
bool NeedsCoordinatedWorldSave(UPackage* Package);
// Save As is a copy into an absent destination; the source is never renamed or saved.
struct FAssetCopyDestination
{
    FString PackageName, Path, Filename, Error;
};
FAssetCopyDestination ReviewAbsentDestination(const FString& PackageName, const FString& SourcePackage, const FString& Root, const FString& Content, bool bMap);
// First Save names the live world; Save As preserves its source packages.
struct FMapSaveDestination
{
    struct FSourceFile
    {
        TWeakObjectPtr<UPackage> Package;
        FString Name, Filename, Hash;
        uint32 Mode = 0;
        bool bDirty = false, bExists = false, bReadOnly = false, bLoaded = false;
    };
    struct FActorDestination
    {
        TWeakObjectPtr<AActor> Actor;
        FGuid Guid;
        FString SourcePackage, SourcePath, Label;
        TWeakObjectPtr<UObject> HLODLayer;
        FAssetCopyDestination Target;
    };
    struct FHLODCompanion
    {
        FHLODLayerBinding Source;
        FAssetCopyDestination Target;
        FString ObjectName;
    };
    struct FPartitionDescriptor
    {
        FGuid Guid;
        FString Package, Path, Label;
        TArray<FGuid> References;
        bool operator==(const FPartitionDescriptor& Other) const
        { return Guid == Other.Guid && Package == Other.Package && Path == Other.Path && Label == Other.Label && References == Other.References; }
    };
    FAssetCopyDestination Map, BuildData;
    TArray<FActorDestination> Actors;
    TArray<FHLODCompanion> HLODCompanions;
    TArray<FSourceFile> SourceFiles;
    TArray<TWeakObjectPtr<AActor>> SourceActors;
    TArray<FString> SourceActorFolders, SourceObjectFolders;
    TArray<FPartitionDescriptor> SourceDescriptors;
    FString SourcePackage, Error;
    TWeakObjectPtr<UObject> SourceBuildData, SourceLevel, SourcePartition;
    TWeakObjectPtr<UObject> SourceContainer;
    bool bSourcePartitionInitialized = false;
    bool bNameCurrent = false, bExternalFirstSave = false, bExternalCopy = false;
    TArray<FString> ExternalPaths() const { TArray<FString> Out; for (const auto& Actor : Actors) Out.Add(Actor.Target.Path); Out.Sort(); return Out; }
    TArray<FString> Paths() const { TArray<FString> Out{Map.Path}; if (!BuildData.Path.IsEmpty()) Out.Add(BuildData.Path); for (const auto& Layer : HLODCompanions) Out.Add(Layer.Target.Path); Out.Append(ExternalPaths()); Out.Sort(); return Out; }
};
void ReviewHLODCompanions(UWorld* World, const FString& Root, const FString& Content, FMapSaveDestination& Destination);
FString CreateHLODCompanions(UWorld* World, const FMapSaveDestination& Destination);
FString GatherOwnedHLODCompanions(UWorld* World, const FString& Root, const FString& Content, FPackageSavePaths::FOwner& Owner, FPackageSavePaths& Out, bool& bSaveMap);
FString ReviewMapSource(UWorld* World);
FString ReviewExternalFirstMapSource(UWorld* World, bool bTemporary, bool bRequireLoaded = true);
void ReviewExternalFirstMapActors(UWorld* World, const FString& Root, const FString& Content, FMapSaveDestination& Destination);
FString ReviewExternalMapCopySource(UWorld* World, bool bAllowUnloaded = false);
FString CaptureExternalMapCopySource(UWorld* World, const FString& Root, const FString& Content, FMapSaveDestination& Destination, bool bAllowUnloaded = false);
FString ValidateExternalMapCopySource(UWorld* World, const FMapSaveDestination& Destination, bool bAllowUnloaded = false);
FMapSaveDestination ReviewMapDestination(UWorld* World, const FString& PackageName, const FString& Root, const FString& Content);
bool SupportsAssetCopy(const UObject* Source);
UObject* GetCopySource(const FAssetEditorToolkit& Editor);
UObject* GetCopyData(FAssetEditorToolkit& Editor);
// Material editors hold unapplied graph edits in a separate transient preview.
FString ReviewCopyData(UObject* Source, UObject* EditedData);
FAssetCopyDestination ReviewCopyDestination(UObject* Source, const FString& PackageName, const FString& Root, const FString& Content);
#if PLATFORM_MAC
// Writes/deletes only the reviewed set, preserving deletion recovery and staging.
GitWorkspace::FResult WriteExternalActorSave(const FPackageSavePaths& Plan, GitWorkspace::FRepository& Repository,
    const GitWorkspace::FAssetSavePermit& Permit, const GitWorkspaceSession::FLease& Lease, const FString& Content, TFunction<FString()> ValidateContext = {});
struct FActorDeletionRecovery { FString Folder, Marker, Manifest, Path, Hash, Head, Branch, Stashes; TArray<uint8> IndexEntries; bool bActive = false; };
GitWorkspace::FResult RemoveExternalActorFile(const FPackageSavePaths& Plan, const FPackageSavePaths::FEntry& Entry, GitWorkspace::FRepository& Repository,
    const GitWorkspace::FAssetSavePermit& Permit, const GitWorkspaceSession::FLease& Lease, FActorDeletionRecovery& Recovery);
GitWorkspace::FResult CompleteExternalActorDeletion(FActorDeletionRecovery& Recovery, GitWorkspace::FRepository& Repository,
    const GitWorkspaceSession::FLease& Lease);
GitWorkspace::FResult WriteAssetCopy(UObject* Source, const FAssetCopyDestination& Destination, GitWorkspace::FRepository& Repository,
    const GitWorkspace::FAssetSavePermit& Permit, const GitWorkspaceSession::FLease& Lease, const FString& Content, UObject*& OutCopy, UObject* EditedData = nullptr);
GitWorkspace::FResult WriteMapDestination(UWorld* Source, const FMapSaveDestination& Destination, GitWorkspace::FRepository& Repository,
    const GitWorkspace::FAssetSavePermit& Permit, const GitWorkspaceSession::FLease& Lease, const FString& Content, UWorld*& OutWorld, TFunction<FString()> ValidateContext = {});
GitWorkspace::FResult WriteExternalFirstMapDestination(UWorld* Source, const FMapSaveDestination& Destination, GitWorkspace::FRepository& Repository,
    const GitWorkspace::FAssetSavePermit& Permit, const GitWorkspaceSession::FLease& Lease, const FString& Content, UWorld*& OutWorld);
GitWorkspace::FResult WriteExternalMapCopyDestination(UWorld* Source, const FMapSaveDestination& Destination, GitWorkspace::FRepository& Repository,
    const GitWorkspace::FAssetSavePermit& Permit, const GitWorkspaceSession::FLease& Lease, const FString& Content, UWorld*& OutWorld, TFunction<FString()> ValidateContext = {});
class FPreparedScope
{
public:
    FPreparedScope(GitWorkspace::FRepository& Repository, const GitWorkspace::FAssetSavePermit& Permit, const GitWorkspaceSession::FLease& Lease, const FString& Root,
        const FPackageSavePaths* ActorPlan = nullptr, TFunction<FString(const FString&, UPackage*)> ValidateBatch = {});
    ~FPreparedScope();
    void ExpectActorBatchWrite(const FString& Path);
    FPreparedScope(const FPreparedScope&) = delete;
    FPreparedScope& operator=(const FPreparedScope&) = delete;
private:
    bool bInstalled = false;
};
#endif
// Retain the original action, including its enable/visibility/check delegates.
void WrapCommand(TSharedRef<FUICommandList> List, TSharedPtr<const FUICommandInfo> Command, TFunction<TArray<UPackage*>()> GetPackages);
void WrapSaveAsCommand(TSharedRef<FUICommandList> List, TSharedPtr<const FUICommandInfo> Command, TFunction<UObject*()> GetSource, TFunction<UObject*()> GetEditedData = {});
void WrapMapCommand(TSharedRef<FUICommandList> List, TSharedPtr<const FUICommandInfo> Command, TFunction<UWorld*()> GetWorld, bool bSaveAs);
void RestoreCommands();
}
