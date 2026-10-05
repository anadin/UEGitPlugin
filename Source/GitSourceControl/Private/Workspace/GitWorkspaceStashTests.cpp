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
    FString StoredOid() { const auto List = Repo.ListStashes(); return List.Entries.IsEmpty() ? FString() : List.Entries[0].Oid; }
    FString Shape(const FString& Oid) { return Call({TEXT("show"), TEXT("--no-patch"), TEXT("--format=%T%n%P"), Oid, TEXT("--")}).Text(); }
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
    TestEqual(TEXT("Named commit preserves exact reviewed tree and parents"), F.Shape(List.Entries[0].Oid), F.Shape(Review.Oid));
    TestEqual(TEXT("Commit subject matches stash label"), F.Call({TEXT("log"), TEXT("-1"), TEXT("--format=%s"), List.Entries[0].Oid}).Text().TrimEnd(), List.Entries[0].Label);
    TestTrue(TEXT("Name listed"), List.Entries[0].Label.Contains(TEXT("Feature material pass")));
    TestTrue(TEXT("Unrelated untracked work permits apply"), F.Repo.ReviewStash(F.StoredOid()).bValid);
    IFileManager::Get().Delete(*FPaths::Combine(F.Root, TEXT("untracked.txt")));
    const auto Apply = F.Repo.ReviewStash(F.StoredOid());
    if (!TestTrue(TEXT("Apply exact staged/working versions"), F.Complete(Apply, Error))) { AddError(Error); return false; }
    TestEqual(TEXT("Work version restored"), F.Read(TEXT("README.md")), FString(TEXT("working\n")));
    TestEqual(TEXT("Index version restored"), F.Call({TEXT("show"), TEXT(":README.md")}).Text(), FString(TEXT("staged\n")));
    TestEqual(TEXT("Literal path restored"), F.Read(Odd), FString(TEXT("changed\n")));
    TestEqual(TEXT("Apply retains stash"), F.Repo.ListStashes().Entries.Num(), 1);
    TestFalse(TEXT("Recovery marker cleared"), IFileManager::Get().FileExists(*F.Marker()));
    F.Call({TEXT("reset"), TEXT("--hard"), TEXT("HEAD")}); // Disposable fixture only.
    const auto WithoutIndex = F.Repo.ReviewStash(F.StoredOid(), false);
    if (!TestTrue(TEXT("Apply without restoring staging"), F.Complete(WithoutIndex, Error))) { AddError(Error); return false; }
    TestTrue(TEXT("Index remains at base"), F.Call({TEXT("diff"), TEXT("--cached"), TEXT("--quiet")}).Ok());
    TestEqual(TEXT("Working content still restored"), F.Read(TEXT("README.md")), FString(TEXT("working\n")));
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitStashNamesTest, "GitWorkspace.Stash.NamesMatchGitClients", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGitStashNamesTest::RunTest(const FString&)
{
    FStashFixture F;
    TArray<FString> SavedOids, SavedNames;
    for (bool bSelected : {false, true}) for (bool bUntracked : {false, true})
    {
        F.Write(TEXT("README.md"), TEXT("staged version\n")); F.Call({TEXT("add"), TEXT("README.md")});
        F.Write(TEXT("README.md"), TEXT("working version\n"));
        TArray<FString> Paths {TEXT("README.md")};
        if (bUntracked) { F.Write(TEXT("Docs/new.md"), TEXT("new work\n")); Paths.Add(TEXT("Docs/new.md")); }
        const FString Name = FString::Printf(TEXT("Material é [v%d] \"copper\" $(literal);"), SavedNames.Num());
        const auto Review = bSelected ? F.Repo.ReviewSelectedStash(Paths, bUntracked) : F.Repo.ReviewStash(FString(), true, bUntracked);
        if (!TestTrue(TEXT("Named snapshot review: ") + Review.Error, Review.IsFresh())) return false;
        const auto Result = F.Repo.ExecuteStash(Review, Name, F.Lease);
        if (!TestTrue(TEXT("Named snapshot creation: ") + Result.Error, Result.Ok())) return false;
        const FString Oid = F.StoredOid(); SavedOids.Add(Oid); SavedNames.Add(Name);
        TestEqual(TEXT("Only snapshot metadata changes"), F.Shape(Oid), F.Shape(Review.Oid));
        TestEqual(TEXT("Commit message is the literal entered name"), F.Call({TEXT("show"), TEXT("--no-patch"), TEXT("--format=%B"), Oid}).Text().TrimEnd(), Name);
        FString MarkerText; FFileHelper::LoadFileToString(MarkerText, *F.Marker());
        TestTrue(TEXT("Recovery points to the named snapshot"), MarkerText.Contains(TEXT("Preserved stash object: ") + Oid));
        TestTrue(TEXT("Named create completes"), F.Repo.CompleteStash(Review, F.Lease).Ok());
        const auto List = F.Repo.ListStashes();
        TestEqual(TEXT("Previous named stashes retained"), List.Entries.Num(), SavedOids.Num());
        for (int32 I = 0; I < List.Entries.Num(); ++I)
        {
            const int32 Saved = SavedOids.Num() - I - 1;
            TestEqual(TEXT("Prior stash identity preserved"), List.Entries[I].Oid, SavedOids[Saved]);
            TestEqual(TEXT("Reflog name matches commit message"), List.Entries[I].Label, SavedNames[Saved]);
        }
    }
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
    TestTrue(TEXT("Staged new ordinary assets eligible for guarded unloading"), F.Repo.ReviewStash().bValid);
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
    TestEqual(TEXT("Named stash retained with reviewed tree and parents"), F.Shape(F.StoredOid()), F.Shape(Review.Oid));
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
    const FString SavedOid = F.StoredOid();
    TestEqual(TEXT("Cleanup hydrates baseline despite skip config"), F.Read(TEXT("Content/Probe.uasset")), FString(TEXT("base payload\n")));
    const auto Apply = F.Repo.ReviewStash(F.StoredOid());
    if (!TestTrue(TEXT("Apply LFS stash"), F.Complete(Apply, Error))) { AddError(Error); return false; }
    TestEqual(TEXT("Hydrates saved working version, not staged version"), F.Read(TEXT("Content/Probe.uasset")), FString(TEXT("working payload\n")));
    TestTrue(TEXT("LFS index keeps distinct staged version"), F.Repo.Refresh().IndexEntries == Index);
    TestEqual(TEXT("Skip config unchanged"), F.Call({TEXT("config"), TEXT("filter.lfs.process")}).Text().TrimEnd(), FString(TEXT("git-lfs filter-process --skip")));
    F.Call({TEXT("reset"), TEXT("--hard"), TEXT("HEAD")});
    F.Write(TEXT("outside.txt"), TEXT("untracked\n")); F.Call({TEXT("stash"), TEXT("push"), TEXT("--include-untracked"), TEXT("-m"), TEXT("external")});
    const auto ThreeParents = F.Repo.ListStashes();
    TestFalse(TEXT("External stash with untracked parent blocked"), F.Repo.ReviewStash(ThreeParents.Entries[0].Oid).bValid);
    F.Write(TEXT("README.md"), TEXT("new base\n")); F.Call({TEXT("add"), TEXT("README.md")}); F.Call({TEXT("commit"), TEXT("-qm"), TEXT("different base")});
    TestFalse(TEXT("Cross-commit stash apply blocked"), F.Repo.ReviewStash(SavedOid).bValid);
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
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitSelectedStashTest, "GitWorkspace.Stash.SelectedSplitVersionsAndRename", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGitSelectedStashTest::RunTest(const FString&)
{
    FStashFixture F; FString Error;
    const FString Odd = TEXT("Docs/é [selected]\nname.txt"), Renamed = TEXT("Docs/renamed.txt");
    F.Write(Odd, TEXT("old name\n")); F.Write(TEXT("Source/Keep.cpp"), TEXT("source baseline\n"));
    F.Call({TEXT("add"), TEXT(".")}); F.Call({TEXT("commit"), TEXT("-qm"), TEXT("selection baseline")});
    F.Write(TEXT("README.md"), TEXT("selected index\n")); F.Write(TEXT("Source/Keep.cpp"), TEXT("excluded index\n"));
    F.Call({TEXT("add"), TEXT(".")});
    F.Write(TEXT("README.md"), TEXT("selected working\n")); F.Write(TEXT("Source/Keep.cpp"), TEXT("excluded working\n"));
    F.Call({TEXT("mv"), Odd, Renamed});
    F.Write(TEXT("untracked.txt"), TEXT("untracked keep\n")); F.Write(TEXT("ignored.txt"), TEXT("ignored keep\n"));
    const auto Before = F.Repo.Refresh();
    TestFalse(TEXT("Whole-repository stash refuses source changes"), F.Repo.ReviewStash().IsFresh());
    const auto Review = F.Repo.ReviewSelectedStash({TEXT("README.md"), Renamed, TEXT("README.md")});
    if (!TestTrue(TEXT("Review selected safe paths: ") + Review.Error, Review.IsFresh())) return false;
    TestEqual(TEXT("Duplicate selection deduplicated and rename pair expanded"), Review.SelectedPaths.Num(), 3);
    TestTrue(TEXT("Selection includes original rename path"), Review.SelectedPaths.Contains(Odd));
    TestTrue(TEXT("Exact selection shown safely"), Review.Text.Contains(TEXT("SELECTED FILES")) && Review.Text.Contains(TEXT("\\nname.txt")));
    TestTrue(TEXT("Preview leaves real index alone"), F.Repo.Refresh().IndexEntries == Before.IndexEntries);
    TestEqual(TEXT("Unselected working change absent from stash tree"), F.Call({TEXT("show"), Review.Oid + TEXT(":Source/Keep.cpp")}).Text(), FString(TEXT("source baseline\n")));
    TestEqual(TEXT("Unselected staging absent from stash index"), F.Call({TEXT("show"), Review.IndexCommit + TEXT(":Source/Keep.cpp")}).Text(), FString(TEXT("source baseline\n")));
    if (!TestTrue(TEXT("Selected creation"), F.Complete(Review, Error))) { AddError(Error); return false; }
    TestEqual(TEXT("Selected work cleaned"), F.Read(TEXT("README.md")), FString(TEXT("base\n")));
    TestEqual(TEXT("Rename reverted only for selected pair"), F.Read(Odd), FString(TEXT("old name\n")));
    TestFalse(TEXT("Selected rename target removed"), IFileManager::Get().FileExists(*FPaths::Combine(F.Root, Renamed)));
    TestEqual(TEXT("Excluded index preserved"), F.Call({TEXT("show"), TEXT(":Source/Keep.cpp")}).Text(), FString(TEXT("excluded index\n")));
    TestEqual(TEXT("Excluded working version preserved"), F.Read(TEXT("Source/Keep.cpp")), FString(TEXT("excluded working\n")));
    TestEqual(TEXT("Untracked preserved"), F.Read(TEXT("untracked.txt")), FString(TEXT("untracked keep\n")));
    TestEqual(TEXT("Ignored preserved"), F.Read(TEXT("ignored.txt")), FString(TEXT("ignored keep\n")));
    TestEqual(TEXT("Exact reviewed stash trees and parents stored"), F.Shape(F.StoredOid()), F.Shape(Review.Oid));
    // Resolve excluded work only in this disposable fixture before testing Apply.
    F.Call({TEXT("reset"), TEXT("--hard"), TEXT("HEAD")}); IFileManager::Get().Delete(*FPaths::Combine(F.Root, TEXT("untracked.txt")));
    if (!TestTrue(TEXT("Apply selected-only snapshot"), F.Complete(F.Repo.ReviewStash(F.StoredOid()), Error))) { AddError(Error); return false; }
    TestEqual(TEXT("Selected staged version restored"), F.Call({TEXT("show"), TEXT(":README.md")}).Text(), FString(TEXT("selected index\n")));
    TestEqual(TEXT("Selected working version restored"), F.Read(TEXT("README.md")), FString(TEXT("selected working\n")));
    TestEqual(TEXT("Unselected source not reapplied"), F.Read(TEXT("Source/Keep.cpp")), FString(TEXT("source baseline\n")));
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitSelectedStashGuardTest, "GitWorkspace.Stash.SelectedGuardsAndRecovery", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGitSelectedStashGuardTest::RunTest(const FString&)
{
    FStashFixture F;
    F.Write(TEXT("Docs/keep.txt"), TEXT("base\n")); F.Call({TEXT("add"), TEXT(".")}); F.Call({TEXT("commit"), TEXT("-qm"), TEXT("base keep")});
    F.Write(TEXT("README.md"), TEXT("selected\n")); F.Write(TEXT("Docs/keep.txt"), TEXT("excluded\n")); F.Write(TEXT("untracked.txt"), TEXT("new\n"));
    TestFalse(TEXT("Empty selection never means whole repository"), F.Repo.ReviewSelectedStash({}).IsFresh());
    F.Write(TEXT(".gitattributes"), TEXT("*.md filter=lfs diff=lfs merge=lfs -text\n"));
    TestFalse(TEXT("Unselected attribute changes cannot alter selected capture semantics"), F.Repo.ReviewSelectedStash({TEXT("README.md")}).IsFresh());
    IFileManager::Get().Delete(*FPaths::Combine(F.Root, TEXT(".gitattributes")));
    for (const FString& Path : {FString(), FString(TEXT("Docs")), FString(TEXT("../README.md")), FString(TEXT("untracked.txt")), FString(TEXT(".gitignore"))})
        TestFalse(TEXT("Invalid selection refused: ") + Path, F.Repo.ReviewSelectedStash({Path}).IsFresh());
    auto Review = F.Repo.ReviewSelectedStash({TEXT("README.md")});
    auto Forged = Review; Forged.ExpectedIndexTree = Review.WorkingTree;
    TestFalse(TEXT("Forged cleanup tree refused"), F.Repo.ExecuteStash(Forged, TEXT("forged"), F.Lease).Ok());
    Forged = Review; Forged.SelectedPaths.Add(TEXT("Docs/keep.txt"));
    TestFalse(TEXT("Expanded unreviewed selection refused"), F.Repo.ExecuteStash(Forged, TEXT("forged paths"), F.Lease).Ok());
    F.Write(TEXT("Docs/keep.txt"), TEXT("changed after review\n"));
    TestFalse(TEXT("Unselected work changed after review blocks execution"), F.Repo.ExecuteStash(Review, TEXT("stale"), F.Lease).Ok());
    TestTrue(TEXT("Refusal publishes no stash"), F.Repo.ListStashes().Entries.IsEmpty());
    Review = F.Repo.ReviewSelectedStash({TEXT("README.md")});
    const auto Execute = F.Repo.ExecuteStash(Review, TEXT("selected recovery"), F.Lease);
    if (!TestTrue(TEXT("Execute selected: ") + Execute.Error, Execute.Ok())) return false;
    F.Write(TEXT("Docs/keep.txt"), TEXT("unexpected change after cleanup\n"));
    TestFalse(TEXT("Unselected postflight change is detected"), F.Repo.CompleteStash(Review, F.Lease).Ok());
    TestTrue(TEXT("Recovery marker retained"), IFileManager::Get().FileExists(*F.Marker()));
    TestEqual(TEXT("Unexpected work preserved without rollback"), F.Read(TEXT("Docs/keep.txt")), FString(TEXT("unexpected change after cleanup\n")));
    TestEqual(TEXT("Selected snapshot retained for recovery"), F.Shape(F.StoredOid()), F.Shape(Review.Oid));
    FString MarkerText; FFileHelper::LoadFileToString(MarkerText, *F.Marker());
    TestTrue(TEXT("Recovery identifies the final named object"), MarkerText.Contains(TEXT("Preserved stash object: ") + F.StoredOid()));
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitSelectedStashLfsTest, "GitWorkspace.Stash.SelectedLfsPreservesExcludedVersions", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGitSelectedStashLfsTest::RunTest(const FString&)
{
    FStashFixture F; FString Error;
    F.Write(TEXT(".gitattributes"), TEXT("*.uasset filter=lfs diff=lfs merge=lfs -text\n"));
    for (const FString& Path : {FString(TEXT("Content/Selected.uasset")), FString(TEXT("Content/Keep.uasset"))}) F.Write(Path, TEXT("base payload\n"));
    F.Call({TEXT("add"), TEXT(".")}); F.Call({TEXT("commit"), TEXT("-qm"), TEXT("asset base")});
    F.Write(TEXT("Content/Selected.uasset"), TEXT("selected index\n")); F.Write(TEXT("Content/Keep.uasset"), TEXT("excluded index\n")); F.Call({TEXT("add"), TEXT(".")});
    const auto ExcludedIndex = F.Call({TEXT("show"), TEXT(":Content/Keep.uasset")}).Out;
    F.Write(TEXT("Content/Selected.uasset"), TEXT("selected work\n")); F.Write(TEXT("Content/Keep.uasset"), TEXT("excluded work\n"));
    F.Call({TEXT("config"), TEXT("filter.lfs.process"), TEXT("git-lfs filter-process --skip")});
    const auto Review = F.Repo.ReviewSelectedStash({TEXT("Content/Selected.uasset")});
    if (!TestTrue(TEXT("Selected LFS creation: ") + Review.Error, F.Complete(Review, Error))) { AddError(Error); return false; }
    TestEqual(TEXT("Selected LFS hydrated to base"), F.Read(TEXT("Content/Selected.uasset")), FString(TEXT("base payload\n")));
    TestEqual(TEXT("Excluded LFS working payload preserved"), F.Read(TEXT("Content/Keep.uasset")), FString(TEXT("excluded work\n")));
    TestTrue(TEXT("Excluded distinct LFS index preserved"), F.Call({TEXT("show"), TEXT(":Content/Keep.uasset")}).Out == ExcludedIndex);
    TestEqual(TEXT("Only one affected asset"), Review.Changes.Num(), 1);
    TestFalse(TEXT("Successful selected stash clears marker"), IFileManager::Get().FileExists(*F.Marker()));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitStashUntrackedTest, "GitWorkspace.Stash.UntrackedLfsRoundtripAndGuards", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGitStashUntrackedTest::RunTest(const FString&)
{
    FStashFixture F; FString Error;
    F.Write(TEXT(".gitattributes"), TEXT("*.uasset filter=lfs diff=lfs merge=lfs -text lockable\n"));
    F.Call({TEXT("add"), TEXT(".")}); F.Call({TEXT("commit"), TEXT("-qm"), TEXT("LFS policy")});
    const FString Asset = TEXT("Content/New [asset].uasset"), Doc = TEXT("Docs/new é\nname.md");
    F.Write(Asset, TEXT("new saved asset payload\n")); F.Write(Doc, TEXT("new document\n")); F.Write(TEXT("ignored.txt"), TEXT("keep ignored\n"));
    const auto Before = F.Repo.Refresh();
    TestFalse(TEXT("Untracked-only stash requires opt-in"), F.Repo.ReviewStash().bValid);
    auto Review = F.Repo.ReviewStash(FString(), true, true);
    if (!TestTrue(TEXT("Review untracked-only work: ") + Review.Error, Review.IsFresh())) return false;
    TestEqual(TEXT("Only non-ignored paths captured"), Review.UntrackedPaths.Num(), 2);
    TestTrue(TEXT("Review preserves real index"), F.Repo.Refresh().IndexEntries == Before.IndexEntries);
    TestTrue(TEXT("Third parent uses LFS pointer"), F.Call({TEXT("show"), Review.Oid + TEXT("^3:") + Asset}).Text().StartsWith(TEXT("version https://git-lfs.github.com/spec/v1")));
    auto Forged = Review; Forged.UntrackedCommit.Empty();
    TestFalse(TEXT("Omitted untracked parent refused before cleanup"), F.Repo.ExecuteStash(Forged, TEXT("bad parent"), F.Lease).Ok());
    F.Write(Asset, TEXT("newer bytes\n"));
    TestFalse(TEXT("Saved bytes changed after review refused"), F.Repo.ExecuteStash(Review, TEXT("stale bytes"), F.Lease).Ok());
    TestEqual(TEXT("Newer untracked bytes preserved"), F.Read(Asset), FString(TEXT("newer bytes\n")));
    TestTrue(TEXT("No stash stored on refusal"), F.Repo.ListStashes().Entries.IsEmpty());
    F.Call({TEXT("config"), TEXT("filter.lfs.process"), TEXT("git-lfs filter-process --skip")});
    Review = F.Repo.ReviewStash(FString(), true, true);
    if (!TestTrue(TEXT("Create untracked stash: ") + Review.Error, F.Complete(Review, Error))) { AddError(Error); return false; }
    TestFalse(TEXT("Captured asset removed"), IFileManager::Get().FileExists(*FPaths::Combine(F.Root, Asset)));
    TestFalse(TEXT("Captured documentation removed"), IFileManager::Get().FileExists(*FPaths::Combine(F.Root, Doc)));
    TestEqual(TEXT("Ignored file retained"), F.Read(TEXT("ignored.txt")), FString(TEXT("keep ignored\n")));
    TestTrue(TEXT("Index and worktree clean after untracked capture"), F.Repo.Refresh().Files.IsEmpty());
    // A new ignore rule can hide a collision from ordinary status. Keep it out
    // of repository configuration so review reflects only the collision test.
    F.Write(TEXT(".git/info/exclude"), TEXT("Content/\n")); F.Write(Asset, TEXT("ignored local collision\n"));
    const auto Collision = F.Repo.ReviewStash(F.StoredOid());
    TestTrue(TEXT("Ignored collision absent from ordinary status"), Collision.IsFresh());
    TestFalse(TEXT("Apply refuses ignored collision"), F.Repo.ExecuteStash(Collision, FString(), F.Lease).Ok());
    TestEqual(TEXT("Ignored collision intact"), F.Read(Asset), FString(TEXT("ignored local collision\n")));
    IFileManager::Get().Delete(*FPaths::Combine(F.Root, Asset), true, true); F.Write(TEXT(".git/info/exclude"), TEXT(""));
    const auto Apply = F.Repo.ReviewStash(F.StoredOid());
    if (!TestTrue(TEXT("Apply untracked stash: ") + Apply.Error, F.Complete(Apply, Error))) { AddError(Error); return false; }
    TestEqual(TEXT("Hydrated working bytes restored despite skip-smudge"), F.Read(Asset), FString(TEXT("newer bytes\n")));
    TestEqual(TEXT("Literal documentation path restored"), F.Read(Doc), FString(TEXT("new document\n")));
    const auto After = F.Repo.Refresh();
    TestTrue(TEXT("Restored new files remain untracked"), After.Files.Num() == 2 && After.Files[0].bUntracked && After.Files[1].bUntracked);
    TestTrue(TEXT("Real staging remains unchanged"), After.IndexEntries == Before.IndexEntries);
    TestEqual(TEXT("Apply retains stash"), F.Repo.ListStashes().Entries.Num(), 1);
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitStashNewSelectionTest, "GitWorkspace.Stash.SelectedNewFilesAndStagedAsset", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGitStashNewSelectionTest::RunTest(const FString&)
{
    FStashFixture F; FString Error;
    F.Write(TEXT("Content/Staged.uasset"), TEXT("staged new asset\n")); F.Call({TEXT("add"), TEXT("Content/Staged.uasset")});
    F.Write(TEXT("Content/Staged.uasset"), TEXT("working new asset\n"));
    F.Write(TEXT("Docs/selected.md"), TEXT("selected new file\n")); F.Write(TEXT("Docs/excluded.md"), TEXT("excluded new file\n"));
    F.Write(TEXT("README.md"), TEXT("unselected tracked edit\n"));
    TestFalse(TEXT("Untracked selection requires opt-in"), F.Repo.ReviewSelectedStash({TEXT("Docs/selected.md")}).bValid);
    const auto Review = F.Repo.ReviewSelectedStash({TEXT("Content/Staged.uasset"), TEXT("Docs/selected.md")}, true);
    if (!TestTrue(TEXT("Review mixed selected new files: ") + Review.Error, Review.IsFresh())) return false;
    if (!TestTrue(TEXT("Create mixed selected stash"), F.Complete(Review, Error))) { AddError(Error); return false; }
    const FString SelectedOid = F.StoredOid();
    TestEqual(TEXT("Unselected untracked bytes preserved"), F.Read(TEXT("Docs/excluded.md")), FString(TEXT("excluded new file\n")));
    TestEqual(TEXT("Unselected tracked bytes preserved"), F.Read(TEXT("README.md")), FString(TEXT("unselected tracked edit\n")));
    TestFalse(TEXT("Selected staged asset removed"), IFileManager::Get().FileExists(*FPaths::Combine(F.Root, TEXT("Content/Staged.uasset"))));
    TestTrue(TEXT("Excluded new file absent from third parent"), F.Call({TEXT("ls-tree"), TEXT("-r"), Review.Oid + TEXT("^3")}).Text().Contains(TEXT("selected.md")) && !F.Call({TEXT("ls-tree"), TEXT("-r"), Review.Oid + TEXT("^3")}).Text().Contains(TEXT("excluded.md")));
    // Preserve exclusions in a second stash so the first can be applied at its
    // clean original base, just as the user can do through the same workflow.
    const auto Rest = F.Repo.ReviewStash(FString(), true, true);
    if (!TestTrue(TEXT("Preserve remaining work"), F.Complete(Rest, Error))) { AddError(Error); return false; }
    const auto Apply = F.Repo.ReviewStash(SelectedOid);
    if (!TestTrue(TEXT("Apply mixed selected stash"), F.Complete(Apply, Error))) { AddError(Error); return false; }
    TestEqual(TEXT("New asset working version restored"), F.Read(TEXT("Content/Staged.uasset")), FString(TEXT("working new asset\n")));
    TestEqual(TEXT("New asset staged version restored separately"), F.Call({TEXT("show"), TEXT(":Content/Staged.uasset")}).Text(), FString(TEXT("staged new asset\n")));
    TestEqual(TEXT("Untracked selected doc restored"), F.Read(TEXT("Docs/selected.md")), FString(TEXT("selected new file\n")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitStashUntrackedPathTest, "GitWorkspace.Stash.UntrackedPathSafety", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGitStashUntrackedPathTest::RunTest(const FString&)
{
    FStashFixture F; FString Error;
    F.Write(TEXT("Docs/new.md"), TEXT("new document\n"));
    const auto Review = F.Repo.ReviewStash(FString(), true, true);
    if (!TestTrue(TEXT("Capture for collision tests"), F.Complete(Review, Error))) { AddError(Error); return false; }
    F.Write(TEXT(".git/info/exclude"), TEXT("Docs\n"));
    IFileManager::Get().MakeDirectory(*FPaths::Combine(F.Root, TEXT("Docs/new.md")), true);
    auto Apply = F.Repo.ReviewStash(F.StoredOid());
    TestTrue(TEXT("Ignored directory hidden from ordinary status"), Apply.IsFresh());
    TestFalse(TEXT("Ignored directory collision blocks apply"), F.Repo.ExecuteStash(Apply, FString(), F.Lease).Ok());
    IFileManager::Get().DeleteDirectory(*FPaths::Combine(F.Root, TEXT("Docs")), false, true);
    F.Write(TEXT("ignored.txt"), TEXT("outside target remains\n"));
    const auto Link = GitWorkspace::Run(TEXT("/bin/ln"), F.Root, {TEXT("-s"), TEXT("."), TEXT("Docs")});
    if (!TestTrue(TEXT("Make ignored ancestor symlink fixture"), Link.Ok())) return false;
    Apply = F.Repo.ReviewStash(F.StoredOid());
    TestTrue(TEXT("Ignored symlink hidden from ordinary status"), Apply.IsFresh());
    TestFalse(TEXT("Symlink ancestor blocks apply before mutation"), F.Repo.ExecuteStash(Apply, FString(), F.Lease).Ok());
    TestFalse(TEXT("No redirected write"), IFileManager::Get().FileExists(*FPaths::Combine(F.Root, TEXT("new.md"))));
    TestFalse(TEXT("No mutation recovery marker for collision"), IFileManager::Get().FileExists(*F.Marker()));
    GitWorkspace::Run(TEXT("/bin/rm"), F.Root, {TEXT("Docs")}); // Fixture symlink only.
    F.Write(TEXT(".git/info/exclude"), TEXT(""));
    IFileManager::Get().MakeDirectory(*FPaths::Combine(F.Root, TEXT("Docs")), true);
    GitWorkspace::Run(TEXT("/bin/ln"), F.Root, {TEXT("-s"), TEXT("../README.md"), TEXT("Docs/link.md")});
    TestFalse(TEXT("Untracked symlink capture refused"), F.Repo.ReviewStash(FString(), true, true).bValid);
    GitWorkspace::Run(TEXT("/bin/rm"), F.Root, {TEXT("Docs/link.md")});
    F.Write(TEXT("Content/New.umap"), TEXT("unsupported map\n"));
    TestFalse(TEXT("Untracked map capture refused"), F.Repo.ReviewStash(FString(), true, true).bValid);
    TestEqual(TEXT("Unsupported file retained"), F.Read(TEXT("Content/New.umap")), FString(TEXT("unsupported map\n")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitStashDisjointApplyTest, "GitWorkspace.Stash.ApplyPreservesDisjointWork", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGitStashDisjointApplyTest::RunTest(const FString&)
{
    for (bool bRestoreIndex : {true, false})
    {
        FStashFixture F; FString Error;
        F.Write(TEXT(".gitattributes"), TEXT("*.uasset filter=lfs diff=lfs merge=lfs -text\n"));
        F.Write(TEXT("Content/Apply.uasset"), TEXT("base asset\n")); F.Write(TEXT("Content/Keep.uasset"), TEXT("base excluded\n"));
        F.Call({TEXT("add"), TEXT(".")}); F.Call({TEXT("commit"), TEXT("-qm"), TEXT("LFS apply baseline")});
        F.Write(TEXT("Content/Apply.uasset"), TEXT("stashed staged asset\n")); F.Call({TEXT("add"), TEXT("Content/Apply.uasset")});
        const FString SavedIndex = F.Call({TEXT("show"), TEXT(":Content/Apply.uasset")}).Text();
        F.Write(TEXT("Content/Apply.uasset"), TEXT("stashed working asset\n"));
        F.Write(TEXT("Docs/Stashed new.md"), TEXT("saved untracked\n"));
        const auto Create = F.Repo.ReviewStash(FString(), true, true);
        if (!TestTrue(TEXT("Create stash for disjoint apply"), F.Complete(Create, Error))) { AddError(Error); return false; }
        F.Write(TEXT("Content/Keep.uasset"), TEXT("unrelated staged asset\n")); F.Call({TEXT("add"), TEXT("Content/Keep.uasset")});
        const FString KeptIndex = F.Call({TEXT("show"), TEXT(":Content/Keep.uasset")}).Text();
        F.Write(TEXT("Content/Keep.uasset"), TEXT("unrelated working asset\n"));
        F.Write(TEXT("Source/Keep.cpp"), TEXT("unrelated staged source\n")); F.Call({TEXT("add"), TEXT("Source/Keep.cpp")}); F.Write(TEXT("Source/Keep.cpp"), TEXT("unrelated working source\n"));
        F.Write(TEXT("Docs/local.md"), TEXT("unrelated untracked\n")); F.Write(TEXT("ignored.txt"), TEXT("unrelated ignored\n"));
        F.Call({TEXT("config"), TEXT("filter.lfs.process"), TEXT("git-lfs filter-process --skip")});
        const auto Before = F.Repo.Refresh();
        const auto Apply = F.Repo.ReviewStash(F.StoredOid(), bRestoreIndex);
        if (!TestTrue(TEXT("Disjoint Apply review: ") + Apply.Error, Apply.IsFresh())) return false;
        TestTrue(TEXT("Review explicitly lists preserved local files"), Apply.Text.Contains(TEXT("LOCAL CHANGES KEPT IN PLACE")) && Apply.Text.Contains(TEXT("Content/Keep.uasset")));
        TestTrue(TEXT("Review leaves real staging untouched"), F.Repo.Refresh().IndexEntries == Before.IndexEntries);
        if (!TestTrue(TEXT("Apply onto disjoint work"), F.Complete(Apply, Error))) { AddError(Error); return false; }
        TestEqual(TEXT("Saved working asset hydrated"), F.Read(TEXT("Content/Apply.uasset")), FString(TEXT("stashed working asset\n")));
        TestEqual(TEXT("Saved staging option respected"), F.Call({TEXT("show"), TEXT(":Content/Apply.uasset")}).Text(), bRestoreIndex ? SavedIndex : F.Call({TEXT("show"), TEXT("HEAD:Content/Apply.uasset")}).Text());
        TestEqual(TEXT("Unrelated staged LFS version preserved"), F.Call({TEXT("show"), TEXT(":Content/Keep.uasset")}).Text(), KeptIndex);
        TestEqual(TEXT("Unrelated working LFS bytes preserved"), F.Read(TEXT("Content/Keep.uasset")), FString(TEXT("unrelated working asset\n")));
        TestEqual(TEXT("Unrelated new staged source preserved"), F.Call({TEXT("show"), TEXT(":Source/Keep.cpp")}).Text(), FString(TEXT("unrelated staged source\n")));
        TestEqual(TEXT("Unrelated new working source preserved"), F.Read(TEXT("Source/Keep.cpp")), FString(TEXT("unrelated working source\n")));
        TestEqual(TEXT("Unrelated untracked bytes preserved"), F.Read(TEXT("Docs/local.md")), FString(TEXT("unrelated untracked\n")));
        TestEqual(TEXT("Ignored bytes preserved"), F.Read(TEXT("ignored.txt")), FString(TEXT("unrelated ignored\n")));
        TestEqual(TEXT("Stashed untracked file restored"), F.Read(TEXT("Docs/Stashed new.md")), FString(TEXT("saved untracked\n")));
        TestEqual(TEXT("HEAD retained"), F.Repo.Refresh().Head, Before.Head);
        TestEqual(TEXT("Stash retained"), F.Repo.ListStashes().Entries.Num(), 1);
        TestFalse(TEXT("Recovery cleared after exact verification"), IFileManager::Get().FileExists(*F.Marker()));
    }
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitStashApplyOverlapTest, "GitWorkspace.Stash.ApplyOverlapStaleAndRecovery", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGitStashApplyOverlapTest::RunTest(const FString&)
{
    FStashFixture F; FString Error;
    F.Write(TEXT("README.md"), TEXT("saved changes\n")); auto Create = F.Repo.ReviewStash();
    if (!TestTrue(TEXT("Preserve stash"), F.Complete(Create, Error))) { AddError(Error); return false; }
    F.Write(TEXT("README.md"), TEXT("local overlap\n"));
    auto Apply = F.Repo.ReviewStash(F.StoredOid());
    TestFalse(TEXT("Working overlap blocked"), Apply.IsFresh());
    TestTrue(TEXT("Overlap identifies file"), Apply.Error.Contains(TEXT("README.md")) && Apply.Error.Contains(TEXT("overlaps")));
    F.Call({TEXT("add"), TEXT("README.md")}); F.Write(TEXT("README.md"), TEXT("base\n"));
    TestFalse(TEXT("Staged overlap blocked even when work equals base"), F.Repo.ReviewStash(F.StoredOid()).IsFresh());
    F.Call({TEXT("reset"), TEXT("--hard"), TEXT("HEAD")});
    F.Call({TEXT("mv"), TEXT("README.md"), TEXT("Docs.md")});
    TestFalse(TEXT("Rename original path overlap blocked"), F.Repo.ReviewStash(F.StoredOid()).IsFresh());
    F.Call({TEXT("reset"), TEXT("--hard"), TEXT("HEAD")});
    F.Write(TEXT("Docs/Keep.md"), TEXT("local staged\n")); F.Call({TEXT("add"), TEXT("Docs/Keep.md")}); F.Write(TEXT("Docs/Keep.md"), TEXT("local working\n"));
    F.Write(TEXT("Docs/local.md"), TEXT("untracked one\n"));
    Apply = F.Repo.ReviewStash(F.StoredOid());
    if (!TestTrue(TEXT("Disjoint review ready"), Apply.IsFresh())) { AddError(Apply.Error); return false; }
    const auto Index = F.Repo.Refresh().IndexEntries;
    F.Write(TEXT("Docs/local.md"), TEXT("untracked two\n"));
    TestFalse(TEXT("Changed unrelated untracked bytes invalidate review"), F.Repo.ExecuteStash(Apply, FString(), F.Lease).Ok());
    TestTrue(TEXT("Refusal preserves staging"), F.Repo.Refresh().IndexEntries == Index);
    TestEqual(TEXT("Refusal leaves target file alone"), F.Read(TEXT("README.md")), FString(TEXT("base\n")));
    Apply = F.Repo.ReviewStash(F.StoredOid()); auto Forged = Apply; Forged.ApplyWorkingPaths.Add(TEXT("Docs/Keep.md"));
    TestFalse(TEXT("Forged restoration scope refused"), F.Repo.ExecuteStash(Forged, FString(), F.Lease).Ok());
    F.Write(TEXT("Docs/Keep.md"), TEXT("changed after preview\n"));
    TestFalse(TEXT("Changed unrelated tracked version invalidates review"), F.Repo.ExecuteStash(Apply, FString(), F.Lease).Ok());
    Apply = F.Repo.ReviewStash(F.StoredOid());
    auto Executed = F.Repo.ExecuteStash(Apply, FString(), F.Lease);
    if (!TestTrue(TEXT("Disjoint execute succeeds: ") + Executed.Error, Executed.Ok())) return false;
    F.Write(TEXT("Docs/local.md"), TEXT("external edit after execution\n"));
    TestFalse(TEXT("Postflight unrelated change retains recovery"), F.Repo.CompleteStash(Apply, F.Lease).Ok());
    TestTrue(TEXT("Recovery marker retained"), IFileManager::Get().FileExists(*F.Marker()));
    TestEqual(TEXT("No rollback of external edit"), F.Read(TEXT("Docs/local.md")), FString(TEXT("external edit after execution\n")));
    TestEqual(TEXT("No reset of unrelated staged version"), F.Call({TEXT("show"), TEXT(":Docs/Keep.md")}).Text(), FString(TEXT("local staged\n")));
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitStashApplyPathOverlapTest, "GitWorkspace.Stash.ApplyDirectoryAndCaseOverlap", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGitStashApplyPathOverlapTest::RunTest(const FString&)
{
    FStashFixture F; FString Error;
    F.Write(TEXT("Docs/New.md"), TEXT("stashed new file\n"));
    const auto Create = F.Repo.ReviewStash(FString(), true, true);
    if (!TestTrue(TEXT("Preserve new file stash"), F.Complete(Create, Error))) { AddError(Error); return false; }
    F.Write(TEXT("Docs/New.md/child.txt"), TEXT("local child\n"));
    auto Apply = F.Repo.ReviewStash(F.StoredOid());
    TestFalse(TEXT("Directory replacement overlaps local child"), Apply.IsFresh());
    TestTrue(TEXT("Directory overlap explained"), Apply.Error.Contains(TEXT("overlaps")));
    IFileManager::Get().DeleteDirectory(*FPaths::Combine(F.Root, TEXT("Docs/New.md")), false, true);
    F.Write(TEXT("Docs/new.md"), TEXT("case variant\n"));
    Apply = F.Repo.ReviewStash(F.StoredOid());
    TestFalse(TEXT("Case variant overlap blocked"), Apply.IsFresh());
    TestTrue(TEXT("Case overlap explained"), Apply.Error.Contains(TEXT("overlaps")));
    TestEqual(TEXT("Case variant remains intact"), F.Read(TEXT("Docs/new.md")), FString(TEXT("case variant\n")));
    return true;
}
#endif
