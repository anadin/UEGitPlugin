// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#include "SGitLocksView.h"

#include "Editor/GitWorkspaceModel.h"
#include "Editor/GitWorkspaceText.h"
#include "SGitDialogs.h"

#include "Framework/Application/SlateApplication.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "HAL/PlatformApplicationMisc.h"
#include "RevisionControlStyle/RevisionControlStyle.h"
#include "Styling/AppStyle.h"
#include "Types/ISlateMetaData.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SSearchBox.h"
#include "Widgets/Input/SSegmentedControl.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/SHeaderRow.h"
#include "Widgets/Views/STableRow.h"

#define LOCTEXT_NAMESPACE "GitWorkspaceLocks"

namespace
{
	const FName FileColumn(TEXT("File"));
	const FName OwnerColumn(TEXT("Owner"));
	const FName SinceColumn(TEXT("Since"));

	class SGitLockRow : public SMultiColumnTableRow<SGitLocksView::FItemPtr>
	{
	public:
		SLATE_BEGIN_ARGS(SGitLockRow) {}
			SLATE_ARGUMENT(SGitLocksView::FItemPtr, Item)
			SLATE_ARGUMENT(TWeakPtr<SGitLocksView>, View)
		SLATE_END_ARGS()

		void Construct(const FArguments& InArgs, const TSharedRef<STableViewBase>& Owner)
		{
			Item = InArgs._Item;
			View = InArgs._View;
			SMultiColumnTableRow<SGitLocksView::FItemPtr>::Construct(FSuperRowType::FArguments().Padding(FMargin(0.0f, 2.0f)), Owner);
		}

		virtual FReply OnPreviewMouseButtonDown(const FGeometry& Geometry, const FPointerEvent& Event) override
		{
			return Item->bFolder ? GitFileTree::ToggleOnAltClick(*this, Event) : FReply::Unhandled();
		}

		virtual TSharedRef<SWidget> GenerateWidgetForColumn(const FName& Column) override
		{
			const TSharedPtr<SGitLocksView> Pinned = View.Pin();
			if (!Pinned.IsValid())
			{
				return SNullWidget::NullWidget;
			}
			const TAttribute<FText> Highlight = TAttribute<FText>::CreateLambda([Weak = View]()
			{
				const TSharedPtr<SGitLocksView> Owner = Weak.Pin();
				return Owner.IsValid() ? Owner->GetFilterText() : FText::GetEmpty();
			});
			if (Item->bFolder)
			{
				return Column == FileColumn
					? GitFileTree::MakeNameCell(SharedThis(this), true, FText::FromString(Item->Label), FText::AsNumber(Item->FileCount), nullptr,
						FText::Format(LOCTEXT("FolderTooltip", "{0}\n{1} locked {1}|plural(one=file,other=files). Alt+click opens or closes everything in it."),
							FText::FromString(Item->Path), Item->FileCount), Highlight)
					: SNullWidget::NullWidget;
			}
			const GitCore::FLockInfo& Lock = *Item->Lock;
			const bool bElsewhere = Pinned->IsElsewhere(Lock);
			if (Column == FileColumn)
			{
				// Same icons as the Content Browser: checked out here, locked by you elsewhere, or someone else's.
				const FGitFileDisplay& Display = Pinned->GetDisplay(Lock.Path);
				const FSlateBrush* Icon = FRevisionControlStyleManager::Get().GetBrush(
					bElsewhere ? "RevisionControl.Locked" : Lock.bOurs ? "RevisionControl.CheckedOut" : "RevisionControl.CheckedOutByOtherUser");
				return GitFileTree::MakeNameCell(SharedThis(this), false, Display.Name, GitFileTree::ShowFolders() ? FText::GetEmpty() : Display.Folder, Icon,
					FText::FromString(Lock.Path), Highlight);
			}
			if (Column == OwnerColumn)
			{
				return SNew(STextBlock)
					.Text(Pinned->GetOwnerText(Lock))
					.ToolTipText(bElsewhere ? GitWorkspaceText::ClaimLockTooltip() : FText::GetEmpty())
					.HighlightText(Highlight)
					.OverflowPolicy(ETextOverflowPolicy::Ellipsis);
			}
			return SNew(STextBlock)
				.Text(Lock.LockedAt.GetTicks() > 0 ? GitWorkspaceText::Age(Lock.LockedAt) : FText::GetEmpty())
				.ColorAndOpacity(FSlateColor::UseSubduedForeground());
		}

