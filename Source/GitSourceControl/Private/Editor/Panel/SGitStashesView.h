// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#pragma once

#include "CoreMinimal.h"
#include "Core/GitTypes.h"
#include "GitFileTree.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/SListView.h"
#include "Widgets/Views/STreeView.h"

class FGitWorkspaceModel;
class SSearchBox;

/** Stashes tab: put changes aside, bring them back, or delete them. */
class SGitStashesView : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SGitStashesView) {}
	SLATE_END_ARGS()

	using FStashPtr = TSharedPtr<GitCore::FStashEntry>;
	using FPathItemPtr = TSharedPtr<FGitPathItem>;

	void Construct(const FArguments& InArgs, const TSharedRef<FGitWorkspaceModel>& InModel);
	virtual ~SGitStashesView() override;
	virtual void Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime) override;
	void FocusSearch();
	FText GetStashFilterText() const { return FText::FromString(StashFilter); }
	FText GetFileFilterText() const { return FText::FromString(FileFilter); }

private:
	void OnModelChanged();
	/** Shows the stashes whose description matches the search. */
	void ApplyStashFilter();
	/** Lays out the selected stash's files that match the file filter. */
	void RebuildFiles();
	void OnSelected(FStashPtr Stash, ESelectInfo::Type SelectInfo);
	TSharedRef<ITableRow> OnGenerateRow(FStashPtr Stash, const TSharedRef<STableViewBase>& Owner);
	TSharedRef<ITableRow> OnGenerateFileRow(FPathItemPtr Item, const TSharedRef<STableViewBase>& Owner);
	void OnGetFileChildren(FPathItemPtr Item, TArray<FPathItemPtr>& OutChildren) const;
	void OnFileExpansionChanged(FPathItemPtr Item, bool bExpanded);
	void OnSetFileExpansionRecursive(FPathItemPtr Item, bool bExpand);
	void OnFileDoubleClick(FPathItemPtr Item);
	TSharedPtr<SWidget> OnContextMenu();
	TSharedPtr<SWidget> OnFileMenu();
	FStashPtr Selection() const;
	void StashChanges();
	void Apply(bool bDrop);
	void Drop();

	TWeakPtr<FGitWorkspaceModel> WeakModel;
	FDelegateHandle ChangedHandle;
	TArray<FStashPtr> AllStashes;
	/** The stashes that match the search. */
	TArray<FStashPtr> Stashes;
	TSharedPtr<SListView<FStashPtr>> List;
	TSharedPtr<SSearchBox> SearchBox;
	/** The selected stash's files, and the tree shown for them. */
	TArray<FString> StashPaths;
	TArray<FPathItemPtr> Files;
	TSharedPtr<STreeView<FPathItemPtr>> FileTree;
	FGitFileDisplays Displays;
	TSet<FString> CollapsedFolders;
	FString StashFilter;
	FString FileFilter;
	uint64 BuiltGeneration = MAX_uint64;
	bool bBuiltFolders = true;
	bool bRestoringExpansion = false;
	bool bIncludeNewFiles = false;
};
