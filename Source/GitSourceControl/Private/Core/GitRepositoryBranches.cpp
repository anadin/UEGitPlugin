// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#include "GitRepository.h"

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

	FOpResult FGitRepository::CreateBranch(const FString& Name) const
	{
		const FProcessResult Valid = Run(ECallKind::LocalQuick, {TEXT("check-ref-format"), TEXT("--branch"), Name});
		if (Name.IsEmpty() || Name.StartsWith(TEXT("-")) || !Valid.Succeeded())
		{
			return FOpResult::Failure(FString::Printf(TEXT("\"%s\" is not a valid branch name."), *Name));
		}
		const FProcessResult Result = Run(ECallKind::LocalMutate, {TEXT("switch"), TEXT("-c"), Name});
		return Result.Succeeded() ? FOpResult::Success() : FOpResult::Failure(DescribeFailure(Result, TEXT("Creating the branch")), Result.ErrorText());
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
}
