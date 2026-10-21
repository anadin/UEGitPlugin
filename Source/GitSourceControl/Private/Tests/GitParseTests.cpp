// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#if WITH_DEV_AUTOMATION_TESTS

#include "Core/GitParse.h"
#include "Process/GitProcess.h"
#include "Misc/AutomationTest.h"

namespace
{
	constexpr EAutomationTestFlags ParseTestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

	TArray<uint8> NulRecords(const TArray<FString>& Records)
	{
		TArray<uint8> Bytes;
		for (const FString& Record : Records)
		{
			GitCore::AppendUtf8(Bytes, Record);
			Bytes.Add(0);
		}
		return Bytes;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitParseStatusTest, "GitWorkspace.Parse.Status", ParseTestFlags)
bool FGitParseStatusTest::RunTest(const FString&)
{
	const TArray<uint8> Bytes = NulRecords({
		TEXT("# branch.oid 0123456789012345678901234567890123456789"), TEXT("# branch.head feature/test"),
		TEXT("# branch.upstream origin/feature/test"), TEXT("# branch.ab +2 -3"),
		TEXT("1 MM N... 100644 100644 100644 aaa bbb Content/my asset.uasset"),
		TEXT("2 R. N... 100644 100644 100644 aaa bbb R100 renamed.txt"), TEXT("original with space.txt"),
		TEXT("? untracked\nfile.txt"),
		TEXT("u UU N... 100644 100644 100644 100644 aaa bbb ccc conflict.txt"),
		TEXT("1 .M S.M. 160000 160000 160000 aaa aaa Plugins/Sub")});
	GitCore::FStatusSnapshot Status;
	FString Error;
	if (!TestTrue(TEXT("Mixed records parse"), GitCore::ParseStatus(Bytes, Status, Error)))
	{
		AddError(Error);
		return false;
	}
	TestEqual(TEXT("Branch"), Status.Branch, FString(TEXT("feature/test")));
	TestEqual(TEXT("Upstream"), Status.Upstream, FString(TEXT("origin/feature/test")));
	TestTrue(TEXT("Ahead and behind"), Status.Ahead == 2 && Status.Behind == 3);
	TestEqual(TEXT("Five entries"), Status.Entries.Num(), 5);
	const GitCore::FStatusEntry* Both = Status.Find(TEXT("Content/my asset.uasset"));
	TestTrue(TEXT("A file can be staged and have later edits"), Both && Both->HasStaged() && Both->HasUnstaged());
	const GitCore::FStatusEntry* Renamed = Status.Find(TEXT("renamed.txt"));
	TestTrue(TEXT("Rename source kept"), Renamed && Renamed->OriginalPath == TEXT("original with space.txt"));
	const GitCore::FStatusEntry* Untracked = Status.Find(TEXT("untracked\nfile.txt"));
	TestTrue(TEXT("Untracked paths may contain newlines"), Untracked && Untracked->bUntracked && !Untracked->HasStaged());
	TestTrue(TEXT("Conflicts are reported"), Status.HasConflicts());
	const GitCore::FStatusEntry* Submodule = Status.Find(TEXT("Plugins/Sub"));
	TestTrue(TEXT("Submodules are flagged"), Submodule && Submodule->bSubmodule);

	GitCore::FStatusSnapshot Unborn;
	TestTrue(TEXT("Unborn branch parses"), GitCore::ParseStatus(NulRecords({TEXT("# branch.oid (initial)"), TEXT("# branch.head main")}), Unborn, Error));
	TestTrue(TEXT("Unborn has no head"), Unborn.bUnborn && Unborn.Head.IsEmpty());
	GitCore::FStatusSnapshot Detached;
	GitCore::ParseStatus(NulRecords({TEXT("# branch.oid 0123456789012345678901234567890123456789"), TEXT("# branch.head (detached)")}), Detached, Error);
	TestTrue(TEXT("Detached HEAD has no branch"), Detached.Branch.IsEmpty());

	TArray<uint8> Truncated = Bytes;
	Truncated.Pop();
	TestFalse(TEXT("Truncated output is rejected"), GitCore::ParseStatus(Truncated, Status, Error));
	TestFalse(TEXT("A rename without its source is rejected"), GitCore::ParseStatus(NulRecords({TEXT("2 R. N... 100644 100644 100644 aaa bbb R100 renamed.txt")}), Status, Error));
	TestFalse(TEXT("Unknown records are rejected"), GitCore::ParseStatus(NulRecords({TEXT("nonsense")}), Status, Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitParseLocksTest, "GitWorkspace.Parse.Locks", ParseTestFlags)
bool FGitParseLocksTest::RunTest(const FString&)
{
	TMap<FString, GitCore::FLockInfo> Locks;
	FString Error;
	const FString Entry = TEXT("{\"id\":\"abc\",\"path\":\"Content/水 asset.uasset\",\"owner\":{\"name\":\"me\"},\"locked_at\":\"2026-10-02T00:00:00Z\"}");
	TestTrue(TEXT("An empty complete response parses"), GitCore::ParseVerifiedLocks(TEXT("{\"ours\":[],\"theirs\":[]}"), Locks, Error));
	TestTrue(TEXT("Ownership comes from the server group"), GitCore::ParseVerifiedLocks(TEXT("{\"ours\":[],\"theirs\":[") + Entry + TEXT("]}"), Locks, Error));
	TestTrue(TEXT("A matching display name does not make a lock ours"), Locks.Num() == 1 && !Locks.CreateConstIterator().Value().bOurs);
	TestFalse(TEXT("Duplicate locks are rejected"), GitCore::ParseVerifiedLocks(TEXT("{\"ours\":[") + Entry + TEXT("],\"theirs\":[") + Entry + TEXT("]}"), Locks, Error));
	TestTrue(TEXT("Rejected responses leave no partial locks"), Locks.IsEmpty());
	for (const TCHAR* Invalid : {TEXT("{}"), TEXT("{\"ours\":null,\"theirs\":[]}"), TEXT("{\"ours\":[],\"theirs\":[],\"next_cursor\":\"next\"}"),
		TEXT("{\"ours\":[],\"theirs\":[],\"message\":\"failed\"}"), TEXT("garbage"),
		TEXT("{\"ours\":[{\"id\":\"x\",\"path\":\"../escape\",\"locked_at\":\"2026-10-02T00:00:00Z\"}],\"theirs\":[]}")})
	{
		TestFalse(FString::Printf(TEXT("Rejects %s"), Invalid), GitCore::ParseVerifiedLocks(Invalid, Locks, Error));
	}

	TArray<GitCore::FLockInfo> Created;
	TestTrue(TEXT("A single lock response parses"), GitCore::ParseLockResponse(Entry, Created, Error) && Created.Num() == 1 && Created[0].bOurs);
	TestTrue(TEXT("An array lock response parses"), GitCore::ParseLockResponse(TEXT("[") + Entry + TEXT("]"), Created, Error) && Created.Num() == 1);
	TestFalse(TEXT("A malformed lock response is rejected"), GitCore::ParseLockResponse(TEXT("{\"id\":\"\"}"), Created, Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitParseHistoryTest, "GitWorkspace.Parse.History", ParseTestFlags)
bool FGitParseHistoryTest::RunTest(const FString&)
{
	const FString Hash = FString::ChrN(40, TEXT('a'));
	const FString Parent = FString::ChrN(40, TEXT('b'));
	TArray<uint8> Bytes = NulRecords({Hash, Parent, TEXT("Artist 水"), TEXT("2026-10-06T10:00:00+10:00"), TEXT("quoted \"message\"")});
	TArray<GitCore::FHistoryCommit> Commits;
	FString Error;
	if (!TestTrue(TEXT("Complete metadata parses"), GitCore::ParseLog(Bytes, Commits, Error)))
	{
		return false;
	}
	TestTrue(TEXT("Fields are literal"), Commits[0].Author == TEXT("Artist 水") && Commits[0].Parents.Num() == 1);
	Bytes.Pop();
	TestFalse(TEXT("Truncated history is rejected"), GitCore::ParseLog(Bytes, Commits, Error));
	TestTrue(TEXT("No partial history leaks"), Commits.IsEmpty());
	TestFalse(TEXT("Invalid commit ids are rejected"), GitCore::ParseLog(NulRecords({TEXT("HEAD"), TEXT(""), TEXT("A"), TEXT("2026-10-06T00:00:00Z"), TEXT("s")}), Commits, Error));
	TestFalse(TEXT("Invalid dates are rejected"), GitCore::ParseLog(NulRecords({Hash, TEXT(""), TEXT("A"), TEXT("unknown"), TEXT("s")}), Commits, Error));

	// File history: 0x01 starts each commit; renames carry two paths.
	TArray<uint8> FileLog;
	auto AddCommit = [&FileLog](const FString& Oid, const FString& Subject, const TArray<FString>& Changes)
	{
		FileLog.Add(0x01);
		for (const FString& Field : {Oid, FString(TEXT("Artist")), FString(TEXT("2026-10-21T13:33:28+10:00")), Subject})
		{
			GitCore::AppendUtf8(FileLog, Field);
			FileLog.Add(0);
		}
		FileLog.Add('\n');
		for (const FString& Change : Changes)
		{
			GitCore::AppendUtf8(FileLog, Change);
			FileLog.Add(0);
		}
	};
	AddCommit(Hash, TEXT("rename it"), {TEXT("R100"), TEXT("a.txt"), TEXT("b b.txt")});
	AddCommit(Parent, TEXT("first"), {TEXT("A"), TEXT("a.txt")});
	TArray<GitCore::FFileRevision> Revisions;
	if (TestTrue(TEXT("File history parses"), GitCore::ParseFileLog(FileLog, Revisions, Error)) && TestEqual(TEXT("Two revisions"), Revisions.Num(), 2))
	{
		TestTrue(TEXT("Renames use the new path"), Revisions[0].Action == TEXT('R') && Revisions[0].Path == TEXT("b b.txt"));
		TestTrue(TEXT("Older revisions keep their own path"), Revisions[1].Action == TEXT('A') && Revisions[1].Path == TEXT("a.txt"));
	}

	TArray<uint8> Raw = NulRecords({TEXT(":000000 100644 ") + FString::ChrN(40, TEXT('0')) + TEXT(" ") + Hash + TEXT(" A"), TEXT("dir/new file.txt"),
		TEXT(":100644 000000 ") + Hash + TEXT(" ") + FString::ChrN(40, TEXT('0')) + TEXT(" D"), TEXT("old.txt")});
	TArray<GitCore::FHistoryChange> Changes;
	TestTrue(TEXT("Raw diff parses"), GitCore::ParseRawDiff(Raw, Changes, Error) && Changes.Num() == 2 && Changes[0].Status == TEXT('A'));
	TestFalse(TEXT("Unsafe paths are rejected"), GitCore::ParseRawDiff(NulRecords({TEXT(":100644 100644 ") + Hash + TEXT(" ") + Parent + TEXT(" M"), TEXT("../x")}), Changes, Error));
	return true;
}

#endif
