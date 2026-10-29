// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#pragma once

#include "CoreMinimal.h"
#include "AssetRegistry/AssetData.h"
#include "Core/GitSession.h"

class FGitSourceControlProvider;

/** What an action did: shown as a notification and, when it failed, in the Git panel's banner. */
struct FGitActionResult
{
	bool bOk = true;
	FText Message;
	FString Details;
	/** Repository paths the action needs or touched (passed between the steps of switches and stashes). */
	TArray<FString> Paths;
	/** The action left commits to publish: the notification offers Push. */
	bool bOfferPush = false;

	static FGitActionResult Success(const FText& InMessage = FText::GetEmpty());
	static FGitActionResult Failure(const FText& InMessage, const FString& InDetails = FString());
	static FGitActionResult FromOp(const GitCore::FOpResult& Op);
};

/**
 * The editor's Git actions, shared by the status bar, the menus and the Git panel.
 * Game thread only. Git work runs on the repository session's lanes, one action at a
 * time; the session keeps refreshing status underneath.
 */
class FGitWorkspaceModel : public TSharedFromThis<FGitWorkspaceModel>
{
public:
	/** Null in commandlets and before the editor integration registers. */
	static FGitWorkspaceModel* Get();
	static void Startup();
	static void Shutdown();
	/** A model bound to one provider instead of the editor's (tests). Reload steps run inline; the provider must outlive the model. */
	static TSharedRef<FGitWorkspaceModel> CreateFor(FGitSourceControlProvider& InProvider);

	FGitWorkspaceModel() = default;
	~FGitWorkspaceModel();

	/** Git Workspace is the editor's revision control provider and found a repository. */
	bool IsActive() const;
	GitCore::FGitSessionPtr GetSession() const;
	GitCore::FSessionSnapshotRef GetSnapshot() const;
	bool IsLockingEnabled() const;

	/** Broadcast after the repository state or the running action changed. */
	DECLARE_MULTICAST_DELEGATE(FOnChanged);
	FOnChanged& OnChanged() { return Changed; }

	bool IsBusy() const;
	const FText& GetActivity() const { return Activity; }
	void Cancel();
	/** The last failure, until dismissed or until the next action starts. */
	const TOptional<FGitActionResult>& GetLastFailure() const { return LastFailure; }
	void DismissFailure();

	void Fetch();
	void Push();
	/** Unreal's Sync: saves, integrates incoming commits and reloads the changed packages. */
	void Sync();
	/** Unreal's Submit Content dialog. */
	void CheckIn();
	void CheckLocks();

	void Stage(const TArray<FString>& Paths);
	void Unstage(const TArray<FString>& Paths);
	/** Commits the index as the user last saw it; refuses if HEAD or the index changed since. */
	void Commit(const FString& Message, bool bPush, TFunction<void(bool bCommitted)> OnDone = nullptr);
	/** Discards local changes. Packages are reverted through the editor, which reloads them. */
	void Discard(const TArray<FString>& Paths);
	/** Unreal's Check Out: takes the LFS locks. */
	void Lock(const TArray<FString>& Paths);
	/** Releases locks held on Paths; bForce also releases other people's locks. */
	void Unlock(const TArray<FString>& Paths, bool bForce);
	/**
	 * Takes over your locks on Paths that were taken in another clone, worktree or computer, so the
	 * files can be saved and checked in here. Nothing is sent to the server.
	 */
	void ClaimLocks(const TArray<FString>& Paths);

	void CreateBranch(const FString& Name);
	void SwitchBranch(const FString& Name);
	void StashChanges(const FString& Message, bool bIncludeUntracked);
	void ApplyStash(const FString& Selector, bool bDrop);
	void DropStash(const FString& Selector);

