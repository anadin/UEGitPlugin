// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#include "SGitPanel.h"

#include "Editor/GitWorkspaceModel.h"
#include "Editor/GitWorkspaceText.h"
#include "SGitChangesView.h"
#include "SGitDialogs.h"
#include "SGitHistoryView.h"
#include "SGitLocksView.h"
#include "SGitStashesView.h"

#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "ISourceControlModule.h"
#include "Logging/MessageLog.h"
#include "RevisionControlStyle/RevisionControlStyle.h"
#include "Styling/AppStyle.h"
#include "Styling/StyleColors.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Images/SThrobber.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Input/SSegmentedControl.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SWidgetSwitcher.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "GitWorkspacePanel"

const FName SGitPanel::TabId(TEXT("GitNativeWorkspace"));

namespace
{
	constexpr float CompactWidth = 560.0f;

	TSharedRef<SWidget> IconText(const FSlateBrush* Icon, const TAttribute<FText>& Text)
	{
		return SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				SNew(SImage)
				.Image(Icon)
				.DesiredSizeOverride(FVector2D(16.0f, 16.0f))
				.ColorAndOpacity(FSlateColor::UseForeground())
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(4.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock)
				.Text(Text)
				.Visibility_Lambda([Text]() { return Text.Get().IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible; })
			];
	}
}

bool SGitPanel::IsCompact() const
{
	return GetCachedGeometry().GetLocalSize().X < CompactWidth;
}

void SGitPanel::Construct(const FArguments& InArgs, const TSharedRef<FGitWorkspaceModel>& InModel)
{
	WeakModel = InModel;
	ChildSlot
	[
		SNew(SWidgetSwitcher)
		.WidgetIndex_Lambda([this]() { const TSharedPtr<FGitWorkspaceModel> Model = WeakModel.Pin(); return Model.IsValid() && Model->IsActive() ? 0 : 1; })
		+ SWidgetSwitcher::Slot()
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				MakeToolbar()
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				MakeBanner()
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(4.0f, 4.0f, 4.0f, 0.0f)
			[
				MakeTabs()
			]
			+ SVerticalBox::Slot()
			.FillHeight(1.0f)
			.Padding(0.0f, 4.0f, 0.0f, 0.0f)
			[
				SNew(SWidgetSwitcher)
				.WidgetIndex_Lambda([this]() { return static_cast<int32>(Tab); })
				+ SWidgetSwitcher::Slot()
				[
					SAssignNew(ChangesView, SGitChangesView, InModel)
				]
				+ SWidgetSwitcher::Slot()
				[
					SAssignNew(HistoryView, SGitHistoryView, InModel)
				]
				+ SWidgetSwitcher::Slot()
				[
					SAssignNew(StashesView, SGitStashesView, InModel)
				]
				+ SWidgetSwitcher::Slot()
				[
					SAssignNew(LocksView, SGitLocksView, InModel)
				]
			]
		]
		// Shown when Git Workspace is not the active revision control provider.
		+ SWidgetSwitcher::Slot()
		.HAlign(HAlign_Center)
		.VAlign(VAlign_Center)
		[
			SNew(SBox)
			.MaxDesiredWidth(420.0f)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					SNew(STextBlock)
					.AutoWrapText(true)
					.Justification(ETextJustify::Center)
					.Text(LOCTEXT("NotActive", "Connect revision control to Git Workspace to see your changes, history, stashes and locks here."))
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.HAlign(HAlign_Center)
				.Padding(0.0f, 12.0f, 0.0f, 0.0f)
				[
					SNew(SButton)
					.ButtonStyle(FAppStyle::Get(), "PrimaryButton")
					.Text(LOCTEXT("Connect", "Connect…"))
					.OnClicked_Lambda([]()
					{
						ISourceControlModule::Get().ShowLoginDialog(FSourceControlLoginClosed(), ELoginWindowMode::Modeless);
						return FReply::Handled();
					})
				]
			]
		]
	];
}

