// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#include "SGitWorkspace.h"
#include "GitWorkspacePullReview.h"
#include "GitWorkspaceEditorPull.h"
#include "Misc/ScopedSlowTask.h"
#if PLATFORM_MAC
#include "GitWorkspaceSession.h"
#endif
#include "Interfaces/IMainFrameModule.h"
#include "Modules/ModuleManager.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/SWindow.h"
#include "HAL/PlatformApplicationMisc.h"
#include "GitSourceControlModule.h"
#include "Async/Async.h"
#include "FileHelpers.h"
#include "Misc/Paths.h"
#include "Misc/MessageDialog.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SSearchBox.h"
#include "Widgets/Input/SSegmentedControl.h"
#include "Framework/Docking/TabManager.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/SHeaderRow.h"
#include "Styling/AppStyle.h"
#include "ToolMenus.h"
#include "HAL/IConsoleManager.h"

namespace
{
FText Text(const FString& S) { return FText::FromString(S); }
FString State(TCHAR C)
{
    switch (C)
    {
    case 'M': return TEXT("Modified"); case 'A': return TEXT("Added");
    case 'D': return TEXT("Deleted"); case 'R': return TEXT("Renamed");
    case 'C': return TEXT("Copied"); case '?': return TEXT("Untracked");
    case 'U': return TEXT("Conflict"); default: return TEXT("—");
    }
}
class SFileRow : public SMultiColumnTableRow<TSharedPtr<FGitWorkspaceRow>>
{
public:
    SLATE_BEGIN_ARGS(SFileRow) {} SLATE_ARGUMENT(TSharedPtr<FGitWorkspaceRow>, Item) SLATE_ARGUMENT(const GitWorkspace::FLockSnapshot*, Locks) SLATE_END_ARGS()
    void Construct(const FArguments& Args, const TSharedRef<STableViewBase>& Owner)
    {
        Item = Args._Item; Locks = Args._Locks;
        SMultiColumnTableRow::Construct(FSuperRowType::FArguments().Padding(5), Owner);
    }
    TSharedRef<SWidget> GenerateWidgetForColumn(const FName& Column) override
    {
        FString Value;
        if (!Item->Group.IsEmpty()) Value = Column == "Asset" ? Item->Group : FString();
        else if (Column == "Asset") return SNew(SVerticalBox)
            + SVerticalBox::Slot().AutoHeight()
            [SNew(STextBlock).Clipping(EWidgetClipping::ClipToBounds).Text(Text(FPaths::GetCleanFilename(Item->File.Path))).ToolTipText(Text(Item->File.Path))]
            + SVerticalBox::Slot().AutoHeight()
            [SNew(STextBlock).Clipping(EWidgetClipping::ClipToBounds).Font(FAppStyle::GetFontStyle("SmallFont")).Text(Text(Item->File.Path)).ToolTipText(Text(Item->File.Path))];
        else if (Column == "Working") Value = Item->File.bSubmodule ? TEXT("Submodule") : State(Item->File.Working);
        else if (Column == "Staged") Value = State(Item->File.Index);
        else if (Column == "Lock") return SNew(STextBlock).Clipping(EWidgetClipping::ClipToBounds).Text_Lambda([this] { return Text(Locks->Label(Item->File.Path, Item->File.bLockable)); }).ToolTipText_Lambda([this] { return Text(Item->File.Path + TEXT("\n") + Locks->Label(Item->File.Path, Item->File.bLockable)); });
        return SNew(STextBlock).Clipping(EWidgetClipping::ClipToBounds).Text(Text(Value)).ToolTipText(Text(Item->File.Path));
    }
private:
    TSharedPtr<FGitWorkspaceRow> Item;
    const GitWorkspace::FLockSnapshot* Locks = nullptr;
};
const FName TabName(TEXT("GitNativeWorkspace"));
TWeakPtr<SGitWorkspace> OpenWorkspace;
TWeakPtr<SDockTab> OpenTab;
FDelegateHandle MenuStartupHandle;
IConsoleObject* OpenCommand = nullptr;
}

