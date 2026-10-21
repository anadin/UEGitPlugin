// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#if WITH_DEV_AUTOMATION_TESTS

#include "GitTestFixtures.h"
#if PLATFORM_MAC || PLATFORM_LINUX
#include <sys/stat.h>
#endif

namespace
{
	constexpr EAutomationTestFlags RepositoryTestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

	void WriteHook(const FString& Repo, const FString& Name, const FString& Script)
	{
		const FString Path = Repo / TEXT(".git/test-hooks") / Name;
		GitTest::WriteFile(Repo, TEXT(".git/test-hooks/") + Name, Script);
#if PLATFORM_MAC || PLATFORM_LINUX
		chmod(TCHAR_TO_UTF8(*Path), 0755);
#endif
	}

	GitCore::FStatusSnapshot Status(FAutomationTestBase& Test, const GitCore::FGitRepository& Repository)
	{
		GitCore::FStatusSnapshot Snapshot;
		FString Error;
		if (!Repository.ReadStatus(Snapshot, Error, true))
		{
			Test.AddError(TEXT("Status failed: ") + Error);
		}
		return Snapshot;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitRepositoryStagedSnapshotTest, "GitWorkspace.Repository.StagedSnapshotCommit", RepositoryTestFlags)
bool FGitRepositoryStagedSnapshotTest::RunTest(const FString&)
{
	GitTest::FFixture Fixture;
	const FString Repo = Fixture.InitRepo();
	GitTest::WriteFile(Repo, TEXT("asset.txt"), TEXT("base\n"));
	GitTest::WriteFile(Repo, TEXT("other.txt"), TEXT("unchanged\n"));
	GitTest::CommitAll(Repo, TEXT("base"));
	const GitCore::FGitRepositoryPtr Repository = GitTest::OpenRepository(*this, Repo);
	if (!Repository.IsValid())
	{
		return false;
	}

	GitTest::WriteFile(Repo, TEXT("asset.txt"), TEXT("version A\n"));
	TestTrue(TEXT("Stage A"), Repository->Stage({TEXT("asset.txt")}).bOk);
	GitTest::WriteFile(Repo, TEXT("asset.txt"), TEXT("version B\n"));
	const GitCore::FStatusSnapshot Reviewed = Status(*this, *Repository);
	TestEqual(TEXT("One staged entry"), Reviewed.StagedCount(), 1);
	const FString StagedBlob = GitTest::Output(GitTest::Git(Repo, {TEXT("show"), TEXT(":asset.txt")}));
	Status(*this, *Repository);
	TestEqual(TEXT("Refreshing never restages later edits"), GitTest::Output(GitTest::Git(Repo, {TEXT("show"), TEXT(":asset.txt")})), StagedBlob);
	FString Diff;
	TestTrue(TEXT("Staged diff shows A"), Repository->Diff(TEXT("asset.txt"), true, Diff).bOk && Diff.Contains(TEXT("+version A")));
	TestTrue(TEXT("Working diff shows B"), Repository->Diff(TEXT("asset.txt"), false, Diff).bOk && Diff.Contains(TEXT("+version B")));

	// Git rewrites the index when it refreshes file timestamps; that is not a change to what is staged.
	IFileManager::Get().SetTimeStamp(*(Repo / TEXT("other.txt")), FDateTime::UtcNow() + FTimespan::FromSeconds(5.0));
	GitTest::Git(Repo, {TEXT("status"), TEXT("--porcelain")});
	TestNotEqual(TEXT("The index file was rewritten"), Repository->ReadIndexToken(), Reviewed.IndexToken);

	FString Commit;
	const GitCore::FOpResult Committed = Repository->CommitIndex(TEXT("Commit the reviewed A"), Reviewed.Head, Reviewed.StagedToken, Commit);
	TestTrue(TEXT("Commit succeeds: ") + Committed.Message, Committed.bOk);
	TestEqual(TEXT("The commit holds A"), GitTest::Output(GitTest::Git(Repo, {TEXT("show"), TEXT("HEAD:asset.txt")})), FString(TEXT("version A")));
	TestEqual(TEXT("B stays on disk"), GitTest::ReadFile(Repo, TEXT("asset.txt")), FString(TEXT("version B\n")));
	TestEqual(TEXT("The reported commit is HEAD"), Commit, GitTest::Output(GitTest::Git(Repo, {TEXT("rev-parse"), TEXT("HEAD")})));

	TestTrue(TEXT("Stage B"), Repository->Stage({TEXT("asset.txt")}).bOk);
	const GitCore::FStatusSnapshot Stale = Status(*this, *Repository);
	GitTest::WriteFile(Repo, TEXT("asset.txt"), TEXT("version C\n"));
	GitTest::Git(Repo, {TEXT("add"), TEXT("asset.txt")});
	TestFalse(TEXT("A changed index is not committed"), Repository->CommitIndex(TEXT("must not commit C"), Stale.Head, Stale.StagedToken, Commit).bOk);
	TestTrue(TEXT("Unstage"), Repository->Unstage({TEXT("asset.txt")}).bOk);
	TestEqual(TEXT("Unstage keeps the working file"), GitTest::ReadFile(Repo, TEXT("asset.txt")), FString(TEXT("version C\n")));
	const GitCore::FStatusSnapshot Clean = Status(*this, *Repository);
	TestFalse(TEXT("An empty index is not committed"), Repository->CommitIndex(TEXT("nothing"), Clean.Head, Clean.StagedToken, Commit).bOk);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitRepositoryPathsTest, "GitWorkspace.Repository.UnbornAndLiteralPaths", RepositoryTestFlags)
bool FGitRepositoryPathsTest::RunTest(const FString&)
{
	GitTest::FFixture Fixture;
	const FString Repo = Fixture.InitRepo();
	const GitCore::FGitRepositoryPtr Repository = GitTest::OpenRepository(*this, Repo);
	if (!Repository.IsValid())
	{
		return false;
	}
	TArray<FString> Names = {TEXT("space name.txt"), TEXT("unicode-水.txt"), TEXT("[abc].txt"), TEXT("a.txt"), TEXT("-leading.txt"), TEXT("two  spaces.txt")};
#if !PLATFORM_WINDOWS
	Names.Append({TEXT("a\"b.txt"), TEXT("line\nbreak.txt")});
#endif
	for (const FString& Name : Names)
	{
		GitTest::WriteFile(Repo, Name, TEXT("original\n"));
	}
	TestTrue(TEXT("Brackets are literal when staging"), Repository->Stage({TEXT("[abc].txt")}).bOk);
	TestEqual(TEXT("The wildcard match a.txt is not staged"), Status(*this, *Repository).StagedCount(), 1);
	GitTest::WriteFile(Repo, TEXT("[abc].txt"), TEXT("later edits\n"));
	TestTrue(TEXT("Unstage works before the first commit"), Repository->Unstage({TEXT("[abc].txt")}).bOk);
	TestEqual(TEXT("The index is empty again"), Status(*this, *Repository).StagedCount(), 0);
	TestEqual(TEXT("Working edits survive"), GitTest::ReadFile(Repo, TEXT("[abc].txt")), FString(TEXT("later edits\n")));

	for (int32 Index = 0; Index < 300; ++Index)
	{
		const FString Path = FString::Printf(TEXT("many/file-%d.txt"), Index);
		Names.Add(Path);
		GitTest::WriteFile(Repo, Path, TEXT("many\n"));
	}
	const GitCore::FOpResult Staged = Repository->Stage(Names);
	TestTrue(TEXT("Hundreds of paths stage through stdin: ") + Staged.Message, Staged.bOk);
	const GitCore::FStatusSnapshot Snapshot = Status(*this, *Repository);
	TestEqual(TEXT("Every path is staged"), Snapshot.StagedCount(), Names.Num());
	TestTrue(TEXT("An unborn branch is reported"), Snapshot.bUnborn && Snapshot.Head.IsEmpty());
	FString Commit;
	TestTrue(TEXT("The first commit works"), Repository->CommitIndex(TEXT("Initial"), Snapshot.Head, Snapshot.StagedToken, Commit).bOk);
	TestTrue(TEXT("The repository is clean"), Status(*this, *Repository).Entries.IsEmpty());

	GitTest::Git(Repo, {TEXT("mv"), TEXT("space name.txt"), TEXT("renamed name.txt")});
	const GitCore::FStatusSnapshot Renamed = Status(*this, *Repository);
	const GitCore::FStatusEntry* Entry = Renamed.Find(TEXT("renamed name.txt"));
	TestTrue(TEXT("Renames keep their source"), Entry && Entry->OriginalPath == TEXT("space name.txt"));
	TestTrue(TEXT("Unstaging both sides undoes the rename in the index"), Repository->Unstage({TEXT("renamed name.txt"), TEXT("space name.txt")}).bOk);
	TestEqual(TEXT("No staged change remains"), Status(*this, *Repository).StagedCount(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitRepositoryHooksTest, "GitWorkspace.Repository.HooksAttributesAndMerges", RepositoryTestFlags)
bool FGitRepositoryHooksTest::RunTest(const FString&)
{
	GitTest::FFixture Fixture;
	const FString Repo = Fixture.InitRepo();
	GitTest::WriteFile(Repo, TEXT("asset.txt"), TEXT("base\n"));
	GitTest::WriteFile(Repo, TEXT(".gitattributes"), TEXT("*.bin lockable\nContent/** -lockable\n"));
	GitTest::CommitAll(Repo, TEXT("base"));
	const GitCore::FGitRepositoryPtr Repository = GitTest::OpenRepository(*this, Repo);
	if (!Repository.IsValid())
	{
		return false;
	}
	TMap<FString, GitCore::FFileAttributes> Attributes;
	FString Error;
	TestTrue(TEXT("Attributes resolve"), Repository->ReadAttributes({TEXT("locked.bin"), TEXT("Content/free.bin"), TEXT("asset.txt")}, Attributes, Error));
	TestTrue(TEXT("Effective lockable attribute"), Attributes.FindRef(TEXT("locked.bin")).bLockable);
	TestFalse(TEXT("Overrides in later rules apply"), Attributes.FindRef(TEXT("Content/free.bin")).bLockable);
	TestFalse(TEXT("Text is not lockable"), Attributes.FindRef(TEXT("asset.txt")).bLockable);

	GitTest::WriteFile(Repo, TEXT("asset.txt"), TEXT("hook candidate\n"));
	Repository->Stage({TEXT("asset.txt")});
	WriteHook(Repo, TEXT("pre-commit"), TEXT("#!/bin/sh\necho fixture-hook-rejected >&2\nexit 1\n"));
	const FString Before = GitTest::Output(GitTest::Git(Repo, {TEXT("rev-parse"), TEXT("HEAD")}));
	const GitCore::FStatusSnapshot Snapshot = Status(*this, *Repository);
	FString Commit;
	const GitCore::FOpResult Failed = Repository->CommitIndex(TEXT("rejected"), Snapshot.Head, Snapshot.StagedToken, Commit);
	TestFalse(TEXT("A failing hook is not success"), Failed.bOk);
	TestTrue(TEXT("The hook's message reaches the user"), Failed.Message.Contains(TEXT("fixture-hook-rejected")) || Failed.Details.Contains(TEXT("fixture-hook-rejected")));
	TestEqual(TEXT("HEAD is unchanged"), GitTest::Output(GitTest::Git(Repo, {TEXT("rev-parse"), TEXT("HEAD")})), Before);
	TestEqual(TEXT("Staging is kept"), Status(*this, *Repository).StagedCount(), 1);

	GitTest::WriteFile(Repo, TEXT(".git/MERGE_HEAD"), Before);
	const GitCore::FStatusSnapshot Merging = Status(*this, *Repository);
	TestTrue(TEXT("A merge in progress is detected"), Merging.bOperationInProgress);
	TestFalse(TEXT("Commit refuses during a merge"), Repository->CommitIndex(TEXT("unsafe"), Merging.Head, Merging.StagedToken, Commit).bOk);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitRepositoryCommitPathsTest, "GitWorkspace.Repository.CommitPathsAndRestore", RepositoryTestFlags)
bool FGitRepositoryCommitPathsTest::RunTest(const FString&)
{
	GitTest::FFixture Fixture;
	const FString Repo = Fixture.InitRepo();
	GitTest::WriteFile(Repo, TEXT("keep-staged.txt"), TEXT("base\n"));
	GitTest::WriteFile(Repo, TEXT("submit.txt"), TEXT("base\n"));
	GitTest::WriteFile(Repo, TEXT("delete me.txt"), TEXT("base\n"));
	GitTest::CommitAll(Repo, TEXT("base"));
	const GitCore::FGitRepositoryPtr Repository = GitTest::OpenRepository(*this, Repo);
	if (!Repository.IsValid())
	{
		return false;
	}

	GitTest::WriteFile(Repo, TEXT("keep-staged.txt"), TEXT("staged elsewhere\n"));
	Repository->Stage({TEXT("keep-staged.txt")});
	GitTest::WriteFile(Repo, TEXT("submit.txt"), TEXT("submitted\n"));
	GitTest::WriteFile(Repo, TEXT("new [file].txt"), TEXT("new\n"));
	IFileManager::Get().Delete(*(Repo / TEXT("delete me.txt")));

	FString Commit;
	const GitCore::FOpResult Committed = Repository->CommitPaths({TEXT("submit.txt"), TEXT("new [file].txt"), TEXT("delete me.txt")}, {TEXT("new [file].txt")}, TEXT("Submit three files"), Commit);
	TestTrue(TEXT("Listed paths commit: ") + Committed.Message, Committed.bOk);
	const FString Files = GitTest::Output(GitTest::Git(Repo, {TEXT("show"), TEXT("--name-status"), TEXT("--format="), TEXT("HEAD")}));
	TestTrue(TEXT("The modified file is in the commit"), Files.Contains(TEXT("M\tsubmit.txt")));
	TestTrue(TEXT("The new file is added"), Files.Contains(TEXT("A\tnew [file].txt")));
	TestTrue(TEXT("The deletion is recorded"), Files.Contains(TEXT("D\tdelete me.txt")));
	TestFalse(TEXT("Unlisted staged work is not committed"), Files.Contains(TEXT("keep-staged.txt")));
	const GitCore::FStatusSnapshot After = Status(*this, *Repository);
	const GitCore::FStatusEntry* Kept = After.Find(TEXT("keep-staged.txt"));
	TestTrue(TEXT("Unlisted staged work stays staged"), Kept && Kept->HasStaged());

	// A failing hook leaves new files untracked again.
	WriteHook(Repo, TEXT("pre-commit"), TEXT("#!/bin/sh\nexit 1\n"));
	GitTest::WriteFile(Repo, TEXT("another.txt"), TEXT("new\n"));
	TestFalse(TEXT("A hook can refuse the submit"), Repository->CommitPaths({TEXT("another.txt")}, {TEXT("another.txt")}, TEXT("refused"), Commit).bOk);
	const GitCore::FStatusEntry* Another = Status(*this, *Repository).Find(TEXT("another.txt"));
	TestTrue(TEXT("The new file is untracked again"), Another && Another->bUntracked);
	IFileManager::Get().Delete(*(Repo / TEXT(".git/test-hooks/pre-commit")));

	// Restore: tracked edits go back to HEAD (index too); new files disappear.
	GitTest::WriteFile(Repo, TEXT("submit.txt"), TEXT("edited again\n"));
	Repository->Stage({TEXT("submit.txt")});
	GitTest::WriteFile(Repo, TEXT("submit.txt"), TEXT("and again\n"));
	GitTest::WriteFile(Repo, TEXT("staged-new.txt"), TEXT("new\n"));
	Repository->Stage({TEXT("staged-new.txt")});
	const GitCore::FOpResult Restored = Repository->RestorePaths({TEXT("submit.txt"), TEXT("another.txt"), TEXT("staged-new.txt")});
	TestTrue(TEXT("Restore succeeds: ") + Restored.Message, Restored.bOk);
	TestEqual(TEXT("The tracked file matches HEAD"), GitTest::ReadFile(Repo, TEXT("submit.txt")), FString(TEXT("submitted\n")));
	TestFalse(TEXT("The untracked file is removed"), GitTest::FileExists(Repo, TEXT("another.txt")));
	TestFalse(TEXT("The staged new file is removed"), GitTest::FileExists(Repo, TEXT("staged-new.txt")));
	const GitCore::FStatusSnapshot Final = Status(*this, *Repository);
	TestTrue(TEXT("Only the other staged file remains changed"), Final.Entries.Num() == 1 && Final.Find(TEXT("keep-staged.txt")) != nullptr);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitRepositoryLfsTest, "GitWorkspace.Repository.LfsPointersAndExport", RepositoryTestFlags)
bool FGitRepositoryLfsTest::RunTest(const FString&)
{
	GitTest::FFixture Fixture;
	const FString Repo = Fixture.InitRepo();
	if (!TestTrue(TEXT("Git LFS is installed"), GitTest::Git(Repo, {TEXT("lfs"), TEXT("install"), TEXT("--local")}).Succeeded()))
	{
		return false;
	}
	GitTest::WriteFile(Repo, TEXT(".gitattributes"), TEXT("*.uasset filter=lfs diff=lfs merge=lfs -text lockable\n"));
	GitTest::WriteFile(Repo, TEXT("Content/Probe.uasset"), TEXT("saved package A\n"));
	GitTest::WriteFile(Repo, TEXT("notes.txt"), TEXT("one\n"));
	GitTest::CommitAll(Repo, TEXT("base"));
	const GitCore::FGitRepositoryPtr Repository = GitTest::OpenRepository(*this, Repo);
	if (!Repository.IsValid())
	{
		return false;
	}
	GitTest::WriteFile(Repo, TEXT("Content/Probe.uasset"), TEXT("saved package B\n"));
	TestTrue(TEXT("Stage the asset"), Repository->Stage({TEXT("Content/Probe.uasset")}).bOk);
	TestTrue(TEXT("The index holds an LFS pointer"), GitTest::Output(GitTest::Git(Repo, {TEXT("show"), TEXT(":Content/Probe.uasset")})).StartsWith(TEXT("version https://git-lfs.github.com/spec/v1")));
	GitTest::WriteFile(Repo, TEXT("Content/Probe.uasset"), TEXT("saved package C\n"));
	TMap<FString, GitCore::FFileAttributes> Attributes;
	FString Error;
	Repository->ReadAttributes({TEXT("Content/Probe.uasset")}, Attributes, Error);
	TestTrue(TEXT("The asset is LFS and lockable"), Attributes.FindRef(TEXT("Content/Probe.uasset")).bLfs && Attributes.FindRef(TEXT("Content/Probe.uasset")).bLockable);
	const GitCore::FStatusSnapshot Snapshot = Status(*this, *Repository);
	FString Commit;
	TestTrue(TEXT("Commit the staged pointer"), Repository->CommitIndex(TEXT("B"), Snapshot.Head, Snapshot.StagedToken, Commit).bOk);
	TestEqual(TEXT("Later bytes stay on disk"), GitTest::ReadFile(Repo, TEXT("Content/Probe.uasset")), FString(TEXT("saved package C\n")));

	const FString Exported = Fixture.Root / TEXT("export/Probe.uasset");
	TestTrue(TEXT("Exporting an LFS revision works"), Repository->ExportRevision(Commit, TEXT("Content/Probe.uasset"), Exported).bOk);
	FString ExportedText;
	FFileHelper::LoadFileToString(ExportedText, *Exported);
	TestEqual(TEXT("Exported bytes are the real asset, not the pointer"), ExportedText, FString(TEXT("saved package B\n")));
	TestFalse(TEXT("No partial file remains"), IFileManager::Get().FileExists(*(Exported + TEXT(".part"))));
	TestFalse(TEXT("Exporting a missing path fails cleanly"), Repository->ExportRevision(Commit, TEXT("Content/Missing.uasset"), Exported + TEXT("2")).bOk);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitRepositoryHistoryTest, "GitWorkspace.Repository.History", RepositoryTestFlags)
bool FGitRepositoryHistoryTest::RunTest(const FString&)
{
	GitTest::FFixture Fixture;
	const FString Repo = Fixture.InitRepo();
	const GitCore::FGitRepositoryPtr Repository = GitTest::OpenRepository(*this, Repo);
	if (!Repository.IsValid())
	{
		return false;
	}
	TArray<GitCore::FHistoryCommit> Commits;
	bool bHasMore = true;
	FString Error;
	TestTrue(TEXT("An unborn branch has empty history"), Repository->ReadHistory(10, Commits, bHasMore, Error) && Commits.IsEmpty() && !bHasMore);

	GitTest::WriteFile(Repo, TEXT("a.txt"), TEXT("one\n"));
	const FString First = GitTest::CommitAll(Repo, TEXT("first"));
	GitTest::WriteFile(Repo, TEXT("a.txt"), TEXT("two\n"));
	const FString Second = GitTest::CommitAll(Repo, TEXT("second"));
	GitTest::Git(Repo, {TEXT("mv"), TEXT("a.txt"), TEXT("b b.txt")});
	const FString Third = GitTest::CommitAll(Repo, TEXT("rename"));

	TestTrue(TEXT("History is newest first"), Repository->ReadHistory(2, Commits, bHasMore, Error) && Commits.Num() == 2 && Commits[0].Oid == Third && bHasMore);
	GitCore::FCommitDetails Details;
	TestTrue(TEXT("A commit's files are listed"), Repository->ReadCommit(Third, Details, Error) && Details.Files.Num() == 2);
	TestTrue(TEXT("The root commit compares with nothing"), Repository->ReadCommit(First, Details, Error) && Details.Files.Num() == 1 && Details.Files[0].Status == TEXT('A'));

	TArray<GitCore::FFileRevision> Revisions;
	TestTrue(TEXT("File history follows renames"), Repository->ReadFileHistory(TEXT("b b.txt"), 50, Revisions, Error) && Revisions.Num() == 3);
	if (Revisions.Num() == 3)
	{
		TestTrue(TEXT("Newest revision uses the new name"), Revisions[0].Commit == Third && Revisions[0].Path == TEXT("b b.txt"));
		TestTrue(TEXT("Older revisions use the old name"), Revisions[2].Commit == First && Revisions[2].Path == TEXT("a.txt"));
		const FString Exported = Fixture.Root / TEXT("export/a.txt");
		TestTrue(TEXT("An old revision exports by its old name"), Repository->ExportRevision(Revisions[1].Commit, Revisions[1].Path, Exported).bOk);
		FString Text;
		FFileHelper::LoadFileToString(Text, *Exported);
		TestEqual(TEXT("The export holds that revision"), Text, FString(TEXT("two\n")));
	}
	(void)Second;
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitRepositoryStashBranchTest, "GitWorkspace.Repository.StashesAndBranches", RepositoryTestFlags)
bool FGitRepositoryStashBranchTest::RunTest(const FString&)
{
	GitTest::FFixture Fixture;
	const FString Repo = Fixture.InitRepo();
	GitTest::WriteFile(Repo, TEXT("a.txt"), TEXT("base\n"));
	GitTest::WriteFile(Repo, TEXT("b.txt"), TEXT("base\n"));
	GitTest::CommitAll(Repo, TEXT("base"));
	const GitCore::FGitRepositoryPtr Repository = GitTest::OpenRepository(*this, Repo);
	if (!Repository.IsValid())
	{
		return false;
	}

	GitTest::WriteFile(Repo, TEXT("a.txt"), TEXT("staged\n"));
	Repository->Stage({TEXT("a.txt")});
	GitTest::WriteFile(Repo, TEXT("a.txt"), TEXT("working\n"));
	GitTest::WriteFile(Repo, TEXT("b.txt"), TEXT("not stashed\n"));
	GitTest::WriteFile(Repo, TEXT("new.txt"), TEXT("untracked\n"));
	TestTrue(TEXT("Stash selected files"), Repository->StashPush({TEXT("a.txt"), TEXT("new.txt")}, TEXT("My stash"), true).bOk);
	TestEqual(TEXT("The stashed file is back to HEAD"), GitTest::ReadFile(Repo, TEXT("a.txt")), FString(TEXT("base\n")));
	TestFalse(TEXT("The untracked file was stashed"), GitTest::FileExists(Repo, TEXT("new.txt")));
	TestEqual(TEXT("Other files are untouched"), GitTest::ReadFile(Repo, TEXT("b.txt")), FString(TEXT("not stashed\n")));

	TArray<GitCore::FStashEntry> Stashes;
	FString Error;
	TestTrue(TEXT("Stashes are listed"), Repository->ReadStashes(Stashes, Error) && Stashes.Num() == 1 && Stashes[0].Message.Contains(TEXT("My stash")));
	TArray<FString> Paths;
	TestTrue(TEXT("A stash's files are listed"), Repository->ReadStashPaths(TEXT("stash@{0}"), Paths, Error) && Paths.Contains(TEXT("a.txt")) && Paths.Contains(TEXT("new.txt")));
	TestTrue(TEXT("Apply restores staging"), Repository->StashApply(TEXT("stash@{0}"), true, false).bOk);
	TestEqual(TEXT("Working version restored"), GitTest::ReadFile(Repo, TEXT("a.txt")), FString(TEXT("working\n")));
	TestEqual(TEXT("Staged version restored"), GitTest::Output(GitTest::Git(Repo, {TEXT("show"), TEXT(":a.txt")})), FString(TEXT("staged")));
	TestTrue(TEXT("Untracked file restored"), GitTest::FileExists(Repo, TEXT("new.txt")));
	TestTrue(TEXT("Apply keeps the stash"), Repository->ReadStashes(Stashes, Error) && Stashes.Num() == 1);
	TestTrue(TEXT("Applying over the same changes is refused"), !Repository->StashApply(TEXT("stash@{0}"), true, false).bOk);
	TestTrue(TEXT("Drop removes it"), Repository->StashDrop(TEXT("stash@{0}")).bOk && Repository->ReadStashes(Stashes, Error) && Stashes.IsEmpty());

	GitTest::CommitAll(Repo, TEXT("save work"));
	TestTrue(TEXT("Create a branch"), Repository->CreateBranch(TEXT("feature/water")).bOk);
	TestFalse(TEXT("Invalid names are refused"), Repository->CreateBranch(TEXT("bad name..")).bOk);
	TArray<GitCore::FBranchInfo> Branches;
	TestTrue(TEXT("Branches are listed"), Repository->ReadBranches(Branches, Error) && Branches.ContainsByPredicate([](const GitCore::FBranchInfo& Branch) { return Branch.Name == TEXT("feature/water") && Branch.bCurrent; }));
	GitTest::WriteFile(Repo, TEXT("a.txt"), TEXT("feature\n"));
	GitTest::CommitAll(Repo, TEXT("feature change"));
	GitTest::WriteFile(Repo, TEXT("a.txt"), TEXT("uncommitted\n"));
	TestFalse(TEXT("Switching over overlapping edits is refused"), Repository->SwitchBranch(TEXT("main")).bOk);
	TestEqual(TEXT("The edit survives the refusal"), GitTest::ReadFile(Repo, TEXT("a.txt")), FString(TEXT("uncommitted\n")));
	Repository->RestorePaths({TEXT("a.txt")});
	TestTrue(TEXT("Switching back works"), Repository->SwitchBranch(TEXT("main")).bOk);
	return true;
}

#endif
