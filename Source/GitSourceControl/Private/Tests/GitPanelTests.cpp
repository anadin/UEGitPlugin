// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#if WITH_DEV_AUTOMATION_TESTS

#include "GitTestFixtures.h"
#include "Editor/GitWorkspaceModel.h"
#include "Editor/Panel/GitFileTree.h"
#include "Editor/Panel/SGitDialogs.h"
#include "Editor/Panel/SGitChangesView.h"
#include "Editor/Panel/SGitDiffView.h"
#include "Editor/Panel/SGitHistoryView.h"
#include "Editor/Panel/SGitLocksView.h"
#include "Editor/Panel/SGitPanel.h"
#include "Editor/Panel/SGitStashesView.h"
#include "Provider/GitSourceControlUserSettings.h"

#include "AssetRegistry/IAssetRegistry.h"
#include "Curves/CurveFloat.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/PlatformApplicationMisc.h"
#include "HAL/PlatformFileManager.h"
#include "ImageCore.h"
#include "ImageUtils.h"
#include "Misc/App.h"
#include "Misc/ScopeExit.h"
#include "Types/ISlateMetaData.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Input/SSearchBox.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"

#include <initializer_list>

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

	/** The first text shown below Root. */
	FString FirstText(const TSharedRef<SWidget>& Root)
	{
		if (Root->GetType() == TEXT("STextBlock"))
		{
			return StaticCastSharedRef<STextBlock>(Root)->GetText().ToString();
		}
		FChildren* Children = Root->GetAllChildren();
		for (int32 Index = 0; Children && Index < Children->Num(); ++Index)
		{
			if (const FString Text = FirstText(Children->GetChildAt(Index)); !Text.IsEmpty())
			{
				return Text;
			}
		}
		return FString();
	}

	/** Every text shown below Root, such as a menu's labels and the shortcuts beside them. */
	TArray<FString> AllTexts(const TSharedRef<SWidget>& Root)
	{
		TArray<FString> Texts;
		if (Root->GetType() == TEXT("STextBlock"))
		{
			Texts.Add(StaticCastSharedRef<STextBlock>(Root)->GetText().ToString());
		}
		FChildren* Children = Root->GetAllChildren();
		for (int32 Index = 0; Children && Index < Children->Num(); ++Index)
		{
			Texts.Append(AllTexts(Children->GetChildAt(Index)));
		}
		return Texts;
	}

	/** What a button says, its bound label brought up to date as painting would. */
	FString ButtonLabel(const TSharedRef<SWidget>& Button)
	{
		Button->SlatePrepass(1.0f);
		return FirstText(Button);
	}

	/**
	 * Slate asks the platform which keys are held and which window is under the real pointer. While this
	 * lives, a stand-in answers: Option (Alt) held when asked for, and no window, so simulated pointer
	 * events find their window by position wherever the real pointer is.
	 */
	class FScopedStandInPlatform
	{
	public:
		explicit FScopedStandInPlatform(bool bOptionHeld = false)
			: Real(FSlateApplication::Get().GetPlatformApplication())
		{
			FSlateApplication::Get().OverridePlatformApplication(MakeShared<FStandIn>(Real->Cursor, bOptionHeld));
		}

		~FScopedStandInPlatform()
		{
			FSlateApplication::Get().OverridePlatformApplication(Real);
		}

	private:
		struct FStandIn : GenericApplication
		{
			FStandIn(const TSharedPtr<ICursor>& InCursor, bool bInOptionHeld)
				: GenericApplication(InCursor)
				, bOptionHeld(bInOptionHeld)
			{
			}

			virtual FModifierKeysState GetModifierKeys() const override
			{
				return FModifierKeysState(false, false, false, false, bOptionHeld, false, false, false, false);
			}

			bool bOptionHeld;
		};

		TSharedPtr<GenericApplication> Real;
	};

	/** One of the Changes tab's lists: "GitPanel.Changes.Unstaged", ".Staged" or ".Conflicts". */
	TSharedPtr<STreeView<SGitChangesView::FItemPtr>> ChangesList(const TSharedRef<SWidget>& Panel, FName Tag)
	{
		return StaticCastSharedPtr<STreeView<SGitChangesView::FItemPtr>>(FindTagged(Panel, Tag));
	}

	using FCommitList = SListView<SGitHistoryView::FCommitPtr>;

	/** The History tab's list of commits. */
	TSharedPtr<FCommitList> HistoryList(const TSharedRef<SWidget>& View)
	{
		return StaticCastSharedPtr<FCommitList>(FindTagged(View, TEXT("GitPanel.History.Commits")));
	}

	/** The listed commit with this id, or with this summary. */
	SGitHistoryView::FCommitPtr FindListed(const FCommitList& List, const FString& OidOrSubject)
	{
		const SGitHistoryView::FCommitPtr* Found = List.GetItems().FindByPredicate([&OidOrSubject](const SGitHistoryView::FCommitPtr& Commit)
		{
			return !Commit->Oid.IsEmpty() && (Commit->Oid.Equals(OidOrSubject, ESearchCase::CaseSensitive) || Commit->Subject.Equals(OidOrSubject, ESearchCase::CaseSensitive));
		});
		return Found ? *Found : nullptr;
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

	// Show in Git Panel replaces the selection instead of adding to it.
	Widget->ShowPaths({TEXT("a.txt")});
	Widget->ShowPaths({TEXT("Content/Hero.uasset")});
	const TSharedPtr<STreeView<SGitChangesView::FItemPtr>> ChangesTree = ChangesList(Widget, TEXT("GitPanel.Changes.Unstaged"));
	const TSharedPtr<STreeView<SGitChangesView::FItemPtr>> StagedTree = ChangesList(Widget, TEXT("GitPanel.Changes.Staged"));
	if (!TestTrue(TEXT("The Changes and Staged lists are tagged"), ChangesTree.IsValid() && StagedTree.IsValid()))
	{
		return false;
	}
	const TArray<SGitChangesView::FItemPtr> Shown = ChangesTree->GetSelectedItems();
	TestTrue(TEXT("Only the file shown last is selected"), Shown.Num() == 1 && Shown[0]->Path.Equals(TEXT("Content/Hero.uasset"), ESearchCase::CaseSensitive));

	Panel.Model->Stage({TEXT("a.txt")});
	TestTrue(TEXT("Staging shows up"), Panel.WaitUntil([](const GitCore::FSessionSnapshot& Snapshot) { return Snapshot.Status.StagedCount() == 1; }));
	TestEqualSensitive(TEXT("Only a.txt is staged"), PanelStaged(Repo), FString(TEXT("a.txt")));

	// The lists share one selection: picking in Staged clears Changes, so Space and the menus act on the new pick.
	Widget->ShowPaths({TEXT("Content/Hero.uasset")});
	if (TestEqual(TEXT("a.txt moved to the Staged list"), StagedTree->GetRootItems().Num(), 1))
	{
		StagedTree->SetSelection(StagedTree->GetRootItems()[0], ESelectInfo::OnMouseClick);
		TestEqual(TEXT("Picking a staged file clears the Changes selection"), ChangesTree->GetSelectedItems().Num(), 0);
		TestEqual(TEXT("And selects the staged file"), StagedTree->GetSelectedItems().Num(), 1);
	}

	bool bCommitted = false;
	Panel.Model->Commit(TEXT("Panel commit"), false, [&bCommitted](bool bDone) { bCommitted = bDone; });
	TestTrue(TEXT("The commit finishes"), Panel.WaitIdle());
	TestTrue(TEXT("The commit is reported"), bCommitted);
	TestEqualSensitive(TEXT("The staged file was committed"), PanelSubject(Repo), FString(TEXT("Panel commit")));
	TestTrue(TEXT("The unstaged asset stays modified"), Panel.Provider.Snapshot()->Status.Find(TEXT("Content/Hero.uasset")) != nullptr);
	TestFalse(TEXT("No failure is shown"), Panel.Model->GetLastFailure().IsSet());

	Panel.Model->Push();
	TestTrue(TEXT("The push finishes"), Panel.WaitIdle());
	TestEqualSensitive(TEXT("The server has the commit"), PanelHead(Origin, TEXT("main")), PanelHead(Repo));

	// The commit bar has one button: it commits, and pushes as well only while Option (Alt) is held over it.
	const TSharedPtr<SMultiLineEditableTextBox> MessageBox = StaticCastSharedPtr<SMultiLineEditableTextBox>(FindTagged(Widget, TEXT("GitPanel.Changes.Message")));
	const TSharedPtr<SButton> CommitButton = StaticCastSharedPtr<SButton>(FindTagged(Widget, TEXT("GitPanel.Changes.Commit")));
	if (!TestTrue(TEXT("The commit bar is tagged"), MessageBox.IsValid() && CommitButton.IsValid()))
	{
		return false;
	}
	Panel.Model->Stage({TEXT("Content/Hero.uasset")});
	Panel.WaitUntil([](const GitCore::FSessionSnapshot& Snapshot) { return Snapshot.Status.StagedCount() == 1; });
	MessageBox->SetText(FText::FromString(TEXT("Hero")));
	{
		const FScopedStandInPlatform OptionHeld(true);
		TestEqualSensitive(TEXT("Option elsewhere leaves Commit alone"), ButtonLabel(CommitButton.ToSharedRef()), FString(TEXT("Commit 1")));
	}
	CommitButton->OnMouseEnter(FGeometry(), FPointerEvent());
	TestEqualSensitive(TEXT("Commit counts the staged files"), ButtonLabel(CommitButton.ToSharedRef()), FString(TEXT("Commit 1")));
	CommitButton->SimulateClick();
	TestTrue(TEXT("The commit finishes"), Panel.WaitIdle());
	TestEqualSensitive(TEXT("Commit commits"), PanelSubject(Repo), FString(TEXT("Hero")));
	TestNotEqual(TEXT("Commit pushes nothing"), PanelHead(Origin, TEXT("main")), PanelHead(Repo));

	GitTest::WriteFile(Repo, TEXT("a.txt"), TEXT("a again\n"));
	Panel.Model->Stage({TEXT("a.txt")});
	Panel.WaitUntil([](const GitCore::FSessionSnapshot& Snapshot) { return Snapshot.Status.StagedCount() == 1; });
	MessageBox->SetText(FText::FromString(TEXT("Again")));
	{
		const FScopedStandInPlatform OptionHeld(true);
		TestEqualSensitive(TEXT("Option over Commit makes it Commit and push"), ButtonLabel(CommitButton.ToSharedRef()), FString(TEXT("Commit 1 and push")));
		CommitButton->SimulateClick();
	}
	TestTrue(TEXT("Commit and push finishes"), Panel.WaitIdle());
	TestEqualSensitive(TEXT("Commit and push commits"), PanelSubject(Repo), FString(TEXT("Again")));
	TestEqualSensitive(TEXT("Commit and push reaches the server"), PanelHead(Origin, TEXT("main")), PanelHead(Repo));
	CommitButton->OnMouseLeave(FPointerEvent());

	// The commit must contain what the user reviewed: staging behind the panel's back is refused.
	GitTest::WriteFile(Repo, TEXT("a.txt"), TEXT("a reviewed\n"));
	Panel.Model->Stage({TEXT("a.txt")});
	Panel.WaitUntil([](const GitCore::FSessionSnapshot& Snapshot) { return Snapshot.Status.StagedCount() == 1; });
	GitTest::WriteFile(Repo, TEXT("b.txt"), TEXT("sneaky\n"));
	GitTest::Git(Repo, {TEXT("add"), TEXT("b.txt")});
	const FString Before = PanelHead(Repo);
	Panel.Model->Commit(TEXT("Reviewed"), false);
	Panel.WaitIdle();
	TestEqualSensitive(TEXT("Nothing was committed"), PanelHead(Repo), Before);
	TestTrue(TEXT("The refusal is shown"), Panel.Model->GetLastFailure().IsSet());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitPanelCommitKeyTest, "GitWorkspace.Panel.CommitKeyClearsMessage", PanelTestFlags)
