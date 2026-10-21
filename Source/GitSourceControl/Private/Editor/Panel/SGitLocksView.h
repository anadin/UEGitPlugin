// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#pragma once

#include "CoreMinimal.h"
#include "Core/GitTypes.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/SListView.h"

class FGitWorkspaceModel;

/** Locks tab: who holds which lockable files, as last checked with the server. */
class SGitLocksView : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SGitLocksView) {}
	SLATE_END_ARGS()

	using FLockPtr = TSharedPtr<GitCore::FLockInfo>;

	void Construct(const FArguments& InArgs, const TSharedRef<FGitWorkspaceModel>& InModel);
	virtual ~SGitLocksView() override;

private:
	void OnModelChanged();
	void Rebuild();
	TSharedRef<ITableRow> OnGenerateRow(FLockPtr Lock, const TSharedRef<STableViewBase>& Owner);
	TSharedPtr<SWidget> OnContextMenu();
	void Release(bool bForce);
	bool IsMine(const GitCore::FLockInfo& Lock) const { return Lock.bOurs; }

	TWeakPtr<FGitWorkspaceModel> WeakModel;
	FDelegateHandle ChangedHandle;
	TArray<FLockPtr> Locks;
	TSharedPtr<SListView<FLockPtr>> List;
	uint64 BuiltGeneration = MAX_uint64;
	bool bMineOnly = false;
};
