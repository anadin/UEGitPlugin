// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#pragma once

#include "GitTypes.h"

namespace GitCore
{
	/**
	 * Remembers which server locks this worktree took. Git LFS locks belong to a user,
	 * not a clone; the journal lets the editor tell "locked by you here" from "locked by
	 * you in another clone" without blocking either. Records survive branch switches and
	 * configuration edits because they are keyed only by endpoint and lock id.
	 *
	 * Layout: <git-common-dir>/uegit/locks-v2/<sha1(endpoint)>/<lock id>.json
	 */
	class FGitLockJournal
	{
	public:
		FGitLockJournal(FString InCommonDir, FString InWorktreeRoot);

		bool Record(const FString& Endpoint, const FLockInfo& Lock, FString& OutError) const;
		void Remove(const FString& Endpoint, const FString& LockId) const;
		/** True when this worktree recorded taking the lock. */
		bool IsOurs(const FString& Endpoint, const FString& LockId) const;
		/** Drops records whose locks are no longer held by the current user on the server. */
		void Prune(const FString& Endpoint, const TSet<FString>& HeldLockIds) const;

	private:
		FString Directory(const FString& Endpoint) const;
		FString CommonDir;
		FString WorktreeRoot;
	};
}