TSharedRef<SWidget> SGitPanel::MakeToolbar()
{
	auto Model = [this]() { return WeakModel.Pin(); };
	auto Idle = [Model]() { const TSharedPtr<FGitWorkspaceModel> M = Model(); return M.IsValid() && !M->IsBusy(); };
	const ISlateStyle& Style = FRevisionControlStyleManager::Get();

	return SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("Brushes.Panel"))
		.Padding(FMargin(6.0f, 4.0f))
		[
			SNew(SHorizontalBox)
			// Branch
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				SNew(SComboButton)
				.ToolTipText(LOCTEXT("BranchTooltip", "Switch, create or publish branches."))
				.OnGetMenuContent(this, &SGitPanel::MakeBranchMenu)
				.IsEnabled_Lambda(Idle)
				.ButtonContent()
				[
					IconText(Style.GetBrush("RevisionControl.BranchExplorer.BranchList.Branch"), TAttribute<FText>::CreateLambda([Model]()
					{
						const TSharedPtr<FGitWorkspaceModel> M = Model();
						if (!M.IsValid())
						{
							return FText::GetEmpty();
						}
						const GitCore::FStatusSnapshot& Status = M->GetSnapshot()->Status;
						return Status.Branch.IsEmpty() ? FText::Format(LOCTEXT("Detached", "Detached at {0}"), FText::FromString(GitWorkspaceText::ShortId(Status.Head)))
							: FText::FromString(Status.Branch);
					}))
				]
			]
			// Sync state
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.VAlign(VAlign_Center)
			.Padding(10.0f, 0.0f)
			[
				SNew(STextBlock)
				.Text(this, &SGitPanel::GetSyncState)
				.ToolTipText(this, &SGitPanel::GetSyncStateTooltip)
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
				.Visibility_Lambda([this]() { return IsCompact() ? EVisibility::Collapsed : EVisibility::Visible; })
			]
			// Running action
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(0.0f, 0.0f, 8.0f, 0.0f)
			[
				SNew(SHorizontalBox)
				.Visibility_Lambda([Model]() { const TSharedPtr<FGitWorkspaceModel> M = Model(); return M.IsValid() && M->IsBusy() ? EVisibility::Visible : EVisibility::Collapsed; })
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(0.0f, 0.0f, 6.0f, 0.0f)
				[
					SNew(SCircularThrobber)
					.Radius(7.0f)
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text_Lambda([Model]() { const TSharedPtr<FGitWorkspaceModel> M = Model(); return M.IsValid() ? M->GetActivity() : FText::GetEmpty(); })
					.Visibility_Lambda([this]() { return IsCompact() ? EVisibility::Collapsed : EVisibility::Visible; })
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				[
					SNew(SButton)
					.ButtonStyle(FAppStyle::Get(), "SimpleButton")
					.ToolTipText(LOCTEXT("CancelTooltip", "Stop the running Git command."))
					.OnClicked_Lambda([Model]() { if (const TSharedPtr<FGitWorkspaceModel> M = Model()) { M->Cancel(); } return FReply::Handled(); })
					[
						SNew(SImage)
						.Image(FAppStyle::GetBrush("Icons.X"))
						.ColorAndOpacity(FSlateColor::UseForeground())
					]
				]
			]
			// Fetch
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.Padding(2.0f, 0.0f)
			[
				SNew(SButton)
				.ButtonStyle(FAppStyle::Get(), "SimpleButton")
				.ToolTipText(LOCTEXT("FetchTooltip", "Check the server for new commits. Nothing in your project changes."))
				.IsEnabled_Lambda(Idle)
				.OnClicked_Lambda([Model]() { if (const TSharedPtr<FGitWorkspaceModel> M = Model()) { M->Fetch(); } return FReply::Handled(); })
				[
					IconText(FAppStyle::GetBrush("Icons.Refresh"), TAttribute<FText>::CreateLambda([this]() { return IsCompact() ? FText::GetEmpty() : LOCTEXT("Fetch", "Fetch"); }))
				]
			]
			// Pull
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.Padding(2.0f, 0.0f)
			[
				SNew(SButton)
				.ButtonStyle(FAppStyle::Get(), "SimpleButton")
				.ToolTipText(LOCTEXT("PullTooltip", "Get the new commits: Unreal saves your work, updates the files and reloads changed assets."))
				.IsEnabled_Lambda([Model, Idle]() { return Idle() && Model()->GetSnapshot()->Remote.Behind > 0; })
				.OnClicked_Lambda([Model]() { if (const TSharedPtr<FGitWorkspaceModel> M = Model()) { M->Sync(); } return FReply::Handled(); })
				[
					IconText(FAppStyle::GetBrush("Icons.CircleArrowDown"), TAttribute<FText>::CreateLambda([this, Model]()
					{
						const TSharedPtr<FGitWorkspaceModel> M = Model();
						const int32 Behind = M.IsValid() ? M->GetSnapshot()->Remote.Behind : 0;
						if (IsCompact())
						{
							return Behind > 0 ? FText::AsNumber(Behind) : FText::GetEmpty();
						}
						return Behind > 0 ? FText::Format(LOCTEXT("PullCount", "Pull {0}"), Behind) : LOCTEXT("Pull", "Pull");
					}))
				]
			]
			// Push
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.Padding(2.0f, 0.0f)
			[
				SNew(SButton)
				.ButtonStyle(FAppStyle::Get(), "SimpleButton")
				.ToolTipText(LOCTEXT("PushTooltip", "Publish your commits (and the branch, if it is new). Locks are kept."))
				.IsEnabled_Lambda([Model, Idle]()
				{
					if (!Idle())
					{
						return false;
					}
					const GitCore::FSessionSnapshotRef Snapshot = Model()->GetSnapshot();
					return Snapshot->Remote.Ahead > 0 || (Snapshot->Remote.Upstream.IsEmpty() && !Snapshot->Status.Branch.IsEmpty() && !Snapshot->Status.bUnborn);
				})
				.OnClicked_Lambda([Model]() { if (const TSharedPtr<FGitWorkspaceModel> M = Model()) { M->Push(); } return FReply::Handled(); })
				[
					IconText(FAppStyle::GetBrush("Icons.CircleArrowUp"), TAttribute<FText>::CreateLambda([this, Model]()
					{
						const TSharedPtr<FGitWorkspaceModel> M = Model();
						if (!M.IsValid())
						{
							return LOCTEXT("Push", "Push");
						}
						const GitCore::FSessionSnapshotRef Snapshot = M->GetSnapshot();
						if (IsCompact())
						{
							return Snapshot->Remote.Ahead > 0 ? FText::AsNumber(Snapshot->Remote.Ahead) : FText::GetEmpty();
						}
						if (Snapshot->Remote.Upstream.IsEmpty() && !Snapshot->Status.Branch.IsEmpty())
						{
							return LOCTEXT("Publish", "Publish");
						}
						return Snapshot->Remote.Ahead > 0 ? FText::Format(LOCTEXT("PushCount", "Push {0}"), Snapshot->Remote.Ahead) : LOCTEXT("Push", "Push");
					}))
				]
			]
			// More
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.Padding(2.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(SComboButton)
				.ComboButtonStyle(FAppStyle::Get(), "SimpleComboButton")
				.HasDownArrow(false)
				.ToolTipText(LOCTEXT("MoreTooltip", "More Git actions"))
				.OnGetMenuContent(this, &SGitPanel::MakeMoreMenu)
				.ButtonContent()
				[
					SNew(SImage)
					.Image(FAppStyle::GetBrush("Icons.EllipsisVerticalNarrow"))
					.ColorAndOpacity(FSlateColor::UseForeground())
				]
			]
		];
}

