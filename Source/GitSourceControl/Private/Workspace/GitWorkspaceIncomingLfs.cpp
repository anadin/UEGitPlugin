// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#include "GitWorkspaceRepository.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Misc/ScopeLock.h"

namespace GitWorkspace
{
bool FIncomingLfsResult::Matches(const FRemoteSnapshot& Remote) const
{
    return bVerified && Error.IsEmpty() && Remote.IsFresh() && Root == Remote.Root &&
        Head == Remote.Head && Commit == Remote.RemoteHead && Context == Remote.Context;
}

FIncomingLfsResult FRepository::PrepareIncomingLfs(const FRemoteSnapshot& Reviewed)
{
    FScopeLock Guard(&Mutex);
    FIncomingLfsResult Result;
    Result.Root = Reviewed.Root; Result.Head = Reviewed.Head;
    Result.Commit = Reviewed.RemoteHead; Result.Context = Reviewed.Context;
    FSnapshot Before;
    if (!ValidateRemoteReview(Reviewed, Before, Result.Error)) return Result;
    if (Reviewed.Ahead || !Reviewed.Behind || !Git({TEXT("merge-base"), TEXT("--is-ancestor"), Reviewed.Head, Reviewed.RemoteHead}).Ok())
    { Result.Error = TEXT("Download requires a reviewed incoming fast-forward. Reconcile divergence externally first."); return Result; }

    // Downloads use the current endpoint configuration. Do not silently adopt
    // an incoming endpoint, or mix a local configuration edit with this review.
    for (const auto& File : Before.Files) if (File.Path == TEXT(".lfsconfig") || File.OriginalPath == TEXT(".lfsconfig"))
    { Result.Error = TEXT("Commit or restore .lfsconfig, then Fetch again before downloading incoming assets."); return Result; }
    const auto ConfigDiff = Git({TEXT("diff"), TEXT("--quiet"), Reviewed.Head, Reviewed.RemoteHead, TEXT("--"), TEXT(".lfsconfig")});
    if (!ConfigDiff.Ok())
    { Result.Error = TEXT("Incoming .lfsconfig changes require external endpoint review before downloading assets. ") + ConfigDiff.Error; return Result; }
    FLockSnapshot Endpoint;
    if (!LockContext(Reviewed.Remote, Endpoint)) { Result.Error = Endpoint.Error; return Result; }

    // fsck installs hooks even with --dry-run in Git LFS 3.8. Give these
    // invocations a disposable hooks directory; never install/replace user hooks.
    const FString Hooks = FPaths::Combine(FPlatformProcess::UserTempDir(), TEXT("uegit-lfs-check-") + FGuid::NewGuid().ToString(EGuidFormats::Digits));
    if (!IFileManager::Get().MakeDirectory(*Hooks, true))
    { Result.Error = TEXT("Cannot create the temporary LFS verification directory."); return Result; }
    ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*Hooks, false, true); };
    auto LfsCheck = [&](const TArray<FString>& Args)
    {
        TArray<FString> All {TEXT("-c"), TEXT("core.hooksPath=") + Hooks,
            TEXT("-c"), TEXT("remote.lfsdefault=") + Reviewed.Remote,
            TEXT("-c"), TEXT("lfs.fetchinclude="), TEXT("-c"), TEXT("lfs.fetchexclude="),
            TEXT("-c"), TEXT("lfs.fetchrecentalways=false"), TEXT("lfs")};
        All.Append(Args); return Git(All);
    };
    auto Failed = [&](const FString& Message, const FResult& Operation)
    {
        // LFS fsck writes useful path/object diagnostics to stdout on failure.
        Result.Error = Message + TEXT("\n") + (Operation.Text() + TEXT("\n") + Operation.Error).Left(8192);
    };
    const auto Pointers = LfsCheck({TEXT("fsck"), TEXT("--pointers"), TEXT("--dry-run"), Reviewed.RemoteHead});
    if (!Pointers.Ok())
    { Failed(TEXT("Incoming LFS pointers failed verification. Working files were not replaced."), Pointers); return Result; }
    // Explicit ref + empty include/exclude cover the whole target tree, even
    // when the Content view or local fetch filters hide some of those paths.
    const auto Download = LfsCheck({TEXT("fetch"), TEXT("--include="), TEXT("--exclude="), Reviewed.Remote, Reviewed.RemoteHead});
    if (!Download.Ok())
    { Failed(TEXT("Incoming LFS download did not complete. Some objects may be cached; working files were not replaced. Retry explicitly after resolving the error."), Download); return Result; }
    const auto Objects = LfsCheck({TEXT("fsck"), TEXT("--objects"), TEXT("--dry-run"), Reviewed.RemoteHead});
    if (!Objects.Ok())
    { Failed(TEXT("Incoming LFS objects failed integrity verification. Corrupt objects were left in place for external repair; working files were not replaced."), Objects); return Result; }

    FSnapshot After; FLockSnapshot FinalEndpoint;
    if (!ValidateRemoteReview(Reviewed, After, Result.Error)) return Result;
    if (After.IndexEntries != Before.IndexEntries || !LockContext(Reviewed.Remote, FinalEndpoint) || FinalEndpoint.Context != Endpoint.Context)
    { Result.Error = TEXT("The index or LFS configuration changed during download. Cached objects remain available; Fetch and review again."); return Result; }
    Result.bVerified = true; Result.VerifiedAt = FDateTime::UtcNow();
    return Result;
}
}
