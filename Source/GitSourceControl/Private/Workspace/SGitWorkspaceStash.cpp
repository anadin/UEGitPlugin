// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#include "SGitWorkspace.h"
#include "Async/Async.h"
#include "GitWorkspacePullReview.h"
#include "GitWorkspaceEditorPull.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/SWindow.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Text/STextBlock.h"
#include "Misc/MessageDialog.h"
#include "Misc/ScopedSlowTask.h"
#if PLATFORM_MAC
#include "GitWorkspaceSession.h"
#endif
namespace { FText StashText(const FString& S) { return FText::FromString(S); } }
FReply SGitWorkspace::ShowStashes()
{
    if (!IsIdle()) return FReply::Handled();
    if (auto Old = StashWindow.Pin()) Old->RequestDestroyWindow();
    StashReview = GitWorkspace::FStashReview(); StashInspection = GitWorkspace::FStashInspection();
    const auto Window = SNew(SWindow).Title(StashText(TEXT("Git stashes"))).ClientSize(FVector2D(860, 650)).SupportsMinimize(false);
    StashWindow = Window;
    TWeakPtr<SGitWorkspace> Weak = SharedThis(this);
    auto Idle = [Weak] { auto P = Weak.Pin(); return P && P->IsIdle(); };
    Window->SetContent(SNew(SBorder).Padding(12)
    [SNew(SVerticalBox)
        + SVerticalBox::Slot().AutoHeight()
        [SNew(STextBlock).Text(StashText(TEXT("Stashes are local snapshots. Create captures all tracked changes across the repository; untracked and ignored files stay in place. Apply keeps the stash and retains locks."))).AutoWrapText(true)]
        + SVerticalBox::Slot().AutoHeight().Padding(0, 10)
        [SNew(SHorizontalBox)
            + SHorizontalBox::Slot().FillWidth(1)
            [SAssignNew(StashName, SEditableTextBox).HintText(StashText(TEXT("Name for a new stash"))).IsEnabled_Lambda(Idle)]
            + SHorizontalBox::Slot().AutoWidth().Padding(8, 0)
            [SNew(SButton).Text(StashText(TEXT("Review tracked changes"))).IsEnabled_Lambda(Idle).OnClicked_Lambda([Weak] { if (auto P = Weak.Pin()) return P->PreviewStash(FString()); return FReply::Handled(); })]
            + SHorizontalBox::Slot().AutoWidth()
            [SNew(SButton).Text(StashText(TEXT("Refresh stashes"))).IsEnabled_Lambda(Idle).OnClicked_Lambda([Weak] { if (auto P = Weak.Pin()) return P->RefreshStashes(); return FReply::Handled(); })]
        ]
        + SVerticalBox::Slot().FillHeight(0.3)
        [SNew(SScrollBox) + SScrollBox::Slot()[SAssignNew(StashRows, SVerticalBox)]]
        + SVerticalBox::Slot().AutoHeight().Padding(0, 8)
        [SNew(SCheckBox).IsChecked_Lambda([Weak] { auto P = Weak.Pin(); return P && P->bRestoreStashIndex ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
            .IsEnabled_Lambda(Idle).OnCheckStateChanged_Lambda([Weak](ECheckBoxState State)
            {
                if (auto P = Weak.Pin()) { P->bRestoreStashIndex = State == ECheckBoxState::Checked; P->StashReview.bValid = false; if (P->StashReport) P->StashReport->SetText(StashText(P->StashInspection.Text + TEXT("\nStaging option changed. Select the stash again before applying."))); }
            })[SNew(STextBlock).Text(StashText(TEXT("Restore staging when applying")))]]
        + SVerticalBox::Slot().FillHeight(0.7)
        [SAssignNew(StashReport, SMultiLineEditableTextBox).IsReadOnly(true).AutoWrapText(true).Text(StashText(TEXT("Select a stash to preview, or review your tracked changes before creating one.")))]
        + SVerticalBox::Slot().AutoHeight().Padding(0, 10, 0, 0)
        [SNew(SHorizontalBox)
            + SHorizontalBox::Slot().AutoWidth()
            [SNew(SButton).Text_Lambda([Weak] { auto P = Weak.Pin(); return StashText(P && P->StashReview.bCreate ? TEXT("Create stash…") : TEXT("Apply stash…")); })
                .IsEnabled_Lambda([Weak] { auto P = Weak.Pin(); return P && P->IsIdle() && P->StashReview.IsFresh() && (P->StashReview.bCreate || P->StashInspection.IsFresh()); })
                .OnClicked_Lambda([Weak] { if (auto P = Weak.Pin()) return P->RunStash(); return FReply::Handled(); })]
            + SHorizontalBox::Slot().AutoWidth().Padding(12, 0)
            [SNew(SButton).Text(StashText(TEXT("Drop selected stash…")))
                .IsEnabled_Lambda([Weak] { auto P = Weak.Pin(); return P && P->IsIdle() && P->StashInspection.IsFresh() && P->StashInspection.DropBlocker.IsEmpty(); })
                .OnClicked_Lambda([Weak] { if (auto P = Weak.Pin()) return P->DropSelectedStash(); return FReply::Handled(); })]
        ]
    ]);
    FSlateApplication::Get().AddWindow(Window);
    return RefreshStashes();
}
FReply SGitWorkspace::RefreshStashes()
{
    if (!IsIdle()) return FReply::Handled();
    StashReview.bValid = false; StashInspection.bValid = false; auto Repo = Repository;
    if (StashReport) StashReport->SetText(StashText(TEXT("Select a stash to inspect it, or review tracked changes to create one.")));
    Start([Repo] { FGitWorkspaceTaskResult R; R.bStashes = true; R.Stashes = Repo->ListStashes(); R.Snapshot = Repo->Refresh(); R.Message = R.Stashes.bValid ? TEXT("Local stash list refreshed.") : R.Stashes.Error; return R; });
    return FReply::Handled();
}
void SGitWorkspace::RebuildStashes()
{
    if (!StashRows) return;
    StashRows->ClearChildren();
    if (!Stashes.bValid || Stashes.Entries.IsEmpty())
        StashRows->AddSlot().AutoHeight()[SNew(STextBlock).Text(StashText(Stashes.bValid ? TEXT("No local stashes.") : Stashes.Error)).AutoWrapText(true)];
    TWeakPtr<SGitWorkspace> Weak = SharedThis(this);
    for (const auto& Entry : Stashes.Entries)
    {
        const FString Oid = Entry.Oid, Selector = Entry.Selector;
        const FString Label = Entry.Selector + TEXT("  ") + Entry.Oid.Left(10) + TEXT("  ") + Entry.Label.Replace(TEXT("\n"), TEXT("\\n")).Replace(TEXT("\r"), TEXT("\\r"));
        StashRows->AddSlot().AutoHeight().Padding(0, 2)
            [SNew(SButton).Text(StashText(Label)).ToolTipText(StashText(Entry.Oid)).IsEnabled_Lambda([Weak] { auto P = Weak.Pin(); return P && P->IsIdle(); })
                .OnClicked_Lambda([Weak, Oid, Selector] { if (auto P = Weak.Pin()) return P->PreviewStash(Oid, Selector); return FReply::Handled(); })];
    }
}
FReply SGitWorkspace::PreviewStash(const FString& Oid, const FString& Selector)
{
    if (!IsIdle()) return FReply::Handled();
    StashReview.bValid = false; StashInspection.bValid = false;
    if (StashReport) StashReport->SetText(StashText(TEXT("Reviewing stash and local work…")));
    auto Repo = Repository; const bool bIndex = bRestoreStashIndex;
    Start([Repo, Oid, Selector, bIndex]
    {
        FGitWorkspaceTaskResult R; R.bStashReview = true;
        if (!Oid.IsEmpty()) R.StashInspection = Repo->InspectStash(Oid, Selector);
        R.StashReview = Repo->ReviewStash(Oid, bIndex); R.Snapshot = Repo->Refresh();
        R.Message = R.StashReview.bValid || R.StashInspection.bValid ? TEXT("Stash preview ready. Review the saved paths and action eligibility.") : R.StashReview.Error;
        return R;
    });
    return FReply::Handled();
}
FReply SGitWorkspace::RunStash()
{
    if (!IsIdle() || !StashReview.IsFresh() || (!StashReview.bCreate && !StashInspection.IsFresh())) return FReply::Handled();
#if PLATFORM_MAC
    const auto Reviewed = StashReview; const FString Name = StashName ? StashName->GetText().ToString() : FString();
    TArray<GitWorkspace::FIncomingPackage> Packages;
    FString Error = GitWorkspace::ReviewPackageChanges(Reviewed.Local.Root, Reviewed.Changes, Packages);
    if (Reviewed.bCreate && Name.TrimStartAndEnd().IsEmpty()) Error = TEXT("Enter a name for the new stash.");
    if (!Error.IsEmpty()) { Feedback = Error; if (StashReport) StashReport->SetText(StashText(Error)); return FReply::Handled(); }
    const FString Prompt = (Reviewed.bCreate ? TEXT("Create stash: ") + Name : TEXT("Apply stash: ") + Reviewed.Oid) + TEXT("\n\n") + Reviewed.Text +
        TEXT("\nLoaded supported assets will reload; Undo/selection may reset. If a file update or reload becomes uncertain, the editor will close without saving and preserve recovery instructions. The stash will not be dropped.");
    if (FMessageDialog::Open(EAppMsgType::YesNo, StashText(Prompt)) != EAppReturnType::Yes) return FReply::Handled();
    {
        TGuardValue<bool> Busy(bReloading, true);
        FScopedSlowTask Task(1.f, StashText(TEXT("Updating stash and refreshing assets…"))); Task.MakeDialog(false); Task.EnterProgressFrame(1.f); Task.ForceRefresh();
        GitWorkspaceSession::FEditorWriteScope Access;
        if (!Access.Acquire(Reviewed.Local.Root, Error)) { Feedback = Error; if (StashReport) StashReport->SetText(StashText(Error)); return FReply::Handled(); }
        const auto Result = GitWorkspace::StashAndReload(*Repository, Reviewed, Name, Access.Lease());
        if (Result.bRecoveryRequired)
        {
            FString Root, GitDir; GitWorkspaceSession::FindRepository(Reviewed.Local.Root, Root, GitDir);
            GitWorkspaceSession::StopForRecovery(Result.Message + TEXT("\n\nThe editor will close without saving. Recovery report: ") + GitWorkspaceSession::RecoveryFile(GitDir));
        }
        Feedback = Result.Message; StashReview.bValid = false;
        if (StashReport) StashReport->SetText(StashText(Feedback));
        if (Result.bSuccess) { RestartMessage = Feedback; Locks.bVerified = false; bVerifyLocksOnOpen = true; }
    }
    auto Repo = Repository; const FString ResultMessage = Feedback;
    Start([Repo, ResultMessage] { FGitWorkspaceTaskResult R; R.bStashes = true; R.Stashes = Repo->ListStashes(); R.Snapshot = Repo->Refresh(); R.Message = ResultMessage; return R; });
#else
    Feedback = TEXT("Guarded stash mutations are currently available on Mac only.");
#endif
    return FReply::Handled();
}

FText SGitWorkspace::StashReportText() const
{
    if (StashReview.bCreate) return StashText(StashReview.bValid ? StashReview.Text : StashReview.Error);
    if (!StashInspection.bValid) return StashText(StashInspection.Error.IsEmpty() ? StashReview.Error : StashInspection.Error);
    return StashText(StashInspection.Text + (StashReview.IsFresh() ? TEXT("\nAPPLY READY\n") + StashReview.Text : TEXT("\nAPPLY BLOCKED\n") + StashReview.Error));
}
FReply SGitWorkspace::DropSelectedStash()
{
    if (!IsIdle() || !StashInspection.IsFresh() || !StashInspection.DropBlocker.IsEmpty()) return FReply::Handled();
#if PLATFORM_MAC
    const auto Reviewed = StashInspection;
    const FString Prompt = TEXT("Remove this selected stash from the local stash list?\n\n") + Reviewed.Text +
        TEXT("\nDrop does not apply or discard working changes, change staging, or release locks. Local Git recovery references will be retained, but they are not an LFS or remote backup. Close other Git clients before continuing.");
    if (FMessageDialog::Open(EAppMsgType::YesNo, StashText(Prompt)) != EAppReturnType::Yes) return FReply::Handled();
    {
        TGuardValue<bool> Busy(bReloading, true);
        FScopedSlowTask Task(1.f, StashText(TEXT("Preserving recovery and dropping the reviewed stash…"))); Task.MakeDialog(false); Task.EnterProgressFrame(1.f); Task.ForceRefresh();
        GitWorkspaceSession::FEditorWriteScope Access; FString Error;
        if (!Access.Acquire(Reviewed.Root, Error)) { Feedback = Error; if (StashReport) StashReport->SetText(StashText(Error)); return FReply::Handled(); }
        const auto Result = Async(EAsyncExecution::ThreadPool, [&] { return Repository->DropStash(Reviewed, Access.Lease()); }).Get();
        Feedback = Result.Ok() ? Result.Text() : Result.Error;
        StashReview.bValid = false; StashInspection.bValid = false;
        if (StashReport) StashReport->SetText(StashText(Feedback));
    }
    auto Repo = Repository; const FString ResultMessage = Feedback;
    Start([Repo, ResultMessage] { FGitWorkspaceTaskResult R; R.bStashes = true; R.Stashes = Repo->ListStashes(); R.Snapshot = Repo->Refresh(); R.Message = ResultMessage; return R; });
#endif
    return FReply::Handled();
}
