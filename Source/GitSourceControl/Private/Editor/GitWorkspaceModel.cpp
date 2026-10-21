// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#include "GitWorkspaceModel.h"

#include "GitSourceControlModule.h"
#include "GitWorkspaceText.h"
#include "Provider/GitProviderIdentity.h"
#include "Provider/GitSourceControlRevision.h"

#include "AssetDefinition.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "Async/Async.h"
#include "ContentBrowserModule.h"
#include "Containers/Ticker.h"
#include "DiffUtils.h"
#include "Editor.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Notifications/NotificationManager.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "IContentBrowserSingleton.h"
#include "ISourceControlModule.h"
#include "Logging/MessageLog.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopedSlowTask.h"
#include "SourceControlHelpers.h"
#include "SourceControlOperations.h"
#include "SourceControlWindows.h"
#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "UObject/Package.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Widgets/SWindow.h"

#define LOCTEXT_NAMESPACE "GitWorkspaceModel"

namespace
{
	TSharedPtr<FGitWorkspaceModel> Instance;

	FGitSourceControlProvider& ProviderInstance()
	{
		return FGitSourceControlModule::Get().GetProvider();
	}

	bool IsPackagePath(const FString& Path)
	{
		return FPackageName::IsPackageExtension(*FPaths::GetExtension(Path));
	}

	bool IsExternalPackage(const FString& Path)
	{
		return Path.Contains(TEXT("/__ExternalActors__/")) || Path.Contains(TEXT("/__ExternalObjects__/"));
	}

	/** Lock check, LFS upload and push of every outgoing commit. Runs on the mutation lane. */
	FGitActionResult PushNow(GitCore::FGitSession& Session, bool bLocking, const GitCore::FCancelTokenPtr& Cancel)
	{
		const GitCore::FGitRepository& Repository = Session.GetRepository();
		Session.RefreshStatus();
		if (bLocking)
		{
			// The server refuses pushes that change files someone else has locked; say so before trying.
			Session.RefreshLocks(GitCore::ECallKind::UserNetwork);
			const GitCore::FSessionSnapshotRef Snapshot = Session.GetSnapshot();
			TArray<FString> Blocked;
			for (const FString& Path : Snapshot->Remote.OutgoingPaths)
			{
				const GitCore::FLockInfo* Lock = Snapshot->Locks.Find(Path);
				if (Lock && !Lock->bOurs)
				{
					Blocked.Add(Path);
				}
			}
			if (!Blocked.IsEmpty())
			{
				return FGitActionResult::Failure(FText::Format(LOCTEXT("PushBlocked", "Not pushed: {0} is locked by someone else."), GitWorkspaceText::FileList(Blocked)));
			}
		}
		const GitCore::FSessionSnapshotRef Snapshot = Session.GetSnapshot();
		Session.CancelBackgroundWork();
		const GitCore::FOpResult Pushed = Repository.Push(Snapshot->Status, Repository.DefaultRemote(), Cancel);
		Session.RefreshStatus();
		Session.RefreshRemote();
		return FGitActionResult::FromOp(Pushed);
	}

	/** Waits on the game thread for Op to run on the mutation lane, with a progress dialog. */
	FGitActionResult RunAndWait(const GitCore::FGitSessionRef& Session, const FText& Activity, TFunctionRef<FGitActionResult(GitCore::FGitSession&)> Op)
	{
		struct FShared
		{
			FThreadSafeBool bDone;
			FGitActionResult Result;
		};
		const TSharedRef<FShared, ESPMode::ThreadSafe> Shared = MakeShared<FShared, ESPMode::ThreadSafe>();
		// The game thread waits below, so referencing Op from the lane is safe.
		Session->EnqueueMutation([Session, Shared, &Op]()
		{
			Shared->Result = Op(*Session);
			Shared->bDone = true;
		});
		FScopedSlowTask Progress(1.0f, Activity);
		Progress.MakeDialogDelayed(0.3f);
		while (!Shared->bDone)
		{
			Progress.TickProgress();
			FPlatformProcess::Sleep(0.02f);
		}
		return Shared->Result;
	}
}

FGitActionResult FGitActionResult::Success(const FText& InMessage)
{
	FGitActionResult Result;
	Result.Message = InMessage;
	return Result;
}

FGitActionResult FGitActionResult::Failure(const FText& InMessage, const FString& InDetails)
{
	FGitActionResult Result;
	Result.bOk = false;
	Result.Message = InMessage;
	Result.Details = InDetails;
	return Result;
}

FGitActionResult FGitActionResult::FromOp(const GitCore::FOpResult& Op)
{
	return Op.bOk ? Success(FText::FromString(Op.Message)) : Failure(FText::FromString(Op.Message), Op.Details);
}

FGitWorkspaceModel* FGitWorkspaceModel::Get()
{
	return Instance.Get();
}

void FGitWorkspaceModel::Startup()
{
	Instance = MakeShared<FGitWorkspaceModel>();
	Instance->RepositoryChangedHandle = ProviderInstance().OnRepositoryChanged().AddSP(Instance.ToSharedRef(), &FGitWorkspaceModel::OnRepositoryChanged);
}

void FGitWorkspaceModel::Shutdown()
{
	if (Instance.IsValid())
	{
		ProviderInstance().OnRepositoryChanged().Remove(Instance->RepositoryChangedHandle);
		Instance.Reset();
	}
}

