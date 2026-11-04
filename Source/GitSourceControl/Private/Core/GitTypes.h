// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#pragma once

#include "CoreMinimal.h"

namespace GitCore
{
	/** One path reported by `git status --porcelain=v2`. Paths are repository-relative with forward slashes. */
	struct FStatusEntry
	{
		FString Path;
		/** Source path of a rename or copy. */
		FString OriginalPath;
		/** Index (staged) and working-tree codes from the XY field; '.' means unchanged. */
		TCHAR Index = TEXT('.');
		TCHAR Working = TEXT('.');
		/** Mode and object id of the staged version (all zeros when the index deletes the path). */
		FString IndexMode;
		FString IndexOid;
		bool bUntracked = false;
		bool bConflict = false;
		bool bSubmodule = false;

		bool HasStaged() const { return !bUntracked && !bConflict && Index != TEXT('.'); }
		bool HasUnstaged() const { return bUntracked || bConflict || Working != TEXT('.'); }
		bool IsDeleted() const { return Index == TEXT('D') || Working == TEXT('D'); }
		bool IsAdded() const { return bUntracked || Index == TEXT('A'); }
	};

	/** A consistent read of HEAD, branch and changed paths. */
	struct FStatusSnapshot
	{
		/** Branch name, or empty when HEAD is detached. */
		FString Branch;
		/** Current commit id, or empty for an unborn branch. */
		FString Head;
		/** Upstream such as "origin/main", or empty when none is configured. */
		FString Upstream;
		/** Commits ahead of / behind the upstream tracking ref; -1 when unknown. */
		int32 Ahead = -1;
		int32 Behind = -1;
		bool bUnborn = false;
		bool bOperationInProgress = false;
		TArray<FStatusEntry> Entries;
		/** Identity of the index file; changes whenever the index file is rewritten, even without new content. */
		FString IndexToken;
		/** Identity of HEAD plus the staged content: what a commit of the index would contain. */
		FString StagedToken;

		const FStatusEntry* Find(const FString& Path) const;
		/** Computes StagedToken from HEAD and the entries' staged object ids. */
		FString ComputeStagedToken() const;
		bool HasConflicts() const;
		int32 StagedCount() const;
		void BuildLookup();

	private:
		TMap<FString, int32> ByPath;
	};

	struct FFileAttributes
	{
		bool bLfs = false;
		bool bLockable = false;
	};

	struct FLockInfo
	{
		FString Id;
		FString Path;
		FString Owner;
		FDateTime LockedAt;
		bool bOurs = false;
	};

	/** Server lock state for one remote, as last verified. Never discarded on failure; Error explains why it is old. */
	struct FLockSnapshot
	{
		FString Remote;
		FString Endpoint;
		TMap<FString, FLockInfo> ByPath;
		FDateTime VerifiedAtUtc;
		bool bVerified = false;
		FString Error;

		const FLockInfo* Find(const FString& Path) const { return ByPath.Find(Path); }
		/** Seconds since the last successful verification; very large when never verified. */
		double AgeSeconds() const;
	};

	/** Upstream comparison based on local remote-tracking refs (updated by fetch). */
	/** Where a push of the current branch goes. */
	struct FPushTarget
	{
		/** Empty when there is nowhere to push. */
		FString Remote;
		/** The branch's name on Remote: always its own name, unless push.default says upstream. */
		FString Branch;
		/** The branch follows Remote/Branch after the push. */
		bool bSetUpstream = false;
		/** Remote/Branch exists, as far as the last fetch knows. */
		bool bExists = false;
		/** The upstream the branch follows now, when the push goes to a branch with another name instead. */
		FString PreviousUpstream;

		/** The push would create a branch on the server, or change the branch this one follows: ask first. */
		bool NeedsAgreement() const { return !Remote.IsEmpty() && (!bExists || !PreviousUpstream.IsEmpty()); }
	};

	/** How a pull brings in commits: from where, and what happens to local commits and changes. */
	struct FPullOptions
	{
		/** The remote branch to pull, such as origin and main; empty: the branch's upstream. */
		FString Remote;
		FString Branch;
		/** Local commits are replayed on top of the pulled ones; off: both are joined by a merge commit. */
		bool bRebase = true;
		/** Local changes in the way are set aside and put back afterwards; off: the pull stops instead. */
		bool bStash = true;
		/**
		 * Lean branching's Sync: the branch is rebased even when its commits are already on the server, then its copy
		 * there is moved to the rebased commits (a push with lease). Commits that were not pushed stay unpushed.
		 */
		bool bSyncBranch = false;
	};

	struct FRemoteState
	{
		FString Remote;
		/** Upstream short name such as "origin/main"; empty when the branch has none. */
		FString Upstream;
		FString UpstreamHead;
		int32 Ahead = 0;
		int32 Behind = 0;
		/** Paths changed on the upstream since the merge base (what Sync would bring in). */
		TSet<FString> IncomingPaths;
		/** Where Push would send the branch. */
		FPushTarget Push;
		FDateTime FetchedAtUtc;
		bool bFetched = false;
		FString Error;
	};

	struct FHistoryCommit
	{
		FString Oid;
		FString Author;
		FString Date;
		FString Subject;
		TArray<FString> Parents;
	};

	struct FHistoryChange
	{
		FString Path;
		FString OldPath;
		FString OldOid;
		FString NewOid;
		FString OldMode;
		FString NewMode;
		TCHAR Status = TEXT('?');
	};

	struct FCommitDetails
	{
		FHistoryCommit Commit;
		FString Message;
		TArray<FHistoryChange> Files;
	};

	/** One entry of a file's history, newest first. */
	struct FFileRevision
	{
		FString Commit;
		FString Author;
		FDateTime Date;
		FString Subject;
		/** Path of the file in this commit (follows renames). */
		FString Path;
		TCHAR Action = TEXT('M');
		/** Blob size in bytes when known; -1 otherwise. */
		int64 Size = -1;
	};

	struct FStashEntry
	{
		/** Selector such as stash@{0}. */
		FString Selector;
		FString Oid;
		FString Message;
	};

	struct FBranchInfo
	{
		FString Name;
		FString Upstream;
		bool bCurrent = false;
		bool bRemote = false;
	};
}
