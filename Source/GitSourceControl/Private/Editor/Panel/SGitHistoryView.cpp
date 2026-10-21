// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#include "SGitHistoryView.h"

#include "Editor/GitWorkspaceModel.h"
#include "Editor/GitWorkspaceText.h"

#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "HAL/PlatformApplicationMisc.h"
#include "RevisionControlStyle/RevisionControlStyle.h"
#include "Styling/AppStyle.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/SHeaderRow.h"
#include "Widgets/Views/STableRow.h"

#define LOCTEXT_NAMESPACE "GitWorkspaceHistory"

namespace
{
	const FName SummaryColumn(TEXT("Summary"));
	const FName AuthorColumn(TEXT("Author"));
	const FName DateColumn(TEXT("Date"));
	const FName IdColumn(TEXT("Id"));

	FText CommitDate(const FString& Iso)
	{
		FDateTime Date;
		return FDateTime::ParseIso8601(*Iso, Date) ? GitWorkspaceText::Age(Date) : FText::FromString(Iso);
	}

	FText CommitDateLong(const FString& Iso)
	{
		FDateTime Date;
		return FDateTime::ParseIso8601(*Iso, Date) ? FText::AsDateTime(Date, EDateTimeStyle::Medium, EDateTimeStyle::Short) : FText::FromString(Iso);
	}

	class SGitCommitRow : public SMultiColumnTableRow<SGitHistoryView::FCommitPtr>
	{
	public:
		SLATE_BEGIN_ARGS(SGitCommitRow) {}
			SLATE_ARGUMENT(SGitHistoryView::FCommitPtr, Commit)
			SLATE_ARGUMENT(bool, bUnpushed)
		SLATE_END_ARGS()

		void Construct(const FArguments& InArgs, const TSharedRef<STableViewBase>& Owner)
		{
			Commit = InArgs._Commit;
			bUnpushed = InArgs._bUnpushed;
			SMultiColumnTableRow<SGitHistoryView::FCommitPtr>::Construct(FSuperRowType::FArguments().Padding(FMargin(0.0f, 2.0f)), Owner);
		}

		virtual TSharedRef<SWidget> GenerateWidgetForColumn(const FName& Column) override
		{
			if (Column == SummaryColumn)
			{
				return SNew(SHorizontalBox)
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					.Padding(4.0f, 0.0f, 6.0f, 0.0f)
					[
						SNew(SImage)
						.Image(FAppStyle::GetBrush("Icons.CircleArrowUp"))
						.DesiredSizeOverride(FVector2D(14.0f, 14.0f))
						.ColorAndOpacity(FSlateColor::UseSubduedForeground())
						.ToolTipText(LOCTEXT("UnpushedTooltip", "Not pushed yet."))
						.Visibility(bUnpushed ? EVisibility::Visible : EVisibility::Hidden)
					]
					+ SHorizontalBox::Slot()
					.FillWidth(1.0f)
					.VAlign(VAlign_Center)
					[
						SNew(STextBlock)
						.Text(FText::FromString(Commit->Subject))
						.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
						.ToolTipText(FText::FromString(Commit->Subject))
					];
			}
			FText Text;
			if (Column == AuthorColumn)
			{
				Text = FText::FromString(Commit->Author);
			}
			else if (Column == DateColumn)
			{
				Text = CommitDate(Commit->Date);
			}
			else
			{
				Text = FText::FromString(GitWorkspaceText::ShortId(Commit->Oid));
			}
			return SNew(STextBlock)
				.Text(Text)
				.ColorAndOpacity(Column == SummaryColumn ? FSlateColor::UseForeground() : FSlateColor::UseSubduedForeground())
				.OverflowPolicy(ETextOverflowPolicy::Ellipsis);
		}

	private:
		SGitHistoryView::FCommitPtr Commit;
		bool bUnpushed = false;
	};

	const FSlateBrush* ChangeBrush(TCHAR Status)
	{
		const ISlateStyle& Style = FRevisionControlStyleManager::Get();
		switch (Status)
		{
		case TEXT('A'):
		case TEXT('C'):
			return Style.GetBrush("RevisionControl.SnapshotHistory.Added");
		case TEXT('D'):
			return Style.GetBrush("RevisionControl.SnapshotHistory.Removed");
		case TEXT('R'):
			return Style.GetBrush("RevisionControl.Branched");
		default:
			return Style.GetBrush("RevisionControl.SnapshotHistory.Modified");
		}
	}
}

