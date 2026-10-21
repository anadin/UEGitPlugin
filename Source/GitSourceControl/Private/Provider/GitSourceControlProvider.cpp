// Copyright (c) 2014-2020 Sebastien Rombauts (sebastien.rombauts@gmail.com)
//
// Distributed under the MIT License (MIT) (See accompanying file LICENSE.txt
// or copy at http://opensource.org/licenses/MIT)

#include "GitSourceControlProvider.h"

#include "GitProviderIdentity.h"
#include "GitSourceControlUserSettings.h"
#include "GitSourceControlWorkers.h"
#include "GitStateBuilder.h"
#include "SGitSourceControlSettings.h"

#include "Framework/Notifications/NotificationManager.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformApplicationMisc.h"
#include "HAL/PlatformTime.h"
#include "Interfaces/IPluginManager.h"
#include "ISourceControlModule.h"
#include "Logging/MessageLog.h"
#include "Misc/Paths.h"
#include "ScopedSourceControlProgress.h"
#include "SourceControlOperations.h"
#include "UObject/ObjectSaveContext.h"
#include "UObject/Package.h"
#include "Widgets/Notifications/SNotificationList.h"

#define LOCTEXT_NAMESPACE "GitSourceControl"

namespace
{
	FString NormalizeFile(const FString& File)
	{
		FString Absolute = FPaths::ConvertRelativePathToFull(File);
		FPaths::NormalizeFilename(Absolute);
		return Absolute;
	}

	bool IsDirectoryPath(const FString& Path)
	{
		return Path.EndsWith(TEXT("/")) || Path.EndsWith(TEXT("\\")) || IFileManager::Get().DirectoryExists(*Path);
	}

	/** Operations whose failures the user should see as a notification, not just in the log. */
	bool IsUserFacing(FName Operation)
	{
		return Operation == TEXT("CheckIn") || Operation == TEXT("CheckOut") || Operation == TEXT("Revert") || Operation == TEXT("Sync") || Operation == TEXT("Delete");
	}

	FText Age(const FDateTime& Utc)
	{
		const double Seconds = FMath::Max(0.0, (FDateTime::UtcNow() - Utc).GetTotalSeconds());
		if (Seconds < 60.0)
		{
			return LOCTEXT("AgeJustNow", "just now");
		}
		if (Seconds < 3600.0)
		{
			return FText::Format(LOCTEXT("AgeMinutes", "{0} min ago"), FMath::FloorToInt(Seconds / 60.0));
		}
		return FText::Format(LOCTEXT("AgeHours", "{0} h ago"), FMath::FloorToInt(Seconds / 3600.0));
	}
}

FGitSourceControlProvider::FGitSourceControlProvider() = default;

FGitSourceControlProvider::FGitSourceControlProvider(const FString& InRepositoryFolder)
	: RepositoryFolder(FPaths::ConvertRelativePathToFull(InRepositoryFolder))
{
}

FGitSourceControlProvider::~FGitSourceControlProvider()
{
	Close();
}

void FGitSourceControlProvider::Init(bool bForceConnection)
{
	if (RepositoryFolder.IsEmpty())
	{
		Settings.LoadSettings();
	}
	if (!Session.IsValid())
	{
		OpenSession();
	}
	if (!PackageSavedHandle.IsValid() && !IsRunningCommandlet() && RepositoryFolder.IsEmpty())
	{
		PackageSavedHandle = UPackage::PackageSavedWithContextEvent.AddRaw(this, &FGitSourceControlProvider::OnPackageSaved);
	}
}

