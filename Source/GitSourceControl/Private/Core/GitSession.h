// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#pragma once

#include "GitLockJournal.h"
#include "GitRepository.h"
#include "GitTypes.h"
#include "Misc/EnumClassFlags.h"

#include <atomic>

namespace GitCore
{
	class FSerialWorkQueue;

	/** Parts of the snapshot that changed in a publication. */
	enum class EChange : uint8
	{
		None = 0,
		Status = 1 << 0,
		Locks = 1 << 1,
		Remote = 1 << 2,
		Stashes = 1 << 3,
		Branches = 1 << 4,
		All = 0xFF
	};
	ENUM_CLASS_FLAGS(EChange);

	/** Immutable view of everything the editor shows about one worktree. Cheap to copy. */
	struct FSessionSnapshot
	{
		uint64 Generation = 0;
		FString Root;

		bool bStatusValid = false;
		FString StatusError;
		FDateTime StatusAtUtc;
		FStatusSnapshot Status;
		TSharedPtr<const TSet<FString>, ESPMode::ThreadSafe> Tracked;
		TSharedPtr<const TMap<FString, FFileAttributes>, ESPMode::ThreadSafe> Attributes;
		/** True when some .gitattributes file declares `lockable`, i.e. the team uses LFS locks. */
		bool bLockableRules = false;

		FLockSnapshot Locks;
		/** Ids of locks this worktree recorded taking (others marked ours were taken elsewhere). */
		TSet<FString> LocksTakenHere;

		FRemoteState Remote;
		TSet<FString> MarkedForAdd;
		TArray<FStashEntry> Stashes;
		TArray<FBranchInfo> Branches;

		bool IsTracked(const FString& Path) const { return Tracked.IsValid() && Tracked->Contains(Path); }
		const FFileAttributes* FindAttributes(const FString& Path) const { return Attributes.IsValid() ? Attributes->Find(Path) : nullptr; }
	};
	using FSessionSnapshotRef = TSharedRef<const FSessionSnapshot, ESPMode::ThreadSafe>;

	/**
	 * One repository worktree as seen by the editor. Owns three worker lanes:
	 * - mutations: status refresh and every index/worktree/ref/lock change, strictly in order;
	 * - background: fetch and lock verification started by the editor, never blocking mutations;
	 * - reads: history, diffs and revision exports.
	 * Results are published as immutable snapshots; listeners hear about them on the game thread.
	 */
	class FGitSession : public TSharedFromThis<FGitSession, ESPMode::ThreadSafe>
	{
	public:
		/** Opens (or reuses) the session for the worktree containing Path. */
		static TSharedPtr<FGitSession, ESPMode::ThreadSafe> Open(const FString& GitBinary, const FString& Path, FString& OutError);
		/** Stops every session's threads. Called when the module shuts down. */
		static void ShutdownAll();
		/** Stops this session and forgets it; a later Open starts a fresh one. */
		void Close();

		~FGitSession();

		const FGitRepository& GetRepository() const { return *Repository; }
		FGitRepositoryRef GetRepositoryRef() const { return Repository; }
		FSessionSnapshotRef GetSnapshot() const;

		DECLARE_MULTICAST_DELEGATE_OneParam(FOnChanged, EChange);
		/** Broadcast on the game thread after new state is published. */
		FOnChanged& OnChanged() { return ChangedDelegate; }

		void EnqueueMutation(TUniqueFunction<void()> Task);
		void EnqueueBackground(TUniqueFunction<void()> Task);
		void EnqueueRead(TUniqueFunction<void()> Task);
		/** Waits for queued mutations. For commandlets and tests; never call from a session lane. */
		void FlushMutations();
		bool IsMutationLane() const;

		// The following run on the calling thread: call them from a session lane, or inline in commandlets.

		/** Re-reads status; OnlyPaths limits the work to some files when possible. */
		bool RefreshStatus(const TArray<FString>* OnlyPaths = nullptr, FString* OutError = nullptr);
		bool RefreshLocks(ECallKind Kind, FString* OutError = nullptr);
		bool Fetch(ECallKind Kind, const FCancelTokenPtr& Cancel, FString* OutError = nullptr);
		void RefreshRemote();
		void RefreshStashes();
		void RefreshBranches();
		/** Makes sure attributes are known for Paths (used before answering lock questions). */
		bool EnsureAttributes(const TArray<FString>& Paths);
		void RecordLockTaken(const FLockInfo& Lock);
		void RecordLockReleased(const FString& Path);
		void SetMarkedForAdd(const TArray<FString>& Paths, bool bMarked);

		// Coalesced requests, safe to call from any thread.
		void RequestStatusRefresh();
		void RequestLockRefresh(bool bUserInitiated);
		void RequestFetch(bool bUserInitiated);
		void RequestStashesAndBranches();

		/** The configured lock remote, else the branch's default remote once status has been read. */
		FString GetLockRemote() const;
		/** Like GetLockRemote, but asks git when status has not been read yet. Call from a session lane. */
		FString ResolveLockRemote();
		void SetLockRemote(const FString& Remote);
		const FGitLockJournal& GetJournal() const { return *Journal; }
		/** Cancelled when the session shuts down, and before Sync/Submit cancel background fetches. */
		FCancelTokenPtr GetBackgroundCancel() const;
		void CancelBackgroundWork();

	private:
		explicit FGitSession(FGitRepositoryRef InRepository);
		void Shutdown();
		void Publish(EChange Changes, TFunctionRef<void(FSessionSnapshot&)> Edit);
		void NotifyChanged(EChange Changes);
		bool RefreshAttributesIfStale(const FStatusSnapshot& Status);
		void LoadMarkedForAdd();
		void SaveMarkedForAdd(const TSet<FString>& Marked) const;

		FGitRepositoryRef Repository;
		TUniquePtr<FGitLockJournal> Journal;
		TUniquePtr<FSerialWorkQueue> MutationQueue;
		TUniquePtr<FSerialWorkQueue> BackgroundQueue;
		TUniquePtr<FSerialWorkQueue> ReadQueue;

		mutable FRWLock SnapshotLock;
		FSessionSnapshotRef Snapshot;
		FOnChanged ChangedDelegate;
		std::atomic<uint8> PendingChanges{0};

		mutable FCriticalSection StateLock;
		FString LockRemote;
		/** The branch's upstream remote (or origin), refreshed off the game thread. */
		FString CachedDefaultRemote;
		FString TrackedToken;
		FString AttributesToken;
		FString RemoteToken;
		FCancelTokenPtr BackgroundCancel;

		std::atomic<bool> bStatusRequested{false};
		std::atomic<bool> bLocksRequested{false};
		std::atomic<bool> bFetchRequested{false};
		std::atomic<bool> bListsRequested{false};
	};
	using FGitSessionPtr = TSharedPtr<FGitSession, ESPMode::ThreadSafe>;
	using FGitSessionRef = TSharedRef<FGitSession, ESPMode::ThreadSafe>;
}
