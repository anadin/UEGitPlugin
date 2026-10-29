// Copyright (c) 2014-2020 Sebastien Rombauts (sebastien.rombauts@gmail.com)
//
// Distributed under the MIT License (MIT) (See accompanying file LICENSE.txt
// or copy at http://opensource.org/licenses/MIT)

#include "GitSourceControlWorkers.h"
#include "GitSourceControlProvider.h"

#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/Paths.h"
#include "SourceControlOperations.h"

#define LOCTEXT_NAMESPACE "GitSourceControl.Workers"

namespace GitWorkers
{
	TSharedPtr<IGitSourceControlWorker, ESPMode::ThreadSafe> Create(FName OperationName)
	{
		static const TMap<FName, TFunction<TSharedRef<IGitSourceControlWorker, ESPMode::ThreadSafe>()>> Factories = {
			{TEXT("Connect"), []() { return MakeShared<FGitConnectWorker, ESPMode::ThreadSafe>(); }},
			{TEXT("UpdateStatus"), []() { return MakeShared<FGitUpdateStatusWorker, ESPMode::ThreadSafe>(); }},
			{TEXT("CheckOut"), []() { return MakeShared<FGitCheckOutWorker, ESPMode::ThreadSafe>(); }},
			{TEXT("CheckIn"), []() { return MakeShared<FGitCheckInWorker, ESPMode::ThreadSafe>(); }},
			{TEXT("Revert"), []() { return MakeShared<FGitRevertWorker, ESPMode::ThreadSafe>(); }},
			{TEXT("SyncPreview"), []() { return MakeShared<FGitSyncPreviewWorker, ESPMode::ThreadSafe>(); }},
			{TEXT("Sync"), []() { return MakeShared<FGitSyncWorker, ESPMode::ThreadSafe>(); }},
			{TEXT("MarkForAdd"), []() { return MakeShared<FGitMarkForAddWorker, ESPMode::ThreadSafe>(); }},
			{TEXT("Delete"), []() { return MakeShared<FGitDeleteWorker, ESPMode::ThreadSafe>(); }},
			{TEXT("Copy"), []() { return MakeShared<FGitCopyWorker, ESPMode::ThreadSafe>(); }},
			{TEXT("Resolve"), []() { return MakeShared<FGitResolveWorker, ESPMode::ThreadSafe>(); }}};
		if (const auto* Factory = Factories.Find(OperationName))
		{
			return (*Factory)();
		}
		return nullptr;
	}

	bool IsSupported(FName OperationName)
	{
		return Create(OperationName).IsValid();
	}
}

namespace
{
	using namespace GitCore;

	/** Refreshes status for the command's files (or everything) before deciding anything. */
	bool RefreshForCommand(FGitSourceControlCommand& Command, bool bFull = false)
	{
		FString Error;
		const bool bWhole = bFull || Command.bWholeRepository || Command.RelativeFiles.IsEmpty();
		if (!Command.Session->RefreshStatus(bWhole ? nullptr : &Command.RelativeFiles, &Error))
		{
			Command.AddError(Error);
			return false;
		}
		if (!bWhole)
		{
			Command.Session->EnsureAttributes(Command.RelativeFiles);
		}
		return true;
	}

	FString ShortId(const FString& Commit)
	{
		return Commit.Left(8);
	}

	FString FileList(const TArray<FString>& Paths, int32 Max = 5)
	{
		TArray<FString> Shown;
		for (int32 Index = 0; Index < Paths.Num() && Index < Max; ++Index)
		{
			Shown.Add(FPaths::GetCleanFilename(Paths[Index]));
		}
		FString Text = FString::Join(Shown, TEXT(", "));
		if (Paths.Num() > Max)
		{
			Text += FString::Printf(TEXT(" and %d more"), Paths.Num() - Max);
		}
		return Text;
	}

	/** The user allows locking and some .gitattributes file marks files `lockable`. */
	bool UsesLocks(const FSessionSnapshot& Snapshot, const FGitSourceControlCommand& Command)
	{
		return Command.Settings.bLockingEnabled && Snapshot.bLockableRules;
	}

