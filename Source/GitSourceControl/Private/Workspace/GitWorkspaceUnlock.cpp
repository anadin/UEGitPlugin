// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#include "GitWorkspaceRepository.h"
#include "Misc/ScopeLock.h"
#include "HAL/FileManager.h"
#if PLATFORM_MAC
#include "GitWorkspaceSession.h"
#endif

namespace GitWorkspace
{
namespace
{
FString DisplayUnlock(FString Value)
{ return Value.Replace(TEXT("\\"), TEXT("\\\\")).Replace(TEXT("\r"), TEXT("\\r")).Replace(TEXT("\n"), TEXT("\\n")).Replace(TEXT("\t"), TEXT("\\t")); }
bool OverlapsUnlockPath(const FString& A, const FString& B)
{
    // Conservative across case-insensitive filesystems and file/directory changes.
    return !A.IsEmpty() && !B.IsEmpty() && (A.Equals(B, ESearchCase::IgnoreCase) ||
        A.StartsWith(B + TEXT("/"), ESearchCase::IgnoreCase) || B.StartsWith(A + TEXT("/"), ESearchCase::IgnoreCase));
}
}
FString FUnlockReview::Text() const
{
    FString Result = TEXT("UNLOCK REVIEW\n\nAsset: ") + DisplayUnlock(Path) + TEXT("\nBranch: ") + DisplayUnlock(Locks.Branch) +
        TEXT("\nCommit: ") + Head + TEXT("\nRemote: ") + DisplayUnlock(Locks.Remote) + TEXT("\nEndpoint: ") + DisplayUnlock(Locks.Endpoint) + TEXT("\n\n");
    for (const auto& Check : Checks) Result += Check + TEXT("\n");
    if (!BlockingStashes.IsEmpty())
    {
        Result += TEXT("\nSTASHES USING THIS ASSET\n");
        for (const auto& Entry : BlockingStashes)
            Result += DisplayUnlock(Entry.Selector) + TEXT("  ") + Entry.Oid.Left(10) + TEXT("  ") + DisplayUnlock(Entry.Label) + TEXT("\n");
    }
    Result += bReady ? TEXT("\nREADY FOR HANDOFF CONFIRMATION\n") : TEXT("\nUNLOCK BLOCKED\n") + Error + TEXT("\n");
    Result += TEXT("\nOnly this asset's lock may be released. Files, staging, commits and other locks stay in place. Nothing is pushed. Server ownership alone cannot prove that another clone no longer needs the lock; confirm the team handoff before releasing it.");
    return Result;
}
FUnlockReview FRepository::ReviewUnlock(const FString& Remote, const FString& Path)
{
    FScopeLock Guard(&Mutex);
    return ReviewUnlockInternal(VerifyLocksInternal(Remote), Path);
}
FUnlockReview FRepository::ReviewUnlockInternal(const FLockSnapshot& Current, const FString& Path, const FString& ReviewedHead)
{
    FUnlockReview R; R.Locks = Current; R.Path = Path;
    auto Fail = [&](const FString& Error) { R.Error = Error; return R; };
    if (!Current.IsFresh()) return Fail(Current.Error.IsEmpty() ? TEXT("Verify server ownership again.") : Current.Error);
    const FLock* Lock = Current.Locks.Find(Path);
    if (!Lock || !Lock->bOurs) return Fail(Lock ? TEXT("Another user owns this lock. It cannot be released here.") : TEXT("The server reports no lock on this asset."));
    R.Checks.Add(TEXT("Server ownership verified; lock ID: ") + Lock->Id);
    const auto Status = RefreshInternal(); R.Head = Status.Head;
    if (!Status.bValid || Status.bOperationInProgress || Status.HasConflicts()) return Fail(TEXT("Resolve repository state before unlocking."));
    if (!ReviewedHead.IsEmpty() && Status.Head != ReviewedHead) return Fail(TEXT("Commit changed since the unlock review. Nothing unlocked; review again."));
    auto Recovery = [&]() -> FString
    {
        const FString Drop = StashDropRecovery(); if (!Drop.IsEmpty()) return Drop;
#if PLATFORM_MAC
        FString CanonicalRoot, GitDir;
        if (!GitWorkspaceSession::FindRepository(Root, CanonicalRoot, GitDir)) return TEXT("Cannot check repository recovery state.");
        if (IFileManager::Get().FileExists(*GitWorkspaceSession::RecoveryFile(GitDir))) return TEXT("Resolve the active asset/repository recovery before releasing locks.");
#endif
        return FString();
    };
    FString Error = Recovery(); if (!Error.IsEmpty()) return Fail(Error);
    if (!ReadLockRecord(Current, *Lock, Error)) return Fail(Error);
    R.Checks.Add(TEXT("Acquisition confirmed in this worktree, branch and endpoint."));
    for (const auto& File : Status.Files)
        if (OverlapsUnlockPath(File.Path, Path) || OverlapsUnlockPath(File.OriginalPath, Path))
            return Fail(TEXT("Selected asset has working or staged changes. Keep its lock."));
    R.Checks.Add(TEXT("No saved working or staged changes to this asset."));
    const auto Stashes = ListStashesInternal();
    if (!Stashes.bValid) return Fail(TEXT("Cannot verify saved stash work. Keep the lock. ") + Stashes.Error);
    if (Stashes.Entries.Num() > 128) return Fail(TEXT("More than 128 stashes require external handoff review. Keep the lock."));
    R.StashFingerprint = Stashes.Fingerprint;
    for (const auto& Entry : Stashes.Entries)
    {
        TArray<FString> Paths;
        const auto Read = ReadStashPaths(Entry, Paths);
        if (!Read.Ok()) return Fail(TEXT("Cannot inspect ") + DisplayUnlock(Entry.Selector) + TEXT(". Keep the lock. ") + Read.Error);
        ++R.StashesChecked;
        if (Paths.ContainsByPredicate([&](const FString& SavedPath) { return OverlapsUnlockPath(SavedPath, Path); })) R.BlockingStashes.Add(Entry);
    }
    const auto Rechecked = ListStashesInternal();
    if (!Rechecked.bValid || Rechecked.Fingerprint != R.StashFingerprint) return Fail(TEXT("Stash list changed during inspection. Keep the lock and review again."));
    if (!R.BlockingStashes.IsEmpty()) return Fail(TEXT("Saved stash work uses this asset. Keep its lock; review the listed stashes first."));
    R.Checks.Add(FString::Printf(TEXT("%d stashes inspected; none contains changes to this asset."), R.StashesChecked));
    const auto UpstreamRemote = Git({TEXT("config"), TEXT("--get"), TEXT("branch.") + Status.Branch + TEXT(".remote")});
    const auto Merge = Git({TEXT("config"), TEXT("--get"), TEXT("branch.") + Status.Branch + TEXT(".merge")});
    const auto Upstream = Git({TEXT("rev-parse"), TEXT("--verify"), TEXT("@{upstream}")});
    if (!UpstreamRemote.Ok() || !Merge.Ok() || !Upstream.Ok())
        return Fail(TEXT("This branch has no readable upstream. Configure its upstream on ") + DisplayUnlock(Current.Remote) + TEXT(" and publish/reconcile the branch before unlocking."));
    if (UpstreamRemote.Text().TrimEnd() != Current.Remote)
        return Fail(TEXT("This branch tracks a different remote. Select its upstream remote for locks, or configure an upstream on ") + DisplayUnlock(Current.Remote) + TEXT(" before reviewing handoff."));
    if (Upstream.Text().TrimEnd() != Status.Head)
        return Fail(TEXT("This branch does not match its upstream on ") + DisplayUnlock(Current.Remote) + TEXT(". Fetch upstream, review the outgoing commits, then Push (or reconcile incoming changes externally). Refresh this unlock review afterward. Push publishes all outgoing commits on the branch and keeps locks; it does not push only this asset."));
    const FString Ref = Merge.Text().TrimEnd();
    if (!Ref.StartsWith(TEXT("refs/heads/"))) return Fail(TEXT("Cannot identify an upstream branch for handoff."));
    const auto RemoteHead = Git({TEXT("ls-remote"), TEXT("--exit-code"), TEXT("--refs"), TEXT("--"), Current.Remote, Ref});
    if (!RemoteHead.Ok() || RemoteHead.Text().TrimEnd() != Status.Head + TEXT("\t") + Ref)
        return Fail(TEXT("The live upstream does not match HEAD or cannot be verified. Keep the lock and refresh your branch externally."));
    R.Checks.Add(TEXT("Published commit matches the live upstream: ") + DisplayUnlock(Current.Remote) + TEXT(" / ") + DisplayUnlock(Ref));
    // Revalidate after network/disk work. Arbitrary external writers are not
    // excluded; any observed change is a refusal, never an automatic retry.
    const auto FinalStatus = RefreshInternal(); FLockSnapshot FinalContext;
    if (!FinalStatus.bValid || FinalStatus.Head != Status.Head || FinalStatus.IndexEntries != Status.IndexEntries ||
        FinalStatus.bOperationInProgress || FinalStatus.HasConflicts() || !LockContext(Current.Remote, FinalContext) || FinalContext.Context != Current.Context)
        return Fail(TEXT("Repository changed during handoff checks. Nothing unlocked."));
    for (const auto& File : FinalStatus.Files)
        if (OverlapsUnlockPath(File.Path, Path) || OverlapsUnlockPath(File.OriginalPath, Path)) return Fail(TEXT("Asset changed during handoff checks. Keep its lock."));
    Error = Recovery(); if (!Error.IsEmpty()) return Fail(Error);
    const auto FinalStashes = ListStashesInternal();
    if (!FinalStashes.bValid || FinalStashes.Fingerprint != R.StashFingerprint) return Fail(TEXT("Stash state changed during handoff checks. Keep the lock."));
    if (!ReadLockRecord(Current, *Lock, Error)) return Fail(Error);
    if (!Current.IsFresh()) return Fail(TEXT("Server ownership verification expired during review. Keep the lock and review again."));
    R.bReady = true; return R;
}
}
