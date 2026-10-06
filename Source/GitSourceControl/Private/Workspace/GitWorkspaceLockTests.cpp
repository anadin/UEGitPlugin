// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#if WITH_DEV_AUTOMATION_TESTS
#include "GitWorkspaceRepository.h"
#include "SGitWorkspace.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Interfaces/IPluginManager.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Misc/App.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "Engine/Texture2D.h"
#if PLATFORM_MAC
#include "GitWorkspaceSession.h"
#include <unistd.h>
#include <sys/stat.h>
#endif

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitLockParserTest, "GitWorkspace.Locks.StrictOwnership", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGitLockParserTest::RunTest(const FString&)
{
    TMap<FString, GitWorkspace::FLock> Locks; FString Error;
    const FString Entry = TEXT("{\"id\":\"abc\",\"path\":\"Content/水 asset.uasset\",\"owner\":{\"name\":\"me\"},\"locked_at\":\"2026-10-02T00:00:00Z\"}");
    TestTrue(TEXT("Empty complete response"), GitWorkspace::ParseVerifiedLocks(TEXT("{\"ours\":[],\"theirs\":[]}"), Locks, Error));
    TestTrue(TEXT("Ownership from server group"), GitWorkspace::ParseVerifiedLocks(TEXT("{\"ours\":[],\"theirs\":[") + Entry + TEXT("]}"), Locks, Error));
    TestTrue(TEXT("Display name does not confer ownership"), Locks.Num() == 1 && !Locks.CreateConstIterator().Value().bOurs);
    TestFalse(TEXT("Duplicate ownership rejected"), GitWorkspace::ParseVerifiedLocks(TEXT("{\"ours\":[") + Entry + TEXT("],\"theirs\":[") + Entry + TEXT("]}"), Locks, Error));
    TestTrue(TEXT("No partial entries leak"), Locks.IsEmpty());
    for (const FString& Invalid : {FString(TEXT("{}")), FString(TEXT("{\"ours\":null,\"theirs\":[]}")), FString(TEXT("{\"ours\":[],\"theirs\":[],\"next_cursor\":\"next\"}")), FString(TEXT("{\"ours\":[],\"theirs\":[],\"message\":\"failed\"}")), FString(TEXT("garbage"))})
        TestFalse(TEXT("Incomplete response not unlocked"), GitWorkspace::ParseVerifiedLocks(Invalid, Locks, Error));
    GitWorkspace::FLockSnapshot S; S.bVerified = true; S.VerifiedAt = FDateTime::UtcNow(); S.VerifiedSeconds = FPlatformTime::Seconds();
    TestTrue(TEXT("Fresh absence means unlocked"), S.State(TEXT("asset"), true) == GitWorkspace::ELockState::Unlocked);
    S.VerifiedSeconds -= 61;
    TestTrue(TEXT("Expired absence is stale"), S.State(TEXT("asset"), true) == GitWorkspace::ELockState::Stale);
    S.VerifiedSeconds = FPlatformTime::Seconds(); S.Error = TEXT("offline");
    TestFalse(TEXT("Failed verification is not fresh"), S.IsFresh());
    return true;
}

