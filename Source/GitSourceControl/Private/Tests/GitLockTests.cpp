// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#if WITH_DEV_AUTOMATION_TESTS

#include "GitTestFixtures.h"

namespace
{
	constexpr EAutomationTestFlags LockTestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitLockLifecycleTest, "GitWorkspace.Locks.Lifecycle", LockTestFlags)
bool FGitLockLifecycleTest::RunTest(const FString&)
{
	GitTest::FLockedTeam Team(*this);
	if (!Team.IsReady())
	{
		return false;
	}
	const GitCore::FGitRepositoryPtr Alice = GitTest::OpenRepository(*this, Team.Alice);
	const GitCore::FGitRepositoryPtr Bob = GitTest::OpenRepository(*this, Team.Bob);
	if (!Alice.IsValid() || !Bob.IsValid())
	{
		return false;
	}
	FString Endpoint;
	FString Error;
	TestTrue(TEXT("The lock endpoint resolves"), Alice->ResolveLockEndpoint(TEXT("origin"), Endpoint, Error) && Endpoint.Contains(TEXT("/users/alice/")));

	GitCore::FLockSnapshot AliceLocks;
	TestTrue(TEXT("Verification succeeds"), Alice->VerifyLocks(TEXT("origin"), GitCore::ECallKind::UserNetwork, AliceLocks) && AliceLocks.bVerified);
	TestTrue(TEXT("Nothing is locked yet"), AliceLocks.ByPath.IsEmpty());

	GitCore::FLockInfo Taken;
	const GitCore::FOpResult Locked = Alice->Lock(TEXT("origin"), TEXT("Content/Hero.uasset"), Taken);
	TestTrue(TEXT("Alice takes the lock: ") + Locked.Message, Locked.bOk && !Taken.Id.IsEmpty() && Taken.bOurs);
	GitCore::FLockSnapshot BobLocks;
	Bob->VerifyLocks(TEXT("origin"), GitCore::ECallKind::UserNetwork, BobLocks);
	const GitCore::FLockInfo* Seen = BobLocks.Find(TEXT("Content/Hero.uasset"));
	TestTrue(TEXT("Bob sees the lock as someone else's"), Seen && !Seen->bOurs && !Seen->Owner.IsEmpty());
	GitCore::FLockInfo Refused;
	const GitCore::FOpResult Conflict = Bob->Lock(TEXT("origin"), TEXT("Content/Hero.uasset"), Refused);
	TestFalse(TEXT("Bob cannot take the same lock"), Conflict.bOk);
	TestTrue(TEXT("The refusal says it is already locked"), Conflict.Message.Contains(TEXT("already locked")));
	TestFalse(TEXT("Bob cannot release Alice's lock without forcing"), Bob->Unlock(TEXT("origin"), Taken.Id, false).bOk);

	TestTrue(TEXT("Alice releases by id"), Alice->Unlock(TEXT("origin"), Taken.Id, false).bOk);
	Bob->VerifyLocks(TEXT("origin"), GitCore::ECallKind::UserNetwork, BobLocks);
	TestTrue(TEXT("The lock is gone"), BobLocks.ByPath.IsEmpty());

	// A failed check keeps the last known locks and explains why they are old.
	Alice->Lock(TEXT("origin"), TEXT("Content/Map.umap"), Taken);
	Alice->VerifyLocks(TEXT("origin"), GitCore::ECallKind::UserNetwork, AliceLocks);
	Team.Server->SetMode(TEXT("offline"));
	TestFalse(TEXT("An unreachable server fails verification"), Alice->VerifyLocks(TEXT("origin"), GitCore::ECallKind::BackgroundNetwork, AliceLocks));
	TestTrue(TEXT("Known locks are kept"), AliceLocks.Find(TEXT("Content/Map.umap")) != nullptr);
	TestFalse(TEXT("The failure is explained"), AliceLocks.Error.IsEmpty());
	Team.Server->SetMode(TEXT(""));
	TestTrue(TEXT("Verification recovers"), Alice->VerifyLocks(TEXT("origin"), GitCore::ECallKind::UserNetwork, AliceLocks) && AliceLocks.Error.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitLfsPushTest, "GitWorkspace.Locks.LfsUploadBeforePush", LockTestFlags)
bool FGitLfsPushTest::RunTest(const FString&)
{
	GitTest::FLockedTeam Team(*this);
	if (!Team.IsReady())
	{
		return false;
	}
	const GitCore::FGitRepositoryPtr Alice = GitTest::OpenRepository(*this, Team.Alice);
	const GitCore::FGitRepositoryPtr Bob = GitTest::OpenRepository(*this, Team.Bob);
	if (!Alice.IsValid() || !Bob.IsValid())
	{
		return false;
	}
	// No pre-push hook: the explicit upload must still happen before the ref update.
	IFileManager::Get().DeleteDirectory(*(Team.Alice / TEXT(".git/test-hooks")), false, true);
	GitTest::WriteFile(Team.Alice, TEXT("Content/Hero.uasset"), TEXT("hero v2 from alice\n"));
	GitTest::CommitAll(Team.Alice, TEXT("Hero v2"));
	GitCore::FStatusSnapshot Status;
	FString Error;
	Alice->ReadStatus(Status, Error, true);
	const GitCore::FOpResult Pushed = Alice->Push(Status, TEXT("origin"), nullptr);
	TestTrue(TEXT("Push succeeds: ") + Pushed.Message, Pushed.bOk);

	TestTrue(TEXT("Bob fetches"), Bob->Fetch(TEXT("origin"), GitCore::ECallKind::UserNetwork, nullptr).bOk);
	TestTrue(TEXT("Bob fast-forwards and LFS downloads the new asset"), Bob->Integrate(TEXT("origin/main"), false, nullptr).bOk);
	TestEqual(TEXT("Bob has the real bytes"), GitTest::ReadFile(Team.Bob, TEXT("Content/Hero.uasset")), FString(TEXT("hero v2 from alice\n")));

	Team.Server->SetMode(TEXT("uploadfail"));
	GitTest::WriteFile(Team.Alice, TEXT("Content/Hero.uasset"), TEXT("hero v3\n"));
	GitTest::CommitAll(Team.Alice, TEXT("Hero v3"));
	Alice->ReadStatus(Status, Error, true);
	const FString RemoteBefore = GitTest::Output(GitTest::Git(Team.Origin, {TEXT("rev-parse"), TEXT("main")}));
	TestFalse(TEXT("A failed LFS upload stops the push"), Alice->Push(Status, TEXT("origin"), nullptr).bOk);
	TestEqual(TEXT("The remote branch did not move"), GitTest::Output(GitTest::Git(Team.Origin, {TEXT("rev-parse"), TEXT("main")})), RemoteBefore);
	return true;
}

#endif