void SGitWorkspace::Construct(const FArguments& Args)
{
    Repository = Args._Repository;
    if (!Repository)
        Repository = MakeShared<GitWorkspace::FRepository, ESPMode::ThreadSafe>(GitWorkspace::FindGitExecutable(FGitSourceControlModule::Get().AccessSettings().GetBinaryPath()), FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()));
    RestartMessage = GitWorkspace::LastRestartResult(Repository->Directory());
    ChildSlot
    [
        SNew(SVerticalBox)
        + SVerticalBox::Slot().AutoHeight().Padding(10)
        [
            SNew(SHorizontalBox)
            + SHorizontalBox::Slot().FillWidth(1)
            [SNew(STextBlock).Text_Lambda([this] { return Text(Snapshot.bValid ? Snapshot.Root + TEXT("  |  ") + Snapshot.Branch : TEXT("Git Workspace — local workflow alpha")); })]
            + SHorizontalBox::Slot().AutoWidth().Padding(4, 0)
            [SNew(SButton).Text(Text(TEXT("Refresh"))).IsEnabled_Lambda([this] { return IsIdle(); }).OnClicked(this, &SGitWorkspace::Refresh)]
            + SHorizontalBox::Slot().AutoWidth().Padding(4, 0)
            [SNew(SButton).Text(Text(TEXT("Save assets…"))).IsEnabled_Lambda([this] { return IsIdle(); }).OnClicked_Lambda([this]
            {
                FEditorFileUtils::SaveDirtyPackages(true, true, true);
                return Refresh();
            })]
        ]
        + SVerticalBox::Slot().AutoHeight().Padding(10, 0, 10, 8)
        [SNew(STextBlock).Text(Text(TEXT("Changes   ·   Commits and pushes retain locks. Fetch before Push/Pull. Asset visual diffs are not yet available."))).AutoWrapText(true)]
        + SVerticalBox::Slot().AutoHeight().Padding(10, 0, 10, 8)
        [
            SNew(SHorizontalBox)
            + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 8, 0)
            [SNew(STextBlock).Text(Text(TEXT("Lock remote")))]
            + SHorizontalBox::Slot().MaxWidth(180).FillWidth(1).Padding(0, 0, 8, 0)
            [SNew(SEditableTextBox).Text(Text(LockRemote)).IsEnabled_Lambda([this] { return IsIdle(); })
                .OnTextChanged_Lambda([this](const FText& Value) { LockRemote = Value.ToString(); Locks = GitWorkspace::FLockSnapshot(); })]
            + SHorizontalBox::Slot().AutoWidth()
            [SNew(SButton).Text(Text(TEXT("Verify locks"))).IsEnabled_Lambda([this] { return IsIdle() && Snapshot.bValid; }).OnClicked(this, &SGitWorkspace::VerifyLocks)]
            + SHorizontalBox::Slot().AutoWidth().Padding(8, 0)
            [SNew(SButton).Text(Text(TEXT("Lock asset"))).ToolTipText(this, &SGitWorkspace::LockHint).IsEnabled(this, &SGitWorkspace::CanLockSelected).OnClicked_Lambda([this] { return ChangeLock(false); })]
            + SHorizontalBox::Slot().AutoWidth()
            [SNew(SButton).Text(Text(TEXT("Unlock…"))).IsEnabled_Lambda([this] { return IsIdle() && Selection && Selection->Group.IsEmpty() && Locks.State(Selection->File.Path, Selection->File.bLockable) == GitWorkspace::ELockState::Ours; }).OnClicked_Lambda([this] { return ChangeLock(true); })]
        ]
        + SVerticalBox::Slot().AutoHeight().Padding(10, 0, 10, 8)
        [SNew(STextBlock).AutoWrapText(true).Text(this, &SGitWorkspace::LockStatusText)]
        + SVerticalBox::Slot().AutoHeight().Padding(10, 0, 10, 8)
        [
            SNew(SHorizontalBox)
            + SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 8, 0)
            [SNew(SButton).Text(Text(TEXT("Fetch upstream"))).IsEnabled_Lambda([this] { return IsIdle() && Snapshot.bValid; }).OnClicked_Lambda([this] { return RemoteAction(0); })]
            + SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 8, 0)
            [SNew(SButton).Text(Text(TEXT("Review incoming…"))).IsEnabled_Lambda([this] { return IsIdle() && Remote.bValid && Remote.Behind > 0; }).OnClicked(this, &SGitWorkspace::ShowIncomingReview)]
            + SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 8, 0)
            [SNew(SButton).Text(Text(TEXT("Push…"))).ToolTipText(this, &SGitWorkspace::PushHint).IsEnabled_Lambda([this] { return IsIdle() && Remote.IsFresh() && Remote.Ahead > 0 && Remote.Behind == 0; }).OnClicked_Lambda([this] { return RemoteAction(1); })]
            + SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 12, 0)
            [SNew(SButton).Text(Text(TEXT("Pull fast-forward…"))).IsEnabled_Lambda([this] { return IsIdle() && Remote.IsFresh() && Remote.Behind > 0 && Remote.Ahead == 0; }).OnClicked_Lambda([this] { return RemoteAction(2); })]
        ]
        + SVerticalBox::Slot().AutoHeight().Padding(10, 0, 10, 8)
        [SNew(STextBlock).AutoWrapText(true).Text_Lambda([this]
            {
                if (!Remote.Error.IsEmpty()) return Text(Remote.Error + TEXT("\n") + PushHint().ToString());
                if (!Remote.IsFresh()) return PushHint();
                return Text(Remote.Remote + TEXT(" / ") + Remote.RemoteRef + FString::Printf(TEXT("  |  %d outgoing · %d incoming  |  "), Remote.Ahead, Remote.Behind) + Remote.FetchedAt.ToIso8601() + TEXT("\n") + PushHint().ToString());
            })]
        + SVerticalBox::Slot().AutoHeight().Padding(10, 0, 10, 10)
        [SNew(STextBlock).Text_Lambda([this] { return Text(!RestartFolder.IsEmpty() ? RestartMessage : IsIdle() ? Feedback : bPreparingRestart ? TEXT("Preparing safe restart Pull… The editor will stay open until preparation succeeds.") : bCheckingLocks ? TEXT("Checking locks…") : bDownloadingLfs ? TEXT("Downloading and verifying incoming LFS assets…") : TEXT("Git operation running…")); }).AutoWrapText(true)]
        + SVerticalBox::Slot().AutoHeight().Padding(10, 0)
        [SNew(STextBlock).Text_Lambda([this] { return Text(RestartFolder.IsEmpty() ? RestartMessage : FString()); }).AutoWrapText(true)]
        + SVerticalBox::Slot().AutoHeight().Padding(10, 0)
        [SNew(SButton).Text(Text(TEXT("Cancel restart Pull"))).Visibility_Lambda([this] { return RestartFolder.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible; }).OnClicked(this, &SGitWorkspace::CancelRestart)]
        + SVerticalBox::Slot().FillHeight(1)
        [
            SNew(SSplitter)
            + SSplitter::Slot().Value(0.65f)
            [
                SNew(SVerticalBox)
                + SVerticalBox::Slot().AutoHeight().Padding(10, 4)
                [
                    SNew(SHorizontalBox)
                    + SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 8, 0)
                    [SNew(SButton).Text_Lambda([this] { return Text(FString::Printf(TEXT("Stage selected (%d)"), SelectedIndexPaths(true).Num())); }).ToolTipText(Text(TEXT("Stage eligible saved files in the selection. Submodules are skipped and reported."))).IsEnabled_Lambda([this] { return IsIdle() && Snapshot.bValid && SelectedIndexPaths(true).Num() > 0; }).OnClicked_Lambda([this] { return ChangeIndex(true); })]
                    + SHorizontalBox::Slot().AutoWidth()
                    [SNew(SButton).Text_Lambda([this] { return Text(FString::Printf(TEXT("Unstage selected (%d)"), SelectedIndexPaths(false).Num())); }).IsEnabled_Lambda([this] { return IsIdle() && Snapshot.bValid && SelectedIndexPaths(false).Num() > 0; }).OnClicked_Lambda([this] { return ChangeIndex(false); })]
                ]
                + SVerticalBox::Slot().AutoHeight().Padding(10, 4)
                [SNew(SSegmentedControl<bool>).Value_Lambda([this] { return bContentOnly; }).IsEnabled_Lambda([this] { return IsIdle(); })
                    .OnValueChanged_Lambda([this](bool Value) { bContentOnly = Value; RebuildRows(); })
                    + SSegmentedControl<bool>::Slot(true).Text(Text(TEXT("Content"))).ToolTip(Text(TEXT("Show only files under Content/. This filters the view, not the Git index.")))
                    + SSegmentedControl<bool>::Slot(false).Text(Text(TEXT("Whole repo"))).ToolTip(Text(TEXT("Show changes and locks throughout the repository, including source, configuration and plugins.")))]
                + SVerticalBox::Slot().AutoHeight().Padding(10, 4)
                [SNew(SSearchBox).HintText(Text(TEXT("Filter files by name or path"))).IsEnabled_Lambda([this] { return IsIdle(); })
                    .OnTextChanged_Lambda([this](const FText& Value) { FileFilter = Value.ToString().TrimStartAndEnd(); RebuildRows(); })]
                + SVerticalBox::Slot().FillHeight(1).Padding(8)
                [
                    SAssignNew(List, SListView<TSharedPtr<FGitWorkspaceRow>>)
                    .ListItemsSource(&Rows).SelectionMode(ESelectionMode::Multi)
                    .IsEnabled_Lambda([this] { return IsIdle(); })
                    .OnGenerateRow(this, &SGitWorkspace::MakeRow)
                    .OnSelectionChanged_Lambda([this](TSharedPtr<FGitWorkspaceRow> Row, ESelectInfo::Type)
                    { Selection = Row; DiffText.Empty(); })
                    .HeaderRow(SNew(SHeaderRow)
                        + SHeaderRow::Column("Asset").DefaultLabel(Text(TEXT("Asset / file"))).FillWidth(0.46f)
                        + SHeaderRow::Column("Working").DefaultLabel(Text(TEXT("Working"))).FillWidth(0.18f)
                        + SHeaderRow::Column("Staged").DefaultLabel(Text(TEXT("Staged"))).FillWidth(0.18f)
                        + SHeaderRow::Column("Lock").DefaultLabel(Text(TEXT("Lock"))).FillWidth(0.18f))
                ]
            ]
            + SSplitter::Slot().Value(0.35f)
            [
                SNew(SVerticalBox)
                + SVerticalBox::Slot().AutoHeight().Padding(12)
                [SNew(STextBlock).Text(this, &SGitWorkspace::Inspector).AutoWrapText(true)]
                + SVerticalBox::Slot().AutoHeight().Padding(12, 0)
                [SNew(SButton).Text_Lambda([this] { return Text(Selection && Selection->bStaged ? TEXT("Compare HEAD to staged") : TEXT("Compare index to working")); })
                    .IsEnabled_Lambda([this] { return IsIdle() && Selection && Selection->Group.IsEmpty() && !Selection->File.bUntracked; })
                    .OnClicked(this, &SGitWorkspace::ShowDiff)]
                + SVerticalBox::Slot().FillHeight(1).Padding(12)
                [SNew(SMultiLineEditableTextBox).IsReadOnly(true).Text_Lambda([this] { return Text(DiffText); }).Font(FAppStyle::GetFontStyle("MonoFont"))]
            ]
        ]
        + SVerticalBox::Slot().AutoHeight().Padding(10, 0)
        [SNew(STextBlock).AutoWrapText(true).Visibility_Lambda([this] { return HiddenStagedCount() ? EVisibility::Visible : EVisibility::Collapsed; })
            .Text_Lambda([this] { return Text(FString::Printf(TEXT("%d staged file(s) hidden by this view. Choose Whole repo and clear the search to review all staged files before committing."), HiddenStagedCount())); })]
        + SVerticalBox::Slot().AutoHeight().Padding(10)
        [
            SNew(SHorizontalBox)
            + SHorizontalBox::Slot().FillWidth(1).Padding(0, 0, 12, 0)
            [SAssignNew(Message, SMultiLineEditableTextBox).HintText(Text(TEXT("Commit message"))).IsEnabled_Lambda([this] { return IsIdle(); })]
            + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
            [SNew(SButton).Text_Lambda([this] { return Text(FString::Printf(TEXT("Commit staged · %d"), Snapshot.StagedCount())); })
                .IsEnabled_Lambda([this] { return IsIdle() && Snapshot.bValid && Snapshot.StagedCount() && !HiddenStagedCount() && !Snapshot.HasConflicts() && !Snapshot.bOperationInProgress; })
                .OnClicked(this, &SGitWorkspace::Commit)]
        ]
    ];
    Refresh();
}
SGitWorkspace::~SGitWorkspace() { WaitForWork(); if (auto Window = IncomingWindow.Pin()) Window->RequestDestroyWindow(); }
void SGitWorkspace::WaitForWork() { if (Pending.IsValid()) { Pending.Wait(); Pending = TFuture<FGitWorkspaceTaskResult>(); } }
void SGitWorkspace::Start(TFunction<FGitWorkspaceTaskResult()> Work)
{
    if (IsIdle()) Pending = Async(EAsyncExecution::ThreadPool, MoveTemp(Work));
}
void SGitWorkspace::Tick(const FGeometry&, double, float)
{
    if (!RestartFolder.IsEmpty() && FPlatformTime::Seconds() >= NextRestartPoll)
    {
        NextRestartPoll = FPlatformTime::Seconds() + 0.5;
        FString State; RestartMessage = GitWorkspace::RestartJobStatus(RestartFolder, State);
        auto Helper = FPlatformProcess::OpenProcess(RestartHelperPid);
        const bool bRunning = Helper.IsValid() && FPlatformProcess::IsProcRunning(Helper);
        FPlatformProcess::CloseProc(Helper);
        if (State == TEXT("failed") || !bRunning)
        {
            if (State != TEXT("failed"))
            {
                GitWorkspace::CancelRestartPull(RestartFolder);
                RestartMessage = TEXT("Restart helper stopped while this editor remained open. No integration was permitted. Fetch and review before retrying.");
            }
            RestartFolder.Empty(); Feedback = RestartMessage;
        }
    }
    if (Pending.IsValid() && Pending.IsReady())
    {
        FGitWorkspaceTaskResult Result = Pending.Get();
        Pending = TFuture<FGitWorkspaceTaskResult>();
        bCheckingLocks = false;
        bDownloadingLfs = false;
        bPreparingRestart = false;
        if (Result.bRemote) { Remote = MoveTemp(Result.Remote); IncomingLfs = GitWorkspace::FIncomingLfsResult(); }
        else if (!Result.bDiff && (Remote.Head != Result.Snapshot.Head || Remote.Branch != Result.Snapshot.Branch)) Remote.bValid = false;
        if (Result.bIncomingLfs) IncomingLfs = MoveTemp(Result.IncomingLfs);
        if (Result.bReload && IncomingLfs.Matches(Remote))
        {
            Result.Message = FinishReloadPull();
            Result.Snapshot = Repository->Refresh();
            if (Result.Snapshot.Head == Remote.RemoteHead)
            {
                RestartMessage = Result.Message;
                Locks.bVerified = false; bVerifyLocksOnOpen = true;
            }
            if (Result.Snapshot.Head != Remote.Head) Remote.bValid = false;
        }
        else if (Result.bReload && Result.Message.IsEmpty()) Result.Message = TEXT("The review changed or expired during preparation. Fetch and review again.");
        if (Result.bLocks)
        {
            // Retain old ownership only as explicitly stale data for the same context.
            if (!Result.Locks.bVerified && Result.Locks.Context == Locks.Context)
            {
                Result.Locks.Locks = Locks.Locks;
                Result.Locks.VerifiedAt = Locks.VerifiedAt;
            }
            Locks = MoveTemp(Result.Locks);
        }
        if (Result.bDiff) DiffText = Result.Message;
        else
        {
            if (Snapshot.Root != Result.Snapshot.Root || Snapshot.Branch != Result.Snapshot.Branch) Locks.bVerified = false;
            Snapshot = MoveTemp(Result.Snapshot);
            Feedback = Snapshot.bValid ? Result.Message : Snapshot.Error;
            if (Result.bCommitSucceeded) Message->SetText(FText::GetEmpty());
            RebuildRows();
        }
        if (Result.bRestart && Result.Restart.bReady)
        {
            const auto Review = GitWorkspace::ReviewIncoming(Remote, Snapshot);
            FString Error;
            if (!Review.bCanRestart || !GitWorkspace::ApproveRestartClose(Result.Restart.Folder, Error))
            {
                GitWorkspace::CancelRestartPull(Result.Restart.Folder);
                Feedback = Review.bCanRestart ? Error : Review.RestartBlocker;
            }
            else
            {
                RestartFolder = Result.Restart.Folder;
                RestartHelperPid = Result.Restart.HelperPid;
                RestartMessage = TEXT("Waiting for normal editor shutdown. If you cancel shutdown, use Cancel restart Pull; the helper times out after three minutes without changing files.");
                if (auto Window = IncomingWindow.Pin()) Window->RequestDestroyWindow();
                FModuleManager::LoadModuleChecked<IMainFrameModule>(TEXT("MainFrame")).RequestCloseEditor();
            }
        }
        if (IncomingWindow.IsValid() && IncomingReport) IncomingReport->SetText(IncomingReportText());
        // Publish local status before starting the network query. Opening a tab
        // schedules one read-only check; failure must not cause a retry loop.
        if (bVerifyLocksOnOpen)
        {
            bVerifyLocksOnOpen = false;
            if (Snapshot.bValid) VerifyLocks();
        }
    }
}
FReply SGitWorkspace::Refresh()
{
    auto Repo = Repository;
    Start([Repo] { FGitWorkspaceTaskResult R; R.Snapshot = Repo->Refresh(); R.Message = TEXT("Local status refreshed. Commit preserves later edits and never pushes or unlocks."); return R; });
    return FReply::Handled();
}
void SGitWorkspace::RebuildRows()
{
    if (!List) return;
    const auto PreviousSelection = List->GetSelectedItems();
    Rows.Empty(); Selection.Reset(); DiffText.Empty(); List->ClearSelection();
    if (Snapshot.bValid)
    {
        auto Files = Snapshot.Files;
        Files.Sort([](const auto& A, const auto& B)
        {
            auto IsAsset = [](const auto& F) { return F.Path.EndsWith(TEXT(".uasset")) || F.Path.EndsWith(TEXT(".umap")); };
            if (IsAsset(A) != IsAsset(B)) return IsAsset(A);
            return A.Path < B.Path;
        });
        for (bool bStaged : {false, true})
        {
            int32 Count = 0, Visible = 0;
            for (const auto& File : Files) if (bStaged ? File.HasStaged() : File.HasUnstaged())
            { ++Count; if (IsPathVisible(File.Path)) ++Visible; }
            auto Header = MakeShared<FGitWorkspaceRow>();
            Header->Group = (bStaged ? FString(TEXT("Staged changes · ")) : FString(TEXT("Unstaged changes · "))) + (Visible == Count ? FString::FromInt(Count) : FString::Printf(TEXT("%d of %d"), Visible, Count));
            Rows.Add(Header);
            for (const auto& File : Files)
            {
                if ((bStaged ? File.HasStaged() : File.HasUnstaged()) && IsPathVisible(File.Path))
                { auto Row = MakeShared<FGitWorkspaceRow>(); Row->File = File; Row->bStaged = bStaged; Rows.Add(Row); }
            }
        }
    }
    if (Snapshot.bValid)
    {
        TMap<FString, GitWorkspace::FFile> Clean;
        for (const auto& File : Locks.Candidates) Clean.Add(File.Path, File);
        for (const auto& Pair : Locks.Locks) if (!Clean.Contains(Pair.Key))
        { GitWorkspace::FFile F; F.Path = Pair.Key; Clean.Add(Pair.Key, F); }
        for (const auto& File : Snapshot.Files) Clean.Remove(File.Path);
        if (Clean.Num())
        {
            TArray<FString> Paths; Clean.GetKeys(Paths); Paths.Sort();
            const int32 Visible = Paths.FilterByPredicate([this](const auto& Path) { return IsPathVisible(Path); }).Num();
            auto Header = MakeShared<FGitWorkspaceRow>(); Header->Group = TEXT("Clean assets / server locks · ") + (Visible == Clean.Num() ? FString::FromInt(Visible) : FString::Printf(TEXT("%d of %d"), Visible, Clean.Num())); Rows.Add(Header);
            for (const auto& Path : Paths) { if (!IsPathVisible(Path)) continue; auto Row = MakeShared<FGitWorkspaceRow>(); Row->File = Clean[Path]; Rows.Add(Row); }
        }
    }
    List->RequestListRefresh();
    for (const auto& Row : Rows) if (Row->Group.IsEmpty())
        for (const auto& Old : PreviousSelection)
            if (Old->Group.IsEmpty() && Old->File.Path == Row->File.Path && Old->bStaged == Row->bStaged)
            { List->SetItemSelection(Row, true); break; }
}
bool SGitWorkspace::IsPathVisible(const FString& Path) const
{
    return (!bContentOnly || Path.StartsWith(TEXT("Content/"), ESearchCase::CaseSensitive)) && (FileFilter.IsEmpty() || Path.Contains(FileFilter));
}
int32 SGitWorkspace::HiddenStagedCount() const
{
    int32 Count = 0;
    for (const auto& File : Snapshot.Files) if (File.HasStaged() && !IsPathVisible(File.Path)) ++Count;
    return Count;
}
TSharedRef<ITableRow> SGitWorkspace::MakeRow(TSharedPtr<FGitWorkspaceRow> Row, const TSharedRef<STableViewBase>& Owner)
{ return SNew(SFileRow, Owner).Item(Row).Locks(&Locks); }
bool SGitWorkspace::HasDirtyPackages() const
{
    TArray<UPackage*> Dirty;
    FEditorFileUtils::GetDirtyContentPackages(Dirty); FEditorFileUtils::GetDirtyWorldPackages(Dirty);
    return Dirty.Num() > 0;
}
TArray<FString> SGitWorkspace::SelectedIndexPaths(bool bStage, int32* SkippedSubmodules) const
{
    TArray<FString> Paths;
    if (SkippedSubmodules) *SkippedSubmodules = 0;
    if (!List) return Paths;
    for (const auto& Row : List->GetSelectedItems())
    {
        if (!Row->Group.IsEmpty() || Row->bStaged == bStage || !(bStage ? Row->File.HasUnstaged() : Row->File.HasStaged())) continue;
        if (Row->File.bSubmodule) { if (SkippedSubmodules) ++*SkippedSubmodules; continue; }
        Paths.AddUnique(Row->File.Path);
    }
    return Paths;
}
FReply SGitWorkspace::ChangeIndex(bool bStage)
{
    if (!IsIdle()) return FReply::Handled();
    if (bStage && HasDirtyPackages()) { Feedback = TEXT("Save dirty assets first. Stage uses saved files; saving never stages automatically."); return FReply::Handled(); }
    int32 Skipped = 0;
    const auto Paths = SelectedIndexPaths(bStage, &Skipped);
    if (!Paths.Num())
    {
        Feedback = Skipped ? TEXT("Selected submodules need their own repository workflow; no eligible files selected.")
            : (bStage ? TEXT("Select files in Unstaged changes.") : TEXT("Select files in Staged changes."));
        return FReply::Handled();
    }
    auto Repo = Repository;
    Start([Repo, Paths, bStage, Skipped]
    {
        const auto Op = bStage ? Repo->Stage(Paths) : Repo->Unstage(Paths);
        FGitWorkspaceTaskResult R; R.Snapshot = Repo->Refresh();
        R.Message = Op.Ok() ? FString::Printf(TEXT("%s %d selected files.%s"), bStage ? TEXT("Staged") : TEXT("Unstaged"), Paths.Num(), bStage ? TEXT("") : TEXT(" Working files preserved.")) : Op.Error;
        if (Skipped) R.Message += FString::Printf(TEXT(" Skipped %d submodule(s); manage their commits/pointers separately."), Skipped);
        return R;
    });
    return FReply::Handled();
}
FReply SGitWorkspace::Commit()
{
    if (!IsIdle()) return FReply::Handled();
    if (HiddenStagedCount()) { Feedback = TEXT("Staged files are hidden by this view. Choose Whole repo and clear the search to review all staged files before committing."); return FReply::Handled(); }
    if (HasDirtyPackages()) { Feedback = TEXT("Save dirty assets before committing here. Saving will not change the staged snapshot; review any later edits afterward."); return FReply::Handled(); }
    auto Repo = Repository; const auto Reviewed = Snapshot; const FString Description = Message->GetText().ToString();
    Start([Repo, Reviewed, Description]
    {
        const auto Op = Repo->Commit(Reviewed, Description);
        FGitWorkspaceTaskResult R; R.Snapshot = Repo->Refresh(); R.bCommitSucceeded = Op.Ok();
        R.Message = Op.Ok() ? TEXT("Local commit created. Nothing pushed. Fetch upstream to review this commit for Push.\nNo locks were released. Use Verify locks to confirm server ownership.\n") + Op.Text() + Op.Error : Op.Error;
        return R;
    });
    return FReply::Handled();
}
FReply SGitWorkspace::ShowDiff()
{
    if (!Selection || !IsIdle()) return FReply::Handled();
    auto Repo = Repository; const auto Row = *Selection;
    Start([Repo, Row]
    {
        FGitWorkspaceTaskResult R; R.bDiff = true;
        const auto Op = Repo->Diff(Row.File.Path, Row.bStaged);
        R.Message = Op.Ok() ? Op.Text() : Op.Error;
        if (Row.File.bLfs) R.Message = TEXT("LFS pointer comparison — object identity, not a visual asset diff.\n\n") + R.Message;
        if (R.Message.IsEmpty()) R.Message = TEXT("No text differences. Refresh if this file changed externally.");
        return R;
    });
    return FReply::Handled();
}
FText SGitWorkspace::Inspector() const
{
    if (!Selection || !Selection->Group.IsEmpty()) return Text(TEXT("Select a file to review its working and staged states.\n\nVerify locks to inspect server ownership and include clean tracked assets."));
    const auto& F = Selection->File;
    return Text(F.Path + TEXT("\n\nWorking: ") + State(F.Working) + TEXT("\nStaged: ") + State(F.Index)
        + (F.HasStaged() && F.HasUnstaged() ? TEXT("\nNew edits since staging; these will remain outside the commit.") : TEXT(""))
        + (TEXT("\n\nLock: ") + Locks.Label(F.Path, F.bLockable))
        + (Locks.Locks.Contains(F.Path) ? TEXT("\nLast known owner: ") + Locks.Locks[F.Path].Owner + TEXT("\nLock ID: ") + Locks.Locks[F.Path].Id + TEXT("\nAcquired: ") + Locks.Locks[F.Path].LockedAt : FString())
        + (F.bSubmodule ? TEXT("\nSubmodule: changes must be handled in its own repository.") : TEXT(""))
        + (F.bLfs && F.bLockable ? TEXT("\n") + LockHint().ToString() : FString()));
}