TSharedRef<FGitWorkspaceModel> FGitWorkspaceModel::CreateFor(FGitSourceControlProvider& InProvider)
{
	const TSharedRef<FGitWorkspaceModel> Model = MakeShared<FGitWorkspaceModel>();
	Model->BoundProvider = &InProvider;
	Model->RepositoryChangedHandle = InProvider.OnRepositoryChanged().AddSP(Model, &FGitWorkspaceModel::OnRepositoryChanged);
	return Model;
}

FGitWorkspaceModel::~FGitWorkspaceModel()
{
	if (BoundProvider)
	{
		BoundProvider->OnRepositoryChanged().Remove(RepositoryChangedHandle);
	}
}

FGitSourceControlProvider& FGitWorkspaceModel::GetProvider() const
{
	return BoundProvider ? *BoundProvider : ProviderInstance();
}

void FGitWorkspaceModel::RunAtSafePoint(TFunction<void()> Task) const
{
	if (BoundProvider)
	{
		Task();
		return;
	}
	FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([Task = MoveTemp(Task)](float)
	{
		Task();
		return false;
	}));
}

bool FGitWorkspaceModel::IsActive() const
{
	if (BoundProvider)
	{
		return BoundProvider->GetSession().IsValid();
	}
	ISourceControlModule& SourceControl = ISourceControlModule::Get();
	return SourceControl.IsEnabled() && SourceControl.GetProvider().GetName() == GitWorkspaceProvider::Name() && ProviderInstance().GetSession().IsValid();
}

GitCore::FGitSessionPtr FGitWorkspaceModel::GetSession() const
{
	return IsActive() ? GetProvider().GetSession() : nullptr;
}

GitCore::FSessionSnapshotRef FGitWorkspaceModel::GetSnapshot() const
{
	static const GitCore::FSessionSnapshotRef Empty = MakeShared<GitCore::FSessionSnapshot, ESPMode::ThreadSafe>();
	const GitCore::FGitSessionPtr Session = GetSession();
	return Session.IsValid() ? Session->GetSnapshot() : Empty;
}

bool FGitWorkspaceModel::IsLockingEnabled() const
{
	return IsActive() && GetProvider().IsLockingEnabled();
}

bool FGitWorkspaceModel::IsBusy() const
{
	// A session closed under a running action (provider switched off) ends it.
	return !Activity.IsEmpty() && ActivitySession.IsValid();
}

void FGitWorkspaceModel::Cancel()
{
	if (ActivityCancel.IsValid())
	{
		ActivityCancel->Cancel();
	}
}

void FGitWorkspaceModel::DismissFailure()
{
	LastFailure.Reset();
	Changed.Broadcast();
}

void FGitWorkspaceModel::OnRepositoryChanged()
{
	Changed.Broadcast();
}

bool FGitWorkspaceModel::Run(const FText& InActivity, ELane Lane, FWork Work, FDone Done, bool bQuietSuccess)
{
	const GitCore::FGitSessionPtr Session = GetSession();
	if (!Session.IsValid() || IsBusy())
	{
		return false;
	}
	Activity = InActivity;
	ActivitySession = Session;
	ActivityCancel = MakeShared<GitCore::FCancelToken, ESPMode::ThreadSafe>();
	LastFailure.Reset();
	Changed.Broadcast();
	auto Task = [Weak = AsWeak(), Session, Cancel = ActivityCancel, Work = MoveTemp(Work), Done = MoveTemp(Done), bQuietSuccess]() mutable
	{
		FGitActionResult Result = Work(*Session, Cancel);
		AsyncTask(ENamedThreads::GameThread, [Weak, Result = MoveTemp(Result), Done = MoveTemp(Done), bQuietSuccess]() mutable
		{
			if (const TSharedPtr<FGitWorkspaceModel> Self = Weak.Pin())
			{
				Self->Finish(Result, bQuietSuccess);
				if (Done)
				{
					Done(Result);
				}
			}
		});
	};
	if (Lane == ELane::Mutation)
	{
		Session->EnqueueMutation(MoveTemp(Task));
	}
	else
	{
		Session->EnqueueBackground(MoveTemp(Task));
	}
	return true;
}

void FGitWorkspaceModel::RunQuick(FWork Work)
{
	const GitCore::FGitSessionPtr Session = GetSession();
	if (!Session.IsValid())
	{
		return;
	}
	Session->EnqueueMutation([Weak = AsWeak(), Session, Work = MoveTemp(Work)]() mutable
	{
		FGitActionResult Result = Work(*Session, nullptr);
		if (!Result.bOk)
		{
			AsyncTask(ENamedThreads::GameThread, [Weak, Result = MoveTemp(Result)]()
			{
				if (const TSharedPtr<FGitWorkspaceModel> Self = Weak.Pin())
				{
					Self->LastFailure = Result;
					Self->Present(Result);
					Self->Changed.Broadcast();
				}
			});
		}
	});
}

void FGitWorkspaceModel::Finish(const FGitActionResult& Result, bool bQuietSuccess)
{
	Activity = FText::GetEmpty();
	ActivitySession.Reset();
	ActivityCancel.Reset();
	if (!Result.bOk)
	{
		LastFailure = Result;
	}
	if (!Result.bOk || !bQuietSuccess)
	{
		Present(Result);
	}
	Changed.Broadcast();
}

