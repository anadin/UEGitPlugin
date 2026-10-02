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
struct FFixture
{
    FString Root = FPaths::Combine(FPlatformProcess::UserTempDir(), TEXT("uegit-tests-") + FGuid::NewGuid().ToString(EGuidFormats::Digits));
    FString Git = GitWorkspace::FindGitExecutable();
    FFixture()
    {
        IFileManager::Get().MakeDirectory(*Root, true);
        Call({TEXT("init"), TEXT("-q")});
        Call({TEXT("config"), TEXT("user.name"), TEXT("UEGit fixture")});
        Call({TEXT("config"), TEXT("user.email"), TEXT("fixture@example.invalid")});
        Call({TEXT("config"), TEXT("commit.gpgsign"), TEXT("false")});
        Call({TEXT("config"), TEXT("core.hooksPath"), TEXT(".git/test-hooks")});
        Call({TEXT("config"), TEXT("core.autocrlf"), TEXT("false")});
    }
    ~FFixture() { IFileManager::Get().DeleteDirectory(*Root, false, true); }
    GitWorkspace::FResult Call(const TArray<FString>& Args) { return GitWorkspace::Run(Git, Root, Args); }
    bool Write(const FString& Path, const FString& Text)
    {
        const FString Full = FPaths::Combine(Root, Path);
        IFileManager::Get().MakeDirectory(*FPaths::GetPath(Full), true);
        return FFileHelper::SaveStringToFile(Text, *Full, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
    }
    void Base()
    {
        Write(TEXT("asset.txt"), TEXT("base\n"));
        Call({TEXT("add"), TEXT("asset.txt")}); Call({TEXT("commit"), TEXT("-qm"), TEXT("base")});
    }
};
TArray<uint8> StatusBytes(const TArray<FString>& Records)
{
    TArray<uint8> Bytes;
    for (const FString& R : Records) { FTCHARToUTF8 U(*R); Bytes.Append(reinterpret_cast<const uint8*>(U.Get()), U.Length()); Bytes.Add(0); }
    return Bytes;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitWorkspaceParserTest, "GitWorkspace.Status.StrictPorcelain", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGitWorkspaceParserTest::RunTest(const FString&)
{
    GitWorkspace::FSnapshot S; FString Error;
    const TArray<uint8> Bytes = StatusBytes({TEXT("# branch.oid 123"), TEXT("# branch.head feature/test"),
        TEXT("1 MM N... 100644 100644 100644 aaa bbb Content/my asset.txt"),
        TEXT("2 R. N... 100644 100644 100644 aaa bbb R100 renamed.txt"), TEXT("original with space.txt"),
        TEXT("? untracked\nfile.txt"), TEXT("u UU N... 100644 100644 100644 100644 aaa bbb ccc conflict.txt")});
    TestTrue(TEXT("Parse valid mixed records"), GitWorkspace::ParseStatus(Bytes, S, Error));
    TestEqual(TEXT("Four independent files"), S.Files.Num(), 4);
    if (S.Files.Num() == 4)
    {
        TestTrue(TEXT("MM staged"), S.Files[0].HasStaged()); TestTrue(TEXT("MM working"), S.Files[0].HasUnstaged());
        TestEqual(TEXT("Rename source retained"), S.Files[1].OriginalPath, FString(TEXT("original with space.txt")));
        TestTrue(TEXT("Untracked is not staged"), !S.Files[2].HasStaged());
        TestTrue(TEXT("Conflict retained"), S.HasConflicts());
    }
    auto Broken = Bytes; Broken.Pop();
    TestFalse(TEXT("Truncated stream rejected"), GitWorkspace::ParseStatus(Broken, S, Error));
    TestFalse(TEXT("Missing rename source rejected"), GitWorkspace::ParseStatus(StatusBytes({TEXT("2 R. N... 100644 100644 100644 aaa bbb R100 renamed.txt")}), S, Error));
    TestFalse(TEXT("Unknown record rejected"), GitWorkspace::ParseStatus(StatusBytes({TEXT("nonsense")}), S, Error));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitWorkspaceIndexTest, "GitWorkspace.Repository.PreserveStagedSnapshot", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGitWorkspaceIndexTest::RunTest(const FString&)
{
    FFixture F; F.Base(); GitWorkspace::FRepository Repo(F.Git, F.Root);
    F.Write(TEXT("asset.txt"), TEXT("version A\n"));
    TestTrue(TEXT("Stage A"), Repo.Stage({TEXT("asset.txt")}).Ok());
    F.Write(TEXT("asset.txt"), TEXT("version B\n"));
    const auto Reviewed = Repo.Refresh();
    TestTrue(TEXT("Snapshot valid"), Reviewed.bValid);
    TestEqual(TEXT("One staged entry"), Reviewed.StagedCount(), 1);
    const auto Before = F.Call({TEXT("show"), TEXT(":asset.txt")}).Text();
    Repo.Refresh(); Repo.Refresh();
    TestEqual(TEXT("Refresh does not restage B"), F.Call({TEXT("show"), TEXT(":asset.txt")}).Text(), Before);
    TestTrue(TEXT("Staged diff contains A"), Repo.Diff(TEXT("asset.txt"), true).Text().Contains(TEXT("+version A")));
    TestTrue(TEXT("Working diff contains B"), Repo.Diff(TEXT("asset.txt"), false).Text().Contains(TEXT("+version B")));
    const auto Commit = Repo.Commit(Reviewed, TEXT("Commit the reviewed A"));
    TestTrue(TEXT("Commit succeeds: ") + Commit.Error, Commit.Ok());
    TestEqual(TEXT("Commit contains A"), F.Call({TEXT("show"), TEXT("HEAD:asset.txt")}).Text(), FString(TEXT("version A\n")));
    FString Working; FFileHelper::LoadFileToString(Working, *FPaths::Combine(F.Root, TEXT("asset.txt")));
    TestEqual(TEXT("B remains on disk"), Working, FString(TEXT("version B\n")));
    TestEqual(TEXT("Only one commit created"), F.Call({TEXT("rev-list"), TEXT("--count"), TEXT("HEAD")}).Text().TrimEnd(), FString(TEXT("2")));
    TestEqual(TEXT("Index clean after commit"), Repo.Refresh().StagedCount(), 0);
    TestTrue(TEXT("Stage B"), Repo.Stage({TEXT("asset.txt")}).Ok());
    auto Stale = Repo.Refresh(); F.Write(TEXT("asset.txt"), TEXT("version C\n")); F.Call({TEXT("add"), TEXT("asset.txt")});
    TestFalse(TEXT("Stale reviewed index rejected"), Repo.Commit(Stale, TEXT("must not commit C")).Ok());
    TestTrue(TEXT("Unstage"), Repo.Unstage({TEXT("asset.txt")}).Ok());
    FFileHelper::LoadFileToString(Working, *FPaths::Combine(F.Root, TEXT("asset.txt")));
    TestEqual(TEXT("Unstage preserves C"), Working, FString(TEXT("version C\n")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitWorkspacePathsTest, "GitWorkspace.Repository.UnbornAndLiteralPaths", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGitWorkspacePathsTest::RunTest(const FString&)
{
    FFixture F; GitWorkspace::FRepository Repo(F.Git, F.Root);
    TArray<FString> Names {TEXT("space name.txt"), TEXT("unicode-水.txt"), TEXT("[abc].txt"), TEXT("a.txt"), TEXT("-leading.txt")};
#if PLATFORM_MAC
    Names.Append({TEXT("a\"b.txt"), TEXT("two  spaces.txt"), TEXT("line\nbreak.txt")});
#endif
    for (const FString& Name : Names) F.Write(Name, TEXT("original\n"));
    TestTrue(TEXT("Literal bracket stage"), Repo.Stage({TEXT("[abc].txt")}).Ok());
    TestEqual(TEXT("Wildcard neighbor not staged"), Repo.Refresh().StagedCount(), 1);
    F.Write(TEXT("[abc].txt"), TEXT("later edits\n"));
    TestTrue(TEXT("Unstage before first commit"), Repo.Unstage({TEXT("[abc].txt")}).Ok());
    TestEqual(TEXT("Empty index after unborn unstage"), Repo.Refresh().StagedCount(), 0);
    FString Content; FFileHelper::LoadFileToString(Content, *FPaths::Combine(F.Root, TEXT("[abc].txt")));
    TestEqual(TEXT("Unborn unstage preserves working edits"), Content, FString(TEXT("later edits\n")));
    for (int32 I = 0; I < 65; ++I) { const FString P = FString::Printf(TEXT("many/file-%d.txt"), I); Names.Add(P); F.Write(P, TEXT("many\n")); }
    const auto Staged = Repo.Stage(Names); TestTrue(TEXT("Bulk paths staged: ") + Staged.Error, Staged.Ok());
    const auto S = Repo.Refresh(); TestTrue(TEXT("Bulk status valid: ") + S.Error, S.bValid);
    TestEqual(TEXT("All paths staged"), S.StagedCount(), Names.Num());
    const auto C = Repo.Commit(S, TEXT("One initial commit")); TestTrue(TEXT("Initial commit: ") + C.Error, C.Ok());
    TestEqual(TEXT("Bulk stage makes one commit"), F.Call({TEXT("rev-list"), TEXT("--count"), TEXT("HEAD")}).Text().TrimEnd(), FString(TEXT("1")));
    TestTrue(TEXT("Repository clean"), Repo.Refresh().Files.IsEmpty());
    F.Call({TEXT("mv"), TEXT("space name.txt"), TEXT("renamed name.txt")});
    auto Renamed = Repo.Refresh();
    TestTrue(TEXT("Rename preserved"), Renamed.Files.Num() == 1 && Renamed.Files[0].OriginalPath == TEXT("space name.txt"));
    TestTrue(TEXT("Unstage both sides of rename"), Repo.Unstage({TEXT("renamed name.txt")}).Ok());
    TestEqual(TEXT("Rename unstage removes index delta"), Repo.Refresh().StagedCount(), 0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitWorkspaceHookTest, "GitWorkspace.Repository.HookFailureAndAttributes", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGitWorkspaceHookTest::RunTest(const FString&)
{
    FFixture F; F.Base(); GitWorkspace::FRepository Repo(F.Git, F.Root);
    F.Write(TEXT(".gitattributes"), TEXT("*.bin lockable\n"));
    F.Write(TEXT("locked.bin"), TEXT("test\n"));
    auto S = Repo.Refresh();
    const auto* Binary = S.Files.FindByPredicate([](const auto& File) { return File.Path == TEXT("locked.bin"); });
    TestTrue(TEXT("Effective lockable attribute"), Binary && Binary->bLockable);
    F.Write(TEXT("asset.txt"), TEXT("hook candidate\n")); Repo.Stage({TEXT("asset.txt")});
    F.Write(TEXT(".git/test-hooks/pre-commit"), TEXT("#!/bin/sh\necho fixture-hook-rejected >&2\nexit 1\n"));
#if PLATFORM_MAC
    chmod(TCHAR_TO_UTF8(*FPaths::Combine(F.Root, TEXT(".git/test-hooks/pre-commit"))), 0755);
#endif
    const FString Before = F.Call({TEXT("rev-parse"), TEXT("HEAD")}).Text();
    const auto Failed = Repo.Commit(Repo.Refresh(), TEXT("rejected"));
    TestFalse(TEXT("Hook failure is not success"), Failed.Ok());
    TestTrue(TEXT("Hook error visible"), Failed.Error.Contains(TEXT("fixture-hook-rejected")));
    TestEqual(TEXT("HEAD unchanged"), F.Call({TEXT("rev-parse"), TEXT("HEAD")}).Text(), Before);
    TestEqual(TEXT("Index retained"), Repo.Refresh().StagedCount(), 1);
    F.Write(TEXT(".git/MERGE_HEAD"), Before);
    TestTrue(TEXT("Merge detected"), Repo.Refresh().bOperationInProgress);
    TestFalse(TEXT("In-progress merge not committed by normal workflow"), Repo.Commit(Repo.Refresh(), TEXT("unsafe")).Ok());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitWorkspaceLfsTest, "GitWorkspace.Repository.LfsSnapshotAndDeletion", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGitWorkspaceLfsTest::RunTest(const FString&)
{
    FFixture F; F.Base(); GitWorkspace::FRepository Repo(F.Git, F.Root);
    const auto Install = F.Call({TEXT("lfs"), TEXT("install"), TEXT("--local"), TEXT("--skip-repo")});
    if (!TestTrue(TEXT("LFS is available: ") + Install.Error, Install.Ok())) return false;
    F.Write(TEXT(".gitattributes"), TEXT("*.uasset filter=lfs diff=lfs merge=lfs -text lockable\n"));
    F.Write(TEXT("Content/Probe.uasset"), TEXT("saved package A\n"));
    const auto Stage = Repo.Stage({TEXT(".gitattributes"), TEXT("Content/Probe.uasset")});
    if (!TestTrue(TEXT("LFS staging: ") + Stage.Error, Stage.Ok())) return false;
    const auto Pointer = F.Call({TEXT("show"), TEXT(":Content/Probe.uasset")}).Text();
    TestTrue(TEXT("Index contains LFS pointer"), Pointer.StartsWith(TEXT("version https://git-lfs.github.com/spec/v1")));
    F.Write(TEXT("Content/Probe.uasset"), TEXT("later package B\n"));
    const auto S = Repo.Refresh();
    const auto* Asset = S.Files.FindByPredicate([](const auto& File) { return File.Path == TEXT("Content/Probe.uasset"); });
    TestTrue(TEXT("LFS asset keeps two deltas"), Asset && Asset->bLfs && Asset->bLockable && Asset->HasStaged() && Asset->HasUnstaged());
    TestTrue(TEXT("Commit staged pointer"), Repo.Commit(S, TEXT("LFS A")).Ok());
    TestEqual(TEXT("Original pointer committed"), F.Call({TEXT("show"), TEXT("HEAD:Content/Probe.uasset")}).Text(), Pointer);
    FString Working; FFileHelper::LoadFileToString(Working, *FPaths::Combine(F.Root, TEXT("Content/Probe.uasset")));
    TestEqual(TEXT("Later asset bytes preserved"), Working, FString(TEXT("later package B\n")));
    IFileManager::Get().Delete(*FPaths::Combine(F.Root, TEXT("asset.txt")));
    TestTrue(TEXT("Stage deletion"), Repo.Stage({TEXT("asset.txt")}).Ok());
    TestTrue(TEXT("Unstage deletion"), Repo.Unstage({TEXT("asset.txt")}).Ok());
    TestFalse(TEXT("Unstage does not restore deleted working file"), IFileManager::Get().FileExists(*FPaths::Combine(F.Root, TEXT("asset.txt"))));
    return true;
}

#if PLATFORM_MAC
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitWorkspaceProcessTest, "GitWorkspace.Process.StreamsAndTimeout", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGitWorkspaceProcessTest::RunTest(const FString&)
{
    const auto R = GitWorkspace::Run(TEXT("/bin/sh"), FPlatformProcess::UserTempDir(), {TEXT("-c"), TEXT("printf output; printf diagnostic >&2")});
    TestTrue(TEXT("Successful subprocess"), R.Ok());
    TestEqual(TEXT("stdout stays separate"), R.Text(), FString(TEXT("output")));
    TestEqual(TEXT("stderr stays separate"), R.Error, FString(TEXT("diagnostic")));
    const auto Timed = GitWorkspace::Run(TEXT("/bin/sh"), FPlatformProcess::UserTempDir(), {TEXT("-c"), TEXT("sleep 5")}, 0.05);
    TestFalse(TEXT("Timeout cannot report success"), Timed.Ok());
    TestTrue(TEXT("Timeout reports uncertain outcome"), Timed.Error.Contains(TEXT("outcome may be partial")));
    return true;
}
#endif
#endif