	/** Reads history on the session's read lane; OnLoaded runs on the game thread. */
	void LoadHistory(int32 Limit, TFunction<void(TArray<GitCore::FHistoryCommit>&& Commits, bool bHasMore, const FString& Error)> OnLoaded);
	void LoadCommit(const FString& Oid, TFunction<void(GitCore::FCommitDetails&& Details, const FString& Error)> OnLoaded);
	void LoadStashPaths(const FString& Selector, TFunction<void(TArray<FString>&& Paths, const FString& Error)> OnLoaded);
	/** A changed file's text diff, staged or unstaged side; a new file shows its whole content. OnLoaded runs on the game thread. */
	void LoadDiff(const FString& RelativePath, bool bStaged, TFunction<void(FString&& Diff, const FString& Error)> OnLoaded);
	/** How a file changed in a commit, as a text diff. OnLoaded runs on the game thread. */
	void LoadCommitDiff(const FString& Oid, const FString& RelativePath, TFunction<void(FString&& Diff, const FString& Error)> OnLoaded);

	FString ToAbsolute(const FString& RelativePath) const;
	void OpenFile(const FString& RelativePath);
	/** What Show in Content Browser selects for these repository files and folders: assets, and Content Browser folders. */
	void GetBrowseTargets(const TArray<FString>& RelativePaths, TArray<FAssetData>& OutAssets, TArray<FString>& OutFolders) const;
	bool CanBrowseTo(const TArray<FString>& RelativePaths) const;
	/**
	 * Selects in the Content Browser: assets and folders under a mounted content folder, a World
	 * Partition actor's level, and the folder of an asset that no longer exists. Other files are left out.
	 */
	void BrowseTo(const TArray<FString>& RelativePaths);
	/** What Show in Finder / Explorer opens: absolute files (shown selected) and folders, one per folder. */
	TArray<FString> GetRevealTargets(const TArray<FString>& RelativePaths) const;
	bool CanReveal(const TArray<FString>& RelativePaths) const;
	/** Shows files and folders in Finder / Explorer, one window per folder; a file that is gone shows the folder it was in. */
	void Reveal(const TArray<FString>& RelativePaths);
	/** "Show in Finder", "Show in Explorer" or the platform's file manager. */
	static FText RevealLabel();
	/** Asset diff against the last commit, or a text diff (staged or unstaged side). */
	void ShowDiff(const FString& RelativePath, bool bStaged);
	void ShowHistory(const FString& RelativePath);
	/** How a file changed in a commit: an asset diff against the parent, or the commit's text diff. */
	void ShowCommitDiff(const GitCore::FHistoryCommit& Commit, const GitCore::FHistoryChange& Change);

private:
	enum class ELane : uint8
	{
		Mutation,
		Background
	};
	using FWork = TUniqueFunction<FGitActionResult(GitCore::FGitSession& Session, const GitCore::FCancelTokenPtr& Cancel)>;
	using FDone = TUniqueFunction<void(const FGitActionResult& Result)>;

	FGitSourceControlProvider& GetProvider() const;
	/** Runs Task from the main loop, outside Slate input handling and task callbacks: package reloads need that. */
	void RunAtSafePoint(TFunction<void()> Task) const;
	void OnRepositoryChanged();
	/** Runs Work on a session lane and reports its result. False when nothing could start. */
	bool Run(const FText& InActivity, ELane Lane, FWork Work, FDone Done = nullptr, bool bQuietSuccess = false);
	/** Quick local work (staging) that queues behind a running action instead of waiting for it; only failures are reported. */
	void RunQuick(FWork Work);
	void Finish(const FGitActionResult& Result, bool bQuietSuccess);
	void Present(const FGitActionResult& Result);
	/**
	 * Second step of branch switches and stashes: runs Op on the mutation lane while the editor
	 * unloads, then reloads, the packages of Paths (the world too when one of its packages is affected).
	 */
	void ApplyAndReload(const FText& InActivity, const TArray<FString>& Paths, FWork Op);
	void ShowTextDiff(const FText& Title, const FString& Diff, const FString& Error) const;

	FGitSourceControlProvider* BoundProvider = nullptr;
	FDelegateHandle RepositoryChangedHandle;
	FOnChanged Changed;
	FText Activity;
	TWeakPtr<GitCore::FGitSession, ESPMode::ThreadSafe> ActivitySession;
	GitCore::FCancelTokenPtr ActivityCancel;
	TOptional<FGitActionResult> LastFailure;
};
