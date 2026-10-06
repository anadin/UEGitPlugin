// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#if WITH_DEV_AUTOMATION_TESTS && PLATFORM_MAC
#include "GitWorkspaceRepository.h"
#include "GitWorkspaceSession.h"
#include "SGitWorkspace.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Misc/MessageDialog.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformProcess.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include <sys/stat.h>
namespace
{
struct FDiscardFixture
{
    FString Root = FPaths::Combine(FPlatformProcess::UserTempDir(), TEXT("uegit-discard-") + FGuid::NewGuid().ToString(EGuidFormats::Digits));
    FString Git = GitWorkspace::FindGitExecutable();
    GitWorkspace::FRepository Repo {Git, Root}; GitWorkspaceSession::FLease Lease;
    FDiscardFixture()
    {
        IFileManager::Get().MakeDirectory(*Root, true); Call({TEXT("init"), TEXT("-q"), TEXT("-b"), TEXT("main")});
        Call({TEXT("config"), TEXT("user.name"), TEXT("Discard fixture")}); Call({TEXT("config"), TEXT("user.email"), TEXT("fixture@example.invalid")});
        Call({TEXT("config"), TEXT("commit.gpgsign"), TEXT("false")}); Call({TEXT("config"), TEXT("core.autocrlf"), TEXT("false")});
        Call({TEXT("config"), TEXT("core.hooksPath"), TEXT(".git/test-hooks")}); Call({TEXT("lfs"), TEXT("install"), TEXT("--local"), TEXT("--skip-repo")});
        Write(TEXT("README.md"), TEXT("base\n")); Write(TEXT("Docs/other.txt"), TEXT("other base\n")); Commit();
        FString Error; Lease.Acquire(Root, true, Error);
    }
    ~FDiscardFixture() { Lease.Release(); IFileManager::Get().DeleteDirectory(*Root, false, true); }
    GitWorkspace::FResult Call(const TArray<FString>& Args) { return GitWorkspace::Run(Git, Root, Args); }
    void Write(const FString& Path, const FString& Text)
    {
        const auto Full = FPaths::IsRelative(Path) ? FPaths::Combine(Root, Path) : Path;
        IFileManager::Get().MakeDirectory(*FPaths::GetPath(Full), true);
        FPlatformFileManager::Get().GetPlatformFile().SetReadOnly(*Full, false);
        FFileHelper::SaveStringToFile(Text, *Full, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
    }
    FString Read(const FString& Path) { FString R; FFileHelper::LoadFileToString(R, *FPaths::Combine(Root, Path)); return R; }
    void Commit() { Call({TEXT("add"), TEXT("-A")}); Call({TEXT("commit"), TEXT("-qm"), TEXT("fixture checkpoint")}); }
    FString Marker() { FString Canonical, GitDir; GitWorkspaceSession::FindRepository(Root, Canonical, GitDir); return GitWorkspaceSession::RecoveryFile(GitDir); }
    FString Backup(const GitWorkspace::FDiscardReview& R) { FString Canonical, GitDir; GitWorkspaceSession::FindRepository(Root, Canonical, GitDir); return FPaths::Combine(GitDir, TEXT("uegit/discard"), R.Id); }
    FString Wrapper(const FString& OnRestore)
    {
        const FString Path = TEXT(".git/test-git");
        Write(Path, TEXT("#!/bin/sh\ncase \" $* \" in\n*\" restore \"*)\n") + OnRestore + TEXT("\n;;\nesac\nexec \"") + Git + TEXT("\" \"$@\"\n"));
        const auto Full = FPaths::Combine(Root, Path); chmod(TCHAR_TO_UTF8(*Full), 0755); return Full;
    }
};
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitDiscardPreservationTest, "GitWorkspace.Discard.StagedVersionsLiteralScopeAndRecovery", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGitDiscardPreservationTest::RunTest(const FString&)
{
    FDiscardFixture F;
    const FString Odd = TEXT("Docs/[é] \"quoted\"\nfile.txt"); F.Write(Odd, TEXT("odd base\n")); F.Commit();
    F.Write(TEXT("README.md"), TEXT("saved stash\n")); F.Call({TEXT("stash"), TEXT("push"), TEXT("-qm"), TEXT("keep this stash")});
    F.Write(TEXT("README.md"), TEXT("staged A\n")); F.Repo.Stage({TEXT("README.md")}); F.Write(TEXT("README.md"), TEXT("working B\n"));
    F.Write(TEXT("Docs/other.txt"), TEXT("staged C\n")); F.Repo.Stage({TEXT("Docs/other.txt")}); F.Write(TEXT("Docs/other.txt"), TEXT("working D\n"));
    F.Write(Odd, TEXT("discard odd edits\n")); F.Write(TEXT("new.txt"), TEXT("untracked untouched\n"));
    F.Write(TEXT(".git/uegit/locks/preservation-probe"), TEXT("held lock record\n"));
    const auto Before = F.Repo.Refresh(); const auto Stashes = F.Repo.ListStashes(); const auto Refs = F.Call({TEXT("show-ref")}).Text();
    const auto Review = F.Repo.ReviewDiscard({Odd, TEXT("README.md")});
    if (!TestTrue(TEXT("Exact discard review: ") + Review.Error, Review.IsFresh())) return false;
    TestTrue(TEXT("Review keeps index"), F.Repo.Refresh().IndexEntries == Before.IndexEntries); TestEqual(TEXT("Review keeps working B"), F.Read(TEXT("README.md")), FString(TEXT("working B\n")));
    TestEqual(TEXT("Preview creates no recovery ref"), F.Call({TEXT("show-ref")}).Text(), Refs); TestFalse(TEXT("Preview creates no backup entry"), IFileManager::Get().DirectoryExists(*F.Backup(Review)));
    TestTrue(TEXT("Literal path is escaped in review"), Review.Text.Contains(TEXT("\\nfile.txt")));
    TestFalse(TEXT("Missing confirmation refused"), F.Repo.ExecuteDiscard(Review, F.Lease).Ok());
    const auto Result = F.Repo.ExecuteDiscard(Review, F.Lease, true);
    if (!TestTrue(TEXT("Discard succeeds: ") + Result.Error, Result.Ok())) return false;
    TestTrue(TEXT("Active marker persists until reload/completion"), IFileManager::Get().FileExists(*F.Marker()));
    TestEqual(TEXT("Selected working B restores staged A"), F.Read(TEXT("README.md")), FString(TEXT("staged A\n")));
    TestEqual(TEXT("Other working D retained"), F.Read(TEXT("Docs/other.txt")), FString(TEXT("working D\n")));
    TestEqual(TEXT("Unstaged-only file restores committed index"), F.Read(Odd), FString(TEXT("odd base\n")));
    TestEqual(TEXT("Untracked bytes retained"), F.Read(TEXT("new.txt")), FString(TEXT("untracked untouched\n")));
    TestTrue(TEXT("All staging retained exactly"), F.Repo.Refresh().IndexEntries == Before.IndexEntries); TestEqual(TEXT("HEAD retained"), F.Repo.Refresh().Head, Before.Head);
    TestEqual(TEXT("Stash list retained"), F.Repo.ListStashes().Fingerprint, Stashes.Fingerprint);
    TestEqual(TEXT("Lock acquisition record retained"), F.Read(TEXT(".git/uegit/locks/preservation-probe")), FString(TEXT("held lock record\n")));
    TestTrue(TEXT("Completion verifies and clears marker"), F.Repo.CompleteDiscard(Review, F.Lease).Ok()); TestFalse(TEXT("Marker cleared"), IFileManager::Get().FileExists(*F.Marker()));
    const FString Saved = FPaths::Combine(F.Backup(Review), TEXT("payload/0001.bin")); // Sorted Docs path precedes README.
    FString Payload; FFileHelper::LoadFileToString(Payload, *Saved); TestEqual(TEXT("Discarded saved bytes are recoverable"), Payload, FString(TEXT("working B\n")));
    TestTrue(TEXT("Backup retained after completion"), IFileManager::Get().FileExists(*FPaths::Combine(F.Backup(Review), TEXT("manifest.json"))));
    TestTrue(TEXT("Manual working-byte recovery succeeds"), IFileManager::Get().Copy(*FPaths::Combine(F.Root, TEXT("README.md")), *Saved, true, true) == COPY_OK);
    TestEqual(TEXT("Recovery restores working B"), F.Read(TEXT("README.md")), FString(TEXT("working B\n"))); TestTrue(TEXT("Recovery leaves staging unchanged"), F.Repo.Refresh().IndexEntries == Before.IndexEntries);
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitDiscardGuardsTest, "GitWorkspace.Discard.StaleForgedAndUnsupported", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGitDiscardGuardsTest::RunTest(const FString&)
{
    FDiscardFixture F; F.Write(TEXT("README.md"), TEXT("working\n")); const auto Review = F.Repo.ReviewDiscard({TEXT("README.md")});
    if (!TestTrue(TEXT("Guard fixture review"), Review.IsFresh())) return false;
    auto Bad = Review; Bad.Capture.ReviewedSeconds -= 301; TestFalse(TEXT("Expired refused"), F.Repo.ExecuteDiscard(Bad, F.Lease, true).Ok());
    Bad = Review; Bad.ExpectedWorkingTree = Review.BeforeWorkingTree; TestFalse(TEXT("Forged target refused"), F.Repo.ExecuteDiscard(Bad, F.Lease, true).Ok());
    Bad = Review; Bad.RawHashes.Add(1); TestFalse(TEXT("Forged saved bytes refused"), F.Repo.ExecuteDiscard(Bad, F.Lease, true).Ok());
    Bad = Review; Bad.Capture.WorkingTree = Review.IndexTree; TestFalse(TEXT("Forged saved tree refused before replacement"), F.Repo.ExecuteDiscard(Bad, F.Lease, true).Ok());
    Bad = Review; Bad.Id = TEXT("../outside"); TestFalse(TEXT("Unsafe backup identity refused"), F.Repo.ExecuteDiscard(Bad, F.Lease, true).Ok());
    TestFalse(TEXT("Duplicate selection refused"), F.Repo.ReviewDiscard({TEXT("README.md"), TEXT("README.md")}).IsFresh());
    F.Write(TEXT("README.md"), TEXT("changed after review\n")); TestFalse(TEXT("Saved edit drift refused"), F.Repo.ExecuteDiscard(Review, F.Lease, true).Ok());
    F.Write(TEXT("README.md"), TEXT("working\n")); F.Call({TEXT("config"), TEXT("fixture.changed"), TEXT("yes")}); TestFalse(TEXT("Config drift refused"), F.Repo.ExecuteDiscard(Review, F.Lease, true).Ok()); F.Call({TEXT("config"), TEXT("--unset"), TEXT("fixture.changed")});
    F.Write(TEXT("new.txt"), TEXT("untracked\n")); TestFalse(TEXT("Untracked removal not offered"), F.Repo.ReviewDiscard({TEXT("new.txt")}).IsFresh());
    IFileManager::Get().Delete(*FPaths::Combine(F.Root, TEXT("README.md"))); TestFalse(TEXT("Deletion not offered"), F.Repo.ReviewDiscard({TEXT("README.md")}).IsFresh());
    F.Write(TEXT("README.md"), TEXT("working\n")); TestFalse(TEXT("Rejected operations created no backup entry"), IFileManager::Get().DirectoryExists(*F.Backup(Review)));
    TestEqual(TEXT("Current bytes preserved"), F.Read(TEXT("README.md")), FString(TEXT("working\n")));
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitDiscardLfsTest, "GitWorkspace.Discard.LfsPayloadAndMissingCache", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGitDiscardLfsTest::RunTest(const FString&)
{
    FDiscardFixture F; F.Write(TEXT(".gitattributes"), TEXT("*.uasset filter=lfs diff=lfs merge=lfs -text lockable\n"));
    F.Write(TEXT("Content/Probe.uasset"), TEXT("base asset\n")); F.Commit();
    F.Write(TEXT("Content/Probe.uasset"), TEXT("staged asset A\n")); F.Repo.Stage({TEXT("Content/Probe.uasset")}); F.Write(TEXT("Content/Probe.uasset"), TEXT("working asset B\n"));
    const auto Index = F.Repo.Refresh().IndexEntries; auto Review = F.Repo.ReviewDiscard({TEXT("Content/Probe.uasset")});
    if (!TestTrue(TEXT("LFS discard review: ") + Review.Error, Review.IsFresh())) return false;
    F.Call({TEXT("config"), TEXT("filter.lfs.smudge"), TEXT("git-lfs smudge --skip -- %f")}); // Existing skip-smudge policy is overridden only for restoration.
    Review = F.Repo.ReviewDiscard({TEXT("Content/Probe.uasset")});
    auto Result = F.Repo.ExecuteDiscard(Review, F.Lease, true);
    if (!TestTrue(TEXT("LFS staged bytes restore: ") + Result.Error, Result.Ok())) return false;
    TestEqual(TEXT("Hydrated staged A restored"), F.Read(TEXT("Content/Probe.uasset")), FString(TEXT("staged asset A\n")));
    FString Backup; FFileHelper::LoadFileToString(Backup, *FPaths::Combine(F.Backup(Review), TEXT("payload/0000.bin")));
    TestEqual(TEXT("Backup holds full working B, not pointer"), Backup, FString(TEXT("working asset B\n")));
    TestTrue(TEXT("Staging retained"), F.Repo.Refresh().IndexEntries == Index); TestTrue(TEXT("Complete LFS discard"), F.Repo.CompleteDiscard(Review, F.Lease).Ok());
    F.Write(TEXT("Content/Probe.uasset"), TEXT("working asset C\n")); const auto Missing = F.Repo.ReviewDiscard({TEXT("Content/Probe.uasset")});
    const FString Pointer = F.Call({TEXT("show"), TEXT(":Content/Probe.uasset")}).Text(); const int32 At = Pointer.Find(TEXT("oid sha256:")); const FString Oid = Pointer.Mid(At + 11, 64);
    TestTrue(TEXT("Remove fixture's staged LFS cache only"), IFileManager::Get().Delete(*FPaths::Combine(F.Root, TEXT(".git/lfs/objects"), Oid.Left(2), Oid.Mid(2, 2), Oid), true, true));
    TestFalse(TEXT("Missing target refuses before replacement"), F.Repo.ExecuteDiscard(Missing, F.Lease, true).Ok());
    TestEqual(TEXT("Working C preserved"), F.Read(TEXT("Content/Probe.uasset")), FString(TEXT("working asset C\n"))); TestFalse(TEXT("No active recovery before replacement"), IFileManager::Get().FileExists(*F.Marker()));
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitDiscardRecoveryTest, "GitWorkspace.Discard.FailureAndCompletionRecovery", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGitDiscardRecoveryTest::RunTest(const FString&)
{
    {
        FDiscardFixture F; F.Write(TEXT("README.md"), TEXT("saved edits\n"));
        const auto Wrapper = F.Wrapper(TEXT("echo fixture-restore-refused >&2\nexit 17")); GitWorkspace::FRepository Repo(Wrapper, F.Root);
        const auto Review = Repo.ReviewDiscard({TEXT("README.md")}); if (!TestTrue(TEXT("Failure fixture review"), Review.IsFresh())) return false;
        TestFalse(TEXT("Failed restore reported"), Repo.ExecuteDiscard(Review, F.Lease, true).Ok());
        TestTrue(TEXT("Failure retains active marker"), IFileManager::Get().FileExists(*F.Marker())); TestEqual(TEXT("Failure keeps original bytes"), F.Read(TEXT("README.md")), FString(TEXT("saved edits\n")));
        TestTrue(TEXT("Failure preserves verified payload"), IFileManager::Get().FileExists(*FPaths::Combine(F.Backup(Review), TEXT("payload/0000.bin"))));
        TestFalse(TEXT("Active recovery blocks another review"), Repo.ReviewDiscard({TEXT("README.md")}).IsFresh());
    }
    {
        FDiscardFixture F; F.Write(TEXT("README.md"), TEXT("saved edits\n")); auto Review = F.Repo.ReviewDiscard({TEXT("README.md")});
        if (!TestTrue(TEXT("Completion fixture execute"), F.Repo.ExecuteDiscard(Review, F.Lease, true).Ok())) return false;
        FString Marker; FFileHelper::LoadFileToString(Marker, *F.Marker()); F.Write(F.Marker(), Marker.Replace(*Review.Id, TEXT("00000000000000000000000000000000")));
        TestFalse(TEXT("Changed recovery identity cannot clear"), F.Repo.CompleteDiscard(Review, F.Lease).Ok()); F.Write(F.Marker(), Marker);
        const auto Payload = FPaths::Combine(F.Backup(Review), TEXT("payload/0000.bin")); F.Write(Payload, TEXT("corrupt backup\n"));
        TestFalse(TEXT("Changed backup cannot clear recovery"), F.Repo.CompleteDiscard(Review, F.Lease).Ok()); F.Write(Payload, TEXT("saved edits\n"));
        const auto Manifest = FPaths::Combine(F.Backup(Review), TEXT("manifest.json")); F.Write(Manifest, TEXT("{}"));
        TestFalse(TEXT("Changed manifest cannot clear recovery"), F.Repo.CompleteDiscard(Review, F.Lease).Ok()); F.Write(Manifest, Marker);
        TestTrue(TEXT("Valid recovery completes"), F.Repo.CompleteDiscard(Review, F.Lease).Ok());
    }
    {
        FDiscardFixture F; F.Write(TEXT("README.md"), TEXT("saved edits\n"));
        const FString Script = TEXT("\"") + F.Git + TEXT("\" \"$@\" || exit $?\nprintf 'external edits\\n' > Docs/other.txt\nexit 0");
        GitWorkspace::FRepository Repo(F.Wrapper(Script), F.Root); const auto Review = Repo.ReviewDiscard({TEXT("README.md")});
        TestFalse(TEXT("Post-write external drift is uncertain"), Repo.ExecuteDiscard(Review, F.Lease, true).Ok());
        TestTrue(TEXT("External drift retains recovery"), IFileManager::Get().FileExists(*F.Marker()));
        TestEqual(TEXT("External edits not rolled back"), F.Read(TEXT("Docs/other.txt")), FString(TEXT("external edits\n")));
        TestFalse(TEXT("Unexpected result cannot complete"), Repo.CompleteDiscard(Review, F.Lease).Ok());
    }
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitDiscardPanelTest, "GitWorkspace.Discard.PanelSelectionPreviewAndCancel", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGitDiscardPanelTest::RunTest(const FString&)
{
    FDiscardFixture F; F.Write(TEXT("README.md"), TEXT("staged\n")); F.Repo.Stage({TEXT("README.md")}); F.Write(TEXT("README.md"), TEXT("working\n")); F.Write(TEXT("new.txt"), TEXT("new\n"));
    auto Repo = MakeShared<GitWorkspace::FRepository, ESPMode::ThreadSafe>(F.Git, F.Root); auto Panel = SNew(SGitWorkspace).Repository(Repo);
    auto Settle = [&]() { while (Panel->Pending.IsValid()) { Panel->Pending.Wait(); Panel->Tick(FGeometry(), 0, 0); } }; Settle();
    Panel->bContentOnly = false; Panel->RebuildRows();
    const auto* Working = Panel->Rows.FindByPredicate([](const auto& Row) { return Row->File.Path == TEXT("README.md") && !Row->bStaged; });
    const auto* Staged = Panel->Rows.FindByPredicate([](const auto& Row) { return Row->File.Path == TEXT("README.md") && Row->bStaged; });
    const auto* New = Panel->Rows.FindByPredicate([](const auto& Row) { return Row->File.Path == TEXT("new.txt"); });
    if (!TestTrue(TEXT("Working, staged and new rows exist"), Working && Staged && New)) return false;
    const auto W = *Working, S = *Staged, N = *New;
    Panel->List->SetSelection(S); TestFalse(TEXT("Staged row does not offer destructive discard"), Panel->CanDiscardSelected());
    Panel->List->SetSelection(N); TestFalse(TEXT("New file does not offer discard"), Panel->CanDiscardSelected());
    Panel->List->SetSelection(W); TestTrue(TEXT("Working modification enables review"), Panel->CanDiscardSelected());
    const auto Before = Repo->Refresh(); Panel->ShowDiscardReview(); Settle();
    TestTrue(TEXT("Panel publishes eligible preview"), Panel->DiscardReview.IsFresh() && Panel->CanRunDiscard());
    TestTrue(TEXT("Preview distinguishes staging and working"), Panel->DiscardReport->GetText().ToString().Contains(TEXT("A stays staged and B is discarded")));
    {
        // Unattended confirmation returns the explicit default No.
        TGuardValue<bool> Unattended(GIsRunningUnattendedScript, true);
        Panel->RunDiscard();
        TestEqual(TEXT("One click reaches confirmation and defaults No"), Panel->Feedback,
            FString(TEXT("Discard cancelled. Working files, staging and locks retained.")));
    }
    TestEqual(TEXT("Cancel preserves working"), F.Read(TEXT("README.md")), FString(TEXT("working\n"))); TestTrue(TEXT("Cancel preserves index"), Repo->Refresh().IndexEntries == Before.IndexEntries);
    TestFalse(TEXT("Cancel creates no recovery entry"), IFileManager::Get().DirectoryExists(*F.Backup(Panel->DiscardReview)));
    Panel->DiscardReview.Capture.ReviewedSeconds -= 301; TestFalse(TEXT("Expired preview disables discard"), Panel->CanRunDiscard()); Panel->RefreshDiscardReview(); Settle(); TestTrue(TEXT("Refresh restores review"), Panel->CanRunDiscard());
    return true;
}
#endif
