// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#include "GitSession.h"
#include "GitWorkQueue.h"

#include "Async/Async.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "Misc/ScopeRWLock.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace GitCore
{
	namespace
	{
		constexpr int32 MaxScopedPaths = 100;

		FCriticalSection RegistryLock;
		TMap<FString, FGitSessionRef>& Registry()
		{
			static TMap<FString, FGitSessionRef> Sessions;
			return Sessions;
		}

		/** Files whose contents decide attributes: tracked .gitattributes plus the repository's info/attributes. */
		TArray<FString> AttributeFiles(const FGitRepository& Repository, const TSet<FString>* Tracked)
		{
			TArray<FString> Files = {Repository.GetCommonDir() / TEXT("info/attributes"), Repository.GetRoot() / TEXT(".gitattributes")};
			if (Tracked)
			{
				for (const FString& Path : *Tracked)
				{
					if (Path.EndsWith(TEXT("/.gitattributes")))
					{
						Files.Add(Repository.ToAbsolute(Path));
					}
				}
			}
			return Files;
		}
	}

	FGitSessionPtr FGitSession::Open(const FString& GitBinary, const FString& Path, FString& OutError)
	{
		FGitRepositoryPtr Repository = FGitRepository::Open(GitBinary, Path, OutError);
		if (!Repository.IsValid())
		{
			return nullptr;
		}
		FScopeLock Lock(&RegistryLock);
		if (FGitSessionRef* Existing = Registry().Find(Repository->GetRoot()))
		{
			if ((*Existing)->GetRepository().GetGitBinary() == GitBinary)
			{
				return *Existing;
			}
			(*Existing)->Shutdown();
			Registry().Remove(Repository->GetRoot());
		}
		FGitSessionRef Session = MakeShareable(new FGitSession(Repository.ToSharedRef()));
		Registry().Add(Repository->GetRoot(), Session);
		return Session;
	}

	void FGitSession::ShutdownAll()
	{
		TArray<FGitSessionRef> Sessions;
		{
			FScopeLock Lock(&RegistryLock);
			Registry().GenerateValueArray(Sessions);
			Registry().Empty();
		}
		for (const FGitSessionRef& Session : Sessions)
		{
			Session->Shutdown();
		}
	}

	void FGitSession::Close()
	{
		{
			FScopeLock Lock(&RegistryLock);
			const FGitSessionRef* Registered = Registry().Find(Repository->GetRoot());
			if (Registered && &Registered->Get() == this)
			{
				Registry().Remove(Repository->GetRoot());
			}
		}
		Shutdown();
	}

	FGitSession::FGitSession(FGitRepositoryRef InRepository)
		: Repository(MoveTemp(InRepository))
		, Snapshot(MakeShared<FSessionSnapshot, ESPMode::ThreadSafe>())
	{
		Journal = MakeUnique<FGitLockJournal>(Repository->GetCommonDir(), Repository->GetRoot());
		MutationQueue = MakeUnique<FSerialWorkQueue>(TEXT("GitMutations"));
		BackgroundQueue = MakeUnique<FSerialWorkQueue>(TEXT("GitBackground"));
		ReadQueue = MakeUnique<FSerialWorkQueue>(TEXT("GitReads"));
		BackgroundCancel = MakeShared<FCancelToken, ESPMode::ThreadSafe>();
		Publish(EChange::None, [this](FSessionSnapshot& State) { State.Root = Repository->GetRoot(); });
		LoadMarkedForAdd();
	}

	FGitSession::~FGitSession()
	{
		Shutdown();
	}

	void FGitSession::Shutdown()
	{
		{
			FScopeLock Lock(&StateLock);
			BackgroundCancel->Cancel();
		}
		if (MutationQueue)
		{
			MutationQueue->Shutdown();
		}
		if (BackgroundQueue)
		{
			BackgroundQueue->Shutdown();
		}
		if (ReadQueue)
		{
			ReadQueue->Shutdown();
		}
	}

	FSessionSnapshotRef FGitSession::GetSnapshot() const
	{
		FReadScopeLock Lock(SnapshotLock);
		return Snapshot;
	}

	void FGitSession::Publish(EChange Changes, TFunctionRef<void(FSessionSnapshot&)> Edit)
	{
		{
			FWriteScopeLock Lock(SnapshotLock);
			TSharedRef<FSessionSnapshot, ESPMode::ThreadSafe> Next = MakeShared<FSessionSnapshot, ESPMode::ThreadSafe>(*Snapshot);
			Edit(*Next);
			Next->Generation = Snapshot->Generation + 1;
			Snapshot = Next;
		}
		if (Changes != EChange::None)
		{
			NotifyChanged(Changes);
		}
	}

	void FGitSession::NotifyChanged(EChange Changes)
	{
		// Coalesce: one game-thread broadcast carries every change published before it runs.
		const uint8 Previous = PendingChanges.fetch_or(static_cast<uint8>(Changes));
		if (Previous != 0)
		{
			return;
		}
		TWeakPtr<FGitSession, ESPMode::ThreadSafe> Weak = AsShared();
		AsyncTask(ENamedThreads::GameThread, [Weak]()
		{
			if (FGitSessionPtr Self = Weak.Pin())
			{
				const EChange Delivered = static_cast<EChange>(Self->PendingChanges.exchange(0));
				Self->ChangedDelegate.Broadcast(Delivered);
			}
		});
	}

	void FGitSession::EnqueueMutation(TUniqueFunction<void()> Task)
	{
		MutationQueue->Enqueue(MoveTemp(Task));
	}

	void FGitSession::EnqueueBackground(TUniqueFunction<void()> Task)
	{
		BackgroundQueue->Enqueue(MoveTemp(Task));
	}

	void FGitSession::EnqueueRead(TUniqueFunction<void()> Task)
	{
		ReadQueue->Enqueue(MoveTemp(Task));
	}

	void FGitSession::FlushMutations()
	{
		MutationQueue->Flush();
	}

	bool FGitSession::IsMutationLane() const
	{
		return MutationQueue->IsWorkerThread();
	}

	bool FGitSession::RefreshStatus(const TArray<FString>* OnlyPaths, FString* OutError)
	{
		const FSessionSnapshotRef Current = GetSnapshot();
		const bool bScoped = OnlyPaths && OnlyPaths->Num() > 0 && OnlyPaths->Num() <= MaxScopedPaths && Current->bStatusValid;
		FStatusSnapshot Status;
		FString Error;
		if (!Repository->ReadStatus(Status, Error, true, bScoped ? OnlyPaths : nullptr))
		{
			if (!bScoped)
			{
				Publish(EChange::Status, [&Error](FSessionSnapshot& State)
				{
					State.bStatusValid = false;
					State.StatusError = Error;
				});
			}
			if (OutError)
			{
				*OutError = Error;
			}
			return false;
		}

		TSet<FString> Requested;
		if (bScoped)
		{
			// Merge the scoped answer into the last full status; branch information is repository-wide.
			Requested.Append(*OnlyPaths);
			FStatusSnapshot Merged = Current->Status;
			auto IsRequested = [&Requested](const FStatusEntry& Entry)
			{
				return Requested.Contains(Entry.Path) || (!Entry.OriginalPath.IsEmpty() && Requested.Contains(Entry.OriginalPath));
			};
			Merged.Entries.RemoveAll(IsRequested);
			for (FStatusEntry& Entry : Status.Entries)
			{
				Merged.Entries.RemoveAll([&Entry](const FStatusEntry& Existing) { return Existing.Path == Entry.Path; });
				Merged.Entries.Add(MoveTemp(Entry));
			}
			Merged.Branch = Status.Branch;
			Merged.Head = Status.Head;
			Merged.Upstream = Status.Upstream;
			Merged.Ahead = Status.Ahead;
			Merged.Behind = Status.Behind;
			Merged.bUnborn = Status.bUnborn;
			Merged.bOperationInProgress = Status.bOperationInProgress;
			Merged.IndexToken = Status.IndexToken;
			Merged.BuildLookup();
			Merged.StagedToken = Merged.ComputeStagedToken();
			Status = MoveTemp(Merged);
		}

		// Tracked files change only when the index does.
		TSharedPtr<const TSet<FString>, ESPMode::ThreadSafe> Tracked = Current->Tracked;
		bool bReadTracked = false;
		{
			FScopeLock Lock(&StateLock);
			bReadTracked = !Tracked.IsValid() || TrackedToken != Status.IndexToken;
		}
		if (bReadTracked)
		{
			TSet<FString> Paths;
			if (Repository->ReadTrackedPaths(Paths, Error))
			{
				Tracked = MakeShared<const TSet<FString>, ESPMode::ThreadSafe>(MoveTemp(Paths));
				FScopeLock Lock(&StateLock);
				TrackedToken = Status.IndexToken;
			}
		}

		// Attributes depend only on the attribute files; drop the cache when one of them changes.
		TSharedPtr<const TMap<FString, FFileAttributes>, ESPMode::ThreadSafe> Attributes = Current->Attributes;
		bool bLockableRules = Current->bLockableRules;
		bool bAttributesReset = false;
		{
			// Checkouts rewrite attribute files, so their timestamps capture every relevant change.
			FString Token;
			const TArray<FString> Files = AttributeFiles(*Repository, Tracked.Get());
			for (const FString& File : Files)
			{
				Token += FString::Printf(TEXT("|%lld"), IFileManager::Get().GetTimeStamp(*File).GetTicks());
			}
			FScopeLock Lock(&StateLock);
			if (!Attributes.IsValid() || Token != AttributesToken)
			{
				Attributes = MakeShared<const TMap<FString, FFileAttributes>, ESPMode::ThreadSafe>();
				AttributesToken = Token;
				bAttributesReset = true;
				bLockableRules = false;
				for (const FString& File : Files)
				{
					FString Text;
					if (FFileHelper::LoadFileToString(Text, *File) && Text.Contains(TEXT("lockable")))
					{
						bLockableRules = true;
						break;
					}
				}
			}
		}
		TArray<FString> MissingAttributes;
		for (const FStatusEntry& Entry : Status.Entries)
		{
			if (!Attributes->Contains(Entry.Path))
			{
				MissingAttributes.Add(Entry.Path);
			}
		}
		for (const FString& Path : Requested)
		{
			if (!Attributes->Contains(Path))
			{
				MissingAttributes.AddUnique(Path);
			}
		}
		if ((bReadTracked || bAttributesReset) && Tracked.IsValid())
		{
			// Unreal asks about packages constantly; know their lock rules up front.
			for (const FString& Path : *Tracked)
			{
				if ((Path.EndsWith(TEXT(".uasset")) || Path.EndsWith(TEXT(".umap"))) && !Attributes->Contains(Path))
				{
					MissingAttributes.Add(Path);
				}
			}
		}
		if (!MissingAttributes.IsEmpty())
		{
			TMap<FString, FFileAttributes> Updated = *Attributes;
			if (Repository->ReadAttributes(MissingAttributes, Updated, Error))
			{
				Attributes = MakeShared<const TMap<FString, FFileAttributes>, ESPMode::ThreadSafe>(MoveTemp(Updated));
			}
		}

		// New files stop being "marked for add" once git tracks them or they disappear.
		TSet<FString> Marked = Current->MarkedForAdd;
		bool bMarkedChanged = false;
		for (auto It = Marked.CreateIterator(); It; ++It)
		{
			if (bScoped && !Requested.Contains(*It))
			{
				continue;
			}
			const FStatusEntry* Entry = Status.Find(*It);
			if (!Entry || !Entry->bUntracked)
			{
				It.RemoveCurrent();
				bMarkedChanged = true;
			}
		}

		// Ahead/behind and incoming paths only change with HEAD or the upstream.
		FRemoteState Remote = Current->Remote;
		bool bRemoteChanged = false;
		const FString NewRemoteToken = FString::Printf(TEXT("%s|%s|%d|%d"), *Status.Head, *Status.Upstream, Status.Ahead, Status.Behind);
		bool bRefreshRemote = false;
		{
			FScopeLock Lock(&StateLock);
			bRefreshRemote = NewRemoteToken != RemoteToken;
			RemoteToken = NewRemoteToken;
		}
		if (bRefreshRemote)
		{
			FString RemoteError;
			Repository->ReadRemoteState(Status, Remote, RemoteError);
			bRemoteChanged = true;
			const FString DefaultRemote = Repository->DefaultRemote();
			FScopeLock Lock(&StateLock);
			CachedDefaultRemote = DefaultRemote;
		}

		Publish(bRemoteChanged ? (EChange::Status | EChange::Remote) : EChange::Status, [&](FSessionSnapshot& State)
		{
			State.bStatusValid = true;
			State.StatusError.Reset();
			State.StatusAtUtc = FDateTime::UtcNow();
			State.Status = MoveTemp(Status);
			State.Tracked = Tracked;
			State.Attributes = Attributes;
			State.bLockableRules = bLockableRules;
			State.MarkedForAdd = Marked;
			if (bRemoteChanged)
			{
				State.Remote = MoveTemp(Remote);
			}
		});
		if (bMarkedChanged)
		{
			SaveMarkedForAdd(Marked);
		}
		return true;
	}

	bool FGitSession::EnsureAttributes(const TArray<FString>& Paths)
	{
		const FSessionSnapshotRef Current = GetSnapshot();
		if (!Current->Attributes.IsValid())
		{
			return false;
		}
		TArray<FString> Missing;
		for (const FString& Path : Paths)
		{
			if (!Current->Attributes->Contains(Path))
			{
				Missing.Add(Path);
			}
		}
		if (Missing.IsEmpty())
		{
			return true;
		}
		TMap<FString, FFileAttributes> Updated = *Current->Attributes;
		FString Error;
		if (!Repository->ReadAttributes(Missing, Updated, Error))
		{
			return false;
		}
		TSharedPtr<const TMap<FString, FFileAttributes>, ESPMode::ThreadSafe> Shared = MakeShared<const TMap<FString, FFileAttributes>, ESPMode::ThreadSafe>(MoveTemp(Updated));
		Publish(EChange::Status, [&Shared](FSessionSnapshot& State) { State.Attributes = Shared; });
		return true;
	}

	bool FGitSession::RefreshLocks(ECallKind Kind, FString* OutError)
	{
		const FString Remote = ResolveLockRemote();
		const FSessionSnapshotRef Current = GetSnapshot();
		FLockSnapshot Locks = Current->Locks;
		if (Locks.Remote != Remote)
		{
			Locks = FLockSnapshot();
		}
		bool bVerified = false;
		if (Remote.IsEmpty())
		{
			Locks.Error = TEXT("This repository has no remote, so locks cannot be checked.");
		}
		else
		{
			bVerified = Repository->VerifyLocks(Remote, Kind, Locks);
		}
		TSet<FString> TakenHere = Current->LocksTakenHere;
		if (bVerified)
		{
			TakenHere.Reset();
			TSet<FString> Held;
			for (const TPair<FString, FLockInfo>& Pair : Locks.ByPath)
			{
				if (Pair.Value.bOurs)
				{
					Held.Add(Pair.Value.Id);
					if (Journal->IsOurs(Locks.Endpoint, Pair.Value.Id))
					{
						TakenHere.Add(Pair.Value.Id);
					}
				}
			}
			Journal->Prune(Locks.Endpoint, Held);
		}
		Publish(EChange::Locks, [&Locks, &TakenHere](FSessionSnapshot& State)
		{
			State.Locks = Locks;
			State.LocksTakenHere = TakenHere;
		});
		if (!bVerified && OutError)
		{
			*OutError = Locks.Error;
		}
		return bVerified;
	}

	bool FGitSession::Fetch(ECallKind Kind, const FCancelTokenPtr& Cancel, FString* OutError)
	{
		FString Remote;
		{
			FScopeLock Lock(&StateLock);
			Remote = CachedDefaultRemote;
		}
		if (Remote.IsEmpty())
		{
			Remote = Repository->DefaultRemote();
		}
		const FOpResult Result = Repository->Fetch(Remote, Kind, Cancel);
		if (!Result.bOk && Cancel.IsValid() && Cancel->IsCancelled())
		{
			// Sync, Submit or shutdown took over; they refresh the remote state themselves.
			if (OutError)
			{
				*OutError = Result.Message;
			}
			return false;
		}
		const FSessionSnapshotRef Current = GetSnapshot();
		FRemoteState State = Current->Remote;
		if (Result.bOk)
		{
			State.bFetched = true;
			State.FetchedAtUtc = FDateTime::UtcNow();
			State.Error.Reset();
		}
		else
		{
			State.Error = Result.Message;
		}
		if (Current->bStatusValid)
		{
			FString RemoteError;
			if (!Repository->ReadRemoteState(Current->Status, State, RemoteError))
			{
				State.Error = RemoteError;
			}
		}
		{
			FScopeLock Lock(&StateLock);
			RemoteToken.Reset();
		}
		Publish(EChange::Remote, [&State](FSessionSnapshot& Snapshot) { Snapshot.Remote = MoveTemp(State); });
		if (!Result.bOk && OutError)
		{
			*OutError = Result.Message;
		}
		return Result.bOk;
	}

	void FGitSession::RefreshRemote()
	{
		const FSessionSnapshotRef Current = GetSnapshot();
		if (!Current->bStatusValid)
		{
			return;
		}
		FRemoteState State = Current->Remote;
		FString Error;
		if (!Repository->ReadRemoteState(Current->Status, State, Error))
		{
			State.Error = Error;
		}
		Publish(EChange::Remote, [&State](FSessionSnapshot& Snapshot) { Snapshot.Remote = MoveTemp(State); });
	}

	void FGitSession::RefreshStashes()
	{
		TArray<FStashEntry> Stashes;
		FString Error;
		if (Repository->ReadStashes(Stashes, Error))
		{
			Publish(EChange::Stashes, [&Stashes](FSessionSnapshot& State) { State.Stashes = MoveTemp(Stashes); });
		}
	}

	void FGitSession::RefreshBranches()
	{
		TArray<FBranchInfo> Branches;
		FString Error;
		if (Repository->ReadBranches(Branches, Error))
		{
			Publish(EChange::Branches, [&Branches](FSessionSnapshot& State) { State.Branches = MoveTemp(Branches); });
		}
	}

	void FGitSession::RecordLockTaken(const FLockInfo& Lock)
	{
		FString Endpoint = GetSnapshot()->Locks.Endpoint;
		const FString Remote = ResolveLockRemote();
		FString Error;
		if (Endpoint.IsEmpty())
		{
			Repository->ResolveLockEndpoint(Remote, Endpoint, Error);
		}
		if (!Endpoint.IsEmpty())
		{
			Journal->Record(Endpoint, Lock, Error);
		}
		Publish(EChange::Locks, [&Lock, &Endpoint, &Remote](FSessionSnapshot& State)
		{
			FLockInfo Ours = Lock;
			Ours.bOurs = true;
			State.Locks.ByPath.Add(Ours.Path, Ours);
			State.LocksTakenHere.Add(Ours.Id);
			if (State.Locks.Endpoint.IsEmpty())
			{
				State.Locks.Endpoint = Endpoint;
				State.Locks.Remote = Remote;
			}
		});
	}

	void FGitSession::RecordLockReleased(const FString& Path)
	{
		const FSessionSnapshotRef Current = GetSnapshot();
		const FLockInfo* Lock = Current->Locks.Find(Path);
		if (!Lock)
		{
			return;
		}
		Journal->Remove(Current->Locks.Endpoint, Lock->Id);
		const FString Id = Lock->Id;
		Publish(EChange::Locks, [&Path, &Id](FSessionSnapshot& State)
		{
			State.Locks.ByPath.Remove(Path);
			State.LocksTakenHere.Remove(Id);
		});
	}

	void FGitSession::SetMarkedForAdd(const TArray<FString>& Paths, bool bMarked)
	{
		TSet<FString> Marked = GetSnapshot()->MarkedForAdd;
		for (const FString& Path : Paths)
		{
			if (bMarked)
			{
				Marked.Add(Path);
			}
			else
			{
				Marked.Remove(Path);
			}
		}
		Publish(EChange::Status, [&Marked](FSessionSnapshot& State) { State.MarkedForAdd = Marked; });
		SaveMarkedForAdd(Marked);
	}

	void FGitSession::RequestStatusRefresh()
	{
		if (bStatusRequested.exchange(true))
		{
			return;
		}
		TWeakPtr<FGitSession, ESPMode::ThreadSafe> Weak = AsShared();
		EnqueueMutation([Weak]()
		{
			if (FGitSessionPtr Self = Weak.Pin())
			{
				Self->bStatusRequested = false;
				Self->RefreshStatus();
			}
		});
	}

	void FGitSession::RequestLockRefresh(bool bUserInitiated)
	{
		if (bLocksRequested.exchange(true))
		{
			return;
		}
		TWeakPtr<FGitSession, ESPMode::ThreadSafe> Weak = AsShared();
		EnqueueBackground([Weak, bUserInitiated]()
		{
			if (FGitSessionPtr Self = Weak.Pin())
			{
				Self->bLocksRequested = false;
				Self->RefreshLocks(bUserInitiated ? ECallKind::UserNetwork : ECallKind::BackgroundNetwork);
			}
		});
	}

	void FGitSession::RequestFetch(bool bUserInitiated)
	{
		if (bFetchRequested.exchange(true))
		{
			return;
		}
		TWeakPtr<FGitSession, ESPMode::ThreadSafe> Weak = AsShared();
		EnqueueBackground([Weak, bUserInitiated]()
		{
			if (FGitSessionPtr Self = Weak.Pin())
			{
				Self->bFetchRequested = false;
				Self->Fetch(bUserInitiated ? ECallKind::UserNetwork : ECallKind::BackgroundNetwork, Self->GetBackgroundCancel());
			}
		});
	}

	void FGitSession::RequestStashesAndBranches()
	{
		if (bListsRequested.exchange(true))
		{
			return;
		}
		TWeakPtr<FGitSession, ESPMode::ThreadSafe> Weak = AsShared();
		EnqueueRead([Weak]()
		{
			if (FGitSessionPtr Self = Weak.Pin())
			{
				Self->bListsRequested = false;
				Self->RefreshStashes();
				Self->RefreshBranches();
			}
		});
	}

	FString FGitSession::GetLockRemote() const
	{
		FScopeLock Lock(&StateLock);
		return LockRemote.IsEmpty() ? CachedDefaultRemote : LockRemote;
	}

	FString FGitSession::ResolveLockRemote()
	{
		const FString Known = GetLockRemote();
		if (!Known.IsEmpty())
		{
			return Known;
		}
		const FString Default = Repository->DefaultRemote();
		FScopeLock Lock(&StateLock);
		CachedDefaultRemote = Default;
		return LockRemote.IsEmpty() ? CachedDefaultRemote : LockRemote;
	}

	void FGitSession::SetLockRemote(const FString& Remote)
	{
		FScopeLock Lock(&StateLock);
		LockRemote = Remote;
	}

	FCancelTokenPtr FGitSession::GetBackgroundCancel() const
	{
		FScopeLock Lock(&StateLock);
		return BackgroundCancel;
	}

	void FGitSession::CancelBackgroundWork()
	{
		FScopeLock Lock(&StateLock);
		BackgroundCancel->Cancel();
		BackgroundCancel = MakeShared<FCancelToken, ESPMode::ThreadSafe>();
	}

	void FGitSession::LoadMarkedForAdd()
	{
		FString Text;
		TArray<TSharedPtr<FJsonValue>> Values;
		if (!FFileHelper::LoadFileToString(Text, *(Repository->GetGitDir() / TEXT("uegit/marked-for-add.json"))) ||
			!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Values))
		{
			return;
		}
		TSet<FString> Marked;
		for (const TSharedPtr<FJsonValue>& Value : Values)
		{
			FString Path;
			if (Value.IsValid() && Value->TryGetString(Path) && !Path.IsEmpty())
			{
				Marked.Add(Path);
			}
		}
		Publish(EChange::None, [&Marked](FSessionSnapshot& State) { State.MarkedForAdd = MoveTemp(Marked); });
	}

	void FGitSession::SaveMarkedForAdd(const TSet<FString>& Marked) const
	{
		TArray<TSharedPtr<FJsonValue>> Values;
		for (const FString& Path : Marked)
		{
			Values.Add(MakeShared<FJsonValueString>(Path));
		}
		FString Text;
		FJsonSerializer::Serialize(Values, TJsonWriterFactory<>::Create(&Text));
		const FString File = Repository->GetGitDir() / TEXT("uegit/marked-for-add.json");
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(File), true);
		FFileHelper::SaveStringToFile(Text, *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}
}
