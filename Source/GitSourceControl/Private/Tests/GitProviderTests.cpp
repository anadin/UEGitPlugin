// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#if WITH_DEV_AUTOMATION_TESTS

#include "GitTestFixtures.h"
#include "Provider/GitStateBuilder.h"

#include "SourceControlOperations.h"

namespace
{
	constexpr EAutomationTestFlags ProviderTestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

	ECommandResult::Type Run(GitTest::FTestProvider& Provider, const FSourceControlOperationRef& Operation, const TArray<FString>& Files)
	{
		return Provider->Execute(Operation, nullptr, Files, EConcurrency::Synchronous);
	}

	FSourceControlStatePtr GetState(GitTest::FTestProvider& Provider, const FString& File, EStateCacheUsage::Type Usage = EStateCacheUsage::Use)
	{
		TArray<FSourceControlStateRef> States;
		Provider->GetState({File}, States, Usage);
		return States.Num() == 1 ? States[0] : FSourceControlStatePtr();
	}

	FString Messages(const FSourceControlOperationRef& Operation)
	{
		const FSourceControlResultInfo& Info = Operation->GetResultInfo();
		TArray<FString> Lines;
		for (const FText& Message : Info.ErrorMessages)
		{
			Lines.Add(Message.ToString());
		}
		for (const FText& Message : Info.InfoMessages)
		{
			Lines.Add(Message.ToString());
		}
		return FString::Join(Lines, TEXT(" | "));
	}

	TArray<FString> Lines(const GitCore::FProcessResult& Result)
	{
		TArray<FString> Out;
		Result.StdoutText().ParseIntoArrayLines(Out);
		Out.Sort();
		return Out;
	}

