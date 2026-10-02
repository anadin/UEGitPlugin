// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#if WITH_DEV_AUTOMATION_TESTS
#include "GitWorkspaceRepository.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Interfaces/IPluginManager.h"

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
#endif
#endif
