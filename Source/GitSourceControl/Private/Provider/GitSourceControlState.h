// Copyright (c) 2014-2020 Sebastien Rombauts (sebastien.rombauts@gmail.com)
//
// Distributed under the MIT License (MIT) (See accompanying file LICENSE.txt
// or copy at http://opensource.org/licenses/MIT)

#pragma once

#include "CoreMinimal.h"
#include "ISourceControlState.h"
#include "GitSourceControlRevision.h"

/** What git says about the file itself. */
enum class EGitFileState : uint8
{
	/** No status has been read yet. */
	Unknown,
	/** Outside the repository (for example an engine or plugin content folder). */
	NotInRepository,
	Ignored,
	/** New file that git does not track yet. */
	Untracked,
	/** New file the editor asked to add; committed by Submit. */
	MarkedForAdd,
	/** New file already staged in git. */
	Added,
	Unchanged,
	Modified,
	Deleted,
	Conflicted
};

/** Who holds the LFS lock on the file. */
enum class EGitLockState : uint8
{
	/** The file is not `lockable` or locking is turned off. */
	NotLockable,
	/** Locks have not been checked yet. */
	Unknown,
	NotLocked,
	/** Locked by the current user from this worktree. */
	LockedHere,
	/** Locked by the current user, but from another clone or worktree. Check Out adopts it. */
	LockedElsewhere,
	LockedByOther
};

class FGitSourceControlState final : public ISourceControlState
{
public:
	explicit FGitSourceControlState(const FString& InLocalFilename)
		: LocalFilename(InLocalFilename)
		, TimeStamp(FDateTime::MinValue())
	{
	}

	// ISourceControlState
	virtual int32 GetHistorySize() const override;
	virtual TSharedPtr<ISourceControlRevision, ESPMode::ThreadSafe> GetHistoryItem(int32 HistoryIndex) const override;
	virtual TSharedPtr<ISourceControlRevision, ESPMode::ThreadSafe> FindHistoryRevision(int32 RevisionNumber) const override;
	virtual TSharedPtr<ISourceControlRevision, ESPMode::ThreadSafe> FindHistoryRevision(const FString& InRevision) const override;
	virtual TSharedPtr<ISourceControlRevision, ESPMode::ThreadSafe> GetCurrentRevision() const override;
	virtual FSlateIcon GetIcon() const override;
	virtual FText GetDisplayName() const override;
	virtual FText GetDisplayTooltip() const override;
	virtual TOptional<FText> GetWarningText() const override;
	virtual TOptional<FText> GetStatusText() const override;
	virtual const FString& GetFilename() const override { return LocalFilename; }
	virtual const FDateTime& GetTimeStamp() const override { return TimeStamp; }
	virtual bool CanCheckIn() const override;
	virtual bool CanCheckout() const override;
	virtual bool IsCheckedOut() const override;
	virtual bool IsCheckedOutOther(FString* Who = nullptr) const override;
	virtual bool IsCheckedOutInOtherBranch(const FString& CurrentBranch = FString()) const override { return false; }
	virtual bool IsModifiedInOtherBranch(const FString& CurrentBranch = FString()) const override { return false; }
	virtual bool IsCheckedOutOrModifiedInOtherBranch(const FString& CurrentBranch = FString()) const override { return false; }
	virtual TArray<FString> GetCheckedOutBranches() const override { return {}; }
	virtual FString GetOtherUserBranchCheckedOuts() const override { return FString(); }
	virtual bool GetOtherBranchHeadModification(FString& HeadBranchOut, FString& ActionOut, int32& HeadChangeListOut) const override { return false; }
	virtual bool IsCurrent() const override { return !bNewerOnServer; }
	virtual bool IsSourceControlled() const override;
	virtual bool IsAdded() const override;
	virtual bool IsDeleted() const override { return FileState == EGitFileState::Deleted; }
	virtual bool IsIgnored() const override { return FileState == EGitFileState::Ignored; }
	virtual bool CanEdit() const override;
	virtual bool CanDelete() const override;
	virtual bool IsUnknown() const override { return FileState == EGitFileState::Unknown; }
	virtual bool IsModified() const override;
	virtual bool CanAdd() const override { return FileState == EGitFileState::Untracked; }
	virtual bool IsConflicted() const override { return FileState == EGitFileState::Conflicted; }
	virtual bool CanRevert() const override;

	FString LocalFilename;
	/** Repository-relative path, empty when the file is outside the repository. */
	FString RelativePath;
	EGitFileState FileState = EGitFileState::Unknown;
	bool bStaged = false;
	bool bLockable = false;
	EGitLockState LockState = EGitLockState::NotLockable;
	FString LockOwner;
	FString LockId;
	/** Lock information is older than the refresh interval or the last check failed. */
	bool bLockInfoStale = false;
	FDateTime LockCheckedAtUtc;
	bool bNewerOnServer = false;
	FString Upstream;
	FDateTime TimeStamp;
	TArray<TSharedRef<FGitSourceControlRevision, ESPMode::ThreadSafe>> History;
};

using FGitSourceControlStateRef = TSharedRef<FGitSourceControlState, ESPMode::ThreadSafe>;
