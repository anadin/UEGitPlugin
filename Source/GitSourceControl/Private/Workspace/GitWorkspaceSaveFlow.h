// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#pragma once
#include "GitWorkspaceRepository.h"
#include "Framework/Commands/UIAction.h"
class UPackage;
class FUICommandList;
class FUICommandInfo;
class ISourceControlProvider;

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
#if PLATFORM_MAC
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
void RestoreCommands();
}
