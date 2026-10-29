// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#pragma once

#include "CoreMinimal.h"
#include "Editor/GitWorkspaceText.h"
#include "GitFileTree.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/STreeView.h"

class FGitWorkspaceModel;
class SGitDiffView;
class SHeaderRow;
class SMultiLineEditableTextBox;
class SSearchBox;

/** A list's group (Conflicts, Changes, Staged), a folder in it, or one side of a changed file. */
struct FGitChangeItem
{
	enum class EGroup : uint8
	{
		Conflicts,
		Staged,
		Unstaged
	};

	bool bGroup = false;
	bool bFolder = false;
	EGroup Group = EGroup::Unstaged;
	/** Groups: files in the group before filtering. */
	int32 Total = 0;
	/** Groups and folders: files below, after filtering. */
	int32 FileCount = 0;
	/** A file's repository path, or a folder's. */
	FString Path;
	/** What a folder row shows ("Maps/Main" for a folder that only holds a folder). */
	FString Label;
	TArray<TSharedPtr<FGitChangeItem>> Children;
	GitCore::FStatusEntry Entry;

	bool IsFile() const { return !bGroup && !bFolder; }
	bool IsStagedSide() const { return Group == EGroup::Staged; }
};

/**
 * Changes tab: changed files in separate lists for Changes and Staged (and Conflicts when there are
 * any), whose title bars are dividers that resize them, then an inspector and the commit box.
 */
class SGitChangesView : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SGitChangesView) {}
	SLATE_END_ARGS()

	using FItemPtr = TSharedPtr<FGitChangeItem>;

	void Construct(const FArguments& InArgs, const TSharedRef<FGitWorkspaceModel>& InModel);
	virtual ~SGitChangesView() override;
	virtual bool SupportsKeyboardFocus() const override { return true; }
	virtual FReply OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent) override;
	virtual void Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime) override;

	/** Narrow docks fold the Change and Lock columns into badges next to the name. */
	bool IsCompact() const { return bCompact; }

	/** Selects these repository paths when they have changes (for "Show in Git panel"). */
	void SelectPaths(const TArray<FString>& Paths);
	void FocusSearch();
	/** Name, folder and icon of a file, cached until the repository changes. */
	const FGitFileDisplay& GetDisplay(const FString& Path);
	/** "You", "Alice · 12 min ago" or empty. */
	FText GetLockText(const FString& Path) const;
	/** The file is locked by you, but the lock was taken in another clone, worktree or computer. */
	bool IsLockedElsewhere(const FString& Path) const;
	/** The search text, highlighted in the rows. */
	FText GetFilterText() const { return FText::FromString(Filter); }
	/** Stages or unstages a file, or every file in a folder. */
	void ToggleStaged(const FItemPtr& Item);

