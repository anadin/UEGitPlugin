// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#include "SGitWorkspace.h"
#include "GitSourceControlModule.h"
#include "Async/Async.h"
#include "FileHelpers.h"
#include "Misc/Paths.h"
#include "Misc/MessageDialog.h"
#include "Widgets/Input/SEditableTextBox.h"
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
        else if (Column == "Asset") Value = FPaths::GetCleanFilename(Item->File.Path);
        else if (Column == "Working") Value = State(Item->File.Working);
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
        [SNew(STextBlock).Text(Text(TEXT("Changes   ·   Local commits retain locks. Verify locks separately; push/pull and visual asset diffs are not yet available."))).AutoWrapText(true)]
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
            [SNew(SButton).Text(Text(TEXT("Lock asset"))).IsEnabled_Lambda([this] { return IsIdle() && Selection && Selection->Group.IsEmpty() && Selection->File.bLockable && Locks.State(Selection->File.Path, true) == GitWorkspace::ELockState::Unlocked; }).OnClicked_Lambda([this] { return ChangeLock(false); })]
            + SHorizontalBox::Slot().AutoWidth()
            [SNew(SButton).Text(Text(TEXT("Unlock…"))).IsEnabled_Lambda([this] { return IsIdle() && Selection && Selection->Group.IsEmpty() && Locks.State(Selection->File.Path, Selection->File.bLockable) == GitWorkspace::ELockState::Ours; }).OnClicked_Lambda([this] { return ChangeLock(true); })]
        ]
        + SVerticalBox::Slot().AutoHeight().Padding(10, 0, 10, 8)
        [SNew(STextBlock).AutoWrapText(true).Text_Lambda([this]
        {
            if (!Locks.Error.IsEmpty()) return Text(Locks.Error);
            if (!Locks.VerifiedAt.GetTicks()) return Text(TEXT("Lock ownership has not been verified. Verify to include clean tracked assets."));
            return Text(Locks.Endpoint + TEXT("  |  ") + (Locks.IsFresh() ? TEXT("Verified at ") : TEXT("Stale — last verified at ")) + Locks.VerifiedAt.ToIso8601());
        })]
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
                    [SNew(SButton).Text(Text(TEXT("Stage selected"))).IsEnabled_Lambda([this] { return IsIdle() && Snapshot.bValid; }).OnClicked_Lambda([this] { return ChangeIndex(true); })]
                    + SHorizontalBox::Slot().AutoWidth()
                    [SNew(SButton).Text(Text(TEXT("Unstage selected"))).IsEnabled_Lambda([this] { return IsIdle() && Snapshot.bValid; }).OnClicked_Lambda([this] { return ChangeIndex(false); })]
                ]
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
        + SVerticalBox::Slot().AutoHeight().Padding(10)
        [
            SNew(SHorizontalBox)
            + SHorizontalBox::Slot().FillWidth(1).Padding(0, 0, 12, 0)
            [SAssignNew(Message, SMultiLineEditableTextBox).HintText(Text(TEXT("Commit message"))).IsEnabled_Lambda([this] { return IsIdle(); })]
            + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
            [SNew(SButton).Text_Lambda([this] { return Text(FString::Printf(TEXT("Commit staged · %d"), Snapshot.StagedCount())); })
                .IsEnabled_Lambda([this] { return IsIdle() && Snapshot.bValid && Snapshot.StagedCount() && !Snapshot.HasConflicts() && !Snapshot.bOperationInProgress; })
                .OnClicked(this, &SGitWorkspace::Commit)]
        ]
        + SVerticalBox::Slot().AutoHeight().Padding(10, 0, 10, 10)
        [SNew(STextBlock).Text_Lambda([this] { return Text(IsIdle() ? Feedback : TEXT("Git operation running…")); }).AutoWrapText(true)]
    ];
    Refresh();
}
SGitWorkspace::~SGitWorkspace() { WaitForWork(); }
void SGitWorkspace::WaitForWork() { if (Pending.IsValid()) { Pending.Wait(); Pending = TFuture<FGitWorkspaceTaskResult>(); } }
void SGitWorkspace::Start(TFunction<FGitWorkspaceTaskResult()> Work)
{
    if (IsIdle()) Pending = Async(EAsyncExecution::ThreadPool, MoveTemp(Work));
}
void SGitWorkspace::Tick(const FGeometry&, double, float)
{
    if (Pending.IsValid() && Pending.IsReady())
    {
        FGitWorkspaceTaskResult Result = Pending.Get();
        Pending = TFuture<FGitWorkspaceTaskResult>();
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
    Rows.Empty(); Selection.Reset(); DiffText.Empty(); List->ClearSelection();
    if (Snapshot.bValid)
    {
        for (bool bStaged : {false, true})
        {
            int32 Count = 0;
            for (const auto& File : Snapshot.Files) Count += bStaged ? File.HasStaged() : File.HasUnstaged();
            auto Header = MakeShared<FGitWorkspaceRow>();
            Header->Group = (bStaged ? FString(TEXT("Staged changes · ")) : FString(TEXT("Unstaged changes · "))) + FString::FromInt(Count);
            Rows.Add(Header);
            for (const auto& File : Snapshot.Files)
            {
                if (bStaged ? File.HasStaged() : File.HasUnstaged())
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
            auto Header = MakeShared<FGitWorkspaceRow>(); Header->Group = TEXT("Clean assets / server locks · ") + FString::FromInt(Clean.Num()); Rows.Add(Header);
            TArray<FString> Paths; Clean.GetKeys(Paths); Paths.Sort();
            for (const auto& Path : Paths) { auto Row = MakeShared<FGitWorkspaceRow>(); Row->File = Clean[Path]; Rows.Add(Row); }
        }
    }
    List->RequestListRefresh();
}
TSharedRef<ITableRow> SGitWorkspace::MakeRow(TSharedPtr<FGitWorkspaceRow> Row, const TSharedRef<STableViewBase>& Owner)
{ return SNew(SFileRow, Owner).Item(Row).Locks(&Locks); }
bool SGitWorkspace::HasDirtyPackages() const
{
    TArray<UPackage*> Dirty;
    FEditorFileUtils::GetDirtyContentPackages(Dirty); FEditorFileUtils::GetDirtyWorldPackages(Dirty);
    return Dirty.Num() > 0;
}
FReply SGitWorkspace::ChangeIndex(bool bStage)
{
    if (!IsIdle()) return FReply::Handled();
    if (bStage && HasDirtyPackages()) { Feedback = TEXT("Save dirty assets first. Stage uses saved files; saving never stages automatically."); return FReply::Handled(); }
    TArray<FString> Paths;
    for (const auto& Row : List->GetSelectedItems())
        if (Row->Group.IsEmpty() && Row->bStaged != bStage && (bStage ? Row->File.HasUnstaged() : Row->File.HasStaged())) Paths.AddUnique(Row->File.Path);
    if (!Paths.Num()) { Feedback = bStage ? TEXT("Select files in Unstaged changes.") : TEXT("Select files in Staged changes."); return FReply::Handled(); }
    auto Repo = Repository;
    Start([Repo, Paths, bStage]
    {
        const auto Op = bStage ? Repo->Stage(Paths) : Repo->Unstage(Paths);
        FGitWorkspaceTaskResult R; R.Snapshot = Repo->Refresh();
        R.Message = Op.Ok() ? (bStage ? TEXT("Selected saved files staged.") : TEXT("Selected files unstaged. Working files preserved.")) : Op.Error;
        return R;
    });
    return FReply::Handled();
}
FReply SGitWorkspace::Commit()
{
    if (!IsIdle()) return FReply::Handled();
    if (HasDirtyPackages()) { Feedback = TEXT("Save dirty assets before committing here. Saving will not change the staged snapshot; review any later edits afterward."); return FReply::Handled(); }
    auto Repo = Repository; const auto Reviewed = Snapshot; const FString Description = Message->GetText().ToString();
    Start([Repo, Reviewed, Description]
    {
        const auto Op = Repo->Commit(Reviewed, Description);
        FGitWorkspaceTaskResult R; R.Snapshot = Repo->Refresh(); R.bCommitSucceeded = Op.Ok();
        R.Message = Op.Ok() ? TEXT("Local commit created. Nothing pushed; locks retained.\n") + Op.Text() + Op.Error : Op.Error;
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
        + (F.bSubmodule ? TEXT("\nSubmodule: changes must be handled in its own repository.") : TEXT("")));
}

FReply SGitWorkspace::VerifyLocks()
{
    if (!IsIdle()) return FReply::Handled();
    auto Repo = Repository; const FString Remote = LockRemote;
    Locks.bVerified = false;
    Start([Repo, Remote]
    {
        FGitWorkspaceTaskResult R; R.bLocks = true; R.Locks = Repo->VerifyLocks(Remote); R.Snapshot = Repo->Refresh();
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
    auto Repo = Repository; const auto Reviewed = Locks; const FString Path = Selection->File.Path;
    if (bUnlock && FMessageDialog::Open(EAppMsgType::YesNo, Text(TEXT("Release the lock on ") + Path + TEXT("?\n\nBranch: ") + Snapshot.Branch + TEXT("\nCommit: ") + Snapshot.Head + TEXT("\n\nConfirm that your team handoff is complete and no other clone or worktree still needs this reservation. The plugin will check saved changes, stashes and the live upstream before unlocking."))) != EAppReturnType::Yes) return FReply::Handled();
    Locks.bVerified = false;
    Start([Repo, Reviewed, Path, bUnlock]
    {
        const auto Op = Repo->ChangeLock(Reviewed, Path, bUnlock, bUnlock);
        FGitWorkspaceTaskResult R; R.bLocks = true; R.Locks = Repo->VerifyLocks(Reviewed.Remote); R.Snapshot = Repo->Refresh();
        R.Message = Op.Ok() ? (bUnlock ? TEXT("Lock released and server state verified.") : TEXT("Lock acquired and server ownership verified.")) : Op.Error;
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
