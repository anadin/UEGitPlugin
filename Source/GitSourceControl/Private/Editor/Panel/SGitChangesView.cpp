// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#include "SGitChangesView.h"

#include "Editor/GitWorkspaceModel.h"
#include "SGitDialogs.h"

#include "Framework/Application/SlateApplication.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "HAL/PlatformApplicationMisc.h"
#include "RevisionControlStyle/RevisionControlStyle.h"
#include "Styling/AppStyle.h"
#include "Types/ISlateMetaData.h"
#include "Styling/StyleColors.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Input/SSearchBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/Layout/SWrapBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/SExpanderArrow.h"
#include "Widgets/Views/SHeaderRow.h"
#include "Widgets/Views/STableRow.h"

#define LOCTEXT_NAMESPACE "GitWorkspaceChanges"

namespace
{
	const FName NameColumn(TEXT("Name"));
	const FName ChangeColumn(TEXT("Change"));
	const FName LockColumn(TEXT("Lock"));
	const FName ActionColumn(TEXT("Action"));

	FText GroupLabel(const FGitChangeItem& Group)
	{
		const FText Count = Group.FileCount == Group.Total ? FText::AsNumber(Group.FileCount) : FText::Format(LOCTEXT("FilteredCount", "{0} of {1}"), Group.FileCount, Group.Total);
		switch (Group.Group)
		{
		case FGitChangeItem::EGroup::Conflicts:
			return FText::Format(LOCTEXT("ConflictsGroup", "Conflicts · {0}"), Count);
		case FGitChangeItem::EGroup::Staged:
			return FText::Format(LOCTEXT("StagedGroup", "Staged · {0}"), Count);
		default:
			return FText::Format(LOCTEXT("UnstagedGroup", "Changes · {0}"), Count);
		}
	}

	/** The files of a group or folder, or the file itself. */
	TArray<SGitChangesView::FItemPtr> FilesUnder(const SGitChangesView::FItemPtr& Item)
	{
		TArray<SGitChangesView::FItemPtr> Files;
		if (Item->bGroup)
		{
			for (const SGitChangesView::FItemPtr& Child : Item->Children)
			{
				GitFileTree::CollectFiles(Child, Files);
			}
		}
		else
		{
			GitFileTree::CollectFiles(Item, Files);
		}
		return Files;
	}

	void SetGroup(const TArray<SGitChangesView::FItemPtr>& Items, FGitChangeItem::EGroup Group)
	{
		for (const SGitChangesView::FItemPtr& Item : Items)
		{
			Item->Group = Group;
			SetGroup(Item->Children, Group);
		}
	}

	void ForEachItem(const TArray<SGitChangesView::FItemPtr>& Items, TFunctionRef<void(const SGitChangesView::FItemPtr&)> Visit)
	{
		for (const SGitChangesView::FItemPtr& Item : Items)
		{
			Visit(Item);
			ForEachItem(Item->Children, Visit);
		}
	}

	/** One side of a changed file, or a folder of them. */
	class SGitChangeRow : public SMultiColumnTableRow<SGitChangesView::FItemPtr>
	{
	public:
		SLATE_BEGIN_ARGS(SGitChangeRow) {}
			SLATE_ARGUMENT(SGitChangesView::FItemPtr, Item)
			SLATE_ARGUMENT(TWeakPtr<SGitChangesView>, View)
		SLATE_END_ARGS()

		void Construct(const FArguments& InArgs, const TSharedRef<STableViewBase>& Owner)
		{
			Item = InArgs._Item;
			View = InArgs._View;
			SMultiColumnTableRow<SGitChangesView::FItemPtr>::Construct(FSuperRowType::FArguments().Padding(FMargin(0.0f, 2.0f)), Owner);
		}

		virtual FReply OnPreviewMouseButtonDown(const FGeometry& Geometry, const FPointerEvent& Event) override
		{
			return Item->bFolder ? GitFileTree::ToggleOnAltClick(*this, Event) : FReply::Unhandled();
		}

		virtual TSharedRef<SWidget> GenerateWidgetForColumn(const FName& Column) override
		{
			const TSharedPtr<SGitChangesView> Pinned = View.Pin();
			if (!Pinned.IsValid())
			{
				return SNullWidget::NullWidget;
			}
			const TAttribute<FText> Highlight = TAttribute<FText>::CreateLambda([Weak = View]()
			{
				const TSharedPtr<SGitChangesView> Owner = Weak.Pin();
				return Owner.IsValid() ? Owner->GetFilterText() : FText::GetEmpty();
			});
			if (Item->bFolder)
			{
				if (Column == NameColumn)
				{
					return GitFileTree::MakeNameCell(SharedThis(this), true, FText::FromString(Item->Label), FText::AsNumber(Item->FileCount), nullptr,
						FText::Format(LOCTEXT("FolderTooltip", "{0}\n{1} {1}|plural(one=file,other=files). Alt+click opens or closes everything in it."),
							FText::FromString(Item->Path), Item->FileCount), Highlight);
				}
				return Column == ActionColumn ? MakeStageButton() : SNullWidget::NullWidget;
			}
			const GitCore::FStatusEntry& Entry = Item->Entry;
			if (Column == NameColumn)
			{
				const FGitFileDisplay& Display = Pinned->GetDisplay(Entry.Path);
				const FText Tooltip = FText::FromString(Entry.OriginalPath.IsEmpty() ? Entry.Path : Entry.OriginalPath + TEXT(" → ") + Entry.Path);
				// Narrow docks hide the Change and Lock columns; their icons move here.
				const TSharedRef<SWidget> Badges = SNew(SHorizontalBox)
					.Visibility_Lambda([this]() { const TSharedPtr<SGitChangesView> Owner = View.Pin(); return Owner && Owner->IsCompact() ? EVisibility::Visible : EVisibility::Collapsed; })
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.Padding(4.0f, 0.0f, 0.0f, 0.0f)
					[
						SNew(SImage)
						.Image(GitWorkspaceText::ChangeIcon(Entry, Item->IsStagedSide()))
						.DesiredSizeOverride(FVector2D(14.0f, 14.0f))
						.ToolTipText(GitWorkspaceText::ChangeKind(Entry, Item->IsStagedSide()))
					]
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.Padding(4.0f, 0.0f)
					[
						SNew(SImage)
						.Image(FAppStyle::GetBrush("Icons.Lock"))
						.DesiredSizeOverride(FVector2D(12.0f, 12.0f))
						.ColorAndOpacity(FSlateColor::UseSubduedForeground())
						.ToolTipText(FText::Format(LOCTEXT("LockBadgeTooltip", "Locked by {0}"), Pinned->GetLockText(Entry.Path)))
						.Visibility(Pinned->GetLockText(Entry.Path).IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible)
					];
				// The tree shows the folder; a flat list names it next to the file.
				return GitFileTree::MakeNameCell(SharedThis(this), false, Display.Name, GitFileTree::ShowFolders() ? FText::GetEmpty() : Display.Folder, Display.Icon,
					Tooltip, Highlight, Badges);
			}
			if (Column == ChangeColumn)
			{
				const bool bStaged = Item->IsStagedSide();
				return SNew(SHorizontalBox)
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					.Padding(0.0f, 0.0f, 4.0f, 0.0f)
					[
						SNew(SImage)
						.Image(GitWorkspaceText::ChangeIcon(Entry, bStaged))
						.DesiredSizeOverride(FVector2D(16.0f, 16.0f))
					]
					+ SHorizontalBox::Slot()
					.FillWidth(1.0f)
					.VAlign(VAlign_Center)
					[
						SNew(STextBlock)
						.Text(GitWorkspaceText::ChangeKind(Entry, bStaged))
						.ToolTipText(bStaged && Entry.HasUnstaged() ? LOCTEXT("LaterEdits", "This file also has later changes that are not staged.") : FText::GetEmpty())
					];
			}
			if (Column == LockColumn)
			{
				const FText Lock = Pinned->GetLockText(Entry.Path);
				return SNew(SHorizontalBox)
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					.Padding(0.0f, 0.0f, 4.0f, 0.0f)
					[
						SNew(SImage)
						.Image(FAppStyle::GetBrush("Icons.Lock"))
						.DesiredSizeOverride(FVector2D(12.0f, 12.0f))
						.ColorAndOpacity(FSlateColor::UseSubduedForeground())
						.Visibility(Lock.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible)
					]
					+ SHorizontalBox::Slot()
					.FillWidth(1.0f)
					.VAlign(VAlign_Center)
					[
						SNew(STextBlock)
						.Text(Lock)
						.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
					];
			}
			return MakeStageButton();
		}