	bool IsLockable(const FSessionSnapshot& Snapshot, const FGitSourceControlCommand& Command, const FString& Path)
	{
		const FFileAttributes* Attributes = Snapshot.FindAttributes(Path);
		return UsesLocks(Snapshot, Command) && Attributes && Attributes->bLockable;
	}

	void MakeWritable(const FGitRepository& Repository, const FString& Path)
	{
		FPlatformFileManager::Get().GetPlatformFile().SetReadOnly(*Repository.ToAbsolute(Path), false);
	}

	/**
	 * Takes (or adopts) the lock on Path. Returns false with an error added when the
	 * file is locked by someone else, out of date, or the server refuses.
	 */
	bool AcquireLock(FGitSourceControlCommand& Command, const FString& Path)
	{
		FGitSession& Session = *Command.Session;
		const FGitRepository& Repository = Session.GetRepository();
		const FSessionSnapshotRef Snapshot = Session.GetSnapshot();
		if (const FLockInfo* Existing = Snapshot->Locks.Find(Path))
		{
			if (!Existing->bOurs)
			{
				Command.AddError(FText::Format(LOCTEXT("LockedByOther", "{0} is locked by {1}."), FText::FromString(FPaths::GetCleanFilename(Path)), FText::FromString(Existing->Owner)));
				return false;
			}
			// Already ours: record it for this worktree (adopting a lock taken in another clone).
			Session.RecordLockTaken(*Existing);
			MakeWritable(Repository, Path);
			return true;
		}
		if (Snapshot->Remote.IncomingPaths.Contains(Path))
		{
			Command.AddError(FText::Format(LOCTEXT("NewerOnServer", "A newer version of {0} is on the server. Pull first, then check it out."), FText::FromString(FPaths::GetCleanFilename(Path))));
			return false;
		}
		FLockInfo Lock;
		const FOpResult Locked = Repository.Lock(Session.ResolveLockRemote(), Path, Lock);
		if (!Locked.bOk)
		{
			Command.AddError(Locked.Message);
			Session.RequestLockRefresh(false);
			return false;
		}
		Session.RecordLockTaken(Lock);
		MakeWritable(Repository, Path);
		return true;
	}

	/** Releases our lock on Path unless a local change or an unpushed commit still needs it. */
	void ReleaseLockIfEligible(FGitSourceControlCommand& Command, const FString& Path, TArray<FString>& OutReleased)
	{
		FGitSession& Session = *Command.Session;
		const FSessionSnapshotRef Snapshot = Session.GetSnapshot();
		const FLockInfo* Lock = Snapshot->Locks.Find(Path);
		if (!Lock || !Lock->bOurs || !Snapshot->LocksTakenHere.Contains(Lock->Id))
		{
			return;
		}
		if (Snapshot->Status.Find(Path))
		{
			return;
		}
		bool bUnpublished = false;
		FString Error;
		if (!Session.GetRepository().HasUnpublishedChanges(Path, bUnpublished, Error) || bUnpublished)
		{
			Command.AddInfo(FText::Format(LOCTEXT("StaysLocked", "{0} stays locked until its commits are pushed."), FText::FromString(FPaths::GetCleanFilename(Path))));
			return;
		}
		const FOpResult Unlocked = Session.GetRepository().Unlock(Session.ResolveLockRemote(), Lock->Id, false);
		if (!Unlocked.bOk)
		{
			Command.AddError(Unlocked.Message);
			return;
		}
		Session.RecordLockReleased(Path);
		OutReleased.Add(Path);
	}

