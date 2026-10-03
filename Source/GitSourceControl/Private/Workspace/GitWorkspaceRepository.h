// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#pragma once

#include "CoreMinimal.h"
#include "HAL/CriticalSection.h"

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

struct FRemoteSnapshot
{
    FString Root, Branch, Head, Remote, RemoteRef, RemoteHead, Context, Error;
    TArray<FString> IncomingPaths;
    int32 Ahead = 0, Behind = 0;
    bool bValid = false;
    FDateTime FetchedAt;
    double FetchedSeconds = 0;
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
    const FString& Directory() const { return RequestedDirectory; }
private:
    FSnapshot RefreshInternal();
    bool RemoteContext(FRemoteSnapshot& Out);
    bool ValidateRemoteReview(const FRemoteSnapshot& Reviewed, FSnapshot& Current, FString& Error);
    FString FetchRef = TEXT("refs/uegit/fetched/") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
    FLockSnapshot VerifyLocksInternal(const FString& Remote);
    bool LockContext(const FString& Remote, FLockSnapshot& Out);
    bool LockCandidates(FLockSnapshot& Out);
    FResult Lfs(const FString& Remote, const TArray<FString>& Args, const FString& Storage = FString()) const;
    struct FAcquiredLock { FString Id, Context, Branch; };
    TMap<FString, FAcquiredLock> AcquiredLocks;
    FResult Git(const TArray<FString>& Args) const;
    FResult ChangeIndex(const TArray<FString>& Paths, bool bStage);
    FString GitBinary;
    FString RequestedDirectory;
    FString Root;
    FCriticalSection Mutex;
};
}
