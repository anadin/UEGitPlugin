// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#if WITH_DEV_AUTOMATION_TESTS

#include "GitTestFixtures.h"
#include "Misc/ScopeExit.h"

namespace
{
	constexpr EAutomationTestFlags SessionTestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

	GitCore::FGitSessionPtr OpenSession(FAutomationTestBase& Test, const FString& Repo)
	{
		FString Error;
		GitCore::FGitSessionPtr Session = GitCore::FGitSession::Open(GitTest::GitBinary(), Repo, Error);
		if (!Session.IsValid())
		{
			Test.AddError(TEXT("Could not open a session: ") + Error);
		}
		return Session;
	}

	/** Runs Work on the session's mutation lane and waits for it. */
	void OnMutationLane(GitCore::FGitSession& Session, TUniqueFunction<void()> Work)
	{
		Session.EnqueueMutation(MoveTemp(Work));
		Session.FlushMutations();
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitSessionSnapshotTest, "GitWorkspace.Session.SnapshotAndScopedRefresh", SessionTestFlags)
bool FGitSessionSnapshotTest::RunTest(const FString&)
{
	GitTest::FFixture Fixture;
	const FString Repo = Fixture.InitRepo();
	GitTest::WriteFile(Repo, TEXT(".gitattributes"), TEXT("*.uasset lockable\n"));
	GitTest::WriteFile(Repo, TEXT("Content/Clean.uasset"), TEXT("clean\n"));
	GitTest::WriteFile(Repo, TEXT("Content/Edited.uasset"), TEXT("base\n"));
	GitTest::WriteFile(Repo, TEXT("notes.txt"), TEXT("base\n"));
	GitTest::CommitAll(Repo, TEXT("base"));
	const GitCore::FGitSessionPtr Session = OpenSession(*this, Repo);
	if (!Session.IsValid())
	{
		return false;
	}
	ON_SCOPE_EXIT { Session->Close(); };
	FString Error;
	GitTest::WriteFile(Repo, TEXT("Content/Edited.uasset"), TEXT("edited\n"));
	GitTest::WriteFile(Repo, TEXT("Content/New.uasset"), TEXT("new\n"));
	bool bRefreshed = false;
	OnMutationLane(*Session, [&]() { bRefreshed = Session->RefreshStatus(nullptr, &Error); });
	GitCore::FSessionSnapshotRef Snapshot = Session->GetSnapshot();
	TestTrue(TEXT("A full refresh succeeds: ") + Error, bRefreshed && Snapshot->bStatusValid);
	TestEqual(TEXT("Two changed files"), Snapshot->Status.Entries.Num(), 2);
	TestTrue(TEXT("Clean files are known as tracked"), Snapshot->IsTracked(TEXT("Content/Clean.uasset")));
	TestFalse(TEXT("New files are not tracked"), Snapshot->IsTracked(TEXT("Content/New.uasset")));
	const GitCore::FFileAttributes* Edited = Snapshot->FindAttributes(TEXT("Content/Edited.uasset"));
	TestTrue(TEXT("Changed files carry their attributes"), Edited && Edited->bLockable);
	TestTrue(TEXT("The repository declares lockable files"), Snapshot->bLockableRules);
	TestTrue(TEXT("Attributes on request"), Session->EnsureAttributes({TEXT("Content/Clean.uasset")}) && Session->GetSnapshot()->FindAttributes(TEXT("Content/Clean.uasset"))->bLockable);

	// A scoped refresh updates only the requested paths.
	GitTest::WriteFile(Repo, TEXT("notes.txt"), TEXT("edited\n"));
	IFileManager::Get().Delete(*(Repo / TEXT("Content/New.uasset")));
	const TArray<FString> Requested = {TEXT("notes.txt")};
	OnMutationLane(*Session, [&]() { Session->RefreshStatus(&Requested, &Error); });
	Snapshot = Session->GetSnapshot();
	TestTrue(TEXT("The requested file appears"), Snapshot->Status.Find(TEXT("notes.txt")) != nullptr);
	TestTrue(TEXT("Unrequested entries are kept until a full refresh"), Snapshot->Status.Find(TEXT("Content/New.uasset")) != nullptr);
	OnMutationLane(*Session, [&]() { Session->RefreshStatus(nullptr, &Error); });
	TestTrue(TEXT("A full refresh drops the deleted file"), Session->GetSnapshot()->Status.Find(TEXT("Content/New.uasset")) == nullptr);

	// Marked-for-add files are remembered until git tracks them.
	GitTest::WriteFile(Repo, TEXT("Content/Added.uasset"), TEXT("added\n"));
	OnMutationLane(*Session, [&]() { Session->RefreshStatus(nullptr, &Error); });
	Session->SetMarkedForAdd({TEXT("Content/Added.uasset")}, true);
	TestTrue(TEXT("Marked for add"), Session->GetSnapshot()->MarkedForAdd.Contains(TEXT("Content/Added.uasset")));
	const GitCore::FGitSessionPtr Reopened = OpenSession(*this, Repo);
	TestTrue(TEXT("The same worktree reuses its session"), Reopened == Session);
	GitTest::CommitAll(Repo, TEXT("commit everything"));
	OnMutationLane(*Session, [&]() { Session->RefreshStatus(nullptr, &Error); });
	TestFalse(TEXT("Committed files are no longer marked"), Session->GetSnapshot()->MarkedForAdd.Contains(TEXT("Content/Added.uasset")));

	// Listeners hear about changes on the game thread, coalesced.
	int32 Broadcasts = 0;
	GitCore::EChange Heard = GitCore::EChange::None;
	const FDelegateHandle Handle = Session->OnChanged().AddLambda([&](GitCore::EChange Changes)
	{
		++Broadcasts;
		Heard |= Changes;
	});
	GitTest::WriteFile(Repo, TEXT("notes.txt"), TEXT("again\n"));
	Session->RequestStatusRefresh();
	Session->RequestStatusRefresh();
	TestTrue(TEXT("A change notification arrives"), GitTest::WaitFor([&]() { return EnumHasAnyFlags(Heard, GitCore::EChange::Status); }));
	TestTrue(TEXT("Requests are coalesced"), Broadcasts <= 2);
	Session->OnChanged().Remove(Handle);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitSessionLockJournalTest, "GitWorkspace.Session.LockJournal", SessionTestFlags)
bool FGitSessionLockJournalTest::RunTest(const FString&)
{
	GitTest::FFixture Fixture;
	GitTest::FLockServer Server(*this, Fixture.Root / TEXT("server"));
	if (!Server.IsRunning())
	{
		return false;
	}
	const FString Origin = Fixture.InitBare();
	const FString Repo = Fixture.InitRepo();
	GitTest::EnableLfs(Repo, Server.Endpoint());
	GitTest::WriteFile(Repo, TEXT("Content/Hero.uasset"), TEXT("hero\n"));
	GitTest::CommitAll(Repo, TEXT("base"));
	GitTest::Git(Repo, {TEXT("remote"), TEXT("add"), TEXT("origin"), Origin});
	GitTest::Git(Repo, {TEXT("push"), TEXT("-q"), TEXT("-u"), TEXT("origin"), TEXT("main")});
	const GitCore::FGitSessionPtr Session = OpenSession(*this, Repo);
	if (!Session.IsValid())
	{
		return false;
	}
	ON_SCOPE_EXIT { Session->Close(); };
	FString Error;
	OnMutationLane(*Session, [&]() { Session->RefreshStatus(nullptr, &Error); });
	TestEqual(TEXT("Locks use the upstream remote"), Session->GetLockRemote(), FString(TEXT("origin")));
	TestTrue(TEXT("Locks verify"), Session->RefreshLocks(GitCore::ECallKind::UserNetwork, &Error));

	GitCore::FLockInfo Lock;
	TestTrue(TEXT("Lock the asset"), Session->GetRepository().Lock(TEXT("origin"), TEXT("Content/Hero.uasset"), Lock).bOk);
	Session->RecordLockTaken(Lock);
	TestTrue(TEXT("The lock is recorded as taken here"), Session->GetSnapshot()->LocksTakenHere.Contains(Lock.Id));

	// Branch switches and configuration edits must not strand the lock.
	GitTest::Git(Repo, {TEXT("switch"), TEXT("-q"), TEXT("-c"), TEXT("feature")});
	GitTest::Git(Repo, {TEXT("config"), TEXT("user.name"), TEXT("Renamed User")});
	OnMutationLane(*Session, [&]() { Session->RefreshStatus(nullptr, &Error); });
	TestTrue(TEXT("Locks verify after switching branch"), Session->RefreshLocks(GitCore::ECallKind::UserNetwork, &Error));
	TestTrue(TEXT("The lock is still known as taken here"), Session->GetSnapshot()->LocksTakenHere.Contains(Lock.Id));

	// Another worktree of the same clone sees the lock as ours but taken elsewhere.
	const FString Other = Fixture.Root / TEXT("other-worktree");
	GitTest::Git(Repo, {TEXT("worktree"), TEXT("add"), TEXT("-q"), Other, TEXT("main")});
	const GitCore::FGitSessionPtr OtherSession = OpenSession(*this, Other);
	if (OtherSession.IsValid())
	{
		ON_SCOPE_EXIT { OtherSession->Close(); };
		OnMutationLane(*OtherSession, [&]() { OtherSession->RefreshStatus(nullptr, &Error); });
		TestTrue(TEXT("The other worktree verifies"), OtherSession->RefreshLocks(GitCore::ECallKind::UserNetwork, &Error));
		const GitCore::FSessionSnapshotRef OtherSnapshot = OtherSession->GetSnapshot();
		const GitCore::FLockInfo* Seen = OtherSnapshot->Locks.Find(TEXT("Content/Hero.uasset"));
		TestTrue(TEXT("The server says the lock is ours"), Seen && Seen->bOurs);
		TestFalse(TEXT("But it was not taken in this worktree"), OtherSnapshot->LocksTakenHere.Contains(Lock.Id));
		TestTrue(TEXT("So it is yours from elsewhere"), Seen && OtherSnapshot->IsOursElsewhere(*Seen));

		// Claiming moves the record to the other worktree: the first one now sees the lock as taken elsewhere.
		TestTrue(TEXT("Claim the lock in the other worktree"), Seen && OtherSession->RecordLockTaken(*Seen, &Error));
		TestTrue(TEXT("The other worktree verifies again"), OtherSession->RefreshLocks(GitCore::ECallKind::UserNetwork, &Error));
		TestTrue(TEXT("The claim survives a lock check"), OtherSession->GetSnapshot()->LocksTakenHere.Contains(Lock.Id));
		TestTrue(TEXT("The first worktree verifies"), Session->RefreshLocks(GitCore::ECallKind::UserNetwork, &Error));
		TestFalse(TEXT("The first worktree no longer holds it here"), Session->GetSnapshot()->LocksTakenHere.Contains(Lock.Id));
	}

	TestTrue(TEXT("Release the lock"), Session->GetRepository().Unlock(TEXT("origin"), Lock.Id, false).bOk);
	Session->RecordLockReleased(TEXT("Content/Hero.uasset"));
	TestFalse(TEXT("Release clears the record"), Session->GetSnapshot()->LocksTakenHere.Contains(Lock.Id));
	TestFalse(TEXT("The journal forgets released locks"), Session->GetJournal().IsOurs(Session->GetSnapshot()->Locks.Endpoint, Lock.Id));
	return true;
}

#endif