TSharedRef<SWidget> SGitPanel::MakeBanner()
{
	auto Failure = [this]() -> const TOptional<FGitActionResult>*
	{
		const TSharedPtr<FGitWorkspaceModel> Model = WeakModel.Pin();
		return Model.IsValid() && Model->GetLastFailure().IsSet() ? &Model->GetLastFailure() : nullptr;
	};
	return SNew(SBorder)
		.Visibility_Lambda([Failure]() { return Failure() ? EVisibility::Visible : EVisibility::Collapsed; })
		.BorderImage(FAppStyle::GetBrush("RoundedError"))
		.Padding(FMargin(8.0f, 6.0f))
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(0.0f, 0.0f, 8.0f, 0.0f)
			[
				SNew(SImage)
				.Image(FAppStyle::GetBrush("Icons.ErrorWithColor"))
			]
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.AutoWrapText(true)
				.Text_Lambda([Failure]() { const TOptional<FGitActionResult>* Result = Failure(); return Result ? Result->GetValue().Message : FText::GetEmpty(); })
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(8.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(SButton)
				.ButtonStyle(FAppStyle::Get(), "SimpleButton")
				.Text(LOCTEXT("ShowLog", "Show log"))
				.OnClicked_Lambda([]() { FMessageLog("SourceControl").Open(); return FReply::Handled(); })
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				SNew(SButton)
				.ButtonStyle(FAppStyle::Get(), "SimpleButton")
				.ToolTipText(LOCTEXT("Dismiss", "Dismiss"))
				.OnClicked_Lambda([this]() { if (const TSharedPtr<FGitWorkspaceModel> Model = WeakModel.Pin()) { Model->DismissFailure(); } return FReply::Handled(); })
				[
					SNew(SImage)
					.Image(FAppStyle::GetBrush("Icons.X"))
					.ColorAndOpacity(FSlateColor::UseForeground())
				]
			]
		];
}