bool FGitSourceControlProvider::OpenSession(FText* OutError)
{
	if (Session.IsValid())
	{
		return true;
	}
	const FString GitBinary = GitCore::FindGitExecutable(Settings.GetBinaryPath());
	if (GitBinary.IsEmpty())
	{
		if (OutError)
		{
			*OutError = LOCTEXT("GitNotFound", "Git was not found. Install Git (and Git LFS), or set the Git path in these settings.");
		}
		return false;
	}
	ToolVersions = GitCore::FGitRepository::ReadToolVersions(GitBinary);
	if (!ToolVersions.IsGitSupported())
	{
		const FText Message = FText::Format(LOCTEXT("GitTooOld", "Git Workspace needs Git 2.38 or newer; {0} reports \"{1}\"."),
			FText::FromString(GitBinary), FText::FromString(ToolVersions.Git));
		UE_LOG(LogSourceControl, Warning, TEXT("%s"), *Message.ToString());
		if (OutError)
		{
			*OutError = Message;
		}
		return false;
	}
	FString Error;
	const FString Folder = RepositoryFolder.IsEmpty() ? FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()) : RepositoryFolder;
	Session = GitCore::FGitSession::Open(GitBinary, Folder, Error);
	if (!Session.IsValid())
	{
		UE_LOG(LogSourceControl, Warning, TEXT("Git Workspace: %s"), *Error);
		if (OutError)
		{
			*OutError = FText::FromString(Error);
		}
		return false;
	}
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("GitSourceControl"));
	UE_LOG(LogSourceControl, Log, TEXT("Git Workspace %s: %s (%s), %s, repository %s"), Plugin.IsValid() ? *Plugin->GetDescriptor().VersionName : TEXT("?"),
		*ToolVersions.Git, *GitBinary, ToolVersions.Lfs.IsEmpty() ? TEXT("Git LFS not found") : *ToolVersions.Lfs, *Session->GetRepository().GetRoot());
	Session->SetLockRemote(Settings.GetLockRemote());
	SessionChangedHandle = Session->OnChanged().AddRaw(this, &FGitSourceControlProvider::OnSessionChanged);
	if (IsRunningCommandlet())
	{
		// Commandlets (such as World Partition builders) need answers before they continue.
		Session->EnqueueMutation([Weak = TWeakPtr<GitCore::FGitSession, ESPMode::ThreadSafe>(Session)]()
		{
			if (GitCore::FGitSessionPtr Pinned = Weak.Pin())
			{
				Pinned->RefreshStatus();
			}
		});
		Session->FlushMutations();
		if (Settings.IsUsingGitLfsLocking() && Session->GetSnapshot()->bLockableRules)
		{
			Session->RefreshLocks(GitCore::ECallKind::BackgroundNetwork);
		}
	}
	else
	{
		const double Now = FPlatformTime::Seconds();
		Session->RequestStatusRefresh();
		Session->RequestStashesAndBranches();
		LastStatusRequest = Now;
		if (Settings.IsUsingGitLfsLocking())
		{
			Session->RequestLockRefresh(false);
			LastLockRequest = Now;
		}
		if (GetDefault<UGitSourceControlUserSettings>()->BackgroundFetchSeconds > 0)
		{
			Session->RequestFetch(false);
			LastFetchRequest = Now;
		}
	}
	return true;
}

void FGitSourceControlProvider::Close()
{
	if (Session.IsValid())
	{
		Session->OnChanged().Remove(SessionChangedHandle);
		SessionChangedHandle.Reset();
		// Waits for the running git command, if any; queued work is dropped.
		Session->Close();
		Session.Reset();
	}
	// Commands still queued never ran: tell their callers they were cancelled.
	const TArray<FGitSourceControlCommandRef> Unfinished = MoveTemp(PendingCommands);
	for (const FGitSourceControlCommandRef& Command : Unfinished)
	{
		Command->CompleteDelegate.ExecuteIfBound(Command->Operation, Command->bDone && Command->bSucceeded ? ECommandResult::Succeeded : ECommandResult::Cancelled);
	}
	if (PackageSavedHandle.IsValid())
	{
		UPackage::PackageSavedWithContextEvent.Remove(PackageSavedHandle);
		PackageSavedHandle.Reset();
	}
	FScopeLock Lock(&StateCacheLock);
	StateCache.Empty();
	CachedGeneration = 0;
}

const FName& FGitSourceControlProvider::GetName() const
{
	return GitWorkspaceProvider::Name();
}

bool FGitSourceControlProvider::IsEnabled() const
{
	return Session.IsValid();
}

bool FGitSourceControlProvider::IsAvailable() const
{
	// Local checks only: badges must not vanish when the network does.
	return Session.IsValid();
}

bool FGitSourceControlProvider::IsLockingEnabled() const
{
	return Settings.IsUsingGitLfsLocking() && Session.IsValid() && Session->GetSnapshot()->bLockableRules;
}

bool FGitSourceControlProvider::UsesCheckout() const
{
	return IsLockingEnabled();
}

