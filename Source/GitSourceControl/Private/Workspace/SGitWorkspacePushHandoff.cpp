// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#include "SGitWorkspace.h"
#include "Async/Async.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/SWindow.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Text/STextBlock.h"
#include "Misc/MessageDialog.h"
#include "Misc/ScopedSlowTask.h"
#if PLATFORM_MAC
#include "GitWorkspaceSession.h"
#endif

namespace
{
FText HandoffText(const FString& Value) { return FText::FromString(Value); }
FString HandoffLabel(FString Value)
{ return Value.Replace(TEXT("\\"), TEXT("\\\\")).Replace(TEXT("\r"), TEXT("\\r")).Replace(TEXT("\n"), TEXT("\\n")).Replace(TEXT("\t"), TEXT("\\t")); }
}
FReply SGitWorkspace::ShowPushHandoff()
{
    if (!IsIdle() || !Remote.IsFresh() || Remote.Ahead <= 0 || Remote.Behind) return FReply::Handled();
    if (auto Old = HandoffWindow.Pin()) Old->RequestDestroyWindow();
    HandoffReview = GitWorkspace::FPushHandoffReview(); HandoffChecked.Empty(); HandoffRetryIds.Empty(); bHasHandoffResult = false;
    const auto Window = SNew(SWindow).Title(HandoffText(TEXT("Push and unlock selected assets"))).ClientSize(FVector2D(1000, 760)).SupportsMinimize(false);
    HandoffWindow = Window; TWeakPtr<SGitWorkspace> Weak = SharedThis(this); TWeakPtr<SWindow> WeakWindow = Window;
    Window->SetContent(SNew(SBorder).Padding(12)
    [SNew(SVerticalBox)
        + SVerticalBox::Slot().FillHeight(0.6f)
        [SAssignNew(HandoffReport, SMultiLineEditableTextBox).IsReadOnly(true).AutoWrapText(true)]
        + SVerticalBox::Slot().AutoHeight().Padding(0, 10)
        [SNew(STextBlock).Text_Lambda([Weak] { auto P = Weak.Pin(); return HandoffText(P && !P->HandoffRetryIds.IsEmpty() ? TEXT("Choose remaining original locks to release. This retry sends no Push.") : TEXT("Choose locks to release. Unchecked locks stay held; every outgoing commit is pushed.")); }).AutoWrapText(true)]
        + SVerticalBox::Slot().FillHeight(0.4f)
        [SNew(SScrollBox) + SScrollBox::Slot()[SAssignNew(HandoffRows, SVerticalBox)]]
        + SVerticalBox::Slot().AutoHeight().Padding(0, 10)
        [SNew(STextBlock).AutoWrapText(true).Text_Lambda([Weak]
        {
            auto P = Weak.Pin(); if (!P) return FText();
            if (!P->IsIdle()) return HandoffText(TEXT("Preparing review. No Push or lock release is performed by a review."));
            if (P->HasDirtyPackages()) return HandoffText(TEXT("Save unsaved editor assets before handoff. Push alone can retain their locks."));
            if (!P->HandoffReview.IsFresh()) return HandoffText(TEXT("Refresh the review before acting. A new commit or expired verification needs another review."));
            return HandoffText(TEXT("Tick eligible locks, then confirm the team handoff. Failed publication releases nothing."));
        })]
        + SVerticalBox::Slot().AutoHeight()
        [SNew(SHorizontalBox)
            + SHorizontalBox::Slot().AutoWidth()
            [SNew(SButton).Text_Lambda([Weak] { auto P = Weak.Pin(); return HandoffText(P && !P->HandoffRetryIds.IsEmpty() ? TEXT("Review remaining locks") : TEXT("Refresh review")); })
                .IsEnabled_Lambda([Weak] { auto P = Weak.Pin(); return P && P->IsIdle(); })
                .OnClicked_Lambda([Weak] { if (auto P = Weak.Pin()) return P->RefreshPushHandoff(); return FReply::Handled(); })]
            + SHorizontalBox::Slot().AutoWidth().Padding(10, 0)
            [SNew(SButton).Text_Lambda([Weak] { auto P = Weak.Pin(); return HandoffText(P && !P->HandoffRetryIds.IsEmpty() ? TEXT("Unlock remaining…") : TEXT("Push and unlock checked…")); })
                .IsEnabled_Lambda([Weak] { auto P = Weak.Pin(); return P && P->CanRunPushHandoff(); })
                .OnClicked_Lambda([Weak] { if (auto P = Weak.Pin()) return P->RunPushHandoff(); return FReply::Handled(); })]
            + SHorizontalBox::Slot().AutoWidth()
            [SNew(SButton).Text(HandoffText(TEXT("Close"))).OnClicked_Lambda([WeakWindow] { if (auto W = WeakWindow.Pin()) W->RequestDestroyWindow(); return FReply::Handled(); })]
        ]
    ]);
    FSlateApplication::Get().AddWindow(Window); return RefreshPushHandoff();
}
FReply SGitWorkspace::RefreshPushHandoff()
{
    if (!IsIdle()) return FReply::Handled();
    HandoffReview.bValid = false;
    if (HandoffReport) HandoffReport->SetText(HandoffText(TEXT("Fetching the destination and checking owned locks, saved/stashed work and acquisition records…")));
    auto Repo = Repository; const auto Window = HandoffWindow;
    const bool Retry = !HandoffRetryIds.IsEmpty();
    const FString Head = HandoffRetryHead, Context = HandoffRetryContext, LockContext = HandoffRetryLockContext, DisplayRemote = LockRemote;
    Start([Repo, Window, Retry, Head, Context, LockContext, DisplayRemote]
    {
        FGitWorkspaceTaskResult R; R.bHandoffReview = true; R.HandoffReviewWindow = Window; R.bRemote = true;
        R.Remote = Repo->Fetch(); R.HandoffReview = Repo->ReviewPushHandoff(R.Remote);
        if (Retry && (!R.HandoffReview.IsRetry() || R.Remote.Head != Head || R.Remote.Context != Context || R.HandoffReview.Locks.Context != LockContext))
        { R.HandoffReview.bValid = false; R.HandoffReview.Error = TEXT("The published commit or acquisition context changed. Review remaining reservations individually; this retry sends no Push."); }
        if (!Retry && R.HandoffReview.IsRetry())
        { R.HandoffReview.bValid = false; R.HandoffReview.Error = TEXT("There are no outgoing commits. Use Unlock for a separate handoff."); }
        R.bLocks = DisplayRemote == R.HandoffReview.Locks.Remote; R.Locks = R.HandoffReview.Locks; R.Snapshot = Repo->Refresh();
        R.Message = R.HandoffReview.IsFresh() ? TEXT("Handoff review ready. No Push or release sent.") : R.HandoffReview.Error; return R;
    });
    return FReply::Handled();
}
void SGitWorkspace::UpdateHandoffReport()
{
    if (!HandoffReport) return;
    auto Paths = HandoffChecked.Array(); Paths.Sort();
    FString Text = bHasHandoffResult ? HandoffResult.Text() + TEXT("\n\n") : FString();
    Text += HandoffReview.Text(Paths);
    if (!HandoffRetryIds.IsEmpty() && HandoffReview.Locks.IsFresh())
        for (const auto& Pair : HandoffRetryIds)
        {
            const auto* Lock = HandoffReview.Locks.Locks.Find(Pair.Key);
            if (!Lock) Text += TEXT("\nOriginal reservation is now absent: ") + HandoffLabel(Pair.Key) + TEXT(". No release retry needed.");
            else if (Lock->Id != Pair.Value || !Lock->bOurs) Text += TEXT("\nOriginal reservation changed: ") + HandoffLabel(Pair.Key) + TEXT(". This retry cannot release its replacement.");
        }
    HandoffReport->SetText(HandoffText(Text));
}
void SGitWorkspace::RebuildHandoffRows()
{
    if (!HandoffRows) return;
    HandoffRows->ClearChildren(); TWeakPtr<SGitWorkspace> Weak = SharedThis(this);
    if (HandoffReview.Assets.IsEmpty()) HandoffRows->AddSlot().AutoHeight()[SNew(STextBlock).Text(HandoffText(TEXT("No owned locks are available for this review.")))];
    for (const auto& Asset : HandoffReview.Assets)
    {
        const FString Path = Asset.Path;
        const auto* Lock = HandoffReview.Locks.Locks.Find(Path);
        const auto* RetryId = HandoffRetryIds.Find(Path);
        const bool Allowed = HandoffRetryIds.IsEmpty() || (RetryId && Lock && Lock->Id == *RetryId);
        const bool Eligible = Asset.bReady && Allowed;
        const FString Reason = !Allowed ? TEXT("Keep: not a remaining reservation from this handoff, or its identity changed.")
            : Asset.bReady ? (HandoffReview.IsRetry() ? TEXT("Eligible for unlock only; published commit will be checked again.") : TEXT("Eligible after a verified Push; final release checks still apply.")) : TEXT("Keep: ") + Asset.Error;
        HandoffRows->AddSlot().AutoHeight().Padding(0, 0, 0, 8)
        [SNew(SCheckBox).IsChecked_Lambda([Weak, Path] { auto P = Weak.Pin(); return P && P->HandoffChecked.Contains(Path) ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
            .IsEnabled_Lambda([Weak, Eligible] { auto P = Weak.Pin(); return P && P->IsIdle() && P->HandoffReview.IsFresh() && Eligible; })
            .OnCheckStateChanged_Lambda([Weak, Path](ECheckBoxState State) { if (auto P = Weak.Pin()) { if (State == ECheckBoxState::Checked) P->HandoffChecked.Add(Path); else P->HandoffChecked.Remove(Path); P->UpdateHandoffReport(); } })
            [SNew(SVerticalBox)
                + SVerticalBox::Slot().AutoHeight()[SNew(STextBlock).Text(HandoffText(HandoffLabel(Path))).AutoWrapText(true)]
                + SVerticalBox::Slot().AutoHeight()[SNew(STextBlock).Text(HandoffText(Reason)).AutoWrapText(true)]
            ]
        ];
    }
}
bool SGitWorkspace::CanRunPushHandoff() const
{
    if (!IsIdle() || !HandoffReview.IsFresh() || HandoffChecked.IsEmpty() || HasDirtyPackages()) return false;
    for (const auto& Path : HandoffChecked)
    {
        const auto* Asset = HandoffReview.Assets.FindByPredicate([&](const auto& Item) { return Item.Path == Path; });
        const auto* Lock = HandoffReview.Locks.Locks.Find(Path);
        const auto* RetryId = HandoffRetryIds.Find(Path);
        if (!Asset || !Asset->IsFresh() || (!HandoffRetryIds.IsEmpty() && (!RetryId || !Lock || Lock->Id != *RetryId))) return false;
    }
    return true;
}
FReply SGitWorkspace::RunPushHandoff()
{
    if (!CanRunPushHandoff()) return FReply::Handled();
    const auto Reviewed = HandoffReview; auto Paths = HandoffChecked.Array(); Paths.Sort();
    // Keep the modal bounded; complete history remains in the scrollable review.
    FString Prompt = Reviewed.IsRetry() ? TEXT("Retry only the checked lock releases. No Push will be sent.")
        : FString::Printf(TEXT("Push ALL %d outgoing commits shown in the review, then release only the checked reservations."), Reviewed.Commits.Num());
    Prompt += TEXT("\n\nReviewed commit: ") + Reviewed.Remote.Head + TEXT("\nDestination: ") + HandoffLabel(Reviewed.Remote.Remote) + TEXT(" / ") + HandoffLabel(Reviewed.Remote.RemoteRef) + TEXT("\n\nLOCKS TO RELEASE\n");
    for (const auto& Path : Paths) Prompt += HandoffLabel(Path) + TEXT("\n");
    Prompt += TEXT("\nUnchecked locks stay held. Files and staging are not changed.\n\nConfirm that the team handoff is complete and no other clone needs these checked reservations. Proceed?");
    if (FMessageDialog::Open(EAppMsgType::YesNo, EAppReturnType::No, HandoffText(Prompt)) != EAppReturnType::Yes)
    { Feedback = TEXT("Handoff cancelled. No Push or lock release sent."); if (HandoffReport) HandoffReport->SetText(HandoffText(Feedback + TEXT("\n\n") + Reviewed.Text(Paths))); return FReply::Handled(); }
    if (HasDirtyPackages()) { Feedback = TEXT("Editor assets changed. Save and review again; locks retained."); return FReply::Handled(); }
    GitWorkspace::FPushHandoffResult Result;
    {
        TGuardValue<bool> Busy(bReloading, true); FScopedSlowTask Task(1.f, HandoffText(Reviewed.IsRetry() ? TEXT("Rechecking and releasing remaining locks…") : TEXT("Publishing reviewed commits, then handing off checked assets…")));
        Task.MakeDialog(false); Task.EnterProgressFrame(1.f); Task.ForceRefresh();
#if PLATFORM_MAC
        GitWorkspaceSession::FEditorWriteScope Access; FString Error;
        if (!Access.Acquire(Reviewed.Remote.Root, Error)) { Feedback = Error; if (HandoffReport) HandoffReport->SetText(HandoffText(Error)); return FReply::Handled(); }
#endif
        Result = Async(EAsyncExecution::ThreadPool, [&] { return Repository->ExecutePushHandoff(Reviewed, Paths, true); }).Get();
    }
    ApplyPushHandoffResult(Reviewed, MoveTemp(Result)); return FReply::Handled();
}
void SGitWorkspace::ApplyPushHandoffResult(const GitWorkspace::FPushHandoffReview& Reviewed, GitWorkspace::FPushHandoffResult Result)
{
    HandoffResult = MoveTemp(Result);
    bHasHandoffResult = true; HandoffReview.bValid = false; HandoffChecked.Empty();
    if (LockRemote == HandoffResult.Locks.Remote) Locks = HandoffResult.Locks;
    else Locks.bVerified = false;
    HandoffRetryIds.Empty();
    if (HandoffResult.bPushVerified)
    {
        HandoffRetryHead = Reviewed.Remote.Head; HandoffRetryContext = Reviewed.Remote.Context; HandoffRetryLockContext = Reviewed.Locks.Context;
        for (const auto& Asset : HandoffResult.Assets) if (!Asset.bReleased) HandoffRetryIds.Add(Asset.Path, Asset.Id);
    }
    Feedback = HandoffResult.Text(); if (HandoffReport) HandoffReport->SetText(HandoffText(Feedback)); RebuildHandoffRows();
    Remote.bValid = false; auto Repo = Repository; const FString ResultMessage = Feedback;
    Start([Repo, ResultMessage] { FGitWorkspaceTaskResult R; R.bRemote = true; R.Remote = Repo->Fetch(); R.Snapshot = Repo->Refresh(); R.Message = ResultMessage; return R; });
}