#if PLATFORM_MAC
namespace
{
struct FLockFixture
{
    FString Root = FPaths::Combine(FPlatformProcess::UserTempDir(), TEXT("uegit-lock-test-") + FGuid::NewGuid().ToString(EGuidFormats::Digits));
    FString Repo = FPaths::Combine(Root, TEXT("repo")), Remote = FPaths::Combine(Root, TEXT("remote.git"));
    FString Git = GitWorkspace::FindGitExecutable(), Endpoint;
    FProcHandle Server;
    FLockFixture()
    {
        IFileManager::Get().MakeDirectory(*Repo, true);
        const auto Plugin = IPluginManager::Get().FindPlugin(TEXT("GitSourceControl"));
        if (!Plugin) return;
        const FString Script = FPaths::ConvertRelativePathToFull(FPaths::Combine(Plugin->GetBaseDir(), TEXT("Source/GitSourceControl/Private/Workspace/LockTestServer.py")));
        const FString Args = TEXT("\"") + Script + TEXT("\" \"") + Root + TEXT("\"");
        Server = FPlatformProcess::CreateProc(TEXT("/usr/bin/python3"), *Args, false, true, true, nullptr, 0, *Root, nullptr);
        const double Deadline = FPlatformTime::Seconds() + 10;
        FString Port;
        while (Port.IsEmpty() && FPlatformTime::Seconds() < Deadline)
        { const FString PortFile = FPaths::Combine(Root, TEXT("port")); if (IFileManager::Get().FileExists(*PortFile)) FFileHelper::LoadFileToString(Port, *PortFile); FPlatformProcess::Sleep(0.02f); }
        if (Port.IsEmpty()) return;
        Endpoint = TEXT("http://127.0.0.1:") + Port + TEXT("/lfs");
        Call({TEXT("init"), TEXT("-q"), TEXT("-b"), TEXT("main")});
        Call({TEXT("config"), TEXT("user.name"), TEXT("UEGit lock fixture")});
        Call({TEXT("config"), TEXT("user.email"), TEXT("fixture@example.invalid")});
        Call({TEXT("config"), TEXT("commit.gpgsign"), TEXT("false")});
        Call({TEXT("config"), TEXT("core.hooksPath"), TEXT(".git/test-hooks")});
        Call({TEXT("lfs"), TEXT("install"), TEXT("--local"), TEXT("--skip-repo")});
        Call({TEXT("config"), TEXT("lfs.url"), Endpoint});
        Call({TEXT("config"), TEXT("lfs.activitytimeout"), TEXT("2")});
        Write(TEXT(".gitattributes"), TEXT("*.uasset filter=lfs diff=lfs merge=lfs -text lockable\n"));
        Write(TEXT("asset.uasset"), TEXT("base A\n"));
        Call({TEXT("add"), TEXT(".")}); Call({TEXT("commit"), TEXT("-qm"), TEXT("base")});
        Call({TEXT("init"), TEXT("--bare"), Remote});
        Call({TEXT("remote"), TEXT("add"), TEXT("origin"), Remote});
        Call({TEXT("push"), TEXT("--no-verify"), TEXT("-u"), TEXT("origin"), TEXT("main")});
    }
    ~FLockFixture()
    {
        if (Server.IsValid()) { FPlatformProcess::TerminateProc(Server, true); FPlatformProcess::CloseProc(Server); }
        IFileManager::Get().DeleteDirectory(*Root, false, true);
    }
    GitWorkspace::FResult Call(const TArray<FString>& Args) { return GitWorkspace::Run(Git, Repo, Args); }
    void Write(const FString& Path, const FString& Text)
    { const FString Full = FPaths::Combine(Repo, Path); FPlatformFileManager::Get().GetPlatformFile().SetReadOnly(*Full, false); FFileHelper::SaveStringToFile(Text, *Full, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM); }
    void Mode(const FString& Mode) { FFileHelper::SaveStringToFile(Mode, *FPaths::Combine(Root, TEXT("mode")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM); }
};
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitLockServerTest, "GitWorkspace.Locks.ServerFailuresAndPagination", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGitLockServerTest::RunTest(const FString&)
{
    FLockFixture F;
    if (!TestFalse(TEXT("Loopback server started"), F.Endpoint.IsEmpty())) return false;
    GitWorkspace::FRepository Repo(F.Git, F.Repo);
    auto S = Repo.VerifyLocks(TEXT("origin"));
    if (!TestTrue(TEXT("Complete verification: ") + S.Error, S.IsFresh())) return false;
    TestEqual(TEXT("Endpoint recorded"), S.Endpoint, F.Endpoint);
    TestEqual(TEXT("Clean lockable asset included"), S.Candidates.Num(), 1);
    F.Mode(TEXT("pages")); S = Repo.VerifyLocks(TEXT("origin"));
    TestTrue(TEXT("All pages verified: ") + S.Error, S.IsFresh() && S.Locks.Num() == 2);
    TestTrue(TEXT("Same name in theirs remains foreign"), S.State(TEXT("second.uasset"), true) == GitWorkspace::ELockState::Theirs);
    for (const TCHAR* Mode : {TEXT("partial"), TEXT("offline"), TEXT("auth"), TEXT("unsupported"), TEXT("timeout"), TEXT("malformed"), TEXT("duplicate")})
    {
        F.Mode(Mode); S = Repo.VerifyLocks(TEXT("origin"));
        TestFalse(FString(Mode) + TEXT(" cannot become fresh unlocked"), S.IsFresh());
        TestFalse(FString(Mode) + TEXT(" reports an error"), S.Error.IsEmpty());
    }
    F.Mode(TEXT("foreign")); S = Repo.VerifyLocks(TEXT("origin"));
    TestFalse(TEXT("Foreign lock cannot unlock"), Repo.ChangeLock(S, TEXT("asset.uasset"), true, true).Ok());
    TestFalse(TEXT("Foreign lock cannot be replaced"), Repo.ChangeLock(S, TEXT("asset.uasset"), false).Ok());
    F.Mode(TEXT("otherclone")); S = Repo.VerifyLocks(TEXT("origin"));
    TestFalse(TEXT("Other clone's own lock retained"), Repo.ChangeLock(S, TEXT("asset.uasset"), true, true).Ok());
    F.Mode(TEXT("")); S = Repo.VerifyLocks(TEXT("origin"));
    F.Mode(TEXT("conflict"));
    TestFalse(TEXT("Lock race conflict is reported"), Repo.ChangeLock(S, TEXT("asset.uasset"), false).Ok());
    F.Mode(TEXT("")); S.VerifiedSeconds -= 61;
    TestFalse(TEXT("Stale review cannot mutate"), Repo.ChangeLock(S, TEXT("asset.uasset"), false).Ok());
    S = Repo.VerifyLocks(TEXT("origin"));
    F.Call({TEXT("config"), TEXT("lfs.url"), F.Endpoint + TEXT("/changed")});
    TestFalse(TEXT("Endpoint change invalidates reviewed action"), Repo.ChangeLock(S, TEXT("asset.uasset"), false).Ok());
    F.Call({TEXT("config"), TEXT("lfs.url"), F.Endpoint});
    F.Write(TEXT(".lfsconfig"), TEXT("[lfs]\n pushurl = ") + F.Endpoint + TEXT("/push\n"));
    TestFalse(TEXT("Split endpoint in .lfsconfig rejected"), Repo.VerifyLocks(TEXT("origin")).IsFresh());
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitLockLifecycleTest, "GitWorkspace.Locks.AcquireAndSafeRelease", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGitLockLifecycleTest::RunTest(const FString&)
{
    FLockFixture F;
    if (!TestFalse(TEXT("Loopback server started"), F.Endpoint.IsEmpty())) return false;
    GitWorkspace::FRepository Repo(F.Git, F.Repo);
    auto S = Repo.VerifyLocks(TEXT("origin"));
    auto R = Repo.ChangeLock(S, TEXT("asset.uasset"), false);
    if (!TestTrue(TEXT("Acquire real LFS lock: ") + R.Error, R.Ok())) return false;
    S = Repo.VerifyLocks(TEXT("origin"));
    TestTrue(TEXT("Server identifies own lock"), S.State(TEXT("asset.uasset"), true) == GitWorkspace::ELockState::Ours);
    TestFalse(TEXT("Handoff required"), Repo.ChangeLock(S, TEXT("asset.uasset"), true).Ok());
    F.Write(TEXT("asset.uasset"), TEXT("new B\n"));
    TestFalse(TEXT("Working changes retain lock"), Repo.ChangeLock(S, TEXT("asset.uasset"), true, true).Ok());
    Repo.Stage({TEXT("asset.uasset")});
    TestFalse(TEXT("Staged changes retain lock"), Repo.ChangeLock(S, TEXT("asset.uasset"), true, true).Ok());
    TestTrue(TEXT("Commit remains independent of lock"), Repo.Commit(Repo.Refresh(), TEXT("B")).Ok());
    S = Repo.VerifyLocks(TEXT("origin"));
    TestTrue(TEXT("Commit retains lock"), S.State(TEXT("asset.uasset"), true) == GitWorkspace::ELockState::Ours);
    TestFalse(TEXT("Outgoing commit retains lock"), Repo.ChangeLock(S, TEXT("asset.uasset"), true, true).Ok());
    const auto Unpublished = Repo.ReviewUnlock(TEXT("origin"), TEXT("asset.uasset"));
    TestTrue(TEXT("Publication blocker gives Fetch, Push and branch-wide scope"), Unpublished.Error.Contains(TEXT("Fetch upstream")) && Unpublished.Error.Contains(TEXT("Push")) && Unpublished.Error.Contains(TEXT("all outgoing commits")));
    F.Call({TEXT("push"), TEXT("--no-verify"), TEXT("origin"), TEXT("main")});
    F.Write(TEXT("asset.uasset"), TEXT("stash C\n")); F.Call({TEXT("stash"), TEXT("push"), TEXT("-m"), TEXT("fixture")});
    TestFalse(TEXT("Stashes retain lock"), Repo.ChangeLock(S, TEXT("asset.uasset"), true, true).Ok());
    F.Call({TEXT("stash"), TEXT("drop")});
    R = Repo.ChangeLock(S, TEXT("asset.uasset"), true, true);
    TestTrue(TEXT("Explicit clean published handoff unlocks: ") + R.Error, R.Ok());
    S = Repo.VerifyLocks(TEXT("origin"));
    TestTrue(TEXT("Server confirms unlocked"), S.State(TEXT("asset.uasset"), true) == GitWorkspace::ELockState::Unlocked);
    FString Bytes; FFileHelper::LoadFileToString(Bytes, *FPaths::Combine(F.Repo, TEXT("asset.uasset")));
    TestEqual(TEXT("Unlock preserves working bytes"), Bytes, FString(TEXT("new B\n")));
    FString Requests; FFileHelper::LoadFileToString(Requests, *FPaths::Combine(F.Root, TEXT("requests")));
    TestFalse(TEXT("No force unlock sent"), Requests.Contains(TEXT("\"force\": true")));
    F.Mode(TEXT("postfail"));
    R = Repo.ChangeLock(S, TEXT("asset.uasset"), false);
    TestFalse(TEXT("Successful mutation with failed verification is uncertain"), R.Ok());
    TestTrue(TEXT("Uncertain outcome is explained"), R.Error.Contains(TEXT("could not be confirmed")));
    F.Mode(TEXT("")); S = Repo.VerifyLocks(TEXT("origin"));
    TestTrue(TEXT("Refresh discovers the acquired reservation"), S.State(TEXT("asset.uasset"), true) == GitWorkspace::ELockState::Ours);
    TestFalse(TEXT("Unconfirmed acquisition cannot be silently adopted"), Repo.ChangeLock(S, TEXT("asset.uasset"), true, true).Ok());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitUnlockStashScopeTest, "GitWorkspace.Locks.StashScopedHandoff", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGitUnlockStashScopeTest::RunTest(const FString&)
{
    FLockFixture F;
    if (!TestFalse(TEXT("Loopback server started"), F.Endpoint.IsEmpty())) return false;
    F.Write(TEXT("texture.uasset"), TEXT("texture base\n"));
    F.Call({TEXT("add"), TEXT(".")}); F.Call({TEXT("commit"), TEXT("-qm"), TEXT("texture")});
    F.Call({TEXT("push"), TEXT("--no-verify"), TEXT("origin"), TEXT("main")});
    GitWorkspace::FRepository Repo(F.Git, F.Repo);
    for (const FString Path : {FString(TEXT("asset.uasset")), FString(TEXT("texture.uasset"))})
        if (!TestTrue(TEXT("Acquire fixture lock"), Repo.ChangeLock(Repo.VerifyLocks(TEXT("origin")), Path, false).Ok())) return false;
    F.Write(TEXT("asset.uasset"), TEXT("unfinished Blueprint\n"));
    F.Call({TEXT("stash"), TEXT("push"), TEXT("-m"), TEXT("Blueprint work")});
    const auto Before = Repo.Refresh(); const auto Stashes = Repo.ListStashes();
    const auto Blocked = Repo.ReviewUnlock(TEXT("origin"), TEXT("asset.uasset"));
    TestTrue(TEXT("Review names the stash that needs this lock"), !Blocked.IsFresh() && Blocked.BlockingStashes.Num() == 1 && Blocked.Text().Contains(TEXT("Blueprint work")));
    TestFalse(TEXT("Stashed asset cannot be released"), Repo.ChangeLock(Blocked.Locks, Blocked.Path, true, true).Ok());
    const auto Ready = Repo.ReviewUnlock(TEXT("origin"), TEXT("texture.uasset"));
    if (!TestTrue(TEXT("Unrelated stash permits texture handoff: ") + Ready.Error, Ready.IsFresh())) return false;
    TestEqual(TEXT("Review inspects every stash"), Ready.StashesChecked, 1);
    TestTrue(TEXT("Review keeps index unchanged"), Repo.Refresh().IndexEntries == Before.IndexEntries);
    TestEqual(TEXT("Review keeps HEAD unchanged"), Repo.Refresh().Head, Before.Head);
    const auto Released = Repo.ChangeLock(Ready.Locks, Ready.Path, true, true, Ready.Head);
    TestTrue(TEXT("Release only the reviewed texture: ") + Released.Error, Released.Ok());
    const auto After = Repo.VerifyLocks(TEXT("origin"));
    TestTrue(TEXT("Blueprint lock remains held"), After.State(TEXT("asset.uasset"), true) == GitWorkspace::ELockState::Ours);
    TestTrue(TEXT("Texture lock released"), After.State(TEXT("texture.uasset"), true) == GitWorkspace::ELockState::Unlocked);
    TestEqual(TEXT("Stash list retained exactly"), Repo.ListStashes().Fingerprint, Stashes.Fingerprint);
    TestTrue(TEXT("Release preserves index"), Repo.Refresh().IndexEntries == Before.IndexEntries);
    TestTrue(TEXT("Release leaves working files clean"), Repo.Refresh().Files.IsEmpty());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitUnlockStashTreesTest, "GitWorkspace.Locks.StashTreesAndLiteralPaths", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGitUnlockStashTreesTest::RunTest(const FString&)
{
    FLockFixture F;
    if (!TestFalse(TEXT("Loopback server started"), F.Endpoint.IsEmpty())) return false;
    GitWorkspace::FRepository Repo(F.Git, F.Repo);
    if (!TestTrue(TEXT("Acquire asset lock"), Repo.ChangeLock(Repo.VerifyLocks(TEXT("origin")), TEXT("asset.uasset"), false).Ok())) return false;
    F.Write(TEXT("asset.uasset"), TEXT("index only version\n")); F.Call({TEXT("add"), TEXT("asset.uasset")});
    F.Call({TEXT("restore"), TEXT("--source=HEAD"), TEXT("--worktree"), TEXT("--"), TEXT("asset.uasset")});
    F.Call({TEXT("stash"), TEXT("push"), TEXT("-m"), TEXT("index only")});
    auto Review = Repo.ReviewUnlock(TEXT("origin"), TEXT("asset.uasset"));
    TestTrue(TEXT("Saved staging blocks even when saved working tree equals base"), !Review.IsFresh() && Review.BlockingStashes.Num() == 1 && Review.Text().Contains(TEXT("index only")));
    F.Call({TEXT("stash"), TEXT("drop")});
    F.Call({TEXT("mv"), TEXT("asset.uasset"), TEXT("renamed.uasset")});
    F.Call({TEXT("stash"), TEXT("push"), TEXT("-m"), TEXT("rename")});
    Review = Repo.ReviewUnlock(TEXT("origin"), TEXT("asset.uasset"));
    TestTrue(TEXT("Rename source keeps original reservation"), !Review.IsFresh() && Review.BlockingStashes.Num() == 1);
    F.Call({TEXT("stash"), TEXT("drop")});
    // The path was untracked in an older stash, and is tracked/published now.
    // Literal newline/Unicode names must not evade overlap.
    const FString Literal = TEXT("Lock [水]\nTester.uasset");
    F.Write(Literal, TEXT("saved untracked version\n"));
    F.Call({TEXT("stash"), TEXT("push"), TEXT("-u"), TEXT("-m"), TEXT("new asset")});
    F.Write(Literal, TEXT("published version\n")); F.Call({TEXT("add"), TEXT("--"), Literal});
    F.Call({TEXT("commit"), TEXT("-qm"), TEXT("publish new asset")}); F.Call({TEXT("push"), TEXT("--no-verify"), TEXT("origin"), TEXT("main")});
    if (!TestTrue(TEXT("Acquire literal path"), Repo.ChangeLock(Repo.VerifyLocks(TEXT("origin")), Literal, false).Ok())) return false;
    Review = Repo.ReviewUnlock(TEXT("origin"), Literal);
    TestTrue(TEXT("Older untracked third parent blocks exact literal path"), !Review.IsFresh() && Review.BlockingStashes.Num() == 1);
    TestTrue(TEXT("Report escapes the newline"), Review.Text().Contains(TEXT("Lock [水]\\nTester.uasset")));
    const FString SavedOid = Repo.ListStashes().Entries[0].Oid;
    // A malformed entry must block even an otherwise unrelated asset.
    F.Call({TEXT("update-ref"), TEXT("-m"), TEXT("malformed"), TEXT("refs/stash"), TEXT("HEAD")});
    Review = Repo.ReviewUnlock(TEXT("origin"), TEXT("asset.uasset"));
    TestTrue(TEXT("Unreadable stash shape fails closed"), !Review.IsFresh() && Review.Error.Contains(TEXT("Cannot inspect")));
    TestFalse(TEXT("Malformed stash never permits release"), Repo.ChangeLock(Review.Locks, Review.Path, true, true).Ok());
    TestTrue(TEXT("Original stash object retained"), F.Call({TEXT("cat-file"), TEXT("-e"), SavedOid}).Ok());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitUnlockRaceTest, "GitWorkspace.Locks.UnlockReviewRacesAndRecovery", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGitUnlockRaceTest::RunTest(const FString&)
{
    FLockFixture F;
    if (!TestFalse(TEXT("Loopback server started"), F.Endpoint.IsEmpty())) return false;
    GitWorkspace::FRepository Repo(F.Git, F.Repo);
    if (!TestTrue(TEXT("Acquire race fixture lock"), Repo.ChangeLock(Repo.VerifyLocks(TEXT("origin")), TEXT("asset.uasset"), false).Ok())) return false;
    F.Write(TEXT("asset.uasset"), TEXT("stashed pending work\n"));
    F.Call({TEXT("stash"), TEXT("push"), TEXT("-m"), TEXT("pending work")});
    const FString Oid = Repo.ListStashes().Entries[0].Oid;
    F.Call({TEXT("stash"), TEXT("drop")});
    const auto Reviewed = Repo.ReviewUnlock(TEXT("origin"), TEXT("asset.uasset"));
    if (!TestTrue(TEXT("Initial review ready"), Reviewed.IsFresh())) return false;
    const FString Wrapper = FPaths::Combine(F.Repo, TEXT(".git/racing-git"));
    const FString Script = TEXT("#!/bin/sh\ncase \" $* \" in *' ls-remote '*) '") + F.Git + TEXT("' stash store -m concurrent ") + Oid + TEXT(" || exit 1 ;; esac\nexec '") + F.Git + TEXT("' \"$@\"\n");
    FFileHelper::SaveStringToFile(Script, *Wrapper, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
    GitWorkspace::Run(TEXT("/bin/chmod"), F.Repo, {TEXT("700"), Wrapper});
    GitWorkspace::FRepository Racing(Wrapper, F.Repo);
    const auto Race = Racing.ChangeLock(Reviewed.Locks, Reviewed.Path, true, true, Reviewed.Head);
    TestTrue(TEXT("Stash created during live remote query blocks unlock"), !Race.Ok() && Race.Error.Contains(TEXT("Stash state changed")));
    F.Call({TEXT("stash"), TEXT("drop")});
    F.Write(TEXT("README.md"), TEXT("another published commit\n")); F.Call({TEXT("add"), TEXT("README.md")});
    F.Call({TEXT("commit"), TEXT("-qm"), TEXT("new head")}); F.Call({TEXT("push"), TEXT("--no-verify"), TEXT("origin"), TEXT("main")});
    const auto Changed = Repo.ChangeLock(Repo.VerifyLocks(TEXT("origin")), Reviewed.Path, true, true, Reviewed.Head);
    TestTrue(TEXT("Confirmation is bound to reviewed commit"), !Changed.Ok() && Changed.Error.Contains(TEXT("Commit changed")));
    FString CanonicalRoot, GitDir; GitWorkspaceSession::FindRepository(F.Repo, CanonicalRoot, GitDir);
    const FString Marker = GitWorkspaceSession::RecoveryFile(GitDir);
    IFileManager::Get().MakeDirectory(*FPaths::GetPath(Marker), true);
    FFileHelper::SaveStringToFile(TEXT("pending asset recovery"), *Marker);
    const auto Recovery = Repo.ReviewUnlock(TEXT("origin"), Reviewed.Path);
    TestTrue(TEXT("Pending asset recovery retains lock"), !Recovery.IsFresh() && Recovery.Error.Contains(TEXT("recovery")));
    TestFalse(TEXT("Execution also respects recovery"), Repo.ChangeLock(Recovery.Locks, Recovery.Path, true, true).Ok());
    FString Requests; FFileHelper::LoadFileToString(Requests, *FPaths::Combine(F.Root, TEXT("requests")));
    TestFalse(TEXT("No rejected release reached server"), Requests.Contains(TEXT("/unlock")));
    TestTrue(TEXT("Lock remains owned"), Repo.VerifyLocks(TEXT("origin")).State(Reviewed.Path, true) == GitWorkspace::ELockState::Ours);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitUnlockReviewPanelTest, "GitWorkspace.Locks.UnlockReviewThroughPanel", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGitUnlockReviewPanelTest::RunTest(const FString&)
{
    FLockFixture F;
    if (!TestFalse(TEXT("Loopback server started"), F.Endpoint.IsEmpty())) return false;
    auto Repo = MakeShared<GitWorkspace::FRepository, ESPMode::ThreadSafe>(F.Git, F.Repo);
    if (!TestTrue(TEXT("Acquire panel fixture lock"), Repo->ChangeLock(Repo->VerifyLocks(TEXT("origin")), TEXT("asset.uasset"), false).Ok())) return false;
    auto Panel = SNew(SGitWorkspace).Repository(Repo);
    auto Settle = [&]() { while (Panel->Pending.IsValid()) { Panel->Pending.Wait(); Panel->Tick(FGeometry(), 0, 0); } };
    Settle(); Panel->bContentOnly = false; Panel->RebuildRows();
    for (const auto& Row : Panel->Rows) if (Row->Group.IsEmpty() && Row->File.Path == TEXT("asset.uasset")) Panel->List->SetItemSelection(Row, true);
    TestTrue(TEXT("Own lock offers Unlock review"), Panel->CanUnlockSelected());
    const auto OwnedLocks = Panel->Locks;
    Panel->Locks.Locks.Remove(TEXT("asset.uasset"));
    TestFalse(TEXT("Verified unlocked asset greys out Unlock"), Panel->CanUnlockSelected());
    TestTrue(TEXT("Unlocked tooltip explains absence"), Panel->UnlockHint().ToString().Contains(TEXT("no lock to release")));
    Panel->ChangeLock(true); Settle();
    TestFalse(TEXT("Unlocked asset cannot open a review"), Panel->UnlockWindow.IsValid());
    Panel->Locks = OwnedLocks; Panel->Locks.Locks[TEXT("asset.uasset")].bOurs = false;
    TestFalse(TEXT("Foreign lock greys out Unlock"), Panel->CanUnlockSelected());
    Panel->ChangeLock(true); Settle();
    TestFalse(TEXT("Foreign lock cannot open a review"), Panel->UnlockWindow.IsValid());
    Panel->Locks = OwnedLocks;
    Panel->Locks.VerifiedSeconds -= 61;
    TestTrue(TEXT("Stale own lock can request fresh review"), Panel->CanUnlockSelected());
    Panel->ChangeLock(true); Settle();
    TestTrue(TEXT("Unlock opens fresh read-only handoff review from stale ownership"), Panel->UnlockReview.IsFresh());
    if (!TestTrue(TEXT("Review report exists"), Panel->UnlockReport.IsValid())) return false;
    TestTrue(TEXT("Review shows ready and exact asset"), Panel->UnlockReport->GetText().ToString().Contains(TEXT("READY FOR HANDOFF")) && Panel->UnlockReview.Path == TEXT("asset.uasset"));
    {
        TGuardValue<bool> Unattended(GIsRunningUnattendedScript, true);
        Panel->RunUnlock();
    }
    TestEqual(TEXT("Declining confirmation preserves lock"), Panel->Feedback, FString(TEXT("Unlock cancelled. Lock retained.")));
    F.Write(TEXT("asset.uasset"), TEXT("pending Blueprint work\n"));
    F.Call({TEXT("stash"), TEXT("push"), TEXT("-m"), TEXT("panel blocked work")});
    Panel->RefreshUnlockReview(); Settle();
    TestFalse(TEXT("Stashed work disables release"), Panel->UnlockReview.IsFresh());
    TestTrue(TEXT("Panel names the blocking stash"), Panel->UnlockReport->GetText().ToString().Contains(TEXT("panel blocked work")));
    FString Requests; FFileHelper::LoadFileToString(Requests, *FPaths::Combine(F.Root, TEXT("requests")));
    TestFalse(TEXT("Preview, refresh and cancellation never release"), Requests.Contains(TEXT("/unlock")));
    TestTrue(TEXT("Panel operations retain server lock"), Repo->VerifyLocks(TEXT("origin")).State(TEXT("asset.uasset"), true) == GitWorkspace::ELockState::Ours);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitLockRestartTest, "GitWorkspace.Locks.RestartAndWorktreeIsolation", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGitLockRestartTest::RunTest(const FString&)
{
    FLockFixture F;
    if (!TestFalse(TEXT("Loopback server started"), F.Endpoint.IsEmpty())) return false;
    {
        GitWorkspace::FRepository First(F.Git, F.Repo);
        const auto Result = First.ChangeLock(First.VerifyLocks(TEXT("origin")), TEXT("asset.uasset"), false);
        if (!TestTrue(TEXT("Acquisition persisted: ") + Result.Error, Result.Ok())) return false;
    }
    const FString Journal = FPaths::Combine(F.Repo, TEXT(".git/uegit/lock-acquisitions-v1"));
    TArray<FString> Records; IFileManager::Get().FindFiles(Records, *FPaths::Combine(Journal, TEXT("*.json")), true, false);
    TestEqual(TEXT("One complete record survives service destruction"), Records.Num(), 1);
    TestTrue(TEXT("Records do not dirty the project"), F.Call({TEXT("status"), TEXT("--porcelain")}).Out.IsEmpty());

    const FString SiblingPath = FPaths::Combine(F.Root, TEXT("sibling"));
    if (!TestTrue(TEXT("Create linked worktree"), F.Call({TEXT("worktree"), TEXT("add"), TEXT("-q"), TEXT("-b"), TEXT("sibling"), SiblingPath, TEXT("HEAD")}).Ok())) return false;
    GitWorkspace::FRepository Sibling(F.Git, SiblingPath);
    auto S = Sibling.VerifyLocks(TEXT("origin"));
    TestTrue(TEXT("Same account sees own lock from sibling"), S.State(TEXT("asset.uasset"), true) == GitWorkspace::ELockState::Ours);
    TestFalse(TEXT("Sibling cannot adopt the original worktree's record"), Sibling.ChangeLock(S, TEXT("asset.uasset"), true, true).Ok());

    GitWorkspace::FRepository Restarted(F.Git, F.Repo);
    F.Call({TEXT("checkout"), TEXT("-qb"), TEXT("other-branch")});
    S = Restarted.VerifyLocks(TEXT("origin"));
    TestFalse(TEXT("Different branch cannot release original acquisition"), Restarted.ChangeLock(S, TEXT("asset.uasset"), true, true).Ok());
    F.Call({TEXT("checkout"), TEXT("-q"), TEXT("main")});
    F.Call({TEXT("config"), TEXT("user.name"), TEXT("changed identity")});
    S = Restarted.VerifyLocks(TEXT("origin"));
    TestFalse(TEXT("Changed Git configuration cannot adopt old acquisition context"), Restarted.ChangeLock(S, TEXT("asset.uasset"), true, true).Ok());
    F.Call({TEXT("config"), TEXT("user.name"), TEXT("UEGit lock fixture")});
    S = Restarted.VerifyLocks(TEXT("origin"));
    TestFalse(TEXT("Restart still requires explicit handoff"), Restarted.ChangeLock(S, TEXT("asset.uasset"), true).Ok());
    F.Write(TEXT("asset.uasset"), TEXT("later working edit\n"));
    TestFalse(TEXT("Recovered acquisition does not bypass working changes"), Restarted.ChangeLock(S, TEXT("asset.uasset"), true, true).Ok());
    F.Write(TEXT("asset.uasset"), TEXT("base A\n"));
    const auto Result = Restarted.ChangeLock(S, TEXT("asset.uasset"), true, true);
    TestTrue(TEXT("Restarted service releases verified clean published lock: ") + Result.Error, Result.Ok());
    TestTrue(TEXT("Server confirms release"), Restarted.VerifyLocks(TEXT("origin")).State(TEXT("asset.uasset"), true) == GitWorkspace::ELockState::Unlocked);
    Records.Empty(); IFileManager::Get().FindFiles(Records, *FPaths::Combine(Journal, TEXT("*.json")), true, false);
    TestTrue(TEXT("Confirmed release removes acquisition record"), Records.IsEmpty());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitLockRecordIntegrityTest, "GitWorkspace.Locks.RecordIntegrityAndReplacement", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGitLockRecordIntegrityTest::RunTest(const FString&)
{
    FLockFixture F;
    if (!TestFalse(TEXT("Loopback server started"), F.Endpoint.IsEmpty())) return false;
    GitWorkspace::FRepository Repo(F.Git, F.Repo);
    if (!TestTrue(TEXT("Initial acquisition"), Repo.ChangeLock(Repo.VerifyLocks(TEXT("origin")), TEXT("asset.uasset"), false).Ok())) return false;
    const FString Journal = FPaths::Combine(F.Repo, TEXT(".git/uegit/lock-acquisitions-v1"));
    TArray<FString> Records; IFileManager::Get().FindFiles(Records, *FPaths::Combine(Journal, TEXT("*.json")), true, false);
    if (!TestEqual(TEXT("One record"), Records.Num(), 1)) return false;
    const FString File = FPaths::Combine(Journal, Records[0]); FString Original;
    if (!TestTrue(TEXT("Readable journal"), FFileHelper::LoadFileToString(Original, *File))) return false;
    auto Save = [&](const FString& Value) { return FFileHelper::SaveStringToFile(Value, *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM); };
    auto RefusesRelease = [&](const TCHAR* Label)
    {
        GitWorkspace::FRepository Reopened(F.Git, F.Repo);
        TestFalse(Label, Reopened.ChangeLock(Reopened.VerifyLocks(TEXT("origin")), TEXT("asset.uasset"), true, true).Ok());
        TestTrue(TEXT("Server lock still held"), Reopened.VerifyLocks(TEXT("origin")).State(TEXT("asset.uasset"), true) == GitWorkspace::ELockState::Ours);
    };
    Save(TEXT("{\"version\":1,")); RefusesRelease(TEXT("Truncated record does not authorize unlock"));
    Save(TEXT("{\"version\":999}")); RefusesRelease(TEXT("Unsupported record version does not authorize unlock"));
    Save(Original.Replace(*F.Repo, TEXT("/another-checkout"))); RefusesRelease(TEXT("Record copied from another root does not authorize unlock"));
    Save(FString::ChrN(65537, 'x')); RefusesRelease(TEXT("Oversized record is rejected"));
    IFileManager::Get().Delete(*File); RefusesRelease(TEXT("Missing record fails closed even in original process"));
    Save(Original);
    // Release/reacquire with an external client: same path and same account, but
    // a new lock ID. The old local record must never authorize its release.
    if (!TestTrue(TEXT("External release"), F.Call({TEXT("lfs"), TEXT("unlock"), TEXT("--json"), TEXT("asset.uasset")}).Ok())) return false;
    if (!TestTrue(TEXT("External reacquisition"), F.Call({TEXT("lfs"), TEXT("lock"), TEXT("--json"), TEXT("asset.uasset")}).Ok())) return false;
    RefusesRelease(TEXT("Replacement server lock is not silently adopted"));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitLockRecordWriteFailureTest, "GitWorkspace.Locks.RecordWriteFailure", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGitLockRecordWriteFailureTest::RunTest(const FString&)
{
    FLockFixture F;
    if (!TestFalse(TEXT("Loopback server started"), F.Endpoint.IsEmpty())) return false;
    // A regular file obstructs creation of the journal directory, regardless of
    // the test process's permissions. The server success must not be hidden.
    F.Write(TEXT(".git/uegit"), TEXT("fixture obstruction"));
    GitWorkspace::FRepository Repo(F.Git, F.Repo);
    const auto R = Repo.ChangeLock(Repo.VerifyLocks(TEXT("origin")), TEXT("asset.uasset"), false);
    TestFalse(TEXT("Journal failure is not reported as complete success"), R.Ok());
    TestTrue(TEXT("Acquired server lock is explicitly reported"), R.Error.Contains(TEXT("Server lock acquired and verified")));
    GitWorkspace::FRepository Reopened(F.Git, F.Repo);
    const auto S = Reopened.VerifyLocks(TEXT("origin"));
    TestTrue(TEXT("Reservation retained after journal failure"), S.State(TEXT("asset.uasset"), true) == GitWorkspace::ELockState::Ours);
    TestFalse(TEXT("Unrecorded acquisition cannot unlock after restart"), Reopened.ChangeLock(S, TEXT("asset.uasset"), true, true).Ok());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitNewAssetLockTest, "GitWorkspace.Locks.NewUntrackedAssetThroughPanel", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGitNewAssetLockTest::RunTest(const FString&)
{
    FLockFixture F;
    if (!TestFalse(TEXT("Loopback server started"), F.Endpoint.IsEmpty())) return false;
    const FString Asset = TEXT("Content/Lock [水] Tester.uasset");
    IFileManager::Get().MakeDirectory(*FPaths::Combine(F.Repo, TEXT("Content")), true);
    F.Write(Asset, TEXT("saved new asset bytes\n"));
    const auto IndexBefore = F.Call({TEXT("ls-files"), TEXT("--stage"), TEXT("-z")}).Out;
    const auto HeadBefore = F.Call({TEXT("rev-parse"), TEXT("HEAD")}).Text();
    auto Repo = MakeShared<GitWorkspace::FRepository, ESPMode::ThreadSafe>(F.Git, F.Repo);
    auto Panel = SNew(SGitWorkspace).Repository(Repo);
    auto Settle = [&]() { while (Panel->Pending.IsValid()) { Panel->Pending.Wait(); Panel->Tick(FGeometry(), 0, 0); } };
    Settle();
    for (const auto& Row : Panel->Rows) if (Row->Group.IsEmpty() && Row->File.Path == Asset) Panel->List->SetItemSelection(Row, true);
    if (!TestTrue(TEXT("New saved asset can lock before manual verification"), Panel->CanLockSelected())) return false;
    Panel->ChangeLock(false); Settle();
    if (!TestTrue(TEXT("Panel verifies then acquires new asset lock: ") + Panel->Feedback, Panel->Locks.State(Asset, true) == GitWorkspace::ELockState::Ours)) return false;
    TestTrue(TEXT("Successful lock leaves asset selected"), Panel->Selection && Panel->Selection->File.Path == Asset);
    TestFalse(TEXT("An owned lock cannot be acquired again"), Panel->CanLockSelected());
    TestTrue(TEXT("Lock does not stage the new asset"), F.Call({TEXT("ls-files"), TEXT("--stage"), TEXT("-z")}).Out == IndexBefore);
    TestEqual(TEXT("Lock does not commit"), F.Call({TEXT("rev-parse"), TEXT("HEAD")}).Text(), HeadBefore);
    FString Bytes; FFileHelper::LoadFileToString(Bytes, *FPaths::Combine(F.Repo, Asset));
    TestEqual(TEXT("Lock preserves new asset bytes"), Bytes, FString(TEXT("saved new asset bytes\n")));
    TestFalse(TEXT("Unpublished new asset cannot be unlocked"), Repo->ChangeLock(Panel->Locks, Asset, true, true).Ok());

    F.Write(TEXT(".gitignore"), TEXT("Content/Ignored.uasset\n"));
    F.Write(TEXT("Content/Ignored.uasset"), TEXT("ignored\n"));
    auto S = Repo->VerifyLocks(TEXT("origin"));
    TestFalse(TEXT("Ignored new path cannot be locked"), Repo->ChangeLock(S, TEXT("Content/Ignored.uasset"), false).Ok());
    TestFalse(TEXT("Unsaved/missing path cannot be locked"), Repo->ChangeLock(S, TEXT("Content/Missing.uasset"), false).Ok());
    const FString LinkPath = FPaths::Combine(F.Repo, TEXT("Content/UntrackedLink.uasset"));
    TestEqual(TEXT("Create untracked symlink fixture"), symlink(TCHAR_TO_UTF8(*FPaths::Combine(F.Repo, Asset)), TCHAR_TO_UTF8(*LinkPath)), 0);
    S = Repo->VerifyLocks(TEXT("origin"));
    TestFalse(TEXT("Untracked symlink cannot reserve a different file"), Repo->ChangeLock(S, TEXT("Content/UntrackedLink.uasset"), false).Ok());
    // A staged symlink is rejected even though it has effective LFS attributes.
    F.Call({TEXT("update-index"), TEXT("--add"), TEXT("--cacheinfo"), TEXT("120000"), F.Call({TEXT("rev-parse"), TEXT("HEAD:asset.uasset")}).Text().TrimEnd(), TEXT("Content/Link.uasset")});
    F.Write(TEXT("Content/Link.uasset"), TEXT("regular working file over indexed symlink\n"));
    S = Repo->VerifyLocks(TEXT("origin"));
    TestFalse(TEXT("Indexed symlink cannot be locked"), Repo->ChangeLock(S, TEXT("Content/Link.uasset"), false).Ok());
    TestTrue(TEXT("Invalid paths never reached the server"), Repo->VerifyLocks(TEXT("origin")).Locks.Num() == 1);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitStartupLockVerificationTest, "GitWorkspace.Locks.VerifyOnWorkspaceOpen", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGitStartupLockVerificationTest::RunTest(const FString&)
{
    FLockFixture F;
    if (!TestFalse(TEXT("Loopback server started"), F.Endpoint.IsEmpty())) return false;
    {
        GitWorkspace::FRepository Acquirer(F.Git, F.Repo);
        if (!TestTrue(TEXT("Existing lock acquired before opening workspace"), Acquirer.ChangeLock(Acquirer.VerifyLocks(TEXT("origin")), TEXT("asset.uasset"), false).Ok())) return false;
    }
    const auto HeadBefore = F.Call({TEXT("rev-parse"), TEXT("HEAD")}).Text();
    const auto IndexBefore = F.Call({TEXT("ls-files"), TEXT("--stage"), TEXT("-z")}).Out;
    FString BytesBefore; FFileHelper::LoadFileToString(BytesBefore, *FPaths::Combine(F.Repo, TEXT("asset.uasset")));
    auto Requests = [&]() { FString Text; FFileHelper::LoadFileToString(Text, *FPaths::Combine(F.Root, TEXT("requests"))); return Text; };
    const FString RequestsBefore = Requests();
    auto MakePanel = [&]() { return SNew(SGitWorkspace).Repository(MakeShared<GitWorkspace::FRepository, ESPMode::ThreadSafe>(F.Git, F.Repo)); };
    auto Settle = [](const TSharedRef<SGitWorkspace>& Panel) { while (Panel->Pending.IsValid()) { Panel->Pending.Wait(); Panel->Tick(FGeometry(), 0, 0); } };
    auto Panel = MakePanel();
    Panel->Pending.Wait(); Panel->Tick(FGeometry(), 0, 0);
    TestTrue(TEXT("Local snapshot is available before network verification completes"), Panel->Snapshot.bValid);
    TestTrue(TEXT("Startup schedules a visible ownership check"), Panel->bCheckingLocks && !Panel->IsIdle() && Panel->LockStatusText().ToString().Contains(TEXT("Checking locks")));
    Settle(Panel);
    TestFalse(TEXT("Checking indicator clears"), Panel->bCheckingLocks);
    TestTrue(TEXT("Opening discovers existing ownership without a Verify click"), Panel->Locks.State(TEXT("asset.uasset"), true) == GitWorkspace::ELockState::Ours);
    TestEqual(TEXT("Opening preserves HEAD"), F.Call({TEXT("rev-parse"), TEXT("HEAD")}).Text(), HeadBefore);
    TestTrue(TEXT("Opening preserves index"), F.Call({TEXT("ls-files"), TEXT("--stage"), TEXT("-z")}).Out == IndexBefore);
    FString BytesAfter; FFileHelper::LoadFileToString(BytesAfter, *FPaths::Combine(F.Repo, TEXT("asset.uasset")));
    TestEqual(TEXT("Opening preserves asset bytes"), BytesAfter, BytesBefore);
    const FString NewRequests = Requests().Mid(RequestsBefore.Len());
    TestTrue(TEXT("Startup actually queried the server"), NewRequests.Contains(TEXT("/locks/verify")));
    TestFalse(TEXT("Startup never acquires a lock"), NewRequests.Contains(TEXT("/lfs/locks\"")));
    TestFalse(TEXT("Startup never releases a lock"), NewRequests.Contains(TEXT("/unlock")));

    F.Mode(TEXT("auth")); auto Failed = MakePanel(); Settle(Failed);
    TestTrue(TEXT("Failed check leaves usable local status"), Failed->IsIdle() && Failed->Snapshot.bValid && !Failed->bCheckingLocks);
    TestFalse(TEXT("Failed startup never claims verified ownership"), Failed->Locks.IsFresh());
    TestTrue(TEXT("Failure explains manual retry"), Failed->LockStatusText().ToString().Contains(TEXT("Use Verify locks to retry")));
    const FString FailedRequests = Requests();
    for (int32 I = 0; I < 5; ++I) Failed->Tick(FGeometry(), 600, 60);
    Failed->Refresh(); Settle(Failed);
    TestEqual(TEXT("Idle ticks and local Refresh do not retry a failed startup check"), Requests(), FailedRequests);
    F.Mode(TEXT("")); Failed->VerifyLocks(); Settle(Failed);
    TestTrue(TEXT("Manual retry recovers after server failure"), Failed->Locks.State(TEXT("asset.uasset"), true) == GitWorkspace::ELockState::Ours);
    return true;
}

namespace
{
FString HandoffRead(const FString& Path) { FString Text; FFileHelper::LoadFileToString(Text, *Path); return Text; }
FString HandoffTip(FLockFixture& F)
{ FString Tip, Ref; F.Call({TEXT("ls-remote"), TEXT("--refs"), TEXT("origin"), TEXT("refs/heads/main")}).Text().TrimEnd().Split(TEXT("\t"), &Tip, &Ref); return Tip; }
FString HandoffQuote(FString Value) { return TEXT("'") + Value.Replace(TEXT("'"), TEXT("'\"'\"'")) + TEXT("'"); }
void HandoffHook(FLockFixture& F, const FString& Body)
{
    IFileManager::Get().MakeDirectory(*FPaths::Combine(F.Repo, TEXT(".git/test-hooks")), true);
    F.Write(TEXT(".git/test-hooks/pre-push"), TEXT("#!/bin/sh\n") + Body + TEXT("\n"));
    chmod(TCHAR_TO_UTF8(*FPaths::Combine(F.Repo, TEXT(".git/test-hooks/pre-push"))), 0755);
}
bool PrepareHandoff(FLockFixture& F, GitWorkspace::FRepository& Repo)
{
    if (F.Endpoint.IsEmpty() || !F.Call({TEXT("config"), TEXT("lfs.transfer.maxretries"), TEXT("0")}).Ok()) return false;
    F.Write(TEXT("texture.uasset"), TEXT("base texture\n")); F.Write(TEXT("material.uasset"), TEXT("base material\n"));
    if (!F.Call({TEXT("add"), TEXT(".")}).Ok() || !F.Call({TEXT("commit"), TEXT("-qm"), TEXT("fixture assets")}).Ok() || !F.Call({TEXT("push"), TEXT("--no-verify"), TEXT("origin"), TEXT("main")}).Ok()) return false;
    for (const FString Path : {FString(TEXT("asset.uasset")), FString(TEXT("texture.uasset")), FString(TEXT("material.uasset"))})
        if (!Repo.ChangeLock(Repo.VerifyLocks(TEXT("origin")), Path, false).Ok()) return false;
    F.Write(TEXT("asset.uasset"), TEXT("committed Blueprint\n")); F.Write(TEXT("texture.uasset"), TEXT("committed texture\n")); F.Write(TEXT("material.uasset"), TEXT("committed material\n"));
    if (!Repo.Stage({TEXT("asset.uasset"), TEXT("texture.uasset"), TEXT("material.uasset")}).Ok() || !Repo.Commit(Repo.Refresh(), TEXT("committed feature assets")).Ok()) return false;
    F.Write(TEXT("README.md"), TEXT("committed feature notes\n"));
    if (!Repo.Stage({TEXT("README.md")}).Ok() || !Repo.Commit(Repo.Refresh(), TEXT("feature notes")).Ok()) return false;
    F.Write(TEXT("asset.uasset"), TEXT("resume Blueprint later\n"));
    if (!F.Call({TEXT("stash"), TEXT("push"), TEXT("-m"), TEXT("pending Blueprint work")}).Ok()) return false;
    F.Write(TEXT("README.md"), TEXT("unrelated staged notes\n"));
    if (!Repo.Stage({TEXT("README.md")}).Ok()) return false;
    F.Write(TEXT("README.md"), TEXT("unrelated working notes\n")); return true;
}
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitSelectivePushHandoffTest, "GitWorkspace.Handoff.SelectivePushAndLfs", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGitSelectivePushHandoffTest::RunTest(const FString&)
{
    FLockFixture F; GitWorkspace::FRepository Repo(F.Git, F.Repo);
    if (!TestTrue(TEXT("Prepare locked feature and pending Blueprint stash"), PrepareHandoff(F, Repo))) return false;
    const auto Before = Repo.Refresh(); const auto Stashes = Repo.ListStashes();
    const auto Review = Repo.ReviewPushHandoff(Repo.Fetch());
    if (!TestTrue(TEXT("Handoff review fresh: ") + Review.Error, Review.IsFresh())) return false;
    TestEqual(TEXT("Every outgoing commit listed"), Review.Commits.Num(), 2);
    TestTrue(TEXT("Unchecked Blueprint is still in outgoing publication"), Review.Text({TEXT("texture.uasset")}).Contains(TEXT("asset.uasset")) && Review.Commits[0].Paths.Contains(TEXT("asset.uasset")));
    const auto* Blueprint = Review.Assets.FindByPredicate([](const auto& A) { return A.Path == TEXT("asset.uasset"); });
    TestTrue(TEXT("Saved Blueprint work blocks only its release"), Blueprint && !Blueprint->bReady && Blueprint->Error.Contains(TEXT("stash")));
    const auto Result = Repo.ExecutePushHandoff(Review, {TEXT("texture.uasset"), TEXT("material.uasset")}, true);
    if (!TestTrue(TEXT("Push verified before selected releases: ") + Result.Text(), Result.bPushVerified)) return false;
    TestTrue(TEXT("Both selected locks released"), Result.Assets.Num() == 2 && Result.Assets[0].bReleased && Result.Assets[1].bReleased);
    TestEqual(TEXT("Reviewed HEAD published"), HandoffTip(F), Review.Remote.Head);
    TestTrue(TEXT("Blueprint lock kept"), Result.Locks.State(TEXT("asset.uasset"), true) == GitWorkspace::ELockState::Ours);
    TestTrue(TEXT("Index preserved"), Repo.Refresh().IndexEntries == Before.IndexEntries);
    TestEqual(TEXT("Working notes preserved"), HandoffRead(FPaths::Combine(F.Repo, TEXT("README.md"))), FString(TEXT("unrelated working notes\n")));
    TestEqual(TEXT("Saved Blueprint stash preserved"), Repo.ListStashes().Fingerprint, Stashes.Fingerprint);
    const FString Clone = FPaths::Combine(F.Root, TEXT("clone"));
    auto At = [&](const TArray<FString>& Args) { return GitWorkspace::Run(F.Git, Clone, Args); };
    if (!TestTrue(TEXT("Clone actual published repository"), F.Call({TEXT("clone"), TEXT("--no-checkout"), F.Remote, Clone}).Ok())) return false;
    if (!TestTrue(TEXT("Configure clone LFS"), At({TEXT("lfs"), TEXT("install"), TEXT("--local"), TEXT("--skip-repo")}).Ok() && At({TEXT("config"), TEXT("lfs.url"), F.Endpoint}).Ok())) return false;
    if (!TestTrue(TEXT("Another clone downloads all published LFS objects"), At({TEXT("checkout"), TEXT("main")}).Ok())) return false;
    TestEqual(TEXT("Unselected Blueprint was published as committed bytes"), HandoffRead(FPaths::Combine(Clone, TEXT("asset.uasset"))), FString(TEXT("committed Blueprint\n")));
    TestEqual(TEXT("Released texture hydrates"), HandoffRead(FPaths::Combine(Clone, TEXT("texture.uasset"))), FString(TEXT("committed texture\n")));
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitPartialPushHandoffTest, "GitWorkspace.Handoff.PartialReleaseAndRetry", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGitPartialPushHandoffTest::RunTest(const FString&)
{
    FLockFixture F; GitWorkspace::FRepository Repo(F.Git, F.Repo);
    if (!TestTrue(TEXT("Prepare handoff"), PrepareHandoff(F, Repo))) return false;
    const FString Counter = FPaths::Combine(F.Root, TEXT("push-count"));
    HandoffHook(F, TEXT("printf 'push\\n' >> ") + HandoffQuote(Counter));
    FFileHelper::SaveStringToFile(TEXT("material.uasset"), *FPaths::Combine(F.Root, TEXT("fail-unlock-path")));
    auto Review = Repo.ReviewPushHandoff(Repo.Fetch());
    auto Result = Repo.ExecutePushHandoff(Review, {TEXT("texture.uasset"), TEXT("material.uasset")}, true);
    TestTrue(TEXT("Publication succeeded despite later release refusal"), Result.bPushVerified && !Result.bUnlockOnly);
    TestTrue(TEXT("Texture success and material failure reported individually"), Result.Assets.Num() == 2 && Result.Assets[0].bReleased && !Result.Assets[1].bReleased);
    TestTrue(TEXT("Failed material reservation remains ours"), Result.Locks.State(TEXT("material.uasset"), true) == GitWorkspace::ELockState::Ours);
    TestTrue(TEXT("Result directs unlock-only retry"), Result.Text().Contains(TEXT("Do not repeat Push")));
    const FString Published = HandoffTip(F);
    // An intentionally stale tracking ref must not require an external Fetch.
    F.Call({TEXT("update-ref"), TEXT("refs/remotes/origin/main"), Review.Remote.RemoteHead});
    IFileManager::Get().Delete(*FPaths::Combine(F.Root, TEXT("fail-unlock-path")));
    Review = Repo.ReviewPushHandoff(Repo.Fetch());
    TestTrue(TEXT("Retry is ready using live publication, despite stale tracking ref"), Review.IsFresh() && Review.IsRetry());
    Result = Repo.ExecutePushHandoff(Review, {TEXT("material.uasset")}, true);
    TestTrue(TEXT("Retry releases remaining material only"), Result.bPushVerified && Result.bUnlockOnly && Result.Assets.Num() == 1 && Result.Assets[0].bReleased);
    TestEqual(TEXT("No second Push hook executed"), HandoffRead(Counter), FString(TEXT("push\n")));
    TestEqual(TEXT("Retry preserves published ref"), HandoffTip(F), Published);
    TestTrue(TEXT("Blueprint reservation still ours"), Result.Locks.State(TEXT("asset.uasset"), true) == GitWorkspace::ELockState::Ours);
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitFailedPushHandoffTest, "GitWorkspace.Handoff.FailedAndUncertainPush", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGitFailedPushHandoffTest::RunTest(const FString&)
{
    FLockFixture F; GitWorkspace::FRepository Repo(F.Git, F.Repo);
    if (!TestTrue(TEXT("Prepare handoff"), PrepareHandoff(F, Repo))) return false;
    auto Review = Repo.ReviewPushHandoff(Repo.Fetch()); const FString Before = HandoffTip(F);
    HandoffHook(F, TEXT("echo handoff-hook-rejected >&2\nexit 1"));
    auto Result = Repo.ExecutePushHandoff(Review, {TEXT("texture.uasset")}, true);
    TestFalse(TEXT("Rejected Push cannot release"), Result.bPushVerified);
    TestTrue(TEXT("Push error visible"), Result.Error.Contains(TEXT("handoff-hook-rejected")));
    TestEqual(TEXT("Rejected ref unchanged"), HandoffTip(F), Before);
    TestFalse(TEXT("No unlock request after rejected Push"), HandoffRead(FPaths::Combine(F.Root, TEXT("requests"))).Contains(TEXT("/unlock")));
    F.Mode(TEXT("uploadfail"));
    Result = Repo.ExecutePushHandoff(Review, {TEXT("texture.uasset")}, true);
    TestTrue(TEXT("LFS failure prevents publication and release"), !Result.bPushVerified && Result.Error.Contains(TEXT("LFS upload failed")) && HandoffTip(F) == Before);
    F.Mode(TEXT("")); IFileManager::Get().Delete(*FPaths::Combine(F.Repo, TEXT(".git/test-hooks/pre-push")));
    const FString Marker = FPaths::Combine(F.Root, TEXT("push-sent"));
    const FString Wrapper = FPaths::Combine(F.Root, TEXT("uncertain-git.sh"));
    const FString Script = TEXT("#!/bin/sh\ncase \"$*\" in\n *'push --porcelain'*) ") + HandoffQuote(F.Git) + TEXT(" \"$@\"; result=$?; if [ $result -eq 0 ]; then touch ") + HandoffQuote(Marker) +
        TEXT("; fi; exit $result;;\n *'ls-remote '*) if [ -f ") + HandoffQuote(Marker) + TEXT(" ]; then echo fixture-confirmation-unavailable >&2; exit 1; fi;;\nesac\nexec ") + HandoffQuote(F.Git) + TEXT(" \"$@\"\n");
    FFileHelper::SaveStringToFile(Script, *Wrapper, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM); chmod(TCHAR_TO_UTF8(*Wrapper), 0755);
    GitWorkspace::FRepository Uncertain(Wrapper, F.Repo);
    Review = Uncertain.ReviewPushHandoff(Uncertain.Fetch());
    Result = Uncertain.ExecutePushHandoff(Review, {TEXT("texture.uasset")}, true);
    TestFalse(TEXT("Lost remote confirmation cannot release"), Result.bPushVerified);
    TestEqual(TEXT("Fixture Git push actually reached remote"), HandoffTip(F), Review.Remote.Head);
    TestTrue(TEXT("Uncertainty is explicit"), Result.Error.Contains(TEXT("could not be confirmed")));
    TestFalse(TEXT("No unlock request after uncertain publication"), HandoffRead(FPaths::Combine(F.Root, TEXT("requests"))).Contains(TEXT("/unlock")));
    TestTrue(TEXT("All three locks retained"), Result.Locks.IsFresh() && Result.Locks.Locks.Num() == 3);
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitPushHandoffPreflightTest, "GitWorkspace.Handoff.SelectionAndReviewRaces", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGitPushHandoffPreflightTest::RunTest(const FString&)
{
    FLockFixture F; GitWorkspace::FRepository Repo(F.Git, F.Repo);
    if (!TestTrue(TEXT("Prepare handoff"), PrepareHandoff(F, Repo))) return false;
    const auto Review = Repo.ReviewPushHandoff(Repo.Fetch()); const FString Tip = HandoffTip(F);
    TestFalse(TEXT("No confirmation cannot publish"), Repo.ExecutePushHandoff(Review, {TEXT("texture.uasset")}).bPushVerified);
    TestFalse(TEXT("Empty selection cannot publish"), Repo.ExecutePushHandoff(Review, {}, true).bPushVerified);
    TestFalse(TEXT("Duplicate selection refused"), Repo.ExecutePushHandoff(Review, {TEXT("texture.uasset"), TEXT("texture.uasset")}, true).bPushVerified);
    TestFalse(TEXT("Unreviewed path cannot publish"), Repo.ExecutePushHandoff(Review, {TEXT("not-reviewed.uasset")}, true).bPushVerified);
    TestFalse(TEXT("Blocked stash path cannot publish"), Repo.ExecutePushHandoff(Review, {TEXT("asset.uasset")}, true).bPushVerified);
    auto Expired = Review; Expired.Locks.VerifiedSeconds -= 61;
    TestFalse(TEXT("Expired review refused"), Repo.ExecutePushHandoff(Expired, {TEXT("texture.uasset")}, true).bPushVerified);
    F.Write(TEXT("texture.uasset"), TEXT("changed since review\n"));
    TestFalse(TEXT("New working edit prevents publication/release"), Repo.ExecutePushHandoff(Review, {TEXT("texture.uasset")}, true).bPushVerified);
    F.Call({TEXT("restore"), TEXT("--"), TEXT("texture.uasset")});
    F.Call({TEXT("lfs"), TEXT("unlock"), TEXT("--remote=origin"), TEXT("--id=") + Review.Locks.Locks[TEXT("texture.uasset")].Id});
    Repo.ChangeLock(Repo.VerifyLocks(TEXT("origin")), TEXT("texture.uasset"), false);
    const auto Replacement = Repo.VerifyLocks(TEXT("origin"));
    TestFalse(TEXT("Replacement identity refuses old selection"), Repo.ExecutePushHandoff(Review, {TEXT("texture.uasset")}, true).bPushVerified);
    TestEqual(TEXT("Replacement remains owned"), Repo.VerifyLocks(TEXT("origin")).Locks[TEXT("texture.uasset")].Id, Replacement.Locks[TEXT("texture.uasset")].Id);
    auto Current = Repo.ReviewPushHandoff(Repo.Fetch());
    F.Write(TEXT("README.md"), TEXT("changed commit\n")); Repo.Stage({TEXT("README.md")}); Repo.Commit(Repo.Refresh(), TEXT("new HEAD"));
    TestFalse(TEXT("Commit drift refuses publication"), Repo.ExecutePushHandoff(Current, {TEXT("texture.uasset")}, true).bPushVerified);
    TestEqual(TEXT("All preflight refusals preserve remote"), HandoffTip(F), Tip);
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitPushHandoffPostflightTest, "GitWorkspace.Handoff.PostPushEditsAndReplacement", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGitPushHandoffPostflightTest::RunTest(const FString&)
{
    FLockFixture F; GitWorkspace::FRepository Repo(F.Git, F.Repo);
    if (!TestTrue(TEXT("Prepare handoff"), PrepareHandoff(F, Repo))) return false;
    auto Review = Repo.ReviewPushHandoff(Repo.Fetch());
    HandoffHook(F, TEXT("printf 'hook local edit\\n' > ") + HandoffQuote(FPaths::Combine(F.Repo, TEXT("texture.uasset"))));
    auto Result = Repo.ExecutePushHandoff(Review, {TEXT("texture.uasset"), TEXT("material.uasset")}, true);
    TestTrue(TEXT("Push succeeds but changed texture retained"), Result.bPushVerified && Result.Assets.Num() == 2 && !Result.Assets[0].bReleased && Result.Assets[1].bReleased);
    TestTrue(TEXT("Texture reason reports local changes"), Result.Assets[0].Error.Contains(TEXT("working or staged")));
    TestEqual(TEXT("Unexpected hook edit preserved"), HandoffRead(FPaths::Combine(F.Repo, TEXT("texture.uasset"))), FString(TEXT("hook local edit\n")));
    FLockFixture Other; GitWorkspace::FRepository OtherRepo(Other.Git, Other.Repo);
    if (!TestTrue(TEXT("Prepare replacement race"), PrepareHandoff(Other, OtherRepo))) return false;
    Review = OtherRepo.ReviewPushHandoff(OtherRepo.Fetch()); const FString OldId = Review.Locks.Locks[TEXT("texture.uasset")].Id;
    HandoffHook(Other, HandoffQuote(Other.Git) + TEXT(" lfs unlock --remote=origin --id=") + HandoffQuote(OldId) + TEXT(" >/dev/null && ") + HandoffQuote(Other.Git) + TEXT(" lfs lock --remote=origin texture.uasset >/dev/null"));
    Result = OtherRepo.ExecutePushHandoff(Review, {TEXT("texture.uasset")}, true);
    TestTrue(TEXT("Replacement after Push is not released"), Result.bPushVerified && Result.Assets.Num() == 1 && !Result.Assets[0].bReleased);
    TestTrue(TEXT("New server identity remains ours"), Result.Locks.Locks.Contains(TEXT("texture.uasset")) && Result.Locks.Locks[TEXT("texture.uasset")].Id != OldId);
    TestTrue(TEXT("Identity refusal explicit"), Result.Assets[0].Error.Contains(TEXT("identity")));
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitPushHandoffPanelTest, "GitWorkspace.Handoff.ReviewSelectionCancelAndRetryPanel", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGitPushHandoffPanelTest::RunTest(const FString&)
{
    FLockFixture F; auto Repo = MakeShared<GitWorkspace::FRepository, ESPMode::ThreadSafe>(F.Git, F.Repo);
    if (!TestTrue(TEXT("Prepare panel handoff"), PrepareHandoff(F, *Repo))) return false;
    auto Panel = SNew(SGitWorkspace).Repository(Repo);
    auto Settle = [&]() { while (Panel->Pending.IsValid()) { Panel->Pending.Wait(); Panel->Tick(FGeometry(), 0, 0); } };
    Settle(); Panel->Remote = Repo->Fetch(); Panel->ShowPushHandoff(); Settle();
    if (!TestTrue(TEXT("Panel prepares fresh handoff"), Panel->HandoffReview.IsFresh())) return false;
    TestTrue(TEXT("No locks checked by default"), Panel->HandoffChecked.IsEmpty());
    TestFalse(TEXT("Empty selection disables action"), Panel->CanRunPushHandoff());
    Panel->HandoffChecked.Add(TEXT("texture.uasset")); Panel->HandoffChecked.Add(TEXT("material.uasset")); Panel->UpdateHandoffReport();
    TestTrue(TEXT("Eligible selected locks enable action"), Panel->CanRunPushHandoff());
    TestTrue(TEXT("Report distinguishes publication and kept locks"), Panel->HandoffReport->GetText().ToString().Contains(TEXT("ALL outgoing commits")) && Panel->HandoffReport->GetText().ToString().Contains(TEXT("LOCKS TO KEEP\nasset.uasset")));
    const auto DisplayLocks = Panel->Locks;
    Panel->LockRemote = TEXT("another-display-remote"); Panel->Locks = GitWorkspace::FLockSnapshot(); Panel->Locks.Remote = Panel->LockRemote;
    Panel->RefreshPushHandoff(); Settle();
    TestEqual(TEXT("Handoff on upstream does not replace a different main lock-remote snapshot"), Panel->Locks.Remote, FString(TEXT("another-display-remote")));
    Panel->LockRemote = TEXT("origin"); Panel->Locks = DisplayLocks;
    const FString DirtyName = TEXT("/Game/Automation/HandoffDirty_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
    UPackage* Dirty = CreatePackage(*DirtyName); UTexture2D* Object = NewObject<UTexture2D>(Dirty, TEXT("HandoffDirtyTexture"), RF_Public | RF_Standalone); Dirty->SetDirtyFlag(true);
    TestFalse(TEXT("Unsaved editor package blocks combined handoff"), Panel->CanRunPushHandoff());
    Dirty->SetDirtyFlag(false); Object->ClearFlags(RF_Public | RF_Standalone);
    const FString Tip = HandoffTip(F);
    { TGuardValue<bool> Unattended(GIsRunningUnattendedScript, true); Panel->RunPushHandoff(); }
    TestTrue(TEXT("Cancellation visible"), Panel->Feedback.Contains(TEXT("cancelled")));
    TestEqual(TEXT("Cancelled action sends no Push"), HandoffTip(F), Tip);
    TestFalse(TEXT("Cancelled action sends no release"), HandoffRead(FPaths::Combine(F.Root, TEXT("requests"))).Contains(TEXT("/unlock")));
    Panel->HandoffReview.Locks.VerifiedSeconds -= 61; TestFalse(TEXT("Expired review disables action"), Panel->CanRunPushHandoff());
    Panel->RefreshPushHandoff(); Settle();
    const auto Review = Panel->HandoffReview;
    FFileHelper::SaveStringToFile(TEXT("material.uasset"), *FPaths::Combine(F.Root, TEXT("fail-unlock-path")));
    const auto Result = Repo->ExecutePushHandoff(Review, {TEXT("texture.uasset"), TEXT("material.uasset")}, true);
    if (!TestTrue(TEXT("Actual partial operation produced retry case"), Result.bPushVerified && Result.Assets.Num() == 2 && !Result.Assets[1].bReleased)) return false;
    Panel->ApplyPushHandoffResult(Review, Result); Settle();
    TestTrue(TEXT("Only failed original ID retained for retry"), Panel->HandoffRetryIds.Num() == 1 && Panel->HandoffRetryIds.Contains(TEXT("material.uasset")));
    Panel->RefreshPushHandoff(); Settle();
    TestTrue(TEXT("Panel retry prepares unlock-only mode"), Panel->HandoffReview.IsFresh() && Panel->HandoffReview.IsRetry());
    Panel->HandoffChecked.Add(TEXT("asset.uasset")); TestFalse(TEXT("Unchecked reservation cannot be added to retry scope"), Panel->CanRunPushHandoff());
    Panel->HandoffChecked.Empty(); Panel->HandoffChecked.Add(TEXT("material.uasset")); Panel->UpdateHandoffReport();
    TestTrue(TEXT("Only remaining eligible reservation can retry"), Panel->CanRunPushHandoff());
    TestTrue(TEXT("Retry report says no Push"), Panel->HandoffReport->GetText().ToString().Contains(TEXT("No Push will be sent")));
    F.Write(TEXT("README.md"), TEXT("commit after publication\n")); Repo->Stage({TEXT("README.md")}); Repo->Commit(Repo->Refresh(), TEXT("new commit after handoff"));
    Panel->RefreshPushHandoff(); Settle(); TestFalse(TEXT("Changed commit disables unlock-only retry"), Panel->CanRunPushHandoff());
    TestTrue(TEXT("Retry commit/context blocker explained"), Panel->HandoffReview.Error.Contains(TEXT("published commit or acquisition context changed")));
    return true;
}
#endif
#endif
