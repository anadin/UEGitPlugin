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
struct FStashReview
{
    FSnapshot Local;
    FString Oid, Base, IndexCommit, WorkingTree, IndexTree, ConfigurationHash, Fingerprint, Error, Text;
    TArray<FIncomingChange> Changes;
    double ReviewedSeconds = 0;
    bool bCreate = false, bRestoreIndex = true, bValid = false;
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

class FRepository
{
public:
    FRepository(FString InGit, FString InDirectory);
    FSnapshot Refresh();
    FResult Stage(const TArray<FString>& Paths);
    FResult Unstage(const TArray<FString>& Paths);
    FResult Commit(const FSnapshot& Reviewed, const FString& Message);
    FResult Diff(const FString& Path, bool bStaged);
    FLockSnapshot VerifyLocks(const FString& Remote);
    FResult ChangeLock(const FLockSnapshot& Reviewed, const FString& Path, bool bUnlock, bool bHandoffConfirmed = false);
    FRemoteSnapshot Fetch();
    FResult Push(const FRemoteSnapshot& Reviewed);
    FResult Pull(const FRemoteSnapshot& Reviewed);
    FIncomingLfsResult PrepareIncomingLfs(const FRemoteSnapshot& Reviewed);
    FStashList ListStashes();
    FStashInspection InspectStash(const FString& Oid, const FString& Selector = FString());
    FStashReview ReviewStash(const FString& Oid = FString(), bool bRestoreIndex = true);
#if PLATFORM_MAC
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
#if PLATFORM_MAC
    FResult PullAssets(const FRemoteSnapshot& Reviewed, const GitWorkspaceSession::FLease& Lease, bool bForReload);
    FResult HydratedGit(const TArray<FString>& Args) const;
#endif
    FStashList ListStashesInternal();
    FStashInspection InspectStashInternal(const FString& Oid, const FString& Selector);
    FString StashDropRecovery(FString* ReportPath = nullptr) const;
    FStashReview ReviewStashInternal(const FString& Oid, bool bRestoreIndex);
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
