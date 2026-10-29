// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#include "SGitChangesView.h"

#include "Editor/GitWorkspaceModel.h"
#include "Provider/GitSourceControlUserSettings.h"
#include "SGitDialogs.h"
#include "SGitDiffView.h"

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
#include "Widgets/SOverlay.h"
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

	/** The key that turns Commit into Commit and push while held over it. */
	FText PushModifierName()
	{
#if PLATFORM_MAC
		return LOCTEXT("OptionKey", "Option");
#else
		return LOCTEXT("AltKey", "Alt");
#endif
	}

	/**
	 * A list's title bar. Below another list it is also the divider between them: dragging it moves
	 * height from one list to the other.
	 */
	class SGitPaneHeader : public SCompoundWidget
	{
	public:
		DECLARE_DELEGATE_OneParam(FOnDividerMoved, float /* Offset from where the bar was pressed, in Slate units */);

		SLATE_BEGIN_ARGS(SGitPaneHeader) {}
			SLATE_DEFAULT_SLOT(FArguments, Content)
			SLATE_ATTRIBUTE(bool, IsDivider)
			SLATE_EVENT(FSimpleDelegate, DividerPressed)
			SLATE_EVENT(FOnDividerMoved, DividerMoved)
			SLATE_EVENT(FSimpleDelegate, DividerReleased)
		SLATE_END_ARGS()

		void Construct(const FArguments& InArgs)
		{
			IsDivider = InArgs._IsDivider;
			DividerPressed = InArgs._DividerPressed;
			DividerMoved = InArgs._DividerMoved;
			DividerReleased = InArgs._DividerReleased;
			ChildSlot
			[
				InArgs._Content.Widget
			];
		}

		virtual FReply OnMouseButtonDown(const FGeometry& Geometry, const FPointerEvent& Event) override
		{
			if (Event.GetEffectingButton() != EKeys::LeftMouseButton || !IsDivider.Get())
			{
				return FReply::Unhandled();
			}
			PressedAt = Event.GetScreenSpacePosition().Y;
			Scale = FMath::Max(Geometry.GetAccumulatedLayoutTransform().GetScale(), UE_SMALL_NUMBER);
			DividerPressed.ExecuteIfBound();
			return FReply::Handled().CaptureMouse(SharedThis(this));
		}

		virtual FReply OnMouseMove(const FGeometry& Geometry, const FPointerEvent& Event) override
		{
			if (!HasMouseCapture())
			{
				return FReply::Unhandled();
			}
			DividerMoved.ExecuteIfBound((Event.GetScreenSpacePosition().Y - PressedAt) / Scale);
			return FReply::Handled();
		}

		virtual FReply OnMouseButtonUp(const FGeometry& Geometry, const FPointerEvent& Event) override
		{
			return HasMouseCapture() ? FReply::Handled().ReleaseMouseCapture() : FReply::Unhandled();
		}

		virtual void OnMouseCaptureLost(const FCaptureLostEvent& CaptureLostEvent) override
		{
			DividerReleased.ExecuteIfBound();
		}

		virtual FCursorReply OnCursorQuery(const FGeometry& Geometry, const FPointerEvent& Event) const override
		{
			return IsDivider.Get() ? FCursorReply::Cursor(EMouseCursor::ResizeUpDown) : FCursorReply::Unhandled();
		}

	private:
		TAttribute<bool> IsDivider;
		FSimpleDelegate DividerPressed;
		FOnDividerMoved DividerMoved;
		FSimpleDelegate DividerReleased;
		float PressedAt = 0.0f;
		float Scale = 1.0f;
	};

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
					.ToolTipText(Pinned->IsLockedElsewhere(Entry.Path) ? GitWorkspaceText::ClaimLockTooltip() : FText::GetEmpty())
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
		// Lists and inspector
		+ SVerticalBox::Slot()
		.FillHeight(1.0f)
		[
			SNew(SSplitter)
			.Orientation(Orient_Horizontal)
			+ SSplitter::Slot()
			.Value(0.68f)
			[
				MakeLists()
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
	if (bNowCompact != bCompact)
	{
		bCompact = bNowCompact;
		for (const FPane& Pane : Panes)
		{
			Pane.HeaderRow->SetShowGeneratedColumn(ChangeColumn, !bCompact);
			Pane.HeaderRow->SetShowGeneratedColumn(LockColumn, !bCompact);
			Pane.Tree->RebuildList();
		}
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
	if (!Model.IsValid() || Panes.IsEmpty())
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
	for (const FItemPtr& Item : SelectedItems())
	{
		Selected.Add(ExpansionKey(*Item));
	}
	const FItemPtr Conflicts = GetPane(FGitChangeItem::EGroup::Conflicts).Group;
	const FItemPtr Staged = GetPane(FGitChangeItem::EGroup::Staged).Group;
	const FItemPtr Unstaged = GetPane(FGitChangeItem::EGroup::Unstaged).Group;
	for (const FItemPtr& Group : {Conflicts, Staged, Unstaged})
	{
		Group->Total = 0;
	}
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

	// "Show in Git panel" selects each path once, in the first list that has it, opening the folders on the way.
	TArray<FItemPtr> Reselect;
	for (const FString& Path : PendingSelection)
	{
		for (const FPane& Pane : Panes)
		{
			TArray<FItemPtr> Chain;
			if (GitFileTree::FindPath<FGitChangeItem>(Pane.Group->Children, [&Path](const FGitChangeItem& Item) { return Item.Path == Path; }, Chain))
			{
				for (const FItemPtr& Folder : Chain)
				{
					CollapsedFolders.Remove(ExpansionKey(*Folder));
				}
				Reselect.Add(Chain.Last());
				break;
			}
		}
	}
	const bool bSearching = !Filter.IsEmpty();
	bRestoringExpansion = true;
	for (const FPane& Pane : Panes)
	{
		GitFileTree::RestoreExpansion(*Pane.Tree, Pane.Group->Children, CollapsedFolders, bSearching, FString::Printf(TEXT("%d|"), static_cast<int32>(Pane.Group->Group)));
	}
	bRestoringExpansion = false;

	// Files shown on request replace the selection; otherwise the selection survives the rebuild.
	if (Reselect.IsEmpty())
	{
		for (const FPane& Pane : Panes)
		{
			ForEachItem(Pane.Group->Children, [this, &Selected, &Reselect](const FItemPtr& Item)
			{
				if (Selected.Contains(ExpansionKey(*Item)))
				{
					Reselect.AddUnique(Item);
				}
			});
		}
	}
	for (const FPane& Pane : Panes)
	{
		Pane.Tree->ClearSelection();
		Pane.Tree->RequestTreeRefresh();
		const TArray<FItemPtr> InPane = Reselect.FilterByPredicate([&Pane](const FItemPtr& Item) { return Item->Group == Pane.Group->Group; });
		if (!InPane.IsEmpty())
		{
			Pane.Tree->SetItemSelection(InPane, true);
			if (!PendingSelection.IsEmpty())
			{
				Pane.Tree->RequestScrollIntoView(InPane[0]);
			}
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
		return Snapshot->IsOursElsewhere(*Lock) ? LOCTEXT("LockedByYouElsewhere", "You, elsewhere") : LOCTEXT("LockedByYou", "You");
	}
	return Lock->LockedAt.GetTicks() > 0
		? FText::Format(LOCTEXT("LockedByOwner", "{0} · {1}"), FText::FromString(Lock->Owner), GitWorkspaceText::Age(Lock->LockedAt))
		: FText::FromString(Lock->Owner);
}

TSharedRef<ITableRow> SGitChangesView::OnGenerateRow(FItemPtr Item, const TSharedRef<STableViewBase>& Owner)
{
	return SNew(SGitChangeRow, Owner)
		.Item(Item)
		.View(SharedThis(this));
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
	if (!Item.IsValid() || !Item->bFolder || bRestoringExpansion)
	{
		return;
	}
	if (bExpanded)
	{
		CollapsedFolders.Remove(ExpansionKey(*Item));
	}
	else
	{
		CollapsedFolders.Add(ExpansionKey(*Item));
	}
}

void SGitChangesView::OnSetExpansionRecursive(FItemPtr Item, bool bExpand)
{
	if (Item.IsValid())
	{
		GitFileTree::SetExpansionRecursive(*GetPane(Item->Group).Tree, Item, bExpand);
	}
}

void SGitChangesView::OnSelectionChanged(FItemPtr Item, ESelectInfo::Type SelectInfo)
{
	// Picking in one list clears the others, so Space, Delete and the menu act on what the user just picked.
	if (Item.IsValid() && SelectInfo != ESelectInfo::Direct && !bSyncingSelection)
	{
		TGuardValue<bool> Syncing(bSyncingSelection, true);
		for (const FPane& Pane : Panes)
		{
			if (Pane.Group->Group != Item->Group)
			{
				Pane.Tree->ClearSelection();
			}
		}
	}
	UpdateDiff();
}

void SGitChangesView::UpdateDiff()
{
	const FItemPtr Item = SingleSelection();
	const TSharedPtr<FGitWorkspaceModel> Model = WeakModel.Pin();
	if (!Item.IsValid() || !Model.IsValid() || !DiffView.IsValid())
	{
		// Keep the key: a rebuild clears and restores the selection, and the same file should not flash "Loading".
		return;
	}
	bDiffShown = !GetDisplay(Item->Path).bPackage;
	if (!bDiffShown)
	{
		return;
	}
	// The repository state is part of the key, so a refresh shows the file's latest edits.
	const FString File = FString::Printf(TEXT("%d|%s|"), Item->IsStagedSide() ? 1 : 0, *Item->Path);
	const FString Key = File + LexToString(BuiltGeneration);
	if (Key == DiffKey)
	{
		return;
	}
	if (!DiffKey.StartsWith(File))
	{
		DiffView->SetMessage(LOCTEXT("DiffLoading", "Loading changes…"));
	}
	DiffKey = Key;
	Model->LoadDiff(Item->Path, Item->IsStagedSide(), [WeakThis = TWeakPtr<SGitChangesView>(SharedThis(this)), Key](FString&& Diff, const FString& Error)
	{
		const TSharedPtr<SGitChangesView> Self = WeakThis.Pin();
		if (!Self.IsValid() || Self->DiffKey != Key)
		{
			return;
		}
		if (Error.IsEmpty())
		{
			Self->DiffView->SetDiff(Diff);
		}
		else
		{
			Self->DiffView->SetMessage(FText::FromString(Error));
		}
	});
}

FText SGitChangesView::GetAssetDiffHint() const
{
	const FItemPtr Item = SingleSelection();
	if (!Item.IsValid())
	{
		return FText::GetEmpty();
	}
	if (Item->Path.Contains(TEXT("/__ExternalActors__/")) || Item->Path.Contains(TEXT("/__ExternalObjects__/")))
	{
		return LOCTEXT("ExternalAssetDiff", "World Partition actors cannot be compared on their own.");
	}
	if (Item->Entry.IsAdded() || Item->Entry.IsDeleted())
	{
		return LOCTEXT("NewAssetDiff", "New and deleted assets have no other version to compare.");
	}
	return LOCTEXT("AssetDiff", "Compare opens this asset in Unreal's own diff: a Blueprint's graphs side by side, a material's nodes, a data table's rows. Other assets open as text in the Tool for diffing text set in Editor Preferences > Loading & Saving.");
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
		const TSharedPtr<STreeView<FItemPtr>>& Tree = GetPane(Item->Group).Tree;
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

TArray<SGitChangesView::FItemPtr> SGitChangesView::SelectedItems() const
{
	TArray<FItemPtr> Items;
	for (const FPane& Pane : Panes)
	{
		Items.Append(Pane.Tree->GetSelectedItems());
	}
	return Items;
}

TArray<SGitChangesView::FItemPtr> SGitChangesView::SelectedFiles() const
{
	return GitFileTree::SelectedFiles(SelectedItems());
}

TArray<SGitChangesView::FItemPtr> SGitChangesView::SelectedFilesIn(FGitChangeItem::EGroup Group) const
{
	return GitFileTree::SelectedFiles(GetPane(Group).Tree->GetSelectedItems());
}

TArray<SGitChangesView::FItemPtr> SGitChangesView::SelectedFolders() const
{
	return SelectedItems().FilterByPredicate([](const FItemPtr& Item) { return Item.IsValid() && Item->bFolder; });
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
	const TArray<FItemPtr> Picked = SelectedItems();
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

void SGitChangesView::StageFromHeader(FGitChangeItem::EGroup Group)
{
	const TSharedPtr<FGitWorkspaceModel> Model = WeakModel.Pin();
	const FPane& Pane = GetPane(Group);
	TArray<FString> Paths;
	for (const FItemPtr& File : SelectedFilesIn(Group))
	{
		Paths.AddUnique(File->Entry.Path);
	}
	if (!Model.IsValid())
	{
		return;
	}
	if (Paths.IsEmpty())
	{
		ToggleStaged(Pane.Group);
	}
	else if (Group == FGitChangeItem::EGroup::Staged)
	{
		Model->Unstage(Paths);
	}
	else
	{
		Model->Stage(Paths);
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

bool SGitChangesView::CanShowSelection(bool bContentBrowser) const
{
	const FItemPtr Item = SingleSelection();
	const TSharedPtr<FGitWorkspaceModel> Model = WeakModel.Pin();
	if (!Item.IsValid() || !Model.IsValid())
	{
		return false;
	}
	// Asked every frame: work it out once per file and repository state.
	const uint64 Generation = Model->GetSnapshot()->Generation;
	if (Item->Entry.Path != ShowPath || Generation != ShowGeneration)
	{
		ShowPath = Item->Entry.Path;
		ShowGeneration = Generation;
		bShowInContentBrowser = Model->CanBrowseTo({ShowPath});
		bShowOnDisk = Model->CanReveal({ShowPath});
	}
	return bContentBrowser ? bShowInContentBrowser : bShowOnDisk;
}

void SGitChangesView::ShowSelection(bool bContentBrowser)
{
	const FItemPtr Item = SingleSelection();
	const TSharedPtr<FGitWorkspaceModel> Model = WeakModel.Pin();
	if (!Item.IsValid() || !Model.IsValid())
	{
		return;
	}
	if (bContentBrowser)
	{
		Model->BrowseTo({Item->Entry.Path});
	}
	else
	{
		Model->Reveal({Item->Entry.Path});
	}
}

bool SGitChangesView::IsLockedElsewhere(const FString& Path) const
{
	const TSharedPtr<FGitWorkspaceModel> Model = WeakModel.Pin();
	if (!Model.IsValid())
	{
		return false;
	}
	const GitCore::FSessionSnapshotRef Snapshot = Model->GetSnapshot();
	const GitCore::FLockInfo* Lock = Snapshot->Locks.Find(Path);
	return Lock && Snapshot->IsOursElsewhere(*Lock);
}

void SGitChangesView::ClaimSelection()
{
	const TSharedPtr<FGitWorkspaceModel> Model = WeakModel.Pin();
	TArray<FString> Paths = SelectedPaths();
	Paths.RemoveAll([this](const FString& Path) { return !IsLockedElsewhere(Path); });
	if (Model.IsValid() && !Paths.IsEmpty())
	{
		Model->ClaimLocks(Paths);
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
	bool bAnyElsewhere = false;
	bool bAnyTheirs = false;
	bool bAnyLockable = false;
	for (const FItemPtr& Item : Files)
	{
		const GitCore::FLockInfo* Lock = Snapshot->Locks.Find(Item->Entry.Path);
		if (Lock)
		{
			(Lock->bOurs ? bAnyMine : bAnyTheirs) = true;
			bAnyElsewhere |= Snapshot->IsOursElsewhere(*Lock);
		}
		// Only unlocked files can be locked; your locks from elsewhere are claimed instead.
		const GitCore::FFileAttributes* Attributes = Snapshot->FindAttributes(Item->Entry.Path);
		bAnyLockable |= !Lock && Attributes && Attributes->bLockable && !Item->Entry.IsAdded();
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
	// Selected folders stand for themselves here, not for the files in them.
	TArray<FString> RowPaths;
	for (const FItemPtr& Item : SelectedItems())
	{
		RowPaths.AddUnique(Item->Path);
	}
	GitFileTree::AddShowEntries(Menu, Model.ToSharedRef(), RowPaths);
	Menu.AddMenuEntry(LOCTEXT("CopyPathMenu", "Copy path"), FText::GetEmpty(), FSlateIcon(FAppStyle::GetAppStyleSetName(), "GenericCommands.Copy"),
		FUIAction(FExecuteAction::CreateLambda([RowPaths]() { FPlatformApplicationMisc::ClipboardCopy(*FString::Join(RowPaths, LINE_TERMINATOR)); })));
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
		if (bAnyElsewhere)
		{
			Menu.AddMenuEntry(LOCTEXT("ClaimMenu", "Claim lock"), GitWorkspaceText::ClaimLockTooltip(),
				FSlateIcon(StyleName, "RevisionControl.Actions.CheckOut"), FUIAction(FExecuteAction::CreateSP(this, &SGitChangesView::ClaimSelection)));
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
		const TArray<FItemPtr> Picked = SelectedItems();
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

const SGitChangesView::FPane& SGitChangesView::GetPane(FGitChangeItem::EGroup Group) const
{
	const FPane* Found = Panes.FindByPredicate([Group](const FPane& Pane) { return Pane.Group->Group == Group; });
	check(Found);
	return *Found;
}

TSharedRef<SWidget> SGitChangesView::MakeLists()
{
	for (const FGitChangeItem::EGroup Group : {FGitChangeItem::EGroup::Conflicts, FGitChangeItem::EGroup::Unstaged, FGitChangeItem::EGroup::Staged})
	{
		FPane& Pane = Panes.AddDefaulted_GetRef();
		Pane.Group = MakeShared<FGitChangeItem>();
		Pane.Group->bGroup = true;
		Pane.Group->Group = Group;
	}
	// Each list takes its share of the height; collapsed Conflicts take none.
	const TSharedRef<SVerticalBox> Lists = SNew(SVerticalBox);
	for (FPane& Pane : Panes)
	{
		const FGitChangeItem::EGroup Group = Pane.Group->Group;
		Lists->AddSlot()
			.FillHeight(TAttribute<float>::CreateLambda([this, Group]() { return ShareOf(Group); }))
			[
				MakePane(Pane)
			];
	}
	return Lists;
}

const SGitChangesView::FPane* SGitChangesView::PaneAbove(FGitChangeItem::EGroup Group) const
{
	const FPane* Above = nullptr;
	for (const FPane& Pane : Panes)
	{
		if (Pane.Group->Group == Group)
		{
			return Above;
		}
		if (Pane.Widget.IsValid() && Pane.Widget->GetVisibility() != EVisibility::Collapsed)
		{
			Above = &Pane;
		}
	}
	return nullptr;
}

float& SGitChangesView::ShareOf(FGitChangeItem::EGroup Group)
{
	UGitSourceControlUserSettings* Settings = GetMutableDefault<UGitSourceControlUserSettings>();
	switch (Group)
	{
	case FGitChangeItem::EGroup::Conflicts:
		return ConflictsShare;
	case FGitChangeItem::EGroup::Staged:
		return Settings->StagedListShare;
	default:
		return Settings->ChangesListShare;
	}
}

void SGitChangesView::BeginDividerDrag(FGitChangeItem::EGroup Below)
{
	const FPane* Above = PaneAbove(Below);
	if (!Above)
	{
		return;
	}
	FDividerDrag Drag;
	Drag.Above = Above->Group->Group;
	Drag.Below = Below;
	Drag.AboveHeight = Above->Widget->GetCachedGeometry().GetLocalSize().Y;
	Drag.BelowHeight = GetPane(Below).Widget->GetCachedGeometry().GetLocalSize().Y;
	Drag.Shares = ShareOf(Drag.Above) + ShareOf(Drag.Below);
	DividerDrag = Drag;
}

void SGitChangesView::DragDivider(float Offset)
{
	if (!DividerDrag.IsSet())
	{
		return;
	}
	const FDividerDrag& Drag = DividerDrag.GetValue();
	const float Total = Drag.AboveHeight + Drag.BelowHeight;
	if (Total <= 0.0f || Drag.Shares <= 0.0f)
	{
		return;
	}
	// The two lists trade height; neither gets smaller than its title bar and a row.
	const float MinHeight = FMath::Min(60.0f, Total * 0.5f);
	const float AboveHeight = FMath::Clamp(Drag.AboveHeight + Offset, MinHeight, Total - MinHeight);
	ShareOf(Drag.Above) = Drag.Shares * AboveHeight / Total;
	ShareOf(Drag.Below) = Drag.Shares - ShareOf(Drag.Above);
}

void SGitChangesView::EndDividerDrag()
{
	if (DividerDrag.IsSet())
	{
		DividerDrag.Reset();
		GetMutableDefault<UGitSourceControlUserSettings>()->SaveConfig();
	}
}

TSharedRef<SWidget> SGitChangesView::MakePane(FPane& Pane)
{
	const FItemPtr Group = Pane.Group;
	const bool bConflicts = Group->Group == FGitChangeItem::EGroup::Conflicts;
	const bool bStaged = Group->Group == FGitChangeItem::EGroup::Staged;
	const FName Tag = bConflicts ? TEXT("GitPanel.Changes.Conflicts") : bStaged ? TEXT("GitPanel.Changes.Staged") : TEXT("GitPanel.Changes.Unstaged");
	return SAssignNew(Pane.Widget, SVerticalBox)
		// Conflicts only take room while there are any.
		.Visibility_Lambda([Group, bConflicts]() { return !bConflicts || Group->Total > 0 ? EVisibility::Visible : EVisibility::Collapsed; })
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			MakePaneHeader(Group)
		]
		+ SVerticalBox::Slot()
		.FillHeight(1.0f)
		[
			SNew(SOverlay)
			+ SOverlay::Slot()
			[
				SAssignNew(Pane.Tree, STreeView<FItemPtr>)
				.AddMetaData<FTagMetaData>(Tag)
				.TreeItemsSource(&Group->Children)
				.SelectionMode(ESelectionMode::Multi)
				.OnGenerateRow(this, &SGitChangesView::OnGenerateRow)
				.OnGetChildren(this, &SGitChangesView::OnGetChildren)
				.OnExpansionChanged(this, &SGitChangesView::OnExpansionChanged)
				.OnSetExpansionRecursive(this, &SGitChangesView::OnSetExpansionRecursive)
				.OnSelectionChanged(this, &SGitChangesView::OnSelectionChanged)
				.OnMouseButtonDoubleClick(this, &SGitChangesView::OnDoubleClick)
				.OnContextMenuOpening(this, &SGitChangesView::OnContextMenu)
				.HeaderRow
				(
					// The list's own title bar names it; the columns speak for themselves.
					SAssignNew(Pane.HeaderRow, SHeaderRow)
					.Visibility(EVisibility::Collapsed)
					+ SHeaderRow::Column(NameColumn)
					.FillWidth(0.55f)
					+ SHeaderRow::Column(ChangeColumn)
					.FillWidth(0.18f)
					+ SHeaderRow::Column(LockColumn)
					.FillWidth(0.27f)
					+ SHeaderRow::Column(ActionColumn)
					.FixedWidth(28.0f)
				)
			]
			+ SOverlay::Slot()
			.HAlign(HAlign_Center)
			.VAlign(VAlign_Center)
			.Padding(16.0f)
			[
				SNew(STextBlock)
				.Visibility_Lambda([Group]() { return Group->FileCount == 0 ? EVisibility::HitTestInvisible : EVisibility::Collapsed; })
				.Text_Lambda([Group, bStaged]()
				{
					if (Group->Total > 0)
					{
						return LOCTEXT("PaneFiltered", "No files here match the filter.");
					}
					return bStaged ? LOCTEXT("StagedEmpty", "Nothing staged. Stage changes to include them in the next commit.") : LOCTEXT("UnstagedEmpty", "No changes.");
				})
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				.Justification(ETextJustify::Center)
				.AutoWrapText(true)
			]
		];
}

TSharedRef<SWidget> SGitChangesView::MakePaneHeader(const FItemPtr& Group)
{
	const FGitChangeItem::EGroup Kind = Group->Group;
	const bool bConflicts = Kind == FGitChangeItem::EGroup::Conflicts;
	const bool bStaged = Kind == FGitChangeItem::EGroup::Staged;
	// A color and an icon per list tell them apart at a glance.
	const FSlateColor Accent = bConflicts ? FStyleColors::Error : bStaged ? FStyleColors::AccentGreen : FStyleColors::AccentOrange;
	const FText Tooltip = bConflicts ? LOCTEXT("ConflictsTooltip", "Files with merge conflicts. Resolve them before committing.")
		: bStaged ? LOCTEXT("StagedTooltip", "What the next commit contains. Drag this bar up or down to give the lists more or less room.")
		: LOCTEXT("ChangesTooltip", "Changes that are not staged yet. Stage them to include them in the next commit.");
	return SNew(SGitPaneHeader)
		.AddMetaData<FTagMetaData>(bConflicts ? TEXT("GitPanel.Changes.ConflictsBar") : bStaged ? TEXT("GitPanel.Changes.StagedBar") : TEXT("GitPanel.Changes.UnstagedBar"))
		.IsDivider_Lambda([this, Kind]() { return PaneAbove(Kind) != nullptr; })
		.DividerPressed(FSimpleDelegate::CreateSP(this, &SGitChangesView::BeginDividerDrag, Kind))
		.DividerMoved(SGitPaneHeader::FOnDividerMoved::CreateSP(this, &SGitChangesView::DragDivider))
		.DividerReleased(FSimpleDelegate::CreateSP(this, &SGitChangesView::EndDividerDrag))
		.ToolTipText(Tooltip)
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("Brushes.Header"))
			.Padding(0.0f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.AutoWidth()
				[
					SNew(SBox)
					.WidthOverride(3.0f)
					[
						SNew(SImage)
						.Image(FAppStyle::GetBrush("WhiteBrush"))
						.ColorAndOpacity(Accent)
					]
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(8.0f, 0.0f, 6.0f, 0.0f)
				[
					SNew(SImage)
					.Image(FAppStyle::GetBrush(bConflicts ? "Icons.Warning" : bStaged ? "Icons.Check" : "Icons.Edit"))
					.ColorAndOpacity(Accent)
				]
				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				.VAlign(VAlign_Center)
				.Padding(0.0f, 7.0f)
				[
					SNew(STextBlock)
					.Font(FAppStyle::GetFontStyle("NormalFontBold"))
					.Text_Lambda([Group]() { return GroupLabel(*Group); })
				]
				// Acts on the list's selection, or on everything it lists when nothing is selected.
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(4.0f, 0.0f, 6.0f, 0.0f)
				[
					SNew(SButton)
					.Visibility(bConflicts ? EVisibility::Collapsed : EVisibility::Visible)
					.ContentPadding(FMargin(8.0f, 0.0f))
					.Text_Lambda([this, Kind, bStaged]()
					{
						const int32 Count = SelectedFilesIn(Kind).Num();
						if (Count == 0)
						{
							return bStaged ? LOCTEXT("UnstageAll", "Unstage all") : LOCTEXT("StageAll", "Stage all");
						}
						return FText::Format(bStaged ? LOCTEXT("UnstageSelected", "Unstage {0}") : LOCTEXT("StageSelected", "Stage {0}"), Count);
					})
					.ToolTipText(bStaged ? LOCTEXT("UnstageButtonTooltip", "Move the selected files back to Changes; with none selected, every file listed here. (Space)")
						: LOCTEXT("StageButtonTooltip", "Stage the selected files; with none selected, every file listed here. (Space)"))
					.IsEnabled_Lambda([Group]() { return Group->FileCount > 0; })
					.OnClicked_Lambda([this, Kind]() { StageFromHeader(Kind); return FReply::Handled(); })
					.Cursor(EMouseCursor::Default)
				]
			]
		];
}

TSharedRef<SWidget> SGitChangesView::MakeInspector()
{
	auto Selected = [this]() { return SingleSelection(); };
	auto Button = [](const FText& Label, const TAttribute<FText>& Tooltip, TFunction<void()> Action, TFunction<bool()> IsEnabled = nullptr) -> TSharedRef<SWidget>
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
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot()
					.FillWidth(1.0f)
					.VAlign(VAlign_Center)
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
									return Snapshot->IsOursElsewhere(*Lock) ? LOCTEXT("YouHoldLockElsewhere", "You took the lock elsewhere. Claim it to save and check in here.")
										: LOCTEXT("YouHoldLock", "You hold the lock.");
								}
								return FText::Format(LOCTEXT("LockedBy", "Locked by {0}."), GetLockText(Item->Entry.Path));
							}
							const GitCore::FFileAttributes* Attributes = Model->GetSnapshot()->FindAttributes(Item->Entry.Path);
							return Attributes && Attributes->bLockable ? LOCTEXT("NotLocked", "Not locked.") : LOCTEXT("NotLockable", "Not lockable.");
						})
					]
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					.Padding(8.0f, 0.0f, 0.0f, 0.0f)
					[
						SNew(SButton)
						.Text(LOCTEXT("InspectorClaim", "Claim lock"))
						.ToolTipText(GitWorkspaceText::ClaimLockTooltip())
						.Visibility_Lambda([this, Selected]()
						{
							const FItemPtr Item = Selected();
							return Item.IsValid() && IsLockedElsewhere(Item->Entry.Path) ? EVisibility::Visible : EVisibility::Collapsed;
						})
						.IsEnabled_Lambda([this]() { const TSharedPtr<FGitWorkspaceModel> M = WeakModel.Pin(); return M.IsValid() && !M->IsBusy(); })
						.OnClicked_Lambda([this]() { ClaimSelection(); return FReply::Handled(); })
					]
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
						Button(LOCTEXT("InspectorBrowse", "Browse"),
							TAttribute<FText>::CreateLambda([this]()
							{
								return CanShowSelection(true) ? LOCTEXT("InspectorBrowseTooltip", "Show in the Content Browser.")
									: LOCTEXT("InspectorBrowseNone", "Only assets in a content folder show in the Content Browser.");
							}),
							[this]() { ShowSelection(true); },
							[this]() { return CanShowSelection(true); })
					]
					+ SWrapBox::Slot()
					[
						Button(FGitWorkspaceModel::RevealLabel(), FText::Format(LOCTEXT("InspectorRevealTooltip", "Show the file in {0}."), FPlatformMisc::GetFileManagerName()),
							[this]() { ShowSelection(false); },
							[this]() { return CanShowSelection(false); })
					]
					+ SWrapBox::Slot()
					[
						Button(LOCTEXT("InspectorOpen", "Open"), LOCTEXT("InspectorOpenTooltip", "Open the asset editor, or the file in its default application."),
							[this, Selected]() { const FItemPtr Item = Selected(); if (const TSharedPtr<FGitWorkspaceModel> M = WeakModel.Pin(); M && Item) { M->OpenFile(Item->Entry.Path); } })
					]
				]
			]
			// What changed: a text file's lines, or what Compare does for an asset
			+ SVerticalBox::Slot()
			.FillHeight(1.0f)
			.Padding(0.0f, 12.0f, 0.0f, 0.0f)
			[
				SNew(SOverlay)
				.Visibility_Lambda([Selected]() { return Selected().IsValid() ? EVisibility::Visible : EVisibility::Collapsed; })
				+ SOverlay::Slot()
				[
					SNew(SBorder)
					.BorderImage(FAppStyle::GetBrush("Brushes.Panel"))
					.Padding(2.0f)
					.Visibility_Lambda([this]() { return bDiffShown ? EVisibility::Visible : EVisibility::Collapsed; })
					[
						SAssignNew(DiffView, SGitDiffView)
					]
				]
				+ SOverlay::Slot()
				.VAlign(VAlign_Top)
				[
					SNew(STextBlock)
					.Visibility_Lambda([this]() { return bDiffShown ? EVisibility::Collapsed : EVisibility::Visible; })
					.Text(this, &SGitChangesView::GetAssetDiffHint)
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
					.AutoWrapText(true)
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
					.AddMetaData<FTagMetaData>(TEXT("GitPanel.Changes.Message"))
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
				// The hint takes the width the button leaves, so it wraps to the panel. A wrap box would size it
				// by its own wrapped width, keeping a longer hint as narrow as the shorter one before it.
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.AddMetaData<FTagMetaData>(TEXT("GitPanel.Changes.CommitHint"))
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
				// Pushing as well takes Option (Alt) held over the button, so it is never one stray click away.
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(6.0f, 0.0f, 0.0f, 0.0f)
				[
					SAssignNew(CommitButton, SButton)
					.AddMetaData<FTagMetaData>(TEXT("GitPanel.Changes.Commit"))
					.ButtonStyle(FAppStyle::Get(), "PrimaryButton")
					.Text_Lambda([this]()
					{
						const TSharedPtr<FGitWorkspaceModel> Model = WeakModel.Pin();
						const int32 Count = Model.IsValid() ? Model->GetSnapshot()->Status.StagedCount() : 0;
						return CommitPushes()
							? FText::Format(LOCTEXT("CommitAndPushCount", "Commit {0} and push"), Count)
							: FText::Format(LOCTEXT("CommitCount", "Commit {0}"), Count);
					})
					.ToolTipText_Lambda([this]()
					{
						return CommitPushes()
							? LOCTEXT("CommitAndPushTooltip", "Commit the staged files, then push every outgoing commit. (Ctrl+Shift+Enter)")
							: FText::Format(LOCTEXT("CommitTooltip", "Commit the staged files. Nothing is pushed. (Ctrl+Enter)\nHold {0} to commit and push."), PushModifierName());
					})
					.IsEnabled(this, &SGitChangesView::CanCommit)
					.OnClicked_Lambda([this]() { Commit(CommitPushes()); return FReply::Handled(); })
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

bool SGitChangesView::CommitPushes() const
{
	// What the button says is what a click does: both ask the platform for the keys held right now.
	return CommitButton.IsValid() && CommitButton->IsHovered() && FSlateApplication::Get().GetModifierKeys().IsAltDown() && CanCommit();
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