	private:
		/** Stage or unstage this file (a folder: its files); shown while hovering. */
		TSharedRef<SWidget> MakeStageButton()
		{
			const bool bStaged = Item->IsStagedSide();
			const FText Tooltip = Item->bFolder
				? (bStaged ? LOCTEXT("UnstageFolder", "Unstage the files in this folder") : LOCTEXT("StageFolder", "Stage the files in this folder"))
				: (bStaged ? LOCTEXT("UnstageOne", "Unstage") : LOCTEXT("StageOne", "Stage"));
			return SNew(SBox)
				.HAlign(HAlign_Center)
				.Visibility_Lambda([this]() { return IsHovered() && Item->Group != FGitChangeItem::EGroup::Conflicts ? EVisibility::Visible : EVisibility::Hidden; })
				[
					SNew(SButton)
					.ButtonStyle(FAppStyle::Get(), "SimpleButton")
					.ToolTipText(Tooltip)
					.OnClicked_Lambda([this]()
					{
						if (const TSharedPtr<SGitChangesView> Owner = View.Pin())
						{
							Owner->ToggleStaged(Item);
						}
						return FReply::Handled();
					})
					[
						SNew(SImage)
						.Image(FAppStyle::GetBrush(bStaged ? "Icons.Minus" : "Icons.Plus"))
						.ColorAndOpacity(FSlateColor::UseForeground())
					]
				];
		}

		SGitChangesView::FItemPtr Item;
		TWeakPtr<SGitChangesView> View;
	};

	/** A group's header row; Alt+click opens or closes the group with all its folders. */
	class SGitGroupRow : public STableRow<SGitChangesView::FItemPtr>
	{
	public:
		SLATE_BEGIN_ARGS(SGitGroupRow) {}
		SLATE_END_ARGS()

		void Construct(const FArguments& InArgs, const TSharedRef<STableViewBase>& Owner)
		{
			STableRow<SGitChangesView::FItemPtr>::Construct(STableRow<SGitChangesView::FItemPtr>::FArguments().ShowSelection(false), Owner);
		}

		virtual FReply OnPreviewMouseButtonDown(const FGeometry& Geometry, const FPointerEvent& Event) override
		{
			return GitFileTree::ToggleOnAltClick(*this, Event);
		}
	};
}