bool SGitWorkspace::CanLockSelected() const
{
    if (!IsIdle() || !Selection || !Selection->Group.IsEmpty() || !List || List->GetSelectedItems().Num() != 1) return false;
    const auto& File = Selection->File;
    if (!File.bLfs || !File.bLockable || File.bSubmodule || File.bConflict || File.Index == 'D' || File.Working == 'D') return false;
    const auto State = Locks.State(File.Path, true);
    return State == GitWorkspace::ELockState::Unknown || State == GitWorkspace::ELockState::Stale || State == GitWorkspace::ELockState::Unlocked;
}
FText SGitWorkspace::LockHint() const
{
    if (!Selection || !Selection->Group.IsEmpty() || !List || List->GetSelectedItems().Num() != 1) return Text(TEXT("Select one saved asset to lock."));
    const auto& File = Selection->File;
    if (!File.bLfs || !File.bLockable) return Text(TEXT("This file is not configured for Git LFS locking."));
    if (File.bSubmodule || File.bConflict || File.Index == 'D' || File.Working == 'D') return Text(TEXT("Resolve this file's state before acquiring a lock."));
    const auto State = Locks.State(File.Path, true);
    if (State == GitWorkspace::ELockState::Ours) return Text(TEXT("You own this lock. Push keeps it; Unlock is a separate handoff."));
    if (State == GitWorkspace::ELockState::Theirs) return Text(TEXT("Another user owns this lock. Verify locks to refresh ownership."));
    return Text(File.bUntracked
        ? TEXT("New saved asset: Lock asset verifies the server and reserves this path. It does not stage, commit or push the asset. Automatic locking is not enabled.")
        : TEXT("Lock asset verifies the server and requests a lock. Verify locks only checks ownership; it does not acquire a lock."));
}

