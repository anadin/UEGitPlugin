// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#include "GitRepository.h"
#include "GitParse.h"

namespace GitCore
{
	bool FGitRepository::ReadBranches(TArray<FBranchInfo>& Out, FString& OutError) const
	{
		Out.Reset();
		// Ref names cannot contain newlines or NUL, so newline-terminated records with NUL fields are unambiguous.
		const FProcessResult Result = Run(ECallKind::LocalScan, {TEXT("for-each-ref"),
			TEXT("--format=%(refname)%00%(upstream:short)%00%(HEAD)"), TEXT("refs/heads"), TEXT("refs/remotes")});
		if (!Result.Succeeded())
		{
			OutError = DescribeFailure(Result, TEXT("Listing branches"));
			return false;
		}
		TArray<FString> Fields;
		int32 FieldStart = 0;
		for (int32 Index = 0; Index < Result.Stdout.Num(); ++Index)
		{
			const uint8 Byte = Result.Stdout[Index];
			if (Byte != 0 && Byte != '\n')
			{
				continue;
			}
			Fields.Add(DecodeUtf8(Result.Stdout.GetData() + FieldStart, Index - FieldStart));
			FieldStart = Index + 1;
			if (Byte != '\n')
			{
				continue;
			}
			if (Fields.Num() == 3)
			{
				FBranchInfo Branch;
				if (Fields[0].StartsWith(TEXT("refs/heads/")))
				{
					Branch.Name = Fields[0].Mid(11);
				}
				else if (Fields[0].StartsWith(TEXT("refs/remotes/")) && !Fields[0].EndsWith(TEXT("/HEAD")))
				{
					Branch.Name = Fields[0].Mid(13);
					Branch.bRemote = true;
				}
				if (!Branch.Name.IsEmpty())
				{
					Branch.Upstream = Fields[1];
					Branch.bCurrent = Fields[2] == TEXT("*");
					Out.Add(MoveTemp(Branch));
				}
			}
			Fields.Reset();
		}
		return true;
	}

	FOpResult FGitRepository::CreateBranch(const FString& Name, const FString& StartPoint) const
	{
		const FProcessResult Valid = Run(ECallKind::LocalQuick, {TEXT("check-ref-format"), TEXT("--branch"), Name});
		if (Name.IsEmpty() || Name.StartsWith(TEXT("-")) || !Valid.Succeeded())
		{
			return FOpResult::Failure(FString::Printf(TEXT("\"%s\" is not a valid branch name."), *Name));
		}
		if (StartPoint.IsEmpty())
		{
			const FProcessResult Result = Run(ECallKind::LocalMutate, {TEXT("switch"), TEXT("-c"), Name});
			return Result.Succeeded() ? FOpResult::Success() : FOpResult::Failure(DescribeFailure(Result, TEXT("Creating the branch")), Result.ErrorText());
		}
		// Files change to StartPoint's, which can download LFS content, so no timeout applies.
		const FProcessResult Result = Run(ECallKind::Integrate, {TEXT("switch"), TEXT("--no-track"), TEXT("-c"), Name, StartPoint});
		if (!Result.Succeeded())
		{
			const FString Output = Result.ErrorText();
			if (Output.Contains(TEXT("would be overwritten")))
			{
				return FOpResult::Failure(TEXT("Your local changes would be overwritten. Commit, stash or revert them first."), Output);
			}
			return FOpResult::Failure(DescribeFailure(Result, TEXT("Creating the branch")), Output);
		}
		return FOpResult::Success();
	}

	FOpResult FGitRepository::SwitchBranch(const FString& Name) const
	{
		if (Name.IsEmpty() || Name.StartsWith(TEXT("-")))
		{
			return FOpResult::Failure(TEXT("Select a branch."));
		}
		// Switching can download LFS content for the other branch, so no timeout applies.
		const FProcessResult Result = Run(ECallKind::Integrate, {TEXT("switch"), Name});
		if (!Result.Succeeded())
		{
			const FString Output = Result.ErrorText();
			if (Output.Contains(TEXT("would be overwritten")))
			{
				return FOpResult::Failure(TEXT("Your local changes would be overwritten. Commit, stash or revert them before switching."), Output);
			}
			return FOpResult::Failure(DescribeFailure(Result, TEXT("Switching branch")), Output);
		}
		return FOpResult::Success();
	}