	/** Copies files that are about to be overwritten into a timestamped backup folder and prunes old ones. */
	void BackUpFiles(FGitSourceControlCommand& Command, const TArray<FString>& Paths)
	{
		const FGitRepository& Repository = Command.Session->GetRepository();
		IFileManager& Files = IFileManager::Get();
		const FString Folder = Command.Settings.BackupDirectory / FDateTime::UtcNow().ToString(TEXT("%Y%m%d-%H%M%S-%s"));
		int32 Copied = 0;
		for (const FString& Path : Paths)
		{
			const FString Source = Repository.ToAbsolute(Path);
			if (Files.FileExists(*Source) && Files.FileSize(*Source) <= 256ll * 1024 * 1024)
			{
				Copied += Files.Copy(*(Folder / Path), *Source) == COPY_OK ? 1 : 0;
			}
		}
		if (Copied > 0)
		{
			Command.AddInfo(FText::Format(LOCTEXT("BackupSaved", "A copy of the reverted files was saved in {0}."), FText::FromString(Folder)));
		}
		TArray<FString> Sets;
		Files.FindFiles(Sets, *(Command.Settings.BackupDirectory / TEXT("*")), false, true);
		Sets.Sort([](const FString& A, const FString& B) { return A > B; });
		const FDateTime Oldest = FDateTime::UtcNow() - FTimespan::FromDays(Command.Settings.BackupDays);
		for (int32 Index = 0; Index < Sets.Num(); ++Index)
		{
			const FString SetFolder = Command.Settings.BackupDirectory / Sets[Index];
			if (Index >= Command.Settings.BackupsToKeep || Files.GetTimeStamp(*SetFolder) < Oldest)
			{
				Files.DeleteDirectory(*SetFolder, false, true);
			}
		}
	}

	bool NeedsEditorRestart(const FString& Path)
	{
		return Path.StartsWith(TEXT("Source/")) || Path.StartsWith(TEXT("Config/")) || Path.StartsWith(TEXT("Plugins/")) ||
			Path.EndsWith(TEXT(".uproject")) || Path.EndsWith(TEXT(".uplugin")) || Path.EndsWith(TEXT(".Build.cs")) || Path.EndsWith(TEXT(".Target.cs"));
	}
}

bool FGitConnectWorker::Execute(FGitSourceControlCommand& Command)
{
	FString Error;
	if (!Command.Session->RefreshStatus(nullptr, &Error))
	{
		StaticCastSharedRef<FConnect>(Command.Operation)->SetErrorText(FText::FromString(Error));
		Command.AddError(Error);
		return false;
	}
	if (UsesLocks(*Command.Session->GetSnapshot(), Command))
	{
		Command.Session->RequestLockRefresh(false);
	}
	Command.Session->RequestFetch(false);
	Command.Session->RequestStashesAndBranches();
	return true;
}

bool FGitUpdateStatusWorker::Execute(FGitSourceControlCommand& Command)
{
	if (!RefreshForCommand(Command))
	{
		return false;
	}
	const TSharedRef<FUpdateStatus> Operation = StaticCastSharedRef<FUpdateStatus>(Command.Operation);
	if (Operation->ShouldUpdateHistory())
	{
		const FGitRepository& Repository = Command.Session->GetRepository();
		for (int32 Index = 0; Index < Command.RelativeFiles.Num() && Index < 50; ++Index)
		{
			TArray<FFileRevision> Revisions;
			FString Error;
			if (Repository.ReadFileHistory(Command.RelativeFiles[Index], 100, Revisions, Error))
			{
				Histories.Add(Command.AbsoluteFiles[Index], MoveTemp(Revisions));
			}
			else
			{
				Command.AddError(Error);
			}
		}
	}
	// Commandlets (for example World Partition builders) have no background refresh.
	const FSessionSnapshotRef Snapshot = Command.Session->GetSnapshot();
	if (IsRunningCommandlet() && UsesLocks(*Snapshot, Command) && Snapshot->Locks.AgeSeconds() > 60.0)
	{
		Command.Session->RefreshLocks(ECallKind::BackgroundNetwork);
	}
	return true;
}

bool FGitUpdateStatusWorker::UpdateStates(FGitSourceControlCommand& Command, FGitSourceControlProvider& Provider)
{
	for (TPair<FString, TArray<GitCore::FFileRevision>>& History : Histories)
	{
		Provider.SetHistory(History.Key, History.Value);
	}
	return true;
}

