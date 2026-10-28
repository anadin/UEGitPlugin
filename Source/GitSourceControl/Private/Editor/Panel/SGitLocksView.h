// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#pragma once

#include "CoreMinimal.h"
#include "Core/GitTypes.h"
#include "GitFileTree.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/STreeView.h"

class FGitWorkspaceModel;
class SSearchBox;

/** A locked file, or a folder of them. */
struct FGitLockItem
{
	bool bFolder = false;
	FString Path;
	FString Label;
	int32 FileCount = 0;
	TArray<TSharedPtr<FGitLockItem>> Children;
	TSharedPtr<GitCore::FLockInfo> Lock;
};

/** Locks tab: who holds which lockable files, as last checked with the server. */
class SGitLocksView : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SGitLocksView) {}
	SLATE_END_ARGS()

	using FItemPtr = TSharedPtr<FGitLockItem>;

	void Construct(const FArguments& InArgs, const TSharedRef<FGitWorkspaceModel>& InModel);
	virtual ~SGitLocksView() override;
	virtual void Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime) override;
	void FocusSearch();
	FText GetFilterText() const { return FText::FromString(Filter); }
	/** Name and icon of a file, cached until the locks change. */
	const FGitFileDisplay& GetDisplay(const FString& Path);
	/** "You", "You, elsewhere" or the owner's name. */
	FText GetOwnerText(const GitCore::FLockInfo& Lock) const;
	/** Yours, but taken in another clone, worktree or computer. */
	bool IsElsewhere(const GitCore::FLockInfo& Lock) const;

private:
	void OnModelChanged();
	void Rebuild();
	TSharedRef<ITableRow> OnGenerateRow(FItemPtr Item, const TSharedRef<STableViewBase>& Owner);
	void OnGetChildren(FItemPtr Item, TArray<FItemPtr>& OutChildren) const;
	void OnExpansionChanged(FItemPtr Item, bool bExpanded);
	void OnSetExpansionRecursive(FItemPtr Item, bool bExpand);
	void OnDoubleClick(FItemPtr Item);
	TSharedPtr<SWidget> OnContextMenu();
	/** Selected locks, and the locks in selected folders. */
	TArray<TSharedPtr<GitCore::FLockInfo>> SelectedLocks() const;
	void Release(bool bForce);
	/** Takes over the selected locks you took elsewhere (another clone, worktree or computer). */
	void Claim();
	bool CanClaim() const;

	TWeakPtr<FGitWorkspaceModel> WeakModel;
	FDelegateHandle ChangedHandle;
	TArray<FItemPtr> Items;
	TSharedPtr<STreeView<FItemPtr>> Tree;
	TSharedPtr<SSearchBox> SearchBox;
	FGitFileDisplays Displays;
	TSet<FString> CollapsedFolders;
	FString Filter;
	uint64 BuiltGeneration = MAX_uint64;
	/** Your locks taken elsewhere, in the whole list (not only the ones shown). */
	int32 ElsewhereCount = 0;
	bool bBuiltFolders = true;
	bool bRestoringExpansion = false;
	bool bMineOnly = false;
};
