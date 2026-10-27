// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#if WITH_DEV_AUTOMATION_TESTS

#include "GitTestFixtures.h"
#include "Editor/GitWorkspaceModel.h"
#include "Editor/Panel/GitFileTree.h"
#include "Editor/Panel/SGitDiffView.h"
#include "Editor/Panel/SGitHistoryView.h"
#include "Editor/Panel/SGitPanel.h"
#include "Editor/Panel/SGitStashesView.h"

#include "Framework/Application/SlateApplication.h"
#include "ImageCore.h"
#include "ImageUtils.h"
#include "Misc/App.h"
#include "Types/ISlateMetaData.h"
#include "Widgets/Input/SSearchBox.h"
#include "Widgets/SWindow.h"

namespace
{
	constexpr EAutomationTestFlags PanelTestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

	/** The Git panel's model bound to a fixture repository through its own provider. */
	struct FPanelFixture
	{
		GitTest::FTestProvider Provider;
		TSharedRef<FGitWorkspaceModel> Model;

		explicit FPanelFixture(const FString& Repo)
			: Provider(Repo)
			, Model(FGitWorkspaceModel::CreateFor(Provider.Provider))
		{
		}

		/** Pumps the provider and game-thread tasks until the running action finished. */
		bool WaitIdle()
		{
			return GitTest::WaitFor([this]() { Provider->Tick(); return !Model->IsBusy(); });
		}

		bool WaitUntil(TFunction<bool(const GitCore::FSessionSnapshot&)> Predicate)
		{
			return GitTest::WaitFor([this, &Predicate]() { Provider->Tick(); return Predicate(*Provider.Snapshot()); });
		}

		void Refresh()
		{
			Provider->GetSession()->RequestStatusRefresh();
			Provider->GetSession()->FlushMutations();
		}
	};

	FString PanelHead(const FString& Repo, const FString& Ref = TEXT("HEAD"))
	{
		return GitTest::Output(GitTest::Git(Repo, {TEXT("rev-parse"), Ref}));
	}

	FString PanelSubject(const FString& Repo)
	{
		return GitTest::Output(GitTest::Git(Repo, {TEXT("log"), TEXT("-1"), TEXT("--format=%s")}));
	}

	FString PanelStaged(const FString& Repo)
	{
		return GitTest::Output(GitTest::Git(Repo, {TEXT("diff"), TEXT("--cached"), TEXT("--name-only")}));
	}

	/** The widget below Root carrying Tag, whether or not it is showing. */
	TSharedPtr<SWidget> FindTagged(const TSharedRef<SWidget>& Root, FName Tag)
	{
		const TSharedPtr<FTagMetaData> Meta = Root->GetMetaData<FTagMetaData>();
		if (Meta.IsValid() && Meta->Tag == Tag)
		{
			return Root;
		}
		FChildren* Children = Root->GetAllChildren();
		for (int32 Index = 0; Children && Index < Children->Num(); ++Index)
		{
			if (const TSharedPtr<SWidget> Found = FindTagged(Children->GetChildAt(Index), Tag))
			{
				return Found;
			}
		}
		return nullptr;
	}