FText SGitWorkspace::LockStatusText() const
{
    if (bCheckingLocks) return Text(TEXT("Checking locks on ") + LockRemote + TEXT("…"));
    if (!Locks.Error.IsEmpty()) return Text(Locks.Error + TEXT("\nOwnership is not verified. Use Verify locks to retry when ready."));
    if (!Locks.VerifiedAt.GetTicks()) return Text(TEXT("Lock ownership has not been verified. Verify to include clean tracked assets."));
    return Text(Locks.Endpoint + TEXT("  |  ") + (Locks.IsFresh() ? TEXT("Verified at ") : TEXT("Stale — last verified at ")) + Locks.VerifiedAt.ToIso8601());
}

FReply SGitWorkspace::VerifyLocks()
{
    if (!IsIdle()) return FReply::Handled();
    auto Repo = Repository; const FString SelectedLockRemote = LockRemote;
    Locks.bVerified = false;
    bCheckingLocks = true;
    Start([Repo, SelectedLockRemote]
    {
        FGitWorkspaceTaskResult R; R.bLocks = true; R.Locks = Repo->VerifyLocks(SelectedLockRemote); R.Snapshot = Repo->Refresh();
        R.Message = R.Locks.IsFresh() ? TEXT("Server ownership verified. This snapshot expires after 60 seconds; every mutation verifies again.") : R.Locks.Error;
        return R;
    });
    return FReply::Handled();
}
FReply SGitWorkspace::ChangeLock(bool bUnlock)
{
    if (!IsIdle() || !Selection || !Selection->Group.IsEmpty()) return FReply::Handled();
    if (List->GetSelectedItems().Num() != 1) { Feedback = TEXT("Select one asset for a lock operation."); return FReply::Handled(); }
    if (bUnlock && HasDirtyPackages()) { Feedback = TEXT("Save dirty assets and review their Git state before unlocking."); return FReply::Handled(); }
    if (!bUnlock && !CanLockSelected()) { Feedback = LockHint().ToString(); return FReply::Handled(); }
    auto Repo = Repository; const auto Reviewed = Locks; const FString Path = Selection->File.Path, SelectedRemote = LockRemote;
    if (bUnlock && FMessageDialog::Open(EAppMsgType::YesNo, Text(TEXT("Release the lock on ") + Path + TEXT("?\n\nBranch: ") + Snapshot.Branch + TEXT("\nCommit: ") + Snapshot.Head + TEXT("\n\nConfirm that your team handoff is complete and no other clone or worktree still needs this reservation. The plugin will check saved changes, stashes and the live upstream before unlocking."))) != EAppReturnType::Yes) return FReply::Handled();
    Locks.bVerified = false;
    Start([Repo, Reviewed, Path, SelectedRemote, bUnlock]
    {
        const auto LockReview = !bUnlock && !Reviewed.IsFresh() ? Repo->VerifyLocks(SelectedRemote) : Reviewed;
        const auto Op = Repo->ChangeLock(LockReview, Path, bUnlock, bUnlock);
        FGitWorkspaceTaskResult R; R.bLocks = true; R.Locks = Repo->VerifyLocks(SelectedRemote); R.Snapshot = Repo->Refresh();
        R.Message = Op.Ok() ? (bUnlock ? TEXT("Lock released and server state verified.") : TEXT("Lock acquired and server ownership verified.")) : Op.Error;
        return R;
    });
    return FReply::Handled();
}