void FGitWorkspaceModel::Present(const FGitActionResult& Result)
{
	if (Result.Message.IsEmpty())
	{
		return;
	}
	FNotificationInfo Info(Result.Message);
	Info.ExpireDuration = Result.bOk ? 4.0f : 8.0f;
	Info.bUseSuccessFailIcons = true;
	if (!Result.bOk)
	{
		FMessageLog Log("SourceControl");
		Log.Error(Result.Message);
		if (!Result.Details.IsEmpty())
		{
			Log.Info(FText::FromString(Result.Details));
		}
		Info.Hyperlink = FSimpleDelegate::CreateLambda([]() { FMessageLog("SourceControl").Open(); });
		Info.HyperlinkText = LOCTEXT("ShowLog", "Show log");
	}
	else if (Result.bOfferPush)
	{
		Info.ExpireDuration = 8.0f;
		Info.ButtonDetails.Add(FNotificationButtonInfo(LOCTEXT("PushNow", "Push"), LOCTEXT("PushNowTooltip", "Publish your commits now."),
			FSimpleDelegate::CreateSP(this, &FGitWorkspaceModel::Push), SNotificationItem::CS_Success));
	}
	if (const TSharedPtr<SNotificationItem> Item = FSlateNotificationManager::Get().AddNotification(Info))
	{
		Item->SetCompletionState(Result.bOk ? SNotificationItem::CS_Success : SNotificationItem::CS_Fail);
	}
}

void FGitWorkspaceModel::Fetch()
{
	Run(LOCTEXT("Fetching", "Checking for new commits…"), ELane::Background, [](GitCore::FGitSession& Session, const GitCore::FCancelTokenPtr& Cancel)
	{
		FString Error;
		if (!Session.Fetch(GitCore::ECallKind::UserNetwork, Cancel, &Error))
		{
			return FGitActionResult::Failure(FText::FromString(Error));
		}
		const GitCore::FRemoteState& Remote = Session.GetSnapshot()->Remote;
		if (Remote.Upstream.IsEmpty())
		{
			return FGitActionResult::Success(LOCTEXT("FetchedNoUpstream", "Fetched. This branch is not published yet."));
		}
		return Remote.Behind > 0
			? FGitActionResult::Success(FText::Format(LOCTEXT("FetchedBehind", "{0} new {0}|plural(one=commit,other=commits) on {1}."), Remote.Behind, FText::FromString(Remote.Upstream)))
			: FGitActionResult::Success(LOCTEXT("FetchedUpToDate", "You are up to date."));
	});
}

void FGitWorkspaceModel::Push()
{
	const bool bLocking = IsLockingEnabled();
	Run(LOCTEXT("Pushing", "Pushing…"), ELane::Mutation, [bLocking](GitCore::FGitSession& Session, const GitCore::FCancelTokenPtr& Cancel)
	{
		return PushNow(Session, bLocking, Cancel);
	});
}

void FGitWorkspaceModel::Sync()
{
	if (IsActive() && !IsBusy())
	{
		FSourceControlWindows::SyncLatest();
	}
}

void FGitWorkspaceModel::CheckIn()
{
	if (IsActive())
	{
		FSourceControlWindows::ChoosePackagesToCheckIn();
	}
}

void FGitWorkspaceModel::CheckLocks()
{
	Run(LOCTEXT("CheckingLocks", "Checking locks…"), ELane::Background, [](GitCore::FGitSession& Session, const GitCore::FCancelTokenPtr&)
	{
		FString Error;
		if (!Session.RefreshLocks(GitCore::ECallKind::UserNetwork, &Error))
		{
			return FGitActionResult::Failure(FText::FromString(Error.IsEmpty() ? Session.GetSnapshot()->Locks.Error : Error));
		}
		int32 Mine = 0;
		const GitCore::FLockSnapshot& Locks = Session.GetSnapshot()->Locks;
		for (const TPair<FString, GitCore::FLockInfo>& Pair : Locks.ByPath)
		{
			Mine += Pair.Value.bOurs ? 1 : 0;
		}
		return FGitActionResult::Success(FText::Format(LOCTEXT("LocksChecked", "{0} {0}|plural(one=lock,other=locks) on the server, {1} yours."), Locks.ByPath.Num(), Mine));
	});
}

void FGitWorkspaceModel::Stage(const TArray<FString>& Paths)
{
	if (Paths.IsEmpty())
	{
		return;
	}
	RunQuick([Paths](GitCore::FGitSession& Session, const GitCore::FCancelTokenPtr&)
	{
		const GitCore::FOpResult Result = Session.GetRepository().Stage(Paths);
		Session.RefreshStatus(&Paths);
		return FGitActionResult::FromOp(Result);
	});
}

void FGitWorkspaceModel::Unstage(const TArray<FString>& Paths)
{
	if (Paths.IsEmpty())
	{
		return;
	}
	RunQuick([Paths](GitCore::FGitSession& Session, const GitCore::FCancelTokenPtr&)
	{
		const GitCore::FOpResult Result = Session.GetRepository().Unstage(Paths);
		Session.RefreshStatus(&Paths);
		return FGitActionResult::FromOp(Result);
	});
}

