// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#include "SGitStashesView.h"

#include "Editor/GitWorkspaceModel.h"
#include "SGitDialogs.h"

#include "Framework/Application/SlateApplication.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "HAL/PlatformApplicationMisc.h"
#include "RevisionControlStyle/RevisionControlStyle.h"
#include "Styling/AppStyle.h"
#include "Types/ISlateMetaData.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SSearchBox.h"
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
	bBuiltFolders = GitFileTree::ShowFolders();
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
				.AutoHeight()
				.Padding(4.0f, 0.0f, 4.0f, 4.0f)
				[
					SAssignNew(SearchBox, SSearchBox)
					.HintText(LOCTEXT("SearchHint", "Search stashes (Ctrl+F)"))
					.OnTextChanged_Lambda([this](const FText& Text)
					{
						StashFilter = Text.ToString().TrimStartAndEnd();
						ApplyStashFilter();
					})
				]
				+ SVerticalBox::Slot()
				.FillHeight(1.0f)
				[
					SAssignNew(List, SListView<FStashPtr>)
					.AddMetaData<FTagMetaData>(TEXT("GitPanel.Stashes.List"))
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
					.Text_Lambda([this]()
					{
						return AllStashes.IsEmpty() ? LOCTEXT("NoStashes", "No stashes. Stash your changes to switch tasks, pull, or try something without committing.")
							: LOCTEXT("NoMatchingStashes", "No stash matches the search.");
					})
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
					SNew(SVerticalBox)
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.0f, 0.0f, 0.0f, 4.0f)
					[
						SNew(SHorizontalBox)
						.Visibility_Lambda([this]() { return StashPaths.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible; })
						+ SHorizontalBox::Slot()
						.FillWidth(1.0f)
						[
							SNew(SSearchBox)
							.HintText_Lambda([this]() { return FText::Format(LOCTEXT("FileSearchHint", "Filter {0} {0}|plural(one=file,other=files)"), StashPaths.Num()); })
							.OnTextChanged_Lambda([this](const FText& Text)
							{
								FileFilter = Text.ToString().TrimStartAndEnd();
								RebuildFiles();
							})
						]
						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						.Padding(4.0f, 0.0f, 0.0f, 0.0f)
						[
							GitFileTree::MakeFoldersToggle()
						]
					]
					+ SVerticalBox::Slot()
					.FillHeight(1.0f)
					[
						SNew(SOverlay)
						+ SOverlay::Slot()
						.Padding(4.0f)
						[
							SNew(STextBlock)
							.Visibility_Lambda([this]() { return StashPaths.IsEmpty() ? EVisibility::Visible : EVisibility::Collapsed; })
							.Text(LOCTEXT("SelectStash", "Select a stash to see its files."))
							.ColorAndOpacity(FSlateColor::UseSubduedForeground())
							.AutoWrapText(true)
						]
						+ SOverlay::Slot()
						[
							SAssignNew(FileTree, STreeView<FPathItemPtr>)
							.Visibility_Lambda([this]() { return StashPaths.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible; })
							.TreeItemsSource(&Files)
							.SelectionMode(ESelectionMode::Multi)
							.OnGenerateRow(this, &SGitStashesView::OnGenerateFileRow)
							.OnGetChildren(this, &SGitStashesView::OnGetFileChildren)
							.OnExpansionChanged(this, &SGitStashesView::OnFileExpansionChanged)
							.OnSetExpansionRecursive(this, &SGitStashesView::OnSetFileExpansionRecursive)
							.OnMouseButtonDoubleClick(this, &SGitStashesView::OnFileDoubleClick)
							.OnContextMenuOpening(this, &SGitStashesView::OnFileMenu)
						]
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

void SGitStashesView::Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime)
{
	SCompoundWidget::Tick(AllottedGeometry, InCurrentTime, InDeltaTime);
	// The folders preference is shared with the other tabs.
	if (GitFileTree::ShowFolders() != bBuiltFolders)
	{
		RebuildFiles();
	}
}

