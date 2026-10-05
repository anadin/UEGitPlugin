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
#include "Misc/App.h"
#include "Misc/ScopedSlowTask.h"
#if PLATFORM_MAC
#include "GitWorkspaceSession.h"
#endif
namespace { FText StashText(const FString& S) { return FText::FromString(S); } }
FReply SGitWorkspace::ShowStashes()
{
    if (!IsIdle()) return FReply::Handled();
    if (auto Old = StashWindow.Pin()) Old->RequestDestroyWindow();
    StashSelectedPaths.Empty(); bIncludeUntrackedStash = false;
    StashFileFilter.Empty(); bStashContentOnly = bContentOnly;
    if (List) for (const auto& Row : List->GetSelectedItems())
        if (Row->Group.IsEmpty() && CanSelectStashFile(Row->File)) StashSelectedPaths.AddUnique(Row->File.Path);
    StashSelectedPaths.Sort();
    StashReview = GitWorkspace::FStashReview(); StashInspection = GitWorkspace::FStashInspection();
    StashReview.bCreate = true; StashReview.bSelected = true;
    const auto Window = SNew(SWindow).Title(StashText(TEXT("Git stashes"))).ClientSize(FVector2D(1150, 760)).SupportsMinimize(false);
    StashWindow = Window;
    TWeakPtr<SGitWorkspace> Weak = SharedThis(this);
    auto Idle = [Weak] { auto P = Weak.Pin(); return P && P->IsIdle(); };
    Window->SetContent(SNew(SBorder).Padding(12)
    [SNew(SHorizontalBox)
        + SHorizontalBox::Slot().FillWidth(0.43).Padding(0, 0, 14, 0)
        [MakeStashFilePicker()]
        + SHorizontalBox::Slot().FillWidth(0.57)
        [SNew(SVerticalBox)
            + SVerticalBox::Slot().AutoHeight()
            [SNew(SHorizontalBox)
                + SHorizontalBox::Slot().FillWidth(1)[SNew(STextBlock).Text(StashText(TEXT("Saved stashes")))]
                + SHorizontalBox::Slot().AutoWidth()
                [SNew(SButton).Text(StashText(TEXT("Refresh files and stashes"))).IsEnabled_Lambda(Idle).OnClicked_Lambda([Weak] { if (auto P = Weak.Pin()) return P->RefreshStashes(); return FReply::Handled(); })]
            ]
            + SVerticalBox::Slot().FillHeight(0.25).Padding(0, 8)
            [SNew(SScrollBox) + SScrollBox::Slot()[SAssignNew(StashRows, SVerticalBox)]]
            + SVerticalBox::Slot().AutoHeight().Padding(0, 8)
            [SNew(SCheckBox).IsChecked_Lambda([Weak] { auto P = Weak.Pin(); return P && P->bRestoreStashIndex ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
                .IsEnabled_Lambda(Idle).OnCheckStateChanged_Lambda([Weak](ECheckBoxState State)
                {
                    if (auto P = Weak.Pin()) { P->bRestoreStashIndex = State == ECheckBoxState::Checked; P->StashReview.bValid = false; if (P->StashReport) P->StashReport->SetText(StashText(P->StashInspection.Text + TEXT("\nStaging option changed. Select the stash again before applying."))); }
                })[SNew(STextBlock).Text(StashText(TEXT("Restore staging when applying")))]]
            + SVerticalBox::Slot().AutoHeight().Padding(0, 4)[SNew(STextBlock).Text(StashText(TEXT("Review")))]
            + SVerticalBox::Slot().FillHeight(0.75)
            [SAssignNew(StashReport, SMultiLineEditableTextBox).IsReadOnly(true).AutoWrapText(true).Text(StashText(TEXT("Tick files on the left and review them to create a stash, or select a saved stash above to inspect and apply it.")))]
            + SVerticalBox::Slot().AutoHeight().Padding(0, 8, 0, 0)
            [SNew(STextBlock).AutoWrapText(true).Text_Lambda([Weak] { auto P = Weak.Pin(); return P ? P->StashActionHint() : FText(); })]
            + SVerticalBox::Slot().AutoHeight().Padding(0, 10, 0, 0)
            [SNew(SHorizontalBox)
                + SHorizontalBox::Slot().AutoWidth()
                [SNew(SButton).Text_Lambda([Weak] { auto P = Weak.Pin(); return P ? P->StashActionText() : FText(); })
                    .IsEnabled_Lambda([Weak] { auto P = Weak.Pin(); return P && P->CanRunStashAction(); })
                    .OnClicked_Lambda([Weak] { if (auto P = Weak.Pin()) return P->RunStashAction(); return FReply::Handled(); })]
                + SHorizontalBox::Slot().AutoWidth().Padding(12, 0)
                [SNew(SButton).Text(StashText(TEXT("Apply and delete…")))
                    .Visibility_Lambda([Weak] { auto P = Weak.Pin(); return P && !P->StashReview.bCreate ? EVisibility::Visible : EVisibility::Collapsed; })
                    .ToolTipText(StashText(TEXT("Restore this stash, then remove it only after files and assets are verified. Locks stay held.")))
                    .IsEnabled_Lambda([Weak] { auto P = Weak.Pin(); return P && P->CanRunStashAction(true); })
                    .OnClicked_Lambda([Weak] { if (auto P = Weak.Pin()) return P->RunStashAction(true); return FReply::Handled(); })]
                + SHorizontalBox::Slot().AutoWidth()
                [SNew(SButton).Text(StashText(TEXT("Drop…")))
                    .ToolTipText(StashText(TEXT("Remove the selected stash without applying it. Working files, staging and locks stay unchanged.")))
                    .IsEnabled_Lambda([Weak] { auto P = Weak.Pin(); return P && P->IsIdle() && P->StashInspection.IsFresh() && P->StashInspection.DropBlocker.IsEmpty(); })
                    .OnClicked_Lambda([Weak] { if (auto P = Weak.Pin()) return P->DropSelectedStash(); return FReply::Handled(); })]
            ]
        ]
    ]);
    RebuildStashFiles();
    FSlateApplication::Get().AddWindow(Window);
    return RefreshStashes();
}
FReply SGitWorkspace::RefreshStashes()
{
    if (!IsIdle()) return FReply::Handled();
    StashReview.bValid = false; StashInspection.bValid = false; auto Repo = Repository;
    if (StashReport) StashReport->SetText(StashText(TEXT("Select a stash to inspect it, or review saved changes to create one.")));
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
FReply SGitWorkspace::PreviewStash(const FString& Oid, const FString& Selector, bool bSelected, bool bConfirmAction, bool bDeleteAfterApply)
{
    if (!IsIdle()) return FReply::Handled();
    StashReview.bValid = false; StashInspection.bValid = false;
    if (StashReport) StashReport->SetText(StashText(TEXT("Reviewing stash and local work…")));
    auto Repo = Repository; const bool bIndex = bRestoreStashIndex, bUntracked = bIncludeUntrackedStash;
    const auto Paths = StashSelectedPaths;
    const auto ConfirmationWindow = bConfirmAction ? StashWindow : TWeakPtr<SWindow>();
    Start([Repo, Oid, Selector, bIndex, bSelected, bUntracked, Paths, ConfirmationWindow, bDeleteAfterApply]
    {
        FGitWorkspaceTaskResult R; R.bStashReview = true;
        R.StashConfirmationWindow = ConfirmationWindow;
        R.bDeleteStashAfterApply = bDeleteAfterApply;
        if (!Oid.IsEmpty()) R.StashInspection = Repo->InspectStash(Oid, Selector);
        R.StashReview = bSelected ? Repo->ReviewSelectedStash(Paths, bUntracked) : Repo->ReviewStash(Oid, bIndex, bUntracked); R.Snapshot = Repo->Refresh();
        R.Message = R.StashReview.bValid || R.StashInspection.bValid ? TEXT("Stash preview ready. Review the saved paths and action eligibility.") : R.StashReview.Error;
        return R;
    });
    return FReply::Handled();
}
bool SGitWorkspace::CanRunStashAction(bool bDeleteAfterApply) const
{
    if (!IsIdle()) return false;
    if (bDeleteAfterApply && (StashReview.bCreate || !StashInspection.DropBlocker.IsEmpty())) return false;
    if (StashReview.bCreate)
        return StashReview.bSelected ? !StashSelectedPaths.IsEmpty() : StashReview.bValid || !StashReview.Oid.IsEmpty();
    return !StashInspection.Entry.Oid.IsEmpty();
}
FText SGitWorkspace::StashActionText() const
{
    if (StashReview.bCreate)
        return StashText(TEXT("Create stash…"));
    return StashText(TEXT("Apply and keep…"));
}
FText SGitWorkspace::StashActionHint() const
{
    if (!IsIdle()) return StashText(TEXT("Stash operation running. Please wait for the result."));
    if (StashReview.bCreate)
    {
        if (StashReview.bSelected && StashSelectedPaths.IsEmpty()) return StashText(TEXT("Tick files on the left. New files require Include untracked files."));
        if (!StashName || StashName->GetText().ToString().TrimStartAndEnd().IsEmpty()) return StashText(TEXT("Enter a stash name, then click Create stash."));
        return StashText(TEXT("Create stash checks the files and opens a Yes/No confirmation. Files change only after Yes."));
    }
    if (StashInspection.Entry.Oid.IsEmpty()) return StashText(TEXT("Select a saved stash above to restore its files."));
    if (StashReview.IsFresh() && StashInspection.IsFresh()) return StashText(TEXT("Apply and keep retains the stash. Apply and delete removes it after a verified restore. Drop removes it without applying. Each action asks for confirmation; locks stay held."));
    if (StashReview.bValid || (StashInspection.bValid && !StashInspection.IsFresh()))
        return StashText(TEXT("The chosen Apply action refreshes the review, then asks for confirmation. No files change before Yes."));
    return StashText(TEXT("Apply is not ready. Read the blocker above, resolve it, then choose an Apply action to check again."));
}
FReply SGitWorkspace::RunStashAction(bool bDeleteAfterApply)
{
    if (!CanRunStashAction(bDeleteAfterApply)) return FReply::Handled();
    if (StashReview.bCreate && (!StashName || StashName->GetText().ToString().TrimStartAndEnd().IsEmpty()))
    {
        Feedback = TEXT("Enter a name for the new stash.");
        if (StashReport) StashReport->SetText(StashText(Feedback));
        if (StashName && !FApp::IsUnattended() && !IsRunningCommandlet()) FSlateApplication::Get().SetKeyboardFocus(StashName);
        return FReply::Handled();
    }
    // The chosen action continues to confirmation after preparation. Explicit
    // Preview actions still only publish a review.
    if (!StashReview.IsFresh() || (!StashReview.bCreate && !StashInspection.IsFresh()))
        return StashReview.bCreate ? PreviewStash(FString(), FString(), StashReview.bSelected, true) :
            PreviewStash(StashInspection.Entry.Oid, StashInspection.Entry.Selector, false, true, bDeleteAfterApply);
    return RunStash(bDeleteAfterApply);
}
FReply SGitWorkspace::RunStash(bool bDeleteAfterApply)
{
    if (!IsIdle()) return FReply::Handled();
    if (!StashReview.IsFresh() || (!StashReview.bCreate && !StashInspection.IsFresh())) return RunStashAction(bDeleteAfterApply);
#if PLATFORM_MAC
    const auto Reviewed = StashReview; const FString Name = StashName ? StashName->GetText().ToString() : FString();
    const auto Inspected = StashInspection;
    TArray<GitWorkspace::FIncomingPackage> Packages;
    FString Error = GitWorkspace::ReviewPackageChanges(Reviewed.Local.Root, Reviewed.Changes, Packages, Reviewed.bCreate);
    if (Reviewed.bCreate && Name.TrimStartAndEnd().IsEmpty()) Error = TEXT("Enter a name for the new stash.");
    if (bDeleteAfterApply && (Reviewed.bCreate || !Inspected.DropBlocker.IsEmpty()))
        Error = Inspected.DropBlocker.IsEmpty() ? TEXT("Select a saved stash before Apply and delete.") : Inspected.DropBlocker;
    if (!Error.IsEmpty()) { Feedback = Error; if (StashReport) StashReport->SetText(StashText(Error)); return FReply::Handled(); }
    const FString Action = bDeleteAfterApply ? TEXT("Apply and delete: ") : TEXT("Apply and keep: ");
    const FString Retention = bDeleteAfterApply ?
        TEXT("\nOnly this selected stash entry will be deleted, after files, staging and asset reloads are verified. A failed restore keeps the stash. Local Git recovery references are retained. Close other Git clients before continuing.") :
        TEXT("\nThe stash will be kept.");
    const FString Prompt = (Reviewed.bCreate ? TEXT("Create stash: ") + Name : Action + Inspected.Entry.Selector + TEXT("  ") + Inspected.Entry.Label + TEXT("\n") + Reviewed.Oid) + TEXT("\n\n") + Reviewed.Text +
        TEXT("\nExisting supported assets will reload. New loaded assets must unload before removal; referencing assets can block this. Undo/selection may reset. If a file update or reload becomes uncertain, the editor will close without saving and preserve recovery instructions.") + Retention;
    if (FMessageDialog::Open(EAppMsgType::YesNo, EAppReturnType::No, StashText(Prompt)) != EAppReturnType::Yes)
    {
        Feedback = Reviewed.bCreate ? TEXT("Stash creation cancelled. No files changed.") :
            (bDeleteAfterApply ? TEXT("Apply and delete cancelled. No files changed; stash kept.") : TEXT("Apply and keep cancelled. No files changed; stash kept."));
        if (StashReport) StashReport->SetText(StashText(Feedback));
        return FReply::Handled();
    }
    {
        TGuardValue<bool> Busy(bReloading, true);
        FScopedSlowTask Task(1.f, StashText(TEXT("Updating stash and refreshing assets…"))); Task.MakeDialog(false); Task.EnterProgressFrame(1.f); Task.ForceRefresh();
        GitWorkspaceSession::FEditorWriteScope Access;
        if (!Access.Acquire(Reviewed.Local.Root, Error)) { Feedback = Error; if (StashReport) StashReport->SetText(StashText(Error)); return FReply::Handled(); }
        const auto Result = bDeleteAfterApply ? GitWorkspace::ApplyStashAndDelete(*Repository, Reviewed, Inspected, Access.Lease()) :
            GitWorkspace::StashAndReload(*Repository, Reviewed, Name, Access.Lease());
        if (Result.bRecoveryRequired)
        {
            FString Root, GitDir; GitWorkspaceSession::FindRepository(Reviewed.Local.Root, Root, GitDir);
            GitWorkspaceSession::StopForRecovery(Result.Message + TEXT("\n\nThe editor will close without saving. Recovery report: ") + GitWorkspaceSession::RecoveryFile(GitDir));
        }
        Feedback = Result.Message; StashReview.bValid = false; StashInspection.bValid = false;
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