TSharedRef<SWidget> SGitPanel::MakeTabs()
{
	auto Snapshot = [this]()
	{
		const TSharedPtr<FGitWorkspaceModel> Model = WeakModel.Pin();
		return Model.IsValid() ? Model->GetSnapshot() : MakeShared<GitCore::FSessionSnapshot, ESPMode::ThreadSafe>();
	};
	auto Counted = [this](const FText& Label, int32 Count)
	{
		if (IsCompact())
		{
			return Count > 0 ? FText::AsNumber(Count) : FText::GetEmpty();
		}
		return Count > 0 ? FText::Format(LOCTEXT("TabCount", "{0} {1}"), Label, Count) : Label;
	};
	const ISlateStyle& Style = FRevisionControlStyleManager::Get();
	return SNew(SHorizontalBox)
		+ SHorizontalBox::Slot()
		.AutoWidth()
		[
			SNew(SSegmentedControl<ETab>)
			.Value_Lambda([this]() { return Tab; })
			.OnValueChanged(this, &SGitPanel::SetTab)
			+ SSegmentedControl<ETab>::Slot(ETab::Changes)
			.Icon(Style.GetBrush("RevisionControl.ModifiedLocally"))
			.Text_Lambda([Snapshot, Counted]() { return Counted(LOCTEXT("ChangesTab", "Changes"), Snapshot()->Status.Entries.Num()); })
			.ToolTip(LOCTEXT("ChangesTabTooltip", "Stage and commit your changes. (Ctrl+1)"))
			+ SSegmentedControl<ETab>::Slot(ETab::History)
			.Icon(Style.GetBrush("RevisionControl.Actions.History"))
			.Text_Lambda([Counted]() { return Counted(LOCTEXT("HistoryTab", "History"), 0); })
			.ToolTip(LOCTEXT("HistoryTabTooltip", "Commits on this branch. (Ctrl+2)"))
			+ SSegmentedControl<ETab>::Slot(ETab::Stashes)
			.Icon(Style.GetBrush("RevisionControl.Shelved"))
			.Text_Lambda([Snapshot, Counted]() { return Counted(LOCTEXT("StashesTab", "Stashes"), Snapshot()->Stashes.Num()); })
			.ToolTip(LOCTEXT("StashesTabTooltip", "Changes you put aside. (Ctrl+3)"))
			+ SSegmentedControl<ETab>::Slot(ETab::Locks)
			.Icon(Style.GetBrush("RevisionControl.Locked"))
			.Text_Lambda([Snapshot, Counted]() { return Counted(LOCTEXT("LocksTab", "Locks"), Snapshot()->Locks.ByPath.Num()); })
			.ToolTip(LOCTEXT("LocksTabTooltip", "Who holds which locks. (Ctrl+4)"))
		];
}

