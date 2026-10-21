// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#pragma once

#include "GitTypes.h"
#include "Process/GitProcess.h"

namespace GitCore
{
	/** Decides timeouts and whether credential prompts may appear. */
	enum class ECallKind : uint8
	{
		/** Quick local reads such as rev-parse or config. */
		LocalQuick,
		/** Local reads that scale with repository size (status, ls-files, log, diff). */
		LocalScan,
		/** Local index/worktree changes (add, restore, update-ref). */
		LocalMutate,
		/** Commands that run user hooks (commit), which may be silent for a long time. */
		Hooked,
		/** Fetch or lock checks started by the editor in the background: never prompts. */
		BackgroundNetwork,
		/** Network work the user asked for (fetch, push, lock): may show a credential window. */
		UserNetwork,
		/** Commands that rewrite the worktree from the network or history (merge, rebase, LFS fetch). */
		Integrate,
		/** Exporting one file revision (may download an LFS object). */
		Blob
	};

	struct FGitCall
	{
		ECallKind Kind = ECallKind::LocalScan;
		TArray<FString> Args;
		TArray<uint8> Stdin;
		/** Lets git refresh the index stat cache (used by the serialized status refresh). */
		bool bOptionalLocks = false;
		/** Treats pathspecs literally. Git LFS commands must turn this off: they match internally with wildcards. */
		bool bLiteralPathspecs = true;
		FCancelTokenPtr Cancel;
		TFunction<bool(const uint8* Data, int32 Size)> StdoutSink;
	};

	/** Outcome of a repository operation with a short user-facing message and raw details for the log. */
	struct FOpResult
	{
		bool bOk = false;
		FString Message;
		FString Details;

		static FOpResult Success(FString InMessage = FString())
		{
			FOpResult Result;
			Result.bOk = true;
			Result.Message = MoveTemp(InMessage);
			return Result;
		}
		static FOpResult Failure(FString InMessage, FString InDetails = FString())
		{
			FOpResult Result;
			Result.Message = MoveTemp(InMessage);
			Result.Details = MoveTemp(InDetails);
			return Result;
		}
	};

	/** Versions reported by `git version` and `git lfs version`; empty when the tool is missing. */
	struct FToolVersions
	{
		FString Git;
		FString Lfs;

		/** Git is new enough for every command this plugin runs (merge-tree --write-tree needs 2.38). */
		bool IsGitSupported() const;
	};

	/**
	 * Git commands for one worktree. Thread-safe for concurrent reads; callers serialize
	 * mutations (the repository session does). Never call from the game thread.
	 */
	class FGitRepository
	{
	public:
		/** Opens the worktree containing Path. Returns null with OutError when Path is not inside a repository. */
		static TSharedPtr<FGitRepository, ESPMode::ThreadSafe> Open(const FString& GitBinary, const FString& Path, FString& OutError);
		/** Asks the git executable (and git-lfs next to it) for their versions. */
		static FToolVersions ReadToolVersions(const FString& GitBinary);

		const FString& GetGitBinary() const { return GitBinary; }
		/** Absolute worktree root with forward slashes and no trailing slash. */
		const FString& GetRoot() const { return Root; }
		const FString& GetGitDir() const { return GitDir; }
		const FString& GetCommonDir() const { return CommonDir; }

		FProcessResult Run(const FGitCall& Call) const;
		FProcessResult Run(ECallKind Kind, TArray<FString> Args) const;
		/** Runs with paths passed through stdin as NUL-separated pathspecs (`--pathspec-from-file=- --pathspec-file-nul`). */
		FProcessResult RunWithPathspecs(ECallKind Kind, TArray<FString> Args, const TArray<FString>& Paths) const;

		/** Repository-relative path for an absolute filename, or empty when outside this worktree. */
		FString ToRelative(const FString& AbsolutePath) const;
		/** Absolute filename, spelled the way the repository was opened (the editor's spelling behind symlinks). */
		FString ToAbsolute(const FString& RelativePath) const;

		/** Turns a failed process into a short message, recognizing credential and lock-file problems. */
		static FString DescribeFailure(const FProcessResult& Result, const FString& What);

		// Status (GitRepositoryStatus.cpp)
		bool ReadStatus(FStatusSnapshot& Out, FString& OutError, bool bAllowIndexRefresh, const TArray<FString>* OnlyPaths = nullptr) const;
		bool ReadTrackedPaths(TSet<FString>& Out, FString& OutError) const;
		bool ReadAttributes(const TArray<FString>& Paths, TMap<FString, FFileAttributes>& Out, FString& OutError) const;
		FString ReadIndexToken() const;
		bool IsOperationInProgress() const;

