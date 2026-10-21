// Copyright (c) 2014-2020 Sebastien Rombauts (sebastien.rombauts@gmail.com)
//
// Distributed under the MIT License (MIT) (See accompanying file LICENSE.txt
// or copy at http://opensource.org/licenses/MIT)

#include "GitSourceControlState.h"

#include "RevisionControlStyle/RevisionControlStyle.h"
#include "Textures/SlateIcon.h"

#define LOCTEXT_NAMESPACE "GitSourceControl.State"

namespace
{
	FSlateIcon RevisionControlIcon(const TCHAR* Name)
	{
		return FSlateIcon(FRevisionControlStyleManager::GetStyleSetName(), Name);
	}
}

int32 FGitSourceControlState::GetHistorySize() const
{
	return History.Num();
}

TSharedPtr<ISourceControlRevision, ESPMode::ThreadSafe> FGitSourceControlState::GetHistoryItem(int32 HistoryIndex) const
{
	return History.IsValidIndex(HistoryIndex) ? History[HistoryIndex] : TSharedPtr<ISourceControlRevision, ESPMode::ThreadSafe>();
}

TSharedPtr<ISourceControlRevision, ESPMode::ThreadSafe> FGitSourceControlState::FindHistoryRevision(int32 RevisionNumber) const
{
	for (const TSharedRef<FGitSourceControlRevision, ESPMode::ThreadSafe>& Revision : History)
	{
		if (Revision->GetRevisionNumber() == RevisionNumber)
		{
			return Revision;
		}
	}
	return nullptr;
}

TSharedPtr<ISourceControlRevision, ESPMode::ThreadSafe> FGitSourceControlState::FindHistoryRevision(const FString& InRevision) const
{
	for (const TSharedRef<FGitSourceControlRevision, ESPMode::ThreadSafe>& Revision : History)
	{
		if (Revision->GetRevision() == InRevision || Revision->GetRevision().StartsWith(InRevision))
		{
			return Revision;
		}
	}
	return nullptr;
}

TSharedPtr<ISourceControlRevision, ESPMode::ThreadSafe> FGitSourceControlState::GetCurrentRevision() const
{
	return History.Num() > 0 ? History[0] : TSharedPtr<ISourceControlRevision, ESPMode::ThreadSafe>();
}

bool FGitSourceControlState::IsSourceControlled() const
{
	switch (FileState)
	{
	case EGitFileState::Unchanged:
	case EGitFileState::Modified:
	case EGitFileState::Deleted:
	case EGitFileState::Added:
	case EGitFileState::MarkedForAdd:
	case EGitFileState::Conflicted:
		return true;
	default:
		return false;
	}
}

bool FGitSourceControlState::IsAdded() const
{
	return FileState == EGitFileState::Added || FileState == EGitFileState::MarkedForAdd;
}

bool FGitSourceControlState::IsModified() const
{
	return FileState == EGitFileState::Modified || FileState == EGitFileState::Deleted || IsAdded() || FileState == EGitFileState::Conflicted;
}

bool FGitSourceControlState::IsCheckedOut() const
{
	if (IsAdded() || !IsSourceControlled())
	{
		return false;
	}
	// Git has no checkout step: tracked files that cannot be locked are always editable.
	return bLockable ? LockState == EGitLockState::LockedHere : true;
}

bool FGitSourceControlState::IsCheckedOutOther(FString* Who) const
{
	if (LockState != EGitLockState::LockedByOther)
	{
		return false;
	}
	if (Who)
	{
		*Who = LockOwner;
	}
	return true;
}

bool FGitSourceControlState::CanCheckout() const
{
	if (!bLockable || IsAdded() || !IsSourceControlled() || IsConflicted() || !IsCurrent())
	{
		return false;
	}
	return LockState == EGitLockState::NotLocked || LockState == EGitLockState::Unknown || LockState == EGitLockState::LockedElsewhere;
}

bool FGitSourceControlState::CanCheckIn() const
{
	if (IsAdded())
	{
		return true;
	}
	if (IsConflicted() || !IsCurrent() || !IsSourceControlled())
	{
		return false;
	}
	if (bLockable)
	{
		// A held lock can be "submitted" even unchanged: that releases it, as in Perforce.
		return LockState == EGitLockState::LockedHere;
	}
	return FileState == EGitFileState::Modified || FileState == EGitFileState::Deleted;
}

bool FGitSourceControlState::CanEdit() const
{
	return IsCheckedOut() || IsAdded() || FileState == EGitFileState::Untracked || FileState == EGitFileState::NotInRepository;
}

bool FGitSourceControlState::CanDelete() const
{
	return IsSourceControlled() && !IsAdded() && !IsDeleted() && IsCurrent() && LockState != EGitLockState::LockedByOther;
}

bool FGitSourceControlState::CanRevert() const
{
	return IsModified() || (bLockable && LockState == EGitLockState::LockedHere);
}