void SGitStashesView::FocusSearch()
{
	if (SearchBox.IsValid())
	{
		FSlateApplication::Get().SetKeyboardFocus(SearchBox, EFocusCause::SetDirectly);
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
	AllStashes.Reset();
	for (const GitCore::FStashEntry& Stash : Snapshot->Stashes)
	{
		AllStashes.Add(MakeShared<GitCore::FStashEntry>(Stash));
	}
	ApplyStashFilter();
}

void SGitStashesView::ApplyStashFilter()
{
	const FStashPtr Current = Selection();
	const FString SelectedOid = Current.IsValid() ? Current->Oid : FString();
	Stashes = AllStashes.FilterByPredicate([this](const FStashPtr& Stash) { return StashFilter.IsEmpty() || Stash->Message.Contains(StashFilter); });
	List->RequestListRefresh();
	if (Current.IsValid() && Stashes.Contains(Current))
	{
		// Still listed: its files are already shown.
		return;
	}
	if (const FStashPtr* Reselect = Stashes.FindByPredicate([&SelectedOid](const FStashPtr& Stash) { return Stash->Oid == SelectedOid; }))
	{
		List->SetSelection(*Reselect, ESelectInfo::Direct);
	}
	else
	{
		List->ClearSelection();
		StashPaths.Reset();
		RebuildFiles();
	}
}

SGitStashesView::FStashPtr SGitStashesView::Selection() const
{
	const TArray<FStashPtr> Selected = List.IsValid() ? List->GetSelectedItems() : TArray<FStashPtr>();
	return Selected.IsEmpty() ? nullptr : Selected[0];
}

void SGitStashesView::OnSelected(FStashPtr Stash, ESelectInfo::Type SelectInfo)
{
	StashPaths.Reset();
	RebuildFiles();
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
		Self->StashPaths = MoveTemp(Paths);
		Self->RebuildFiles();
	});
}

void SGitStashesView::RebuildFiles()
{
	if (!FileTree.IsValid())
	{
		return;
	}
	const TSharedPtr<FGitWorkspaceModel> Model = WeakModel.Pin();
	bBuiltFolders = GitFileTree::ShowFolders();
	TArray<TPair<FString, FPathItemPtr>> Named;
	for (const FString& Path : StashPaths)
	{
		const FString Name = Displays.Get(Model.Get(), Path).Name.ToString();
		if (!FileFilter.IsEmpty() && !Path.Contains(FileFilter) && !Name.Contains(FileFilter))
		{
			continue;
		}
		const FPathItemPtr Item = MakeShared<FGitPathItem>();
		Item->Path = Path;
		// In folders, files go by the name shown; a flat list goes by path.
		Named.Emplace(bBuiltFolders ? Name : Path, Item);
	}
	Named.Sort([](const TPair<FString, FPathItemPtr>& A, const TPair<FString, FPathItemPtr>& B)
	{
		const int32 Order = A.Key.Compare(B.Key, ESearchCase::IgnoreCase);
		return Order != 0 ? Order < 0 : A.Value->Path < B.Value->Path;
	});
	TArray<FPathItemPtr> Flat;
	for (const TPair<FString, FPathItemPtr>& Pair : Named)
	{
		Flat.Add(Pair.Value);
	}
	Files = GitFileTree::Build(Flat, bBuiltFolders);
	bRestoringExpansion = true;
	GitFileTree::RestoreExpansion(*FileTree, Files, CollapsedFolders, !FileFilter.IsEmpty());
	bRestoringExpansion = false;
	FileTree->RequestTreeRefresh();
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
				.HighlightText(this, &SGitStashesView::GetStashFilterText)
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
				.HighlightText(this, &SGitStashesView::GetStashFilterText)
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			]
		];
}

TSharedRef<ITableRow> SGitStashesView::OnGenerateFileRow(FPathItemPtr Item, const TSharedRef<STableViewBase>& Owner)
{
	const TSharedRef<GitFileTree::STreeRow<FGitPathItem>> Row = SNew(GitFileTree::STreeRow<FGitPathItem>, Owner).Item(Item);
	const TAttribute<FText> Highlight(this, &SGitStashesView::GetFileFilterText);
	if (Item->bFolder)
	{
		Row->SetCell(GitFileTree::MakeNameCell(Row, true, FText::FromString(Item->Label), FText::AsNumber(Item->FileCount), nullptr,
			FText::Format(LOCTEXT("FolderTooltip", "{0}\n{1} {1}|plural(one=file,other=files). Alt+click opens or closes everything in it."), FText::FromString(Item->Path), Item->FileCount),
			Highlight));
		return Row;
	}
	const FGitFileDisplay& Display = Displays.Get(WeakModel.Pin().Get(), Item->Path);
	Row->SetCell(GitFileTree::MakeNameCell(Row, false, Display.Name, bBuiltFolders ? FText::GetEmpty() : Display.Folder, Display.Icon,
		FText::FromString(Item->Path), Highlight));
	return Row;
}

