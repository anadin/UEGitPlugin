// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#include "SGitHistoryView.h"

#include "Editor/GitWorkspaceModel.h"
#include "Editor/GitWorkspaceText.h"
#include "SGitDiffView.h"

#include "Framework/Application/SlateApplication.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "HAL/PlatformApplicationMisc.h"
#include "RevisionControlStyle/RevisionControlStyle.h"
#include "Styling/AppStyle.h"
#include "Types/ISlateMetaData.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SSearchBox.h"
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

	/** Summary and author contain the search, or the commit id starts with it. */
	bool CommitMatches(const GitCore::FHistoryCommit& Commit, const FString& Filter)
	{
		return Filter.IsEmpty() || Commit.Subject.Contains(Filter) || Commit.Author.Contains(Filter) || Commit.Oid.StartsWith(Filter);
	}

	class SGitCommitRow : public SMultiColumnTableRow<SGitHistoryView::FCommitPtr>
	{
	public:
		SLATE_BEGIN_ARGS(SGitCommitRow) {}
			SLATE_ARGUMENT(SGitHistoryView::FCommitPtr, Commit)
			SLATE_ARGUMENT(bool, bUnpushed)
			SLATE_ATTRIBUTE(FText, HighlightText)
		SLATE_END_ARGS()

		void Construct(const FArguments& InArgs, const TSharedRef<STableViewBase>& Owner)
		{
			Commit = InArgs._Commit;
			bUnpushed = InArgs._bUnpushed;
			Highlight = InArgs._HighlightText;
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
						.HighlightText(Highlight)
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
				.HighlightText(Column == DateColumn ? TAttribute<FText>() : Highlight)
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				.OverflowPolicy(ETextOverflowPolicy::Ellipsis);
		}

	private:
		SGitHistoryView::FCommitPtr Commit;
		bool bUnpushed = false;
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
				.HintText(LOCTEXT("SearchHint", "Search commits by summary, author or id (Ctrl+F)"))
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
	if (Model.IsValid() && !bLoading && Model->GetSnapshot()->Status.Head != LoadedHead)
	{
		Load();
	}
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
		Self->Displays.Reset();
		const FString SelectedOid = Self->Selected.IsValid() ? Self->Selected->Oid : FString();
		Self->AllCommits.Reset();
		for (GitCore::FHistoryCommit& Commit : Loaded)
		{
			Self->AllCommits.Add(MakeShared<GitCore::FHistoryCommit>(MoveTemp(Commit)));
		}
		Self->ApplyCommitFilter();
		if (const FCommitPtr* Reselect = Self->Commits.FindByPredicate([&SelectedOid](const FCommitPtr& Commit) { return Commit->Oid == SelectedOid; }))
		{
			Self->CommitList->SetSelection(*Reselect, ESelectInfo::Direct);
		}
	});
}

void SGitHistoryView::ApplyCommitFilter()
{
	Commits = AllCommits.FilterByPredicate([this](const FCommitPtr& Commit) { return CommitMatches(*Commit, CommitFilter); });
	if (CommitList.IsValid())
	{
		CommitList->RequestListRefresh();
	}
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
	const int32 Index = AllCommits.IndexOfByKey(Commit);
	return Index != INDEX_NONE && Index < Remote.Ahead;
}

TSharedRef<ITableRow> SGitHistoryView::OnGenerateCommitRow(FCommitPtr Commit, const TSharedRef<STableViewBase>& Owner)
{
	return SNew(SGitCommitRow, Owner)
		.Commit(Commit)
		.bUnpushed(IsUnpushed(Commit))
		.HighlightText(this, &SGitHistoryView::GetCommitFilterText);
}

void SGitHistoryView::OnCommitSelected(FCommitPtr Commit, ESelectInfo::Type SelectInfo)
{
	Selected = Commit;
	Details = GitCore::FCommitDetails();
	RebuildFiles();
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
		Self->RebuildFiles();
	});
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
			FText::Format(LOCTEXT("FolderTooltip", "{0}\n{1} {1}|plural(one=file,other=files). Alt+click opens or closes everything in it."), FText::FromString(Item->Path), Item->FileCount),
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
	const FString Oid = Selected->Oid;
	const FString Subject = Selected->Subject;
	FMenuBuilder Menu(true, nullptr);
	Menu.AddMenuEntry(LOCTEXT("CopyId", "Copy commit id"), FText::GetEmpty(), FSlateIcon(FAppStyle::GetAppStyleSetName(), "GenericCommands.Copy"),
		FUIAction(FExecuteAction::CreateLambda([Oid]() { FPlatformApplicationMisc::ClipboardCopy(*Oid); })));
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
		Menu.AddMenuEntry(LOCTEXT("CompareChange", "Compare with previous version"), FText::GetEmpty(),
			FSlateIcon(FRevisionControlStyleManager::GetStyleSetName(), "RevisionControl.Actions.Diff"),
			FUIAction(FExecuteAction::CreateLambda([this, Change = Picked[0]->Change]() { CompareChange(Change); })));
	}
	if (const TSharedPtr<FGitWorkspaceModel> M = WeakModel.Pin())
	{
		GitFileTree::AddShowEntries(Menu, M.ToSharedRef(), ItemPaths);
	}
	Menu.AddMenuEntry(LOCTEXT("CopyChangePath", "Copy path"), FText::GetEmpty(), FSlateIcon(FAppStyle::GetAppStyleSetName(), "GenericCommands.Copy"),
		FUIAction(FExecuteAction::CreateLambda([ItemPaths]() { FPlatformApplicationMisc::ClipboardCopy(*FString::Join(ItemPaths, LINE_TERMINATOR)); })));
	if (!Folders.IsEmpty())
	{
		Menu.AddMenuSeparator();
		Menu.AddMenuEntry(LOCTEXT("ExpandAllMenu", "Open all inside"), LOCTEXT("ExpandAllMenuTooltip", "Open these folders and every folder in them. (Alt+click)"),
			FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.FolderOpen"),
			FUIAction(FExecuteAction::CreateLambda([this, Folders]() { for (const FFileItemPtr& Folder : Folders) { OnSetFileExpansionRecursive(Folder, true); } })));
		Menu.AddMenuEntry(LOCTEXT("CollapseAllMenu", "Close all inside"), LOCTEXT("CollapseAllMenuTooltip", "Close these folders and every folder in them. (Alt+click)"),
			FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.FolderClosed"),
			FUIAction(FExecuteAction::CreateLambda([this, Folders]() { for (const FFileItemPtr& Folder : Folders) { OnSetFileExpansionRecursive(Folder, false); } })));
	}
	return Menu.MakeWidget();
}

#undef LOCTEXT_NAMESPACE