void FGitWorkspaceModel::Commit(const FString& Message, bool bPush, TFunction<void(bool bCommitted)> OnDone)
{
	// The commit must contain what the user reviewed; the repository refuses if HEAD or the index moved since.
	const GitCore::FSessionSnapshotRef Seen = GetSnapshot();
	const FString Head = Seen->Status.Head;
	const FString StagedToken = Seen->Status.StagedToken;
	const int32 Count = Seen->Status.StagedCount();
	const bool bLocking = IsLockingEnabled();
	const FText Doing = bPush ? LOCTEXT("CommittingAndPushing", "Committing and pushing…") : LOCTEXT("Committing", "Committing…");
	const bool bStarted = Run(Doing, ELane::Mutation, [Message, bPush, Head, StagedToken, Count, bLocking](GitCore::FGitSession& Session, const GitCore::FCancelTokenPtr& Cancel)
	{
		FString Commit;
		const GitCore::FOpResult Committed = Session.GetRepository().CommitIndex(Message, Head, StagedToken, Commit);
		Session.RefreshStatus();
		if (!Committed.bOk)
		{
			return FGitActionResult::FromOp(Committed);
		}
		const FText Id = FText::FromString(GitWorkspaceText::ShortId(Commit));
		FGitActionResult Result = FGitActionResult::Success(FText::Format(LOCTEXT("Committed", "Committed {0} {0}|plural(one=file,other=files) as {1}."), Count, Id));
		Result.Paths = {Commit};
		if (!bPush)
		{
			Result.bOfferPush = true;
			return Result;
		}
		const FGitActionResult Pushed = PushNow(Session, bLocking, Cancel);
		if (!Pushed.bOk)
		{
			FGitActionResult Failed = FGitActionResult::Failure(FText::Format(LOCTEXT("CommittedNotPushed", "Committed {0} locally, but not pushed: {1}"), Id, Pushed.Message), Pushed.Details);
			Failed.Paths = {Commit};
			return Failed;
		}
		Result.Message = FText::Format(LOCTEXT("CommittedAndPushed", "Committed {0} {0}|plural(one=file,other=files) as {1} and pushed."), Count, Id);
		return Result;
	},
	[OnDone](const FGitActionResult& Result)
	{
		if (OnDone)
		{
			OnDone(!Result.Paths.IsEmpty());
		}
	});
	if (!bStarted && OnDone)
	{
		OnDone(false);
	}
}

void FGitWorkspaceModel::Discard(const TArray<FString>& Paths)
{
	const GitCore::FGitSessionPtr Session = GetSession();
	if (!Session.IsValid() || Paths.IsEmpty() || IsBusy())
	{
		return;
	}
	Activity = LOCTEXT("Discarding", "Discarding changes…");
	ActivitySession = Session;
	Changed.Broadcast();
	RunAtSafePoint([Weak = AsWeak(), Session, Paths]()
	{
		const TSharedPtr<FGitWorkspaceModel> Self = Weak.Pin();
		if (!Self.IsValid())
		{
			return;
		}
		const GitCore::FSessionSnapshotRef Snapshot = Session->GetSnapshot();
		TArray<FString> Packages;
		TArray<FString> Others;
		TArray<FString> NewOthers;
		for (const FString& Path : Paths)
		{
			const FString File = Session->GetRepository().ToAbsolute(Path);
			const GitCore::FStatusEntry* Entry = Snapshot->Status.Find(Path);
			if (IsPackagePath(Path))
			{
				Packages.Add(File);
			}
			else if (Entry && Entry->bUntracked)
			{
				NewOthers.Add(File);
			}
			else
			{
				Others.Add(File);
			}
		}
		bool bOk = true;
		if (Self->BoundProvider)
		{
			// Nothing is loaded from a test repository: the provider reverts everything directly.
			Others.Append(Packages);
			Packages.Reset();
		}
		// The editor unloads the packages, the provider reverts them (keeping a backup) and the editor reloads them.
		if (!Packages.IsEmpty())
		{
			bOk &= USourceControlHelpers::RevertAndReloadPackages(Packages, false, true);
		}
		if (!Others.IsEmpty())
		{
			bOk &= Self->GetProvider().Execute(ISourceControlOperation::Create<FRevert>(), Others) == ECommandResult::Succeeded;
		}
		for (const FString& File : NewOthers)
		{
			bOk &= IFileManager::Get().Delete(*File, false, true, true);
		}
		Session->RequestStatusRefresh();
		Self->Finish(bOk ? FGitActionResult::Success(FText::Format(LOCTEXT("Discarded", "Discarded changes to {0}."), GitWorkspaceText::FileList(Paths)))
			: FGitActionResult::Failure(LOCTEXT("DiscardFailed", "Some changes could not be discarded. See the Source Control log.")), false);
	});
}

void FGitWorkspaceModel::Lock(const TArray<FString>& Paths)
{
	TArray<FString> Files;
	for (const FString& Path : Paths)
	{
		Files.Add(ToAbsolute(Path));
	}
	if (IsActive() && !Files.IsEmpty())
	{
		GetProvider().Execute(ISourceControlOperation::Create<FCheckOut>(), Files, EConcurrency::Asynchronous);
	}
}