	FOpResult FGitRepository::FinishBranch(const FString& Main, const FString& Upstream, int32& OutCommits) const
	{
		OutCommits = 0;
		const FProcessResult Current = Run(ECallKind::LocalQuick, {TEXT("symbolic-ref"), TEXT("--quiet"), TEXT("--short"), TEXT("HEAD")});
		const FString Branch = Current.Succeeded() ? Current.StdoutText().TrimStartAndEnd() : FString();
		if (Branch.IsEmpty() || Branch.Equals(Main, ESearchCase::CaseSensitive))
		{
			return FOpResult::Failure(FString::Printf(TEXT("Switch to the branch you want to merge into %s first."), *Main));
		}
		FString Head;
		FString MainOid;
		FString UpstreamOid;
		if (!ResolveCommit(TEXT("HEAD"), Head) || !ResolveCommit(TEXT("refs/heads/") + Main, MainOid) || !ResolveCommit(Upstream, UpstreamOid))
		{
			return FOpResult::Failure(FString::Printf(TEXT("%s or %s could not be found. Fetch, then try again."), *Main, *Upstream));
		}
		bool bMainPushed = false;
		bool bSynced = false;
		FString Error;
		if (!IsAncestor(MainOid, UpstreamOid, bMainPushed, Error) || !IsAncestor(UpstreamOid, Head, bSynced, Error))
		{
			return FOpResult::Failure(Error);
		}
		if (!bMainPushed)
		{
			return FOpResult::Failure(FString::Printf(TEXT("Nothing was changed: %s has commits that are not on %s. Switch to %s and push or pull it first."), *Main, *Upstream, *Main));
		}
		if (!bSynced)
		{
			return FOpResult::Failure(FString::Printf(TEXT("Nothing was changed: %s has commits this branch does not have. Sync, then finish."), *Upstream));
		}
		if (!CountCommits({UpstreamOid + TEXT("..") + Head}, OutCommits, Error))
		{
			return FOpResult::Failure(Error);
		}
		if (OutCommits == 0)
		{
			return FOpResult::Failure(FString::Printf(TEXT("Nothing was changed: %s already has every commit of %s."), *Main, *Branch));
		}

		// Both ways the new main has exactly HEAD's content, so switching to it changes no file.
		FString NewMain = Head;
		if (OutCommits > 1)
		{
			const FProcessResult Merge = Run(ECallKind::LocalMutate, {TEXT("commit-tree"), Head + TEXT("^{tree}"), TEXT("-p"), UpstreamOid, TEXT("-p"), Head,
				TEXT("-m"), FString::Printf(TEXT("Merge branch '%s'"), *Branch)});
			NewMain = Merge.StdoutText().TrimStartAndEnd();
			if (!Merge.Succeeded() || !IsObjectId(NewMain))
			{
				return FOpResult::Failure(DescribeFailure(Merge, TEXT("Creating the merge commit")), Merge.ErrorText());
			}
		}
		// The old value makes the update fail if main moved meanwhile.
		const FProcessResult Moved = Run(ECallKind::LocalMutate, {TEXT("update-ref"), TEXT("-m"), TEXT("finish: ") + Branch, TEXT("refs/heads/") + Main, NewMain, MainOid});
		if (!Moved.Succeeded())
		{
			return FOpResult::Failure(DescribeFailure(Moved, TEXT("Moving ") + Main), Moved.ErrorText());
		}
		const FProcessResult Switched = Run(ECallKind::LocalMutate, {TEXT("switch"), Main});
		if (!Switched.Succeeded())
		{
			Run(ECallKind::LocalMutate, {TEXT("update-ref"), TEXT("-m"), TEXT("finish: undo"), TEXT("refs/heads/") + Main, MainOid, NewMain});
			return FOpResult::Failure(DescribeFailure(Switched, TEXT("Switching to ") + Main), Switched.ErrorText());
		}
		return FOpResult::Success();
	}

	bool FGitRepository::ResolveCommit(const FString& Revision, FString& OutOid) const
	{
		const FProcessResult Result = Run(ECallKind::LocalQuick, {TEXT("rev-parse"), TEXT("--verify"), TEXT("--quiet"), TEXT("--end-of-options"), Revision + TEXT("^{commit}")});
		OutOid = Result.Succeeded() ? Result.StdoutText().TrimStartAndEnd() : FString();
		return !OutOid.IsEmpty();
	}

	bool FGitRepository::IsAncestor(const FString& Ancestor, const FString& Descendant, bool& bOut, FString& OutError) const
	{
		const FProcessResult Result = Run(ECallKind::LocalScan, {TEXT("merge-base"), TEXT("--is-ancestor"), Ancestor, Descendant});
		if (Result.Exited() && (Result.ExitCode == 0 || Result.ExitCode == 1))
		{
			bOut = Result.ExitCode == 0;
			return true;
		}
		OutError = DescribeFailure(Result, TEXT("Comparing branches"));
		return false;
	}

	bool FGitRepository::CountCommits(const TArray<FString>& Revisions, int32& OutCount, FString& OutError) const
	{
		TArray<FString> Args = {TEXT("rev-list"), TEXT("--count")};
		Args.Append(Revisions);
		Args.Add(TEXT("--"));
		const FProcessResult Result = Run(ECallKind::LocalScan, MoveTemp(Args));
		if (!Result.Succeeded())
		{
			OutError = DescribeFailure(Result, TEXT("Counting commits"));
			return false;
		}
		OutCount = FCString::Atoi(*Result.StdoutText());
		return true;
	}
}
