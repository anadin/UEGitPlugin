// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#include "GitWorkspaceRepository.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "Serialization/JsonSerializer.h"

namespace GitWorkspace
{
namespace
{
TSharedRef<FJsonObject> LockRecord(const FLockSnapshot& Context, const FLock& Lock, const FString& GitDir)
{
    auto Record = MakeShared<FJsonObject>();
    Record->SetNumberField(TEXT("version"), 1);
    Record->SetStringField(TEXT("root"), Context.Root);
    Record->SetStringField(TEXT("gitDir"), GitDir);
    Record->SetStringField(TEXT("remote"), Context.Remote);
    Record->SetStringField(TEXT("endpoint"), Context.Endpoint);
    Record->SetStringField(TEXT("branch"), Context.Branch);
    Record->SetStringField(TEXT("context"), Context.Context);
    Record->SetStringField(TEXT("path"), Lock.Path);
    Record->SetStringField(TEXT("id"), Lock.Id);
    Record->SetStringField(TEXT("lockedAt"), Lock.LockedAt);
    return Record;
}
}

bool FRepository::LockRecordPath(const FLockSnapshot& Context, const FLock& Lock, FString& File, FString& GitDir, FString& Error) const
{
    // --absolute-git-dir is specific to this worktree, including linked worktrees
    // and submodules. Never store acquisition authority in the shared common-dir.
    const auto Directory = Git({TEXT("rev-parse"), TEXT("--absolute-git-dir")});
    GitDir = Directory.Text(); GitDir.RemoveFromEnd(TEXT("\n")); GitDir.RemoveFromEnd(TEXT("\r"));
    if (!Directory.Ok() || GitDir.IsEmpty() || FPaths::IsRelative(GitDir) || !IFileManager::Get().DirectoryExists(*GitDir))
    { Error = TEXT("Cannot locate this worktree's Git directory for lock acquisition records."); return false; }
    const FString Identity = Context.Context + TEXT("\n") + Lock.Path + TEXT("\n") + Lock.Id;
    FTCHARToUTF8 Utf8(*Identity); uint8 Hash[20]; FSHA1::HashBuffer(Utf8.Get(), Utf8.Length(), Hash);
    File = FPaths::Combine(GitDir, TEXT("uegit/lock-acquisitions-v1"), BytesToHex(Hash, 20) + TEXT(".json"));
    return true;
}

bool FRepository::ReadLockRecord(const FLockSnapshot& Context, const FLock& Lock, FString& Error) const
{
    FString File, GitDir;
    if (!LockRecordPath(Context, Lock, File, GitDir, Error)) return false;
    const int64 Size = IFileManager::Get().FileSize(*File);
    if (Size < 0)
    { Error = TEXT("No acquisition record matches this lock in this worktree and branch. It may predate this feature or belong to another clone/worktree. Review and release it externally."); return false; }
    FString Json; TSharedPtr<FJsonObject> Record; double Version = 0;
    if (Size > 65536 || !FFileHelper::LoadFileToString(Json, *File) ||
        !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Record) || !Record ||
        !Record->TryGetNumberField(TEXT("version"), Version) || Version != 1)
    { Error = TEXT("Lock acquisition record is unreadable or unsupported. Keep the lock and review it externally."); return false; }
    const auto Expected = LockRecord(Context, Lock, GitDir);
    for (const auto& Field : Expected->Values)
    {
        if (Field.Key == TEXT("version")) continue;
        FString Value;
        if (!Record->TryGetStringField(Field.Key, Value) || Value != Field.Value->AsString())
        { Error = TEXT("Lock acquisition record does not match the verified server lock and worktree. Nothing unlocked."); return false; }
    }
    return true;
}

bool FRepository::WriteLockRecord(const FLockSnapshot& Context, const FLock& Lock, FString& Error) const
{
    FString File, GitDir;
    if (!LockRecordPath(Context, Lock, File, GitDir, Error)) return false;
    FString Json;
    if (!FJsonSerializer::Serialize(LockRecord(Context, Lock, GitDir), TJsonWriterFactory<>::Create(&Json)) ||
        Json.Len() > 16000 || !IFileManager::Get().MakeDirectory(*FPaths::GetPath(File), true))
    { Error = TEXT("Cannot prepare the worktree lock acquisition record."); return false; }
    // One immutable record per lock identity avoids read/modify/write races
    // between editors. Publish only a complete record in the same directory.
    const FString Temp = File + TEXT(".") + FGuid::NewGuid().ToString(EGuidFormats::Digits) + TEXT(".tmp");
    const bool bSaved = FFileHelper::SaveStringToFile(Json, *Temp, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
    const bool bPublished = bSaved && IFileManager::Get().Move(*File, *Temp, false, false, false, true);
    if (!bPublished)
    {
        IFileManager::Get().Delete(*Temp, false, false, true);
        Error = TEXT("Cannot persist the worktree lock acquisition record. The server lock remains held; review and release it externally.");
        return false;
    }
    return ReadLockRecord(Context, Lock, Error);
}

bool FRepository::RemoveLockRecord(const FLockSnapshot& Context, const FLock& Lock, FString& Error) const
{
    FString File, GitDir;
    if (!LockRecordPath(Context, Lock, File, GitDir, Error)) return false;
    if (!IFileManager::Get().Delete(*File, false, false, true))
    { Error = TEXT("Server confirms the lock was released, but its local acquisition record could not be removed."); return false; }
    return true;
}
}