FText FGitSourceControlProvider::GetStatusText() const
{
	if (!Session.IsValid())
	{
		return LOCTEXT("NotConnected", "Not connected to a Git repository.");
	}
	const GitCore::FSessionSnapshotRef Snapshot = Session->GetSnapshot();
	TArray<FText> Lines;
	Lines.Add(FText::Format(LOCTEXT("StatusRepository", "Repository: {0}"), FText::FromString(Snapshot->Root)));
	const FText Branch = Snapshot->Status.Branch.IsEmpty() ? LOCTEXT("StatusDetached", "(detached)") : FText::FromString(Snapshot->Status.Branch);
	const GitCore::FRemoteState& Remote = Snapshot->Remote;
	if (Remote.Upstream.IsEmpty())
	{
		Lines.Add(FText::Format(LOCTEXT("StatusBranch", "Branch: {0}"), Branch));
	}
	else
	{
		const FText Counts = FText::Format(LOCTEXT("StatusCounts", "{0} ahead, {1} behind"), Remote.Ahead, Remote.Behind);
		Lines.Add(Remote.bFetched
			? FText::Format(LOCTEXT("StatusBranchFetched", "Branch: {0} → {1} ({2}, fetched {3})"), Branch, FText::FromString(Remote.Upstream), Counts, Age(Remote.FetchedAtUtc))
			: FText::Format(LOCTEXT("StatusBranchUpstream", "Branch: {0} → {1} ({2})"), Branch, FText::FromString(Remote.Upstream), Counts));
	}
	if (IsLockingEnabled())
	{
		Lines.Add(Snapshot->Locks.bVerified ? FText::Format(LOCTEXT("StatusLocks", "Locks: checked {0}"), Age(Snapshot->Locks.VerifiedAtUtc))
			: LOCTEXT("StatusLocksUnchecked", "Locks: not checked yet"));
	}
	Lines.Add(ToolVersions.Lfs.IsEmpty() ? FText::Format(LOCTEXT("StatusToolsNoLfs", "{0}, Git LFS not installed"), FText::FromString(ToolVersions.Git))
		: FText::FromString(ToolVersions.Git + TEXT(", ") + ToolVersions.Lfs));
	for (const FString* Error : {&Snapshot->StatusError, &Snapshot->Locks.Error, &Snapshot->Remote.Error})
	{
		if (!Error->IsEmpty())
		{
			Lines.Add(FText::FromString(*Error));
		}
	}
	return FText::Join(FText::FromString(TEXT("\n")), Lines);
}

TMap<ISourceControlProvider::EStatus, FString> FGitSourceControlProvider::GetStatus() const
{
	TMap<EStatus, FString> Result;
	Result.Add(EStatus::Enabled, IsEnabled() ? TEXT("Yes") : TEXT("No"));
	Result.Add(EStatus::Connected, IsAvailable() ? TEXT("Yes") : TEXT("No"));
	if (Session.IsValid())
	{
		const GitCore::FSessionSnapshotRef Snapshot = Session->GetSnapshot();
		Result.Add(EStatus::Repository, Snapshot->Root);
		Result.Add(EStatus::Branch, Snapshot->Status.Branch);
		Result.Add(EStatus::Remote, Snapshot->Remote.Upstream);
	}
	return Result;
}

TOptional<bool> FGitSourceControlProvider::HasChangesToSync() const
{
	if (!Session.IsValid() || !Session->GetSnapshot()->bStatusValid)
	{
		return {};
	}
	return Session->GetSnapshot()->Remote.Behind > 0;
}

TOptional<bool> FGitSourceControlProvider::HasChangesToCheckIn() const
{
	if (!Session.IsValid() || !Session->GetSnapshot()->bStatusValid)
	{
		return {};
	}
	const GitCore::FSessionSnapshotRef Snapshot = Session->GetSnapshot();
	if (Snapshot->Remote.Ahead > 0)
	{
		return true;
	}
	// Untracked files only count once they were marked for add.
	return Snapshot->Status.Entries.ContainsByPredicate([&Snapshot](const GitCore::FStatusEntry& Entry)
	{
		return !Entry.bUntracked || Snapshot->MarkedForAdd.Contains(Entry.Path);
	});
}