FSlateIcon FGitSourceControlState::GetIcon() const
{
	if (IsConflicted())
	{
		return RevisionControlIcon(TEXT("RevisionControl.Conflicted"));
	}
	if (!IsCurrent())
	{
		return RevisionControlIcon(TEXT("RevisionControl.NotAtHeadRevision"));
	}
	if (LockState == EGitLockState::LockedByOther)
	{
		return RevisionControlIcon(TEXT("RevisionControl.CheckedOutByOtherUser"));
	}
	if (IsAdded())
	{
		return RevisionControlIcon(TEXT("RevisionControl.OpenForAdd"));
	}
	if (IsDeleted())
	{
		return RevisionControlIcon(TEXT("RevisionControl.MarkedForDelete"));
	}
	if (LockState == EGitLockState::LockedHere)
	{
		return RevisionControlIcon(TEXT("RevisionControl.CheckedOut"));
	}
	if (LockState == EGitLockState::LockedElsewhere)
	{
		return RevisionControlIcon(TEXT("RevisionControl.Locked"));
	}
	if (FileState == EGitFileState::Modified)
	{
		return RevisionControlIcon(TEXT("RevisionControl.ModifiedLocally"));
	}
	if (FileState == EGitFileState::Untracked)
	{
		return RevisionControlIcon(TEXT("RevisionControl.NotInDepot"));
	}
	return FSlateIcon();
}

FText FGitSourceControlState::GetDisplayName() const
{
	if (IsConflicted())
	{
		return LOCTEXT("Conflicted", "Conflicted");
	}
	if (!IsCurrent())
	{
		return LOCTEXT("NotCurrent", "Newer version on server");
	}
	switch (LockState)
	{
	case EGitLockState::LockedByOther:
		return FText::Format(LOCTEXT("LockedByOther", "Locked by {0}"), FText::FromString(LockOwner));
	case EGitLockState::LockedHere:
		return LOCTEXT("LockedHere", "Locked by you");
	case EGitLockState::LockedElsewhere:
		return LOCTEXT("LockedElsewhere", "Locked by you elsewhere");
	default:
		break;
	}
	switch (FileState)
	{
	case EGitFileState::Added:
	case EGitFileState::MarkedForAdd:
		return LOCTEXT("Added", "Added");
	case EGitFileState::Deleted:
		return LOCTEXT("Deleted", "Deleted");
	case EGitFileState::Modified:
		return LOCTEXT("Modified", "Modified");
	case EGitFileState::Untracked:
		return LOCTEXT("Untracked", "Not in Git");
	case EGitFileState::Ignored:
		return LOCTEXT("Ignored", "Ignored");
	case EGitFileState::NotInRepository:
		return LOCTEXT("NotInRepository", "Not in this repository");
	case EGitFileState::Unknown:
		return LOCTEXT("Unknown", "Unknown");
	default:
		return LOCTEXT("Unchanged", "Up to date");
	}
}

FText FGitSourceControlState::GetDisplayTooltip() const
{
	if (IsConflicted())
	{
		return LOCTEXT("ConflictedTooltip", "This file has a merge conflict. Resolve it in a Git client, then mark it resolved.");
	}
	if (!IsCurrent())
	{
		return FText::Format(LOCTEXT("NotCurrentTooltip", "A newer version is on {0}. Sync before editing."), FText::FromString(Upstream.IsEmpty() ? TEXT("the server") : Upstream));
	}
	if (LockState == EGitLockState::LockedByOther)
	{
		return bLockInfoStale
			? FText::Format(LOCTEXT("LockedByOtherStaleTooltip", "Locked by {0} (last checked {1})."), FText::FromString(LockOwner), FText::AsDateTime(LockCheckedAtUtc))
			: FText::Format(LOCTEXT("LockedByOtherTooltip", "Locked by {0}."), FText::FromString(LockOwner));
	}
	if (LockState == EGitLockState::LockedElsewhere)
	{
		return LOCTEXT("LockedElsewhereTooltip", "You hold this lock in another clone or worktree. Check Out to use it here.");
	}
	if (LockState == EGitLockState::LockedHere)
	{
		return FileState == EGitFileState::Unchanged ? LOCTEXT("LockedHereTooltip", "Locked by you.") : LOCTEXT("LockedHereModifiedTooltip", "Locked by you and modified.");
	}
	if (bLockable && FileState == EGitFileState::Modified)
	{
		return LOCTEXT("ModifiedUnlockedTooltip", "Modified without a lock. Check out before submitting.");
	}
	switch (FileState)
	{
	case EGitFileState::Added:
	case EGitFileState::MarkedForAdd:
		return LOCTEXT("AddedTooltip", "New file. It will be added when you submit.");
	case EGitFileState::Untracked:
		return LOCTEXT("UntrackedTooltip", "New file that Git does not track yet.");
	case EGitFileState::Deleted:
		return LOCTEXT("DeletedTooltip", "Deleted. The deletion will be committed when you submit.");
	case EGitFileState::Modified:
		return LOCTEXT("ModifiedTooltip", "Modified.");
	default:
		return GetDisplayName();
	}
}

TOptional<FText> FGitSourceControlState::GetWarningText() const
{
	if (IsConflicted() || !IsCurrent() || LockState == EGitLockState::LockedByOther || LockState == EGitLockState::LockedElsewhere ||
		(bLockable && FileState == EGitFileState::Modified && LockState != EGitLockState::LockedHere))
	{
		return GetDisplayTooltip();
	}
	return {};
}

TOptional<FText> FGitSourceControlState::GetStatusText() const
{
	TOptional<FText> Warning = GetWarningText();
	if (Warning.IsSet())
	{
		return Warning;
	}
	// Unlike Perforce, every tracked git file counts as "checked out"; only say something useful.
	if (IsModified() || LockState == EGitLockState::LockedHere)
	{
		return GetDisplayTooltip();
	}
	return {};
}

#undef LOCTEXT_NAMESPACE
