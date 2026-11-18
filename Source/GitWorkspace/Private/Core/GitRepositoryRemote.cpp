// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#include "GitRepository.h"
#include "GitParse.h"

namespace GitCore
{
	namespace
	{
		FString ConfigValue(const FGitRepository& Repository, const FString& Key)
		{
			const FProcessResult Result = Repository.Run(ECallKind::LocalQuick, {TEXT("config"), TEXT("--get"), Key});
			return Result.Succeeded() ? Result.StdoutText().TrimStartAndEnd() : FString();
		}

		FString CurrentBranch(const FGitRepository& Repository)
		{
			const FProcessResult Result = Repository.Run(ECallKind::LocalQuick, {TEXT("symbolic-ref"), TEXT("--quiet"), TEXT("--short"), TEXT("HEAD")});
			return Result.Succeeded() ? Result.StdoutText().TrimStartAndEnd() : FString();
		}

		bool IsRejection(const FString& Errors)
		{
			return Errors.Contains(TEXT("[rejected]")) || Errors.Contains(TEXT("non-fast-forward")) || Errors.Contains(TEXT("fetch first")) ||
				Errors.Contains(TEXT("Updates were rejected"));
		}
	}

	bool FGitRepository::ReadRemotes(TArray<FString>& Out) const
	{
		const FProcessResult Result = Run(ECallKind::LocalQuick, {TEXT("remote")});
		if (!Result.Succeeded())
		{
			return false;
		}
		Result.StdoutText().ParseIntoArrayLines(Out);
		return true;
	}

	FString FGitRepository::DefaultRemote() const
	{
		TArray<FString> Remotes;
		if (!ReadRemotes(Remotes) || Remotes.IsEmpty())
		{
			return FString();
		}
		const FString Branch = CurrentBranch(*this);
		if (!Branch.IsEmpty())
		{
			const FString Upstream = ConfigValue(*this, TEXT("branch.") + Branch + TEXT(".remote"));
			if (Remotes.Contains(Upstream))
			{
				return Upstream;
			}
		}
		if (Remotes.Contains(TEXT("origin")))
		{
			return TEXT("origin");
		}
		return Remotes.Num() == 1 ? Remotes[0] : FString();
	}

	FOpResult FGitRepository::Fetch(const FString& Remote, ECallKind Kind, const FCancelTokenPtr& Cancel) const
	{
		if (Remote.IsEmpty())
		{
			return FOpResult::Failure(TEXT("This repository has no remote to fetch from."));
		}
		FGitCall Call;
		Call.Kind = Kind;
		Call.Cancel = Cancel;
		// --progress keeps output flowing so a slow but healthy fetch is not mistaken for a hang.
		Call.Args = {TEXT("fetch"), TEXT("--progress"), TEXT("--no-recurse-submodules"), Remote};
		const FProcessResult Result = Run(Call);
		return Result.Succeeded() ? FOpResult::Success() : FOpResult::Failure(DescribeFailure(Result, TEXT("Fetching")), Result.ErrorText());
	}

	bool FGitRepository::ReadRemoteState(const FStatusSnapshot& Status, const FString& FallbackRemote, FRemoteState& Out, FString& OutError) const
	{
		Out.Ahead = 0;
		Out.Behind = 0;
		Out.IncomingPaths.Reset();
		Out.Upstream = Status.Upstream;
		Out.UpstreamHead.Reset();
		Out.Remote = Status.Branch.IsEmpty() ? FString() : ConfigValue(*this, TEXT("branch.") + Status.Branch + TEXT(".remote"));
		// Without a branch, a commit or a remote there is nothing to push, and the target stays empty.
		ResolvePush(Status, FallbackRemote, Out.Push);
		if (Status.Upstream.IsEmpty())
		{
			return true;
		}
		const FProcessResult Head = Run(ECallKind::LocalQuick, {TEXT("rev-parse"), TEXT("--verify"), TEXT("--quiet"), Status.Upstream + TEXT("^{commit}")});
		if (!Head.Succeeded())
		{
			// The upstream branch was configured but has not been fetched (or was deleted).
			return true;
		}
		Out.UpstreamHead = Head.StdoutText().TrimStartAndEnd();
		if (Status.bUnborn)
		{
			const FProcessResult Count = Run(ECallKind::LocalScan, {TEXT("rev-list"), TEXT("--count"), Out.UpstreamHead});
			const FProcessResult Tree = Run(ECallKind::LocalScan, {TEXT("ls-tree"), TEXT("-r"), TEXT("-z"), TEXT("--name-only"), Out.UpstreamHead});
			Out.Behind = FCString::Atoi(*Count.StdoutText());
			return Tree.Succeeded() && ParseNameList(Tree.Stdout, Out.IncomingPaths);
		}
		const FProcessResult Counts = Run(ECallKind::LocalScan, {TEXT("rev-list"), TEXT("--left-right"), TEXT("--count"), TEXT("HEAD...") + Out.UpstreamHead});
		TArray<FString> Numbers;
		Counts.StdoutText().TrimStartAndEnd().ParseIntoArrayWS(Numbers);
		if (!Counts.Succeeded() || Numbers.Num() != 2)
		{
			OutError = DescribeFailure(Counts, TEXT("Comparing with the upstream"));
			return false;
		}
		Out.Ahead = FCString::Atoi(*Numbers[0]);
		Out.Behind = FCString::Atoi(*Numbers[1]);
		if (Out.Behind > 0)
		{
			const FProcessResult Incoming = Run(ECallKind::LocalScan, {TEXT("diff"), TEXT("--name-only"), TEXT("-z"), TEXT("--no-renames"), TEXT("HEAD...") + Out.UpstreamHead, TEXT("--")});
			if (!Incoming.Succeeded() || !ParseNameList(Incoming.Stdout, Out.IncomingPaths))
			{
				OutError = DescribeFailure(Incoming, TEXT("Listing incoming changes"));
				return false;
			}
		}
		return true;
	}

