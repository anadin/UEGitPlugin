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

		/** One file's diff in the shape picking lines needs, whatever the user's diff settings. */
		FProcessResult RunFileDiff(const FGitRepository& Repository, const FString& Path, bool bStaged)
		{
			// Empty context lines keep their space, file names their a/ and b/, and hunks three lines of context.
			TArray<FString> Args = {TEXT("-c"), TEXT("diff.suppressBlankEmpty=false"), TEXT("diff"), TEXT("--no-ext-diff"), TEXT("--no-textconv"),
				TEXT("--no-color"), TEXT("--no-renames"), TEXT("--unified=3"), TEXT("--src-prefix=a/"), TEXT("--dst-prefix=b/")};
			if (bStaged)
			{
				Args.Add(TEXT("--cached"));
			}
			Args.Append({TEXT("--"), Path});
			return Repository.Run(ECallKind::LocalScan, Args);
		}

		/** A file's lines, each with its line break. */
		TArray<TArrayView<const uint8>> SplitLines(const TArray<uint8>& Bytes)
		{
			TArray<TArrayView<const uint8>> Lines;
			for (int32 Start = 0; Start < Bytes.Num();)
			{
				int32 End = Start;
				while (End < Bytes.Num() && Bytes[End] != '\n')
				{
					++End;
				}
				End = FMath::Min(End + 1, Bytes.Num());
				Lines.Emplace(Bytes.GetData() + Start, End - Start);
				Start = End;
			}
			return Lines;
		}

		/** The same line, ignoring its line break: staging may turn "\r\n" into "\n". */
		bool SameLine(TArrayView<const uint8> A, TArrayView<const uint8> B)
		{
			auto Trim = [](TArrayView<const uint8> Line)
			{
				int32 Size = Line.Num();
				Size -= Size > 0 && Line[Size - 1] == '\n' ? 1 : 0;
				Size -= Size > 0 && Line[Size - 1] == '\r' ? 1 : 0;
				return Line.Slice(0, Size);
			};
			A = Trim(A);
			B = Trim(B);
			return A.Num() == B.Num() && FMemory::Memcmp(A.GetData(), B.GetData(), A.Num()) == 0;
		}

		const TCHAR* const ChangedSinceShown = TEXT("The file changed since its lines were shown. Select them again.");
		const TCHAR* const NotUtf8 = TEXT("Only UTF-8 text can be staged line by line. Stage the whole file instead.");
		const TCHAR* const NothingPicked = TEXT("Select added or removed lines first.");

		/** ApplyLines for a file Git does not track: its diff is its content (NewFileDiff), so its lines are numbered from 1. */
		FOpResult ApplyNewFileLines(const FGitRepository& Repository, const FString& Path, ELineAction Action, const FString& ShownDiff,
			const FLineSelection& Lines, const TFunction<void()>& BeforeDiscard)
		{
			const FString File = Repository.ToAbsolute(Path);
			TArray<uint8> Bytes;
			if (IFileManager::Get().FileSize(*File) > NewFileDiffLimit || !FFileHelper::LoadFileToArray(Bytes, *File, FILEREAD_Silent) ||
				!NewFileDiff(Bytes).Equals(ShownDiff, ESearchCase::CaseSensitive))
			{
				return FOpResult::Failure(ChangedSinceShown);
			}
			if (!IsValidUtf8(Bytes.GetData(), Bytes.Num()))
			{
				return FOpResult::Failure(NotUtf8);
			}
			const TArray<TArrayView<const uint8>> FileLines = SplitLines(Bytes);
			FLineSelection Rest;
			TArray<uint8> Kept;
			for (int32 Number = 1; Number <= FileLines.Num(); ++Number)
			{
				if (!Lines.Added.Contains(Number))
				{
					Rest.Added.Add(Number);
					Kept.Append(FileLines[Number - 1].GetData(), FileLines[Number - 1].Num());
				}
			}
			if (Rest.Added.Num() == FileLines.Num())
			{
				return FOpResult::Failure(NothingPicked);
			}
			if (Action == ELineAction::Discard)
			{
				if (BeforeDiscard)
				{
					BeforeDiscard();
				}
				// Discarding every line discards the new file, as Discard does for the whole file.
				const bool bWritten = Rest.IsEmpty() ? IFileManager::Get().Delete(*File, false, true, true) : FFileHelper::SaveArrayToFile(Kept, *File);
				return bWritten ? FOpResult::Success() : FOpResult::Failure(FString::Printf(TEXT("Could not change %s."), *Path));
			}

			// Staged whole, the file gets the mode, filters and line endings Git gives it; then the lines left out come back out.
			const FOpResult Added = Repository.Stage({Path});
			if (!Added.bOk || Rest.IsEmpty())
			{
				return Added;
			}
			auto Undo = [&Repository, &Path](const FString& Message, const FString& Details)
			{
				Repository.Unstage({Path});
				return FOpResult::Failure(Message, Details);
			};
			const FProcessResult Blob = Repository.Run(ECallKind::LocalScan, {TEXT("cat-file"), TEXT("blob"), TEXT(":") + Path});
			const TArray<TArrayView<const uint8>> StagedLines = SplitLines(Blob.Stdout);
			bool bSame = Blob.Succeeded() && StagedLines.Num() == FileLines.Num();
			for (int32 Index = 0; bSame && Index < FileLines.Num(); ++Index)
			{
				bSame = SameLine(StagedLines[Index], FileLines[Index]);
			}
			if (!bSame)
			{
				return Undo(TEXT("Git changes this file as it stages it (a filter, or Git LFS), so its lines cannot be staged one by one. ")
					TEXT("Stage the whole file instead."), Blob.ErrorText());
			}
			const FProcessResult Staged = RunFileDiff(Repository, Path, true);
			FString Patch;
			FString Error;
			if (!Staged.Succeeded() || !BuildLinePatch(Staged.StdoutText(), Rest, true, Patch, Error) || Patch.IsEmpty())
			{
				return Undo(TEXT("Staging the lines failed."), Error.IsEmpty() ? Staged.ErrorText() : Error);
			}
			const FOpResult Applied = Repository.ApplyPatch(Patch, true, true);
			return Applied.bOk ? Applied : Undo(Applied.Message, Applied.Details);
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
		const FProcessResult Result = RunFileDiff(*this, Path, bStaged);
		if (!Result.Succeeded())
		{
			return FOpResult::Failure(DescribeFailure(Result, TEXT("Comparing")), Result.ErrorText());
		}
		OutText = Result.StdoutText();
		return FOpResult::Success();
	}

	FOpResult FGitRepository::ApplyPatch(const FString& Patch, bool bIndex, bool bReverse) const
	{
		FGitCall Apply;
		Apply.Kind = ECallKind::LocalMutate;
		// The lines' whitespace is the user's: never warn about it, or "fix" it.
		Apply.Args = {TEXT("apply"), TEXT("--whitespace=nowarn")};
		if (bIndex)
		{
			Apply.Args.Add(TEXT("--cached"));
		}
		if (bReverse)
		{
			Apply.Args.Add(TEXT("--reverse"));
		}
		AppendUtf8(Apply.Stdin, Patch);
		const FProcessResult Result = Run(Apply);
		return Result.Succeeded() ? FOpResult::Success() : FOpResult::Failure(DescribeFailure(Result, TEXT("Applying the lines")), Result.ErrorText());
	}

	FOpResult FGitRepository::ApplyLines(const FString& Path, ELineAction Action, const FString& ShownDiff, const FLineSelection& Lines,
		const TFunction<void()>& BeforeDiscard) const
	{
		if (Lines.IsEmpty())
		{
			return FOpResult::Failure(NothingPicked);
		}
		const bool bStaged = Action == ELineAction::Unstage;
		if (!bStaged)
		{
			// Git has no diff of a file it does not track: what was shown is the file's content.
			const FProcessResult Listed = Run(ECallKind::LocalQuick, {TEXT("ls-files"), TEXT("-z"), TEXT("--"), Path});
			if (!Listed.Succeeded())
			{
				return FOpResult::Failure(DescribeFailure(Listed, TEXT("Reading the index")), Listed.ErrorText());
			}
			if (Listed.Stdout.IsEmpty())
			{
				return ApplyNewFileLines(*this, Path, Action, ShownDiff, Lines, BeforeDiscard);
			}
		}
		const FProcessResult Fresh = RunFileDiff(*this, Path, bStaged);
		if (!Fresh.Succeeded())
		{
			return FOpResult::Failure(DescribeFailure(Fresh, TEXT("Comparing")), Fresh.ErrorText());
		}
		if (!Fresh.StdoutText().Equals(ShownDiff, ESearchCase::CaseSensitive))
		{
			return FOpResult::Failure(ChangedSinceShown);
		}
		// Text that does not round-trip would come back into the file changed.
		if (!IsValidUtf8(Fresh.Stdout.GetData(), Fresh.Stdout.Num()))
		{
			return FOpResult::Failure(NotUtf8);
		}
		FString Patch;
		FString Error;
		if (!BuildLinePatch(ShownDiff, Lines, Action != ELineAction::Stage, Patch, Error))
		{
			return FOpResult::Failure(Error);
		}
		if (Patch.IsEmpty())
		{
			return FOpResult::Failure(NothingPicked);
		}
		if (Action == ELineAction::Discard && BeforeDiscard)
		{
			BeforeDiscard();
		}
		return ApplyPatch(Patch, Action != ELineAction::Discard, Action != ELineAction::Stage);
	}
}
