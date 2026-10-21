// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#include "SGitLocksView.h"

#include "Editor/GitWorkspaceModel.h"
#include "Editor/GitWorkspaceText.h"
#include "SGitDialogs.h"

#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "RevisionControlStyle/RevisionControlStyle.h"
#include "Styling/AppStyle.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
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

	class SGitLockRow : public SMultiColumnTableRow<SGitLocksView::FLockPtr>
	{
	public:
		SLATE_BEGIN_ARGS(SGitLockRow) {}
			SLATE_ARGUMENT(SGitLocksView::FLockPtr, Lock)
			SLATE_ARGUMENT(bool, bHere)
		SLATE_END_ARGS()

		void Construct(const FArguments& InArgs, const TSharedRef<STableViewBase>& Owner)
		{
			Lock = InArgs._Lock;
			bHere = InArgs._bHere;
			SMultiColumnTableRow<SGitLocksView::FLockPtr>::Construct(FSuperRowType::FArguments().Padding(FMargin(0.0f, 2.0f)), Owner);
		}

		virtual TSharedRef<SWidget> GenerateWidgetForColumn(const FName& Column) override
		{
			if (Column == FileColumn)
			{
				return SNew(SHorizontalBox)
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					.Padding(4.0f, 0.0f, 6.0f, 0.0f)
					[
						SNew(SImage)
						.Image(FRevisionControlStyleManager::Get().GetBrush(Lock->bOurs ? "RevisionControl.CheckedOut" : "RevisionControl.CheckedOutByOtherUser"))
						.DesiredSizeOverride(FVector2D(16.0f, 16.0f))
					]
					+ SHorizontalBox::Slot()
					.FillWidth(1.0f)
					.VAlign(VAlign_Center)
					[
						SNew(STextBlock)
						.Text(FText::FromString(Lock->Path))
						.ToolTipText(FText::FromString(Lock->Path))
						.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
					];
			}
			if (Column == OwnerColumn)
			{
				const FText Owner = !Lock->bOurs ? FText::FromString(Lock->Owner)
					: bHere ? LOCTEXT("OwnerYou", "You") : LOCTEXT("OwnerYouElsewhere", "You, in another clone");
				return SNew(STextBlock).Text(Owner).OverflowPolicy(ETextOverflowPolicy::Ellipsis);
			}
			return SNew(STextBlock)
				.Text(Lock->LockedAt.GetTicks() > 0 ? GitWorkspaceText::Age(Lock->LockedAt) : FText::GetEmpty())
				.ColorAndOpacity(FSlateColor::UseSubduedForeground());
		}

	private:
		SGitLocksView::FLockPtr Lock;
		bool bHere = false;
	};
}