	FOpResult FGitRepository::ResolvePush(const FStatusSnapshot& Status, const FString& FallbackRemote, FPushTarget& Out) const
	{
		Out = FPushTarget();
		if (Status.Branch.IsEmpty())
		{
			return FOpResult::Failure(TEXT("Switch to a branch before pushing."));
		}
		if (Status.bUnborn)
		{
			return FOpResult::Failure(TEXT("There are no commits to push yet."));
		}
		const FString Key = TEXT("branch.") + Status.Branch;
		Out.Branch = Status.Branch;
		Out.Remote = ConfigValue(*this, Key + TEXT(".pushRemote"));
		if (Out.Remote.IsEmpty())
		{
			Out.Remote = ConfigValue(*this, TEXT("remote.pushDefault"));
		}
		if (Out.Remote.IsEmpty() && !Status.Upstream.IsEmpty())
		{
			// An upstream remote of "." is a local branch: pushing there would move that branch.
			const FString UpstreamRemote = ConfigValue(*this, Key + TEXT(".remote"));
			const bool bOnRemote = !UpstreamRemote.IsEmpty() && UpstreamRemote != TEXT(".");
			FString UpstreamBranch = ConfigValue(*this, Key + TEXT(".merge"));
			UpstreamBranch.RemoveFromStart(TEXT("refs/heads/"));
			const auto PushesToUpstream = [this]()
			{
				const FString Mode = ConfigValue(*this, TEXT("push.default"));
				return Mode == TEXT("upstream") || Mode == TEXT("tracking");
			};
			if (bOnRemote && (UpstreamBranch == Status.Branch || PushesToUpstream()))
			{
				Out.Remote = UpstreamRemote;
				Out.Branch = UpstreamBranch;
			}
			else
			{
				// A feature branch made from origin/main follows it, but its commits must not land on main.
				Out.Remote = bOnRemote ? UpstreamRemote : FallbackRemote;
				Out.bSetUpstream = true;
				Out.PreviousUpstream = Status.Upstream;
			}
		}
		else if (Out.Remote.IsEmpty())
		{
			Out.Remote = FallbackRemote;
			Out.bSetUpstream = true;
		}
		if (Out.Remote.IsEmpty())
		{
			Out = FPushTarget();
			return FOpResult::Failure(TEXT("This repository has no remote to push to. Add one (for example origin) first."));
		}
		Out.bExists = Run(ECallKind::LocalQuick, {TEXT("rev-parse"), TEXT("--verify"), TEXT("--quiet"), TEXT("refs/remotes/") + Out.Remote + TEXT("/") + Out.Branch}).Succeeded();
		return FOpResult::Success();
	}

