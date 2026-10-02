// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#if WITH_DEV_AUTOMATION_TESTS
#include "SGitWorkspace.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/PackageName.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
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
#include "AssetRegistry/AssetRegistryModule.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"

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
#endif
