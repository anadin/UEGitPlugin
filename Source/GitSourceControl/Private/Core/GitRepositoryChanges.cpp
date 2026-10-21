// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#include "GitRepository.h"
#include "GitParse.h"

#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace GitCore
{
	namespace
	{
		constexpr int32 PathsPerArgumentBatch = 100;

		/** A temporary file removed when it goes out of scope. */
		struct FScopedTempFile
		{
			FString Path = FPaths::CreateTempFilename(FPlatformProcess::UserTempDir(), TEXT("git-workspace-"), TEXT(".txt"));
			~FScopedTempFile() { IFileManager::Get().Delete(*Path, false, true, true); }
		};

		FString CurrentHead(const FGitRepository& Repository)
		{
			const FProcessResult Head = Repository.Run(ECallKind::LocalQuick, {TEXT("rev-parse"), TEXT("--verify"), TEXT("--quiet"), TEXT("HEAD")});
			return Head.Succeeded() ? Head.StdoutText().TrimStartAndEnd() : FString();
		}
	}

	FOpResult FGitRepository::Stage(const TArray<FString>& Paths) const
	{
		if (Paths.IsEmpty())
		{
			return FOpResult::Failure(TEXT("Select files to stage."));
		}
		const FProcessResult Result = RunWithPathspecs(ECallKind::LocalMutate, {TEXT("add"), TEXT("-A")}, Paths);
		return Result.Succeeded() ? FOpResult::Success() : FOpResult::Failure(DescribeFailure(Result, TEXT("Staging")), Result.ErrorText());
	}

	FOpResult FGitRepository::Unstage(const TArray<FString>& Paths) const
	{
		if (Paths.IsEmpty())
		{
			return FOpResult::Failure(TEXT("Select files to unstage."));
		}
		// Before the first commit there is no HEAD to restore from, so drop the index entries.
		const bool bUnborn = CurrentHead(*this).IsEmpty();
		const TArray<FString> Args = bUnborn
			? TArray<FString>{TEXT("rm"), TEXT("--cached"), TEXT("-r"), TEXT("-f"), TEXT("-q"), TEXT("--ignore-unmatch")}
			: TArray<FString>{TEXT("restore"), TEXT("--staged")};
		const FProcessResult Result = RunWithPathspecs(ECallKind::LocalMutate, Args, Paths);
		return Result.Succeeded() ? FOpResult::Success() : FOpResult::Failure(DescribeFailure(Result, TEXT("Unstaging")), Result.ErrorText());
	}

	FOpResult FGitRepository::CommitIndex(const FString& Message, const FString& ReviewedHead, const FString& ReviewedStagedToken, FString& OutCommit) const
	{
		if (Message.TrimStartAndEnd().IsEmpty())
		{
			return FOpResult::Failure(TEXT("Enter a commit message."));
		}
		// Compare content, not the index file: git rewrites the index for stat refreshes without changing what is staged.
		FStatusSnapshot Now;
		FString Error;
		if (!ReadStatus(Now, Error, false))
		{
			return FOpResult::Failure(Error);
		}
		if (CurrentHead(*this) != ReviewedHead || Now.StagedToken != ReviewedStagedToken)
		{
			return FOpResult::Failure(TEXT("The staged files changed since you reviewed them. Check them and commit again."));
		}
		if (IsOperationInProgress())
		{
			return FOpResult::Failure(TEXT("Finish the merge or rebase in progress before committing."));
		}
		const FProcessResult Staged = Run(ECallKind::LocalScan, {TEXT("diff"), TEXT("--cached"), TEXT("--quiet")});
		if (Staged.Succeeded())
		{
			return FOpResult::Failure(TEXT("Stage files to commit."));
		}
		const FProcessResult Tree = Run(ECallKind::LocalMutate, {TEXT("write-tree")});
		if (!Tree.Succeeded())
		{
			return FOpResult::Failure(DescribeFailure(Tree, TEXT("Committing")), Tree.ErrorText());
		}

		// No pathspec and no implicit add: the commit is exactly the reviewed index. Hooks run.
		FGitCall Commit;
		Commit.Kind = ECallKind::Hooked;
		Commit.Args = {TEXT("-c"), TEXT("core.editor=false"), TEXT("commit"), TEXT("--cleanup=strip"), TEXT("-F"), TEXT("-")};
		AppendUtf8(Commit.Stdin, Message);
		const FProcessResult Result = Run(Commit);
		if (!Result.Succeeded())
		{
			return FOpResult::Failure(DescribeFailure(Result, TEXT("Committing")), Result.ErrorText());
		}
		OutCommit = CurrentHead(*this);
		FOpResult Success = FOpResult::Success();
		const FProcessResult CommittedTree = Run(ECallKind::LocalQuick, {TEXT("rev-parse"), TEXT("HEAD^{tree}")});
		if (!CommittedTree.Succeeded() || CommittedTree.StdoutText().TrimStartAndEnd() != Tree.StdoutText().TrimStartAndEnd())
		{
			Success.Details = TEXT("A commit hook changed the committed files; check the commit in History.");
		}
		return Success;
	}

	FOpResult FGitRepository::CommitPaths(const TArray<FString>& Paths, const TArray<FString>& NewPaths, const FString& Message, FString& OutCommit) const
	{
		if (Message.TrimStartAndEnd().IsEmpty())
		{
			return FOpResult::Failure(TEXT("Enter a description of your changes."));
		}
		if (Paths.IsEmpty())
		{
			return FOpResult::Failure(TEXT("There are no changed files to commit."));
		}
		if (IsOperationInProgress())
		{
			return FOpResult::Failure(TEXT("Finish the merge or rebase in progress before submitting."));
		}
		if (!NewPaths.IsEmpty())
		{
			const FProcessResult Added = RunWithPathspecs(ECallKind::LocalMutate, {TEXT("add")}, NewPaths);
			if (!Added.Succeeded())
			{
				return FOpResult::Failure(DescribeFailure(Added, TEXT("Adding new files")), Added.ErrorText());
			}
		}

		FScopedTempFile MessageFile;
		if (!FFileHelper::SaveStringToFile(Message, *MessageFile.Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			return FOpResult::Failure(TEXT("Could not write the commit message to a temporary file."));
		}
		// --only commits the working-tree content of exactly these paths and leaves any
		// other staged entries staged. Hooks run.
		FGitCall Commit;
		Commit.Kind = ECallKind::Hooked;
		Commit.Args = {TEXT("-c"), TEXT("core.editor=false"), TEXT("commit"), TEXT("--only"), TEXT("--cleanup=strip"),
			TEXT("-F"), MessageFile.Path, TEXT("--pathspec-from-file=-"), TEXT("--pathspec-file-nul")};
		for (const FString& Path : Paths)
		{
			AppendUtf8(Commit.Stdin, Path);
			Commit.Stdin.Add(0);
		}
		const FProcessResult Result = Run(Commit);
		if (!Result.Succeeded())
		{
			if (!NewPaths.IsEmpty())
			{
				// Leave new files exactly as they were: untracked.
				RunWithPathspecs(ECallKind::LocalMutate, {TEXT("rm"), TEXT("--cached"), TEXT("-q"), TEXT("--ignore-unmatch")}, NewPaths);
			}
			return FOpResult::Failure(DescribeFailure(Result, TEXT("Committing")), Result.ErrorText());
		}
		OutCommit = CurrentHead(*this);
		return FOpResult::Success();
	}

	FOpResult FGitRepository::RestorePaths(const TArray<FString>& Paths) const
	{
		if (Paths.IsEmpty())
		{
			return FOpResult::Success();
		}
		TSet<FString> InHead;
		if (!CurrentHead(*this).IsEmpty())
		{
			for (int32 Start = 0; Start < Paths.Num(); Start += PathsPerArgumentBatch)
			{
				TArray<FString> Args = {TEXT("ls-tree"), TEXT("-z"), TEXT("--full-name"), TEXT("--name-only"), TEXT("HEAD"), TEXT("--")};
				for (int32 Index = Start; Index < FMath::Min(Start + PathsPerArgumentBatch, Paths.Num()); ++Index)
				{
					Args.Add(Paths[Index]);
				}
				const FProcessResult Listed = Run(ECallKind::LocalScan, Args);
				TSet<FString> Found;
				if (!Listed.Succeeded() || !ParseNameList(Listed.Stdout, Found))
				{
					return FOpResult::Failure(DescribeFailure(Listed, TEXT("Reverting")), Listed.ErrorText());
				}
				InHead.Append(Found);
			}
		}
		TArray<FString> Restore;
		TArray<FString> Remove;
		for (const FString& Path : Paths)
		{
			(InHead.Contains(Path) ? Restore : Remove).Add(Path);
		}
		if (!Restore.IsEmpty())
		{
			// Restoring may need an LFS download, so no timeout applies.
			const FProcessResult Restored = RunWithPathspecs(ECallKind::Integrate, {TEXT("restore"), TEXT("--source=HEAD"), TEXT("--staged"), TEXT("--worktree")}, Restore);
			if (!Restored.Succeeded())
			{
				return FOpResult::Failure(DescribeFailure(Restored, TEXT("Reverting")), Restored.ErrorText());
			}
		}
		if (!Remove.IsEmpty())
		{
			const FProcessResult Unstaged = RunWithPathspecs(ECallKind::LocalMutate, {TEXT("rm"), TEXT("--cached"), TEXT("-q"), TEXT("--ignore-unmatch")}, Remove);
			if (!Unstaged.Succeeded())
			{
				return FOpResult::Failure(DescribeFailure(Unstaged, TEXT("Reverting")), Unstaged.ErrorText());
			}
			for (const FString& Path : Remove)
			{
				IFileManager::Get().Delete(*ToAbsolute(Path), false, true, true);
			}
		}
		return FOpResult::Success();
	}

	FOpResult FGitRepository::Diff(const FString& Path, bool bStaged, FString& OutText) const
	{
		TArray<FString> Args = {TEXT("diff"), TEXT("--no-ext-diff"), TEXT("--no-textconv"), TEXT("--no-color")};
		if (bStaged)
		{
			Args.Add(TEXT("--cached"));
		}
		Args.Append({TEXT("--"), Path});
		const FProcessResult Result = Run(ECallKind::LocalScan, Args);
		if (!Result.Succeeded())
		{
			return FOpResult::Failure(DescribeFailure(Result, TEXT("Comparing")), Result.ErrorText());
		}
		OutText = Result.StdoutText();
		return FOpResult::Success();
	}
}
