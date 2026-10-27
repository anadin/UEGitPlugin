// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#pragma once

#include "CoreMinimal.h"
#include "Core/GitTypes.h"
#include "GitFileTree.h"
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

/** History tab: commits reachable from HEAD, newest first, with the selected commit's files. */
class SGitHistoryView : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SGitHistoryView) {}
	SLATE_END_ARGS()

	using FCommitPtr = TSharedPtr<GitCore::FHistoryCommit>;
	using FFileItemPtr = TSharedPtr<FGitHistoryFileItem>;

	void Construct(const FArguments& InArgs, const TSharedRef<FGitWorkspaceModel>& InModel);
	virtual ~SGitHistoryView() override;
	virtual void Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime) override;
	/** Loads history when HEAD moved since the last load. */
	void RefreshIfNeeded();
	void FocusSearch();
	FText GetCommitFilterText() const { return FText::FromString(CommitFilter); }
	FText GetFileFilterText() const { return FText::FromString(FileFilter); }

private:
	void Load();
	void OnModelChanged();
	/** Shows the loaded commits that match the search. */
	void ApplyCommitFilter();
	/** Lays out the selected commit's files that match the file filter. */
	void RebuildFiles();
	void OnCommitSelected(FCommitPtr Commit, ESelectInfo::Type SelectInfo);
	TSharedRef<ITableRow> OnGenerateCommitRow(FCommitPtr Commit, const TSharedRef<STableViewBase>& Owner);
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
	void CompareChange(const TSharedPtr<GitCore::FHistoryChange>& Change);
	bool IsUnpushed(const FCommitPtr& Commit) const;

	TWeakPtr<FGitWorkspaceModel> WeakModel;
	FDelegateHandle ChangedHandle;
	/** Loaded commits, newest first. */
	TArray<FCommitPtr> AllCommits;
	/** The loaded commits that match the search. */
	TArray<FCommitPtr> Commits;
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
	FString LoadedHead;
	FString Error;
	int32 Limit = 200;
	bool bHasMore = false;
	bool bLoading = false;
	bool bVisible = false;
};
