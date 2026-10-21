// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#pragma once

#include "CoreMinimal.h"
#include "Editor/GitWorkspaceText.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/STreeView.h"

class FGitWorkspaceModel;
class SHeaderRow;
class SMultiLineEditableTextBox;

/** A group header (Conflicts, Staged, Changes) or one side of a changed file. */
struct FGitChangeItem
{
	enum class EGroup : uint8
	{
		Conflicts,
		Staged,
		Unstaged
	};

	bool bGroup = false;
	EGroup Group = EGroup::Unstaged;
	/** Files in the group before filtering. */
	int32 Total = 0;
	TArray<TSharedPtr<FGitChangeItem>> Children;
	GitCore::FStatusEntry Entry;

	bool IsStagedSide() const { return Group == EGroup::Staged; }
};

/** Changes tab: changed files grouped by staged state, an inspector and the commit box. */
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
	/** Name, folder and icon of a file, cached until the repository changes. */
	const FGitFileDisplay& GetDisplay(const FString& Path);
	/** "You", "Alice · 12 min ago" or empty. */
	FText GetLockText(const FString& Path) const;
	void ToggleStaged(const FItemPtr& Item);

private:
	void OnModelChanged();
	void Rebuild();
	bool Matches(const GitCore::FStatusEntry& Entry);
	TSharedRef<ITableRow> OnGenerateRow(FItemPtr Item, const TSharedRef<STableViewBase>& Owner);
	void OnGetChildren(FItemPtr Item, TArray<FItemPtr>& OutChildren) const;
	void OnExpansionChanged(FItemPtr Item, bool bExpanded);
	void OnDoubleClick(FItemPtr Item);
	TSharedPtr<SWidget> OnContextMenu();
	TSharedRef<SWidget> MakeInspector();
	TSharedRef<SWidget> MakeCommitBar();

	TArray<FItemPtr> SelectedFiles() const;
	TArray<FString> SelectedPaths(bool bStagedSide) const;
	TArray<FString> SelectedPaths() const;
	FItemPtr SingleSelection() const;
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
	TSharedPtr<SMultiLineEditableTextBox> MessageBox;
	TArray<FItemPtr> Groups;
	TSet<FItemPtr> AllItems;
	TSet<FGitChangeItem::EGroup> CollapsedGroups;
	TMap<FString, FGitFileDisplay> Displays;
	uint64 BuiltGeneration = MAX_uint64;
	FString Filter;
	bool bContentOnly = false;
	bool bCompact = false;
	FString CommitMessage;
	TArray<FString> PendingSelection;
};