bool FGitCheckOutWorker::Execute(FGitSourceControlCommand& Command)
{
	if (!RefreshForCommand(Command))
	{
		return false;
	}
	if (UsesLocks(*Command.Session->GetSnapshot(), Command) && !Command.Session->GetSnapshot()->Locks.bVerified)
	{
		Command.Session->RefreshLocks(ECallKind::UserNetwork);
	}
	bool bAllLocked = true;
	for (const FString& Path : Command.RelativeFiles)
	{
		const FSessionSnapshotRef Snapshot = Command.Session->GetSnapshot();
		const FStatusEntry* Entry = Snapshot->Status.Find(Path);
		const bool bNew = (Entry && (Entry->bUntracked || Entry->Index == TEXT('A'))) || (!Entry && !Snapshot->IsTracked(Path));
		if (!IsLockable(*Snapshot, Command, Path) || bNew)
		{
			// Files that cannot be locked (or that only exist locally) are always editable.
			continue;
		}
		const FLockInfo* Lock = Snapshot->Locks.Find(Path);
		if (Lock && Lock->bOurs && Snapshot->LocksTakenHere.Contains(Lock->Id))
		{
			continue;
		}
		bAllLocked &= AcquireLock(Command, Path);
	}
	return bAllLocked;
}

bool FGitCheckInWorker::Execute(FGitSourceControlCommand& Command)
{
	FGitSession& Session = *Command.Session;
	const FGitRepository& Repository = Session.GetRepository();
	if (!RefreshForCommand(Command, true))
	{
		return false;
	}
	FSessionSnapshotRef Snapshot = Session.GetSnapshot();
	if (Snapshot->Status.bOperationInProgress || Snapshot->Status.HasConflicts())
	{
		Command.AddError(LOCTEXT("SubmitMidOperation", "Finish the merge or rebase in progress (or resolve its conflicts) before submitting."));
		return false;
	}
	if (Snapshot->Status.Branch.IsEmpty())
	{
		Command.AddError(LOCTEXT("SubmitDetached", "Switch to a branch before submitting; HEAD is detached."));
		return false;
	}

	TArray<FString> CommitPaths;
	TArray<FString> NewPaths;
	TArray<FString> LockOnly;
	TArray<FString> NeedsLock;
	for (const FString& Path : Command.RelativeFiles)
	{
		const FStatusEntry* Entry = Snapshot->Status.Find(Path);
		const bool bLockable = IsLockable(*Snapshot, Command, Path);
		const FLockInfo* Lock = Snapshot->Locks.Find(Path);
		const bool bLockedHere = Lock && Lock->bOurs && Snapshot->LocksTakenHere.Contains(Lock->Id);
		if (!Entry)
		{
			const bool bRenameSource = Snapshot->Status.Entries.ContainsByPredicate([&Path](const FStatusEntry& Other) { return Other.OriginalPath == Path; });
			if (bRenameSource)
			{
				CommitPaths.Add(Path);
			}
			else if (bLockedHere)
			{
				LockOnly.Add(Path);
			}
			continue;
		}
		if (bLockable && !Entry->bUntracked && Entry->Index != TEXT('A') && !bLockedHere)
		{
			NeedsLock.Add(Path);
			continue;
		}
		CommitPaths.Add(Path);
		if (!Entry->OriginalPath.IsEmpty())
		{
			CommitPaths.AddUnique(Entry->OriginalPath);
		}
		if (Entry->bUntracked)
		{
			NewPaths.Add(Path);
		}
	}
	if (!NeedsLock.IsEmpty())
	{
		Command.AddError(FText::Format(LOCTEXT("SubmitNeedsLock", "Check out {0} before submitting: these files are lockable and not locked by you."), FText::FromString(FileList(NeedsLock))));
		return false;
	}

	const FString Description = StaticCastSharedRef<FCheckIn>(Command.Operation)->GetDescription().ToString();
	FString Commit;
	if (!CommitPaths.IsEmpty())
	{
		const FOpResult Committed = Repository.CommitPaths(CommitPaths, NewPaths, Description, Commit);
		if (!Committed.bOk)
		{
			Command.AddError(Committed.Message);
			Command.AddError(Committed.Details);
			return false;
		}
		Session.SetMarkedForAdd(NewPaths, false);
		Session.RefreshStatus();
	}

	bool bPushed = false;
	FString Remote;
	if (Command.Settings.bPushOnSubmit)
	{
		Snapshot = Session.GetSnapshot();
		if (UsesLocks(*Snapshot, Command))
		{
			// The server refuses pushes that change files someone else has locked; say so before trying.
			Session.RefreshLocks(ECallKind::UserNetwork);
			Snapshot = Session.GetSnapshot();
			TArray<FString> Blocked;
			for (const FString& Path : Snapshot->Remote.OutgoingPaths)
			{
				const FLockInfo* Lock = Snapshot->Locks.Find(Path);
				if (Lock && !Lock->bOurs)
				{
					Blocked.Add(Path);
				}
			}
			if (!Blocked.IsEmpty())
			{
				Command.AddError(Commit.IsEmpty()
					? FText::Format(LOCTEXT("PushBlocked", "Not pushed: {0} is locked by someone else."), FText::FromString(FileList(Blocked)))
					: FText::Format(LOCTEXT("CommittedPushBlocked", "Committed locally ({0}), but not pushed: {1} is locked by someone else."),
						FText::FromString(ShortId(Commit)), FText::FromString(FileList(Blocked))));
				return false;
			}
		}
		Remote = Repository.DefaultRemote();
		const bool bHasOutgoing = !Commit.IsEmpty() || Snapshot->Remote.Ahead > 0 || (Snapshot->Remote.Upstream.IsEmpty() && !Snapshot->Status.bUnborn);
		if (bHasOutgoing && (!Snapshot->Remote.Upstream.IsEmpty() || !Remote.IsEmpty()))
		{
			Session.CancelBackgroundWork();
			const FOpResult Pushed = Repository.Push(Snapshot->Status, Remote, Command.Cancel);
			if (!Pushed.bOk)
			{
				Command.AddError(Commit.IsEmpty()
					? FText::FromString(Pushed.Message)
					: FText::Format(LOCTEXT("CommittedNotPushed", "Committed locally ({0}). {1}"), FText::FromString(ShortId(Commit)), FText::FromString(Pushed.Message)));
				Command.AddError(Pushed.Details);
				Session.RefreshStatus();
				Session.RefreshRemote();
				return false;
			}
			bPushed = true;
			// Push updates the remote-tracking branch, so a local refresh is enough.
			Session.RefreshStatus();
			Session.RefreshRemote();
		}
	}

	TArray<FString> Released;
	const bool bKeepCheckedOut = StaticCastSharedRef<FCheckIn>(Command.Operation)->GetKeepCheckedOut();
	if (!bKeepCheckedOut)
	{
		// Each lock is only released when no local change or unpushed commit still needs it.
		for (const FString& Path : Command.RelativeFiles)
		{
			ReleaseLockIfEligible(Command, Path, Released);
		}
	}

	TArray<FText> Summary;
	if (!Commit.IsEmpty())
	{
		const FString PushedTo = Session.GetSnapshot()->Remote.Upstream.IsEmpty() ? Remote : Session.GetSnapshot()->Remote.Upstream;
		Summary.Add(bPushed
			? FText::Format(LOCTEXT("CommittedPushed", "Committed {0} {0}|plural(one=file,other=files) as {1} and pushed to {2}."), CommitPaths.Num(),
				FText::FromString(ShortId(Commit)), FText::FromString(PushedTo))
			: FText::Format(LOCTEXT("CommittedOnly", "Committed {0} {0}|plural(one=file,other=files) as {1}."), CommitPaths.Num(), FText::FromString(ShortId(Commit))));
	}
	else if (bPushed)
	{
		Summary.Add(LOCTEXT("PushedEarlier", "Pushed your earlier commits."));
	}
	if (!Released.IsEmpty())
	{
		Summary.Add(FText::Format(LOCTEXT("ReleasedLocks", "Released {0} {0}|plural(one=lock,other=locks)."), Released.Num()));
	}
	SuccessMessage = FText::Join(FText::FromString(TEXT(" ")), Summary);
	return Command.ErrorMessages.IsEmpty();
}

