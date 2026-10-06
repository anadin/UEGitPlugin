// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#if WITH_DEV_AUTOMATION_TESTS
#include "GitWorkspaceRepository.h"
#include "SGitWorkspaceHistory.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#if PLATFORM_MAC
#include <sys/stat.h>
#endif
namespace
{
struct FHistoryFixture
{
    FString Root = FPaths::Combine(FPlatformProcess::UserTempDir(), TEXT("uegit-history-") + FGuid::NewGuid().ToString(EGuidFormats::Digits));
    FString Git = GitWorkspace::FindGitExecutable();
    FHistoryFixture()
    {
        IFileManager::Get().MakeDirectory(*Root, true); Call({TEXT("init"), TEXT("-q"), TEXT("-b"), TEXT("main")});
        Call({TEXT("config"), TEXT("user.name"), TEXT("History fixture")}); Call({TEXT("config"), TEXT("user.email"), TEXT("history@example.invalid")});
        Call({TEXT("config"), TEXT("commit.gpgsign"), TEXT("false")}); Call({TEXT("config"), TEXT("core.hooksPath"), TEXT(".git/test-hooks")});
        Call({TEXT("config"), TEXT("core.autocrlf"), TEXT("false")});
    }
    ~FHistoryFixture() { IFileManager::Get().DeleteDirectory(*Root, false, true); }
    GitWorkspace::FResult Call(const TArray<FString>& Args) { return GitWorkspace::Run(Git, Root, Args); }
    void Write(const FString& Path, const FString& Text)
    {
        const auto Full = FPaths::Combine(Root, Path); IFileManager::Get().MakeDirectory(*FPaths::GetPath(Full), true);
        FFileHelper::SaveStringToFile(Text, *Full, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
    }
    FString Read(const FString& Path) { FString R; FFileHelper::LoadFileToString(R, *FPaths::Combine(Root, Path)); return R; }
    FString Commit(const FString& Message)
    { Call({TEXT("add"), TEXT("-A")}); Call({TEXT("commit"), TEXT("-qm"), Message}); return Call({TEXT("rev-parse"), TEXT("HEAD")}).Text().TrimEnd(); }
};
TArray<uint8> HistoryBytes(const TArray<FString>& Fields)
{
    TArray<uint8> R;
    for (const auto& Field : Fields) { FTCHARToUTF8 Value(*Field); R.Append(reinterpret_cast<const uint8*>(Value.Get()), Value.Length()); R.Add(0); }
    return R;
}
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitHistoryParserTest, "GitWorkspace.History.StrictRecords", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGitHistoryParserTest::RunTest(const FString&)
{
    const FString Hash = FString::ChrN(40, 'a'), Parent = FString::ChrN(40, 'b');
    auto Bytes = HistoryBytes({Hash, Parent, TEXT("Artist 水"), TEXT("2026-10-06T10:00:00+10:00"), TEXT("quoted \"message\"\nline")});
    TArray<GitWorkspace::FHistoryCommit> Commits; FString Error;
    if (!TestTrue(TEXT("Complete literal metadata"), GitWorkspace::ParseHistory(Bytes, Commits, Error))) return false;
    TestEqual(TEXT("Literal subject"), Commits[0].Subject, FString(TEXT("quoted \"message\"\nline")));
    Bytes.Pop(); TestFalse(TEXT("Truncated history rejected"), GitWorkspace::ParseHistory(Bytes, Commits, Error)); TestTrue(TEXT("No partial success"), Commits.IsEmpty());
    TestFalse(TEXT("Invalid object rejected"), GitWorkspace::ParseHistory(HistoryBytes({TEXT("HEAD"), TEXT(""), TEXT("Artist"), TEXT("2026-10-06T00:00:00Z"), TEXT("subject")}), Commits, Error));
    TestFalse(TEXT("Invalid parent rejected"), GitWorkspace::ParseHistory(HistoryBytes({Hash, TEXT("not-a-parent"), TEXT("Artist"), TEXT("2026-10-06T00:00:00Z"), TEXT("subject")}), Commits, Error));
    TestFalse(TEXT("Invalid date rejected"), GitWorkspace::ParseHistory(HistoryBytes({Hash, Parent, TEXT("Artist"), TEXT("unknown"), TEXT("subject")}), Commits, Error));
    const auto Entry = HistoryBytes({Hash, TEXT(""), TEXT("Artist"), TEXT("2026-10-06T00:00:00Z"), TEXT("subject")});
    Bytes = Entry; Bytes.Append(Entry); TestFalse(TEXT("Duplicate record rejected"), GitWorkspace::ParseHistory(Bytes, Commits, Error));
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitHistoryReadOnlyTest, "GitWorkspace.History.FrozenRevisionsAndLiteralPaths", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGitHistoryReadOnlyTest::RunTest(const FString&)
{
    FHistoryFixture F; GitWorkspace::FRepository Repo(F.Git, F.Root);
#if PLATFORM_MAC
    const FString Path = TEXT("Docs/[asset] 水 \"quoted\"\nline.txt");
#else
    const FString Path = TEXT("Docs/[asset] 水 spaces.txt");
#endif
    F.Write(Path, TEXT("base\n")); const auto Base = F.Commit(TEXT("Initial commit"));
    F.Write(Path, TEXT("committed A\n")); const auto Oid = F.Commit(TEXT("Artist revision\n\nFull commit description"));
    F.Write(Path, TEXT("saved stash work\n"));
    TestTrue(TEXT("Keep a saved stash during history inspection"), F.Call({TEXT("stash"), TEXT("push"), TEXT("-q"), TEXT("-m"), TEXT("Historical backup")}).Ok());
    F.Write(Path, TEXT("staged B\n")); TestTrue(TEXT("Stage later edits"), Repo.Stage({Path}).Ok()); F.Write(Path, TEXT("working C\n"));
    F.Write(TEXT("Docs/untracked.txt"), TEXT("new working file\n"));
    // A custom diff command would fail if inspection executed it.
    F.Call({TEXT("config"), TEXT("diff.external"), TEXT("a-command-that-must-never-run")});
    const auto Before = Repo.Refresh(); const auto Refs = F.Call({TEXT("show-ref")}).Text(); const auto Stashes = F.Call({TEXT("stash"), TEXT("list")}).Text();
    const auto History = Repo.ListHistory();
    if (!TestTrue(TEXT("History available in dirty checkout: ") + History.Error, History.bValid)) return false;
    TestEqual(TEXT("Frozen HEAD"), History.Head, Oid); TestEqual(TEXT("Both commits visible"), History.Commits.Num(), 2);
    const auto Commit = Repo.InspectCommit(Oid);
    TestTrue(TEXT("Exact commit inspected: ") + Commit.Error, Commit.bValid); TestTrue(TEXT("Full body retained"), Commit.Message.Contains(TEXT("Full commit description")));
    TestTrue(TEXT("Exact literal path"), Commit.Files.Num() == 1 && Commit.Files[0].Path == Path);
    const auto Diff = Repo.InspectCommitFile(Oid, Path);
    TestTrue(TEXT("Historical text preview: ") + Diff.Error, Diff.Ok()); TestTrue(TEXT("Committed A shown"), Diff.Text().Contains(TEXT("+committed A")));
    TestFalse(TEXT("Staged B excluded"), Diff.Text().Contains(TEXT("+staged B"))); TestFalse(TEXT("Working C excluded"), Diff.Text().Contains(TEXT("+working C")));
    TestTrue(TEXT("Initial commit diff"), Repo.InspectCommitFile(Base, Path).Text().Contains(TEXT("+base")));
    TestFalse(TEXT("Path not changed in commit refused"), Repo.InspectCommitFile(Oid, TEXT("Docs/untracked.txt")).Ok());
    TestFalse(TEXT("Revision expression refused"), Repo.InspectCommit(TEXT("HEAD~1")).bValid);
    TestFalse(TEXT("Flag refused"), Repo.InspectCommit(TEXT("--all")).bValid);
    TestEqual(TEXT("HEAD preserved"), Repo.Refresh().Head, Before.Head); TestTrue(TEXT("Index bytes preserved"), Repo.Refresh().IndexEntries == Before.IndexEntries);
    TestEqual(TEXT("Working bytes preserved"), F.Read(Path), FString(TEXT("working C\n"))); TestEqual(TEXT("Untracked bytes preserved"), F.Read(TEXT("Docs/untracked.txt")), FString(TEXT("new working file\n")));
    TestEqual(TEXT("Refs preserved"), F.Call({TEXT("show-ref")}).Text(), Refs); TestEqual(TEXT("Stashes preserved"), F.Call({TEXT("stash"), TEXT("list")}).Text(), Stashes);
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitHistoryMergeTest, "GitWorkspace.History.MergeRootDetachedAndLimit", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGitHistoryMergeTest::RunTest(const FString&)
{
    FHistoryFixture F; GitWorkspace::FRepository Repo(F.Git, F.Root);
    auto History = Repo.ListHistory(); TestTrue(TEXT("Unborn history valid and empty"), History.bValid && History.Commits.IsEmpty());
    F.Write(TEXT("base.txt"), TEXT("base\n")); const auto Base = F.Commit(TEXT("base"));
    TestTrue(TEXT("Root compared against empty tree"), Repo.InspectCommit(Base).Text().Contains(TEXT("initial commit against the empty tree")));
    F.Call({TEXT("checkout"), TEXT("-qb"), TEXT("feature")}); F.Write(TEXT("feature.txt"), TEXT("feature\n")); F.Commit(TEXT("feature"));
    F.Call({TEXT("checkout"), TEXT("-q"), TEXT("main")}); F.Write(TEXT("main.txt"), TEXT("main\n")); const auto FirstParent = F.Commit(TEXT("main update"));
    TestTrue(TEXT("Create merge fixture"), F.Call({TEXT("merge"), TEXT("--no-ff"), TEXT("-qm"), TEXT("merge feature"), TEXT("feature")}).Ok());
    const auto Merge = Repo.InspectCommit(Repo.Refresh().Head);
    TestTrue(TEXT("Merge has exact parent comparison"), Merge.bValid && Merge.Commit.Parents.Num() == 2 && Merge.Commit.Parents[0] == FirstParent);
    TestTrue(TEXT("Merge disclosure"), Merge.Text().Contains(TEXT("first parent only")));
    TestTrue(TEXT("First parent includes feature only"), Merge.Files.Num() == 1 && Merge.Files[0].Path == TEXT("feature.txt"));
    TestTrue(TEXT("Merged text shown"), Repo.InspectCommitFile(Merge.Commit.Oid, TEXT("feature.txt")).Text().Contains(TEXT("+feature")));
    History = Repo.ListHistory(2); TestTrue(TEXT("Bounded history exposes more"), History.bValid && History.Commits.Num() == 2 && History.bHasMore);
    TestTrue(TEXT("Larger page includes both parents"), Repo.ListHistory(100).Commits.Num() == 4);
    TestFalse(TEXT("Out of range limit refused"), Repo.ListHistory(1001).bValid);
    F.Call({TEXT("checkout"), TEXT("--detach"), TEXT("-q"), Base}); History = Repo.ListHistory();
    TestTrue(TEXT("Detached history available"), History.bValid && History.Head == Base && History.Commits.Num() == 1);
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitHistoryLfsTest, "GitWorkspace.History.LfsBinaryDeletionAndSubmodule", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGitHistoryLfsTest::RunTest(const FString&)
{
    FHistoryFixture F; GitWorkspace::FRepository Repo(F.Git, F.Root);
    if (!TestTrue(TEXT("LFS available"), F.Call({TEXT("lfs"), TEXT("install"), TEXT("--local"), TEXT("--skip-repo")}).Ok())) return false;
    F.Write(TEXT(".gitattributes"), TEXT("*.uasset filter=lfs diff=lfs merge=lfs -text lockable\n"));
    F.Write(TEXT("Content/Asset.uasset"), TEXT("historical saved asset\n")); F.Write(TEXT("old.txt"), TEXT("rename bytes\n"));
    const auto Base = F.Commit(TEXT("assets"));
    const auto Pointer = F.Call({TEXT("show"), TEXT("HEAD:Content/Asset.uasset")}).Text();
    F.Call({TEXT("mv"), TEXT("old.txt"), TEXT("renamed.txt")}); F.Call({TEXT("rm"), TEXT("Content/Asset.uasset")});
    F.Write(TEXT(".gitattributes"), TEXT("")); // Current attributes must not override historical identity.
    F.Call({TEXT("update-index"), TEXT("--add"), TEXT("--cacheinfo"), TEXT("160000,") + Base + TEXT(",Plugins/Nested")});
    F.Call({TEXT("add"), TEXT(".gitattributes")});
    TestTrue(TEXT("Commit historical deletion and gitlink"), F.Call({TEXT("commit"), TEXT("-qm"), TEXT("deletion and nested commit")}).Ok());
    const auto Oid = Repo.Refresh().Head;
    const auto BeforeIndex = Repo.Refresh().IndexEntries;
    const auto Report = Repo.InspectCommitFile(Oid, TEXT("Content/Asset.uasset"));
    TestTrue(TEXT("Deleted asset metadata: ") + Report.Error, Report.Ok()); TestTrue(TEXT("Exact old LFS pointer shown"), Report.Text().Contains(Pointer));
    TestTrue(TEXT("New version absent"), Report.Text().Contains(TEXT("AFTER\nAbsent"))); TestTrue(TEXT("No visual diff promise"), Report.Text().Contains(TEXT("no LFS object was downloaded")));
    TestTrue(TEXT("Gitlink commit metadata"), Repo.InspectCommitFile(Oid, TEXT("Plugins/Nested")).Text().Contains(Base));
    const auto Commit = Repo.InspectCommit(Oid);
    TestTrue(TEXT("Renames explicitly represented as both paths"), Commit.Files.ContainsByPredicate([](const auto& C) { return C.Path == TEXT("old.txt") && C.Status == 'D'; }) && Commit.Files.ContainsByPredicate([](const auto& C) { return C.Path == TEXT("renamed.txt") && C.Status == 'A'; }));
    TestTrue(TEXT("Asset and submodule inspection preserve index"), Repo.Refresh().IndexEntries == BeforeIndex);
    TArray<uint8> Binary{1, 0, 2, 3}; FFileHelper::SaveArrayToFile(Binary, *FPaths::Combine(F.Root, TEXT("binary.bin")));
    // Remove fixture gitlink before add -A (it deliberately has no checkout).
    F.Call({TEXT("update-index"), TEXT("--force-remove"), TEXT("Plugins/Nested")}); const auto BinaryCommit = F.Commit(TEXT("binary"));
    TestTrue(TEXT("Binary metadata only"), Repo.InspectCommitFile(BinaryCommit, TEXT("binary.bin")).Text().Contains(TEXT("Revision metadata only")));
    F.Write(TEXT("large.txt"), FString::ChrN(1024 * 1024 + 1, 'x')); const auto LargeCommit = F.Commit(TEXT("large"));
    TestTrue(TEXT("Large text bounded"), Repo.InspectCommitFile(LargeCommit, TEXT("large.txt")).Text().Contains(TEXT("text preview limit is 1 MiB")));
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitHistoryPanelTest, "GitWorkspace.History.PanelSelectionRefreshAndClose", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGitHistoryPanelTest::RunTest(const FString&)
{
    FHistoryFixture F; F.Write(TEXT("README.md"), TEXT("base\n")); const auto Base = F.Commit(TEXT("initial"));
    F.Write(TEXT("README.md"), TEXT("committed A\n")); const auto Oid = F.Commit(TEXT("revision A"));
    auto Repo = MakeShared<GitWorkspace::FRepository, ESPMode::ThreadSafe>(F.Git, F.Root);
    F.Write(TEXT("README.md"), TEXT("staged B\n")); Repo->Stage({TEXT("README.md")}); F.Write(TEXT("README.md"), TEXT("working C\n"));
    const auto Index = Repo->Refresh().IndexEntries;
    TSharedPtr<SGitWorkspaceHistory> Panel = SNew(SGitWorkspaceHistory).Repository(Repo);
    auto Settle = [&]() { while (Panel->Pending.IsValid()) { Panel->Pending.Wait(); Panel->Tick(FGeometry(), 0, 0); } };
    Settle();
    if (!TestTrue(TEXT("History panel reads latest exact commit"), Panel->Inspection.bValid && Panel->Inspection.Commit.Oid == Oid && Panel->Files.Num() == 1)) return false;
    Panel->FileList->SetSelection(Panel->Files[0]); Settle();
    TestTrue(TEXT("Panel diff is committed snapshot"), Panel->FileReport->GetText().ToString().Contains(TEXT("+committed A")) && !Panel->FileReport->GetText().ToString().Contains(TEXT("+working C")));
    const auto File = Panel->Files[0]; const FString OriginalPath = File->Path;
    File->Path = TEXT("not-a-committed-file"); Panel->ReadFile(File); Settle();
    TestTrue(TEXT("Unreadable selection reports error"), Panel->Feedback.Contains(TEXT("Select a changed file")));
    File->Path = OriginalPath; Panel->ReadFile(File); Settle();
    TestEqual(TEXT("Successful inspection clears previous error"), Panel->Feedback, Panel->Summary);
    Panel->CommitList->SetSelection(Panel->Commits[1]); Settle(); Panel->FileList->SetSelection(Panel->Files[0]); Settle();
    TestTrue(TEXT("Selecting initial commit resets file comparison"), Panel->Inspection.Commit.Oid == Base && Panel->FileReport->GetText().ToString().Contains(TEXT("+base")));
    // External ref movement changes neither the displayed commit nor its blobs.
    F.Call({TEXT("update-ref"), TEXT("HEAD"), Base, Oid});
    Panel->CommitList->SetSelection(Panel->Commits[0]); Settle();
    TestEqual(TEXT("Old list selection keeps exact revision"), Panel->Inspection.Commit.Oid, Oid);
    Panel->RefreshHistory(); Settle(); TestEqual(TEXT("Explicit refresh follows changed HEAD"), Panel->History.Head, Base);
    TestEqual(TEXT("Refresh shows one reachable commit"), Panel->Commits.Num(), 1); TestTrue(TEXT("Index preserved"), Repo->Refresh().IndexEntries == Index);
    TestEqual(TEXT("Working bytes preserved"), F.Read(TEXT("README.md")), FString(TEXT("working C\n")));
    Panel->RefreshHistory(); Panel.Reset(); // Closing while a read is pending safely drains its worker.
    auto Reopened = SNew(SGitWorkspaceHistory).Repository(Repo);
    while (Reopened->Pending.IsValid()) { Reopened->Pending.Wait(); Reopened->Tick(FGeometry(), 0, 0); }
    TestEqual(TEXT("Reopened panel owns its own results"), Reopened->Inspection.Commit.Oid, Base);
    return true;
}
#endif