void SGitHistoryView::Construct(const FArguments& InArgs, const TSharedRef<FGitWorkspaceModel>& InModel)
{
	WeakModel = InModel;
	ChangedHandle = InModel->OnChanged().AddSP(this, &SGitHistoryView::OnModelChanged);

	ChildSlot
	[
		SAssignNew(Splitter, SSplitter)
		.Orientation(Orient_Horizontal)
		+ SSplitter::Slot()
		.Value(0.6f)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.FillHeight(1.0f)
			[
				SAssignNew(CommitList, SListView<FCommitPtr>)
				.ListItemsSource(&Commits)
				.SelectionMode(ESelectionMode::Single)
				.OnGenerateRow(this, &SGitHistoryView::OnGenerateCommitRow)
				.OnSelectionChanged(this, &SGitHistoryView::OnCommitSelected)
				.OnContextMenuOpening(this, &SGitHistoryView::OnCommitMenu)
				.HeaderRow
				(
					SNew(SHeaderRow)
					+ SHeaderRow::Column(SummaryColumn)
					.DefaultLabel(LOCTEXT("SummaryColumn", "Summary"))
					.FillWidth(0.75f)
					+ SHeaderRow::Column(AuthorColumn)
					.DefaultLabel(LOCTEXT("AuthorColumn", "Author"))
					.FillWidth(0.25f)
					+ SHeaderRow::Column(DateColumn)
					.DefaultLabel(LOCTEXT("DateColumn", "Date"))
					.FixedWidth(96.0f)
					+ SHeaderRow::Column(IdColumn)
					.DefaultLabel(LOCTEXT("IdColumn", "Commit"))
					.FixedWidth(84.0f)
				)
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(4.0f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
					.Text_Lambda([this]()
					{
						if (!Error.IsEmpty())
						{
							return FText::FromString(Error);
						}
						if (bLoading)
						{
							return LOCTEXT("Loading", "Loading history…");
						}
						return Commits.IsEmpty() ? LOCTEXT("NoCommits", "No commits yet.") : FText::Format(LOCTEXT("CommitCount", "{0} {0}|plural(one=commit,other=commits)"), Commits.Num());
					})
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				[
					SNew(SButton)
					.Text(LOCTEXT("LoadMore", "Load more"))
					.Visibility_Lambda([this]() { return bHasMore ? EVisibility::Visible : EVisibility::Collapsed; })
					.IsEnabled_Lambda([this]() { return !bLoading; })
					.OnClicked_Lambda([this]()
					{
						Limit += 200;
						Load();
						return FReply::Handled();
					})
				]
			]
		]
		+ SSplitter::Slot()
		.Value(0.4f)
		.MinSize(220.0f)
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("Brushes.Recessed"))
			.Padding(8.0f)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					SNew(STextBlock)
					.Visibility_Lambda([this]() { return Selected.IsValid() ? EVisibility::Collapsed : EVisibility::Visible; })
					.Text(LOCTEXT("SelectCommit", "Select a commit to see what it changed."))
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
					.AutoWrapText(true)
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					SNew(STextBlock)
					.Visibility_Lambda([this]() { return Selected.IsValid() ? EVisibility::Visible : EVisibility::Collapsed; })
					.Font(FAppStyle::GetFontStyle("HeadingExtraSmall"))
					.AutoWrapText(true)
					.Text_Lambda([this]() { return Selected.IsValid() ? FText::FromString(Selected->Subject) : FText::GetEmpty(); })
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 4.0f)
				[
					SNew(STextBlock)
					.Visibility_Lambda([this]() { return Selected.IsValid() ? EVisibility::Visible : EVisibility::Collapsed; })
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
					.Text_Lambda([this]()
					{
						return Selected.IsValid()
							? FText::Format(LOCTEXT("CommitByline", "{0} · {1} · {2}"), FText::FromString(Selected->Author), CommitDateLong(Selected->Date),
								FText::FromString(GitWorkspaceText::ShortId(Selected->Oid)))
							: FText::GetEmpty();
					})
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.MaxHeight(120.0f)
				.Padding(0.0f, 4.0f)
				[
					SNew(SScrollBox)
					+ SScrollBox::Slot()
					[
						SNew(STextBlock)
						.AutoWrapText(true)
						.Text_Lambda([this]()
						{
							// The body: the message without its first line.
							FString Body;
							if (Details.Message.Split(TEXT("\n"), nullptr, &Body))
							{
								return FText::FromString(Body.TrimStartAndEnd());
							}
							return FText::GetEmpty();
						})
					]
				]
				+ SVerticalBox::Slot()
				.FillHeight(1.0f)
				.Padding(0.0f, 6.0f, 0.0f, 0.0f)
				[
					SAssignNew(ChangeList, SListView<FChangePtr>)
					.ListItemsSource(&Changes)
					.SelectionMode(ESelectionMode::Single)
					.OnGenerateRow(this, &SGitHistoryView::OnGenerateChangeRow)
					.OnMouseButtonDoubleClick(this, &SGitHistoryView::CompareChange)
					.OnContextMenuOpening(this, &SGitHistoryView::OnChangeMenu)
				]
			]
		]
	];
}

