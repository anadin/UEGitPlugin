// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#include "GitWorkspaceRepository.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "Misc/SecureHash.h"
#if PLATFORM_MAC
#include <sys/stat.h>
#endif

namespace GitWorkspace
{
namespace
{
FResult LockFailure(const FString& Message) { FResult R; R.Error = Message; return R; }
bool SafePath(const FString& Path)
{
    if (Path.IsEmpty() || !FPaths::IsRelative(Path) || Path.Contains(TEXT("\\")) || Path.Contains(TEXT(":"))) return false;
    TArray<FString> Parts; Path.ParseIntoArray(Parts, TEXT("/"), false);
    for (const auto& Part : Parts) if (Part.IsEmpty() || Part == TEXT(".") || Part == TEXT("..") || Part.Equals(TEXT(".git"), ESearchCase::IgnoreCase)) return false;
    for (TCHAR C : Path) if (C == 0) return false;
    return true;
}
struct FLockCacheDirectory
{
    FString Path = FPaths::Combine(FPlatformProcess::UserTempDir(), TEXT("uegit-locks-") + FGuid::NewGuid().ToString(EGuidFormats::Digits));
    ~FLockCacheDirectory() { IFileManager::Get().DeleteDirectory(*Path, false, true); }
};
TArray<FString> NullRecords(const FResult& R)
{
    TArray<FString> Records;
    int32 Start = 0;
    for (int32 I = 0; I < R.Out.Num(); ++I) if (!R.Out[I])
    {
        FUTF8ToTCHAR U(reinterpret_cast<const ANSICHAR*>(R.Out.GetData() + Start), I - Start);
        Records.Emplace(U.Length(), U.Get()); Start = I + 1;
    }
    if (Start != R.Out.Num()) Records.Empty();
    return Records;
}
}
bool FLockSnapshot::IsFresh() const
{
    const double Age = FPlatformTime::Seconds() - VerifiedSeconds;
    return bVerified && Error.IsEmpty() && Age >= 0 && Age < 60;
}
ELockState FLockSnapshot::State(const FString& Path, bool bLockable) const
{
    const FLock* Lock = Locks.Find(Path);
    if (!bLockable && !Lock) return ELockState::NotLockable;
    if (!IsFresh()) return VerifiedAt.GetTicks() ? ELockState::Stale : ELockState::Unknown;
    return !Lock ? ELockState::Unlocked : (Lock->bOurs ? ELockState::Ours : ELockState::Theirs);
}
FString FLockSnapshot::Label(const FString& Path, bool bLockable) const
{
    switch (State(Path, bLockable))
    {
    case ELockState::NotLockable: return TEXT("Not required");
    case ELockState::Unlocked: return TEXT("Verified unlocked");
    case ELockState::Ours: return TEXT("Owned by me");
    case ELockState::Theirs: return TEXT("Owned by ") + Locks[Path].Owner;
    case ELockState::Stale: return TEXT("Stale — verify");
    default: return TEXT("Not verified");
    }
}
bool ParseVerifiedLocks(const FString& Json, TMap<FString, FLock>& Locks, FString& Error)
{
    Locks.Empty(); Error.Empty(); TMap<FString, FLock> Parsed; TSet<FString> Ids;
    TSharedPtr<FJsonObject> Object;
    if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Object) || !Object)
    { Error = TEXT("Malformed LFS lock JSON."); return false; }
    FString Message, Cursor;
    if ((Object->TryGetStringField(TEXT("message"), Message) && !Message.IsEmpty()) ||
        (Object->TryGetStringField(TEXT("next_cursor"), Cursor) && !Cursor.IsEmpty()))
    { Error = TEXT("Incomplete or failed lock verification."); return false; }
    for (const TCHAR* Key : {TEXT("ours"), TEXT("theirs")})
    {
        const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
        if (!Object->TryGetArrayField(Key, Entries)) { Error = TEXT("LFS verification omitted ownership arrays."); return false; }
        for (const auto& Value : *Entries)
        {
            const TSharedPtr<FJsonObject>* Entry = nullptr; FLock Lock;
            if (!Value || !Value->TryGetObject(Entry) || !Entry || !(*Entry) ||
                !(*Entry)->TryGetStringField(TEXT("id"), Lock.Id) || Lock.Id.IsEmpty() ||
                !(*Entry)->TryGetStringField(TEXT("path"), Lock.Path) || !SafePath(Lock.Path) ||
                !(*Entry)->TryGetStringField(TEXT("locked_at"), Lock.LockedAt))
            { Error = TEXT("Invalid LFS lock identity or path."); return false; }
            FDateTime Time;
            if (!FDateTime::ParseIso8601(*Lock.LockedAt, Time) || Parsed.Contains(Lock.Path) || Ids.Contains(Lock.Id))
            { Error = TEXT("Duplicate or malformed LFS lock record."); return false; }
            const TSharedPtr<FJsonObject>* Owner = nullptr;
            if ((*Entry)->TryGetObjectField(TEXT("owner"), Owner) && Owner && *Owner) (*Owner)->TryGetStringField(TEXT("name"), Lock.Owner);
            if (Lock.Owner.IsEmpty()) Lock.Owner = TEXT("another user");
            Lock.bOurs = FString(Key) == TEXT("ours"); Ids.Add(Lock.Id); Parsed.Add(Lock.Path, MoveTemp(Lock));
        }
    }
    Locks = MoveTemp(Parsed); return true;
}
FResult FRepository::Lfs(const FString& Remote, const TArray<FString>& Args, const FString& Storage) const
{
    TArray<FString> All {TEXT("-c"), TEXT("remote.lfsdefault=") + Remote, TEXT("-c"), TEXT("remote.lfspushdefault=") + Remote};
    if (!Storage.IsEmpty()) All.Append({TEXT("-c"), TEXT("lfs.storage=") + Storage});
    All.Add(TEXT("lfs")); All.Append(Args); return Git(All);
}
bool FRepository::LockContext(const FString& Remote, FLockSnapshot& Out)
{
    Out.Root = Root; Out.Remote = Remote;
    const auto Remotes = Git({TEXT("remote")}); TArray<FString> Names; Remotes.Text().ParseIntoArrayLines(Names);
    if (!Remotes.Ok() || !Names.Contains(Remote)) { Out.Error = TEXT("Choose a configured Git remote for locking."); return false; }
    // Split upload/download endpoints need separate endpoint discovery. Fail closed in this slice.
    for (const FString& Key : {FString(TEXT("lfs.pushurl")), TEXT("remote.") + Remote + TEXT(".lfspushurl"), TEXT("remote.") + Remote + TEXT(".pushurl")})
    {
        const auto Config = Git({TEXT("config"), TEXT("--get"), Key});
        if (Config.Ok() && !Config.Text().TrimEnd().IsEmpty()) { Out.Error = TEXT("Separate push endpoints are not supported by this locking slice. Use an external LFS client."); return false; }
        if (!Config.Ok() && Config.Code != 1) { Out.Error = Config.Error; return false; }
    }
    const auto Env = Lfs(Remote, {TEXT("env")});
    if (!Env.Ok()) { Out.Error = TEXT("Git LFS is unavailable: ") + Env.Error; return false; }
    TArray<FString> Lines; Env.Text().ParseIntoArrayLines(Lines);
    const FString Prefix = TEXT("Endpoint (") + Remote + TEXT(")=");
    for (const auto& Line : Lines)
        if (Line.StartsWith(Prefix) || Line.StartsWith(TEXT("Endpoint=")))
        {
            FString Endpoint = Line.Mid(Line.StartsWith(Prefix) ? Prefix.Len() : 9);
            int32 Auth = Endpoint.Find(TEXT(" (auth=")); if (Auth != INDEX_NONE) Endpoint.LeftInline(Auth);
            Out.Endpoint = Endpoint;
            if (Line.StartsWith(Prefix)) break;
        }
    if (Out.Endpoint.IsEmpty()) { Out.Error = TEXT("Git LFS did not resolve a locking endpoint."); return false; }
    // Do not expose embedded secrets in the panel.
    if (Out.Endpoint.Contains(TEXT("@")) || Out.Endpoint.Contains(TEXT("?")) || Out.Endpoint.Contains(TEXT("#")))
    { Out.Endpoint.Empty(); Out.Error = TEXT("Use a credential helper instead of credentials or query parameters in the LFS URL."); return false; }
    const auto Config = Git({TEXT("config"), TEXT("--null"), TEXT("--list")});
    const auto Branch = Git({TEXT("symbolic-ref"), TEXT("--quiet"), TEXT("--short"), TEXT("HEAD")});
    if (!Config.Ok() || !Branch.Ok()) { Out.Error = TEXT("Locking requires a named branch and readable Git configuration."); return false; }
    Out.Branch = Branch.Text().TrimEnd();
    TArray<uint8> Fingerprint = Config.Out, LfsConfig;
    const FString LfsConfigPath = FPaths::Combine(Root, TEXT(".lfsconfig"));
    if (IFileManager::Get().FileExists(*LfsConfigPath) && !FFileHelper::LoadFileToArray(LfsConfig, *LfsConfigPath))
    { Out.Error = TEXT("Cannot read .lfsconfig to verify endpoint identity."); return false; }
    if (!IFileManager::Get().FileExists(*LfsConfigPath))
    {
        auto Stored = Git({TEXT("show"), TEXT(":.lfsconfig")});
        if (!Stored.Ok()) Stored = Git({TEXT("show"), TEXT("HEAD:.lfsconfig")});
        if (Stored.Ok()) LfsConfig = Stored.Out;
    }
    FResult EffectiveConfig; EffectiveConfig.Out = LfsConfig;
    // LFS also reads .lfsconfig from the index/HEAD when absent on disk. Keep that
    // source in the identity check and reject split endpoints there as well.
    if (EffectiveConfig.Text().Contains(TEXT("pushurl"), ESearchCase::IgnoreCase))
    { Out.Error = TEXT("A .lfsconfig push endpoint requires external locking in this slice."); return false; }
    Fingerprint.Append(LfsConfig);
    uint8 Hash[20]; FSHA1::HashBuffer(Fingerprint.GetData(), Fingerprint.Num(), Hash);
    Out.Context = Root + TEXT("|") + Remote + TEXT("|") + Out.Endpoint + TEXT("|") + Out.Branch + TEXT("|") + BytesToHex(Hash, 20);
    return true;
}
bool FRepository::LockCandidates(FLockSnapshot& Out)
{
    const auto Files = Git({TEXT("ls-files"), TEXT("--cached"), TEXT("-z")});
    if (!Files.Ok()) { Out.Error = Files.Error; return false; }
    const auto Paths = NullRecords(Files);
    for (int32 Start = 0; Start < Paths.Num(); Start += 32)
    {
        const int32 End = FMath::Min(Start + 32, Paths.Num());
        TArray<FString> Args {TEXT("check-attr"), TEXT("-z"), TEXT("filter"), TEXT("lockable"), TEXT("--")};
        for (int32 I = Start; I < End; ++I) Args.Add(Paths[I]);
        const auto R = Git(Args); const auto Attrs = NullRecords(R);
        if (!R.Ok() || Attrs.Num() != (End - Start) * 6) { Out.Error = TEXT("Cannot read lockable file attributes."); return false; }
        for (int32 I = Start; I < End; ++I)
        {
            const int32 Offset = (I - Start) * 6;
            if (Attrs[Offset + 2] == TEXT("lfs") && Attrs[Offset + 5] == TEXT("set"))
            { FFile F; F.Path = Paths[I]; F.bLfs = F.bLockable = true; Out.Candidates.Add(F); }
        }
    }
    return true;
}
FLockSnapshot FRepository::VerifyLocks(const FString& Remote) { FScopeLock Guard(&Mutex); return VerifyLocksInternal(Remote); }
FLockSnapshot FRepository::VerifyLocksInternal(const FString& Remote)
{
    FLockSnapshot Out; Out.Remote = Remote;
    const auto Current = RefreshInternal(); Out.Root = Current.Root;
    if (!Current.bValid) { Out.Error = Current.Error; return Out; }
    if (!LockCandidates(Out) || !LockContext(Remote, Out)) return Out;
    FLockCacheDirectory Cache;
    // --verify --json can exit zero with partial results on API failure (LFS 3.8).
    // First require non-JSON verification to complete every page successfully, then
    // read JSON from this invocation's private cache. Never trust a shared old cache.
    auto R = Lfs(Remote, {TEXT("locks"), TEXT("--verify"), TEXT("--remote=" ) + Remote}, Cache.Path);
    if (!R.Ok()) { Out.Error = TEXT("Lock verification failed; ownership is unknown. ") + R.Error; return Out; }
    R = Lfs(Remote, {TEXT("locks"), TEXT("--verify"), TEXT("--cached"), TEXT("--json"), TEXT("--remote=") + Remote}, Cache.Path);
    if (!R.Ok() || !ParseVerifiedLocks(R.Text(), Out.Locks, Out.Error)) { if (!R.Ok()) Out.Error = R.Error; return Out; }
    FLockSnapshot After;
    if (!LockContext(Remote, After) || After.Context != Out.Context)
    { Out.Error = TEXT("Repository or endpoint changed during verification. Verify again."); Out.Locks.Empty(); return Out; }
    Out.bVerified = true; Out.VerifiedAt = FDateTime::UtcNow(); Out.VerifiedSeconds = FPlatformTime::Seconds(); return Out;
}
FResult FRepository::ChangeLock(const FLockSnapshot& Reviewed, const FString& Path, bool bUnlock, bool bHandoffConfirmed, const FString& ReviewedHead)
{
    FScopeLock Guard(&Mutex);
    if (!Reviewed.IsFresh() || !SafePath(Path)) return LockFailure(TEXT("Verify locks and review the selected path before continuing."));
    auto Current = VerifyLocksInternal(Reviewed.Remote);
    if (!Current.IsFresh()) return LockFailure(Current.Error);
    if (Current.Context != Reviewed.Context) return LockFailure(TEXT("Repository, branch or endpoint changed. Verify and review again."));
    const FLock* Lock = Current.Locks.Find(Path);
    if (bUnlock)
    {
        const FLock* Old = Reviewed.Locks.Find(Path);
        if (!Lock || !Lock->bOurs || !Old || Old->Id != Lock->Id) return LockFailure(TEXT("Lock ownership or identity changed. Nothing unlocked."));
        if (!bHandoffConfirmed) return LockFailure(TEXT("Confirm the team handoff before releasing this lock."));
        const auto Release = ReviewUnlockInternal(Current, Path, ReviewedHead);
        if (!Release.IsFresh()) return LockFailure(Release.Error.IsEmpty() ? TEXT("Unlock review expired. Review again.") : Release.Error);
        FString RecordError;
        // ID prevents releasing a replacement lock after a race; never use --force.
        auto Result = Lfs(Current.Remote, {TEXT("unlock"), TEXT("--json"), TEXT("--remote=") + Current.Remote, TEXT("--id=") + Lock->Id});
        if (!Result.Ok()) return LockFailure(TEXT("Unlock did not complete reliably. Verify before retrying. ") + Result.Error);
        auto After = VerifyLocksInternal(Current.Remote);
        if (!After.IsFresh() || After.Context != Current.Context || After.Locks.Contains(Path))
            return LockFailure(TEXT("Unlock was sent, but the unlocked state could not be confirmed. Verify before retrying. ") + After.Error);
        if (!RemoveLockRecord(Current, *Lock, RecordError)) return LockFailure(RecordError);
        return Result;
    }
    if (Reviewed.State(Path, true) != ELockState::Unlocked) return LockFailure(TEXT("Review a verified unlocked asset before acquiring a lock."));
    if (Lock) return LockFailure(Lock->bOurs ? TEXT("You already own this lock; it may belong to another clone.") : TEXT("Another user owns this lock."));
    const auto Attributes = Git({TEXT("check-attr"), TEXT("-z"), TEXT("filter"), TEXT("lockable"), TEXT("--"), Path});
    const auto Attrs = NullRecords(Attributes);
    if (!Attributes.Ok() || Attrs.Num() != 6 || Attrs[2] != TEXT("lfs") || Attrs[5] != TEXT("set")) return LockFailure(TEXT("Only LFS files with the effective lockable attribute can be locked here."));
    const auto Status = RefreshInternal();
    if (!Status.bValid || Status.bOperationInProgress || Status.HasConflicts()) return LockFailure(TEXT("Resolve repository state before locking."));
    const auto Index = Git({TEXT("ls-files"), TEXT("--stage"), TEXT("-z"), TEXT("--"), Path});
    const auto Entries = NullRecords(Index);
    const bool bTracked = Entries.Num() == 1 && (Entries[0].StartsWith(TEXT("100644 ")) || Entries[0].StartsWith(TEXT("100755 ")));
    // Only accept a new path that Git actually reported as an untracked file.
    // Ignored files and paths inside nested repositories are not parent assets.
    const bool bUntracked = Entries.IsEmpty() && Status.Files.ContainsByPredicate([&Path](const FFile& File) { return File.Path == Path && File.bUntracked; });
    if (!Index.Ok() || (!bTracked && !bUntracked) || !IFileManager::Get().FileExists(*FPaths::Combine(Root, Path)))
        return LockFailure(TEXT("Save the asset to a regular, non-ignored file in this repository before locking. Deleted and submodule paths require external handling."));
    TArray<FString> Parts; Path.ParseIntoArray(Parts, TEXT("/")); FString Component = Root;
    for (const auto& Part : Parts)
    {
        Component = FPaths::Combine(Component, Part);
#if PLATFORM_MAC
        // Apple's IPlatformFile::IsSymlink uses stat in UE 5.8, which follows
        // links. lstat must inspect the path itself for this boundary check.
        struct stat Info;
        if (lstat(TCHAR_TO_UTF8(*Component), &Info) != 0 || S_ISLNK(Info.st_mode) ||
            (Component == FPaths::Combine(Root, Path) ? !S_ISREG(Info.st_mode) : !S_ISDIR(Info.st_mode)))
#else
        if (FPlatformFileManager::Get().GetPlatformFile().IsSymlink(*Component) != ESymlinkResult::NonSymlink)
#endif
            return LockFailure(TEXT("Cannot lock through a symlink or a path whose type cannot be verified. Select a saved regular asset inside this repository."));
    }
    FLockSnapshot BeforeLock;
    if (!LockContext(Current.Remote, BeforeLock) || BeforeLock.Context != Current.Context)
        return LockFailure(TEXT("Endpoint or branch changed before locking. Verify again."));
    auto Result = Lfs(Current.Remote, {TEXT("lock"), TEXT("--json"), TEXT("--remote=") + Current.Remote, TEXT("--"), Path});
    if (!Result.Ok()) return LockFailure(TEXT("Lock did not complete reliably. Verify before retrying. ") + Result.Error);
    TMap<FString, FLock> Created; FString ParseError;
    if (!ParseVerifiedLocks(TEXT("{\"ours\":") + Result.Text() + TEXT(",\"theirs\":[]}"), Created, ParseError) || Created.Num() != 1 || !Created.Contains(Path))
        return LockFailure(TEXT("Lock response could not be validated. Verify before retrying."));
    auto After = VerifyLocksInternal(Current.Remote); const auto* Acquired = After.Locks.Find(Path);
    if (!After.IsFresh() || After.Context != Current.Context || !Acquired || !Acquired->bOurs || Acquired->Id != Created[Path].Id)
        return LockFailure(TEXT("Lock was sent, but ownership could not be confirmed. Verify before retrying. ") + After.Error);
    FString RecordError;
    if (!WriteLockRecord(Current, *Acquired, RecordError))
        return LockFailure(TEXT("Server lock acquired and verified, but local acquisition tracking failed. ") + RecordError);
    return Result;
}
}
