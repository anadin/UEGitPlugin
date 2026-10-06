// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#if WITH_DEV_AUTOMATION_TESTS
#include "SGitWorkspace.h"
#include "GitWorkspacePullReview.h"
#include "GitWorkspaceEditorPull.h"
#if PLATFORM_MAC
#include "GitWorkspaceSession.h"
#endif
#include "PackageTools.h"
#include "UObject/Linker.h"
#include "UObject/PackageReload.h"
#include "Misc/ScopeExit.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/PackageName.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformFileManager.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/StrongObjectPtr.h"
#include "Engine/Blueprint.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GameFramework/WorldSettings.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceConstant.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Input/SEditableTextBox.h"

namespace
{
struct FEditorAssetFixture
{
    FString Id = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    FString Root = FPaths::Combine(FPlatformProcess::UserTempDir(), TEXT("uegit-editor-") + Id);
    FString Mount = TEXT("/GitWorkspaceAcceptance") + Id + TEXT("/");
    FString Git = GitWorkspace::FindGitExecutable();
    TArray<TStrongObjectPtr<UObject>> Assets;
    TArray<FString> Paths;
    UWorld* World = nullptr;
    FEditorAssetFixture()
    {
        IFileManager::Get().MakeDirectory(*FPaths::Combine(Root, TEXT("Content")), true);
        FPackageName::RegisterMountPoint(Mount, FPaths::Combine(Root, TEXT("Content/")));
        Call({TEXT("init"), TEXT("-q")});
        Call({TEXT("config"), TEXT("user.name"), TEXT("UEGit editor fixture")});
        Call({TEXT("config"), TEXT("user.email"), TEXT("fixture@example.invalid")});
        Call({TEXT("config"), TEXT("commit.gpgsign"), TEXT("false")});
        Call({TEXT("config"), TEXT("core.hooksPath"), TEXT(".git/test-hooks")});
        Call({TEXT("lfs"), TEXT("install"), TEXT("--local"), TEXT("--skip-repo")});
        FFileHelper::SaveStringToFile(TEXT("*.uasset filter=lfs diff=lfs merge=lfs -text lockable\n*.umap filter=lfs diff=lfs merge=lfs -text lockable\n"), *FPaths::Combine(Root, TEXT(".gitattributes")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
    }
    ~FEditorAssetFixture()
    {
        // Remove fixture assets before deferred save validation queries the registry.
        // These packages are temporary and must not survive the unmounted fixture.
        for (auto& Asset : Assets)
        {
            FAssetRegistryModule::AssetDeleted(Asset.Get());
            Asset->GetOutermost()->SetDirtyFlag(false);
            Asset->GetOutermost()->SetFlags(RF_Transient);
            Asset->ClearFlags(RF_Public | RF_Standalone);
        }
        if (World) World->DestroyWorld(false);
        Assets.Empty();
        FPackageName::UnRegisterMountPoint(Mount, FPaths::Combine(Root, TEXT("Content/")));
        IFileManager::Get().DeleteDirectory(*Root, false, true);
    }
    GitWorkspace::FResult Call(const TArray<FString>& Args) { return GitWorkspace::Run(Git, Root, Args); }
    UPackage* Package(const TCHAR* Name) { return CreatePackage(*(Mount + Name)); }
    void Create()
    {
        Assets.Emplace(FKismetEditorUtilities::CreateBlueprint(AActor::StaticClass(), Package(TEXT("BP_Probe")), TEXT("BP_Probe"), BPTYPE_Normal));
        Assets.Emplace(NewObject<UMaterial>(Package(TEXT("M_Probe")), TEXT("M_Probe"), RF_Public | RF_Standalone));
        auto* Texture = NewObject<UTexture2D>(Package(TEXT("T_Probe")), TEXT("T_Probe"), RF_Public | RF_Standalone);
        const uint8 Pixels[16] = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255};
        Texture->Source.Init(2, 2, 1, 1, TSF_BGRA8, Pixels);
        Assets.Emplace(Texture);
        World = UWorld::CreateWorld(EWorldType::Inactive, false, TEXT("L_Probe"), Package(TEXT("L_Probe")), false);
        World->SetFlags(RF_Public | RF_Standalone);
        Assets.Emplace(World);
        Paths = {TEXT("Content/BP_Probe.uasset"), TEXT("Content/M_Probe.uasset"), TEXT("Content/T_Probe.uasset"), TEXT("Content/L_Probe.umap")};
    }
    void Edit(int32 Version)
    {
        CastChecked<UBlueprint>(Assets[0].Get())->BlueprintDescription = FString::Printf(TEXT("Fixture version %d"), Version);
        CastChecked<UMaterial>(Assets[1].Get())->TwoSided = Version % 2 != 0;
        CastChecked<UTexture2D>(Assets[2].Get())->SRGB = Version % 2 != 0;
        World->GetWorldSettings()->KillZ = -1000.f * (Version + 1);
        for (auto& Asset : Assets) Asset->MarkPackageDirty();
    }
    bool Save()
    {
        bool bOk = true;
        for (int32 I = 0; I < Assets.Num(); ++I)
        {
            FSavePackageArgs Args; Args.TopLevelFlags = RF_Public | RF_Standalone; Args.SaveFlags = SAVE_NoError;
            UObject* Asset = Assets[I].Get();
            bOk &= UPackage::SavePackage(Asset->GetOutermost(), Asset, *FPaths::Combine(Root, Paths[I]), Args);
        }
        UPackage::WaitForAsyncFileWrites();
        return bOk;
    }
    TArray<uint8> Bytes(const FString& Path)
    {
        TArray<uint8> B; FFileHelper::LoadFileToArray(B, *FPaths::Combine(Root, Path)); return B;
    }
};
}

#if PLATFORM_MAC
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitEditorReloadPullTest, "GitWorkspace.Editor.PullAndReloadRealAssets", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGitEditorReloadPullTest::RunTest(const FString&)
{
    FEditorAssetFixture F; F.Create(); F.Edit(0);
    auto* Instance = NewObject<UMaterialInstanceConstant>(F.Package(TEXT("MI_Probe")), TEXT("MI_Probe"), RF_Public | RF_Standalone);
    Instance->SetParentEditorOnly(CastChecked<UMaterial>(F.Assets[1].Get()));
    Instance->BasePropertyOverrides.bOverride_TwoSided = true;
    Instance->BasePropertyOverrides.TwoSided = false;
    F.Assets.Emplace(Instance); F.Paths.Add(TEXT("Content/MI_Probe.uasset"));
    if (!TestTrue(TEXT("Save reload baseline"), F.Save())) return false;
    F.Call({TEXT("add"), TEXT(".")}); F.Call({TEXT("commit"), TEXT("-qm"), TEXT("base")});
    const FString Base = F.Call({TEXT("rev-parse"), TEXT("HEAD")}).Text().TrimEnd();
    const FString Branch = F.Call({TEXT("branch"), TEXT("--show-current")}).Text().TrimEnd();
    F.Edit(1); Instance->BasePropertyOverrides.TwoSided = true;
    if (!TestTrue(TEXT("Save incoming versions"), F.Save())) return false;
    F.Call({TEXT("add"), TEXT("--"), F.Paths[0], F.Paths[1], F.Paths[2], F.Paths[4]});
    // Add one valid, unloaded asset so Content Browser discovery is exercised.
    UPackage* AddedPackage = F.Package(TEXT("M_Added"));
    UMaterial* Added = NewObject<UMaterial>(AddedPackage, TEXT("M_Added"), RF_Public | RF_Standalone);
    FSavePackageArgs SaveArgs; SaveArgs.TopLevelFlags = RF_Public | RF_Standalone; SaveArgs.SaveFlags = SAVE_NoError;
    TestTrue(TEXT("Save incoming new asset"), UPackage::SavePackage(AddedPackage, Added, *FPaths::Combine(F.Root, TEXT("Content/M_Added.uasset")), SaveArgs));
    UPackage::WaitForAsyncFileWrites();
    F.Call({TEXT("add"), TEXT("--"), TEXT("Content/M_Added.uasset")});
    F.Call({TEXT("commit"), TEXT("-qm"), TEXT("incoming assets")});
    TArray<TArray<uint8>> ExpectedBytes;
    for (int32 I = 0; I < 3; ++I) ExpectedBytes.Add(F.Bytes(F.Paths[I]));
    const FString RemotePath = FPaths::Combine(F.Root, TEXT(".git/fixture-remote.git"));
    F.Call({TEXT("init"), TEXT("--bare"), RemotePath});
    F.Call({TEXT("remote"), TEXT("add"), TEXT("origin"), RemotePath});
    if (!TestTrue(TEXT("Upload real LFS payloads"), F.Call({TEXT("lfs"), TEXT("push"), TEXT("origin"), Branch}).Ok())) return false;
    if (!TestTrue(TEXT("Publish incoming asset commit"), F.Call({TEXT("push"), TEXT("-u"), TEXT("origin"), Branch}).Ok())) return false;
    FText UnloadError; TestTrue(TEXT("Unload newly added asset"), UPackageTools::UnloadPackages({AddedPackage}, UnloadError));
    AddedPackage = nullptr; Added = nullptr;
    for (auto& Asset : F.Assets) ResetLoaders(Asset->GetPackage());
    F.Call({TEXT("reset"), TEXT("--hard"), Base}); // Only this disposable fixture.
    F.Edit(0); for (auto& Asset : F.Assets) Asset->GetPackage()->SetDirtyFlag(false);
    Instance->BasePropertyOverrides.TwoSided = false;
    F.Call({TEXT("config"), TEXT("filter.lfs.process"), TEXT("git-lfs filter-process --skip")});
    GitWorkspace::FRepository Repo(F.Git, F.Root);
    const auto Reviewed = Repo.Fetch();
    if (!TestTrue(TEXT("Fetch actual asset update: ") + Reviewed.Error, Reviewed.IsFresh())) return false;
    const auto Prepared = Repo.PrepareIncomingLfs(Reviewed);
    if (!TestTrue(TEXT("LFS prepared: ") + Prepared.Error, Prepared.bVerified)) return false;
    FString Error; GitWorkspaceSession::FLease Lease;
    if (!TestTrue(TEXT("Fixture write lease"), Lease.Acquire(F.Root, true, Error))) return false;
    F.Assets[0]->MarkPackageDirty();
    auto Result = GitWorkspace::PullAndReload(Repo, Reviewed, Prepared, Lease);
    TestFalse(TEXT("Unsaved work blocks after LFS preparation"), Result.bSuccess);
    TestFalse(TEXT("Preflight block needs no recovery"), Result.bRecoveryRequired);
    TestEqual(TEXT("Preflight leaves HEAD unchanged"), Repo.Refresh().Head, Base);
    F.Assets[0]->GetPackage()->SetDirtyFlag(false);
    auto Stale = Prepared; Stale.Commit = FString::ChrN(40, 'a');
    TestFalse(TEXT("Wrong cache preparation refused"), GitWorkspace::PullAndReload(Repo, Reviewed, Stale, Lease).bSuccess);
    const auto Delegate = FCoreUObjectDelegates::OnPackageReloaded.AddLambda([&](EPackageReloadPhase Phase, FPackageReloadedEvent* Event)
    {
        if (Phase == EPackageReloadPhase::OnPackageFixup && Event)
            for (auto& Asset : F.Assets)
                if (UObject* const* Replacement = Event->GetRepointedObjects().Find(Asset.Get())) Asset.Reset(*Replacement);
    });
    ON_SCOPE_EXIT { FCoreUObjectDelegates::OnPackageReloaded.Remove(Delegate); };
    TWeakObjectPtr<UObject> OldBlueprint = F.Assets[0].Get();
    Result = GitWorkspace::PullAndReload(Repo, Reviewed, Prepared, Lease);
    if (!TestTrue(TEXT("Pull and reload completed: ") + Result.Message, Result.bSuccess)) return false;
    TestFalse(TEXT("Recovery cleared only after reload"), Result.bRecoveryRequired);
    TestEqual(TEXT("Four loaded packages reloaded"), Result.Reloaded, 4);
    TestEqual(TEXT("Five packages refreshed"), Result.Refreshed, 5);
    TestFalse(TEXT("Old Blueprint was purged"), OldBlueprint.IsValid());
    TestEqual(TEXT("Blueprint memory now has incoming value"), CastChecked<UBlueprint>(F.Assets[0].Get())->BlueprintDescription, FString(TEXT("Fixture version 1")));
    TestTrue(TEXT("Material memory now has incoming value"), !!CastChecked<UMaterial>(F.Assets[1].Get())->TwoSided);
    TestTrue(TEXT("Texture memory now has incoming value"), !!CastChecked<UTexture2D>(F.Assets[2].Get())->SRGB);
    Instance = CastChecked<UMaterialInstanceConstant>(F.Assets[4].Get());
    TestTrue(TEXT("Material instance has incoming override"), !!Instance->BasePropertyOverrides.TwoSided);
    TestTrue(TEXT("Dependency reference points at the reloaded material"), Instance->Parent.Get() == F.Assets[1].Get());
    TestEqual(TEXT("Unchanged map remains loaded"), F.World->GetWorldSettings()->KillZ, -1000.f);
    TestEqual(TEXT("Exact reviewed commit integrated"), Repo.Refresh().Head, Reviewed.RemoteHead);
    TestTrue(TEXT("No generated working edits"), Repo.Refresh().Files.IsEmpty());
    for (int32 I = 0; I < 3; ++I) TestTrue(TEXT("Incoming LFS payload hydrated exactly"), F.Bytes(F.Paths[I]) == ExpectedBytes[I]);
    TestNull(TEXT("Added asset discovered without loading it"), FindPackage(nullptr, *(F.Mount + TEXT("M_Added"))));
    // Do not leave temporary registry entries after the mount is removed.
    if (UObject* Cleanup = LoadObject<UObject>(nullptr, *(F.Mount + TEXT("M_Added.M_Added")))) F.Assets.Emplace(Cleanup);
    return true;
}
#endif

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitWorkspaceEditorAssetsTest, "GitWorkspace.Editor.RealAssetsThroughPanel", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGitWorkspaceEditorAssetsTest::RunTest(const FString&)
{
    FEditorAssetFixture F;
    auto Repo = MakeShared<GitWorkspace::FRepository, ESPMode::ThreadSafe>(F.Git, F.Root);
    auto Panel = SNew(SGitWorkspace).Repository(Repo);
    auto Settle = [&]()
    {
        const double Deadline = FPlatformTime::Seconds() + 120;
        while (!Panel->IsIdle() && FPlatformTime::Seconds() < Deadline)
        {
            Panel->Tick(FGeometry(), 0, 0); FPlatformProcess::Sleep(0.01f);
        }
        return TestTrue(TEXT("Panel background operation completed"), Panel->IsIdle());
    };
    if (!Settle()) return false;
    if (!TestFalse(TEXT("Acceptance starts without unsaved user packages"), Panel->HasDirtyPackages())) return false;
    F.Create(); F.Edit(0);
    if (!TestTrue(TEXT("Save four real Unreal packages"), F.Save())) return false;
    F.Call({TEXT("add"), TEXT(".")});
    if (!TestTrue(TEXT("Create isolated baseline"), F.Call({TEXT("commit"), TEXT("-qm"), TEXT("Fixture base")}).Ok())) return false;
    F.Edit(1);
    if (!TestTrue(TEXT("Save version A"), F.Save())) return false;
    Panel->Refresh(); if (!Settle()) return false;
    TestEqual(TEXT("Four unstaged assets"), Panel->Snapshot.Files.Num(), 4);
    auto Select = [&](bool bStaged)
    {
        Panel->List->ClearSelection();
        for (const auto& Row : Panel->Rows)
            if (Row->Group.IsEmpty() && Row->bStaged == bStaged) Panel->List->SetItemSelection(Row, true);
    };
    Select(false); Panel->ChangeIndex(true); if (!Settle()) return false;
    TestEqual(TEXT("Panel stages all four assets"), Panel->Snapshot.StagedCount(), 4);
    TArray<FString> PointersA; TArray<TArray<uint8>> BytesA;
    for (const FString& Path : F.Paths)
    {
        PointersA.Add(F.Call({TEXT("show"), TEXT(":") + Path}).Text()); BytesA.Add(F.Bytes(Path));
        TestTrue(Path + TEXT(" staged as LFS pointer"), PointersA.Last().StartsWith(TEXT("version https://git-lfs.github.com/spec/v1")));
    }
    F.Edit(2);
    TestTrue(TEXT("Real unsaved packages detected"), Panel->HasDirtyPackages());
    Panel->Message->SetText(FText::FromString(TEXT("Commit A through the panel")));
    const FString HeadBefore = F.Call({TEXT("rev-parse"), TEXT("HEAD")}).Text();
    Panel->Commit();
    TestTrue(TEXT("Commit blocked with save explanation"), Panel->IsIdle() && Panel->Feedback.Contains(TEXT("Save dirty assets")));
    TestEqual(TEXT("Dirty-package guard leaves HEAD unchanged"), F.Call({TEXT("rev-parse"), TEXT("HEAD")}).Text(), HeadBefore);
    Panel->ChangeIndex(true);
    TestTrue(TEXT("Stage blocked while packages unsaved"), Panel->Feedback.Contains(TEXT("Save dirty assets")));
    if (!TestTrue(TEXT("Save newer version B"), F.Save())) return false;
    TArray<TArray<uint8>> BytesB;
    for (int32 I = 0; I < F.Paths.Num(); ++I)
    {
        BytesB.Add(F.Bytes(F.Paths[I]));
        TestTrue(F.Paths[I] + TEXT(" A and B differ"), BytesA[I] != BytesB[I]);
        TestEqual(F.Paths[I] + TEXT(" saving B does not restage"), F.Call({TEXT("show"), TEXT(":") + F.Paths[I]}).Text(), PointersA[I]);
    }
    Panel->Refresh(); if (!Settle()) return false;
    int32 AssetRows = 0;
    for (const auto& Row : Panel->Rows)
    {
        if (Row->Group.IsEmpty())
        {
            ++AssetRows;
            TestTrue(TEXT("Each asset exposes both deltas and LFS lock intent"), Row->File.HasStaged() && Row->File.HasUnstaged() && Row->File.bLfs && Row->File.bLockable);
        }
    }
    TestEqual(TEXT("Four assets appear in both sections"), AssetRows, 8);
    Select(true);
    TestTrue(TEXT("Inspector explains later edits"), Panel->Inspector().ToString().Contains(TEXT("New edits since staging")));
    Panel->ShowDiff(); if (!Settle()) return false;
    TestTrue(TEXT("Inspector honestly labels LFS pointer comparison"), Panel->DiffText.Contains(TEXT("LFS pointer comparison")));
    Panel->Commit(); if (!Settle()) return false;
    TestTrue(TEXT("Panel reports local commit"), Panel->Feedback.Contains(TEXT("Local commit created")));
    TestEqual(TEXT("One local commit for four assets"), F.Call({TEXT("rev-list"), TEXT("--count"), TEXT("HEAD")}).Text().TrimEnd(), FString(TEXT("2")));
    for (int32 I = 0; I < F.Paths.Num(); ++I)
    {
        TestEqual(F.Paths[I] + TEXT(" history contains A"), F.Call({TEXT("show"), TEXT("HEAD:") + F.Paths[I]}).Text(), PointersA[I]);
        TestTrue(F.Paths[I] + TEXT(" B bytes survive commit"), F.Bytes(F.Paths[I]) == BytesB[I]);
    }
    Select(false); Panel->ChangeIndex(true); if (!Settle()) return false;
    Select(true); Panel->ChangeIndex(false); if (!Settle()) return false;
    TestEqual(TEXT("Panel unstage clears index"), Panel->Snapshot.StagedCount(), 0);
    for (int32 I = 0; I < F.Paths.Num(); ++I) TestTrue(F.Paths[I] + TEXT(" unstage preserves B"), F.Bytes(F.Paths[I]) == BytesB[I]);
    TestEqual(TEXT("Blueprint edits remain loaded"), CastChecked<UBlueprint>(F.Assets[0].Get())->BlueprintDescription, FString(TEXT("Fixture version 2")));
    TestEqual(TEXT("Map edits remain loaded"), F.World->GetWorldSettings()->KillZ, -3000.f);
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitIncomingPackageReviewTest, "GitWorkspace.Editor.IncomingPackageReview", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGitIncomingPackageReviewTest::RunTest(const FString&)
{
    FEditorAssetFixture F; F.Create(); F.Edit(0);
    if (!TestTrue(TEXT("Save real incoming review fixture packages"), F.Save())) return false;
    F.Call({TEXT("add"), TEXT(".")}); F.Call({TEXT("commit"), TEXT("-qm"), TEXT("review baseline")});
    GitWorkspace::FRepository Repo(F.Git, F.Root); const auto Local = Repo.Refresh();
    GitWorkspace::FRemoteSnapshot Remote;
    Remote.Root = Local.Root; Remote.Head = Local.Head; Remote.Branch = Local.Branch;
    Remote.Remote = TEXT("fixture"); Remote.RemoteRef = TEXT("refs/heads/main"); Remote.RemoteHead = FString::ChrN(40, 'a');
    Remote.bValid = true; Remote.Behind = 1; Remote.FetchedAt = FDateTime::UtcNow(); Remote.FetchedSeconds = FPlatformTime::Seconds();
    for (const auto& Path : F.Paths)
    {
        GitWorkspace::FIncomingChange C; C.Path = Path; C.OldMode = C.NewMode = TEXT("100644"); C.Status = 'M'; C.Kind = GitWorkspace::EPullPathKind::Package;
        Remote.IncomingChanges.Add(C);
    }
    GitWorkspace::FIncomingChange Unloaded; Unloaded.Path = TEXT("Content/NotLoaded.uasset"); Unloaded.Status = 'A'; Unloaded.Kind = GitWorkspace::EPullPathKind::Package;
    Remote.IncomingChanges.Add(Unloaded);
    auto External = Unloaded; External.Path = TEXT("Content/__ExternalActors__/Map/Actor.uasset"); Remote.IncomingChanges.Add(External);
    TArray<TArray<uint8>> Before;
    for (const auto& Path : F.Paths) Before.Add(F.Bytes(Path));
    auto Review = GitWorkspace::ReviewIncoming(Remote, Local);
    TestFalse(TEXT("Maps/external actors require restart"), Review.bCanReload);
    const auto CompleteList = Remote.IncomingChanges;
    Remote.IncomingChanges.SetNum(3);
    #if PLATFORM_MAC
    TestTrue(TEXT("Saved Blueprints, materials and textures can reload"), GitWorkspace::ReviewIncoming(Remote, Local).bCanReload);
    #endif
    Remote.IncomingChanges[0].Status = 'D';
    TestFalse(TEXT("Deletion requires restart"), GitWorkspace::ReviewIncoming(Remote, Local).bCanReload);
    Remote.IncomingChanges[0].Status = 'A';
    TestFalse(TEXT("In-memory add collision blocks reload"), GitWorkspace::ReviewIncoming(Remote, Local).bCanReload);
    Remote.IncomingChanges = CompleteList;
    TestFalse(TEXT("Package changes require editor-close handoff"), Review.bCanPull);
    #if PLATFORM_MAC
    TestTrue(TEXT("Saved Content packages eligible for coordinated restart"), Review.bCanRestart);
    #endif
    TestEqual(TEXT("All affected packages represented"), Review.Packages.Num(), 6);
    for (int32 I = 0; I < 4; ++I)
        TestTrue(TEXT("Saved loaded package identified"), Review.Packages[I].bMounted && Review.Packages[I].bLoaded && !Review.Packages[I].bDirty);
    TestTrue(TEXT("Map identified"), Review.Packages[3].bMap);
    TestFalse(TEXT("Review does not load absent package"), Review.Packages[4].bLoaded);
    TestTrue(TEXT("External actor package identified"), Review.Packages[5].bExternal);
    F.Edit(2); Review = GitWorkspace::ReviewIncoming(Remote, Local);
    TestFalse(TEXT("Unsaved packages also block coordinated restart"), Review.bCanRestart);
    TestTrue(TEXT("Unsaved package blocks at highest priority"), Review.Blocker.Contains(TEXT("Unsaved")));
    for (int32 I = 0; I < 4; ++I)
    {
        TestTrue(TEXT("Actual package dirty state reported"), Review.Packages[I].bDirty);
        TestTrue(TEXT("Review preserves saved package bytes"), F.Bytes(F.Paths[I]) == Before[I]);
    }
    TestEqual(TEXT("Review preserves unsaved Blueprint edits"), CastChecked<UBlueprint>(F.Assets[0].Get())->BlueprintDescription, FString(TEXT("Fixture version 2")));
    TestEqual(TEXT("Review does not change repository HEAD"), Repo.Refresh().Head, Local.Head);
    auto PanelRepo = MakeShared<GitWorkspace::FRepository, ESPMode::ThreadSafe>(F.Git, F.Root);
    auto Panel = SNew(SGitWorkspace).Repository(PanelRepo);
    while (Panel->Pending.IsValid()) { Panel->Pending.Wait(); Panel->Tick(FGeometry(), 0, 0); }
    Panel->Remote = Remote;
    Panel->RemoteAction(2);
    TestTrue(TEXT("Blocked Pull opens the native review without starting Git work"), Panel->IsIdle() && Panel->IncomingWindow.IsValid());
    TestTrue(TEXT("Panel surfaces unsaved-package reason"), Panel->Feedback.Contains(TEXT("Unsaved")));
    TestEqual(TEXT("Blocked panel Pull preserves HEAD"), Repo.Refresh().Head, Local.Head);
    GitWorkspace::FIncomingChange Doc; Doc.Path = TEXT("Docs/read\nme.md"); Doc.Status = 'M'; Doc.Kind = GitWorkspace::EPullPathKind::Documentation;
    Remote.IncomingChanges = {Doc};
    Review = GitWorkspace::ReviewIncoming(Remote, Local);
    TestFalse(TEXT("Unrelated unsaved packages also block documentation Pull"), Review.bCanPull);
    TestTrue(TEXT("Control characters escaped in displayed paths"), Review.Text.Contains(TEXT("Docs/read\\nme.md")));
    for (auto& Asset : F.Assets) Asset->GetOutermost()->SetDirtyFlag(false);
    Review = GitWorkspace::ReviewIncoming(Remote, Local);
    TestTrue(TEXT("Clean documentation update eligible"), Review.bCanPull);
    Remote.FetchedSeconds -= 301;
    TestFalse(TEXT("Expired review ineligible"), GitWorkspace::ReviewIncoming(Remote, Local).bCanPull);
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitWorkspaceSelectionTest, "GitWorkspace.Editor.SelectionWithSubmoduleAndNewAssets", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGitWorkspaceSelectionTest::RunTest(const FString&)
{
    FEditorAssetFixture F; F.Create(); F.Edit(0);
    if (!TestTrue(TEXT("Save untracked real assets"), F.Save())) return false;
    auto Write = [&](const FString& Path, const FString& Value)
    {
        const FString Filename = FPaths::Combine(F.Root, Path);
        IFileManager::Get().MakeDirectory(*FPaths::GetPath(Filename), true);
        FFileHelper::SaveStringToFile(Value, *Filename, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
    };
    const FString Nested = FPaths::Combine(F.Root, TEXT("Plugins/Nested"));
    IFileManager::Get().MakeDirectory(*Nested, true);
    auto InNested = [&](const TArray<FString>& Args) { return GitWorkspace::Run(F.Git, Nested, Args); };
    InNested({TEXT("init"), TEXT("-q")});
    InNested({TEXT("config"), TEXT("user.name"), TEXT("fixture")});
    InNested({TEXT("config"), TEXT("user.email"), TEXT("fixture@example.invalid")});
    InNested({TEXT("config"), TEXT("commit.gpgsign"), TEXT("false")});
    InNested({TEXT("config"), TEXT("core.hooksPath"), TEXT(".git/test-hooks")});
    Write(TEXT("Plugins/Nested/README.md"), TEXT("base\n"));
    InNested({TEXT("add"), TEXT("README.md")});
    if (!TestTrue(TEXT("Nested fixture commit"), InNested({TEXT("commit"), TEXT("-qm"), TEXT("base")}).Ok())) return false;
    const FString NestedHead = InNested({TEXT("rev-parse"), TEXT("HEAD")}).Text().TrimEnd();
    Write(TEXT("README.md"), TEXT("base\n")); Write(TEXT("graphify-out/cache.txt"), TEXT("base\n"));
    F.Call({TEXT("add"), TEXT(".gitattributes"), TEXT("README.md"), TEXT("graphify-out/cache.txt")});
    F.Call({TEXT("update-index"), TEXT("--add"), TEXT("--cacheinfo"), TEXT("160000"), NestedHead, TEXT("Plugins/Nested")});
    if (!TestTrue(TEXT("Parent fixture baseline excludes new assets"), F.Call({TEXT("commit"), TEXT("-qm"), TEXT("base")}).Ok())) return false;
    Write(TEXT("Plugins/Nested/README.md"), TEXT("nested edit\n"));
    Write(TEXT("README.md"), TEXT("parent edit\n")); Write(TEXT("graphify-out/cache.txt"), TEXT("cache edit\n"));
    auto Repo = MakeShared<GitWorkspace::FRepository, ESPMode::ThreadSafe>(F.Git, F.Root);
    auto Panel = SNew(SGitWorkspace).Repository(Repo);
    auto Settle = [&]() { while (Panel->Pending.IsValid()) { Panel->Pending.Wait(); Panel->Tick(FGeometry(), 0, 0); } };
    Settle();
    TestTrue(TEXT("Content is the default view"), Panel->bContentOnly);
    int32 DefaultFiles = 0;
    for (const auto& Row : Panel->Rows) if (Row->Group.IsEmpty())
    {
        ++DefaultFiles;
        TestTrue(TEXT("Default view contains only game content"), Row->File.Path.StartsWith(TEXT("Content/")));
        Panel->List->SetItemSelection(Row, true);
    }
    TestEqual(TEXT("Default view exposes four new real assets"), DefaultFiles, 4);
    Panel->List->ClearSelection(); Panel->List->SetItemSelection(Panel->Rows[1], true);
    TestTrue(TEXT("Saved new asset offers Lock without a separate verification step"), Panel->CanLockSelected());
    TestTrue(TEXT("New asset lock guidance explains no automatic locking or staging"), Panel->LockHint().ToString().Contains(TEXT("does not stage")) && Panel->LockHint().ToString().Contains(TEXT("Automatic locking")));
    Panel->RebuildRows();
    TestEqual(TEXT("Refresh preserves the selected asset"), Panel->SelectedIndexPaths(true).Num(), 1);
    Panel->List->ClearSelection(); Panel->bContentOnly = false; Panel->RebuildRows();
    TestTrue(TEXT("Untracked assets sort before tracked documentation/cache"), Panel->Rows.Num() > 1 && Panel->Rows[1]->File.bUntracked && Panel->Rows[1]->File.Path.StartsWith(TEXT("Content/")));
    for (const auto& Row : Panel->Rows) if (Row->Group.IsEmpty() && Row->File.bSubmodule) Panel->List->SetItemSelection(Row, true);
    TestEqual(TEXT("Submodule-only selection has zero eligible paths"), Panel->SelectedIndexPaths(true).Num(), 0);
    Panel->ChangeIndex(true);
    TestTrue(TEXT("Submodule-only action explains limitation without Git work"), Panel->IsIdle() && Panel->Feedback.Contains(TEXT("submodules")));
    for (const auto& Row : Panel->Rows) if (!Row->bStaged) Panel->List->SetItemSelection(Row, true);
    int32 Skipped = 0;
    TestEqual(TEXT("Mixed selection counts six eligible files"), Panel->SelectedIndexPaths(true, &Skipped).Num(), 6);
    TestEqual(TEXT("Mixed selection identifies one protected submodule"), Skipped, 1);
    const FString Gitlink = F.Call({TEXT("ls-files"), TEXT("--stage"), TEXT("--"), TEXT("Plugins/Nested")}).Text();
    const auto ActorBytes = F.Bytes(F.Paths[0]);
    Panel->ChangeIndex(true); Settle();
    TestEqual(TEXT("Eligible files stage despite selected submodule"), Panel->Snapshot.StagedCount(), 6);
    TestTrue(TEXT("Successful partial staging reports skip"), Panel->Feedback.Contains(TEXT("Staged 6")) && Panel->Feedback.Contains(TEXT("Skipped 1")));
    TestEqual(TEXT("Gitlink untouched"), F.Call({TEXT("ls-files"), TEXT("--stage"), TEXT("--"), TEXT("Plugins/Nested")}).Text(), Gitlink);
    TestTrue(TEXT("New actor stages an actual LFS pointer"), F.Call({TEXT("show"), TEXT(":") + F.Paths[0]}).Text().StartsWith(TEXT("version https://git-lfs.github.com/spec/v1")));
    Panel->bContentOnly = true; Panel->RebuildRows();
    TestEqual(TEXT("Content filter does not alter the index"), Panel->Snapshot.StagedCount(), 6);
    TestEqual(TEXT("Outside-content staged files are disclosed"), Panel->HiddenStagedCount(), 2);
    const FString BeforeHiddenCommit = F.Call({TEXT("rev-parse"), TEXT("HEAD")}).Text();
    Panel->Commit();
    TestTrue(TEXT("Hidden staged files require review before commit"), Panel->IsIdle() && Panel->Feedback.Contains(TEXT("hidden by this view")));
    TestEqual(TEXT("Hidden-file guard preserves HEAD"), F.Call({TEXT("rev-parse"), TEXT("HEAD")}).Text(), BeforeHiddenCommit);
    Panel->bContentOnly = false; Panel->RebuildRows();
    for (const auto& Row : Panel->Rows) if (Row->bStaged) Panel->List->SetItemSelection(Row, true);
    Panel->ChangeIndex(false); Settle();
    TestEqual(TEXT("Unstage returns new assets to untracked"), Panel->Snapshot.StagedCount(), 0);
    TestTrue(TEXT("Stage/unstage preserves actor bytes"), F.Bytes(F.Paths[0]) == ActorBytes);
    Panel->FileFilter = TEXT("Content/BP_Probe"); Panel->RebuildRows();
    int32 Visible = 0;
    for (const auto& Row : Panel->Rows) if (Row->Group.IsEmpty()) { ++Visible; Panel->List->SetItemSelection(Row, true); }
    TestEqual(TEXT("Path filter isolates new Blueprint"), Visible, 1);
    TestEqual(TEXT("Filtered selection excludes hidden rows"), Panel->SelectedIndexPaths(true).Num(), 1);
    Panel->ChangeIndex(true); Settle();
    TestEqual(TEXT("Filtered staging stages only actor"), Panel->Snapshot.StagedCount(), 1);
    Panel->FileFilter.Empty(); Panel->bContentOnly = true;
    GitWorkspace::FFile Inside, Outside; Inside.Path = TEXT("Content/Clean.uasset"); Outside.Path = TEXT("Source/Outside.cpp");
    Panel->Locks.Candidates = {Inside, Outside}; Panel->RebuildRows();
    bool bInsideVisible = false, bOutsideVisible = false;
    for (const auto& Row : Panel->Rows) { bInsideVisible |= Row->File.Path == Inside.Path; bOutsideVisible |= Row->File.Path == Outside.Path; }
    TestTrue(TEXT("Scope applies to clean lock candidates"), bInsideVisible && !bOutsideVisible);
    Panel->bContentOnly = false; Panel->RebuildRows(); bOutsideVisible = false;
    for (const auto& Row : Panel->Rows) bOutsideVisible |= Row->File.Path == Outside.Path;
    TestTrue(TEXT("Whole repo restores outside-content lock candidates"), bOutsideVisible);
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitIncomingLfsPanelTest, "GitWorkspace.Editor.IncomingLfsThroughPanel", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGitIncomingLfsPanelTest::RunTest(const FString&)
{
    FEditorAssetFixture F; F.Create(); F.Edit(0);
    if (!TestTrue(TEXT("Save real download fixture packages"), F.Save())) return false;
    F.Call({TEXT("add"), TEXT(".")}); F.Call({TEXT("commit"), TEXT("-qm"), TEXT("download baseline")});
    F.Call({TEXT("branch"), TEXT("-M"), TEXT("main")});
    const FString Bare = FPaths::Combine(F.Root, TEXT(".git/incoming-remote.git"));
    const FString Other = FPaths::Combine(F.Root, TEXT(".git/incoming-clone"));
    F.Call({TEXT("init"), TEXT("--bare"), Bare}); F.Call({TEXT("remote"), TEXT("add"), TEXT("origin"), Bare});
    if (!TestTrue(TEXT("Upload baseline objects"), F.Call({TEXT("lfs"), TEXT("push"), TEXT("origin"), TEXT("main")}).Ok()) ||
        !TestTrue(TEXT("Publish baseline"), F.Call({TEXT("push"), TEXT("-u"), TEXT("origin"), TEXT("main")}).Ok()) ||
        !TestTrue(TEXT("Clone baseline packages"), F.Call({TEXT("clone"), TEXT("--branch"), TEXT("main"), Bare, Other}).Ok())) return false;
    auto InOther = [&](const TArray<FString>& Args) { return GitWorkspace::Run(F.Git, Other, Args); };
    InOther({TEXT("config"), TEXT("user.name"), TEXT("fixture")}); InOther({TEXT("config"), TEXT("user.email"), TEXT("fixture@example.invalid")});
    InOther({TEXT("config"), TEXT("commit.gpgsign"), TEXT("false")}); InOther({TEXT("config"), TEXT("core.hooksPath"), TEXT(".git/test-hooks")});
    TArray<TArray<uint8>> BeforeBytes;
    for (const auto& Path : F.Paths) BeforeBytes.Add(F.Bytes(Path));
    F.Edit(1); if (!TestTrue(TEXT("Save incoming package revision"), F.Save())) return false;
    const FString IncomingFile = FPaths::Combine(Other, F.Paths[0]);
    FPlatformFileManager::Get().GetPlatformFile().SetReadOnly(*IncomingFile, false);
    if (!TestTrue(TEXT("Copy real incoming Blueprint to publisher clone"), FFileHelper::SaveArrayToFile(F.Bytes(F.Paths[0]), *IncomingFile))) return false;
    InOther({TEXT("add"), TEXT("--"), F.Paths[0]}); InOther({TEXT("commit"), TEXT("-qm"), TEXT("incoming Blueprint")});
    if (!TestTrue(TEXT("Upload incoming Blueprint"), InOther({TEXT("lfs"), TEXT("push"), TEXT("origin"), TEXT("main")}).Ok()) ||
        !TestTrue(TEXT("Publish incoming Blueprint"), InOther({TEXT("push"), TEXT("origin"), TEXT("main")}).Ok())) return false;
    // Only these disposable fixture files are restored. Leave a newer unsaved
    // version loaded to prove preparation never saves or reloads editor packages.
    for (int32 I = 0; I < F.Paths.Num(); ++I)
        if (!TestTrue(TEXT("Restore fixture baseline bytes"), FFileHelper::SaveArrayToFile(BeforeBytes[I], *FPaths::Combine(F.Root, F.Paths[I])))) return false;
    F.Edit(2);
    auto Repo = MakeShared<GitWorkspace::FRepository, ESPMode::ThreadSafe>(F.Git, F.Root);
    auto Panel = SNew(SGitWorkspace).Repository(Repo);
    auto Settle = [&]() { while (Panel->Pending.IsValid()) { Panel->Pending.Wait(); Panel->Tick(FGeometry(), 0, 0); } };
    Settle(); Panel->RemoteAction(0); Settle();
    if (!TestTrue(TEXT("Panel fetches incoming package"), Panel->Remote.IsFresh() && Panel->Remote.Behind == 1)) return false;
    const auto Before = Repo->Refresh();
    Panel->ShowIncomingReview();
    TestTrue(TEXT("Review discloses unsaved packages"), Panel->IncomingReport->GetText().ToString().Contains(TEXT("UNSAVED PACKAGES")));
    Panel->DownloadIncomingLfs();
    TestTrue(TEXT("Download progress visible"), Panel->bDownloadingLfs && Panel->IncomingReport->GetText().ToString().Contains(TEXT("Downloading and verifying")));
    Settle();
    TestTrue(TEXT("Native action verifies cache: ") + Panel->Feedback, Panel->IncomingLfs.Matches(Panel->Remote));
    TestTrue(TEXT("Open review updates after download"), Panel->IncomingReport->GetText().ToString().Contains(TEXT("LFS cache verified for commit")));
    TestTrue(TEXT("Review keeps Pull blocker separate"), Panel->IncomingReport->GetText().ToString().Contains(TEXT("PULL BLOCKED")));
    TestEqual(TEXT("Panel download preserves HEAD"), Repo->Refresh().Head, Before.Head);
    TestTrue(TEXT("Panel download preserves index"), Repo->Refresh().IndexEntries == Before.IndexEntries);
    for (int32 I = 0; I < F.Paths.Num(); ++I)
    {
        TestTrue(TEXT("Panel download preserves saved package bytes"), F.Bytes(F.Paths[I]) == BeforeBytes[I]);
        TestTrue(TEXT("Unsaved package remains dirty"), F.Assets[I]->GetOutermost()->IsDirty());
    }
    TestEqual(TEXT("Unsaved Blueprint remains loaded"), CastChecked<UBlueprint>(F.Assets[0].Get())->BlueprintDescription, FString(TEXT("Fixture version 2")));
    TestEqual(TEXT("Unsaved map remains loaded"), F.World->GetWorldSettings()->KillZ, -3000.f);
    Panel->RemoteAction(0); Settle();
    TestFalse(TEXT("New fetch clears previous cache certification"), Panel->IncomingLfs.bVerified);
    TestTrue(TEXT("Open review clears previous success"), Panel->IncomingReport->GetText().ToString().Contains(TEXT("has not been verified")));
    return true;
}
#endif

#if WITH_DEV_AUTOMATION_TESTS && PLATFORM_MAC
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitStashAssetReloadTest, "GitWorkspace.Editor.StashAndReloadRealAssets", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGitStashAssetReloadTest::RunTest(const FString&)
{
    FEditorAssetFixture F; F.Create(); F.Edit(0);
    if (!TestTrue(TEXT("Save stash baseline"), F.Save())) return false;
    F.Call({TEXT("add"), TEXT(".")}); F.Call({TEXT("commit"), TEXT("-qm"), TEXT("stash base")});
    const auto BaselineMap = F.Bytes(F.Paths[3]);
    auto SaveAssets = [&](int32 Version)
    {
        CastChecked<UBlueprint>(F.Assets[0].Get())->BlueprintDescription = FString::Printf(TEXT("Fixture version %d"), Version);
        CastChecked<UMaterial>(F.Assets[1].Get())->TwoSided = true;
        CastChecked<UTexture2D>(F.Assets[2].Get())->SRGB = true;
        bool bOk = true;
        for (int32 I = 0; I < 3; ++I)
        {
            auto* Asset = F.Assets[I].Get(); Asset->MarkPackageDirty();
            FSavePackageArgs Args; Args.TopLevelFlags = RF_Public | RF_Standalone; Args.SaveFlags = SAVE_NoError;
            bOk &= UPackage::SavePackage(Asset->GetPackage(), Asset, *FPaths::Combine(F.Root, F.Paths[I]), Args);
        }
        UPackage::WaitForAsyncFileWrites(); return bOk;
    };
    if (!TestTrue(TEXT("Save staged assets"), SaveAssets(1))) return false;
    F.Call({TEXT("add"), TEXT(".")});
    GitWorkspace::FRepository Repo(F.Git, F.Root);
    const auto Index = Repo.Refresh().IndexEntries;
    if (!TestTrue(TEXT("Save separate working Blueprint"), SaveAssets(2))) return false;
    TArray<TArray<uint8>> WorkingBytes;
    for (int32 I = 0; I < 3; ++I) WorkingBytes.Add(F.Bytes(F.Paths[I]));
    F.Call({TEXT("config"), TEXT("filter.lfs.process"), TEXT("git-lfs filter-process --skip")});
    FString Error; GitWorkspaceSession::FLease Lease;
    if (!TestTrue(TEXT("Stash fixture write lease"), Lease.Acquire(F.Root, true, Error))) return false;
    const auto Review = Repo.ReviewStash();
    if (!TestTrue(TEXT("Review real assets: ") + Review.Error, Review.bValid)) return false;
    F.Assets[0]->MarkPackageDirty();
    auto Result = GitWorkspace::StashAndReload(Repo, Review, TEXT("Asset iteration"), Lease);
    TestFalse(TEXT("Unsaved memory blocks stash"), Result.bSuccess);
    TestFalse(TEXT("No recovery needed for preflight refusal"), Result.bRecoveryRequired);
    TestTrue(TEXT("Unsaved guard preserves index"), Repo.Refresh().IndexEntries == Index);
    F.Assets[0]->GetPackage()->SetDirtyFlag(false);
    const auto Delegate = FCoreUObjectDelegates::OnPackageReloaded.AddLambda([&](EPackageReloadPhase Phase, FPackageReloadedEvent* Event)
    {
        if (Phase == EPackageReloadPhase::OnPackageFixup && Event)
            for (auto& Asset : F.Assets)
                if (UObject* const* Replacement = Event->GetRepointedObjects().Find(Asset.Get())) Asset.Reset(*Replacement);
    });
    ON_SCOPE_EXIT { FCoreUObjectDelegates::OnPackageReloaded.Remove(Delegate); };
    TWeakObjectPtr<UObject> OldBlueprint = F.Assets[0].Get();
    Result = GitWorkspace::StashAndReload(Repo, Review, TEXT("Asset iteration"), Lease);
    if (!TestTrue(TEXT("Create and reload: ") + Result.Message, Result.bSuccess)) return false;
    TestEqual(TEXT("Three loaded assets reloaded on create"), Result.Reloaded, 3);
    TestFalse(TEXT("Old loaded Blueprint purged"), OldBlueprint.IsValid());
    TestEqual(TEXT("Blueprint reverts in memory"), CastChecked<UBlueprint>(F.Assets[0].Get())->BlueprintDescription, FString(TEXT("Fixture version 0")));
    TestFalse(TEXT("Material reverts in memory"), !!CastChecked<UMaterial>(F.Assets[1].Get())->TwoSided);
    TestFalse(TEXT("Texture reverts in memory"), !!CastChecked<UTexture2D>(F.Assets[2].Get())->SRGB);
    TestTrue(TEXT("Clean after create"), Repo.Refresh().Files.IsEmpty());
    Result = GitWorkspace::StashAndReload(Repo, Repo.ReviewStash(Repo.ListStashes().Entries[0].Oid), FString(), Lease);
    if (!TestTrue(TEXT("Apply and reload: ") + Result.Message, Result.bSuccess)) return false;
    TestEqual(TEXT("Three loaded assets reloaded on apply"), Result.Reloaded, 3);
    TestEqual(TEXT("Working Blueprint restored in memory"), CastChecked<UBlueprint>(F.Assets[0].Get())->BlueprintDescription, FString(TEXT("Fixture version 2")));
    TestTrue(TEXT("Material restored in memory"), !!CastChecked<UMaterial>(F.Assets[1].Get())->TwoSided);
    TestTrue(TEXT("Texture restored in memory"), !!CastChecked<UTexture2D>(F.Assets[2].Get())->SRGB);
    for (int32 I = 0; I < 3; ++I) TestTrue(TEXT("Exact hydrated working payload restored"), F.Bytes(F.Paths[I]) == WorkingBytes[I]);
    TestTrue(TEXT("Staging restored separately"), Repo.Refresh().IndexEntries == Index);
    TestTrue(TEXT("Map bytes untouched"), F.Bytes(F.Paths[3]) == BaselineMap);
    TestEqual(TEXT("Stash remains after apply"), Repo.ListStashes().Entries.Num(), 1);
    const FString KeptOid = Repo.ListStashes().Entries[0].Oid;
    const TWeakObjectPtr<UObject> ExcludedMaterial = F.Assets[1].Get(), ExcludedTexture = F.Assets[2].Get();
    const auto Selected = Repo.ReviewSelectedStash({F.Paths[0]});
    Result = GitWorkspace::StashAndReload(Repo, Selected, TEXT("Blueprint only"), Lease);
    if (!TestTrue(TEXT("Selected loaded-asset stash: ") + Result.Message, Result.bSuccess)) return false;
    TestEqual(TEXT("Only selected loaded package reloads"), Result.Reloaded, 1);
    TestEqual(TEXT("Selected Blueprint reverts in memory"), CastChecked<UBlueprint>(F.Assets[0].Get())->BlueprintDescription, FString(TEXT("Fixture version 0")));
    TestTrue(TEXT("Excluded material object retained"), ExcludedMaterial.IsValid() && ExcludedMaterial.Get() == F.Assets[1].Get());
    TestTrue(TEXT("Excluded texture object retained"), ExcludedTexture.IsValid() && ExcludedTexture.Get() == F.Assets[2].Get());
    for (int32 I = 1; I < 3; ++I) TestTrue(TEXT("Excluded loaded payload unchanged"), F.Bytes(F.Paths[I]) == WorkingBytes[I]);
    const auto DisjointApply = Repo.ReviewStash(Repo.ListStashes().Entries[0].Oid);
    const auto ToDelete = Repo.InspectStash(DisjointApply.Oid, TEXT("stash@{0}"));
    F.Assets[0]->MarkPackageDirty();
    Result = GitWorkspace::ApplyStashAndDelete(Repo, DisjointApply, ToDelete, Lease);
    TestTrue(TEXT("Unsaved asset prevents Apply and delete without recovery"), !Result.bSuccess && !Result.bRecoveryRequired);
    TestEqual(TEXT("Failed restore keeps both stashes"), Repo.ListStashes().Entries.Num(), 2);
    F.Assets[0]->GetPackage()->SetDirtyFlag(false);
    Result = GitWorkspace::ApplyStashAndDelete(Repo, DisjointApply, ToDelete, Lease);
    if (!TestTrue(TEXT("Apply while unrelated assets remain changed: ") + Result.Message, Result.bSuccess)) return false;
    TestEqual(TEXT("Only applied Blueprint reloads"), Result.Reloaded, 1);
    TestEqual(TEXT("Saved Blueprint restored"), CastChecked<UBlueprint>(F.Assets[0].Get())->BlueprintDescription, FString(TEXT("Fixture version 2")));
    TestTrue(TEXT("Unrelated material object retained through Apply"), ExcludedMaterial.Get() == F.Assets[1].Get());
    TestTrue(TEXT("Unrelated texture object retained through Apply"), ExcludedTexture.Get() == F.Assets[2].Get());
    TestTrue(TEXT("All distinct staged versions retained/restored"), Repo.Refresh().IndexEntries == Index);
    for (int32 I = 1; I < 3; ++I) TestTrue(TEXT("Unrelated loaded bytes untouched through Apply"), F.Bytes(F.Paths[I]) == WorkingBytes[I]);
    const auto Remaining = Repo.ListStashes();
    TestTrue(TEXT("Only successfully restored selected stash deleted"), Remaining.Entries.Num() == 1 && Remaining.Entries[0].Oid == KeptOid);
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitDiscardLoadedAssetTest, "GitWorkspace.Discard.LoadedAssetPreservesStaging", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGitDiscardLoadedAssetTest::RunTest(const FString&)
{
    FEditorAssetFixture F; F.Create(); F.Edit(0);
    if (!TestTrue(TEXT("Save discard baseline"), F.Save())) return false;
    F.Call({TEXT("add"), TEXT(".")}); F.Call({TEXT("commit"), TEXT("-qm"), TEXT("discard base")});
    const auto MapBytes = F.Bytes(F.Paths[3]);
    auto SaveAssets = [&](int32 Version)
    {
        CastChecked<UBlueprint>(F.Assets[0].Get())->BlueprintDescription = FString::Printf(TEXT("Fixture version %d"), Version);
        CastChecked<UMaterial>(F.Assets[1].Get())->TwoSided = Version % 2 != 0;
        CastChecked<UTexture2D>(F.Assets[2].Get())->SRGB = Version % 2 != 0;
        bool bOk = true;
        for (int32 I = 0; I < 3; ++I)
        {
            auto* Asset = F.Assets[I].Get(); Asset->MarkPackageDirty();
            FSavePackageArgs Args; Args.TopLevelFlags = RF_Public | RF_Standalone; Args.SaveFlags = SAVE_NoError;
            bOk &= UPackage::SavePackage(Asset->GetPackage(), Asset, *FPaths::Combine(F.Root, F.Paths[I]), Args);
        }
        UPackage::WaitForAsyncFileWrites(); return bOk;
    };
    if (!TestTrue(TEXT("Save staged version one"), SaveAssets(1))) return false;
    F.Call({TEXT("add"), TEXT(".")}); const auto StagedBlueprint = F.Bytes(F.Paths[0]);
    if (!TestTrue(TEXT("Save working version two"), SaveAssets(2))) return false;
    TArray<TArray<uint8>> Working; for (int32 I = 0; I < 3; ++I) Working.Add(F.Bytes(F.Paths[I]));
    F.Call({TEXT("config"), TEXT("filter.lfs.process"), TEXT("git-lfs filter-process --skip")});
    GitWorkspace::FRepository Repo(F.Git, F.Root); const auto Before = Repo.Refresh();
    const auto Review = Repo.ReviewDiscard({F.Paths[0]});
    if (!TestTrue(TEXT("Review selected loaded Blueprint: ") + Review.Error, Review.IsFresh())) return false;
    FString Error; GitWorkspaceSession::FLease Lease;
    if (!TestTrue(TEXT("Discard fixture lease"), Lease.Acquire(F.Root, true, Error))) return false;
    F.Assets[0]->MarkPackageDirty(); auto Result = GitWorkspace::DiscardAndReload(Repo, Review, Lease);
    TestTrue(TEXT("Unsaved memory blocks discard before recovery"), !Result.bSuccess && !Result.bRecoveryRequired);
    TestTrue(TEXT("Refusal retains saved bytes"), F.Bytes(F.Paths[0]) == Working[0]);
    F.Assets[0]->GetPackage()->SetDirtyFlag(false);
    const auto Delegate = FCoreUObjectDelegates::OnPackageReloaded.AddLambda([&](EPackageReloadPhase Phase, FPackageReloadedEvent* Event)
    {
        if (Phase == EPackageReloadPhase::OnPackageFixup && Event)
            for (auto& Asset : F.Assets)
                if (UObject* const* Replacement = Event->GetRepointedObjects().Find(Asset.Get())) Asset.Reset(*Replacement);
    });
    ON_SCOPE_EXIT { FCoreUObjectDelegates::OnPackageReloaded.Remove(Delegate); };
    const TWeakObjectPtr<UObject> OldBlueprint = F.Assets[0].Get(), Material = F.Assets[1].Get(), Texture = F.Assets[2].Get();
    Result = GitWorkspace::DiscardAndReload(Repo, Review, Lease);
    if (!TestTrue(TEXT("Discard and reload: ") + Result.Message, Result.bSuccess)) return false;
    TestEqual(TEXT("Only selected asset reloaded"), Result.Reloaded, 1); TestFalse(TEXT("Old Blueprint purged"), OldBlueprint.IsValid());
    TestEqual(TEXT("Memory restores staged version one"), CastChecked<UBlueprint>(F.Assets[0].Get())->BlueprintDescription, FString(TEXT("Fixture version 1")));
    TestTrue(TEXT("Exact hydrated staged bytes restored"), F.Bytes(F.Paths[0]) == StagedBlueprint);
    TestTrue(TEXT("Staging retained exactly"), Repo.Refresh().IndexEntries == Before.IndexEntries); TestEqual(TEXT("HEAD retained"), Repo.Refresh().Head, Before.Head);
    TestTrue(TEXT("Other loaded objects retained"), Material.Get() == F.Assets[1].Get() && Texture.Get() == F.Assets[2].Get());
    for (int32 I = 1; I < 3; ++I) TestTrue(TEXT("Other saved edits retained"), F.Bytes(F.Paths[I]) == Working[I]);
    TestTrue(TEXT("Map retained"), F.Bytes(F.Paths[3]) == MapBytes); TestTrue(TEXT("No stash created"), Repo.ListStashes().Entries.IsEmpty());
    FString Canonical, GitDir; GitWorkspaceSession::FindRepository(F.Root, Canonical, GitDir);
    TArray<uint8> Backup; FFileHelper::LoadFileToArray(Backup, *FPaths::Combine(GitDir, TEXT("uegit/discard"), Review.Id, TEXT("payload/0000.bin")));
    TestTrue(TEXT("Backup has discarded full working asset"), Backup == Working[0]);
    TestFalse(TEXT("Successful asset reload clears recovery marker"), IFileManager::Get().FileExists(*GitWorkspaceSession::RecoveryFile(GitDir)));
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitStashApplyDeleteIdentityTest, "GitWorkspace.Editor.StashApplyDeleteIdentity", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGitStashApplyDeleteIdentityTest::RunTest(const FString&)
{
    FEditorAssetFixture F;
    const FString Path = FPaths::Combine(F.Root, TEXT("README.md"));
    FFileHelper::SaveStringToFile(TEXT("base"), *Path);
    F.Call({TEXT("add"), TEXT(".")}); F.Call({TEXT("commit"), TEXT("-qm"), TEXT("base")});
    FFileHelper::SaveStringToFile(TEXT("staged"), *Path); F.Call({TEXT("add"), TEXT("README.md")});
    GitWorkspace::FRepository Repo(F.Git, F.Root);
    const auto Staged = Repo.Refresh().IndexEntries;
    FFileHelper::SaveStringToFile(TEXT("working"), *Path); const auto Working = F.Bytes(TEXT("README.md"));
    F.Call({TEXT("stash"), TEXT("push"), TEXT("-m"), TEXT("original")});
    const FString Oid = F.Call({TEXT("rev-parse"), TEXT("refs/stash")}).Text().TrimEnd();
    // Git coalesces consecutive identical reflog values; separate duplicates
    // with a genuinely different stash, as an external client would.
    FFileHelper::SaveStringToFile(TEXT("intermediate work"), *Path);
    F.Call({TEXT("stash"), TEXT("push"), TEXT("-m"), TEXT("intermediate")});
    const FString Middle = F.Call({TEXT("rev-parse"), TEXT("refs/stash")}).Text().TrimEnd();
    F.Call({TEXT("stash"), TEXT("store"), TEXT("-m"), TEXT("duplicate"), Oid});
    FString Error; GitWorkspaceSession::FLease Lease;
    if (!TestTrue(TEXT("Apply/delete fixture lease"), Lease.Acquire(F.Root, true, Error))) return false;
    const auto Base = Repo.Refresh(); const auto BeforeBytes = F.Bytes(TEXT("README.md"));
    const auto Review = Repo.ReviewStash(Oid);
    auto Inspection = Repo.InspectStash(Oid, TEXT("stash@{2}"));
    if (!TestTrue(TEXT("Review exact older duplicate entry"), Review.IsFresh() && Inspection.IsFresh())) return false;
    auto Expired = Inspection; Expired.ReviewedSeconds -= 301;
    TestFalse(TEXT("Expired deletion inspection refused"), GitWorkspace::ApplyStashAndDelete(Repo, Review, Expired, Lease).bSuccess);
    auto Mismatch = Inspection; Mismatch.Entry.Oid = Base.Head;
    TestFalse(TEXT("Different inspected object refused"), GitWorkspace::ApplyStashAndDelete(Repo, Review, Mismatch, Lease).bSuccess);
    F.Call({TEXT("stash"), TEXT("store"), TEXT("-m"), TEXT("newer entry"), Middle});
    const auto Stale = GitWorkspace::ApplyStashAndDelete(Repo, Review, Inspection, Lease);
    TestFalse(TEXT("Changed ordinal list refused before Apply"), Stale.bSuccess);
    TestTrue(TEXT("Preflight failures leave saved files untouched"), F.Bytes(TEXT("README.md")) == BeforeBytes);
    TestTrue(TEXT("Preflight failures leave staging untouched"), Repo.Refresh().IndexEntries == Base.IndexEntries);
    Inspection = Repo.InspectStash(Oid, TEXT("stash@{3}"));
    const auto Applied = GitWorkspace::ApplyStashAndDelete(Repo, Review, Inspection, Lease);
    if (!TestTrue(TEXT("Apply/delete older duplicate: ") + Applied.Message, Applied.bSuccess)) return false;
    TestTrue(TEXT("Working bytes restored"), F.Bytes(TEXT("README.md")) == Working);
    TestTrue(TEXT("Distinct staging restored"), Repo.Refresh().IndexEntries == Staged);
    TestEqual(TEXT("HEAD unchanged"), Repo.Refresh().Head, Base.Head);
    const auto Remaining = Repo.ListStashes();
    TestTrue(TEXT("Only original entry deleted; duplicates retained in order"), Remaining.Entries.Num() == 3 &&
        Remaining.Entries[0].Oid == Middle && Remaining.Entries[0].Label == TEXT("newer entry") &&
        Remaining.Entries[1].Oid == Oid && Remaining.Entries[1].Label == TEXT("duplicate") &&
        Remaining.Entries[2].Oid == Middle && Remaining.Entries[2].Label.Contains(TEXT("intermediate")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitStashApplyDeleteRecoveryTest, "GitWorkspace.Editor.StashApplyDeleteRecovery", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGitStashApplyDeleteRecoveryTest::RunTest(const FString&)
{
    // Simulate external changes during package reload, on each side of the
    // completion gate. These repositories and packages are disposable.
    for (bool bFailCompletion : {false, true})
    {
        FEditorAssetFixture F;
        F.Assets.Emplace(FKismetEditorUtilities::CreateBlueprint(AActor::StaticClass(), F.Package(TEXT("BP_Probe")), TEXT("BP_Probe"), BPTYPE_Normal));
        F.Paths = {TEXT("Content/BP_Probe.uasset")};
        const FString Document = FPaths::Combine(F.Root, TEXT("README.md"));
        FFileHelper::SaveStringToFile(TEXT("base"), *Document);
        if (!TestTrue(TEXT("Save baseline Blueprint"), F.Save())) return false;
        F.Call({TEXT("add"), TEXT(".")}); F.Call({TEXT("commit"), TEXT("-qm"), TEXT("baseline")});
        CastChecked<UBlueprint>(F.Assets[0].Get())->BlueprintDescription = TEXT("Saved iteration");
        F.Assets[0]->MarkPackageDirty();
        if (!TestTrue(TEXT("Save modified Blueprint"), F.Save())) return false;
        const auto Saved = F.Bytes(F.Paths[0]);
        GitWorkspace::FRepository Repo(F.Git, F.Root); FString Error; GitWorkspaceSession::FLease Lease;
        if (!TestTrue(TEXT("Recovery fixture lease"), Lease.Acquire(F.Root, true, Error))) return false;
        bool bInterfere = false, bObservedStashBeforeCompletion = false, bInterferenceSucceeded = false;
        FString Oid, AlternateOid;
        const auto Delegate = FCoreUObjectDelegates::OnPackageReloaded.AddLambda([&](EPackageReloadPhase Phase, FPackageReloadedEvent* Event)
        {
            if (Phase == EPackageReloadPhase::OnPackageFixup && Event)
                if (UObject* const* Replacement = Event->GetRepointedObjects().Find(F.Assets[0].Get())) F.Assets[0].Reset(*Replacement);
            if (Phase == EPackageReloadPhase::PostPackageFixup && bInterfere)
            {
                bInterfere = false;
                const auto List = Repo.ListStashes();
                bObservedStashBeforeCompletion = List.Entries.Num() == 1 && List.Entries[0].Oid == Oid;
                bInterferenceSucceeded = bFailCompletion ? FFileHelper::SaveStringToFile(TEXT("external edit"), *Document) :
                    F.Call({TEXT("stash"), TEXT("store"), TEXT("-m"), TEXT("concurrent entry"), AlternateOid}).Ok();
            }
        });
        ON_SCOPE_EXIT { FCoreUObjectDelegates::OnPackageReloaded.Remove(Delegate); };
        const auto CreateReview = Repo.ReviewStash();
        AlternateOid = CreateReview.Oid; // Same saved files, distinct commit title.
        const auto Created = GitWorkspace::StashAndReload(Repo, CreateReview, TEXT("Recovery test"), Lease);
        if (!TestTrue(TEXT("Create recovery fixture stash: ") + Created.Message, Created.bSuccess)) return false;
        Oid = Repo.ListStashes().Entries[0].Oid;
        const auto Review = Repo.ReviewStash(Oid);
        const auto Inspection = Repo.InspectStash(Oid, TEXT("stash@{0}"));
        bInterfere = true;
        const auto Result = GitWorkspace::ApplyStashAndDelete(Repo, Review, Inspection, Lease);
        TestTrue(TEXT("Stash still present during reload, before completion"), bObservedStashBeforeCompletion);
        TestTrue(TEXT("External interference exercised"), bInterferenceSucceeded);
        TestFalse(TEXT("Interrupted composite action not reported as success"), Result.bSuccess);
        TestEqual(TEXT("Only failed restore verification needs editor recovery"), Result.bRecoveryRequired, bFailCompletion);
        TestTrue(TEXT("Hydrated working asset bytes restored"), F.Bytes(F.Paths[0]) == Saved);
        TestEqual(TEXT("Loaded asset reflects restored stash"), CastChecked<UBlueprint>(F.Assets[0].Get())->BlueprintDescription, FString(TEXT("Saved iteration")));
        const auto Remaining = Repo.ListStashes();
        TestEqual(TEXT("No entry deleted after interference"), Remaining.Entries.Num(), bFailCompletion ? 1 : 2);
        TestTrue(TEXT("Original stash retained"), Remaining.Entries.ContainsByPredicate([&](const auto& Entry) { return Entry.Oid == Oid && Entry.Label == TEXT("Recovery test"); }));
        if (!bFailCompletion) TestTrue(TEXT("Deletion failure explains Apply completed and must not repeat"), Result.Message.Contains(TEXT("Stash applied successfully")) && Result.Message.Contains(TEXT("Do not apply again")));
        FString CanonicalRoot, GitDir; GitWorkspaceSession::FindRepository(F.Root, CanonicalRoot, GitDir);
        TestEqual(TEXT("Recovery marker follows file verification outcome"), IFileManager::Get().FileExists(*GitWorkspaceSession::RecoveryFile(GitDir)), bFailCompletion);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitStashPanelTest, "GitWorkspace.Editor.StashPreviewThroughPanel", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGitStashPanelTest::RunTest(const FString&)
{
    FEditorAssetFixture F;
    const FString Path = FPaths::Combine(F.Root, TEXT("README.md"));
    FFileHelper::SaveStringToFile(TEXT("base"), *Path); F.Call({TEXT("add"), TEXT(".")}); F.Call({TEXT("commit"), TEXT("-qm"), TEXT("base")});
    FFileHelper::SaveStringToFile(TEXT("working"), *Path);
    FFileHelper::SaveStringToFile(TEXT("new saved file"), *FPaths::Combine(F.Root, TEXT("Content/New.uasset")));
    auto Repo = MakeShared<GitWorkspace::FRepository, ESPMode::ThreadSafe>(F.Git, F.Root);
    auto Panel = SNew(SGitWorkspace).Repository(Repo);
    auto Settle = [&]() { while (Panel->Pending.IsValid()) { Panel->Pending.Wait(); Panel->Tick(FGeometry(), 0, 0); } };
    Settle(); const auto Before = Repo->Refresh();
    const auto WorkingBefore = F.Bytes(TEXT("README.md"));
    Panel->ShowStashes(); Settle();
    TestTrue(TEXT("Empty stash list visible"), Panel->Stashes.bValid && Panel->Stashes.Entries.IsEmpty());
    TestFalse(TEXT("Empty selection cannot create a stash"), Panel->CanRunStashAction());
    TestFalse(TEXT("Delete not offered in Create mode"), Panel->CanRunStashAction(true));
    TestEqual(TEXT("Initial action is Create stash"), Panel->StashActionText().ToString(), FString(TEXT("Create stash…")));
    TestEqual(TEXT("Content checklist shows new file directly"), Panel->StashFileItems.Num(), 1);
    Panel->SetStashPathChecked(TEXT("Content/New.uasset"), true);
    TestTrue(TEXT("Untracked check requires opt-in"), Panel->StashSelectedPaths.IsEmpty());
    Panel->SetStashIncludeUntracked(true); Panel->SetStashPathChecked(TEXT("Content/New.uasset"), true);
    TestEqual(TEXT("Check files inside Stashes without selecting Changes rows"), Panel->StashSelectedPaths.Num(), 1);
    TestTrue(TEXT("Changed selection offers Create"), Panel->CanRunStashAction());
    TestTrue(TEXT("Missing name is explained visibly"), Panel->StashActionHint().ToString().Contains(TEXT("Enter a stash name")));
    Panel->RunStashAction(); Settle();
    TestTrue(TEXT("Create requests a name before preparation"), !Panel->StashReview.IsFresh() && Panel->Feedback.Contains(TEXT("Enter a name")));
    Panel->StashName->SetText(FText::FromString(TEXT("Panel stash")));
    Panel->PreviewStash(FString(), FString(), true); Settle();
    TestTrue(TEXT("Checklist drives exact new-file preview"), Panel->StashReview.IsFresh() && Panel->StashReview.UntrackedPaths == TArray<FString>{TEXT("Content/New.uasset")});
    TestTrue(TEXT("Fresh create action explains confirmation"), Panel->StashActionHint().ToString().Contains(TEXT("Yes/No")));
    Panel->StashReview.ReviewedSeconds -= 301;
    TestTrue(TEXT("Expired Create retains its action label"), Panel->CanRunStashAction() && Panel->StashActionText().ToString() == TEXT("Create stash…"));
    TestTrue(TEXT("Create explains automatic checks and confirmation"), Panel->StashActionHint().ToString().Contains(TEXT("checks the files")));
    Panel->PreviewStash(FString(), FString(), true); Settle();
    TestTrue(TEXT("Expired action refreshes only the selected scope"), Panel->StashReview.IsFresh() && Panel->StashReview.bSelected && Panel->StashReview.SelectedPaths == TArray<FString>{TEXT("Content/New.uasset")});
    TestTrue(TEXT("Refreshing action does not create a stash"), Repo->ListStashes().Entries.IsEmpty());
    Panel->StashFileFilter = TEXT("nothing matches"); Panel->RebuildStashFiles();
    TestTrue(TEXT("Filtering retains and discloses hidden checks"), Panel->StashSelectedPaths.Num() == 1 && Panel->StashSelectionSummary().ToString().Contains(TEXT("1 checked files hidden")));
    Panel->SetStashIncludeUntracked(false);
    TestTrue(TEXT("Disabling untracked clears ineligible checks"), Panel->StashSelectedPaths.IsEmpty());
    TestFalse(TEXT("Changing checks invalidates review"), Panel->StashReview.IsFresh());
    Panel->StashFileFilter.Empty(); Panel->bStashContentOnly = false; Panel->RebuildStashFiles();
    TestEqual(TEXT("Whole repo checklist exposes document and asset"), Panel->StashFileItems.Num(), 2);
    Panel->SetStashPathChecked(TEXT("README.md"), true);
    // Unattended dialogs explicitly default to No. Verify one Create click
    // reaches confirmation, while rejection leaves the checkout untouched.
    {
        TGuardValue<bool> Unattended(GIsRunningUnattendedScript, true);
        Panel->RunStashAction(); Settle();
        TestEqual(TEXT("One Create click prepares and reaches confirmation"), Panel->Feedback, FString(TEXT("Stash creation cancelled. No files changed.")));
        Panel->StashReview.ReviewedSeconds -= 301;
        Panel->Feedback.Empty(); Panel->RunStashAction(); Settle();
        TestEqual(TEXT("Expired Create refreshes and reaches confirmation in one click"), Panel->Feedback, FString(TEXT("Stash creation cancelled. No files changed.")));
    }
    TestTrue(TEXT("Declining confirmation creates no stash"), Repo->ListStashes().Entries.IsEmpty());
    TestTrue(TEXT("Declining confirmation keeps working bytes"), F.Bytes(TEXT("README.md")) == WorkingBefore);
    TestTrue(TEXT("Tracked selection works inside window"), Panel->StashReview.IsFresh() && Panel->StashReview.SelectedPaths == TArray<FString>{TEXT("README.md")});
    Panel->SetStashPathChecked(TEXT("README.md"), false);
    TestFalse(TEXT("Unchecking invalidates old Create preview"), Panel->StashReview.IsFresh());
    Panel->PreviewStash(FString()); Settle();
    TestTrue(TEXT("Panel offers reviewed creation: ") + Panel->Feedback, Panel->StashReview.IsFresh());
    Panel->StashReview.ReviewedSeconds -= 301;
    {
        TGuardValue<bool> Unattended(GIsRunningUnattendedScript, true);
        Panel->Feedback.Empty(); Panel->RunStashAction(); Settle();
        TestEqual(TEXT("Whole-repo Create reaches confirmation after refresh"), Panel->Feedback, FString(TEXT("Stash creation cancelled. No files changed.")));
    }
    TestTrue(TEXT("Explicit whole-repository review retains its scope on refresh"), Panel->StashReview.IsFresh() && !Panel->StashReview.bSelected);
    Panel->PreviewStash(FString()); Settle();
    TestTrue(TEXT("Review labels whole repository scope"), Panel->StashReport->GetText().ToString().Contains(TEXT("WHOLE REPOSITORY")));
    TestTrue(TEXT("Preview lists exact path"), Panel->StashReport->GetText().ToString().Contains(TEXT("README.md")));
    TestEqual(TEXT("Preview preserves HEAD"), Repo->Refresh().Head, Before.Head);
    TestTrue(TEXT("Preview preserves index"), Repo->Refresh().IndexEntries == Before.IndexEntries);
    TestTrue(TEXT("Preview does not publish a stash"), Repo->ListStashes().Entries.IsEmpty());
    // Capture real row selection when opening Stashes, including duplicate
    // staged/unstaged rows as a single whole-file selection.
    Panel->bContentOnly = false; Panel->RebuildRows();
    for (const auto& Row : Panel->Rows) if (Row->File.Path == TEXT("README.md")) Panel->List->SetItemSelection(Row, true);
    Panel->ShowStashes(); Settle();
    TestEqual(TEXT("Selected changed path captured on open"), Panel->StashSelectedPaths.Num(), 1);
    Panel->PreviewStash(FString(), FString(), true); Settle();
    TestTrue(TEXT("Panel offers selected creation"), Panel->StashReview.IsFresh() && Panel->StashReview.bSelected);
    TestTrue(TEXT("Selected scope clearly disclosed"), Panel->StashReport->GetText().ToString().Contains(TEXT("SELECTED FILES")));
    Panel->RefreshStashes(); Settle();
    TestFalse(TEXT("Refresh invalidates previous preview"), Panel->StashReview.IsFresh());
    F.Call({TEXT("stash"), TEXT("push"), TEXT("-m"), TEXT("saved work")});
    const FString Oid = F.Call({TEXT("rev-parse"), TEXT("refs/stash")}).Text().TrimEnd();
    FFileHelper::SaveStringToFile(TEXT("new local work"), *Path);
    Panel->RefreshStashes(); Settle(); Panel->PreviewStash(Oid, TEXT("stash@{0}")); Settle();
    TestTrue(TEXT("Panel inspects stash with dirty checkout"), Panel->StashInspection.IsFresh());
    TestFalse(TEXT("Panel Apply remains blocked"), Panel->StashReview.IsFresh());
    TestTrue(TEXT("Drop available independently"), Panel->StashInspection.DropBlocker.IsEmpty());
    const FString Report = Panel->StashReport->GetText().ToString();
    TestTrue(TEXT("Panel explains saved paths and Apply blocker together"), Report.Contains(TEXT("SAVED WORKING CHANGES")) && Report.Contains(TEXT("README.md")) && Report.Contains(TEXT("APPLY BLOCKED")));
    TestTrue(TEXT("Blocked Apply offers review and explains blocker"), Panel->CanRunStashAction() && Panel->StashActionHint().ToString().Contains(TEXT("blocker")));
    F.Call({TEXT("restore"), TEXT("--"), TEXT("README.md")});
    {
        TGuardValue<bool> Unattended(GIsRunningUnattendedScript, true);
        Panel->RunStashAction(); Settle();
        TestEqual(TEXT("Apply and keep refreshes and reaches confirmation"), Panel->Feedback, FString(TEXT("Apply and keep cancelled. No files changed; stash kept.")));
    }
    TestTrue(TEXT("Resolve overlap and review the exact same stash"), Panel->StashReview.IsFresh() && Panel->StashReview.Oid == Oid);
    TestTrue(TEXT("Pruning old Create checks preserves Apply mode and inspection"), Panel->StashSelectedPaths.IsEmpty() && !Panel->StashReview.bCreate && Panel->StashInspection.IsFresh());
    Panel->StashReview.ReviewedSeconds -= 301; Panel->StashInspection.ReviewedSeconds -= 301;
    TestTrue(TEXT("Expired Apply keeps stable labels and both actions"), Panel->CanRunStashAction() && Panel->CanRunStashAction(true) && Panel->StashActionText().ToString() == TEXT("Apply and keep…"));
    {
        TGuardValue<bool> Unattended(GIsRunningUnattendedScript, true);
        Panel->RunStashAction(true); Settle();
        TestEqual(TEXT("Expired Apply and delete preserves intent through confirmation"), Panel->Feedback, FString(TEXT("Apply and delete cancelled. No files changed; stash kept.")));
    }
    TestTrue(TEXT("Expired Apply refreshes inspection and review"), Panel->StashReview.IsFresh() && Panel->StashInspection.IsFresh());
    TestEqual(TEXT("Refreshing Apply does not apply saved work"), F.Call({TEXT("diff"), TEXT("--name-only")}).Text().TrimEnd(), FString());
    TestEqual(TEXT("Refreshing Apply keeps the saved stash"), Repo->ListStashes().Entries.Num(), 1);
    Panel->RefreshStashes(); Settle();
    TestFalse(TEXT("List refresh invalidates Drop inspection"), Panel->StashInspection.IsFresh());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitStashNewAssetTest, "GitWorkspace.Editor.StashNewLoadedAsset", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGitStashNewAssetTest::RunTest(const FString&)
{
    FEditorAssetFixture F;
    F.Call({TEXT("add"), TEXT(".")}); F.Call({TEXT("commit"), TEXT("-qm"), TEXT("new asset baseline")});
    const FString PackageName = F.Mount + TEXT("BP_New"), Path = FPaths::Combine(F.Root, TEXT("Content/BP_New.uasset"));
    TStrongObjectPtr<UBlueprint> Asset(FKismetEditorUtilities::CreateBlueprint(AActor::StaticClass(), F.Package(TEXT("BP_New")), TEXT("BP_New"), BPTYPE_Normal));
    Asset->BlueprintDescription = TEXT("Saved new asset acceptance");
    ON_SCOPE_EXIT { if (Asset.IsValid()) { Asset->GetPackage()->SetDirtyFlag(false); Asset->ClearFlags(RF_Public | RF_Standalone); } };
    FSavePackageArgs Args; Args.TopLevelFlags = RF_Public | RF_Standalone; Args.SaveFlags = SAVE_NoError;
    if (!TestTrue(TEXT("Save new Blueprint"), UPackage::SavePackage(Asset->GetPackage(), Asset.Get(), *Path, Args))) return false;
    UPackage::WaitForAsyncFileWrites();
    const auto Saved = F.Bytes(TEXT("Content/BP_New.uasset"));
    auto& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
    Registry.ScanFilesSynchronous({Path}, true);
    GitWorkspace::FRepository Repo(F.Git, F.Root); FString Error; GitWorkspaceSession::FLease Lease;
    if (!TestTrue(TEXT("New asset lease"), Lease.Acquire(F.Root, true, Error))) return false;
    auto Review = Repo.ReviewSelectedStash({TEXT("Content/BP_New.uasset")}, true);
    if (!TestTrue(TEXT("New asset preview: ") + Review.Error, Review.IsFresh())) return false;
    Asset->MarkPackageDirty();
    auto Result = GitWorkspace::StashAndReload(Repo, Review, TEXT("New Blueprint"), Lease);
    TestFalse(TEXT("Unsaved edits block removal before unload"), Result.bSuccess);
    TestTrue(TEXT("Unsaved object retained"), Asset.IsValid() && Asset->GetPackage()->IsDirty());
    TestTrue(TEXT("Saved bytes untouched"), F.Bytes(TEXT("Content/BP_New.uasset")) == Saved);
    Asset->GetPackage()->SetDirtyFlag(false);
    Result = GitWorkspace::StashAndReload(Repo, Review, TEXT("New Blueprint"), Lease);
    TestFalse(TEXT("Strong reference prevents removal"), Result.bSuccess);
    TestTrue(TEXT("Reference blocker explained"), Result.Message.Contains(TEXT("reference")));
    TestFalse(TEXT("No recovery for safe refusal"), Result.bRecoveryRequired);
    TestTrue(TEXT("Referenced asset file intact"), F.Bytes(TEXT("Content/BP_New.uasset")) == Saved);
    TestTrue(TEXT("No stash published on refusal"), Repo.ListStashes().Entries.IsEmpty());
    TWeakObjectPtr<UObject> Original = Asset.Get(); Asset.Reset();
    Review = Repo.ReviewSelectedStash({TEXT("Content/BP_New.uasset")}, true);
    Result = GitWorkspace::StashAndReload(Repo, Review, TEXT("New Blueprint"), Lease);
    if (!TestTrue(TEXT("Unload and stash new asset: ") + Result.Message, Result.bSuccess)) return false;
    TestEqual(TEXT("One new package unloaded"), Result.Unloaded, 1);
    TestFalse(TEXT("Original asset purged"), Original.IsValid());
    TestFalse(TEXT("Saved file removed only after unload"), IFileManager::Get().FileExists(*Path));
    TArray<FAssetData> Entries; Registry.GetAssetsByPackageName(*PackageName, Entries, true);
    TestTrue(TEXT("Removed asset absent from Content Browser registry"), Entries.IsEmpty());
    Result = GitWorkspace::StashAndReload(Repo, Repo.ReviewStash(Repo.ListStashes().Entries[0].Oid), FString(), Lease);
    if (!TestTrue(TEXT("Apply restores new asset: ") + Result.Message, Result.bSuccess)) return false;
    TestTrue(TEXT("Exact hydrated bytes restored"), F.Bytes(TEXT("Content/BP_New.uasset")) == Saved);
    Registry.GetAssetsByPackageName(*PackageName, Entries, true);
    TestFalse(TEXT("Restored asset registered"), Entries.IsEmpty());
    Asset.Reset(LoadObject<UBlueprint>(nullptr, *(PackageName + TEXT(".BP_New"))));
    if (!TestTrue(TEXT("Restored asset loads"), Asset.IsValid())) return false;
    TestEqual(TEXT("Saved Blueprint content restored"), Asset->BlueprintDescription, FString(TEXT("Saved new asset acceptance")));
    const auto After = Repo.Refresh();
    TestTrue(TEXT("Restored Blueprint remains untracked"), After.Files.Num() == 1 && After.Files[0].bUntracked);
    TestEqual(TEXT("Stash retained"), Repo.ListStashes().Entries.Num(), 1);
    FAssetRegistryModule::AssetDeleted(Asset.Get());
    return true;
}
#endif
