// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#if WITH_DEV_AUTOMATION_TESTS

#include "GitTestFixtures.h"
#include "Core/GitPatch.h"
#include "Editor/Panel/SGitDiffView.h"

#include <initializer_list>

namespace
{
	constexpr EAutomationTestFlags LineTestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

	/** What the index holds for Path, byte for byte. */
	FString Staged(const FString& Repo, const FString& Path)
	{
		return GitTest::Git(Repo, {TEXT("cat-file"), TEXT("blob"), TEXT(":") + Path}).StdoutText();
	}

	/** Lines of a diff picked as the Git panel picks them: "-two" is the removed line "two", "+2" the added line "2". */
	GitCore::FLineSelection Pick(const FString& Diff, std::initializer_list<const TCHAR*> Picks)
	{
		GitCore::FLineSelection Selection;
		for (const TSharedPtr<FGitDiffLine>& Line : GitDiff::Parse(Diff))
		{
			for (const TCHAR* Picked : Picks)
			{
				if (Picked[0] == TEXT('-') && Line->Kind == FGitDiffLine::EKind::Removed && Line->Text.Equals(Picked + 1, ESearchCase::CaseSensitive))
				{
					Selection.Removed.Add(Line->OldLine);
				}
				else if (Picked[0] == TEXT('+') && Line->Kind == FGitDiffLine::EKind::Added && Line->Text.Equals(Picked + 1, ESearchCase::CaseSensitive))
				{
					Selection.Added.Add(Line->NewLine);
				}
			}
		}
		return Selection;
	}

	/** Picks lines in the file's diff as it is now (staged for Unstage), then applies Action to them. */
	GitCore::FOpResult ApplyPicked(const GitCore::FGitRepository& Repository, const FString& Path, GitCore::ELineAction Action, std::initializer_list<const TCHAR*> Picks)
	{
		FString Diff;
		Repository.Diff(Path, Action == GitCore::ELineAction::Unstage, Diff);
		return Repository.ApplyLines(Path, Action, Diff, Pick(Diff, Picks));
	}

	/** The same for a file Git does not track, whose diff is its content. */
	GitCore::FOpResult ApplyPickedNew(const GitCore::FGitRepository& Repository, const FString& Path, GitCore::ELineAction Action, std::initializer_list<const TCHAR*> Picks)
	{
		TArray<uint8> Bytes;
		FFileHelper::LoadFileToArray(Bytes, *Repository.ToAbsolute(Path));
		const FString Diff = GitCore::NewFileDiff(Bytes);
		return Repository.ApplyLines(Path, Action, Diff, Pick(Diff, Picks));
	}