bool FGitCheckInWorker::UpdateStates(FGitSourceControlCommand& Command, FGitSourceControlProvider& Provider)
{
	StaticCastSharedRef<FCheckIn>(Command.Operation)->SetSuccessMessage(SuccessMessage);
	return true;
}

bool FGitRevertWorker::Execute(FGitSourceControlCommand& Command)
{
	FGitSession& Session = *Command.Session;
	const FGitRepository& Repository = Session.GetRepository();
	const TSharedRef<FRevert> Operation = StaticCastSharedRef<FRevert>(Command.Operation);
	if (!RefreshForCommand(Command, Operation->IsRevertAll()))
	{
		return false;
	}
	FSessionSnapshotRef Snapshot = Session.GetSnapshot();
	TArray<FString> Paths = Command.RelativeFiles;
	if (Operation->IsRevertAll())
	{
		Paths.Reset();
		for (const FStatusEntry& Entry : Snapshot->Status.Entries)
		{
			Paths.Add(Entry.Path);
		}
	}
	if (Paths.IsEmpty())
	{
		return true;
	}

	TArray<FString> NewFiles;
	TArray<FString> Restore;
	for (const FString& Path : Paths)
	{
		const FStatusEntry* Entry = Snapshot->Status.Find(Path);
		const bool bRenameSource = !Entry && Snapshot->Status.Entries.ContainsByPredicate([&Path](const FStatusEntry& Other) { return Other.OriginalPath == Path; });
		if (Entry && (Entry->bUntracked || Entry->Index == TEXT('A')))
		{
			NewFiles.Add(Path);
		}
		else if (Entry || bRenameSource)
		{
			Restore.Add(Path);
		}
	}

	if (Operation->IsSoftRevert())
	{
		// Used by delete flows: forget pending adds, leave files and locks alone.
		Session.SetMarkedForAdd(NewFiles, false);
		TArray<FString> Staged = NewFiles.FilterByPredicate([&Snapshot](const FString& Path)
		{
			const FStatusEntry* Entry = Snapshot->Status.Find(Path);
			return Entry && !Entry->bUntracked;
		});
		if (!Staged.IsEmpty())
		{
			Repository.Unstage(Staged);
		}
		Session.RefreshStatus(&Paths);
		return true;
	}

	TArray<FString> Overwritten = Restore;
	if (Operation->ShouldDeleteNewFiles())
	{
		Overwritten.Append(NewFiles);
	}
	BackUpFiles(Command, Overwritten);
	if (!Restore.IsEmpty() || (Operation->ShouldDeleteNewFiles() && !NewFiles.IsEmpty()))
	{
		const FOpResult Restored = Repository.RestorePaths(Operation->ShouldDeleteNewFiles() ? Overwritten : Restore);
		if (!Restored.bOk)
		{
			Command.AddError(Restored.Message);
			Command.AddError(Restored.Details);
			return false;
		}
	}
	if (Operation->ShouldDeleteNewFiles())
	{
		for (const FString& Path : NewFiles)
		{
			DeletedFiles.Add(Repository.ToAbsolute(Path));
		}
	}
	else if (!NewFiles.IsEmpty())
	{
		Repository.Unstage(NewFiles.FilterByPredicate([&Snapshot](const FString& Path) { return !Snapshot->Status.Find(Path)->bUntracked; }));
	}
	Session.SetMarkedForAdd(NewFiles, false);
	Session.RefreshStatus();

	TArray<FString> Released;
	for (const FString& Path : Paths)
	{
		ReleaseLockIfEligible(Command, Path, Released);
	}
	return Command.ErrorMessages.IsEmpty();
}

