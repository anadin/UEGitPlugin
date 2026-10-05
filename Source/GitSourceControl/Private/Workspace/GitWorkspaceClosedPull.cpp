// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#include "GitWorkspaceRepository.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "Misc/ScopeExit.h"
#include "HAL/PlatformProcess.h"
#if PLATFORM_MAC
#include "GitWorkspaceSession.h"
#include <sys/stat.h>
#endif

namespace GitWorkspace
{
namespace { FResult ClosedFailure(const FString& Error) { FResult R; R.Error = Error; return R; } }
FString RestartPullPathBlocker(const FRemoteSnapshot& Reviewed)
{
#if !PLATFORM_MAC
    return TEXT("Coordinated restart Pull is currently available on Mac only.");
#endif
    if (!Reviewed.IsFresh() || Reviewed.Ahead || !Reviewed.Behind) return TEXT("Fetch and review an incoming fast-forward first.");
    for (const auto& Change : Reviewed.IncomingChanges)
        if (Change.Kind != EPullPathKind::Documentation && !(Change.Kind == EPullPathKind::Package && Change.Path.StartsWith(TEXT("Content/"), ESearchCase::CaseSensitive)))
            return TEXT("Integrate source, configuration, plugin, symlink or other unsupported changes externally: ") + Change.Path;
    return FString();
}
FString EditorPullPathBlocker(const FRemoteSnapshot& Reviewed)
{
    const FString Base = RestartPullPathBlocker(Reviewed);
    if (!Base.IsEmpty()) return Base;
    for (const auto& Change : Reviewed.IncomingChanges)
    {
        if (Change.Kind == EPullPathKind::Documentation) continue;
        if ((Change.Status != 'A' && Change.Status != 'M') || !Change.Path.EndsWith(TEXT(".uasset"), ESearchCase::CaseSensitive) ||
            Change.Path.Contains(TEXT("/__ExternalActors__/")) || Change.Path.Contains(TEXT("/__ExternalObjects__/")) ||
            Change.Path.EndsWith(TEXT("_BuiltData.uasset")))
            return TEXT("Use Pull and reopen for maps, external packages, deletions or renames: ") + Change.Path;
    }
    return FString();
}

FResult FRepository::VerifyWorkingLfs(const FString& Commit) const
{
    const FString Hooks = FPaths::Combine(FPlatformProcess::UserTempDir(), TEXT("uegit-hydration-") + FGuid::NewGuid().ToString(EGuidFormats::Digits));
    if (!IFileManager::Get().MakeDirectory(*Hooks, true)) return ClosedFailure(TEXT("Cannot create LFS verification directory."));
    ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*Hooks, false, true); };
    const auto Cache = Git({TEXT("-c"), TEXT("core.hooksPath=") + Hooks, TEXT("-c"), TEXT("lfs.fetchinclude="), TEXT("-c"), TEXT("lfs.fetchexclude="),
        TEXT("lfs"), TEXT("fsck"), TEXT("--objects"), TEXT("--pointers"), TEXT("--dry-run"), Commit});
    if (!Cache.Ok()) return ClosedFailure(TEXT("LFS integrity changed during integration. ") + Cache.Text() + Cache.Error);
    // Compare actual working bytes to the hash-verified cache, without clean
    // filters: an unhydrated pointer can otherwise appear clean to git status.
    const auto Manifest = Git({TEXT("lfs"), TEXT("ls-files"), TEXT("--json"), TEXT("--include="), TEXT("--exclude="), Commit});
    TSharedPtr<FJsonObject> Json; const TArray<TSharedPtr<FJsonValue>>* Files = nullptr;
    if (!Manifest.Ok() || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Manifest.Text()), Json) || !Json)
        return ClosedFailure(TEXT("Cannot inspect the complete LFS hydration manifest. ") + Manifest.Error);
    // Git LFS serializes an empty manifest as null, not an empty array. A
    // missing field is still malformed; an explicit null means no LFS files.
    const auto* FileValue = Json->Values.Find(TEXT("files"));
    if (FileValue && *FileValue && (*FileValue)->Type == EJson::Null) { FResult Result; Result.Code = 0; return Result; }
    if (!Json->TryGetArrayField(TEXT("files"), Files)) return ClosedFailure(TEXT("Malformed LFS hydration manifest."));
    const auto Env = Git({TEXT("lfs"), TEXT("env")}); FString Media; TArray<FString> Lines;
    Env.Text().ParseIntoArrayLines(Lines);
    for (const auto& Line : Lines) if (Line.StartsWith(TEXT("LocalMediaDir="))) Media = Line.Mid(14);
    if (!Env.Ok() || Media.IsEmpty() || FPaths::IsRelative(Media)) return ClosedFailure(TEXT("Cannot locate the LFS object cache for hydration checks."));
    TArray<FString> ToHash; TSet<FString> Seen;
    for (const auto& Value : *Files)
    {
        const TSharedPtr<FJsonObject>* Object = nullptr; FString Path, Oid, Type; double Size = -1;
        if (!Value->TryGetObject(Object) || !Object || !*Object || !(*Object)->TryGetStringField(TEXT("name"), Path) ||
            !(*Object)->TryGetStringField(TEXT("oid"), Oid) || !(*Object)->TryGetStringField(TEXT("oid_type"), Type) ||
            !(*Object)->TryGetNumberField(TEXT("size"), Size) || Type != TEXT("sha256") || Oid.Len() != 64 || Size < 0 || Size > 9007199254740991.0 || Size != FMath::FloorToDouble(Size) ||
            Path.IsEmpty() || !FPaths::IsRelative(Path) || Path.Contains(TEXT("\\")) || Seen.Contains(Path)) return ClosedFailure(TEXT("Malformed LFS hydration manifest."));
        for (TCHAR C : Oid) if (!FChar::IsHexDigit(C)) return ClosedFailure(TEXT("Malformed LFS object identity."));
        TArray<FString> Parts; Path.ParseIntoArray(Parts, TEXT("/"), false);
        FString Working = Root;
        for (const auto& Part : Parts)
        {
            if (Part.IsEmpty() || Part == TEXT("..") || Part == TEXT(".")) return ClosedFailure(TEXT("Unsafe LFS hydration path."));
            Working = FPaths::Combine(Working, Part);
#if PLATFORM_MAC
            struct stat Info;
            if (lstat(TCHAR_TO_UTF8(*Working), &Info) || S_ISLNK(Info.st_mode)) return ClosedFailure(TEXT("Missing or symlinked LFS working path: ") + Path);
#endif
        }
        Seen.Add(Path);
        if (IFileManager::Get().FileSize(*Working) != int64(Size)) return ClosedFailure(TEXT("LFS working bytes are missing or not hydrated: ") + Path);
        if (!Size)
        {
            if (Oid != TEXT("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855")) return ClosedFailure(TEXT("Invalid empty LFS object: ") + Path);
            continue;
        }
        ToHash.Add(Working); ToHash.Add(FPaths::Combine(Media, Oid.Left(2), Oid.Mid(2, 2), Oid));
    }
    for (int32 Start = 0; Start < ToHash.Num(); Start += 64)
    {
        TArray<FString> Args {TEXT("hash-object"), TEXT("--no-filters"), TEXT("--")};
        const int32 End = FMath::Min(Start + 64, ToHash.Num());
        for (int32 I = Start; I < End; ++I) Args.Add(ToHash[I]);
        const auto Hashes = Git(Args); TArray<FString> Values; Hashes.Text().ParseIntoArrayLines(Values);
        if (!Hashes.Ok() || Values.Num() != End - Start) return ClosedFailure(TEXT("Cannot hash all hydrated files. ") + Hashes.Error);
        for (int32 I = 0; I < Values.Num(); I += 2) if (Values[I] != Values[I + 1])
            return ClosedFailure(TEXT("Working LFS bytes do not match the verified object: ") + ToHash[Start + I]);
    }
    FResult Result; Result.Code = 0; return Result;
}