FReply SGitWorkspace::ShowIncomingReview()
{
    if (!IsIdle()) return FReply::Handled();
    if (auto Old = IncomingWindow.Pin()) Old->RequestDestroyWindow();
    const auto Window = SNew(SWindow).Title(Text(TEXT("Review incoming changes"))).ClientSize(FVector2D(840, 640)).SupportsMinimize(false);
    IncomingWindow = Window;
    TWeakPtr<SWindow> WeakWindow = Window;
    TWeakPtr<SGitWorkspace> WeakThis = SharedThis(this);
    Window->SetContent(SNew(SBorder).Padding(12)
    [
        SNew(SVerticalBox)
        + SVerticalBox::Slot().FillHeight(1)
        [SAssignNew(IncomingReport, SMultiLineEditableTextBox).IsReadOnly(true).AutoWrapText(true).Text(IncomingReportText()).Font(FAppStyle::GetFontStyle("MonoFont"))]
        + SVerticalBox::Slot().AutoHeight().Padding(0, 12, 0, 0)
        [
            SNew(SHorizontalBox)
            + SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 8, 0)
            [SNew(SButton).Text(Text(TEXT("Copy review"))).OnClicked_Lambda([WeakThis]
            {
                if (auto Panel = WeakThis.Pin()) if (Panel->IncomingReport) FPlatformApplicationMisc::ClipboardCopy(*Panel->IncomingReport->GetText().ToString());
                return FReply::Handled();
            })]
            + SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 8, 0)
            [SNew(SButton).Text(Text(TEXT("Download LFS assets")))
                .ToolTipText(Text(TEXT("Download and verify all LFS objects in the fetched commit, including files outside the Content view. Saves objects to the local cache without replacing working files, saving packages or changing locks. Pull still requires its own safety checks.")))
                .IsEnabled_Lambda([WeakThis] { auto Panel = WeakThis.Pin(); return Panel && Panel->IsIdle() && Panel->Remote.IsFresh() && Panel->Remote.Behind > 0 && Panel->Remote.Ahead == 0; })
                .OnClicked_Lambda([WeakThis] { if (auto Panel = WeakThis.Pin()) return Panel->DownloadIncomingLfs(); return FReply::Handled(); })]
            + SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 8, 0)
            [SNew(SButton).Text(Text(TEXT("Pull…")))
                .ToolTipText_Lambda([WeakThis] { auto Panel = WeakThis.Pin(); if (!Panel) return FText::GetEmpty(); const auto Review = GitWorkspace::ReviewIncoming(Panel->Remote, Panel->Snapshot); return Text(Review.bCanReload ? TEXT("Pull and refresh assets while this editor stays open. Locks retained.") : Review.ReloadBlocker); })
                .IsEnabled_Lambda([WeakThis] { auto Panel = WeakThis.Pin(); return Panel && Panel->IsIdle() && GitWorkspace::ReviewIncoming(Panel->Remote, Panel->Snapshot).bCanReload; })
                .OnClicked_Lambda([WeakThis] { if (auto Panel = WeakThis.Pin()) return Panel->ReloadPull(); return FReply::Handled(); })]
            + SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 8, 0)
            [SNew(SButton).Text(Text(TEXT("Pull and reopen…")))
                .ToolTipText_Lambda([WeakThis] { auto Panel = WeakThis.Pin(); if (!Panel) return FText::GetEmpty(); const auto Review = GitWorkspace::ReviewIncoming(Panel->Remote, Panel->Snapshot); return Text(Review.bCanRestart ? TEXT("Verify LFS, close this editor, fast-forward the reviewed assets and reopen. Locks retained.") : Review.RestartBlocker); })
                .IsEnabled_Lambda([WeakThis] { auto Panel = WeakThis.Pin(); return Panel && Panel->IsIdle() && GitWorkspace::ReviewIncoming(Panel->Remote, Panel->Snapshot).bCanRestart; })
                .OnClicked_Lambda([WeakThis] { if (auto Panel = WeakThis.Pin()) return Panel->RestartPull(); return FReply::Handled(); })]
            + SHorizontalBox::Slot().AutoWidth()
            [SNew(SButton).Text(Text(TEXT("Close"))).OnClicked_Lambda([WeakWindow] { if (auto W = WeakWindow.Pin()) W->RequestDestroyWindow(); return FReply::Handled(); })]
        ]
    ]);
    FSlateApplication::Get().AddWindow(Window);
    return FReply::Handled();
}