FGitCommandSettings FGitSourceControlProvider::MakeCommandSettings() const
{
	const UGitSourceControlUserSettings* User = GetDefault<UGitSourceControlUserSettings>();
	FGitCommandSettings Result;
	Result.bLockingEnabled = Settings.IsUsingGitLfsLocking();
	Result.bPushOnSubmit = User->bPushOnSubmit;
	Result.BackupsToKeep = User->BackupsToKeep;
	Result.BackupDays = User->BackupDays;
	const FString SavedDir = RepositoryFolder.IsEmpty() ? FPaths::ProjectSavedDir() : RepositoryFolder / TEXT("Saved");
	Result.BackupDirectory = FPaths::ConvertRelativePathToFull(SavedDir / TEXT("GitSourceControl/Backups"));
	return Result;
}

ECommandResult::Type FGitSourceControlProvider::GetState(const TArray<FString>& InFiles, TArray<FSourceControlStateRef>& OutState, EStateCacheUsage::Type InStateCacheUsage)
{
	if (!IsEnabled())
	{
		return ECommandResult::Failed;
	}
	if (InStateCacheUsage == EStateCacheUsage::ForceUpdate)
	{
		Execute(ISourceControlOperation::Create<FUpdateStatus>(), InFiles);
	}
	for (const FString& File : InFiles)
	{
		OutState.Add(GetStateInternal(NormalizeFile(File)));
	}
	return ECommandResult::Succeeded;
}

ECommandResult::Type FGitSourceControlProvider::GetState(const TArray<FSourceControlChangelistRef>& InChangelists, TArray<FSourceControlChangelistStateRef>& OutState, EStateCacheUsage::Type InStateCacheUsage)
{
	return ECommandResult::Failed;
}

TArray<FSourceControlStateRef> FGitSourceControlProvider::GetCachedStateByPredicate(TFunctionRef<bool(const FSourceControlStateRef&)> Predicate) const
{
	TArray<FSourceControlStateRef> Result;
	FScopeLock Lock(&StateCacheLock);
	for (const TPair<FString, FGitSourceControlStateRef>& Pair : StateCache)
	{
		const FSourceControlStateRef State = Pair.Value;
		if (Predicate(State))
		{
			Result.Add(State);
		}
	}
	return Result;
}

FGitSourceControlStateRef FGitSourceControlProvider::GetStateInternal(const FString& AbsoluteFile)
{
	FScopeLock Lock(&StateCacheLock);
	if (const FGitSourceControlStateRef* Existing = StateCache.Find(AbsoluteFile))
	{
		return *Existing;
	}
	FGitSourceControlStateRef State = MakeShared<FGitSourceControlState, ESPMode::ThreadSafe>(AbsoluteFile);
	if (Session.IsValid())
	{
		RebuildState(*State, *Session->GetSnapshot());
	}
	StateCache.Add(AbsoluteFile, State);
	return State;
}

void FGitSourceControlProvider::RebuildState(FGitSourceControlState& State, const GitCore::FSessionSnapshot& Snapshot) const
{
	GitProvider::FStateContext Context;
	Context.Snapshot = &Snapshot;
	Context.RenameSources = &RenameSources;
	Context.bLockingEnabled = Settings.IsUsingGitLfsLocking() && Snapshot.bLockableRules;
	Context.LockStaleSeconds = FMath::Max(900.0, 3.0 * GetDefault<UGitSourceControlUserSettings>()->LockRefreshSeconds);
	Context.Now = FDateTime::Now();
	const FString Relative = Session->GetRepository().ToRelative(State.LocalFilename);
	// Only paths git knows nothing about need a disk check (ignored vs. not created yet).
	const bool bKnown = Relative.IsEmpty() || Snapshot.Status.Find(Relative) || Snapshot.IsTracked(Relative) || RenameSources.Contains(Relative);
	const bool bExists = bKnown || IFileManager::Get().FileExists(*State.LocalFilename);
	GitProvider::BuildState(Context, Relative, bExists, State);
}