void FGitWorkspaceModel::Unlock(const TArray<FString>& Paths, bool bForce)
{
	Run(LOCTEXT("Unlocking", "Releasing locks…"), ELane::Mutation, [Paths, bForce](GitCore::FGitSession& Session, const GitCore::FCancelTokenPtr&)
	{
		const GitCore::FSessionSnapshotRef Snapshot = Session.GetSnapshot();
		const FString Remote = Session.ResolveLockRemote();
		TArray<FString> Released;
		TArray<FString> Failed;
		FString Details;
		for (const FString& Path : Paths)
		{
			const GitCore::FLockInfo* Lock = Snapshot->Locks.Find(Path);
			if (!Lock || (!Lock->bOurs && !bForce))
			{
				continue;
			}
			const GitCore::FOpResult Result = Session.GetRepository().Unlock(Remote, Lock->Id, !Lock->bOurs);
			if (Result.bOk)
			{
				Session.RecordLockReleased(Path);
				Released.Add(Path);
			}
			else
			{
				Failed.Add(Path);
				Details += Result.Message + TEXT("\n") + Result.Details + TEXT("\n");
			}
		}
		if (!Failed.IsEmpty())
		{
			return FGitActionResult::Failure(FText::Format(LOCTEXT("UnlockFailed", "Could not release the lock on {0}."), GitWorkspaceText::FileList(Failed)), Details);
		}
		return FGitActionResult::Success(FText::Format(LOCTEXT("Unlocked", "Released {0} {0}|plural(one=lock,other=locks)."), Released.Num()));
	});
}

void FGitWorkspaceModel::CreateBranch(const FString& Name)
{
	Run(LOCTEXT("CreatingBranch", "Creating the branch…"), ELane::Mutation, [Name](GitCore::FGitSession& Session, const GitCore::FCancelTokenPtr&)
	{
		const GitCore::FOpResult Created = Session.GetRepository().CreateBranch(Name);
		Session.RefreshStatus();
		Session.RefreshBranches();
		return Created.bOk ? FGitActionResult::Success(FText::Format(LOCTEXT("BranchCreated", "Created {0} and switched to it. Push to publish it."), FText::FromString(Name)))
			: FGitActionResult::FromOp(Created);
	});
}

void FGitWorkspaceModel::SwitchBranch(const FString& Name)
{
	const FText Doing = FText::Format(LOCTEXT("SwitchingTo", "Switching to {0}…"), FText::FromString(Name));
	// First, without changing anything: refuse when local changes are in the way, and download LFS files.
	Run(Doing, ELane::Mutation, [Name](GitCore::FGitSession& Session, const GitCore::FCancelTokenPtr& Cancel)
	{
		const GitCore::FGitRepository& Repository = Session.GetRepository();
		Session.RefreshStatus();
		const GitCore::FSessionSnapshotRef Snapshot = Session.GetSnapshot();
		if (Snapshot->Status.bOperationInProgress)
		{
			return FGitActionResult::Failure(LOCTEXT("SwitchMidOperation", "Finish the merge or rebase in progress before switching branches."));
		}
		TSet<FString> Changed;
		FString Error;
		if (!Repository.ChangedPaths(TEXT("HEAD"), Name, Changed, Error))
		{
			return FGitActionResult::Failure(FText::FromString(Error));
		}
		TArray<FString> Overlap;
		for (const GitCore::FStatusEntry& Entry : Snapshot->Status.Entries)
		{
			if (Changed.Contains(Entry.Path))
			{
				Overlap.Add(Entry.Path);
			}
		}
		if (!Overlap.IsEmpty())
		{
			return FGitActionResult::Failure(FText::Format(LOCTEXT("SwitchOverlap", "Nothing was changed: {0} also differs on {1}. Commit, stash or discard those changes first."),
				GitWorkspaceText::FileList(Overlap), FText::FromString(Name)));
		}
		const FString Remote = Repository.DefaultRemote();
		if (!Remote.IsEmpty())
		{
			const GitCore::FOpResult Downloaded = Repository.FetchLfsObjects(Remote, Name, Cancel);
			if (!Downloaded.bOk)
			{
				return FGitActionResult::FromOp(Downloaded);
			}
		}
		FGitActionResult Prepared = FGitActionResult::Success();
		Prepared.Paths = Changed.Array();
		return Prepared;
	},
	[this, Name, Doing](const FGitActionResult& Prepared)
	{
		if (!Prepared.bOk)
		{
			return;
		}
		ApplyAndReload(Doing, Prepared.Paths, [Name](GitCore::FGitSession& Session, const GitCore::FCancelTokenPtr&)
		{
			const GitCore::FOpResult Switched = Session.GetRepository().SwitchBranch(Name);
			Session.RefreshStatus();
			Session.RefreshBranches();
			return Switched.bOk ? FGitActionResult::Success(FText::Format(LOCTEXT("Switched", "Switched to {0}."), FText::FromString(Name))) : FGitActionResult::FromOp(Switched);
		});
	}, true);
}

void FGitWorkspaceModel::StashChanges(const FString& Message, bool bIncludeUntracked)
{
	const FText Doing = LOCTEXT("Stashing", "Stashing changes…");
	Run(Doing, ELane::Mutation, [bIncludeUntracked](GitCore::FGitSession& Session, const GitCore::FCancelTokenPtr&)
	{
		Session.RefreshStatus();
		const GitCore::FSessionSnapshotRef Snapshot = Session.GetSnapshot();
		if (Snapshot->Status.HasConflicts() || Snapshot->Status.bOperationInProgress)
		{
			return FGitActionResult::Failure(LOCTEXT("StashMidOperation", "Finish the merge or rebase in progress (or resolve its conflicts) before stashing."));
		}
		FGitActionResult Prepared = FGitActionResult::Success();
		for (const GitCore::FStatusEntry& Entry : Snapshot->Status.Entries)
		{
			if (!Entry.bUntracked || bIncludeUntracked)
			{
				Prepared.Paths.Add(Entry.Path);
			}
		}
		if (Prepared.Paths.IsEmpty())
		{
			return FGitActionResult::Failure(LOCTEXT("NothingToStash", "There are no changes to stash."));
		}
		return Prepared;
	},
	[this, Doing, Message, bIncludeUntracked](const FGitActionResult& Prepared)
	{
		if (!Prepared.bOk)
		{
			return;
		}
		// Stashing puts the files back to their committed versions, so loaded packages reload.
		ApplyAndReload(Doing, Prepared.Paths, [Message, bIncludeUntracked](GitCore::FGitSession& Session, const GitCore::FCancelTokenPtr&)
		{
			const GitCore::FOpResult Stashed = Session.GetRepository().StashPush({}, Message, bIncludeUntracked);
			Session.RefreshStatus();
			Session.RefreshStashes();
			return Stashed.bOk ? FGitActionResult::Success(LOCTEXT("Stashed", "Your changes were stashed.")) : FGitActionResult::FromOp(Stashed);
		});
	}, true);
}