SGitHistoryView::~SGitHistoryView()
{
	if (const TSharedPtr<FGitWorkspaceModel> Model = WeakModel.Pin())
	{
		Model->OnChanged().Remove(ChangedHandle);
	}
}

void SGitHistoryView::Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime)
{
	SCompoundWidget::Tick(AllottedGeometry, InCurrentTime, InDeltaTime);
	// Narrow docks show the commit details below the list.
	const EOrientation Wanted = AllottedGeometry.GetLocalSize().X < 760.0f ? Orient_Vertical : Orient_Horizontal;
	if (Splitter.IsValid() && Splitter->GetOrientation() != Wanted)
	{
		Splitter->SetOrientation(Wanted);
	}
}

void SGitHistoryView::OnModelChanged()
{
	if (bVisible)
	{
		RefreshIfNeeded();
	}
}

void SGitHistoryView::RefreshIfNeeded()
{
	bVisible = true;
	const TSharedPtr<FGitWorkspaceModel> Model = WeakModel.Pin();
	if (Model.IsValid() && !bLoading && Model->GetSnapshot()->Status.Head != LoadedHead)
	{
		Load();
	}
}

void SGitHistoryView::Load()
{
	const TSharedPtr<FGitWorkspaceModel> Model = WeakModel.Pin();
	if (!Model.IsValid())
	{
		return;
	}
	bLoading = true;
	const FString Head = Model->GetSnapshot()->Status.Head;
	Model->LoadHistory(Limit, [WeakThis = TWeakPtr<SGitHistoryView>(SharedThis(this)), Head](TArray<GitCore::FHistoryCommit>&& Loaded, bool bMore, const FString& LoadError)
	{
		const TSharedPtr<SGitHistoryView> Self = WeakThis.Pin();
		if (!Self.IsValid())
		{
			return;
		}
		Self->bLoading = false;
		Self->LoadedHead = Head;
		Self->bHasMore = bMore;
		Self->Error = LoadError;
		const FString SelectedOid = Self->Selected.IsValid() ? Self->Selected->Oid : FString();
		Self->Commits.Reset();
		FCommitPtr Reselect;
		for (GitCore::FHistoryCommit& Commit : Loaded)
		{
			Self->Commits.Add(MakeShared<GitCore::FHistoryCommit>(MoveTemp(Commit)));
			if (Self->Commits.Last()->Oid == SelectedOid)
			{
				Reselect = Self->Commits.Last();
			}
		}
		Self->CommitList->RequestListRefresh();
		if (Reselect.IsValid())
		{
			Self->CommitList->SetSelection(Reselect, ESelectInfo::Direct);
		}
	});
}

bool SGitHistoryView::IsUnpushed(const FCommitPtr& Commit) const
{
	// The newest commits ahead of the upstream are listed first.
	const TSharedPtr<FGitWorkspaceModel> Model = WeakModel.Pin();
	if (!Model.IsValid())
	{
		return false;
	}
	const GitCore::FRemoteState& Remote = Model->GetSnapshot()->Remote;
	if (Remote.Upstream.IsEmpty())
	{
		return false;
	}
	const int32 Index = Commits.IndexOfByKey(Commit);
	return Index != INDEX_NONE && Index < Remote.Ahead;
}

TSharedRef<ITableRow> SGitHistoryView::OnGenerateCommitRow(FCommitPtr Commit, const TSharedRef<STableViewBase>& Owner)
{
	return SNew(SGitCommitRow, Owner)
		.Commit(Commit)
		.bUnpushed(IsUnpushed(Commit));
}

void SGitHistoryView::OnCommitSelected(FCommitPtr Commit, ESelectInfo::Type SelectInfo)
{
	Selected = Commit;
	Details = GitCore::FCommitDetails();
	Changes.Reset();
	ChangeList->RequestListRefresh();
	const TSharedPtr<FGitWorkspaceModel> Model = WeakModel.Pin();
	if (!Commit.IsValid() || !Model.IsValid())
	{
		return;
	}
	Model->LoadCommit(Commit->Oid, [WeakThis = TWeakPtr<SGitHistoryView>(SharedThis(this)), Oid = Commit->Oid](GitCore::FCommitDetails&& Loaded, const FString& LoadError)
	{
		const TSharedPtr<SGitHistoryView> Self = WeakThis.Pin();
		if (!Self.IsValid() || !Self->Selected.IsValid() || Self->Selected->Oid != Oid)
		{
			return;
		}
		Self->Details = MoveTemp(Loaded);
		Self->Changes.Reset();
		for (const GitCore::FHistoryChange& Change : Self->Details.Files)
		{
			Self->Changes.Add(MakeShared<GitCore::FHistoryChange>(Change));
		}
		Self->ChangeList->RequestListRefresh();
	});
}

