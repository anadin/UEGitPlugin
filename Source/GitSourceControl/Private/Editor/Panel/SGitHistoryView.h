// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#pragma once

#include "CoreMinimal.h"
#include "Core/GitTypes.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/SListView.h"

class FGitWorkspaceModel;
class SSplitter;

/** History tab: commits reachable from HEAD, newest first, with the selected commit's files. */
class SGitHistoryView : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SGitHistoryView) {}
	SLATE_END_ARGS()

	using FCommitPtr = TSharedPtr<GitCore::FHistoryCommit>;
	using FChangePtr = TSharedPtr<GitCore::FHistoryChange>;

	void Construct(const FArguments& InArgs, const TSharedRef<FGitWorkspaceModel>& InModel);
	virtual ~SGitHistoryView() override;
	virtual void Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime) override;
	/** Loads history when HEAD moved since the last load. */
	void RefreshIfNeeded();

private:
	void Load();
	void OnModelChanged();
	void OnCommitSelected(FCommitPtr Commit, ESelectInfo::Type SelectInfo);
	TSharedRef<ITableRow> OnGenerateCommitRow(FCommitPtr Commit, const TSharedRef<STableViewBase>& Owner);
	TSharedRef<ITableRow> OnGenerateChangeRow(FChangePtr Change, const TSharedRef<STableViewBase>& Owner);
	TSharedPtr<SWidget> OnCommitMenu();
	TSharedPtr<SWidget> OnChangeMenu();
	void CompareChange(FChangePtr Change);
	bool IsUnpushed(const FCommitPtr& Commit) const;

	TWeakPtr<FGitWorkspaceModel> WeakModel;
	FDelegateHandle ChangedHandle;
	TArray<FCommitPtr> Commits;
	TArray<FChangePtr> Changes;
	TSharedPtr<SListView<FCommitPtr>> CommitList;
	TSharedPtr<SListView<FChangePtr>> ChangeList;
	TSharedPtr<SSplitter> Splitter;
	FCommitPtr Selected;
	GitCore::FCommitDetails Details;
	FString LoadedHead;
	FString Error;
	int32 Limit = 200;
	bool bHasMore = false;
	bool bLoading = false;
	bool bVisible = false;
};
