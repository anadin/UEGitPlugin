// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#pragma once

#include "CoreMinimal.h"
#include "HAL/CriticalSection.h"
namespace GitWorkspaceSession { class FLease; }

namespace GitWorkspace
{
FString FindGitExecutable(const FString& ConfiguredPath = FString());

struct FResult
{
    int32 Code = -1;
    TArray<uint8> Out;
    FString Error;
    bool Ok() const { return Code == 0; }
    FString Text() const;
};

// Byte streams remain separate. Must run on a worker thread in interactive use.
FResult Run(const FString& Executable, const FString& Directory, const TArray<FString>& Arguments, double TimeoutSeconds = 60.0);

struct FFile
{
    FString Path;
    FString OriginalPath;
    TCHAR Index = '.';
    TCHAR Working = '.';
    bool bUntracked = false;
    bool bConflict = false;
    bool bSubmodule = false;
    bool bLockable = false;
    bool bLfs = false;
    bool HasStaged() const { return !bUntracked && Index != '.' && !bConflict; }
    bool HasUnstaged() const { return bUntracked || Working != '.' || bConflict; }
};

struct FSnapshot
{
    FString Root;
    FString Branch;
    FString Head;
    FString Upstream;
    TArray<FFile> Files;
    TArray<uint8> IndexEntries;
    bool bValid = false;
    bool bUnborn = false;
    bool bOperationInProgress = false;
    FString Error;
    int32 StagedCount() const;
    bool HasConflicts() const;
};

bool ParseStatus(const TArray<uint8>& Bytes, FSnapshot& Out, FString& Error);

enum class ELockState { NotLockable, Unknown, Unlocked, Ours, Theirs, Stale };
struct FLock
{
    FString Id, Path, Owner, LockedAt;
    bool bOurs = false;
};
struct FLockSnapshot
{
    FString Root, Remote, Endpoint, Context, Branch, Error;
    TMap<FString, FLock> Locks;
    TArray<FFile> Candidates;
    FDateTime VerifiedAt;
    double VerifiedSeconds = 0;
    bool bVerified = false;
    bool IsFresh() const;
    ELockState State(const FString& Path, bool bLockable) const;
    FString Label(const FString& Path, bool bLockable) const;
};
// Strictly parse a complete, verified LFS response. Never infer ownership from user.name.
bool ParseVerifiedLocks(const FString& Json, TMap<FString, FLock>& Locks, FString& Error);

enum class EPullPathKind { Documentation, Package, RestartRequired, Unsupported };
struct FIncomingChange
{
    FString Path, OldMode, NewMode;
    TCHAR Status = '?';
    EPullPathKind Kind = EPullPathKind::Unsupported;
};
// Raw, NUL-delimited, no-rename tree diff; paths are never split on whitespace.
bool ParseIncomingChanges(const TArray<uint8>& Bytes, TArray<FIncomingChange>& Changes, FString& Error);

struct FHistoryCommit { FString Oid, Author, Date, Subject; TArray<FString> Parents; };
struct FHistoryList
{
    FString Root, Branch, Head, Error;
    TArray<FHistoryCommit> Commits;
    bool bValid = false, bHasMore = false;
};
struct FHistoryChange { FString Path, OldOid, NewOid, OldMode, NewMode; TCHAR Status = '?'; };
struct FCommitInspection
{
    FHistoryCommit Commit;
    FString Root, Message, Error;
    TArray<FHistoryChange> Files;
    bool bValid = false;
    FString Text() const;
};
// Complete NUL-delimited commit records; never split paths/messages on newlines.
bool ParseHistory(const TArray<uint8>& Bytes, TArray<FHistoryCommit>& Commits, FString& Error);

struct FRemoteSnapshot
{
    FString Root, Branch, Head, Remote, RemoteRef, RemoteHead, Context, Error;
    TArray<FIncomingChange> IncomingChanges;
    int32 Ahead = 0, Behind = 0;
    bool bValid = false;
    FDateTime FetchedAt;
    double FetchedSeconds = 0;
    bool IsFresh() const;
};
FString RestartPullPathBlocker(const FRemoteSnapshot& Reviewed);
FString EditorPullPathBlocker(const FRemoteSnapshot& Reviewed);

// Historical cache verification for one reviewed commit, not permission to
// replace packages or a guarantee against later external cache changes.
struct FIncomingLfsResult
{
    FString Root, Head, Commit, Context, Error;
    FDateTime VerifiedAt;
    bool bVerified = false;
    bool Matches(const FRemoteSnapshot& Remote) const;
};

struct FStashEntry { FString Oid, Selector, Label; };
struct FStashList { TArray<FStashEntry> Entries; FString Error, Fingerprint; bool bValid = false; };
struct FUnlockReview
{
    FLockSnapshot Locks;
    FString Path, Head, StashFingerprint, Error;
    TArray<FString> Checks;
    TArray<FStashEntry> BlockingStashes;
    int32 StashesChecked = 0;
    bool bReady = false;
    bool bAwaitingPush = false;
    bool IsFresh() const { return bReady && Error.IsEmpty() && Locks.IsFresh(); }
    FString Text() const;
};
struct FOutgoingCommit { FString Oid, Subject; TArray<FString> Paths; };
struct FPushHandoffReview
{
    FRemoteSnapshot Remote;
    FLockSnapshot Locks;
    TArray<FOutgoingCommit> Commits;
    TArray<FUnlockReview> Assets;
    FString Error;
    bool bValid = false;
    bool IsFresh() const { return bValid && Error.IsEmpty() && Remote.IsFresh() && Locks.IsFresh(); }
    bool IsRetry() const { return Remote.Ahead == 0 && Remote.RemoteHead == Remote.Head; }
    FString Text(const TArray<FString>& Selected) const;
};
struct FHandoffLockResult { FString Path, Id, Error; bool bReleased = false; };
struct FPushHandoffResult
{
    bool bPushVerified = false;
    bool bUnlockOnly = false;
    FString Error;
    TArray<FHandoffLockResult> Assets;
    FLockSnapshot Locks;
    FString Text() const;
};
struct FStashReview
{
    FSnapshot Local;
    FString Oid, Base, IndexCommit, WorkingTree, IndexTree, ConfigurationHash, Fingerprint, Error, Text;
    TArray<FIncomingChange> Changes;
    TArray<FString> SelectedPaths, UntrackedPaths;
    TArray<FString> ApplyWorkingPaths, ApplyIndexPaths, PreservedUntrackedPaths;
    FString PreservedUntrackedBytes, LocalIndexTree, LocalWorkingTree;
    FString UntrackedTree, UntrackedCommit, UntrackedBytes;
    FString ExpectedIndexTree, ExpectedWorkingTree, ExpectedWorkingCommit;
    double ReviewedSeconds = 0;
    bool bCreate = false, bSelected = false, bIncludeUntracked = false, bRestoreIndex = true, bValid = false;
    bool IsFresh() const;
};

// Inspection is independent of whether this checkout can apply the stash.
struct FStashInspection
{
    FStashEntry Entry;
    FString Root, ListFingerprint, Text, Error, DropBlocker;
    double ReviewedSeconds = 0;
    bool bValid = false;
    bool IsFresh() const;
};

struct FDiscardReview
{
    FStashReview Capture;
    TArray<FString> Paths, UntrackedPaths;
    TArray<uint8> RawHashes, UntrackedHashes;
    FString Id = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    FString BeforeWorkingTree, BeforeWorkingCommit, IndexTree, ExpectedWorkingTree, ExpectedWorkingCommit;
    FString StashFingerprint, Fingerprint, Text, Error;
    TArray<FIncomingChange> Changes;
    bool bValid = false;
    bool IsFresh() const { return bValid && Error.IsEmpty() && Capture.IsFresh(); }
};

struct FAssetSaveReview
{
    FSnapshot Local;
    FLockSnapshot Locks;
    TArray<FString> Paths, NeedsLock, NewPaths;
    TArray<uint8> RawHashes;
    FString Fingerprint, Error;
    bool bValid = false;
    bool IsFresh() const { return bValid && Error.IsEmpty() && (Paths.IsEmpty() || Locks.IsFresh()); }
    FString Text() const;
};
// Issued only after preparation. The caller cannot edit its verified identity.
class FAssetSavePermit
{
public:
    bool ContainsPath(const FString& Path) const { return Review.Paths.Contains(Path); }
private:
    friend class FRepository;
    FAssetSaveReview Review;
};
struct FAssetSavePreparation
{
    FResult Result;
    TSharedPtr<const FAssetSavePermit, ESPMode::ThreadSafe> Permit;
    TArray<FString> AcquiredPaths;
};

class FRepository
{
public:
    FRepository(FString InGit, FString InDirectory);
    FSnapshot Refresh();
    FAssetSaveReview ReviewAssetSave(const TArray<FString>& Paths, const FString& Remote, const TArray<FString>& NewPaths = {});
#if PLATFORM_MAC
    FAssetSavePreparation PrepareAssetSave(const FAssetSaveReview& Reviewed, const GitWorkspaceSession::FLease& Lease, bool bLockConfirmed = false);
    FResult ValidateAssetSave(const FAssetSavePermit& Permit, const FString& Path, const GitWorkspaceSession::FLease& Lease);
#endif
    FResult IsLockableAsset(const FString& Path);