TSharedRef<ITableRow> SGitHistoryView::OnGenerateChangeRow(FChangePtr Change, const TSharedRef<STableViewBase>& Owner)
{
	const FString Label = Change->OldPath.IsEmpty() || Change->OldPath == Change->Path ? Change->Path : Change->OldPath + TEXT(" → ") + Change->Path;
	return SNew(STableRow<FChangePtr>, Owner)
		.Padding(FMargin(2.0f, 2.0f))
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(0.0f, 0.0f, 6.0f, 0.0f)
			[
				SNew(SImage)
				.Image(ChangeBrush(Change->Status))
				.DesiredSizeOverride(FVector2D(14.0f, 14.0f))
			]
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text(FText::FromString(Label))
				.ToolTipText(LOCTEXT("ChangeTooltip", "Double-click to compare with the previous version."))
				.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
			]
		];
}

void SGitHistoryView::CompareChange(FChangePtr Change)
{
	const TSharedPtr<FGitWorkspaceModel> Model = WeakModel.Pin();
	if (Model.IsValid() && Change.IsValid() && Selected.IsValid())
	{
		GitCore::FHistoryCommit Commit = *Selected;
		if (Commit.Parents.IsEmpty() && !Details.Commit.Parents.IsEmpty())
		{
			Commit.Parents = Details.Commit.Parents;
		}
		Model->ShowCommitDiff(Commit, *Change);
	}
}

TSharedPtr<SWidget> SGitHistoryView::OnCommitMenu()
{
	if (!Selected.IsValid())
	{
		return nullptr;
	}
	const FString Oid = Selected->Oid;
	const FString Subject = Selected->Subject;
	FMenuBuilder Menu(true, nullptr);
	Menu.AddMenuEntry(LOCTEXT("CopyId", "Copy commit id"), FText::GetEmpty(), FSlateIcon(FAppStyle::GetAppStyleSetName(), "GenericCommands.Copy"),
		FUIAction(FExecuteAction::CreateLambda([Oid]() { FPlatformApplicationMisc::ClipboardCopy(*Oid); })));
	Menu.AddMenuEntry(LOCTEXT("CopySummary", "Copy summary"), FText::GetEmpty(), FSlateIcon(FAppStyle::GetAppStyleSetName(), "GenericCommands.Copy"),
		FUIAction(FExecuteAction::CreateLambda([Subject]() { FPlatformApplicationMisc::ClipboardCopy(*Subject); })));
	return Menu.MakeWidget();
}

TSharedPtr<SWidget> SGitHistoryView::OnChangeMenu()
{
	const TArray<FChangePtr> Picked = ChangeList->GetSelectedItems();
	if (Picked.IsEmpty())
	{
		return nullptr;
	}
	const FChangePtr Change = Picked[0];
	FMenuBuilder Menu(true, nullptr);
	Menu.AddMenuEntry(LOCTEXT("CompareChange", "Compare with previous version"), FText::GetEmpty(),
		FSlateIcon(FRevisionControlStyleManager::GetStyleSetName(), "RevisionControl.Actions.Diff"),
		FUIAction(FExecuteAction::CreateSP(this, &SGitHistoryView::CompareChange, Change)));
	Menu.AddMenuEntry(LOCTEXT("BrowseChange", "Show in Content Browser"), FText::GetEmpty(), FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.BrowseContent"),
		FUIAction(FExecuteAction::CreateLambda([this, Path = Change->Path]() { if (const TSharedPtr<FGitWorkspaceModel> M = WeakModel.Pin()) { M->BrowseTo({Path}); } })));
	Menu.AddMenuEntry(LOCTEXT("CopyChangePath", "Copy path"), FText::GetEmpty(), FSlateIcon(FAppStyle::GetAppStyleSetName(), "GenericCommands.Copy"),
		FUIAction(FExecuteAction::CreateLambda([Path = Change->Path]() { FPlatformApplicationMisc::ClipboardCopy(*Path); })));
	return Menu.MakeWidget();
}

#undef LOCTEXT_NAMESPACE
