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
	TestEqual(TEXT("The upstream remote is the default"), Repository->DefaultRemote(), FString(TEXT("origin")));

	Clones.PushFromB(TEXT("from-b.txt"), TEXT("b\n"), TEXT("B adds a file"));
	const FString BHead = GitTest::Output(GitTest::Git(Clones.B, {TEXT("rev-parse"), TEXT("HEAD")}));
	TestTrue(TEXT("Fetch succeeds"), Repository->Fetch(TEXT("origin"), GitCore::ECallKind::UserNetwork, nullptr).bOk);
	TestEqual(TEXT("Fetch updates the remote-tracking branch"), GitTest::Output(GitTest::Git(Clones.A, {TEXT("rev-parse"), TEXT("origin/main")})), BHead);
	GitCore::FStatusSnapshot Status = ReadStatus(*this, *Repository);
	TestEqual(TEXT("Status knows the upstream"), Status.Upstream, FString(TEXT("origin/main")));
	GitCore::FRemoteState Remote;
	FString Error;
	TestTrue(TEXT("Remote state is read from local refs"), Repository->ReadRemoteState(Status, Remote, Error));
	TestTrue(TEXT("One incoming commit"), Remote.Behind == 1 && Remote.Ahead == 0);
	TestTrue(TEXT("Incoming paths are listed"), Remote.IncomingPaths.Contains(TEXT("from-b.txt")));
	const GitCore::FOpResult FastForward = Repository->Integrate(Remote.Upstream, false, nullptr);
	TestTrue(TEXT("Fast-forward integrates: ") + FastForward.Message + TEXT(" ") + FastForward.Details, FastForward.bOk);
	TestEqual(TEXT("HEAD now matches B"), GitTest::Output(GitTest::Git(Clones.A, {TEXT("rev-parse"), TEXT("HEAD")})), BHead);

	// Diverged but compatible: local commits are rebased onto the incoming ones.
	GitTest::WriteFile(Clones.A, TEXT("from-a.txt"), TEXT("a\n"));
	GitTest::CommitAll(Clones.A, TEXT("A adds a file"));
	GitTest::WriteFile(Clones.A, TEXT("uncommitted.txt"), TEXT("keep me\n"));
	Clones.PushFromB(TEXT("from-b-2.txt"), TEXT("b2\n"), TEXT("B adds another file"));
	Repository->Fetch(TEXT("origin"), GitCore::ECallKind::UserNetwork, nullptr);
	Status = ReadStatus(*this, *Repository);
	Repository->ReadRemoteState(Status, Remote, Error);
	TestTrue(TEXT("Diverged history is reported"), Remote.Ahead == 1 && Remote.Behind == 1);
	TestTrue(TEXT("Outgoing paths are listed"), Remote.OutgoingPaths.Contains(TEXT("from-a.txt")));
	bool bConflicts = true;
	TestTrue(TEXT("Conflict prediction runs"), Repository->PredictConflicts(Remote.Upstream, bConflicts, Error));
	TestFalse(TEXT("No conflict is predicted"), bConflicts);
	const GitCore::FOpResult Rebased = Repository->Integrate(Remote.Upstream, true, nullptr);
	TestTrue(TEXT("Rebase integrates: ") + Rebased.Message, Rebased.bOk);
	TestEqual(TEXT("Local work sits on top of the incoming commit"), GitTest::Output(GitTest::Git(Clones.A, {TEXT("rev-list"), TEXT("--count"), TEXT("origin/main..HEAD")})), FString(TEXT("1")));
	TestTrue(TEXT("Both files are present"), GitTest::FileExists(Clones.A, TEXT("from-a.txt")) && GitTest::FileExists(Clones.A, TEXT("from-b-2.txt")));
	TestEqual(TEXT("Uncommitted work survives"), GitTest::ReadFile(Clones.A, TEXT("uncommitted.txt")), FString(TEXT("keep me\n")));

	// Conflicting history: prediction says so, and a forced attempt leaves nothing half-done.
	GitTest::WriteFile(Clones.A, TEXT("shared.txt"), TEXT("line 1\nA's line 2\nline 3\n"));
	GitTest::CommitAll(Clones.A, TEXT("A edits line 2"));
	Clones.PushFromB(TEXT("shared.txt"), TEXT("line 1\nB's line 2\nline 3\n"), TEXT("B edits line 2"));
	Repository->Fetch(TEXT("origin"), GitCore::ECallKind::UserNetwork, nullptr);
	TestTrue(TEXT("Conflict prediction runs again"), Repository->PredictConflicts(TEXT("origin/main"), bConflicts, Error));
	TestTrue(TEXT("The conflict is predicted"), bConflicts);
	const FString Before = GitTest::Output(GitTest::Git(Clones.A, {TEXT("rev-parse"), TEXT("HEAD")}));
	TestFalse(TEXT("A conflicting rebase fails"), Repository->Integrate(TEXT("origin/main"), true, nullptr).bOk);
	TestEqual(TEXT("HEAD is unchanged"), GitTest::Output(GitTest::Git(Clones.A, {TEXT("rev-parse"), TEXT("HEAD")})), Before);
	TestFalse(TEXT("No rebase is left in progress"), Repository->IsOperationInProgress());
	TestEqual(TEXT("The local edit is intact"), GitTest::ReadFile(Clones.A, TEXT("shared.txt")), FString(TEXT("line 1\nA's line 2\nline 3\n")));
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
	const GitCore::FOpResult Published = Repository->Push(Status, Repository->DefaultRemote(), nullptr);
	TestTrue(TEXT("Pushing publishes the branch: ") + Published.Message, Published.bOk);
	TestTrue(TEXT("The remote has the branch"), GitTest::Git(Clones.Origin, {TEXT("rev-parse"), TEXT("--verify"), TEXT("refs/heads/feature/new")}).Succeeded());
	TestEqual(TEXT("The upstream is now set"), ReadStatus(*this, *Repository).Upstream, FString(TEXT("origin/feature/new")));

	GitTest::Git(Clones.A, {TEXT("switch"), TEXT("-q"), TEXT("main")});
	Clones.PushFromB(TEXT("b.txt"), TEXT("b\n"), TEXT("B moves main"));
	GitTest::WriteFile(Clones.A, TEXT("a.txt"), TEXT("a\n"));
	GitTest::CommitAll(Clones.A, TEXT("A commits on old main"));
	Status = ReadStatus(*this, *Repository);
	const GitCore::FOpResult Rejected = Repository->Push(Status, TEXT("origin"), nullptr);
	TestFalse(TEXT("A push behind the server is rejected"), Rejected.bOk);
	TestTrue(TEXT("The message says to pull first"), Rejected.Message.Contains(TEXT("Pull")));
	return true;
}

#endif