void SGitLocksView::Construct(const FArguments& InArgs, const TSharedRef<FGitWorkspaceModel>& InModel)
{
	WeakModel = InModel;
	ChangedHandle = InModel->OnChanged().AddSP(this, &SGitLocksView::OnModelChanged);
	auto Selected = [this]() { return List.IsValid() ? List->GetSelectedItems() : TArray<FLockPtr>(); };

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
			.VAlign(VAlign_Center)
			.Padding(8.0f, 0.0f)
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
					if (!Snapshot.Error.IsEmpty())
					{
						return Snapshot.bVerified
							? FText::Format(LOCTEXT("LocksStale", "Checked {0}; the last check failed: {1}"), GitWorkspaceText::Age(Snapshot.VerifiedAtUtc), FText::FromString(Snapshot.Error))
							: FText::FromString(Snapshot.Error);
					}
					return Snapshot.bVerified ? FText::Format(LOCTEXT("LocksChecked", "Checked {0}."), GitWorkspaceText::Age(Snapshot.VerifiedAtUtc))
						: LOCTEXT("LocksNotChecked", "Not checked yet.");
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
				.Text(LOCTEXT("Release", "Release"))
				.ToolTipText(LOCTEXT("ReleaseTooltip", "Release your selected locks. Local changes stay."))
				.IsEnabled_Lambda([this, Selected]()
				{
					const TSharedPtr<FGitWorkspaceModel> Model = WeakModel.Pin();
					return Model.IsValid() && !Model->IsBusy() && Selected().ContainsByPredicate([](const FLockPtr& Lock) { return Lock->bOurs; });
				})
				.OnClicked_Lambda([this]() { Release(false); return FReply::Handled(); })
			]
		]
		+ SVerticalBox::Slot()
		.FillHeight(1.0f)
		[
			SAssignNew(List, SListView<FLockPtr>)
			.ListItemsSource(&Locks)
			.SelectionMode(ESelectionMode::Multi)
			.OnGenerateRow(this, &SGitLocksView::OnGenerateRow)
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

void SGitLocksView::OnModelChanged()
{
	Rebuild();
}

void SGitLocksView::Rebuild()
{
	const TSharedPtr<FGitWorkspaceModel> Model = WeakModel.Pin();
	if (!Model.IsValid() || !List.IsValid())
	{
		return;
	}
	const GitCore::FSessionSnapshotRef Snapshot = Model->GetSnapshot();
	if (Snapshot->Generation == BuiltGeneration)
	{
		return;
	}
	BuiltGeneration = Snapshot->Generation;
	TSet<FString> Selected;
	for (const FLockPtr& Lock : List->GetSelectedItems())
	{
		Selected.Add(Lock->Id);
	}
	Locks.Reset();
	TArray<FLockPtr> Reselect;
	for (const TPair<FString, GitCore::FLockInfo>& Pair : Snapshot->Locks.ByPath)
	{
		if (bMineOnly && !Pair.Value.bOurs)
		{
			continue;
		}
		Locks.Add(MakeShared<GitCore::FLockInfo>(Pair.Value));
		if (Selected.Contains(Pair.Value.Id))
		{
			Reselect.Add(Locks.Last());
		}
	}
	// Your locks first, then by file.
	Locks.Sort([](const FLockPtr& A, const FLockPtr& B) { return A->bOurs != B->bOurs ? A->bOurs : A->Path < B->Path; });
	List->RequestListRefresh();
	List->ClearSelection();
	List->SetItemSelection(Reselect, true);
}

TSharedRef<ITableRow> SGitLocksView::OnGenerateRow(FLockPtr Lock, const TSharedRef<STableViewBase>& Owner)
{
	const TSharedPtr<FGitWorkspaceModel> Model = WeakModel.Pin();
	const bool bHere = Model.IsValid() && Model->GetSnapshot()->LocksTakenHere.Contains(Lock->Id);
	return SNew(SGitLockRow, Owner)
		.Lock(Lock)
		.bHere(bHere);
}

TSharedPtr<SWidget> SGitLocksView::OnContextMenu()
{
	const TArray<FLockPtr> Selected = List->GetSelectedItems();
	if (Selected.IsEmpty())
	{
		return nullptr;
	}
	const bool bAnyMine = Selected.ContainsByPredicate([](const FLockPtr& Lock) { return Lock->bOurs; });
	const bool bAnyTheirs = Selected.ContainsByPredicate([](const FLockPtr& Lock) { return !Lock->bOurs; });
	FMenuBuilder Menu(true, nullptr);
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
	Menu.AddMenuEntry(LOCTEXT("BrowseMenu", "Show in Content Browser"), FText::GetEmpty(), FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.BrowseContent"),
		FUIAction(FExecuteAction::CreateLambda([this, Selected]()
		{
			if (const TSharedPtr<FGitWorkspaceModel> Model = WeakModel.Pin())
			{
				TArray<FString> Paths;
				for (const FLockPtr& Lock : Selected)
				{
					Paths.Add(Lock->Path);
				}
				Model->BrowseTo(Paths);
			}
		})));
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
	for (const FLockPtr& Lock : List->GetSelectedItems())
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

#undef LOCTEXT_NAMESPACE
