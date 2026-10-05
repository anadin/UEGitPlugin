// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#if WITH_DEV_AUTOMATION_TESTS && PLATFORM_MAC
#include "GitWorkspaceRepository.h"
#include "GitWorkspaceSession.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformProcess.h"
namespace
{
struct FStashFixture
{
    FString Root = FPaths::Combine(FPlatformProcess::UserTempDir(), TEXT("uegit-stash-test-") + FGuid::NewGuid().ToString(EGuidFormats::Digits));
    FString Git = GitWorkspace::FindGitExecutable();
    GitWorkspace::FRepository Repo {Git, Root};
    GitWorkspaceSession::FLease Lease;
    FStashFixture()
    {
        IFileManager::Get().MakeDirectory(*Root, true);
        Call({TEXT("init"), TEXT("-q")});
        Call({TEXT("config"), TEXT("user.name"), TEXT("Stash fixture")}); Call({TEXT("config"), TEXT("user.email"), TEXT("fixture@example.invalid")});
        Call({TEXT("config"), TEXT("commit.gpgsign"), TEXT("false")}); Call({TEXT("config"), TEXT("core.autocrlf"), TEXT("false")});
        Call({TEXT("config"), TEXT("core.hooksPath"), TEXT(".git/test-hooks")});
        Call({TEXT("lfs"), TEXT("install"), TEXT("--local"), TEXT("--skip-repo")});
        Write(TEXT("README.md"), TEXT("base\n")); Write(TEXT(".gitignore"), TEXT("ignored.txt\n"));
        Call({TEXT("add"), TEXT(".")}); Call({TEXT("commit"), TEXT("-qm"), TEXT("base")});
        FString Error; Lease.Acquire(Root, true, Error);
    }
    ~FStashFixture() { Lease.Release(); IFileManager::Get().DeleteDirectory(*Root, false, true); }
    GitWorkspace::FResult Call(const TArray<FString>& Args) { return GitWorkspace::Run(Git, Root, Args); }
    void Write(const FString& Path, const FString& Value)
    {
        const FString Full = FPaths::Combine(Root, Path); IFileManager::Get().MakeDirectory(*FPaths::GetPath(Full), true);
        FPlatformFileManager::Get().GetPlatformFile().SetReadOnly(*Full, false);
        FFileHelper::SaveStringToFile(Value, *Full, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
    }
    FString Read(const FString& Path) { FString Value; FFileHelper::LoadFileToString(Value, *FPaths::Combine(Root, Path)); return Value; }
    FString Marker() { FString Canonical, GitDir; GitWorkspaceSession::FindRepository(Root, Canonical, GitDir); return GitWorkspaceSession::RecoveryFile(GitDir); }
    bool Complete(const GitWorkspace::FStashReview& Review, FString& Error)
    {
        auto R = Repo.ExecuteStash(Review, TEXT("Feature material pass"), Lease);
        if (R.Ok()) R = Repo.CompleteStash(Review, Lease);
        Error = R.Error; return R.Ok();
    }
};
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitStashRoundtripTest, "GitWorkspace.Stash.RoundtripAndLiteralPaths", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGitStashRoundtripTest::RunTest(const FString&)
{
    FStashFixture F; FString Error;
    const FString Odd = TEXT("Docs/é [literal]\nname.txt");
    F.Write(Odd, TEXT("original\n")); F.Call({TEXT("add"), TEXT(".")}); F.Call({TEXT("commit"), TEXT("-qm"), TEXT("literal name")});
    F.Write(TEXT("README.md"), TEXT("staged\n")); F.Call({TEXT("add"), TEXT("README.md")}); F.Write(TEXT("README.md"), TEXT("working\n"));
    F.Write(Odd, TEXT("changed\n")); F.Write(TEXT("untracked.txt"), TEXT("keep me\n")); F.Write(TEXT("ignored.txt"), TEXT("also keep\n"));
    const auto Before = F.Repo.Refresh();
    const auto Review = F.Repo.ReviewStash();
    if (!TestTrue(TEXT("Review exact tracked versions: ") + Review.Error, Review.IsFresh())) return false;
    TestTrue(TEXT("Preview leaves index alone"), F.Repo.Refresh().IndexEntries == Before.IndexEntries);
    TestEqual(TEXT("Preview leaves working content alone"), F.Read(TEXT("README.md")), FString(TEXT("working\n")));
    TestTrue(TEXT("Control character displayed safely"), Review.Text.Contains(TEXT("\\nname.txt")));
    if (!TestTrue(TEXT("Create: ") + Error, F.Complete(Review, Error))) { AddError(Error); return false; }
    TestEqual(TEXT("Original HEAD retained"), F.Repo.Refresh().Head, Before.Head);
    TestEqual(TEXT("Tracked file restored"), F.Read(TEXT("README.md")), FString(TEXT("base\n")));
    TestEqual(TEXT("Untracked preserved"), F.Read(TEXT("untracked.txt")), FString(TEXT("keep me\n")));
    TestEqual(TEXT("Ignored preserved"), F.Read(TEXT("ignored.txt")), FString(TEXT("also keep\n")));
    const auto List = F.Repo.ListStashes();
    if (!TestTrue(TEXT("Named stash is listed"), List.bValid && List.Entries.Num() == 1)) return false;
    TestEqual(TEXT("Immutable reviewed object stored"), List.Entries[0].Oid, Review.Oid);
    TestTrue(TEXT("Name listed"), List.Entries[0].Label.Contains(TEXT("Feature material pass")));
    TestFalse(TEXT("Untracked work blocks apply"), F.Repo.ReviewStash(Review.Oid).bValid);
    IFileManager::Get().Delete(*FPaths::Combine(F.Root, TEXT("untracked.txt")));
    const auto Apply = F.Repo.ReviewStash(Review.Oid);
    if (!TestTrue(TEXT("Apply exact staged/working versions"), F.Complete(Apply, Error))) { AddError(Error); return false; }
    TestEqual(TEXT("Work version restored"), F.Read(TEXT("README.md")), FString(TEXT("working\n")));
    TestEqual(TEXT("Index version restored"), F.Call({TEXT("show"), TEXT(":README.md")}).Text(), FString(TEXT("staged\n")));
    TestEqual(TEXT("Literal path restored"), F.Read(Odd), FString(TEXT("changed\n")));
    TestEqual(TEXT("Apply retains stash"), F.Repo.ListStashes().Entries.Num(), 1);
    TestFalse(TEXT("Recovery marker cleared"), IFileManager::Get().FileExists(*F.Marker()));
    F.Call({TEXT("reset"), TEXT("--hard"), TEXT("HEAD")}); // Disposable fixture only.
    const auto WithoutIndex = F.Repo.ReviewStash(Review.Oid, false);
    if (!TestTrue(TEXT("Apply without restoring staging"), F.Complete(WithoutIndex, Error))) { AddError(Error); return false; }
    TestTrue(TEXT("Index remains at base"), F.Call({TEXT("diff"), TEXT("--cached"), TEXT("--quiet")}).Ok());
    TestEqual(TEXT("Working content still restored"), F.Read(TEXT("README.md")), FString(TEXT("working\n")));
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitStashGuardsTest, "GitWorkspace.Stash.StaleUnsupportedAndRecovery", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGitStashGuardsTest::RunTest(const FString&)
{
    FStashFixture F; FString Error;
    F.Write(TEXT("README.md"), TEXT("reviewed\n")); auto Review = F.Repo.ReviewStash();
    F.Write(TEXT("README.md"), TEXT("new work\n"));
    TestFalse(TEXT("Stale working content refused"), F.Repo.ExecuteStash(Review, TEXT("stale"), F.Lease).Ok());
    TestEqual(TEXT("New work retained"), F.Read(TEXT("README.md")), FString(TEXT("new work\n")));
    TestTrue(TEXT("No stash ref created by preview"), F.Repo.ListStashes().Entries.IsEmpty());
    Review = F.Repo.ReviewStash(); auto Forged = Review; Forged.Changes.Empty();
    TestFalse(TEXT("Omitted package review refused"), F.Repo.ExecuteStash(Forged, TEXT("forged"), F.Lease).Ok());
    F.Call({TEXT("config"), TEXT("uegit.fixture"), TEXT("changed")});
    TestFalse(TEXT("Stale configuration refused"), F.Repo.ExecuteStash(Review, TEXT("stale config"), F.Lease).Ok());
    F.Write(TEXT("Source/Code.cpp"), TEXT("source\n")); F.Call({TEXT("add"), TEXT("Source/Code.cpp")});
    TestFalse(TEXT("Code changes blocked"), F.Repo.ReviewStash().bValid);
    F.Call({TEXT("reset"), TEXT("--hard"), TEXT("HEAD")});
    F.Write(TEXT("Content/New.uasset"), TEXT("new asset")); F.Call({TEXT("add"), TEXT("Content/New.uasset")});
    TestFalse(TEXT("New assets blocked"), F.Repo.ReviewStash().bValid);
    F.Call({TEXT("reset"), TEXT("--hard"), TEXT("HEAD")});
    F.Call({TEXT("rm"), TEXT("README.md")}); F.Write(TEXT("README.md"), TEXT("untracked replacement\n"));
    TestFalse(TEXT("Untracked replacement of staged deletion protected"), F.Repo.ReviewStash().bValid);
    F.Call({TEXT("reset"), TEXT("--hard"), TEXT("HEAD")});
    F.Write(TEXT("README.md"), TEXT("preserve\n")); Review = F.Repo.ReviewStash();
    auto Executed = F.Repo.ExecuteStash(Review, TEXT("retained for recovery"), F.Lease);
    if (!TestTrue(TEXT("Execute writes recovery marker: ") + Executed.Error, Executed.Ok())) return false;
    TestTrue(TEXT("Marker exists until reload completion"), IFileManager::Get().FileExists(*F.Marker()));
    F.Write(TEXT("README.md"), TEXT("unexpected postflight work\n"));
    TestFalse(TEXT("Uncertain completion fails"), F.Repo.CompleteStash(Review, F.Lease).Ok());
    TestTrue(TEXT("Recovery retained"), IFileManager::Get().FileExists(*F.Marker()));
    TestEqual(TEXT("Stash retained"), F.Repo.ListStashes().Entries[0].Oid, Review.Oid);
    TestEqual(TEXT("No automatic rollback"), F.Read(TEXT("README.md")), FString(TEXT("unexpected postflight work\n")));
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitStashLfsTest, "GitWorkspace.Stash.LfsSplitVersionsAndExternalStashes", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGitStashLfsTest::RunTest(const FString&)
{
    FStashFixture F; FString Error;
    F.Write(TEXT(".gitattributes"), TEXT("*.uasset filter=lfs diff=lfs merge=lfs -text\n"));
    F.Write(TEXT("Content/Probe.uasset"), TEXT("base payload\n")); F.Call({TEXT("add"), TEXT(".")}); F.Call({TEXT("commit"), TEXT("-qm"), TEXT("LFS base")});
    F.Write(TEXT("Content/Probe.uasset"), TEXT("staged payload\n")); F.Call({TEXT("add"), TEXT(".")});
    const auto Index = F.Repo.Refresh().IndexEntries;
    F.Write(TEXT("Content/Probe.uasset"), TEXT("working payload\n"));
    F.Call({TEXT("config"), TEXT("filter.lfs.process"), TEXT("git-lfs filter-process --skip")});
    const auto Review = F.Repo.ReviewStash();
    if (!TestTrue(TEXT("Create LFS stash: ") + Review.Error, F.Complete(Review, Error))) { AddError(Error); return false; }
    TestEqual(TEXT("Cleanup hydrates baseline despite skip config"), F.Read(TEXT("Content/Probe.uasset")), FString(TEXT("base payload\n")));
    const auto Apply = F.Repo.ReviewStash(Review.Oid);
    if (!TestTrue(TEXT("Apply LFS stash"), F.Complete(Apply, Error))) { AddError(Error); return false; }
    TestEqual(TEXT("Hydrates saved working version, not staged version"), F.Read(TEXT("Content/Probe.uasset")), FString(TEXT("working payload\n")));
    TestTrue(TEXT("LFS index keeps distinct staged version"), F.Repo.Refresh().IndexEntries == Index);
    TestEqual(TEXT("Skip config unchanged"), F.Call({TEXT("config"), TEXT("filter.lfs.process")}).Text().TrimEnd(), FString(TEXT("git-lfs filter-process --skip")));
    F.Call({TEXT("reset"), TEXT("--hard"), TEXT("HEAD")});
    F.Write(TEXT("outside.txt"), TEXT("untracked\n")); F.Call({TEXT("stash"), TEXT("push"), TEXT("--include-untracked"), TEXT("-m"), TEXT("external")});
    const auto ThreeParents = F.Repo.ListStashes();
    TestFalse(TEXT("External stash with untracked parent blocked"), F.Repo.ReviewStash(ThreeParents.Entries[0].Oid).bValid);
    F.Write(TEXT("README.md"), TEXT("new base\n")); F.Call({TEXT("add"), TEXT("README.md")}); F.Call({TEXT("commit"), TEXT("-qm"), TEXT("different base")});
    TestFalse(TEXT("Cross-commit stash apply blocked"), F.Repo.ReviewStash(Review.Oid).bValid);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitStashInspectDropTest, "GitWorkspace.Stash.InspectAndDropWithDirtyWork", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGitStashInspectDropTest::RunTest(const FString&)
{
    FStashFixture F;
    F.Write(TEXT("README.md"), TEXT("first stash\n")); F.Call({TEXT("stash"), TEXT("push"), TEXT("-m"), TEXT("first")});
    const FString First = F.Call({TEXT("rev-parse"), TEXT("refs/stash")}).Text().TrimEnd();
    F.Write(TEXT("Source/External.cpp"), TEXT("external source\n")); F.Write(TEXT("Content/New.umap"), TEXT("external map\n"));
    F.Write(TEXT("README.md"), TEXT("second stash\n")); F.Call({TEXT("stash"), TEXT("push"), TEXT("--include-untracked"), TEXT("-m"), TEXT("external with untracked")});
    const FString Second = F.Call({TEXT("rev-parse"), TEXT("refs/stash")}).Text().TrimEnd();
    F.Write(TEXT("README.md"), TEXT("staged now\n")); F.Call({TEXT("add"), TEXT("README.md")}); F.Write(TEXT("README.md"), TEXT("working now\n"));
    F.Write(TEXT("untracked.txt"), TEXT("local untracked\n")); F.Write(TEXT("ignored.txt"), TEXT("local ignored\n"));
    const auto Before = F.Repo.Refresh();
    const auto Inspect = F.Repo.InspectStash(Second, TEXT("stash@{0}"));
    if (!TestTrue(TEXT("Inspect external stash despite dirty work: ") + Inspect.Error, Inspect.IsFresh())) return false;
    TestTrue(TEXT("Untracked paths disclosed"), Inspect.Text.Contains(TEXT("SAVED UNTRACKED FILES")) && Inspect.Text.Contains(TEXT("Source/External.cpp")) && Inspect.Text.Contains(TEXT("Content/New.umap")));
    TestTrue(TEXT("Both tracked trees disclosed"), Inspect.Text.Contains(TEXT("SAVED STAGING")) && Inspect.Text.Contains(TEXT("SAVED WORKING CHANGES")));
    TestFalse(TEXT("Apply is separately blocked"), F.Repo.ReviewStash(Second).IsFresh());
    const auto Dropped = F.Repo.DropStash(Inspect, F.Lease);
    if (!TestTrue(TEXT("Explicit Drop succeeds with dirty work: ") + Dropped.Error, Dropped.Ok())) return false;
    const auto Remaining = F.Repo.ListStashes();
    TestTrue(TEXT("Only selected entry removed"), Remaining.bValid && Remaining.Entries.Num() == 1 && Remaining.Entries[0].Oid == First);
    TestEqual(TEXT("HEAD preserved"), F.Repo.Refresh().Head, Before.Head);
    TestTrue(TEXT("Staging preserved"), F.Repo.Refresh().IndexEntries == Before.IndexEntries);
    TestEqual(TEXT("Working bytes retained"), F.Read(TEXT("README.md")), FString(TEXT("working now\n")));
    TestEqual(TEXT("Untracked bytes retained"), F.Read(TEXT("untracked.txt")), FString(TEXT("local untracked\n")));
    TestEqual(TEXT("Ignored bytes retained"), F.Read(TEXT("ignored.txt")), FString(TEXT("local ignored\n")));
    const auto Recovery = F.Call({TEXT("for-each-ref"), TEXT("--format=%(objectname) %(refname)"), TEXT("refs/uegit/stash-drop/")});
    TestTrue(TEXT("Both reviewed stashes remain reachable for recovery"), Recovery.Ok() && Recovery.Text().Contains(First) && Recovery.Text().Contains(Second));
    TestTrue(TEXT("Recovery report disclosed"), Dropped.Text().Contains(TEXT(".json")));
    const auto Last = F.Repo.InspectStash(First, TEXT("stash@{0}"));
    TestTrue(TEXT("Last entry can be explicitly dropped"), F.Repo.DropStash(Last, F.Lease).Ok());
    TestTrue(TEXT("Empty stash list valid"), F.Repo.ListStashes().bValid && F.Repo.ListStashes().Entries.IsEmpty());
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitStashDropIdentityTest, "GitWorkspace.Stash.DropIdentityExpiryAndRecovery", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGitStashDropIdentityTest::RunTest(const FString&)
{
    FStashFixture F;
    F.Write(TEXT("README.md"), TEXT("snapshot\n")); F.Call({TEXT("stash"), TEXT("push"), TEXT("-m"), TEXT("original")});
    const FString Oid = F.Call({TEXT("rev-parse"), TEXT("refs/stash")}).Text().TrimEnd();
    const auto Old = F.Repo.InspectStash(Oid, TEXT("stash@{0}"));
    F.Write(TEXT("README.md"), TEXT("intermediate snapshot\n"));
    F.Call({TEXT("stash"), TEXT("push"), TEXT("-m"), TEXT("middle")});
    F.Call({TEXT("stash"), TEXT("store"), TEXT("-m"), TEXT("duplicate snapshot"), Oid});
    TestEqual(TEXT("Duplicate object separated by another stash"), F.Repo.ListStashes().Entries.Num(), 3);
    TestFalse(TEXT("Ambiguous object alone is not a Drop selection"), F.Repo.InspectStash(Oid).IsFresh());
    TestFalse(TEXT("Reordered list invalidates reviewed Drop"), F.Repo.DropStash(Old, F.Lease).Ok());
    auto Selected = F.Repo.InspectStash(Oid, TEXT("stash@{2}"));
    auto Expired = Selected; Expired.ReviewedSeconds -= 301;
    TestFalse(TEXT("Expired inspection refused"), F.Repo.DropStash(Expired, F.Lease).Ok());
    auto Forged = Selected; Forged.Entry.Label = TEXT("wrong label");
    TestFalse(TEXT("Wrong entry metadata refused"), F.Repo.DropStash(Forged, F.Lease).Ok());
    IFileManager::Get().MakeDirectory(*FPaths::GetPath(F.Marker()), true);
    FFileHelper::SaveStringToFile(TEXT("active asset recovery"), *F.Marker());
    const auto RecoveryBlocked = F.Repo.InspectStash(Oid, TEXT("stash@{2}"));
    TestTrue(TEXT("Inspection works during recovery"), RecoveryBlocked.IsFresh());
    TestFalse(TEXT("Drop disabled during asset recovery"), RecoveryBlocked.DropBlocker.IsEmpty());
    TestFalse(TEXT("Recovery created after review still blocks"), F.Repo.DropStash(Selected, F.Lease).Ok());
    IFileManager::Get().Delete(*F.Marker());
    Selected = F.Repo.InspectStash(Oid, TEXT("stash@{2}"));
    const auto Result = F.Repo.DropStash(Selected, F.Lease);
    if (!TestTrue(TEXT("Exact duplicate entry removed: ") + Result.Error, Result.Ok())) return false;
    const auto Remaining = F.Repo.ListStashes();
    TestTrue(TEXT("Other duplicate entry retained"), Remaining.Entries.Num() == 2 && Remaining.Entries[0].Label == TEXT("duplicate snapshot"));
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitStashDropRaceTest, "GitWorkspace.Stash.DropConcurrentChangePreservesRecovery", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGitStashDropRaceTest::RunTest(const FString&)
{
    FStashFixture F;
    F.Write(TEXT("README.md"), TEXT("first\n")); F.Call({TEXT("stash"), TEXT("push"), TEXT("-m"), TEXT("first")});
    const FString First = F.Call({TEXT("rev-parse"), TEXT("refs/stash")}).Text().TrimEnd();
    F.Write(TEXT("README.md"), TEXT("second\n")); F.Call({TEXT("stash"), TEXT("push"), TEXT("-m"), TEXT("second")});
    const FString Second = F.Call({TEXT("rev-parse"), TEXT("refs/stash")}).Text().TrimEnd();
    // Inject an external client inserting a stash after the final review and
    // before Git resolves the ordinal. No production test hooks are needed.
    const FString Wrapper = FPaths::Combine(F.Root, TEXT(".git/racing-git"));
    const FString Script = TEXT("#!/bin/sh\ncase \" $* \" in *' stash drop -- '*) '") + F.Git + TEXT("' stash store -m concurrent ") + First + TEXT(" || exit 1 ;; esac\nexec '") + F.Git + TEXT("' \"$@\"\n");
    FFileHelper::SaveStringToFile(Script, *Wrapper, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
    FPlatformFileManager::Get().GetPlatformFile().SetReadOnly(*Wrapper, false);
    GitWorkspace::Run(TEXT("/bin/chmod"), F.Root, {TEXT("700"), Wrapper});
    GitWorkspace::FRepository Racing(Wrapper, F.Root);
    const auto Review = Racing.InspectStash(First, TEXT("stash@{1}"));
    const auto Result = Racing.DropStash(Review, F.Lease);
    TestFalse(TEXT("Concurrent list change cannot report successful Drop"), Result.Ok());
    TestTrue(TEXT("Uncertain result names recovery report"), Result.Error.Contains(TEXT("pending.json")));
    const auto Refs = F.Call({TEXT("for-each-ref"), TEXT("--format=%(objectname)"), TEXT("refs/uegit/stash-drop/")});
    TestTrue(TEXT("Both original objects preserved, including unexpectedly removed entry"), Refs.Text().Contains(First) && Refs.Text().Contains(Second));
    TestTrue(TEXT("Working files untouched"), F.Repo.Refresh().Files.IsEmpty());
    const auto Blocked = F.Repo.InspectStash(First, TEXT("stash@{0}"));
    TestTrue(TEXT("Inspection stays available"), Blocked.IsFresh());
    TestFalse(TEXT("Pending report blocks another Drop"), Blocked.DropBlocker.IsEmpty());
    const FString Linked = FPaths::Combine(F.Root, TEXT(".git/linked-stash-worktree"));
    if (!TestTrue(TEXT("Create linked-worktree recovery probe"), F.Call({TEXT("worktree"), TEXT("add"), TEXT("--detach"), Linked, TEXT("HEAD")}).Ok())) return false;
    GitWorkspace::FRepository Other(F.Git, Linked);
    const auto OtherInspection = Other.InspectStash(First, TEXT("stash@{0}"));
    TestTrue(TEXT("Linked worktree can inspect shared stash"), OtherInspection.IsFresh());
    TestFalse(TEXT("Pending Drop recovery shared across worktrees"), OtherInspection.DropBlocker.IsEmpty());
    F.Write(TEXT("README.md"), TEXT("new local work\n"));
    TestFalse(TEXT("Pending report blocks stash creation"), F.Repo.ReviewStash().IsFresh());
    return true;
}
#endif