void FGitSourceControlProvider::RefreshCachedStates()
{
	if (!Session.IsValid())
	{
		return;
	}
	const GitCore::FSessionSnapshotRef Snapshot = Session->GetSnapshot();
	{
		FScopeLock Lock(&StateCacheLock);
		if (Snapshot->Generation == CachedGeneration)
		{
			return;
		}
		CachedGeneration = Snapshot->Generation;
		RenameSources = GitProvider::CollectRenameSources(Snapshot->Status);
		for (TPair<FString, FGitSourceControlStateRef>& Pair : StateCache)
		{
			RebuildState(*Pair.Value, *Snapshot);
		}
		// Changed and locked files are always cached: the editor finds pending deletes (and
		// other submittable files) through cache queries, not by asking for each file.
		auto AddMissing = [this, &Snapshot](const FString& Relative)
		{
			const FString Absolute = Session->GetRepository().ToAbsolute(Relative);
			if (!StateCache.Contains(Absolute))
			{
				const FGitSourceControlStateRef State = MakeShared<FGitSourceControlState, ESPMode::ThreadSafe>(Absolute);
				RebuildState(*State, *Snapshot);
				StateCache.Add(Absolute, State);
			}
		};
		for (const GitCore::FStatusEntry& Entry : Snapshot->Status.Entries)
		{
			AddMissing(Entry.Path);
		}
		for (const FString& Source : RenameSources)
		{
			AddMissing(Source);
		}
		for (const TPair<FString, GitCore::FLockInfo>& Pair : Snapshot->Locks.ByPath)
		{
			AddMissing(Pair.Key);
		}
	}
	OnSourceControlStateChanged.Broadcast();
	RepositoryChanged.Broadcast();
}

void FGitSourceControlProvider::OnSessionChanged(GitCore::EChange Changes)
{
	RefreshCachedStates();
}

void FGitSourceControlProvider::SetHistory(const FString& AbsoluteFile, const TArray<GitCore::FFileRevision>& Revisions)
{
	if (!Session.IsValid())
	{
		return;
	}
	FGitSourceControlStateRef State = GetStateInternal(AbsoluteFile);
	State->History.Reset();
	for (int32 Index = 0; Index < Revisions.Num(); ++Index)
	{
		const GitCore::FFileRevision& Source = Revisions[Index];
		TSharedRef<FGitSourceControlRevision, ESPMode::ThreadSafe> Revision = MakeShared<FGitSourceControlRevision, ESPMode::ThreadSafe>();
		Revision->LocalFilename = AbsoluteFile;
		Revision->PathInCommit = Source.Path;
		Revision->CommitId = Source.Commit;
		Revision->ShortCommitId = Source.Commit.Left(8);
		Revision->Description = Source.Subject;
		Revision->UserName = Source.Author;
		Revision->Date = Source.Date;
		Revision->RevisionNumber = Revisions.Num() - Index;
		Revision->Action = Source.Action == TEXT('A') ? TEXT("add") : Source.Action == TEXT('D') ? TEXT("delete")
			: (Source.Action == TEXT('R') || Source.Action == TEXT('C')) ? TEXT("branch") : TEXT("edit");
		Revision->Repository = Session->GetRepositoryRef();
		State->History.Add(Revision);
	}
}

FDelegateHandle FGitSourceControlProvider::RegisterSourceControlStateChanged_Handle(const FSourceControlStateChanged::FDelegate& SourceControlStateChanged)
{
	return OnSourceControlStateChanged.Add(SourceControlStateChanged);
}

void FGitSourceControlProvider::UnregisterSourceControlStateChanged_Handle(FDelegateHandle Handle)
{
	OnSourceControlStateChanged.Remove(Handle);
}

