// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#include "SGitWorkspace.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SSearchBox.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/STableRow.h"

namespace
{
FText PickerText(const FString& S) { return FText::FromString(S); }
FString PickerPath(FString S) { return S.Replace(TEXT("\\"), TEXT("\\\\")).Replace(TEXT("\n"), TEXT("\\n")).Replace(TEXT("\r"), TEXT("\\r")).Replace(TEXT("\t"), TEXT("\\t")); }
}
bool SGitWorkspace::CanSelectStashFile(const GitWorkspace::FFile& File) const
{
    return !File.bSubmodule && !File.bConflict && (File.HasStaged() || File.HasUnstaged()) && (!File.bUntracked || bIncludeUntrackedStash);
}
void SGitWorkspace::InvalidateStashSelection()
{
    StashReview = GitWorkspace::FStashReview(); StashReview.bCreate = true; StashReview.bSelected = true;
    StashInspection = GitWorkspace::FStashInspection();
    StashReview.Error = TEXT("File selection changed. Review the checked files before creating a stash.");
    if (StashReport) StashReport->SetText(PickerText(StashReview.Error));
}
void SGitWorkspace::SetStashPathChecked(const FString& Path, bool bChecked)
{
    if (!IsIdle()) return;
    const auto* File = Snapshot.Files.FindByPredicate([&](const auto& F) { return F.Path == Path; });
    if (bChecked && (!File || !CanSelectStashFile(*File))) return;
    if (bChecked) StashSelectedPaths.AddUnique(Path); else StashSelectedPaths.Remove(Path);
    StashSelectedPaths.Sort(); InvalidateStashSelection();
}
void SGitWorkspace::SetStashIncludeUntracked(bool bInclude)
{
    if (!IsIdle()) return;
    bIncludeUntrackedStash = bInclude;
    InvalidateStashSelection(); RebuildStashFiles();
}
void SGitWorkspace::RebuildStashFiles()
{
    if (!StashFiles) return;
    const int32 Before = StashSelectedPaths.Num();
    StashSelectedPaths.RemoveAll([&](const FString& Path)
    {
        const auto* File = Snapshot.Files.FindByPredicate([&](const auto& F) { return F.Path == Path; });
        return !File || !CanSelectStashFile(*File);
    });
    if (Before != StashSelectedPaths.Num() && StashReview.bValid) InvalidateStashSelection();
    StashFileItems.Empty();
    for (const auto& File : Snapshot.Files)
        if ((!bStashContentOnly || File.Path.StartsWith(TEXT("Content/"))) &&
            (StashFileFilter.IsEmpty() || File.Path.Contains(StashFileFilter))) StashFileItems.Add(MakeShared<GitWorkspace::FFile>(File));
    StashFileItems.Sort([](const auto& A, const auto& B) { return A->Path < B->Path; });
    StashFiles->RequestListRefresh();
}
FText SGitWorkspace::StashSelectionSummary() const
{
    int32 VisibleSelected = 0;
    for (const auto& File : StashFileItems) if (StashSelectedPaths.Contains(File->Path)) ++VisibleSelected;
    return PickerText(FString::Printf(TEXT("%d checked · %d files shown · %d checked files hidden by filters"),
        StashSelectedPaths.Num(), StashFileItems.Num(), StashSelectedPaths.Num() - VisibleSelected));
}
TSharedRef<SWidget> SGitWorkspace::MakeStashFilePicker()
{
    TWeakPtr<SGitWorkspace> Weak = SharedThis(this);
    auto Idle = [Weak] { auto P = Weak.Pin(); return P && P->IsIdle(); };
    return SNew(SVerticalBox)
        + SVerticalBox::Slot().AutoHeight()[SNew(STextBlock).Text(PickerText(TEXT("Create a stash")))]
        + SVerticalBox::Slot().AutoHeight().Padding(0, 8)
        [SNew(STextBlock).Text(PickerText(TEXT("Tick the files to stash here. Both saved staged and working versions are included. Locks stay held."))).AutoWrapText(true)]
        + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
        [SAssignNew(StashName, SEditableTextBox).HintText(PickerText(TEXT("Name for a new stash"))).IsEnabled_Lambda(Idle)]
        + SVerticalBox::Slot().AutoHeight().Padding(0, 4)
        [SNew(SCheckBox).IsChecked_Lambda([Weak] { auto P = Weak.Pin(); return P && P->bIncludeUntrackedStash ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
            .IsEnabled_Lambda(Idle).OnCheckStateChanged_Lambda([Weak](ECheckBoxState State) { if (auto P = Weak.Pin()) P->SetStashIncludeUntracked(State == ECheckBoxState::Checked); })
            [SNew(STextBlock).Text(PickerText(TEXT("Include untracked files (new saved assets)")))]]
        + SVerticalBox::Slot().AutoHeight().Padding(0, 8)
        [SNew(SHorizontalBox)
            + SHorizontalBox::Slot().FillWidth(1)
            [SNew(SCheckBox).IsChecked_Lambda([Weak] { auto P = Weak.Pin(); return P && P->bStashContentOnly ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
                .IsEnabled_Lambda(Idle).OnCheckStateChanged_Lambda([Weak](ECheckBoxState) { if (auto P = Weak.Pin()) { P->bStashContentOnly = true; P->RebuildStashFiles(); } })[SNew(STextBlock).Text(PickerText(TEXT("Content")))]]
            + SHorizontalBox::Slot().FillWidth(1)
            [SNew(SCheckBox).IsChecked_Lambda([Weak] { auto P = Weak.Pin(); return P && !P->bStashContentOnly ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
                .IsEnabled_Lambda(Idle).OnCheckStateChanged_Lambda([Weak](ECheckBoxState) { if (auto P = Weak.Pin()) { P->bStashContentOnly = false; P->RebuildStashFiles(); } })[SNew(STextBlock).Text(PickerText(TEXT("Whole repo")))]]
        ]
        + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
        [SNew(SSearchBox).HintText(PickerText(TEXT("Filter files to stash"))).IsEnabled_Lambda(Idle)
            .OnTextChanged_Lambda([Weak](const FText& Value) { if (auto P = Weak.Pin()) { P->StashFileFilter = Value.ToString(); P->RebuildStashFiles(); } })]
        + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
        [SNew(SHorizontalBox)
            + SHorizontalBox::Slot().AutoWidth()
            [SNew(SButton).Text(PickerText(TEXT("Check shown"))).IsEnabled_Lambda(Idle).OnClicked_Lambda([Weak]
                { if (auto P = Weak.Pin()) for (const auto& File : P->StashFileItems) P->SetStashPathChecked(File->Path, true); return FReply::Handled(); })]
            + SHorizontalBox::Slot().AutoWidth().Padding(8, 0)
            [SNew(SButton).Text(PickerText(TEXT("Clear checks"))).IsEnabled_Lambda(Idle).OnClicked_Lambda([Weak]
                { if (auto P = Weak.Pin()) { P->StashSelectedPaths.Empty(); P->InvalidateStashSelection(); } return FReply::Handled(); })]
        ]
        + SVerticalBox::Slot().FillHeight(1)
        [SAssignNew(StashFiles, SListView<TSharedPtr<GitWorkspace::FFile>>).ListItemsSource(&StashFileItems).SelectionMode(ESelectionMode::None)
            .OnGenerateRow_Lambda([Weak](TSharedPtr<GitWorkspace::FFile> File, const TSharedRef<STableViewBase>& Owner)
            {
                const FString Path = File->Path;
                return SNew(STableRow<TSharedPtr<GitWorkspace::FFile>>, Owner).Padding(4)
                [SNew(SVerticalBox)
                    + SVerticalBox::Slot().AutoHeight()
                    [SNew(SCheckBox).IsChecked_Lambda([Weak, Path] { auto P = Weak.Pin(); return P && P->StashSelectedPaths.Contains(Path) ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
                        .IsEnabled_Lambda([Weak, File] { auto P = Weak.Pin(); return P && P->IsIdle() && P->CanSelectStashFile(*File); })
                        .OnCheckStateChanged_Lambda([Weak, Path](ECheckBoxState State) { if (auto P = Weak.Pin()) P->SetStashPathChecked(Path, State == ECheckBoxState::Checked); })
                        [SNew(STextBlock).Text(PickerText(PickerPath(Path))).AutoWrapText(true)]]
                    + SVerticalBox::Slot().AutoHeight().Padding(20, 2, 0, 2)
                    [SNew(STextBlock).Text_Lambda([Weak, File]
                    {
                        auto P = Weak.Pin();
                        return PickerText(File->bSubmodule ? TEXT("Submodule — preserve separately") : File->bConflict ? TEXT("Resolve conflict first") :
                            File->bUntracked ? (P && P->bIncludeUntrackedStash ? TEXT("New untracked file") : TEXT("New file — enable Include untracked files above")) :
                            File->HasStaged() && File->HasUnstaged() ? TEXT("Staged and working changes") : File->HasStaged() ? TEXT("Staged changes") : TEXT("Working changes"));
                    }).AutoWrapText(true)]
                ];
            })]
        + SVerticalBox::Slot().AutoHeight().Padding(0, 8)
        [SNew(STextBlock).Text_Lambda([Weak] { auto P = Weak.Pin(); return P ? P->StashSelectionSummary() : FText(); }).AutoWrapText(true)]
        + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
        [SNew(SButton).Text_Lambda([Weak] { auto P = Weak.Pin(); return PickerText(FString::Printf(TEXT("Review selected (%d)"), P ? P->StashSelectedPaths.Num() : 0)); })
            .IsEnabled_Lambda([Weak] { auto P = Weak.Pin(); return P && P->IsIdle() && !P->StashSelectedPaths.IsEmpty(); })
            .OnClicked_Lambda([Weak] { if (auto P = Weak.Pin()) return P->PreviewStash(FString(), FString(), true); return FReply::Handled(); })]
        + SVerticalBox::Slot().AutoHeight()
        [SNew(SButton).Text_Lambda([Weak] { auto P = Weak.Pin(); return PickerText(P && P->bIncludeUntrackedStash ? TEXT("Review all repo changes") : TEXT("Review all tracked repo changes")); })
            .ToolTipText(PickerText(TEXT("Repository-wide; ignores the file checks and filters above. The review lists every affected path.")))
            .IsEnabled_Lambda(Idle).OnClicked_Lambda([Weak] { if (auto P = Weak.Pin()) return P->PreviewStash(FString()); return FReply::Handled(); })];
}
