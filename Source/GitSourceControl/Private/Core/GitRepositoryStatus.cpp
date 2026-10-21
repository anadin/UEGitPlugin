// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#include "GitRepository.h"
#include "GitParse.h"

#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/Paths.h"

namespace GitCore
{
	namespace
	{
		/** Scoped status passes paths on the command line; larger requests use a full status. */
		constexpr int32 MaxScopedStatusPaths = 100;
		constexpr int32 AttributeBatchPaths = 8192;
	}

	bool FGitRepository::ReadStatus(FStatusSnapshot& Out, FString& OutError, bool bAllowIndexRefresh, const TArray<FString>* OnlyPaths) const
	{
		FGitCall Call;
		Call.Kind = ECallKind::LocalScan;
		Call.bOptionalLocks = bAllowIndexRefresh;
		Call.Args = {TEXT("status"), TEXT("--porcelain=v2"), TEXT("-z"), TEXT("--branch"), TEXT("--untracked-files=all"), TEXT("--ignore-submodules=none")};
		if (OnlyPaths && OnlyPaths->Num() > 0 && OnlyPaths->Num() <= MaxScopedStatusPaths)
		{
			Call.Args.Add(TEXT("--"));
			Call.Args.Append(*OnlyPaths);
		}
		const FString TokenBefore = ReadIndexToken();
		const FProcessResult Result = Run(Call);
		if (!Result.Succeeded())
		{
			OutError = DescribeFailure(Result, TEXT("Reading repository status"));
			return false;
		}
		Out = FStatusSnapshot();
		if (!ParseStatus(Result.Stdout, Out, OutError))
		{
			return false;
		}
		Out.bOperationInProgress = IsOperationInProgress();
		Out.IndexToken = ReadIndexToken();
		Out.StagedToken = Out.ComputeStagedToken();
		// Status itself may refresh the index; any other change means a concurrent writer.
		if (!bAllowIndexRefresh && Out.IndexToken != TokenBefore)
		{
			OutError = TEXT("The repository changed while its status was read. Refresh again.");
			return false;
		}
		return true;
	}

	bool FGitRepository::ReadTrackedPaths(TSet<FString>& Out, FString& OutError) const
	{
		const FProcessResult Result = Run(ECallKind::LocalScan, {TEXT("ls-files"), TEXT("-z")});
		TArray<FString> Paths;
		if (!Result.Succeeded() || !SplitNulRecords(Result.Stdout, Paths))
		{
			OutError = Result.Succeeded() ? TEXT("The tracked-file list was incomplete.") : DescribeFailure(Result, TEXT("Listing tracked files"));
			return false;
		}
		Out.Reset();
		Out.Reserve(Paths.Num());
		for (FString& Path : Paths)
		{
			Out.Add(MoveTemp(Path));
		}
		return true;
	}

	bool FGitRepository::ReadAttributes(const TArray<FString>& Paths, TMap<FString, FFileAttributes>& Out, FString& OutError) const
	{
		for (int32 Start = 0; Start < Paths.Num(); Start += AttributeBatchPaths)
		{
			const int32 End = FMath::Min(Start + AttributeBatchPaths, Paths.Num());
			FGitCall Call;
			Call.Kind = ECallKind::LocalScan;
			Call.Args = {TEXT("check-attr"), TEXT("--stdin"), TEXT("-z"), TEXT("filter"), TEXT("lockable")};
			for (int32 Index = Start; Index < End; ++Index)
			{
				AppendUtf8(Call.Stdin, Paths[Index]);
				Call.Stdin.Add(0);
			}
			const FProcessResult Result = Run(Call);
			TArray<FString> Fields;
			if (!Result.Succeeded() || !SplitNulRecords(Result.Stdout, Fields) || Fields.Num() != (End - Start) * 6)
			{
				OutError = Result.Succeeded() ? TEXT("Git returned an incomplete attribute list.") : DescribeFailure(Result, TEXT("Reading file attributes"));
				return false;
			}
			for (int32 Index = Start; Index < End; ++Index)
			{
				const int32 Offset = (Index - Start) * 6;
				if (Fields[Offset] != Paths[Index] || Fields[Offset + 1] != TEXT("filter") || Fields[Offset + 3] != Paths[Index] || Fields[Offset + 4] != TEXT("lockable"))
				{
					OutError = TEXT("Git returned attributes for an unexpected path.");
					return false;
				}
				FFileAttributes& Attributes = Out.FindOrAdd(Paths[Index]);
				Attributes.bLfs = Fields[Offset + 2] == TEXT("lfs");
				Attributes.bLockable = Fields[Offset + 5] == TEXT("set");
			}
		}
		return true;
	}

	FString FGitRepository::ReadIndexToken() const
	{
		// The index ends with a checksum of its contents; together with size and time it
		// identifies the index without asking git to list every entry.
		const FString IndexFile = GitDir + TEXT("/index");
		IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
		const FFileStatData Stat = PlatformFile.GetStatData(*IndexFile);
		if (!Stat.bIsValid)
		{
			return TEXT("no-index");
		}
		FString Token = FString::Printf(TEXT("%lld:%lld"), Stat.FileSize, Stat.ModificationTime.GetTicks());
		TUniquePtr<IFileHandle> Handle(PlatformFile.OpenRead(*IndexFile, true));
		if (Handle.IsValid() && Stat.FileSize >= 32 && Handle->Seek(Stat.FileSize - 32))
		{
			uint8 Trailer[32];
			if (Handle->Read(Trailer, sizeof(Trailer)))
			{
				Token += TEXT(":") + BytesToHex(Trailer, sizeof(Trailer));
			}
		}
		return Token;
	}

	bool FGitRepository::IsOperationInProgress() const
	{
		IFileManager& Files = IFileManager::Get();
		for (const TCHAR* Marker : {TEXT("MERGE_HEAD"), TEXT("CHERRY_PICK_HEAD"), TEXT("REVERT_HEAD")})
		{
			if (Files.FileExists(*(GitDir / Marker)))
			{
				return true;
			}
		}
		return Files.DirectoryExists(*(GitDir / TEXT("rebase-merge"))) || Files.DirectoryExists(*(GitDir / TEXT("rebase-apply")));
	}
}
