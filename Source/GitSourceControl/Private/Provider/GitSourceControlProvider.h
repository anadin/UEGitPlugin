// Copyright (c) 2014-2020 Sebastien Rombauts (sebastien.rombauts@gmail.com)
//
// Distributed under the MIT License (MIT) (See accompanying file LICENSE.txt
// or copy at http://opensource.org/licenses/MIT)

#pragma once

#include "CoreMinimal.h"
#include "ISourceControlProvider.h"
#include "Core/GitSession.h"
#include "GitSourceControlCommand.h"
#include "GitSourceControlSettings.h"
#include "GitSourceControlState.h"

class UPackage;
class FObjectPostSaveContext;

/**
 * Unreal's revision control provider for Git and Git LFS. Unreal's own flows drive the
 * work: Check Out takes an LFS lock, Submit commits and pushes, Sync pulls, Revert discards.
 * All git work runs on the repository session's threads; states come from its snapshots.
 */
class FGitSourceControlProvider final : public ISourceControlProvider
{
public:
	FGitSourceControlProvider();
	/** A provider for one folder that neither reads nor writes the editor's saved settings. Used by tests. */
	explicit FGitSourceControlProvider(const FString& InRepositoryFolder);
	virtual ~FGitSourceControlProvider() override;

	// ISourceControlProvider
	virtual void Init(bool bForceConnection = true) override;
	virtual void Close() override;
	virtual const FName& GetName() const override;
	virtual FText GetStatusText() const override;
	virtual TMap<EStatus, FString> GetStatus() const override;
	virtual bool IsEnabled() const override;
	virtual bool IsAvailable() const override;
	virtual bool QueryStateBranchConfig(const FString& ConfigSrc, const FString& ConfigDest) override { return false; }
	virtual void RegisterStateBranches(const TArray<FString>& BranchNames, const FString& ContentRoot) override {}
	virtual int32 GetStateBranchIndex(const FString& BranchName) const override { return INDEX_NONE; }
	virtual bool GetStateBranchAtIndex(int32 BranchIndex, FString& OutBranchName) const override { return false; }
	virtual ECommandResult::Type GetState(const TArray<FString>& InFiles, TArray<FSourceControlStateRef>& OutState, EStateCacheUsage::Type InStateCacheUsage) override;
	virtual ECommandResult::Type GetState(const TArray<FSourceControlChangelistRef>& InChangelists, TArray<FSourceControlChangelistStateRef>& OutState, EStateCacheUsage::Type InStateCacheUsage) override;
	virtual TArray<FSourceControlStateRef> GetCachedStateByPredicate(TFunctionRef<bool(const FSourceControlStateRef&)> Predicate) const override;
	virtual FDelegateHandle RegisterSourceControlStateChanged_Handle(const FSourceControlStateChanged::FDelegate& SourceControlStateChanged) override;
	virtual void UnregisterSourceControlStateChanged_Handle(FDelegateHandle Handle) override;
	virtual ECommandResult::Type Execute(const FSourceControlOperationRef& InOperation, FSourceControlChangelistPtr InChangelist, const TArray<FString>& InFiles,
		EConcurrency::Type InConcurrency = EConcurrency::Synchronous, const FSourceControlOperationComplete& InOperationCompleteDelegate = FSourceControlOperationComplete()) override;
	virtual bool CanExecuteOperation(const FSourceControlOperationRef& InOperation) const override;
	virtual bool CanCancelOperation(const FSourceControlOperationRef& InOperation) const override;
	virtual void CancelOperation(const FSourceControlOperationRef& InOperation) override;
	virtual TArray<TSharedRef<class ISourceControlLabel>> GetLabels(const FString& InMatchingSpec) const override { return {}; }
	virtual TArray<FSourceControlChangelistRef> GetChangelists(EStateCacheUsage::Type InStateCacheUsage) override { return {}; }
	virtual bool UsesLocalReadOnlyState() const override { return false; }
	virtual bool UsesChangelists() const override { return false; }
	virtual bool UsesUncontrolledChangelists() const override { return false; }
	virtual bool UsesCheckout() const override;
	virtual bool UsesFileRevisions() const override { return true; }
	virtual bool UsesSnapshots() const override { return false; }
	virtual bool UsesSoftRevertOnDelete() const override { return true; }
	virtual bool AllowsDiffAgainstDepot() const override { return true; }
	virtual TOptional<bool> HasChangesToSync() const override;
	virtual TOptional<bool> HasChangesToCheckIn() const override;
	virtual void Tick() override;
#if SOURCE_CONTROL_WITH_SLATE
	virtual TSharedRef<class SWidget> MakeSettingsWidget() const override;
#endif
	using ISourceControlProvider::Execute;
	using ISourceControlProvider::GetState;

	FGitSourceControlSettings& AccessSettings() { return Settings; }
	const FGitSourceControlSettings& AccessSettings() const { return Settings; }
	GitCore::FGitSessionPtr GetSession() const { return Session; }
	/** Opens the session for the project's repository. Safe to call repeatedly. */
	bool OpenSession(FText* OutError = nullptr);
	/** Locking is turned on and the repository marks files `lockable`. */
	bool IsLockingEnabled() const;
	/** Attaches file history read by an UpdateStatus operation. */
	void SetHistory(const FString& AbsoluteFile, const TArray<GitCore::FFileRevision>& Revisions);

	/** Fired on the game thread after the repository state changed (status bar, panel). */
	DECLARE_MULTICAST_DELEGATE(FOnRepositoryChanged);
	FOnRepositoryChanged& OnRepositoryChanged() { return RepositoryChanged; }

private:
	FGitCommandSettings MakeCommandSettings() const;
	FGitSourceControlStateRef GetStateInternal(const FString& AbsoluteFile);
	void RebuildState(FGitSourceControlState& State, const GitCore::FSessionSnapshot& Snapshot) const;
	void RefreshCachedStates();
	void OnSessionChanged(GitCore::EChange Changes);
	void OnPackageSaved(const FString& PackageFilename, UPackage* Package, FObjectPostSaveContext Context);
	ECommandResult::Type WaitForCommand(const FGitSourceControlCommandRef& Command);
	void FinishCommand(FGitSourceControlCommand& Command);
	void TickScheduler();

	/** Empty for the editor's provider, which serves the project folder. */
	FString RepositoryFolder;
	FGitSourceControlSettings Settings;
	GitCore::FToolVersions ToolVersions;
	GitCore::FGitSessionPtr Session;
	FDelegateHandle SessionChangedHandle;
	FDelegateHandle PackageSavedHandle;

	mutable FCriticalSection StateCacheLock;
	TMap<FString, FGitSourceControlStateRef> StateCache;
	uint64 CachedGeneration = 0;
	TSet<FString> RenameSources;

	TArray<FGitSourceControlCommandRef> PendingCommands;
	FSourceControlStateChanged OnSourceControlStateChanged;
	FOnRepositoryChanged RepositoryChanged;

	double LastStatusRequest = 0.0;
	double LastLockRequest = 0.0;
	double LastFetchRequest = 0.0;
	double PackageSavedAt = 0.0;
	bool bWasForeground = true;
};
