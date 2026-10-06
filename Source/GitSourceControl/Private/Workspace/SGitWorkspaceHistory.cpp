// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#include "SGitWorkspaceHistory.h"
#include "SGitWorkspace.h"
#include "Async/Async.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/SWindow.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Text/STextBlock.h"
namespace
{
FText HistoryUI(const FString& Value) { return FText::FromString(Value); }
FString HistoryRow(FString Value) { return Value.Replace(TEXT("\r"), TEXT("\\r")).Replace(TEXT("\n"), TEXT("\\n")).Replace(TEXT("\t"), TEXT("\\t")); }
}
FReply SGitWorkspace::ShowHistory()
{
    if (!IsIdle() || !Snapshot.bValid) return FReply::Handled();
    if (auto Old = HistoryWindow.Pin()) Old->RequestDestroyWindow();
    const auto Window = SNew(SWindow).Title(HistoryUI(TEXT("Git history"))).ClientSize(FVector2D(1100, 760)).SupportsMinimize(false);
    HistoryWindow = Window;
    Window->SetContent(SNew(SGitWorkspaceHistory).Repository(Repository));
    FSlateApplication::Get().AddWindow(Window); return FReply::Handled();
}
void SGitWorkspaceHistory::Construct(const FArguments& Args)
{
    Repository = Args._Repository;
    ChildSlot[SNew(SBorder).Padding(12)[SNew(SVerticalBox)
        + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
        [SNew(SHorizontalBox)
            + SHorizontalBox::Slot().FillWidth(1)
            [SNew(STextBlock).AutoWrapText(true).Text_Lambda([this]
            { return HistoryUI(History.bValid ? History.Root + TEXT("  |  ") + History.Branch + TEXT("  |  HEAD ") + History.Head.Left(12) : TEXT("Git history")); })]
            + SHorizontalBox::Slot().AutoWidth().Padding(8, 0)
            [SNew(SButton).Text(HistoryUI(TEXT("Refresh history"))).IsEnabled_Lambda([this] { return IsIdle(); })
                .OnClicked_Lambda([this] { return RefreshHistory(); })]
            + SHorizontalBox::Slot().AutoWidth()
            [SNew(SButton).Text(HistoryUI(TEXT("Load 100 more"))).IsEnabled_Lambda([this] { return IsIdle() && History.bValid && History.bHasMore && Limit < 1000; })
                .OnClicked_Lambda([this] { return RefreshHistory(true); })]
        ]
        + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
        [SNew(STextBlock).AutoWrapText(true).Text(HistoryUI(TEXT("Committed revisions reachable from the displayed HEAD. Select a commit, then a changed file. Working files, staging, stashes and locks are preserved.")))]
        + SVerticalBox::Slot().FillHeight(1)
        [SNew(SSplitter)
            + SSplitter::Slot().Value(0.33f)
            [SAssignNew(CommitList, SListView<TSharedPtr<GitWorkspace::FHistoryCommit>>)
                .ListItemsSource(&Commits).SelectionMode(ESelectionMode::Single).IsEnabled_Lambda([this] { return IsIdle(); })
                .OnGenerateRow_Lambda([](TSharedPtr<GitWorkspace::FHistoryCommit> Commit, const TSharedRef<STableViewBase>& Owner)
                {
                    return SNew(STableRow<TSharedPtr<GitWorkspace::FHistoryCommit>>, Owner).Padding(6)
                    [SNew(SVerticalBox)
                        + SVerticalBox::Slot().AutoHeight()[SNew(STextBlock).AutoWrapText(true).Text(HistoryUI(Commit->Oid.Left(10) + TEXT("  ") + HistoryRow(Commit->Subject)))]
                        + SVerticalBox::Slot().AutoHeight().Padding(0, 4, 0, 0)[SNew(STextBlock).AutoWrapText(true).Text(HistoryUI(HistoryRow(Commit->Author) + TEXT("  ") + Commit->Date))]
                    ];
                })
                .OnSelectionChanged_Lambda([this](TSharedPtr<GitWorkspace::FHistoryCommit> Commit, ESelectInfo::Type) { if (Commit) ReadCommit(Commit); })]
            + SSplitter::Slot().Value(0.67f)
            [SNew(SVerticalBox)
                + SVerticalBox::Slot().FillHeight(1)
                [SNew(SSplitter).Orientation(Orient_Vertical)
                    + SSplitter::Slot().Value(0.3f)
                    [SAssignNew(CommitReport, SMultiLineEditableTextBox).IsReadOnly(true).AutoWrapText(true).Text(HistoryUI(TEXT("Select a commit.")))]
                    + SSplitter::Slot().Value(0.25f)
                    [SNew(SVerticalBox)
                        + SVerticalBox::Slot().AutoHeight().Padding(0, 8, 0, 4)
                        [SNew(STextBlock).Text_Lambda([this] { return HistoryUI(Inspection.bValid ? FString::Printf(TEXT("Changed files · %d"), Files.Num()) : TEXT("Changed files")); })]
                        + SVerticalBox::Slot().FillHeight(1)
                        [SAssignNew(FileList, SListView<TSharedPtr<GitWorkspace::FHistoryChange>>)
                            .ListItemsSource(&Files).SelectionMode(ESelectionMode::Single).IsEnabled_Lambda([this] { return IsIdle() && Inspection.bValid; })
                            .OnGenerateRow_Lambda([](TSharedPtr<GitWorkspace::FHistoryChange> File, const TSharedRef<STableViewBase>& Owner)
                            {
                                return SNew(STableRow<TSharedPtr<GitWorkspace::FHistoryChange>>, Owner).Padding(6)
                                [SNew(STextBlock).AutoWrapText(true).Text(HistoryUI(FString::Chr(File->Status) + TEXT("  ") + HistoryRow(File->Path)))];
                            })
                            .OnSelectionChanged_Lambda([this](TSharedPtr<GitWorkspace::FHistoryChange> File, ESelectInfo::Type) { if (File) ReadFile(File); })]
                    ]
                    + SSplitter::Slot().Value(0.45f)
                    [SAssignNew(FileReport, SMultiLineEditableTextBox).IsReadOnly(true).AutoWrapText(false).Text(HistoryUI(TEXT("Select a changed file for its committed text diff or revision metadata.")))]
                ]
            ]
        ]
        + SVerticalBox::Slot().AutoHeight().Padding(0, 8, 0, 0)
        [SNew(STextBlock).AutoWrapText(true).Text_Lambda([this] { return HistoryUI(IsIdle() ? Feedback : TEXT("Reading committed revisions…")); })]
    ]];
    RefreshHistory();
}
SGitWorkspaceHistory::~SGitWorkspaceHistory() { if (Pending.IsValid()) Pending.Wait(); }
FReply SGitWorkspaceHistory::RefreshHistory(bool bMore)
{
    if (!IsIdle()) return FReply::Handled();
    if (bMore) Limit = FMath::Min(1000, Limit + 100);
    Inspection = GitWorkspace::FCommitInspection(); History = GitWorkspace::FHistoryList();
    CommitList->ClearSelection(); FileList->ClearSelection(); Commits.Empty(); Files.Empty();
    CommitList->RequestListRefresh(); FileList->RequestListRefresh();
    CommitReport->SetText(HistoryUI(TEXT("Reading history…"))); FileReport->SetText(FText());
    auto Repo = Repository; const int32 Count = Limit;
    Pending = Async(EAsyncExecution::ThreadPool, [Repo, Count] { FReadResult R; R.History = Repo->ListHistory(Count); return R; });
    return FReply::Handled();
}
void SGitWorkspaceHistory::ReadCommit(TSharedPtr<GitWorkspace::FHistoryCommit> Commit)
{
    if (!IsIdle() || !Commits.Contains(Commit)) return;
    Inspection = GitWorkspace::FCommitInspection(); Files.Empty(); FileList->ClearSelection(); FileList->RequestListRefresh();
    CommitReport->SetText(HistoryUI(TEXT("Reading commit ") + Commit->Oid + TEXT("…"))); FileReport->SetText(FText());
    auto Repo = Repository; const FString Oid = Commit->Oid;
    Pending = Async(EAsyncExecution::ThreadPool, [Repo, Oid] { FReadResult R; R.Kind = 1; R.Commit = Repo->InspectCommit(Oid); return R; });
}
void SGitWorkspaceHistory::ReadFile(TSharedPtr<GitWorkspace::FHistoryChange> File)
{
    if (!IsIdle() || !Inspection.bValid || !Files.Contains(File)) return;
    FileReport->SetText(HistoryUI(TEXT("Reading committed file ") + HistoryRow(File->Path) + TEXT("…")));
    auto Repo = Repository; const FString Oid = Inspection.Commit.Oid, Path = File->Path;
    Pending = Async(EAsyncExecution::ThreadPool, [Repo, Oid, Path] { FReadResult R; R.Kind = 2; R.File = Repo->InspectCommitFile(Oid, Path); return R; });
}
void SGitWorkspaceHistory::Tick(const FGeometry&, double, float)
{
    if (!Pending.IsValid() || !Pending.IsReady()) return;
    auto Result = Pending.Get(); Pending = TFuture<FReadResult>();
    if (Result.Kind == 0)
    {
        History = MoveTemp(Result.History);
        Feedback = !History.bValid ? History.Error : History.Commits.IsEmpty() ? TEXT("No commits yet.") : FString::Printf(TEXT("Showing %d recent commits from HEAD %s. Refresh after external branch changes."), History.Commits.Num(), *History.Head.Left(12));
        if (History.bHasMore) Feedback += Limit == 1000 ? TEXT(" The 1000-commit limit is reached; use an external client for older history.") : TEXT(" Older commits are available with Load 100 more.");
        Summary = Feedback;
        if (!History.bValid || History.Commits.IsEmpty()) CommitReport->SetText(HistoryUI(Feedback));
        for (const auto& Commit : History.Commits) Commits.Add(MakeShared<GitWorkspace::FHistoryCommit>(Commit));
        CommitList->RequestListRefresh();
        if (Commits.Num()) CommitList->SetSelection(Commits[0]);
    }
    else if (Result.Kind == 1)
    {
        Inspection = MoveTemp(Result.Commit); CommitReport->SetText(HistoryUI(Inspection.Text()));
        for (const auto& File : Inspection.Files) Files.Add(MakeShared<GitWorkspace::FHistoryChange>(File));
        FileList->RequestListRefresh();
        FileReport->SetText(HistoryUI(Inspection.bValid ? Files.IsEmpty() ? TEXT("No changed files in this comparison.") : TEXT("Select a changed file for its committed text diff or revision metadata.") : Inspection.Error));
        Feedback = Inspection.bValid ? Summary : Inspection.Error;
    }
    else
    {
        FileReport->SetText(HistoryUI(Result.File.Ok() ? Result.File.Text() : Result.File.Error));
        Feedback = Result.File.Ok() ? Summary : Result.File.Error;
    }
}