	FString Patch(const FString& Diff, std::initializer_list<const TCHAR*> Picks, bool bReverse)
	{
		FString Out;
		FString Error;
		return GitCore::BuildLinePatch(Diff, Pick(Diff, Picks), bReverse, Out, Error) ? Out : TEXT("error: ") + Error;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitLinePatchTextTest, "GitWorkspace.Lines.PatchText", LineTestFlags)
bool FGitLinePatchTextTest::RunTest(const FString&)
{
	const FString TwoHunks = TEXT("diff --git a/f.txt b/f.txt\nindex 1111111..2222222 100644\n--- a/f.txt\n+++ b/f.txt\n")
		TEXT("@@ -1,4 +1,5 @@\n a\n+new1\n+new2\n b\n-c\n d\n@@ -10,3 +11,4 @@ section\n j\n k\n l\n+m\n");
	TestEqualSensitive(TEXT("A hunk with nothing picked is left out, and the next one starts where the old version has it"), Patch(TwoHunks, {TEXT("+m")}, false),
		FString(TEXT("diff --git a/f.txt b/f.txt\n--- a/f.txt\n+++ b/f.txt\n@@ -10,3 +10,4 @@\n j\n k\n l\n+m\n")));
	TestEqualSensitive(TEXT("An unpicked removed line stays as context, an unpicked added line goes, and later hunks move by what was picked"),
		Patch(TwoHunks, {TEXT("+new1"), TEXT("+m")}, false),
		FString(TEXT("diff --git a/f.txt b/f.txt\n--- a/f.txt\n+++ b/f.txt\n@@ -1,4 +1,5 @@\n a\n+new1\n b\n c\n d\n@@ -10,3 +11,4 @@\n j\n k\n l\n+m\n")));
	TestEqualSensitive(TEXT("In reverse, unpicked added lines stay as context and unpicked removed lines go"), Patch(TwoHunks, {TEXT("-c"), TEXT("+m")}, true),
		FString(TEXT("diff --git a/f.txt b/f.txt\n--- a/f.txt\n+++ b/f.txt\n@@ -1,6 +1,5 @@\n a\n new1\n new2\n b\n-c\n d\n@@ -12,3 +11,4 @@\n j\n k\n l\n+m\n")));

	// "b" and "c" were edited into "B" and "C": each removed line pairs with the added line in its place.
	const FString Edits = TEXT("diff --git a/e.txt b/e.txt\n--- a/e.txt\n+++ b/e.txt\n@@ -1,4 +1,4 @@\n a\n-b\n-c\n+B\n+C\n x\n");
	TestEqualSensitive(TEXT("Staging one edit of a run keeps the line after it in place"), Patch(Edits, {TEXT("-b"), TEXT("+B")}, false),
		FString(TEXT("diff --git a/e.txt b/e.txt\n--- a/e.txt\n+++ b/e.txt\n@@ -1,4 +1,4 @@\n a\n-b\n+B\n c\n x\n")));
	TestEqualSensitive(TEXT("An added line goes in after the line it replaces"), Patch(Edits, {TEXT("+B")}, false),
		FString(TEXT("diff --git a/e.txt b/e.txt\n--- a/e.txt\n+++ b/e.txt\n@@ -1,4 +1,5 @@\n a\n b\n+B\n c\n x\n")));
	TestEqualSensitive(TEXT("In reverse, a removed line comes back before its replacement"), Patch(Edits, {TEXT("-c")}, true),
		FString(TEXT("diff --git a/e.txt b/e.txt\n--- a/e.txt\n+++ b/e.txt\n@@ -1,5 +1,4 @@\n a\n B\n-c\n C\n x\n")));

	const FString NewFile = TEXT("diff --git a/my file.txt b/my file.txt\nnew file mode 100755\nindex 0000000..3333333\n--- /dev/null\n+++ b/my file.txt\t\n@@ -0,0 +1,3 @@\n+x\n+y\n+z\n");
	TestEqualSensitive(TEXT("Taking some lines out of a new file changes it in place"), Patch(NewFile, {TEXT("+y")}, true),
		FString(TEXT("diff --git a/my file.txt b/my file.txt\n--- a/my file.txt\t\n+++ b/my file.txt\t\n@@ -1,2 +1,3 @@\n x\n+y\n z\n")));
	TestEqualSensitive(TEXT("Taking every line out of a new file removes it"), Patch(NewFile, {TEXT("+x"), TEXT("+y"), TEXT("+z")}, true),
		FString(TEXT("diff --git a/my file.txt b/my file.txt\nnew file mode 100755\n--- /dev/null\n+++ b/my file.txt\t\n@@ -0,0 +1,3 @@\n+x\n+y\n+z\n")));
	TestEqualSensitive(TEXT("Some lines of a new file make a new file of their own"), Patch(NewFile, {TEXT("+z")}, false),
		FString(TEXT("diff --git a/my file.txt b/my file.txt\nnew file mode 100755\n--- /dev/null\n+++ b/my file.txt\t\n@@ -0,0 +1,1 @@\n+z\n")));

	const FString Deleted = TEXT("diff --git \"a/d\\\"q.txt\" \"b/d\\\"q.txt\"\ndeleted file mode 100644\nindex 4444444..0000000\n--- \"a/d\\\"q.txt\"\n+++ /dev/null\n@@ -1,2 +0,0 @@\n-p\n-q\n");
	TestEqualSensitive(TEXT("Staging some lines of a deleted file keeps the rest, and a quoted name stays quoted"), Patch(Deleted, {TEXT("-q")}, false),
		FString(TEXT("diff --git \"a/d\\\"q.txt\" \"b/d\\\"q.txt\"\n--- \"a/d\\\"q.txt\"\n+++ \"b/d\\\"q.txt\"\n@@ -1,2 +1,1 @@\n p\n-q\n")));
	TestEqualSensitive(TEXT("Bringing back some lines of a deleted file brings the file back with them"), Patch(Deleted, {TEXT("-q")}, true),
		FString(TEXT("diff --git \"a/d\\\"q.txt\" \"b/d\\\"q.txt\"\ndeleted file mode 100644\n--- \"a/d\\\"q.txt\"\n+++ /dev/null\n@@ -1,1 +0,0 @@\n-q\n")));

	// "c" had no newline; adding "d" after it gives it one, so picking "d" needs that change too.
	const FString NoNewline = TEXT("diff --git a/n.txt b/n.txt\n--- a/n.txt\n+++ b/n.txt\n@@ -1,3 +1,4 @@\n a\n b\n-c\n\\ No newline at end of file\n+c\n+d\n\\ No newline at end of file\n");
	TestEqualSensitive(TEXT("A line added after one without a newline brings the newline"), Patch(NoNewline, {TEXT("+d")}, false),
		FString(TEXT("diff --git a/n.txt b/n.txt\n--- a/n.txt\n+++ b/n.txt\n@@ -1,3 +1,4 @@\n a\n b\n-c\n\\ No newline at end of file\n+c\n+d\n\\ No newline at end of file\n")));
	TestEqualSensitive(TEXT("Removing the old last line leaves the one before it last"), Patch(NoNewline, {TEXT("-c")}, false),
		FString(TEXT("diff --git a/n.txt b/n.txt\n--- a/n.txt\n+++ b/n.txt\n@@ -1,3 +1,2 @@\n a\n b\n-c\n\\ No newline at end of file\n")));
	TestEqualSensitive(TEXT("In reverse, a removed line put back before the last one gets a newline"), Patch(NoNewline, {TEXT("-c"), TEXT("+d")}, true),
		FString(TEXT("diff --git a/n.txt b/n.txt\n--- a/n.txt\n+++ b/n.txt\n@@ -1,4 +1,4 @@\n a\n b\n-c\n c\n+d\n\\ No newline at end of file\n")));

	TestEqualSensitive(TEXT("Picking only context changes nothing"), Patch(TwoHunks, {}, false), FString());
	TestTrue(TEXT("A conflict's combined diff is refused"), Patch(TEXT("diff --cc f.txt\n@@@ -1,1 -1,1 +1,1 @@@\n- a\n -b\n++c\n"), {TEXT("+c")}, false).StartsWith(TEXT("error: ")));
	TestTrue(TEXT("A binary file is refused"), Patch(TEXT("diff --git a/x.png b/x.png\nindex 1111111..2222222 100644\nBinary files a/x.png and b/x.png differ\n"), {}, false).StartsWith(TEXT("error: ")));
	TestTrue(TEXT("A submodule is refused"), Patch(TEXT("diff --git a/sub b/sub\nindex 1111111..2222222 160000\n--- a/sub\n+++ b/sub\n@@ -1 +1 @@\n-Subproject commit 1111111\n+Subproject commit 2222222\n"),
		{TEXT("+Subproject commit 2222222")}, false).StartsWith(TEXT("error: ")));
	TestTrue(TEXT("A diff that ends early is refused"), Patch(TEXT("diff --git a/f.txt b/f.txt\n--- a/f.txt\n+++ b/f.txt\n@@ -1,3 +1,3 @@\n a\n-b\n+c\n"), {TEXT("+c")}, false).StartsWith(TEXT("error: ")));

	const FTCHARToUTF8 Text(TEXT("café 水 \U0001F600"));
	TestTrue(TEXT("UTF-8 is valid"), GitCore::IsValidUtf8(reinterpret_cast<const uint8*>(Text.Get()), Text.Length()));
	const uint8 Latin1[] = {'c', 'a', 'f', 0xE9, '\n'};
	const uint8 Overlong[] = {0xC0, 0xAF};
	const uint8 Surrogate[] = {0xED, 0xA0, 0x80};
	const uint8 Cut[] = {0xE6, 0xB0};
	TestFalse(TEXT("Latin-1 is not"), GitCore::IsValidUtf8(Latin1, UE_ARRAY_COUNT(Latin1)));
	TestFalse(TEXT("Nor an overlong form"), GitCore::IsValidUtf8(Overlong, UE_ARRAY_COUNT(Overlong)));
	TestFalse(TEXT("Nor a surrogate"), GitCore::IsValidUtf8(Surrogate, UE_ARRAY_COUNT(Surrogate)));
	TestFalse(TEXT("Nor a character cut short"), GitCore::IsValidUtf8(Cut, UE_ARRAY_COUNT(Cut)));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitLineStageTest, "GitWorkspace.Lines.StageUnstageDiscard", LineTestFlags)
bool FGitLineStageTest::RunTest(const FString&)
{
	GitTest::FFixture Fixture;
	const FString Repo = Fixture.InitRepo();
	const FString Ten = TEXT("one\ntwo\nthree\nfour\nfive\nsix\nseven\neight\nnine\nten\n");
	GitTest::WriteFile(Repo, TEXT("list.txt"), Ten);
	GitTest::WriteFile(Repo, TEXT("short.txt"), TEXT("one\ntwo\nthree\n"));
	GitTest::CommitAll(Repo, TEXT("base"));
	const GitCore::FGitRepositoryPtr Repository = GitTest::OpenRepository(*this, Repo);
	if (!Repository.IsValid())
	{
		return false;
	}
	using GitCore::ELineAction;

	// Two hunks: an edit at the top and a line added at the end.
	const FString Edited = TEXT("one\n2\nthree\nfour\nfive\nsix\nseven\neight\nnine\nten\neleven\n");
	GitTest::WriteFile(Repo, TEXT("list.txt"), Edited);
	const GitCore::FOpResult StagedEnd = ApplyPicked(*Repository, TEXT("list.txt"), ELineAction::Stage, {TEXT("+eleven")});
	TestTrue(TEXT("Stage one added line: ") + StagedEnd.Message, StagedEnd.bOk);
	TestEqualSensitive(TEXT("Only that line is staged"), Staged(Repo, TEXT("list.txt")), Ten + TEXT("eleven\n"));
	TestEqualSensitive(TEXT("The file on disk is untouched"), GitTest::ReadFile(Repo, TEXT("list.txt")), Edited);
	const GitCore::FOpResult Discarded = ApplyPicked(*Repository, TEXT("list.txt"), ELineAction::Discard, {TEXT("-two"), TEXT("+2")});
	TestTrue(TEXT("Discard the edited line: ") + Discarded.Message, Discarded.bOk);
	TestEqualSensitive(TEXT("It is back as it was, and the staged line stays"), GitTest::ReadFile(Repo, TEXT("list.txt")), Ten + TEXT("eleven\n"));
	const GitCore::FOpResult Unstaged = ApplyPicked(*Repository, TEXT("list.txt"), ELineAction::Unstage, {TEXT("+eleven")});
	TestTrue(TEXT("Unstage it: ") + Unstaged.Message, Unstaged.bOk);
	TestEqualSensitive(TEXT("The index is back at the last commit"), Staged(Repo, TEXT("list.txt")), Ten);
	TestEqualSensitive(TEXT("The line stays on disk"), GitTest::ReadFile(Repo, TEXT("list.txt")), Ten + TEXT("eleven\n"));

	// Two edits in a row: staging the first leaves the second line where it was.
	GitTest::WriteFile(Repo, TEXT("list.txt"), TEXT("one\nTWO\nTHREE\nfour\nfive\nsix\nseven\neight\nnine\nten\neleven\n"));
	TestTrue(TEXT("Stage one edit of two"), ApplyPicked(*Repository, TEXT("list.txt"), ELineAction::Stage, {TEXT("-two"), TEXT("+TWO")}).bOk);
	TestEqualSensitive(TEXT("The other line keeps its place"), Staged(Repo, TEXT("list.txt")), FString(TEXT("one\nTWO\nthree\nfour\nfive\nsix\nseven\neight\nnine\nten\n")));

	// Within one run of changes: "two" became "2", and "four" was added.
	const FString Short = TEXT("one\ntwo\nthree\n");
	GitTest::WriteFile(Repo, TEXT("short.txt"), TEXT("one\n2\nthree\nfour\n"));
	auto Reset = [&Repo]() { GitTest::Git(Repo, {TEXT("reset"), TEXT("-q"), TEXT("--"), TEXT("short.txt")}); };
	TestTrue(TEXT("Stage the removed line alone"), ApplyPicked(*Repository, TEXT("short.txt"), ELineAction::Stage, {TEXT("-two")}).bOk);
	TestEqualSensitive(TEXT("The index loses the line"), Staged(Repo, TEXT("short.txt")), FString(TEXT("one\nthree\n")));
	Reset();
	TestTrue(TEXT("Stage the replacement alone"), ApplyPicked(*Repository, TEXT("short.txt"), ELineAction::Stage, {TEXT("+2")}).bOk);
	TestEqualSensitive(TEXT("The index gains it after the old line"), Staged(Repo, TEXT("short.txt")), FString(TEXT("one\ntwo\n2\nthree\n")));
	Reset();
	TestTrue(TEXT("Stage the edit"), ApplyPicked(*Repository, TEXT("short.txt"), ELineAction::Stage, {TEXT("-two"), TEXT("+2")}).bOk);
	TestEqualSensitive(TEXT("The index has the edit without the added line"), Staged(Repo, TEXT("short.txt")), FString(TEXT("one\n2\nthree\n")));

	GitTest::Git(Repo, {TEXT("add"), TEXT("short.txt")});
	TestTrue(TEXT("Unstage the replacement alone"), ApplyPicked(*Repository, TEXT("short.txt"), ELineAction::Unstage, {TEXT("+2")}).bOk);
	TestEqualSensitive(TEXT("The index loses it, without the old line"), Staged(Repo, TEXT("short.txt")), FString(TEXT("one\nthree\nfour\n")));
	GitTest::Git(Repo, {TEXT("add"), TEXT("short.txt")});
	TestTrue(TEXT("Unstage the removal alone"), ApplyPicked(*Repository, TEXT("short.txt"), ELineAction::Unstage, {TEXT("-two")}).bOk);
	TestEqualSensitive(TEXT("The old line comes back before the new one"), Staged(Repo, TEXT("short.txt")), FString(TEXT("one\ntwo\n2\nthree\nfour\n")));
	Reset();
	TestTrue(TEXT("Discard the added line"), ApplyPicked(*Repository, TEXT("short.txt"), ELineAction::Discard, {TEXT("+four")}).bOk);
	TestEqualSensitive(TEXT("It is gone from disk, the edit stays"), GitTest::ReadFile(Repo, TEXT("short.txt")), FString(TEXT("one\n2\nthree\n")));
	TestEqualSensitive(TEXT("The index is untouched"), Staged(Repo, TEXT("short.txt")), Short);
	TestTrue(TEXT("Discard the removal alone"), ApplyPicked(*Repository, TEXT("short.txt"), ELineAction::Discard, {TEXT("-two")}).bOk);
	TestEqualSensitive(TEXT("The old line comes back"), GitTest::ReadFile(Repo, TEXT("short.txt")), FString(TEXT("one\ntwo\n2\nthree\n")));

	// The diff was read, then the file changed: nothing happens.
	FString Shown;
	Repository->Diff(TEXT("short.txt"), false, Shown);
	GitTest::WriteFile(Repo, TEXT("short.txt"), TEXT("one\ntwo\n2\nthree\nlater\n"));
	const GitCore::FOpResult Late = Repository->ApplyLines(TEXT("short.txt"), ELineAction::Stage, Shown, Pick(Shown, {TEXT("+2")}));
	TestFalse(TEXT("Lines picked in an older diff are refused"), Late.bOk);
	Repository->Diff(TEXT("short.txt"), false, Shown);
	GitTest::WriteFile(Repo, TEXT("short.txt"), TEXT("one\ntwo\n2\nthree\nLATER\n"));
	TestFalse(TEXT("even when a letter only changed case"), Repository->ApplyLines(TEXT("short.txt"), ELineAction::Stage, Shown, Pick(Shown, {TEXT("+later")})).bOk);
	TestEqualSensitive(TEXT("The index is untouched"), Staged(Repo, TEXT("short.txt")), Short);
	TestFalse(TEXT("Nothing picked is refused"), Repository->ApplyLines(TEXT("short.txt"), ELineAction::Stage, Shown, {}).bOk);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitLineFilesTest, "GitWorkspace.Lines.NewAndDeletedFiles", LineTestFlags)
bool FGitLineFilesTest::RunTest(const FString&)
{
	GitTest::FFixture Fixture;
	const FString Repo = Fixture.InitRepo();
	GitTest::WriteFile(Repo, TEXT("gone.txt"), TEXT("p\nq\nr\n"));
	GitTest::CommitAll(Repo, TEXT("base"));
	const GitCore::FGitRepositoryPtr Repository = GitTest::OpenRepository(*this, Repo);
	if (!Repository.IsValid())
	{
		return false;
	}
	using GitCore::ELineAction;
	auto IsTracked = [&Repo](const TCHAR* Path) { return !GitTest::Output(GitTest::Git(Repo, {TEXT("ls-files"), TEXT("--"), Path})).IsEmpty(); };

	// A staged new file.
	GitTest::WriteFile(Repo, TEXT("new.txt"), TEXT("a\nb\nc\n"));
	GitTest::Git(Repo, {TEXT("add"), TEXT("new.txt")});
	TestTrue(TEXT("Unstage a line of a staged new file"), ApplyPicked(*Repository, TEXT("new.txt"), ELineAction::Unstage, {TEXT("+b")}).bOk);
	TestEqualSensitive(TEXT("The new file stays staged without it"), Staged(Repo, TEXT("new.txt")), FString(TEXT("a\nc\n")));
	TestTrue(TEXT("Unstage its other lines"), ApplyPicked(*Repository, TEXT("new.txt"), ELineAction::Unstage, {TEXT("+a"), TEXT("+c")}).bOk);
	TestFalse(TEXT("Without lines it is no longer staged"), IsTracked(TEXT("new.txt")));
	TestEqualSensitive(TEXT("It stays on disk"), GitTest::ReadFile(Repo, TEXT("new.txt")), FString(TEXT("a\nb\nc\n")));

	// A file Git does not track, without a newline at its end.
	GitTest::WriteFile(Repo, TEXT("fresh.txt"), TEXT("x\ny\nz"));
	const GitCore::FOpResult StagedNew = ApplyPickedNew(*Repository, TEXT("fresh.txt"), ELineAction::Stage, {TEXT("+y")});
	TestTrue(TEXT("Stage one line of a new file: ") + StagedNew.Message, StagedNew.bOk);
	TestEqualSensitive(TEXT("The index holds that line alone"), Staged(Repo, TEXT("fresh.txt")), FString(TEXT("y\n")));
	TestEqualSensitive(TEXT("The file is untouched"), GitTest::ReadFile(Repo, TEXT("fresh.txt")), FString(TEXT("x\ny\nz")));
	GitTest::WriteFile(Repo, TEXT("last.txt"), TEXT("x\ny\nz"));
	TestTrue(TEXT("Stage the last line"), ApplyPickedNew(*Repository, TEXT("last.txt"), ELineAction::Stage, {TEXT("+z")}).bOk);
	TestEqualSensitive(TEXT("It keeps its missing newline"), Staged(Repo, TEXT("last.txt")), FString(TEXT("z")));
	GitTest::WriteFile(Repo, TEXT("whole.txt"), TEXT("1\n2\n"));
	TestTrue(TEXT("Stage every line"), ApplyPickedNew(*Repository, TEXT("whole.txt"), ELineAction::Stage, {TEXT("+1"), TEXT("+2")}).bOk);
	TestEqualSensitive(TEXT("The whole file is staged"), Staged(Repo, TEXT("whole.txt")), FString(TEXT("1\n2\n")));
	GitTest::WriteFile(Repo, TEXT("other.txt"), TEXT("1\n2\n3\n"));
	TestTrue(TEXT("Discard a line of a new file"), ApplyPickedNew(*Repository, TEXT("other.txt"), ELineAction::Discard, {TEXT("+2")}).bOk);
	TestEqualSensitive(TEXT("The line is gone"), GitTest::ReadFile(Repo, TEXT("other.txt")), FString(TEXT("1\n3\n")));
	TestFalse(TEXT("The file is still not tracked"), IsTracked(TEXT("other.txt")));
	TestTrue(TEXT("Discard the rest"), ApplyPickedNew(*Repository, TEXT("other.txt"), ELineAction::Discard, {TEXT("+1"), TEXT("+3")}).bOk);
	TestFalse(TEXT("A new file with every line discarded is gone"), GitTest::FileExists(Repo, TEXT("other.txt")));

	// Git LFS would store a staged part as a whole object of its own: such files are staged whole.
	GitTest::WriteFile(Repo, TEXT(".gitattributes"), TEXT("*.bin.txt filter=upper\n"));
	GitTest::Git(Repo, {TEXT("config"), TEXT("filter.upper.clean"), TEXT("tr a-z A-Z")});
	GitTest::WriteFile(Repo, TEXT("filtered.bin.txt"), TEXT("abc\ndef\n"));
	TestFalse(TEXT("A file a filter changes on the way in is refused"), ApplyPickedNew(*Repository, TEXT("filtered.bin.txt"), ELineAction::Stage, {TEXT("+abc")}).bOk);
	TestFalse(TEXT("and left untracked"), IsTracked(TEXT("filtered.bin.txt")));

	// A tracked file deleted from disk.
	IFileManager::Get().Delete(*(Repo / TEXT("gone.txt")));
	TestTrue(TEXT("Stage one line of a deletion"), ApplyPicked(*Repository, TEXT("gone.txt"), ELineAction::Stage, {TEXT("-q")}).bOk);
	TestEqualSensitive(TEXT("The index keeps the others"), Staged(Repo, TEXT("gone.txt")), FString(TEXT("p\nr\n")));
	TestTrue(TEXT("Discard the deletion of one line"), ApplyPicked(*Repository, TEXT("gone.txt"), ELineAction::Discard, {TEXT("-p")}).bOk);
	TestEqualSensitive(TEXT("The file comes back with that line"), GitTest::ReadFile(Repo, TEXT("gone.txt")), FString(TEXT("p\n")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitLineTextTest, "GitWorkspace.Lines.TextAndSettings", LineTestFlags)
bool FGitLineTextTest::RunTest(const FString&)
{
	GitTest::FFixture Fixture;
	const FString Repo = Fixture.InitRepo();
	// Settings that change how Git prints and applies diffs must not change what gets staged.
	GitTest::Git(Repo, {TEXT("config"), TEXT("diff.noprefix"), TEXT("true")});
	GitTest::Git(Repo, {TEXT("config"), TEXT("diff.context"), TEXT("0")});
	GitTest::Git(Repo, {TEXT("config"), TEXT("diff.suppressBlankEmpty"), TEXT("true")});
	GitTest::Git(Repo, {TEXT("config"), TEXT("apply.whitespace"), TEXT("fix")});
	GitTest::WriteFile(Repo, TEXT("blank.txt"), TEXT("a\n\nb\n\nc\n"));
	GitTest::WriteFile(Repo, TEXT("windows.txt"), TEXT("one\r\ntwo\r\n"));
	GitTest::WriteFile(Repo, TEXT("latin.txt"), TEXT("plain\n"));
	GitTest::WriteFile(Repo, TEXT("tail.txt"), TEXT("a\nb\nc"));
	GitTest::CommitAll(Repo, TEXT("base"));
	const GitCore::FGitRepositoryPtr Repository = GitTest::OpenRepository(*this, Repo);
	if (!Repository.IsValid())
	{
		return false;
	}
	using GitCore::ELineAction;

	GitTest::WriteFile(Repo, TEXT("blank.txt"), TEXT("a\n\nb \n\nc\nd\n"));
	TestTrue(TEXT("Stage lines between empty lines"), ApplyPicked(*Repository, TEXT("blank.txt"), ELineAction::Stage, {TEXT("-b"), TEXT("+b ")}).bOk);
	TestEqualSensitive(TEXT("Trailing spaces are staged as they are"), Staged(Repo, TEXT("blank.txt")), FString(TEXT("a\n\nb \n\nc\n")));

	GitTest::WriteFile(Repo, TEXT("windows.txt"), TEXT("one\r\nTWO\r\nthree\r\n"));
	TestTrue(TEXT("Stage a line of a CRLF file"), ApplyPicked(*Repository, TEXT("windows.txt"), ELineAction::Stage, {TEXT("+three")}).bOk);
	TestEqualSensitive(TEXT("Its line endings are kept"), Staged(Repo, TEXT("windows.txt")), FString(TEXT("one\r\ntwo\r\nthree\r\n")));

	const uint8 Latin1[] = {'c', 'a', 'f', 0xE9, '\n', 'p', 'l', 'a', 'i', 'n', '\n'};
	FFileHelper::SaveArrayToFile(TArrayView<const uint8>(Latin1, UE_ARRAY_COUNT(Latin1)), *(Repo / TEXT("latin.txt")));
	FString LatinDiff;
	Repository->Diff(TEXT("latin.txt"), false, LatinDiff);
	GitCore::FLineSelection FirstLine;
	FirstLine.Added.Add(1);
	const GitCore::FOpResult Latin = Repository->ApplyLines(TEXT("latin.txt"), ELineAction::Stage, LatinDiff, FirstLine);
	TestTrue(TEXT("Text that is not UTF-8 is refused"), !Latin.bOk && Latin.Message.Contains(TEXT("UTF-8")));
	TestEqualSensitive(TEXT("and stays unstaged"), Staged(Repo, TEXT("latin.txt")), FString(TEXT("plain\n")));

	GitTest::WriteFile(Repo, TEXT("tail.txt"), TEXT("a\nb\nc\nd"));
	TestTrue(TEXT("Stage a line added after one without a newline"), ApplyPicked(*Repository, TEXT("tail.txt"), ELineAction::Stage, {TEXT("+d")}).bOk);
	TestEqualSensitive(TEXT("The line before it gets its newline"), Staged(Repo, TEXT("tail.txt")), FString(TEXT("a\nb\nc\nd")));
	GitTest::Git(Repo, {TEXT("reset"), TEXT("-q")});
	GitTest::WriteFile(Repo, TEXT("tail.txt"), TEXT("a\nB\nc\nd"));
	TestTrue(TEXT("Stage an edit above it"), ApplyPicked(*Repository, TEXT("tail.txt"), ELineAction::Stage, {TEXT("-b"), TEXT("+B")}).bOk);
	TestEqualSensitive(TEXT("The end of the file stays as it was"), Staged(Repo, TEXT("tail.txt")), FString(TEXT("a\nB\nc")));
	TestTrue(TEXT("Discard the line at the end"), ApplyPicked(*Repository, TEXT("tail.txt"), ELineAction::Discard, {TEXT("-c"), TEXT("+c"), TEXT("+d")}).bOk);
	TestEqualSensitive(TEXT("The file ends as the index does"), GitTest::ReadFile(Repo, TEXT("tail.txt")), FString(TEXT("a\nB\nc")));
	return true;
}

#endif
