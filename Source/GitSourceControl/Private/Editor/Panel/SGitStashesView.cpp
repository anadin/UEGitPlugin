// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#include "SGitStashesView.h"

#include "Editor/GitWorkspaceModel.h"
#include "SGitDialogs.h"

#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "RevisionControlStyle/RevisionControlStyle.h"
#include "Styling/AppStyle.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/STableRow.h"

#define LOCTEXT_NAMESPACE "GitWorkspaceStashes"

void SGitStashesView::Construct(const FArguments& InArgs, const TSharedRef<FGitWorkspaceModel>& InModel)
{
	WeakModel = InModel;
	ChangedHandle = InModel->OnChanged().AddSP(this, &SGitStashesView::OnModelChanged);
	auto HasSelection = [this]() { return Selection().IsValid() && WeakModel.IsValid() && !WeakModel.Pin()->IsBusy(); };

	ChildSlot
	[
		SNew(SVerticalBox)
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(4.0f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			[
				SNew(SButton)
				.Text(LOCTEXT("StashChanges", "Stash changes…"))
				.ToolTipText(LOCTEXT("StashChangesTooltip", "Put your local changes aside and go back to the last commit. Loaded assets reload."))
				.IsEnabled_Lambda([this]() { const TSharedPtr<FGitWorkspaceModel> Model = WeakModel.Pin(); return Model.IsValid() && !Model->IsBusy(); })
				.OnClicked_Lambda([this]() { StashChanges(); return FReply::Handled(); })
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(8.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(SCheckBox)
				.IsChecked_Lambda([this]() { return bIncludeNewFiles ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
				.OnCheckStateChanged_Lambda([this](ECheckBoxState State) { bIncludeNewFiles = State == ECheckBoxState::Checked; })
				[
					SNew(STextBlock)
					.Text(LOCTEXT("IncludeNewFiles", "Include new files"))
				]
			]
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.Padding(4.0f, 0.0f)
			[
				SNew(SButton)
				.Text(LOCTEXT("Apply", "Apply"))
				.ToolTipText(LOCTEXT("ApplyTooltip", "Bring these changes back and keep the stash."))
				.IsEnabled_Lambda(HasSelection)
				.OnClicked_Lambda([this]() { Apply(false); return FReply::Handled(); })
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			[
				SNew(SButton)
				.ButtonStyle(FAppStyle::Get(), "PrimaryButton")
				.Text(LOCTEXT("Pop", "Restore"))
				.ToolTipText(LOCTEXT("PopTooltip", "Bring these changes back and delete the stash."))
				.IsEnabled_Lambda(HasSelection)
				.OnClicked_Lambda([this]() { Apply(true); return FReply::Handled(); })
			]
		]
		+ SVerticalBox::Slot()
		.FillHeight(1.0f)
		[
			SNew(SSplitter)
			+ SSplitter::Slot()
			.Value(0.6f)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot()
				.FillHeight(1.0f)
				[
					SAssignNew(List, SListView<FStashPtr>)
					.ListItemsSource(&Stashes)
					.SelectionMode(ESelectionMode::Single)
					.OnGenerateRow(this, &SGitStashesView::OnGenerateRow)
					.OnSelectionChanged(this, &SGitStashesView::OnSelected)
					.OnContextMenuOpening(this, &SGitStashesView::OnContextMenu)
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(8.0f)
				[
					SNew(STextBlock)
					.Visibility_Lambda([this]() { return Stashes.IsEmpty() ? EVisibility::Visible : EVisibility::Collapsed; })
					.Text(LOCTEXT("NoStashes", "No stashes. Stash your changes to switch tasks, sync, or try something without committing."))
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
					.AutoWrapText(true)
				]
			]
			+ SSplitter::Slot()
			.Value(0.4f)
			[
				SNew(SBorder)
				.BorderImage(FAppStyle::GetBrush("Brushes.Recessed"))
				.Padding(6.0f)
				[
					SNew(SOverlay)
					+ SOverlay::Slot()
					.Padding(4.0f)
					[
						SNew(STextBlock)
						.Visibility_Lambda([this]() { return SelectedPaths.IsEmpty() ? EVisibility::Visible : EVisibility::Collapsed; })
						.Text(LOCTEXT("SelectStash", "Select a stash to see its files."))
						.ColorAndOpacity(FSlateColor::UseSubduedForeground())
						.AutoWrapText(true)
					]
					+ SOverlay::Slot()
					[
					SAssignNew(PathList, SListView<TSharedPtr<FString>>)
					.ListItemsSource(&SelectedPaths)
					.SelectionMode(ESelectionMode::None)
					.OnGenerateRow_Lambda([](TSharedPtr<FString> Path, const TSharedRef<STableViewBase>& Owner)
					{
						return SNew(STableRow<TSharedPtr<FString>>, Owner)
							[
								SNew(STextBlock)
								.Text(FText::FromString(*Path))
								.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
							];
					})
					]
				]
			]
		]
	];
	OnModelChanged();
}

SGitStashesView::~SGitStashesView()
{
	if (const TSharedPtr<FGitWorkspaceModel> Model = WeakModel.Pin())
	{
		Model->OnChanged().Remove(ChangedHandle);
	}
}

void SGitStashesView::OnModelChanged()
{
	const TSharedPtr<FGitWorkspaceModel> Model = WeakModel.Pin();
	if (!Model.IsValid())
	{
		return;
	}
	const GitCore::FSessionSnapshotRef Snapshot = Model->GetSnapshot();
	if (Snapshot->Generation == BuiltGeneration)
	{
		return;
	}
	BuiltGeneration = Snapshot->Generation;
	const FString SelectedOid = Selection().IsValid() ? Selection()->Oid : FString();
	Stashes.Reset();
	FStashPtr Reselect;
	for (const GitCore::FStashEntry& Stash : Snapshot->Stashes)
	{
		Stashes.Add(MakeShared<GitCore::FStashEntry>(Stash));
		if (Stash.Oid == SelectedOid)
		{
			Reselect = Stashes.Last();
		}
	}
	List->RequestListRefresh();
	if (Reselect.IsValid())
	{
		List->SetSelection(Reselect, ESelectInfo::Direct);
	}
	else
	{
		SelectedPaths.Reset();
		PathList->RequestListRefresh();
	}
}

SGitStashesView::FStashPtr SGitStashesView::Selection() const
{
	const TArray<FStashPtr> Selected = List.IsValid() ? List->GetSelectedItems() : TArray<FStashPtr>();
	return Selected.IsEmpty() ? nullptr : Selected[0];
}

void SGitStashesView::OnSelected(FStashPtr Stash, ESelectInfo::Type SelectInfo)
{
	SelectedPaths.Reset();
	PathList->RequestListRefresh();
	const TSharedPtr<FGitWorkspaceModel> Model = WeakModel.Pin();
	if (!Stash.IsValid() || !Model.IsValid())
	{
		return;
	}
	Model->LoadStashPaths(Stash->Selector, [WeakThis = TWeakPtr<SGitStashesView>(SharedThis(this)), Oid = Stash->Oid](TArray<FString>&& Paths, const FString& Error)
	{
		const TSharedPtr<SGitStashesView> Self = WeakThis.Pin();
		if (!Self.IsValid() || !Self->Selection().IsValid() || Self->Selection()->Oid != Oid)
		{
			return;
		}
		for (FString& Path : Paths)
		{
			Self->SelectedPaths.Add(MakeShared<FString>(MoveTemp(Path)));
		}
		Self->PathList->RequestListRefresh();
	});
}

TSharedRef<ITableRow> SGitStashesView::OnGenerateRow(FStashPtr Stash, const TSharedRef<STableViewBase>& Owner)
{
	// Git describes stashes as "On main: message" (or "WIP on main: …" without one).
	FString Branch;
	FString Message = Stash->Message;
	FString Prefix;
	FString Rest;
	if (Message.Split(TEXT(": "), &Prefix, &Rest) && (Prefix.StartsWith(TEXT("On ")) || Prefix.StartsWith(TEXT("WIP on "))))
	{
		Prefix.Split(TEXT("on "), nullptr, &Branch, ESearchCase::CaseSensitive, ESearchDir::FromEnd);
		Message = Rest;
	}
	return SNew(STableRow<FStashPtr>, Owner)
		.Padding(FMargin(4.0f, 3.0f))
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(0.0f, 0.0f, 8.0f, 0.0f)
			[
				SNew(SImage)
				.Image(FRevisionControlStyleManager::Get().GetBrush("RevisionControl.Shelved"))
				.DesiredSizeOverride(FVector2D(16.0f, 16.0f))
			]
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text(FText::FromString(Message))
				.ToolTipText(FText::FromString(Stash->Message))
				.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(8.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock)
				.Text(Branch.IsEmpty() ? FText::GetEmpty() : FText::Format(LOCTEXT("StashBranch", "on {0}"), FText::FromString(Branch)))
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			]
		];
}

TSharedPtr<SWidget> SGitStashesView::OnContextMenu()
{
	if (!Selection().IsValid())
	{
		return nullptr;
	}
	FMenuBuilder Menu(true, nullptr);
	Menu.AddMenuEntry(LOCTEXT("ApplyMenu", "Apply"), LOCTEXT("ApplyMenuTooltip", "Bring these changes back and keep the stash."), FSlateIcon(),
		FUIAction(FExecuteAction::CreateSP(this, &SGitStashesView::Apply, false)));
	Menu.AddMenuEntry(LOCTEXT("PopMenu", "Restore and delete"), LOCTEXT("PopMenuTooltip", "Bring these changes back and delete the stash."), FSlateIcon(),
		FUIAction(FExecuteAction::CreateSP(this, &SGitStashesView::Apply, true)));
	Menu.AddMenuSeparator();
	Menu.AddMenuEntry(LOCTEXT("DropMenu", "Delete…"), LOCTEXT("DropMenuTooltip", "Throw these changes away."), FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Delete"),
		FUIAction(FExecuteAction::CreateSP(this, &SGitStashesView::Drop)));
	return Menu.MakeWidget();
}

void SGitStashesView::StashChanges()
{
	const TSharedPtr<FGitWorkspaceModel> Model = WeakModel.Pin();
	if (!Model.IsValid())
	{
		return;
	}
	FString Message;
	if (GitWorkspaceDialogs::AskForText(LOCTEXT("StashTitle", "Stash changes"),
		bIncludeNewFiles ? LOCTEXT("StashPromptAll", "Put all local changes aside, including new files. Name the stash so you can find it later.")
			: LOCTEXT("StashPrompt", "Put your changes to tracked files aside. New files stay. Name the stash so you can find it later."),
		LOCTEXT("StashHint", "What were you working on?"), Message))
	{
		Model->StashChanges(Message, bIncludeNewFiles);
	}
}

void SGitStashesView::Apply(bool bDrop)
{
	const TSharedPtr<FGitWorkspaceModel> Model = WeakModel.Pin();
	if (const FStashPtr Stash = Selection(); Model.IsValid() && Stash.IsValid())
	{
		Model->ApplyStash(Stash->Selector, bDrop);
	}
}

void SGitStashesView::Drop()
{
	const TSharedPtr<FGitWorkspaceModel> Model = WeakModel.Pin();
	const FStashPtr Stash = Selection();
	if (!Model.IsValid() || !Stash.IsValid())
	{
		return;
	}
	TArray<FString> Paths;
	for (const TSharedPtr<FString>& Path : SelectedPaths)
	{
		Paths.Add(*Path);
	}
	if (GitWorkspaceDialogs::Confirm(LOCTEXT("DropTitle", "Delete stash"),
		FText::Format(LOCTEXT("DropMessage", "Delete \"{0}\"? Its changes cannot be recovered."), FText::FromString(Stash->Message)), Paths, LOCTEXT("DropButton", "Delete")))
	{
		Model->DropStash(Stash->Selector);
	}
}

#undef LOCTEXT_NAMESPACE
