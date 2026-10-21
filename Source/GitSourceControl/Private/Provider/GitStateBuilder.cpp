// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#include "GitStateBuilder.h"

namespace GitProvider
{
	TSet<FString> CollectRenameSources(const GitCore::FStatusSnapshot& Status)
	{
		TSet<FString> Sources;
		for (const GitCore::FStatusEntry& Entry : Status.Entries)
		{
			if (!Entry.OriginalPath.IsEmpty())
			{
				Sources.Add(Entry.OriginalPath);
			}
		}
		return Sources;
	}

	void BuildState(const FStateContext& Context, const FString& RelativePath, bool bFileExists, FGitSourceControlState& State)
	{
		const GitCore::FSessionSnapshot& Snapshot = *Context.Snapshot;
		State.RelativePath = RelativePath;
		State.FileState = EGitFileState::Unknown;
		State.bStaged = false;
		State.bLockable = false;
		State.LockState = EGitLockState::NotLockable;
		State.LockOwner.Reset();
		State.LockId.Reset();
		State.bLockInfoStale = false;
		State.bNewerOnServer = false;
		State.Upstream = Snapshot.Remote.Upstream;

		if (RelativePath.IsEmpty())
		{
			State.FileState = EGitFileState::NotInRepository;
			State.TimeStamp = Context.Now;
			return;
		}
		if (!Snapshot.bStatusValid)
		{
			// Unknown states must look old so the editor asks again once status is available.
			State.TimeStamp = FDateTime::MinValue();
			return;
		}

		if (const GitCore::FStatusEntry* Entry = Snapshot.Status.Find(RelativePath))
		{
			State.bStaged = Entry->HasStaged();
			if (Entry->bConflict)
			{
				State.FileState = EGitFileState::Conflicted;
			}
			else if (Entry->bUntracked)
			{
				State.FileState = Snapshot.MarkedForAdd.Contains(RelativePath) ? EGitFileState::MarkedForAdd : EGitFileState::Untracked;
			}
			else if (Entry->IsDeleted())
			{
				State.FileState = EGitFileState::Deleted;
			}
			else if (Entry->Index == TEXT('A') || Entry->Index == TEXT('R') || Entry->Index == TEXT('C'))
			{
				State.FileState = EGitFileState::Added;
			}
			else
			{
				State.FileState = EGitFileState::Modified;
			}
		}
		else if (Context.RenameSources && Context.RenameSources->Contains(RelativePath))
		{
			State.FileState = EGitFileState::Deleted;
		}
		else if (Snapshot.IsTracked(RelativePath))
		{
			State.FileState = EGitFileState::Unchanged;
		}
		else
		{
			// Git reports every untracked file that is not ignored, so an existing file
			// missing from status is ignored; a missing file is about to be created.
			State.FileState = bFileExists ? EGitFileState::Ignored : EGitFileState::Untracked;
		}

		const GitCore::FFileAttributes* Attributes = Snapshot.FindAttributes(RelativePath);
		State.bLockable = Context.bLockingEnabled && Attributes && Attributes->bLockable;
		if (State.bLockable)
		{
			const GitCore::FLockSnapshot& Locks = Snapshot.Locks;
			State.LockCheckedAtUtc = Locks.VerifiedAtUtc;
			State.bLockInfoStale = !Locks.bVerified || !Locks.Error.IsEmpty() || Locks.AgeSeconds() > Context.LockStaleSeconds;
			if (const GitCore::FLockInfo* Lock = Locks.Find(RelativePath))
			{
				State.LockId = Lock->Id;
				State.LockOwner = Lock->Owner;
				if (!Lock->bOurs)
				{
					State.LockState = EGitLockState::LockedByOther;
				}
				else
				{
					State.LockState = Snapshot.LocksTakenHere.Contains(Lock->Id) ? EGitLockState::LockedHere : EGitLockState::LockedElsewhere;
				}
			}
			else
			{
				State.LockState = Locks.bVerified ? EGitLockState::NotLocked : EGitLockState::Unknown;
			}
		}
		State.bNewerOnServer = Snapshot.Remote.IncomingPaths.Contains(RelativePath);
		State.TimeStamp = Context.Now;
	}
}