ECommandResult::Type FGitSourceControlProvider::Execute(const FSourceControlOperationRef& InOperation, FSourceControlChangelistPtr InChangelist, const TArray<FString>& InFiles,
	EConcurrency::Type InConcurrency, const FSourceControlOperationComplete& InOperationCompleteDelegate)
{
	const FName Name = InOperation->GetName();
	FText OpenError;
	if (!Session.IsValid() && (Name != TEXT("Connect") || !OpenSession(&OpenError)))
	{
		if (Name == TEXT("Connect"))
		{
			StaticCastSharedRef<FConnect>(InOperation)->SetErrorText(OpenError);
			InOperation->AddErrorMessge(OpenError);
		}
		InOperationCompleteDelegate.ExecuteIfBound(InOperation, ECommandResult::Failed);
		return ECommandResult::Failed;
	}
	const TSharedPtr<IGitSourceControlWorker, ESPMode::ThreadSafe> Worker = GitWorkers::Create(Name);
	if (!Worker.IsValid())
	{
		const FText Message = FText::Format(LOCTEXT("UnsupportedOperation", "Git Workspace does not support the '{0}' operation."), FText::FromName(Name));
		InOperation->AddErrorMessge(Message);
		InOperationCompleteDelegate.ExecuteIfBound(InOperation, ECommandResult::Failed);
		return ECommandResult::Failed;
	}

	const FGitSourceControlCommandRef Command = MakeShared<FGitSourceControlCommand, ESPMode::ThreadSafe>(InOperation, Worker.ToSharedRef());
	Command->Session = Session;
	Command->Settings = MakeCommandSettings();
	Command->CompleteDelegate = InOperationCompleteDelegate;
	Command->bWholeRepository = InFiles.IsEmpty();
	for (const FString& File : InFiles)
	{
		const FString Absolute = NormalizeFile(File);
		if (IsDirectoryPath(Absolute))
		{
			Command->bWholeRepository = true;
			continue;
		}
		const FString Relative = Session->GetRepository().ToRelative(Absolute);
		if (!Relative.IsEmpty())
		{
			Command->AbsoluteFiles.Add(Absolute);
			Command->RelativeFiles.Add(Relative);
		}
	}

	if (Session->IsMutationLane())
	{
		// Called from within a running operation: run inline to avoid waiting on ourselves.
		Command->bSucceeded = Command->Worker->Execute(*Command);
		Command->bDone = true;
		FinishCommand(*Command);
		return Command->bSucceeded ? ECommandResult::Succeeded : ECommandResult::Failed;
	}
	Session->EnqueueMutation([Command]()
	{
		Command->bSucceeded = Command->Worker->Execute(*Command);
		Command->bDone = true;
	});
	if (InConcurrency == EConcurrency::Synchronous)
	{
		return WaitForCommand(Command);
	}
	PendingCommands.Add(Command);
	return ECommandResult::Succeeded;
}

ECommandResult::Type FGitSourceControlProvider::WaitForCommand(const FGitSourceControlCommandRef& Command)
{
	{
		const GitCore::FCancelTokenPtr Cancel = Command->Cancel;
		FScopedSourceControlProgress Progress(Command->Operation->GetInProgressString(), FSimpleDelegate::CreateLambda([Cancel]() { Cancel->Cancel(); }));
		while (!Command->bDone)
		{
			Progress.Tick();
			FPlatformProcess::Sleep(0.01f);
		}
	}
	FinishCommand(*Command);
	if (Command->Cancel->IsCancelled())
	{
		return ECommandResult::Cancelled;
	}
	return Command->bSucceeded ? ECommandResult::Succeeded : ECommandResult::Failed;
}

void FGitSourceControlProvider::FinishCommand(FGitSourceControlCommand& Command)
{
	Command.Worker->UpdateStates(Command, *this);
	RefreshCachedStates();

	FMessageLog Log("SourceControl");
	for (const FText& Message : Command.ErrorMessages)
	{
		Command.Operation->AddErrorMessge(Message);
		Log.Error(Message);
	}
	for (const FText& Message : Command.InfoMessages)
	{
		Command.Operation->AddInfoMessge(Message);
		Log.Info(Message);
	}
	const FName Name = Command.Operation->GetName();
	if (!IsRunningCommandlet() && IsUserFacing(Name))
	{
		const bool bFailed = !Command.bSucceeded && !Command.ErrorMessages.IsEmpty();
		const TArray<FText>& Shown = bFailed ? Command.ErrorMessages : Command.InfoMessages;
		if (!Shown.IsEmpty() && Name != TEXT("CheckIn"))
		{
			FNotificationInfo Info(Shown[0]);
			Info.ExpireDuration = bFailed ? 8.0f : 5.0f;
			Info.bUseSuccessFailIcons = true;
			if (bFailed)
			{
				Info.Hyperlink = FSimpleDelegate::CreateLambda([]() { FMessageLog("SourceControl").Open(); });
				Info.HyperlinkText = LOCTEXT("ShowLog", "Show log");
			}
			if (TSharedPtr<SNotificationItem> Item = FSlateNotificationManager::Get().AddNotification(Info))
			{
				Item->SetCompletionState(bFailed ? SNotificationItem::CS_Fail : SNotificationItem::CS_Success);
			}
		}
		else if (bFailed && Name == TEXT("CheckIn"))
		{
			// The editor only reports "Failed to check in files!"; say what actually happened.
			Log.Notify(Command.ErrorMessages[0], EMessageSeverity::Error, true);
		}
	}
	Command.CompleteDelegate.ExecuteIfBound(Command.Operation,
		Command.Cancel->IsCancelled() ? ECommandResult::Cancelled : (Command.bSucceeded ? ECommandResult::Succeeded : ECommandResult::Failed));
}