void FGitWorkspaceModel::ApplyStash(const FString& Selector, bool bDrop)
{
	const FText Doing = bDrop ? LOCTEXT("Popping", "Restoring the stash…") : LOCTEXT("Applying", "Applying the stash…");
	Run(Doing, ELane::Mutation, [Selector](GitCore::FGitSession& Session, const GitCore::FCancelTokenPtr&)
	{
		Session.RefreshStatus();
		TArray<FString> Paths;
		FString Error;
		if (!Session.GetRepository().ReadStashPaths(Selector, Paths, Error))
		{
			return FGitActionResult::Failure(FText::FromString(Error));
		}
		const GitCore::FSessionSnapshotRef Snapshot = Session.GetSnapshot();
		TArray<FString> Overlap;
		for (const FString& Path : Paths)
		{
			if (Snapshot->Status.Find(Path))
			{
				Overlap.Add(Path);
			}
		}
		if (!Overlap.IsEmpty())
		{
			return FGitActionResult::Failure(FText::Format(LOCTEXT("StashOverlap", "Nothing was changed: you have local changes to {0}, which the stash also changes. Commit, stash or discard them first."),
				GitWorkspaceText::FileList(Overlap)));
		}
		FGitActionResult Prepared = FGitActionResult::Success();
		Prepared.Paths = MoveTemp(Paths);
		return Prepared;
	},
	[this, Doing, Selector, bDrop](const FGitActionResult& Prepared)
	{
		if (!Prepared.bOk)
		{
			return;
		}
		ApplyAndReload(Doing, Prepared.Paths, [Selector, bDrop](GitCore::FGitSession& Session, const GitCore::FCancelTokenPtr&)
		{
			const GitCore::FOpResult Applied = Session.GetRepository().StashApply(Selector, true, bDrop);
			Session.RefreshStatus();
			Session.RefreshStashes();
			if (!Applied.bOk)
			{
				return FGitActionResult::FromOp(Applied);
			}
			return FGitActionResult::Success(bDrop ? LOCTEXT("Popped", "Restored the stash and removed it.") : LOCTEXT("Applied", "Applied the stash. It is kept until you delete it."));
		});
	}, true);
}

void FGitWorkspaceModel::DropStash(const FString& Selector)
{
	Run(LOCTEXT("DroppingStash", "Deleting the stash…"), ELane::Mutation, [Selector](GitCore::FGitSession& Session, const GitCore::FCancelTokenPtr&)
	{
		const GitCore::FOpResult Dropped = Session.GetRepository().StashDrop(Selector);
		Session.RefreshStashes();
		return Dropped.bOk ? FGitActionResult::Success(LOCTEXT("Dropped", "Deleted the stash.")) : FGitActionResult::FromOp(Dropped);
	});
}

void FGitWorkspaceModel::ApplyAndReload(const FText& InActivity, const TArray<FString>& Paths, FWork Op)
{
	const GitCore::FGitSessionPtr Session = GetSession();
	if (!Session.IsValid())
	{
		return;
	}
	Activity = InActivity;
	ActivitySession = Session;
	Changed.Broadcast();
	const TSharedRef<FWork> SharedOp = MakeShared<FWork>(MoveTemp(Op));
	RunAtSafePoint([Weak = AsWeak(), Session = Session.ToSharedRef(), InActivity, Paths, SharedOp]()
	{
		const TSharedPtr<FGitWorkspaceModel> Self = Weak.Pin();
		if (!Self.IsValid())
		{
			return;
		}
		TArray<FString> Packages;
		for (const FString& Path : Paths)
		{
			if (IsPackagePath(Path))
			{
				Packages.Add(Session->GetRepository().ToAbsolute(Path));
			}
		}
		FGitActionResult Result = FGitActionResult::Failure(LOCTEXT("ReloadRefused", "Nothing was changed: the editor could not unload the affected assets. See the Output Log."));
		auto Apply = [&Result, &Session, &InActivity, &SharedOp](const TArray<FString>&)
		{
			Result = RunAndWait(Session, InActivity, [&SharedOp](GitCore::FGitSession& InSession) { return (*SharedOp)(InSession, nullptr); });
			return Result.bOk;
		};
		if (Self->BoundProvider)
		{
			Apply(Packages);
		}
		else
		{
			USourceControlHelpers::ApplyOperationAndReloadPackages(Packages, Apply, true, true);
		}
		// New and changed packages show up in the Content Browser without waiting for the file watcher.
		TArray<FString> Existing = Packages.FilterByPredicate([](const FString& File) { return IFileManager::Get().FileExists(*File); });
		if (!Existing.IsEmpty())
		{
			FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get().ScanModifiedAssetFiles(Existing);
		}
		Session->RequestLockRefresh(false);
		Self->Finish(Result, false);
	});
}