bool FGitRevertWorker::UpdateStates(FGitSourceControlCommand& Command, FGitSourceControlProvider& Provider)
{
	const TSharedRef<FRevert> Operation = StaticCastSharedRef<FRevert>(Command.Operation);
	for (const FString& File : DeletedFiles)
	{
		Operation->AddDeletedFile(File);
	}
	return true;
}

bool FGitSyncPreviewWorker::Execute(FGitSourceControlCommand& Command)
{
	if (!StaticCastSharedRef<FSyncPreview>(Command.Operation)->GetRevision().IsEmpty())
	{
		Command.AddError(LOCTEXT("SyncRevisionUnsupported", "Syncing to a specific revision is not supported. Use the Git panel to switch branches."));
		return false;
	}
	FGitSession& Session = *Command.Session;
	Session.CancelBackgroundWork();
	FString Error;
	if (!Session.Fetch(ECallKind::UserNetwork, Command.Cancel, &Error))
	{
		// An empty preview keeps the editor from reloading every package; Sync reports the problem.
		Command.AddInfo(Error);
		return true;
	}
	Session.RefreshStatus();
	for (const FString& Path : Session.GetSnapshot()->Remote.IncomingPaths)
	{
		AffectedFiles.Add(Session.GetRepository().ToAbsolute(Path));
	}
	return true;
}