void SGitChangesView::Construct(const FArguments& InArgs, const TSharedRef<FGitWorkspaceModel>& InModel)
{
	WeakModel = InModel;
	ChangedHandle = InModel->OnChanged().AddSP(this, &SGitChangesView::OnModelChanged);
	bBuiltFolders = GitFileTree::ShowFolders();

	ChildSlot
	[
		SNew(SVerticalBox)
		// Filter row
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(4.0f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			[
				SAssignNew(SearchBox, SSearchBox)
				.AddMetaData<FTagMetaData>(TEXT("GitPanel.Changes.Search"))
				.HintText(LOCTEXT("SearchHint", "Search changed files (Ctrl+F)"))
				.OnTextChanged_Lambda([this](const FText& Text)
				{
					Filter = Text.ToString();
					BuiltGeneration = MAX_uint64;
					Rebuild();
				})
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(4.0f, 0.0f, 0.0f, 0.0f)
			[
				GitFileTree::MakeFoldersToggle()
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(8.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(SCheckBox)
				.ToolTipText(LOCTEXT("ContentOnlyTooltip", "Only show files under Content/."))
				.IsChecked_Lambda([this]() { return bContentOnly ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
				.OnCheckStateChanged_Lambda([this](ECheckBoxState State)
				{
					bContentOnly = State == ECheckBoxState::Checked;
					BuiltGeneration = MAX_uint64;
					Rebuild();
				})
				[
					SNew(STextBlock)
					.Text(LOCTEXT("ContentOnly", "Content only"))
				]
			]
		]
		// List and inspector
		+ SVerticalBox::Slot()
		.FillHeight(1.0f)
		[
			SNew(SSplitter)
			.Orientation(Orient_Horizontal)
			+ SSplitter::Slot()
			.Value(0.68f)
			[
				SAssignNew(Tree, STreeView<FItemPtr>)
				.TreeItemsSource(&Groups)
				.SelectionMode(ESelectionMode::Multi)
				.OnGenerateRow(this, &SGitChangesView::OnGenerateRow)
				.OnGetChildren(this, &SGitChangesView::OnGetChildren)
				.OnExpansionChanged(this, &SGitChangesView::OnExpansionChanged)
				.OnSetExpansionRecursive(this, &SGitChangesView::OnSetExpansionRecursive)
				.OnMouseButtonDoubleClick(this, &SGitChangesView::OnDoubleClick)
				.OnContextMenuOpening(this, &SGitChangesView::OnContextMenu)
				.OnIsSelectableOrNavigable_Lambda([](FItemPtr Item) { return Item.IsValid() && !Item->bGroup; })
				.HeaderRow
				(
					SAssignNew(HeaderRow, SHeaderRow)
					+ SHeaderRow::Column(NameColumn)
					.DefaultLabel(LOCTEXT("NameColumn", "Name"))
					.FillWidth(0.55f)
					+ SHeaderRow::Column(ChangeColumn)
					.DefaultLabel(LOCTEXT("ChangeColumn", "Change"))
					.FillWidth(0.18f)
					+ SHeaderRow::Column(LockColumn)
					.DefaultLabel(LOCTEXT("LockColumn", "Lock"))
					.FillWidth(0.27f)
					+ SHeaderRow::Column(ActionColumn)
					.DefaultLabel(FText::GetEmpty())
					.FixedWidth(28.0f)
				)
			]
			+ SSplitter::Slot()
			.Value(0.32f)
			.MinSize(240.0f)
			[
				SNew(SBox)
				.Visibility_Lambda([this]()
				{
					// Narrow docks keep the whole width for the list.
					return GetCachedGeometry().GetLocalSize().X >= 720.0f ? EVisibility::Visible : EVisibility::Collapsed;
				})
				[
					MakeInspector()
				]
			]
		]
		// Commit box
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			MakeCommitBar()
		]
	];
	Rebuild();
}

SGitChangesView::~SGitChangesView()
{
	if (const TSharedPtr<FGitWorkspaceModel> Model = WeakModel.Pin())
	{
		Model->OnChanged().Remove(ChangedHandle);
	}
}

void SGitChangesView::OnModelChanged()
{
	Rebuild();
}

void SGitChangesView::Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime)
{
	SCompoundWidget::Tick(AllottedGeometry, InCurrentTime, InDeltaTime);
	const bool bNowCompact = AllottedGeometry.GetLocalSize().X < 560.0f;
	if (bNowCompact != bCompact && HeaderRow.IsValid())
	{
		bCompact = bNowCompact;
		HeaderRow->SetShowGeneratedColumn(ChangeColumn, !bCompact);
		HeaderRow->SetShowGeneratedColumn(LockColumn, !bCompact);
		Tree->RebuildList();
	}
	// The folders preference is shared with the other tabs.
	if (GitFileTree::ShowFolders() != bBuiltFolders)
	{
		Rebuild();
	}
}

bool SGitChangesView::Matches(const GitCore::FStatusEntry& Entry)
{
	if (bContentOnly && !Entry.Path.StartsWith(TEXT("Content/")))
	{
		return false;
	}
	if (Filter.IsEmpty())
	{
		return true;
	}
	return Entry.Path.Contains(Filter) || GetDisplay(Entry.Path).Name.ToString().Contains(Filter);
}

FString SGitChangesView::ExpansionKey(const FGitChangeItem& Item) const
{
	return FString::Printf(TEXT("%d|%s"), static_cast<int32>(Item.Group), *Item.Path);
}

void SGitChangesView::Rebuild()
{
	const TSharedPtr<FGitWorkspaceModel> Model = WeakModel.Pin();
	if (!Model.IsValid() || !Tree.IsValid())
	{
		return;
	}
	const GitCore::FSessionSnapshotRef Snapshot = Model->GetSnapshot();
	const bool bFolders = GitFileTree::ShowFolders();
	if (Snapshot->Generation == BuiltGeneration && bFolders == bBuiltFolders)
	{
		return;
	}
	if (BuiltGeneration != MAX_uint64 && Snapshot->Generation != BuiltGeneration)
	{
		// Asset names can change with any refresh.
		Displays.Reset();
	}
	BuiltGeneration = Snapshot->Generation;
	bBuiltFolders = bFolders;

	TSet<FString> Selected;
	for (const FItemPtr& Item : Tree->GetSelectedItems())
	{
		Selected.Add(ExpansionKey(*Item));
	}
	auto MakeGroup = [](FGitChangeItem::EGroup Group)
	{
		const FItemPtr Item = MakeShared<FGitChangeItem>();
		Item->bGroup = true;
		Item->Group = Group;
		return Item;
	};
	const FItemPtr Conflicts = MakeGroup(FGitChangeItem::EGroup::Conflicts);
	const FItemPtr Staged = MakeGroup(FGitChangeItem::EGroup::Staged);
	const FItemPtr Unstaged = MakeGroup(FGitChangeItem::EGroup::Unstaged);
	TArray<FItemPtr> ConflictFiles;
	TArray<FItemPtr> StagedFiles;
	TArray<FItemPtr> UnstagedFiles;
	auto Add = [this](const FItemPtr& Group, TArray<FItemPtr>& Files, const GitCore::FStatusEntry& Entry)
	{
		++Group->Total;
		if (Matches(Entry))
		{
			const FItemPtr Item = MakeShared<FGitChangeItem>();
			Item->Group = Group->Group;
			Item->Path = Entry.Path;
			Item->Entry = Entry;
			Files.Add(Item);
		}
	};
	for (const GitCore::FStatusEntry& Entry : Snapshot->Status.Entries)
	{
		if (Entry.bConflict)
		{
			Add(Conflicts, ConflictFiles, Entry);
			continue;
		}
		if (Entry.HasStaged())
		{
			Add(Staged, StagedFiles, Entry);
		}
		if (Entry.HasUnstaged())
		{
			Add(Unstaged, UnstagedFiles, Entry);
		}
	}
	auto Arrange = [this, bFolders](const FItemPtr& Group, TArray<FItemPtr>& Files)
	{
		if (bFolders)
		{
			// The tree orders folders; inside one, files go by the name shown (actor labels for World Partition files).
			TArray<TPair<FString, FItemPtr>> Named;
			for (const FItemPtr& File : Files)
			{
				Named.Emplace(GetDisplay(File->Path).Name.ToString(), File);
			}
			Named.Sort([](const TPair<FString, FItemPtr>& A, const TPair<FString, FItemPtr>& B)
			{
				const int32 Order = A.Key.Compare(B.Key, ESearchCase::IgnoreCase);
				return Order != 0 ? Order < 0 : A.Value->Path < B.Value->Path;
			});
			Files.Reset();
			for (const TPair<FString, FItemPtr>& Pair : Named)
			{
				Files.Add(Pair.Value);
			}
		}
		else
		{
			Files.Sort([](const FItemPtr& A, const FItemPtr& B) { return A->Path < B->Path; });
		}
		Group->FileCount = Files.Num();
		Group->Children = GitFileTree::Build(Files, bFolders);
		SetGroup(Group->Children, Group->Group);
	};
	Arrange(Conflicts, ConflictFiles);
	Arrange(Staged, StagedFiles);
	Arrange(Unstaged, UnstagedFiles);

	Groups.Reset();
	if (Conflicts->Total > 0)
	{
		Groups.Add(Conflicts);
	}
	Groups.Add(Staged);
	Groups.Add(Unstaged);

	// "Show in Git panel" opens the folders on the way to each path.
	TArray<FItemPtr> Reselect;
	for (const FString& Path : PendingSelection)
	{
		for (const FItemPtr& Group : Groups)
		{
			TArray<FItemPtr> Chain;
			if (GitFileTree::FindPath<FGitChangeItem>(Group->Children, [&Path](const FGitChangeItem& Item) { return Item.Path == Path; }, Chain))
			{
				CollapsedGroups.Remove(Group->Group);
				for (const FItemPtr& Folder : Chain)
				{
					CollapsedFolders.Remove(ExpansionKey(*Folder));
				}
				Reselect.Add(Chain.Last());
			}
		}
	}
	const bool bSearching = !Filter.IsEmpty();
	bRestoringExpansion = true;
	for (const FItemPtr& Group : Groups)
	{
		Tree->SetItemExpansion(Group, bSearching || !CollapsedGroups.Contains(Group->Group));
		GitFileTree::RestoreExpansion(*Tree, Group->Children, CollapsedFolders, bSearching, FString::Printf(TEXT("%d|"), static_cast<int32>(Group->Group)));
	}
	bRestoringExpansion = false;

	ForEachItem(Groups, [this, &Selected, &Reselect](const FItemPtr& Item)
	{
		if (!Item->bGroup && Selected.Contains(ExpansionKey(*Item)))
		{
			Reselect.AddUnique(Item);
		}
	});
	Tree->ClearSelection();
	Tree->RequestTreeRefresh();
	if (!Reselect.IsEmpty())
	{
		Tree->SetItemSelection(Reselect, true);
		if (!PendingSelection.IsEmpty())
		{
			Tree->RequestScrollIntoView(Reselect[0]);
		}
	}
	PendingSelection.Reset();
}

void SGitChangesView::SelectPaths(const TArray<FString>& Paths)
{
	PendingSelection = Paths;
	Filter.Reset();
	if (SearchBox.IsValid())
	{
		SearchBox->SetText(FText::GetEmpty());
	}
	BuiltGeneration = MAX_uint64;
	Rebuild();
}

void SGitChangesView::FocusSearch()
{
	if (SearchBox.IsValid())
	{
		FSlateApplication::Get().SetKeyboardFocus(SearchBox, EFocusCause::SetDirectly);
	}
}

const FGitFileDisplay& SGitChangesView::GetDisplay(const FString& Path)
{
	return Displays.Get(WeakModel.Pin().Get(), Path);
}

FText SGitChangesView::GetLockText(const FString& Path) const
{
	const TSharedPtr<FGitWorkspaceModel> Model = WeakModel.Pin();
	if (!Model.IsValid())
	{
		return FText::GetEmpty();
	}
	const GitCore::FSessionSnapshotRef Snapshot = Model->GetSnapshot();
	const GitCore::FLockInfo* Lock = Snapshot->Locks.Find(Path);
	if (!Lock)
	{
		return FText::GetEmpty();
	}
	if (Lock->bOurs)
	{
		return Snapshot->LocksTakenHere.Contains(Lock->Id) ? LOCTEXT("LockedByYou", "You") : LOCTEXT("LockedByYouElsewhere", "You, in another clone");
	}
	return Lock->LockedAt.GetTicks() > 0
		? FText::Format(LOCTEXT("LockedByOwner", "{0} · {1}"), FText::FromString(Lock->Owner), GitWorkspaceText::Age(Lock->LockedAt))
		: FText::FromString(Lock->Owner);
}

TSharedRef<ITableRow> SGitChangesView::OnGenerateRow(FItemPtr Item, const TSharedRef<STableViewBase>& Owner)
{
	if (!Item->bGroup)
	{
		return SNew(SGitChangeRow, Owner)
			.Item(Item)
			.View(SharedThis(this));
	}
	const bool bStagedGroup = Item->Group == FGitChangeItem::EGroup::Staged;
	const TSharedRef<SGitGroupRow> Row = SNew(SGitGroupRow, Owner);
	// The whole row, replacing the expander STableRow adds in trees: this one indents like the file rows.
	Row->SetRowContent(
		SNew(SBox)
		.Padding(FMargin(0.0f, 4.0f))
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			[
				SNew(SExpanderArrow, Row)
				.IndentAmount(12.0f)
			]
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text(GroupLabel(*Item))
				.Font(FAppStyle::GetFontStyle("NormalFontBold"))
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(4.0f, 0.0f)
			[
				SNew(STextBlock)
				.Text(bStagedGroup ? LOCTEXT("StagedEmpty", "Stage files to commit them.") : LOCTEXT("UnstagedEmpty", "No changes."))
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				.Visibility(Item->Total == 0 && Item->Group != FGitChangeItem::EGroup::Conflicts ? EVisibility::Visible : EVisibility::Collapsed)
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				SNew(SButton)
				.ButtonStyle(FAppStyle::Get(), "SimpleButton")
				.Visibility(Item->Children.IsEmpty() || Item->Group == FGitChangeItem::EGroup::Conflicts ? EVisibility::Collapsed : EVisibility::Visible)
				.Text(bStagedGroup ? LOCTEXT("UnstageAll", "Unstage all") : LOCTEXT("StageAll", "Stage all"))
				.ToolTipText(bStagedGroup ? LOCTEXT("UnstageAllTooltip", "Move every listed file back to Changes.") : LOCTEXT("StageAllTooltip", "Stage every listed file for the next commit."))
				.OnClicked_Lambda([this, Item]()
				{
					ToggleStaged(Item);
					return FReply::Handled();
				})
			]
		]);
	return Row;
}

void SGitChangesView::OnGetChildren(FItemPtr Item, TArray<FItemPtr>& OutChildren) const
{
	if (Item.IsValid())
	{
		OutChildren = Item->Children;
	}
}

void SGitChangesView::OnExpansionChanged(FItemPtr Item, bool bExpanded)
{
	if (!Item.IsValid() || bRestoringExpansion)
	{
		return;
	}
	if (Item->bGroup)
	{
		if (bExpanded)
		{
			CollapsedGroups.Remove(Item->Group);
		}
		else
		{
			CollapsedGroups.Add(Item->Group);
		}
	}
	else if (Item->bFolder)
	{
		if (bExpanded)
		{
			CollapsedFolders.Remove(ExpansionKey(*Item));
		}
		else
		{
			CollapsedFolders.Add(ExpansionKey(*Item));
		}
	}
}

void SGitChangesView::OnSetExpansionRecursive(FItemPtr Item, bool bExpand)
{
	if (Tree.IsValid())
	{
		GitFileTree::SetExpansionRecursive(*Tree, Item, bExpand);
	}
}

void SGitChangesView::OnDoubleClick(FItemPtr Item)
{
	const TSharedPtr<FGitWorkspaceModel> Model = WeakModel.Pin();
	if (!Model.IsValid() || !Item.IsValid())
	{
		return;
	}
	if (!Item->IsFile())
	{
		Tree->SetItemExpansion(Item, !Tree->IsItemExpanded(Item));
		return;
	}
	const GitCore::FStatusEntry& Entry = Item->Entry;
	// Edited files open their comparison; new files open the file itself.
	if (Entry.IsAdded() || Entry.IsDeleted() || Entry.bConflict)
	{
		Model->OpenFile(Entry.Path);
	}
	else
	{
		Model->ShowDiff(Entry.Path, Item->IsStagedSide());
	}
}

TArray<SGitChangesView::FItemPtr> SGitChangesView::SelectedFiles() const
{
	if (!Tree.IsValid())
	{
		return TArray<FItemPtr>();
	}
	return GitFileTree::SelectedFiles(Tree->GetSelectedItems().FilterByPredicate([](const FItemPtr& Item) { return Item.IsValid() && !Item->bGroup; }));
}

TArray<SGitChangesView::FItemPtr> SGitChangesView::SelectedFolders() const
{
	return Tree.IsValid() ? Tree->GetSelectedItems().FilterByPredicate([](const FItemPtr& Item) { return Item.IsValid() && Item->bFolder; }) : TArray<FItemPtr>();
}

TArray<FString> SGitChangesView::SelectedPaths(bool bStagedSide) const
{
	TArray<FString> Paths;
	for (const FItemPtr& Item : SelectedFiles())
	{
		if (Item->IsStagedSide() == bStagedSide && Item->Group != FGitChangeItem::EGroup::Conflicts)
		{
			Paths.AddUnique(Item->Entry.Path);
		}
	}
	return Paths;
}

TArray<FString> SGitChangesView::SelectedPaths() const
{
	TArray<FString> Paths;
	for (const FItemPtr& Item : SelectedFiles())
	{
		Paths.AddUnique(Item->Entry.Path);
	}
	return Paths;
}

SGitChangesView::FItemPtr SGitChangesView::SingleSelection() const
{
	const TArray<FItemPtr> Picked = Tree.IsValid() ? Tree->GetSelectedItems() : TArray<FItemPtr>();
	return Picked.Num() == 1 && Picked[0].IsValid() && Picked[0]->IsFile() ? Picked[0] : nullptr;
}

void SGitChangesView::ToggleStaged(const FItemPtr& Item)
{
	const TSharedPtr<FGitWorkspaceModel> Model = WeakModel.Pin();
	if (!Model.IsValid() || !Item.IsValid())
	{
		return;
	}
	TArray<FString> Paths;
	for (const FItemPtr& File : FilesUnder(Item))
	{
		Paths.Add(File->Entry.Path);
	}
	if (!Paths.IsEmpty())
	{
		Item->IsStagedSide() ? Model->Unstage(Paths) : Model->Stage(Paths);
	}
}

void SGitChangesView::StageSelection()
{
	if (const TSharedPtr<FGitWorkspaceModel> Model = WeakModel.Pin())
	{
		Model->Stage(SelectedPaths(false));
	}
}

void SGitChangesView::UnstageSelection()
{
	if (const TSharedPtr<FGitWorkspaceModel> Model = WeakModel.Pin())
	{
		Model->Unstage(SelectedPaths(true));
	}
}

void SGitChangesView::DiscardSelection()
{
	const TSharedPtr<FGitWorkspaceModel> Model = WeakModel.Pin();
	const TArray<FString> Paths = SelectedPaths();
	if (!Model.IsValid() || Paths.IsEmpty() || Model->IsBusy())
	{
		return;
	}
	const FText Message = FText::Format(LOCTEXT("DiscardMessage",
		"Discard your changes to {0} {0}|plural(one=file,other=files)? Edited files go back to their last committed version and new files are deleted. A copy of edited files is kept in Saved/GitSourceControl/Backups."),
		Paths.Num());
	if (GitWorkspaceDialogs::Confirm(LOCTEXT("DiscardTitle", "Discard changes"), Message, Paths, LOCTEXT("DiscardButton", "Discard")))
	{
		Model->Discard(Paths);
	}
}

void SGitChangesView::DiffSelection()
{
	const TSharedPtr<FGitWorkspaceModel> Model = WeakModel.Pin();
	if (const FItemPtr Item = SingleSelection(); Model.IsValid() && Item.IsValid())
	{
		Model->ShowDiff(Item->Entry.Path, Item->IsStagedSide());
	}
}

void SGitChangesView::LockSelection(bool bLock, bool bForce)
{
	const TSharedPtr<FGitWorkspaceModel> Model = WeakModel.Pin();
	const TArray<FString> Paths = SelectedPaths();
	if (!Model.IsValid() || Paths.IsEmpty())
	{
		return;
	}
	if (bLock)
	{
		Model->Lock(Paths);
		return;
	}
	if (bForce && !GitWorkspaceDialogs::Confirm(LOCTEXT("ForceUnlockTitle", "Release someone else's lock"),
		LOCTEXT("ForceUnlockMessage", "Release these locks even though other people hold them? They may lose work if they are still editing."), Paths,
		LOCTEXT("ForceUnlockButton", "Release locks")))
	{
		return;
	}
	Model->Unlock(Paths, bForce);
}

TSharedPtr<SWidget> SGitChangesView::OnContextMenu()
{
	const TSharedPtr<FGitWorkspaceModel> Model = WeakModel.Pin();
	const TArray<FItemPtr> Files = SelectedFiles();
	if (!Model.IsValid() || Files.IsEmpty())
	{
		return nullptr;
	}
	const GitCore::FSessionSnapshotRef Snapshot = Model->GetSnapshot();
	const bool bAnyUnstaged = !SelectedPaths(false).IsEmpty();
	const bool bAnyStaged = !SelectedPaths(true).IsEmpty();
	bool bAnyMine = false;
	bool bAnyTheirs = false;
	bool bAnyLockable = false;
	for (const FItemPtr& Item : Files)
	{
		if (const GitCore::FLockInfo* Lock = Snapshot->Locks.Find(Item->Entry.Path))
		{
			(Lock->bOurs ? bAnyMine : bAnyTheirs) = true;
		}
		const GitCore::FFileAttributes* Attributes = Snapshot->FindAttributes(Item->Entry.Path);
		bAnyLockable |= Attributes && Attributes->bLockable && !Item->Entry.IsAdded();
	}
	const FItemPtr Single = SingleSelection();
	const TArray<FItemPtr> Folders = SelectedFolders();
	const FName StyleName = FRevisionControlStyleManager::GetStyleSetName();

	FMenuBuilder Menu(true, nullptr);
	Menu.BeginSection("Stage", LOCTEXT("StageSection", "Commit"));
	if (bAnyUnstaged)
	{
		Menu.AddMenuEntry(LOCTEXT("StageMenu", "Stage"), LOCTEXT("StageMenuTooltip", "Include these changes in the next commit. (Space)"),
			FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Plus"), FUIAction(FExecuteAction::CreateSP(this, &SGitChangesView::StageSelection)));
	}
	if (bAnyStaged)
	{
		Menu.AddMenuEntry(LOCTEXT("UnstageMenu", "Unstage"), LOCTEXT("UnstageMenuTooltip", "Leave these changes out of the next commit. (Space)"),
			FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Minus"), FUIAction(FExecuteAction::CreateSP(this, &SGitChangesView::UnstageSelection)));
	}
	Menu.AddMenuEntry(LOCTEXT("DiscardMenu", "Discard changes…"), LOCTEXT("DiscardMenuTooltip", "Go back to the last committed version. (Delete)"),
		FSlateIcon(StyleName, "RevisionControl.Actions.Revert"), FUIAction(FExecuteAction::CreateSP(this, &SGitChangesView::DiscardSelection)));
	Menu.EndSection();

	Menu.BeginSection("Inspect", LOCTEXT("InspectSection", "Inspect"));
	if (Single.IsValid())
	{
		const FString Path = Single->Entry.Path;
		Menu.AddMenuEntry(LOCTEXT("DiffMenu", "Compare with last commit"), LOCTEXT("DiffMenuTooltip", "Show what changed. (Ctrl+D)"),
			FSlateIcon(StyleName, "RevisionControl.Actions.Diff"), FUIAction(FExecuteAction::CreateSP(this, &SGitChangesView::DiffSelection)));
		Menu.AddMenuEntry(LOCTEXT("HistoryMenu", "Show history"), LOCTEXT("HistoryMenuTooltip", "Earlier versions of this asset."),
			FSlateIcon(StyleName, "RevisionControl.Actions.History"),
			FUIAction(FExecuteAction::CreateLambda([Weak = WeakModel, Path]() { if (const TSharedPtr<FGitWorkspaceModel> M = Weak.Pin()) { M->ShowHistory(Path); } }),
				FCanExecuteAction::CreateLambda([this, Path]() { return GetDisplay(Path).bPackage; })));
		Menu.AddMenuEntry(LOCTEXT("OpenMenu", "Open"), LOCTEXT("OpenMenuTooltip", "Open the asset editor, or the file in its default application."),
			FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.OpenInExternalEditor"),
			FUIAction(FExecuteAction::CreateLambda([Weak = WeakModel, Path]() { if (const TSharedPtr<FGitWorkspaceModel> M = Weak.Pin()) { M->OpenFile(Path); } })));
	}
	Menu.AddMenuEntry(LOCTEXT("BrowseMenu", "Show in Content Browser"), LOCTEXT("BrowseMenuTooltip", "Select these assets in the Content Browser (other files open in the file browser)."),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.BrowseContent"),
		FUIAction(FExecuteAction::CreateLambda([this]() { if (const TSharedPtr<FGitWorkspaceModel> M = WeakModel.Pin()) { M->BrowseTo(SelectedPaths()); } })));
	Menu.AddMenuEntry(LOCTEXT("CopyPathMenu", "Copy path"), FText::GetEmpty(), FSlateIcon(FAppStyle::GetAppStyleSetName(), "GenericCommands.Copy"),
		FUIAction(FExecuteAction::CreateLambda([this]()
		{
			// Selected folders copy their own path, files theirs.
			TArray<FString> Paths;
			for (const FItemPtr& Item : Tree->GetSelectedItems())
			{
				if (!Item->bGroup)
				{
					Paths.AddUnique(Item->Path);
				}
			}
			FPlatformApplicationMisc::ClipboardCopy(*FString::Join(Paths, LINE_TERMINATOR));
		})));
	Menu.EndSection();

	if (!Folders.IsEmpty())
	{
		Menu.BeginSection("Folders", LOCTEXT("FoldersSection", "Folders"));
		Menu.AddMenuEntry(LOCTEXT("ExpandAllMenu", "Open all inside"), LOCTEXT("ExpandAllMenuTooltip", "Open these folders and every folder in them. (Alt+click)"),
			FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.FolderOpen"),
			FUIAction(FExecuteAction::CreateLambda([this, Folders]() { for (const FItemPtr& Folder : Folders) { OnSetExpansionRecursive(Folder, true); } })));
		Menu.AddMenuEntry(LOCTEXT("CollapseAllMenu", "Close all inside"), LOCTEXT("CollapseAllMenuTooltip", "Close these folders and every folder in them. (Alt+click)"),
			FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.FolderClosed"),
			FUIAction(FExecuteAction::CreateLambda([this, Folders]() { for (const FItemPtr& Folder : Folders) { OnSetExpansionRecursive(Folder, false); } })));
		Menu.EndSection();
	}

	if (Model->IsLockingEnabled() && (bAnyLockable || bAnyMine || bAnyTheirs))
	{
		Menu.BeginSection("Locks", LOCTEXT("LocksSection", "Lock"));
		if (bAnyLockable)
		{
			Menu.AddMenuEntry(LOCTEXT("LockMenu", "Lock (check out)"), LOCTEXT("LockMenuTooltip", "Take the lock so nobody else edits these assets."),
				FSlateIcon(StyleName, "RevisionControl.Actions.CheckOut"), FUIAction(FExecuteAction::CreateSP(this, &SGitChangesView::LockSelection, true, false)));
		}
		if (bAnyMine)
		{
			Menu.AddMenuEntry(LOCTEXT("UnlockMenu", "Release lock"), LOCTEXT("UnlockMenuTooltip", "Let others edit these assets. Your local changes stay."),
				FSlateIcon(StyleName, "RevisionControl.Unlocked"), FUIAction(FExecuteAction::CreateSP(this, &SGitChangesView::LockSelection, false, false)));
		}
		if (bAnyTheirs)
		{
			Menu.AddMenuEntry(LOCTEXT("ForceUnlockMenu", "Release someone else's lock…"), LOCTEXT("ForceUnlockMenuTooltip", "Only when its owner cannot release it."),
				FSlateIcon(StyleName, "RevisionControl.Unlocked"), FUIAction(FExecuteAction::CreateSP(this, &SGitChangesView::LockSelection, false, true)));
		}
		Menu.EndSection();
	}
	return Menu.MakeWidget();
}

FReply SGitChangesView::OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent)
{
	const FKey Key = InKeyEvent.GetKey();
	const bool bControl = InKeyEvent.IsControlDown() || InKeyEvent.IsCommandDown();
	if (Key == EKeys::SpaceBar && !SelectedFiles().IsEmpty())
	{
		// Space moves the selection to the other group.
		const TArray<FString> Unstaged = SelectedPaths(false);
		Unstaged.IsEmpty() ? UnstageSelection() : StageSelection();
		return FReply::Handled();
	}
	if ((Key == EKeys::Delete || Key == EKeys::BackSpace) && !SelectedFiles().IsEmpty())
	{
		DiscardSelection();
		return FReply::Handled();
	}
	if (Key == EKeys::Enter)
	{
		const TArray<FItemPtr> Picked = Tree->GetSelectedItems();
		if (Picked.Num() == 1)
		{
			// A file opens; a folder opens or closes.
			OnDoubleClick(Picked[0]);
			return FReply::Handled();
		}
	}
	if (bControl && Key == EKeys::D)
	{
		DiffSelection();
		return FReply::Handled();
	}
	return SCompoundWidget::OnKeyDown(MyGeometry, InKeyEvent);
}

TSharedRef<SWidget> SGitChangesView::MakeInspector()
{
	auto Selected = [this]() { return SingleSelection(); };
	auto Button = [](const FText& Label, const FText& Tooltip, TFunction<void()> Action, TFunction<bool()> IsEnabled = nullptr) -> TSharedRef<SWidget>
	{
		return SNew(SButton)
			.Text(Label)
			.ToolTipText(Tooltip)
			.IsEnabled_Lambda([IsEnabled]() { return !IsEnabled || IsEnabled(); })
			.OnClicked_Lambda([Action]() { Action(); return FReply::Handled(); });
	};
	return SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("Brushes.Recessed"))
		.Padding(10.0f)
		[
			SNew(SVerticalBox)
			// Nothing or several files selected
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(STextBlock)
				.Visibility_Lambda([this]() { return SingleSelection().IsValid() ? EVisibility::Collapsed : EVisibility::Visible; })
				.Text_Lambda([this]()
				{
					const int32 Count = SelectedFiles().Num();
					return Count == 0 ? LOCTEXT("InspectorEmpty", "Select a file to see its details.")
						: FText::Format(LOCTEXT("InspectorMany", "{0} files selected. Space stages or unstages them; Delete discards their changes."), Count);
				})
				.AutoWrapText(true)
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			]
			// One file
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(SVerticalBox)
				.Visibility_Lambda([this]() { return SingleSelection().IsValid() ? EVisibility::Visible : EVisibility::Collapsed; })
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					.Padding(0.0f, 0.0f, 8.0f, 0.0f)
					[
						SNew(SImage)
						.DesiredSizeOverride(FVector2D(24.0f, 24.0f))
						.Image_Lambda([this, Selected]() { const FItemPtr Item = Selected(); return Item.IsValid() ? GetDisplay(Item->Entry.Path).Icon : nullptr; })
					]
					+ SHorizontalBox::Slot()
					.FillWidth(1.0f)
					.VAlign(VAlign_Center)
					[
						SNew(STextBlock)
						.Font(FAppStyle::GetFontStyle("HeadingExtraSmall"))
						.Text_Lambda([this, Selected]() { const FItemPtr Item = Selected(); return Item.IsValid() ? GetDisplay(Item->Entry.Path).Name : FText::GetEmpty(); })
						.AutoWrapText(true)
					]
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 4.0f, 0.0f, 0.0f)
				[
					SNew(STextBlock)
					.Text_Lambda([Selected]() { const FItemPtr Item = Selected(); return Item.IsValid() ? FText::FromString(Item->Entry.Path) : FText::GetEmpty(); })
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
					.AutoWrapText(true)
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 10.0f, 0.0f, 0.0f)
				[
					SNew(STextBlock)
					.AutoWrapText(true)
					.Text_Lambda([Selected]()
					{
						const FItemPtr Item = Selected();
						if (!Item.IsValid())
						{
							return FText::GetEmpty();
						}
						const GitCore::FStatusEntry& Entry = Item->Entry;
						if (Entry.HasStaged() && Entry.HasUnstaged())
						{
							return FText::Format(LOCTEXT("BothSides", "Staged: {0}. Later changes not staged: {1}."),
								GitWorkspaceText::ChangeKind(Entry, true), GitWorkspaceText::ChangeKind(Entry, false));
						}
						return FText::Format(Entry.HasStaged() ? LOCTEXT("StagedSide", "Staged: {0}.") : LOCTEXT("UnstagedSide", "Not staged: {0}."),
							GitWorkspaceText::ChangeKind(Entry, Entry.HasStaged()));
					})
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 4.0f, 0.0f, 0.0f)
				[
					SNew(STextBlock)
					.AutoWrapText(true)
					.Text_Lambda([this, Selected]()
					{
						const FItemPtr Item = Selected();
						const TSharedPtr<FGitWorkspaceModel> Model = WeakModel.Pin();
						if (!Item.IsValid() || !Model.IsValid() || !Model->IsLockingEnabled())
						{
							return FText::GetEmpty();
						}
						const GitCore::FSessionSnapshotRef Snapshot = Model->GetSnapshot();
						if (const GitCore::FLockInfo* Lock = Snapshot->Locks.Find(Item->Entry.Path))
						{
							if (Lock->bOurs)
							{
								return Snapshot->LocksTakenHere.Contains(Lock->Id) ? LOCTEXT("YouHoldLock", "You hold the lock.")
									: LOCTEXT("YouHoldLockElsewhere", "You hold the lock in another clone. Check out to use it here.");
							}
							return FText::Format(LOCTEXT("LockedBy", "Locked by {0}."), GetLockText(Item->Entry.Path));
						}
						const GitCore::FFileAttributes* Attributes = Model->GetSnapshot()->FindAttributes(Item->Entry.Path);
						return Attributes && Attributes->bLockable ? LOCTEXT("NotLocked", "Not locked.") : LOCTEXT("NotLockable", "Not lockable.");
					})
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 12.0f, 0.0f, 0.0f)
				[
					SNew(SWrapBox)
					.UseAllottedSize(true)
					.InnerSlotPadding(FVector2D(4.0f, 4.0f))
					+ SWrapBox::Slot()
					[
						Button(LOCTEXT("InspectorDiff", "Compare"), LOCTEXT("InspectorDiffTooltip", "Show what changed since the last commit."), [this]() { DiffSelection(); })
					]
					+ SWrapBox::Slot()
					[
						Button(LOCTEXT("InspectorHistory", "History"), LOCTEXT("InspectorHistoryTooltip", "Earlier versions of this asset."),
							[this, Selected]() { const FItemPtr Item = Selected(); if (const TSharedPtr<FGitWorkspaceModel> M = WeakModel.Pin(); M && Item) { M->ShowHistory(Item->Entry.Path); } },
							[this, Selected]() { const FItemPtr Item = Selected(); return Item.IsValid() && GetDisplay(Item->Entry.Path).bPackage; })
					]
					+ SWrapBox::Slot()
					[
						Button(LOCTEXT("InspectorBrowse", "Browse"), LOCTEXT("InspectorBrowseTooltip", "Show in the Content Browser."),
							[this]() { if (const TSharedPtr<FGitWorkspaceModel> M = WeakModel.Pin()) { M->BrowseTo(SelectedPaths()); } })
					]
					+ SWrapBox::Slot()
					[
						Button(LOCTEXT("InspectorOpen", "Open"), LOCTEXT("InspectorOpenTooltip", "Open the asset editor, or the file in its default application."),
							[this, Selected]() { const FItemPtr Item = Selected(); if (const TSharedPtr<FGitWorkspaceModel> M = WeakModel.Pin(); M && Item) { M->OpenFile(Item->Entry.Path); } })
					]
				]
			]
		];
}