	private:
		SGitLocksView::FItemPtr Item;
		TWeakPtr<SGitLocksView> View;
	};
}

void SGitLocksView::Construct(const FArguments& InArgs, const TSharedRef<FGitWorkspaceModel>& InModel)
{
	WeakModel = InModel;
	ChangedHandle = InModel->OnChanged().AddSP(this, &SGitLocksView::OnModelChanged);
	bBuiltFolders = GitFileTree::ShowFolders();

	ChildSlot
	[
		SNew(SVerticalBox)
		// Which locks: everyone's or yours, a search, and folders or a flat list.
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(4.0f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.Padding(0.0f, 0.0f, 8.0f, 0.0f)
			[
				SNew(SSegmentedControl<bool>)
				.Value_Lambda([this]() { return bMineOnly; })
				.OnValueChanged_Lambda([this](bool bValue)
				{
					bMineOnly = bValue;
					BuiltGeneration = MAX_uint64;
					Rebuild();
				})
				+ SSegmentedControl<bool>::Slot(false)
				.Text(LOCTEXT("AllLocks", "All"))
				+ SSegmentedControl<bool>::Slot(true)
				.Text(LOCTEXT("MyLocks", "Mine"))
			]
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			[
				SAssignNew(SearchBox, SSearchBox)
				.HintText(LOCTEXT("SearchHint", "Search locked files or owners (Ctrl+F)"))
				.OnTextChanged_Lambda([this](const FText& Text)
				{
					Filter = Text.ToString().TrimStartAndEnd();
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
		]
		// When locks were last checked, and the actions.
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(4.0f, 0.0f, 4.0f, 4.0f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.VAlign(VAlign_Center)
			.Padding(0.0f, 0.0f, 8.0f, 0.0f)
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
					if (!Model->IsLockingEnabled())
					{
						return LOCTEXT("LockingOff", "This project does not use locks: no .gitattributes file marks files lockable, or locking is turned off in the revision control settings.");
					}
					const GitCore::FLockSnapshot& Snapshot = Model->GetSnapshot()->Locks;
					FText Checked;
					if (!Snapshot.Error.IsEmpty())
					{
						Checked = Snapshot.bVerified
							? FText::Format(LOCTEXT("LocksStale", "Checked {0}; the last check failed: {1}"), GitWorkspaceText::Age(Snapshot.VerifiedAtUtc), FText::FromString(Snapshot.Error))
							: FText::FromString(Snapshot.Error);
					}
					else
					{
						Checked = Snapshot.bVerified ? FText::Format(LOCTEXT("LocksChecked", "Checked {0}."), GitWorkspaceText::Age(Snapshot.VerifiedAtUtc))
							: LOCTEXT("LocksNotChecked", "Not checked yet.");
					}
					return ElsewhereCount == 0 ? Checked
						: FText::Format(LOCTEXT("LocksElsewhere", "{0} You hold {1} {1}|plural(one=lock,other=locks) taken elsewhere; Claim makes {1}|plural(one=it,other=them) usable here."),
							Checked, ElsewhereCount);
				})
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			[
				SNew(SButton)
				.Text(LOCTEXT("CheckNow", "Check now"))
				.ToolTipText(LOCTEXT("CheckNowTooltip", "Ask the server who holds which locks."))
				.IsEnabled_Lambda([this]() { const TSharedPtr<FGitWorkspaceModel> Model = WeakModel.Pin(); return Model.IsValid() && Model->IsLockingEnabled() && !Model->IsBusy(); })
				.OnClicked_Lambda([this]() { if (const TSharedPtr<FGitWorkspaceModel> Model = WeakModel.Pin()) { Model->CheckLocks(); } return FReply::Handled(); })
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.Padding(4.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(SButton)
				.Text(LOCTEXT("Claim", "Claim"))
				.ToolTipText(GitWorkspaceText::ClaimLockTooltip())
				.Visibility_Lambda([this]() { return ElsewhereCount > 0 ? EVisibility::Visible : EVisibility::Collapsed; })
				.IsEnabled(this, &SGitLocksView::CanClaim)
				.OnClicked_Lambda([this]() { Claim(); return FReply::Handled(); })
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.Padding(4.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(SButton)
				.Text(LOCTEXT("Release", "Release"))
				.ToolTipText(LOCTEXT("ReleaseTooltip", "Release your selected locks (in selected folders too). Local changes stay."))
				.IsEnabled_Lambda([this]()
				{
					const TSharedPtr<FGitWorkspaceModel> Model = WeakModel.Pin();
					return Model.IsValid() && !Model->IsBusy() && SelectedLocks().ContainsByPredicate([](const TSharedPtr<GitCore::FLockInfo>& Lock) { return Lock->bOurs; });
				})
				.OnClicked_Lambda([this]() { Release(false); return FReply::Handled(); })
			]
		]
		+ SVerticalBox::Slot()
		.FillHeight(1.0f)
		[
			SAssignNew(Tree, STreeView<FItemPtr>)
			.AddMetaData<FTagMetaData>(TEXT("GitPanel.Locks.List"))
			.TreeItemsSource(&Items)
			.SelectionMode(ESelectionMode::Multi)
			.OnGenerateRow(this, &SGitLocksView::OnGenerateRow)
			.OnGetChildren(this, &SGitLocksView::OnGetChildren)
			.OnExpansionChanged(this, &SGitLocksView::OnExpansionChanged)
			.OnSetExpansionRecursive(this, &SGitLocksView::OnSetExpansionRecursive)
			.OnMouseButtonDoubleClick(this, &SGitLocksView::OnDoubleClick)
			.OnContextMenuOpening(this, &SGitLocksView::OnContextMenu)
			.HeaderRow
			(
				SNew(SHeaderRow)
				+ SHeaderRow::Column(FileColumn)
				.DefaultLabel(LOCTEXT("FileColumn", "File"))
				.FillWidth(0.6f)
				+ SHeaderRow::Column(OwnerColumn)
				.DefaultLabel(LOCTEXT("OwnerColumn", "Locked by"))
				.FillWidth(0.25f)
				+ SHeaderRow::Column(SinceColumn)
				.DefaultLabel(LOCTEXT("SinceColumn", "Since"))
				.FillWidth(0.15f)
			)
		]
	];
	Rebuild();
}

SGitLocksView::~SGitLocksView()
{
	if (const TSharedPtr<FGitWorkspaceModel> Model = WeakModel.Pin())
	{
		Model->OnChanged().Remove(ChangedHandle);
	}
}

void SGitLocksView::Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime)
{
	SCompoundWidget::Tick(AllottedGeometry, InCurrentTime, InDeltaTime);
	// The folders preference is shared with the other tabs.
	if (GitFileTree::ShowFolders() != bBuiltFolders)
	{
		Rebuild();
	}
}

void SGitLocksView::FocusSearch()
{
	if (SearchBox.IsValid())
	{
		FSlateApplication::Get().SetKeyboardFocus(SearchBox, EFocusCause::SetDirectly);
	}
}

void SGitLocksView::OnModelChanged()
{
	Rebuild();
}

const FGitFileDisplay& SGitLocksView::GetDisplay(const FString& Path)
{
	return Displays.Get(WeakModel.Pin().Get(), Path);
}

FText SGitLocksView::GetOwnerText(const GitCore::FLockInfo& Lock) const
{
	if (!Lock.bOurs)
	{
		return FText::FromString(Lock.Owner);
	}
	return IsElsewhere(Lock) ? LOCTEXT("OwnerYouElsewhere", "You, elsewhere") : LOCTEXT("OwnerYou", "You");
}

bool SGitLocksView::IsElsewhere(const GitCore::FLockInfo& Lock) const
{
	const TSharedPtr<FGitWorkspaceModel> Model = WeakModel.Pin();
	return Model.IsValid() && Model->GetSnapshot()->IsOursElsewhere(Lock);
}

void SGitLocksView::Rebuild()
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
	if (Snapshot->Generation != BuiltGeneration)
	{
		Displays.Reset();
	}
	BuiltGeneration = Snapshot->Generation;
	bBuiltFolders = bFolders;
	TSet<FString> Selected;
	for (const FItemPtr& Item : Tree->GetSelectedItems())
	{
		Selected.Add(Item->Path);
	}

	struct FSortable
	{
		bool bOurs = false;
		FString Key;
		FItemPtr Item;
	};
	TArray<FSortable> Sortable;
	ElsewhereCount = 0;
	for (const TPair<FString, GitCore::FLockInfo>& Pair : Snapshot->Locks.ByPath)
	{
		const GitCore::FLockInfo& Lock = Pair.Value;
		ElsewhereCount += Snapshot->IsOursElsewhere(Lock) ? 1 : 0;
		if (bMineOnly && !Lock.bOurs)
		{
			continue;
		}
		const FString Name = GetDisplay(Lock.Path).Name.ToString();
		if (!Filter.IsEmpty() && !Lock.Path.Contains(Filter) && !Name.Contains(Filter) && !GetOwnerText(Lock).ToString().Contains(Filter))
		{
			continue;
		}
		const FItemPtr Item = MakeShared<FGitLockItem>();
		Item->Path = Lock.Path;
		Item->Lock = MakeShared<GitCore::FLockInfo>(Lock);
		Sortable.Add({Lock.bOurs, bFolders ? Name : Lock.Path, Item});
	}
	// Your locks first, then by the name shown (by path in a flat list).
	Sortable.Sort([](const FSortable& A, const FSortable& B)
	{
		if (A.bOurs != B.bOurs)
		{
			return A.bOurs;
		}
		const int32 Order = A.Key.Compare(B.Key, ESearchCase::IgnoreCase);
		return Order != 0 ? Order < 0 : A.Item->Path < B.Item->Path;
	});
	TArray<FItemPtr> Files;
	for (const FSortable& Entry : Sortable)
	{
		Files.Add(Entry.Item);
	}
	Items = GitFileTree::Build(Files, bFolders);
	bRestoringExpansion = true;
	GitFileTree::RestoreExpansion(*Tree, Items, CollapsedFolders, !Filter.IsEmpty());
	bRestoringExpansion = false;
	TArray<FItemPtr> Reselect;
	TFunction<void(const TArray<FItemPtr>&)> Visit = [&Visit, &Selected, &Reselect](const TArray<FItemPtr>& Level)
	{
		for (const FItemPtr& Item : Level)
		{
			if (Selected.Contains(Item->Path))
			{
				Reselect.Add(Item);
			}
			Visit(Item->Children);
		}
	};
	Visit(Items);
	Tree->RequestTreeRefresh();
	Tree->ClearSelection();
	Tree->SetItemSelection(Reselect, true);
}

TSharedRef<ITableRow> SGitLocksView::OnGenerateRow(FItemPtr Item, const TSharedRef<STableViewBase>& Owner)
{
	return SNew(SGitLockRow, Owner)
		.Item(Item)
		.View(SharedThis(this));
}

void SGitLocksView::OnGetChildren(FItemPtr Item, TArray<FItemPtr>& OutChildren) const
{
	if (Item.IsValid())
	{
		OutChildren = Item->Children;
	}
}

void SGitLocksView::OnExpansionChanged(FItemPtr Item, bool bExpanded)
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

void SGitLocksView::OnSetExpansionRecursive(FItemPtr Item, bool bExpand)
{
	if (Tree.IsValid())
	{
		GitFileTree::SetExpansionRecursive(*Tree, Item, bExpand);
	}
}

void SGitLocksView::OnDoubleClick(FItemPtr Item)
{
	if (Item.IsValid() && Item->bFolder)
	{
		Tree->SetItemExpansion(Item, !Tree->IsItemExpanded(Item));
	}
}

TArray<TSharedPtr<GitCore::FLockInfo>> SGitLocksView::SelectedLocks() const
{
	TArray<TSharedPtr<GitCore::FLockInfo>> Locks;
	if (Tree.IsValid())
	{
		for (const FItemPtr& File : GitFileTree::SelectedFiles(Tree->GetSelectedItems()))
		{
			Locks.Add(File->Lock);
		}
	}
	return Locks;
}

TSharedPtr<SWidget> SGitLocksView::OnContextMenu()
{
	const TArray<TSharedPtr<GitCore::FLockInfo>> Selected = SelectedLocks();
	if (Selected.IsEmpty())
	{
		return nullptr;
	}
	const bool bAnyMine = Selected.ContainsByPredicate([](const TSharedPtr<GitCore::FLockInfo>& Lock) { return Lock->bOurs; });
	const bool bAnyTheirs = Selected.ContainsByPredicate([](const TSharedPtr<GitCore::FLockInfo>& Lock) { return !Lock->bOurs; });
	TArray<FString> ItemPaths;
	TArray<FItemPtr> Folders;
	for (const FItemPtr& Item : Tree->GetSelectedItems())
	{
		ItemPaths.Add(Item->Path);
		if (Item->bFolder)
		{
			Folders.Add(Item);
		}
	}
	FMenuBuilder Menu(true, nullptr);
	if (Selected.ContainsByPredicate([this](const TSharedPtr<GitCore::FLockInfo>& Lock) { return IsElsewhere(*Lock); }))
	{
		Menu.AddMenuEntry(LOCTEXT("ClaimMenu", "Claim lock"), GitWorkspaceText::ClaimLockTooltip(),
			FSlateIcon(FRevisionControlStyleManager::GetStyleSetName(), "RevisionControl.Actions.CheckOut"),
			FUIAction(FExecuteAction::CreateSP(this, &SGitLocksView::Claim), FCanExecuteAction::CreateSP(this, &SGitLocksView::CanClaim)));
	}
	if (bAnyMine)
	{
		Menu.AddMenuEntry(LOCTEXT("ReleaseMenu", "Release"), LOCTEXT("ReleaseMenuTooltip", "Release your selected locks. Local changes stay."),
			FSlateIcon(FRevisionControlStyleManager::GetStyleSetName(), "RevisionControl.Unlocked"), FUIAction(FExecuteAction::CreateSP(this, &SGitLocksView::Release, false)));
	}
	if (bAnyTheirs)
	{
		Menu.AddMenuEntry(LOCTEXT("ForceReleaseMenu", "Release someone else's lock…"), LOCTEXT("ForceReleaseMenuTooltip", "Only when its owner cannot release it."),
			FSlateIcon(FRevisionControlStyleManager::GetStyleSetName(), "RevisionControl.Unlocked"), FUIAction(FExecuteAction::CreateSP(this, &SGitLocksView::Release, true)));
	}
	if (const TSharedPtr<FGitWorkspaceModel> Model = WeakModel.Pin())
	{
		GitFileTree::AddShowEntries(Menu, Model.ToSharedRef(), ItemPaths);
	}
	Menu.AddMenuEntry(LOCTEXT("CopyPathMenu", "Copy path"), FText::GetEmpty(), FSlateIcon(FAppStyle::GetAppStyleSetName(), "GenericCommands.Copy"),
		FUIAction(FExecuteAction::CreateLambda([ItemPaths]() { FPlatformApplicationMisc::ClipboardCopy(*FString::Join(ItemPaths, LINE_TERMINATOR)); })));
	if (!Folders.IsEmpty())
	{
		Menu.AddMenuSeparator();
		Menu.AddMenuEntry(LOCTEXT("ExpandAllMenu", "Open all inside"), LOCTEXT("ExpandAllMenuTooltip", "Open these folders and every folder in them. (Alt+click)"),
			FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.FolderOpen"),
			FUIAction(FExecuteAction::CreateLambda([this, Folders]() { for (const FItemPtr& Folder : Folders) { OnSetExpansionRecursive(Folder, true); } })));
		Menu.AddMenuEntry(LOCTEXT("CollapseAllMenu", "Close all inside"), LOCTEXT("CollapseAllMenuTooltip", "Close these folders and every folder in them. (Alt+click)"),
			FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.FolderClosed"),
			FUIAction(FExecuteAction::CreateLambda([this, Folders]() { for (const FItemPtr& Folder : Folders) { OnSetExpansionRecursive(Folder, false); } })));
	}
	return Menu.MakeWidget();
}

void SGitLocksView::Release(bool bForce)
{
	const TSharedPtr<FGitWorkspaceModel> Model = WeakModel.Pin();
	if (!Model.IsValid())
	{
		return;
	}
	const GitCore::FSessionSnapshotRef Snapshot = Model->GetSnapshot();
	TArray<FString> Paths;
	TArray<FString> Changed;
	for (const TSharedPtr<GitCore::FLockInfo>& Lock : SelectedLocks())
	{
		if (Lock->bOurs != bForce)
		{
			Paths.Add(Lock->Path);
			if (Snapshot->Status.Find(Lock->Path))
			{
				Changed.Add(Lock->Path);
			}
		}
	}
	if (Paths.IsEmpty())
	{
		return;
	}
	if (bForce)
	{
		if (!GitWorkspaceDialogs::Confirm(LOCTEXT("ForceTitle", "Release someone else's lock"),
			LOCTEXT("ForceMessage", "Release these locks even though other people hold them? They may lose work if they are still editing."), Paths,
			LOCTEXT("ForceButton", "Release locks")))
		{
			return;
		}
	}
	else if (!Changed.IsEmpty() && !GitWorkspaceDialogs::Confirm(LOCTEXT("ChangedTitle", "Release locks on changed files"),
		LOCTEXT("ChangedMessage", "You have changes to these files. Release their locks anyway? Someone else could then edit them too."), Changed,
		LOCTEXT("ChangedButton", "Release locks")))
	{
		return;
	}
	Model->Unlock(Paths, bForce);
}

bool SGitLocksView::CanClaim() const
{
	const TSharedPtr<FGitWorkspaceModel> Model = WeakModel.Pin();
	return Model.IsValid() && !Model->IsBusy() && SelectedLocks().ContainsByPredicate([this](const TSharedPtr<GitCore::FLockInfo>& Lock) { return IsElsewhere(*Lock); });
}

void SGitLocksView::Claim()
{
	TArray<FString> Paths;
	for (const TSharedPtr<GitCore::FLockInfo>& Lock : SelectedLocks())
	{
		if (IsElsewhere(*Lock))
		{
			Paths.Add(Lock->Path);
		}
	}
	const TSharedPtr<FGitWorkspaceModel> Model = WeakModel.Pin();
	if (Model.IsValid() && !Paths.IsEmpty())
	{
		Model->ClaimLocks(Paths);
	}
}

#undef LOCTEXT_NAMESPACE