bool FGitSyncPreviewWorker::UpdateStates(FGitSourceControlCommand& Command, FGitSourceControlProvider& Provider)
{
	StaticCastSharedRef<FSyncPreview>(Command.Operation)->SetAffectedFiles(MoveTemp(AffectedFiles));
	return true;
}

bool FGitSyncWorker::Execute(FGitSourceControlCommand& Command)
{
	FGitSession& Session = *Command.Session;
	const FGitRepository& Repository = Session.GetRepository();
	if (!StaticCastSharedRef<FSync>(Command.Operation)->GetRevision().IsEmpty())
	{
		Command.AddError(LOCTEXT("SyncRevisionUnsupported", "Syncing to a specific revision is not supported. Use the Git panel to switch branches."));
		return false;
	}
	Session.CancelBackgroundWork();
	FString Error;
	if (!Session.Fetch(ECallKind::UserNetwork, Command.Cancel, &Error))
	{
		Command.AddError(Error);
		return false;
	}
	if (!RefreshForCommand(Command, true))
	{
		return false;
	}
	const FSessionSnapshotRef Snapshot = Session.GetSnapshot();
	const FRemoteState& Remote = Snapshot->Remote;
	if (Snapshot->Status.Branch.IsEmpty())
	{
		Command.AddError(LOCTEXT("SyncDetached", "Switch to a branch before pulling; HEAD is detached."));
		return false;
	}
	if (Remote.Upstream.IsEmpty())
	{
		Command.AddInfo(LOCTEXT("SyncUnpublished", "This branch has not been published yet, so there is nothing to pull."));
		return true;
	}
	if (Remote.Behind == 0)
	{
		Command.AddInfo(LOCTEXT("SyncUpToDate", "Already up to date."));
		return true;
	}
	if (Snapshot->Status.bOperationInProgress || Snapshot->Status.HasConflicts())
	{
		Command.AddError(LOCTEXT("SyncMidOperation", "Finish the merge or rebase in progress before pulling."));
		return false;
	}
	if (!Command.bWholeRepository)
	{
		// Git updates the whole working tree; only allow a file-level sync when it changes nothing else.
		const TSet<FString> Requested(Command.RelativeFiles);
		TArray<FString> Others;
		for (const FString& Path : Remote.IncomingPaths)
		{
			if (!Requested.Contains(Path))
			{
				Others.Add(Path);
			}
		}
		if (!Others.IsEmpty())
		{
			Command.AddError(FText::Format(LOCTEXT("SyncOtherFiles", "Other files also changed on the server ({0}). Use Pull in the status bar to update everything safely."), FText::FromString(FileList(Others))));
			return false;
		}
	}
	TArray<FString> Overlap;
	for (const FStatusEntry& Entry : Snapshot->Status.Entries)
	{
		if (Remote.IncomingPaths.Contains(Entry.Path) || (!Entry.OriginalPath.IsEmpty() && Remote.IncomingPaths.Contains(Entry.OriginalPath)))
		{
			Overlap.Add(Entry.Path);
		}
	}
	if (!Overlap.IsEmpty())
	{
		Command.AddError(FText::Format(LOCTEXT("SyncOverlap", "Nothing was changed: you have local changes to files that also changed on the server ({0}). Submit, stash or revert them first."), FText::FromString(FileList(Overlap))));
		return false;
	}
	const bool bRebase = Remote.Ahead > 0;
	if (bRebase)
	{
		bool bConflicts = false;
		if (!Repository.PredictConflicts(Remote.Upstream, bConflicts, Error))
		{
			Command.AddError(Error);
			return false;
		}
		if (bConflicts)
		{
			Command.AddError(LOCTEXT("SyncConflict", "Nothing was changed: your unpushed commits conflict with changes on the server. Resolve this in a Git client."));
			return false;
		}
	}
	const FOpResult Downloaded = Repository.FetchLfsObjects(Remote.Remote.IsEmpty() ? Repository.DefaultRemote() : Remote.Remote, Remote.Upstream, Command.Cancel);
	if (!Downloaded.bOk)
	{
		Command.AddError(Downloaded.Message);
		return false;
	}
	const FString OldHead = Snapshot->Status.Head;
	const FOpResult Integrated = Repository.Integrate(Remote.Upstream, bRebase, Command.Cancel);
	Session.RefreshStatus();
	if (!Integrated.bOk)
	{
		Command.AddError(Integrated.Message);
		Command.AddError(Integrated.Details);
		return false;
	}
	TSet<FString> Changed;
	if (!OldHead.IsEmpty() && Repository.ChangedPaths(OldHead, TEXT("HEAD"), Changed, Error))
	{
		TArray<FString> NeedRestart;
		for (const FString& Path : Changed)
		{
			if (NeedsEditorRestart(Path))
			{
				NeedRestart.Add(Path);
			}
		}
		if (!NeedRestart.IsEmpty())
		{
			Command.AddInfo(FText::Format(LOCTEXT("SyncNeedsRestart", "Code, config or plugin files changed ({0}). Rebuild and restart the editor to use them."), FText::FromString(FileList(NeedRestart))));
		}
	}
	Command.AddInfo(FText::Format(LOCTEXT("Pulled", "Pulled {0} {0}|plural(one=commit,other=commits) from {1}."), Remote.Behind, FText::FromString(Remote.Upstream)));
	Session.RequestLockRefresh(false);
	Session.RequestStashesAndBranches();
	return true;
}

