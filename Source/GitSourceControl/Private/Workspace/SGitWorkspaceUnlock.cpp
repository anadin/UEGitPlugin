// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#include "SGitWorkspace.h"
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

namespace { FText UnlockText(const FString& Value) { return FText::FromString(Value); } }
bool SGitWorkspace::CanUnlockSelected() const
{
    if (!IsIdle() || !Selection || !Selection->Group.IsEmpty() || !List || List->GetSelectedItems().Num() != 1) return false;
    const auto* Lock = Locks.Locks.Find(Selection->File.Path);
    // A stale reservation owned by us can open a fresh, read-only review.
    // Absence and foreign ownership do not offer an unlock action.
    return Lock && Lock->bOurs;
}
FText SGitWorkspace::UnlockHint() const
{
    if (!Selection || !Selection->Group.IsEmpty() || !List || List->GetSelectedItems().Num() != 1)
        return UnlockText(TEXT("Select one asset with a lock owned by you."));
    const auto State = Locks.State(Selection->File.Path, Selection->File.bLockable);
    if (State == GitWorkspace::ELockState::Unlocked) return UnlockText(TEXT("This asset is verified unlocked. There is no lock to release."));
    if (State == GitWorkspace::ELockState::Theirs) return UnlockText(TEXT("Another user owns this lock. You cannot release it here."));
    if (!CanUnlockSelected()) return UnlockText(TEXT("No lock owned by you is known for this asset. Verify locks to refresh ownership."));
    return UnlockText(TEXT("Review ownership, saved changes, stashes and publication before releasing this asset's lock. Stale ownership is verified again."));
}
FReply SGitWorkspace::ShowUnlockReview()
{
    if (!CanUnlockSelected()) return FReply::Handled();
    if (auto Old = UnlockWindow.Pin()) Old->RequestDestroyWindow();
    UnlockReview = GitWorkspace::FUnlockReview(); UnlockReview.Path = Selection->File.Path; UnlockReview.Locks.Remote = LockRemote;
    const auto Window = SNew(SWindow).Title(UnlockText(TEXT("Review unlock"))).ClientSize(FVector2D(840, 640)).SupportsMinimize(false);
    UnlockWindow = Window; TWeakPtr<SGitWorkspace> Weak = SharedThis(this); TWeakPtr<SWindow> WeakWindow = Window;
    Window->SetContent(SNew(SBorder).Padding(12)
    [SNew(SVerticalBox)
        + SVerticalBox::Slot().FillHeight(1)
        [SAssignNew(UnlockReport, SMultiLineEditableTextBox).IsReadOnly(true).AutoWrapText(true)]
        + SVerticalBox::Slot().AutoHeight().Padding(0, 10)
        [SNew(STextBlock).AutoWrapText(true).Text_Lambda([Weak]
        {
            auto P = Weak.Pin(); if (!P) return FText();
            if (!P->IsIdle()) return UnlockText(TEXT("Checking handoff eligibility. No lock is released by a review."));
            if (P->HasDirtyPackages()) return UnlockText(TEXT("Unsaved editor assets block release. Save them, then refresh this review."));
            if (P->UnlockReview.bReady && !P->UnlockReview.IsFresh()) return UnlockText(TEXT("Ownership verification expired. Refresh the review before unlocking."));
            return UnlockText(TEXT("Refresh review checks again. Unlock asset asks for your handoff confirmation and repeats the safety checks."));
        })]
        + SVerticalBox::Slot().AutoHeight()
        [SNew(SHorizontalBox)
            + SHorizontalBox::Slot().AutoWidth()
            [SNew(SButton).Text(UnlockText(TEXT("Refresh review"))).IsEnabled_Lambda([Weak] { auto P = Weak.Pin(); return P && P->IsIdle(); })
                .OnClicked_Lambda([Weak] { if (auto P = Weak.Pin()) return P->RefreshUnlockReview(); return FReply::Handled(); })]
            + SHorizontalBox::Slot().AutoWidth().Padding(10, 0)
            [SNew(SButton).Text(UnlockText(TEXT("Unlock asset…"))).IsEnabled_Lambda([Weak] { auto P = Weak.Pin(); return P && P->IsIdle() && P->UnlockReview.IsFresh() && !P->HasDirtyPackages(); })
                .OnClicked_Lambda([Weak] { if (auto P = Weak.Pin()) return P->RunUnlock(); return FReply::Handled(); })]
            + SHorizontalBox::Slot().AutoWidth()
            [SNew(SButton).Text(UnlockText(TEXT("Close"))).OnClicked_Lambda([WeakWindow] { if (auto W = WeakWindow.Pin()) W->RequestDestroyWindow(); return FReply::Handled(); })]
        ]
    ]);
    FSlateApplication::Get().AddWindow(Window);
    return RefreshUnlockReview();
}
FReply SGitWorkspace::RefreshUnlockReview()
{
    if (!IsIdle() || UnlockReview.Path.IsEmpty()) return FReply::Handled();
    UnlockReview.bReady = false;
    if (UnlockReport) UnlockReport->SetText(UnlockText(TEXT("Checking server ownership, local acquisition, saved changes, stashes and the live upstream…")));
    auto Repo = Repository; const FString Path = UnlockReview.Path, RemoteName = UnlockReview.Locks.Remote;
    const auto Window = UnlockWindow;
    Start([Repo, Path, RemoteName, Window]
    {
        FGitWorkspaceTaskResult R; R.bUnlockReview = true; R.UnlockReviewWindow = Window;
        R.UnlockReview = Repo->ReviewUnlock(RemoteName, Path); R.bLocks = true; R.Locks = R.UnlockReview.Locks;
        R.Snapshot = Repo->Refresh(); R.Message = R.UnlockReview.IsFresh() ? TEXT("Unlock review ready. No locks released.") : R.UnlockReview.Error;
        return R;
    });
    return FReply::Handled();
}
FReply SGitWorkspace::RunUnlock()
{
    if (!IsIdle() || !UnlockReview.IsFresh()) return FReply::Handled();
    if (HasDirtyPackages()) { Feedback = TEXT("Save dirty assets and refresh the unlock review."); return FReply::Handled(); }
    const auto Reviewed = UnlockReview;
    const FString Prompt = Reviewed.Text() + TEXT("\n\nConfirm that your team handoff is complete and no other clone or worktree still needs this reservation. Release this one lock?");
    if (FMessageDialog::Open(EAppMsgType::YesNo, EAppReturnType::No, UnlockText(Prompt)) != EAppReturnType::Yes)
    {
        Feedback = TEXT("Unlock cancelled. Lock retained.");
        if (UnlockReport) UnlockReport->SetText(UnlockText(Feedback + TEXT("\n\n") + Reviewed.Text()));
        return FReply::Handled();
    }
    // Recheck after the modal; block saves/autosave while the network mutation
    // runs. No editor ticks are pumped while the worker owns this operation.
    if (HasDirtyPackages()) { Feedback = TEXT("Editor assets changed. Save and review again; lock retained."); return FReply::Handled(); }
    {
        TGuardValue<bool> Busy(bReloading, true);
        FScopedSlowTask Task(1.f, UnlockText(TEXT("Rechecking handoff and releasing the selected lock…")));
        Task.MakeDialog(false); Task.EnterProgressFrame(1.f); Task.ForceRefresh();
#if PLATFORM_MAC
        GitWorkspaceSession::FEditorWriteScope Access; FString Error;
        if (!Access.Acquire(Reviewed.Locks.Root, Error))
        { Feedback = Error; if (UnlockReport) UnlockReport->SetText(UnlockText(Error)); return FReply::Handled(); }
#endif
        const auto Result = Async(EAsyncExecution::ThreadPool, [&]
        { return Repository->ChangeLock(Reviewed.Locks, Reviewed.Path, true, true, Reviewed.Head); }).Get();
        Feedback = Result.Ok() ? TEXT("Selected lock released and server state verified. Other locks retained; no files changed or pushed.") : Result.Error;
        UnlockReview.bReady = false; Locks.bVerified = false;
        if (UnlockReport) UnlockReport->SetText(UnlockText(Feedback));
    }
    auto Repo = Repository; const FString RemoteName = Reviewed.Locks.Remote, ResultMessage = Feedback;
    Start([Repo, RemoteName, ResultMessage]
    {
        FGitWorkspaceTaskResult R; R.bLocks = true; R.Locks = Repo->VerifyLocks(RemoteName); R.Snapshot = Repo->Refresh(); R.Message = ResultMessage; return R;
    });
    return FReply::Handled();
}
