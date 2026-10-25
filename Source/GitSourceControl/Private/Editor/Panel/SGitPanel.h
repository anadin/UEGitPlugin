// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"

class FGitWorkspaceModel;
class SGitChangesView;
class SGitHistoryView;
class SGitLocksView;
class SGitStashesView;

/**
 * The Git panel: one toolbar row (branch, sync state, Fetch / Sync / Push), an error banner
 * when the last action failed, and the Changes, History, Stashes and Locks tabs.
 */
class SGitPanel : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SGitPanel) {}
	SLATE_END_ARGS()

	/** Kept from the previous Git Workspace window so saved editor layouts still find the tab. */
	static const FName TabId;

	void Construct(const FArguments& InArgs, const TSharedRef<FGitWorkspaceModel>& InModel);
	virtual bool SupportsKeyboardFocus() const override { return true; }
	virtual FReply OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent) override;

	/** Shows the Changes tab with these repository paths selected. */
	void ShowPaths(const TArray<FString>& Paths);
	/** 0 Changes, 1 History, 2 Stashes, 3 Locks. */
	void ShowTab(int32 Index);

private:
	enum class ETab : uint8
	{
		Changes,
		History,
		Stashes,
		Locks
	};

	/** Narrow docks show icons and counts instead of labels. */
	bool IsCompact() const;
	TSharedRef<SWidget> MakeToolbar();
	TSharedRef<SWidget> MakeBanner();
	TSharedRef<SWidget> MakeTabs();
	TSharedRef<SWidget> MakeBranchMenu();
	TSharedRef<SWidget> MakeMoreMenu();
	FText GetSyncState() const;
	FText GetSyncStateTooltip() const;
	void SetTab(ETab InTab);
	void CreateBranch();
	void Refresh();

	TWeakPtr<FGitWorkspaceModel> WeakModel;
	ETab Tab = ETab::Changes;
	TSharedPtr<SGitChangesView> ChangesView;
	TSharedPtr<SGitHistoryView> HistoryView;
	TSharedPtr<SGitStashesView> StashesView;
	TSharedPtr<SGitLocksView> LocksView;
};