FText SGitWorkspace::IncomingLfsStatus() const
{
    if (bDownloadingLfs) return Text(TEXT("Downloading and verifying incoming LFS assets… Working files remain in place."));
    if (!IncomingLfs.Error.IsEmpty()) return Text(TEXT("LFS DOWNLOAD / VERIFICATION FAILED\n") + IncomingLfs.Error);
    if (IncomingLfs.Matches(Remote)) return Text(TEXT("LFS cache verified for commit ") + IncomingLfs.Commit + TEXT(" at ") + IncomingLfs.VerifiedAt.ToIso8601()
        + TEXT(".\nWorking files have not been updated. Pull repeats safety checks before replacing files. External cache changes can invalidate this result."));
    return Text(TEXT("LFS cache has not been verified for this review. Download LFS assets checks the complete fetched commit, independent of the Content view. Fetch again if the review has expired."));
}

FText SGitWorkspace::IncomingReportText() const
{
    return Text(TEXT("INCOMING LFS ASSETS\n") + IncomingLfsStatus().ToString() + TEXT("\n\n") + GitWorkspace::ReviewIncoming(Remote, Snapshot).Text);
}

FReply SGitWorkspace::DownloadIncomingLfs()
{
    if (!IsIdle()) return FReply::Handled();
    if (!Remote.IsFresh() || Remote.Behind <= 0 || Remote.Ahead != 0)
    { Feedback = TEXT("Fetch and review an incoming fast-forward before downloading LFS assets."); return FReply::Handled(); }
    IncomingLfs = GitWorkspace::FIncomingLfsResult(); bDownloadingLfs = true;
    if (IncomingReport) IncomingReport->SetText(IncomingReportText());
    auto Repo = Repository; const auto Reviewed = Remote;
    Start([Repo, Reviewed]
    {
        FGitWorkspaceTaskResult R; R.bIncomingLfs = true;
        R.IncomingLfs = Repo->PrepareIncomingLfs(Reviewed); R.Snapshot = Repo->Refresh();
        R.Message = R.IncomingLfs.bVerified ? TEXT("Incoming LFS cache verified. Working files were not replaced; locks retained. Review incoming for integration requirements.") : R.IncomingLfs.Error;
        return R;
    });
    return FReply::Handled();
}