bool FGitMarkForAddWorker::Execute(FGitSourceControlCommand& Command)
{
	RefreshForCommand(Command);
	const FSessionSnapshotRef Snapshot = Command.Session->GetSnapshot();
	TArray<FString> Untracked;
	for (const FString& Path : Command.RelativeFiles)
	{
		const FStatusEntry* Entry = Snapshot->Status.Find(Path);
		if (Entry && Entry->bUntracked)
		{
			Untracked.Add(Path);
		}
	}
	// Nothing is staged: Submit adds these files when it commits them.
	Command.Session->SetMarkedForAdd(Untracked, true);
	return true;
}

bool FGitDeleteWorker::Execute(FGitSourceControlCommand& Command)
{
	if (!RefreshForCommand(Command))
	{
		return false;
	}
	FGitSession& Session = *Command.Session;
	const FGitRepository& Repository = Session.GetRepository();
	bool bAllDeleted = true;
	for (const FString& Path : Command.RelativeFiles)
	{
		const FSessionSnapshotRef Snapshot = Session.GetSnapshot();
		if (Snapshot->IsTracked(Path) && IsLockable(*Snapshot, Command, Path))
		{
			const FLockInfo* Lock = Snapshot->Locks.Find(Path);
			const bool bLockedHere = Lock && Lock->bOurs && Snapshot->LocksTakenHere.Contains(Lock->Id);
			if (!bLockedHere && !AcquireLock(Command, Path))
			{
				bAllDeleted = false;
				continue;
			}
		}
		const FString Absolute = Repository.ToAbsolute(Path);
		FPlatformFileManager::Get().GetPlatformFile().SetReadOnly(*Absolute, false);
		if (IFileManager::Get().FileExists(*Absolute) && !IFileManager::Get().Delete(*Absolute, false, true, true))
		{
			Command.AddError(FText::Format(LOCTEXT("DeleteFailed", "Could not delete {0}."), FText::FromString(FPaths::GetCleanFilename(Path))));
			bAllDeleted = false;
		}
	}
	Session.SetMarkedForAdd(Command.RelativeFiles, false);
	Session.RefreshStatus(&Command.RelativeFiles);
	return bAllDeleted;
}

bool FGitResolveWorker::Execute(FGitSourceControlCommand& Command)
{
	const GitCore::FOpResult Result = Command.Session->GetRepository().Stage(Command.RelativeFiles);
	if (!Result.bOk)
	{
		Command.AddError(Result.Message);
	}
	Command.Session->RefreshStatus(&Command.RelativeFiles);
	return Result.bOk;
}

#undef LOCTEXT_NAMESPACE
