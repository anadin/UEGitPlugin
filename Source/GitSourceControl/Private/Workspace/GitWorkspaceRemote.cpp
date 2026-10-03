// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#include "GitWorkspaceRepository.h"
#include "Misc/ScopeLock.h"
#include "Misc/ScopeExit.h"
#include "Misc/SecureHash.h"
#include "HAL/PlatformTime.h"
#include "HAL/PlatformMisc.h"

namespace GitWorkspace
{
namespace
{
FResult RemoteFailure(const FString& Message) { FResult R; R.Error = Message; return R; }
bool RemoteRecords(const FResult& R, TArray<FString>& Values)
{
    if (!R.Ok()) return false;
    int32 Start = 0;
    for (int32 I = 0; I < R.Out.Num(); ++I) if (!R.Out[I])
    {
        FUTF8ToTCHAR Text(reinterpret_cast<const ANSICHAR*>(R.Out.GetData() + Start), I - Start);
        Values.Emplace(Text.Length(), Text.Get()); Start = I + 1;
    }
    return Start == R.Out.Num();
}
bool IsDocumentation(const FString& Path)
{
    // Until package reload/code restart coordination exists, only these files are
    // allowed to change while the editor is open. Include both sides of renames.
    return (Path.StartsWith(TEXT("Docs/")) && (Path.EndsWith(TEXT(".md")) || Path.EndsWith(TEXT(".txt")))) ||
        Path == TEXT("README.md") || Path == TEXT("LICENSE.txt");
}
}
bool ParseIncomingChanges(const TArray<uint8>& Bytes, TArray<FIncomingChange>& Changes, FString& Error)
{
    Changes.Empty(); Error.Empty(); FResult Records; Records.Code = 0; Records.Out = Bytes;
    TArray<FString> Fields;
    auto Fail = [&]() { Changes.Empty(); Error = TEXT("Cannot read the complete incoming tree diff. Fetch again."); return false; };
    if (!RemoteRecords(Records, Fields) || Fields.Num() % 2) return Fail();
    TSet<FString> Seen;
    for (int32 I = 0; I < Fields.Num(); I += 2)
    {
        TArray<FString> Header; Fields[I].ParseIntoArray(Header, TEXT(" "));
        const FString& Path = Fields[I + 1];
        if (Header.Num() != 5 || !Header[0].StartsWith(TEXT(":")) || Header[4].Len() != 1 ||
            !FString(TEXT("AMDT")).Contains(Header[4]) || Path.IsEmpty() || Path.StartsWith(TEXT("/")) || Seen.Contains(Path)) return Fail();
        TArray<FString> Parts; Path.ParseIntoArray(Parts, TEXT("/"), false);
        for (const auto& Part : Parts) if (Part.IsEmpty() || Part == TEXT("..") || Part == TEXT(".")) return Fail();
        auto IsHash = [](const FString& Hash)
        {
            if (Hash.Len() != 40 && Hash.Len() != 64) return false;
            for (TCHAR C : Hash) if (!FChar::IsHexDigit(C)) return false;
            return true;
        };
        if (!IsHash(Header[2]) || !IsHash(Header[3])) return Fail();
        FIncomingChange Change; Change.Path = Path; Change.OldMode = Header[0].Mid(1); Change.NewMode = Header[1]; Change.Status = Header[4][0];
        auto ValidMode = [](const FString& Mode) { return Mode.Len() == 6 && Mode.IsNumeric(); };
        if (!ValidMode(Change.OldMode) || !ValidMode(Change.NewMode)) return Fail();
        const bool bOldAbsent = Change.OldMode == TEXT("000000"), bNewAbsent = Change.NewMode == TEXT("000000");
        if ((Change.Status == 'A' && (!bOldAbsent || bNewAbsent)) || (Change.Status == 'D' && (bOldAbsent || !bNewAbsent)) ||
            ((Change.Status == 'M' || Change.Status == 'T') && (bOldAbsent || bNewAbsent))) return Fail();
        const bool bRegular = (bOldAbsent || Change.OldMode == TEXT("100644")) && (bNewAbsent || Change.NewMode == TEXT("100644"));
        if (bRegular)
        {
            if (IsDocumentation(Path)) Change.Kind = EPullPathKind::Documentation;
            else if (Path.EndsWith(TEXT(".uasset")) || Path.EndsWith(TEXT(".umap"))) Change.Kind = EPullPathKind::Package;
            else Change.Kind = EPullPathKind::RestartRequired;
        }
        Changes.Add(MoveTemp(Change)); Seen.Add(Path);
    }
    return true;
}
bool FRemoteSnapshot::IsFresh() const
{
    const double Age = FPlatformTime::Seconds() - FetchedSeconds;
    return bValid && Error.IsEmpty() && Age >= 0 && Age < 300;
}
bool FRepository::RemoteContext(FRemoteSnapshot& Out)
{
    const auto S = RefreshInternal(); Out.Root = S.Root; Out.Branch = S.Branch; Out.Head = S.Head;
    if (!S.bValid || S.bUnborn || S.Branch == TEXT("(detached)")) { Out.Error = TEXT("Remote operations require an existing commit on a named branch."); return false; }
    const auto Remote = Git({TEXT("config"), TEXT("--get"), TEXT("branch.") + S.Branch + TEXT(".remote")});
    const auto Ref = Git({TEXT("config"), TEXT("--get"), TEXT("branch.") + S.Branch + TEXT(".merge")});
    Out.Remote = Remote.Text().TrimEnd(); Out.RemoteRef = Ref.Text().TrimEnd();
    const auto NamesResult = Git({TEXT("remote")}); TArray<FString> Names; NamesResult.Text().ParseIntoArrayLines(Names);
    if (!Remote.Ok() || !Ref.Ok() || !NamesResult.Ok() || !Names.Contains(Out.Remote) || Out.Remote.StartsWith(TEXT("-")) || !Out.RemoteRef.StartsWith(TEXT("refs/heads/")) || !Git({TEXT("check-ref-format"), Out.RemoteRef}).Ok())
    { Out.Error = TEXT("Configure a branch upstream on a named Git remote before Fetch/Push/Pull."); return false; }
    const auto Url = Git({TEXT("remote"), TEXT("get-url"), TEXT("--all"), Out.Remote});
    const auto PushUrl = Git({TEXT("remote"), TEXT("get-url"), TEXT("--push"), TEXT("--all"), Out.Remote});
    TArray<FString> Urls, PushUrls; Url.Text().ParseIntoArrayLines(Urls); PushUrl.Text().ParseIntoArrayLines(PushUrls);
    if (!Url.Ok() || !PushUrl.Ok() || Urls.Num() != 1 || PushUrls.Num() != 1 || Urls[0] != PushUrls[0])
    { Out.Error = TEXT("This remote slice requires one shared fetch/push URL. Review split endpoints externally."); return false; }
    const auto Shallow = Git({TEXT("rev-parse"), TEXT("--is-shallow-repository")});
    if (!Shallow.Ok() || Shallow.Text().TrimEnd() != TEXT("false")) { Out.Error = TEXT("Complete the shallow repository externally before remote operations."); return false; }
    const auto Config = Git({TEXT("config"), TEXT("--null"), TEXT("--list")});
    if (!Config.Ok()) { Out.Error = Config.Error; return false; }
    uint8 Hash[20]; FSHA1::HashBuffer(Config.Out.GetData(), Config.Out.Num(), Hash);
    Out.Context = Out.Root + TEXT("|") + Out.Branch + TEXT("|") + Out.Remote + TEXT("|") + Out.RemoteRef + TEXT("|") + BytesToHex(Hash, 20);
    return true;
}
FRemoteSnapshot FRepository::Fetch()
{
    FScopeLock Guard(&Mutex); FRemoteSnapshot R;
    if (!RemoteContext(R)) return R;
    ON_SCOPE_EXIT { Git({TEXT("update-ref"), TEXT("-d"), FetchRef}); };
    // Explicit destination cannot overwrite a local branch through custom fetch mappings.
    const auto Result = Git({TEXT("fetch"), TEXT("--no-tags"), TEXT("--no-recurse-submodules"), TEXT("--no-write-fetch-head"), TEXT("--refmap="), TEXT("--"), R.Remote, TEXT("+") + R.RemoteRef + TEXT(":") + FetchRef});
    if (!Result.Ok()) { R.Error = TEXT("Fetch failed. Local files were not integrated. ") + Result.Error; return R; }
    const auto Target = Git({TEXT("rev-parse"), TEXT("--verify"), FetchRef + TEXT("^{commit}")});
    if (!Target.Ok()) { R.Error = Target.Error; return R; } R.RemoteHead = Target.Text().TrimEnd();
    FRemoteSnapshot After;
    if (!RemoteContext(After) || After.Context != R.Context || After.Head != R.Head) { R.Error = TEXT("Branch or remote changed during fetch. Fetch again."); return R; }
    const auto Counts = Git({TEXT("rev-list"), TEXT("--left-right"), TEXT("--count"), R.Head + TEXT("...") + R.RemoteHead});
    TArray<FString> Numbers; Counts.Text().TrimEnd().ParseIntoArray(Numbers, TEXT("\t"));
    if (!Counts.Ok() || Numbers.Num() != 2 || !Numbers[0].IsNumeric() || !Numbers[1].IsNumeric()) { R.Error = TEXT("Cannot determine incoming/outgoing commits."); return R; }
    R.Ahead = FCString::Atoi(*Numbers[0]); R.Behind = FCString::Atoi(*Numbers[1]);
    const auto Diff = Git({TEXT("diff"), TEXT("--raw"), TEXT("--no-abbrev"), TEXT("--no-renames"), TEXT("-z"), R.Head, R.RemoteHead, TEXT("--")});
    if (!Diff.Ok()) { R.Error = Diff.Error; return R; }
    if (!ParseIncomingChanges(Diff.Out, R.IncomingChanges, R.Error)) return R;
    R.bValid = true; R.FetchedAt = FDateTime::UtcNow(); R.FetchedSeconds = FPlatformTime::Seconds(); return R;
}
bool FRepository::ValidateRemoteReview(const FRemoteSnapshot& Reviewed, FSnapshot& Current, FString& Error)
{
    if (!Reviewed.IsFresh()) { Error = TEXT("Fetch and review the remote state first (review expires after five minutes)."); return false; }
    FRemoteSnapshot Context;
    if (!RemoteContext(Context)) { Error = Context.Error; return false; }
    if (Context.Context != Reviewed.Context || Context.Head != Reviewed.Head) { Error = TEXT("HEAD, branch or remote configuration changed. Fetch and review again."); return false; }
    Current = RefreshInternal();
    if (Current.Head != Reviewed.Head || Current.Branch != Reviewed.Branch) { Error = TEXT("Branch changed during remote checks. Fetch again."); return false; }
    if (!Current.bValid || Current.HasConflicts() || Current.bOperationInProgress) { Error = TEXT("Resolve the in-progress repository operation first."); return false; }
    const auto Live = Git({TEXT("ls-remote"), TEXT("--exit-code"), TEXT("--refs"), TEXT("--"), Reviewed.Remote, Reviewed.RemoteRef});
    if (!Live.Ok() || Live.Text().TrimEnd() != Reviewed.RemoteHead + TEXT("\t") + Reviewed.RemoteRef)
    { Error = TEXT("Remote branch changed or cannot be checked. Fetch and review again."); return false; }
    return true;
}
FResult FRepository::Push(const FRemoteSnapshot& Reviewed)
{
    FScopeLock Guard(&Mutex); FSnapshot Current; FString Error;
    if (!ValidateRemoteReview(Reviewed, Current, Error)) return RemoteFailure(Error);
    if (!Reviewed.Ahead || Reviewed.Behind || !Git({TEXT("merge-base"), TEXT("--is-ancestor"), Reviewed.RemoteHead, Reviewed.Head}).Ok())
        return RemoteFailure(TEXT("Push requires outgoing commits without divergence. No rebase, merge or force-push is performed."));
    const auto Commits = Git({TEXT("rev-list"), Reviewed.RemoteHead + TEXT("..") + Reviewed.Head});
    if (!Commits.Ok()) return RemoteFailure(TEXT("Cannot inspect outgoing history."));
    TArray<FString> Revisions; Commits.Text().ParseIntoArrayLines(Revisions);
    TArray<FString> Lockable;
    for (const auto& Revision : Revisions)
    {
        TArray<FString> Paths;
        if (!RemoteRecords(Git({TEXT("diff-tree"), TEXT("--root"), TEXT("--no-commit-id"), TEXT("--name-only"), TEXT("--no-renames"), TEXT("-r"), TEXT("-m"), TEXT("-z"), Revision, TEXT("--")}), Paths))
            return RemoteFailure(TEXT("Cannot inspect outgoing commit paths."));
        // Include parent attributes: a deletion can remove both the asset and
        // its lockable rule in the same commit.
        const auto Parents = Git({TEXT("rev-list"), TEXT("--parents"), TEXT("-n"), TEXT("1"), Revision});
        if (!Parents.Ok()) return RemoteFailure(TEXT("Cannot inspect outgoing parent attributes."));
        TArray<FString> AttributeSources; Parents.Text().TrimEnd().ParseIntoArray(AttributeSources, TEXT(" "));
        AttributeSources.AddUnique(Reviewed.RemoteHead);
        for (const auto& Path : Paths) for (const auto& Commit : AttributeSources)
        {
            TArray<FString> Attrs;
            if (!RemoteRecords(Git({TEXT("check-attr"), TEXT("--source=") + Commit, TEXT("-z"), TEXT("lockable"), TEXT("--"), Path}), Attrs) || Attrs.Num() != 3)
                return RemoteFailure(TEXT("Cannot verify outgoing lock requirements. Git with check-attr --source support is required."));
            if (Attrs[2] == TEXT("set")) Lockable.AddUnique(Path);
        }
    }
    auto CheckLocks = [&]() -> FString
    {
        if (!Lockable.Num()) return FString();
        const auto Locks = VerifyLocksInternal(Reviewed.Remote);
        if (!Locks.IsFresh()) return TEXT("Push requires verified asset locks. ") + Locks.Error;
        for (const auto& Path : Lockable) if (Locks.State(Path, true) != ELockState::Ours)
            return TEXT("Own the lock before pushing this asset: ") + Path;
        return FString();
    };
    Error = CheckLocks(); if (!Error.IsEmpty()) return RemoteFailure(Error);
    // Explicit LFS upload prevents a missing pre-push hook from publishing pointers
    // whose objects were never uploaded. Normal Git pre-push hooks still run below.
    for (const auto& File : Current.Files) if (File.Path == TEXT(".lfsconfig") || File.OriginalPath == TEXT(".lfsconfig"))
        return RemoteFailure(TEXT("Commit or restore .lfsconfig before Push so object uploads use the reviewed configuration."));
    const FString SkipPush = FPlatformMisc::GetEnvironmentVariable(TEXT("GIT_LFS_SKIP_PUSH"));
    if (!SkipPush.IsEmpty() && SkipPush != TEXT("0") && !SkipPush.Equals(TEXT("false"), ESearchCase::IgnoreCase))
        return RemoteFailure(TEXT("GIT_LFS_SKIP_PUSH is set. Remove it before publishing from Git Workspace."));
    auto Uploaded = Git({TEXT("-c"), TEXT("lfs.allowincompletepush=false"), TEXT("lfs"), TEXT("push"), Reviewed.Remote, Reviewed.Head});
    if (!Uploaded.Ok()) return RemoteFailure(TEXT("LFS upload failed; Git push was not sent. ") + Uploaded.Error);
    if (!ValidateRemoteReview(Reviewed, Current, Error)) return RemoteFailure(Error);
    Error = CheckLocks(); if (!Error.IsEmpty()) return RemoteFailure(Error);
    const auto Result = Git({TEXT("-c"), TEXT("remote.") + Reviewed.Remote + TEXT(".mirror=false"), TEXT("push"), TEXT("--porcelain"), TEXT("--no-force"), TEXT("--no-follow-tags"), TEXT("--recurse-submodules=check"), TEXT("--"), Reviewed.Remote, Reviewed.Head + TEXT(":") + Reviewed.RemoteRef});
    if (!Result.Ok()) return RemoteFailure(TEXT("Push failed or is uncertain. Fetch before retrying; locks are retained. ") + Result.Error);
    const auto Live = Git({TEXT("ls-remote"), TEXT("--exit-code"), TEXT("--refs"), TEXT("--"), Reviewed.Remote, Reviewed.RemoteRef});
    if (!Live.Ok() || Live.Text().TrimEnd() != Reviewed.Head + TEXT("\t") + Reviewed.RemoteRef)
        return RemoteFailure(TEXT("Push was sent but its remote result could not be confirmed. Fetch and inspect; locks are retained."));
    return Result;
}
FResult FRepository::Pull(const FRemoteSnapshot& Reviewed)
{
    FScopeLock Guard(&Mutex); FSnapshot Current; FString Error;
    if (!ValidateRemoteReview(Reviewed, Current, Error)) return RemoteFailure(Error);
    if (!Current.Files.IsEmpty()) return RemoteFailure(TEXT("Pull requires a clean index and working tree, including untracked files. No automatic stash or discard."));
    if (Reviewed.Ahead || !Reviewed.Behind || !Git({TEXT("merge-base"), TEXT("--is-ancestor"), Reviewed.Head, Reviewed.RemoteHead}).Ok())
        return RemoteFailure(TEXT("Only a fast-forward Pull is supported. Reconcile divergence externally."));
    TArray<FIncomingChange> Changes;
    const auto Diff = Git({TEXT("diff"), TEXT("--raw"), TEXT("--no-abbrev"), TEXT("--no-renames"), TEXT("-z"), Reviewed.Head, Reviewed.RemoteHead, TEXT("--")});
    if (!Diff.Ok() || !ParseIncomingChanges(Diff.Out, Changes, Error)) return RemoteFailure(TEXT("Cannot inspect incoming paths. ") + Error);
    for (const auto& Change : Changes)
    {
        if (Change.Kind != EPullPathKind::Documentation)
            return RemoteFailure(TEXT("Pull requires editor reload/restart coordination. Review incoming changes and integrate with the editor closed: ") + Change.Path);
    }
    // Recheck after remote inspection, then integrate the exact reviewed commit.
    Current = RefreshInternal(); FRemoteSnapshot Context;
    if (!Current.bValid || !Current.Files.IsEmpty() || Current.Head != Reviewed.Head || Current.bOperationInProgress || !RemoteContext(Context) || Context.Context != Reviewed.Context)
        return RemoteFailure(TEXT("Local state changed during Pull checks. Review again."));
    auto Result = Git({TEXT("merge"), TEXT("--ff-only"), TEXT("--no-autostash"), TEXT("--no-edit"), TEXT("--no-stat"), Reviewed.RemoteHead});
    if (!Result.Ok()) return RemoteFailure(TEXT("Pull did not complete reliably. Inspect local state before retrying. ") + Result.Error);
    const auto After = RefreshInternal();
    if (!After.bValid || After.Head != Reviewed.RemoteHead || !After.Files.IsEmpty())
        return RemoteFailure(TEXT("Pull ran, but resulting state differs from the reviewed update. Inspect changes and hooks before continuing."));
    return Result;
}
}