TSharedRef<SWidget> SGitChangesView::MakeCommitBar()
{
	return SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("Brushes.Panel"))
		.Padding(6.0f)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(SBox)
				.MinDesiredHeight(54.0f)
				.MaxDesiredHeight(140.0f)
				[
					SAssignNew(MessageBox, SMultiLineEditableTextBox)
					.HintText(LOCTEXT("MessageHint", "Commit message (the first line is the summary)"))
					.AutoWrapText(true)
					.Text_Lambda([this]() { return FText::FromString(CommitMessage); })
					.OnTextChanged_Lambda([this](const FText& Text) { CommitMessage = Text.ToString(); })
					.OnKeyDownHandler_Lambda([this](const FGeometry&, const FKeyEvent& Event)
					{
						if (Event.GetKey() == EKeys::Enter && (Event.IsControlDown() || Event.IsCommandDown()))
						{
							Commit(Event.IsShiftDown());
							return FReply::Handled();
						}
						return FReply::Unhandled();
					})
				]
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 6.0f, 0.0f, 0.0f)
			[
				SNew(SWrapBox)
				.UseAllottedSize(true)
				.InnerSlotPadding(FVector2D(6.0f, 4.0f))
				+ SWrapBox::Slot()
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
					.AutoWrapText(true)
					.Text_Lambda([this]()
					{
						const TSharedPtr<FGitWorkspaceModel> Model = WeakModel.Pin();
						if (!Model.IsValid())
						{
							return FText::GetEmpty();
						}
						const GitCore::FSessionSnapshotRef Snapshot = Model->GetSnapshot();
						if (Snapshot->Status.HasConflicts())
						{
							return LOCTEXT("CommitConflicts", "Resolve the conflicts before committing.");
						}
						if (Snapshot->Status.StagedCount() == 0)
						{
							return LOCTEXT("CommitNothingStaged", "Stage files to commit them. Artists can use Check in in the status bar instead.");
						}
						return CommitMessage.TrimStartAndEnd().IsEmpty() ? LOCTEXT("CommitNeedsMessage", "Describe the change to commit it.") : FText::GetEmpty();
					})
				]
				// The buttons fill the rest of their line and sit at its right edge.
				+ SWrapBox::Slot()
				.FillEmptySpace(true)
				.HAlign(HAlign_Right)
				.VAlign(VAlign_Center)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot()
					.AutoWidth()
					[
						SNew(SButton)
						.Text(LOCTEXT("CommitAndPush", "Commit and push"))
						.ToolTipText(LOCTEXT("CommitAndPushTooltip", "Commit the staged files, then push every outgoing commit. (Ctrl+Shift+Enter)"))
						.IsEnabled(this, &SGitChangesView::CanCommit)
						.OnClicked_Lambda([this]() { Commit(true); return FReply::Handled(); })
					]
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.Padding(4.0f, 0.0f, 0.0f, 0.0f)
					[
						SNew(SButton)
						.ButtonStyle(FAppStyle::Get(), "PrimaryButton")
						.Text_Lambda([this]()
						{
							const TSharedPtr<FGitWorkspaceModel> Model = WeakModel.Pin();
							const int32 Count = Model.IsValid() ? Model->GetSnapshot()->Status.StagedCount() : 0;
							return FText::Format(LOCTEXT("CommitCount", "Commit {0}"), Count);
						})
						.ToolTipText(LOCTEXT("CommitTooltip", "Commit the staged files. Nothing is pushed. (Ctrl+Enter)"))
						.IsEnabled(this, &SGitChangesView::CanCommit)
						.OnClicked_Lambda([this]() { Commit(false); return FReply::Handled(); })
					]
				]
			]
		];
}

bool SGitChangesView::CanCommit() const
{
	const TSharedPtr<FGitWorkspaceModel> Model = WeakModel.Pin();
	if (!Model.IsValid() || Model->IsBusy() || CommitMessage.TrimStartAndEnd().IsEmpty())
	{
		return false;
	}
	const GitCore::FSessionSnapshotRef Snapshot = Model->GetSnapshot();
	return Snapshot->Status.StagedCount() > 0 && !Snapshot->Status.HasConflicts();
}

void SGitChangesView::Commit(bool bPush)
{
	const TSharedPtr<FGitWorkspaceModel> Model = WeakModel.Pin();
	if (!CanCommit() || !Model.IsValid())
	{
		return;
	}
	Model->Commit(CommitMessage.TrimStartAndEnd(), bPush, [WeakThis = TWeakPtr<SGitChangesView>(SharedThis(this))](bool bCommitted)
	{
		if (const TSharedPtr<SGitChangesView> Self = WeakThis.Pin(); Self.IsValid() && bCommitted)
		{
			Self->CommitMessage.Reset();
		}
	});
}

#undef LOCTEXT_NAMESPACE
