// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#if WITH_DEV_AUTOMATION_TESTS

#include "GitTestFixtures.h"

namespace
{
	constexpr EAutomationTestFlags RemoteTestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

	/** Bare origin with two clones, A and B, sharing one initial commit. */
	struct FTwoClones
	{
		GitTest::FFixture Fixture;
		FString Origin;
		FString A;
		FString B;

		FTwoClones()
		{
			Origin = Fixture.InitBare();
			const FString Seed = Fixture.InitRepo(TEXT("seed"));
			GitTest::WriteFile(Seed, TEXT("shared.txt"), TEXT("line 1\nline 2\nline 3\n"));
			GitTest::CommitAll(Seed, TEXT("initial"));
			GitTest::Git(Seed, {TEXT("remote"), TEXT("add"), TEXT("origin"), Origin});
			GitTest::Git(Seed, {TEXT("push"), TEXT("-q"), TEXT("origin"), TEXT("main")});
			A = Fixture.Clone(Origin, TEXT("a"), TEXT("Alice"));
			B = Fixture.Clone(Origin, TEXT("b"), TEXT("Bob"));
		}

		void PushFromB(const FString& File, const FString& Content, const FString& Message) const
		{
			GitTest::WriteFile(B, File, Content);
			GitTest::CommitAll(B, Message);
			GitTest::Git(B, {TEXT("push"), TEXT("-q"), TEXT("origin"), TEXT("HEAD")});
		}
	};

