// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#pragma once

#include "Core/GitSession.h"
#include "GitSourceControlState.h"

namespace GitProvider
{
	/** Inputs shared by every state built from one snapshot. */
	struct FStateContext
	{
		const GitCore::FSessionSnapshot* Snapshot = nullptr;
		/** Old paths of staged renames; those files read as deleted. */
		const TSet<FString>* RenameSources = nullptr;
		/** Locking is configured and turned on in the connection settings. */
		bool bLockingEnabled = false;
		/** Lock information older than this is shown with its age. */
		double LockStaleSeconds = 900.0;
		FDateTime Now;
	};

	/** Old paths of renamed or copied entries, so they can be reported as deleted. */
	TSet<FString> CollectRenameSources(const GitCore::FStatusSnapshot& Status);

	/**
	 * Fills State's git-derived fields for RelativePath (empty when outside the repository).
	 * bFileExists distinguishes ignored files from paths that do not exist yet.
	 */
	void BuildState(const FStateContext& Context, const FString& RelativePath, bool bFileExists, FGitSourceControlState& State);
}
