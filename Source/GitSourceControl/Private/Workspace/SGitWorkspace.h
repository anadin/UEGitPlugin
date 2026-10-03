// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#pragma once
#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/SListView.h"
#include "Async/Future.h"
#include "GitWorkspaceRepository.h"

class SMultiLineEditableTextBox;
class SWindow;
struct FGitWorkspaceRow
{
    GitWorkspace::FFile File;
    bool bStaged = false;
    FString Group;
};
struct FGitWorkspaceTaskResult
{
    GitWorkspace::FSnapshot Snapshot;
    FString Message;
    GitWorkspace::FLockSnapshot Locks;
    GitWorkspace::FRemoteSnapshot Remote;
    bool bRemote = false;
    bool bLocks = false;
    bool bDiff = false;
    bool bCommitSucceeded = false;
};

class SGitWorkspace : public SCompoundWidget
{
public:
    using FRepositoryPtr = TSharedPtr<GitWorkspace::FRepository, ESPMode::ThreadSafe>;
    SLATE_BEGIN_ARGS(SGitWorkspace) {}
        SLATE_ARGUMENT(FRepositoryPtr, Repository)
    SLATE_END_ARGS()
    void Construct(const FArguments& Args);
    virtual ~SGitWorkspace() override;
    virtual void Tick(const FGeometry&, double, float) override;
    void WaitForWork();
private:
    friend class FGitWorkspaceEditorAssetsTest;
    friend class FGitIncomingPackageReviewTest;
    friend class FGitWorkspaceSelectionTest;
    bool IsIdle() const { return !Pending.IsValid(); }
    void Start(TFunction<FGitWorkspaceTaskResult()> Work);
    FReply Refresh();
    FReply ChangeIndex(bool bStage);
    TArray<FString> SelectedIndexPaths(bool bStage, int32* SkippedSubmodules = nullptr) const;
    FReply Commit();
    FReply ShowDiff();
    FReply VerifyLocks();
    FReply RemoteAction(int32 Action);
    FText PushHint() const;
    FReply ShowIncomingReview();
    FReply ChangeLock(bool bUnlock);
    bool HasDirtyPackages() const;
    FText Inspector() const;
    void RebuildRows();
    TSharedRef<ITableRow> MakeRow(TSharedPtr<FGitWorkspaceRow> Row, const TSharedRef<STableViewBase>& Owner);
    TSharedPtr<GitWorkspace::FRepository, ESPMode::ThreadSafe> Repository;
    GitWorkspace::FSnapshot Snapshot;
    GitWorkspace::FLockSnapshot Locks;
    GitWorkspace::FRemoteSnapshot Remote;
    FString LockRemote = TEXT("origin");
    TFuture<FGitWorkspaceTaskResult> Pending;
    TArray<TSharedPtr<FGitWorkspaceRow>> Rows;
    TSharedPtr<SListView<TSharedPtr<FGitWorkspaceRow>>> List;
    TSharedPtr<FGitWorkspaceRow> Selection;
    TSharedPtr<SMultiLineEditableTextBox> Message;
    FString Feedback;
    FString DiffText;
    FString FileFilter;
    TWeakPtr<SWindow> IncomingWindow;
};

namespace GitWorkspaceUI
{
    void Register();
    void Unregister();
    void Open();
}
