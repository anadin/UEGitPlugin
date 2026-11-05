// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#include "SGitHistoryView.h"

#include "Editor/GitWorkspaceModel.h"
#include "Editor/GitWorkspaceText.h"
#include "SGitDiffView.h"

#include "Brushes/SlateRoundedBoxBrush.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "HAL/PlatformApplicationMisc.h"
#include "RevisionControlStyle/RevisionControlStyle.h"
#include "Styling/AppStyle.h"
#include "Styling/StyleColors.h"
#include "Types/ISlateMetaData.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SSearchBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SNullWidget.h"
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

	/** "Today at 9:15 am", "Yesterday at 2:30 pm", "3 Nov at 1:15 pm": local time. */
	FText CommitDate(const FString& Iso)
	{
		FDateTime Date;
		return FDateTime::ParseIso8601(*Iso, Date) ? GitWorkspaceText::DayAndTime(Date) : FText::FromString(Iso);
	}

	FText UncommittedTitle()
	{
		return LOCTEXT("UncommittedTitle", "Uncommitted changes");
	}

	/** A branch's name, on the commit it points to. */
	TSharedRef<SWidget> MakeLabel(const FText& Text, const FSlateBrush* Brush, const FText& Tooltip)
	{
		return SNew(SBorder)
			.BorderImage(Brush)
			.Padding(FMargin(5.0f, 0.0f, 5.0f, 1.0f))
			.ToolTipText(Tooltip)
			[
				SNew(STextBlock)
				.Text(Text)
				.Font(FCoreStyle::GetDefaultFontStyle("Bold", 8))
				.ColorAndOpacity(FStyleColors::White)
			];
	}

	/** Summary and author contain the search, or the commit id starts with it. */
	bool CommitMatches(const GitCore::FHistoryCommit& Commit, const FString& Filter)
	{
		return Filter.IsEmpty() || Commit.Subject.Contains(Filter) || Commit.Author.Contains(Filter) || Commit.Oid.StartsWith(Filter);
	}

	/** A commit, or the uncommitted changes above the commit they would follow. */
	class SGitCommitRow : public SMultiColumnTableRow<SGitHistoryView::FCommitPtr>
	{
	public:
		SLATE_BEGIN_ARGS(SGitCommitRow) {}
			SLATE_ARGUMENT(SGitHistoryView::FCommitPtr, Commit)
			/** The uncommitted changes' row: a dot, the title and Note. */
			SLATE_ARGUMENT(bool, bUncommitted)
			/** On HEAD's commit: the branch checked out ("HEAD" when detached). */
			SLATE_ARGUMENT(FText, Branch)
			SLATE_ARGUMENT(FText, BranchTooltip)
			/** On the commit the branch's upstream points to: its name, such as "origin/main". */
			SLATE_ARGUMENT(FText, Upstream)
			SLATE_ATTRIBUTE(FText, Note)
			SLATE_ATTRIBUTE(FSlateColor, NoteColor)
			SLATE_ATTRIBUTE(FText, HighlightText)
		SLATE_END_ARGS()

		void Construct(const FArguments& InArgs, const TSharedRef<STableViewBase>& Owner)
		{
			Commit = InArgs._Commit;
			bUncommitted = InArgs._bUncommitted;
			Branch = InArgs._Branch;
			BranchTooltip = InArgs._BranchTooltip;
			Upstream = InArgs._Upstream;
			Note = InArgs._Note;
			NoteColor = InArgs._NoteColor;
			Highlight = InArgs._HighlightText;
			SMultiColumnTableRow<SGitHistoryView::FCommitPtr>::Construct(FSuperRowType::FArguments().Padding(FMargin(0.0f, 2.0f)), Owner);
		}

		virtual TSharedRef<SWidget> GenerateWidgetForColumn(const FName& Column) override
		{
			if (Column == SummaryColumn)
			{
				return MakeSummary();
			}
			if (bUncommitted)
			{
				return SNullWidget::NullWidget;
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
				.HighlightText(Column == DateColumn ? TAttribute<FText>() : Highlight)
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				.OverflowPolicy(ETextOverflowPolicy::Ellipsis);
		}

	private:
		/** The marker, the labels and the summary. */
		TSharedRef<SWidget> MakeSummary() const
		{
			const TSharedRef<SHorizontalBox> Summary = SNew(SHorizontalBox);
			Summary->AddSlot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(4.0f, 0.0f, 6.0f, 0.0f)
			[
				MakeMarker()
			];
			if (bUncommitted)
			{
				Summary->AddSlot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(UncommittedTitle())
					.ToolTipText(LOCTEXT("UncommittedTooltip", "Changes not committed yet. Double-click to show them in the Changes tab."))
				];
				Summary->AddSlot()
				.FillWidth(1.0f)
				.VAlign(VAlign_Center)
				.Padding(8.0f, 0.0f, 0.0f, 0.0f)
				[
					SNew(STextBlock)
					.Text(Note)
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
					.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
				];
				return Summary;
			}
			// Your branch and the server's copy of it: side by side when they are on the same commit.
			static const FSlateRoundedBoxBrush BranchBrush(FStyleColors::AccentBlue, 4.0f);
			static const FSlateRoundedBoxBrush UpstreamBrush(FStyleColors::AccentPurple, 4.0f);
			if (!Branch.IsEmpty())
			{
				Summary->AddSlot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(0.0f, 0.0f, 4.0f, 0.0f)
				[
					MakeLabel(Branch, &BranchBrush, BranchTooltip)
				];
			}
			if (!Upstream.IsEmpty())
			{
				Summary->AddSlot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(0.0f, 0.0f, 4.0f, 0.0f)
				[
					MakeLabel(Upstream, &UpstreamBrush, FText::Format(LOCTEXT("UpstreamLabelTooltip", "Where {0} is on the server, as of the last fetch."), Upstream))
				];
			}
			Summary->AddSlot()
			.FillWidth(1.0f)
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text(FText::FromString(Commit->Subject))
				.HighlightText(Highlight)
				// Commits that are only on the server are not part of your branch yet.
				.ColorAndOpacity(Commit->bIncoming ? FSlateColor::UseSubduedForeground() : FSlateColor::UseForeground())
				.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
				.ToolTipText(FText::FromString(Commit->Subject))
			];
			return Summary;
		}

		/** A dot for uncommitted changes, an arrow for a commit only you have (up) or only the server has (down). */
		TSharedRef<SWidget> MakeMarker() const
		{
			if (bUncommitted)
			{
				return SNew(SBox)
					.WidthOverride(14.0f)
					.HeightOverride(14.0f)
					.HAlign(HAlign_Center)
					.VAlign(VAlign_Center)
					[
						SNew(SImage)
						.Image(FAppStyle::GetBrush("Icons.FilledCircle"))
						.DesiredSizeOverride(FVector2D(8.0f, 8.0f))
						.ColorAndOpacity(NoteColor)
					];
			}
			return SNew(SImage)
				.Image(FAppStyle::GetBrush(Commit->bIncoming ? "Icons.CircleArrowDown" : "Icons.CircleArrowUp"))
				.DesiredSizeOverride(FVector2D(14.0f, 14.0f))
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				.ToolTipText(Commit->bIncoming ? LOCTEXT("IncomingTooltip", "Not in your branch yet. Pull brings it in.") : LOCTEXT("UnpushedTooltip", "Not pushed yet."))
				.Visibility(Commit->bUnpushed || Commit->bIncoming ? EVisibility::Visible : EVisibility::Hidden);
		}

		SGitHistoryView::FCommitPtr Commit;
		bool bUncommitted = false;
		FText Branch;
		FText BranchTooltip;
		FText Upstream;
		TAttribute<FText> Note;
		TAttribute<FSlateColor> NoteColor;
		TAttribute<FText> Highlight;
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
	OnShowChangesDelegate = InArgs._OnShowChanges;
	Uncommitted = MakeShared<GitCore::FHistoryCommit>();
	ChangedHandle = InModel->OnChanged().AddSP(this, &SGitHistoryView::OnModelChanged);
	bBuiltFolders = GitFileTree::ShowFolders();

	ChildSlot
	[
		SAssignNew(Splitter, SSplitter)
		.Orientation(Orient_Horizontal)
		+ SSplitter::Slot()
		.Value(0.6f)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(4.0f)
			[
				SAssignNew(CommitSearch, SSearchBox)
				.AddMetaData<FTagMetaData>(TEXT("GitPanel.History.Search"))
				.HintText(FText::Format(LOCTEXT("SearchHint", "Search commits by summary, author or id ({0})"), GitWorkspaceText::Shortcut(EModifierKey::Control, EKeys::F)))
				.OnTextChanged_Lambda([this](const FText& Text)
				{
					CommitFilter = Text.ToString().TrimStartAndEnd();
					ApplyCommitFilter();
				})
			]
			+ SVerticalBox::Slot()
			.FillHeight(1.0f)
			[
				SAssignNew(CommitList, SListView<FCommitPtr>)
				.AddMetaData<FTagMetaData>(TEXT("GitPanel.History.Commits"))
				.ListItemsSource(&Commits)
				.SelectionMode(ESelectionMode::Single)
				.OnGenerateRow(this, &SGitHistoryView::OnGenerateCommitRow)
				.OnSelectionChanged(this, &SGitHistoryView::OnCommitSelected)
				.OnMouseButtonDoubleClick(this, &SGitHistoryView::OnCommitDoubleClick)
				.OnContextMenuOpening(this, &SGitHistoryView::OnCommitMenu)
				.OnKeyDownHandler(this, &SGitHistoryView::OnCommitKeyDown)
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
					.FixedWidth(150.0f)
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
					.AutoWrapText(true)
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
						if (AllCommits.IsEmpty())
						{
							return LOCTEXT("NoCommits", "No commits yet.");
						}
						if (!CommitFilter.IsEmpty())
						{
							return bHasMore
								? FText::Format(LOCTEXT("CommitMatchesMore", "{0} of the latest {1} commits match. Load more to search further back."), Commits.Num(), AllCommits.Num())
								: FText::Format(LOCTEXT("CommitMatches", "{0} of {1} commits match."), Commits.Num(), AllCommits.Num());
						}
						return FText::Format(LOCTEXT("CommitCount", "{0} {0}|plural(one=commit,other=commits)"), AllCommits.Num());
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
					.Text_Lambda([this]()
					{
						return !Selected.IsValid() ? FText::GetEmpty() : IsUncommitted(Selected) ? UncommittedTitle() : FText::FromString(Selected->Subject);
					})
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
						if (!Selected.IsValid())
						{
							return FText::GetEmpty();
						}
						if (IsUncommitted(Selected))
						{
							return GetUncommittedNote();
						}
						return FText::Format(LOCTEXT("CommitByline", "{0} · {1} · {2}"), FText::FromString(Selected->Author), CommitDate(Selected->Date),
							FText::FromString(GitWorkspaceText::ShortId(Selected->Oid)));
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
							if (IsUncommitted(Selected))
							{
								return LOCTEXT("UncommittedBody", "Changes in your working copy that are not committed yet. Review, stage and commit them in the Changes tab.");
							}
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
				.AutoHeight()
				.HAlign(HAlign_Left)
				.Padding(0.0f, 4.0f)
				[
					SNew(SButton)
					.AddMetaData<FTagMetaData>(TEXT("GitPanel.History.ShowChanges"))
					.Visibility_Lambda([this]() { return IsUncommitted(Selected) ? EVisibility::Visible : EVisibility::Collapsed; })
					.Text(LOCTEXT("ShowInChanges", "Show in Changes"))
					.OnClicked_Lambda([this]()
					{
						ShowChanges();
						return FReply::Handled();
					})
				]
				// The commit's files: a filter, the folders toggle and the tree.
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 6.0f, 0.0f, 2.0f)
				[
					SNew(SHorizontalBox)
					.Visibility_Lambda([this]() { return Selected.IsValid() && !Details.Files.IsEmpty() ? EVisibility::Visible : EVisibility::Collapsed; })
					+ SHorizontalBox::Slot()
					.FillWidth(1.0f)
					[
						SNew(SSearchBox)
						.HintText_Lambda([this]() { return FText::Format(LOCTEXT("FileSearchHint", "Filter {0} {0}|plural(one=file,other=files)"), Details.Files.Num()); })
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
				// The files, and below them how the selected one changed.
				+ SVerticalBox::Slot()
				.FillHeight(1.0f)
				[
					SNew(SSplitter)
					.Orientation(Orient_Vertical)
					.Visibility_Lambda([this]() { return IsUncommitted(Selected) ? EVisibility::Collapsed : EVisibility::Visible; })
					+ SSplitter::Slot()
					.Value(0.4f)
					[
						SAssignNew(FileTree, STreeView<FFileItemPtr>)
						.AddMetaData<FTagMetaData>(TEXT("GitPanel.History.Files"))
						.TreeItemsSource(&Files)
						.SelectionMode(ESelectionMode::Multi)
						.OnGenerateRow(this, &SGitHistoryView::OnGenerateFileRow)
						.OnGetChildren(this, &SGitHistoryView::OnGetFileChildren)
						.OnExpansionChanged(this, &SGitHistoryView::OnFileExpansionChanged)
						.OnSetExpansionRecursive(this, &SGitHistoryView::OnSetFileExpansionRecursive)
						.OnSelectionChanged(this, &SGitHistoryView::OnFileSelected)
						.OnMouseButtonDoubleClick(this, &SGitHistoryView::OnFileDoubleClick)
						.OnContextMenuOpening(this, &SGitHistoryView::OnFileMenu)
						.OnKeyDownHandler(this, &SGitHistoryView::OnFileKeyDown)
					]
					+ SSplitter::Slot()
					.Value(0.6f)
					[
						SNew(SBorder)
						.BorderImage(FAppStyle::GetBrush("Brushes.Panel"))
						.Padding(2.0f)
						.Visibility_Lambda([this]() { return Selected.IsValid() ? EVisibility::Visible : EVisibility::Collapsed; })
						[
							SAssignNew(FileDiff, SGitDiffView)
						]
					]
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
	// The folders preference is shared with the other tabs.
	if (GitFileTree::ShowFolders() != bBuiltFolders)
	{
		RebuildFiles();
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
	if (!Model.IsValid())
	{
		return;
	}
	const GitCore::FSessionSnapshotRef Snapshot = Model->GetSnapshot();
	UpdateUncommitted(*Snapshot);
	if (!bLoading && !IsLoadedAt(*Snapshot))
	{
		Load();
	}
}

bool SGitHistoryView::IsLoadedAt(const GitCore::FSessionSnapshot& Snapshot) const
{
	// Branch names differ by case too.
	return Snapshot.Status.Head.Equals(LoadedHead, ESearchCase::CaseSensitive) && Snapshot.Status.Branch.Equals(LoadedBranch, ESearchCase::CaseSensitive)
		&& Snapshot.Remote.Upstream.Equals(LoadedUpstream, ESearchCase::CaseSensitive) && Snapshot.Remote.UpstreamHead.Equals(LoadedUpstreamHead, ESearchCase::CaseSensitive);
}

void SGitHistoryView::UpdateUncommitted(const GitCore::FSessionSnapshot& Snapshot)
{
	int32 Unstaged = 0;
	int32 Staged = 0;
	bool bAnyConflict = false;
	for (const GitCore::FStatusEntry& Entry : Snapshot.Status.Entries)
	{
		Unstaged += Entry.HasUnstaged() ? 1 : 0;
		Staged += Entry.HasStaged() ? 1 : 0;
		bAnyConflict |= Entry.bConflict;
	}
	const bool bWasListed = ChangedFiles > 0;
	ChangedFiles = Snapshot.Status.Entries.Num();
	UnstagedFiles = Unstaged;
	StagedFiles = Staged;
	bConflicts = bAnyConflict;
	if (bWasListed != (ChangedFiles > 0))
	{
		ApplyCommitFilter();
	}
}

FText SGitHistoryView::GetUncommittedNote() const
{
	if (UnstagedFiles > 0 && StagedFiles > 0)
	{
		return FText::Format(LOCTEXT("UnstagedAndStaged", "{0} {0}|plural(one=file,other=files) not staged · {1} staged"), UnstagedFiles, StagedFiles);
	}
	if (UnstagedFiles > 0)
	{
		return FText::Format(LOCTEXT("UnstagedOnly", "{0} {0}|plural(one=file,other=files) not staged"), UnstagedFiles);
	}
	return FText::Format(LOCTEXT("StagedOnly", "{0} {0}|plural(one=file,other=files) staged, ready to commit"), StagedFiles);
}

FSlateColor SGitHistoryView::GetUncommittedColor() const
{
	return bConflicts ? FStyleColors::Error : UnstagedFiles > 0 ? FStyleColors::AccentOrange : FStyleColors::AccentGreen;
}

void SGitHistoryView::ShowChanges()
{
	OnShowChangesDelegate.ExecuteIfBound();
}

void SGitHistoryView::FocusSearch()
{
	if (CommitSearch.IsValid())
	{
		FSlateApplication::Get().SetKeyboardFocus(CommitSearch, EFocusCause::SetDirectly);
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
	// The upstream's commit as the snapshot has it, so its label goes where the history was read from.
	const GitCore::FSessionSnapshotRef Snapshot = Model->GetSnapshot();
	const FString Head = Snapshot->Status.Head;
	const FString Branch = Snapshot->Status.Branch;
	const FString Upstream = Snapshot->Remote.Upstream;
	const FString UpstreamHead = Snapshot->Remote.UpstreamHead;
	Model->LoadHistory(Limit, UpstreamHead, [WeakThis = TWeakPtr<SGitHistoryView>(SharedThis(this)), Head, Branch, Upstream, UpstreamHead](
		TArray<GitCore::FHistoryCommit>&& Loaded, bool bMore, const FString& LoadError)
	{
		const TSharedPtr<SGitHistoryView> Self = WeakThis.Pin();
		if (!Self.IsValid())
		{
			return;
		}
		Self->bLoading = false;
		Self->LoadedHead = Head;
		Self->LoadedBranch = Branch;
		Self->LoadedUpstream = Upstream;
		Self->LoadedUpstreamHead = UpstreamHead;
		Self->bHasMore = bMore;
		Self->Error = LoadError;
		Self->Displays.Reset();
		const FCommitPtr Previous = Self->Selected;
		Self->AllCommits.Reset();
		for (GitCore::FHistoryCommit& Commit : Loaded)
		{
			Self->AllCommits.Add(MakeShared<GitCore::FHistoryCommit>(MoveTemp(Commit)));
		}
		Self->ApplyCommitFilter();
		if (Previous.IsValid() && !Self->IsUncommitted(Previous))
		{
			const FString& PreviousOid = Previous->Oid;
			if (const FCommitPtr* Reselect = Self->Commits.FindByPredicate([&PreviousOid](const FCommitPtr& Commit) { return Commit->Oid.Equals(PreviousOid, ESearchCase::CaseSensitive); }))
			{
				Self->CommitList->SetSelection(*Reselect, ESelectInfo::Direct);
			}
		}
		// HEAD or the upstream may have moved while the history was read.
		Self->RefreshIfNeeded();
	});
}

void SGitHistoryView::ApplyCommitFilter()
{
	Commits = AllCommits.FilterByPredicate([this](const FCommitPtr& Commit) { return CommitMatches(*Commit, CommitFilter); });
	if (CommitFilter.IsEmpty() && ChangedFiles > 0)
	{
		// Committing them would put them on top of HEAD's commit.
		const int32 HeadIndex = Commits.IndexOfByPredicate([this](const FCommitPtr& Commit) { return Commit->Oid.Equals(LoadedHead, ESearchCase::CaseSensitive); });
		Commits.Insert(Uncommitted, HeadIndex == INDEX_NONE ? 0 : HeadIndex);
	}
	if (CommitList.IsValid())
	{
		CommitList->RequestListRefresh();
	}
}

TSharedRef<ITableRow> SGitHistoryView::OnGenerateCommitRow(FCommitPtr Commit, const TSharedRef<STableViewBase>& Owner)
{
	if (IsUncommitted(Commit))
	{
		return SNew(SGitCommitRow, Owner)
			.Commit(Commit)
			.bUncommitted(true)
			.Note(this, &SGitHistoryView::GetUncommittedNote)
			.NoteColor(this, &SGitHistoryView::GetUncommittedColor);
	}
	FText Branch;
	FText BranchTooltip;
	if (!LoadedHead.IsEmpty() && Commit->Oid.Equals(LoadedHead, ESearchCase::CaseSensitive))
	{
		Branch = LoadedBranch.IsEmpty() ? LOCTEXT("DetachedLabel", "HEAD") : FText::FromString(LoadedBranch);
		BranchTooltip = LoadedBranch.IsEmpty() ? LOCTEXT("DetachedTooltip", "HEAD is at this commit. No branch is checked out.")
			: FText::Format(LOCTEXT("BranchLabelTooltip", "Your branch {0} is at this commit."), Branch);
	}
	const bool bUpstream = !LoadedUpstreamHead.IsEmpty() && Commit->Oid.Equals(LoadedUpstreamHead, ESearchCase::CaseSensitive);
	return SNew(SGitCommitRow, Owner)
		.Commit(Commit)
		.Branch(Branch)
		.BranchTooltip(BranchTooltip)
		.Upstream(bUpstream ? FText::FromString(LoadedUpstream) : FText::GetEmpty())
		.HighlightText(this, &SGitHistoryView::GetCommitFilterText);
}

void SGitHistoryView::OnCommitSelected(FCommitPtr Commit, ESelectInfo::Type SelectInfo)
{
	Selected = Commit;
	Details = GitCore::FCommitDetails();
	RebuildFiles();
	const TSharedPtr<FGitWorkspaceModel> Model = WeakModel.Pin();
	if (!Commit.IsValid() || IsUncommitted(Commit) || !Model.IsValid())
	{
		return;
	}
	Model->LoadCommit(Commit->Oid, [WeakThis = TWeakPtr<SGitHistoryView>(SharedThis(this)), Oid = Commit->Oid](GitCore::FCommitDetails&& Loaded, const FString& LoadError)
	{
		const TSharedPtr<SGitHistoryView> Self = WeakThis.Pin();
		if (!Self.IsValid() || !Self->Selected.IsValid() || !Self->Selected->Oid.Equals(Oid, ESearchCase::CaseSensitive))
		{
			return;
		}
		Self->Details = MoveTemp(Loaded);
		Self->RebuildFiles();
	});
}

void SGitHistoryView::OnCommitDoubleClick(FCommitPtr Commit)
{
	if (IsUncommitted(Commit))
	{
		ShowChanges();
	}
}

void SGitHistoryView::RebuildFiles()
{
	if (!FileTree.IsValid())
	{
		return;
	}
	const TSharedPtr<FGitWorkspaceModel> Model = WeakModel.Pin();
	bBuiltFolders = GitFileTree::ShowFolders();
	TSet<FString> Picked;
	for (const FFileItemPtr& Item : FileTree->GetSelectedItems())
	{
		Picked.Add(Item->Path);
	}
	TArray<TPair<FString, FFileItemPtr>> Named;
	for (const GitCore::FHistoryChange& Change : Details.Files)
	{
		const FString Name = Displays.Get(Model.Get(), Change.Path).Name.ToString();
		if (!FileFilter.IsEmpty() && !Change.Path.Contains(FileFilter) && !Change.OldPath.Contains(FileFilter) && !Name.Contains(FileFilter))
		{
			continue;
		}
		const FFileItemPtr Item = MakeShared<FGitHistoryFileItem>();
		Item->Path = Change.Path;
		Item->Change = MakeShared<GitCore::FHistoryChange>(Change);
		// In folders, files go by the name shown; a flat list goes by path.
		Named.Emplace(bBuiltFolders ? Name : Change.Path, Item);
	}
	Named.Sort([](const TPair<FString, FFileItemPtr>& A, const TPair<FString, FFileItemPtr>& B)
	{
		const int32 Order = A.Key.Compare(B.Key, ESearchCase::IgnoreCase);
		return Order != 0 ? Order < 0 : A.Value->Path < B.Value->Path;
	});
	TArray<FFileItemPtr> Flat;
	for (const TPair<FString, FFileItemPtr>& Pair : Named)
	{
		Flat.Add(Pair.Value);
	}
	Files = GitFileTree::Build(Flat, bBuiltFolders);
	bRestoringExpansion = true;
	GitFileTree::RestoreExpansion(*FileTree, Files, CollapsedFolders, !FileFilter.IsEmpty());
	bRestoringExpansion = false;
	FileTree->RequestTreeRefresh();
	bRebuildingFiles = true;
	TArray<FFileItemPtr> Reselect;
	TFunction<void(const TArray<FFileItemPtr>&)> Visit = [&Visit, &Picked, &Reselect](const TArray<FFileItemPtr>& Items)
	{
		for (const FFileItemPtr& Item : Items)
		{
			if (Picked.Contains(Item->Path))
			{
				Reselect.Add(Item);
			}
			Visit(Item->Children);
		}
	};
	Visit(Files);
	FileTree->ClearSelection();
	FileTree->SetItemSelection(Reselect, true);
	bRebuildingFiles = false;
	UpdateFileDiff();
}

void SGitHistoryView::OnFileSelected(FFileItemPtr Item, ESelectInfo::Type SelectInfo)
{
	if (!bRebuildingFiles)
	{
		UpdateFileDiff();
	}
}

void SGitHistoryView::UpdateFileDiff()
{
	const TSharedPtr<FGitWorkspaceModel> Model = WeakModel.Pin();
	if (!FileDiff.IsValid() || !Model.IsValid())
	{
		return;
	}
	const TArray<FFileItemPtr> Picked = FileTree->GetSelectedItems();
	if (Picked.Num() != 1 || Picked[0]->bFolder || !Selected.IsValid())
	{
		FileDiffKey.Reset();
		FileDiff->SetMessage(LOCTEXT("SelectFileDiff", "Select a file to see how it changed."));
		return;
	}
	const GitCore::FHistoryChange& Change = *Picked[0]->Change;
	if (Displays.Get(Model.Get(), Change.Path).bPackage)
	{
		FileDiffKey.Reset();
		FileDiff->SetMessage(LOCTEXT("AssetFileDiff", "Unreal asset: double-click it to compare the two versions in Unreal's own diff."));
		return;
	}
	const FString Key = Selected->Oid + TEXT("|") + Change.Path;
	if (Key == FileDiffKey)
	{
		return;
	}
	FileDiffKey = Key;
	FileDiff->SetMessage(LOCTEXT("DiffLoading", "Loading changes…"));
	Model->LoadCommitDiff(Selected->Oid, Change.Path, [WeakThis = TWeakPtr<SGitHistoryView>(SharedThis(this)), Key](FString&& Diff, const FString& DiffError)
	{
		const TSharedPtr<SGitHistoryView> Self = WeakThis.Pin();
		if (!Self.IsValid() || Self->FileDiffKey != Key)
		{
			return;
		}
		if (DiffError.IsEmpty())
		{
			Self->FileDiff->SetDiff(Diff);
		}
		else
		{
			Self->FileDiff->SetMessage(FText::FromString(DiffError));
		}
	});
}

TSharedRef<ITableRow> SGitHistoryView::OnGenerateFileRow(FFileItemPtr Item, const TSharedRef<STableViewBase>& Owner)
{
	const TSharedRef<GitFileTree::STreeRow<FGitHistoryFileItem>> Row = SNew(GitFileTree::STreeRow<FGitHistoryFileItem>, Owner).Item(Item);
	const TAttribute<FText> Highlight(this, &SGitHistoryView::GetFileFilterText);
	if (Item->bFolder)
	{
		Row->SetCell(GitFileTree::MakeNameCell(Row, true, FText::FromString(Item->Label), FText::AsNumber(Item->FileCount), nullptr,
			FText::Format(LOCTEXT("FolderTooltip", "{0}\n{1} {1}|plural(one=file,other=files). {2} opens or closes everything in it."),
				FText::FromString(Item->Path), Item->FileCount, GitWorkspaceText::AltClick()),
			Highlight));
		return Row;
	}
	const GitCore::FHistoryChange& Change = *Item->Change;
	const FGitFileDisplay& Display = Displays.Get(WeakModel.Pin().Get(), Change.Path);
	const bool bRenamed = !Change.OldPath.IsEmpty() && Change.OldPath != Change.Path;
	FText Note = bBuiltFolders ? FText::GetEmpty() : Display.Folder;
	if (bRenamed)
	{
		Note = FText::Format(LOCTEXT("RenamedFrom", "from {0}"), FText::FromString(Change.OldPath));
	}
	const FString Location = bRenamed ? Change.OldPath + TEXT(" → ") + Change.Path : Change.Path;
	Row->SetCell(GitFileTree::MakeNameCell(Row, false, Display.Name, Note, ChangeBrush(Change.Status),
		FText::Format(LOCTEXT("ChangeTooltip", "{0}\nDouble-click to compare with the previous version."), FText::FromString(Location)), Highlight));
	return Row;
}

void SGitHistoryView::OnGetFileChildren(FFileItemPtr Item, TArray<FFileItemPtr>& OutChildren) const
{
	if (Item.IsValid())
	{
		OutChildren = Item->Children;
	}
}

void SGitHistoryView::OnFileExpansionChanged(FFileItemPtr Item, bool bExpanded)
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

void SGitHistoryView::OnSetFileExpansionRecursive(FFileItemPtr Item, bool bExpand)
{
	if (FileTree.IsValid())
	{
		GitFileTree::SetExpansionRecursive(*FileTree, Item, bExpand);
	}
}

void SGitHistoryView::OnFileDoubleClick(FFileItemPtr Item)
{
	if (!Item.IsValid())
	{
		return;
	}
	if (Item->bFolder)
	{
		FileTree->SetItemExpansion(Item, !FileTree->IsItemExpanded(Item));
		return;
	}
	CompareChange(Item->Change);
}

void SGitHistoryView::CompareChange(const TSharedPtr<GitCore::FHistoryChange>& Change)
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
	FMenuBuilder Menu(true, nullptr);
	if (IsUncommitted(Selected))
	{
		Menu.AddMenuEntry(LOCTEXT("ShowInChanges", "Show in Changes"), LOCTEXT("ShowInChangesTooltip", "Review, stage and commit them in the Changes tab."),
			FSlateIcon(FRevisionControlStyleManager::GetStyleSetName(), "RevisionControl.ModifiedLocally"), FUIAction(FExecuteAction::CreateSP(this, &SGitHistoryView::ShowChanges)));
		return Menu.MakeWidget();
	}
	const FString Oid = Selected->Oid;
	const FString Subject = Selected->Subject;
	GitWorkspaceText::AddMenuEntry(Menu, LOCTEXT("CopyId", "Copy commit id"), FText::GetEmpty(), FSlateIcon(FAppStyle::GetAppStyleSetName(), "GenericCommands.Copy"),
		FUIAction(FExecuteAction::CreateLambda([Oid]() { FPlatformApplicationMisc::ClipboardCopy(*Oid); })), GitWorkspaceText::Shortcut(EModifierKey::Control, EKeys::C));
	Menu.AddMenuEntry(LOCTEXT("CopySummary", "Copy summary"), FText::GetEmpty(), FSlateIcon(FAppStyle::GetAppStyleSetName(), "GenericCommands.Copy"),
		FUIAction(FExecuteAction::CreateLambda([Subject]() { FPlatformApplicationMisc::ClipboardCopy(*Subject); })));
	return Menu.MakeWidget();
}

TSharedPtr<SWidget> SGitHistoryView::OnFileMenu()
{
	const TArray<FFileItemPtr> Picked = FileTree->GetSelectedItems();
	if (Picked.IsEmpty())
	{
		return nullptr;
	}
	TArray<FString> ItemPaths;
	TArray<FFileItemPtr> Folders;
	for (const FFileItemPtr& Item : Picked)
	{
		ItemPaths.Add(Item->Path);
		if (Item->bFolder)
		{
			Folders.Add(Item);
		}
	}
	FMenuBuilder Menu(true, nullptr);
	if (Picked.Num() == 1 && !Picked[0]->bFolder)
	{
		GitWorkspaceText::AddMenuEntry(Menu, LOCTEXT("CompareChange", "Compare with previous version"), FText::GetEmpty(),
			FSlateIcon(FRevisionControlStyleManager::GetStyleSetName(), "RevisionControl.Actions.Diff"),
			FUIAction(FExecuteAction::CreateLambda([this, Change = Picked[0]->Change]() { CompareChange(Change); })), GitWorkspaceText::Shortcut(EModifierKey::Control, EKeys::D));
	}
	if (const TSharedPtr<FGitWorkspaceModel> M = WeakModel.Pin())
	{
		GitFileTree::AddShowEntries(Menu, M.ToSharedRef(), ItemPaths);
	}
	GitFileTree::AddCopyPathEntry(Menu, ItemPaths);
	if (!Folders.IsEmpty())
	{
		Menu.AddMenuSeparator();
		GitFileTree::AddFolderEntries(Menu, [this, Folders](bool bExpand) { for (const FFileItemPtr& Folder : Folders) { OnSetFileExpansionRecursive(Folder, bExpand); } });
	}
	return Menu.MakeWidget();
}

FReply SGitHistoryView::OnCommitKeyDown(const FGeometry&, const FKeyEvent& InKeyEvent)
{
	if (IsUncommitted(Selected))
	{
		if (InKeyEvent.GetKey() == EKeys::Enter)
		{
			ShowChanges();
			return FReply::Handled();
		}
		return FReply::Unhandled();
	}
	if (GitFileTree::IsCopyKey(InKeyEvent) && Selected.IsValid())
	{
		FPlatformApplicationMisc::ClipboardCopy(*Selected->Oid);
		return FReply::Handled();
	}
	return FReply::Unhandled();
}

FReply SGitHistoryView::OnFileKeyDown(const FGeometry&, const FKeyEvent& InKeyEvent)
{
	const TArray<FFileItemPtr> Picked = FileTree->GetSelectedItems();
	if (GitFileTree::IsCopyKey(InKeyEvent) && !Picked.IsEmpty())
	{
		TArray<FString> Paths;
		for (const FFileItemPtr& Item : Picked)
		{
			Paths.Add(Item->Path);
		}
		GitFileTree::CopyPaths(Paths);
		return FReply::Handled();
	}
	// As in the Changes tab: Ctrl+D compares a file, and Enter does what a double-click does.
	const bool bCompareKey = InKeyEvent.GetKey() == EKeys::D && (InKeyEvent.IsControlDown() || InKeyEvent.IsCommandDown()) && !InKeyEvent.IsShiftDown() && !InKeyEvent.IsAltDown();
	if (Picked.Num() == 1 && ((bCompareKey && !Picked[0]->bFolder) || InKeyEvent.GetKey() == EKeys::Enter))
	{
		OnFileDoubleClick(Picked[0]);
		return FReply::Handled();
	}
	return FReply::Unhandled();
}

#undef LOCTEXT_NAMESPACE