bool FGitPanelCommitKeyTest::RunTest(const FString&)
{
	GitTest::FFixture Fixture;
	const FString Origin = Fixture.InitBare();
	const FString Seed = Fixture.InitRepo(TEXT("seed"));
	GitTest::WriteFile(Seed, TEXT("a.txt"), TEXT("a\n"));
	GitTest::CommitAll(Seed, TEXT("base"));
	GitTest::Git(Seed, {TEXT("push"), TEXT("-q"), Origin, TEXT("main")});
	const FString Repo = Fixture.Clone(Origin, TEXT("work"));
	GitTest::FScopedNoBackgroundFetch NoFetch;
	FPanelFixture Panel(Repo);
	const TSharedRef<SGitChangesView> Changes = SNew(SGitChangesView, Panel.Model);
	const TSharedPtr<SMultiLineEditableTextBox> MessageBox = StaticCastSharedPtr<SMultiLineEditableTextBox>(FindTagged(Changes, TEXT("GitPanel.Changes.Message")));
	if (!TestTrue(TEXT("The message box is tagged"), MessageBox.IsValid()))
	{
		return false;
	}
	// Typing keeps the keyboard in the message box, which takes a window.
#if PLATFORM_MAC
	if (!FApp::CanEverRender())
	{
		// Without rendering a window has no native window, and macOS's input methods say so when a text box takes the keyboard.
		AddExpectedErrorPlain(TEXT("a context failed when its window couldn't be found"), EAutomationExpectedErrorFlags::Contains, 0);
	}
#endif
	const TSharedRef<SWindow> Window = SNew(SWindow).Title(FText::FromString(TEXT("Commit keys"))).ClientSize(FVector2D(900.0f, 600.0f));
	Window->SetContent(Changes);
	FSlateApplication::Get().AddWindow(Window);
	ON_SCOPE_EXIT
	{
		Window->RequestDestroyWindow();
	};
	// Commits Message with the keys; Meanwhile is typed while the commit runs.
	auto CommitWithKeys = [this, &Panel, &Repo, &MessageBox](const FString& Message, bool bPush, const FString& Meanwhile = FString())
	{
		GitTest::WriteFile(Repo, TEXT("a.txt"), Message + TEXT("\n"));
		Panel.Refresh();
		Panel.Model->Stage({TEXT("a.txt")});
		TestTrue(TEXT("The edit is staged"), Panel.WaitUntil([](const GitCore::FSessionSnapshot& Snapshot) { return Snapshot.Status.StagedCount() == 1; }));
		FSlateApplication::Get().SetKeyboardFocus(MessageBox, EFocusCause::SetDirectly);
		if (!TestTrue(TEXT("The message box has the keyboard"), MessageBox->HasKeyboardFocus()))
		{
			return;
		}
		MessageBox->SetText(FText::FromString(Message));
		// Ctrl+Enter (Cmd+Enter on macOS) commits, with Shift it pushes too.
		const FModifierKeysState Keys(bPush, false, true, false, false, false, false, false, false);
		FSlateApplication::Get().ProcessKeyDownEvent(FKeyEvent(EKeys::Enter, Keys, FSlateApplication::Get().GetUserIndexForKeyboard(), false, 0, 0));
		// The commit finishes on the game thread, which runs it only while waiting below.
		if (!Meanwhile.IsEmpty())
		{
			MessageBox->SetText(FText::FromString(Meanwhile));
		}
		TestTrue(TEXT("The commit finishes"), Panel.WaitIdle());
		TestEqualSensitive(TEXT("The commit has the message"), PanelSubject(Repo), Message);
		TestEqualSensitive(TEXT("The message box is empty again, or holds what was typed meanwhile"), MessageBox->GetPlainText().ToString(), Meanwhile);
		TestTrue(TEXT("It keeps the keyboard for the next message"), MessageBox->HasKeyboardFocus());
	};
	CommitWithKeys(TEXT("Committed with the keyboard"), false);
	CommitWithKeys(TEXT("Committed and pushed with the keyboard"), true);
	TestEqualSensitive(TEXT("Shift pushes as well"), PanelHead(Origin, TEXT("main")), PanelHead(Repo));
	CommitWithKeys(TEXT("Committed while typing on"), false, TEXT("The next message"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitPanelPublishTest, "GitWorkspace.Panel.PublishAsks", PanelTestFlags)
bool FGitPanelPublishTest::RunTest(const FString&)
{
	GitTest::FFixture Fixture;
	const FString Origin = Fixture.InitBare();
	const FString Seed = Fixture.InitRepo(TEXT("seed"));
	GitTest::WriteFile(Seed, TEXT("a.txt"), TEXT("a\n"));
	GitTest::CommitAll(Seed, TEXT("base"));
	GitTest::Git(Seed, {TEXT("push"), TEXT("-q"), Origin, TEXT("main")});
	const FString Repo = Fixture.Clone(Origin, TEXT("work"));
	const FString MainBefore = PanelHead(Origin, TEXT("main"));
	auto OnServer = [&Origin](const TCHAR* Branch) { return GitTest::Git(Origin, {TEXT("rev-parse"), TEXT("--verify"), TEXT("-q"), FString(TEXT("refs/heads/")) + Branch}).Succeeded(); };
	// A branch made from origin/main follows it.
	GitTest::Git(Repo, {TEXT("switch"), TEXT("-q"), TEXT("-c"), TEXT("feature"), TEXT("--track"), TEXT("origin/main")});
	GitTest::WriteFile(Repo, TEXT("a.txt"), TEXT("feature\n"));
	GitTest::CommitAll(Repo, TEXT("Feature work"));
	GitTest::FScopedNoBackgroundFetch NoFetch;
	FPanelFixture Panel(Repo);
	Panel.Refresh();
	TestTrue(TEXT("The snapshot knows the push needs a yes"), Panel.Provider.Snapshot()->Remote.Push.NeedsAgreement());

	{
		GitWorkspaceDialogs::FScopedConfirmAnswer No(false);
		Panel.Model->Push();
		Panel.WaitIdle();
		TestEqual(TEXT("Push asks once"), No.Questions.Num(), 1);
		TestTrue(TEXT("The question says where the commits go"),
			No.Questions.Num() == 1 && No.Questions[0].ToString().Contains(TEXT("origin/main"), ESearchCase::CaseSensitive) && No.Questions[0].ToString().Contains(TEXT("origin/feature"), ESearchCase::CaseSensitive));
	}
	TestEqualSensitive(TEXT("No leaves main on the server alone"), PanelHead(Origin, TEXT("main")), MainBefore);
	TestFalse(TEXT("No publishes nothing"), OnServer(TEXT("feature")));

	// Commit and push asks before committing.
	GitTest::WriteFile(Repo, TEXT("b.txt"), TEXT("b\n"));
	Panel.Model->Stage({TEXT("b.txt")});
	Panel.WaitUntil([](const GitCore::FSessionSnapshot& Snapshot) { return Snapshot.Status.StagedCount() == 1; });
	const FString HeadBefore = PanelHead(Repo);
	{
		GitWorkspaceDialogs::FScopedConfirmAnswer No(false);
		bool bCommitted = true;
		Panel.Model->Commit(TEXT("Not yet"), true, [&bCommitted](bool bDone) { bCommitted = bDone; });
		Panel.WaitIdle();
		TestEqual(TEXT("Commit and push asks"), No.Questions.Num(), 1);
		TestFalse(TEXT("No commits nothing"), bCommitted);
	}
	TestEqualSensitive(TEXT("HEAD did not move"), PanelHead(Repo), HeadBefore);
	{
		GitWorkspaceDialogs::FScopedConfirmAnswer Yes(true);
		Panel.Model->Commit(TEXT("Publish it"), true);
		Panel.WaitIdle();
		TestEqual(TEXT("It asks again"), Yes.Questions.Num(), 1);
	}
	TestEqualSensitive(TEXT("Yes publishes the branch under its own name"), PanelHead(Origin, TEXT("feature")), PanelHead(Repo));
	TestEqualSensitive(TEXT("main on the server still did not move"), PanelHead(Origin, TEXT("main")), MainBefore);
	TestEqualSensitive(TEXT("The branch follows origin/feature now"), Panel.Provider.Snapshot()->Status.Upstream, FString(TEXT("origin/feature")));

	// A branch that is not on the server: Publish is its own confirmation, other pushes ask.
	GitTest::Git(Repo, {TEXT("switch"), TEXT("-q"), TEXT("-c"), TEXT("spike")});
	Panel.Refresh();
	{
		GitWorkspaceDialogs::FScopedConfirmAnswer No(false);
		Panel.Model->Push();
		Panel.WaitIdle();
		TestEqual(TEXT("Push on a new branch asks"), No.Questions.Num(), 1);
		TestFalse(TEXT("And No keeps it local"), OnServer(TEXT("spike")));
		Panel.Model->Push(true);
		Panel.WaitIdle();
		TestEqual(TEXT("Publish does not ask"), No.Questions.Num(), 1);
	}
	TestTrue(TEXT("Publish published the branch"), OnServer(TEXT("spike")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitPanelPullChoicesTest, "GitWorkspace.Panel.PullChoices", PanelTestFlags)
bool FGitPanelPullChoicesTest::RunTest(const FString&)
{
	auto Branch = [](const TCHAR* Name, bool bRemote)
	{
		GitCore::FBranchInfo Info;
		Info.Name = Name;
		Info.bRemote = bRemote;
		return Info;
	};
	GitCore::FSessionSnapshot Snapshot;
	// A remote whose name holds a slash: its branches must not be taken for origin's.
	Snapshot.Remotes = {TEXT("origin"), TEXT("origin/mirror")};
	Snapshot.Branches = {Branch(TEXT("feature"), false), Branch(TEXT("origin/main"), true), Branch(TEXT("origin/feature"), true), Branch(TEXT("origin/mirror/main"), true)};
	Snapshot.Status.Branch = TEXT("feature");
	Snapshot.Status.Upstream = TEXT("origin/feature");
	UGitSourceControlUserSettings* User = GetMutableDefault<UGitSourceControlUserSettings>();
	const bool bRebaseWas = User->bPullRebase;
	User->bPullRebase = false;

	TMap<FString, TArray<FString>> Branches;
	GitCore::FPullOptions Options = FGitWorkspaceModel::PullChoices(Snapshot, Branches);
	TestTrue(TEXT("The upstream is picked"), Options.Remote.Equals(TEXT("origin"), ESearchCase::CaseSensitive) && Options.Branch.Equals(TEXT("feature"), ESearchCase::CaseSensitive));
	TestFalse(TEXT("The remembered choice is offered"), Options.bRebase);
	TestEqualSensitive(TEXT("origin lists its branches"), GitTest::Joined(Branches.FindRef(TEXT("origin"))), GitTest::Joined({TEXT("main"), TEXT("feature")}));
	TestEqualSensitive(TEXT("The mirror lists its own"), GitTest::Joined(Branches.FindRef(TEXT("origin/mirror"))), FString(TEXT("main")));

	// Without an upstream: the branch's own name on origin when it is there, else main.
	Snapshot.Status.Upstream.Reset();
	Snapshot.Status.Branch = TEXT("spike");
	Options = FGitWorkspaceModel::PullChoices(Snapshot, Branches);
	TestTrue(TEXT("A new branch is offered origin's main"), Options.Remote.Equals(TEXT("origin"), ESearchCase::CaseSensitive) && Options.Branch.Equals(TEXT("main"), ESearchCase::CaseSensitive));
	User->bPullRebase = bRebaseWas;
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitPanelStageKeyTest, "GitWorkspace.Panel.StageKey", PanelTestFlags)
bool FGitPanelStageKeyTest::RunTest(const FString&)
{
	GitTest::FFixture Fixture;
	const FString Repo = Fixture.InitRepo();
	GitTest::WriteFile(Repo, TEXT("a.txt"), TEXT("a\n"));
	GitTest::CommitAll(Repo, TEXT("base"));
	GitTest::WriteFile(Repo, TEXT("a.txt"), TEXT("a changed\n"));
	GitTest::FScopedNoBackgroundFetch NoFetch;
	FPanelFixture Panel(Repo);
	Panel.Refresh();
	const TSharedRef<SGitChangesView> View = SNew(SGitChangesView, Panel.Model);
	const TSharedPtr<STreeView<SGitChangesView::FItemPtr>> StagedTree = ChangesList(View, TEXT("GitPanel.Changes.Staged"));
	if (!TestTrue(TEXT("The Staged list is tagged"), StagedTree.IsValid()))
	{
		return false;
	}
	// Slate on macOS reports Command as Control, so this is Cmd+S there, and tooltips say so.
	TestEqualSensitive(TEXT("Tooltips name the shortcut as this platform does"), GitWorkspaceText::Shortcut(EModifierKey::Control, EKeys::S).ToString(),
		FString(PLATFORM_MAC ? TEXT("Cmd+S") : TEXT("Ctrl+S")));
	TestEqualSensitive(TEXT("and Alt+click too"), GitWorkspaceText::AltClick().ToString(), FString(PLATFORM_MAC ? TEXT("Option+click") : TEXT("Alt+click")));
	auto PressS = [&View](bool bShift)
	{
		const FModifierKeysState Keys(bShift, false, true, false, false, false, false, false, false);
		return View->OnKeyDown(FGeometry(), FKeyEvent(EKeys::S, Keys, 0, false, 0, 0)).IsEventHandled();
	};

	View->SelectPaths({TEXT("a.txt")});
	TestFalse(TEXT("Ctrl+Shift+S stays the editor's Save All"), PressS(true));
	TestTrue(TEXT("Ctrl+S is taken"), PressS(false));
	TestTrue(TEXT("Ctrl+S stages the selected file"), Panel.WaitUntil([](const GitCore::FSessionSnapshot& Snapshot) { return Snapshot.Status.StagedCount() == 1; }));
	TestEqualSensitive(TEXT("Only a.txt is staged"), PanelStaged(Repo), FString(TEXT("a.txt")));

	View->SelectPaths({TEXT("a.txt")});
	TestEqual(TEXT("The staged file is selected"), StagedTree->GetSelectedItems().Num(), 1);
	TestTrue(TEXT("Ctrl+S is taken again"), PressS(false));
	TestTrue(TEXT("Ctrl+S unstages it"), Panel.WaitUntil([](const GitCore::FSessionSnapshot& Snapshot) { return Snapshot.Status.StagedCount() == 0; }));
	TestEqualSensitive(TEXT("Nothing is staged"), PanelStaged(Repo), FString());
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
	TestEqualSensitive(TEXT("The new branch is current"), Panel.Provider.Snapshot()->Status.Branch, FString(TEXT("feature")));
	GitTest::WriteFile(Repo, TEXT("a.txt"), TEXT("feature\n"));
	GitTest::CommitAll(Repo, TEXT("Feature work"));

	// Local edits to a file that differs on the target branch stop the switch before anything changes.
	GitTest::WriteFile(Repo, TEXT("a.txt"), TEXT("uncommitted\n"));
	Panel.Model->SwitchBranch(TEXT("main"));
	Panel.WaitIdle();
	TestEqualSensitive(TEXT("Still on feature"), GitTest::Output(GitTest::Git(Repo, {TEXT("branch"), TEXT("--show-current")})), FString(TEXT("feature")));
	TestEqualSensitive(TEXT("The local edit is untouched"), GitTest::ReadFile(Repo, TEXT("a.txt")), FString(TEXT("uncommitted\n")));

	GitTest::Git(Repo, {TEXT("checkout"), TEXT("--"), TEXT("a.txt")});
	Panel.Model->SwitchBranch(TEXT("main"));
	TestTrue(TEXT("Switching finishes"), Panel.WaitIdle());
	TestEqualSensitive(TEXT("Now on main"), GitTest::Output(GitTest::Git(Repo, {TEXT("branch"), TEXT("--show-current")})), FString(TEXT("main")));
	TestEqualSensitive(TEXT("Files match main"), GitTest::ReadFile(Repo, TEXT("a.txt")), FString(TEXT("base\n")));

	// Stash everything (new files too), then bring it back.
	GitTest::WriteFile(Repo, TEXT("a.txt"), TEXT("work in progress\n"));
	GitTest::WriteFile(Repo, TEXT("new.txt"), TEXT("new\n"));
	Panel.Refresh();
	Panel.Model->StashChanges(TEXT("wip"), true);
	TestTrue(TEXT("Stashing finishes"), Panel.WaitIdle());
	TestEqualSensitive(TEXT("The edit was put aside"), GitTest::ReadFile(Repo, TEXT("a.txt")), FString(TEXT("base\n")));
	TestFalse(TEXT("The new file was put aside"), GitTest::FileExists(Repo, TEXT("new.txt")));
	TestEqual(TEXT("One stash is listed"), Panel.Provider.Snapshot()->Stashes.Num(), 1);

	Panel.Model->ApplyStash(TEXT("stash@{0}"), true);
	TestTrue(TEXT("Restoring finishes"), Panel.WaitIdle());
	TestEqualSensitive(TEXT("The edit is back"), GitTest::ReadFile(Repo, TEXT("a.txt")), FString(TEXT("work in progress\n")));
	TestTrue(TEXT("The new file is back"), GitTest::FileExists(Repo, TEXT("new.txt")));
	TestEqual(TEXT("The stash was removed"), Panel.Provider.Snapshot()->Stashes.Num(), 0);

	Panel.Model->StashChanges(TEXT("throwaway"), false);
	Panel.WaitIdle();
	Panel.Model->DropStash(TEXT("stash@{0}"));
	TestTrue(TEXT("Deleting finishes"), Panel.WaitIdle());
	TestEqual(TEXT("No stash is left"), Panel.Provider.Snapshot()->Stashes.Num(), 0);
	TestEqualSensitive(TEXT("The tracked edit is gone"), GitTest::ReadFile(Repo, TEXT("a.txt")), FString(TEXT("base\n")));
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
	TestTrue(TEXT("Locking finishes"), Panel.WaitIdle());
	TestTrue(TEXT("Locking goes through Check Out"), Panel.WaitUntil([](const GitCore::FSessionSnapshot& Snapshot)
	{
		const GitCore::FLockInfo* Lock = Snapshot.Locks.Find(TEXT("Content/Hero.uasset"));
		return Lock && Lock->bOurs;
	}));

	// A lock you took in another clone: the server says it is yours, but this clone has no record of taking it.
	const FString HeroFile = Team.Alice / TEXT("Content/Hero.uasset");
	auto IsElsewhere = [&Panel]()
	{
		const GitCore::FSessionSnapshotRef Snapshot = Panel.Provider.Snapshot();
		const GitCore::FLockInfo* Lock = Snapshot->Locks.Find(TEXT("Content/Hero.uasset"));
		return Lock && Snapshot->IsOursElsewhere(*Lock);
	};
	{
		const GitCore::FSessionSnapshotRef Snapshot = Panel.Provider.Snapshot();
		if (const GitCore::FLockInfo* Taken = Snapshot->Locks.Find(TEXT("Content/Hero.uasset")))
		{
			Panel.Provider->GetSession()->GetJournal().Remove(Snapshot->Locks.Endpoint, Taken->Id);
		}
	}
	Panel.Model->CheckLocks();
	TestTrue(TEXT("Checking locks finishes"), Panel.WaitIdle());
	TestTrue(TEXT("The lock shows as yours from elsewhere"), IsElsewhere());
	FPlatformFileManager::Get().GetPlatformFile().SetReadOnly(*HeroFile, true);
	Panel.Model->ClaimLocks({TEXT("Content/Hero.uasset"), TEXT("readme.txt")});
	TestTrue(TEXT("Claiming finishes"), Panel.WaitIdle());
	TestFalse(TEXT("The claim succeeded"), Panel.Model->GetLastFailure().IsSet());
	TestFalse(TEXT("The claimed lock is used here"), IsElsewhere());
	TestFalse(TEXT("The claimed file is writable"), FPlatformFileManager::Get().GetPlatformFile().IsReadOnly(*HeroFile));
	Panel.Model->CheckLocks();
	TestTrue(TEXT("Checking locks again finishes"), Panel.WaitIdle());
	TestFalse(TEXT("The claim survives a lock check"), IsElsewhere());
	TArray<FSourceControlStateRef> States;
	Panel.Provider->GetState({HeroFile}, States, EStateCacheUsage::Use);
	TestTrue(TEXT("The editor shows the claimed file as checked out"), States.Num() == 1 && States[0]->IsCheckedOut());
	AddExpectedErrorPlain(TEXT("None of these locks is yours from elsewhere"), EAutomationExpectedErrorFlags::Contains, 1);
	Panel.Model->ClaimLocks({TEXT("Content/Hero.uasset")});
	TestTrue(TEXT("Claiming again finishes"), Panel.WaitIdle());
	TestTrue(TEXT("Nothing is left to claim"), Panel.Model->GetLastFailure().IsSet());
	Panel.Model->DismissFailure();

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
	TestEqualSensitive(TEXT("The edit is gone"), GitTest::ReadFile(Team.Alice, TEXT("readme.txt")), FString(TEXT("text\n")));
	TestFalse(TEXT("The new file is deleted"), GitTest::FileExists(Team.Alice, TEXT("scratch.txt")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitPanelLockProgressTest, "GitWorkspace.Panel.LockProgress", PanelTestFlags)
bool FGitPanelLockProgressTest::RunTest(const FString&)
{
	GitTest::FLockedTeam Team(*this);
	if (!Team.IsReady())
	{
		return false;
	}
	TArray<FString> Props;
	for (int32 Index = 1; Index <= 4; ++Index)
	{
		Props.Add(FString::Printf(TEXT("Content/Props/Prop%d.uasset"), Index));
		GitTest::WriteFile(Team.Alice, Props.Last(), Props.Last() + TEXT("\n"));
	}
	GitTest::CommitAll(Team.Alice, TEXT("Props"));
	GitTest::FScopedNoBackgroundFetch NoFetch;
	FPanelFixture Panel(Team.Alice);
	Panel.Refresh();
	TestTrue(TEXT("Locks are read after connecting"), Panel.WaitUntil([](const GitCore::FSessionSnapshot& Snapshot) { return Snapshot.Locks.bVerified; }));
	// Each lock taken or released takes a moment, as with a distant server.
	FFileHelper::SaveStringToFile(TEXT("0.25"), *(Team.Server->Root / TEXT("lock-delay")));
	auto Mine = [&Panel]()
	{
		int32 Count = 0;
		for (const TPair<FString, GitCore::FLockInfo>& Pair : Panel.Provider.Snapshot()->Locks.ByPath)
		{
			Count += Pair.Value.bOurs ? 1 : 0;
		}
		return Count;
	};
	// What the toolbar says while the action runs.
	auto Watch = [&Panel](TArray<FString>& OutSeen, TFunction<bool()> Until)
	{
		return GitTest::WaitFor([&Panel, &OutSeen, &Until]()
		{
			Panel.Provider->Tick();
			if (Panel.Model->IsBusy())
			{
				OutSeen.AddUnique(Panel.Model->GetActivity().ToString());
			}
			return Until();
		});
	};

	TArray<FString> Seen;
	Panel.Model->Lock(Props);
	TestTrue(TEXT("Locking is the running action"), Panel.Model->IsBusy());
	TestTrue(TEXT("Locking finishes"), Watch(Seen, [&Panel]() { return !Panel.Model->IsBusy(); }));
	TestTrue(TEXT("The toolbar counts the files: ") + GitTest::Joined(Seen), GitTest::HasExactly(Seen, TEXT("Locking files: 2 of 4")));
	TestEqual(TEXT("All four are locked"), Mine(), 4);

	Seen.Reset();
	Panel.Model->Unlock(Props, false);
	TestTrue(TEXT("Releasing finishes"), Watch(Seen, [&Panel]() { return !Panel.Model->IsBusy(); }));
	TestTrue(TEXT("The toolbar counts the locks: ") + GitTest::Joined(Seen), GitTest::HasExactly(Seen, TEXT("Releasing locks: 2 of 4")));
	TestEqual(TEXT("All four are released"), Mine(), 0);

	// Cancel stops locking after the file being locked.
	Seen.Reset();
	Panel.Model->Lock(Props);
	TestTrue(TEXT("The first file is locked"), Watch(Seen, [&Panel]() { return Panel.Model->GetActivity().ToString().Contains(TEXT("1 of 4"), ESearchCase::CaseSensitive); }));
	Panel.Model->Cancel();
	TestTrue(TEXT("The cancelled locking finishes"), Panel.WaitIdle());
	TestTrue(FString::Printf(TEXT("It stopped early (%d locked)"), Mine()), Mine() >= 1 && Mine() < 4);
	TestFalse(TEXT("Stopping is not a failure"), Panel.Model->GetLastFailure().IsSet());

	// And releasing.
	Panel.Model->Lock(Props);
	Panel.WaitIdle();
	Seen.Reset();
	Panel.Model->Unlock(Props, false);
	TestTrue(TEXT("The first lock is released"), Watch(Seen, [&Panel]() { return Panel.Model->GetActivity().ToString().Contains(TEXT("1 of 4"), ESearchCase::CaseSensitive); }));
	Panel.Model->Cancel();
	TestTrue(TEXT("The cancelled release finishes"), Panel.WaitIdle());
	TestTrue(FString::Printf(TEXT("It stopped early (%d still locked)"), Mine()), Mine() >= 1 && Mine() < 4);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitPanelMenuShortcutsTest, "GitWorkspace.Panel.MenuShortcuts", PanelTestFlags)
bool FGitPanelMenuShortcutsTest::RunTest(const FString&)
{
	GitTest::FFixture Fixture;
	const FString Repo = Fixture.InitRepo();
	GitTest::WriteFile(Repo, TEXT("a.txt"), TEXT("a\n"));
	GitTest::WriteFile(Repo, TEXT("Docs/b.txt"), TEXT("b\n"));
	GitTest::CommitAll(Repo, TEXT("base"));
	GitTest::WriteFile(Repo, TEXT("Docs/b.txt"), TEXT("b put aside\n"));
	GitTest::Git(Repo, {TEXT("stash"), TEXT("push"), TEXT("-q"), TEXT("-m"), TEXT("Put aside")});
	GitTest::WriteFile(Repo, TEXT("a.txt"), TEXT("a changed\n"));
	GitTest::FScopedNoBackgroundFetch NoFetch;
	FPanelFixture Panel(Repo);
	Panel.Refresh();
	// The clipboard is the user's: put it back afterwards.
	FString ClipboardWas;
	FPlatformApplicationMisc::ClipboardPaste(ClipboardWas);
	ON_SCOPE_EXIT
	{
		FPlatformApplicationMisc::ClipboardCopy(*ClipboardWas);
	};
	auto Copied = []()
	{
		FString Text;
		FPlatformApplicationMisc::ClipboardPaste(Text);
		return Text;
	};
	auto Press = [](const TSharedRef<SWidget>& Widget, FKey Key, bool bControl = false)
	{
		const FModifierKeysState Keys(false, false, bControl, false, false, false, false, false, false);
		return Widget->OnKeyDown(FGeometry(), FKeyEvent(Key, Keys, 0, false, 0, 0)).IsEventHandled();
	};
	auto Texts = [](const TSharedPtr<SWidget>& Menu) { return Menu.IsValid() ? AllTexts(Menu.ToSharedRef()) : TArray<FString>(); };
	// Unreal's menus show shortcuts in capitals.
	auto Shown = [](const FText& Keys) { return Keys.ToString().ToUpper(); };
	const FString Space = Shown(GitWorkspaceText::SpaceKey());
	const FString Delete = Shown(GitWorkspaceText::DeleteKey());
	const FString CopyKeys = Shown(GitWorkspaceText::Shortcut(EModifierKey::Control, EKeys::C));

	// Menus show each entry's shortcut beside it, as Unreal's own menus do.
	const TSharedRef<SGitChangesView> Changes = SNew(SGitChangesView, Panel.Model);
	Changes->SelectPaths({TEXT("a.txt")});
	const TArray<FString> ChangesMenu = Texts(Changes->OnContextMenu());
	TestTrue(TEXT("Stage shows Space: ") + GitTest::Joined(ChangesMenu), GitTest::HasExactly(ChangesMenu, Space));
	TestTrue(TEXT("Discard shows Delete"), GitTest::HasExactly(ChangesMenu, Delete));
	TestTrue(TEXT("Compare shows its shortcut"), GitTest::HasExactly(ChangesMenu, Shown(GitWorkspaceText::Shortcut(EModifierKey::Control, EKeys::D))));
	TestTrue(TEXT("Copy path shows its shortcut"), GitTest::HasExactly(ChangesMenu, CopyKeys));
	TestTrue(TEXT("Ctrl+C is taken in the Changes tab"), Press(Changes, EKeys::C, true));
	TestEqualSensitive(TEXT("It copies the selected path"), Copied(), FString(TEXT("a.txt")));

	const TSharedRef<SGitDiffView> Diff = SNew(SGitDiffView).OnLineAction_Lambda([](GitCore::ELineAction, const GitCore::FLineSelection&) { return true; });
	Diff->SetLineActions({GitCore::ELineAction::Stage, GitCore::ELineAction::Discard});
	Diff->SetDiff(TEXT("diff --git a/a.txt b/a.txt\n--- a/a.txt\n+++ b/a.txt\n@@ -1 +1 @@\n-a\n+a changed\n"));
	const TArray<FString> DiffMenu = Texts(Diff->OnContextMenu());
	TestTrue(TEXT("Stage lines shows Space: ") + GitTest::Joined(DiffMenu), GitTest::HasExactly(DiffMenu, Space));
	TestTrue(TEXT("Discard lines shows Delete"), GitTest::HasExactly(DiffMenu, Delete));
	TestTrue(TEXT("Copy shows its shortcut"), GitTest::HasExactly(DiffMenu, CopyKeys));
	TestTrue(TEXT("Select all shows its shortcut"), GitTest::HasExactly(DiffMenu, Shown(GitWorkspaceText::Shortcut(EModifierKey::Control, EKeys::A))));

	// History: Ctrl+C copies the selected commit's id.
	const TSharedRef<SGitHistoryView> History = SNew(SGitHistoryView, Panel.Model);
	History->RefreshIfNeeded();
	const TSharedPtr<FCommitList> Commits = HistoryList(History);
	const FString Head = PanelHead(Repo);
	if (!TestTrue(TEXT("History lists the commit"), Commits.IsValid() && GitTest::WaitFor([&Commits, &Head]() { return FindListed(*Commits, Head).IsValid(); })))
	{
		return false;
	}
	Commits->SetSelection(FindListed(*Commits, Head), ESelectInfo::OnMouseClick);
	TestTrue(TEXT("Ctrl+C is taken in the commit list"), Press(Commits.ToSharedRef(), EKeys::C, true));
	TestEqualSensitive(TEXT("It copies the commit id"), Copied(), Head);

	// Stashes: Delete deletes the selected stash, after asking.
	Panel.Provider->GetSession()->RequestStashesAndBranches();
	TestTrue(TEXT("The stash is read"), Panel.WaitUntil([](const GitCore::FSessionSnapshot& Snapshot) { return Snapshot.Stashes.Num() == 1; }));
	const TSharedRef<SGitStashesView> Stashes = SNew(SGitStashesView, Panel.Model);
	const TSharedPtr<SListView<SGitStashesView::FStashPtr>> StashList = StaticCastSharedPtr<SListView<SGitStashesView::FStashPtr>>(FindTagged(Stashes, TEXT("GitPanel.Stashes.List")));
	if (!TestTrue(TEXT("The stash is listed"), StashList.IsValid() && StashList->GetItems().Num() == 1))
	{
		return false;
	}
	StashList->SetSelection(StashList->GetItems()[0], ESelectInfo::OnMouseClick);
	{
		GitWorkspaceDialogs::FScopedConfirmAnswer No(false);
		TestTrue(TEXT("Delete is taken in the stash list"), Press(StashList.ToSharedRef(), EKeys::Delete));
		TestEqual(TEXT("Delete asks first"), No.Questions.Num(), 1);
	}
	Panel.WaitIdle();
	TestEqualSensitive(TEXT("No keeps the stash"), GitTest::Output(GitTest::Git(Repo, {TEXT("stash"), TEXT("list"), TEXT("--format=%s")})), FString(TEXT("On main: Put aside")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitPanelPushAfterMergeTest, "GitWorkspace.Panel.PushAfterMergingLockedWork", PanelTestFlags)
bool FGitPanelPushAfterMergeTest::RunTest(const FString&)
{
	GitTest::FLockedTeam Team(*this);
	if (!Team.IsReady())
	{
		return false;
	}
	AddExpectedErrorPlain(TEXT("is locked by someone else"), EAutomationExpectedErrorFlags::Contains, 1);
	GitTest::Git(Team.Bob, {TEXT("switch"), TEXT("-q"), TEXT("-c"), TEXT("feature")});
	GitTest::WriteFile(Team.Bob, TEXT("readme.txt"), TEXT("bob's readme\n"));
	GitTest::CommitAll(Team.Bob, TEXT("Bob's readme"));
	GitTest::Git(Team.Bob, {TEXT("push"), TEXT("-q"), TEXT("-u"), TEXT("origin"), TEXT("feature")});

	// Alice changes the hero on main and keeps it locked.
	const GitCore::FGitRepositoryPtr Alice = GitTest::OpenRepository(*this, Team.Alice);
	GitCore::FLockInfo Lock;
	if (!Alice.IsValid() || !TestTrue(TEXT("Alice locks the hero"), Alice->Lock(TEXT("origin"), TEXT("Content/Hero.uasset"), Lock).bOk))
	{
		return false;
	}
	GitTest::WriteFile(Team.Alice, TEXT("Content/Hero.uasset"), TEXT("hero v2\n"));
	GitTest::CommitAll(Team.Alice, TEXT("Hero v2"));
	GitTest::Git(Team.Alice, {TEXT("push"), TEXT("-q"), TEXT("origin"), TEXT("main")});

	// Bob merges main into his branch, which brings Alice's locked hero along.
	GitTest::Git(Team.Bob, {TEXT("fetch"), TEXT("-q"), TEXT("origin")});
	GitTest::Git(Team.Bob, {TEXT("merge"), TEXT("-q"), TEXT("--no-edit"), TEXT("origin/main")});
	TestEqualSensitive(TEXT("The merge brought the hero in"), GitTest::ReadFile(Team.Bob, TEXT("Content/Hero.uasset")), FString(TEXT("hero v2\n")));
	GitTest::FScopedNoBackgroundFetch NoFetch;
	FPanelFixture Panel(Team.Bob);
	Panel.Refresh();
	Panel.Model->Push();
	TestTrue(TEXT("The push finishes"), Panel.WaitIdle());
	TestFalse(TEXT("Alice's lock on work already on the server does not stop it: ") + (Panel.Model->GetLastFailure().IsSet() ? Panel.Model->GetLastFailure()->Message.ToString() : FString()),
		Panel.Model->GetLastFailure().IsSet());
	TestEqualSensitive(TEXT("The merge reached the server"), PanelHead(Team.Origin, TEXT("feature")), PanelHead(Team.Bob));

	// Changing the locked hero himself is still refused.
	GitTest::WriteFile(Team.Bob, TEXT("Content/Hero.uasset"), TEXT("bob's hero\n"));
	GitTest::CommitAll(Team.Bob, TEXT("Bob edits the hero"));
	const FString ServerBefore = PanelHead(Team.Origin, TEXT("feature"));
	Panel.Refresh();
	Panel.Model->Push();
	TestTrue(TEXT("The second push finishes"), Panel.WaitIdle());
	TestTrue(TEXT("Bob's own change to the locked hero is refused"), Panel.Model->GetLastFailure().IsSet() &&
		Panel.Model->GetLastFailure()->Message.ToString().Contains(TEXT("Hero.uasset is locked by someone else"), ESearchCase::CaseSensitive));
	TestEqualSensitive(TEXT("Nothing reached the server"), PanelHead(Team.Origin, TEXT("feature")), ServerBefore);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitPanelLeanBranchingTest, "GitWorkspace.Panel.LeanBranching", PanelTestFlags)
bool FGitPanelLeanBranchingTest::RunTest(const FString&)
{
	GitTest::FFixture Fixture;
	const FString Origin = Fixture.InitBare();
	const FString Seed = Fixture.InitRepo(TEXT("seed"));
	GitTest::WriteFile(Seed, TEXT("a.txt"), TEXT("a\n"));
	GitTest::WriteFile(Seed, TEXT("notes.txt"), TEXT("notes\n"));
	GitTest::CommitAll(Seed, TEXT("base"));
	GitTest::Git(Seed, {TEXT("push"), TEXT("-q"), Origin, TEXT("main")});
	const FString Alice = Fixture.Clone(Origin, TEXT("alice"), TEXT("Alice"));
	const FString Bob = Fixture.Clone(Origin, TEXT("bob"), TEXT("Bob"));
	const FString SeedCommit = PanelHead(Bob);
	auto AlicePushes = [&Alice](const FString& Content)
	{
		GitTest::WriteFile(Alice, TEXT("alice.txt"), Content);
		GitTest::CommitAll(Alice, TEXT("Alice: ") + Content);
		GitTest::Git(Alice, {TEXT("push"), TEXT("-q"), TEXT("origin"), TEXT("main")});
	};
	// Bob's commits leave his local edit to notes.txt alone.
	auto BobCommits = [&Bob](const FString& Path)
	{
		GitTest::WriteFile(Bob, Path, Path + TEXT("\n"));
		GitTest::Git(Bob, {TEXT("add"), TEXT("--"), Path});
		GitTest::Git(Bob, {TEXT("commit"), TEXT("-q"), TEXT("--no-verify"), TEXT("-m"), Path, TEXT("--"), Path});
	};
	auto Count = [&Bob](const FString& Range) { return GitTest::Output(GitTest::Git(Bob, {TEXT("rev-list"), TEXT("--count"), Range})); };
	auto Current = [&Bob]() { return GitTest::Output(GitTest::Git(Bob, {TEXT("branch"), TEXT("--show-current")})); };
	AddExpectedErrorPlain(TEXT("Pull them first, then sync"), EAutomationExpectedErrorFlags::Contains, 1);
	AddExpectedErrorPlain(TEXT("Sync, then finish"), EAutomationExpectedErrorFlags::Contains, 1);
	AddExpectedErrorPlain(TEXT("that are not on origin/main"), EAutomationExpectedErrorFlags::Contains, 1);
	GitTest::FScopedNoBackgroundFetch NoFetch;
	FPanelFixture Panel(Bob);
	Panel.Refresh();
	TestTrue(TEXT("Branches are read after connecting"), Panel.WaitUntil([](const GitCore::FSessionSnapshot& Snapshot)
	{
		return Snapshot.Branches.ContainsByPredicate([](const GitCore::FBranchInfo& Branch) { return Branch.bRemote && Branch.Name.Equals(TEXT("origin/main"), ESearchCase::CaseSensitive); });
	}));
	FString Main;
	FString Upstream;
	TestTrue(TEXT("main is the main branch"), FGitWorkspaceModel::FindMainBranch(*Panel.Provider.Snapshot(), Main, Upstream) &&
		Main.Equals(TEXT("main"), ESearchCase::CaseSensitive) && Upstream.Equals(TEXT("origin/main"), ESearchCase::CaseSensitive));

	// Start: from origin/main, which is ahead of Bob's main. A local edit comes along.
	AlicePushes(TEXT("alice 1\n"));
	GitTest::Git(Bob, {TEXT("fetch"), TEXT("-q"), TEXT("origin")});
	GitTest::WriteFile(Bob, TEXT("notes.txt"), TEXT("bob's notes\n"));
	Panel.Refresh();
	Panel.Model->StartBranch(TEXT("feature"));
	TestTrue(TEXT("Starting finishes"), Panel.WaitIdle());
	TestEqualSensitive(TEXT("Bob is on the new branch"), Current(), FString(TEXT("feature")));
	TestEqualSensitive(TEXT("It starts from origin/main, the newer of the two"), PanelHead(Bob), PanelHead(Bob, TEXT("origin/main")));
	TestEqualSensitive(TEXT("Bob's main is left where it was"), PanelHead(Bob, TEXT("main")), SeedCommit);
	TestEqualSensitive(TEXT("The local edit came along"), GitTest::ReadFile(Bob, TEXT("notes.txt")), FString(TEXT("bob's notes\n")));
	TestTrue(TEXT("The branch follows nothing until it is published"), Panel.Provider.Snapshot()->Status.Upstream.IsEmpty());

	// Sync: the branch is rebased onto the new main, and its copy on the server moves to the rebased commit it had.
	BobCommits(TEXT("b1.txt"));
	Panel.Model->Push(true);
	TestTrue(TEXT("Publishing finishes"), Panel.WaitIdle());
	BobCommits(TEXT("b2.txt"));
	AlicePushes(TEXT("alice 2\n"));
	Panel.Refresh();
	Panel.Model->SyncBranch();
	TestEqualSensitive(TEXT("Bob's commits sit on the newest main"), PanelHead(Bob, TEXT("HEAD~2")), PanelHead(Origin, TEXT("main")));
	TestEqualSensitive(TEXT("The copy on the server has the rebased first commit"), PanelHead(Origin, TEXT("feature")), PanelHead(Bob, TEXT("HEAD~1")));
	TestEqualSensitive(TEXT("The unpushed commit stays unpushed"), Count(TEXT("origin/feature..HEAD")), FString(TEXT("1")));
	TestEqualSensitive(TEXT("The local edit is back"), GitTest::ReadFile(Bob, TEXT("notes.txt")), FString(TEXT("bob's notes\n")));
	TestTrue(TEXT("No stash is left behind"), GitTest::Output(GitTest::Git(Bob, {TEXT("stash"), TEXT("list")})).IsEmpty());

	// Someone else pushed to Bob's branch: Sync changes nothing until Bob has their commit.
	GitTest::Git(Alice, {TEXT("fetch"), TEXT("-q"), TEXT("origin")});
	GitTest::Git(Alice, {TEXT("switch"), TEXT("-q"), TEXT("-c"), TEXT("feature"), TEXT("origin/feature")});
	GitTest::WriteFile(Alice, TEXT("from-alice.txt"), TEXT("from alice\n"));
	GitTest::CommitAll(Alice, TEXT("Alice helps"));
	GitTest::Git(Alice, {TEXT("push"), TEXT("-q"), TEXT("origin"), TEXT("feature")});
	GitTest::Git(Alice, {TEXT("switch"), TEXT("-q"), TEXT("main")});
	AlicePushes(TEXT("alice 3\n"));
	const FString HeadBefore = PanelHead(Bob);
	Panel.Model->SyncBranch();
	TestEqualSensitive(TEXT("Sync refuses: HEAD did not move"), PanelHead(Bob), HeadBefore);
	TestEqualSensitive(TEXT("Alice's commit on the branch is still on the server"), PanelHead(Origin, TEXT("feature")), PanelHead(Alice, TEXT("feature")));
	GitTest::Git(Bob, {TEXT("pull"), TEXT("-q"), TEXT("--rebase"), TEXT("--autostash")});
	Panel.Refresh();
	Panel.Model->SyncBranch();
	TestEqualSensitive(TEXT("With her commit in, Sync rebases all three"), PanelHead(Bob, TEXT("HEAD~3")), PanelHead(Origin, TEXT("main")));
	TestTrue(TEXT("Alice's file is kept"), GitTest::FileExists(Bob, TEXT("from-alice.txt")));
	TestEqualSensitive(TEXT("The copy has the two pushed commits, rebased"), PanelHead(Origin, TEXT("feature")), PanelHead(Bob, TEXT("HEAD~1")));

	// Finish refuses while main on the server has commits the branch lacks.
	AlicePushes(TEXT("alice 4\n"));
	GitTest::Git(Bob, {TEXT("fetch"), TEXT("-q"), TEXT("origin")});
	Panel.Refresh();
	{
		GitWorkspaceDialogs::FScopedConfirmAnswer Yes(true);
		Panel.Model->FinishBranch();
		TestTrue(TEXT("The refused finish finishes"), Panel.WaitIdle());
		TestEqual(TEXT("Finish asks first"), Yes.Questions.Num(), 1);
	}
	TestEqualSensitive(TEXT("Still on feature"), Current(), FString(TEXT("feature")));

	// After a sync, Finish merges the branch into main without touching a file; Push publishes it.
	Panel.Model->SyncBranch();
	const FString FeatureHead = PanelHead(Bob);
	{
		GitWorkspaceDialogs::FScopedConfirmAnswer Yes(true);
		Panel.Model->FinishBranch();
		TestTrue(TEXT("Finishing finishes"), Panel.WaitIdle());
	}
	TestFalse(TEXT("Finish succeeded"), Panel.Model->GetLastFailure().IsSet());
	TestEqualSensitive(TEXT("Bob is on main"), Current(), FString(TEXT("main")));
	TestEqualSensitive(TEXT("A merge commit follows origin/main"), PanelHead(Bob, TEXT("HEAD^1")), PanelHead(Bob, TEXT("origin/main")));
	TestEqualSensitive(TEXT("and joins the branch"), PanelHead(Bob, TEXT("HEAD^2")), FeatureHead);
	TestEqualSensitive(TEXT("main has exactly the branch's content"), PanelHead(Bob, TEXT("HEAD^{tree}")), PanelHead(Bob, TEXT("feature^{tree}")));
	TestEqualSensitive(TEXT("Only the local edit differs from main"), GitTest::Output(GitTest::Git(Bob, {TEXT("status"), TEXT("--porcelain")})), FString(TEXT("M notes.txt")));
	Panel.Model->Push();
	TestTrue(TEXT("Pushing main finishes"), Panel.WaitIdle());
	TestEqualSensitive(TEXT("main on the server has the merge"), PanelHead(Origin, TEXT("main")), PanelHead(Bob));

	// A branch of one commit moves main forward to it.
	Panel.Model->StartBranch(TEXT("fix"));
	TestTrue(TEXT("Starting fix finishes"), Panel.WaitIdle());
	BobCommits(TEXT("fix.txt"));
	Panel.Refresh();
	{
		GitWorkspaceDialogs::FScopedConfirmAnswer Yes(true);
		Panel.Model->FinishBranch();
		TestTrue(TEXT("Finishing fix finishes"), Panel.WaitIdle());
	}
	TestEqualSensitive(TEXT("main moved forward to the fix"), PanelHead(Bob, TEXT("main")), PanelHead(Bob, TEXT("fix")));
	TestEqualSensitive(TEXT("Bob is on main again"), Current(), FString(TEXT("main")));

	// Finish refuses while main has commits that are not on the server.
	Panel.Model->StartBranch(TEXT("other"));
	TestTrue(TEXT("Starting other finishes"), Panel.WaitIdle());
	TestEqualSensitive(TEXT("It starts from main, which is ahead of origin/main"), PanelHead(Bob), PanelHead(Bob, TEXT("main")));
	BobCommits(TEXT("other.txt"));
	Panel.Refresh();
	{
		GitWorkspaceDialogs::FScopedConfirmAnswer Yes(true);
		Panel.Model->FinishBranch();
		TestTrue(TEXT("The second refused finish finishes"), Panel.WaitIdle());
	}
	TestEqualSensitive(TEXT("Still on other"), Current(), FString(TEXT("other")));

	// Sync on main pulls with rebase: main's unpushed commit goes on top of the server's.
	GitTest::Git(Bob, {TEXT("switch"), TEXT("-q"), TEXT("main")});
	AlicePushes(TEXT("alice 5\n"));
	Panel.Refresh();
	Panel.Model->SyncBranch();
	TestEqualSensitive(TEXT("main's own commit sits on the server's newest"), PanelHead(Bob, TEXT("HEAD~1")), PanelHead(Origin, TEXT("main")));
	TestTrue(TEXT("The fix is kept"), GitTest::FileExists(Bob, TEXT("fix.txt")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitPanelHistoryLabelsTest, "GitWorkspace.Panel.HistoryLabels", PanelTestFlags)
bool FGitPanelHistoryLabelsTest::RunTest(const FString&)
{
	GitTest::FFixture Fixture;
	const FString Origin = Fixture.InitBare();
	const FString Seed = Fixture.InitRepo(TEXT("seed"));
	GitTest::WriteFile(Seed, TEXT("a.txt"), TEXT("a\n"));
	GitTest::CommitAll(Seed, TEXT("base"));
	GitTest::Git(Seed, {TEXT("push"), TEXT("-q"), Origin, TEXT("main")});
	const FString Repo = Fixture.Clone(Origin, TEXT("work"));
	const FString Other = Fixture.Clone(Origin, TEXT("other"), TEXT("Bob"));
	GitTest::FScopedNoBackgroundFetch NoFetch;
	FPanelFixture Panel(Repo);
	bool bShowedChanges = false;
	const TSharedRef<SGitHistoryView> History = SNew(SGitHistoryView, Panel.Model).OnShowChanges_Lambda([&bShowedChanges]() { bShowedChanges = true; });
	const TSharedPtr<FCommitList> List = HistoryList(History);
	if (!TestTrue(TEXT("The commit list is tagged"), List.IsValid()))
	{
		return false;
	}
	// Reloads as the panel does when the repository changes, until Done.
	auto Listed = [&Panel, &History](TFunctionRef<bool()> Done)
	{
		return GitTest::WaitFor([&Panel, &History, &Done]()
		{
			Panel.Provider->Tick();
			History->RefreshIfNeeded();
			return Done();
		});
	};
	auto Row = [&History, &List](const SGitHistoryView::FCommitPtr& Commit)
	{
		return Commit.IsValid() ? AllTexts(History->OnGenerateCommitRow(Commit, List.ToSharedRef())->AsWidget()) : TArray<FString>();
	};
	auto UncommittedIndex = [&History, &List]() { return List->GetItems().IndexOfByPredicate([&History](const SGitHistoryView::FCommitPtr& Commit) { return History->IsUncommitted(Commit); }); };
	auto Press = [](const TSharedRef<SWidget>& Widget, FKey Key, bool bControl = false)
	{
		const FModifierKeysState Keys(false, false, bControl, false, false, false, false, false, false);
		return Widget->OnKeyDown(FGeometry(), FKeyEvent(Key, Keys, 0, false, 0, 0)).IsEventHandled();
	};

	// In step with the server: the branch and its upstream are labelled side by side.
	TestTrue(TEXT("History is read"), Listed([&List]() { return FindListed(*List, TEXT("base")).IsValid(); }));
	TArray<FString> Base = Row(FindListed(*List, TEXT("base")));
	TestTrue(TEXT("The branch is labelled: ") + GitTest::Joined(Base), GitTest::HasExactly(Base, TEXT("main")));
	TestTrue(TEXT("Its upstream is labelled on the same commit"), GitTest::HasExactly(Base, TEXT("origin/main")));
	TestTrue(TEXT("The date says when, in words"), Base.ContainsByPredicate([](const FString& Text) { return Text.StartsWith(TEXT("Today at "), ESearchCase::CaseSensitive); }));
	TestEqual(TEXT("Without changes there is no row for them"), UncommittedIndex(), INDEX_NONE);

	// A teammate's commit comes in with a fetch, and one of yours is not pushed yet.
	GitTest::WriteFile(Other, TEXT("b.txt"), TEXT("b\n"));
	GitTest::CommitAll(Other, TEXT("theirs"));
	GitTest::Git(Other, {TEXT("push"), TEXT("-q"), TEXT("origin"), TEXT("main")});
	Panel.Model->Fetch();
	Panel.WaitIdle();
	GitTest::WriteFile(Repo, TEXT("c.txt"), TEXT("c\n"));
	GitTest::CommitAll(Repo, TEXT("mine"));
	Panel.Refresh();
	TestTrue(TEXT("Both sides are listed"), Listed([&List]() { return FindListed(*List, TEXT("mine")).IsValid() && FindListed(*List, TEXT("theirs")).IsValid(); }));
	const SGitHistoryView::FCommitPtr Mine = FindListed(*List, TEXT("mine"));
	const SGitHistoryView::FCommitPtr Theirs = FindListed(*List, TEXT("theirs"));
	if (!TestTrue(TEXT("The commits are found"), Mine.IsValid() && Theirs.IsValid() && FindListed(*List, TEXT("base")).IsValid()))
	{
		return false;
	}
	TestTrue(TEXT("Your commit is not pushed yet"), Mine->bUnpushed && !Mine->bIncoming);
	TestTrue(TEXT("The teammate's is not in your branch yet"), Theirs->bIncoming && !Theirs->bUnpushed);
	const TArray<FString> MineRow = Row(Mine);
	const TArray<FString> TheirsRow = Row(Theirs);
	Base = Row(FindListed(*List, TEXT("base")));
	TestTrue(TEXT("The branch label is on your commit: ") + GitTest::Joined(MineRow), GitTest::HasExactly(MineRow, TEXT("main")) && !GitTest::HasExactly(MineRow, TEXT("origin/main")));
	TestTrue(TEXT("The upstream label is on the teammate's: ") + GitTest::Joined(TheirsRow), GitTest::HasExactly(TheirsRow, TEXT("origin/main")) && !GitTest::HasExactly(TheirsRow, TEXT("main")));
	TestTrue(TEXT("The commit both share has no label"), !GitTest::HasExactly(Base, TEXT("main")) && !GitTest::HasExactly(Base, TEXT("origin/main")));
	TestFalse(TEXT("The shared commit is not marked"), FindListed(*List, TEXT("base"))->bUnpushed || FindListed(*List, TEXT("base"))->bIncoming);

	// Uncommitted changes are a row right above the commit they would follow.
	GitTest::WriteFile(Repo, TEXT("a.txt"), TEXT("a changed\n"));
	Panel.Refresh();
	TestTrue(TEXT("The changes are listed"), Listed([&UncommittedIndex]() { return UncommittedIndex() != INDEX_NONE; }));
	const int32 HeadIndex = List->GetItems().IndexOfByKey(FindListed(*List, TEXT("mine")));
	TestEqual(TEXT("They sit right above HEAD's commit"), UncommittedIndex(), HeadIndex - 1);
	const SGitHistoryView::FCommitPtr Changes = UncommittedIndex() != INDEX_NONE ? List->GetItems()[UncommittedIndex()] : nullptr;
	TArray<FString> ChangesRow = Row(Changes);
	TestTrue(TEXT("The row is titled: ") + GitTest::Joined(ChangesRow), GitTest::HasExactly(ChangesRow, TEXT("Uncommitted changes")));
	TestTrue(TEXT("It counts the changes not staged"), GitTest::HasExactly(ChangesRow, TEXT("1 file not staged")));
	Panel.Model->Stage({TEXT("a.txt")});
	TestTrue(TEXT("Staging shows up"), Panel.WaitUntil([](const GitCore::FSessionSnapshot& Snapshot) { return Snapshot.Status.StagedCount() == 1; }));
	History->RefreshIfNeeded();
	ChangesRow = Row(Changes);
	TestTrue(TEXT("Staged changes are counted as ready: ") + GitTest::Joined(ChangesRow), GitTest::HasExactly(ChangesRow, TEXT("1 file staged, ready to commit")));

	// Opening the row shows the changes in the Changes tab; it has no id to copy.
	List->SetSelection(Changes, ESelectInfo::OnMouseClick);
	TestFalse(TEXT("Ctrl+C has nothing to copy"), Press(List.ToSharedRef(), EKeys::C, true));
	TestTrue(TEXT("Enter is taken"), Press(List.ToSharedRef(), EKeys::Enter));
	TestTrue(TEXT("Enter shows the Changes tab"), bShowedChanges);

	// A search lists only the commits that match.
	const TSharedPtr<SSearchBox> Search = StaticCastSharedPtr<SSearchBox>(FindTagged(History, TEXT("GitPanel.History.Search")));
	if (TestTrue(TEXT("The search is tagged"), Search.IsValid()))
	{
		Search->SetText(FText::FromString(TEXT("mine")));
		TestEqual(TEXT("Searching lists the match alone"), List->GetItems().Num(), 1);
		Search->SetText(FText::GetEmpty());
		TestTrue(TEXT("Clearing the search brings the changes back"), UncommittedIndex() != INDEX_NONE);
	}

	// Committing them moves the branch's label to the new commit, and the row goes.
	Panel.Model->Commit(TEXT("more"), false);
	TestTrue(TEXT("The commit finishes"), Panel.WaitIdle());
	TestTrue(TEXT("The new commit is listed"), Listed([&List]() { return FindListed(*List, TEXT("more")).IsValid(); }));
	TestEqual(TEXT("No changes are left to list"), UncommittedIndex(), INDEX_NONE);
	TestTrue(TEXT("The branch label moved on"), GitTest::HasExactly(Row(FindListed(*List, TEXT("more"))), TEXT("main")) && !GitTest::HasExactly(Row(FindListed(*List, TEXT("mine"))), TEXT("main")));

	// Without a branch, HEAD is labelled as such.
	GitTest::Git(Repo, {TEXT("switch"), TEXT("-q"), TEXT("--detach")});
	Panel.Refresh();
	TestTrue(TEXT("A detached HEAD is labelled HEAD"), Listed([&List, &Row]() { return GitTest::HasExactly(Row(FindListed(*List, TEXT("more"))), TEXT("HEAD")); }));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitPanelDayAndTimeTest, "GitWorkspace.Panel.DayAndTime", PanelTestFlags)
bool FGitPanelDayAndTimeTest::RunTest(const FString&)
{
	const FDateTime Now(2026, 11, 5, 16, 40, 0);
	auto Shown = [&Now](int32 Year, int32 Month, int32 Day, int32 Hour, int32 Minute)
	{
		return GitWorkspaceText::DayAndTimeLocal(FDateTime(Year, Month, Day, Hour, Minute, 0), Now).ToString();
	};
	TestEqualSensitive(TEXT("Today"), Shown(2026, 11, 5, 9, 15), FString(TEXT("Today at 9:15 am")));
	TestEqualSensitive(TEXT("Yesterday"), Shown(2026, 11, 4, 14, 30), FString(TEXT("Yesterday at 2:30 pm")));
	TestEqualSensitive(TEXT("Earlier this year"), Shown(2026, 11, 3, 13, 15), FString(TEXT("3 Nov at 1:15 pm")));
	TestEqualSensitive(TEXT("Another year"), Shown(2025, 11, 3, 13, 15), FString(TEXT("3 Nov 2025 at 1:15 pm")));
	TestEqualSensitive(TEXT("Just after midnight"), Shown(2026, 11, 5, 0, 5), FString(TEXT("Today at 12:05 am")));
	TestEqualSensitive(TEXT("Noon"), Shown(2026, 11, 5, 12, 0), FString(TEXT("Today at 12:00 pm")));
	TestEqualSensitive(TEXT("Yesterday in the month before"), GitWorkspaceText::DayAndTimeLocal(FDateTime(2026, 10, 31, 23, 59, 0), FDateTime(2026, 11, 1, 8, 0, 0)).ToString(),
		FString(TEXT("Yesterday at 11:59 pm")));
	TestEqualSensitive(TEXT("Yesterday in the year before"), GitWorkspaceText::DayAndTimeLocal(FDateTime(2025, 12, 31, 18, 0, 0), FDateTime(2026, 1, 1, 8, 0, 0)).ToString(),
		FString(TEXT("Yesterday at 6:00 pm")));
	TestTrue(TEXT("Now is today, in local time"), GitWorkspaceText::DayAndTime(FDateTime::UtcNow()).ToString().StartsWith(TEXT("Today at "), ESearchCase::CaseSensitive));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitPanelFileTreeTest, "GitWorkspace.Panel.FileTree", PanelTestFlags)
bool FGitPanelFileTreeTest::RunTest(const FString&)
{
	TestEqualSensitive(TEXT("A World Partition actor is listed under its level"), GitWorkspaceText::TreeFolder(TEXT("Content/__ExternalActors__/Maps/Main/A/7Q/0F2KD8XN3PZ4W1.uasset")),
		FString(TEXT("Content/__ExternalActors__/Maps/Main")));
	TestEqualSensitive(TEXT("Other files are listed under their folder"), GitWorkspaceText::TreeFolder(TEXT("Content/Props/Barrel.uasset")), FString(TEXT("Content/Props")));
	TestEqualSensitive(TEXT("Top-level files have no folder"), GitWorkspaceText::TreeFolder(TEXT("readme.txt")), FString());

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
	TestTrue(TEXT("Content comes first"), Roots[0]->bFolder && Roots[0]->Label.Equals(TEXT("Content"), ESearchCase::CaseSensitive) && Roots[0]->FileCount == 5);
	TestTrue(TEXT("A chain of single folders is one row"), Roots[1]->bFolder && Roots[1]->Label.Equals(TEXT("Source/Demo/Private"), ESearchCase::CaseSensitive) && Roots[1]->Path.Equals(TEXT("Source/Demo/Private"), ESearchCase::CaseSensitive));
	TestTrue(TEXT("Files follow the folders"), !Roots[2]->bFolder && Roots[2]->Path.Equals(TEXT("readme.txt"), ESearchCase::CaseSensitive));
	auto Child = [](const TSharedPtr<FGitPathItem>& Folder, const TCHAR* Label)
	{
		const TSharedPtr<FGitPathItem>* Found = Folder->Children.FindByPredicate([Label](const TSharedPtr<FGitPathItem>& Item) { return Item->Label.Equals(Label, ESearchCase::CaseSensitive); });
		return Found ? *Found : TSharedPtr<FGitPathItem>();
	};
	const TSharedPtr<FGitPathItem> Actors = Child(Roots[0], TEXT("__ExternalActors__/Maps/Main"));
	TestTrue(TEXT("A level's actors sit in one folder, without the hash folders"),
		Actors.IsValid() && Actors->Path.Equals(TEXT("Content/__ExternalActors__/Maps/Main"), ESearchCase::CaseSensitive) && Actors->Children.Num() == 2 && !Actors->Children[0]->bFolder);
	const TSharedPtr<FGitPathItem> Props = Child(Roots[0], TEXT("Props"));
	if (!TestTrue(TEXT("Files keep the order given"), Props.IsValid() && Props->Children.Num() == 2 && Props->Children[0]->Path.Equals(TEXT("Content/Props/Barrel.uasset"), ESearchCase::CaseSensitive)))
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
	TestTrue(TEXT("An edited line marks the part that changed"), Lines[3]->HasChange() && Lines[3]->Text.Mid(Lines[3]->ChangeStart, Lines[3]->ChangeEnd - Lines[3]->ChangeStart).Equals(TEXT(" Two"), ESearchCase::CaseSensitive));
	TestFalse(TEXT("Nothing was taken out of the old line"), Lines[2]->HasChange());
	TestTrue(TEXT("An added line without a partner is not marked"), Lines[4]->Kind == EKind::Added && Lines[4]->NewLine == 3 && !Lines[4]->HasChange());
	TestTrue(TEXT("Tabs are expanded"), Lines[5]->Kind == EKind::Context && Lines[5]->Text.Equals(TEXT("    Indented=1"), ESearchCase::CaseSensitive) && Lines[5]->OldLine == 3 && Lines[5]->NewLine == 4);
	TestTrue(TEXT("Remarks are kept"), Lines[6]->Kind == EKind::Note && Lines[6]->Text.Equals(TEXT("No newline at end of file"), ESearchCase::CaseSensitive));

	const TArray<TSharedPtr<FGitDiffLine>> Rewritten = GitDiff::Parse(TEXT("@@ -1 +1 @@\n-alpha\n+zzzzzzzz\n"));
	TestTrue(TEXT("A rewritten line is not marked part by part"), Rewritten.Num() == 3 && !Rewritten[1]->HasChange() && !Rewritten[2]->HasChange());
	const TArray<TSharedPtr<FGitDiffLine>> Pointer = GitDiff::Parse(TEXT("@@ -1,3 +1,3 @@\n version https://git-lfs.github.com/spec/v1\n-oid sha256:aaaa\n-size 10\n+oid sha256:bbbb\n+size 12\n"));
	TestTrue(TEXT("A Git LFS pointer change is one note"), Pointer.Num() == 1 && Pointer[0]->Kind == EKind::Note);
	const TArray<TSharedPtr<FGitDiffLine>> Binary = GitDiff::Parse(TEXT("diff --git a/x.png b/x.png\nindex 1111111..2222222 100644\nBinary files a/x.png and b/x.png differ\n"));
	TestTrue(TEXT("A binary file is one note"), Binary.Num() == 1 && Binary[0]->Kind == EKind::Note);
	TestEqual(TEXT("No output means no differences"), GitDiff::Parse(FString()).Num(), 0);
	const TArray<TSharedPtr<FGitDiffLine>> Merge = GitDiff::Parse(TEXT("@@@ -1,1 -1,1 +1,3 @@@\n  shared\n+ ours\n ++both\n"));
	TestTrue(TEXT("A merge commit's combined diff reads both marker columns"),
		Merge.Num() == 4 && Merge[1]->Kind == EKind::Context && Merge[2]->Kind == EKind::Added && Merge[2]->Text.Equals(TEXT("ours"), ESearchCase::CaseSensitive) && Merge[3]->Kind == EKind::Added && Merge[3]->NewLine == 3);

	const FTCHARToUTF8 Utf8(TEXT("one\ntwo\n"));
	const TArray<TSharedPtr<FGitDiffLine>> NewFile = GitDiff::Parse(GitCore::NewFileDiff(TArray<uint8>(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length())));
	TestTrue(TEXT("A new file is all added lines"), NewFile.Num() == 3 && NewFile[1]->Kind == EKind::Added && NewFile[1]->NewLine == 1 && NewFile[2]->Text.Equals(TEXT("two"), ESearchCase::CaseSensitive));
	const TArray<TSharedPtr<FGitDiffLine>> NewBinary = GitDiff::Parse(GitCore::NewFileDiff({0x89, 'P', 'N', 'G', 0, 1}));
	TestTrue(TEXT("A new binary file is one note"), NewBinary.Num() == 1 && NewBinary[0]->Kind == EKind::Note);

	// Lines are shown as rich text, so code must not read as markup.
	FGitDiffLine Code;
	Code.Kind = EKind::Added;
	Code.Text = TEXT("if (A < B && C > \"D\")");
	Code.ChangeStart = 4;
	Code.ChangeEnd = 5;
	TestEqualSensitive(TEXT("Rich text escapes code and tags the changed part"), GitDiff::ToMarkup(Code), FString(TEXT("if (<add>A</> &lt; B &amp;&amp; C &gt; &quot;D&quot;)")));
	Code.Kind = EKind::Removed;
	TestEqualSensitive(TEXT("A removed line's change is tagged del"), GitDiff::ToMarkup(Code), FString(TEXT("if (<del>A</> &lt; B &amp;&amp; C &gt; &quot;D&quot;)")));
	Code.Kind = EKind::Context;
	TestEqualSensitive(TEXT("Context lines carry no tags"), GitDiff::ToMarkup(Code), FString(TEXT("if (A &lt; B &amp;&amp; C &gt; &quot;D&quot;)")));
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
		return Lines.ContainsByPredicate([Kind, Text](const TSharedPtr<FGitDiffLine>& Line) { return Line->Kind == Kind && Line->Text.Equals(Text, ESearchCase::CaseSensitive); });
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitPanelLineStagingTest, "GitWorkspace.Panel.LineStaging", PanelTestFlags)
bool FGitPanelLineStagingTest::RunTest(const FString&)
{
	using GitCore::ELineAction;
	using FLineList = SListView<SGitDiffView::FLinePtr>;
	auto PressKey = [](const TSharedRef<SGitDiffView>& Diff, FKey Key, bool bControl = false)
	{
		const FModifierKeysState Keys(false, false, bControl, false, false, false, false, false, false);
		return Diff->OnKeyDown(FGeometry(), FKeyEvent(Key, Keys, 0, false, 0, 0)).IsEventHandled();
	};

	// The diff view on its own: what is selected, and what the keys ask its owner to do.
	TArray<TPair<ELineAction, GitCore::FLineSelection>> Asked;
	bool bStarts = true;
	const TSharedRef<SGitDiffView> Diff = SNew(SGitDiffView)
		.OnLineAction_Lambda([&Asked, &bStarts](ELineAction Action, const GitCore::FLineSelection& Lines) { Asked.Emplace(Action, Lines); return bStarts; });
	Diff->SetLineActions({ELineAction::Stage, ELineAction::Discard});
	const FString Text = TEXT("diff --git a/a.ini b/a.ini\n--- a/a.ini\n+++ b/a.ini\n@@ -1,3 +1,3 @@\n [A]\n-One=1\n+One=one\n Two=2\n")
		TEXT("@@ -10,2 +10,3 @@\n Nine=9\n Ten=10\n+Eleven=11\n");
	Diff->SetDiff(Text);
	const TSharedPtr<FLineList> List = StaticCastSharedPtr<FLineList>(FindTagged(Diff, TEXT("GitPanel.Diff.Lines")));
	const TArray<SGitDiffView::FLinePtr>& Lines = Diff->GetLines();
	if (!TestTrue(TEXT("The diff's lines are tagged"), List.IsValid()) || !TestEqual(TEXT("Both hunks show"), Lines.Num(), 9))
	{
		return false;
	}
	TestFalse(TEXT("Without changed lines selected, Space goes on to the file"), PressKey(Diff, EKeys::SpaceBar));
	List->SetSelection(Lines[0], ESelectInfo::OnMouseClick);
	const GitCore::FLineSelection Hunk = Diff->GetSelectedLines();
	TestTrue(TEXT("Clicking a hunk's header selects its changes"), Hunk.Removed.Num() == 1 && Hunk.Removed.Contains(2) && Hunk.Added.Num() == 1 && Hunk.Added.Contains(2));
	List->SetSelection(Lines[8], ESelectInfo::OnMouseClick);
	TestTrue(TEXT("Space stages the selected lines"), PressKey(Diff, EKeys::SpaceBar) && Asked.Num() == 1 && Asked[0].Key == ELineAction::Stage &&
		Asked[0].Value.Added.Num() == 1 && Asked[0].Value.Added.Contains(12) && Asked[0].Value.Removed.IsEmpty());
	TestTrue(TEXT("Pressed again before the diff comes back, it waits"), PressKey(Diff, EKeys::SpaceBar) && Asked.Num() == 1);
	Diff->SetDiff(Text);
	TestEqual(TEXT("The same diff again keeps the selection"), Diff->GetSelectedLines().Added.Num(), 1);
	TestTrue(TEXT("Ctrl+S stages too"), PressKey(Diff, EKeys::S, true) && Asked.Num() == 2 && Asked[1].Key == ELineAction::Stage);
	Diff->SetDiff(Text);
	bStarts = false;
	TestTrue(TEXT("Delete asks to discard them"), PressKey(Diff, EKeys::Delete) && Asked.Num() == 3 && Asked[2].Key == ELineAction::Discard);
	TestTrue(TEXT("After a cancelled discard the keys work at once"), PressKey(Diff, EKeys::Delete) && Asked.Num() == 4);
	TestFalse(TEXT("Unstaging is not offered here"), Diff->RunLineAction(ELineAction::Unstage));
	Diff->SetDiff(Text.Replace(TEXT("+Eleven=11"), TEXT("+Eleven=eleven")), true);
	TestTrue(TEXT("A changed diff clears the selection"), Diff->GetSelectedLines().IsEmpty());
	const FString Upper = Diff->GetDiffText().Replace(TEXT("Eleven=eleven"), TEXT("ELEVEN=ELEVEN"));
	Diff->SetDiff(Upper, true);
	TestEqualSensitive(TEXT("A diff that differs only in case is shown"), Diff->GetDiffText(), Upper);

	// Through the Changes tab, on a repository.
	GitTest::FFixture Fixture;
	const FString Repo = Fixture.InitRepo();
	GitTest::WriteFile(Repo, TEXT("Game.ini"), TEXT("[A]\nOne=1\nTwo=2\n"));
	GitTest::CommitAll(Repo, TEXT("base"));
	const FString Edited = TEXT("[A]\nOne=one\nTwo=2\nThree=3\n");
	GitTest::WriteFile(Repo, TEXT("Game.ini"), Edited);
	GitTest::FScopedNoBackgroundFetch NoFetch;
	FPanelFixture Panel(Repo);
	Panel.Refresh();
	const TSharedRef<SGitChangesView> View = SNew(SGitChangesView, Panel.Model);
	const TSharedPtr<SGitDiffView> PanelDiff = StaticCastSharedPtr<SGitDiffView>(FindTagged(View, TEXT("GitPanel.Changes.Diff")));
	if (!TestTrue(TEXT("The Changes tab's diff is tagged"), PanelDiff.IsValid()))
	{
		return false;
	}
	const TSharedRef<FLineList> PanelList = StaticCastSharedRef<FLineList>(FindTagged(PanelDiff.ToSharedRef(), TEXT("GitPanel.Diff.Lines")).ToSharedRef());
	auto Shows = [&Panel, &PanelDiff](const TCHAR* Line, bool bShown)
	{
		return GitTest::WaitFor([&]() { Panel.Provider->Tick(); return PanelDiff->GetDiffText().Contains(Line, ESearchCase::CaseSensitive) == bShown && !PanelDiff->GetDiffText().IsEmpty(); });
	};
	auto Select = [&PanelDiff, &PanelList](std::initializer_list<const TCHAR*> Texts)
	{
		TArray<SGitDiffView::FLinePtr> Picked;
		for (const SGitDiffView::FLinePtr& Line : PanelDiff->GetLines())
		{
			for (const TCHAR* Wanted : Texts)
			{
				const FGitDiffLine::EKind Kind = Wanted[0] == TEXT('+') ? FGitDiffLine::EKind::Added : FGitDiffLine::EKind::Removed;
				if (Line->Kind == Kind && Line->Text.Equals(Wanted + 1, ESearchCase::CaseSensitive))
				{
					Picked.Add(Line);
				}
			}
		}
		PanelList->ClearSelection();
		PanelList->SetItemSelection(Picked, true, ESelectInfo::OnMouseClick);
	};

	View->SelectPaths({TEXT("Game.ini")});
	TestTrue(TEXT("The file's diff shows"), Shows(TEXT("+Three=3"), true));
	Select({TEXT("+Three=3")});
	TestTrue(TEXT("Space in the diff is taken"), PressKey(PanelDiff.ToSharedRef(), EKeys::SpaceBar));
	TestTrue(TEXT("and stages the line"), Panel.WaitUntil([](const GitCore::FSessionSnapshot& Snapshot) { return Snapshot.Status.StagedCount() == 1; }));
	TestEqualSensitive(TEXT("Only that line is staged"), GitTest::Git(Repo, {TEXT("cat-file"), TEXT("blob"), TEXT(":Game.ini")}).StdoutText(), FString(TEXT("[A]\nOne=1\nTwo=2\nThree=3\n")));
	TestEqualSensitive(TEXT("The file is untouched"), GitTest::ReadFile(Repo, TEXT("Game.ini")), Edited);
	TestTrue(TEXT("The diff comes back without it"), Shows(TEXT("+Three=3"), false) && PanelDiff->GetDiffText().Contains(TEXT("+One=one"), ESearchCase::CaseSensitive));

	{
		GitWorkspaceDialogs::FScopedConfirmAnswer Yes(true);
		Select({TEXT("-One=1"), TEXT("+One=one")});
		TestTrue(TEXT("Delete in the diff is taken"), PressKey(PanelDiff.ToSharedRef(), EKeys::Delete));
		TestEqual(TEXT("and asks first"), Yes.Questions.Num(), 1);
	}
	TestTrue(TEXT("The edit is discarded"), GitTest::WaitFor([&Panel, &Repo]()
	{
		Panel.Provider->Tick();
		return GitTest::ReadFile(Repo, TEXT("Game.ini")).Equals(TEXT("[A]\nOne=1\nTwo=2\nThree=3\n"), ESearchCase::CaseSensitive);
	}));
	TArray<FString> Backups;
	IFileManager::Get().FindFilesRecursive(Backups, *(Repo / TEXT("Saved/GitSourceControl/Backups")), TEXT("Game.ini"), true, false);
	TestTrue(TEXT("after a copy of the file was kept"), Backups.Num() == 1 && GitTest::ReadFile(FString(), Backups[0]).Equals(Edited, ESearchCase::CaseSensitive));

	// Now the file has staged changes only: its staged diff offers Unstage.
	Panel.WaitUntil([](const GitCore::FSessionSnapshot& Snapshot) { const GitCore::FStatusEntry* Entry = Snapshot.Status.Find(TEXT("Game.ini")); return Entry && !Entry->HasUnstaged(); });
	View->SelectPaths({TEXT("Game.ini")});
	TestTrue(TEXT("The staged diff shows"), Shows(TEXT("+Three=3"), true) && !PanelDiff->GetDiffText().Contains(TEXT("One=one"), ESearchCase::CaseSensitive));
	Select({TEXT("+Three=3")});
	TestTrue(TEXT("Space unstages the line"), PressKey(PanelDiff.ToSharedRef(), EKeys::SpaceBar) &&
		Panel.WaitUntil([](const GitCore::FSessionSnapshot& Snapshot) { return Snapshot.Status.StagedCount() == 0; }));
	TestEqualSensitive(TEXT("The line stays in the file"), GitTest::ReadFile(Repo, TEXT("Game.ini")), FString(TEXT("[A]\nOne=1\nTwo=2\nThree=3\n")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitPanelShowInTest, "GitWorkspace.Panel.ShowIn", PanelTestFlags)
bool FGitPanelShowInTest::RunTest(const FString&)
{
	GitTest::FFixture Fixture;
	const FString Repo = Fixture.InitRepo();
	GitTest::WriteFile(Repo, TEXT("Config/DefaultGame.ini"), TEXT("[Game]\n"));
	GitTest::WriteFile(Repo, TEXT("Source/Demo/Demo.cpp"), TEXT("// demo\n"));
	GitTest::WriteFile(Repo, TEXT("Docs/Loose.uasset"), TEXT("outside every content folder\n"));
	IFileManager::Get().MakeDirectory(*(Repo / TEXT("Content/Maps")), true);
	IFileManager::Get().MakeDirectory(*(Repo / TEXT("Content/__ExternalActors__/Maps/Main")), true);
	GitTest::CommitAll(Repo, TEXT("base"));

	// The fixture's Content folder, mounted the way a plugin's is, with one real asset in it.
	const FString MountRoot = TEXT("/GitShowInTest/");
	const FString ContentDir = Repo / TEXT("Content/");
	FPackageName::RegisterMountPoint(MountRoot, ContentDir);
	UCurveFloat* Barrel = nullptr;
	ON_SCOPE_EXIT
	{
		if (Barrel)
		{
			Barrel->ClearFlags(RF_Public | RF_Standalone);
		}
		FPackageName::UnRegisterMountPoint(MountRoot, ContentDir);
	};
	UPackage* Package = CreatePackage(TEXT("/GitShowInTest/Props/Barrel"));
	Barrel = NewObject<UCurveFloat>(Package, TEXT("Barrel"), RF_Public | RF_Standalone);
	const FString BarrelFile = Repo / TEXT("Content/Props/Barrel.uasset");
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	if (!TestTrue(TEXT("The test asset saves"), UPackage::SavePackage(Package, Barrel, *BarrelFile, SaveArgs)))
	{
		return false;
	}
	IAssetRegistry::GetChecked().ScanFilesSynchronous({BarrelFile}, true);

	GitTest::FScopedNoBackgroundFetch NoFetch;
	FPanelFixture Panel(Repo);
	auto Browse = [&Panel](const TArray<FString>& Paths)
	{
		TArray<FAssetData> Assets;
		TArray<FString> Folders;
		Panel.Model->GetBrowseTargets(Paths, Assets, Folders);
		TArray<FString> Shown;
		for (const FAssetData& Asset : Assets)
		{
			Shown.Add(Asset.PackageName.ToString());
		}
		for (const FString& Folder : Folders)
		{
			Shown.Add(Folder + TEXT("/"));
		}
		return FString::Join(Shown, TEXT(", "));
	};
	TestEqualSensitive(TEXT("An asset in a content folder is selected"), Browse({TEXT("Content/Props/Barrel.uasset")}), FString(TEXT("/GitShowInTest/Props/Barrel")));
	TestEqualSensitive(TEXT("A content folder shows itself"), Browse({TEXT("Content/Props")}), FString(TEXT("/GitShowInTest/Props/")));
	TestEqualSensitive(TEXT("A deleted asset shows its folder"), Browse({TEXT("Content/Props/Gone.uasset")}), FString(TEXT("/GitShowInTest/Props/")));
	TestEqualSensitive(TEXT("A World Partition actor without its level shows the level's folder"),
		Browse({TEXT("Content/__ExternalActors__/Maps/Main/A/7Q/Actor.uasset")}), FString(TEXT("/GitShowInTest/Maps/")));
	TestEqualSensitive(TEXT("A level's actor folder without the level shows the folder the level would be in"),
		Browse({TEXT("Content/__ExternalActors__/Maps/Main")}), FString(TEXT("/GitShowInTest/Maps/")));
	TestEqualSensitive(TEXT("Files and folders outside content folders are left out"),
		Browse({TEXT("Config/DefaultGame.ini"), TEXT("Source/Demo"), TEXT("Source/Demo/Demo.cpp"), TEXT("Docs/Loose.uasset")}), FString());
	TestFalse(TEXT("So Show in Content Browser is not offered for them"), Panel.Model->CanBrowseTo({TEXT("Config/DefaultGame.ini"), TEXT("Docs/Loose.uasset")}));
	TestTrue(TEXT("It is offered when one of them is an asset"), Panel.Model->CanBrowseTo({TEXT("Config/DefaultGame.ini"), TEXT("Content/Props/Barrel.uasset")}));

	auto Reveal = [&Panel](const TArray<FString>& Paths) { return FString::Join(Panel.Model->GetRevealTargets(Paths), TEXT(", ")); };
	auto Absolute = [&Panel](const FString& Path) { return Panel.Model->ToAbsolute(Path); };
	TestEqualSensitive(TEXT("Files show selected in their folders"), Reveal({TEXT("Config/DefaultGame.ini"), TEXT("Content/Props/Barrel.uasset")}),
		Absolute(TEXT("Config/DefaultGame.ini")) + TEXT(", ") + Absolute(TEXT("Content/Props/Barrel.uasset")));
	TestEqualSensitive(TEXT("Each folder opens once"), Reveal({TEXT("Content/Props/Barrel.uasset"), TEXT("Content/Props/Gone.uasset")}), Absolute(TEXT("Content/Props/Barrel.uasset")));
	TestEqualSensitive(TEXT("A folder shows itself"), Reveal({TEXT("Source/Demo")}), Absolute(TEXT("Source/Demo")));
	TestEqualSensitive(TEXT("A file that is gone shows the nearest folder that exists"), Reveal({TEXT("Old/Deep/Gone.txt")}), Absolute(FString()));
	TestTrue(TEXT("Show in Finder / Explorer is offered outside content folders too"), Panel.Model->CanReveal({TEXT("Docs/Loose.uasset")}));
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
		float ChangesShareWas = GetDefault<UGitSourceControlUserSettings>()->ChangesListShare;
		float StagedShareWas = GetDefault<UGitSourceControlUserSettings>()->StagedListShare;
		float HintLineHeight = 0.0f;
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
	Panel.WaitIdle();
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
		// Lines picked in the diff: the buttons that stage or discard them show beside the first one.
		const TSharedPtr<SWidget> Diff = FindTagged(State->Widget.ToSharedRef(), TEXT("GitPanel.Changes.Diff"));
		if (const TSharedPtr<SWidget> Found = Diff.IsValid() ? FindTagged(Diff.ToSharedRef(), TEXT("GitPanel.Diff.Lines")) : nullptr)
		{
			const TSharedPtr<SListView<SGitDiffView::FLinePtr>> Lines = StaticCastSharedPtr<SListView<SGitDiffView::FLinePtr>>(Found);
			for (const SGitDiffView::FLinePtr& Line : Lines->GetItems())
			{
				if (Line->Kind == FGitDiffLine::EKind::Removed || Line->Kind == FGitDiffLine::EKind::Added)
				{
					Lines->SetItemSelection(Line, true, ESelectInfo::OnMouseClick);
				}
			}
		}
	}, 1.5f));
	ADD_LATENT_AUTOMATION_COMMAND(FDelayedFunctionLatentCommand([State]()
	{
		State->Capture(TEXT("changes-diff-lines"));
		State->bWrapWas = GetDefault<UGitSourceControlUserSettings>()->bWrapDiffLines;
		GetMutableDefault<UGitSourceControlUserSettings>()->bWrapDiffLines = true;
	}, 1.0f));
	ADD_LATENT_AUTOMATION_COMMAND(FDelayedFunctionLatentCommand([State]()
	{
		State->Capture(TEXT("changes-diff-wrapped"));
		GetMutableDefault<UGitSourceControlUserSettings>()->bWrapDiffLines = State->bWrapWas;
	}, 1.0f));
	// Resize takes screen pixels; sizes here are Slate units.
	ADD_LATENT_AUTOMATION_COMMAND(FDelayedFunctionLatentCommand([State]() { State->Window->Resize(FVector2D(460.0f, 680.0f) * State->Window->GetDPIScaleFactor()); }, 0.1f));
	ADD_LATENT_AUTOMATION_COMMAND(FDelayedFunctionLatentCommand([State]() { State->Capture(TEXT("changes-narrow")); }, 1.0f));
	ADD_LATENT_AUTOMATION_COMMAND(FDelayedFunctionLatentCommand([State]() { State->Window->Resize(FVector2D(1180.0f, 680.0f) * State->Window->GetDPIScaleFactor()); State->Widget->ShowTab(1); }, 0.1f));
	ADD_LATENT_AUTOMATION_COMMAND(FDelayedFunctionLatentCommand([State]()
	{
		// The newest commit, below the uncommitted changes.
		if (const TSharedPtr<FCommitList> Commits = HistoryList(State->Widget.ToSharedRef()))
		{
			if (const SGitHistoryView::FCommitPtr* Commit = Commits->GetItems().FindByPredicate([](const SGitHistoryView::FCommitPtr& Each) { return !Each->Oid.IsEmpty(); }))
			{
				Commits->SetSelection(*Commit, ESelectInfo::OnMouseClick);
			}
		}
	}, 2.0f));
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
	ADD_LATENT_AUTOMATION_COMMAND(FDelayedFunctionLatentCommand([State]()
	{
		State->Capture(TEXT("locks"));
		// Alice's lock on Hero as if she took it in another clone: the server says it is hers, but this clone has no record of it.
		const GitCore::FGitSessionPtr Session = State->Panel->Provider->GetSession();
		const GitCore::FSessionSnapshotRef Snapshot = Session->GetSnapshot();
		if (const GitCore::FLockInfo* Lock = Snapshot->Locks.Find(TEXT("Content/Hero.uasset")))
		{
			Session->GetJournal().Remove(Snapshot->Locks.Endpoint, Lock->Id);
		}
		State->Panel->Model->CheckLocks();
	}, 1.0f));
	ADD_LATENT_AUTOMATION_COMMAND(FDelayedFunctionLatentCommand([State]() { SelectFirst<SGitLocksView::FItemPtr>(State->Widget.ToSharedRef(), TEXT("GitPanel.Locks.List")); }, 2.0f));
	ADD_LATENT_AUTOMATION_COMMAND(FDelayedFunctionLatentCommand([State]()
	{
		State->Capture(TEXT("locks-elsewhere"));
		State->Widget->ShowPaths({TEXT("Content/Hero.uasset")});
	}, 1.0f));
	ADD_LATENT_AUTOMATION_COMMAND(FDelayedFunctionLatentCommand([this, State]()
	{
		State->Capture(TEXT("changes-claim"));
		// Drag the Staged list's title bar up through Slate, as a mouse would: the lists trade height.
		const TSharedPtr<SWidget> Bar = FindTagged(State->Widget.ToSharedRef(), TEXT("GitPanel.Changes.StagedBar"));
		if (!TestTrue(TEXT("The Staged title bar is found"), Bar.IsValid()))
		{
			return;
		}
		const FGeometry& Geometry = Bar->GetCachedGeometry();
		const FVector2D Start = Geometry.LocalToAbsolute(FVector2D(40.0f, Geometry.GetLocalSize().Y * 0.5f));
		const FVector2D End = Start - FVector2D(0.0f, 120.0f * Geometry.GetAccumulatedLayoutTransform().GetScale());
		const TSet<FKey> Pressed = {EKeys::LeftMouseButton};
		{
			// Otherwise Slate looks only in the window under the real pointer, which may be another one.
			const FScopedStandInPlatform Platform;
			FSlateApplication& App = FSlateApplication::Get();
			App.ProcessMouseButtonDownEvent(nullptr, FPointerEvent(0, Start, Start, Pressed, EKeys::LeftMouseButton, 0.0f, FModifierKeysState()));
			App.ProcessMouseMoveEvent(FPointerEvent(0, End, Start, Pressed, FKey(), 0.0f, FModifierKeysState()));
			App.ProcessMouseButtonUpEvent(FPointerEvent(0, End, End, TSet<FKey>(), EKeys::LeftMouseButton, 0.0f, FModifierKeysState()));
		}
		const UGitSourceControlUserSettings* Settings = GetDefault<UGitSourceControlUserSettings>();
		TestTrue(TEXT("Dragging the Staged bar up gives Staged more room"),
			Settings->StagedListShare > State->StagedShareWas && Settings->ChangesListShare < State->ChangesShareWas);
	}, 1.5f));
	ADD_LATENT_AUTOMATION_COMMAND(FDelayedFunctionLatentCommand([State]()
	{
		State->Capture(TEXT("changes-resized"));
		// Leave the user's own split as it was.
		UGitSourceControlUserSettings* Settings = GetMutableDefault<UGitSourceControlUserSettings>();
		Settings->ChangesListShare = State->ChangesShareWas;
		Settings->StagedListShare = State->StagedShareWas;
		Settings->SaveConfig();
		// A message lets the staged readme be committed.
		if (const TSharedPtr<SWidget> Message = FindTagged(State->Widget.ToSharedRef(), TEXT("GitPanel.Changes.Message")))
		{
			StaticCastSharedPtr<SMultiLineEditableTextBox>(Message)->SetText(FText::FromString(TEXT("Update the readme")));
		}
	}, 1.0f));
	ADD_LATENT_AUTOMATION_COMMAND(FDelayedFunctionLatentCommand([this, State]()
	{
		// The pointer over Commit, then Option (Alt) held there as well.
		const TSharedPtr<SWidget> Button = FindTagged(State->Widget.ToSharedRef(), TEXT("GitPanel.Changes.Commit"));
		if (TestTrue(TEXT("The Commit button is found"), Button.IsValid()))
		{
			Button->OnMouseEnter(Button->GetCachedGeometry(), FPointerEvent());
			State->Capture(TEXT("commit-hover"));
			{
				const FScopedStandInPlatform OptionHeld(true);
				State->Capture(TEXT("commit-and-push"));
			}
			Button->OnMouseLeave(FPointerEvent());
		}
		// Without a message, the hint below the box asks for one.
		if (const TSharedPtr<SWidget> Message = FindTagged(State->Widget.ToSharedRef(), TEXT("GitPanel.Changes.Message")))
		{
			StaticCastSharedPtr<SMultiLineEditableTextBox>(Message)->SetText(FText::GetEmpty());
		}
	}, 1.0f));
	ADD_LATENT_AUTOMATION_COMMAND(FDelayedFunctionLatentCommand([State]()
	{
		// With nothing staged, a longer hint replaces that one-line hint.
		if (const TSharedPtr<SWidget> Hint = FindTagged(State->Widget.ToSharedRef(), TEXT("GitPanel.Changes.CommitHint")))
		{
			State->HintLineHeight = Hint->GetDesiredSize().Y;
		}
		State->Panel->Model->Unstage({TEXT("readme.txt")});
		State->Panel->WaitUntil([](const GitCore::FSessionSnapshot& Snapshot) { return Snapshot.Status.StagedCount() == 0; });
	}, 1.0f));
	ADD_LATENT_AUTOMATION_COMMAND(FDelayedFunctionLatentCommand([this, State]()
	{
		State->Capture(TEXT("commit-hint"));
		// It wraps to the panel's width, not to the width of the hint it replaced.
		const TSharedPtr<SWidget> Hint = FindTagged(State->Widget.ToSharedRef(), TEXT("GitPanel.Changes.CommitHint"));
		if (TestTrue(TEXT("The commit hint is found"), Hint.IsValid() && State->HintLineHeight > 0.0f))
		{
			TestEqual(TEXT("The longer hint still takes one line of a wide panel"), Hint->GetDesiredSize().Y, State->HintLineHeight, 0.5f);
		}
		State->Window->RequestDestroyWindow();
	}, 1.0f));
	return true;
}

#endif