	/** Selects the first item of a tagged list in the panel. */
	template<typename ItemType>
	void SelectFirst(const TSharedRef<SWidget>& Panel, FName Tag)
	{
		if (const TSharedPtr<SWidget> Found = FindTagged(Panel, Tag))
		{
			const TSharedPtr<SListView<ItemType>> List = StaticCastSharedPtr<SListView<ItemType>>(Found);
			if (!List->GetItems().IsEmpty())
			{
				List->SetSelection(List->GetItems()[0], ESelectInfo::OnMouseClick);
			}
		}
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitPanelStageCommitPushTest, "GitWorkspace.Panel.StageCommitAndPush", PanelTestFlags)
bool FGitPanelStageCommitPushTest::RunTest(const FString&)
{
	GitTest::FFixture Fixture;
	const FString Origin = Fixture.InitBare();
	const FString Seed = Fixture.InitRepo(TEXT("seed"));
	GitTest::WriteFile(Seed, TEXT("a.txt"), TEXT("a\n"));
	GitTest::WriteFile(Seed, TEXT("Content/Hero.uasset"), TEXT("hero\n"));
	GitTest::CommitAll(Seed, TEXT("base"));
	GitTest::Git(Seed, {TEXT("push"), TEXT("-q"), Origin, TEXT("main")});
	const FString Repo = Fixture.Clone(Origin, TEXT("work"));
	AddExpectedErrorPlain(TEXT("changed since"), EAutomationExpectedErrorFlags::Contains, 0);
	GitTest::FScopedNoBackgroundFetch NoFetch;
	FPanelFixture Panel(Repo);
	TestTrue(TEXT("The model is active for its repository"), Panel.Model->IsActive());
	// The panel builds on the model without touching git itself.
	const TSharedRef<SGitPanel> Widget = SNew(SGitPanel, Panel.Model);
	Widget->ShowPaths({TEXT("a.txt")});

	GitTest::WriteFile(Repo, TEXT("a.txt"), TEXT("a changed\n"));
	GitTest::WriteFile(Repo, TEXT("Content/Hero.uasset"), TEXT("hero changed\n"));
	Panel.Refresh();
	TestEqual(TEXT("Both changes are listed"), Panel.Provider.Snapshot()->Status.Entries.Num(), 2);

	Panel.Model->Stage({TEXT("a.txt")});
	TestTrue(TEXT("Staging shows up"), Panel.WaitUntil([](const GitCore::FSessionSnapshot& Snapshot) { return Snapshot.Status.StagedCount() == 1; }));
	TestEqual(TEXT("Only a.txt is staged"), PanelStaged(Repo), FString(TEXT("a.txt")));

	bool bCommitted = false;
	Panel.Model->Commit(TEXT("Panel commit"), false, [&bCommitted](bool bDone) { bCommitted = bDone; });
	TestTrue(TEXT("The commit finishes"), Panel.WaitIdle());
	TestTrue(TEXT("The commit is reported"), bCommitted);
	TestEqual(TEXT("The staged file was committed"), PanelSubject(Repo), FString(TEXT("Panel commit")));
	TestTrue(TEXT("The unstaged asset stays modified"), Panel.Provider.Snapshot()->Status.Find(TEXT("Content/Hero.uasset")) != nullptr);
	TestFalse(TEXT("No failure is shown"), Panel.Model->GetLastFailure().IsSet());

	Panel.Model->Push();
	TestTrue(TEXT("The push finishes"), Panel.WaitIdle());
	TestEqual(TEXT("The server has the commit"), PanelHead(Origin, TEXT("main")), PanelHead(Repo));

	Panel.Model->Stage({TEXT("Content/Hero.uasset")});
	Panel.WaitUntil([](const GitCore::FSessionSnapshot& Snapshot) { return Snapshot.Status.StagedCount() == 1; });
	Panel.Model->Commit(TEXT("Hero"), true);
	TestTrue(TEXT("Commit and push finishes"), Panel.WaitIdle());
	TestEqual(TEXT("Commit and push reaches the server"), PanelHead(Origin, TEXT("main")), PanelHead(Repo));

	// The commit must contain what the user reviewed: staging behind the panel's back is refused.
	GitTest::WriteFile(Repo, TEXT("a.txt"), TEXT("a reviewed\n"));
	Panel.Model->Stage({TEXT("a.txt")});
	Panel.WaitUntil([](const GitCore::FSessionSnapshot& Snapshot) { return Snapshot.Status.StagedCount() == 1; });
	GitTest::WriteFile(Repo, TEXT("b.txt"), TEXT("sneaky\n"));
	GitTest::Git(Repo, {TEXT("add"), TEXT("b.txt")});
	const FString Before = PanelHead(Repo);
	Panel.Model->Commit(TEXT("Reviewed"), false);
	Panel.WaitIdle();
	TestEqual(TEXT("Nothing was committed"), PanelHead(Repo), Before);
	TestTrue(TEXT("The refusal is shown"), Panel.Model->GetLastFailure().IsSet());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitPanelBranchesAndStashesTest, "GitWorkspace.Panel.BranchesAndStashes", PanelTestFlags)
bool FGitPanelBranchesAndStashesTest::RunTest(const FString&)
{
	GitTest::FFixture Fixture;
	const FString Repo = Fixture.InitRepo();
	GitTest::WriteFile(Repo, TEXT("a.txt"), TEXT("base\n"));
	GitTest::WriteFile(Repo, TEXT("Content/B.uasset"), TEXT("b\n"));
	GitTest::CommitAll(Repo, TEXT("base"));
	AddExpectedErrorPlain(TEXT("also differs on main"), EAutomationExpectedErrorFlags::Contains, 1);
	GitTest::FScopedNoBackgroundFetch NoFetch;
	FPanelFixture Panel(Repo);
	Panel.Refresh();

	Panel.Model->CreateBranch(TEXT("feature"));
	TestTrue(TEXT("Creating a branch finishes"), Panel.WaitIdle());
	TestEqual(TEXT("The new branch is current"), Panel.Provider.Snapshot()->Status.Branch, FString(TEXT("feature")));
	GitTest::WriteFile(Repo, TEXT("a.txt"), TEXT("feature\n"));
	GitTest::CommitAll(Repo, TEXT("Feature work"));

	// Local edits to a file that differs on the target branch stop the switch before anything changes.
	GitTest::WriteFile(Repo, TEXT("a.txt"), TEXT("uncommitted\n"));
	Panel.Model->SwitchBranch(TEXT("main"));
	Panel.WaitIdle();
	TestEqual(TEXT("Still on feature"), GitTest::Output(GitTest::Git(Repo, {TEXT("branch"), TEXT("--show-current")})), FString(TEXT("feature")));
	TestEqual(TEXT("The local edit is untouched"), GitTest::ReadFile(Repo, TEXT("a.txt")), FString(TEXT("uncommitted\n")));

	GitTest::Git(Repo, {TEXT("checkout"), TEXT("--"), TEXT("a.txt")});
	Panel.Model->SwitchBranch(TEXT("main"));
	TestTrue(TEXT("Switching finishes"), Panel.WaitIdle());
	TestEqual(TEXT("Now on main"), GitTest::Output(GitTest::Git(Repo, {TEXT("branch"), TEXT("--show-current")})), FString(TEXT("main")));
	TestEqual(TEXT("Files match main"), GitTest::ReadFile(Repo, TEXT("a.txt")), FString(TEXT("base\n")));

	// Stash everything (new files too), then bring it back.
	GitTest::WriteFile(Repo, TEXT("a.txt"), TEXT("work in progress\n"));
	GitTest::WriteFile(Repo, TEXT("new.txt"), TEXT("new\n"));
	Panel.Refresh();
	Panel.Model->StashChanges(TEXT("wip"), true);
	TestTrue(TEXT("Stashing finishes"), Panel.WaitIdle());
	TestEqual(TEXT("The edit was put aside"), GitTest::ReadFile(Repo, TEXT("a.txt")), FString(TEXT("base\n")));
	TestFalse(TEXT("The new file was put aside"), GitTest::FileExists(Repo, TEXT("new.txt")));
	TestEqual(TEXT("One stash is listed"), Panel.Provider.Snapshot()->Stashes.Num(), 1);

	Panel.Model->ApplyStash(TEXT("stash@{0}"), true);
	TestTrue(TEXT("Restoring finishes"), Panel.WaitIdle());
	TestEqual(TEXT("The edit is back"), GitTest::ReadFile(Repo, TEXT("a.txt")), FString(TEXT("work in progress\n")));
	TestTrue(TEXT("The new file is back"), GitTest::FileExists(Repo, TEXT("new.txt")));
	TestEqual(TEXT("The stash was removed"), Panel.Provider.Snapshot()->Stashes.Num(), 0);

	Panel.Model->StashChanges(TEXT("throwaway"), false);
	Panel.WaitIdle();
	Panel.Model->DropStash(TEXT("stash@{0}"));
	TestTrue(TEXT("Deleting finishes"), Panel.WaitIdle());
	TestEqual(TEXT("No stash is left"), Panel.Provider.Snapshot()->Stashes.Num(), 0);
	TestEqual(TEXT("The tracked edit is gone"), GitTest::ReadFile(Repo, TEXT("a.txt")), FString(TEXT("base\n")));
	TestTrue(TEXT("Untracked files were not stashed"), GitTest::FileExists(Repo, TEXT("new.txt")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitPanelLocksAndDiscardTest, "GitWorkspace.Panel.LocksAndDiscard", PanelTestFlags)
bool FGitPanelLocksAndDiscardTest::RunTest(const FString&)
{
	GitTest::FLockedTeam Team(*this);
	if (!Team.IsReady())
	{
		return false;
	}
	GitTest::FScopedNoBackgroundFetch NoFetch;
	FPanelFixture Panel(Team.Alice);
	Panel.Refresh();
	TestTrue(TEXT("Locks are read after connecting"), Panel.WaitUntil([](const GitCore::FSessionSnapshot& Snapshot) { return Snapshot.Locks.bVerified; }));

	Panel.Model->Lock({TEXT("Content/Hero.uasset")});
	TestTrue(TEXT("Locking goes through Check Out"), Panel.WaitUntil([](const GitCore::FSessionSnapshot& Snapshot)
	{
		const GitCore::FLockInfo* Lock = Snapshot.Locks.Find(TEXT("Content/Hero.uasset"));
		return Lock && Lock->bOurs;
	}));
	Panel.Model->Unlock({TEXT("Content/Hero.uasset")}, false);
	TestTrue(TEXT("Releasing finishes"), Panel.WaitIdle());
	GitCore::FLockSnapshot Server;
	Panel.Provider->GetSession()->GetRepository().VerifyLocks(TEXT("origin"), GitCore::ECallKind::UserNetwork, Server);
	TestTrue(TEXT("The server no longer holds the lock"), Server.Find(TEXT("Content/Hero.uasset")) == nullptr);

	GitTest::WriteFile(Team.Alice, TEXT("readme.txt"), TEXT("oops\n"));
	GitTest::WriteFile(Team.Alice, TEXT("scratch.txt"), TEXT("scratch\n"));
	Panel.Refresh();
	Panel.Model->Discard({TEXT("readme.txt"), TEXT("scratch.txt")});
	TestTrue(TEXT("Discarding finishes"), Panel.WaitIdle());
	TestEqual(TEXT("The edit is gone"), GitTest::ReadFile(Team.Alice, TEXT("readme.txt")), FString(TEXT("text\n")));
	TestFalse(TEXT("The new file is deleted"), GitTest::FileExists(Team.Alice, TEXT("scratch.txt")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitPanelFileTreeTest, "GitWorkspace.Panel.FileTree", PanelTestFlags)
bool FGitPanelFileTreeTest::RunTest(const FString&)
{
	TestEqual(TEXT("A World Partition actor is listed under its level"), GitWorkspaceText::TreeFolder(TEXT("Content/__ExternalActors__/Maps/Main/A/7Q/0F2KD8XN3PZ4W1.uasset")),
		FString(TEXT("Content/__ExternalActors__/Maps/Main")));
	TestEqual(TEXT("Other files are listed under their folder"), GitWorkspaceText::TreeFolder(TEXT("Content/Props/Barrel.uasset")), FString(TEXT("Content/Props")));
	TestEqual(TEXT("Top-level files have no folder"), GitWorkspaceText::TreeFolder(TEXT("readme.txt")), FString());

	const TArray<FString> Paths = {TEXT("Content/Props/Barrel.uasset"), TEXT("Content/Props/Crate.uasset"), TEXT("Content/Maps/Main.umap"),
		TEXT("Content/__ExternalActors__/Maps/Main/A/7Q/0F2KD8XN3PZ4W1.uasset"), TEXT("Content/__ExternalActors__/Maps/Main/B/3X/1ABCDEF.uasset"),
		TEXT("Source/Demo/Private/DemoCharacter.cpp"), TEXT("readme.txt")};
	TArray<TSharedPtr<FGitPathItem>> Files;
	for (const FString& Path : Paths)
	{
		const TSharedPtr<FGitPathItem> File = MakeShared<FGitPathItem>();
		File->Path = Path;
		Files.Add(File);
	}
	const TArray<TSharedPtr<FGitPathItem>> Roots = GitFileTree::Build(Files, true);
	if (!TestEqual(TEXT("Two folders, then the top-level file"), Roots.Num(), 3))
	{
		return false;
	}
	TestTrue(TEXT("Content comes first"), Roots[0]->bFolder && Roots[0]->Label == TEXT("Content") && Roots[0]->FileCount == 5);
	TestTrue(TEXT("A chain of single folders is one row"), Roots[1]->bFolder && Roots[1]->Label == TEXT("Source/Demo/Private") && Roots[1]->Path == TEXT("Source/Demo/Private"));
	TestTrue(TEXT("Files follow the folders"), !Roots[2]->bFolder && Roots[2]->Path == TEXT("readme.txt"));
	auto Child = [](const TSharedPtr<FGitPathItem>& Folder, const TCHAR* Label)
	{
		const TSharedPtr<FGitPathItem>* Found = Folder->Children.FindByPredicate([Label](const TSharedPtr<FGitPathItem>& Item) { return Item->Label == Label; });
		return Found ? *Found : TSharedPtr<FGitPathItem>();
	};
	const TSharedPtr<FGitPathItem> Actors = Child(Roots[0], TEXT("__ExternalActors__/Maps/Main"));
	TestTrue(TEXT("A level's actors sit in one folder, without the hash folders"),
		Actors.IsValid() && Actors->Path == TEXT("Content/__ExternalActors__/Maps/Main") && Actors->Children.Num() == 2 && !Actors->Children[0]->bFolder);
	const TSharedPtr<FGitPathItem> Props = Child(Roots[0], TEXT("Props"));
	if (!TestTrue(TEXT("Files keep the order given"), Props.IsValid() && Props->Children.Num() == 2 && Props->Children[0]->Path == TEXT("Content/Props/Barrel.uasset")))
	{
		return false;
	}
	TestEqual(TEXT("Without folders the list is unchanged"), GitFileTree::Build(Files, false).Num(), Paths.Num());
	TestEqual(TEXT("A selection means the files in it, each once"), GitFileTree::SelectedFiles<FGitPathItem>({Roots[0], Props, Props->Children[0]}).Num(), 5);

	// Alt+click and Shift+click open or close a folder with everything in it.
	TArray<TSharedPtr<FGitPathItem>> Shown = Roots;
	const TSharedRef<STreeView<TSharedPtr<FGitPathItem>>> Tree = SNew(STreeView<TSharedPtr<FGitPathItem>>)
		.TreeItemsSource(&Shown)
		.OnGenerateRow_Lambda([](TSharedPtr<FGitPathItem>, const TSharedRef<STableViewBase>& Owner) { return SNew(STableRow<TSharedPtr<FGitPathItem>>, Owner); })
		.OnGetChildren_Lambda([](TSharedPtr<FGitPathItem> Item, TArray<TSharedPtr<FGitPathItem>>& Out) { Out = Item->Children; });
	GitFileTree::SetExpansionRecursive(*Tree, Roots[0], true);
	TestTrue(TEXT("Opening a folder that way opens every folder in it"), Tree->IsItemExpanded(Roots[0]) && Tree->IsItemExpanded(Actors) && Tree->IsItemExpanded(Props));
	GitFileTree::SetExpansionRecursive(*Tree, Roots[0], false);
	TestTrue(TEXT("Closing it that way closes them all"), !Tree->IsItemExpanded(Roots[0]) && !Tree->IsItemExpanded(Actors));
	// A rebuilt tree keeps the folders the user closed closed, except while searching.
	const TSet<FString> Collapsed = {Props->Path};
	GitFileTree::RestoreExpansion(*Tree, Roots, Collapsed, false);
	TestTrue(TEXT("Folders open unless the user closed them"), Tree->IsItemExpanded(Roots[0]) && Tree->IsItemExpanded(Actors) && !Tree->IsItemExpanded(Props));
	GitFileTree::RestoreExpansion(*Tree, Roots, Collapsed, true);
	TestTrue(TEXT("A search opens every folder"), Tree->IsItemExpanded(Props));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitPanelTextDiffTest, "GitWorkspace.Panel.TextDiff", PanelTestFlags)
bool FGitPanelTextDiffTest::RunTest(const FString&)
{
	using EKind = FGitDiffLine::EKind;
	const TArray<TSharedPtr<FGitDiffLine>> Lines = GitDiff::Parse(TEXT("diff --git a/Config/DefaultGame.ini b/Config/DefaultGame.ini\n")
		TEXT("index 1111111..2222222 100644\n--- a/Config/DefaultGame.ini\n+++ b/Config/DefaultGame.ini\n")
		TEXT("@@ -1,3 +1,4 @@ [Settings]\n [Settings]\n-ProjectName=Demo\n+ProjectName=Demo Two\n+CompanyName=Pippin\n \tIndented=1\n\\ No newline at end of file\n"));
	if (!TestEqual(TEXT("File headers are dropped; the hunk and its lines stay"), Lines.Num(), 7))
	{
		return false;
	}
	TestTrue(TEXT("The hunk header comes first"), Lines[0]->Kind == EKind::Hunk);
	TestTrue(TEXT("Context lines carry both line numbers"), Lines[1]->Kind == EKind::Context && Lines[1]->OldLine == 1 && Lines[1]->NewLine == 1);
	TestTrue(TEXT("Removed lines carry the old number"), Lines[2]->Kind == EKind::Removed && Lines[2]->OldLine == 2 && Lines[2]->NewLine == 0);
	TestTrue(TEXT("Added lines carry the new number"), Lines[3]->Kind == EKind::Added && Lines[3]->OldLine == 0 && Lines[3]->NewLine == 2);
	TestTrue(TEXT("An edited line marks the part that changed"), Lines[3]->HasChange() && Lines[3]->Text.Mid(Lines[3]->ChangeStart, Lines[3]->ChangeEnd - Lines[3]->ChangeStart) == TEXT(" Two"));
	TestFalse(TEXT("Nothing was taken out of the old line"), Lines[2]->HasChange());
	TestTrue(TEXT("An added line without a partner is not marked"), Lines[4]->Kind == EKind::Added && Lines[4]->NewLine == 3 && !Lines[4]->HasChange());
	TestTrue(TEXT("Tabs are expanded"), Lines[5]->Kind == EKind::Context && Lines[5]->Text == TEXT("    Indented=1") && Lines[5]->OldLine == 3 && Lines[5]->NewLine == 4);
	TestTrue(TEXT("Remarks are kept"), Lines[6]->Kind == EKind::Note && Lines[6]->Text == TEXT("No newline at end of file"));

	const TArray<TSharedPtr<FGitDiffLine>> Rewritten = GitDiff::Parse(TEXT("@@ -1 +1 @@\n-alpha\n+zzzzzzzz\n"));
	TestTrue(TEXT("A rewritten line is not marked part by part"), Rewritten.Num() == 3 && !Rewritten[1]->HasChange() && !Rewritten[2]->HasChange());
	const TArray<TSharedPtr<FGitDiffLine>> Pointer = GitDiff::Parse(TEXT("@@ -1,3 +1,3 @@\n version https://git-lfs.github.com/spec/v1\n-oid sha256:aaaa\n-size 10\n+oid sha256:bbbb\n+size 12\n"));
	TestTrue(TEXT("A Git LFS pointer change is one note"), Pointer.Num() == 1 && Pointer[0]->Kind == EKind::Note);
	const TArray<TSharedPtr<FGitDiffLine>> Binary = GitDiff::Parse(TEXT("diff --git a/x.png b/x.png\nindex 1111111..2222222 100644\nBinary files a/x.png and b/x.png differ\n"));
	TestTrue(TEXT("A binary file is one note"), Binary.Num() == 1 && Binary[0]->Kind == EKind::Note);
	TestEqual(TEXT("No output means no differences"), GitDiff::Parse(FString()).Num(), 0);
	const TArray<TSharedPtr<FGitDiffLine>> Merge = GitDiff::Parse(TEXT("@@@ -1,1 -1,1 +1,3 @@@\n  shared\n+ ours\n ++both\n"));
	TestTrue(TEXT("A merge commit's combined diff reads both marker columns"),
		Merge.Num() == 4 && Merge[1]->Kind == EKind::Context && Merge[2]->Kind == EKind::Added && Merge[2]->Text == TEXT("ours") && Merge[3]->Kind == EKind::Added && Merge[3]->NewLine == 3);

	const FTCHARToUTF8 Utf8(TEXT("one\ntwo\n"));
	const TArray<TSharedPtr<FGitDiffLine>> NewFile = GitDiff::Parse(GitDiff::FromNewFile(TArray<uint8>(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length())));
	TestTrue(TEXT("A new file is all added lines"), NewFile.Num() == 3 && NewFile[1]->Kind == EKind::Added && NewFile[1]->NewLine == 1 && NewFile[2]->Text == TEXT("two"));
	const TArray<TSharedPtr<FGitDiffLine>> NewBinary = GitDiff::Parse(GitDiff::FromNewFile({0x89, 'P', 'N', 'G', 0, 1}));
	TestTrue(TEXT("A new binary file is one note"), NewBinary.Num() == 1 && NewBinary[0]->Kind == EKind::Note);

	// Lines are shown as rich text, so code must not read as markup.
	FGitDiffLine Code;
	Code.Kind = EKind::Added;
	Code.Text = TEXT("if (A < B && C > \"D\")");
	Code.ChangeStart = 4;
	Code.ChangeEnd = 5;
	TestEqual(TEXT("Rich text escapes code and tags the changed part"), GitDiff::ToMarkup(Code), FString(TEXT("if (<add>A</> &lt; B &amp;&amp; C &gt; &quot;D&quot;)")));
	Code.Kind = EKind::Removed;
	TestEqual(TEXT("A removed line's change is tagged del"), GitDiff::ToMarkup(Code), FString(TEXT("if (<del>A</> &lt; B &amp;&amp; C &gt; &quot;D&quot;)")));
	Code.Kind = EKind::Context;
	TestEqual(TEXT("Context lines carry no tags"), GitDiff::ToMarkup(Code), FString(TEXT("if (A &lt; B &amp;&amp; C &gt; &quot;D&quot;)")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitPanelFileDiffsTest, "GitWorkspace.Panel.FileDiffs", PanelTestFlags)
bool FGitPanelFileDiffsTest::RunTest(const FString&)
{
	GitTest::FFixture Fixture;
	const FString Repo = Fixture.InitRepo();
	GitTest::WriteFile(Repo, TEXT("a.txt"), TEXT("one\ntwo\n"));
	const FString Base = GitTest::CommitAll(Repo, TEXT("base"));
	GitTest::WriteFile(Repo, TEXT("a.txt"), TEXT("one\ntwo too\nthree\n"));
	GitTest::WriteFile(Repo, TEXT("b.txt"), TEXT("staged\n"));
	GitTest::Git(Repo, {TEXT("add"), TEXT("b.txt")});
	GitTest::WriteFile(Repo, TEXT("new.txt"), TEXT("hello\n"));
	GitTest::FScopedNoBackgroundFetch NoFetch;
	FPanelFixture Panel(Repo);
	Panel.Refresh();
	Panel.WaitUntil([](const GitCore::FSessionSnapshot& Snapshot) { return Snapshot.Status.Find(TEXT("new.txt")) != nullptr; });

	auto Load = [this, &Panel](TFunction<void(TFunction<void(FString&&, const FString&)>)> Start)
	{
		bool bDone = false;
		FString Result;
		Start([&bDone, &Result](FString&& Diff, const FString& Error)
		{
			Result = Error.IsEmpty() ? MoveTemp(Diff) : TEXT("error: ") + Error;
			bDone = true;
		});
		TestTrue(TEXT("The diff loads"), GitTest::WaitFor([&Panel, &bDone]() { Panel.Provider->Tick(); return bDone; }));
		return GitDiff::Parse(Result);
	};
	auto Has = [](const TArray<TSharedPtr<FGitDiffLine>>& Lines, FGitDiffLine::EKind Kind, const TCHAR* Text)
	{
		return Lines.ContainsByPredicate([Kind, Text](const TSharedPtr<FGitDiffLine>& Line) { return Line->Kind == Kind && Line->Text == Text; });
	};
	const TArray<TSharedPtr<FGitDiffLine>> Edited = Load([&Panel](auto Done) { Panel.Model->LoadDiff(TEXT("a.txt"), false, MoveTemp(Done)); });
	TestTrue(TEXT("An edit shows the old and the new line"), Has(Edited, FGitDiffLine::EKind::Removed, TEXT("two")) && Has(Edited, FGitDiffLine::EKind::Added, TEXT("two too")));
	TestTrue(TEXT("Added lines show"), Has(Edited, FGitDiffLine::EKind::Added, TEXT("three")));
	const TArray<TSharedPtr<FGitDiffLine>> Staged = Load([&Panel](auto Done) { Panel.Model->LoadDiff(TEXT("b.txt"), true, MoveTemp(Done)); });
	TestTrue(TEXT("A staged new file shows its staged content"), Has(Staged, FGitDiffLine::EKind::Added, TEXT("staged")));
	const TArray<TSharedPtr<FGitDiffLine>> Untracked = Load([&Panel](auto Done) { Panel.Model->LoadDiff(TEXT("new.txt"), false, MoveTemp(Done)); });
	TestTrue(TEXT("A file Git does not track yet shows its content"), Has(Untracked, FGitDiffLine::EKind::Added, TEXT("hello")));
	const TArray<TSharedPtr<FGitDiffLine>> Committed = Load([&Panel, &Base](auto Done) { Panel.Model->LoadCommitDiff(Base, TEXT("a.txt"), MoveTemp(Done)); });
	TestTrue(TEXT("A commit shows how it changed the file"), Has(Committed, FGitDiffLine::EKind::Added, TEXT("one")) && Has(Committed, FGitDiffLine::EKind::Added, TEXT("two")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitPanelScreenshotsTest, "GitWorkspace.Panel.Screenshots", PanelTestFlags)
bool FGitPanelScreenshotsTest::RunTest(const FString&)
{
	// Layout review aid: needs an editor that renders (the headless suite skips it).
	if (!FApp::CanEverRender() || !FSlateApplication::IsInitialized())
	{
		AddInfo(TEXT("Screenshots need an editor that renders; skipped."));
		return true;
	}
	struct FState
	{
		GitTest::FLockedTeam Team;
		GitTest::FScopedNoBackgroundFetch NoFetch;
		TUniquePtr<FPanelFixture> Panel;
		TSharedPtr<SWindow> Window;
		TSharedPtr<SGitPanel> Widget;
		bool bWrapWas = false;
		FString Folder = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("Automation/GitWorkspace/Screenshots"));

		explicit FState(FAutomationTestBase& Test)
			: Team(Test)
		{
		}

		void Capture(const FString& Name) const
		{
			TArray<FColor> Pixels;
			FIntVector Size;
			if (Window.IsValid() && FSlateApplication::Get().TakeScreenshot(Window.ToSharedRef(), Pixels, Size))
			{
				for (FColor& Pixel : Pixels)
				{
					Pixel.A = 255;
				}
				FImageUtils::SaveImageAutoFormat(*(Folder / Name + TEXT(".png")), FImageView(Pixels.GetData(), Size.X, Size.Y));
			}
		}
	};
	const TSharedRef<FState> State = MakeShared<FState>(*this);
	if (!State->Team.IsReady())
	{
		return false;
	}
	const FString Alice = State->Team.Alice;
	const FString Bob = State->Team.Bob;
	// A teammate holds a lock and pushed a commit; Alice has a local commit, a stash and a mix of changes.
	if (const GitCore::FGitRepositoryPtr BobRepository = GitTest::OpenRepository(*this, Bob))
	{
		GitCore::FLockInfo Lock;
		BobRepository->Lock(TEXT("origin"), TEXT("Content/Map.umap"), Lock);
	}
	GitTest::WriteFile(Bob, TEXT("Docs/Notes.txt"), TEXT("notes\n"));
	GitTest::CommitAll(Bob, TEXT("Add level notes"));
	GitTest::Git(Bob, {TEXT("push"), TEXT("-q"), TEXT("origin"), TEXT("main")});
	GitTest::WriteFile(Alice, TEXT("Config/DefaultGame.ini"), TEXT("[/Script/EngineSettings.GeneralProjectSettings]\nProjectName=Demo\nProjectVersion=1.0\n"));
	GitTest::CommitAll(Alice, TEXT("Name the project"));
	GitTest::WriteFile(Alice, TEXT("readme.txt"), TEXT("lighting experiment\n"));
	GitTest::Git(Alice, {TEXT("stash"), TEXT("push"), TEXT("-m"), TEXT("Lighting experiment")});
	GitTest::WriteFile(Alice, TEXT("Config/DefaultGame.ini"), TEXT("[/Script/EngineSettings.GeneralProjectSettings]\nProjectName=Demo Two\nProjectVersion=1.1\nCompanyName=Pippin Games\n"));
	GitTest::WriteFile(Alice, TEXT("readme.txt"), TEXT("updated readme\n"));
	GitTest::Git(Alice, {TEXT("add"), TEXT("readme.txt")});
	GitTest::WriteFile(Alice, TEXT("Content/Hero.uasset"), TEXT("hero v2\n"));
	GitTest::WriteFile(Alice, TEXT("Content/Props/Barrel.uasset"), TEXT("barrel\n"));
	GitTest::WriteFile(Alice, TEXT("Content/__ExternalActors__/Maps/Main/A/7Q/0F2KD8XN3PZ4W1.uasset"), TEXT("actor\n"));
	GitTest::WriteFile(Alice, TEXT("Source/Demo/DemoCharacter.cpp"), TEXT("// edited\n"));

	State->Panel = MakeUnique<FPanelFixture>(Alice);
	FPanelFixture& Panel = *State->Panel;
	Panel.Model->Fetch();
	Panel.WaitIdle();
	Panel.Model->Lock({TEXT("Content/Hero.uasset")});
	Panel.WaitUntil([](const GitCore::FSessionSnapshot& Snapshot) { const GitCore::FLockInfo* Lock = Snapshot.Locks.Find(TEXT("Content/Hero.uasset")); return Lock && Lock->bOurs; });
	Panel.Provider->GetSession()->RequestStashesAndBranches();
	Panel.WaitUntil([](const GitCore::FSessionSnapshot& Snapshot) { return Snapshot.Stashes.Num() == 1 && Snapshot.Locks.Find(TEXT("Content/Map.umap")); });
	Panel.Refresh();

	State->Window = SNew(SWindow)
		.Title(FText::FromString(TEXT("Git panel layout")))
		.ClientSize(FVector2D(1180.0f, 680.0f));
	State->Window->SetContent(SAssignNew(State->Widget, SGitPanel, Panel.Model));
	FSlateApplication::Get().AddWindow(State->Window.ToSharedRef());
	State->Widget->ShowPaths({TEXT("Content/Hero.uasset")});

	ADD_LATENT_AUTOMATION_COMMAND(FDelayedFunctionLatentCommand([State]()
	{
		State->Capture(TEXT("changes-wide"));
		if (const TSharedPtr<SWidget> Search = FindTagged(State->Widget.ToSharedRef(), TEXT("GitPanel.Changes.Search")))
		{
			StaticCastSharedPtr<SSearchBox>(Search)->SetText(FText::FromString(TEXT("barrel")));
		}
	}, 1.5f));
	ADD_LATENT_AUTOMATION_COMMAND(FDelayedFunctionLatentCommand([State]()
	{
		State->Capture(TEXT("changes-search"));
		if (const TSharedPtr<SWidget> Search = FindTagged(State->Widget.ToSharedRef(), TEXT("GitPanel.Changes.Search")))
		{
			StaticCastSharedPtr<SSearchBox>(Search)->SetText(FText::GetEmpty());
		}
		State->Widget->ShowPaths({TEXT("Config/DefaultGame.ini")});
	}, 1.0f));
	ADD_LATENT_AUTOMATION_COMMAND(FDelayedFunctionLatentCommand([State]()
	{
		State->Capture(TEXT("changes-diff"));
		State->bWrapWas = GetDefault<UGitSourceControlUserSettings>()->bWrapDiffLines;
		GetMutableDefault<UGitSourceControlUserSettings>()->bWrapDiffLines = true;
	}, 1.5f));
	ADD_LATENT_AUTOMATION_COMMAND(FDelayedFunctionLatentCommand([State]()
	{
		State->Capture(TEXT("changes-diff-wrapped"));
		GetMutableDefault<UGitSourceControlUserSettings>()->bWrapDiffLines = State->bWrapWas;
	}, 1.0f));
	// Resize takes screen pixels; sizes here are Slate units.
	ADD_LATENT_AUTOMATION_COMMAND(FDelayedFunctionLatentCommand([State]() { State->Window->Resize(FVector2D(460.0f, 680.0f) * State->Window->GetDPIScaleFactor()); }, 0.1f));
	ADD_LATENT_AUTOMATION_COMMAND(FDelayedFunctionLatentCommand([State]() { State->Capture(TEXT("changes-narrow")); }, 1.0f));
	ADD_LATENT_AUTOMATION_COMMAND(FDelayedFunctionLatentCommand([State]() { State->Window->Resize(FVector2D(1180.0f, 680.0f) * State->Window->GetDPIScaleFactor()); State->Widget->ShowTab(1); }, 0.1f));
	ADD_LATENT_AUTOMATION_COMMAND(FDelayedFunctionLatentCommand([State]() { SelectFirst<SGitHistoryView::FCommitPtr>(State->Widget.ToSharedRef(), TEXT("GitPanel.History.Commits")); }, 2.0f));
	ADD_LATENT_AUTOMATION_COMMAND(FDelayedFunctionLatentCommand([State]()
	{
		// The commit's first file, inside its folders.
		if (const TSharedPtr<SWidget> Found = FindTagged(State->Widget.ToSharedRef(), TEXT("GitPanel.History.Files")))
		{
			const TSharedPtr<STreeView<SGitHistoryView::FFileItemPtr>> Tree = StaticCastSharedPtr<STreeView<SGitHistoryView::FFileItemPtr>>(Found);
			SGitHistoryView::FFileItemPtr Item = Tree->GetRootItems().IsEmpty() ? nullptr : Tree->GetRootItems()[0];
			while (Item.IsValid() && Item->bFolder && !Item->Children.IsEmpty())
			{
				Item = Item->Children[0];
			}
			if (Item.IsValid())
			{
				Tree->SetSelection(Item, ESelectInfo::OnMouseClick);
			}
		}
	}, 1.5f));
	ADD_LATENT_AUTOMATION_COMMAND(FDelayedFunctionLatentCommand([State]() { State->Capture(TEXT("history")); State->Widget->ShowTab(2); }, 1.5f));
	ADD_LATENT_AUTOMATION_COMMAND(FDelayedFunctionLatentCommand([State]() { SelectFirst<SGitStashesView::FStashPtr>(State->Widget.ToSharedRef(), TEXT("GitPanel.Stashes.List")); }, 1.0f));
	ADD_LATENT_AUTOMATION_COMMAND(FDelayedFunctionLatentCommand([State]() { State->Capture(TEXT("stashes")); State->Widget->ShowTab(3); }, 1.5f));
	ADD_LATENT_AUTOMATION_COMMAND(FDelayedFunctionLatentCommand([State]() { State->Capture(TEXT("locks")); State->Window->RequestDestroyWindow(); }, 1.0f));
	return true;
}

#endif
