// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#include "GitParse.h"
#include "Process/GitProcess.h"

#include "Dom/JsonObject.h"
#include "Misc/SecureHash.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace GitCore
{
	const TCHAR* const LogFormat = TEXT("--format=%H%x00%P%x00%an%x00%aI%x00%s");
	const TCHAR* const FileLogFormat = TEXT("--format=%x01%H%x00%an%x00%aI%x00%s");

	bool SplitNulRecords(const TArray<uint8>& Bytes, TArray<FString>& OutRecords)
	{
		OutRecords.Reset();
		int32 Start = 0;
		for (int32 Index = 0; Index < Bytes.Num(); ++Index)
		{
			if (Bytes[Index] == 0)
			{
				OutRecords.Add(DecodeUtf8(Bytes.GetData() + Start, Index - Start));
				Start = Index + 1;
			}
		}
		return Start == Bytes.Num();
	}

	bool IsObjectId(const FString& Value)
	{
		if (Value.Len() != 40 && Value.Len() != 64)
		{
			return false;
		}
		for (const TCHAR Character : Value)
		{
			if (!FChar::IsHexDigit(Character))
			{
				return false;
			}
		}
		return true;
	}

	bool IsSafeRelativePath(const FString& Path)
	{
		if (Path.IsEmpty() || Path.StartsWith(TEXT("/")) || Path.Contains(TEXT("\\")) || Path.Contains(TEXT(":")))
		{
			return false;
		}
		TArray<FString> Parts;
		Path.ParseIntoArray(Parts, TEXT("/"), false);
		for (const FString& Part : Parts)
		{
			if (Part.IsEmpty() || Part == TEXT(".") || Part == TEXT("..") || Part.Equals(TEXT(".git"), ESearchCase::IgnoreCase))
			{
				return false;
			}
		}
		return true;
	}

	const FStatusEntry* FStatusSnapshot::Find(const FString& Path) const
	{
		const int32* Index = ByPath.Find(Path);
		return Index ? &Entries[*Index] : nullptr;
	}

	bool FStatusSnapshot::HasConflicts() const
	{
		return Entries.ContainsByPredicate([](const FStatusEntry& Entry) { return Entry.bConflict; });
	}

	int32 FStatusSnapshot::StagedCount() const
	{
		int32 Count = 0;
		for (const FStatusEntry& Entry : Entries)
		{
			Count += Entry.HasStaged() ? 1 : 0;
		}
		return Count;
	}

	FString FStatusSnapshot::ComputeStagedToken() const
	{
		// One line per path the index changes. A staged rename is written as a deletion plus an
		// addition, so it reads the same whether or not git detected the rename.
		static const FString Deleted = TEXT("000000 0000000000000000000000000000000000000000");
		TArray<FString> Lines;
		for (const FStatusEntry& Entry : Entries)
		{
			if (Entry.bConflict)
			{
				Lines.Add(Entry.Path + TEXT(" conflict"));
				continue;
			}
			if (!Entry.HasStaged())
			{
				continue;
			}
			if (!Entry.OriginalPath.IsEmpty() && Entry.Index == TEXT('R'))
			{
				Lines.Add(Entry.OriginalPath + TEXT(" ") + Deleted);
			}
			Lines.Add(Entry.Path + TEXT(" ") + Entry.IndexMode + TEXT(" ") + Entry.IndexOid);
		}
		Lines.Sort();
		const FString Text = Head + TEXT("\n") + FString::Join(Lines, TEXT("\n"));
		FTCHARToUTF8 Utf8(*Text);
		return FSHA1::HashBuffer(Utf8.Get(), Utf8.Length()).ToString();
	}

	void FStatusSnapshot::BuildLookup()
	{
		ByPath.Reset();
		ByPath.Reserve(Entries.Num());
		for (int32 Index = 0; Index < Entries.Num(); ++Index)
		{
			ByPath.Add(Entries[Index].Path, Index);
		}
	}

	double FLockSnapshot::AgeSeconds() const
	{
		if (!bVerified)
		{
			return TNumericLimits<double>::Max();
		}
		return (FDateTime::UtcNow() - VerifiedAtUtc).GetTotalSeconds();
	}

	bool ParseStatus(const TArray<uint8>& Bytes, FStatusSnapshot& Out, FString& OutError)
	{
		TArray<FString> Records;
		if (!SplitNulRecords(Bytes, Records))
		{
			OutError = TEXT("Git status output was incomplete.");
			return false;
		}
		Out.Entries.Reset();
		for (int32 Index = 0; Index < Records.Num(); ++Index)
		{
			const FString& Record = Records[Index];
			if (Record.StartsWith(TEXT("# branch.oid ")))
			{
				Out.Head = Record.Mid(13);
				Out.bUnborn = Out.Head == TEXT("(initial)");
				if (Out.bUnborn)
				{
					Out.Head.Reset();
				}
				continue;
			}
			if (Record.StartsWith(TEXT("# branch.head ")))
			{
				Out.Branch = Record.Mid(14);
				if (Out.Branch == TEXT("(detached)"))
				{
					Out.Branch.Reset();
				}
				continue;
			}
			if (Record.StartsWith(TEXT("# branch.upstream ")))
			{
				Out.Upstream = Record.Mid(18);
				continue;
			}
			if (Record.StartsWith(TEXT("# branch.ab ")))
			{
				TArray<FString> Counts;
				Record.Mid(12).ParseIntoArray(Counts, TEXT(" "));
				if (Counts.Num() != 2 || !Counts[0].StartsWith(TEXT("+")) || !Counts[1].StartsWith(TEXT("-")))
				{
					OutError = TEXT("Malformed ahead/behind record in git status.");
					return false;
				}
				Out.Ahead = FCString::Atoi(*Counts[0].Mid(1));
				Out.Behind = FCString::Atoi(*Counts[1].Mid(1));
				continue;
			}
			if (Record.StartsWith(TEXT("# ")))
			{
				// Porcelain v2 headers are extensible; unknown ones are ignored.
				continue;
			}
			if (Record.StartsWith(TEXT("! ")))
			{
				continue;
			}

			FStatusEntry Entry;
			if (Record.StartsWith(TEXT("? ")))
			{
				Entry.Path = Record.Mid(2);
				Entry.bUntracked = true;
				Entry.Working = TEXT('?');
			}
			else if (Record.StartsWith(TEXT("1 ")) || Record.StartsWith(TEXT("2 ")) || Record.StartsWith(TEXT("u ")))
			{
				const TCHAR Kind = Record[0];
				const int32 FieldCount = Kind == TEXT('1') ? 8 : (Kind == TEXT('2') ? 9 : 10);
				TArray<FString> Fields;
				int32 Start = 0;
				for (int32 Field = 0; Field < FieldCount; ++Field)
				{
					const int32 End = Record.Find(TEXT(" "), ESearchCase::CaseSensitive, ESearchDir::FromStart, Start);
					if (End == INDEX_NONE)
					{
						OutError = TEXT("Malformed git status record.");
						return false;
					}
					Fields.Add(Record.Mid(Start, End - Start));
					Start = End + 1;
				}
				if (Fields[1].Len() != 2 || Fields[2].Len() != 4)
				{
					OutError = TEXT("Malformed git status flags.");
					return false;
				}
				Entry.Index = Fields[1][0];
				Entry.Working = Fields[1][1];
				Entry.bConflict = Kind == TEXT('u');
				if (Kind != TEXT('u'))
				{
					// "1 XY sub mH mI mW hH hI path" and "2 XY sub mH mI mW hH hI Xscore path"
					Entry.IndexMode = Fields[4];
					Entry.IndexOid = Fields[7];
				}
				Entry.bSubmodule = Fields[2][0] == TEXT('S');
				Entry.Path = Record.Mid(Start);
				if (Kind == TEXT('2'))
				{
					if (++Index >= Records.Num())
					{
						OutError = TEXT("Git status omitted a rename source path.");
						return false;
					}
					Entry.OriginalPath = Records[Index];
				}
			}
			else
			{
				OutError = TEXT("Unrecognized git status record.");
				return false;
			}
			if (Entry.Path.IsEmpty())
			{
				OutError = TEXT("Git status returned an empty path.");
				return false;
			}
			Out.Entries.Add(MoveTemp(Entry));
		}
		Out.BuildLookup();
		return true;
	}

	static bool ParseLockObject(const TSharedPtr<FJsonObject>& Object, FLockInfo& Out, FString& OutError)
	{
		FString LockedAt;
		if (!Object.IsValid() || !Object->TryGetStringField(TEXT("id"), Out.Id) || Out.Id.IsEmpty() ||
			!Object->TryGetStringField(TEXT("path"), Out.Path) || !IsSafeRelativePath(Out.Path) ||
			!Object->TryGetStringField(TEXT("locked_at"), LockedAt) || !FDateTime::ParseIso8601(*LockedAt, Out.LockedAt))
		{
			OutError = TEXT("The lock server returned an invalid lock record.");
			return false;
		}
		const TSharedPtr<FJsonObject>* Owner = nullptr;
		if (Object->TryGetObjectField(TEXT("owner"), Owner) && Owner && Owner->IsValid())
		{
			(*Owner)->TryGetStringField(TEXT("name"), Out.Owner);
		}
		if (Out.Owner.IsEmpty())
		{
			Out.Owner = TEXT("another user");
		}
		return true;
	}

	bool ParseVerifiedLocks(const FString& Json, TMap<FString, FLockInfo>& OutLocks, FString& OutError)
	{
		OutLocks.Reset();
		TSharedPtr<FJsonObject> Root;
		if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Root) || !Root.IsValid())
		{
			OutError = TEXT("The lock server response was not valid JSON.");
			return false;
		}
		FString Message;
		FString Cursor;
		if ((Root->TryGetStringField(TEXT("message"), Message) && !Message.IsEmpty()) ||
			(Root->TryGetStringField(TEXT("next_cursor"), Cursor) && !Cursor.IsEmpty()))
		{
			OutError = TEXT("Lock verification was incomplete or failed on the server.");
			return false;
		}
		TMap<FString, FLockInfo> Parsed;
		TSet<FString> Ids;
		for (const TCHAR* Group : {TEXT("ours"), TEXT("theirs")})
		{
			const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
			if (!Root->TryGetArrayField(Group, Entries) || !Entries)
			{
				OutError = TEXT("Lock verification did not report lock ownership.");
				return false;
			}
			for (const TSharedPtr<FJsonValue>& Value : *Entries)
			{
				const TSharedPtr<FJsonObject>* Object = nullptr;
				FLockInfo Lock;
				if (!Value.IsValid() || !Value->TryGetObject(Object) || !Object || !ParseLockObject(*Object, Lock, OutError))
				{
					if (OutError.IsEmpty())
					{
						OutError = TEXT("The lock server returned an invalid lock record.");
					}
					return false;
				}
				if (Parsed.Contains(Lock.Path) || Ids.Contains(Lock.Id))
				{
					OutError = TEXT("The lock server returned duplicate lock records.");
					return false;
				}
				Lock.bOurs = FCString::Strcmp(Group, TEXT("ours")) == 0;
				Ids.Add(Lock.Id);
				Parsed.Add(Lock.Path, MoveTemp(Lock));
			}
		}
		OutLocks = MoveTemp(Parsed);
		return true;
	}

	bool ParseLockResponse(const FString& Json, TArray<FLockInfo>& OutLocks, FString& OutError)
	{
		OutLocks.Reset();
		const FString Trimmed = Json.TrimStartAndEnd();
		TArray<TSharedPtr<FJsonValue>> Values;
		if (Trimmed.StartsWith(TEXT("[")))
		{
			if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Trimmed), Values))
			{
				OutError = TEXT("The lock response was not valid JSON.");
				return false;
			}
		}
		else
		{
			TSharedPtr<FJsonObject> Object;
			if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Trimmed), Object) || !Object.IsValid())
			{
				OutError = TEXT("The lock response was not valid JSON.");
				return false;
			}
			Values.Add(MakeShared<FJsonValueObject>(Object));
		}
		for (const TSharedPtr<FJsonValue>& Value : Values)
		{
			const TSharedPtr<FJsonObject>* Object = nullptr;
			FLockInfo Lock;
			if (!Value.IsValid() || !Value->TryGetObject(Object) || !Object || !ParseLockObject(*Object, Lock, OutError))
			{
				return false;
			}
			Lock.bOurs = true;
			OutLocks.Add(MoveTemp(Lock));
		}
		return true;
	}

	bool ParseLog(const TArray<uint8>& Bytes, TArray<FHistoryCommit>& OutCommits, FString& OutError)
	{
		OutCommits.Reset();
		TArray<FString> Fields;
		if (!SplitNulRecords(Bytes, Fields) || Fields.Num() % 5 != 0)
		{
			OutError = TEXT("Commit history output was incomplete.");
			return false;
		}
		TSet<FString> Seen;
		for (int32 Index = 0; Index < Fields.Num(); Index += 5)
		{
			FHistoryCommit Commit;
			Commit.Oid = Fields[Index];
			Commit.Author = Fields[Index + 2];
			Commit.Date = Fields[Index + 3];
			Commit.Subject = Fields[Index + 4];
			if (!Fields[Index + 1].IsEmpty())
			{
				Fields[Index + 1].ParseIntoArray(Commit.Parents, TEXT(" "));
			}
			FDateTime Date;
			bool bValid = IsObjectId(Commit.Oid) && !Seen.Contains(Commit.Oid) && FDateTime::ParseIso8601(*Commit.Date, Date);
			for (const FString& Parent : Commit.Parents)
			{
				bValid &= IsObjectId(Parent);
			}
			if (!bValid)
			{
				OutError = TEXT("Commit history contained an invalid record.");
				OutCommits.Reset();
				return false;
			}
			Seen.Add(Commit.Oid);
			OutCommits.Add(MoveTemp(Commit));
		}
		return true;
	}

	bool ParseFileLog(const TArray<uint8>& Bytes, TArray<FFileRevision>& OutRevisions, FString& OutError)
	{
		OutRevisions.Reset();
		// Each commit starts with 0x01, then four NUL-terminated fields, a newline and
		// NUL-terminated name-status records (status, path[, new path for renames]).
		TArray<TArrayView<const uint8>> Chunks;
		int32 Start = INDEX_NONE;
		for (int32 Index = 0; Index < Bytes.Num(); ++Index)
		{
			if (Bytes[Index] == 0x01)
			{
				if (Start != INDEX_NONE)
				{
					Chunks.Add(TArrayView<const uint8>(Bytes.GetData() + Start, Index - Start));
				}
				Start = Index + 1;
			}
		}
		if (Start != INDEX_NONE)
		{
			Chunks.Add(TArrayView<const uint8>(Bytes.GetData() + Start, Bytes.Num() - Start));
		}
		else if (Bytes.Num() > 0)
		{
			OutError = TEXT("File history output was malformed.");
			return false;
		}
		for (const TArrayView<const uint8>& Chunk : Chunks)
		{
			TArray<uint8> Copy(Chunk.GetData(), Chunk.Num());
			// Drop the newline that separates the formatted header from the file records.
			for (int32 Index = 0; Index < Copy.Num(); ++Index)
			{
				if (Copy[Index] == '\n' && Index > 0 && Copy[Index - 1] == 0)
				{
					Copy.RemoveAt(Index);
					break;
				}
			}
			TArray<FString> Fields;
			if (!SplitNulRecords(Copy, Fields) || Fields.Num() < 4)
			{
				OutError = TEXT("File history output was incomplete.");
				OutRevisions.Reset();
				return false;
			}
			FFileRevision Revision;
			Revision.Commit = Fields[0];
			Revision.Author = Fields[1];
			Revision.Subject = Fields[3];
			if (!IsObjectId(Revision.Commit) || !FDateTime::ParseIso8601(*Fields[2], Revision.Date))
			{
				OutError = TEXT("File history contained an invalid commit.");
				OutRevisions.Reset();
				return false;
			}
			if (Fields.Num() >= 6)
			{
				const FString& Status = Fields[4];
				Revision.Action = Status.IsEmpty() ? TEXT('M') : Status[0];
				const bool bTwoPaths = (Revision.Action == TEXT('R') || Revision.Action == TEXT('C')) && Fields.Num() >= 7;
				Revision.Path = bTwoPaths ? Fields[6] : Fields[5];
			}
			OutRevisions.Add(MoveTemp(Revision));
		}
		return true;
	}

	bool ParseRawDiff(const TArray<uint8>& Bytes, TArray<FHistoryChange>& OutChanges, FString& OutError)
	{
		OutChanges.Reset();
		TArray<FString> Fields;
		if (!SplitNulRecords(Bytes, Fields) || Fields.Num() % 2 != 0)
		{
			OutError = TEXT("The changed-file list was incomplete.");
			return false;
		}
		TSet<FString> Seen;
		for (int32 Index = 0; Index < Fields.Num(); Index += 2)
		{
			TArray<FString> Header;
			Fields[Index].ParseIntoArray(Header, TEXT(" "));
			FHistoryChange Change;
			Change.Path = Fields[Index + 1];
			const bool bShape = Header.Num() == 5 && Header[0].StartsWith(TEXT(":")) && Header[4].Len() == 1 &&
				FString(TEXT("AMDT")).Contains(Header[4]) && IsSafeRelativePath(Change.Path) && !Seen.Contains(Change.Path);
			if (!bShape || !IsObjectId(Header[2]) || !IsObjectId(Header[3]))
			{
				OutError = TEXT("The changed-file list contained an invalid record.");
				OutChanges.Reset();
				return false;
			}
			Change.OldMode = Header[0].Mid(1);
			Change.NewMode = Header[1];
			Change.OldOid = Header[2];
			Change.NewOid = Header[3];
			Change.Status = Header[4][0];
			Seen.Add(Change.Path);
			OutChanges.Add(MoveTemp(Change));
		}
		return true;
	}

	bool ParseNameList(const TArray<uint8>& Bytes, TSet<FString>& OutPaths)
	{
		OutPaths.Reset();
		TArray<FString> Records;
		if (!SplitNulRecords(Bytes, Records))
		{
			return false;
		}
		for (FString& Record : Records)
		{
			if (!Record.IsEmpty())
			{
				OutPaths.Add(MoveTemp(Record));
			}
		}
		return true;
	}
}