void SGitPanel::SetTab(ETab InTab)
{
	Tab = InTab;
	if (Tab == ETab::History && HistoryView.IsValid())
	{
		HistoryView->RefreshIfNeeded();
	}
}

FText SGitPanel::GetSyncState() const
{
	const TSharedPtr<FGitWorkspaceModel> Model = WeakModel.Pin();
	if (!Model.IsValid())
	{
		return FText::GetEmpty();
	}
	const GitCore::FSessionSnapshotRef Snapshot = Model->GetSnapshot();
	const GitCore::FRemoteState& Remote = Snapshot->Remote;
	if (Snapshot->Status.Branch.IsEmpty())
	{
		return LOCTEXT("StateDetached", "Not on a branch");
	}
	if (Remote.Upstream.IsEmpty())
	{
		return LOCTEXT("StateUnpublished", "Not published yet");
	}
	FText Counts = Remote.Ahead == 0 && Remote.Behind == 0 ? LOCTEXT("StateUpToDate", "Up to date")
		: FText::Format(LOCTEXT("StateCounts", "↓{0} ↑{1}"), Remote.Behind, Remote.Ahead);
	if (!Remote.Error.IsEmpty())
	{
		return Remote.bFetched ? FText::Format(LOCTEXT("StateOffline", "{0} · offline, fetched {1}"), Counts, GitWorkspaceText::Age(Remote.FetchedAtUtc))
			: FText::Format(LOCTEXT("StateOfflineNever", "{0} · could not fetch"), Counts);
	}
	return Remote.bFetched ? FText::Format(LOCTEXT("StateFetched", "{0} · fetched {1}"), Counts, GitWorkspaceText::Age(Remote.FetchedAtUtc)) : Counts;
}

FText SGitPanel::GetSyncStateTooltip() const
{
	const TSharedPtr<FGitWorkspaceModel> Model = WeakModel.Pin();
	if (!Model.IsValid())
	{
		return FText::GetEmpty();
	}
	const GitCore::FRemoteState& Remote = Model->GetSnapshot()->Remote;
	if (!Remote.Error.IsEmpty())
	{
		return FText::FromString(Remote.Error);
	}
	return Remote.Upstream.IsEmpty() ? LOCTEXT("UnpublishedTooltip", "Push to publish this branch.")
		: FText::Format(LOCTEXT("CountsTooltip", "{0} new {0}|plural(one=commit,other=commits) on {2}; {1} of yours not pushed yet."), Remote.Behind, Remote.Ahead, FText::FromString(Remote.Upstream));
}