#if PLATFORM_MAC
FResult FRepository::PullAfterEditorExit(const FRemoteSnapshot& Reviewed, const GitWorkspaceSession::FLease& Lease)
{
    FString Error = RestartPullPathBlocker(Reviewed);
    if (!Error.IsEmpty()) return ClosedFailure(Error);
    if (!Lease.IsExclusiveFor(Reviewed.Root) || !GitWorkspaceSession::NoOtherEditors(0, Error))
        return ClosedFailure(TEXT("Exclusive closed-editor access is required. ") + Error);
    const auto Prepared = PrepareIncomingLfs(Reviewed);
    if (!Prepared.bVerified) return ClosedFailure(Prepared.Error);
    return PullAssets(Reviewed, Lease, false);
}
FResult FRepository::PullForReload(const FRemoteSnapshot& Reviewed, const FIncomingLfsResult& Prepared, const GitWorkspaceSession::FLease& Lease)
{
    const FString Error = EditorPullPathBlocker(Reviewed);
    if (!Error.IsEmpty()) return ClosedFailure(Error);
    if (!Prepared.Matches(Reviewed)) return ClosedFailure(TEXT("Download and verify LFS for this exact review before pulling assets."));
    return PullAssets(Reviewed, Lease, true);
}
FResult FRepository::PullAssets(const FRemoteSnapshot& Reviewed, const GitWorkspaceSession::FLease& Lease, bool bForReload)
{
    FString Error;
    if (!Lease.IsExclusiveFor(Reviewed.Root) || !GitWorkspaceSession::NoOtherEditors(0, Error)) return ClosedFailure(TEXT("Exclusive editor access is required. ") + Error);
    FString CanonicalRoot, GitDir;
    if (!GitWorkspaceSession::FindRepository(Reviewed.Root, CanonicalRoot, GitDir)) return ClosedFailure(TEXT("Cannot locate recovery storage."));
    const FString Recovery = GitWorkspaceSession::RecoveryFile(GitDir);
    if (IFileManager::Get().FileExists(*Recovery)) return ClosedFailure(TEXT("An earlier update requires recovery: ") + Recovery);
    FScopeLock Guard(&Mutex); FSnapshot Current;
    if (!ValidateRemoteReview(Reviewed, Current, Error)) return ClosedFailure(Error);
    if (!Current.Files.IsEmpty()) return ClosedFailure(TEXT("Local changes appeared before integration. Preserve them and retry; no automatic stash or discard."));
    // Recompute the allowed paths rather than trusting a serialized/UI list.
    auto Actual = Reviewed;
    const auto Diff = Git({TEXT("diff"), TEXT("--raw"), TEXT("--no-abbrev"), TEXT("--no-renames"), TEXT("-z"), Reviewed.Head, Reviewed.RemoteHead, TEXT("--")});
    if (!Diff.Ok() || !ParseIncomingChanges(Diff.Out, Actual.IncomingChanges, Error)) return ClosedFailure(TEXT("Cannot validate incoming paths. ") + Error);
    Error = bForReload ? EditorPullPathBlocker(Actual) : RestartPullPathBlocker(Actual); if (!Error.IsEmpty()) return ClosedFailure(Error);
    if (bForReload)
    {
        if (Actual.IncomingChanges.Num() != Reviewed.IncomingChanges.Num()) return ClosedFailure(TEXT("Incoming paths differ from the package review."));
        for (int32 I = 0; I < Actual.IncomingChanges.Num(); ++I)
        {
            const auto& A = Actual.IncomingChanges[I]; const auto& B = Reviewed.IncomingChanges[I];
            if (A.Path != B.Path || A.Status != B.Status || A.Kind != B.Kind || A.OldMode != B.OldMode || A.NewMode != B.NewMode)
                return ClosedFailure(TEXT("Incoming paths differ from the package review."));
        }
    }
    // Git normally overwrites ignored paths during a merge. Reject collisions
    // before the transaction, and keep --no-overwrite-ignore on the merge too.
    for (const auto& Change : Actual.IncomingChanges) if (Change.Status == 'A' && IFileManager::Get().FileExists(*FPaths::Combine(Root, Change.Path)))
        return ClosedFailure(TEXT("An existing local file occupies an incoming path: ") + Change.Path);
    if (!Lease.IsExclusiveFor(Root) || !GitWorkspaceSession::NoOtherEditors(0, Error)) return ClosedFailure(Error);
    IFileManager::Get().MakeDirectory(*FPaths::GetPath(Recovery), true);
    const FString Marker = TEXT("Git Workspace asset Pull requires recovery.\nRepository: ") + Root + TEXT("\nOriginal commit: ") + Reviewed.Head + TEXT("\nIntended commit: ") + Reviewed.RemoteHead
        + TEXT("\nKeep the editor closed. Inspect git status, HEAD and the restart-pull job status/log. Preserve unexpected work before repairing. Verify LFS working files are hydrated. Remove this marker only after explicit external recovery. No automatic rollback was attempted.\n");
    if (!FFileHelper::SaveStringToFile(Marker, *Recovery, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return ClosedFailure(TEXT("Cannot persist the recovery marker; Pull was not started."));
    const auto Merged = Git({TEXT("merge"), TEXT("--ff-only"), TEXT("--no-autostash"), TEXT("--no-overwrite-ignore"), TEXT("--no-edit"), TEXT("--no-stat"), Reviewed.RemoteHead});
    if (!Merged.Ok()) return ClosedFailure(TEXT("Pull may be incomplete. The editor will stay closed; inspect ") + Recovery + TEXT("\n") + Merged.Error);
    // This never downloads or overwrites modified files; all target objects were
    // verified beforehand. A failed checkout is not reported as completed Pull.
    // LFS checkout performs its own wildcard scan. The wrapper's literal
    // pathspec setting suppresses that scan and can return zero with pointers
    // untouched. No user paths are passed here, so restore normal scanning.
    const auto Checkout = Git({TEXT("--no-literal-pathspecs"), TEXT("lfs"), TEXT("checkout")});
    if (!Checkout.Ok()) return ClosedFailure(TEXT("LFS hydration failed; the editor will stay closed. ") + Checkout.Error);
    const auto Hydrated = VerifyWorkingLfs(Reviewed.RemoteHead);
    if (!Hydrated.Ok()) return ClosedFailure(TEXT("Hydration could not be verified; the editor will stay closed. ") + Hydrated.Error);
    const auto After = RefreshInternal(); FRemoteSnapshot Context;
    if (!After.bValid || After.Head != Reviewed.RemoteHead || !After.Files.IsEmpty() || After.bOperationInProgress ||
        !RemoteContext(Context) || Context.Context != Reviewed.Context)
        return ClosedFailure(TEXT("Pull ran but local state/configuration differs from the reviewed result. The editor will stay closed; inspect ") + Recovery);
    if (!bForReload && !IFileManager::Get().Delete(*Recovery)) return ClosedFailure(TEXT("Update verified, but its recovery marker could not be cleared. Inspect ") + Recovery);
    return Merged;
}
FResult FRepository::CompleteReloadPull(const FRemoteSnapshot& Reviewed, const GitWorkspaceSession::FLease& Lease)
{
    FScopeLock Guard(&Mutex);
    if (!Lease.IsExclusiveFor(Reviewed.Root)) return ClosedFailure(TEXT("Editor exclusivity was lost during reload."));
    const auto After = RefreshInternal(); FRemoteSnapshot Context;
    if (!After.bValid || After.Head != Reviewed.RemoteHead || !After.Files.IsEmpty() || After.bOperationInProgress ||
        !RemoteContext(Context) || Context.Context != Reviewed.Context) return ClosedFailure(TEXT("Repository state changed during asset reload."));
    FString CanonicalRoot, GitDir;
    if (!GitWorkspaceSession::FindRepository(Reviewed.Root, CanonicalRoot, GitDir) || !IFileManager::Get().Delete(*GitWorkspaceSession::RecoveryFile(GitDir)))
        return ClosedFailure(TEXT("Reload finished, but its recovery marker could not be cleared."));
    FResult Result; Result.Code = 0; return Result;
}
#endif
}
