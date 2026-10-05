// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#include "GitWorkspaceRepository.h"
#include "Misc/ScopeLock.h"
#include "Misc/ScopeExit.h"
#include "Misc/SecureHash.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#if PLATFORM_MAC
#include "GitWorkspaceSession.h"
#endif
namespace GitWorkspace
{
namespace
{
FResult StashFailure(const FString& Error) { FResult R; R.Error = Error; return R; }
bool ObjectId(const FString& Id)
{
    if (Id.Len() != 40 && Id.Len() != 64) return false;
    for (TCHAR C : Id) if (!FChar::IsHexDigit(C)) return false;
    return true;
}
FString Digest(const TArray<uint8>& Bytes)
{
    uint8 Hash[20]; FSHA1::HashBuffer(Bytes.GetData(), Bytes.Num(), Hash); return BytesToHex(Hash, 20);
}
FString Display(FString S) { return S.Replace(TEXT("\\"), TEXT("\\\\")).Replace(TEXT("\n"), TEXT("\\n")).Replace(TEXT("\r"), TEXT("\\r")).Replace(TEXT("\t"), TEXT("\\t")); }
}
bool FStashReview::IsFresh() const { const double Age = FPlatformTime::Seconds() - ReviewedSeconds; return bValid && Error.IsEmpty() && Age >= 0 && Age < 300; }
FStashList FRepository::ListStashes()
{
    FScopeLock Guard(&Mutex); return ListStashesInternal();
}
FStashList FRepository::ListStashesInternal()
{
    FStashList List;
    const auto Local = RefreshInternal(); if (!Local.bValid) { List.Error = Local.Error; return List; }
    const auto Ref = Git({TEXT("show-ref"), TEXT("--verify"), TEXT("--quiet"), TEXT("refs/stash")});
    if (Ref.Code == 1) { List.bValid = true; return List; }
    if (!Ref.Ok()) { List.Error = Ref.Error; return List; }
    const auto Log = Git({TEXT("log"), TEXT("-g"), TEXT("-z"), TEXT("--date=raw"), TEXT("--format=%H%x00%gd%x00%gs%x00%gD"), TEXT("refs/stash"), TEXT("--")});
    TArray<FString> Fields; int32 Start = 0;
    for (int32 I = 0; I < Log.Out.Num(); ++I) if (!Log.Out[I])
    {
        FUTF8ToTCHAR Value(reinterpret_cast<const ANSICHAR*>(Log.Out.GetData() + Start), I - Start);
        Fields.Emplace(Value.Length(), Value.Get()); Start = I + 1;
    }
    if (!Log.Ok() || Start != Log.Out.Num() || Fields.Num() % 4) { List.Error = TEXT("Cannot read the complete stash list. ") + Log.Error; return List; }
    for (int32 I = 0; I < Fields.Num(); I += 4)
    {
        if (!ObjectId(Fields[I])) { List.Error = TEXT("Malformed stash identity."); return List; }
        List.Entries.Add({Fields[I], FString::Printf(TEXT("stash@{%d}"), I / 4), Fields[I + 2]});
    }
    List.Fingerprint = Digest(Log.Out); List.bValid = true; return List;
}
FStashReview FRepository::ReviewStash(const FString& Oid, bool bRestoreIndex)
{ FScopeLock Guard(&Mutex); return ReviewStashInternal(Oid, bRestoreIndex); }
FStashReview FRepository::ReviewStashInternal(const FString& Oid, bool bRestoreIndex)
{
    FStashReview R; R.bCreate = Oid.IsEmpty(); R.bRestoreIndex = bRestoreIndex; R.Local = RefreshInternal();
    auto Fail = [&](const FString& Error) { R.Error = Error; return R; };
    if (!R.Local.bValid || R.Local.bUnborn || R.Local.bOperationInProgress || R.Local.HasConflicts()) return Fail(TEXT("Stashes require an existing commit and no conflicts or repository operation. ") + R.Local.Error);
    const FString DropRecovery = StashDropRecovery(); if (!DropRecovery.IsEmpty()) return Fail(DropRecovery);
    const auto Config = Git({TEXT("config"), TEXT("--null"), TEXT("--list")});
    if (!Config.Ok()) return Fail(Config.Error);
    R.ConfigurationHash = Digest(Config.Out);
    R.Fingerprint = R.Local.Root + R.Local.Branch + R.Local.Head + Digest(R.Local.IndexEntries) + R.ConfigurationHash;
    for (const auto& File : R.Local.Files)
    {
        if (File.bSubmodule) return Fail(TEXT("Preserve submodule changes externally before stashing."));
        if (File.bUntracked) R.Fingerprint += TEXT("|") + File.Path;
    }
    if (R.bCreate)
    {
        // Creates immutable Git objects only: does not update refs, index or
        // working files. Storing this exact object precedes any cleanup.
        const auto Captured = Git({TEXT("stash"), TEXT("create")});
        if (!Captured.Ok()) return Fail(Captured.Error);
        R.Oid = Captured.Text().TrimEnd();
        if (R.Oid.IsEmpty()) return Fail(TEXT("No tracked changes to stash. Untracked and ignored files are left in place."));
    }
    else
    {
        if (!ObjectId(Oid)) return Fail(TEXT("Select a stash by its full object identity."));
        R.Oid = Oid;
        const auto Entries = Git({TEXT("log"), TEXT("-g"), TEXT("--format=%H"), TEXT("refs/stash"), TEXT("--")});
        TArray<FString> Ids; Entries.Text().ParseIntoArrayLines(Ids);
        if (!Entries.Ok() || !Ids.Contains(Oid)) return Fail(TEXT("That stash is no longer in the stash list. Refresh before applying."));
        if (!R.Local.Files.IsEmpty()) return Fail(TEXT("Apply requires a clean index and working tree, including untracked files. Preserve local work first."));
    }
    const auto Parents = Git({TEXT("rev-list"), TEXT("--parents"), TEXT("-n"), TEXT("1"), R.Oid, TEXT("--")});
    TArray<FString> Parts; Parents.Text().TrimEnd().ParseIntoArray(Parts, TEXT(" "));
    if (!Parents.Ok() || Parts.Num() != 3 || Parts[0] != R.Oid) return Fail(TEXT("This slice supports tracked-file stashes with two parents. Stashes containing untracked files require external review."));
    R.Base = Parts[1]; R.IndexCommit = Parts[2];
    if (R.Base != R.Local.Head) return Fail(TEXT("Apply currently requires the stash's original base commit. Cross-commit reconciliation requires an external Git client."));
    const auto IndexParent = Git({TEXT("rev-list"), TEXT("--parents"), TEXT("-n"), TEXT("1"), R.IndexCommit, TEXT("--")});
    if (!IndexParent.Ok() || IndexParent.Text().TrimEnd() != R.IndexCommit + TEXT(" ") + R.Base) return Fail(TEXT("The selected object is not a supported Git stash."));
    auto ReadTree = [&](const FString& Commit, FString& Tree)
    { const auto V = Git({TEXT("rev-parse"), TEXT("--verify"), Commit + TEXT("^{tree}")}); Tree = V.Text().TrimEnd(); return V.Ok() && ObjectId(Tree); };
    if (!ReadTree(R.Oid, R.WorkingTree) || !ReadTree(R.IndexCommit, R.IndexTree)) return Fail(TEXT("Cannot inspect stash trees."));
    R.Fingerprint += R.WorkingTree + R.IndexTree;
    TMap<FString, FIncomingChange> Combined;
    for (const FString& Target : {R.Oid, R.IndexCommit})
    {
        const auto Diff = Git({TEXT("diff"), TEXT("--raw"), TEXT("--no-abbrev"), TEXT("--no-renames"), TEXT("-z"), R.Base, Target, TEXT("--")});
        TArray<FIncomingChange> Changes; FString Error;
        if (!Diff.Ok() || !ParseIncomingChanges(Diff.Out, Changes, Error)) return Fail(TEXT("Cannot inspect all stashed paths. ") + Error);
        for (auto Change : Changes)
        {
            if (!R.bCreate && !bRestoreIndex && Change.Status != 'M') return Fail(TEXT("Apply without restoring staging currently supports modifications only. Enable Restore staging for additions/deletions."));
            // Both the saved index and work tree must be safe, even if one
            // intermediate staged edit was subsequently undone in the work tree.
            if (Change.Kind != EPullPathKind::Documentation && !(Change.Kind == EPullPathKind::Package && Change.Path.StartsWith(TEXT("Content/"))))
                return Fail(TEXT("Source, configuration, plugins, symlinks and executable changes require external stashing: ") + Display(Change.Path));
            if (R.bCreate) { Swap(Change.OldMode, Change.NewMode); if (Change.Status == 'A') Change.Status = 'D'; else if (Change.Status == 'D') Change.Status = 'A'; }
            if (Change.Kind == EPullPathKind::Package && (Change.Status != 'M' || !Change.Path.EndsWith(TEXT(".uasset")) ||
                Change.Path.Contains(TEXT("/__External")) || Change.Path.EndsWith(TEXT("_BuiltData.uasset"))))
                return Fail(TEXT("Maps, external packages and asset additions/deletions require external stashing in this slice: ") + Display(Change.Path));
            if (!Combined.Contains(Change.Path)) Combined.Add(Change.Path, Change);
        }
    }
    for (const auto& File : R.Local.Files)
        if (File.bUntracked && Combined.Contains(File.Path)) return Fail(TEXT("An untracked file occupies a tracked stash path. Preserve it externally first: ") + Display(File.Path));
    Combined.GenerateValueArray(R.Changes); R.Changes.Sort([](const auto& A, const auto& B) { return A.Path < B.Path; });
    R.Text = R.bCreate ? TEXT("CREATE STASH — WHOLE REPOSITORY, TRACKED FILES\n") : TEXT("APPLY STASH — KEEP THE STASH\n");
    R.Text += TEXT("\nRepository: ") + Display(R.Local.Root) + TEXT("\nBase: ") + R.Base + TEXT("\nSnapshot: ") + R.Oid;
    R.Text += R.bCreate ? TEXT("\nSaved staged and unstaged versions are captured separately. Untracked and ignored files stay in place. Filters in Changes do not limit this operation.\n") :
        (R.bRestoreIndex ? TEXT("\nRestore the saved staging state.\n") : TEXT("\nApply modifications without restoring staging.\n"));
    R.Text += TEXT("Locks remain held. No save, commit, push or automatic stash deletion.\n\nAFFECTED TRACKED PATHS\n");
    for (const auto& Change : R.Changes) R.Text += FString::Chr(Change.Status) + TEXT("  ") + Display(Change.Path) + TEXT("\n");
    const auto After = RefreshInternal();
    if (!After.bValid || After.Head != R.Local.Head || After.Branch != R.Local.Branch || After.IndexEntries != R.Local.IndexEntries) return Fail(TEXT("Repository changed during review; refresh stashes."));
    R.bValid = true; R.ReviewedSeconds = FPlatformTime::Seconds(); return R;
}
FResult FRepository::CheckLocalLfs(const FString& Commit) const
{
    const FString Hooks = FPaths::Combine(FPlatformProcess::UserTempDir(), TEXT("uegit-stash-lfs-") + FGuid::NewGuid().ToString(EGuidFormats::Digits));
    if (!IFileManager::Get().MakeDirectory(*Hooks, true)) return StashFailure(TEXT("Cannot create LFS verification directory."));
    ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*Hooks, false, true); };
    const auto R = Git({TEXT("-c"), TEXT("core.hooksPath=") + Hooks, TEXT("-c"), TEXT("lfs.fetchinclude="), TEXT("-c"), TEXT("lfs.fetchexclude="),
        TEXT("lfs"), TEXT("fsck"), TEXT("--objects"), TEXT("--pointers"), TEXT("--dry-run"), Commit});
    return R.Ok() ? R : StashFailure(TEXT("Required local LFS objects are unavailable or corrupt. Resolve this externally before stashing.\n") + R.Text() + R.Error);
}
#if PLATFORM_MAC
FResult FRepository::HydratedGit(const TArray<FString>& Args) const
{
    // Per-process overrides avoid pointer-only working files when users normally
    // skip smudge. In particular, a stash work tree may differ from its index;
    // a later 'lfs checkout' would hydrate the wrong version or skip the file.
    TArray<FString> All {TEXT("GIT_LFS_SKIP_SMUDGE=0"), TEXT("GIT_LFS_SKIP_DOWNLOAD_ERRORS=0"), GitBinary,
        TEXT("--no-optional-locks"), TEXT("--literal-pathspecs"), TEXT("-c"), TEXT("color.ui=false"),
        TEXT("-c"), TEXT("filter.lfs.process=git-lfs filter-process"), TEXT("-c"), TEXT("filter.lfs.smudge=git-lfs smudge -- %f"),
        TEXT("-c"), TEXT("lfs.fetchinclude="), TEXT("-c"), TEXT("lfs.fetchexclude=")};
    All.Append(Args); return Run(TEXT("/usr/bin/env"), Root, All);
}
FResult FRepository::ExecuteStash(const FStashReview& Reviewed, const FString& Name, const GitWorkspaceSession::FLease& Lease)
{
    FScopeLock Guard(&Mutex); FString Error;
    if (!Reviewed.IsFresh()) return StashFailure(TEXT("Stash review expired; review again."));
    if (!Lease.IsExclusiveFor(Reviewed.Local.Root) || !GitWorkspaceSession::NoOtherEditors(0, Error)) return StashFailure(TEXT("Exclusive editor access is required. ") + Error);
    const auto Current = ReviewStashInternal(Reviewed.bCreate ? FString() : Reviewed.Oid, Reviewed.bRestoreIndex);
    if (!Current.bValid || Current.Fingerprint != Reviewed.Fingerprint) return StashFailure(TEXT("Stash or local work changed after review. ") + Current.Error);
    if (Current.Changes.Num() != Reviewed.Changes.Num()) return StashFailure(TEXT("Stash paths differ from the package review."));
    for (int32 I = 0; I < Current.Changes.Num(); ++I)
    {
        const auto& A = Current.Changes[I]; const auto& B = Reviewed.Changes[I];
        if (A.Path != B.Path || A.Status != B.Status || A.Kind != B.Kind || A.OldMode != B.OldMode || A.NewMode != B.NewMode)
            return StashFailure(TEXT("Stash paths differ from the package review."));
    }
    if (Reviewed.Base != Current.Base || Reviewed.WorkingTree != Current.WorkingTree || Reviewed.IndexTree != Current.IndexTree || Reviewed.ConfigurationHash != Current.ConfigurationHash)
        return StashFailure(TEXT("Stash metadata differs from the reviewed snapshot."));
    if (!ObjectId(Reviewed.Oid) || !ObjectId(Reviewed.IndexCommit)) return StashFailure(TEXT("Invalid stash snapshot identity."));
    const auto Tree = Git({TEXT("rev-parse"), TEXT("--verify"), Reviewed.Oid + TEXT("^{tree}")});
    const auto Base = Git({TEXT("rev-parse"), TEXT("--verify"), Reviewed.Oid + TEXT("^1")});
    const auto IndexCommit = Git({TEXT("rev-parse"), TEXT("--verify"), Reviewed.Oid + TEXT("^2")});
    const auto IndexTree = Git({TEXT("rev-parse"), TEXT("--verify"), Reviewed.Oid + TEXT("^2^{tree}")});
    if (!IndexCommit.Ok() || IndexCommit.Text().TrimEnd() != Reviewed.IndexCommit || !Tree.Ok() || !Base.Ok() || !IndexTree.Ok() || Tree.Text().TrimEnd() != Current.WorkingTree || Base.Text().TrimEnd() != Current.Base || IndexTree.Text().TrimEnd() != Current.IndexTree)
        return StashFailure(TEXT("The captured stash object no longer matches the reviewed work."));
    if (Reviewed.bCreate && (Name.TrimStartAndEnd().IsEmpty() || Name.Len() > 256 || Name.Contains(TEXT("\n")) || Name.Contains(TEXT("\r")))) return StashFailure(TEXT("Enter a stash name of 1–256 characters on one line."));
    FString CanonicalRoot, GitDir;
    if (!GitWorkspaceSession::FindRepository(Root, CanonicalRoot, GitDir)) return StashFailure(TEXT("Cannot locate stash recovery storage."));
    const FString Recovery = GitWorkspaceSession::RecoveryFile(GitDir);
    if (IFileManager::Get().FileExists(*Recovery)) return StashFailure(TEXT("Resolve the existing recovery report before stashing: ") + Recovery);
    for (const auto& Commit : {Reviewed.Base, Reviewed.Oid, Reviewed.IndexCommit})
    { const auto Check = CheckLocalLfs(Commit); if (!Check.Ok()) return Check; }
    // Apply never overwrites ignored files that ordinary status omitted.
    if (!Reviewed.bCreate) for (const auto& Change : Current.Changes)
        if (Change.Status == 'A' && IFileManager::Get().FileExists(*FPaths::Combine(Root, Change.Path))) return StashFailure(TEXT("An ignored/local file occupies a stashed path: ") + Change.Path);
    const auto Rechecked = ReviewStashInternal(Reviewed.bCreate ? FString() : Reviewed.Oid, Reviewed.bRestoreIndex);
    if (!Rechecked.bValid || Rechecked.Fingerprint != Reviewed.Fingerprint) return StashFailure(TEXT("Local work changed during LFS verification; review again."));
    IFileManager::Get().MakeDirectory(*FPaths::GetPath(Recovery), true);
    const FString Marker = TEXT("Git Workspace stash operation requires recovery.\nKeep the editor closed. No automatic rollback.\nRepository: ") + Root +
        TEXT("\nOriginal HEAD: ") + Reviewed.Local.Head + TEXT("\nPreserved stash object: ") + Reviewed.Oid +
        TEXT("\nOperation: ") + (Reviewed.bCreate ? TEXT("create") : TEXT("apply")) + TEXT("\nInspect status, index and working files. The stash is never automatically dropped. Preserve unexpected work and verify LFS hydration before removing this marker.\n");
    if (!FFileHelper::SaveStringToFile(Marker, *Recovery, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return StashFailure(TEXT("Cannot persist stash recovery state."));
    FResult Changed;
    if (Reviewed.bCreate)
    {
        const auto Stored = Git({TEXT("stash"), TEXT("store"), TEXT("-m"), Name, Reviewed.Oid});
        if (!Stored.Ok()) return Stored;
        const FString PathFile = FPaths::Combine(FPlatformProcess::UserTempDir(), TEXT("uegit-stash-paths-") + FGuid::NewGuid().ToString(EGuidFormats::Digits));
        ON_SCOPE_EXIT { IFileManager::Get().Delete(*PathFile); };
        TArray<uint8> Bytes;
        for (const auto& Change : Current.Changes) { FTCHARToUTF8 Path(*Change.Path); Bytes.Append(reinterpret_cast<const uint8*>(Path.Get()), Path.Length()); Bytes.Add(0); }
        if (!FFileHelper::SaveArrayToFile(Bytes, *PathFile)) return StashFailure(TEXT("Stash preserved, but cleanup could not start."));
        Changed = HydratedGit({TEXT("restore"), TEXT("--source=HEAD"), TEXT("--staged"), TEXT("--worktree"), TEXT("--pathspec-from-file=") + PathFile, TEXT("--pathspec-file-nul")});
    }
    else
    {
        TArray<FString> Args {TEXT("stash"), TEXT("apply")}; if (Reviewed.bRestoreIndex) Args.Add(TEXT("--index")); Args.Add(Reviewed.Oid);
        Changed = HydratedGit(Args);
    }
    if (!Changed.Ok()) return StashFailure(TEXT("Stash operation may be partial. The stash and working files are retained for recovery.\n") + Changed.Error);
    const auto Hydrated = VerifyWorkingLfs(Reviewed.bCreate ? Reviewed.Base : Reviewed.Oid);
    if (!Hydrated.Ok()) return Hydrated;
    return VerifyStashResult(Reviewed); // Marker remains until package reload succeeds.
}
FResult FRepository::CompleteStash(const FStashReview& Reviewed, const GitWorkspaceSession::FLease& Lease)
{
    FScopeLock Guard(&Mutex);
    if (!Lease.IsExclusiveFor(Reviewed.Local.Root)) return StashFailure(TEXT("Stash completion lost editor exclusivity."));
    const auto Checked = VerifyStashResult(Reviewed); if (!Checked.Ok()) return Checked;
    FString CanonicalRoot, GitDir;
    if (!GitWorkspaceSession::FindRepository(Root, CanonicalRoot, GitDir) || !IFileManager::Get().Delete(*GitWorkspaceSession::RecoveryFile(GitDir))) return StashFailure(TEXT("Stash verified, but recovery state could not be cleared."));
    return Checked;
}
#endif
FResult FRepository::VerifyStashResult(const FStashReview& Reviewed)
{
    const auto After = RefreshInternal();
    if (!After.bValid || After.Head != Reviewed.Local.Head || After.Branch != Reviewed.Local.Branch || After.bOperationInProgress || After.HasConflicts()) return StashFailure(TEXT("Unexpected repository state after stashing."));
    const auto Config = Git({TEXT("config"), TEXT("--null"), TEXT("--list")});
    if (!Config.Ok() || Digest(Config.Out) != Reviewed.ConfigurationHash) return StashFailure(TEXT("Repository configuration changed during stashing."));
    if (!Reviewed.bCreate) for (const auto& File : After.Files)
        if (File.bUntracked) return StashFailure(TEXT("Unexpected untracked file after applying stash."));
    const auto Index = Git({TEXT("diff"), TEXT("--cached"), TEXT("--quiet"), Reviewed.bCreate ? Reviewed.Base : Reviewed.bRestoreIndex ? Reviewed.IndexCommit : Reviewed.Base, TEXT("--")});
    // Without --index Git stages newly added files. Restrict this initial mode
    // to modification-only stashes so a changed index cannot go unnoticed.
    if (!Index.Ok()) return StashFailure(TEXT("Staging differs from the expected stash result. Inspect recovery state."));
    if (Reviewed.bCreate)
    {
        TSet<FString> Expected;
        for (const auto& File : Reviewed.Local.Files) if (File.bUntracked) Expected.Add(File.Path);
        for (const auto& File : After.Files) if (!File.bUntracked || !Expected.Remove(File.Path)) return StashFailure(TEXT("Local files differ from the expected stash cleanup."));
        if (Expected.Num()) return StashFailure(TEXT("An untracked file disappeared while stashing."));
    }
    const auto Work = Git({TEXT("diff"), TEXT("--quiet"), TEXT("--no-ext-diff"), TEXT("--no-textconv"), Reviewed.bCreate ? Reviewed.Base : Reviewed.Oid, TEXT("--")});
    if (!Work.Ok()) return StashFailure(TEXT("Working files differ from the saved stash snapshot."));
    FResult Result; Result.Code = 0; return Result;
}
}