	GitCore::FStatusSnapshot ReadStatus(FAutomationTestBase& Test, const GitCore::FGitRepository& Repository)
	{
		GitCore::FStatusSnapshot Status;
		FString Error;
		if (!Repository.ReadStatus(Status, Error, true))
		{
			Test.AddError(Error);
		}
		return Status;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitRemoteSyncTest, "GitWorkspace.Remote.FetchStateAndIntegrate", RemoteTestFlags)
bool FGitRemoteSyncTest::RunTest(const FString&)
{
	FTwoClones Clones;
	const GitCore::FGitRepositoryPtr Repository = GitTest::OpenRepository(*this, Clones.A);
	if (!Repository.IsValid())
	{
		return false;
	}
	TestEqualSensitive(TEXT("The upstream remote is the default"), Repository->DefaultRemote(), FString(TEXT("origin")));

	Clones.PushFromB(TEXT("from-b.txt"), TEXT("b\n"), TEXT("B adds a file"));
	const FString BHead = GitTest::Output(GitTest::Git(Clones.B, {TEXT("rev-parse"), TEXT("HEAD")}));
	TestTrue(TEXT("Fetch succeeds"), Repository->Fetch(TEXT("origin"), GitCore::ECallKind::UserNetwork, nullptr).bOk);
	TestEqualSensitive(TEXT("Fetch updates the remote-tracking branch"), GitTest::Output(GitTest::Git(Clones.A, {TEXT("rev-parse"), TEXT("origin/main")})), BHead);
	GitCore::FStatusSnapshot Status = ReadStatus(*this, *Repository);
	TestEqualSensitive(TEXT("Status knows the upstream"), Status.Upstream, FString(TEXT("origin/main")));
	GitCore::FRemoteState Remote;
	FString Error;
	TestTrue(TEXT("Remote state is read from local refs"), Repository->ReadRemoteState(Status, TEXT("origin"), Remote, Error));
	TestTrue(TEXT("One incoming commit"), Remote.Behind == 1 && Remote.Ahead == 0);
	TestTrue(TEXT("Incoming paths are listed"), GitTest::HasExactly(Remote.IncomingPaths, TEXT("from-b.txt")));
	const GitCore::FOpResult FastForward = Repository->Integrate(Remote.Upstream, GitCore::EIntegrate::FastForward, nullptr);
	TestTrue(TEXT("Fast-forward integrates: ") + FastForward.Message + TEXT(" ") + FastForward.Details, FastForward.bOk);
	TestEqualSensitive(TEXT("HEAD now matches B"), GitTest::Output(GitTest::Git(Clones.A, {TEXT("rev-parse"), TEXT("HEAD")})), BHead);

	// Diverged but compatible: local commits are rebased onto the incoming ones.
	GitTest::WriteFile(Clones.A, TEXT("from-a.txt"), TEXT("a\n"));
	GitTest::CommitAll(Clones.A, TEXT("A adds a file"));
	GitTest::WriteFile(Clones.A, TEXT("uncommitted.txt"), TEXT("keep me\n"));
	Clones.PushFromB(TEXT("from-b-2.txt"), TEXT("b2\n"), TEXT("B adds another file"));
	Repository->Fetch(TEXT("origin"), GitCore::ECallKind::UserNetwork, nullptr);
	Status = ReadStatus(*this, *Repository);
	Repository->ReadRemoteState(Status, TEXT("origin"), Remote, Error);
	TestTrue(TEXT("Diverged history is reported"), Remote.Ahead == 1 && Remote.Behind == 1);
	TSet<FString> Pushed;
	TestTrue(TEXT("What a push would change is read"), Repository->ReadPushedPaths(TEXT("origin"), TEXT("HEAD"), Pushed, Error));
	TestEqualSensitive(TEXT("A push changes the local commit's file"), GitTest::Joined(Pushed.Array()), FString(TEXT("from-a.txt")));
	bool bConflicts = true;
	TestTrue(TEXT("Conflict prediction runs"), Repository->PredictConflicts(Remote.Upstream, bConflicts, Error));
	TestFalse(TEXT("No conflict is predicted"), bConflicts);
	const GitCore::FOpResult Rebased = Repository->Integrate(Remote.Upstream, GitCore::EIntegrate::Rebase, nullptr);
	TestTrue(TEXT("Rebase integrates: ") + Rebased.Message, Rebased.bOk);
	TestEqualSensitive(TEXT("Local work sits on top of the incoming commit"), GitTest::Output(GitTest::Git(Clones.A, {TEXT("rev-list"), TEXT("--count"), TEXT("origin/main..HEAD")})), FString(TEXT("1")));
	TestTrue(TEXT("Both files are present"), GitTest::FileExists(Clones.A, TEXT("from-a.txt")) && GitTest::FileExists(Clones.A, TEXT("from-b-2.txt")));
	TestEqualSensitive(TEXT("Uncommitted work survives"), GitTest::ReadFile(Clones.A, TEXT("uncommitted.txt")), FString(TEXT("keep me\n")));

	// Conflicting history: prediction says so, and a forced attempt leaves nothing half-done.
	GitTest::WriteFile(Clones.A, TEXT("shared.txt"), TEXT("line 1\nA's line 2\nline 3\n"));
	GitTest::CommitAll(Clones.A, TEXT("A edits line 2"));
	Clones.PushFromB(TEXT("shared.txt"), TEXT("line 1\nB's line 2\nline 3\n"), TEXT("B edits line 2"));
	Repository->Fetch(TEXT("origin"), GitCore::ECallKind::UserNetwork, nullptr);
	TestTrue(TEXT("Conflict prediction runs again"), Repository->PredictConflicts(TEXT("origin/main"), bConflicts, Error));
	TestTrue(TEXT("The conflict is predicted"), bConflicts);
	const FString Before = GitTest::Output(GitTest::Git(Clones.A, {TEXT("rev-parse"), TEXT("HEAD")}));
	TestFalse(TEXT("A conflicting rebase fails"), Repository->Integrate(TEXT("origin/main"), GitCore::EIntegrate::Rebase, nullptr).bOk);
	TestEqualSensitive(TEXT("HEAD is unchanged"), GitTest::Output(GitTest::Git(Clones.A, {TEXT("rev-parse"), TEXT("HEAD")})), Before);
	TestFalse(TEXT("No rebase is left in progress"), Repository->IsOperationInProgress());
	TestEqualSensitive(TEXT("The local edit is intact"), GitTest::ReadFile(Clones.A, TEXT("shared.txt")), FString(TEXT("line 1\nA's line 2\nline 3\n")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitRemotePushedPathsTest, "GitWorkspace.Remote.PushedPaths", RemoteTestFlags)
bool FGitRemotePushedPathsTest::RunTest(const FString&)
{
	FTwoClones Clones;
	const GitCore::FGitRepositoryPtr Repository = GitTest::OpenRepository(*this, Clones.A);
	if (!Repository.IsValid())
	{
		return false;
	}
	auto Pushed = [this, &Repository]()
	{
		TSet<FString> Paths;
		FString Error;
		TestTrue(TEXT("What a push would change is read: ") + Error, Repository->ReadPushedPaths(TEXT("origin"), TEXT("HEAD"), Paths, Error));
		TArray<FString> Sorted = Paths.Array();
		Sorted.Sort();
		return GitTest::Joined(Sorted);
	};
	GitTest::WriteFile(Clones.A, TEXT("doomed.txt"), TEXT("doomed\n"));
	GitTest::CommitAll(Clones.A, TEXT("A adds a file to delete later"));
	GitTest::Git(Clones.A, {TEXT("push"), TEXT("-q"), TEXT("origin"), TEXT("main")});
	GitTest::Git(Clones.A, {TEXT("switch"), TEXT("-q"), TEXT("-c"), TEXT("feature")});
	GitTest::WriteFile(Clones.A, TEXT("feature.txt"), TEXT("feature\n"));
	GitTest::CommitAll(Clones.A, TEXT("Feature work"));
	GitTest::Git(Clones.A, {TEXT("push"), TEXT("-q"), TEXT("-u"), TEXT("origin"), TEXT("feature")});
	TestEqualSensitive(TEXT("A published branch changes nothing more"), Pushed(), FString());

	// Merging in a teammate's work that is already on the server adds nothing to the push.
	GitTest::Git(Clones.B, {TEXT("pull"), TEXT("-q"), TEXT("--ff-only")});
	Clones.PushFromB(TEXT("shared.txt"), TEXT("line 1\nB's line 2\nline 3\n"), TEXT("B edits line 2"));
	Repository->Fetch(TEXT("origin"), GitCore::ECallKind::UserNetwork, nullptr);
	GitTest::Git(Clones.A, {TEXT("merge"), TEXT("-q"), TEXT("--no-edit"), TEXT("origin/main")});
	TestEqualSensitive(TEXT("The merge brings in shared.txt from main"), GitTest::ReadFile(Clones.A, TEXT("shared.txt")), FString(TEXT("line 1\nB's line 2\nline 3\n")));
	TestEqualSensitive(TEXT("A push of the merge changes nothing on the server"), Pushed(), FString());

	// Edits and deletions of A's own count.
	GitTest::WriteFile(Clones.A, TEXT("a.txt"), TEXT("a\n"));
	GitTest::Git(Clones.A, {TEXT("rm"), TEXT("-q"), TEXT("doomed.txt")});
	GitTest::CommitAll(Clones.A, TEXT("A adds and deletes"));
	TestEqualSensitive(TEXT("Its own edit and deletion count"), Pushed(), GitTest::Joined({TEXT("a.txt"), TEXT("doomed.txt")}));

	// A conflict resolved with content of A's own is A's change, though the file came from both sides.
	Clones.PushFromB(TEXT("shared.txt"), TEXT("line 1\nB's line 2 again\nline 3\n"), TEXT("B edits line 2 again"));
	GitTest::WriteFile(Clones.A, TEXT("shared.txt"), TEXT("line 1\nA's line 2\nline 3\n"));
	GitTest::CommitAll(Clones.A, TEXT("A edits line 2"));
	GitTest::Git(Clones.A, {TEXT("push"), TEXT("-q"), TEXT("origin"), TEXT("feature")});
	Repository->Fetch(TEXT("origin"), GitCore::ECallKind::UserNetwork, nullptr);
	GitTest::Git(Clones.A, {TEXT("merge"), TEXT("-q"), TEXT("--no-edit"), TEXT("origin/main")});
	GitTest::WriteFile(Clones.A, TEXT("shared.txt"), TEXT("line 1\nA's and B's line 2\nline 3\n"));
	GitTest::Git(Clones.A, {TEXT("add"), TEXT("shared.txt")});
	GitTest::Git(Clones.A, {TEXT("commit"), TEXT("-q"), TEXT("--no-edit"), TEXT("--no-verify")});
	TestEqualSensitive(TEXT("The resolution counts"), Pushed(), FString(TEXT("shared.txt")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitRemotePushTest, "GitWorkspace.Remote.PushUpstreamAndRejection", RemoteTestFlags)
bool FGitRemotePushTest::RunTest(const FString&)
{
	FTwoClones Clones;
	const GitCore::FGitRepositoryPtr Repository = GitTest::OpenRepository(*this, Clones.A);
	if (!Repository.IsValid())
	{
		return false;
	}
	GitTest::Git(Clones.A, {TEXT("switch"), TEXT("-q"), TEXT("-c"), TEXT("feature/new")});
	GitTest::WriteFile(Clones.A, TEXT("feature.txt"), TEXT("feature\n"));
	GitTest::CommitAll(Clones.A, TEXT("feature work"));
	GitCore::FStatusSnapshot Status = ReadStatus(*this, *Repository);
	TestTrue(TEXT("A new branch has no upstream"), Status.Upstream.IsEmpty());
	const GitCore::FOpResult Published = GitTest::PushBranch(*Repository, Status, Repository->DefaultRemote());
	TestTrue(TEXT("Pushing publishes the branch: ") + Published.Message, Published.bOk);
	TestTrue(TEXT("The remote has the branch"), GitTest::Git(Clones.Origin, {TEXT("rev-parse"), TEXT("--verify"), TEXT("refs/heads/feature/new")}).Succeeded());
	TestEqualSensitive(TEXT("The upstream is now set"), ReadStatus(*this, *Repository).Upstream, FString(TEXT("origin/feature/new")));

	GitTest::Git(Clones.A, {TEXT("switch"), TEXT("-q"), TEXT("main")});
	Clones.PushFromB(TEXT("b.txt"), TEXT("b\n"), TEXT("B moves main"));
	GitTest::WriteFile(Clones.A, TEXT("a.txt"), TEXT("a\n"));
	GitTest::CommitAll(Clones.A, TEXT("A commits on old main"));
	Status = ReadStatus(*this, *Repository);
	const GitCore::FOpResult Rejected = GitTest::PushBranch(*Repository, Status, TEXT("origin"));
	TestFalse(TEXT("A push behind the server is rejected"), Rejected.bOk);
	TestTrue(TEXT("The message says to pull first"), Rejected.Message.Contains(TEXT("Pull"), ESearchCase::CaseSensitive));
	return true;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitRemotePushTargetTest, "GitWorkspace.Remote.PushTarget", RemoteTestFlags)
bool FGitRemotePushTargetTest::RunTest(const FString&)
{
	FTwoClones Clones;
	const GitCore::FGitRepositoryPtr Repository = GitTest::OpenRepository(*this, Clones.A);
	if (!Repository.IsValid())
	{
		return false;
	}
	const FString MainBefore = GitTest::Output(GitTest::Git(Clones.Origin, {TEXT("rev-parse"), TEXT("main")}));
	GitCore::FPushTarget Target;
	TestTrue(TEXT("main resolves"), Repository->ResolvePush(ReadStatus(*this, *Repository), TEXT("origin"), Target).bOk);
	TestTrue(TEXT("main pushes to origin/main"), Target.Remote.Equals(TEXT("origin"), ESearchCase::CaseSensitive) && Target.Branch.Equals(TEXT("main"), ESearchCase::CaseSensitive) && !Target.bSetUpstream);
	TestFalse(TEXT("Pushing main asks nothing"), Target.NeedsAgreement());

	// A branch made from origin/main follows it, but its commits go to a branch with its own name.
	GitTest::Git(Clones.A, {TEXT("switch"), TEXT("-q"), TEXT("-c"), TEXT("feature"), TEXT("--track"), TEXT("origin/main")});
	GitTest::WriteFile(Clones.A, TEXT("feature.txt"), TEXT("feature\n"));
	GitTest::CommitAll(Clones.A, TEXT("feature work"));
	GitCore::FStatusSnapshot Status = ReadStatus(*this, *Repository);
	TestEqualSensitive(TEXT("feature follows origin/main"), Status.Upstream, FString(TEXT("origin/main")));
	TestTrue(TEXT("feature resolves"), Repository->ResolvePush(Status, TEXT("origin"), Target).bOk);
	TestTrue(TEXT("It goes to origin/feature, not main"), Target.Remote.Equals(TEXT("origin"), ESearchCase::CaseSensitive) && Target.Branch.Equals(TEXT("feature"), ESearchCase::CaseSensitive) && Target.bSetUpstream);
	TestEqualSensitive(TEXT("It names the upstream it leaves"), Target.PreviousUpstream, FString(TEXT("origin/main")));
	TestTrue(TEXT("Publishing it asks first"), !Target.bExists && Target.NeedsAgreement());
	const GitCore::FOpResult Pushed = Repository->Push(Target, nullptr);
	TestTrue(TEXT("The push succeeds: ") + Pushed.Message, Pushed.bOk);
	TestEqualSensitive(TEXT("main on the server did not move"), GitTest::Output(GitTest::Git(Clones.Origin, {TEXT("rev-parse"), TEXT("main")})), MainBefore);
	TestEqualSensitive(TEXT("origin/feature has the commit"), GitTest::Output(GitTest::Git(Clones.Origin, {TEXT("rev-parse"), TEXT("feature")})),
		GitTest::Output(GitTest::Git(Clones.A, {TEXT("rev-parse"), TEXT("HEAD")})));
	Status = ReadStatus(*this, *Repository);
	TestEqualSensitive(TEXT("feature follows origin/feature now"), Status.Upstream, FString(TEXT("origin/feature")));
	Repository->ResolvePush(Status, TEXT("origin"), Target);
	TestFalse(TEXT("Its next push asks nothing"), Target.NeedsAgreement());

	// push.default=upstream asks for the upstream whatever its name, and git push would go there too.
	GitTest::Git(Clones.A, {TEXT("switch"), TEXT("-q"), TEXT("-c"), TEXT("hotfix"), TEXT("--track"), TEXT("origin/main")});
	GitTest::Git(Clones.A, {TEXT("config"), TEXT("push.default"), TEXT("upstream")});
	Repository->ResolvePush(ReadStatus(*this, *Repository), TEXT("origin"), Target);
	TestTrue(TEXT("With push.default=upstream it goes to main"), Target.Branch.Equals(TEXT("main"), ESearchCase::CaseSensitive) && !Target.bSetUpstream && !Target.NeedsAgreement());
	GitTest::Git(Clones.A, {TEXT("config"), TEXT("--unset"), TEXT("push.default")});

	// A branch following a local branch is published on the remote; pushing to "." would move that branch.
	GitTest::Git(Clones.A, {TEXT("switch"), TEXT("-q"), TEXT("-c"), TEXT("local"), TEXT("--track"), TEXT("main")});
	Status = ReadStatus(*this, *Repository);
	TestEqualSensitive(TEXT("local follows main"), Status.Upstream, FString(TEXT("main")));
	Repository->ResolvePush(Status, TEXT("origin"), Target);
	TestTrue(TEXT("It goes to origin/local"), Target.Remote.Equals(TEXT("origin"), ESearchCase::CaseSensitive) && Target.Branch.Equals(TEXT("local"), ESearchCase::CaseSensitive) && Target.bSetUpstream && Target.NeedsAgreement());
	return true;
}

#endif
