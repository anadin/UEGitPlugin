// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#include "GitWorkspaceRepository.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "Misc/SecureHash.h"
#if PLATFORM_MAC
#include "GitWorkspaceSession.h"
#endif

namespace GitWorkspace
{
namespace
{
FResult SaveFailure(const FString& Message) { FResult R; R.Error = Message; return R; }
TArray<FString> SaveRecords(const FResult& R)
{
    TArray<FString> Records; int32 Start = 0;
    for (int32 I = 0; I < R.Out.Num(); ++I) if (!R.Out[I])
    { FUTF8ToTCHAR U(reinterpret_cast<const ANSICHAR*>(R.Out.GetData() + Start), I - Start); Records.Emplace(U.Length(), U.Get()); Start = I + 1; }
    if (Start != R.Out.Num()) Records.Empty();
    return Records;
}
FString SaveFingerprint(const FAssetSaveReview& R)
{
    FResult Parts; Parts.Out = R.Local.IndexEntries; Parts.Out.Append(R.RawHashes);
    FString Identity = R.Local.Root + TEXT("|") + R.Local.Head + TEXT("|") + R.Locks.Context;
    for (const FString& Path : R.Paths)
    {
        Identity += TEXT("|") + Path;
        if (const auto* L = R.Locks.Locks.Find(Path)) Identity += TEXT("|") + L->Id + TEXT("|") + L->LockedAt;
    }
    FTCHARToUTF8 Utf8(*Identity); Parts.Out.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
    uint8 Hash[20]; FSHA1::HashBuffer(Parts.Out.GetData(), Parts.Out.Num(), Hash); return BytesToHex(Hash, 20);
}
}
FString FAssetSaveReview::Text() const
{
    FString Report = TEXT("Repository: ") + Local.Root + TEXT("\nLock remote: ") + Locks.Remote + TEXT("\n\n");
    for (const auto& Path : Paths)
        Report += (NeedsLock.Contains(Path) ? TEXT("Lock before saving: ") : TEXT("Keep my existing lock: ")) + Path + TEXT("\n");
    return Report + TEXT("\nSaving retains locks. Staging, commits and pushes remain separate.\nCancel keeps your unsaved edits in the editor.\nThis review expires after 60 seconds; cancel and Save again to refresh.");
}
FResult FRepository::IsLockableAssetInternal(const FString& Path) const
{
    if (Path.IsEmpty() || !FPaths::IsRelative(Path) || Path.Contains(TEXT("\\"))) return SaveFailure(TEXT("Invalid asset save path."));
    TArray<FString> Parts; Path.ParseIntoArray(Parts, TEXT("/"), false);
    for (const auto& Part : Parts) if (Part.IsEmpty() || Part == TEXT(".") || Part == TEXT("..") || Part.Equals(TEXT(".git"), ESearchCase::IgnoreCase)) return SaveFailure(TEXT("Invalid asset save path."));
    const auto R = Git({TEXT("check-attr"), TEXT("-z"), TEXT("filter"), TEXT("lockable"), TEXT("--"), Path});
    const auto Records = SaveRecords(R);
    if (!R.Ok() || Records.Num() != 6 || Records[0] != Path || Records[3] != Path) return SaveFailure(TEXT("Cannot verify effective save attributes. ") + R.Error);
    FResult Result; Result.Code = Records[2] == TEXT("lfs") && Records[5] == TEXT("set") ? 0 : 1; return Result;
}
FResult FRepository::IsLockableAsset(const FString& Path)
{ FScopeLock Guard(&Mutex); const auto Local = RefreshInternal(); if (!Local.bValid) return SaveFailure(Local.Error); return IsLockableAssetInternal(Path); }
FAssetSaveReview FRepository::ReviewAssetSave(const TArray<FString>& Paths, const FString& Remote)
{ FScopeLock Guard(&Mutex); return ReviewAssetSaveInternal(Paths, Remote); }
FAssetSaveReview FRepository::ReviewAssetSaveInternal(const TArray<FString>& Requested, const FString& Remote)
{
    FAssetSaveReview R; R.Local = RefreshInternal(); R.Locks.Remote = Remote;
    if (!R.Local.bValid || R.Local.bOperationInProgress || R.Local.HasConflicts())
    { R.Error = R.Local.Error.IsEmpty() ? TEXT("Resolve the repository operation or conflicts before saving locked assets.") : R.Local.Error; return R; }
    TArray<FString> Paths = Requested; Paths.Sort();
    for (const auto& Path : Paths)
    {
        const auto Attribute = IsLockableAssetInternal(Path);
        if (Attribute.Code == 1) continue;
        if (!Attribute.Ok()) { R.Error = Attribute.Error; return R; }
        if (!Path.EndsWith(TEXT(".uasset")) && !Path.EndsWith(TEXT(".umap"))) { R.Error = TEXT("Only saved Unreal packages are supported by Lock and save."); return R; }
        if (Path.Contains(TEXT("/__ExternalActors__/")) || Path.Contains(TEXT("/__ExternalObjects__/")))
        { R.Error = TEXT("External actor/object package saves need a coordinated map save workflow. Save cancelled."); return R; }
        const auto File = UntrackedFingerprint({Path});
        if (!File.Ok()) { R.Error = File.Error; return R; }
        const auto Index = Git({TEXT("ls-files"), TEXT("--stage"), TEXT("-z"), TEXT("--"), Path});
        const auto Entries = SaveRecords(Index);
        const bool Tracked = Entries.Num() == 1 && Entries[0].StartsWith(TEXT("100644 "));
        const bool Untracked = Entries.IsEmpty() && R.Local.Files.ContainsByPredicate([&](const FFile& F) { return F.Path == Path && F.bUntracked; });
        const auto* Changed = R.Local.Files.FindByPredicate([&](const FFile& F) { return F.Path == Path; });
        if (!Index.Ok() || (!Tracked && !Untracked) || (Changed && (!Changed->OriginalPath.IsEmpty() || Changed->bSubmodule || Changed->bConflict)))
        { R.Error = TEXT("Only regular tracked or saved, non-ignored untracked assets can use Lock and save: ") + Path; return R; }
        const auto Pointer = Lfs(Remote, {TEXT("pointer"), TEXT("--check"), TEXT("--file=") + FPaths::Combine(R.Local.Root, Path)});
        if (Pointer.Code != 1) { R.Error = Pointer.Ok() ? TEXT("Hydrate this LFS pointer before saving: ") + Path : TEXT("Cannot verify hydrated asset bytes: ") + Path + TEXT(". ") + Pointer.Error; return R; }
        R.Paths.AddUnique(Path);
    }
    if (R.Paths.IsEmpty()) { R.bValid = true; return R; }
    R.Locks = VerifyLocksInternal(Remote);
    if (!R.Locks.IsFresh()) { R.Error = R.Locks.Error; return R; }
    for (const auto& Path : R.Paths)
    {
        const auto* Lock = R.Locks.Locks.Find(Path);
        if (!Lock) R.NeedsLock.Add(Path);
        else if (!Lock->bOurs) { R.Error = TEXT("Save blocked: ") + Path + TEXT(" is locked by ") + Lock->Owner + TEXT("."); return R; }
        else if (!ReadLockRecord(R.Locks, *Lock, R.Error)) { R.Error = TEXT("Save blocked; this lock was not acquired by this checkout. ") + R.Error; return R; }
    }
    const auto Bytes = UntrackedFingerprint(R.Paths);
    if (!Bytes.Ok()) { R.Error = Bytes.Error; return R; }
    R.RawHashes = Bytes.Out;
    const auto After = RefreshInternal();
    if (!After.bValid || After.Head != R.Local.Head || After.IndexEntries != R.Local.IndexEntries || After.bOperationInProgress || After.HasConflicts())
    { R.Error = TEXT("Repository changed while reviewing the save. Try Save again."); return R; }
    R.Fingerprint = SaveFingerprint(R); R.bValid = true; return R;
}
#if PLATFORM_MAC
FAssetSavePreparation FRepository::PrepareAssetSave(const FAssetSaveReview& Reviewed, const GitWorkspaceSession::FLease& Lease, bool bLockConfirmed)
{
    FScopeLock Guard(&Mutex); FAssetSavePreparation Out;
    auto Fail = [&](const FString& Message) { Out.Result = SaveFailure(Message); return Out; };
    if (!Reviewed.IsFresh() || Reviewed.Paths.IsEmpty()) return Fail(TEXT("Save review expired or has no protected assets. Try Save again."));
    FString Error;
    if (!Lease.IsExclusiveFor(Reviewed.Local.Root) || !GitWorkspaceSession::NoOtherEditors(0, Error)) return Fail(Error.IsEmpty() ? TEXT("Save requires exclusive access to this editor session.") : Error);
    if (!Reviewed.NeedsLock.IsEmpty() && !bLockConfirmed) return Fail(TEXT("Lock and save was not confirmed. No locks acquired."));
    auto Current = ReviewAssetSaveInternal(Reviewed.Paths, Reviewed.Locks.Remote);
    if (!Current.IsFresh() || Current.Fingerprint != Reviewed.Fingerprint || Current.Paths != Reviewed.Paths || Current.NeedsLock != Reviewed.NeedsLock)
        return Fail(Current.Error.IsEmpty() ? TEXT("Assets, staging or lock ownership changed. Try Save again.") : Current.Error);
    for (const auto& Path : Current.NeedsLock)
    {
        const auto Locked = ChangeLockInternal(Current.Locks, Path, false, false, FString());
        if (!Locked.Ok()) return Fail(TEXT("Save cancelled. Any acquired locks remain held; verify locks before retrying.\n") + Path + TEXT(": ") + Locked.Error);
        Out.AcquiredPaths.Add(Path);
    }
    auto Ready = ReviewAssetSaveInternal(Current.Paths, Current.Locks.Remote);
    if (!Ready.IsFresh() || !Ready.NeedsLock.IsEmpty() || Ready.RawHashes != Current.RawHashes || Ready.Local.Head != Current.Local.Head ||
        Ready.Local.IndexEntries != Current.Local.IndexEntries || Ready.Locks.Context != Current.Locks.Context)
        return Fail(TEXT("Save cancelled after locking; acquired locks remain held. ") + Ready.Error);
    for (const auto& Path : Ready.Paths)
    {
        // Only locally recorded, freshly verified owned files become writable.
        const FString Full = FPaths::Combine(Ready.Local.Root, Path);
        const auto Check = CheckStashPaths({Path}, true);
        if (!Check.Ok() || !FPlatformFileManager::Get().GetPlatformFile().SetReadOnly(*Full, false))
            return Fail(TEXT("Save cancelled; cannot make the verified owned asset writable: ") + Path);
    }
    auto Permit = MakeShared<FAssetSavePermit, ESPMode::ThreadSafe>(); Permit->Review = MoveTemp(Ready);
    Out.Permit = Permit; Out.Result.Code = 0; return Out;
}
FResult FRepository::ValidateAssetSave(const FAssetSavePermit& Permit, const FString& Path, const GitWorkspaceSession::FLease& Lease)
{
    FScopeLock Guard(&Mutex); const auto& Approved = Permit.Review;
    if (!Lease.IsExclusiveFor(Approved.Local.Root) || !Approved.Paths.Contains(Path)) return SaveFailure(TEXT("Asset was not prepared for this save."));
    FString EditorError;
    if (!GitWorkspaceSession::NoOtherEditors(0, EditorError)) return SaveFailure(EditorError);
    auto Current = ReviewAssetSaveInternal({Path}, Approved.Locks.Remote);
    if (!Current.IsFresh() || !Current.NeedsLock.IsEmpty()) return SaveFailure(Current.Error.IsEmpty() ? TEXT("Lock ownership changed before saving.") : Current.Error);
    const auto* Old = Approved.Locks.Locks.Find(Path); const auto* Now = Current.Locks.Locks.Find(Path);
    TArray<FString> Hashes; FResult Raw; Raw.Out = Approved.RawHashes; Raw.Text().ParseIntoArrayLines(Hashes);
    const int32 Position = Approved.Paths.IndexOfByKey(Path);
    FResult CurrentBytes; CurrentBytes.Out = Current.RawHashes;
    if (!Old || !Now || Old->Id != Now->Id || Old->LockedAt != Now->LockedAt || Current.Locks.Context != Approved.Locks.Context ||
        Current.Local.Head != Approved.Local.Head || Current.Local.IndexEntries != Approved.Local.IndexEntries ||
        !Hashes.IsValidIndex(Position) || CurrentBytes.Text().TrimEnd() != Hashes[Position])
        return SaveFailure(TEXT("Saved bytes, staging, branch or lock identity changed before saving. Try Save again."));
    FResult R; R.Code = 0; return R;
}
#endif
}
