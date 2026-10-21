// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#include "GitRepository.h"
#include "GitParse.h"

#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"

namespace GitCore
{
	bool FGitRepository::ReadHistory(int32 Limit, TArray<FHistoryCommit>& Out, bool& bOutHasMore, FString& OutError) const
	{
		Out.Reset();
		bOutHasMore = false;
		const FProcessResult Head = Run(ECallKind::LocalQuick, {TEXT("rev-parse"), TEXT("--verify"), TEXT("--quiet"), TEXT("HEAD")});
		if (!Head.Succeeded())
		{
			// No commits yet.
			return true;
		}
		const FProcessResult Log = Run(ECallKind::LocalScan, {TEXT("log"), TEXT("-z"), TEXT("--no-notes"), TEXT("--encoding=UTF-8"),
			FString::Printf(TEXT("--max-count=%d"), Limit + 1), LogFormat, Head.StdoutText().TrimStartAndEnd(), TEXT("--")});
		if (!Log.Succeeded() || !ParseLog(Log.Stdout, Out, OutError))
		{
			if (!Log.Succeeded())
			{
				OutError = DescribeFailure(Log, TEXT("Reading history"));
			}
			return false;
		}
		bOutHasMore = Out.Num() > Limit;
		if (bOutHasMore)
		{
			Out.SetNum(Limit);
		}
		return true;
	}

	bool FGitRepository::ReadCommit(const FString& Oid, FCommitDetails& Out, FString& OutError) const
	{
		if (!IsObjectId(Oid))
		{
			OutError = TEXT("Select a commit from the history list.");
			return false;
		}
		const FProcessResult Log = Run(ECallKind::LocalScan, {TEXT("log"), TEXT("-z"), TEXT("--no-walk"), TEXT("--no-notes"), TEXT("--encoding=UTF-8"), LogFormat, Oid, TEXT("--")});
		TArray<FHistoryCommit> Commits;
		if (!Log.Succeeded() || !ParseLog(Log.Stdout, Commits, OutError) || Commits.Num() != 1 || Commits[0].Oid != Oid)
		{
			OutError = Log.Succeeded() ? TEXT("This commit could not be read.") : DescribeFailure(Log, TEXT("Reading the commit"));
			return false;
		}
		Out.Commit = MoveTemp(Commits[0]);
		const FProcessResult Message = Run(ECallKind::LocalScan, {TEXT("show"), TEXT("--no-patch"), TEXT("--no-notes"), TEXT("--encoding=UTF-8"), TEXT("--format=%B"), Oid, TEXT("--")});
		if (!Message.Succeeded())
		{
			OutError = DescribeFailure(Message, TEXT("Reading the commit message"));
			return false;
		}
		Out.Message = Message.StdoutText().TrimEnd();
		TArray<FString> Args = Out.Commit.Parents.IsEmpty()
			? TArray<FString>{TEXT("diff-tree"), TEXT("--root"), TEXT("--no-commit-id"), TEXT("-r")}
			: TArray<FString>{TEXT("diff"), Out.Commit.Parents[0]};
		Args.Append({TEXT("--raw"), TEXT("--no-ext-diff"), TEXT("--no-textconv"), TEXT("--no-abbrev"), TEXT("--no-renames"), TEXT("-z"), Oid, TEXT("--")});
		const FProcessResult Diff = Run(ECallKind::LocalScan, Args);
		if (!Diff.Succeeded() || !ParseRawDiff(Diff.Stdout, Out.Files, OutError))
		{
			if (!Diff.Succeeded())
			{
				OutError = DescribeFailure(Diff, TEXT("Listing the commit's files"));
			}
			return false;
		}
		return true;
	}

	bool FGitRepository::ReadFileHistory(const FString& Path, int32 Limit, TArray<FFileRevision>& Out, FString& OutError) const
	{
		Out.Reset();
		const FProcessResult Head = Run(ECallKind::LocalQuick, {TEXT("rev-parse"), TEXT("--verify"), TEXT("--quiet"), TEXT("HEAD")});
		if (!Head.Succeeded())
		{
			return true;
		}
		const FProcessResult Log = Run(ECallKind::LocalScan, {TEXT("log"), TEXT("--follow"), TEXT("-z"), TEXT("--name-status"), TEXT("--no-notes"), TEXT("--encoding=UTF-8"),
			FString::Printf(TEXT("--max-count=%d"), Limit), FileLogFormat, TEXT("HEAD"), TEXT("--"), Path});
		if (!Log.Succeeded() || !ParseFileLog(Log.Stdout, Out, OutError))
		{
			if (!Log.Succeeded())
			{
				OutError = DescribeFailure(Log, TEXT("Reading file history"));
			}
			return false;
		}
		for (FFileRevision& Revision : Out)
		{
			if (Revision.Path.IsEmpty())
			{
				Revision.Path = Path;
			}
		}
		return true;
	}

	FOpResult FGitRepository::CommitDiff(const FString& Oid, const FString& Path, FString& OutText) const
	{
		if (!IsObjectId(Oid))
		{
			return FOpResult::Failure(TEXT("Select a commit."));
		}
		const FProcessResult Result = Run(ECallKind::LocalScan,
			{TEXT("show"), TEXT("--no-ext-diff"), TEXT("--no-textconv"), TEXT("--no-color"), TEXT("--format="), TEXT("-M"), Oid, TEXT("--"), Path});
		if (!Result.Succeeded())
		{
			return FOpResult::Failure(DescribeFailure(Result, TEXT("Comparing")), Result.ErrorText());
		}
		OutText = Result.StdoutText();
		return FOpResult::Success();
	}

	FOpResult FGitRepository::ExportRevision(const FString& Revision, const FString& Path, const FString& Destination) const
	{
		// A unique partial name lets two exports of the same revision run at once.
		const FString Partial = Destination + TEXT(".") + FGuid::NewGuid().ToString(EGuidFormats::Digits) + TEXT(".part");
		IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
		PlatformFile.CreateDirectoryTree(*FPaths::GetPath(Destination));
		TUniquePtr<IFileHandle> File(PlatformFile.OpenWrite(*Partial));
		if (!File.IsValid())
		{
			return FOpResult::Failure(FString::Printf(TEXT("Could not create %s."), *Partial));
		}
		bool bWriteFailed = false;
		FGitCall Call;
		Call.Kind = ECallKind::Blob;
		// --filters applies the LFS smudge filter, so the file holds real asset bytes.
		Call.Args = {TEXT("cat-file"), TEXT("--filters"), Revision + TEXT(":") + Path};
		Call.StdoutSink = [&File, &bWriteFailed](const uint8* Data, int32 Size)
		{
			bWriteFailed = !File->Write(Data, Size);
			return !bWriteFailed;
		};
		const FProcessResult Result = Run(Call);
		File.Reset();
		if (!Result.Succeeded() || bWriteFailed)
		{
			PlatformFile.DeleteFile(*Partial);
			return FOpResult::Failure(bWriteFailed ? TEXT("Could not write the exported revision to disk.") : DescribeFailure(Result, TEXT("Exporting the revision")), Result.ErrorText());
		}
		PlatformFile.DeleteFile(*Destination);
		if (!PlatformFile.MoveFile(*Destination, *Partial))
		{
			PlatformFile.DeleteFile(*Partial);
			return FOpResult::Failure(FString::Printf(TEXT("Could not create %s."), *Destination));
		}
		return FOpResult::Success();
	}
}
