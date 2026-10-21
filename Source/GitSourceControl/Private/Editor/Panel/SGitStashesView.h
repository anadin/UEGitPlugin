// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#pragma once

#include "CoreMinimal.h"
#include "Core/GitTypes.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/SListView.h"

class FGitWorkspaceModel;

/** Stashes tab: put changes aside, bring them back, or delete them. */
class SGitStashesView : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SGitStashesView) {}
	SLATE_END_ARGS()

	using FStashPtr = TSharedPtr<GitCore::FStashEntry>;

	void Construct(const FArguments& InArgs, const TSharedRef<FGitWorkspaceModel>& InModel);
	virtual ~SGitStashesView() override;

private:
	void OnModelChanged();
	void OnSelected(FStashPtr Stash, ESelectInfo::Type SelectInfo);
	TSharedRef<ITableRow> OnGenerateRow(FStashPtr Stash, const TSharedRef<STableViewBase>& Owner);
	TSharedPtr<SWidget> OnContextMenu();
	FStashPtr Selection() const;
	void StashChanges();
	void Apply(bool bDrop);
	void Drop();

	TWeakPtr<FGitWorkspaceModel> WeakModel;
	FDelegateHandle ChangedHandle;
	TArray<FStashPtr> Stashes;
	TSharedPtr<SListView<FStashPtr>> List;
	TArray<TSharedPtr<FString>> SelectedPaths;
	TSharedPtr<SListView<TSharedPtr<FString>>> PathList;
	uint64 BuiltGeneration = MAX_uint64;
	bool bIncludeNewFiles = false;
};
