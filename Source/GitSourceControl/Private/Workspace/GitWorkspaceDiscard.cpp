// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#include "GitWorkspaceRepository.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "Misc/SecureHash.h"
#include "Serialization/JsonSerializer.h"
#if PLATFORM_MAC
#include "GitWorkspaceSession.h"
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#endif
namespace GitWorkspace
{
namespace
{
FResult DiscardFailure(const FString& Error) { FResult R; R.Error = Error; return R; }
FResult DiscardMessage(const FString& Text)
{ FResult R; R.Code = 0; FTCHARToUTF8 Bytes(*Text); R.Out.Append(reinterpret_cast<const uint8*>(Bytes.Get()), Bytes.Length()); return R; }
FString DiscardDigest(const TArray<uint8>& Bytes)
{ uint8 Hash[20]; FSHA1::HashBuffer(Bytes.GetData(), Bytes.Num(), Hash); return BytesToHex(Hash, 20); }
FString DiscardDisplay(FString Path)
{ return Path.Replace(TEXT("\\"), TEXT("\\\\")).Replace(TEXT("\r"), TEXT("\\r")).Replace(TEXT("\n"), TEXT("\\n")).Replace(TEXT("\t"), TEXT("\\t")); }
FString DiscardPayload(int32 Index) { return FString::Printf(TEXT("payload/%04d.bin"), Index); }
FString DiscardBackupText(const FString& Folder)
{
    return TEXT("Recovery backup: ") + Folder + TEXT("\nRead manifest.json for exact original paths and numbered payload files. These are the original saved bytes, including LFS payloads. With the editor closed, copying a listed payload back to its original filename restores working edits only. Staging is separate. Keep this backup until it is no longer needed; no automatic cleanup.\n");
}
#if PLATFORM_MAC
bool DiscardStorage(const FDiscardReview& R, FString& Folder, FString& Marker, FString& Error)
{
    FGuid Id;
    if (R.Id.Len() != 32 || !FGuid::ParseExact(R.Id, EGuidFormats::Digits, Id)) { Error = TEXT("Invalid discard review identity."); return false; }
    FString Root, GitDir;
    if (!GitWorkspaceSession::FindRepository(R.Capture.Local.Root, Root, GitDir)) { Error = TEXT("Cannot locate discard recovery storage."); return false; }
    Folder = FPaths::Combine(GitDir, TEXT("uegit/discard"), R.Id);
    Marker = GitWorkspaceSession::RecoveryFile(GitDir); return true;
}
bool ReadDiscardMarker(const FDiscardReview& R, const FString& Marker)
{
    FString Text, Operation, Id, Fingerprint, Root; TSharedPtr<FJsonObject> Json;
    return FFileHelper::LoadFileToString(Text, *Marker) && FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Json) && Json &&
        Json->TryGetStringField(TEXT("operation"), Operation) && Operation == TEXT("discard-working") &&
        Json->TryGetStringField(TEXT("id"), Id) && Id == R.Id &&
        Json->TryGetStringField(TEXT("fingerprint"), Fingerprint) && Fingerprint == R.Fingerprint &&
        Json->TryGetStringField(TEXT("root"), Root) && Root == R.Capture.Local.Root;
}
bool SyncDiscardPath(const FString& Path, bool bDirectory = false)
{
    const int Fd = open(TCHAR_TO_UTF8(*Path), O_RDONLY | O_NOFOLLOW | (bDirectory ? O_DIRECTORY : 0));
    if (Fd < 0) return false;
    const bool bOk = fsync(Fd) == 0; const int Closed = close(Fd); return bOk && Closed == 0;
}
bool CreateDiscardMarker(const FString& File, const FString& Text)
{
    const FString Parent = FPaths::GetPath(File);
    if (!IFileManager::Get().MakeDirectory(*Parent, true) || !SyncDiscardPath(FPaths::GetPath(Parent), true)) return false;
    // Never overwrite another operation's marker, including one created by an
    // external process between preflight and this final write.
    const int Fd = open(TCHAR_TO_UTF8(*File), O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (Fd < 0) return false;
    FTCHARToUTF8 Bytes(*Text); int32 Written = 0; bool bOk = true;
    while (Written < Bytes.Length())
    {
        const auto Count = write(Fd, Bytes.Get() + Written, Bytes.Length() - Written);
        if (Count < 0 && errno == EINTR) continue;
        if (Count <= 0) { bOk = false; break; }
        Written += Count;
    }
    bOk = bOk && fsync(Fd) == 0; const int Closed = close(Fd);
    return bOk && Closed == 0 && SyncDiscardPath(Parent, true);
}
#endif
}
FDiscardReview FRepository::ReviewDiscard(const TArray<FString>& Paths)
{ FScopeLock Guard(&Mutex); return ReviewDiscardInternal(Paths); }
FDiscardReview FRepository::ReviewDiscardInternal(const TArray<FString>& Paths)
{
    FDiscardReview R;
    auto Fail = [&](const FString& Error) { R.Error = Error; R.Text = Error; return R; };
#if !PLATFORM_MAC
    return Fail(TEXT("Guarded discard with asset refresh is currently available on Mac only."));
#else
    if (Paths.IsEmpty() || Paths.Num() > 512) return Fail(TEXT("Select 1–512 tracked files with saved working edits."));
    const auto Local = RefreshInternal();
    if (!Local.bValid) return Fail(Local.Error);
    for (const auto& Path : Paths)
    {
        const auto* File = Local.Files.FindByPredicate([&](const auto& F) { return F.Path == Path; });
        if (R.Paths.Contains(Path) || !File || File->bUntracked || File->bConflict || File->bSubmodule || File->Working != 'M' || !File->OriginalPath.IsEmpty())
            return Fail(TEXT("Discard currently supports tracked working modifications only. Untracked files, deletions, renames and conflicts require external review: ") + DiscardDisplay(Path));
        R.Paths.Add(Path);
    }
    R.Paths.Sort();
    const auto Bytes = UntrackedFingerprint(R.Paths);
    if (!Bytes.Ok()) return Fail(Bytes.Error);
    R.RawHashes = Bytes.Out;
    R.Capture = ReviewStashInternal(FString(), true, true, R.Paths, false);
    if (!R.Capture.IsFresh()) return Fail(R.Capture.Error);
    if (R.Capture.SelectedPaths != R.Paths) return Fail(TEXT("The discard selection includes a rename or directory replacement. Review it externally."));
    FString Folder, Marker, Error;
    if (!DiscardStorage(R, Folder, Marker, Error)) return Fail(Error);
    if (IFileManager::Get().FileExists(*Marker)) return Fail(TEXT("Resolve active asset/repository recovery before discarding: ") + Marker);
    const auto Listed = ListStashesInternal(); if (!Listed.bValid) return Fail(Listed.Error);
    R.StashFingerprint = Listed.Fingerprint;
    for (const auto& File : R.Capture.Local.Files) if (File.bUntracked) R.UntrackedPaths.Add(File.Path);
    R.UntrackedPaths.Sort();
    if (R.UntrackedPaths.Num() > 512) return Fail(TEXT("Too many unrelated untracked files to verify safely. Preserve them externally first."));
    if (!R.UntrackedPaths.IsEmpty())
    {
        const auto Untracked = UntrackedFingerprint(R.UntrackedPaths);
        if (!Untracked.Ok()) return Fail(Untracked.Error);
        R.UntrackedHashes = Untracked.Out;
    }
    // The selected stash capture already retains both versions and the exact
    // unselected remainder. Reconstruct full trees in private indexes only.
    const auto Work = ProjectStashTree(R.Capture.ExpectedWorkingTree, R.Capture.WorkingTree, R.Paths);
    const auto Index = ProjectStashTree(R.Capture.ExpectedIndexTree, R.Capture.IndexTree, R.Paths);
    if (!Work.Ok() || !Index.Ok()) return Fail(Work.Error + Index.Error);
    R.BeforeWorkingTree = Work.Text().TrimEnd(); R.IndexTree = Index.Text().TrimEnd();
    const auto Expected = ProjectStashTree(R.BeforeWorkingTree, R.IndexTree, R.Paths);
    if (!Expected.Ok()) return Fail(Expected.Error);
    R.ExpectedWorkingTree = Expected.Text().TrimEnd();
    const auto BeforeCommit = MakeStashCommit(R.BeforeWorkingTree, {R.Capture.Local.Head});
    const auto ExpectedCommit = MakeStashCommit(R.ExpectedWorkingTree, {R.Capture.Local.Head});
    if (!BeforeCommit.Ok() || !ExpectedCommit.Ok()) return Fail(BeforeCommit.Error + ExpectedCommit.Error);
    R.BeforeWorkingCommit = BeforeCommit.Text().TrimEnd(); R.ExpectedWorkingCommit = ExpectedCommit.Text().TrimEnd();
    TArray<FString> Args {TEXT("diff"), TEXT("--raw"), TEXT("--no-ext-diff"), TEXT("--no-textconv"), TEXT("--no-abbrev"), TEXT("--no-renames"), TEXT("-z"), R.BeforeWorkingTree, R.IndexTree, TEXT("--")};
    Args.Append(R.Paths); const auto Diff = Git(Args);
    if (!Diff.Ok() || !ParseIncomingChanges(Diff.Out, R.Changes, Error)) return Fail(TEXT("Cannot inspect every discard target. ") + Error + Diff.Error);
    if (R.Changes.Num() != R.Paths.Num()) return Fail(TEXT("Some selected files no longer have working edits. Refresh and select them again."));
    for (const auto& Change : R.Changes)
        if (!R.Paths.Contains(Change.Path) || Change.Status != 'M' || Change.OldMode != TEXT("100644") || Change.NewMode != TEXT("100644") ||
            (Change.Kind != EPullPathKind::Documentation && !(Change.Kind == EPullPathKind::Package && Change.Path.StartsWith(TEXT("Content/")) &&
                Change.Path.EndsWith(TEXT(".uasset")) && !Change.Path.Contains(TEXT("/__External")) && !Change.Path.EndsWith(TEXT("_BuiltData.uasset")))))
            return Fail(TEXT("Only ordinary modified Content assets and documentation can be discarded here. Maps, external packages, source, configuration and other file types require external review: ") + DiscardDisplay(Change.Path));
    const auto AfterBytes = UntrackedFingerprint(R.Paths);
    const auto After = RefreshInternal(); const auto AfterStashes = ListStashesInternal();
    if (!AfterBytes.Ok() || AfterBytes.Out != R.RawHashes || !After.bValid || After.Head != R.Capture.Local.Head || After.Branch != R.Capture.Local.Branch ||
        After.IndexEntries != R.Capture.Local.IndexEntries || !AfterStashes.bValid || AfterStashes.Fingerprint != R.StashFingerprint)
        return Fail(TEXT("Saved files, staging, branch or stashes changed during discard review. Refresh and review again."));
    R.Fingerprint = R.Capture.Fingerprint + R.BeforeWorkingTree + R.IndexTree + R.ExpectedWorkingTree + DiscardDigest(R.RawHashes) + DiscardDigest(R.UntrackedHashes) + R.StashFingerprint;
    R.Text = TEXT("DISCARD WORKING EDITS\n\nRepository: ") + DiscardDisplay(R.Capture.Local.Root) + TEXT("\nHEAD: ") + R.Capture.Local.Head +
        TEXT("\n\nReplace ONLY the listed saved working files with their reviewed staged/index versions. If a file has staged A and newer working B, A stays staged and B is discarded. With no staged edit, its index version is the committed version.\n\nA durable recovery backup of the original saved bytes is verified before replacement, including LFS payloads. No untracked file is deleted. Other working edits, all staging, stashes, commits and locks remain in place. Unsaved packages must be resolved first. Asset reload can reset Undo and selection.\n\nFILES TO REPLACE\n");
    for (const auto& Path : R.Paths) R.Text += DiscardDisplay(Path) + TEXT("\n");
    R.Text += TEXT("\nReview does not replace files or create a recovery-list entry. Confirmation is required before Discard.\n");
    R.bValid = true; return R;
#endif
}
#if PLATFORM_MAC
FResult FRepository::ExecuteDiscard(const FDiscardReview& Reviewed, const GitWorkspaceSession::FLease& Lease, bool bConfirmed)
{
    FScopeLock Guard(&Mutex); FString Folder, Marker, Error;
    if (!bConfirmed || !Reviewed.IsFresh()) return DiscardFailure(TEXT("A fresh discard review and explicit confirmation are required. Nothing replaced."));
    if (!Lease.IsExclusiveFor(Reviewed.Capture.Local.Root) || !GitWorkspaceSession::NoOtherEditors(0, Error)) return DiscardFailure(TEXT("Exclusive editor access is required. ") + Error);
    if (!DiscardStorage(Reviewed, Folder, Marker, Error)) return DiscardFailure(Error);
    if (IFileManager::Get().FileExists(*Marker) || IFileManager::Get().DirectoryExists(*Folder)) return DiscardFailure(TEXT("Recovery or an earlier result already exists. Inspect it before repeating Discard."));
    const auto Current = ReviewDiscardInternal(Reviewed.Paths);
    if (!Current.IsFresh() || Current.Fingerprint != Reviewed.Fingerprint || Current.Paths != Reviewed.Paths || Current.RawHashes != Reviewed.RawHashes ||
        Current.IndexTree != Reviewed.IndexTree || Current.BeforeWorkingTree != Reviewed.BeforeWorkingTree || Current.ExpectedWorkingTree != Reviewed.ExpectedWorkingTree ||
        Current.Capture.Local.Root != Reviewed.Capture.Local.Root || Current.Capture.Local.IndexEntries != Reviewed.Capture.Local.IndexEntries ||
        Current.Capture.Local.Head != Reviewed.Capture.Local.Head || Current.Capture.Local.Branch != Reviewed.Capture.Local.Branch ||
        Current.Capture.WorkingTree != Reviewed.Capture.WorkingTree || Current.Capture.IndexTree != Reviewed.Capture.IndexTree ||
        Current.Capture.ConfigurationHash != Reviewed.Capture.ConfigurationHash || Current.StashFingerprint != Reviewed.StashFingerprint ||
        Current.UntrackedPaths != Reviewed.UntrackedPaths || Current.UntrackedHashes != Reviewed.UntrackedHashes)
        return DiscardFailure(TEXT("Discard review changed. Nothing replaced. ") + Current.Error);
    for (const auto& Pair : TArray<TPair<FString, FString>>{{Reviewed.BeforeWorkingCommit, Current.BeforeWorkingTree}, {Reviewed.ExpectedWorkingCommit, Current.ExpectedWorkingTree}})
    {
        const auto Tree = Git({TEXT("rev-parse"), TEXT("--verify"), Pair.Key + TEXT("^{tree}")});
        if (!Tree.Ok() || Tree.Text().TrimEnd() != Pair.Value) return DiscardFailure(TEXT("Discard snapshot differs from the reviewed files."));
    }
    for (const auto& Commit : {Current.BeforeWorkingCommit, Current.ExpectedWorkingCommit, Current.Capture.IndexCommit})
    {
        const auto Cache = CheckLocalLfs(Commit);
        if (!Cache.Ok()) return DiscardFailure(TEXT("Cannot discard without complete verified local LFS versions. Nothing replaced.\n") + Cache.Error);
    }
    const auto Hydrated = VerifyWorkingLfs(Current.BeforeWorkingCommit); if (!Hydrated.Ok()) return Hydrated;
    if (!IFileManager::Get().MakeDirectory(*FPaths::Combine(Folder, TEXT("payload")), true)) return DiscardFailure(TEXT("Cannot create discard backup storage. Nothing replaced."));
    const FString BackupText = DiscardBackupText(Folder);
    auto BackupFailure = [&](const FString& Message) { auto R = DiscardMessage(BackupText); R.Code = -1; R.Error = Message + TEXT("\n") + BackupText; return R; };
    TArray<FString> Raw; FResult RawResult; RawResult.Out = Current.RawHashes; RawResult.Text().ParseIntoArrayLines(Raw);
    if (Raw.Num() != Current.Paths.Num()) return BackupFailure(TEXT("Cannot identify every original saved file. Nothing replaced."));
    TSharedRef<FJsonObject> Manifest = MakeShared<FJsonObject>(); TArray<TSharedPtr<FJsonValue>> Files;
    Manifest->SetStringField(TEXT("operation"), TEXT("discard-working")); Manifest->SetStringField(TEXT("id"), Reviewed.Id);
    Manifest->SetStringField(TEXT("root"), Current.Capture.Local.Root); Manifest->SetStringField(TEXT("fingerprint"), Reviewed.Fingerprint);
    Manifest->SetStringField(TEXT("head"), Current.Capture.Local.Head); Manifest->SetStringField(TEXT("branch"), Current.Capture.Local.Branch);
    Manifest->SetStringField(TEXT("index_tree"), Current.IndexTree); Manifest->SetStringField(TEXT("saved_commit"), Current.Capture.Oid);
    Manifest->SetStringField(TEXT("backup_folder"), Folder);
    const FString Ref = TEXT("refs/uegit/discard/") + Reviewed.Id;
    Manifest->SetStringField(TEXT("saved_ref"), Ref);
    for (int32 I = 0; I < Current.Paths.Num(); ++I)
    {
        const FString Saved = FPaths::Combine(Folder, DiscardPayload(I));
        if (IFileManager::Get().Copy(*Saved, *FPaths::Combine(Root, Current.Paths[I]), false, true) != COPY_OK) return BackupFailure(TEXT("Could not preserve every selected saved file. Nothing replaced."));
        const auto Hash = Git({TEXT("hash-object"), TEXT("--no-filters"), TEXT("--"), Saved});
        if (!Hash.Ok() || Hash.Text().TrimEnd() != Raw[I] || !SyncDiscardPath(Saved)) return BackupFailure(TEXT("Recovery payload verification or persistence failed. Nothing replaced."));
        auto File = MakeShared<FJsonObject>(); File->SetStringField(TEXT("path"), Current.Paths[I]); File->SetStringField(TEXT("payload"), DiscardPayload(I));
        File->SetStringField(TEXT("raw_git_blob_hash"), Raw[I]); Files.Add(MakeShared<FJsonValueObject>(File));
    }
    Manifest->SetArrayField(TEXT("files"), Files);
    FString Json; if (!FJsonSerializer::Serialize(Manifest, TJsonWriterFactory<>::Create(&Json)) ||
        !FFileHelper::SaveStringToFile(Json, *FPaths::Combine(Folder, TEXT("manifest.json")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM) ||
        !SyncDiscardPath(FPaths::Combine(Folder, TEXT("manifest.json"))))
        return BackupFailure(TEXT("Cannot persist the recovery manifest. Nothing replaced."));
    // Persist the payload directory and every new directory entry up to GitDir
    // before any working file is replaced. Raw LFS bytes survive cache pruning.
    FString Directory = FPaths::Combine(Folder, TEXT("payload"));
    for (int32 I = 0; I < 5; ++I)
    {
        if (!SyncDiscardPath(Directory, true)) return BackupFailure(TEXT("Cannot persist discard recovery directories. Nothing replaced."));
        Directory = FPaths::GetPath(Directory);
    }
    const auto Recheck = ReviewDiscardInternal(Reviewed.Paths);
    if (!Recheck.IsFresh() || Recheck.Fingerprint != Reviewed.Fingerprint) return BackupFailure(TEXT("Local state changed during backup. Nothing replaced. ") + Recheck.Error);
    const auto Preserved = Git({TEXT("update-ref"), TEXT("--create-reflog"), Ref, Current.Capture.Oid, FString::ChrN(Current.Capture.Oid.Len(), '0')});
    const auto RefCheck = Git({TEXT("rev-parse"), TEXT("--verify"), Ref});
    if (!Preserved.Ok() || !RefCheck.Ok() || RefCheck.Text().TrimEnd() != Current.Capture.Oid) return BackupFailure(TEXT("Cannot confirm the saved Git recovery reference. Nothing replaced."));
    if (!CreateDiscardMarker(Marker, Json)) return BackupFailure(TEXT("Cannot persist exclusive active discard recovery. Nothing replaced."));
    const FString PathFile = FPaths::Combine(Folder, TEXT("paths.nul")); TArray<uint8> Paths;
    for (const auto& Path : Current.Paths) { FTCHARToUTF8 Bytes(*Path); Paths.Append(reinterpret_cast<const uint8*>(Bytes.Get()), Bytes.Length()); Paths.Add(0); }
    if (!FFileHelper::SaveArrayToFile(Paths, *PathFile)) return BackupFailure(TEXT("Recovery saved, but exact replacement paths could not be written. Nothing replaced."));
    const auto Restored = HydratedGit({TEXT("restore"), TEXT("--source=") + Current.IndexTree, TEXT("--worktree"), TEXT("--no-recurse-submodules"),
        TEXT("--pathspec-from-file=") + PathFile, TEXT("--pathspec-file-nul")});
    if (!Restored.Ok()) return BackupFailure(TEXT("Discard may be partial. Preserve the backup and inspect recovery with the editor closed.\n") + Restored.Error);
    for (const auto& Path : Current.Paths)
    {
        const FString File = FPaths::Combine(Root, Path);
        if (!SyncDiscardPath(File) || !SyncDiscardPath(FPaths::GetPath(File), true))
            return BackupFailure(TEXT("Replacement persistence could not be confirmed. Keep recovery active and inspect the files with the editor closed."));
    }
    const auto Checked = VerifyDiscardResult(Reviewed);
    if (!Checked.Ok()) return BackupFailure(Checked.Error);
    return DiscardMessage(BackupText); // Active marker remains until asset reload succeeds.
}
FResult FRepository::CompleteDiscard(const FDiscardReview& Reviewed, const GitWorkspaceSession::FLease& Lease)
{
    FScopeLock Guard(&Mutex); FString Folder, Marker, Error;
    if (!Lease.IsExclusiveFor(Reviewed.Capture.Local.Root)) return DiscardFailure(TEXT("Discard completion lost editor exclusivity."));
    if (!DiscardStorage(Reviewed, Folder, Marker, Error) || !ReadDiscardMarker(Reviewed, Marker)) return DiscardFailure(TEXT("Discard recovery identity changed; inspect the active report."));
    const auto Checked = VerifyDiscardResult(Reviewed); if (!Checked.Ok()) return Checked;
    if (!IFileManager::Get().Delete(*Marker)) return DiscardFailure(TEXT("Discard verified, but active recovery could not be cleared: ") + Marker);
    return DiscardMessage(DiscardBackupText(Folder));
}
#endif
FResult FRepository::VerifyDiscardResult(const FDiscardReview& Reviewed)
{
#if PLATFORM_MAC
    FString Folder, Marker, Error;
    if (!DiscardStorage(Reviewed, Folder, Marker, Error) || !ReadDiscardMarker(Reviewed, Marker)) return DiscardFailure(TEXT("Discard recovery identity changed. Keep recovery active."));
    FString ActiveText, ManifestText;
    if (!FFileHelper::LoadFileToString(ActiveText, *Marker) ||
        !FFileHelper::LoadFileToString(ManifestText, *FPaths::Combine(Folder, TEXT("manifest.json"))) || ManifestText != ActiveText)
        return DiscardFailure(TEXT("Discard recovery manifest changed. Keep recovery active and inspect the backup."));
    TArray<uint8> ActualHashes;
    for (int32 Start = 0; Start < Reviewed.Paths.Num(); Start += 32)
    {
        TArray<FString> Args {TEXT("hash-object"), TEXT("--no-filters"), TEXT("--")};
        for (int32 I = Start; I < FMath::Min(Start + 32, Reviewed.Paths.Num()); ++I) Args.Add(FPaths::Combine(Folder, DiscardPayload(I)));
        const auto Hash = Git(Args); if (!Hash.Ok()) return DiscardFailure(TEXT("Cannot verify the preserved discard payloads. Keep recovery active."));
        ActualHashes.Append(Hash.Out);
    }
    if (ActualHashes != Reviewed.RawHashes) return DiscardFailure(TEXT("Discard backup bytes changed. Keep recovery active and inspect the saved reference."));
    const FString Ref = TEXT("refs/uegit/discard/") + Reviewed.Id;
    const auto SavedWork = Git({TEXT("rev-parse"), TEXT("--verify"), Ref + TEXT("^{tree}")});
    const auto SavedIndex = Git({TEXT("rev-parse"), TEXT("--verify"), Ref + TEXT("^2^{tree}")});
    if (!SavedWork.Ok() || !SavedIndex.Ok() || SavedWork.Text().TrimEnd() != Reviewed.Capture.WorkingTree || SavedIndex.Text().TrimEnd() != Reviewed.Capture.IndexTree)
        return DiscardFailure(TEXT("The saved discard reference changed. Keep recovery active."));
#endif
    const auto After = RefreshInternal(); const auto Config = Git({TEXT("config"), TEXT("--null"), TEXT("--list")});
    if (!After.bValid || After.Head != Reviewed.Capture.Local.Head || After.Branch != Reviewed.Capture.Local.Branch || After.HasConflicts() || After.bOperationInProgress ||
        After.IndexEntries != Reviewed.Capture.Local.IndexEntries || !Config.Ok() || DiscardDigest(Config.Out) != Reviewed.Capture.ConfigurationHash)
        return DiscardFailure(TEXT("Repository or staging changed during discard. Keep the recovery backup and inspect before continuing."));
    const auto Work = Git({TEXT("diff"), TEXT("--quiet"), TEXT("--no-ext-diff"), TEXT("--no-textconv"), Reviewed.ExpectedWorkingTree, TEXT("--")});
    if (!Work.Ok()) return DiscardFailure(TEXT("Working files differ from the expected discard result. Keep recovery active."));
    TArray<FString> Untracked; for (const auto& File : After.Files) if (File.bUntracked) Untracked.Add(File.Path); Untracked.Sort();
    if (Untracked != Reviewed.UntrackedPaths) return DiscardFailure(TEXT("Untracked files changed during discard. Inspect recovery."));
    if (!Untracked.IsEmpty()) { const auto Bytes = UntrackedFingerprint(Untracked); if (!Bytes.Ok() || Bytes.Out != Reviewed.UntrackedHashes) return DiscardFailure(TEXT("Unrelated untracked bytes changed during discard.")); }
    const auto Stashes = ListStashesInternal();
    if (!Stashes.bValid || Stashes.Fingerprint != Reviewed.StashFingerprint) return DiscardFailure(TEXT("Stash list changed during discard. Inspect recovery before continuing."));
    return VerifyWorkingLfs(Reviewed.ExpectedWorkingCommit);
}
}