TSharedRef<SWidget> SGitPanel::MakeBranchMenu()
{
	FMenuBuilder Menu(true, nullptr);
	const TSharedPtr<FGitWorkspaceModel> Model = WeakModel.Pin();
	if (!Model.IsValid())
	{
		return Menu.MakeWidget();
	}
	const GitCore::FSessionSnapshotRef Snapshot = Model->GetSnapshot();
	const FSlateIcon BranchIcon(FRevisionControlStyleManager::GetStyleSetName(), "RevisionControl.BranchExplorer.BranchList.Branch");

	Menu.BeginSection("Actions");
	Menu.AddMenuEntry(LOCTEXT("NewBranch", "New branch…"), LOCTEXT("NewBranchTooltip", "Start a branch from here. Your changes come along."),
		FSlateIcon(FRevisionControlStyleManager::GetStyleSetName(), "RevisionControl.BranchExplorer.NewBranch"), FUIAction(FExecuteAction::CreateSP(this, &SGitPanel::CreateBranch)));
	if (Snapshot->Remote.Upstream.IsEmpty() && !Snapshot->Status.Branch.IsEmpty())
	{
		Menu.AddMenuEntry(LOCTEXT("PublishBranch", "Publish branch"), LOCTEXT("PublishBranchTooltip", "Push this branch so others can get it."), FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.CircleArrowUp"),
			FUIAction(FExecuteAction::CreateLambda([this]() { if (const TSharedPtr<FGitWorkspaceModel> M = WeakModel.Pin()) { M->Push(); } })));
	}
	Menu.EndSection();

	TSet<FString> Local;
	Menu.BeginSection("Local", LOCTEXT("LocalBranches", "Branches"));
	for (const GitCore::FBranchInfo& Branch : Snapshot->Branches)
	{
		if (Branch.bRemote)
		{
			continue;
		}
		Local.Add(Branch.Name);
		const FString Name = Branch.Name;
		Menu.AddMenuEntry(FText::FromString(Name), Branch.bCurrent ? LOCTEXT("CurrentBranch", "The current branch.")
			: FText::Format(LOCTEXT("SwitchTooltip", "Switch to {0}. Loaded assets that differ reload."), FText::FromString(Name)),
			BranchIcon,
			FUIAction(FExecuteAction::CreateLambda([this, Name]() { if (const TSharedPtr<FGitWorkspaceModel> M = WeakModel.Pin()) { M->SwitchBranch(Name); } }),
				FCanExecuteAction::CreateLambda([bCurrent = Branch.bCurrent]() { return !bCurrent; }),
				FIsActionChecked::CreateLambda([bCurrent = Branch.bCurrent]() { return bCurrent; })),
			NAME_None, EUserInterfaceActionType::RadioButton);
	}
	Menu.EndSection();

	// Remote branches without a local branch: switching creates one that tracks it.
	TArray<FString> RemoteOnly;
	for (const GitCore::FBranchInfo& Branch : Snapshot->Branches)
	{
		FString Remote;
		FString Name;
		if (Branch.bRemote && Branch.Name.Split(TEXT("/"), &Remote, &Name) && Name != TEXT("HEAD") && !Local.Contains(Name))
		{
			RemoteOnly.AddUnique(Name);
		}
	}
	if (!RemoteOnly.IsEmpty())
	{
		Menu.BeginSection("Remote", LOCTEXT("RemoteBranches", "On the server"));
		for (const FString& Name : RemoteOnly)
		{
			Menu.AddMenuEntry(FText::FromString(Name), FText::Format(LOCTEXT("SwitchRemoteTooltip", "Get {0} from the server and switch to it."), FText::FromString(Name)), BranchIcon,
				FUIAction(FExecuteAction::CreateLambda([this, Name]() { if (const TSharedPtr<FGitWorkspaceModel> M = WeakModel.Pin()) { M->SwitchBranch(Name); } })));
		}
		Menu.EndSection();
	}
	return Menu.MakeWidget();
}

TSharedRef<SWidget> SGitPanel::MakeMoreMenu()
{
	FMenuBuilder Menu(true, nullptr);
	const FName Style = FRevisionControlStyleManager::GetStyleSetName();
	Menu.AddMenuEntry(LOCTEXT("SubmitContent", "Check in (Submit Content)…"), LOCTEXT("SubmitContentTooltip", "Unreal's submit dialog: commit, push and release locks in one step."),
		FSlateIcon(Style, "RevisionControl.Actions.Submit"),
		FUIAction(FExecuteAction::CreateLambda([this]() { if (const TSharedPtr<FGitWorkspaceModel> M = WeakModel.Pin()) { M->CheckIn(); } })));
	Menu.AddMenuEntry(LOCTEXT("RefreshMenu", "Refresh"), LOCTEXT("RefreshMenuTooltip", "Read the working tree again. (F5)"), FSlateIcon(Style, "RevisionControl.Actions.Refresh"),
		FUIAction(FExecuteAction::CreateSP(this, &SGitPanel::Refresh)));
	Menu.AddMenuEntry(LOCTEXT("CheckLocksMenu", "Check locks"), LOCTEXT("CheckLocksMenuTooltip", "Ask the server who holds which locks."), FSlateIcon(Style, "RevisionControl.Locked"),
		FUIAction(FExecuteAction::CreateLambda([this]() { if (const TSharedPtr<FGitWorkspaceModel> M = WeakModel.Pin()) { M->CheckLocks(); } })));
	Menu.AddMenuSeparator();
	Menu.AddMenuEntry(LOCTEXT("SettingsMenu", "Revision control settings…"), FText::GetEmpty(), FSlateIcon(Style, "RevisionControl.Actions.ChangeSettings"),
		FUIAction(FExecuteAction::CreateLambda([]() { ISourceControlModule::Get().ShowLoginDialog(FSourceControlLoginClosed(), ELoginWindowMode::Modeless); })));
	Menu.AddMenuEntry(LOCTEXT("LogMenu", "Show log"), LOCTEXT("LogMenuTooltip", "Revision control messages and errors."), FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Info"),
		FUIAction(FExecuteAction::CreateLambda([]() { FMessageLog("SourceControl").Open(); })));
	return Menu.MakeWidget();
}

