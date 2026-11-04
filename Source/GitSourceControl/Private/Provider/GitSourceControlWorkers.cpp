// Copyright (c) 2014-2020 Sebastien Rombauts (sebastien.rombauts@gmail.com)
//
// Distributed under the MIT License (MIT) (See accompanying file LICENSE.txt
// or copy at http://opensource.org/licenses/MIT)

#include "GitSourceControlWorkers.h"
#include "GitSourceControlProvider.h"

#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
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

	FString BackUpFiles(const GitCore::FGitRepository& Repository, const TArray<FString>& Paths, const FGitCommandSettings& Settings)
	{
		IFileManager& Files = IFileManager::Get();
		const FString Folder = Settings.BackupDirectory / FDateTime::UtcNow().ToString(TEXT("%Y%m%d-%H%M%S-%s"));
		int32 Copied = 0;
		for (const FString& Path : Paths)
		{
			const FString Source = Repository.ToAbsolute(Path);
			if (Files.FileExists(*Source) && Files.FileSize(*Source) <= 256ll * 1024 * 1024)
			{
				Copied += Files.Copy(*(Folder / Path), *Source) == COPY_OK ? 1 : 0;
			}
		}
		TArray<FString> Sets;
		Files.FindFiles(Sets, *(Settings.BackupDirectory / TEXT("*")), false, true);
		Sets.Sort([](const FString& A, const FString& B) { return A > B; });
		const FDateTime Oldest = FDateTime::UtcNow() - FTimespan::FromDays(Settings.BackupDays);
		for (int32 Index = 0; Index < Sets.Num(); ++Index)
		{
			const FString SetFolder = Settings.BackupDirectory / Sets[Index];
			if (Index >= Settings.BackupsToKeep || Files.GetTimeStamp(*SetFolder) < Oldest)
			{
				Files.DeleteDirectory(*SetFolder, false, true);
			}
		}
		return Copied > 0 ? Folder : FString();
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

	bool IsLockedHere(const FSessionSnapshot& Snapshot, const FString& Path)
	{
		const FLockInfo* Lock = Snapshot.Locks.Find(Path);
		return Lock && Lock->bOurs && Snapshot.LocksTakenHere.Contains(Lock->Id);
	}

	/** Counts, for the progress bar, the locks this worktree holds on Paths: the ones ReleaseLockIfEligible looks at. */
	void StartReleaseProgress(FGitSourceControlCommand& Command, const TArray<FString>& Paths)
	{
		const FSessionSnapshotRef Snapshot = Command.Session->GetSnapshot();
		Command.Progress->Start(Paths.FilterByPredicate([&Snapshot](const FString& Path) { return IsLockedHere(*Snapshot, Path); }).Num(), true);
	}

	/** Releases our lock on Path unless a local change or an unpushed commit still needs it. */
	void ReleaseLockIfEligible(FGitSourceControlCommand& Command, const FString& Path, TArray<FString>& OutReleased)
	{
		FGitSession& Session = *Command.Session;
		const FSessionSnapshotRef Snapshot = Session.GetSnapshot();
		const FLockInfo* Lock = Snapshot->Locks.Find(Path);
		if (!IsLockedHere(*Snapshot, Path))
		{
			return;
		}
		ON_SCOPE_EXIT
		{
			++Command.Progress->Done;
		};
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

	bool NeedsEditorRestart(const FString& Path)
	{
		return Path.StartsWith(TEXT("Source/")) || Path.StartsWith(TEXT("Config/")) || Path.StartsWith(TEXT("Plugins/")) ||
			Path.EndsWith(TEXT(".uproject")) || Path.EndsWith(TEXT(".uplugin")) || Path.EndsWith(TEXT(".Build.cs")) || Path.EndsWith(TEXT(".Target.cs"));
	}

	/** What a pull would bring in: from the upstream, or from the remote branch the Git panel's Pull dialog chose. */
	bool ReadPullSource(FGitSourceControlCommand& Command, FRemoteState& Out, FString& OutError)
	{
		const FPullOptions& Pull = Command.Settings.Pull;
		const FSessionSnapshotRef Snapshot = Command.Session->GetSnapshot();
		if (Pull.Branch.IsEmpty())
		{
			Out = Snapshot->Remote;
			return true;
		}
		// Compared like an upstream, through the remote-tracking branch the fetch just updated.
		FStatusSnapshot Status = Snapshot->Status;
		Status.Upstream = Pull.Remote + TEXT("/") + Pull.Branch;
		const bool bRead = Command.Session->GetRepository().ReadRemoteState(Status, Pull.Remote, Out, OutError);
		Out.Remote = Pull.Remote;
		return bRead;
	}

	/** How a pull goes, decided before anything changes. */
	struct FPullPlan
	{
		EIntegrate Mode = EIntegrate::FastForward;
		/** Local changes to files that also changed on the server. */
		TArray<FString> Overlap;
		/** New local files where the server adds files of the same name. */
		TArray<FString> NewInTheWay;
		/** The tracked files a stash would set aside. */
		TArray<FString> Stashed;
		/** Local changes overlap the incoming ones, or a rebase needs them out of the way. */
		bool bStash = false;
	};

	FPullPlan PlanPull(const FSessionSnapshot& Snapshot, const FRemoteState& Source, const FPullOptions& Pull)
	{
		FPullPlan Plan;
		Plan.Mode = Source.Ahead == 0 ? EIntegrate::FastForward : Pull.bRebase ? EIntegrate::Rebase : EIntegrate::Merge;
		for (const FStatusEntry& Entry : Snapshot.Status.Entries)
		{
			const bool bIncoming = Source.IncomingPaths.Contains(Entry.Path) || (!Entry.OriginalPath.IsEmpty() && Source.IncomingPaths.Contains(Entry.OriginalPath));
			if (Entry.bUntracked)
			{
				if (bIncoming)
				{
					Plan.NewInTheWay.Add(Entry.Path);
				}
				continue;
			}
			if (bIncoming)
			{
				Plan.Overlap.Add(Entry.Path);
			}
			Plan.Stashed.Add(Entry.Path);
			if (!Entry.OriginalPath.IsEmpty())
			{
				Plan.Stashed.AddUnique(Entry.OriginalPath);
			}
		}
		// A merge or a fast-forward keeps local changes it does not touch; a rebase needs a clean working tree.
		Plan.bStash = !Plan.Stashed.IsEmpty() && (!Plan.Overlap.IsEmpty() || Plan.Mode == EIntegrate::Rebase);
		return Plan;
	}

	/** Puts back the changes a pull set aside, staged as they were when that still applies. */
	bool ReapplyStash(const FGitRepository& Repository)
	{
		return Repository.StashApply(TEXT("stash@{0}"), true, true).bOk || Repository.StashApply(TEXT("stash@{0}"), false, true).bOk;
	}

	/** The branch's own copy on the server, which lean branching's Sync keeps in step with the rebased branch. */
	struct FBranchCopy
	{
		FPushTarget Target;
		/** Such as origin/feature; empty when the branch is not on the server. */
		FString Name;
		FString Oid;
	};

	/**
	 * Finds the branch's copy on the server, and makes sure Sync may replace it: each of its commits must be in the
	 * branch already, as itself or as a rebased version. Otherwise someone pushed to it, and their commits come first.
	 */
	bool ReadBranchCopy(const FGitRepository& Repository, const FSessionSnapshot& Snapshot, const FRemoteState& Source, FBranchCopy& Out, FText& OutError)
	{
		if (!Repository.ResolvePush(Snapshot.Status, Repository.DefaultRemote(), Out.Target).bOk || !Out.Target.bExists)
		{
			return true;
		}
		// Pushing where it pulls from (push.default=upstream) has no copy of its own to keep in step.
		const FString Name = Out.Target.Remote + TEXT("/") + Out.Target.Branch;
		if (Name.Equals(Source.Upstream, ESearchCase::CaseSensitive) || !Repository.ResolveCommit(TEXT("refs/remotes/") + Name, Out.Oid))
		{
			Out.Oid.Reset();
			return true;
		}
		Out.Name = Name;
		int32 Missing = 0;
		FString Error;
		if (!Repository.CountCommits({TEXT("--cherry-pick"), TEXT("--right-only"), TEXT("--no-merges"), TEXT("HEAD...") + Out.Oid, TEXT("--not"), Source.UpstreamHead}, Missing, Error))
		{
			OutError = FText::FromString(Error);
			return false;
		}
		if (Missing > 0)
		{
			OutError = FText::Format(LOCTEXT("SyncCopyAhead", "Nothing was changed: {0} has commits this branch does not have. Pull them first, then sync."), FText::FromString(Name));
			return false;
		}
		return true;
	}

	/**
	 * After lean branching's Sync rebased the branch, moves its copy on the server to the rebased versions of the commits it
	 * had (a push with lease), so commits never pushed stay unpushed. Nothing is pushed when the copy is still in the branch.
	 */
	bool UpdateBranchCopy(FGitSourceControlCommand& Command, const FBranchCopy& Copy, const FRemoteState& Source, bool& bOutUpdated)
	{
		bOutUpdated = false;
		const FGitRepository& Repository = Command.Session->GetRepository();
		bool bContained = false;
		int32 Unpushed = 0;
		int32 OnBranch = 0;
		FString Error;
		if (!Repository.IsAncestor(Copy.Oid, TEXT("HEAD"), bContained, Error) || (!bContained &&
			(!Repository.CountCommits({TEXT("--cherry-pick"), TEXT("--left-only"), TEXT("--no-merges"), TEXT("HEAD...") + Copy.Oid, TEXT("--not"), Source.UpstreamHead}, Unpushed, Error) ||
			!Repository.CountCommits({Source.UpstreamHead + TEXT("..HEAD")}, OnBranch, Error))))
		{
			Command.AddError(Error);
			return false;
		}
		if (bContained)
		{
			return true;
		}
		// The unpushed commits are the newest ones: the copy goes to the commit just below them.
		FString Commit;
		const FString Wanted = Unpushed <= 0 ? FString(TEXT("HEAD")) : Unpushed >= OnBranch ? Source.UpstreamHead : FString::Printf(TEXT("HEAD~%d"), Unpushed);
		FPushTarget Target = Copy.Target;
		Target.bSetUpstream = false;
		const FOpResult Pushed = Repository.ResolveCommit(Wanted, Commit) ? Repository.Push(Target, Command.Cancel, Commit, Copy.Oid)
			: FOpResult::Failure(TEXT("The commit to push could not be found."));
		Command.Session->RefreshRemote();
		if (!Pushed.bOk)
		{
			Command.AddError(FText::Format(LOCTEXT("SyncCopyNotUpdated", "Rebased onto {0}, but {1} on the server was not updated: {2} Sync again to retry."),
				FText::FromString(Source.Upstream), FText::FromString(Copy.Name), FText::FromString(Pushed.Message)));
			Command.AddError(Pushed.Details);
			return false;
		}
		bOutUpdated = true;
		return true;
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
	TArray<FString> ToLock;
	{
		const FSessionSnapshotRef Snapshot = Command.Session->GetSnapshot();
		for (const FString& Path : Command.RelativeFiles)
		{
			const FStatusEntry* Entry = Snapshot->Status.Find(Path);
			const bool bNew = (Entry && (Entry->bUntracked || Entry->Index == TEXT('A'))) || (!Entry && !Snapshot->IsTracked(Path));
			// Files that cannot be locked (or that only exist locally) are always editable.
			if (IsLockable(*Snapshot, Command, Path) && !bNew && !IsLockedHere(*Snapshot, Path))
			{
				ToLock.Add(Path);
			}
		}
	}
	Command.Progress->Start(ToLock.Num(), false);
	bool bAllLocked = true;
	for (const FString& Path : ToLock)
	{
		if (Command.Cancel->IsCancelled())
		{
			Command.AddInfo(FText::Format(LOCTEXT("LockingStopped", "Stopped after locking {0} of {1} files."), Command.Progress->Done.load(), ToLock.Num()));
			return false;
		}
		bAllLocked &= AcquireLock(Command, Path);
		++Command.Progress->Done;
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
	FPushTarget Target;
	if (Command.Settings.bPushOnSubmit)
	{
		Snapshot = Session.GetSnapshot();
		Remote = Repository.DefaultRemote();
		const bool bHasOutgoing = !Commit.IsEmpty() || Snapshot->Remote.Ahead > 0 || (Snapshot->Remote.Upstream.IsEmpty() && !Snapshot->Status.bUnborn);
		// Without a remote there is nowhere to push, and the commit is the whole check in.
		if (bHasOutgoing && Repository.ResolvePush(Snapshot->Status, Remote, Target).bOk)
		{
			if (Target.NeedsAgreement() && !Command.Settings.bPushAgreed)
			{
				// The provider asks before the check in starts, from the last snapshot; the branch has changed since, or nobody could be asked.
				const FText Destination = FText::FromString(Target.Remote + TEXT("/") + Target.Branch);
				Command.AddError(Commit.IsEmpty()
					? FText::Format(LOCTEXT("PushNeedsAgreement", "Not pushed: this push would publish {0}. Push from the Git panel to be asked."), Destination)
					: FText::Format(LOCTEXT("CommittedNeedsAgreement", "Committed locally ({0}), but not pushed: this push would publish {1}. Push from the Git panel to be asked."),
						FText::FromString(ShortId(Commit)), Destination));
				Session.RefreshStatus();
				Session.RefreshRemote();
				return false;
			}
			if (UsesLocks(*Snapshot, Command))
			{
				// Changing a file someone else has locked clashes with their work, and Git LFS refuses such pushes
				// when lfs.locksverify is on: say so before trying.
				Session.RefreshLocks(ECallKind::UserNetwork);
				TArray<FString> Blocked;
				FString Error;
				if (!Session.FindLockedByOthers(Target.Remote, Blocked, &Error))
				{
					Command.AddError(Commit.IsEmpty() ? FText::FromString(Error)
						: FText::Format(LOCTEXT("CommittedNotChecked", "Committed locally ({0}), but not pushed: {1}"), FText::FromString(ShortId(Commit)), FText::FromString(Error)));
					return false;
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
			Session.CancelBackgroundWork();
			const FOpResult Pushed = Repository.Push(Target, Command.Cancel);
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
		StartReleaseProgress(Command, Command.RelativeFiles);
		for (const FString& Path : Command.RelativeFiles)
		{
			ReleaseLockIfEligible(Command, Path, Released);
		}
	}

	TArray<FText> Summary;
	if (!Commit.IsEmpty())
	{
		const FString PushedTo = Target.Remote + TEXT("/") + Target.Branch;
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
	const FString Backup = GitWorkers::BackUpFiles(Repository, Overwritten, Command.Settings);
	if (!Backup.IsEmpty())
	{
		Command.AddInfo(FText::Format(LOCTEXT("BackupSaved", "A copy of the reverted files was saved in {0}."), FText::FromString(Backup)));
	}
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
	StartReleaseProgress(Command, Paths);
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
	const FPullOptions& Pull = Command.Settings.Pull;
	Session.CancelBackgroundWork();
	FString Error;
	FRemoteState Source;
	if (!Session.Fetch(ECallKind::UserNetwork, Command.Cancel, &Error, Pull.Remote) || !Session.RefreshStatus(nullptr, &Error) || !ReadPullSource(Command, Source, Error))
	{
		// An empty preview keeps the editor from reloading every package; Sync reports the problem.
		Command.AddInfo(Error);
		return true;
	}
	for (const FString& Path : Source.IncomingPaths)
	{
		AffectedFiles.Add(Session.GetRepository().ToAbsolute(Path));
	}
	// Files whose changes are set aside during the pull change on disk too: the editor unloads and reloads them as well.
	const FPullPlan Plan = PlanPull(*Session.GetSnapshot(), Source, Pull);
	if (Source.Behind > 0 && Plan.bStash && Pull.bStash)
	{
		for (const FString& Path : Plan.Stashed)
		{
			AffectedFiles.AddUnique(Session.GetRepository().ToAbsolute(Path));
		}
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
	const FPullOptions& Pull = Command.Settings.Pull;
	Session.CancelBackgroundWork();
	FString Error;
	if (!Session.Fetch(ECallKind::UserNetwork, Command.Cancel, &Error, Pull.Remote))
	{
		Command.AddError(Error);
		return false;
	}
	if (!RefreshForCommand(Command, true))
	{
		return false;
	}
	const FSessionSnapshotRef Snapshot = Session.GetSnapshot();
	if (Snapshot->Status.Branch.IsEmpty())
	{
		Command.AddError(LOCTEXT("SyncDetached", "Switch to a branch before pulling; HEAD is detached."));
		return false;
	}
	FRemoteState Source;
	if (!ReadPullSource(Command, Source, Error))
	{
		Command.AddError(Error);
		return false;
	}
	if (Source.Upstream.IsEmpty())
	{
		Command.AddInfo(LOCTEXT("SyncUnpublished", "This branch has not been published yet, so there is nothing to pull."));
		return true;
	}
	FBranchCopy Copy;
	FText CopyError;
	if (Pull.bSyncBranch && !ReadBranchCopy(Repository, *Snapshot, Source, Copy, CopyError))
	{
		Command.AddError(CopyError);
		return false;
	}
	if (Source.Behind == 0)
	{
		// The branch is on top already; its copy on the server may still be waiting from a sync whose push failed.
		bool bCopyUpdated = false;
		if (!Copy.Name.IsEmpty() && !UpdateBranchCopy(Command, Copy, Source, bCopyUpdated))
		{
			return false;
		}
		Command.AddInfo(bCopyUpdated ? FText::Format(LOCTEXT("SyncCopyCaughtUp", "Already up to date with {0}. {1} on the server now matches."), FText::FromString(Source.Upstream), FText::FromString(Copy.Name))
			: LOCTEXT("SyncUpToDate", "Already up to date."));
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
		for (const FString& Path : Source.IncomingPaths)
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

	const FPullPlan Plan = PlanPull(*Snapshot, Source, Pull);
	if (!Plan.NewInTheWay.IsEmpty())
	{
		Command.AddError(FText::Format(LOCTEXT("SyncNewInTheWay", "Nothing was changed: new files of yours have the same names as files coming from the server ({0}). Move or delete them first."),
			FText::FromString(FileList(Plan.NewInTheWay))));
		return false;
	}
	if (Plan.bStash && !Pull.bStash)
	{
		Command.AddError(Plan.Overlap.IsEmpty()
			? LOCTEXT("SyncNeedsStash", "Nothing was changed: rebasing needs your local changes out of the way. Submit, stash or revert them first, or pull with Stash and reapply local changes.")
			: FText::Format(LOCTEXT("SyncOverlap", "Nothing was changed: you have local changes to files that also changed on the server ({0}). Submit, stash or revert them first, or pull with Stash and reapply local changes."),
				FText::FromString(FileList(Plan.Overlap))));
		return false;
	}
	if (Plan.Mode == EIntegrate::Rebase)
	{
		// A rebase rewrites the commits it replays, so never ones the server already has; lean branching's Sync
		// may rewrite those on the branch's own copy, which it moves along afterwards.
		TArray<FString> Unpublished = {TEXT("HEAD"), TEXT("--not")};
		if (!Copy.Name.IsEmpty())
		{
			Unpublished.Add(TEXT("--exclude=") + Copy.Name);
		}
		Unpublished.Add(TEXT("--remotes"));
		int32 Count = 0;
		if (!Repository.CountCommits(Unpublished, Count, Error))
		{
			Command.AddError(Error);
			return false;
		}
		if (Count < Source.Ahead)
		{
			Command.AddError(Pull.bSyncBranch
				? LOCTEXT("SyncBranchShared", "Nothing was changed: some of this branch's commits are also on other branches on the server, and rebasing would leave those behind. Pull with merge instead.")
				: FText::Format(LOCTEXT("SyncRewritesPublished", "Nothing was changed: rebasing onto {0} would rewrite commits that are already on the server. Pull from the Git panel with Rebase instead of merge unticked to merge them instead."),
					FText::FromString(Source.Upstream)));
			return false;
		}
	}
	if (Plan.Mode != EIntegrate::FastForward)
	{
		bool bConflicts = false;
		if (!Repository.PredictConflicts(Source.Upstream, bConflicts, Error))
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
	if (!Plan.Overlap.IsEmpty())
	{
		// Local changes to incoming files are put back on top of them: only when the two combine without a conflict.
		bool bConflicts = false;
		if (!Repository.PredictReapplyConflicts(Source.Upstream, bConflicts, Error))
		{
			Command.AddError(Error);
			return false;
		}
		if (bConflicts)
		{
			Command.AddError(FText::Format(LOCTEXT("SyncOverlapConflict", "Nothing was changed: you have local changes to files that also changed on the server, and the two cannot be combined ({0}). Submit, stash or revert yours first."),
				FText::FromString(FileList(Plan.Overlap))));
			return false;
		}
	}
	const FOpResult Downloaded = Repository.FetchLfsObjects(Source.Remote.IsEmpty() ? Repository.DefaultRemote() : Source.Remote, Source.Upstream, Command.Cancel);
	if (!Downloaded.bOk)
	{
		Command.AddError(Downloaded.Message);
		return false;
	}

	const FString OldHead = Snapshot->Status.Head;
	// Set-aside changes come back after the pull; if they cannot, they wait in the Git panel's Stashes tab under this name.
	const FString StashName = TEXT("Local changes set aside by Pull");
	if (Plan.bStash)
	{
		const FOpResult Stashed = Repository.StashPush({}, StashName, false);
		if (!Stashed.bOk)
		{
			Command.AddError(FText::Format(LOCTEXT("SyncStashFailed", "Nothing was changed: your local changes could not be set aside. {0}"), FText::FromString(Stashed.Message)));
			Command.AddError(Stashed.Details);
			Session.RefreshStatus();
			return false;
		}
	}
	const FOpResult Integrated = Repository.Integrate(Source.Upstream, Plan.Mode, Command.Cancel);
	if (!Integrated.bOk)
	{
		if (Plan.bStash)
		{
			ReapplyStash(Repository);
		}
		Session.RefreshStatus();
		Command.AddError(Integrated.Message);
		Command.AddError(Integrated.Details);
		return false;
	}
	const bool bRestored = !Plan.bStash || ReapplyStash(Repository);
	Session.RefreshStatus();
	bool bCopyUpdated = false;
	const bool bCopyInStep = Copy.Name.IsEmpty() || UpdateBranchCopy(Command, Copy, Source, bCopyUpdated);
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
	if (!bRestored)
	{
		Command.AddError(FText::Format(LOCTEXT("SyncNotRestored", "Pulled, but your local changes could not be put back. They are kept as the stash \"{0}\" in the Git panel's Stashes tab."),
			FText::FromString(StashName)));
	}
	if (Pull.bSyncBranch)
	{
		Command.AddInfo(FText::Format(bCopyUpdated
			? LOCTEXT("SyncedBranchAndCopy", "Synced with {1}: {0} new {0}|plural(one=commit,other=commits). {2} on the server now matches.")
			: LOCTEXT("SyncedBranch", "Synced with {1}: {0} new {0}|plural(one=commit,other=commits)."), Source.Behind, FText::FromString(Source.Upstream), FText::FromString(Copy.Name)));
	}
	else
	{
		Command.AddInfo(FText::Format(Plan.Mode == EIntegrate::Merge
			? LOCTEXT("Merged", "Merged {0} {0}|plural(one=commit,other=commits) from {1}.")
			: LOCTEXT("Pulled", "Pulled {0} {0}|plural(one=commit,other=commits) from {1}."), Source.Behind, FText::FromString(Source.Upstream)));
	}
	Session.RequestLockRefresh(false);
	Session.RequestStashesAndBranches();
	return bRestored && bCopyInStep;
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
	// Lockable files are locked before they go, so nobody else edits them meanwhile.
	auto NeedsLock = [&Command](const FSessionSnapshot& Snapshot, const FString& Path)
	{
		return Snapshot.IsTracked(Path) && IsLockable(Snapshot, Command, Path) && !IsLockedHere(Snapshot, Path);
	};
	{
		const FSessionSnapshotRef Snapshot = Session.GetSnapshot();
		Command.Progress->Start(Command.RelativeFiles.FilterByPredicate([&](const FString& Path) { return NeedsLock(*Snapshot, Path); }).Num(), false);
	}
	bool bAllDeleted = true;
	for (const FString& Path : Command.RelativeFiles)
	{
		if (NeedsLock(*Session.GetSnapshot(), Path))
		{
			const bool bLocked = AcquireLock(Command, Path);
			++Command.Progress->Done;
			if (!bLocked)
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