		// Index and worktree changes (GitRepositoryChanges.cpp)
		FOpResult Stage(const TArray<FString>& Paths) const;
		FOpResult Unstage(const TArray<FString>& Paths) const;
		/** Commits exactly the current index. Fails if HEAD or the staged content changed since they were reviewed (FStatusSnapshot::StagedToken). */
		FOpResult CommitIndex(const FString& Message, const FString& ReviewedHead, const FString& ReviewedStagedToken, FString& OutCommit) const;
		/** Commits the working-tree content of Paths only, leaving other staged entries staged. */
		FOpResult CommitPaths(const TArray<FString>& Paths, const TArray<FString>& NewPaths, const FString& Message, FString& OutCommit) const;
		/** Restores Paths in the index and working tree to HEAD. Paths absent from HEAD are unstaged and deleted. */
		FOpResult RestorePaths(const TArray<FString>& Paths) const;
		FOpResult Diff(const FString& Path, bool bStaged, FString& OutText) const;

		// LFS locks (GitRepositoryLocks.cpp)
		bool ResolveLockEndpoint(const FString& Remote, FString& OutEndpoint, FString& OutError) const;
		/** Verifies locks against the server into Out (Out keeps its previous locks when verification fails). */
		bool VerifyLocks(const FString& Remote, ECallKind Kind, FLockSnapshot& Out) const;
		FOpResult Lock(const FString& Remote, const FString& Path, FLockInfo& OutLock) const;
		FOpResult Unlock(const FString& Remote, const FString& LockId, bool bForce) const;

		// Remotes (GitRepositoryRemote.cpp)
		bool ReadRemotes(TArray<FString>& Out) const;
		/** The branch's upstream remote, else "origin", else the only remote; empty when none exist. */
		FString DefaultRemote() const;
		FOpResult Fetch(const FString& Remote, ECallKind Kind, const FCancelTokenPtr& Cancel) const;
		/** Compares HEAD with its upstream using local remote-tracking refs. */
		bool ReadRemoteState(const FStatusSnapshot& Status, FRemoteState& Out, FString& OutError) const;
		/** Uploads LFS objects then pushes HEAD to the upstream (or to Remote/Branch with -u when there is none). */
		FOpResult Push(const FStatusSnapshot& Status, const FString& FallbackRemote, const FCancelTokenPtr& Cancel) const;
		/** True when merging Upstream into HEAD would conflict. */
		bool PredictConflicts(const FString& Upstream, bool& bOutConflicts, FString& OutError) const;
		/** Fast-forwards to Upstream, or rebases local commits onto it when bRebase. Aborts cleanly on failure. */
		FOpResult Integrate(const FString& Upstream, bool bRebase, const FCancelTokenPtr& Cancel) const;
		bool ChangedPaths(const FString& From, const FString& To, TSet<FString>& Out, FString& OutError) const;
		/** Downloads the LFS objects Ref needs, before HEAD moves, so a failed download changes nothing. */
		FOpResult FetchLfsObjects(const FString& Remote, const FString& Ref, const FCancelTokenPtr& Cancel) const;
		/** True when a commit not yet on any remote changes Path (so releasing its lock would be premature). */
		bool HasUnpublishedChanges(const FString& Path, bool& bOutUnpublished, FString& OutError) const;

		// History (GitRepositoryHistory.cpp)
		bool ReadHistory(int32 Limit, TArray<FHistoryCommit>& Out, bool& bOutHasMore, FString& OutError) const;
		bool ReadCommit(const FString& Oid, FCommitDetails& Out, FString& OutError) const;
		bool ReadFileHistory(const FString& Path, int32 Limit, TArray<FFileRevision>& Out, FString& OutError) const;
		/** Writes the filtered (LFS-smudged) bytes of Path at Revision to Destination. */
		FOpResult ExportRevision(const FString& Revision, const FString& Path, const FString& Destination) const;
		/** The text diff of Path in commit Oid against its first parent. */
		FOpResult CommitDiff(const FString& Oid, const FString& Path, FString& OutText) const;

		// Stashes (GitRepositoryStash.cpp)
		bool ReadStashes(TArray<FStashEntry>& Out, FString& OutError) const;
		bool ReadStashPaths(const FString& Selector, TArray<FString>& Out, FString& OutError) const;
		FOpResult StashPush(const TArray<FString>& Paths, const FString& Message, bool bIncludeUntracked) const;
		FOpResult StashApply(const FString& Selector, bool bRestoreIndex, bool bDrop) const;
		FOpResult StashDrop(const FString& Selector) const;

		// Branches (GitRepositoryBranches.cpp)
		bool ReadBranches(TArray<FBranchInfo>& Out, FString& OutError) const;
		FOpResult CreateBranch(const FString& Name) const;
		FOpResult SwitchBranch(const FString& Name) const;

	private:
		FGitRepository() = default;
		void AddEnvironment(FProcessSpec& Spec, ECallKind Kind) const;

		FString GitBinary;
		FString Root;
		FString GitDir;
		FString CommonDir;
		/** The editor's spelling of Root when it reaches the repository through a symlink. */
		FString AliasRoot;
		/** True when the user configured their own ssh command; we then never force BatchMode. */
		bool bUserSshCommand = false;
	};

	using FGitRepositoryPtr = TSharedPtr<FGitRepository, ESPMode::ThreadSafe>;
	using FGitRepositoryRef = TSharedRef<FGitRepository, ESPMode::ThreadSafe>;

	/** Finds a git executable: the configured path, common install locations, then PATH. */
	FString FindGitExecutable(const FString& ConfiguredPath = FString());
}
