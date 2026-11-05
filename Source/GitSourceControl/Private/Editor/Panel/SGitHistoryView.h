// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#pragma once

#include "CoreMinimal.h"
#include "Core/GitTypes.h"
#include "GitFileTree.h"
#include "GitHistoryGraph.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/SListView.h"
#include "Widgets/Views/STreeView.h"

class FGitWorkspaceModel;
class SGitDiffView;
class SSearchBox;
class SSplitter;

/** A file a commit changed, or a folder of them. */
struct FGitHistoryFileItem
{
	bool bFolder = false;
	FString Path;
	FString Label;
	int32 FileCount = 0;
	TArray<TSharedPtr<FGitHistoryFileItem>> Children;
	TSharedPtr<GitCore::FHistoryChange> Change;
};

/**
 * History tab: the commits of HEAD and of its upstream, newest first, with the selected commit's files. Your branch and its
 * upstream are labelled on the commits they point to, and uncommitted changes show as a row above HEAD's commit.
 */
class SGitHistoryView : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SGitHistoryView) {}
		/** The uncommitted changes' row was opened: show them in the Changes tab. */
		SLATE_EVENT(FSimpleDelegate, OnShowChanges)
	SLATE_END_ARGS()

	using FCommitPtr = TSharedPtr<GitCore::FHistoryCommit>;
	using FFileItemPtr = TSharedPtr<FGitHistoryFileItem>;

	void Construct(const FArguments& InArgs, const TSharedRef<FGitWorkspaceModel>& InModel);
	virtual ~SGitHistoryView() override;
	virtual void Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime) override;
	/** Loads history when HEAD, the branch or its upstream moved since the last load, and counts the uncommitted changes. */
	void RefreshIfNeeded();
	void FocusSearch();
	FText GetCommitFilterText() const { return FText::FromString(CommitFilter); }
	FText GetFileFilterText() const { return FText::FromString(FileFilter); }
	/** The row of the list (public for tests: rows are otherwise made when the list is painted). */
	TSharedRef<ITableRow> OnGenerateCommitRow(FCommitPtr Commit, const TSharedRef<STableViewBase>& Owner);
	/** The item of the uncommitted changes' row (no commit id). */
	bool IsUncommitted(const FCommitPtr& Commit) const { return Commit.IsValid() && Commit == Uncommitted; }
	/** A listed commit's row of the graph; none while searching, when the rows listed are not each other's neighbours. */
	const GitHistoryGraph::FRow* FindGraphRow(FCommitPtr Commit) const { return GraphRows.Find(Commit.Get()); }
	/** The lanes the graph makes room for. */
	int32 GetGraphLanes() const { return GraphLanes; }

private:
	void Load();
	void OnModelChanged();
	/** The history shown was read where Snapshot's HEAD, branch and upstream are. */
	bool IsLoadedAt(const GitCore::FSessionSnapshot& Snapshot) const;
	/** Counts the uncommitted changes, adding or removing their row. */
	void UpdateUncommitted(const GitCore::FSessionSnapshot& Snapshot);
	/** "3 files not staged · 2 staged". */
	FText GetUncommittedNote() const;
	/** Orange while changes are not staged (as in the Changes tab), green when all are, red with conflicts. */
	FSlateColor GetUncommittedColor() const;
	void ShowChanges();
	/** Shows the loaded commits that match the search; without a search, the uncommitted changes above HEAD's commit. */
	void ApplyCommitFilter();
	/** Lays out the selected commit's files that match the file filter. */
	void RebuildFiles();
	void OnCommitSelected(FCommitPtr Commit, ESelectInfo::Type SelectInfo);
	void OnCommitDoubleClick(FCommitPtr Commit);
	TSharedRef<ITableRow> OnGenerateFileRow(FFileItemPtr Item, const TSharedRef<STableViewBase>& Owner);
	void OnGetFileChildren(FFileItemPtr Item, TArray<FFileItemPtr>& OutChildren) const;
	void OnFileExpansionChanged(FFileItemPtr Item, bool bExpanded);
	void OnSetFileExpansionRecursive(FFileItemPtr Item, bool bExpand);
	void OnFileDoubleClick(FFileItemPtr Item);
	void OnFileSelected(FFileItemPtr Item, ESelectInfo::Type SelectInfo);
	/** Shows how the single selected text file changed in the commit, below the files. */
	void UpdateFileDiff();
	TSharedPtr<SWidget> OnCommitMenu();
	TSharedPtr<SWidget> OnFileMenu();
	/** Ctrl+C copies the commit id; Enter on the uncommitted changes shows them. */
	FReply OnCommitKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent);
	/** Ctrl+C copies the selected paths, Ctrl+D compares a file and Enter does what a double-click does. */
	FReply OnFileKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent);
	void CompareChange(const TSharedPtr<GitCore::FHistoryChange>& Change);

	TWeakPtr<FGitWorkspaceModel> WeakModel;
	FDelegateHandle ChangedHandle;
	FSimpleDelegate OnShowChangesDelegate;
	/** Loaded commits, newest first. */
	TArray<FCommitPtr> AllCommits;
	/** The loaded commits that match the search, and the uncommitted changes' row. */
	TArray<FCommitPtr> Commits;
	/** The uncommitted changes' row, listed while there are some. */
	FCommitPtr Uncommitted;
	/** The graph of the commits listed, by commit. */
	TMap<const GitCore::FHistoryCommit*, GitHistoryGraph::FRow> GraphRows;
	int32 GraphLanes = 0;
	int32 ChangedFiles = 0;
	int32 UnstagedFiles = 0;
	int32 StagedFiles = 0;
	bool bConflicts = false;
	/** The selected commit's files, in folders when the panel shows them. */
	TArray<FFileItemPtr> Files;
	TSharedPtr<SListView<FCommitPtr>> CommitList;
	TSharedPtr<STreeView<FFileItemPtr>> FileTree;
	TSharedPtr<SSearchBox> CommitSearch;
	TSharedPtr<SGitDiffView> FileDiff;
	/** The commit and file of the diff shown. */
	FString FileDiffKey;
	/** Set while the file list is rebuilt, which clears and restores the selection. */
	bool bRebuildingFiles = false;
	TSharedPtr<SSplitter> Splitter;
	FCommitPtr Selected;
	GitCore::FCommitDetails Details;
	FGitFileDisplays Displays;
	TSet<FString> CollapsedFolders;
	FString CommitFilter;
	FString FileFilter;
	bool bBuiltFolders = true;
	bool bRestoringExpansion = false;
	/** Where HEAD, its branch and its upstream were when the history shown was read. */
	FString LoadedHead;
	FString LoadedBranch;
	FString LoadedUpstream;
	FString LoadedUpstreamHead;
	FString Error;
	int32 Limit = 200;
	bool bHasMore = false;
	bool bLoading = false;
	bool bVisible = false;
};
