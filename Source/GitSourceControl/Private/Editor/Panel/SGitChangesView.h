// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#pragma once

#include "CoreMinimal.h"
#include "Editor/GitWorkspaceText.h"
#include "GitFileTree.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/STreeView.h"

class FGitWorkspaceModel;
class SHeaderRow;
class SMultiLineEditableTextBox;
class SSearchBox;

/** A group (Conflicts, Staged, Changes), a folder in it, or one side of a changed file. */
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

/** Changes tab: changed files grouped by staged state and folder, an inspector and the commit box. */
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
	void OnDoubleClick(FItemPtr Item);
	TSharedPtr<SWidget> OnContextMenu();
	TSharedRef<SWidget> MakeInspector();
	TSharedRef<SWidget> MakeCommitBar();

	/** Selected files, and the files in selected folders. */
	TArray<FItemPtr> SelectedFiles() const;
	TArray<FString> SelectedPaths(bool bStagedSide) const;
	TArray<FString> SelectedPaths() const;
	/** The single selected file; null when a folder or several items are selected. */
	FItemPtr SingleSelection() const;
	TArray<FItemPtr> SelectedFolders() const;
	void StageSelection();
	void UnstageSelection();
	void DiscardSelection();
	void DiffSelection();
	void LockSelection(bool bLock, bool bForce);
	void Commit(bool bPush);
	bool CanCommit() const;

	TWeakPtr<FGitWorkspaceModel> WeakModel;
	FDelegateHandle ChangedHandle;
	TSharedPtr<STreeView<FItemPtr>> Tree;
	TSharedPtr<SHeaderRow> HeaderRow;
	TSharedPtr<SSearchBox> SearchBox;
	TSharedPtr<SMultiLineEditableTextBox> MessageBox;
	TArray<FItemPtr> Groups;
	TSet<FGitChangeItem::EGroup> CollapsedGroups;
	/** Folders the user closed, by group and path. */
	TSet<FString> CollapsedFolders;
	FGitFileDisplays Displays;
	uint64 BuiltGeneration = MAX_uint64;
	bool bBuiltFolders = true;
	/** Set while a rebuild restores expansion, so only the user's own changes are remembered. */
	bool bRestoringExpansion = false;
	FString Filter;
	bool bContentOnly = false;
	bool bCompact = false;
	FString CommitMessage;
	TArray<FString> PendingSelection;
};
