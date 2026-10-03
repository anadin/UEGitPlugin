// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#if WITH_DEV_AUTOMATION_TESTS
#include "GitWorkspaceRepository.h"
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
struct FRemoteFixture
{
    FString Root = FPaths::Combine(FPlatformProcess::UserTempDir(), TEXT("uegit-remote-") + FGuid::NewGuid().ToString(EGuidFormats::Digits));
    FString A = FPaths::Combine(Root, TEXT("a")), B = FPaths::Combine(Root, TEXT("b")), Remote = FPaths::Combine(Root, TEXT("remote.git"));
    FString Git = GitWorkspace::FindGitExecutable();
    FRemoteFixture()
    {
        IFileManager::Get().MakeDirectory(*A, true);
        At(A, {TEXT("init"), TEXT("-q"), TEXT("-b"), TEXT("main")}); Setup(A);
        Write(A, TEXT("README.md"), TEXT("base\n")); Commit(A);
        At(A, {TEXT("init"), TEXT("--bare"), Remote});
        At(A, {TEXT("remote"), TEXT("add"), TEXT("origin"), Remote});
        At(A, {TEXT("push"), TEXT("-u"), TEXT("origin"), TEXT("main")});
        At(A, {TEXT("clone"), TEXT("-c"), TEXT("core.autocrlf=false"), TEXT("--branch"), TEXT("main"), Remote, B}); Setup(B);
    }
    ~FRemoteFixture() { IFileManager::Get().DeleteDirectory(*Root, false, true); }
    GitWorkspace::FResult At(const FString& Dir, const TArray<FString>& Args) { return GitWorkspace::Run(Git, Dir, Args); }
    void Setup(const FString& Dir)
    {
        At(Dir, {TEXT("config"), TEXT("user.name"), TEXT("UEGit remote fixture")});
        At(Dir, {TEXT("config"), TEXT("user.email"), TEXT("fixture@example.invalid")});
        At(Dir, {TEXT("config"), TEXT("commit.gpgsign"), TEXT("false")});
        At(Dir, {TEXT("config"), TEXT("core.autocrlf"), TEXT("false")});
        At(Dir, {TEXT("config"), TEXT("core.hooksPath"), TEXT(".git/test-hooks")});
        At(Dir, {TEXT("lfs"), TEXT("install"), TEXT("--local"), TEXT("--skip-repo")});
    }
    void Write(const FString& Dir, const FString& Path, const FString& Text)
    {
        const FString Full = FPaths::Combine(Dir, Path); IFileManager::Get().MakeDirectory(*FPaths::GetPath(Full), true);
        FFileHelper::SaveStringToFile(Text, *Full, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
    }
    FString Read(const FString& Dir, const FString& Path) { FString S; FFileHelper::LoadFileToString(S, *FPaths::Combine(Dir, Path)); return S; }
    void Commit(const FString& Dir) { At(Dir, {TEXT("add"), TEXT("-A")}); At(Dir, {TEXT("commit"), TEXT("-qm"), TEXT("fixture update")}); }
    FString Tip() { return At(Remote, {TEXT("rev-parse"), TEXT("refs/heads/main")}).Text().TrimEnd(); }
};
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitRemoteFetchPullTest, "GitWorkspace.Remote.FetchAndGuardedPull", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGitRemoteFetchPullTest::RunTest(const FString&)
{
    FRemoteFixture F; GitWorkspace::FRepository Repo(F.Git, F.A);
    const FString Original = Repo.Refresh().Head;
    F.Write(F.B, TEXT("README.md"), TEXT("incoming docs\n")); F.Commit(F.B); F.At(F.B, {TEXT("push"), TEXT("origin"), TEXT("main")});
    auto Review = Repo.Fetch();
    if (!TestTrue(TEXT("Fetch succeeds: ") + Review.Error, Review.IsFresh())) return false;
    TestEqual(TEXT("One incoming"), Review.Behind, 1); TestEqual(TEXT("No outgoing"), Review.Ahead, 0);
    TestEqual(TEXT("Fetch preserves HEAD"), Repo.Refresh().Head, Original);
    TestEqual(TEXT("Fetch preserves files"), F.Read(F.A, TEXT("README.md")), FString(TEXT("base\n")));
    F.Write(F.A, TEXT("README.md"), TEXT("local working\n")); Repo.Stage({TEXT("README.md")});
    const auto Index = Repo.Refresh().IndexEntries; auto DirtyFetch = Repo.Fetch();
    TestTrue(TEXT("Fetch preserves index"), Repo.Refresh().IndexEntries == Index);
    TestFalse(TEXT("Dirty Pull blocked"), Repo.Pull(DirtyFetch).Ok());
    F.At(F.A, {TEXT("restore"), TEXT("--source=HEAD"), TEXT("--staged"), TEXT("--worktree"), TEXT("--"), TEXT("README.md")});
    auto Pulled = Repo.Pull(Review);
    TestTrue(TEXT("Documentation fast-forward: ") + Pulled.Error, Pulled.Ok());
    TestEqual(TEXT("Exact reviewed commit integrated"), Repo.Refresh().Head, Review.RemoteHead);
    TestEqual(TEXT("Incoming document updated"), F.Read(F.A, TEXT("README.md")), FString(TEXT("incoming docs\n")));
    F.Write(F.B, TEXT("Content/Probe.uasset"), TEXT("package fixture\n")); F.Commit(F.B); F.At(F.B, {TEXT("push"), TEXT("origin"), TEXT("main")});
    Review = Repo.Fetch(); const FString Before = Repo.Refresh().Head;
    TestFalse(TEXT("Asset Pull requires editor handoff"), Repo.Pull(Review).Ok());
    TestEqual(TEXT("Blocked asset update preserves HEAD"), Repo.Refresh().Head, Before);
    TestFalse(TEXT("Blocked asset not created"), IFileManager::Get().FileExists(*FPaths::Combine(F.A, TEXT("Content/Probe.uasset"))));
    F.Write(F.A, TEXT("README.md"), TEXT("local divergence\n")); F.Commit(F.A); Review = Repo.Fetch();
    TestTrue(TEXT("Divergence represented"), Review.Ahead == 1 && Review.Behind == 1);
    TestFalse(TEXT("Divergent Pull refused"), Repo.Pull(Review).Ok());
    TestFalse(TEXT("Divergent Push refused"), Repo.Push(Review).Ok());
    TestTrue(TEXT("Temporary fetch refs cleaned"), F.At(F.A, {TEXT("for-each-ref"), TEXT("refs/uegit/fetched/")}).Out.IsEmpty());
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitRemotePushTest, "GitWorkspace.Remote.ReviewedPushAndRejection", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGitRemotePushTest::RunTest(const FString&)
{
    FRemoteFixture F; GitWorkspace::FRepository Repo(F.Git, F.A);
    F.Write(F.A, TEXT("README.md"), TEXT("committed A\n")); F.Commit(F.A);
    auto Review = Repo.Fetch();
    F.Write(F.A, TEXT("README.md"), TEXT("staged B\n")); Repo.Stage({TEXT("README.md")});
    F.Write(F.A, TEXT("README.md"), TEXT("working C\n")); const auto Index = Repo.Refresh().IndexEntries;
    auto Pushed = Repo.Push(Review);
    if (!TestTrue(TEXT("Explicit commit pushed: ") + Pushed.Error, Pushed.Ok())) return false;
    TestEqual(TEXT("Only reviewed commit published"), F.Tip(), Review.Head);
    TestTrue(TEXT("Index unchanged by Push"), Repo.Refresh().IndexEntries == Index);
    TestEqual(TEXT("Working data unchanged by Push"), F.Read(F.A, TEXT("README.md")), FString(TEXT("working C\n")));
    F.Commit(F.A); Review = Repo.Fetch(); const FString Before = F.Tip();
    auto Stale = Review; Stale.FetchedSeconds -= 301;
    TestFalse(TEXT("Expired review rejected"), Repo.Push(Stale).Ok());
#if PLATFORM_MAC
    F.Write(F.A, TEXT(".git/test-hooks/pre-push"), TEXT("#!/bin/sh\necho fixture-push-rejected >&2\nexit 1\n"));
    chmod(TCHAR_TO_UTF8(*FPaths::Combine(F.A, TEXT(".git/test-hooks/pre-push"))), 0755);
    auto Rejected = Repo.Push(Review);
    TestFalse(TEXT("Normal pre-push hook honored"), Rejected.Ok());
    TestTrue(TEXT("Hook error visible"), Rejected.Error.Contains(TEXT("fixture-push-rejected")));
    TestEqual(TEXT("Rejected push leaves remote unchanged"), F.Tip(), Before);
    IFileManager::Get().Delete(*FPaths::Combine(F.A, TEXT(".git/test-hooks/pre-push")));
#endif
    const auto Fetched = F.At(F.B, {TEXT("fetch"), TEXT("origin")});
    if (!TestTrue(TEXT("Other clone fetches published commit: ") + Fetched.Error, Fetched.Ok())) return false;
    const auto Integrated = F.At(F.B, {TEXT("merge"), TEXT("--ff-only"), TEXT("origin/main")});
    if (!TestTrue(TEXT("Other clone integrates published commit: ") + Integrated.Error, Integrated.Ok())) return false;
    F.Write(F.B, TEXT("Docs/race.md"), TEXT("another publisher\n")); F.Commit(F.B);
    const auto Advanced = F.At(F.B, {TEXT("push"), TEXT("origin"), TEXT("main")});
    if (!TestTrue(TEXT("Other clone advances remote: ") + Advanced.Error, Advanced.Ok())) return false;
    TestFalse(TEXT("Changed remote invalidates reviewed push"), Repo.Push(Review).Ok());
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitRemoteLfsTest, "GitWorkspace.Remote.LfsUploadBeforePush", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGitRemoteLfsTest::RunTest(const FString&)
{
    FRemoteFixture F; GitWorkspace::FRepository Repo(F.Git, F.A);
    F.Write(F.A, TEXT(".gitattributes"), TEXT("*.uasset filter=lfs diff=lfs merge=lfs -text\n"));
    F.Write(F.A, TEXT("Probe.uasset"), TEXT("LFS payload A\n")); F.Commit(F.A);
    auto Review = Repo.Fetch(); auto R = Repo.Push(Review);
    if (!TestTrue(TEXT("LFS upload and Git push: ") + R.Error, R.Ok())) return false;
    auto Updated = F.At(F.B, {TEXT("pull"), TEXT("--ff-only")});
    TestTrue(TEXT("Second clone downloads pushed LFS object: ") + Updated.Error, Updated.Ok());
    TestEqual(TEXT("Hydrated bytes available in second clone"), F.Read(F.B, TEXT("Probe.uasset")), FString(TEXT("LFS payload A\n")));
    F.Write(F.A, TEXT("Probe.uasset"), TEXT("LFS payload B\n")); F.Commit(F.A); Review = Repo.Fetch();
    const FString Before = F.Tip();
    const FString Pointer = F.At(F.A, {TEXT("show"), TEXT("HEAD:Probe.uasset")}).Text();
    int32 Start = Pointer.Find(TEXT("oid sha256:"));
    if (!TestTrue(TEXT("Pointer identified"), Start != INDEX_NONE)) return false;
    const FString Oid = Pointer.Mid(Start + 11, 64);
    const FString Object = FPaths::Combine(F.A, TEXT(".git/lfs/objects"), Oid.Left(2), Oid.Mid(2, 2), Oid);
    TestTrue(TEXT("Remove only disposable fixture object"), IFileManager::Get().Delete(*Object));
    // LFS can repair a missing object from matching working bytes. Make the
    // reviewed bytes unavailable while retaining a newer local edit.
    F.Write(F.A, TEXT("Probe.uasset"), TEXT("newer uncommitted payload C\n"));
    R = Repo.Push(Review);
    TestFalse(TEXT("Missing LFS object blocks Git push"), R.Ok());
    TestEqual(TEXT("LFS failure preserves remote branch"), F.Tip(), Before);
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitRemoteLockHistoryTest, "GitWorkspace.Remote.LockableHistoryBeforePush", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGitRemoteLockHistoryTest::RunTest(const FString&)
{
    FRemoteFixture F; GitWorkspace::FRepository Repo(F.Git, F.A);
    const FString Before = F.Tip();
    F.Write(F.A, TEXT(".gitattributes"), TEXT("*.uasset lockable\n"));
    F.Write(F.A, TEXT("Probe.uasset"), TEXT("intermediate asset revision\n")); F.Commit(F.A);
    TestTrue(TEXT("Remove fixture additions"), F.At(F.A, {TEXT("rm"), TEXT("--"), TEXT(".gitattributes"), TEXT("Probe.uasset")}).Ok());
    F.Commit(F.A);
    TestTrue(TEXT("Final trees identical"), F.At(F.A, {TEXT("diff"), TEXT("--exit-code"), Before, TEXT("HEAD"), TEXT("--")}).Ok());
    const auto Review = Repo.Fetch();
    TestTrue(TEXT("Intermediate revisions are outgoing"), Review.IsFresh() && Review.Ahead == 2);
    const auto Result = Repo.Push(Review);
    TestFalse(TEXT("Unverified lock history blocks push"), Result.Ok());
    TestTrue(TEXT("Lock check covers reverted intermediate asset"), Result.Error.Contains(TEXT("Push requires verified asset locks")));
    TestEqual(TEXT("No outgoing history published"), F.Tip(), Before);
    FRemoteFixture Deleted; GitWorkspace::FRepository DeletedRepo(Deleted.Git, Deleted.A);
    Deleted.Write(Deleted.A, TEXT(".gitattributes"), TEXT("README.md lockable\n")); Deleted.Commit(Deleted.A);
    TestTrue(TEXT("Remove fixture file and rule together"), Deleted.At(Deleted.A, {TEXT("rm"), TEXT("--"), TEXT(".gitattributes"), TEXT("README.md")}).Ok());
    Deleted.Commit(Deleted.A);
    const auto DeletedResult = DeletedRepo.Push(DeletedRepo.Fetch());
    TestTrue(TEXT("Deleted file uses parent lockable attributes"), DeletedResult.Error.Contains(TEXT("Push requires verified asset locks")));
    return true;
}
#endif