bool FGitSourceControlProvider::CanExecuteOperation(const FSourceControlOperationRef& InOperation) const
{
	return GitWorkers::IsSupported(InOperation->GetName());
}

bool FGitSourceControlProvider::CanCancelOperation(const FSourceControlOperationRef& InOperation) const
{
	return PendingCommands.ContainsByPredicate([&InOperation](const FGitSourceControlCommandRef& Command) { return Command->Operation == InOperation; });
}

void FGitSourceControlProvider::CancelOperation(const FSourceControlOperationRef& InOperation)
{
	for (const FGitSourceControlCommandRef& Command : PendingCommands)
	{
		if (Command->Operation == InOperation)
		{
			Command->Cancel->Cancel();
		}
	}
}

void FGitSourceControlProvider::Tick()
{
	for (int32 Index = 0; Index < PendingCommands.Num(); ++Index)
	{
		if (PendingCommands[Index]->bDone)
		{
			const FGitSourceControlCommandRef Command = PendingCommands[Index];
			PendingCommands.RemoveAt(Index);
			FinishCommand(*Command);
			// Completion delegates may queue more commands; handle the rest next tick.
			break;
		}
	}
	if (Session.IsValid())
	{
		RefreshCachedStates();
		if (!IsRunningCommandlet())
		{
			TickScheduler();
		}
	}
}

void FGitSourceControlProvider::OnPackageSaved(const FString& PackageFilename, UPackage* Package, FObjectPostSaveContext Context)
{
	if (!Context.IsProceduralSave())
	{
		PackageSavedAt = FPlatformTime::Seconds();
	}
}

void FGitSourceControlProvider::TickScheduler()
{
	const UGitSourceControlUserSettings* User = GetDefault<UGitSourceControlUserSettings>();
	const GitCore::FSessionSnapshotRef Snapshot = Session->GetSnapshot();
	const double Now = FPlatformTime::Seconds();
	const bool bForeground = FPlatformApplicationMisc::IsThisApplicationForeground();
	const bool bLocking = IsLockingEnabled();
	// Back off while the server keeps failing (offline, signed out) instead of retrying constantly.
	const double LockInterval = Snapshot->Locks.Error.IsEmpty() ? User->LockRefreshSeconds : FMath::Min(4.0 * User->LockRefreshSeconds, 1800.0);
	const double FetchInterval = Snapshot->Remote.Error.IsEmpty() ? User->BackgroundFetchSeconds : FMath::Min(4.0 * User->BackgroundFetchSeconds, 1800.0);

	if (bForeground && !bWasForeground)
	{
		Session->RequestStatusRefresh();
		Session->RequestStashesAndBranches();
		LastStatusRequest = Now;
		if (bLocking && Now - LastLockRequest > 30.0)
		{
			Session->RequestLockRefresh(false);
			LastLockRequest = Now;
		}
		if (User->BackgroundFetchSeconds > 0 && Now - LastFetchRequest > 120.0)
		{
			Session->RequestFetch(false);
			LastFetchRequest = Now;
		}
	}
	bWasForeground = bForeground;

	if ((bForeground && Now - LastStatusRequest > User->StatusRefreshSeconds) || (PackageSavedAt > 0.0 && Now - PackageSavedAt > 1.0))
	{
		PackageSavedAt = 0.0;
		Session->RequestStatusRefresh();
		LastStatusRequest = Now;
	}
	if (bLocking && Now - LastLockRequest > LockInterval)
	{
		Session->RequestLockRefresh(false);
		LastLockRequest = Now;
	}
	if (User->BackgroundFetchSeconds > 0 && Now - LastFetchRequest > FetchInterval)
	{
		Session->RequestFetch(false);
		LastFetchRequest = Now;
	}
}

#if SOURCE_CONTROL_WITH_SLATE
TSharedRef<SWidget> FGitSourceControlProvider::MakeSettingsWidget() const
{
	return SNew(SGitSourceControlSettings);
}
#endif

#undef LOCTEXT_NAMESPACE