void FGitWorkspaceModel::LoadHistory(int32 Limit, TFunction<void(TArray<GitCore::FHistoryCommit>&&, bool, const FString&)> OnLoaded)
{
	const GitCore::FGitSessionPtr Session = GetSession();
	if (!Session.IsValid())
	{
		OnLoaded({}, false, FString());
		return;
	}
	Session->EnqueueRead([Session, Limit, OnLoaded]()
	{
		TArray<GitCore::FHistoryCommit> Commits;
		bool bHasMore = false;
		FString Error;
		Session->GetRepository().ReadHistory(Limit, Commits, bHasMore, Error);
		AsyncTask(ENamedThreads::GameThread, [OnLoaded, Commits = MoveTemp(Commits), bHasMore, Error]() mutable
		{
			OnLoaded(MoveTemp(Commits), bHasMore, Error);
		});
	});
}

void FGitWorkspaceModel::LoadCommit(const FString& Oid, TFunction<void(GitCore::FCommitDetails&&, const FString&)> OnLoaded)
{
	const GitCore::FGitSessionPtr Session = GetSession();
	if (!Session.IsValid())
	{
		OnLoaded({}, FString());
		return;
	}
	Session->EnqueueRead([Session, Oid, OnLoaded]()
	{
		GitCore::FCommitDetails Details;
		FString Error;
		Session->GetRepository().ReadCommit(Oid, Details, Error);
		AsyncTask(ENamedThreads::GameThread, [OnLoaded, Details = MoveTemp(Details), Error]() mutable
		{
			OnLoaded(MoveTemp(Details), Error);
		});
	});
}

void FGitWorkspaceModel::LoadStashPaths(const FString& Selector, TFunction<void(TArray<FString>&&, const FString&)> OnLoaded)
{
	const GitCore::FGitSessionPtr Session = GetSession();
	if (!Session.IsValid())
	{
		OnLoaded({}, FString());
		return;
	}
	Session->EnqueueRead([Session, Selector, OnLoaded]()
	{
		TArray<FString> Paths;
		FString Error;
		Session->GetRepository().ReadStashPaths(Selector, Paths, Error);
		AsyncTask(ENamedThreads::GameThread, [OnLoaded, Paths = MoveTemp(Paths), Error]() mutable
		{
			OnLoaded(MoveTemp(Paths), Error);
		});
	});
}

FString FGitWorkspaceModel::ToAbsolute(const FString& RelativePath) const
{
	const GitCore::FGitSessionPtr Session = GetSession();
	return Session.IsValid() ? Session->GetRepository().ToAbsolute(RelativePath) : FString();
}

void FGitWorkspaceModel::OpenFile(const FString& RelativePath)
{
	const FString File = ToAbsolute(RelativePath);
	if (File.IsEmpty() || !IFileManager::Get().FileExists(*File))
	{
		return;
	}
	if (!IsPackagePath(RelativePath))
	{
		FPlatformProcess::LaunchFileInDefaultExternalApplication(*File);
		return;
	}
	// External actors and objects only open as part of their level.
	TArray<FAssetData> Assets;
	if (IsExternalPackage(RelativePath) || !USourceControlHelpers::GetAssetData(File, Assets) || Assets.IsEmpty() || !GEditor)
	{
		return;
	}
	if (UObject* Asset = Assets[0].GetAsset())
	{
		GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()->OpenEditorForAsset(Asset);
	}
}

void FGitWorkspaceModel::BrowseTo(const TArray<FString>& RelativePaths)
{
	TArray<FAssetData> Assets;
	for (const FString& Path : RelativePaths)
	{
		TArray<FAssetData> Found;
		if (IsPackagePath(Path) && !IsExternalPackage(Path) && USourceControlHelpers::GetAssetData(ToAbsolute(Path), Found))
		{
			Assets.Append(Found.FilterByPredicate([](const FAssetData& Asset) { return !Asset.IsRedirector(); }));
		}
	}
	if (!Assets.IsEmpty())
	{
		FModuleManager::LoadModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser")).Get().SyncBrowserToAssets(Assets);
	}
	else if (RelativePaths.Num() == 1)
	{
		FPlatformProcess::ExploreFolder(*ToAbsolute(RelativePaths[0]));
	}
}

void FGitWorkspaceModel::ShowDiff(const FString& RelativePath, bool bStaged)
{
	const GitCore::FGitSessionPtr Session = GetSession();
	if (!Session.IsValid())
	{
		return;
	}
	if (IsPackagePath(RelativePath))
	{
		const GitCore::FStatusEntry* Entry = GetSnapshot()->Status.Find(RelativePath);
		if (!Entry || Entry->IsAdded() || Entry->IsDeleted() || IsExternalPackage(RelativePath))
		{
			Present(FGitActionResult::Failure(LOCTEXT("NoAssetDiff", "Only edits to existing assets can be compared. Open new assets instead.")));
			return;
		}
		const FString File = ToAbsolute(RelativePath);
		TArray<FAssetData> Assets;
		if (USourceControlHelpers::GetAssetData(File, Assets) && !Assets.IsEmpty())
		{
			if (UObject* Asset = Assets[0].GetAsset())
			{
				FAssetToolsModule::GetModule().Get().DiffAgainstDepot(Asset, Assets[0].PackageName.ToString(), Assets[0].AssetName.ToString());
			}
		}
		return;
	}
	const FText Title = FText::Format(LOCTEXT("DiffTitle", "{0} ({1})"), FText::FromString(FPaths::GetCleanFilename(RelativePath)),
		bStaged ? LOCTEXT("StagedSide", "staged") : LOCTEXT("WorkingSide", "not staged"));
	Session->EnqueueRead([Weak = AsWeak(), Session, RelativePath, bStaged, Title]()
	{
		FString Text;
		const GitCore::FOpResult Result = Session->GetRepository().Diff(RelativePath, bStaged, Text);
		AsyncTask(ENamedThreads::GameThread, [Weak, Title, Text = Result.bOk ? MoveTemp(Text) : Result.Message]()
		{
			if (const TSharedPtr<FGitWorkspaceModel> Self = Weak.Pin())
			{
				Self->ShowTextDiff(Title, Text);
			}
		});
	});
}

