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

namespace GitWorkspaceSave
{
bool HandlesProvider(const ISourceControlProvider& Provider);
void Register();
void Unregister();
void SaveDirtyAssets();
FString LockRemote();
void SetLockRemote(const FString& Remote);
// Public in this private module for fixture acceptance tests and lifecycle checks.
void InstallGuard();
void RemoveGuard();
struct FPackageSavePaths
{
    TArray<FString> Paths, NewPaths;
    FString Error;
};
FPackageSavePaths GatherPackageSavePaths(const TArray<UPackage*>& Packages, const FString& Root, const FString& Content);
// Save As is a copy into an absent destination; the source is never renamed or saved.
struct FAssetCopyDestination
{
    FString PackageName, Path, Filename, Error;
};
FAssetCopyDestination ReviewAbsentDestination(const FString& PackageName, const FString& SourcePackage, const FString& Root, const FString& Content, bool bMap);
// Ordinary persistent maps only. First Save names the live world; Save As copies it.
struct FMapSaveDestination
{
    FAssetCopyDestination Map, BuildData;
    FString SourcePackage, Error;
    TWeakObjectPtr<UObject> SourceBuildData;
    bool bNameCurrent = false;
    TArray<FString> Paths() const { TArray<FString> Out{Map.Path}; if (!BuildData.Path.IsEmpty()) Out.Add(BuildData.Path); return Out; }
};
FString ReviewMapSource(UWorld* World);
FMapSaveDestination ReviewMapDestination(UWorld* World, const FString& PackageName, const FString& Root, const FString& Content);
bool SupportsAssetCopy(const UObject* Source);
UObject* GetCopySource(const FAssetEditorToolkit& Editor);
UObject* GetCopyData(FAssetEditorToolkit& Editor);
// Material editors hold unapplied graph edits in a separate transient preview.
FString ReviewCopyData(UObject* Source, UObject* EditedData);
FAssetCopyDestination ReviewCopyDestination(UObject* Source, const FString& PackageName, const FString& Root, const FString& Content);
#if PLATFORM_MAC
GitWorkspace::FResult WriteAssetCopy(UObject* Source, const FAssetCopyDestination& Destination, GitWorkspace::FRepository& Repository,
    const GitWorkspace::FAssetSavePermit& Permit, const GitWorkspaceSession::FLease& Lease, const FString& Content, UObject*& OutCopy, UObject* EditedData = nullptr);
GitWorkspace::FResult WriteMapDestination(UWorld* Source, const FMapSaveDestination& Destination, GitWorkspace::FRepository& Repository,
    const GitWorkspace::FAssetSavePermit& Permit, const GitWorkspaceSession::FLease& Lease, const FString& Content, UWorld*& OutWorld);
class FPreparedScope
{
public:
    FPreparedScope(GitWorkspace::FRepository& Repository, const GitWorkspace::FAssetSavePermit& Permit, const GitWorkspaceSession::FLease& Lease, const FString& Root);
    ~FPreparedScope();
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