FReply SGitWorkspace::RestartPull()
{
    if (!IsIdle()) return FReply::Handled();
    const auto Review = GitWorkspace::ReviewIncoming(Remote, Snapshot);
    if (!Review.bCanRestart) { Feedback = Review.RestartBlocker; return FReply::Handled(); }
    const FString Prompt = TEXT("Pull reviewed commit ") + Remote.RemoteHead + TEXT("\n") + Remote.Remote + TEXT(" / ") + Remote.RemoteRef
        + TEXT("\n\nDownload and verify LFS, close the editor normally, fast-forward these assets/documents, then reopen this project. Locks remain held.\n\nAll packages must be saved and the entire Git working tree clean. No automatic stash or discard. Close other Unreal editors first. If integration fails after files change, the editor stays closed with recovery instructions.");
    if (FMessageDialog::Open(EAppMsgType::YesNo, Text(Prompt)) != EAppReturnType::Yes) return FReply::Handled();
    if (HasDirtyPackages()) { Feedback = TEXT("Save or resolve unsaved packages before restart Pull."); return FReply::Handled(); }
    auto Repo = Repository; const auto Reviewed = Remote; const FString Project = FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath());
    bPreparingRestart = true;
    Start([Repo, Reviewed, Project]
    {
        FGitWorkspaceTaskResult R; R.bRestart = true; R.Restart = GitWorkspace::StartRestartPull(*Repo, Reviewed, Project);
        R.Snapshot = Repo->Refresh(); R.Message = R.Restart.Error; return R;
    });
    return FReply::Handled();
}
FReply SGitWorkspace::ReloadPull()
{
    if (!IsIdle()) return FReply::Handled();
    const auto Review = GitWorkspace::ReviewIncoming(Remote, Snapshot);
    if (!Review.bCanReload) { Feedback = Review.ReloadBlocker; return ShowIncomingReview(); }
    const FString Prompt = TEXT("Pull reviewed commit ") + Remote.RemoteHead + TEXT("\n") + Remote.Remote + TEXT(" / ") + Remote.RemoteRef
        + TEXT("\n\nDownload and verify LFS, then refresh incoming assets while this editor stays open. Loaded assets will reload. Undo history and selection may reset; locks remain held.\n\nAll packages must be saved and the entire working tree clean. Close other Unreal editors first. No automatic save, stash or discard. If files change but hydration or reload fails, this editor will close without saving and show recovery instructions.");
    if (FMessageDialog::Open(EAppMsgType::YesNo, Text(Prompt)) != EAppReturnType::Yes) return FReply::Handled();
    const auto Checked = GitWorkspace::ReviewIncoming(Remote, Snapshot);
    if (!Checked.bCanReload) { Feedback = Checked.ReloadBlocker; return FReply::Handled(); }
    auto Repo = Repository; const auto Reviewed = Remote;
    bDownloadingLfs = true;
    Start([Repo, Reviewed]
    {
        FGitWorkspaceTaskResult R; R.bIncomingLfs = true; R.bReload = true;
        R.IncomingLfs = Repo->PrepareIncomingLfs(Reviewed); R.Snapshot = Repo->Refresh(); R.Message = R.IncomingLfs.Error; return R;
    });
    return FReply::Handled();
}
FString SGitWorkspace::FinishReloadPull()
{
#if PLATFORM_MAC
    TGuardValue<bool> Busy(bReloading, true);
    FScopedSlowTask Task(1.f, Text(TEXT("Pulling and refreshing assets…")));
    Task.MakeDialog(false); Task.EnterProgressFrame(1.f); Task.ForceRefresh();
    GitWorkspaceSession::FEditorWriteScope Access; FString Error;
    if (!Access.Acquire(Remote.Root, Error)) return Error;
    const auto Result = GitWorkspace::PullAndReload(*Repository, Remote, IncomingLfs, Access.Lease());
    if (Result.bRecoveryRequired)
    {
        FString Root, GitDir; GitWorkspaceSession::FindRepository(Remote.Root, Root, GitDir);
        GitWorkspaceSession::StopForRecovery(Result.Message + TEXT("\n\nThe editor will close without saving to protect the updated files. Recovery instructions: ") + GitWorkspaceSession::RecoveryFile(GitDir));
    }
    return Result.Message;
#else
    return TEXT("Asset reload Pull is currently available on Mac only.");
#endif
}
FReply SGitWorkspace::CancelRestart()
{
    GitWorkspace::CancelRestartPull(RestartFolder);
    RestartMessage = TEXT("Cancellation requested. Waiting for the helper to stop; working files remain unchanged while this editor is open.");
    return FReply::Handled();
}

