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
#include "GitWorkspaceSession.h"
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
        const FString Full = FPaths::IsRelative(Path) ? FPaths::Combine(Dir, Path) : Path; IFileManager::Get().MakeDirectory(*FPaths::GetPath(Full), true);
        FFileHelper::SaveStringToFile(Text, *Full, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
    }
    FString Read(const FString& Dir, const FString& Path) { FString S; FFileHelper::LoadFileToString(S, *(FPaths::IsRelative(Path) ? FPaths::Combine(Dir, Path) : Path)); return S; }
    void Commit(const FString& Dir) { At(Dir, {TEXT("add"), TEXT("-A")}); At(Dir, {TEXT("commit"), TEXT("-qm"), TEXT("fixture update")}); }
    FString Tip() { return At(Remote, {TEXT("rev-parse"), TEXT("refs/heads/main")}).Text().TrimEnd(); }
    FString Oid(const FString& Dir, const FString& Path)
    {
        const FString Pointer = At(Dir, {TEXT("show"), TEXT("HEAD:") + Path}).Text();
        const int32 Start = Pointer.Find(TEXT("oid sha256:"));
        return Start == INDEX_NONE ? FString() : Pointer.Mid(Start + 11, 64);
    }
    FString Object(const FString& Dir, const FString& Oid)
    { return FPaths::Combine(Dir, TEXT(".git/lfs/objects"), Oid.Left(2), Oid.Mid(2, 2), Oid); }
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
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitIncomingTreeTest, "GitWorkspace.Remote.IncomingTreeReview", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGitIncomingTreeTest::RunTest(const FString&)
{
    FRemoteFixture F; GitWorkspace::FRepository Repo(F.Git, F.A);
    const FString Before = Repo.Refresh().Head;
    const FString OddPath = TEXT("Docs/a [bracket]\nline.md");
    F.Write(F.B, OddPath, TEXT("incoming document\n"));
    F.Write(F.B, TEXT("Docs/executable.md"), TEXT("executable requires review\n"));
    F.Write(F.B, TEXT("Content/Probe.uasset"), TEXT("package fixture\n"));
    F.Write(F.B, TEXT("Content/Map.umap"), TEXT("map fixture\n"));
    F.Write(F.B, TEXT("Config/DefaultGame.ini"), TEXT("[fixture]\n"));
    F.Write(F.B, TEXT("Source/Probe.cpp"), TEXT("// fixture\n"));
    TestTrue(TEXT("Rename fixture document"), F.At(F.B, {TEXT("mv"), TEXT("README.md"), TEXT("Docs/renamed.md")}).Ok());
    F.At(F.B, {TEXT("add"), TEXT("-A")});
    TestTrue(TEXT("Stage executable tree mode"), F.At(F.B, {TEXT("update-index"), TEXT("--chmod=+x"), TEXT("--"), TEXT("Docs/executable.md")}).Ok());
    TestTrue(TEXT("Stage disposable gitlink"), F.At(F.B, {TEXT("update-index"), TEXT("--add"), TEXT("--cacheinfo"), TEXT("160000"), Before, TEXT("Plugins/Fixture")}).Ok());
    TestTrue(TEXT("Commit incoming fixture tree"), F.At(F.B, {TEXT("commit"), TEXT("-qm"), TEXT("incoming mixed tree")}).Ok());
    TestTrue(TEXT("Publish fixture tree"), F.At(F.B, {TEXT("push"), TEXT("origin"), TEXT("main")}).Ok());
    const auto Review = Repo.Fetch();
    if (!TestTrue(TEXT("Fetch classified incoming tree: ") + Review.Error, Review.IsFresh())) return false;
    TestEqual(TEXT("All incoming paths retained"), Review.IncomingChanges.Num(), 9);
    auto Check = [&](const FString& Path, GitWorkspace::EPullPathKind Kind, TCHAR Status)
    {
        const auto* Change = Review.IncomingChanges.FindByPredicate([&](const auto& C) { return C.Path == Path; });
        TestTrue(Path + TEXT(" classified with exact literal path/status"), Change && Change->Kind == Kind && Change->Status == Status);
    };
    Check(OddPath, GitWorkspace::EPullPathKind::Documentation, 'A');
    Check(TEXT("README.md"), GitWorkspace::EPullPathKind::Documentation, 'D');
    Check(TEXT("Docs/renamed.md"), GitWorkspace::EPullPathKind::Documentation, 'A');
    Check(TEXT("Docs/executable.md"), GitWorkspace::EPullPathKind::Unsupported, 'A');
    Check(TEXT("Plugins/Fixture"), GitWorkspace::EPullPathKind::Unsupported, 'A');
    Check(TEXT("Content/Probe.uasset"), GitWorkspace::EPullPathKind::Package, 'A');
    Check(TEXT("Content/Map.umap"), GitWorkspace::EPullPathKind::Package, 'A');
    Check(TEXT("Config/DefaultGame.ini"), GitWorkspace::EPullPathKind::RestartRequired, 'A');
    Check(TEXT("Source/Probe.cpp"), GitWorkspace::EPullPathKind::RestartRequired, 'A');
    TestFalse(TEXT("Mixed incoming tree is not integrated in editor"), Repo.Pull(Review).Ok());
    TestEqual(TEXT("Review preserves HEAD"), Repo.Refresh().Head, Before);
    TestEqual(TEXT("Review preserves README bytes"), F.Read(F.A, TEXT("README.md")), FString(TEXT("base\n")));
    auto Raw = F.At(F.A, {TEXT("diff"), TEXT("--raw"), TEXT("--no-abbrev"), TEXT("--no-renames"), TEXT("-z"), Before, Review.RemoteHead, TEXT("--")});
    TArray<GitWorkspace::FIncomingChange> Parsed; FString Error;
    TestTrue(TEXT("Actual raw diff parses"), GitWorkspace::ParseIncomingChanges(Raw.Out, Parsed, Error));
    const auto Complete = Raw.Out; Raw.Out.Append(Complete);
    TestFalse(TEXT("Duplicate paths rejected"), GitWorkspace::ParseIncomingChanges(Raw.Out, Parsed, Error));
    TestTrue(TEXT("Failed review exposes no partial paths"), Parsed.IsEmpty());
    Raw.Out = Complete; Raw.Out.Pop();
    TestFalse(TEXT("Truncated raw diff rejected"), GitWorkspace::ParseIncomingChanges(Raw.Out, Parsed, Error));
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitIncomingLfsPreservationTest, "GitWorkspace.Remote.IncomingLfsPreservesLocalWork", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGitIncomingLfsPreservationTest::RunTest(const FString&)
{
    FRemoteFixture F; GitWorkspace::FRepository Repo(F.Git, F.A);
    F.Write(F.B, TEXT(".gitattributes"), TEXT("*.uasset filter=lfs diff=lfs merge=lfs -text\n"));
    F.Write(F.B, TEXT("Content/Probe.uasset"), TEXT("previous object\n")); F.Commit(F.B);
    const FString OldOid = F.Oid(F.B, TEXT("Content/Probe.uasset"));
    const FString OddPath = TEXT("Art/Texture [one]\n雪.uasset");
    F.Write(F.B, TEXT("Content/Probe.uasset"), TEXT("incoming object\n"));
    F.Write(F.B, OddPath, TEXT("outside Content\n")); F.Commit(F.B);
    if (!TestTrue(TEXT("Publish fixture LFS objects"), F.At(F.B, {TEXT("lfs"), TEXT("push"), TEXT("origin"), TEXT("main")}).Ok()) ||
        !TestTrue(TEXT("Publish fixture commit"), F.At(F.B, {TEXT("push"), TEXT("origin"), TEXT("main")}).Ok())) return false;
    const FString Oid = F.Oid(F.B, TEXT("Content/Probe.uasset")), OutsideOid = F.Oid(F.B, OddPath);
    F.At(F.A, {TEXT("config"), TEXT("lfs.fetchinclude"), TEXT("Nothing/**")});
    F.At(F.A, {TEXT("config"), TEXT("lfs.fetchexclude"), TEXT("*")});
    F.At(F.A, {TEXT("config"), TEXT("lfs.fetchrecentalways"), TEXT("true")});
    F.At(F.A, {TEXT("config"), TEXT("lfs.fetchrecentcommitsdays"), TEXT("7")});
    F.Write(F.A, TEXT("README.md"), TEXT("staged local A\n")); Repo.Stage({TEXT("README.md")});
    F.Write(F.A, TEXT("README.md"), TEXT("working local B\n"));
    F.Write(F.A, TEXT("Content/Probe.uasset"), TEXT("untracked local asset\n"));
    F.Write(F.A, TEXT(".git/test-hooks/pre-push"), TEXT("user hook sentinel\n"));
    const auto Review = Repo.Fetch(); const auto Before = Repo.Refresh();
    TArray<uint8> IndexBefore, IndexAfter;
    FFileHelper::LoadFileToArray(IndexBefore, *FPaths::Combine(F.A, TEXT(".git/index")));
    const auto RefsBefore = F.At(F.A, {TEXT("show-ref")}).Out;
    const auto Result = Repo.PrepareIncomingLfs(Review);
    if (!TestTrue(TEXT("Incoming objects downloaded and verified: ") + Result.Error, Result.Matches(Review))) return false;
    TestEqual(TEXT("Content object cached"), F.Read(F.A, F.Object(F.A, Oid)), FString(TEXT("incoming object\n")));
    TestEqual(TEXT("Outside-content literal path object cached"), F.Read(F.A, F.Object(F.A, OutsideOid)), FString(TEXT("outside Content\n")));
    TestFalse(TEXT("Recent history not implicitly downloaded"), IFileManager::Get().FileExists(*F.Object(F.A, OldOid)));
    TestEqual(TEXT("HEAD preserved"), Repo.Refresh().Head, Before.Head);
    TestTrue(TEXT("Staged snapshot preserved"), Repo.Refresh().IndexEntries == Before.IndexEntries);
    FFileHelper::LoadFileToArray(IndexAfter, *FPaths::Combine(F.A, TEXT(".git/index")));
    TestTrue(TEXT("Index bytes preserved"), IndexBefore == IndexAfter);
    TestTrue(TEXT("Refs preserved"), F.At(F.A, {TEXT("show-ref")}).Out == RefsBefore);
    TestEqual(TEXT("Working edit preserved"), F.Read(F.A, TEXT("README.md")), FString(TEXT("working local B\n")));
    TestEqual(TEXT("Untracked collision preserved"), F.Read(F.A, TEXT("Content/Probe.uasset")), FString(TEXT("untracked local asset\n")));
    TestFalse(TEXT("Other incoming working file not created"), IFileManager::Get().FileExists(*FPaths::Combine(F.A, OddPath)));
    TestEqual(TEXT("Existing hook preserved"), F.Read(F.A, TEXT(".git/test-hooks/pre-push")), FString(TEXT("user hook sentinel\n")));
    TestFalse(TEXT("LFS checker did not install checkout hook"), IFileManager::Get().FileExists(*FPaths::Combine(F.A, TEXT(".git/test-hooks/post-checkout"))));
    TestEqual(TEXT("Fetch exclusions not persisted"), F.At(F.A, {TEXT("config"), TEXT("lfs.fetchexclude")}).Text().TrimEnd(), FString(TEXT("*")));
    auto Stale = Review; Stale.FetchedSeconds -= 301;
    TestFalse(TEXT("Verification not current after review expires"), Result.Matches(Stale));
    auto Changed = Review; Changed.RemoteHead = Before.Head;
    TestFalse(TEXT("Verification cannot certify another commit"), Result.Matches(Changed));
    TestFalse(TEXT("Verified downloads do not bypass asset Pull guard"), Repo.Pull(Review).Ok());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitIncomingLfsFailureTest, "GitWorkspace.Remote.IncomingLfsFailuresAndRecovery", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGitIncomingLfsFailureTest::RunTest(const FString&)
{
    FRemoteFixture F; GitWorkspace::FRepository Repo(F.Git, F.A);
    F.Write(F.B, TEXT(".gitattributes"), TEXT("*.uasset filter=lfs diff=lfs merge=lfs -text\n"));
    const FString Payload = TEXT("valid incoming payload\n");
    F.Write(F.B, TEXT("Probe.uasset"), Payload); F.Commit(F.B);
    // The clean filter may install hooks during fixture staging. Explicitly
    // bypass only this disposable publisher's hooks to leave the object missing.
    if (!TestTrue(TEXT("Publish fixture pointer without LFS hook"), F.At(F.B, {TEXT("-c"), TEXT("core.hooksPath=.git/missing-fixture-hooks"), TEXT("push"), TEXT("origin"), TEXT("main")}).Ok())) return false;
    const FString Oid = F.Oid(F.B, TEXT("Probe.uasset")); const auto Review = Repo.Fetch();
    const auto Before = Repo.Refresh();
    auto Result = Repo.PrepareIncomingLfs(Review);
    TestFalse(TEXT("Missing remote object cannot be verified"), Result.bVerified);
    TestTrue(TEXT("Download failure visible"), Result.Error.Contains(TEXT("download did not complete")));
    TestEqual(TEXT("Failed download preserves HEAD"), Repo.Refresh().Head, Before.Head);
    TestFalse(TEXT("Failed download creates no working asset"), IFileManager::Get().FileExists(*FPaths::Combine(F.A, TEXT("Probe.uasset"))));
    if (!TestTrue(TEXT("Repair fixture remote object"), F.At(F.B, {TEXT("lfs"), TEXT("push"), TEXT("--all"), TEXT("origin"), TEXT("main")}).Ok())) return false;
    Result = Repo.PrepareIncomingLfs(Review);
    if (!TestTrue(TEXT("Explicit retry recovers after remote repair: ") + Result.Error, Result.bVerified)) return false;
    const FString Object = F.Object(F.A, Oid), Corrupt = FString::ChrN(Payload.Len(), 'x');
    F.Write(F.A, Object, Corrupt);
    Result = Repo.PrepareIncomingLfs(Review);
    TestFalse(TEXT("Same-size cached corruption fails hash verification"), Result.bVerified);
    TestTrue(TEXT("Integrity failure visible"), Result.Error.Contains(TEXT("integrity verification")));
    TestEqual(TEXT("Dry-run verification preserves corrupt object for review"), F.Read(F.A, Object), Corrupt);
    TestFalse(TEXT("No quarantine mutation"), IFileManager::Get().DirectoryExists(*FPaths::Combine(F.A, TEXT(".git/lfs/bad"))));
    F.Write(F.A, Object, Payload);
    TestTrue(TEXT("Explicit retry recovers after cache repair"), Repo.PrepareIncomingLfs(Review).bVerified);
    // Insert a raw blob without the clean filter, simulating a malformed publisher.
    F.Write(F.B, TEXT(".git/not-a-pointer"), TEXT("broken LFS pointer fixture\n"));
    const auto Blob = F.At(F.B, {TEXT("hash-object"), TEXT("-w"), TEXT(".git/not-a-pointer")});
    TestTrue(TEXT("Stage malformed fixture blob"), F.At(F.B, {TEXT("update-index"), TEXT("--add"), TEXT("--cacheinfo"), TEXT("100644"), Blob.Text().TrimEnd(), TEXT("Broken.uasset")}).Ok());
    TestTrue(TEXT("Commit malformed fixture pointer"), F.At(F.B, {TEXT("commit"), TEXT("-qm"), TEXT("broken pointer fixture")}).Ok());
    TestTrue(TEXT("Publish malformed fixture pointer"), F.At(F.B, {TEXT("push"), TEXT("origin"), TEXT("main")}).Ok());
    Result = Repo.PrepareIncomingLfs(Repo.Fetch());
    TestFalse(TEXT("Raw blob under LFS attributes is not accepted"), Result.bVerified);
    TestTrue(TEXT("Pointer error names the problem"), Result.Error.Contains(TEXT("pointers failed verification")) && Result.Error.Contains(TEXT("Broken.uasset")));
    TestEqual(TEXT("All failed checks preserve HEAD"), Repo.Refresh().Head, Before.Head);
    TestTrue(TEXT("All failed checks preserve index"), Repo.Refresh().IndexEntries == Before.IndexEntries);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitIncomingLfsGuardsTest, "GitWorkspace.Remote.IncomingLfsReviewGuards", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGitIncomingLfsGuardsTest::RunTest(const FString&)
{
    FRemoteFixture F; GitWorkspace::FRepository Repo(F.Git, F.A);
    TestFalse(TEXT("No incoming commit is not a download review"), Repo.PrepareIncomingLfs(Repo.Fetch()).bVerified);
    F.Write(F.B, TEXT("README.md"), TEXT("incoming\n")); F.Commit(F.B); F.At(F.B, {TEXT("push"), TEXT("origin"), TEXT("main")});
    auto Review = Repo.Fetch(), Stale = Review; Stale.FetchedSeconds -= 301;
    TestFalse(TEXT("Expired review rejected before download"), Repo.PrepareIncomingLfs(Stale).bVerified);
    F.At(F.A, {TEXT("config"), TEXT("uegit.fixture"), TEXT("changed")});
    TestTrue(TEXT("Changed configuration invalidates review"), Repo.PrepareIncomingLfs(Review).Error.Contains(TEXT("configuration changed")));
    Review = Repo.Fetch();
    F.Write(F.B, TEXT("README.md"), TEXT("remote moved\n")); F.Commit(F.B); F.At(F.B, {TEXT("push"), TEXT("origin"), TEXT("main")});
    TestTrue(TEXT("Changed remote invalidates review"), Repo.PrepareIncomingLfs(Review).Error.Contains(TEXT("Remote branch changed")));
    Review = Repo.Fetch(); F.Write(F.A, TEXT(".lfsconfig"), TEXT("[lfs]\nfetchinclude = Content/**\n"));
    TestTrue(TEXT("Untracked LFS config rejected"), Repo.PrepareIncomingLfs(Review).Error.Contains(TEXT("Commit or restore .lfsconfig")));
    IFileManager::Get().Delete(*FPaths::Combine(F.A, TEXT(".lfsconfig")));
    F.Write(F.B, TEXT(".lfsconfig"), TEXT("[lfs]\nfetchinclude = Content/**\n")); F.Commit(F.B); F.At(F.B, {TEXT("push"), TEXT("origin"), TEXT("main")});
    TestTrue(TEXT("Incoming endpoint config requires explicit external review"), Repo.PrepareIncomingLfs(Repo.Fetch()).Error.Contains(TEXT("Incoming .lfsconfig changes")));
    return true;
}

#if PLATFORM_MAC
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitRestartLeaseTest, "GitWorkspace.Restart.CheckoutLease", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGitRestartLeaseTest::RunTest(const FString&)
{
    FRemoteFixture F; FString Error;
    GitWorkspaceSession::FLease Editor, OtherEditor, Helper;
    TestTrue(TEXT("Editor holds shared checkout lease"), Editor.Acquire(F.A, false, Error));
    TestTrue(TEXT("A second editor can share an idle checkout"), OtherEditor.Acquire(F.A, false, Error));
    TestFalse(TEXT("Pull cannot acquire a checkout while editors hold it"), Helper.Acquire(F.A, true, Error));
    TestFalse(TEXT("Promotion refused while another editor is open"), Editor.Promote(Error));
    OtherEditor.Release();
    TestFalse(TEXT("Failed promotion retained the original shared lease"), Helper.Acquire(F.A, true, Error));
    TestTrue(TEXT("Sole editor can promote without losing coordination"), Editor.Promote(Error));
    TestTrue(TEXT("Promoted editor owns exclusive checkout"), Editor.IsExclusiveFor(F.A));
    TestFalse(TEXT("Promoted editor blocks another startup"), OtherEditor.Acquire(F.A, false, Error));
    Editor.Demote();
    TestTrue(TEXT("Shared access restored after in-editor Pull"), OtherEditor.Acquire(F.A, false, Error));
    Editor.Release();
    TestFalse(TEXT("Closing only one editor is insufficient"), Helper.Acquire(F.A, true, Error));
    OtherEditor.Release();
    TestTrue(TEXT("Closed checkout can be acquired exclusively"), Helper.Acquire(F.A, true, Error));
    TestTrue(TEXT("Canonical checkout identity matches"), Helper.IsExclusiveFor(F.A));
    TestFalse(TEXT("A lease never authorizes a different checkout"), Helper.IsExclusiveFor(F.B));
    TestFalse(TEXT("New editor blocked while integration owns checkout"), Editor.Acquire(F.A, false, Error));
    Helper.Release(); TestTrue(TEXT("Editor can reopen after integration releases checkout"), Editor.Acquire(F.A, false, Error));
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitRestartHydrationTest, "GitWorkspace.Restart.VerifiedHydration", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGitRestartHydrationTest::RunTest(const FString&)
{
    FRemoteFixture F;
    F.Write(F.A, TEXT(".gitattributes"), TEXT("*.uasset filter=lfs diff=lfs merge=lfs -text\n")); F.Commit(F.A);
    TestTrue(TEXT("Publish base attributes"), F.At(F.A, {TEXT("push"), TEXT("origin"), TEXT("main")}).Ok());
    TestTrue(TEXT("Publisher receives attributes"), F.At(F.B, {TEXT("pull"), TEXT("--ff-only")}).Ok());
    const FString Payload = TEXT("hydrated incoming bytes, not an LFS pointer\n");
    F.Write(F.B, TEXT("Content/Probe.uasset"), Payload); F.Commit(F.B);
    TestTrue(TEXT("Upload incoming objects"), F.At(F.B, {TEXT("lfs"), TEXT("push"), TEXT("origin"), TEXT("main")}).Ok());
    TestTrue(TEXT("Publish incoming commit"), F.At(F.B, {TEXT("push"), TEXT("origin"), TEXT("main")}).Ok());
    F.At(F.A, {TEXT("config"), TEXT("filter.lfs.process"), TEXT("git-lfs filter-process --skip")});
    F.Write(F.A, TEXT(".git/uegit/locks/fixture.json"), TEXT("retained lock receipt\n"));
    GitWorkspace::FRepository Repo(F.Git, F.A); const auto Review = Repo.Fetch();
    GitWorkspaceSession::FLease Lease; FString Error;
    TestTrue(TEXT("Acquire exclusive checkout"), Lease.Acquire(F.A, true, Error));
    const auto Result = Repo.PullAfterEditorExit(Review, Lease);
    if (!TestTrue(TEXT("Closed-editor Pull succeeds: ") + Result.Error, Result.Ok())) return false;
    TestEqual(TEXT("Reviewed commit integrated exactly"), Repo.Refresh().Head, Review.RemoteHead);
    TestEqual(TEXT("Actual bytes hydrated despite skip-smudge"), F.Read(F.A, TEXT("Content/Probe.uasset")), Payload);
    TestTrue(TEXT("Index and working tree clean"), Repo.Refresh().Files.IsEmpty());
    TestEqual(TEXT("Lock receipt retained"), F.Read(F.A, TEXT(".git/uegit/locks/fixture.json")), FString(TEXT("retained lock receipt\n")));
    TestFalse(TEXT("Successful verification clears recovery marker"), IFileManager::Get().FileExists(*FPaths::Combine(F.A, TEXT(".git/uegit/restart-pull/recovery-required.txt"))));
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitRestartGuardsTest, "GitWorkspace.Restart.PreflightGuards", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGitRestartGuardsTest::RunTest(const FString&)
{
    FRemoteFixture F; GitWorkspace::FRepository Repo(F.Git, F.A); const FString Original = Repo.Refresh().Head;
    F.Write(F.B, TEXT("Content/Probe.uasset"), TEXT("incoming package\n")); F.Commit(F.B); F.At(F.B, {TEXT("push"), TEXT("origin"), TEXT("main")});
    auto Review = Repo.Fetch(); GitWorkspaceSession::FLease Lease; FString Error;
    TestFalse(TEXT("No lease means no integration"), Repo.PullAfterEditorExit(Review, Lease).Ok());
    Lease.Acquire(F.B, true, Error); TestFalse(TEXT("Other checkout lease rejected"), Repo.PullAfterEditorExit(Review, Lease).Ok());
    Lease.Acquire(F.A, true, Error);
    auto Stale = Review; Stale.FetchedSeconds -= 301; TestFalse(TEXT("Expired review rejected"), Repo.PullAfterEditorExit(Stale, Lease).Ok());
    F.Write(F.A, TEXT("local.txt"), TEXT("keep me\n")); TestFalse(TEXT("Untracked work blocks closed-editor Pull"), Repo.PullAfterEditorExit(Review, Lease).Ok());
    IFileManager::Get().Delete(*FPaths::Combine(F.A, TEXT("local.txt")));
    F.Write(F.A, TEXT(".git/info/exclude"), TEXT("Content/Probe.uasset\n")); Review = Repo.Fetch();
    F.Write(F.A, TEXT("Content/Probe.uasset"), TEXT("ignored artist work\n"));
    TestTrue(TEXT("Collision is ignored by ordinary status"), Repo.Refresh().Files.IsEmpty());
    TestFalse(TEXT("Ignored file is protected"), Repo.PullAfterEditorExit(Review, Lease).Ok());
    TestEqual(TEXT("Ignored bytes preserved"), F.Read(F.A, TEXT("Content/Probe.uasset")), FString(TEXT("ignored artist work\n")));
    IFileManager::Get().Delete(*FPaths::Combine(F.A, TEXT("Content/Probe.uasset")));
    F.Write(F.B, TEXT("Source/Probe.cpp"), TEXT("// rebuild required\n")); F.Commit(F.B); F.At(F.B, {TEXT("push"), TEXT("origin"), TEXT("main")});
    TestFalse(TEXT("Remote advance invalidates old review"), Repo.PullAfterEditorExit(Review, Lease).Ok());
    auto Forged = Repo.Fetch(); Forged.IncomingChanges.Empty();
    TestFalse(TEXT("Backend recomputes unsupported code paths"), Repo.PullAfterEditorExit(Forged, Lease).Ok());
    TestEqual(TEXT("All preflight failures preserve HEAD"), Repo.Refresh().Head, Original);
    TestFalse(TEXT("Preflight failures need no recovery marker"), IFileManager::Get().FileExists(*FPaths::Combine(F.A, TEXT(".git/uegit/restart-pull/recovery-required.txt"))));
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitReloadTransactionTest, "GitWorkspace.Remote.ReloadTransactionRecovery", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGitReloadTransactionTest::RunTest(const FString&)
{
    FRemoteFixture F; GitWorkspace::FRepository Repo(F.Git, F.A);
    F.Write(F.B, TEXT("Content/Probe.uasset"), TEXT("backend-only fixture payload\n")); F.Commit(F.B); F.At(F.B, {TEXT("push"), TEXT("origin"), TEXT("main")});
    const auto Review = Repo.Fetch(); const auto Prepared = Repo.PrepareIncomingLfs(Review);
    if (!TestTrue(TEXT("Prepare reload transaction: ") + Prepared.Error, Prepared.bVerified)) return false;
    GitWorkspaceSession::FLease Lease; FString Error; Lease.Acquire(F.A, true, Error);
    auto Forged = Review; Forged.IncomingChanges.Empty();
    TestFalse(TEXT("Missing package review cannot skip linker preparation"), Repo.PullForReload(Forged, Prepared, Lease).Ok());
    TestEqual(TEXT("Rejected path list preserves HEAD"), Repo.Refresh().Head, Review.Head);
    F.Write(F.A, TEXT(".git/uegit/locks/receipt.json"), TEXT("keep lock ownership\n"));
    const auto Integrated = Repo.PullForReload(Review, Prepared, Lease);
    if (!TestTrue(TEXT("Integrate backend transaction: ") + Integrated.Error, Integrated.Ok())) return false;
    const FString Marker = TEXT(".git/uegit/restart-pull/recovery-required.txt");
    TestTrue(TEXT("Disk success alone does not clear recovery"), IFileManager::Get().FileExists(*FPaths::Combine(F.A, Marker)));
    F.Write(F.A, TEXT("README.md"), TEXT("unexpected reload-time change\n"));
    TestFalse(TEXT("Reload-time repository mutation prevents success"), Repo.CompleteReloadPull(Review, Lease).Ok());
    TestTrue(TEXT("Uncertain reload retains recovery"), IFileManager::Get().FileExists(*FPaths::Combine(F.A, Marker)));
    TestEqual(TEXT("Unexpected work retained"), F.Read(F.A, TEXT("README.md")), FString(TEXT("unexpected reload-time change\n")));
    F.Write(F.A, TEXT("README.md"), TEXT("base\n"));
    TestTrue(TEXT("Confirmed completion clears marker"), Repo.CompleteReloadPull(Review, Lease).Ok());
    TestFalse(TEXT("Recovery cleared after confirmed completion"), IFileManager::Get().FileExists(*FPaths::Combine(F.A, Marker)));
    TestEqual(TEXT("Lock receipt unchanged"), F.Read(F.A, TEXT(".git/uegit/locks/receipt.json")), FString(TEXT("keep lock ownership\n")));
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitRestartRecoveryTest, "GitWorkspace.Restart.FailedPostflightRequiresRecovery", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGitRestartRecoveryTest::RunTest(const FString&)
{
    FRemoteFixture F; GitWorkspace::FRepository Repo(F.Git, F.A);
    F.Write(F.A, TEXT(".git/test-hooks/post-merge"), TEXT("#!/bin/sh\nprintf 'hook-created local work\\n' > README.md\n"));
    chmod(TCHAR_TO_UTF8(*FPaths::Combine(F.A, TEXT(".git/test-hooks/post-merge"))), 0755);
    F.Write(F.B, TEXT("Content/Probe.uasset"), TEXT("incoming package\n")); F.Commit(F.B); F.At(F.B, {TEXT("push"), TEXT("origin"), TEXT("main")});
    const auto Review = Repo.Fetch(); GitWorkspaceSession::FLease Lease; FString Error; Lease.Acquire(F.A, true, Error);
    const auto Result = Repo.PullAfterEditorExit(Review, Lease);
    TestFalse(TEXT("Hook-created work fails postflight: ") + Result.Error, Result.Ok());
    TestEqual(TEXT("No destructive rollback of an integrated commit"), Repo.Refresh().Head, Review.RemoteHead);
    TestEqual(TEXT("Unexpected local work preserved for recovery"), F.Read(F.A, TEXT("README.md")), FString(TEXT("hook-created local work\n")));
    TestTrue(TEXT("Recovery marker retained"), IFileManager::Get().FileExists(*FPaths::Combine(F.A, TEXT(".git/uegit/restart-pull/recovery-required.txt"))));
    const FString Marker = F.Read(F.A, TEXT(".git/uegit/restart-pull/recovery-required.txt"));
    TestTrue(TEXT("Recovery records both commit identities"), Marker.Contains(Review.Head) && Marker.Contains(Review.RemoteHead));
    TestFalse(TEXT("Another attempt cannot bypass unresolved recovery"), Repo.PullAfterEditorExit(Review, Lease).Ok());
    return true;
}
#endif
#endif
