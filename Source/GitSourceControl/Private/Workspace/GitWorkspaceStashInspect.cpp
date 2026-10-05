// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#include "GitWorkspaceRepository.h"
#include "Misc/ScopeLock.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "Serialization/JsonSerializer.h"
#if PLATFORM_MAC
#include "GitWorkspaceSession.h"
#include <fcntl.h>
#include <unistd.h>
#endif
namespace GitWorkspace
{
namespace
{
FResult StashInspectFailure(const FString& Error) { FResult R; R.Error = Error; return R; }
FString StashDisplay(FString Value) { return Value.Replace(TEXT("\\"), TEXT("\\\\")).Replace(TEXT("\r"), TEXT("\\r")).Replace(TEXT("\n"), TEXT("\\n")).Replace(TEXT("\t"), TEXT("\\t")); }
bool SameStash(const FStashEntry& A, const FStashEntry& B) { return A.Oid == B.Oid && A.Label == B.Label; }
}
bool FStashInspection::IsFresh() const
{
    const double Age = FPlatformTime::Seconds() - ReviewedSeconds;
    return bValid && Error.IsEmpty() && Age >= 0 && Age < 300;
}
FString FRepository::StashDropRecovery(FString* ReportPath) const
{
    // refs/stash is shared by linked worktrees, so its recovery state is too.
    const auto Common = Git({TEXT("rev-parse"), TEXT("--path-format=absolute"), TEXT("--git-common-dir")});
    const FString Directory = Common.Text().TrimEnd();
    if (!Common.Ok() || Directory.IsEmpty() || FPaths::IsRelative(Directory)) return TEXT("Cannot locate shared stash recovery storage.");
    const FString Report = FPaths::Combine(Directory, TEXT("uegit/stash-drop/pending.json"));
    if (ReportPath) *ReportPath = Report;
    return IFileManager::Get().FileExists(*Report) ? TEXT("A stash-list operation needs recovery. Inspect the preserved references and report before modifying stashes or releasing locks: ") + Report : FString();
}
FResult FRepository::ReadStashPaths(const FStashEntry& Entry, TArray<FString>& Paths, FString* Details) const
{
    Paths.Empty(); FString Text; TSet<FString> Seen;
    const auto Parents = Git({TEXT("rev-list"), TEXT("--parents"), TEXT("-n"), TEXT("1"), Entry.Oid, TEXT("--")});
    TArray<FString> Parts; Parents.Text().TrimEnd().ParseIntoArray(Parts, TEXT(" "));
    if (!Parents.Ok() || (Parts.Num() != 3 && Parts.Num() != 4) || Parts[0] != Entry.Oid) return StashInspectFailure(TEXT("Cannot inspect this stash's complete saved trees. Review it externally."));
    const auto IndexParents = Git({TEXT("rev-list"), TEXT("--parents"), TEXT("-n"), TEXT("1"), Parts[2], TEXT("--")});
    if (!IndexParents.Ok() || IndexParents.Text().TrimEnd() != Parts[2] + TEXT(" ") + Parts[1]) return StashInspectFailure(TEXT("The saved index is not a valid stash index commit."));
    if (Parts.Num() == 4)
    {
        const auto UntrackedParents = Git({TEXT("rev-list"), TEXT("--parents"), TEXT("-n"), TEXT("1"), Parts[3], TEXT("--")});
        if (!UntrackedParents.Ok() || UntrackedParents.Text().TrimEnd() != Parts[3]) return StashInspectFailure(TEXT("The saved untracked tree is not a valid stash root commit."));
    }
    if (Details) Text = TEXT("STASH INSPECTION\n\n") + StashDisplay(Entry.Selector) + TEXT("  ") + StashDisplay(Entry.Label) +
        TEXT("\nSnapshot: ") + Entry.Oid + TEXT("\nBase: ") + Parts[1] + TEXT("\n\n");
    const TCHAR* Headings[] = {TEXT("SAVED WORKING CHANGES"), TEXT("SAVED STAGING"), TEXT("SAVED UNTRACKED FILES")};
    const TArray<FString> Trees {Entry.Oid, Parts[2], Parts.Num() == 4 ? Parts[3] : FString()};
    for (int32 I = 0; I < Trees.Num(); ++I)
    {
        if (Trees[I].IsEmpty()) continue;
        TArray<FString> Args;
        if (I == 2) Args = {TEXT("diff-tree"), TEXT("--root"), TEXT("--no-commit-id"), TEXT("-r"), TEXT("--raw"), TEXT("--no-ext-diff"), TEXT("--no-textconv"), TEXT("--no-abbrev"), TEXT("--no-renames"), TEXT("-z"), Trees[I], TEXT("--")};
        else Args = {TEXT("diff"), TEXT("--raw"), TEXT("--no-ext-diff"), TEXT("--no-textconv"), TEXT("--no-abbrev"), TEXT("--no-renames"), TEXT("-z"), Parts[1], Trees[I], TEXT("--")};
        const auto Diff = Git(Args); TArray<FIncomingChange> Changes; FString Error;
        if (!Diff.Ok() || !ParseIncomingChanges(Diff.Out, Changes, Error)) return StashInspectFailure(TEXT("Cannot inspect the complete stash paths. ") + Error + Diff.Error);
        if (Details) Text += FString(Headings[I]) + TEXT("\n");
        if (Details && Changes.IsEmpty()) Text += TEXT("(none)\n");
        for (const auto& Change : Changes)
        {
            if (!Seen.Contains(Change.Path)) { Seen.Add(Change.Path); Paths.Add(Change.Path); }
            if (Details) Text += FString::Chr(Change.Status) + TEXT("  ") + StashDisplay(Change.Path) + TEXT("\n");
        }
        if (Details) Text += TEXT("\n");
    }
    if (Details) *Details = MoveTemp(Text);
    FResult Result; Result.Code = 0; return Result;
}
FStashInspection FRepository::InspectStash(const FString& Oid, const FString& Selector)
{ FScopeLock Guard(&Mutex); return InspectStashInternal(Oid, Selector); }
FStashInspection FRepository::InspectStashInternal(const FString& Oid, const FString& Selector)
{
    FStashInspection R;
    auto Fail = [&](const FString& Error) { R.Error = Error; return R; };
    const auto Local = RefreshInternal(); R.Root = Local.Root;
    if (!Local.bValid) return Fail(Local.Error);
    const auto List = ListStashesInternal();
    if (!List.bValid) return Fail(List.Error);
    int32 Found = 0;
    for (const auto& Entry : List.Entries) if (Entry.Oid == Oid && (Selector.IsEmpty() || Entry.Selector == Selector)) { R.Entry = Entry; ++Found; }
    if (Found != 1) return Fail(TEXT("Select one exact stash entry from the refreshed list; it may have moved or been removed."));
    R.ListFingerprint = List.Fingerprint;
    TArray<FString> Paths;
    const auto Read = ReadStashPaths(R.Entry, Paths, &R.Text);
    if (!Read.Ok()) return Fail(Read.Error);
    R.Text += TEXT("Apply and keep restores files and retains this stash. Apply and delete removes this entry only after a verified restore. Drop removes the entry without applying it. Locks stay held.\n");
    const auto After = ListStashesInternal();
    if (!After.bValid || After.Fingerprint != R.ListFingerprint) return Fail(TEXT("Stash list changed during inspection. Refresh and select it again."));
    R.DropBlocker = StashDropRecovery();
#if PLATFORM_MAC
    FString CanonicalRoot, GitDir;
    if (R.DropBlocker.IsEmpty() && (!GitWorkspaceSession::FindRepository(R.Root, CanonicalRoot, GitDir) || IFileManager::Get().FileExists(*GitWorkspaceSession::RecoveryFile(GitDir))))
        R.DropBlocker = TEXT("Resolve the active asset/repository recovery before dropping a stash.");
#else
    R.DropBlocker = TEXT("Guarded stash Drop is currently available on Mac only.");
#endif
    if (R.DropBlocker.IsEmpty() && (Local.bOperationInProgress || Local.HasConflicts())) R.DropBlocker = TEXT("Resolve the repository operation/conflicts before dropping a stash.");
    if (!R.DropBlocker.IsEmpty()) R.Text += TEXT("\nDROP BLOCKED\n") + R.DropBlocker + TEXT("\n");
    R.bValid = true; R.ReviewedSeconds = FPlatformTime::Seconds(); return R;
}
#if PLATFORM_MAC
FResult FRepository::DropStash(const FStashInspection& Reviewed, const GitWorkspaceSession::FLease& Lease)
{
    FScopeLock Guard(&Mutex); FString Error;
    if (!Reviewed.IsFresh() || !Reviewed.DropBlocker.IsEmpty()) return StashInspectFailure(TEXT("Inspect the selected stash again before dropping it."));
    if (!Lease.IsExclusiveFor(Reviewed.Root) || !GitWorkspaceSession::NoOtherEditors(0, Error)) return StashInspectFailure(TEXT("Exclusive editor access is required. ") + Error);
    const auto Current = InspectStashInternal(Reviewed.Entry.Oid, Reviewed.Entry.Selector);
    if (!Current.IsFresh() || !Current.DropBlocker.IsEmpty() || Current.Root != Reviewed.Root || Current.ListFingerprint != Reviewed.ListFingerprint || !SameStash(Current.Entry, Reviewed.Entry))
        return StashInspectFailure(TEXT("Stash list or recovery state changed. Nothing dropped. Inspect again. ") + Current.Error + Current.DropBlocker);
    const auto Symbolic = Git({TEXT("symbolic-ref"), TEXT("--quiet"), TEXT("refs/stash")});
    if (Symbolic.Code != 1) return StashInspectFailure(TEXT("A symbolic or unreadable stash reference requires external review."));
    const auto Before = ListStashesInternal();
    if (!Before.bValid || Before.Fingerprint != Reviewed.ListFingerprint || Before.Entries.Num() > 128) return StashInspectFailure(TEXT("Refresh the stash list; more than 128 entries require external cleanup."));
    const auto Local = RefreshInternal();
    FString Pending; Error = StashDropRecovery(&Pending); if (!Error.IsEmpty()) return StashInspectFailure(Error);
    const FString Batch = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString Folder = FPaths::GetPath(Pending), Report = FPaths::Combine(Folder, Batch + TEXT(".json"));
    if (!IFileManager::Get().MakeDirectory(*Folder, true)) return StashInspectFailure(TEXT("Cannot create stash recovery storage."));
    TArray<FString> Refs; TArray<TSharedPtr<FJsonValue>> Entries;
    for (int32 I = 0; I < Before.Entries.Num(); ++I)
    {
        const auto& Entry = Before.Entries[I];
        const FString Ref = TEXT("refs/uegit/stash-drop/") + Batch + TEXT("/") + FString::FromInt(I); Refs.Add(Ref);
        auto Item = MakeShared<FJsonObject>(); Item->SetStringField(TEXT("selector"), Entry.Selector); Item->SetStringField(TEXT("oid"), Entry.Oid);
        Item->SetStringField(TEXT("label"), Entry.Label); Item->SetStringField(TEXT("recoveryRef"), Ref); Entries.Add(MakeShared<FJsonValueObject>(Item));
    }
    auto Record = MakeShared<FJsonObject>(); Record->SetStringField(TEXT("operation"), TEXT("drop-stash")); Record->SetStringField(TEXT("repository"), Root);
    Record->SetStringField(TEXT("selectedOid"), Reviewed.Entry.Oid); Record->SetStringField(TEXT("selectedEntry"), Reviewed.Entry.Selector);
    Record->SetStringField(TEXT("report"), Report); Record->SetArrayField(TEXT("before"), Entries);
    Record->SetStringField(TEXT("recovery"), TEXT("Inspect git stash list and the recovery references. To restore a saved snapshot, run git stash store -m recovered <recoveryRef>. No automatic rollback. Remove pending.json only after resolving the stash list. Successful drops retain local Git recovery references; these do not back up LFS payloads or publish anything."));
    FString Json; FJsonSerializer::Serialize(Record, TJsonWriterFactory<>::Create(&Json));
    // Shared worktree recovery marker: O_EXCL refuses another in-flight Drop.
    const int Fd = open(TCHAR_TO_UTF8(*Pending), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (Fd < 0) return StashInspectFailure(TEXT("Cannot exclusively persist stash-drop recovery state: ") + Pending);
    FTCHARToUTF8 Bytes(*Json); int32 Offset = 0;
    while (Offset < Bytes.Length()) { const auto N = write(Fd, Bytes.Get() + Offset, Bytes.Length() - Offset); if (N <= 0) break; Offset += N; }
    const bool bSaved = Offset == Bytes.Length() && fsync(Fd) == 0; close(Fd);
    if (!bSaved) return StashInspectFailure(TEXT("Stash list untouched. Could not persist recovery state; inspect: ") + Pending);
    auto Uncertain = [&](const FString& Why) { return StashInspectFailure(Why + TEXT("\nStash-list recovery is required; no retry or rollback was attempted. Report: ") + Pending); };
    // Retain every reviewed entry before changing the ordinal-based reflog.
    // An external Git client is outside the editor lease: revalidate afterward
    // and preserve all references on mismatch instead of claiming success.
    for (int32 I = 0; I < Refs.Num(); ++I)
    {
        const auto Saved = Git({TEXT("update-ref"), TEXT("--no-deref"), Refs[I], Before.Entries[I].Oid, FString::ChrN(Before.Entries[I].Oid.Len(), '0')});
        if (!Saved.Ok()) return Uncertain(TEXT("Could not preserve every stash before Drop. ") + Saved.Error);
    }
    const auto Rechecked = ListStashesInternal();
    if (!Rechecked.bValid || Rechecked.Fingerprint != Before.Fingerprint) return Uncertain(TEXT("Another Git client changed the stash list before Drop. Nothing dropped by Git Workspace."));
    const auto Dropped = Git({TEXT("stash"), TEXT("drop"), TEXT("--"), Reviewed.Entry.Selector});
    const auto After = ListStashesInternal();
    TArray<FStashEntry> Expected;
    for (const auto& Entry : Before.Entries) if (Entry.Selector != Reviewed.Entry.Selector) Expected.Add(Entry);
    bool bExact = Dropped.Ok() && After.bValid && After.Entries.Num() == Expected.Num();
    for (int32 I = 0; bExact && I < Expected.Num(); ++I) bExact &= SameStash(Expected[I], After.Entries[I]);
    if (!bExact) return Uncertain(TEXT("Drop did not produce exactly the reviewed stash list. ") + Dropped.Error);
    const auto Now = RefreshInternal();
    if (!Now.bValid || Now.Head != Local.Head || Now.Branch != Local.Branch || Now.IndexEntries != Local.IndexEntries)
        return Uncertain(TEXT("Repository state changed during Drop; preserve and inspect the external changes."));
    Record->SetStringField(TEXT("result"), TEXT("Only the selected stash entry was removed. Local Git recovery references are retained. No working-file or lock mutation requested."));
    Json.Empty(); FJsonSerializer::Serialize(Record, TJsonWriterFactory<>::Create(&Json));
    if (!FFileHelper::SaveStringToFile(Json, *Report, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM) || !IFileManager::Get().Delete(*Pending))
        return Uncertain(TEXT("Drop verified, but its recovery report could not be finalized."));
    FResult Result; Result.Code = 0; FTCHARToUTF8 Message(*(TEXT("Stash dropped from the list. Working files, staging and locks retained. Local recovery report: ") + Report));
    Result.Out.Append(reinterpret_cast<const uint8*>(Message.Get()), Message.Length()); return Result;
}
#endif
}