void SGitStashesView::OnGetFileChildren(FPathItemPtr Item, TArray<FPathItemPtr>& OutChildren) const
{
	if (Item.IsValid())
	{
		OutChildren = Item->Children;
	}
}

void SGitStashesView::OnFileExpansionChanged(FPathItemPtr Item, bool bExpanded)
{
	if (!Item.IsValid() || !Item->bFolder || bRestoringExpansion)
	{
		return;
	}
	if (bExpanded)
	{
		CollapsedFolders.Remove(Item->Path);
	}
	else
	{
		CollapsedFolders.Add(Item->Path);
	}
}

void SGitStashesView::OnSetFileExpansionRecursive(FPathItemPtr Item, bool bExpand)
{
	if (FileTree.IsValid())
	{
		GitFileTree::SetExpansionRecursive(*FileTree, Item, bExpand);
	}
}

void SGitStashesView::OnFileDoubleClick(FPathItemPtr Item)
{
	if (Item.IsValid() && Item->bFolder)
	{
		FileTree->SetItemExpansion(Item, !FileTree->IsItemExpanded(Item));
	}
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

TSharedPtr<SWidget> SGitStashesView::OnFileMenu()
{
	const TArray<FPathItemPtr> Picked = FileTree->GetSelectedItems();
	if (Picked.IsEmpty())
	{
		return nullptr;
	}
	TArray<FString> ItemPaths;
	TArray<FPathItemPtr> Folders;
	for (const FPathItemPtr& Item : Picked)
	{
		ItemPaths.Add(Item->Path);
		if (Item->bFolder)
		{
			Folders.Add(Item);
		}
	}
	FMenuBuilder Menu(true, nullptr);
	if (const TSharedPtr<FGitWorkspaceModel> M = WeakModel.Pin())
	{
		GitFileTree::AddShowEntries(Menu, M.ToSharedRef(), ItemPaths);
	}
	Menu.AddMenuEntry(LOCTEXT("CopyPathMenu", "Copy path"), FText::GetEmpty(), FSlateIcon(FAppStyle::GetAppStyleSetName(), "GenericCommands.Copy"),
		FUIAction(FExecuteAction::CreateLambda([ItemPaths]() { FPlatformApplicationMisc::ClipboardCopy(*FString::Join(ItemPaths, LINE_TERMINATOR)); })));
	if (!Folders.IsEmpty())
	{
		Menu.AddMenuSeparator();
		Menu.AddMenuEntry(LOCTEXT("ExpandAllMenu", "Open all inside"), LOCTEXT("ExpandAllMenuTooltip", "Open these folders and every folder in them. (Alt+click)"),
			FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.FolderOpen"),
			FUIAction(FExecuteAction::CreateLambda([this, Folders]() { for (const FPathItemPtr& Folder : Folders) { OnSetFileExpansionRecursive(Folder, true); } })));
		Menu.AddMenuEntry(LOCTEXT("CollapseAllMenu", "Close all inside"), LOCTEXT("CollapseAllMenuTooltip", "Close these folders and every folder in them. (Alt+click)"),
			FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.FolderClosed"),
			FUIAction(FExecuteAction::CreateLambda([this, Folders]() { for (const FPathItemPtr& Folder : Folders) { OnSetFileExpansionRecursive(Folder, false); } })));
	}
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
	if (GitWorkspaceDialogs::Confirm(LOCTEXT("DropTitle", "Delete stash"),
		FText::Format(LOCTEXT("DropMessage", "Delete \"{0}\"? Its changes cannot be recovered."), FText::FromString(Stash->Message)), StashPaths, LOCTEXT("DropButton", "Delete")))
	{
		Model->DropStash(Stash->Selector);
	}
}

#undef LOCTEXT_NAMESPACE