void SGitPanel::CreateBranch()
{
	const TSharedPtr<FGitWorkspaceModel> Model = WeakModel.Pin();
	if (!Model.IsValid())
	{
		return;
	}
	TSet<FString> Existing;
	for (const GitCore::FBranchInfo& Branch : Model->GetSnapshot()->Branches)
	{
		Existing.Add(Branch.Name);
	}
	FString Name;
	const bool bAccepted = GitWorkspaceDialogs::AskForText(LOCTEXT("NewBranchTitle", "New branch"),
		LOCTEXT("NewBranchPrompt", "Name the branch. You switch to it right away; your uncommitted changes come along."),
		LOCTEXT("NewBranchHint", "feature/my-change"), Name,
		[Existing](const FString& Value) -> FText
		{
			if (Value.IsEmpty())
			{
				return LOCTEXT("NameRequired", "Enter a name.");
			}
			if (Value.Contains(TEXT(" ")) || Value.StartsWith(TEXT("-")) || Value.Contains(TEXT("..")) || Value.EndsWith(TEXT("/")) || Value.EndsWith(TEXT(".lock")))
			{
				return LOCTEXT("NameInvalid", "Use letters, numbers, - _ . and / (no spaces).");
			}
			return Existing.Contains(Value) ? LOCTEXT("NameTaken", "A branch with this name already exists.") : FText::GetEmpty();
		});
	if (bAccepted)
	{
		Model->CreateBranch(Name);
	}
}

void SGitPanel::Refresh()
{
	if (const TSharedPtr<FGitWorkspaceModel> Model = WeakModel.Pin())
	{
		if (const GitCore::FGitSessionPtr Session = Model->GetSession())
		{
			Session->RequestStatusRefresh();
			Session->RequestStashesAndBranches();
		}
	}
}

void SGitPanel::ShowTab(int32 Index)
{
	SetTab(static_cast<ETab>(FMath::Clamp(Index, 0, 3)));
}

void SGitPanel::ShowPaths(const TArray<FString>& Paths)
{
	SetTab(ETab::Changes);
	if (ChangesView.IsValid())
	{
		ChangesView->SelectPaths(Paths);
	}
}

FReply SGitPanel::OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent)
{
	const FKey Key = InKeyEvent.GetKey();
	if (Key == EKeys::F5)
	{
		Refresh();
		return FReply::Handled();
	}
	if (InKeyEvent.IsControlDown() || InKeyEvent.IsCommandDown())
	{
		if (Key == EKeys::F)
		{
			switch (Tab)
			{
			case ETab::Changes:
				ChangesView->FocusSearch();
				break;
			case ETab::History:
				HistoryView->FocusSearch();
				break;
			case ETab::Stashes:
				StashesView->FocusSearch();
				break;
			case ETab::Locks:
				LocksView->FocusSearch();
				break;
			}
			return FReply::Handled();
		}
		const TArray<FKey> TabKeys = {EKeys::One, EKeys::Two, EKeys::Three, EKeys::Four};
		const int32 Index = TabKeys.IndexOfByKey(Key);
		if (Index != INDEX_NONE)
		{
			SetTab(static_cast<ETab>(Index));
			return FReply::Handled();
		}
	}
	return SCompoundWidget::OnKeyDown(MyGeometry, InKeyEvent);
}

#undef LOCTEXT_NAMESPACE
