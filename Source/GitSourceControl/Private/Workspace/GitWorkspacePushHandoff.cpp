// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#include "GitWorkspaceRepository.h"
#include "Misc/ScopeLock.h"

namespace GitWorkspace
{
namespace
{
FString HandoffDisplay(FString Value)
{ return Value.Replace(TEXT("\\"), TEXT("\\\\")).Replace(TEXT("\r"), TEXT("\\r")).Replace(TEXT("\n"), TEXT("\\n")).Replace(TEXT("\t"), TEXT("\\t")); }
bool HandoffPaths(const FResult& Result, TArray<FString>& Paths)
{
    if (!Result.Ok()) return false;
    int32 Start = 0;
    for (int32 I = 0; I < Result.Out.Num(); ++I) if (!Result.Out[I])
    {
        FUTF8ToTCHAR Text(reinterpret_cast<const ANSICHAR*>(Result.Out.GetData() + Start), I - Start);
        Paths.AddUnique(FString(Text.Length(), Text.Get())); Start = I + 1;
    }
    return Start == Result.Out.Num();
}
}
FString FPushHandoffReview::Text(const TArray<FString>& Selected) const
{
    FString Out = IsRetry() ? TEXT("RETRY LOCK RELEASE ONLY\n") : TEXT("PUSH AND SELECTED LOCK HANDOFF\n");
    Out += TEXT("\nBranch: ") + HandoffDisplay(Remote.Branch) + TEXT("\nCommit: ") + Remote.Head + TEXT("\nDestination: ") + HandoffDisplay(Remote.Remote) + TEXT(" / ") + HandoffDisplay(Remote.RemoteRef) + TEXT("\n");
    Out += IsRetry() ? TEXT("\nThis reviewed commit is already published. No Push will be sent.\n")
        : TEXT("\nPush publishes ALL outgoing commits and their files. The lock checklist does not limit what is pushed. Staged, working and unsaved edits are excluded.\n");
    for (const auto& Commit : Commits)
    {
        Out += TEXT("\n") + Commit.Oid + TEXT("  ") + HandoffDisplay(Commit.Subject) + TEXT("\n");
        for (const auto& Path : Commit.Paths) Out += TEXT("  ") + HandoffDisplay(Path) + TEXT("\n");
    }
    Out += TEXT("\nLOCKS CHECKED FOR RELEASE\n");
    if (Selected.IsEmpty()) Out += TEXT("(none; tick eligible assets below)\n");
    for (const auto& Path : Selected) Out += HandoffDisplay(Path) + TEXT("\n");
    Out += TEXT("\nLOCKS TO KEEP\n");
    for (const auto& Asset : Assets) if (!Selected.Contains(Asset.Path)) Out += HandoffDisplay(Asset.Path) + TEXT("\n");
    for (const auto& Asset : Assets) if (!Asset.bReady)
    {
        Out += TEXT("\nRELEASE BLOCKED: ") + HandoffDisplay(Asset.Path) + TEXT("\n") + Asset.Error + TEXT("\n");
        for (const auto& Stash : Asset.BlockingStashes)
            Out += TEXT("  Saved stash: ") + HandoffDisplay(Stash.Selector) + TEXT("  ") + Stash.Oid.Left(10) + TEXT("  ") + HandoffDisplay(Stash.Label) + TEXT("\n");
    }
    Out += TEXT("\nBefore every release, ownership, acquisition, saved/stashed work and live publication are checked again. Failed or unconfirmed Push releases nothing. A later release failure retains that reservation or reports uncertainty; successful releases are not rolled back. Confirm that the team handoff is complete and no other clone still needs each checked lock.\n");
    if (!Error.IsEmpty()) Out += TEXT("\nREVIEW BLOCKED\n") + Error;
    return Out;
}
FString FPushHandoffResult::Text() const
{
    FString Out = bPushVerified ? (bUnlockOnly ? TEXT("PUBLISHED COMMIT VERIFIED; UNLOCK RETRY ONLY\n") : TEXT("PUSH SUCCEEDED AND WAS VERIFIED\n"))
        : bUnlockOnly ? TEXT("UNLOCK RETRY BLOCKED; NO RELEASE REQUESTS SENT\n") : TEXT("PUSH NOT COMPLETED OR NOT CONFIRMED; NO LOCKS RELEASED BY THIS ACTION\n");
    if (!Error.IsEmpty()) Out += TEXT("\n") + Error + TEXT("\n");
    for (const auto& Asset : Assets)
        Out += TEXT("\n") + HandoffDisplay(Asset.Path) + (Asset.bReleased ? TEXT("\nRELEASED; server absence verified.\n") : TEXT("\nRETAINED OR UNCONFIRMED: ") + Asset.Error + TEXT("\n"));
    Out += TEXT("\nUnchecked locks were not requested for release. Files and staging were not changed.");
    if (bPushVerified && Assets.ContainsByPredicate([](const auto& Asset) { return !Asset.bReleased; }))
        Out += TEXT("\nReview remaining locks to retry only their release. Do not repeat Push for this completed publication.");
    if (!Locks.IsFresh()) Out += TEXT("\nFinal ownership refresh is unavailable or expired. Verify locks to inspect current server state.");
    return Out;
}
FPushHandoffReview FRepository::ReviewPushHandoff(const FRemoteSnapshot& Reviewed)
{ FScopeLock Guard(&Mutex); return ReviewPushHandoffInternal(Reviewed); }
FPushHandoffReview FRepository::ReviewPushHandoffInternal(const FRemoteSnapshot& Reviewed)
{
    FPushHandoffReview R; R.Remote = Reviewed; FSnapshot Current;
    auto Fail = [&](const FString& Error) { R.Error = Error; return R; };
    if (!ValidateRemoteReview(Reviewed, Current, R.Error)) return R;
    if (Reviewed.Behind || (Reviewed.Ahead == 0 && Reviewed.RemoteHead != Reviewed.Head))
        return Fail(TEXT("Handoff requires an outgoing fast-forward or the exact already-published commit. Fetch and reconcile incoming changes first."));
    if (Reviewed.Ahead && !Git({TEXT("merge-base"), TEXT("--is-ancestor"), Reviewed.RemoteHead, Reviewed.Head}).Ok())
        return Fail(TEXT("Reconcile divergence before Push and handoff."));
    if (!R.IsRetry())
    {
        const auto Commits = Git({TEXT("rev-list"), TEXT("--reverse"), Reviewed.RemoteHead + TEXT("..") + Reviewed.Head});
        TArray<FString> Revisions; Commits.Text().ParseIntoArrayLines(Revisions);
        if (!Commits.Ok() || Revisions.Num() != Reviewed.Ahead || Revisions.Num() > 128)
            return Fail(TEXT("Cannot show all outgoing commits, or more than 128 require external review. No Push or unlock was sent."));
        for (const auto& Oid : Revisions)
        {
            FOutgoingCommit Commit; Commit.Oid = Oid;
            const auto Subject = Git({TEXT("show"), TEXT("-s"), TEXT("--format=%s"), Oid, TEXT("--")});
            if (!Subject.Ok() || !HandoffPaths(Git({TEXT("diff-tree"), TEXT("--root"), TEXT("--no-commit-id"), TEXT("--name-only"), TEXT("--no-renames"), TEXT("-r"), TEXT("-m"), TEXT("-z"), Oid, TEXT("--")}), Commit.Paths))
                return Fail(TEXT("Cannot read the complete outgoing commit/file list."));
            Commit.Subject = Subject.Text().TrimEnd(); Commit.Paths.Sort(); R.Commits.Add(MoveTemp(Commit));
        }
    }
    R.Locks = VerifyLocksInternal(Reviewed.Remote);
    if (!R.Locks.IsFresh()) return Fail(R.Locks.Error);
    TArray<FString> Paths;
    for (const auto& Pair : R.Locks.Locks) if (Pair.Value.bOurs) Paths.Add(Pair.Key);
    if (Paths.Num() > 128) return Fail(TEXT("More than 128 owned locks require external handoff review."));
    Paths.Sort();
    for (const auto& Path : Paths) R.Assets.Add(ReviewUnlockInternal(R.Locks, Path, Reviewed.Head, R.IsRetry()));
    FRemoteSnapshot Context;
    if (!RemoteContext(Context) || Context.Context != Reviewed.Context || Context.Head != Reviewed.Head || !R.Locks.IsFresh())
        return Fail(TEXT("Repository changed or ownership review expired while preparing handoff. Refresh the review."));
    R.bValid = true; return R;
}
FPushHandoffResult FRepository::ExecutePushHandoff(const FPushHandoffReview& Reviewed, const TArray<FString>& Selected, bool bHandoffConfirmed)
{
    FScopeLock Guard(&Mutex); FPushHandoffResult R; R.bUnlockOnly = Reviewed.IsRetry();
    auto Fail = [&](const FString& Error)
    {
        R.Error = Error;
        for (auto& Asset : R.Assets) if (Asset.Error.IsEmpty()) Asset.Error = TEXT("Release was not attempted.");
        R.Locks = VerifyLocksInternal(Reviewed.Remote.Remote); return R;
    };
    if (!bHandoffConfirmed || !Reviewed.IsFresh() || Selected.IsEmpty() || Selected.Num() > 128)
        return Fail(TEXT("Refresh the review, check eligible locks and confirm the team handoff first. No Push or unlock sent."));
    TSet<FString> Seen;
    for (const auto& Path : Selected)
    {
        const auto* Asset = Reviewed.Assets.FindByPredicate([&](const auto& Item) { return Item.Path == Path; });
        const auto* Lock = Reviewed.Locks.Locks.Find(Path);
        if (Seen.Contains(Path) || !Asset || !Asset->IsFresh() || !Lock || !Lock->bOurs)
            return Fail(TEXT("Checked lock set is invalid or ineligible. Nothing published or unlocked."));
        Seen.Add(Path); FHandoffLockResult Entry; Entry.Path = Path; Entry.Id = Lock->Id; R.Assets.Add(MoveTemp(Entry));
    }
    FSnapshot Current; FString Error;
    if (!ValidateRemoteReview(Reviewed.Remote, Current, Error)) return Fail(Error);
    const auto Fresh = VerifyLocksInternal(Reviewed.Remote.Remote);
    if (!Fresh.IsFresh() || Fresh.Context != Reviewed.Locks.Context) return Fail(TEXT("Lock endpoint or repository context changed. Nothing published or unlocked. ") + Fresh.Error);
    for (const auto& Entry : R.Assets)
    {
        const auto* Lock = Fresh.Locks.Find(Entry.Path);
        if (!Lock || !Lock->bOurs || Lock->Id != Entry.Id) return Fail(TEXT("Checked lock ownership or identity changed: ") + HandoffDisplay(Entry.Path));
        const auto Check = ReviewUnlockInternal(Fresh, Entry.Path, Reviewed.Remote.Head, R.bUnlockOnly);
        if (!Check.IsFresh()) return Fail(TEXT("Checked lock is no longer eligible: ") + HandoffDisplay(Entry.Path) + TEXT("\n") + Check.Error);
    }
    if (!R.bUnlockOnly)
    {
        const auto Pushed = PushInternal(Reviewed.Remote);
        if (!Pushed.Ok()) return Fail(Pushed.Error);
    }
    R.bPushVerified = true;
    // Push completion can outlive the initial lock snapshot. Fresh ownership is
    // required for each release, but only the originally checked IDs may be used.
    for (auto& Entry : R.Assets)
    {
        const auto Locks = VerifyLocksInternal(Reviewed.Remote.Remote);
        const auto* Lock = Locks.Locks.Find(Entry.Path);
        if (!Locks.IsFresh() || Locks.Context != Reviewed.Locks.Context || !Lock || !Lock->bOurs || Lock->Id != Entry.Id)
        { Entry.Error = TEXT("Ownership, identity or acquisition context changed; no release requested. ") + Locks.Error; continue; }
        const auto Released = ChangeLockInternal(Locks, Entry.Path, true, true, Reviewed.Remote.Head);
        Entry.bReleased = Released.Ok(); Entry.Error = Released.Error;
    }
    R.Locks = VerifyLocksInternal(Reviewed.Remote.Remote); return R;
}
}