	FOpResult FGitRepository::Push(const FPushTarget& Target, const FCancelTokenPtr& Cancel, const FString& Commit, const FString& ExpectedOld) const
	{
		if (Target.Remote.IsEmpty() || Target.Branch.IsEmpty())
		{
			return FOpResult::Failure(TEXT("This repository has no remote to push to. Add one (for example origin) first."));
		}

		// Upload LFS objects first so a missing pre-push hook can never publish pointers
		// whose content the server does not have.
		FGitCall Lfs;
		Lfs.Kind = ECallKind::UserNetwork;
		Lfs.Cancel = Cancel;
		Lfs.bLiteralPathspecs = false;
		Lfs.Args = {TEXT("-c"), TEXT("lfs.allowincompletepush=false"), TEXT("lfs"), TEXT("push"), Target.Remote, Commit};
		const FProcessResult Uploaded = Run(Lfs);
		if (!Uploaded.Succeeded())
		{
			const bool bLfsMissing = Uploaded.ErrorText().Contains(TEXT("is not a git command"));
			const bool bUsesLfs = !ConfigValue(*this, TEXT("filter.lfs.process")).IsEmpty() || !ConfigValue(*this, TEXT("filter.lfs.clean")).IsEmpty();
			if (!bLfsMissing || bUsesLfs)
			{
				return FOpResult::Failure(bLfsMissing ? TEXT("Git LFS is not installed, so LFS files cannot be uploaded. Install Git LFS, then push again.")
					: DescribeFailure(Uploaded, TEXT("Uploading LFS files")), Uploaded.ErrorText());
			}
		}

		FGitCall Push;
		Push.Kind = ECallKind::UserNetwork;
		Push.Cancel = Cancel;
		Push.Args = {TEXT("push"), TEXT("--porcelain"), TEXT("--progress")};
		if (Target.bSetUpstream)
		{
			Push.Args.Add(TEXT("--set-upstream"));
		}
		const FString Destination = TEXT("refs/heads/") + Target.Branch;
		if (!ExpectedOld.IsEmpty())
		{
			Push.Args.Add(TEXT("--force-with-lease=") + Destination + TEXT(":") + ExpectedOld);
		}
		Push.Args.Append({Target.Remote, Commit + TEXT(":") + Destination});
		const FProcessResult Result = Run(Push);
		if (!Result.Succeeded())
		{
			const FString Output = Result.StdoutText() + Result.ErrorText();
			if (!ExpectedOld.IsEmpty() && IsRejection(Output))
			{
				// The lease turns the push down when the branch moved on the server; the server itself may forbid rewriting it.
				return FOpResult::Failure(Output.Contains(TEXT("stale info"))
					? FString::Printf(TEXT("Someone pushed to %s/%s meanwhile."), *Target.Remote, *Target.Branch)
					: FString::Printf(TEXT("The server does not allow rewriting %s/%s."), *Target.Remote, *Target.Branch), Output);
			}
			if (IsRejection(Output))
			{
				return FOpResult::Failure(TEXT("The server has newer commits. Pull, then push again."), Output);
			}
			return FOpResult::Failure(DescribeFailure(Result, TEXT("Pushing")), Output);
		}
		return FOpResult::Success(FString::Printf(TEXT("Pushed to %s/%s."), *Target.Remote, *Target.Branch));
	}

	bool FGitRepository::ReadPushedPaths(const FString& Remote, const FString& Commit, TSet<FString>& Out, FString& OutError) const
	{
		// -c lists only the files a merge changed from every parent: what it took from one side is already there.
		// The settings keep a first commit's files listed and signature checks out of the output.
		const FProcessResult Result = Run(ECallKind::LocalScan, {TEXT("-c"), TEXT("log.showRoot=true"), TEXT("-c"), TEXT("log.showSignature=false"),
			TEXT("log"), TEXT("--format="), TEXT("--name-only"), TEXT("-z"), TEXT("--no-renames"), TEXT("-c"), Commit, TEXT("--not"), TEXT("--remotes=") + Remote, TEXT("--")});
		if (!Result.Succeeded() || !ParseNameList(Result.Stdout, Out))
		{
			OutError = DescribeFailure(Result, TEXT("Listing what the push changes"));
			return false;
		}
		return true;
	}

	bool FGitRepository::PredictConflicts(const FString& Upstream, bool& bOutConflicts, FString& OutError) const
	{
		const FProcessResult Result = Run(ECallKind::LocalScan, {TEXT("merge-tree"), TEXT("--write-tree"), TEXT("--no-messages"), TEXT("HEAD"), Upstream});
		if (Result.Exited() && (Result.ExitCode == 0 || Result.ExitCode == 1))
		{
			bOutConflicts = Result.ExitCode == 1;
			return true;
		}
		OutError = DescribeFailure(Result, TEXT("Checking for conflicts"));
		return false;
	}