	FString Head(const FString& Repo, const FString& Ref = TEXT("HEAD"))
	{
		return GitTest::Output(GitTest::Git(Repo, {TEXT("rev-parse"), Ref}));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitProviderStateMappingTest, "GitWorkspace.Provider.StateMapping", ProviderTestFlags)
bool FGitProviderStateMappingTest::RunTest(const FString&)
{
	using namespace GitCore;
	FSessionSnapshot Snapshot;
	Snapshot.bStatusValid = true;
	auto AddEntry = [&Snapshot](const TCHAR* Path, TCHAR Index, TCHAR Working, bool bUntracked = false, bool bConflict = false, const TCHAR* Original = TEXT(""))
	{
		FStatusEntry Entry;
		Entry.Path = Path;
		Entry.Index = Index;
		Entry.Working = Working;
		Entry.bUntracked = bUntracked;
		Entry.bConflict = bConflict;
		Entry.OriginalPath = Original;
		Snapshot.Status.Entries.Add(Entry);
	};
	AddEntry(TEXT("Source/Code.cpp"), TEXT('.'), TEXT('M'));
	AddEntry(TEXT("Content/Modified.uasset"), TEXT('.'), TEXT('M'));
	AddEntry(TEXT("Content/Deleted.uasset"), TEXT('.'), TEXT('D'));
	AddEntry(TEXT("Content/New.uasset"), TEXT('.'), TEXT('.'), true);
	AddEntry(TEXT("Content/Marked.uasset"), TEXT('.'), TEXT('.'), true);
	AddEntry(TEXT("Content/Staged.uasset"), TEXT('A'), TEXT('.'));
	AddEntry(TEXT("Content/Conflict.uasset"), TEXT('U'), TEXT('U'), false, true);
	AddEntry(TEXT("Content/Renamed.uasset"), TEXT('R'), TEXT('.'), false, false, TEXT("Content/Old.uasset"));
	Snapshot.Status.BuildLookup();

	const TArray<FString> Tracked = {TEXT("readme.txt"), TEXT("Source/Code.cpp"), TEXT("Content/Clean.uasset"), TEXT("Content/Locked.uasset"),
		TEXT("Content/Theirs.uasset"), TEXT("Content/Elsewhere.uasset"), TEXT("Content/Incoming.uasset"), TEXT("Content/Modified.uasset"),
		TEXT("Content/Deleted.uasset"), TEXT("Content/Conflict.uasset"), TEXT("Content/Old.uasset")};
	Snapshot.Tracked = MakeShared<TSet<FString>, ESPMode::ThreadSafe>(TSet<FString>(Tracked));
	TMap<FString, FFileAttributes> Attributes;
	for (const FString& Path : Tracked)
	{
		Attributes.Add(Path, FFileAttributes{Path.EndsWith(TEXT(".uasset")), Path.EndsWith(TEXT(".uasset"))});
	}
	for (const TCHAR* Path : {TEXT("Content/New.uasset"), TEXT("Content/Marked.uasset"), TEXT("Content/Staged.uasset"), TEXT("Content/Renamed.uasset")})
	{
		Attributes.Add(Path, FFileAttributes{true, true});
	}
	Snapshot.Attributes = MakeShared<TMap<FString, FFileAttributes>, ESPMode::ThreadSafe>(MoveTemp(Attributes));
	Snapshot.bLockableRules = true;

	auto AddLock = [&Snapshot](const TCHAR* Path, const TCHAR* Id, const TCHAR* Owner, bool bOurs)
	{
		FLockInfo Lock;
		Lock.Id = Id;
		Lock.Path = Path;
		Lock.Owner = Owner;
		Lock.bOurs = bOurs;
		Snapshot.Locks.ByPath.Add(Path, Lock);
	};
	Snapshot.Locks.bVerified = true;
	Snapshot.Locks.VerifiedAtUtc = FDateTime::UtcNow();
	AddLock(TEXT("Content/Locked.uasset"), TEXT("1"), TEXT("Me"), true);
	AddLock(TEXT("Content/Theirs.uasset"), TEXT("2"), TEXT("Bob"), false);
	AddLock(TEXT("Content/Elsewhere.uasset"), TEXT("3"), TEXT("Me"), true);
	AddLock(TEXT("Content/Deleted.uasset"), TEXT("4"), TEXT("Me"), true);
	Snapshot.LocksTakenHere = {TEXT("1"), TEXT("4")};
	Snapshot.Remote.Upstream = TEXT("origin/main");
	Snapshot.Remote.IncomingPaths = {TEXT("Content/Incoming.uasset")};
	Snapshot.MarkedForAdd = {TEXT("Content/Marked.uasset")};

	const TSet<FString> RenameSources = GitProvider::CollectRenameSources(Snapshot.Status);
	GitProvider::FStateContext Context;
	Context.Snapshot = &Snapshot;
	Context.RenameSources = &RenameSources;
	Context.bLockingEnabled = true;
	Context.Now = FDateTime::Now();
	auto Build = [](const GitProvider::FStateContext& InContext, const FString& Path, bool bExists = true)
	{
		const TSharedRef<FGitSourceControlState, ESPMode::ThreadSafe> State = MakeShared<FGitSourceControlState, ESPMode::ThreadSafe>(TEXT("/Project/") + Path);
		GitProvider::BuildState(InContext, Path, bExists, *State);
		return State;
	};

	struct FExpected
	{
		const TCHAR* Path;
		bool bControlled, bCheckedOut, bCheckedOutOther, bCanCheckout, bCanCheckIn, bModified, bCurrent, bAdded, bDeleted, bCanRevert, bCanAdd;
		const TCHAR* Icon;
	};
	// Path                          Ctrl  CO     COO    CanCO  CanCI  Mod    Cur    Add    Del    Rev    CanAdd Icon
	const FExpected Table[] = {
		{TEXT("readme.txt"),               true, true,  false, false, false, false, true,  false, false, false, false, nullptr},
		{TEXT("Source/Code.cpp"),          true, true,  false, false, true,  true,  true,  false, false, true,  false, TEXT("RevisionControl.ModifiedLocally")},
		{TEXT("Content/Clean.uasset"),     true, false, false, true,  false, false, true,  false, false, false, false, nullptr},
		{TEXT("Content/Locked.uasset"),    true, true,  false, false, true,  false, true,  false, false, true,  false, TEXT("RevisionControl.CheckedOut")},
		{TEXT("Content/Theirs.uasset"),    true, false, true,  false, false, false, true,  false, false, false, false, TEXT("RevisionControl.CheckedOutByOtherUser")},
		{TEXT("Content/Elsewhere.uasset"), true, false, false, true,  false, false, true,  false, false, false, false, TEXT("RevisionControl.Locked")},
		{TEXT("Content/Incoming.uasset"),  true, false, false, false, false, false, false, false, false, false, false, TEXT("RevisionControl.NotAtHeadRevision")},
		{TEXT("Content/Modified.uasset"),  true, false, false, true,  false, true,  true,  false, false, true,  false, TEXT("RevisionControl.ModifiedLocally")},
		{TEXT("Content/Deleted.uasset"),   true, true,  false, false, true,  true,  true,  false, true,  true,  false, TEXT("RevisionControl.MarkedForDelete")},
		{TEXT("Content/New.uasset"),       false, false, false, false, false, false, true, false, false, false, true,  TEXT("RevisionControl.NotInDepot")},
		{TEXT("Content/Marked.uasset"),    true, false, false, false, true,  true,  true,  true,  false, true,  false, TEXT("RevisionControl.OpenForAdd")},
		{TEXT("Content/Staged.uasset"),    true, false, false, false, true,  true,  true,  true,  false, true,  false, TEXT("RevisionControl.OpenForAdd")},
		{TEXT("Content/Conflict.uasset"),  true, false, false, false, false, true,  true,  false, false, true,  false, TEXT("RevisionControl.Conflicted")},
		{TEXT("Content/Renamed.uasset"),   true, false, false, false, true,  true,  true,  true,  false, true,  false, TEXT("RevisionControl.OpenForAdd")},
		{TEXT("Content/Old.uasset"),       true, false, false, true,  false, true,  true,  false, true,  true,  false, TEXT("RevisionControl.MarkedForDelete")},
	};
	for (const FExpected& Row : Table)
	{
		const TSharedRef<FGitSourceControlState, ESPMode::ThreadSafe> State = Build(Context, Row.Path);
		const FString P = Row.Path;
		TestEqual(P + TEXT(": IsSourceControlled"), State->IsSourceControlled(), Row.bControlled);
		TestEqual(P + TEXT(": IsCheckedOut"), State->IsCheckedOut(), Row.bCheckedOut);
		TestEqual(P + TEXT(": IsCheckedOutOther"), State->IsCheckedOutOther(), Row.bCheckedOutOther);
		TestEqual(P + TEXT(": CanCheckout"), State->CanCheckout(), Row.bCanCheckout);
		TestEqual(P + TEXT(": CanCheckIn"), State->CanCheckIn(), Row.bCanCheckIn);
		TestEqual(P + TEXT(": IsModified"), State->IsModified(), Row.bModified);
		TestEqual(P + TEXT(": IsCurrent"), State->IsCurrent(), Row.bCurrent);
		TestEqual(P + TEXT(": IsAdded"), State->IsAdded(), Row.bAdded);
		TestEqual(P + TEXT(": IsDeleted"), State->IsDeleted(), Row.bDeleted);
		TestEqual(P + TEXT(": CanRevert"), State->CanRevert(), Row.bCanRevert);
		TestEqual(P + TEXT(": CanAdd"), State->CanAdd(), Row.bCanAdd);
		TestEqual(P + TEXT(": icon"), State->GetIcon().GetStyleName().ToString(), Row.Icon ? FString(Row.Icon) : FName(NAME_None).ToString());
		TestTrue(P + TEXT(": resolved states are fresh"), State->GetTimeStamp() == Context.Now);
	}

	FString Who;
	TestTrue(TEXT("Another person's lock names them"), Build(Context, TEXT("Content/Theirs.uasset"))->IsCheckedOutOther(&Who) && Who == TEXT("Bob"));
	TestTrue(TEXT("The lock owner is shown in the Content Browser"), Build(Context, TEXT("Content/Theirs.uasset"))->GetStatusText().Get(FText()).ToString().Contains(TEXT("Bob")));
	TestTrue(TEXT("Editing without a lock is called out"), Build(Context, TEXT("Content/Modified.uasset"))->GetWarningText().IsSet());
	TestFalse(TEXT("Clean files say nothing"), Build(Context, TEXT("readme.txt"))->GetStatusText().IsSet());

	const TSharedRef<FGitSourceControlState, ESPMode::ThreadSafe> Ignored = Build(Context, TEXT("Saved/Log.txt"), true);
	TestTrue(TEXT("An existing file git does not report is ignored"), Ignored->IsIgnored() && !Ignored->CanAdd() && !Ignored->IsSourceControlled());
	TestTrue(TEXT("A file not written yet can be added"), Build(Context, TEXT("Content/NotYet.uasset"), false)->CanAdd());
	const TSharedRef<FGitSourceControlState, ESPMode::ThreadSafe> Outside = Build(Context, FString());
	TestTrue(TEXT("Files outside the repository are editable and not controlled"), Outside->CanEdit() && !Outside->IsSourceControlled() && !Outside->IsUnknown());

	FSessionSnapshot Unread;
	GitProvider::FStateContext UnreadContext = Context;
	UnreadContext.Snapshot = &Unread;
	const TSharedRef<FGitSourceControlState, ESPMode::ThreadSafe> Unknown = Build(UnreadContext, TEXT("Content/Clean.uasset"));
	TestTrue(TEXT("Before the first status read, states are unknown"), Unknown->IsUnknown());
	TestTrue(TEXT("Unknown states look old so the editor asks again"), Unknown->GetTimeStamp() == FDateTime::MinValue());

	GitProvider::FStateContext Unlocked = Context;
	Unlocked.bLockingEnabled = false;
	TestTrue(TEXT("With locking off, lockable files are simply editable"), Build(Unlocked, TEXT("Content/Clean.uasset"))->IsCheckedOut());
	TestFalse(TEXT("With locking off, other people's locks are not shown"), Build(Unlocked, TEXT("Content/Theirs.uasset"))->IsCheckedOutOther());

	FSessionSnapshot Unverified = Snapshot;
	Unverified.Locks = FLockSnapshot();
	GitProvider::FStateContext UnverifiedContext = Context;
	UnverifiedContext.Snapshot = &Unverified;
	const TSharedRef<FGitSourceControlState, ESPMode::ThreadSafe> NotChecked = Build(UnverifiedContext, TEXT("Content/Clean.uasset"));
	TestTrue(TEXT("Files can be checked out before locks were read"), NotChecked->CanCheckout() && NotChecked->bLockInfoStale);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitProviderLocalOperationsTest, "GitWorkspace.Provider.LocalOperations", ProviderTestFlags)
bool FGitProviderLocalOperationsTest::RunTest(const FString&)
{
	GitTest::FFixture Fixture;
	const FString Repo = Fixture.InitRepo();
	GitTest::WriteFile(Repo, TEXT(".gitignore"), TEXT("Saved/\n"));
	GitTest::WriteFile(Repo, TEXT("Content/Hero.uasset"), TEXT("hero v1\n"));
	GitTest::WriteFile(Repo, TEXT("Source/Game.cpp"), TEXT("int Main() { return 0; }\n"));
	GitTest::WriteFile(Repo, TEXT("notes.txt"), TEXT("notes\n"));
	GitTest::CommitAll(Repo, TEXT("base"));
	GitTest::FScopedNoBackgroundFetch NoFetch;
	GitTest::FTestProvider Provider(Repo);
	if (!TestTrue(TEXT("The provider connects to the repository"), Provider->IsEnabled() && Provider->IsAvailable()))
	{
		return false;
	}
	const FString Hero = Repo / TEXT("Content/Hero.uasset");
	const FString Game = Repo / TEXT("Source/Game.cpp");
	const FString New = Repo / TEXT("Content/New.uasset");
	// Temporary folders are often reached through a symlink (/var -> /private/var on macOS).
	const GitCore::FGitRepository& Repository = Provider->GetSession()->GetRepository();
	TestEqual(TEXT("Editor paths map into the repository"), Repository.ToRelative(Hero), FString(TEXT("Content/Hero.uasset")));
	TestEqual(TEXT("Repository paths map back to the editor's spelling"), Repository.ToAbsolute(TEXT("Content/Hero.uasset")), Hero);

	FSourceControlStatePtr State = GetState(Provider, Hero, EStateCacheUsage::ForceUpdate);
	if (!TestTrue(TEXT("A state is always returned"), State.IsValid()))
	{
		return false;
	}
	TestTrue(TEXT("A tracked file is controlled and, without lockable rules, editable"), State->IsSourceControlled() && State->IsCheckedOut() && !State->IsModified());
	TestTrue(TEXT("Resolved states are fresh"), State->GetTimeStamp() > FDateTime::MinValue());
	TestFalse(TEXT("Without lockable rules there is no Check Out step"), Provider->UsesCheckout());
	const FSourceControlStatePtr Outside = GetState(Provider, Fixture.Root / TEXT("outside.uasset"));
	TestTrue(TEXT("Files outside the repository are answered without git"), Outside.IsValid() && !Outside->IsSourceControlled() && Outside->CanEdit());

	// Saving a new asset marks it for add without touching the index.
	GitTest::WriteFile(Repo, TEXT("Content/New.uasset"), TEXT("new asset\n"));
	TestEqual(TEXT("MarkForAdd succeeds"), Run(Provider, ISourceControlOperation::Create<FMarkForAdd>(), {New}), ECommandResult::Succeeded);
	TestTrue(TEXT("Nothing is staged by MarkForAdd"), GitTest::Output(GitTest::Git(Repo, {TEXT("diff"), TEXT("--cached"), TEXT("--name-only")})).IsEmpty());
	State = GetState(Provider, New);
	TestTrue(TEXT("The new asset reads as added"), State->IsAdded() && State->IsSourceControlled() && State->CanCheckIn());
	TestTrue(TEXT("The mark survives in the git directory"), GitTest::ReadFile(Repo, TEXT(".git/uegit/marked-for-add.json")).Contains(TEXT("Content/New.uasset")));

	const TSharedRef<FCopy, ESPMode::ThreadSafe> Copy = ISourceControlOperation::Create<FCopy>();
	Copy->SetDestination(Repo / TEXT("Content/HeroCopy.uasset"));
	TestEqual(TEXT("Copy succeeds"), Run(Provider, Copy, {Hero}), ECommandResult::Succeeded);
	TestTrue(TEXT("Copy stages nothing"), GitTest::Output(GitTest::Git(Repo, {TEXT("diff"), TEXT("--cached"), TEXT("--name-only")})).IsEmpty());

	TestEqual(TEXT("Delete succeeds"), Run(Provider, ISourceControlOperation::Create<FDelete>(), {Hero}), ECommandResult::Succeeded);
	TestFalse(TEXT("The file is gone"), GitTest::FileExists(Repo, TEXT("Content/Hero.uasset")));
	TestTrue(TEXT("Delete stages nothing"), GitTest::Output(GitTest::Git(Repo, {TEXT("diff"), TEXT("--cached"), TEXT("--name-only")})).IsEmpty());
	TestTrue(TEXT("The file reads as deleted"), GetState(Provider, Hero)->IsDeleted());
	const TArray<FSourceControlStateRef> PendingDeletes = Provider->GetCachedStateByPredicate([](const FSourceControlStateRef& Candidate) { return Candidate->IsDeleted(); });
	TestTrue(TEXT("Pending deletes are found through the cache"), PendingDeletes.ContainsByPredicate([&Hero](const FSourceControlStateRef& Candidate) { return Candidate->GetFilename() == Hero; }));

	TestEqual(TEXT("Revert succeeds"), Run(Provider, ISourceControlOperation::Create<FRevert>(), {Hero}), ECommandResult::Succeeded);
	TestEqual(TEXT("The deleted file is back"), GitTest::ReadFile(Repo, TEXT("Content/Hero.uasset")), FString(TEXT("hero v1\n")));
	State = GetState(Provider, Hero);
	TestTrue(TEXT("The restored file is unchanged"), !State->IsModified() && !State->IsDeleted());

	GitTest::WriteFile(Repo, TEXT("Source/Game.cpp"), TEXT("changed\n"));
	TestTrue(TEXT("A modified file reads as modified"), GetState(Provider, Game, EStateCacheUsage::ForceUpdate)->IsModified());
	TestEqual(TEXT("Reverting a modified file succeeds"), Run(Provider, ISourceControlOperation::Create<FRevert>(), {Game}), ECommandResult::Succeeded);
	TestEqual(TEXT("The committed content is restored"), GitTest::ReadFile(Repo, TEXT("Source/Game.cpp")), FString(TEXT("int Main() { return 0; }\n")));
	TArray<FString> Backups;
	IFileManager::Get().FindFilesRecursive(Backups, *(Repo / TEXT("Saved/GitSourceControl/Backups")), TEXT("Game.cpp"), true, false);
	TestTrue(TEXT("The reverted content was backed up"), Backups.Num() == 1 && GitTest::ReadFile(FString(), Backups[0]) == TEXT("changed\n"));

	const TSharedRef<FRevert, ESPMode::ThreadSafe> SoftRevert = ISourceControlOperation::Create<FRevert>();
	SoftRevert->SetSoftRevert(true);
	TestEqual(TEXT("A soft revert succeeds"), Run(Provider, SoftRevert, {New}), ECommandResult::Succeeded);
	State = GetState(Provider, New);
	TestTrue(TEXT("A soft revert only forgets the add"), GitTest::FileExists(Repo, TEXT("Content/New.uasset")) && !State->IsAdded() && State->CanAdd());

	// Submit commits exactly the listed files; anything else that was staged stays staged.
	GitTest::WriteFile(Repo, TEXT("Source/Game.cpp"), TEXT("int Main() { return 1; }\n"));
	GitTest::WriteFile(Repo, TEXT("notes.txt"), TEXT("staged elsewhere\n"));
	GitTest::Git(Repo, {TEXT("add"), TEXT("notes.txt")});
	Run(Provider, ISourceControlOperation::Create<FMarkForAdd>(), {New});
	TestTrue(TEXT("There is something to check in"), Provider->HasChangesToCheckIn().Get(false));
	const TSharedRef<FCheckIn, ESPMode::ThreadSafe> CheckIn = ISourceControlOperation::Create<FCheckIn>();
	CheckIn->SetDescription(FText::FromString(TEXT("Submit test")));
	TestEqual(TEXT("Submit succeeds: ") + Messages(CheckIn), Run(Provider, CheckIn, {Game, New}), ECommandResult::Succeeded);
	TestEqual(TEXT("Only the listed files were committed"), Lines(GitTest::Git(Repo, {TEXT("show"), TEXT("--name-only"), TEXT("--format="), TEXT("HEAD")})),
		TArray<FString>({TEXT("Content/New.uasset"), TEXT("Source/Game.cpp")}));
	TestEqual(TEXT("The description is the commit message"), GitTest::Output(GitTest::Git(Repo, {TEXT("log"), TEXT("-1"), TEXT("--format=%s")})), FString(TEXT("Submit test")));
	TestEqual(TEXT("Other staged changes are untouched"), GitTest::Output(GitTest::Git(Repo, {TEXT("diff"), TEXT("--cached"), TEXT("--name-only")})), FString(TEXT("notes.txt")));
	TestTrue(TEXT("The result names the commit"), CheckIn->GetSuccessMessage().ToString().StartsWith(TEXT("Committed 2 files as ")));
	TestFalse(TEXT("Without a remote nothing is pushed"), CheckIn->GetSuccessMessage().ToString().Contains(TEXT("pushed")));
	TestFalse(TEXT("Committed files are no longer marked"), Provider.Snapshot()->MarkedForAdd.Contains(TEXT("Content/New.uasset")));
	TestTrue(TEXT("The committed file is unchanged"), !GetState(Provider, New)->IsModified());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitProviderCheckOutAndSubmitTest, "GitWorkspace.Provider.CheckOutAndSubmit", ProviderTestFlags)
bool FGitProviderCheckOutAndSubmitTest::RunTest(const FString&)
{
	GitTest::FLockedTeam Team(*this);
	if (!Team.IsReady())
	{
		return false;
	}
	// Refusals are reported to the Source Control message log as errors.
	AddExpectedErrorPlain(TEXT("is locked by"), EAutomationExpectedErrorFlags::Contains, 1);
	AddExpectedErrorPlain(TEXT("Committed locally"), EAutomationExpectedErrorFlags::Contains, 1);
	AddExpectedErrorPlain(TEXT("[rejected]"), EAutomationExpectedErrorFlags::Contains, 1);
	GitTest::FScopedNoBackgroundFetch NoFetch;
	GitTest::FTestProvider Alice(Team.Alice);
	GitTest::FTestProvider Bob(Team.Bob);
	const bool bReady = GitTest::WaitFor([&Alice, &Bob]()
	{
		return Alice.Snapshot()->Locks.bVerified && Bob.Snapshot()->Locks.bVerified;
	});
	if (!TestTrue(TEXT("Both providers read the locks after connecting"), bReady))
	{
		return false;
	}
	const FString Hero = Team.Alice / TEXT("Content/Hero.uasset");
	const FString Map = Team.Alice / TEXT("Content/Map.umap");
	FSourceControlStatePtr State = GetState(Alice, Hero, EStateCacheUsage::ForceUpdate);
	TestTrue(TEXT("Lockable rules turn on Check Out"), Alice->UsesCheckout());
	TestTrue(TEXT("An unlocked asset can be checked out"), State->CanCheckout() && !State->IsCheckedOut());

	const FSourceControlOperationRef AliceCheckOut = ISourceControlOperation::Create<FCheckOut>();
	TestEqual(TEXT("Alice checks out: ") + Messages(AliceCheckOut), Run(Alice, AliceCheckOut, {Hero}), ECommandResult::Succeeded);
	TestTrue(TEXT("Checked out means locked by Alice"), GetState(Alice, Hero)->IsCheckedOut());
	TestFalse(TEXT("The checked-out file is writable"), IFileManager::Get().IsReadOnly(*Hero));

	Bob->GetSession()->RequestLockRefresh(true);
	const FString BobHero = Team.Bob / TEXT("Content/Hero.uasset");
	GitTest::WaitFor([&Bob]() { return Bob.Snapshot()->Locks.Find(TEXT("Content/Hero.uasset")) != nullptr; });
	FString Who;
	State = GetState(Bob, BobHero);
	TestTrue(TEXT("Bob sees who holds the lock"), State->IsCheckedOutOther(&Who) && !Who.IsEmpty());
	TestFalse(TEXT("Bob cannot check it out"), State->CanCheckout());
	const FSourceControlOperationRef BobCheckOut = ISourceControlOperation::Create<FCheckOut>();
	TestEqual(TEXT("Bob's check out is refused"), Run(Bob, BobCheckOut, {BobHero}), ECommandResult::Failed);
	TestTrue(TEXT("The refusal says who holds it: ") + Messages(BobCheckOut), Messages(BobCheckOut).Contains(TEXT("locked by")));

	// What the editor does around a save: status of the saved files and an add for new ones. No lock traffic.
	const int32 RequestsBeforeSave = Team.Server->CountRequests(TEXT("/locks"));
	GitTest::WriteFile(Team.Alice, TEXT("Content/Hero.uasset"), TEXT("hero v2\n"));
	GitTest::WriteFile(Team.Alice, TEXT("Content/Prop.uasset"), TEXT("prop v1\n"));
	const FString Prop = Team.Alice / TEXT("Content/Prop.uasset");
	Run(Alice, ISourceControlOperation::Create<FUpdateStatus>(), {Hero, Prop});
	Run(Alice, ISourceControlOperation::Create<FMarkForAdd>(), {Prop});
	GetState(Alice, Hero, EStateCacheUsage::ForceUpdate);
	TestEqual(TEXT("Saving makes no lock requests"), Team.Server->CountRequests(TEXT("/locks")), RequestsBeforeSave);

	const TSharedRef<FCheckIn, ESPMode::ThreadSafe> CheckIn = ISourceControlOperation::Create<FCheckIn>();
	CheckIn->SetDescription(FText::FromString(TEXT("Hero v2")));
	TestEqual(TEXT("Alice submits: ") + Messages(CheckIn), Run(Alice, CheckIn, {Hero, Prop}), ECommandResult::Succeeded);
	TestEqual(TEXT("The commit reached the server"), Head(Team.Origin, TEXT("main")), Head(Team.Alice));
	TestTrue(TEXT("The result says it was pushed: ") + CheckIn->GetSuccessMessage().ToString(), CheckIn->GetSuccessMessage().ToString().Contains(TEXT("pushed to origin/main")));
	TestTrue(TEXT("The result counts the released lock"), CheckIn->GetSuccessMessage().ToString().Contains(TEXT("Released 1 lock")));
	GitCore::FLockSnapshot ServerLocks;
	Bob->GetSession()->GetRepository().VerifyLocks(TEXT("origin"), GitCore::ECallKind::UserNetwork, ServerLocks);
	TestTrue(TEXT("The lock was released on the server"), ServerLocks.Find(TEXT("Content/Hero.uasset")) == nullptr);
	TestFalse(TEXT("Alice no longer has it checked out"), GetState(Alice, Hero)->IsCheckedOut());

	// Keep checked out: the commit is pushed and the lock stays.
	TestEqual(TEXT("Alice checks out the map"), Run(Alice, ISourceControlOperation::Create<FCheckOut>(), {Map}), ECommandResult::Succeeded);
	GitTest::WriteFile(Team.Alice, TEXT("Content/Map.umap"), TEXT("map v2\n"));
	const TSharedRef<FCheckIn, ESPMode::ThreadSafe> KeepLocked = ISourceControlOperation::Create<FCheckIn>();
	KeepLocked->SetDescription(FText::FromString(TEXT("Map v2")));
	KeepLocked->SetKeepCheckedOut(true);
	TestEqual(TEXT("Submit keeping the lock: ") + Messages(KeepLocked), Run(Alice, KeepLocked, {Map}), ECommandResult::Succeeded);
	TestEqual(TEXT("The map commit reached the server"), Head(Team.Origin, TEXT("main")), Head(Team.Alice));
	TestTrue(TEXT("The map is still checked out"), GetState(Alice, Map)->IsCheckedOut());
	Bob->GetSession()->GetRepository().VerifyLocks(TEXT("origin"), GitCore::ECallKind::UserNetwork, ServerLocks);
	TestTrue(TEXT("The server still holds the map lock"), ServerLocks.Find(TEXT("Content/Map.umap")) != nullptr);

	// A teammate pushed first: the commit stays local, the lock stays, and the message says what to do.
	GitTest::Git(Team.Bob, {TEXT("pull"), TEXT("-q"), TEXT("--ff-only")});
	GitTest::WriteFile(Team.Bob, TEXT("readme.txt"), TEXT("bob was here\n"));
	GitTest::CommitAll(Team.Bob, TEXT("Readme from Bob"));
	GitTest::Git(Team.Bob, {TEXT("push"), TEXT("-q"), TEXT("origin"), TEXT("main")});
	GitTest::WriteFile(Team.Alice, TEXT("Content/Map.umap"), TEXT("map v3\n"));
	const TSharedRef<FCheckIn, ESPMode::ThreadSafe> Rejected = ISourceControlOperation::Create<FCheckIn>();
	Rejected->SetDescription(FText::FromString(TEXT("Map v3")));
	TestEqual(TEXT("Submit reports the rejected push"), Run(Alice, Rejected, {Map}), ECommandResult::Failed);
	TestTrue(TEXT("The commit is kept and Sync is suggested: ") + Messages(Rejected), Messages(Rejected).Contains(TEXT("Committed locally")) && Messages(Rejected).Contains(TEXT("Sync")));
	TestEqual(TEXT("Alice's commit is local"), GitTest::Output(GitTest::Git(Team.Alice, {TEXT("log"), TEXT("-1"), TEXT("--format=%s")})), FString(TEXT("Map v3")));
	TestTrue(TEXT("The map stays checked out"), GetState(Alice, Map)->IsCheckedOut());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitProviderRevertAndDeleteLocksTest, "GitWorkspace.Provider.RevertAndDeleteLocks", ProviderTestFlags)
bool FGitProviderRevertAndDeleteLocksTest::RunTest(const FString&)
{
	GitTest::FLockedTeam Team(*this);
	if (!Team.IsReady())
	{
		return false;
	}
	AddExpectedErrorPlain(TEXT("is locked by"), EAutomationExpectedErrorFlags::Contains, 1);
	GitTest::FScopedNoBackgroundFetch NoFetch;
	GitTest::FTestProvider Alice(Team.Alice);
	GitTest::FTestProvider Bob(Team.Bob);
	const FString Hero = Team.Alice / TEXT("Content/Hero.uasset");
	const FString Map = Team.Alice / TEXT("Content/Map.umap");
	GetState(Alice, Hero, EStateCacheUsage::ForceUpdate);
	GetState(Bob, Team.Bob / TEXT("Content/Hero.uasset"), EStateCacheUsage::ForceUpdate);
	auto ServerLocks = [&Bob]()
	{
		GitCore::FLockSnapshot Locks;
		Bob->GetSession()->GetRepository().VerifyLocks(TEXT("origin"), GitCore::ECallKind::UserNetwork, Locks);
		return Locks;
	};

	// Reverting a checked-out, modified asset restores it and gives the lock back.
	TestEqual(TEXT("Alice checks out the hero"), Run(Alice, ISourceControlOperation::Create<FCheckOut>(), {Hero}), ECommandResult::Succeeded);
	GitTest::WriteFile(Team.Alice, TEXT("Content/Hero.uasset"), TEXT("experiment\n"));
	const FSourceControlOperationRef Revert = ISourceControlOperation::Create<FRevert>();
	TestEqual(TEXT("Revert succeeds: ") + Messages(Revert), Run(Alice, Revert, {Hero}), ECommandResult::Succeeded);
	TestEqual(TEXT("The committed version is back"), GitTest::ReadFile(Team.Alice, TEXT("Content/Hero.uasset")), FString(TEXT("hero v1\n")));
	TestTrue(TEXT("The lock was released"), ServerLocks().Find(TEXT("Content/Hero.uasset")) == nullptr);
	TestFalse(TEXT("The hero is no longer checked out"), GetState(Alice, Hero)->IsCheckedOut());

	// The editor's "revert unchanged files" step releases a lock that was never used.
	TestEqual(TEXT("Alice checks out the hero again"), Run(Alice, ISourceControlOperation::Create<FCheckOut>(), {Hero}), ECommandResult::Succeeded);
	TestEqual(TEXT("Reverting an unchanged checked-out file succeeds"), Run(Alice, ISourceControlOperation::Create<FRevert>(), {Hero}), ECommandResult::Succeeded);
	TestTrue(TEXT("Its lock was released"), ServerLocks().Find(TEXT("Content/Hero.uasset")) == nullptr);

	// Deleting a lockable asset locks it first, so nobody else edits it meanwhile.
	TestEqual(TEXT("Alice deletes the map"), Run(Alice, ISourceControlOperation::Create<FDelete>(), {Map}), ECommandResult::Succeeded);
	TestFalse(TEXT("The map file is gone"), GitTest::FileExists(Team.Alice, TEXT("Content/Map.umap")));
	TestTrue(TEXT("The deleted map is locked by Alice"), ServerLocks().Find(TEXT("Content/Map.umap")) != nullptr);
	TestTrue(TEXT("The deletion can be submitted"), GetState(Alice, Map)->CanCheckIn());

	// Someone else's lock stops a delete before the file is touched.
	const FString BobMap = Team.Bob / TEXT("Content/Map.umap");
	Bob->GetSession()->RequestLockRefresh(true);
	GitTest::WaitFor([&Bob]() { return Bob.Snapshot()->Locks.Find(TEXT("Content/Map.umap")) != nullptr; });
	TestEqual(TEXT("Bob cannot delete Alice's locked map"), Run(Bob, ISourceControlOperation::Create<FDelete>(), {BobMap}), ECommandResult::Failed);
	TestTrue(TEXT("Bob's copy of the map is untouched"), GitTest::FileExists(Team.Bob, TEXT("Content/Map.umap")));

	// Reverting the delete brings the file back and releases the lock.
	TestEqual(TEXT("Alice reverts the delete"), Run(Alice, ISourceControlOperation::Create<FRevert>(), {Map}), ECommandResult::Succeeded);
	TestEqual(TEXT("The map is back"), GitTest::ReadFile(Team.Alice, TEXT("Content/Map.umap")), FString(TEXT("map v1\n")));
	TestTrue(TEXT("The map lock was released"), ServerLocks().Find(TEXT("Content/Map.umap")) == nullptr);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitProviderSyncTest, "GitWorkspace.Provider.Sync", ProviderTestFlags)
bool FGitProviderSyncTest::RunTest(const FString&)
{
	GitTest::FFixture Fixture;
	const FString Origin = Fixture.InitBare();
	const FString Alice = Fixture.InitRepo(TEXT("alice"), TEXT("Alice"));
	GitTest::WriteFile(Alice, TEXT(".gitignore"), TEXT("Saved/\n"));
	GitTest::WriteFile(Alice, TEXT("Content/Hero.uasset"), TEXT("hero v1\n"));
	GitTest::WriteFile(Alice, TEXT("Content/Other.uasset"), TEXT("other v1\n"));
	GitTest::WriteFile(Alice, TEXT("Source/Game.cpp"), TEXT("v1\n"));
	GitTest::WriteFile(Alice, TEXT("notes.txt"), TEXT("notes\n"));
	GitTest::CommitAll(Alice, TEXT("base"));
	GitTest::Git(Alice, {TEXT("remote"), TEXT("add"), TEXT("origin"), Origin});
	GitTest::Git(Alice, {TEXT("push"), TEXT("-q"), TEXT("-u"), TEXT("origin"), TEXT("main")});
	const FString Bob = Fixture.Clone(Origin, TEXT("bob"), TEXT("Bob"));
	auto AlicePushes = [&Alice](const FString& Path, const FString& Content, const FString& Message)
	{
		GitTest::WriteFile(Alice, Path, Content);
		GitTest::CommitAll(Alice, Message);
		GitTest::Git(Alice, {TEXT("push"), TEXT("-q"), TEXT("origin"), TEXT("main")});
	};
	AddExpectedErrorPlain(TEXT("you have local changes to files that also changed"), EAutomationExpectedErrorFlags::Contains, 1);
	AddExpectedErrorPlain(TEXT("Other files also changed on the server"), EAutomationExpectedErrorFlags::Contains, 1);
	AddExpectedErrorPlain(TEXT("your unpushed commits conflict"), EAutomationExpectedErrorFlags::Contains, 1);
	GitTest::FScopedNoBackgroundFetch NoFetch;
	GitTest::FTestProvider Provider(Bob);
	const FString Content = Bob / TEXT("Content");
	const FString Hero = Bob / TEXT("Content/Hero.uasset");

	AlicePushes(TEXT("Content/Hero.uasset"), TEXT("hero v2\n"), TEXT("Hero v2"));
	const TSharedRef<FSyncPreview, ESPMode::ThreadSafe> Preview = ISourceControlOperation::Create<FSyncPreview>();
	TestEqual(TEXT("The preview succeeds: ") + Messages(Preview), Run(Provider, Preview, {Content}), ECommandResult::Succeeded);
	TestEqual(TEXT("The preview lists exactly the changed file, as the editor spells it"), Preview->GetAffectedFiles(), TArray<FString>({Hero}));
	TestTrue(TEXT("There is something to sync"), Provider->HasChangesToSync().Get(false));
	TestFalse(TEXT("The incoming file is not current"), GetState(Provider, Hero)->IsCurrent());

	// Fast-forward with an unrelated local edit in place.
	GitTest::WriteFile(Bob, TEXT("notes.txt"), TEXT("bob's notes\n"));
	const FSourceControlOperationRef FastForward = ISourceControlOperation::Create<FSync>();
	TestEqual(TEXT("Sync fast-forwards: ") + Messages(FastForward), Run(Provider, FastForward, {Content}), ECommandResult::Succeeded);
	TestEqual(TEXT("The new version arrived"), GitTest::ReadFile(Bob, TEXT("Content/Hero.uasset")), FString(TEXT("hero v2\n")));
	TestEqual(TEXT("The unrelated local edit is untouched"), GitTest::ReadFile(Bob, TEXT("notes.txt")), FString(TEXT("bob's notes\n")));
	TestFalse(TEXT("Nothing is left to sync"), Provider->HasChangesToSync().Get(true));
	TestTrue(TEXT("The file is current again"), GetState(Provider, Hero)->IsCurrent());

	// Unpushed local commits are rebased onto the new upstream commits.
	GitTest::WriteFile(Bob, TEXT("Content/Other.uasset"), TEXT("other from bob\n"));
	GitTest::Git(Bob, {TEXT("commit"), TEXT("-q"), TEXT("--no-verify"), TEXT("-m"), TEXT("Other from Bob"), TEXT("--"), TEXT("Content/Other.uasset")});
	GitTest::WriteFile(Alice, TEXT("Source/Game.cpp"), TEXT("v2\n"));
	AlicePushes(TEXT("Content/Hero.uasset"), TEXT("hero v3\n"), TEXT("Hero v3 and code"));
	const FSourceControlOperationRef Rebase = ISourceControlOperation::Create<FSync>();
	TestEqual(TEXT("Sync rebases local commits: ") + Messages(Rebase), Run(Provider, Rebase, {Content}), ECommandResult::Succeeded);
	TestEqual(TEXT("The new version arrived"), GitTest::ReadFile(Bob, TEXT("Content/Hero.uasset")), FString(TEXT("hero v3\n")));
	TestEqual(TEXT("Bob's commit sits on top of the upstream"), Head(Bob, TEXT("HEAD~1")), Head(Bob, TEXT("origin/main")));
	TestEqual(TEXT("Bob's commit kept its content"), GitTest::ReadFile(Bob, TEXT("Content/Other.uasset")), FString(TEXT("other from bob\n")));
	TestTrue(TEXT("A code change asks for a rebuild"), Messages(Rebase).Contains(TEXT("Rebuild")));

	// Local edits to an incoming file stop Sync before anything changes.
	AlicePushes(TEXT("Content/Hero.uasset"), TEXT("hero v4\n"), TEXT("Hero v4"));
	GitTest::WriteFile(Bob, TEXT("Content/Hero.uasset"), TEXT("bob's hero\n"));
	const FString HeadBefore = Head(Bob);
	const FSourceControlOperationRef Overlap = ISourceControlOperation::Create<FSync>();
	TestEqual(TEXT("Sync refuses to overwrite local edits"), Run(Provider, Overlap, {Content}), ECommandResult::Failed);
	TestTrue(TEXT("The refusal explains itself: ") + Messages(Overlap), Messages(Overlap).Contains(TEXT("Nothing was changed")));
	TestEqual(TEXT("The local edit is untouched"), GitTest::ReadFile(Bob, TEXT("Content/Hero.uasset")), FString(TEXT("bob's hero\n")));
	TestEqual(TEXT("HEAD did not move"), Head(Bob), HeadBefore);

	// Syncing single files only works when nothing else changed on the server.
	GitTest::Git(Bob, {TEXT("checkout"), TEXT("--"), TEXT("Content/Hero.uasset")});
	AlicePushes(TEXT("Content/Third.uasset"), TEXT("third\n"), TEXT("Third"));
	const FString Third = Bob / TEXT("Content/Third.uasset");
	TestEqual(TEXT("A file sync that would change other files is refused"), Run(Provider, ISourceControlOperation::Create<FSync>(), {Hero}), ECommandResult::Failed);
	const FSourceControlOperationRef Files = ISourceControlOperation::Create<FSync>();
	TestEqual(TEXT("Syncing exactly the incoming files works: ") + Messages(Files), Run(Provider, Files, {Hero, Third}), ECommandResult::Succeeded);
	TestTrue(TEXT("The new file arrived"), GitTest::FileExists(Bob, TEXT("Content/Third.uasset")));

	// Unpushed commits that would conflict stop Sync before anything changes.
	AlicePushes(TEXT("Content/Other.uasset"), TEXT("other from alice\n"), TEXT("Other from Alice"));
	const FString HeadBeforeConflict = Head(Bob);
	const FSourceControlOperationRef Conflict = ISourceControlOperation::Create<FSync>();
	TestEqual(TEXT("Sync refuses a conflicting rebase"), Run(Provider, Conflict, {Content}), ECommandResult::Failed);
	TestTrue(TEXT("The refusal mentions the conflict: ") + Messages(Conflict), Messages(Conflict).Contains(TEXT("conflict")));
	TestEqual(TEXT("HEAD did not move"), Head(Bob), HeadBeforeConflict);
	TestFalse(TEXT("No rebase was left behind"), IFileManager::Get().DirectoryExists(*(Bob / TEXT(".git/rebase-merge"))));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitProviderHistoryTest, "GitWorkspace.Provider.HistoryAndRevisions", ProviderTestFlags)
bool FGitProviderHistoryTest::RunTest(const FString&)
{
	GitTest::FFixture Fixture;
	const FString Repo = Fixture.InitRepo();
	// Objects stay local: the endpoint is never contacted.
	GitTest::EnableLfs(Repo, TEXT("http://127.0.0.1:9/lfs"));
	GitTest::WriteFile(Repo, TEXT("Content/Hero.uasset"), TEXT("hero v1\n"));
	GitTest::CommitAll(Repo, TEXT("Hero v1"));
	GitTest::WriteFile(Repo, TEXT("Content/Hero.uasset"), TEXT("hero v2\n"));
	GitTest::CommitAll(Repo, TEXT("Hero v2"));
	GitTest::FScopedNoBackgroundFetch NoFetch;
	GitTest::FTestProvider Provider(Repo);
	const FString Hero = Repo / TEXT("Content/Hero.uasset");

	const TSharedRef<FUpdateStatus, ESPMode::ThreadSafe> Update = ISourceControlOperation::Create<FUpdateStatus>();
	Update->SetUpdateHistory(true);
	TestEqual(TEXT("History is read"), Run(Provider, Update, {Hero}), ECommandResult::Succeeded);
	const FSourceControlStatePtr State = GetState(Provider, Hero);
	if (!TestEqual(TEXT("Both commits are listed"), State->GetHistorySize(), 2))
	{
		return false;
	}
	const TSharedPtr<ISourceControlRevision, ESPMode::ThreadSafe> Newest = State->GetHistoryItem(0);
	const TSharedPtr<ISourceControlRevision, ESPMode::ThreadSafe> Oldest = State->GetHistoryItem(1);
	TestEqual(TEXT("Newest first"), Newest->GetDescription(), FString(TEXT("Hero v2")));
	TestEqual(TEXT("Revision numbers count up"), Oldest->GetRevisionNumber(), 1);
	TestEqual(TEXT("The author is recorded"), Oldest->GetUserName(), FString(TEXT("Test User")));
	TestTrue(TEXT("Revisions are found by number"), State->FindHistoryRevision(1) == Oldest);
	FString Exported;
	TestTrue(TEXT("An old revision can be exported"), Oldest->Get(Exported));
	TestEqual(TEXT("The export keeps the extension"), FPaths::GetExtension(Exported), FString(TEXT("uasset")));
	TestEqual(TEXT("The export holds the real bytes, not the LFS pointer"), GitTest::ReadFile(FString(), Exported), FString(TEXT("hero v1\n")));
	IFileManager::Get().Delete(*Exported);
	return true;
}

#endif
