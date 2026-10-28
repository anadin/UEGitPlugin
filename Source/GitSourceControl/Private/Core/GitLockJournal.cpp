// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#include "GitLockJournal.h"

#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace GitCore
{
	namespace
	{
		bool IsSafeId(const FString& LockId)
		{
			if (LockId.IsEmpty() || LockId.Len() > 128)
			{
				return false;
			}
			for (const TCHAR Character : LockId)
			{
				if (!FChar::IsAlnum(Character) && Character != TEXT('-') && Character != TEXT('_'))
				{
					return false;
				}
			}
			return true;
		}
	}

	FGitLockJournal::FGitLockJournal(FString InCommonDir, FString InWorktreeRoot)
		: CommonDir(MoveTemp(InCommonDir))
		, WorktreeRoot(MoveTemp(InWorktreeRoot))
	{
	}

	FString FGitLockJournal::Directory(const FString& Endpoint) const
	{
		const FTCHARToUTF8 Utf8(*Endpoint);
		uint8 Hash[20];
		FSHA1::HashBuffer(Utf8.Get(), Utf8.Length(), Hash);
		return CommonDir / TEXT("uegit/locks-v2") / BytesToHex(Hash, 20);
	}

	bool FGitLockJournal::Record(const FString& Endpoint, const FLockInfo& Lock, FString& OutError) const
	{
		if (!IsSafeId(Lock.Id))
		{
			OutError = TEXT("The lock server returned an unusable lock id.");
			return false;
		}
		const TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("id"), Lock.Id);
		Json->SetStringField(TEXT("path"), Lock.Path);
		Json->SetStringField(TEXT("endpoint"), Endpoint);
		Json->SetStringField(TEXT("worktree"), WorktreeRoot);
		Json->SetStringField(TEXT("acquired"), FDateTime::UtcNow().ToIso8601());
		FString Text;
		FJsonSerializer::Serialize(Json, TJsonWriterFactory<>::Create(&Text));

		const FString Folder = Directory(Endpoint);
		const FString Final = Folder / (Lock.Id + TEXT(".json"));
		const FString Temporary = Folder / (Lock.Id + TEXT(".") + FGuid::NewGuid().ToString(EGuidFormats::Digits) + TEXT(".tmp"));
		IFileManager::Get().MakeDirectory(*Folder, true);
		if (!FFileHelper::SaveStringToFile(Text, *Temporary, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM) ||
			!IFileManager::Get().Move(*Final, *Temporary, true, true))
		{
			IFileManager::Get().Delete(*Temporary, false, true, true);
			OutError = FString::Printf(TEXT("Could not write the lock record %s."), *Final);
			return false;
		}
		return true;
	}

	void FGitLockJournal::Remove(const FString& Endpoint, const FString& LockId) const
	{
		if (IsSafeId(LockId))
		{
			IFileManager::Get().Delete(*(Directory(Endpoint) / (LockId + TEXT(".json"))), false, true, true);
		}
	}

	bool FGitLockJournal::IsOurs(const FString& Endpoint, const FString& LockId) const
	{
		if (!IsSafeId(LockId))
		{
			return false;
		}
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *(Directory(Endpoint) / (LockId + TEXT(".json")))))
		{
			return false;
		}
		TSharedPtr<FJsonObject> Json;
		FString Worktree;
		return FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Json) && Json.IsValid() &&
			Json->TryGetStringField(TEXT("worktree"), Worktree) && Worktree == WorktreeRoot;
	}

	void FGitLockJournal::Prune(const FString& Endpoint, const TSet<FString>& HeldLockIds) const
	{
		TArray<FString> Records;
		IFileManager::Get().FindFiles(Records, *(Directory(Endpoint) / TEXT("*.json")), true, false);
		for (const FString& Record : Records)
		{
			const FString LockId = FPaths::GetBaseFilename(Record);
			if (!HeldLockIds.Contains(LockId) && IsOurs(Endpoint, LockId))
			{
				Remove(Endpoint, LockId);
			}
		}
	}
}
