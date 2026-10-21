// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#include "GitRepository.h"
#include "GitParse.h"

namespace GitCore
{
	namespace
	{
		bool IsStashSelector(const FString& Selector)
		{
			return Selector.StartsWith(TEXT("stash@{")) && Selector.EndsWith(TEXT("}"));
		}
	}

	bool FGitRepository::ReadStashes(TArray<FStashEntry>& Out, FString& OutError) const
	{
		Out.Reset();
		const FProcessResult Result = Run(ECallKind::LocalScan, {TEXT("stash"), TEXT("list"), TEXT("-z"), TEXT("--format=%gd%x00%H%x00%gs")});
		TArray<FString> Fields;
		if (!Result.Succeeded() || !SplitNulRecords(Result.Stdout, Fields) || Fields.Num() % 3 != 0)
		{
			OutError = Result.Succeeded() ? TEXT("The stash list was incomplete.") : DescribeFailure(Result, TEXT("Listing stashes"));
			return false;
		}
		for (int32 Index = 0; Index < Fields.Num(); Index += 3)
		{
			FStashEntry Entry;
			Entry.Selector = Fields[Index];
			Entry.Oid = Fields[Index + 1];
			Entry.Message = Fields[Index + 2];
			if (!IsStashSelector(Entry.Selector) || !IsObjectId(Entry.Oid))
			{
				OutError = TEXT("The stash list contained an invalid entry.");
				Out.Reset();
				return false;
			}
			Out.Add(MoveTemp(Entry));
		}
		return true;
	}

	bool FGitRepository::ReadStashPaths(const FString& Selector, TArray<FString>& Out, FString& OutError) const
	{
		if (!IsStashSelector(Selector))
		{
			OutError = TEXT("Select a stash.");
			return false;
		}
		const FProcessResult Result = Run(ECallKind::LocalScan, {TEXT("stash"), TEXT("show"), TEXT("--include-untracked"), TEXT("--name-only"), TEXT("-z"), Selector});
		if (!Result.Succeeded() || !SplitNulRecords(Result.Stdout, Out))
		{
			OutError = Result.Succeeded() ? TEXT("The stash's file list was incomplete.") : DescribeFailure(Result, TEXT("Reading the stash"));
			return false;
		}
		Out.RemoveAll([](const FString& Path) { return Path.IsEmpty(); });
		return true;
	}

	FOpResult FGitRepository::StashPush(const TArray<FString>& Paths, const FString& Message, bool bIncludeUntracked) const
	{
		TArray<FString> Args = {TEXT("stash"), TEXT("push")};
		if (bIncludeUntracked)
		{
			Args.Add(TEXT("--include-untracked"));
		}
		Args.Append({TEXT("-m"), Message.IsEmpty() ? FString(TEXT("Stashed in Unreal Editor")) : Message});
		FGitCall Call;
		// Putting files back to HEAD can need LFS downloads, so this has no timeout.
		Call.Kind = ECallKind::Integrate;
		// A global --literal-pathspecs stops `stash push --include-untracked` from removing the new files
		// it stashed, so each path is made literal with pathspec magic instead.
		Call.bLiteralPathspecs = false;
		Call.Args = MoveTemp(Args);
		if (!Paths.IsEmpty())
		{
			Call.Args.Append({TEXT("--pathspec-from-file=-"), TEXT("--pathspec-file-nul")});
			for (const FString& Path : Paths)
			{
				AppendUtf8(Call.Stdin, TEXT(":(literal)") + Path);
				Call.Stdin.Add(0);
			}
		}
		const FProcessResult Result = Run(Call);
		if (!Result.Succeeded())
		{
			return FOpResult::Failure(DescribeFailure(Result, TEXT("Stashing")), Result.ErrorText());
		}
		if (Result.StdoutText().Contains(TEXT("No local changes to save")))
		{
			return FOpResult::Failure(TEXT("There are no changes to stash."));
		}
		return FOpResult::Success();
	}

	FOpResult FGitRepository::StashApply(const FString& Selector, bool bRestoreIndex, bool bDrop) const
	{
		if (!IsStashSelector(Selector))
		{
			return FOpResult::Failure(TEXT("Select a stash."));
		}
		TArray<FString> Args = {TEXT("stash"), bDrop ? TEXT("pop") : TEXT("apply")};
		if (bRestoreIndex)
		{
			Args.Add(TEXT("--index"));
		}
		Args.Add(Selector);
		FGitCall Call;
		Call.Kind = ECallKind::Integrate;
		Call.bLiteralPathspecs = false;
		Call.Args = MoveTemp(Args);
		const FProcessResult Result = Run(Call);
		if (!Result.Succeeded())
		{
			const FString Output = Result.StdoutText() + Result.ErrorText();
			if (Output.Contains(TEXT("CONFLICT")) || Output.Contains(TEXT("conflict")))
			{
				return FOpResult::Failure(TEXT("The stash conflicts with your current files. It was kept; resolve the conflicts or revert them."), Output);
			}
			if (Output.Contains(TEXT("would be overwritten")) || Output.Contains(TEXT("already exists")))
			{
				return FOpResult::Failure(TEXT("Applying the stash would overwrite local changes. Commit, stash or revert those files first; the stash was kept."), Output);
			}
			return FOpResult::Failure(DescribeFailure(Result, TEXT("Applying the stash")), Output);
		}
		return FOpResult::Success();
	}

	FOpResult FGitRepository::StashDrop(const FString& Selector) const
	{
		if (!IsStashSelector(Selector))
		{
			return FOpResult::Failure(TEXT("Select a stash."));
		}
		const FProcessResult Result = Run(ECallKind::LocalMutate, {TEXT("stash"), TEXT("drop"), Selector});
		return Result.Succeeded() ? FOpResult::Success() : FOpResult::Failure(DescribeFailure(Result, TEXT("Deleting the stash")), Result.ErrorText());
	}
}