private:
	void OnModelChanged();
	void Rebuild();
	bool Matches(const GitCore::FStatusEntry& Entry);
	FString ExpansionKey(const FGitChangeItem& Item) const;
	TSharedRef<ITableRow> OnGenerateRow(FItemPtr Item, const TSharedRef<STableViewBase>& Owner);
	void OnGetChildren(FItemPtr Item, TArray<FItemPtr>& OutChildren) const;
	void OnExpansionChanged(FItemPtr Item, bool bExpanded);
	void OnSetExpansionRecursive(FItemPtr Item, bool bExpand);
	void OnSelectionChanged(FItemPtr Item, ESelectInfo::Type SelectInfo);
	void OnDoubleClick(FItemPtr Item);
	/** Shows what changed in the single selected text file, below its details. */
	void UpdateDiff();
	/** What Compare does for the selected asset, which has no text diff. */
	FText GetAssetDiffHint() const;
	TSharedPtr<SWidget> OnContextMenu();
	/** One list of the Changes tab: its group (the files and their counts) and its tree. */
	struct FPane
	{
		FItemPtr Group;
		TSharedPtr<SWidget> Widget;
		TSharedPtr<STreeView<FItemPtr>> Tree;
		TSharedPtr<SHeaderRow> HeaderRow;
	};
	/** A title bar being dragged: the lists on either side, and their heights and shares when it was pressed. */
	struct FDividerDrag
	{
		FGitChangeItem::EGroup Above = FGitChangeItem::EGroup::Unstaged;
		FGitChangeItem::EGroup Below = FGitChangeItem::EGroup::Staged;
		float AboveHeight = 0.0f;
		float BelowHeight = 0.0f;
		float Shares = 0.0f;
	};
	/** The lists on top of each other: Conflicts (only while there are any), Changes, Staged. */
	TSharedRef<SWidget> MakeLists();
	TSharedRef<SWidget> MakePane(FPane& Pane);
	/** The list's title bar, which tells the lists apart and holds its Stage or Unstage button. */
	TSharedRef<SWidget> MakePaneHeader(const FItemPtr& Group);
	TSharedRef<SWidget> MakeInspector();
	TSharedRef<SWidget> MakeCommitBar();
	const FPane& GetPane(FGitChangeItem::EGroup Group) const;
	/** The list shown above Group's, whose boundary its title bar moves; null for the top list. */
	const FPane* PaneAbove(FGitChangeItem::EGroup Group) const;
	/** A list's part of the height, relative to the others'. */
	float& ShareOf(FGitChangeItem::EGroup Group);
	void BeginDividerDrag(FGitChangeItem::EGroup Below);
	void DragDivider(float Offset);
	void EndDividerDrag();

	/** Selected rows in every list; selecting in one list clears the others. */
	TArray<FItemPtr> SelectedItems() const;
	/** Selected files, and the files in selected folders. */
	TArray<FItemPtr> SelectedFiles() const;
	/** The same for one list. */
	TArray<FItemPtr> SelectedFilesIn(FGitChangeItem::EGroup Group) const;
	TArray<FString> SelectedPaths(bool bStagedSide) const;
	TArray<FString> SelectedPaths() const;
	/** The single selected file; null when a folder or several items are selected. */
	FItemPtr SingleSelection() const;
	TArray<FItemPtr> SelectedFolders() const;
	void StageSelection();
	void UnstageSelection();
	/** A list's header button: stages or unstages its selected files, or every file it lists when none is selected. */
	void StageFromHeader(FGitChangeItem::EGroup Group);
	void DiscardSelection();
	void DiffSelection();
	void LockSelection(bool bLock, bool bForce);
	/** Takes over the selected files' locks you took in another clone, worktree or computer. */
	void ClaimSelection();
	/** The selected file shows in the Content Browser (bContentBrowser) or in Finder / Explorer. */
	bool CanShowSelection(bool bContentBrowser) const;
	void ShowSelection(bool bContentBrowser);
	void Commit(bool bPush);
	bool CanCommit() const;

	TWeakPtr<FGitWorkspaceModel> WeakModel;
	FDelegateHandle ChangedHandle;
	TArray<FPane> Panes;
	TSharedPtr<SSearchBox> SearchBox;
	TSharedPtr<SMultiLineEditableTextBox> MessageBox;
	TSharedPtr<SGitDiffView> DiffView;
	/** The file, side and repository state of the diff shown. */
	FString DiffKey;
	/** The selection is a text file, so the inspector shows its diff. */
	bool bDiffShown = false;
	/** Folders the user closed, by group and path. */
	TSet<FString> CollapsedFolders;
	FGitFileDisplays Displays;
	uint64 BuiltGeneration = MAX_uint64;
	bool bBuiltFolders = true;
	/** Set while a rebuild restores expansion, so only the user's own changes are remembered. */
	bool bRestoringExpansion = false;
	/** Set while one list clears the others' selection. */
	bool bSyncingSelection = false;
	/** Conflicts' part of the height while there are any (Changes and Staged keep theirs in the user settings). */
	float ConflictsShare = 0.25f;
	TOptional<FDividerDrag> DividerDrag;
	FString Filter;
	bool bContentOnly = false;
	bool bCompact = false;
	FString CommitMessage;
	TArray<FString> PendingSelection;
	/** Where the selected file can be shown, for the file and repository state it was worked out for. */
	mutable FString ShowPath;
	mutable uint64 ShowGeneration = MAX_uint64;
	mutable bool bShowInContentBrowser = false;
	mutable bool bShowOnDisk = false;
};
