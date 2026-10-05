// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#pragma once
#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/SListView.h"
#include "Async/Future.h"
#include "GitWorkspaceRepository.h"
#include "GitWorkspaceRestart.h"

class SMultiLineEditableTextBox;
class SWindow;
class SVerticalBox;
class SEditableTextBox;
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
    GitWorkspace::FIncomingLfsResult IncomingLfs;
    GitWorkspace::FRestartJob Restart;
    GitWorkspace::FStashList Stashes;
    GitWorkspace::FStashReview StashReview;
    GitWorkspace::FStashInspection StashInspection;
    bool bStashes = false, bStashReview = false;
    bool bRestart = false;
    bool bReload = false;
    bool bIncomingLfs = false;
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
    friend class FGitNewAssetLockTest;
    friend class FGitStartupLockVerificationTest;
    friend class FGitIncomingLfsPanelTest;
    friend class FGitStashPanelTest;
    bool IsIdle() const { return !Pending.IsValid() && RestartFolder.IsEmpty() && !bReloading; }
    void Start(TFunction<FGitWorkspaceTaskResult()> Work);
    FReply Refresh();
    FReply ChangeIndex(bool bStage);
    TArray<FString> SelectedIndexPaths(bool bStage, int32* SkippedSubmodules = nullptr) const;
    FReply Commit();
    FReply ShowDiff();
    FReply VerifyLocks();
    FReply RemoteAction(int32 Action);
    FText PushHint() const;
    FText LockHint() const;
    FText LockStatusText() const;
    bool CanLockSelected() const;
    bool IsPathVisible(const FString& Path) const;
    int32 HiddenStagedCount() const;
    FReply ShowIncomingReview();
    FReply DownloadIncomingLfs();
    FReply RestartPull();
    FReply ReloadPull();
    FString FinishReloadPull();
    FReply CancelRestart();
    FReply ShowStashes();
    FReply RefreshStashes();
    FReply PreviewStash(const FString& Oid, const FString& Selector = FString(), bool bSelected = false);
    FReply DropSelectedStash();
    FText StashReportText() const;
    FReply RunStash();
    void RebuildStashes();
    TSharedRef<SWidget> MakeStashFilePicker();
    void RebuildStashFiles();
    void SetStashPathChecked(const FString& Path, bool bChecked);
    void SetStashIncludeUntracked(bool bInclude);
    void InvalidateStashSelection();
    bool CanSelectStashFile(const GitWorkspace::FFile& File) const;
    FText StashSelectionSummary() const;
    FText IncomingLfsStatus() const;
    FText IncomingReportText() const;
    FReply ChangeLock(bool bUnlock);
    bool HasDirtyPackages() const;
    FText Inspector() const;
    void RebuildRows();
    TSharedRef<ITableRow> MakeRow(TSharedPtr<FGitWorkspaceRow> Row, const TSharedRef<STableViewBase>& Owner);
    TSharedPtr<GitWorkspace::FRepository, ESPMode::ThreadSafe> Repository;
    GitWorkspace::FSnapshot Snapshot;
    GitWorkspace::FLockSnapshot Locks;
    GitWorkspace::FRemoteSnapshot Remote;
    GitWorkspace::FIncomingLfsResult IncomingLfs;
    FString LockRemote = TEXT("origin");
    TFuture<FGitWorkspaceTaskResult> Pending;
    TArray<TSharedPtr<FGitWorkspaceRow>> Rows;
    TSharedPtr<SListView<TSharedPtr<FGitWorkspaceRow>>> List;
    TSharedPtr<FGitWorkspaceRow> Selection;
    TSharedPtr<SMultiLineEditableTextBox> Message;
    FString Feedback;
    FString RestartFolder, RestartMessage;
    double NextRestartPoll = 0;
    uint32 RestartHelperPid = 0;
    bool bPreparingRestart = false;
    bool bReloading = false;
    FString DiffText;
    FString FileFilter;
    bool bContentOnly = true;
    bool bVerifyLocksOnOpen = true;
    bool bCheckingLocks = false;
    bool bDownloadingLfs = false;
    TWeakPtr<SWindow> IncomingWindow;
    TSharedPtr<SMultiLineEditableTextBox> IncomingReport;
    GitWorkspace::FStashList Stashes;
    GitWorkspace::FStashReview StashReview;
    GitWorkspace::FStashInspection StashInspection;
    TWeakPtr<SWindow> StashWindow;
    TSharedPtr<SVerticalBox> StashRows;
    TSharedPtr<SEditableTextBox> StashName;
    TSharedPtr<SMultiLineEditableTextBox> StashReport;
    TArray<FString> StashSelectedPaths;
    TArray<TSharedPtr<GitWorkspace::FFile>> StashFileItems;
    TSharedPtr<SListView<TSharedPtr<GitWorkspace::FFile>>> StashFiles;
    FString StashFileFilter;
    bool bStashContentOnly = true;
    bool bRestoreStashIndex = true, bIncludeUntrackedStash = false;
};

namespace GitWorkspaceUI
{
    void Register();
    void Unregister();
    void Open();
}