	bool FGitRepository::PredictReapplyConflicts(const FString& Upstream, bool& bOutConflicts, FString& OutError) const
	{
		bOutConflicts = false;
		// stash create records the changes as a commit on top of HEAD without touching the working tree.
		const FProcessResult Stash = Run(ECallKind::LocalScan, {TEXT("stash"), TEXT("create")});
		if (!Stash.Succeeded())
		{
			OutError = DescribeFailure(Stash, TEXT("Reading your local changes"));
			return false;
		}
		const FString Changes = Stash.StdoutText().TrimStartAndEnd();
		if (Changes.IsEmpty())
		{
			return true;
		}
		// Its merge with Upstream is HEAD's merge with Upstream plus the local changes put back on top.
		const FProcessResult Result = Run(ECallKind::LocalScan, {TEXT("merge-tree"), TEXT("--write-tree"), TEXT("--no-messages"), Upstream, Changes});
		if (Result.Exited() && (Result.ExitCode == 0 || Result.ExitCode == 1))
		{
			bOutConflicts = Result.ExitCode == 1;
			return true;
		}
		OutError = DescribeFailure(Result, TEXT("Checking your local changes against the incoming ones"));
		return false;
	}

	FOpResult FGitRepository::Integrate(const FString& Upstream, EIntegrate Mode, const FCancelTokenPtr& Cancel) const
	{
		FGitCall Call;
		Call.Kind = ECallKind::Integrate;
		Call.Cancel = Cancel;
		switch (Mode)
		{
		case EIntegrate::Rebase:
			Call.Args = {TEXT("-c"), TEXT("core.editor=true"), TEXT("rebase"), TEXT("--autostash"), Upstream};
			break;
		case EIntegrate::Merge:
			Call.Args = {TEXT("merge"), TEXT("--no-edit"), TEXT("--no-stat"), Upstream};
			break;
		default:
			Call.Args = {TEXT("merge"), TEXT("--ff-only"), TEXT("--no-edit"), TEXT("--no-stat"), Upstream};
			break;
		}
		const FProcessResult Result = Run(Call);
		if (Result.Succeeded())
		{
			return FOpResult::Success();
		}
		if (Mode != EIntegrate::FastForward && IsOperationInProgress())
		{
			// Never leave a half-finished rebase or merge behind: restore the original commits and edits.
			Run(ECallKind::LocalMutate, {Mode == EIntegrate::Rebase ? TEXT("rebase") : TEXT("merge"), TEXT("--abort")});
			return FOpResult::Failure(TEXT("Your commits conflict with the incoming changes. Nothing was changed; resolve the conflict in an external Git client."), Result.ErrorText());
		}
		return FOpResult::Failure(DescribeFailure(Result, TEXT("Pulling")), Result.ErrorText());
	}

	bool FGitRepository::ChangedPaths(const FString& From, const FString& To, TSet<FString>& Out, FString& OutError) const
	{
		const FProcessResult Result = Run(ECallKind::LocalScan, {TEXT("diff"), TEXT("--name-only"), TEXT("-z"), TEXT("--no-renames"), From, To, TEXT("--")});
		if (!Result.Succeeded() || !ParseNameList(Result.Stdout, Out))
		{
			OutError = DescribeFailure(Result, TEXT("Listing changed files"));
			return false;
		}
		return true;
	}

	FOpResult FGitRepository::FetchLfsObjects(const FString& Remote, const FString& Ref, const FCancelTokenPtr& Cancel) const
	{
		FGitCall Call;
		Call.Kind = ECallKind::Integrate;
		Call.Cancel = Cancel;
		Call.bLiteralPathspecs = false;
		Call.Args = {TEXT("lfs"), TEXT("fetch"), Remote, Ref};
		const FProcessResult Result = Run(Call);
		if (Result.Succeeded() || Result.ErrorText().Contains(TEXT("is not a git command")))
		{
			// Without Git LFS installed there is nothing to prefetch.
			return FOpResult::Success();
		}
		return FOpResult::Failure(DescribeFailure(Result, TEXT("Downloading LFS files")), Result.ErrorText());
	}

	bool FGitRepository::HasUnpublishedChanges(const FString& Path, bool& bOutUnpublished, FString& OutError) const
	{
		bOutUnpublished = false;
		const FProcessResult Head = Run(ECallKind::LocalQuick, {TEXT("rev-parse"), TEXT("--verify"), TEXT("--quiet"), TEXT("HEAD")});
		if (!Head.Succeeded())
		{
			return true;
		}
		const FProcessResult Count = Run(ECallKind::LocalScan, {TEXT("rev-list"), TEXT("--count"), TEXT("HEAD"), TEXT("--not"), TEXT("--remotes"), TEXT("--"), Path});
		if (!Count.Succeeded())
		{
			OutError = DescribeFailure(Count, TEXT("Checking for unpushed commits"));
			return false;
		}
		bOutUnpublished = FCString::Atoi(*Count.StdoutText()) > 0;
		return true;
	}
}
