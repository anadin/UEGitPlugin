// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#pragma once
#include "GitWorkspaceRepository.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/SListView.h"
#include "Async/Future.h"
class SMultiLineEditableTextBox;

class SGitWorkspaceHistory : public SCompoundWidget
{
public:
    using FRepositoryPtr = TSharedPtr<GitWorkspace::FRepository, ESPMode::ThreadSafe>;
    SLATE_BEGIN_ARGS(SGitWorkspaceHistory) {}
        SLATE_ARGUMENT(FRepositoryPtr, Repository)
    SLATE_END_ARGS()
    void Construct(const FArguments& Args);
    virtual ~SGitWorkspaceHistory() override;
    virtual void Tick(const FGeometry&, double, float) override;
private:
    friend class FGitHistoryPanelTest;
    struct FReadResult
    {
        int32 Kind = 0;
        GitWorkspace::FHistoryList History;
        GitWorkspace::FCommitInspection Commit;
        GitWorkspace::FResult File;
    };
    bool IsIdle() const { return !Pending.IsValid(); }
    FReply RefreshHistory(bool bMore = false);
    void ReadCommit(TSharedPtr<GitWorkspace::FHistoryCommit> Commit);
    void ReadFile(TSharedPtr<GitWorkspace::FHistoryChange> File);
    FRepositoryPtr Repository;
    TFuture<FReadResult> Pending;
    GitWorkspace::FHistoryList History;
    GitWorkspace::FCommitInspection Inspection;
    int32 Limit = 100;
    FString Feedback = TEXT("Reading commit history…");
    FString Summary;
    TArray<TSharedPtr<GitWorkspace::FHistoryCommit>> Commits;
    TArray<TSharedPtr<GitWorkspace::FHistoryChange>> Files;
    TSharedPtr<SListView<TSharedPtr<GitWorkspace::FHistoryCommit>>> CommitList;
    TSharedPtr<SListView<TSharedPtr<GitWorkspace::FHistoryChange>>> FileList;
    TSharedPtr<SMultiLineEditableTextBox> CommitReport, FileReport;
};