    FResult Stage(const TArray<FString>& Paths);
    FResult Unstage(const TArray<FString>& Paths);
    FResult Commit(const FSnapshot& Reviewed, const FString& Message);
    FResult Diff(const FString& Path, bool bStaged);
    FHistoryList ListHistory(int32 Limit = 100);
    FCommitInspection InspectCommit(const FString& Oid);
    FResult InspectCommitFile(const FString& Oid, const FString& Path);
    FLockSnapshot VerifyLocks(const FString& Remote);
    FUnlockReview ReviewUnlock(const FString& Remote, const FString& Path);
    FResult ChangeLock(const FLockSnapshot& Reviewed, const FString& Path, bool bUnlock, bool bHandoffConfirmed = false, const FString& ReviewedHead = FString());
    FRemoteSnapshot Fetch();
    FResult Push(const FRemoteSnapshot& Reviewed);
    FPushHandoffReview ReviewPushHandoff(const FRemoteSnapshot& Reviewed);
    FPushHandoffResult ExecutePushHandoff(const FPushHandoffReview& Reviewed, const TArray<FString>& Selected, bool bHandoffConfirmed = false);
    FResult Pull(const FRemoteSnapshot& Reviewed);
    FIncomingLfsResult PrepareIncomingLfs(const FRemoteSnapshot& Reviewed);
    FStashList ListStashes();
    FStashInspection InspectStash(const FString& Oid, const FString& Selector = FString());
    FStashReview ReviewStash(const FString& Oid = FString(), bool bRestoreIndex = true, bool bIncludeUntracked = false);
    FStashReview ReviewSelectedStash(const TArray<FString>& Paths, bool bIncludeUntracked = false);
    FDiscardReview ReviewDiscard(const TArray<FString>& Paths);
#if PLATFORM_MAC
    FResult ExecuteDiscard(const FDiscardReview& Reviewed, const GitWorkspaceSession::FLease& Lease, bool bConfirmed = false);
    FResult CompleteDiscard(const FDiscardReview& Reviewed, const GitWorkspaceSession::FLease& Lease);
    FResult PullAfterEditorExit(const FRemoteSnapshot& Reviewed, const GitWorkspaceSession::FLease& Lease);
    // Requires quiesced package loaders on the game thread. Leaves recovery
    // active until that caller has verified package reload and registry refresh.
    FResult PullForReload(const FRemoteSnapshot& Reviewed, const FIncomingLfsResult& Prepared, const GitWorkspaceSession::FLease& Lease);
    FResult CompleteReloadPull(const FRemoteSnapshot& Reviewed, const GitWorkspaceSession::FLease& Lease);
    FResult DropStash(const FStashInspection& Reviewed, const GitWorkspaceSession::FLease& Lease);
    FResult ExecuteStash(const FStashReview& Reviewed, const FString& Name, const GitWorkspaceSession::FLease& Lease);
    FResult CompleteStash(const FStashReview& Reviewed, const GitWorkspaceSession::FLease& Lease);
#endif
    const FString& GitExecutable() const { return GitBinary; }
    const FString& Directory() const { return RequestedDirectory; }
private:
    FAssetSaveReview ReviewAssetSaveInternal(const TArray<FString>& Paths, const FString& Remote, const TArray<FString>& NewPaths = {});
    FResult CheckNewAssetPath(const FString& Path, const FSnapshot& Local) const;
    FResult IsLockableAssetInternal(const FString& Path) const;
    FDiscardReview ReviewDiscardInternal(const TArray<FString>& Paths);
    FResult VerifyDiscardResult(const FDiscardReview& Reviewed);
    FCommitInspection InspectCommitInternal(const FString& Oid);
#if PLATFORM_MAC
    FResult PullAssets(const FRemoteSnapshot& Reviewed, const GitWorkspaceSession::FLease& Lease, bool bForReload);
    FResult HydratedGit(const TArray<FString>& Args) const;
#endif
    FStashList ListStashesInternal();
    FStashInspection InspectStashInternal(const FString& Oid, const FString& Selector);
    FResult ReadStashPaths(const FStashEntry& Entry, TArray<FString>& Paths, FString* Details = nullptr) const;
    FUnlockReview ReviewUnlockInternal(const FLockSnapshot& Current, const FString& Path, const FString& ReviewedHead = FString(), bool bRequirePublished = true);
    FPushHandoffReview ReviewPushHandoffInternal(const FRemoteSnapshot& Reviewed);
    FResult PushInternal(const FRemoteSnapshot& Reviewed);
    FResult ChangeLockInternal(const FLockSnapshot& Reviewed, const FString& Path, bool bUnlock, bool bHandoffConfirmed, const FString& ReviewedHead, bool bNewAsset = false);
    FString StashDropRecovery(FString* ReportPath = nullptr) const;
    FStashReview ReviewStashInternal(const FString& Oid, bool bRestoreIndex, bool bSelected = false, const TArray<FString>& Paths = {}, bool bIncludeUntracked = false);
    FResult CaptureUntrackedTree(const TArray<FString>& Paths) const;
    FResult CheckStashPaths(const TArray<FString>& Paths, bool bExisting) const;
    FResult UntrackedFingerprint(const TArray<FString>& Paths) const;
    FResult MakeStashCommit(const FString& Tree, const TArray<FString>& Parents, const FString& Message = TEXT("Git Workspace snapshot")) const;
    FResult PrepareStashApply(FStashReview& Review) const;
#if PLATFORM_MAC
    FResult RestoreStashPaths(const FStashReview& Review) const;
#endif
    FResult ProjectStashTree(const FString& Initial, const FString& Target, const TArray<FString>& Paths) const;
    FResult CheckLocalLfs(const FString& Commit) const;
    FResult VerifyStashResult(const FStashReview& Reviewed);
    FSnapshot RefreshInternal();
    bool RemoteContext(FRemoteSnapshot& Out);
    bool ValidateRemoteReview(const FRemoteSnapshot& Reviewed, FSnapshot& Current, FString& Error);
    FString FetchRef = TEXT("refs/uegit/fetched/") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
    FLockSnapshot VerifyLocksInternal(const FString& Remote);
    bool LockContext(const FString& Remote, FLockSnapshot& Out);
    bool LockCandidates(FLockSnapshot& Out);
    FResult Lfs(const FString& Remote, const TArray<FString>& Args, const FString& Storage = FString()) const;
    bool LockRecordPath(const FLockSnapshot& Context, const FLock& Lock, FString& File, FString& GitDir, FString& Error) const;
    bool ReadLockRecord(const FLockSnapshot& Context, const FLock& Lock, FString& Error) const;
    bool WriteLockRecord(const FLockSnapshot& Context, const FLock& Lock, FString& Error) const;
    bool RemoveLockRecord(const FLockSnapshot& Context, const FLock& Lock, FString& Error) const;
    FResult Git(const TArray<FString>& Args) const;
    FResult VerifyWorkingLfs(const FString& Commit) const;
    FResult ChangeIndex(const TArray<FString>& Paths, bool bStage);
    FString GitBinary;
    FString RequestedDirectory;
    FString Root;
    FCriticalSection Mutex;
};
}
