// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#include "GitWorkspaceSaveFlow.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "Serialization/JsonSerializer.h"
#if PLATFORM_MAC
#include "GitWorkspaceSession.h"
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>
#include <errno.h>

namespace GitWorkspaceSave
{
namespace
{
struct FFd
{
    int Value;
    explicit FFd(int In = -1) : Value(In) {}
    ~FFd() { if (Value >= 0) close(Value); }
    FFd(const FFd&) = delete;
    FFd& operator=(const FFd&) = delete;
};
int Directory(int Parent, const FString& Name, bool bCreate)
{
    if (bCreate && mkdirat(Parent, TCHAR_TO_UTF8(*Name), 0700) && errno != EEXIST) return -1;
    const int Fd = openat(Parent, TCHAR_TO_UTF8(*Name), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (Fd >= 0 && bCreate && fsync(Parent)) { close(Fd); return -1; }
    return Fd;
}
bool WriteAll(int Fd, const void* Data, size_t Count)
{
    const auto* Bytes = static_cast<const uint8*>(Data);
    while (Count)
    {
        const auto Written = write(Fd, Bytes, Count);
        if (Written < 0 && errno == EINTR) continue;
        if (Written <= 0) return false;
        Bytes += Written; Count -= Written;
    }
    return true;
}
bool WriteExclusive(int Parent, const char* Name, const FString& Text)
{
    FFd File(openat(Parent, Name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
    FTCHARToUTF8 Bytes(*Text);
    return File.Value >= 0 && WriteAll(File.Value, Bytes.Get(), Bytes.Length()) && fsync(File.Value) == 0 && fsync(Parent) == 0;
}
FString Digest(int Fd, int Copy = -1)
{
    if (lseek(Fd, 0, SEEK_SET) < 0) return {};
    FSHA1 Sha; uint8 Bytes[65536];
    for (;;)
    {
        const auto Count = read(Fd, Bytes, sizeof(Bytes));
        if (Count < 0 && errno == EINTR) continue;
        if (Count < 0) return {};
        if (!Count) break;
        Sha.Update(Bytes, Count);
        if (Copy >= 0 && !WriteAll(Copy, Bytes, Count)) return {};
    }
    Sha.Final(); uint8 Hash[20]; Sha.GetHash(Hash); return BytesToHex(Hash, 20);
}
bool SameFile(const struct stat& A, const struct stat& B)
{ return S_ISREG(A.st_mode) && S_ISREG(B.st_mode) && A.st_dev == B.st_dev && A.st_ino == B.st_ino && A.st_size == B.st_size; }
GitWorkspace::FResult Failure(const FString& Error, const FActorDeletionRecovery& R)
{
    GitWorkspace::FResult Result; Result.Error = Error;
    if (!R.Folder.IsEmpty()) Result.Error += TEXT("\nRecovery backup: ") + R.Folder;
    if (R.bActive) Result.Error += TEXT("\nActive deletion recovery remains. Preserve other unsaved editor work before closing. Inspect manifest.json, payload.uasset and removed.uasset; do not overwrite a competing destination.");
    return Result;
}
}
GitWorkspace::FResult RemoveExternalActorFile(const FPackageSavePaths& Plan, const FPackageSavePaths::FEntry& Entry, GitWorkspace::FRepository& Repository,
    const GitWorkspace::FAssetSavePermit& Permit, const GitWorkspaceSession::FLease& Lease, FActorDeletionRecovery& Recovery)
{
    check(IsInGameThread());
    if (!Plan.bCoordinatedActors || !Plan.DeletePaths.Contains(Entry.Path) ||
        &Entry != Plan.Entries.FindByPredicate([&](const auto& E) { return E.Path == Entry.Path; }) ||
        Entry.Kind != FPackageSavePaths::EKind::DeleteActor || !Permit.ContainsDeletePath(Entry.Path) || !Permit.ContainsExternalActorPath(Entry.Path))
        return Failure(TEXT("Actor deletion requires explicit deletion authorization in the coordinated plan."), Recovery);
    const FString Binding = ValidateExternalActorBinding(Plan, Entry.Path, Entry.Package.Get());
    if (!Binding.IsEmpty()) return Failure(Binding, Recovery);
    const auto Ready = Repository.ValidateAssetSave(Permit, Entry.Path, Lease); if (!Ready.Ok()) return Ready;
    const auto Before = Repository.Refresh(); const auto Stashes = Repository.ListStashes();
    FString Root, GitDir;
    if (!Before.bValid || !Stashes.bValid || !GitWorkspaceSession::FindRepository(Before.Root, Root, GitDir) || !Lease.IsExclusiveFor(Root) ||
        FPaths::Combine(Root, Entry.Path) != Entry.Filename)
        return Failure(TEXT("Cannot establish exact deletion/recovery storage."), Recovery);
    FFd Repo(open(TCHAR_TO_UTF8(*Root), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    FFd Git(open(TCHAR_TO_UTF8(*GitDir), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (Repo.Value < 0 || Git.Value < 0) return Failure(TEXT("Cannot open repository directories safely."), Recovery);
    TArray<FString> Parts; Entry.Path.ParseIntoArray(Parts, TEXT("/"), false);
    FFd Parent(dup(Repo.Value));
    for (int32 I = 0; I + 1 < Parts.Num(); ++I)
    {
        const int Next = Directory(Parent.Value, Parts[I], false);
        if (Next < 0) return Failure(TEXT("Deletion parent changed or is a symlink."), Recovery);
        close(Parent.Value); Parent.Value = Next;
    }
    FFd Source(openat(Parent.Value, TCHAR_TO_UTF8(*Parts.Last()), O_RDONLY | O_NOFOLLOW | O_CLOEXEC));
    struct stat Original;
    if (Source.Value < 0 || fstat(Source.Value, &Original) || !S_ISREG(Original.st_mode) || Original.st_nlink != 1)
        return Failure(TEXT("Only one regular, non-hardlinked actor file can be removed."), Recovery);
    FFd UeGit(Directory(Git.Value, TEXT("uegit"), true));
    FFd Store(UeGit.Value < 0 ? -1 : Directory(UeGit.Value, TEXT("actor-delete"), true));
    FFd MarkerDir(UeGit.Value < 0 ? -1 : Directory(UeGit.Value, TEXT("restart-pull"), true));
    if (Store.Value < 0 || MarkerDir.Value < 0) return Failure(TEXT("Cannot persist safe actor deletion recovery directories."), Recovery);
    struct stat Storage;
    if (fstat(Store.Value, &Storage) || Storage.st_dev != Original.st_dev)
        return Failure(TEXT("Actor deletion requires working files and Git recovery storage on the same volume. Nothing removed."), Recovery);
    const FString Id = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    if (mkdirat(Store.Value, TCHAR_TO_UTF8(*Id), 0700)) return Failure(TEXT("Cannot create a unique actor deletion backup."), Recovery);
    FFd Folder(Directory(Store.Value, Id, false));
    Recovery.Folder = FPaths::Combine(GitDir, TEXT("uegit/actor-delete"), Id); Recovery.Marker = GitWorkspaceSession::RecoveryFile(GitDir);
    Recovery.Path = Entry.Path; Recovery.Head = Before.Head; Recovery.Branch = Before.Branch;
    Recovery.IndexEntries = Before.IndexEntries; Recovery.Stashes = Stashes.Fingerprint;
    FFd Payload(openat(Folder.Value, "payload.uasset", O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
    if (Folder.Value < 0 || Payload.Value < 0) return Failure(TEXT("Cannot create the deletion payload."), Recovery);
    Recovery.Hash = Digest(Source.Value, Payload.Value);
    if (Recovery.Hash.IsEmpty() || Digest(Payload.Value) != Recovery.Hash || fsync(Payload.Value))
        return Failure(TEXT("Deletion backup byte verification/persistence failed. Nothing removed."), Recovery);
    auto Json = MakeShared<FJsonObject>();
    Json->SetStringField(TEXT("operation"), TEXT("external-actor-delete")); Json->SetStringField(TEXT("id"), Id);
    Json->SetStringField(TEXT("root"), Root); Json->SetStringField(TEXT("path"), Entry.Path); Json->SetStringField(TEXT("backup_folder"), Recovery.Folder);
    Json->SetStringField(TEXT("payload"), TEXT("payload.uasset")); Json->SetStringField(TEXT("removed_file"), TEXT("removed.uasset"));
    Json->SetStringField(TEXT("raw_sha1"), Recovery.Hash); Json->SetNumberField(TEXT("original_mode"), Original.st_mode & 0777);
    Json->SetStringField(TEXT("actor_label"), Entry.ActorLabel); Json->SetStringField(TEXT("actor_guid"), Entry.ActorGuid.ToString()); Json->SetStringField(TEXT("actor_path"), Entry.ActorPath);
    Json->SetStringField(TEXT("owning_map"), Entry.WorldName); Json->SetStringField(TEXT("head"), Before.Head); Json->SetStringField(TEXT("branch"), Before.Branch);
    Json->SetStringField(TEXT("restore"), TEXT("Close the editor without saving. Confirm the destination is absent and no newer work must be kept. Copy payload.uasset to root/path; apply original_mode if needed. Reopen the map. This restores saved working bytes only; staging, HEAD, stashes and locks stay separate. Keep this backup; no automatic cleanup."));
    if (!FJsonSerializer::Serialize(Json, TJsonWriterFactory<>::Create(&Recovery.Manifest)) ||
        !WriteExclusive(Folder.Value, "manifest.json", Recovery.Manifest) || fsync(Folder.Value) || fsync(Store.Value) || fsync(UeGit.Value) || fsync(Git.Value))
        return Failure(TEXT("Cannot durably preserve deletion manifest. Nothing removed."), Recovery);
    const auto Recheck = Repository.ValidateAssetSave(Permit, Entry.Path, Lease);
    struct stat Current, ParentNow, ParentHeld;
    const FString LateBinding = ValidateExternalActorBinding(Plan, Entry.Path, Entry.Package.Get());
    if (!LateBinding.IsEmpty() || stat(TCHAR_TO_UTF8(*FPaths::GetPath(Entry.Filename)), &ParentNow) || fstat(Parent.Value, &ParentHeld) ||
        ParentNow.st_dev != ParentHeld.st_dev || ParentNow.st_ino != ParentHeld.st_ino || !Recheck.Ok() || fstatat(Parent.Value, TCHAR_TO_UTF8(*Parts.Last()), &Current, AT_SYMLINK_NOFOLLOW) ||
        !SameFile(Original, Current) || Digest(Source.Value) != Recovery.Hash)
        return Failure(TEXT("Actor bytes, path, staging or lock changed during backup. Nothing removed. ") + Recheck.Error, Recovery);
    // Exclusive durable marker before moving a working file. Rename retains the
    // exact original inode; a racing replacement is quarantined, never unlinked.
    if (!WriteExclusive(MarkerDir.Value, "recovery-required.txt", Recovery.Manifest))
        return Failure(TEXT("Another recovery is active, or its marker could not be persisted. Nothing removed."), Recovery);
    Recovery.bActive = true;
    if (renameatx_np(Parent.Value, TCHAR_TO_UTF8(*Parts.Last()), Folder.Value, "removed.uasset", RENAME_EXCL))
        return Failure(TEXT("Actor removal did not complete; inspect active recovery."), Recovery);
    struct stat Moved, Absent;
    const bool bAbsent = fstatat(Parent.Value, TCHAR_TO_UTF8(*Parts.Last()), &Absent, AT_SYMLINK_NOFOLLOW) != 0 && errno == ENOENT;
    FFd Removed(openat(Folder.Value, "removed.uasset", O_RDONLY | O_NOFOLLOW | O_CLOEXEC));
    if (!bAbsent || Removed.Value < 0 || fstat(Removed.Value, &Moved) || !SameFile(Original, Moved) || Digest(Removed.Value) != Recovery.Hash ||
        fsync(Removed.Value) || fsync(Parent.Value) || fsync(Folder.Value))
        return Failure(TEXT("Removal identity or persistence could not be confirmed. Preserve both recovery files and inspect the destination."), Recovery);
    GitWorkspace::FResult Result; Result.Code = 0; return Result;
}
GitWorkspace::FResult CompleteExternalActorDeletion(FActorDeletionRecovery& Recovery, GitWorkspace::FRepository& Repository,
    const GitWorkspaceSession::FLease& Lease)
{
    const auto Local = Repository.Refresh(); const auto Stashes = Repository.ListStashes();
    FString Marker, Manifest;
    FFd Payload(open(TCHAR_TO_UTF8(*FPaths::Combine(Recovery.Folder, TEXT("payload.uasset"))), O_RDONLY | O_NOFOLLOW | O_CLOEXEC));
    FFd Removed(open(TCHAR_TO_UTF8(*FPaths::Combine(Recovery.Folder, TEXT("removed.uasset"))), O_RDONLY | O_NOFOLLOW | O_CLOEXEC));
    struct stat Exists;
    if (!Recovery.bActive || !Lease.IsExclusiveFor(Local.Root) || !Local.bValid || Local.Head != Recovery.Head || Local.Branch != Recovery.Branch ||
        Local.IndexEntries != Recovery.IndexEntries || !Stashes.bValid || Stashes.Fingerprint != Recovery.Stashes ||
        !FFileHelper::LoadFileToString(Marker, *Recovery.Marker) || Marker != Recovery.Manifest ||
        !FFileHelper::LoadFileToString(Manifest, *FPaths::Combine(Recovery.Folder, TEXT("manifest.json"))) || Manifest != Recovery.Manifest ||
        Payload.Value < 0 || Removed.Value < 0 || Digest(Payload.Value) != Recovery.Hash || Digest(Removed.Value) != Recovery.Hash ||
        lstat(TCHAR_TO_UTF8(*FPaths::Combine(Local.Root, Recovery.Path)), &Exists) == 0 || errno != ENOENT)
        return Failure(TEXT("Deletion/registry completion changed local state or its backup. Keep recovery active."), Recovery);
    FFd Folder(open(TCHAR_TO_UTF8(*Recovery.Folder), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (Folder.Value < 0 || !WriteExclusive(Folder.Value, "complete.json", Recovery.Manifest))
        return Failure(TEXT("Cannot persist deletion completion. Keep recovery active."), Recovery);
    FFd MarkerDir(open(TCHAR_TO_UTF8(*FPaths::GetPath(Recovery.Marker)), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (MarkerDir.Value < 0 || unlinkat(MarkerDir.Value, "recovery-required.txt", 0) || fsync(MarkerDir.Value))
        return Failure(TEXT("Deletion is preserved but recovery clearance failed."), Recovery);
    Recovery.bActive = false;
    GitWorkspace::FResult Result; Result.Code = 0; return Result;
}
}
#endif