void FGitWorkspaceModel::ShowHistory(const FString& RelativePath)
{
	FString PackageName;
	if (IsPackagePath(RelativePath) && FPackageName::TryConvertFilenameToLongPackageName(ToAbsolute(RelativePath), PackageName))
	{
		FSourceControlWindows::DisplayRevisionHistory({PackageName});
	}
}

void FGitWorkspaceModel::ShowCommitDiff(const GitCore::FHistoryCommit& Commit, const GitCore::FHistoryChange& Change)
{
	const GitCore::FGitSessionPtr Session = GetSession();
	if (!Session.IsValid())
	{
		return;
	}
	if (!IsPackagePath(Change.Path))
	{
		const FText Title = FText::Format(LOCTEXT("CommitDiffTitle", "{0} in {1}"), FText::FromString(FPaths::GetCleanFilename(Change.Path)),
			FText::FromString(GitWorkspaceText::ShortId(Commit.Oid)));
		Session->EnqueueRead([Weak = AsWeak(), Session, Oid = Commit.Oid, Path = Change.Path, Title]()
		{
			FString Text;
			const GitCore::FOpResult Result = Session->GetRepository().CommitDiff(Oid, Path, Text);
			AsyncTask(ENamedThreads::GameThread, [Weak, Title, Text = Result.bOk ? MoveTemp(Text) : Result.Message]()
			{
				if (const TSharedPtr<FGitWorkspaceModel> Self = Weak.Pin())
				{
					Self->ShowTextDiff(Title, Text);
				}
			});
		});
		return;
	}
	auto LoadRevision = [this, &Session, &Change](const FString& Oid, const FString& Path) -> UObject*
	{
		const TSharedRef<FGitSourceControlRevision, ESPMode::ThreadSafe> Revision = MakeShared<FGitSourceControlRevision, ESPMode::ThreadSafe>();
		Revision->LocalFilename = ToAbsolute(Change.Path);
		Revision->PathInCommit = Path;
		Revision->CommitId = Oid;
		Revision->ShortCommitId = GitWorkspaceText::ShortId(Oid);
		Revision->Repository = Session->GetRepositoryRef();
		UPackage* Package = DiffUtils::LoadPackageForDiff(Revision);
		return Package ? Package->FindAssetInPackage() : nullptr;
	};
	const bool bHasOld = Change.Status != TEXT('A') && !Commit.Parents.IsEmpty();
	UObject* NewAsset = Change.Status != TEXT('D') ? LoadRevision(Commit.Oid, Change.Path) : nullptr;
	UObject* OldAsset = bHasOld ? LoadRevision(Commit.Parents[0], Change.OldPath.IsEmpty() ? Change.Path : Change.OldPath) : nullptr;
	if (!NewAsset || !OldAsset)
	{
		Present(FGitActionResult::Failure(Change.Status == TEXT('A') ? LOCTEXT("AddedInCommit", "This asset was added in this commit; there is no earlier version to compare.")
			: Change.Status == TEXT('D') ? LOCTEXT("DeletedInCommit", "This asset was deleted in this commit.")
			: LOCTEXT("DiffLoadFailed", "This asset version could not be loaded for comparison.")));
		return;
	}
	FRevisionInfo OldRevision;
	OldRevision.Revision = GitWorkspaceText::ShortId(Commit.Parents[0]);
	FRevisionInfo NewRevision;
	NewRevision.Revision = GitWorkspaceText::ShortId(Commit.Oid);
	FAssetToolsModule::GetModule().Get().DiffAssets(OldAsset, NewAsset, OldRevision, NewRevision);
}

void FGitWorkspaceModel::ShowTextDiff(const FText& Title, const FString& Text) const
{
	constexpr int32 MaxCharacters = 2 * 1024 * 1024;
	FString Shown = Text.IsEmpty() ? LOCTEXT("NoTextDifferences", "No differences.").ToString() : Text.Left(MaxCharacters);
	if (Text.Len() > MaxCharacters)
	{
		Shown += TEXT("\n…");
	}
	const TSharedRef<SWindow> Window = SNew(SWindow)
		.Title(Title)
		.ClientSize(FVector2D(900.0f, 640.0f))
		.SupportsMinimize(false);
	Window->SetContent(
		SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
		.Padding(4.0f)
		[
			SNew(SMultiLineEditableTextBox)
			.Text(FText::FromString(Shown))
			.IsReadOnly(true)
			.Font(FCoreStyle::GetDefaultFontStyle("Mono", 9))
		]);
	FSlateApplication::Get().AddWindow(Window);
}

#undef LOCTEXT_NAMESPACE