FText SGitWorkspace::PushHint() const
{
    if (!IsIdle()) return Text(TEXT("Push unavailable while a Git operation is running."));
    if (!Remote.IsFresh()) return Text(TEXT("Fetch upstream to enable Push. A new commit or an expired review requires another Fetch."));
    if (Remote.Ahead > 0 && Remote.Behind > 0) return Text(TEXT("Push blocked: branches have diverged. Reconcile incoming changes externally, then Fetch again."));
    if (Remote.Behind > 0) return Text(TEXT("Push unavailable: incoming commits need review. Use Review incoming to choose a safe Pull option."));
    if (Remote.Ahead == 0) return Text(TEXT("Nothing to push: no outgoing commits."));
    return Text(TEXT("Push is ready for review. LFS upload and lock ownership are checked before publishing; locks are not released."));
}

FReply SGitWorkspace::RemoteAction(int32 Action)
{
    if (!IsIdle()) return FReply::Handled();
    if (Action == 2)
    {
        const auto Review = GitWorkspace::ReviewIncoming(Remote, Snapshot);
        if (Review.bCanReload) return ReloadPull();
        if (!Review.bCanPull) { Feedback = Review.Blocker; return ShowIncomingReview(); }
    }
    auto Repo = Repository; const auto Reviewed = Remote;
    if (Action && !Reviewed.IsFresh()) { Feedback = TEXT("Fetch and review the upstream first."); return FReply::Handled(); }
    if (Action)
    {
        FString Prompt = (Action == 1 ? TEXT("Push reviewed commit ") + Reviewed.Head : TEXT("Fast-forward to reviewed commit ") + Reviewed.RemoteHead)
            + TEXT("\n") + Reviewed.Remote + TEXT(" / ") + Reviewed.RemoteRef;
        Prompt += Action == 1 ? TEXT("\n\nOnly committed data is published. Staged, working and unsaved edits are excluded. Locks are retained.")
            : FString::Printf(TEXT("\n\n%d reviewed paths. Requires a clean working tree and documentation-only changes. Use Review incoming for file/package details. Asset, code and configuration updates require external integration with the editor closed."), Reviewed.IncomingChanges.Num());
        if (FMessageDialog::Open(EAppMsgType::YesNo, Text(Prompt)) != EAppReturnType::Yes) return FReply::Handled();
    }
    Remote.bValid = false;
    Start([Repo, Reviewed, Action]
    {
        FGitWorkspaceTaskResult R; R.bRemote = true;
        if (Action)
        {
            const auto Op = Action == 1 ? Repo->Push(Reviewed) : Repo->Pull(Reviewed);
            R.Message = Op.Ok() ? (Action == 1 ? TEXT("Reviewed commit pushed and verified. Locks retained.") : TEXT("Reviewed documentation update fast-forwarded.")) : Op.Error;
        }
        R.Remote = Repo->Fetch(); R.Snapshot = Repo->Refresh();
        if (!Action) R.Message = R.Remote.IsFresh() ? TEXT("Upstream fetched. Working files and index were not changed.") : R.Remote.Error;
        return R;
    });
    return FReply::Handled();
}

void GitWorkspaceUI::Open() { FGlobalTabmanager::Get()->TryInvokeTab(TabName); }
void GitWorkspaceUI::Register()
{
    OpenCommand = IConsoleManager::Get().RegisterConsoleCommand(TEXT("GitWorkspace.Open"), TEXT("Open the Git Workspace tab."), FConsoleCommandDelegate::CreateStatic(&GitWorkspaceUI::Open));
    FGlobalTabmanager::Get()->RegisterNomadTabSpawner(TabName, FOnSpawnTab::CreateLambda([](const FSpawnTabArgs&)
    {
        TSharedRef<SGitWorkspace> Workspace = SNew(SGitWorkspace); OpenWorkspace = Workspace;
        TSharedRef<SDockTab> Tab = SNew(SDockTab).TabRole(ETabRole::NomadTab)[Workspace]; OpenTab = Tab;
        return Tab;
    })).SetDisplayName(Text(TEXT("Git Workspace"))).SetTooltipText(Text(TEXT("Review, stage and commit local Git changes.")));
    MenuStartupHandle = UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateLambda([]
    {
        UToolMenu* Menu = UToolMenus::Get()->ExtendMenu("LevelEditor.MainMenu.Window");
        Menu->FindOrAddSection("GitWorkspace").AddMenuEntry("OpenGitWorkspace", Text(TEXT("Git Workspace")),
            Text(TEXT("Open the Git-native local workflow.")), FSlateIcon(), FUIAction(FExecuteAction::CreateStatic(&GitWorkspaceUI::Open)));
    }));
}
void GitWorkspaceUI::Unregister()
{
    if (OpenCommand) { IConsoleManager::Get().UnregisterConsoleObject(OpenCommand); OpenCommand = nullptr; }
    UToolMenus::UnRegisterStartupCallback(MenuStartupHandle);
    if (auto Workspace = OpenWorkspace.Pin()) Workspace->WaitForWork();
    if (auto Tab = OpenTab.Pin()) Tab->RequestCloseTab();
    if (UToolMenus::IsToolMenuUIEnabled())
    {
        if (UToolMenu* Menu = UToolMenus::Get()->FindMenu("LevelEditor.MainMenu.Window")) Menu->RemoveSection("GitWorkspace");
    }
    FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(TabName);
}
