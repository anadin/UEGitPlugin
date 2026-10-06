// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#include "SGitWorkspace.h"
#include "GitWorkspaceEditorPull.h"
#include "GitWorkspacePullReview.h"
#include "Async/Async.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/SWindow.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Text/STextBlock.h"
#include "Misc/MessageDialog.h"
#include "Misc/ScopedSlowTask.h"
#if PLATFORM_MAC
#include "GitWorkspaceSession.h"
#endif
namespace { FText DiscardUI(const FString& Text) { return FText::FromString(Text); } }
TArray<FString> SGitWorkspace::SelectedDiscardPaths() const
{
    TArray<FString> Paths;
    if (!List) return Paths;
    for (const auto& Row : List->GetSelectedItems())
    {
        const auto& File = Row->File;
        // A destructive selection never silently skips an unsupported row.
        if (!Row->Group.IsEmpty() || Row->bStaged || File.bUntracked || File.bConflict || File.bSubmodule || File.Working != 'M' || !File.OriginalPath.IsEmpty()) return {};
        Paths.AddUnique(File.Path);
    }
    Paths.Sort(); return Paths;
}
bool SGitWorkspace::CanDiscardSelected() const
{ return IsIdle() && Snapshot.bValid && !Snapshot.bOperationInProgress && !Snapshot.HasConflicts() && !SelectedDiscardPaths().IsEmpty(); }
bool SGitWorkspace::CanRunDiscard() const
{ return IsIdle() && DiscardReview.IsFresh() && DiscardPackageBlocker.IsEmpty() && !HasDirtyPackages(); }
FReply SGitWorkspace::ShowDiscardReview()
{
    if (!CanDiscardSelected()) return FReply::Handled();
    if (auto Old = DiscardWindow.Pin()) Old->RequestDestroyWindow();
    DiscardReview = GitWorkspace::FDiscardReview(); DiscardReview.Paths = SelectedDiscardPaths(); DiscardPackageBlocker.Empty();
    const auto Window = SNew(SWindow).Title(DiscardUI(TEXT("Review discard working edits"))).ClientSize(FVector2D(840, 640)).SupportsMinimize(false);
    DiscardWindow = Window; TWeakPtr<SGitWorkspace> Weak = SharedThis(this); TWeakPtr<SWindow> WeakWindow = Window;
    Window->SetContent(SNew(SBorder).Padding(12)[SNew(SVerticalBox)
        + SVerticalBox::Slot().FillHeight(1)
        [SAssignNew(DiscardReport, SMultiLineEditableTextBox).IsReadOnly(true).AutoWrapText(true)]
        + SVerticalBox::Slot().AutoHeight().Padding(0, 10)
        [SNew(STextBlock).AutoWrapText(true).Text_Lambda([Weak]
        {
            auto P = Weak.Pin(); if (!P) return FText();
            if (!P->IsIdle()) return DiscardUI(TEXT("Reviewing saved working edits. No file is replaced by a review."));
            if (P->HasDirtyPackages()) return DiscardUI(TEXT("Save or resolve unsaved packages, then Refresh review. Discard never saves them automatically."));
            if (P->DiscardReview.bValid && !P->DiscardReview.IsFresh()) return DiscardUI(TEXT("Review expired. Refresh before discarding."));
            return DiscardUI(TEXT("Discard replaces the listed working files. Staged versions stay staged; locks stay held. A verified recovery backup is kept."));
        })]
        + SVerticalBox::Slot().AutoHeight()
        [SNew(SHorizontalBox)
            + SHorizontalBox::Slot().AutoWidth()
            [SNew(SButton).Text(DiscardUI(TEXT("Refresh review"))).IsEnabled_Lambda([Weak] { auto P = Weak.Pin(); return P && P->IsIdle(); })
                .OnClicked_Lambda([Weak] { if (auto P = Weak.Pin()) return P->RefreshDiscardReview(); return FReply::Handled(); })]
            + SHorizontalBox::Slot().AutoWidth().Padding(10, 0)
            [SNew(SButton).Text(DiscardUI(TEXT("Discard working edits…"))).IsEnabled_Lambda([Weak] { auto P = Weak.Pin(); return P && P->CanRunDiscard(); })
                .OnClicked_Lambda([Weak] { if (auto P = Weak.Pin()) return P->RunDiscard(); return FReply::Handled(); })]
            + SHorizontalBox::Slot().AutoWidth()
            [SNew(SButton).Text(DiscardUI(TEXT("Close"))).OnClicked_Lambda([WeakWindow] { if (auto W = WeakWindow.Pin()) W->RequestDestroyWindow(); return FReply::Handled(); })]
        ]
    ]);
    FSlateApplication::Get().AddWindow(Window); return RefreshDiscardReview();
}
FReply SGitWorkspace::RefreshDiscardReview()
{
    if (!IsIdle() || DiscardReview.Paths.IsEmpty()) return FReply::Handled();
    const auto Paths = DiscardReview.Paths; DiscardReview.bValid = false; DiscardPackageBlocker.Empty();
    if (DiscardReport) DiscardReport->SetText(DiscardUI(TEXT("Reviewing the exact working/index versions and preservation checks…")));
    auto Repo = Repository; const auto Window = DiscardWindow;
    Start([Repo, Paths, Window]
    {
        FGitWorkspaceTaskResult R; R.bDiscardReview = true; R.DiscardReviewWindow = Window; R.DiscardReview = Repo->ReviewDiscard(Paths);
        R.Snapshot = Repo->Refresh(); R.Message = R.DiscardReview.IsFresh() ? TEXT("Discard review ready. No file replaced or backup entry created.") : R.DiscardReview.Error; return R;
    });
    return FReply::Handled();
}
FReply SGitWorkspace::RunDiscard()
{
    if (!CanRunDiscard()) return FReply::Handled();
    const auto Reviewed = DiscardReview;
    TArray<GitWorkspace::FIncomingPackage> Packages;
    const FString Blocker = GitWorkspace::ReviewPackageChanges(Reviewed.Capture.Local.Root, Reviewed.Changes, Packages);
    if (!Blocker.IsEmpty()) { DiscardPackageBlocker = Blocker; Feedback = Blocker; return FReply::Handled(); }
    const FString Prompt = Reviewed.Text + TEXT("\nReplace these saved working edits with their staged/index versions after preserving recovery?");
    if (FMessageDialog::Open(EAppMsgType::YesNo, EAppReturnType::No, DiscardUI(Prompt)) != EAppReturnType::Yes)
    { Feedback = TEXT("Discard cancelled. Working files, staging and locks retained."); if (DiscardReport) DiscardReport->SetText(DiscardUI(Feedback + TEXT("\n\n") + Reviewed.Text)); return FReply::Handled(); }
    {
        TGuardValue<bool> Busy(bReloading, true);
        FScopedSlowTask Task(1.f, DiscardUI(TEXT("Preserving recovery and discarding selected working edits…")));
        Task.MakeDialog(false); Task.EnterProgressFrame(1.f); Task.ForceRefresh();
#if PLATFORM_MAC
        GitWorkspaceSession::FEditorWriteScope Access; FString Error;
        if (!Access.Acquire(Reviewed.Capture.Local.Root, Error)) { Feedback = Error; return FReply::Handled(); }
        const auto Result = GitWorkspace::DiscardAndReload(*Repository, Reviewed, Access.Lease());
        Feedback = Result.Message;
        if (Result.bRecoveryRequired)
        {
            FString Root, GitDir; GitWorkspaceSession::FindRepository(Reviewed.Capture.Local.Root, Root, GitDir);
            GitWorkspaceSession::StopForRecovery(Result.Message + TEXT("\n\nThe editor will close without saving. Recovery report: ") + GitWorkspaceSession::RecoveryFile(GitDir));
            return FReply::Handled();
        }
#else
        Feedback = TEXT("Guarded discard is currently available on Mac only.");
#endif
        DiscardReview.bValid = false;
        if (DiscardReport) DiscardReport->SetText(DiscardUI(Feedback));
    }
    auto Repo = Repository; const FString Report = Feedback;
    Start([Repo, Report] { FGitWorkspaceTaskResult R; R.Snapshot = Repo->Refresh(); R.Message = Report; return R; });
    return FReply::Handled();
}
