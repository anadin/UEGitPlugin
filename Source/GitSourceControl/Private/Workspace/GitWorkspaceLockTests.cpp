// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#if WITH_DEV_AUTOMATION_TESTS
#include "GitWorkspaceRepository.h"
#include "GitWorkspaceSaveFlow.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "UObject/SavePackage.h"
#include "UObject/ObjectSaveContext.h"
#include "UObject/StrongObjectPtr.h"
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
#include "UObject/UnrealType.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "FileHelpers.h"
#include "Engine/Level.h"
#include "Engine/MapBuildDataRegistry.h"
#include "PrecomputedLightVolume.h"
#include "Engine/WorldComposition.h"
#include "WorldPartition/WorldPartition.h"
#include "WorldPartition/WorldPartitionActorDescUtils.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "GameFramework/WorldSettings.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionConstant3Vector.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialFunctionInstance.h"
#include "Materials/MaterialFunctionMaterialLayer.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionFunctionOutput.h"
#include "Materials/MaterialExpressionComment.h"
#include "MaterialGraph/MaterialGraphNode.h"
#include "MaterialEditor/MaterialEditorInstanceConstant.h"
#include "MaterialEditor/DEditorScalarParameterValue.h"
#include "MaterialGraph/MaterialGraph.h"
#include "MaterialEditorModule.h"
#include "IMaterialEditor.h"
#include "MaterialEditingLibrary.h"
#include "Framework/Commands/InputBindingManager.h"
#include "Framework/Commands/UICommandList.h"
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
    FLockFixture(bool bMapAttributes = false)
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
        Write(TEXT(".gitattributes"), FString(TEXT("*.uasset filter=lfs diff=lfs merge=lfs -text lockable\n")) + (bMapAttributes ? TEXT("*.umap filter=lfs diff=lfs merge=lfs -text lockable\n") : TEXT("")));
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
namespace
{
struct FMapSaveFixture
{
    FLockFixture Files{true};
    FString Content = FPaths::Combine(Files.Repo, TEXT("Content"));
    FString Mount = TEXT("/GitWorkspaceMap_") + FGuid::NewGuid().ToString(EGuidFormats::Digits) + TEXT("/");
    TArray<TStrongObjectPtr<UWorld>> Worlds;
    FMapSaveFixture()
    {
        // Git reports /private/var while UserTempDir may use the /var alias.
        // Keep the mount, destination checks and exclusive lease on one path.
        Files.Repo = Files.Call({TEXT("rev-parse"), TEXT("--show-toplevel")}).Text().TrimEnd(); Content = FPaths::Combine(Files.Repo, TEXT("Content"));
        IFileManager::Get().MakeDirectory(*Content, true); FPackageName::RegisterMountPoint(Mount, Content + TEXT("/")); GitWorkspaceSave::InstallGuard();
    }
    UWorld* NewWorld(bool bData = true)
    {
        const FString Name = TEXT("/Temp/Untitled_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
        UWorld* World = UWorld::CreateWorld(EWorldType::Inactive, false, FPackageName::GetShortFName(*Name), CreatePackage(*Name), false);
        World->SetFlags(RF_Public | RF_Standalone); World->MarkPackageDirty();
        if (bData) World->PersistentLevel->GetOrCreateMapBuildData()->MarkPackageDirty();
        Worlds.Emplace(World); return World;
    }
    void Keep(UWorld* World) { if (World) Worlds.Emplace(World); }
    void AddValidBuildData(UWorld* World)
    {
        auto* Data = World->PersistentLevel->GetOrCreateMapBuildData(); const FGuid Id = FGuid::NewGuid(); World->PersistentLevel->LevelBuildDataId = Id;
        auto& Volume = Data->AllocateLevelPrecomputedLightVolumeBuildData(Id); Volume.Initialize(FBox(FVector(-100), FVector(100)));
        FVolumeLightingSample Sample; Sample.Position = FVector3f(1, 2, 3); Sample.Radius = 10; Volume.AddHighQualityLightingSample(Sample); Volume.FinalizeSamples();
        Data->LevelLightingQuality = Quality_Production; Data->MarkPackageDirty(); World->MarkPackageDirty();
    }
    UWorld* ExistingActorWorld(bool bPartitioned, TArray<AActor*>& Actors, bool bData = false)
    {
        const FString Name = Mount + TEXT("L_Actors");
        UWorld::InitializationValues Init; Init.CreateWorldPartition(bPartitioned).EnableWorldPartitionStreaming(false).CreateNavigation(false).CreateAISystem(false);
        UWorld* World = UWorld::CreateWorld(EWorldType::Inactive, false, FPackageName::GetShortFName(*Name), CreatePackage(*Name), false, ERHIFeatureLevel::Num, &Init);
        World->SetFlags(RF_Public | RF_Standalone); World->GetPackage()->ThisContainsMap(); Worlds.Emplace(World);
        World->PersistentLevel->SetUseExternalActors(true);
        for (int32 I = 0; I < 2; ++I)
        {
            FActorSpawnParameters Params; Params.Name = FName(*FString::Printf(TEXT("FixtureActor%d"), I)); Params.OverrideLevel = World->PersistentLevel; Params.bCreateActorPackage = false;
            AActor* Actor = World->SpawnActor<AActor>(AActor::StaticClass(), FTransform(FVector(I * 100.f, 0, 0)), Params);
            Actor->SetActorLabel(FString::Printf(TEXT("BaselineActor%d"), I)); Actor->SetPackageExternal(true); Actors.Add(Actor);
        }
        // Bootstrap real fixtures in an isolated mount outside game Content.
        // This does not call the production first-save flow or acquire hosted locks.
        FSavePackageArgs Args; Args.TopLevelFlags = RF_Standalone; Args.SaveFlags = SAVE_NoError;
        if (bData)
        {
            AddValidBuildData(World); auto* Data = World->PersistentLevel->MapBuildData.Get(); FString Filename;
            FPackageName::TryConvertLongPackageNameToFilename(Data->GetPackage()->GetName(), Filename, TEXT(".uasset"));
            if (!UPackage::SavePackage(Data->GetPackage(), Data, *Filename, Args)) return nullptr;
        }
        for (UPackage* Package : World->PersistentLevel->GetLoadedExternalObjectPackages())
        {
            FString Filename; FPackageName::TryConvertLongPackageNameToFilename(Package->GetName(), Filename, TEXT(".uasset"));
            if (!UPackage::SavePackage(Package, nullptr, *Filename, Args)) return nullptr;
        }
        FString Map; FPackageName::TryConvertLongPackageNameToFilename(Name, Map, TEXT(".umap"));
        if (!UPackage::SavePackage(World->GetPackage(), World, *Map, Args)) return nullptr;
        UPackage::WaitForAsyncFileWrites();
        if (!Files.Call({TEXT("add"), TEXT("Content")}).Ok() || !Files.Call({TEXT("commit"), TEXT("-qm"), TEXT("real external actor baseline")}).Ok()) return nullptr;
        return World;
    }
    AActor* NewActor(UWorld* World, const FName& Name, const FString& Label)
    {
        FActorSpawnParameters Params; Params.Name = Name; Params.OverrideLevel = World->PersistentLevel; Params.bCreateActorPackage = false;
        AActor* Actor = World->SpawnActor<AActor>(AActor::StaticClass(), FTransform(FVector(200, 30, 40)), Params);
        Actor->SetActorLabel(Label); Actor->SetPackageExternal(true); return Actor;
    }
    void Export(const FString& Folder)
    {
        IFileManager::Get().DeleteDirectory(*Folder, false, true); IFileManager::Get().MakeDirectory(*Folder, true);
        FFileHelper::SaveStringToFile(Mount, *FPaths::Combine(Folder, TEXT("mount.txt")));
        TArray<FString> FilesToCopy; IFileManager::Get().FindFilesRecursive(FilesToCopy, *Content, TEXT("*"), true, false);
        for (const auto& File : FilesToCopy)
        {
            FString Relative = File; FPaths::MakePathRelativeTo(Relative, *(Content + TEXT("/")));
            const FString Target = FPaths::Combine(Folder, TEXT("Content"), Relative); IFileManager::Get().MakeDirectory(*FPaths::GetPath(Target), true);
            IFileManager::Get().Copy(*Target, *File);
        }
    }
    ~FMapSaveFixture()
    {
        for (auto& World : Worlds) if (World)
        {
            for (UPackage* Package : World->PersistentLevel->GetLoadedExternalObjectPackages())
                if (Package) Package->SetDirtyFlag(false);
            if (auto* Data = World->PersistentLevel->MapBuildData.Get())
            { FAssetRegistryModule::AssetDeleted(Data); Data->GetPackage()->SetDirtyFlag(false); Data->ClearFlags(RF_Standalone); }
            FAssetRegistryModule::AssetDeleted(World.Get()); World->GetPackage()->SetDirtyFlag(false); World->ClearFlags(RF_Standalone); World->DestroyWorld(false);
        }
        Worlds.Empty(); GitWorkspaceSave::RemoveGuard(); FPackageName::UnRegisterMountPoint(Mount, Content + TEXT("/"));
    }
};
class FMapFixtureCleanup : public IAutomationLatentCommand
{
    TSharedPtr<FMapSaveFixture> Fixture; int32 Ticks = 0;
public:
    explicit FMapFixtureCleanup(TSharedPtr<FMapSaveFixture> InFixture) : Fixture(MoveTemp(InFixture)) {}
    virtual bool Update() override
    {
        // Save validation runs on the next editor tick. Keep both the mount
        // and real assets alive through that callback; do not disable validators.
        if (++Ticks < 3) return false;
        FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get().WaitForCompletion();
        Fixture.Reset(); return true;
    }
};
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitExistingActorsTest, "GitWorkspace.SaveLock.ExternalActorExistingWrites", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGitExistingActorsTest::RunTest(const FString&)
{
    for (bool bPartitioned : {false, true})
    {
        auto Lifetime = MakeShared<FMapSaveFixture>(); auto& F = *Lifetime; auto& Files = F.Files;
        ADD_LATENT_AUTOMATION_COMMAND(FMapFixtureCleanup(Lifetime)); TArray<AActor*> Actors;
        UWorld* World = F.ExistingActorWorld(bPartitioned, Actors);
        if (!TestNotNull(bPartitioned ? TEXT("Real World Partition fixture") : TEXT("Real OFPA fixture"), World)) return false;
        TestEqual(TEXT("Fixture partition mode"), World->GetWorldPartition() != nullptr, bPartitioned);
        GitWorkspace::FRepository Repo(Files.Git, Files.Repo); const auto Before = Repo.Refresh(); const FString Stashes = Repo.ListStashes().Fingerprint;
        FString Map; FPackageName::TryConvertLongPackageNameToFilename(World->GetPackage()->GetName(), Map, TEXT(".umap"));
        const FString MapBytes = Files.Call({TEXT("hash-object"), TEXT("--no-filters"), Map}).Text();
        Actors[0]->Modify(); Actors[0]->SetActorLabel(TEXT("Edited external actor"));
        const auto Plan = GitWorkspaceSave::GatherPackageSavePaths({World->GetPackage()}, Files.Repo, F.Content);
        if (!TestTrue(TEXT("Only one dirty actor is reviewed: ") + Plan.Error, Plan.Error.IsEmpty() && Plan.Paths.Num() == 1 && Plan.ExternalActorPaths == Plan.Paths && Plan.NewPaths.IsEmpty())) return false;
        TestTrue(TEXT("Clean owning map stays clean"), !World->GetPackage()->IsDirty());
        const auto OnlyActor = GitWorkspaceSave::GatherPackageSavePaths({Actors[0]->GetPackage()}, Files.Repo, F.Content);
        TestEqual(TEXT("Save All can find owner from the actor package alone"), OnlyActor.Paths, Plan.Paths);
        TestFalse(TEXT("Uncoordinated service route refuses external actor"), Repo.ReviewAssetSave(Plan.Paths, TEXT("origin")).IsFresh());
        auto Review = Repo.ReviewAssetSave(Plan.Paths, TEXT("origin"), {}, Plan.ExternalActorPaths);
        if (!TestTrue(TEXT("Coordinated actor lock review: ") + Review.Error, Review.IsFresh())) return false;
        GitWorkspaceSession::FLease Lease; FString Error; if (!TestTrue(TEXT("Actor-save lease"), Lease.Acquire(Files.Repo, true, Error))) return false;
        const auto Cancelled = Repo.PrepareAssetSave(Review, Lease, false);
        TestFalse(TEXT("Cancelled lock review acquires nothing"), Cancelled.Result.Ok()); TestEqual(TEXT("Cancel preserves map bytes"), Files.Call({TEXT("hash-object"), TEXT("--no-filters"), Map}).Text(), MapBytes);
        auto Prepared = Repo.PrepareAssetSave(Review, Lease, true);
        if (!TestTrue(TEXT("Actor locks prepared: ") + Prepared.Result.Error, Prepared.Result.Ok() && Prepared.Permit)) return false;
        const auto Result = GitWorkspaceSave::WriteExternalActorSave(Plan, Repo, *Prepared.Permit, Lease, F.Content);
        if (!TestTrue(TEXT("Existing actor write: ") + Result.Error, Result.Ok())) return false;
        TestFalse(TEXT("Written actor becomes clean"), Actors[0]->GetPackage()->IsDirty());
        TestEqual(TEXT("Clean map bytes unchanged"), Files.Call({TEXT("hash-object"), TEXT("--no-filters"), Map}).Text(), MapBytes);
        TestEqual(TEXT("Index snapshot unchanged"), Repo.Refresh().IndexEntries, Before.IndexEntries);
        TestEqual(TEXT("HEAD unchanged"), Repo.Refresh().Head, Before.Head); TestEqual(TEXT("Stashes unchanged"), Repo.ListStashes().Fingerprint, Stashes);
        const auto Locks = Repo.VerifyLocks(TEXT("origin")); TestTrue(TEXT("Only edited actor locked, map and other actor unlocked"), Locks.IsFresh() && Locks.Locks.Num() == 1 && Locks.Locks.Contains(Plan.Paths[0]));
        // Read freshly scanned, on-disk descriptor data rather than the live actor.
        auto& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
        Registry.ScanFilesSynchronous({Plan.Entries[0].Filename}, true);
        TArray<FAssetData> DiskAssets; Registry.GetAssetsByPackageName(FName(*Plan.Entries[0].PackageName), DiskAssets, true);
        bool bDescriptor = false;
        for (const auto& Asset : DiskAssets)
            if (auto Desc = FWorldPartitionActorDescUtils::GetActorDescriptorFromAssetData(Asset))
            { bDescriptor = Desc->GetGuid() == Actors[0]->GetActorGuid() && Desc->GetActorLabel() == FName(TEXT("Edited external actor")); }
        TestTrue(TEXT("Saved package contains edited actor descriptor and original GUID"), bDescriptor);
        if (!bDescriptor) return false;
        // Keep disposable copies for independent native-editor load/cancel checks.
        const FString Export = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Automation/ExternalActorFixtures"), bPartitioned ? TEXT("WorldPartition") : TEXT("OFPA"));
        IFileManager::Get().DeleteDirectory(*Export, false, true); IFileManager::Get().MakeDirectory(*Export, true);
        FFileHelper::SaveStringToFile(F.Mount, *FPaths::Combine(Export, TEXT("mount.txt")));
        TArray<FString> SavedFiles; IFileManager::Get().FindFilesRecursive(SavedFiles, *F.Content, TEXT("*"), true, false);
        for (const FString& File : SavedFiles)
        {
            FString Relative = File; FPaths::MakePathRelativeTo(Relative, *(F.Content + TEXT("/")));
            const FString Target = FPaths::Combine(Export, TEXT("Content"), Relative);
            IFileManager::Get().MakeDirectory(*FPaths::GetPath(Target), true);
            TestEqual(TEXT("Export disposable actor fixture"), IFileManager::Get().Copy(*Target, *File), COPY_OK);
        }
        Actors[1]->Modify(); Actors[1]->SetActorLabel(TEXT("Mixed batch actor"));
        UPackage* TexturePackage = CreatePackage(*(F.Mount + TEXT("A_Texture")));
        TStrongObjectPtr<UTexture2D> Texture(NewObject<UTexture2D>(TexturePackage, TEXT("A_Texture"), RF_Public | RF_Standalone));
        uint8 Pixels[16] = {}; Texture->Source.Init(2, 2, 1, 1, TSF_BGRA8, Pixels); Texture->MarkPackageDirty();
        const auto Mixed = GitWorkspaceSave::GatherPackageSavePaths({World->GetPackage(), TexturePackage}, Files.Repo, F.Content);
        if (!TestTrue(TEXT("Save All includes actor and new ordinary asset"), Mixed.Error.IsEmpty() && Mixed.Paths.Num() == 2 && Mixed.NewPaths.Num() == 1)) return false;
        Prepared = Repo.PrepareAssetSave(Repo.ReviewAssetSave(Mixed.Paths, TEXT("origin"), Mixed.NewPaths, Mixed.ExternalActorPaths), Lease, true);
        if (!TestTrue(TEXT("Mixed permits"), Prepared.Result.Ok() && Prepared.Permit)) return false;
        const auto MixedResult = GitWorkspaceSave::WriteExternalActorSave(Mixed, Repo, *Prepared.Permit, Lease, F.Content);
        TestTrue(TEXT("Fixed batch writes ordinary first-save and actor: ") + MixedResult.Error, MixedResult.Ok());
        Texture->ClearFlags(RF_Standalone); TexturePackage->SetDirtyFlag(false);
        TestEqual(TEXT("Mixed batch preserves index"), Repo.Refresh().IndexEntries, Before.IndexEntries);
        TestEqual(TEXT("Mixed batch still leaves map bytes unchanged"), Files.Call({TEXT("hash-object"), TEXT("--no-filters"), Map}).Text(), MapBytes);
    }
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitActorSaveBoundariesTest, "GitWorkspace.SaveLock.ExternalActorBoundariesAndDrift", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGitActorSaveBoundariesTest::RunTest(const FString&)
{
    auto Lifetime = MakeShared<FMapSaveFixture>(); auto& F = *Lifetime; auto& Files = F.Files; ADD_LATENT_AUTOMATION_COMMAND(FMapFixtureCleanup(Lifetime));
    TArray<AActor*> Actors; UWorld* World = F.ExistingActorWorld(true, Actors); if (!TestNotNull(TEXT("Partitioned boundary fixture"), World)) return false;
    Actors[0]->Modify(); Actors[0]->SetActorLabel(TEXT("Boundary edit"));
    const auto Plan = GitWorkspaceSave::GatherPackageSavePaths({World->GetPackage()}, Files.Repo, F.Content);
    if (!TestTrue(TEXT("Boundary plan: ") + Plan.Error, Plan.Error.IsEmpty() && Plan.Paths.Num() == 1)) return false;
    GitWorkspace::FRepository Repo(Files.Git, Files.Repo); const auto Before = Repo.Refresh();
    const FString Bytes = Files.Call({TEXT("hash-object"), TEXT("--no-filters"), Plan.Entries[0].Filename}).Text();
    GitWorkspaceSession::FLease Lease; FString Error; if (!TestTrue(TEXT("Boundary lease"), Lease.Acquire(Files.Repo, true, Error))) return false;
    auto Prepared = Repo.PrepareAssetSave(Repo.ReviewAssetSave(Plan.Paths, TEXT("origin"), {}, Plan.ExternalActorPaths), Lease, true);
    if (!TestTrue(TEXT("Boundary locks"), Prepared.Result.Ok() && Prepared.Permit)) return false;
    TestFalse(TEXT("Exact package substitution refused"), GitWorkspaceSave::ValidateExternalActorBinding(Plan, Plan.Paths[0], Actors[1]->GetPackage()).IsEmpty());
    {
        GitWorkspaceSave::FPreparedScope MissingPlan(Repo, *Prepared.Permit, Lease, Files.Repo);
        FSavePackageArgs Args; Args.TopLevelFlags = RF_Standalone; Args.SaveFlags = SAVE_NoError;
        TestFalse(TEXT("A worker permit alone cannot authorize external writes"), UPackage::SavePackage(Actors[0]->GetPackage(), nullptr, *Plan.Entries[0].Filename, Args));
    }
    Actors[1]->Modify(); Actors[1]->SetActorLabel(TEXT("Discovered after review"));
    TestFalse(TEXT("Additional dirty actor refuses the old plan"), GitWorkspaceSave::WriteExternalActorSave(Plan, Repo, *Prepared.Permit, Lease, F.Content).Ok());
    Actors[1]->GetPackage()->SetDirtyFlag(false);
    bool bCallbackWrite = false;
    const auto Callback = FEditorDelegates::PreSaveExternalActors.AddLambda([&](UWorld* W)
    {
        if (W != World) return;
        FSavePackageArgs Args; Args.TopLevelFlags = RF_Standalone; Args.SaveFlags = SAVE_NoError;
        bCallbackWrite = UPackage::SavePackage(Actors[0]->GetPackage(), nullptr, *Plan.Entries[0].Filename, Args);
        World->GetPackage()->SetDirtyFlag(true);
    });
    const auto CallbackResult = GitWorkspaceSave::WriteExternalActorSave(Plan, Repo, *Prepared.Permit, Lease, F.Content);
    FEditorDelegates::PreSaveExternalActors.Remove(Callback); World->GetPackage()->SetDirtyFlag(false);
    TestFalse(TEXT("World callback drift refuses before writing"), CallbackResult.Ok());
    TestFalse(TEXT("Pre-save callback cannot write even a reviewed actor"), bCallbackWrite);
    World->GetPackage()->SetDirtyFlag(true);
    const auto DirtyMap = GitWorkspaceSave::GatherPackageSavePaths({World->GetPackage()}, Files.Repo, F.Content);
    TestTrue(TEXT("Dirty map joins the reviewed actor set"), DirtyMap.Error.IsEmpty() && DirtyMap.Paths.Num() == 2); World->GetPackage()->SetDirtyFlag(false);
    World->PersistentLevel->GetOrCreateMapBuildData()->MarkPackageDirty();
    const auto NewData = GitWorkspaceSave::GatherPackageSavePaths({World->GetPackage()}, Files.Repo, F.Content);
    TestTrue(TEXT("New build data includes its existing map"), NewData.Error.IsEmpty() && NewData.Paths.Num() == 3 && NewData.NewPaths.Num() == 1); World->PersistentLevel->MapBuildData->GetPackage()->SetDirtyFlag(false);
    FActorSpawnParameters Params; Params.OverrideLevel = World->PersistentLevel;
    AActor* NewActor = World->SpawnActor<AActor>(AActor::StaticClass(), FTransform::Identity, Params); NewActor->SetPackageExternal(true); World->GetPackage()->SetDirtyFlag(false);
    const auto WithNewActor = GitWorkspaceSave::GatherPackageSavePaths({World->GetPackage()}, Files.Repo, F.Content);
    TestTrue(TEXT("New actor joins the explicit first-save set"), WithNewActor.Error.IsEmpty() && WithNewActor.NewPaths.Num() == 2 && WithNewActor.ExternalActorPaths.Num() == 2);
    World->DestroyActor(NewActor); World->GetPackage()->SetDirtyFlag(false);
    Actors[0]->SetPackageExternal(false); World->GetPackage()->SetDirtyFlag(false);
    TestFalse(TEXT("Deleted/empty external packages remain blocked"), GitWorkspaceSave::GatherPackageSavePaths({World->GetPackage()}, Files.Repo, F.Content).Error.IsEmpty());
    TestEqual(TEXT("All refusals preserve actor bytes"), Files.Call({TEXT("hash-object"), TEXT("--no-filters"), Plan.Entries[0].Filename}).Text(), Bytes);
    TestEqual(TEXT("All refusals preserve index"), Repo.Refresh().IndexEntries, Before.IndexEntries);
    TestTrue(TEXT("The acquired lock remains held"), Repo.VerifyLocks(TEXT("origin")).Locks.Contains(Plan.Paths[0]));
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitActorPartialTest, "GitWorkspace.SaveLock.ExternalActorPartialFailures", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGitActorPartialTest::RunTest(const FString&)
{
    auto Lifetime = MakeShared<FMapSaveFixture>(); auto& F = *Lifetime; auto& Files = F.Files; ADD_LATENT_AUTOMATION_COMMAND(FMapFixtureCleanup(Lifetime));
    TArray<AActor*> Actors; UWorld* World = F.ExistingActorWorld(false, Actors); if (!TestNotNull(TEXT("OFPA partial fixture"), World)) return false;
    for (AActor* Actor : Actors) { Actor->Modify(); Actor->SetActorLabel(TEXT("Partial save edit")); }
    const auto Plan = GitWorkspaceSave::GatherPackageSavePaths({World->GetPackage()}, Files.Repo, F.Content);
    if (!TestTrue(TEXT("Two-actor plan: ") + Plan.Error, Plan.Error.IsEmpty() && Plan.Paths.Num() == 2)) return false;
    GitWorkspace::FRepository Repo(Files.Git, Files.Repo); const auto Before = Repo.Refresh(); const FString Stashes = Repo.ListStashes().Fingerprint;
    const FString FirstBytes = Files.Call({TEXT("hash-object"), TEXT("--no-filters"), Plan.Entries[0].Filename}).Text(), LastBytes = Files.Call({TEXT("hash-object"), TEXT("--no-filters"), Plan.Entries[1].Filename}).Text();
    GitWorkspaceSession::FLease Lease; FString Error; if (!TestTrue(TEXT("Partial lease"), Lease.Acquire(Files.Repo, true, Error))) return false;
    FFileHelper::SaveStringToFile(Plan.Paths[1], *FPaths::Combine(Files.Root, TEXT("fail-lock-path")));
    auto Prepared = Repo.PrepareAssetSave(Repo.ReviewAssetSave(Plan.Paths, TEXT("origin"), {}, Plan.ExternalActorPaths), Lease, true);
    TestTrue(TEXT("Partial lock acquisition retains first lock"), !Prepared.Result.Ok() && Prepared.AcquiredPaths == TArray<FString>{Plan.Paths[0]});
    TestEqual(TEXT("Partial acquisition writes nothing"), Files.Call({TEXT("hash-object"), TEXT("--no-filters"), Plan.Entries[0].Filename}).Text(), FirstBytes);
    IFileManager::Get().Delete(*FPaths::Combine(Files.Root, TEXT("fail-lock-path")));
    Prepared = Repo.PrepareAssetSave(Repo.ReviewAssetSave(Plan.Paths, TEXT("origin"), {}, Plan.ExternalActorPaths), Lease, true);
    if (!TestTrue(TEXT("Retry prepares both locks"), Prepared.Result.Ok() && Prepared.Permit)) return false;
    GitWorkspaceSave::RemoveGuard(); const auto Previous = FCoreUObjectDelegates::IsPackageOKToSaveDelegate;
    FCoreUObjectDelegates::IsPackageOKToSaveDelegate.BindLambda([Last = Plan.Entries[1].Filename](UPackage*, const FString& File, FOutputDevice*) { return FPaths::ConvertRelativePathToFull(File) != FPaths::ConvertRelativePathToFull(Last); });
    GitWorkspaceSave::InstallGuard();
    const auto Partial = GitWorkspaceSave::WriteExternalActorSave(Plan, Repo, *Prepared.Permit, Lease, F.Content);
    GitWorkspaceSave::RemoveGuard(); FCoreUObjectDelegates::IsPackageOKToSaveDelegate = Previous; GitWorkspaceSave::InstallGuard();
    TestFalse(TEXT("Second write failure is reported"), Partial.Ok());
    TestTrue(TEXT("Report distinguishes completed and remaining paths"), Partial.Error.Contains(TEXT("Saved: ") + Plan.Paths[0]) && Partial.Error.Contains(TEXT("Not completed: ") + Plan.Paths[1]));
    TestNotEqual(TEXT("First completed write is retained"), Files.Call({TEXT("hash-object"), TEXT("--no-filters"), Plan.Entries[0].Filename}).Text(), FirstBytes);
    TestEqual(TEXT("Failed second write preserves bytes"), Files.Call({TEXT("hash-object"), TEXT("--no-filters"), Plan.Entries[1].Filename}).Text(), LastBytes);
    TestTrue(TEXT("Failed actor stays dirty"), Plan.Entries[1].Package->IsDirty());
    TestTrue(TEXT("Both actor locks remain held"), Repo.VerifyLocks(TEXT("origin")).Locks.Num() == 2);
    TestEqual(TEXT("Partial write preserves index"), Repo.Refresh().IndexEntries, Before.IndexEntries);
    TestEqual(TEXT("Partial write preserves HEAD"), Repo.Refresh().Head, Before.Head); TestEqual(TEXT("Partial write preserves stashes"), Repo.ListStashes().Fingerprint, Stashes);
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitNewExternalActorTest, "GitWorkspace.SaveLock.ExternalActorFirstWrites", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGitNewExternalActorTest::RunTest(const FString&)
{
    for (int32 Case = 0; Case < 3; ++Case)
    {
        const bool bPartitioned = Case != 0, bMixed = Case == 2;
        auto Lifetime = MakeShared<FMapSaveFixture>(); auto& F = *Lifetime; auto& Files = F.Files; ADD_LATENT_AUTOMATION_COMMAND(FMapFixtureCleanup(Lifetime));
        TArray<AActor*> Actors; UWorld* World = F.ExistingActorWorld(bPartitioned, Actors, bMixed); if (!TestNotNull(TEXT("New-actor owner"), World)) return false;
        GitWorkspace::FRepository Repo(Files.Git, Files.Repo); const auto Before = Repo.Refresh(); const FString Stashes = Repo.ListStashes().Fingerprint;
        AActor* Actor = F.NewActor(World, TEXT("FirstSavedActor"), TEXT("First saved actor")); UPackage* Package = Actor->GetPackage(); const FGuid Guid = Actor->GetActorGuid();
        World->GetPackage()->SetDirtyFlag(bMixed);
        if (bMixed) { Actors[0]->Modify(); Actors[0]->SetActorLabel(TEXT("Existing actor with new companion")); World->PersistentLevel->MapBuildData->MarkPackageDirty(); }
        // Unreal includes newly created external packages even if not dirty.
        Package->SetDirtyFlag(false);
        const auto Plan = GitWorkspaceSave::GatherPackageSavePaths({Package}, Files.Repo, F.Content);
        if (!TestTrue(TEXT("New actor destination and exact mixed save set: ") + Plan.Error, Plan.Error.IsEmpty() && Plan.NewPaths.Num() == 1 && Plan.Paths.Num() == (bMixed ? 4 : 1) && Plan.ExternalActorPaths.Num() == (bMixed ? 2 : 1))) return false;
        const FString Path = Plan.NewPaths[0]; const auto* Entry = Plan.Entries.FindByPredicate([&](const auto& E) { return E.Path == Path; });
        const FString Full = Entry->Filename, Map = Plan.Owners[0].Filename;
        const FString MapBytes = Files.Call({TEXT("hash-object"), TEXT("--no-filters"), Map}).Text();
        TestFalse(TEXT("Review creates no actor file"), IFileManager::Get().FileExists(*Full));
        TestEqual(TEXT("Save Current Level resolves same new actor"), GitWorkspaceSave::GatherPackageSavePaths({World->GetPackage()}, Files.Repo, F.Content).Paths, Plan.Paths);
        TestFalse(TEXT("New actor without coordinated opt-in is refused"), Repo.ReviewAssetSave(Plan.Paths, TEXT("origin"), Plan.NewPaths).IsFresh());
        const auto Review = Repo.ReviewAssetSave(Plan.Paths, TEXT("origin"), Plan.NewPaths, Plan.ExternalActorPaths);
        if (!TestTrue(TEXT("First-save review retains external authorization: ") + Review.Error, Review.IsFresh() && Review.NewPaths.Contains(Path) && Review.ExternalActorPaths.Contains(Path) && Review.Text().Contains(TEXT("First save")))) return false;
        GitWorkspaceSession::FLease Lease; FString Error; if (!TestTrue(TEXT("New actor lease"), Lease.Acquire(Files.Repo, true, Error))) return false;
        TestFalse(TEXT("Cancellation acquires no lock"), Repo.PrepareAssetSave(Review, Lease, false).Result.Ok());
        TestTrue(TEXT("Cancellation leaves new actor loaded and newly created"), Actor->GetActorGuid() == Guid && Package->HasAnyPackageFlags(PKG_NewlyCreated) && !IFileManager::Get().FileExists(*Full) && Repo.VerifyLocks(TEXT("origin")).Locks.IsEmpty());
        auto Prepared = Repo.PrepareAssetSave(Review, Lease, true);
        if (!TestTrue(TEXT("First-save reservations: ") + Prepared.Result.Error, Prepared.Result.Ok() && Prepared.Permit && Prepared.Permit->ContainsNewPath(Path) && Prepared.Permit->ContainsExternalActorPath(Path))) return false;
        const auto Result = GitWorkspaceSave::WriteExternalActorSave(Plan, Repo, *Prepared.Permit, Lease, F.Content);
        if (!TestTrue(TEXT("New actor saves: ") + Result.Error, Result.Ok())) return false;
        TestTrue(TEXT("Written actor becomes an existing clean package"), IFileManager::Get().FileExists(*Full) && !Package->IsDirty() && !Package->HasAnyPackageFlags(PKG_NewlyCreated));
        if (!bMixed) TestEqual(TEXT("Clean map bytes stay unchanged"), Files.Call({TEXT("hash-object"), TEXT("--no-filters"), Map}).Text(), MapBytes);
        const auto Locks = Repo.VerifyLocks(TEXT("origin")); TestTrue(TEXT("Only reviewed paths remain owned"), Locks.IsFresh() && Locks.Locks.Num() == Plan.Paths.Num() && !Plan.Paths.ContainsByPredicate([&](const auto& P) { return !Locks.Locks.Contains(P) || !Locks.Locks[P].bOurs; }));
        auto& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get(); Registry.ScanFilesSynchronous({Full}, true);
        TArray<FAssetData> DiskAssets; Registry.GetAssetsByPackageName(FName(*Package->GetName()), DiskAssets, true); bool bDescriptor = false;
        for (const auto& Asset : DiskAssets) if (auto Desc = FWorldPartitionActorDescUtils::GetActorDescriptorFromAssetData(Asset)) bDescriptor |= Desc->GetGuid() == Guid && Desc->GetActorLabel() == FName(TEXT("First saved actor"));
        TestTrue(TEXT("First file contains readable actor descriptor and stable GUID"), bDescriptor);
        Actor->Modify(); Actor->SetActorLabel(TEXT("First saved actor edit"));
        const auto Next = GitWorkspaceSave::GatherPackageSavePaths({Package}, Files.Repo, F.Content);
        TestTrue(TEXT("Following edit is an existing-file save"), Next.Error.IsEmpty() && Next.Paths == TArray<FString>{Path} && Next.NewPaths.IsEmpty());
        const auto NextReview = Repo.ReviewAssetSave(Next.Paths, TEXT("origin"), Next.NewPaths, Next.ExternalActorPaths);
        TestTrue(TEXT("Following save keeps the existing lock"), NextReview.IsFresh() && NextReview.NeedsLock.IsEmpty());
        Prepared = Repo.PrepareAssetSave(NextReview, Lease, false); if (!TestTrue(TEXT("Existing lock reused"), Prepared.Result.Ok() && Prepared.Permit)) return false;
        TestTrue(TEXT("Following actor edit saves"), GitWorkspaceSave::WriteExternalActorSave(Next, Repo, *Prepared.Permit, Lease, F.Content).Ok());
        TestEqual(TEXT("Actor GUID unchanged across first and subsequent writes"), Actor->GetActorGuid(), Guid);
        TestEqual(TEXT("First saves leave staging unchanged"), Repo.Refresh().IndexEntries, Before.IndexEntries); TestEqual(TEXT("First saves leave HEAD unchanged"), Repo.Refresh().Head, Before.Head); TestEqual(TEXT("First saves leave stashes unchanged"), Repo.ListStashes().Fingerprint, Stashes);
        if (!bMixed) F.Export(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Automation/ExternalNewActorFixtures"), bPartitioned ? TEXT("WorldPartition") : TEXT("OFPA")));
    }
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitNewExternalActorBoundaryTest, "GitWorkspace.SaveLock.ExternalActorFirstSaveBoundaries", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGitNewExternalActorBoundaryTest::RunTest(const FString&)
{
    auto Lifetime = MakeShared<FMapSaveFixture>(); auto& F = *Lifetime; auto& Files = F.Files; ADD_LATENT_AUTOMATION_COMMAND(FMapFixtureCleanup(Lifetime));
    TArray<AActor*> Actors; UWorld* World = F.ExistingActorWorld(true, Actors); if (!TestNotNull(TEXT("New actor boundary owner"), World)) return false;
    AActor* Actor = F.NewActor(World, TEXT("BoundaryNewActor"), TEXT("Boundary new actor")); World->GetPackage()->SetDirtyFlag(false); UPackage* Package = Actor->GetPackage();
    const auto Plan = GitWorkspaceSave::GatherPackageSavePaths({Package}, Files.Repo, F.Content); if (!TestTrue(TEXT("Boundary new plan: ") + Plan.Error, Plan.Error.IsEmpty() && Plan.Paths.Num() == 1)) return false;
    const FString Path = Plan.Paths[0], Full = Plan.Entries[0].Filename, Map = Plan.Owners[0].Filename;
    GitWorkspace::FRepository Repo(Files.Git, Files.Repo); const auto Before = Repo.Refresh(); const FString Stashes = Repo.ListStashes().Fingerprint, MapBytes = Files.Call({TEXT("hash-object"), TEXT("--no-filters"), Map}).Text();
    GitWorkspaceSession::FLease Lease; FString Error; if (!TestTrue(TEXT("New boundary lease"), Lease.Acquire(Files.Repo, true, Error))) return false;
    auto Prepared = Repo.PrepareAssetSave(Repo.ReviewAssetSave(Plan.Paths, TEXT("origin"), Plan.NewPaths, Plan.ExternalActorPaths), Lease, true);
    if (!TestTrue(TEXT("New boundary reservation"), Prepared.Result.Ok() && Prepared.Permit)) return false;
    {
        GitWorkspaceSave::FPreparedScope NoPlan(Repo, *Prepared.Permit, Lease, Files.Repo); FSavePackageArgs Args; Args.TopLevelFlags = RF_Standalone; Args.SaveFlags = SAVE_NoError;
        TestFalse(TEXT("An absent external-file permit alone cannot write"), UPackage::SavePackage(Package, nullptr, *Full, Args));
    }
    auto* GuidProperty = FindFProperty<FStructProperty>(AActor::StaticClass(), TEXT("ActorGuid")); if (!TestNotNull(TEXT("Reflected fixture actor GUID"), GuidProperty)) return false;
    FGuid* ReflectedGuid = GuidProperty->ContainerPtrToValuePtr<FGuid>(Actor); const FGuid Guid = Actor->GetActorGuid(); *ReflectedGuid = FGuid::NewGuid();
    TestFalse(TEXT("New actor GUID drift refuses before writing"), GitWorkspaceSave::WriteExternalActorSave(Plan, Repo, *Prepared.Permit, Lease, F.Content).Ok()); *ReflectedGuid = Guid;
    TestFalse(TEXT("New actor cannot bind a different package"), GitWorkspaceSave::ValidateExternalActorBinding(Plan, Path, Actors[0]->GetPackage()).IsEmpty());
    const auto Drift = FCoreUObjectDelegates::OnObjectPreSave.AddLambda([&](UObject* Object, FObjectPreSaveContext)
    { if (Object == Actor) { Actors[1]->Modify(); Actors[1]->SetActorLabel(TEXT("Late unreviewed edit")); } });
    TestFalse(TEXT("Core PreSave cannot add an unreviewed actor to first save"), GitWorkspaceSave::WriteExternalActorSave(Plan, Repo, *Prepared.Permit, Lease, F.Content).Ok()); FCoreUObjectDelegates::OnObjectPreSave.Remove(Drift); Actors[1]->GetPackage()->SetDirtyFlag(false);
    TestFalse(TEXT("Callback drift creates no new actor file"), IFileManager::Get().FileExists(*Full));
    bool bOccupied = false; const auto Collision = FCoreUObjectDelegates::OnObjectPreSave.AddLambda([&](UObject* Object, FObjectPreSaveContext)
    { if (Object == Actor) { Files.Write(Path, TEXT("occupied by another writer\n")); bOccupied = true; } });
    const auto Collided = GitWorkspaceSave::WriteExternalActorSave(Plan, Repo, *Prepared.Permit, Lease, F.Content); FCoreUObjectDelegates::OnObjectPreSave.Remove(Collision);
    TestTrue(TEXT("Appearing destination during core PreSave blocks output"), bOccupied && !Collided.Ok()); FString Occupied; FFileHelper::LoadFileToString(Occupied, *Full);
    TestEqual(TEXT("Foreign destination bytes were not overwritten"), Occupied, FString(TEXT("occupied by another writer\n")));
    TestFalse(TEXT("Occupied destination cannot be gathered as new"), GitWorkspaceSave::GatherPackageSavePaths({Package}, Files.Repo, F.Content).Error.IsEmpty()); IFileManager::Get().Delete(*Full);
    for (const TCHAR* Mode : {TEXT("foreign"), TEXT("otherclone"), TEXT("offline"), TEXT("auth")})
    {
        Files.Mode(Mode); TestFalse(FString(Mode) + TEXT(" blocks new actor write"), GitWorkspaceSave::WriteExternalActorSave(Plan, Repo, *Prepared.Permit, Lease, F.Content).Ok());
        TestFalse(TEXT("Lock failures leave the destination absent"), IFileManager::Get().FileExists(*Full));
    }
    Files.Mode(TEXT(""));
    TestTrue(TEXT("New actor stays loaded, dirty and newly created after refusals"), Actor->GetActorGuid() == Guid && Package->IsDirty() && Package->HasAnyPackageFlags(PKG_NewlyCreated));
    FString Existing; FPackageName::TryConvertLongPackageNameToFilename(Actors[0]->GetPackage()->GetName(), Existing, TEXT(".uasset")); IFileManager::Get().Delete(*Existing);
    Actors[0]->Modify(); Actors[0]->SetActorLabel(TEXT("Missing existing actor"));
    TestFalse(TEXT("A deleted existing file cannot masquerade as a new actor"), GitWorkspaceSave::GatherPackageSavePaths({World->GetPackage()}, Files.Repo, F.Content).Error.IsEmpty());
    TestEqual(TEXT("First-save refusals preserve map bytes"), Files.Call({TEXT("hash-object"), TEXT("--no-filters"), Map}).Text(), MapBytes);
    TestEqual(TEXT("First-save refusals preserve staging"), Repo.Refresh().IndexEntries, Before.IndexEntries); TestEqual(TEXT("First-save refusals preserve HEAD"), Repo.Refresh().Head, Before.Head); TestEqual(TEXT("First-save refusals preserve stashes"), Repo.ListStashes().Fingerprint, Stashes);
    TestTrue(TEXT("The new destination's reservation remains held"), Repo.VerifyLocks(TEXT("origin")).Locks.Contains(Path));
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitNewExternalActorPartialTest, "GitWorkspace.SaveLock.ExternalActorFirstSavePartialAndRetry", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGitNewExternalActorPartialTest::RunTest(const FString&)
{
    for (bool bMapFailure : {false, true})
    {
        auto Lifetime = MakeShared<FMapSaveFixture>(); auto& F = *Lifetime; auto& Files = F.Files; ADD_LATENT_AUTOMATION_COMMAND(FMapFixtureCleanup(Lifetime));
        TArray<AActor*> Actors; UWorld* World = F.ExistingActorWorld(bMapFailure, Actors); if (!TestNotNull(TEXT("New actor partial owner"), World)) return false;
        F.NewActor(World, TEXT("FirstPartialActor"), TEXT("First partial actor")); F.NewActor(World, TEXT("SecondPartialActor"), TEXT("Second partial actor")); World->GetPackage()->SetDirtyFlag(bMapFailure);
        const auto Plan = GitWorkspaceSave::GatherPackageSavePaths({World->GetPackage()}, Files.Repo, F.Content);
        if (!TestTrue(TEXT("Two absent actor files and optional map"), Plan.Error.IsEmpty() && Plan.NewPaths.Num() == 2 && Plan.Paths.Num() == (bMapFailure ? 3 : 2))) return false;
        GitWorkspace::FRepository Repo(Files.Git, Files.Repo); const auto Before = Repo.Refresh(); const FString Stashes = Repo.ListStashes().Fingerprint;
        const FString MapBytes = Files.Call({TEXT("hash-object"), TEXT("--no-filters"), Plan.Owners[0].Filename}).Text();
        GitWorkspaceSession::FLease Lease; FString Error; if (!TestTrue(TEXT("New partial lease"), Lease.Acquire(Files.Repo, true, Error))) return false;
        FFileHelper::SaveStringToFile(Plan.Paths.Last(), *FPaths::Combine(Files.Root, TEXT("fail-lock-path")));
        auto Prepared = Repo.PrepareAssetSave(Repo.ReviewAssetSave(Plan.Paths, TEXT("origin"), Plan.NewPaths, Plan.ExternalActorPaths), Lease, true);
        TestTrue(TEXT("Partial acquisition retains earlier reservations"), !Prepared.Result.Ok() && Prepared.AcquiredPaths.Num() == Plan.Paths.Num() - 1);
        for (const auto& P : Plan.NewPaths) TestFalse(TEXT("Failed reservation creates no actor file"), IFileManager::Get().FileExists(*FPaths::Combine(Files.Repo, P)));
        IFileManager::Get().Delete(*FPaths::Combine(Files.Root, TEXT("fail-lock-path")));
        Prepared = Repo.PrepareAssetSave(Repo.ReviewAssetSave(Plan.Paths, TEXT("origin"), Plan.NewPaths, Plan.ExternalActorPaths), Lease, true); if (!TestTrue(TEXT("Retry acquires all reservations"), Prepared.Result.Ok() && Prepared.Permit)) return false;
        const auto& Failed = Plan.Entries.Last(); GitWorkspaceSave::RemoveGuard(); const auto Previous = FCoreUObjectDelegates::IsPackageOKToSaveDelegate;
        FCoreUObjectDelegates::IsPackageOKToSaveDelegate.BindLambda([File = Failed.Filename](UPackage*, const FString& Destination, FOutputDevice*) { return FPaths::ConvertRelativePathToFull(Destination) != File; });
        GitWorkspaceSave::InstallGuard(); const auto Partial = GitWorkspaceSave::WriteExternalActorSave(Plan, Repo, *Prepared.Permit, Lease, F.Content);
        GitWorkspaceSave::RemoveGuard(); FCoreUObjectDelegates::IsPackageOKToSaveDelegate = Previous; GitWorkspaceSave::InstallGuard();
        TestTrue(TEXT("Failed final write reports completed and remaining paths"), !Partial.Ok() && Partial.Error.Contains(TEXT("Not completed: ") + Failed.Path) && Partial.Error.Contains(TEXT("Saved: ") + Plan.Entries[0].Path));
        for (int32 I = 0; I + 1 < Plan.Entries.Num(); ++I)
            TestTrue(TEXT("Completed new files retained and clean"), IFileManager::Get().FileExists(*Plan.Entries[I].Filename) && !Plan.Entries[I].Package->IsDirty() && !Plan.Entries[I].Package->HasAnyPackageFlags(PKG_NewlyCreated));
        if (!bMapFailure) TestFalse(TEXT("Failed new actor destination stays absent"), IFileManager::Get().FileExists(*Failed.Filename));
        TestTrue(TEXT("Incomplete package stays dirty"), Failed.Package->IsDirty()); TestEqual(TEXT("Failed batch leaves map bytes unchanged"), Files.Call({TEXT("hash-object"), TEXT("--no-filters"), Plan.Owners[0].Filename}).Text(), MapBytes);
        const auto Retry = GitWorkspaceSave::GatherPackageSavePaths({World->GetPackage()}, Files.Repo, F.Content);
        TestTrue(TEXT("Retry reviews only unfinished file with correct first-save intent"), Retry.Error.IsEmpty() && Retry.Paths == TArray<FString>{Failed.Path} && Retry.NewPaths.Num() == (bMapFailure ? 0 : 1));
        const auto Review = Repo.ReviewAssetSave(Retry.Paths, TEXT("origin"), Retry.NewPaths, Retry.ExternalActorPaths); TestTrue(TEXT("Retry keeps reserved locks"), Review.IsFresh() && Review.NeedsLock.IsEmpty());
        Prepared = Repo.PrepareAssetSave(Review, Lease, false); if (!TestTrue(TEXT("Retry permit"), Prepared.Result.Ok() && Prepared.Permit)) return false;
        const auto Result = GitWorkspaceSave::WriteExternalActorSave(Retry, Repo, *Prepared.Permit, Lease, F.Content); TestTrue(TEXT("Retry completes: ") + Result.Error, Result.Ok());
        TestTrue(TEXT("Retry retains all reviewed reservations"), Repo.VerifyLocks(TEXT("origin")).Locks.Num() == Plan.Paths.Num());
        TestEqual(TEXT("Partial and retry preserve staging"), Repo.Refresh().IndexEntries, Before.IndexEntries); TestEqual(TEXT("Partial and retry preserve HEAD"), Repo.Refresh().Head, Before.Head); TestEqual(TEXT("Partial and retry preserve stashes"), Repo.ListStashes().Fingerprint, Stashes);
    }
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitExternalWorldWriteTest, "GitWorkspace.SaveLock.ExternalWorldMapAndBuildData", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGitExternalWorldWriteTest::RunTest(const FString&)
{
    for (int32 Case = 0; Case < 3; ++Case)
    {
        const bool bPartitioned = Case != 0, bExistingData = Case != 2;
        auto Lifetime = MakeShared<FMapSaveFixture>(); auto& F = *Lifetime; auto& Files = F.Files; ADD_LATENT_AUTOMATION_COMMAND(FMapFixtureCleanup(Lifetime));
        TArray<AActor*> Actors; UWorld* World = F.ExistingActorWorld(bPartitioned, Actors, bExistingData);
        if (!TestNotNull(TEXT("External world fixture"), World)) return false;
        GitWorkspace::FRepository Repo(Files.Git, Files.Repo); const auto Before = Repo.Refresh(); const FString Stashes = Repo.ListStashes().Fingerprint;
        if (!bExistingData) F.AddValidBuildData(World);
        UMapBuildDataRegistry* Data = World->PersistentLevel->MapBuildData;
        Data->LevelLightingQuality = Quality_High; Data->MarkPackageDirty();
        World->GetWorldSettings()->Modify(); World->GetWorldSettings()->KillZ = -34567.f;
        Actors[0]->Modify(); Actors[0]->SetActorLabel(TEXT("Coordinated actor edit"));
        const auto Plan = GitWorkspaceSave::GatherPackageSavePaths({World->GetPackage()}, Files.Repo, F.Content);
        if (!TestTrue(TEXT("Map, build data and one actor reviewed: ") + Plan.Error, Plan.Error.IsEmpty() && Plan.Paths.Num() == 3 && Plan.ExternalActorPaths.Num() == 1 && Plan.NewPaths.Num() == (bExistingData ? 0 : 1))) return false;
        TestTrue(TEXT("Dependency writes precede the map"), Plan.Entries[0].Kind == GitWorkspaceSave::FPackageSavePaths::EKind::BuildData && Plan.Entries.Last().Kind == GitWorkspaceSave::FPackageSavePaths::EKind::Map);
        const auto FromData = GitWorkspaceSave::GatherPackageSavePaths({Data->GetPackage()}, Files.Repo, F.Content);
        TestEqual(TEXT("Save All resolves exact map owner from build data alone"), FromData.Paths, Plan.Paths);
        GitWorkspaceSession::FLease Lease; FString Error; if (!TestTrue(TEXT("World-save lease"), Lease.Acquire(Files.Repo, true, Error))) return false;
        auto Review = Repo.ReviewAssetSave(Plan.Paths, TEXT("origin"), Plan.NewPaths, Plan.ExternalActorPaths);
        TestFalse(TEXT("Cancellation acquires no map/data/actor locks"), Repo.PrepareAssetSave(Review, Lease, false).Result.Ok());
        TestTrue(TEXT("Cancellation leaves all edits dirty"), World->GetPackage()->IsDirty() && Data->GetPackage()->IsDirty() && Actors[0]->GetPackage()->IsDirty());
        auto Prepared = Repo.PrepareAssetSave(Review, Lease, true);
        if (!TestTrue(TEXT("World-save locks: ") + Prepared.Result.Error, Prepared.Result.Ok() && Prepared.Permit)) return false;
        TArray<FString> Written;
        const auto Track = UPackage::PackageSavedWithContextEvent.AddLambda([&](const FString&, UPackage* Package, FObjectPostSaveContext Context)
        { if (Context.SaveSucceeded()) for (const auto& Entry : Plan.Entries) if (Entry.Package.Get() == Package) Written.Add(Entry.Path); });
        const auto Result = GitWorkspaceSave::WriteExternalActorSave(Plan, Repo, *Prepared.Permit, Lease, F.Content);
        UPackage::PackageSavedWithContextEvent.Remove(Track);
        if (!TestTrue(TEXT("Coordinated world save: ") + Result.Error, Result.Ok())) return false;
        TestTrue(TEXT("Map saved after its dependency and actor"), Written.Num() == 3 && Written[0] == Plan.Entries[0].Path && Written.Last() == Plan.Entries.Last().Path);
        TestTrue(TEXT("Written map/data/actor are clean"), !World->GetPackage()->IsDirty() && !Data->GetPackage()->IsDirty() && !Actors[0]->GetPackage()->IsDirty());
        TestEqual(TEXT("Edited map value retained"), World->GetWorldSettings()->KillZ, -34567.f);
        TestTrue(TEXT("Same modern registry and valid volume retained"), World->PersistentLevel->MapBuildData == Data && Data->LevelLightingQuality == Quality_High && Data->IsLightingValid(ERHIFeatureLevel::SM5));
        const auto Locks = Repo.VerifyLocks(TEXT("origin"));
        bool bExactLocks = Locks.IsFresh() && Locks.Locks.Num() == Plan.Paths.Num();
        for (const FString& Path : Plan.Paths) bExactLocks &= Locks.Locks.Contains(Path) && Locks.Locks[Path].bOurs;
        TestTrue(TEXT("Exactly the reviewed map/data/dirty actor locks are owned; clean actors stay unlocked"), bExactLocks);
        TestEqual(TEXT("World save preserves staging"), Repo.Refresh().IndexEntries, Before.IndexEntries); TestEqual(TEXT("World save preserves HEAD"), Repo.Refresh().Head, Before.Head); TestEqual(TEXT("World save preserves stashes"), Repo.ListStashes().Fingerprint, Stashes);
        if (bExistingData) F.Export(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Automation/ExternalWorldFixtures"), bPartitioned ? TEXT("WorldPartition") : TEXT("OFPA")));
    }
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitExternalWorldCallbackTest, "GitWorkspace.SaveLock.ExternalWorldCallbacksAndBinding", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGitExternalWorldCallbackTest::RunTest(const FString&)
{
    auto Lifetime = MakeShared<FMapSaveFixture>(); auto& F = *Lifetime; auto& Files = F.Files; ADD_LATENT_AUTOMATION_COMMAND(FMapFixtureCleanup(Lifetime));
    TArray<AActor*> Actors; UWorld* World = F.ExistingActorWorld(true, Actors, true); if (!TestNotNull(TEXT("Callback world fixture"), World)) return false;
    auto* Data = World->PersistentLevel->MapBuildData.Get();
    World->GetWorldSettings()->Modify(); World->GetWorldSettings()->KillZ = -123456.f; Data->LevelLightingQuality = Quality_High; Data->MarkPackageDirty();
    Actors[0]->Modify(); Actors[0]->SetActorLabel(TEXT("Callback reviewed edit"));
    const auto Plan = GitWorkspaceSave::GatherPackageSavePaths({World->GetPackage()}, Files.Repo, F.Content);
    if (!TestTrue(TEXT("Callback plan: ") + Plan.Error, Plan.Error.IsEmpty() && Plan.Paths.Num() == 3)) return false;
    TMap<FString, FString> Bytes; for (const auto& Entry : Plan.Entries) Bytes.Add(Entry.Path, Files.Call({TEXT("hash-object"), TEXT("--no-filters"), Entry.Filename}).Text());
    GitWorkspace::FRepository Repo(Files.Git, Files.Repo); const auto Before = Repo.Refresh();
    GitWorkspaceSession::FLease Lease; FString Error; if (!TestTrue(TEXT("Callback lease"), Lease.Acquire(Files.Repo, true, Error))) return false;
    auto Prepared = Repo.PrepareAssetSave(Repo.ReviewAssetSave(Plan.Paths, TEXT("origin"), {}, Plan.ExternalActorPaths), Lease, true);
    if (!TestTrue(TEXT("Callback permits"), Prepared.Result.Ok() && Prepared.Permit)) return false;
    {
        GitWorkspaceSave::FPreparedScope NoPlan(Repo, *Prepared.Permit, Lease, Files.Repo); FSavePackageArgs Args; Args.TopLevelFlags = RF_Standalone; Args.SaveFlags = SAVE_NoError;
        TestFalse(TEXT("Permit without map plan cannot write external world"), UPackage::SavePackage(World->GetPackage(), World, *Plan.Owners[0].Filename, Args));
        TestFalse(TEXT("Permit without map plan cannot write its build data"), UPackage::SavePackage(Data->GetPackage(), Data, *Plan.Entries[0].Filename, Args));
    }
    const auto Pre = FEditorDelegates::PreSaveWorldWithContext.AddLambda([&](UWorld* W, FObjectPreSaveContext)
    { if (W == World) { Actors[1]->Modify(); Actors[1]->SetActorLabel(TEXT("Unreviewed pre-world actor")); } });
    TestFalse(TEXT("Editor world callback cannot expand dirty set"), GitWorkspaceSave::WriteExternalActorSave(Plan, Repo, *Prepared.Permit, Lease, F.Content).Ok());
    FEditorDelegates::PreSaveWorldWithContext.Remove(Pre); Actors[1]->GetPackage()->SetDirtyFlag(false);
    const auto Late = FCoreUObjectDelegates::OnObjectPreSave.AddLambda([&](UObject* Object, FObjectPreSaveContext)
    { if (Object == Data) { Actors[1]->Modify(); Actors[1]->SetActorLabel(TEXT("Unreviewed core-presave actor")); } });
    const auto LateResult = GitWorkspaceSave::WriteExternalActorSave(Plan, Repo, *Prepared.Permit, Lease, F.Content);
    FCoreUObjectDelegates::OnObjectPreSave.Remove(Late); Actors[1]->GetPackage()->SetDirtyFlag(false);
    TestTrue(TEXT("Late validator refuses core PreSave drift before dependency file write"), !LateResult.Ok() && LateResult.Error.Contains(TEXT("Dirty packages changed")));
    const auto Association = UPackage::PreSavePackageWithContextEvent.AddLambda([&](UPackage* P, FObjectPreSaveContext)
    { if (P == Data->GetPackage()) World->PersistentLevel->MapBuildData = nullptr; });
    TestFalse(TEXT("Registry association drift refuses before writing"), GitWorkspaceSave::WriteExternalActorSave(Plan, Repo, *Prepared.Permit, Lease, F.Content).Ok());
    UPackage::PreSavePackageWithContextEvent.Remove(Association); World->PersistentLevel->MapBuildData = Data;
    for (const auto& Entry : Plan.Entries) TestEqual(TEXT("Refusals preserve package bytes: ") + Entry.Path, Files.Call({TEXT("hash-object"), TEXT("--no-filters"), Entry.Filename}).Text(), Bytes[Entry.Path]);
    const auto Post = FEditorDelegates::PostSaveWorldWithContext.AddLambda([&](UWorld* W, FObjectPostSaveContext Context)
    { if (W == World && Context.SaveSucceeded()) { Actors[0]->Modify(); Actors[0]->SetActorLabel(TEXT("New post-save edit")); } });
    const auto PostResult = GitWorkspaceSave::WriteExternalActorSave(Plan, Repo, *Prepared.Permit, Lease, F.Content);
    FEditorDelegates::PostSaveWorldWithContext.Remove(Post);
    TestTrue(TEXT("New post-save edits are reported and stay unsaved"), !PostResult.Ok() && PostResult.Error.Contains(TEXT("new unsaved edits")) && Actors[0]->GetPackage()->IsDirty());
    TestTrue(TEXT("All locks remain held"), Repo.VerifyLocks(TEXT("origin")).Locks.Num() == 3); TestEqual(TEXT("Callbacks preserve staging"), Repo.Refresh().IndexEntries, Before.IndexEntries);
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitExternalWorldPartialTest, "GitWorkspace.SaveLock.ExternalWorldPartialAndRetry", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGitExternalWorldPartialTest::RunTest(const FString&)
{
    auto Lifetime = MakeShared<FMapSaveFixture>(); auto& F = *Lifetime; auto& Files = F.Files; ADD_LATENT_AUTOMATION_COMMAND(FMapFixtureCleanup(Lifetime));
    TArray<AActor*> Actors; UWorld* World = F.ExistingActorWorld(false, Actors, true); if (!TestNotNull(TEXT("Partial world fixture"), World)) return false;
    auto* Data = World->PersistentLevel->MapBuildData.Get(); World->GetWorldSettings()->Modify(); World->GetWorldSettings()->KillZ = -67890.f;
    Data->LevelLightingQuality = Quality_High; Data->MarkPackageDirty(); Actors[0]->Modify(); Actors[0]->SetActorLabel(TEXT("Partial world edit"));
    const auto Plan = GitWorkspaceSave::GatherPackageSavePaths({World->GetPackage()}, Files.Repo, F.Content);
    if (!TestTrue(TEXT("Partial world plan: ") + Plan.Error, Plan.Error.IsEmpty() && Plan.Paths.Num() == 3)) return false;
    GitWorkspace::FRepository Repo(Files.Git, Files.Repo); const auto Before = Repo.Refresh(); const FString Stashes = Repo.ListStashes().Fingerprint;
    TMap<FString, FString> Bytes; for (const auto& Entry : Plan.Entries) Bytes.Add(Entry.Path, Files.Call({TEXT("hash-object"), TEXT("--no-filters"), Entry.Filename}).Text());
    GitWorkspaceSession::FLease Lease; FString Error; if (!TestTrue(TEXT("Partial world lease"), Lease.Acquire(Files.Repo, true, Error))) return false;
    FFileHelper::SaveStringToFile(Plan.Paths.Last(), *FPaths::Combine(Files.Root, TEXT("fail-lock-path")));
    auto Prepared = Repo.PrepareAssetSave(Repo.ReviewAssetSave(Plan.Paths, TEXT("origin"), {}, Plan.ExternalActorPaths), Lease, true);
    TestTrue(TEXT("Partial reservation retains acquired map/data locks"), !Prepared.Result.Ok() && Prepared.AcquiredPaths.Num() == 2);
    for (const auto& Entry : Plan.Entries) TestEqual(TEXT("Partial acquisition writes no package"), Files.Call({TEXT("hash-object"), TEXT("--no-filters"), Entry.Filename}).Text(), Bytes[Entry.Path]);
    IFileManager::Get().Delete(*FPaths::Combine(Files.Root, TEXT("fail-lock-path")));
    Prepared = Repo.PrepareAssetSave(Repo.ReviewAssetSave(Plan.Paths, TEXT("origin"), {}, Plan.ExternalActorPaths), Lease, true);
    if (!TestTrue(TEXT("Retry reservations"), Prepared.Result.Ok() && Prepared.Permit)) return false;
    GitWorkspaceSave::RemoveGuard(); const auto Previous = FCoreUObjectDelegates::IsPackageOKToSaveDelegate;
    FCoreUObjectDelegates::IsPackageOKToSaveDelegate.BindLambda([Map = Plan.Owners[0].Filename](UPackage*, const FString& File, FOutputDevice*) { return FPaths::ConvertRelativePathToFull(File) != Map; });
    GitWorkspaceSave::InstallGuard(); const auto Partial = GitWorkspaceSave::WriteExternalActorSave(Plan, Repo, *Prepared.Permit, Lease, F.Content);
    GitWorkspaceSave::RemoveGuard(); FCoreUObjectDelegates::IsPackageOKToSaveDelegate = Previous; GitWorkspaceSave::InstallGuard();
    TestTrue(TEXT("Failed map reports completed data/actor and pending map"), !Partial.Ok() && Partial.Error.Contains(TEXT("Saved: ") + Plan.Entries[0].Path) && Partial.Error.Contains(TEXT("Not completed: ") + Plan.Owners[0].Path));
    TestEqual(TEXT("Failed map bytes stay unchanged"), Files.Call({TEXT("hash-object"), TEXT("--no-filters"), Plan.Owners[0].Filename}).Text(), Bytes[Plan.Owners[0].Path]);
    TestTrue(TEXT("Pending map stays dirty, completed dependency/actor clean"), World->GetPackage()->IsDirty() && !Data->GetPackage()->IsDirty() && !Actors[0]->GetPackage()->IsDirty());
    const auto Retry = GitWorkspaceSave::GatherPackageSavePaths({World->GetPackage()}, Files.Repo, F.Content);
    TestTrue(TEXT("Retry reviews only the remaining map"), Retry.Error.IsEmpty() && Retry.Paths == TArray<FString>{Plan.Owners[0].Path} && Retry.ExternalActorPaths.IsEmpty());
    Prepared = Repo.PrepareAssetSave(Repo.ReviewAssetSave(Retry.Paths, TEXT("origin")), Lease, true);
    if (!TestTrue(TEXT("Map-only retry permit"), Prepared.Result.Ok() && Prepared.Permit)) return false;
    const auto Result = GitWorkspaceSave::WriteExternalActorSave(Retry, Repo, *Prepared.Permit, Lease, F.Content);
    TestTrue(TEXT("Map-only retry completes: ") + Result.Error, Result.Ok()); TestFalse(TEXT("Successful retry clears map dirty state"), World->GetPackage()->IsDirty());
    TestTrue(TEXT("Retry retains all map/data/actor locks"), Repo.VerifyLocks(TEXT("origin")).Locks.Num() == 3);
    TestEqual(TEXT("Partial and retry preserve staging"), Repo.Refresh().IndexEntries, Before.IndexEntries); TestEqual(TEXT("Partial and retry preserve HEAD"), Repo.Refresh().Head, Before.Head); TestEqual(TEXT("Partial and retry preserve stashes"), Repo.ListStashes().Fingerprint, Stashes);
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitMapNameCopyTest, "GitWorkspace.SaveLock.MapFirstSaveAndCopy", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGitMapNameCopyTest::RunTest(const FString&)
{
    auto Lifetime = MakeShared<FMapSaveFixture>(); auto& F = *Lifetime; auto& Files = F.Files;
    ADD_LATENT_AUTOMATION_COMMAND(FMapFixtureCleanup(Lifetime));
    GitWorkspace::FRepository Repo(Files.Git, Files.Repo); FString Error; GitWorkspaceSession::FLease Lease;
    if (!TestTrue(TEXT("Map fixture lease"), Lease.Acquire(Files.Repo, true, Error))) return false;
    UWorld* Source = F.NewWorld(); const FString OldName = Source->GetPathName(); UObject* OldData = Source->PersistentLevel->MapBuildData;
    const auto Before = Repo.Refresh(); const auto Stashes = Repo.ListStashes().Fingerprint;
    auto Destination = GitWorkspaceSave::ReviewMapDestination(Source, F.Mount + TEXT("L_Named"), Files.Repo, F.Content);
    if (!TestTrue(TEXT("Ordinary map and build data review: ") + Destination.Error, Destination.Error.IsEmpty() && Destination.bNameCurrent && Destination.Paths().Num() == 2)) return false;
    auto Review = Repo.ReviewAssetSave(Destination.Paths(), TEXT("origin"), Destination.Paths());
    if (!TestTrue(TEXT("Both absent paths reviewed: ") + Review.Error, Review.IsFresh() && Review.NeedsLock.Num() == 2)) return false;
    TestFalse(TEXT("No consent acquires neither lock"), Repo.PrepareAssetSave(Review, Lease).Result.Ok());
    TestEqual(TEXT("Cancelled naming preserves map identity"), Source->GetPathName(), OldName);
    TestTrue(TEXT("Cancelled naming preserves build-data object"), Source->PersistentLevel->MapBuildData == OldData);
    TestTrue(TEXT("Cancelled naming writes nothing"), !IFileManager::Get().FileExists(*Destination.Map.Filename) && !IFileManager::Get().FileExists(*Destination.BuildData.Filename));
    auto Prepared = Repo.PrepareAssetSave(Review, Lease, true); if (!TestTrue(TEXT("Both locks acquired: ") + Prepared.Result.Error, Prepared.Result.Ok() && Prepared.Permit)) return false;
    UWorld* Named = nullptr; const auto Saved = GitWorkspaceSave::WriteMapDestination(Source, Destination, Repo, *Prepared.Permit, Lease, F.Content, Named);
    if (!TestTrue(TEXT("First map save: ") + Saved.Error, Saved.Ok())) return false;
    TestTrue(TEXT("First Save names the same live world"), Named == Source && Source->GetPackage()->GetName() == Destination.Map.PackageName);
    TestTrue(TEXT("First Save keeps and renames the same build data"), Source->PersistentLevel->MapBuildData == OldData && OldData->GetPackage()->GetName() == Destination.BuildData.PackageName);
    TestTrue(TEXT("Both first-save files exist"), IFileManager::Get().FileExists(*Destination.Map.Filename) && IFileManager::Get().FileExists(*Destination.BuildData.Filename));
    const auto SourceBytes = Files.Call({TEXT("hash-object"), TEXT("--no-filters"), Destination.Map.Filename}).Text(); const auto DataBytes = Files.Call({TEXT("hash-object"), TEXT("--no-filters"), Destination.BuildData.Filename}).Text();
    Source->GetWorldSettings()->KillZ = -65432.f; Source->MarkPackageDirty();
    const FGuid VolumeId = FGuid::NewGuid(); Source->PersistentLevel->LevelBuildDataId = VolumeId;
    const FBox VolumeBounds(FVector(-100, -200, -300), FVector(100, 200, 300));
    auto* SourceRegistry = CastChecked<UMapBuildDataRegistry>(OldData);
    auto& Volume = SourceRegistry->AllocateLevelPrecomputedLightVolumeBuildData(VolumeId);
    Volume.Initialize(VolumeBounds);
    FVolumeLightingSample Sample; Sample.Position = FVector3f(12, 34, 56); Sample.Radius = 42; Sample.DirectionalLightShadowing = .37f;
    Volume.AddHighQualityLightingSample(Sample); Volume.FinalizeSamples();
    SourceRegistry->LevelLightingQuality = Quality_Production;
    TestTrue(TEXT("Fixture build data passes engine lighting validity"), SourceRegistry->IsLightingValid(ERHIFeatureLevel::SM5));

    auto CopyDestination = GitWorkspaceSave::ReviewMapDestination(Source, F.Mount + TEXT("L_Copy"), Files.Repo, F.Content);
    if (!TestTrue(TEXT("Saved-map copy review"), CopyDestination.Error.IsEmpty() && !CopyDestination.bNameCurrent)) return false;
    Review = Repo.ReviewAssetSave(CopyDestination.Paths(), TEXT("origin"), CopyDestination.Paths()); Prepared = Repo.PrepareAssetSave(Review, Lease, true);
    if (!TestTrue(TEXT("Copy locks: ") + Prepared.Result.Error, Prepared.Result.Ok() && Prepared.Permit)) return false;
    UWorld* Copy = nullptr; const auto Copied = GitWorkspaceSave::WriteMapDestination(Source, CopyDestination, Repo, *Prepared.Permit, Lease, F.Content, Copy); F.Keep(Copy);
    if (!TestTrue(TEXT("Map copy saved: ") + Copied.Error, Copied.Ok() && Copy)) return false;
    TestTrue(TEXT("Copy has independent level and build data"), Copy != Source && Copy->PersistentLevel != Source->PersistentLevel && Copy->PersistentLevel->MapBuildData != OldData);
    TestEqual(TEXT("Copy contains current unsaved map edits"), Copy->GetWorldSettings()->KillZ, -65432.f);
    auto* VolumeData = Copy->PersistentLevel->MapBuildData->GetLevelPrecomputedLightVolumeBuildData(VolumeId);
    TestTrue(TEXT("Copy contains independent initialized lighting volume"), VolumeData && VolumeData != &Volume && VolumeData->IsInitialized());
    if (VolumeData) TestEqual(TEXT("Copy contains edited light-volume bounds"), VolumeData->GetBounds(), VolumeBounds);
    TestTrue(TEXT("Copy contains edited lighting-quality metadata"), Copy->PersistentLevel->MapBuildData->LevelLightingQuality == Quality_Production);
    TestTrue(TEXT("Original build-data edits remain dirty"), SourceRegistry->GetPackage()->IsDirty());

    TestTrue(TEXT("Original remains dirty and at the same name"), Source->GetPackage()->IsDirty() && Source->GetPackage()->GetName() == Destination.Map.PackageName);
    TestEqual(TEXT("Original map bytes unchanged"), Files.Call({TEXT("hash-object"), TEXT("--no-filters"), Destination.Map.Filename}).Text(), SourceBytes);
    TestEqual(TEXT("Original build-data bytes unchanged"), Files.Call({TEXT("hash-object"), TEXT("--no-filters"), Destination.BuildData.Filename}).Text(), DataBytes);
    const auto Gathered = GitWorkspaceSave::GatherPackageSavePaths({Source->GetPackage()}, Files.Repo, F.Content);
    TestTrue(TEXT("Ordinary Save prepares map and separate build data"), Gathered.Error.IsEmpty() && Gathered.Paths.Num() == 2 && Gathered.NewPaths.IsEmpty());
    TestEqual(TEXT("No implicit staging"), Repo.Refresh().IndexEntries, Before.IndexEntries);
    TestEqual(TEXT("No implicit commit"), Repo.Refresh().Head, Before.Head);
    TestEqual(TEXT("No implicit stash"), Repo.ListStashes().Fingerprint, Stashes);
    const auto Locks = Repo.VerifyLocks(TEXT("origin")); TestTrue(TEXT("All four locks remain verified and owned"), Locks.IsFresh() && Locks.Locks.Num() == 4);
    for (const FString& Path : Destination.Paths()) TestTrue(TEXT("Original lock retained"), Locks.State(Path, true) == GitWorkspace::ELockState::Ours);
    for (const FString& Path : CopyDestination.Paths()) TestTrue(TEXT("Copy lock retained"), Locks.State(Path, true) == GitWorkspace::ELockState::Ours);
    // Exercise Unreal's next ordinary save on the named map. It must resolve
    // its package filename without entering a naming dialog or changing staging.
    Source->GetWorldSettings()->KillZ = -12345.f; Source->MarkPackageDirty();
    auto Again = Repo.ReviewAssetSave(Gathered.Paths, TEXT("origin")); Prepared = Repo.PrepareAssetSave(Again, Lease, true);
    if (!TestTrue(TEXT("Follow-up ordinary save prepared"), Prepared.Result.Ok() && Prepared.Permit)) return false;
    {
        GitWorkspaceSave::FPreparedScope Scope(Repo, *Prepared.Permit, Lease, Files.Repo);
        TestTrue(TEXT("Unreal ordinary Save resolves the newly named map"), FEditorFileUtils::SaveLevel(Source->PersistentLevel));
    }
    TestFalse(TEXT("Follow-up Save clears map dirty state"), Source->GetPackage()->IsDirty());
    TestEqual(TEXT("Follow-up Save preserves index"), Repo.Refresh().IndexEntries, Before.IndexEntries);

    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitMapBoundaryTest, "GitWorkspace.SaveLock.MapDestinationsAndDrift", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGitMapBoundaryTest::RunTest(const FString&)
{
    auto Lifetime = MakeShared<FMapSaveFixture>(); auto& F = *Lifetime; auto& Files = F.Files;
    ADD_LATENT_AUTOMATION_COMMAND(FMapFixtureCleanup(Lifetime)); UWorld* World = F.NewWorld(false);
    const FString PackageName = F.Mount + TEXT("L_Probe");
    auto ReviewDestination = [&] { return GitWorkspaceSave::ReviewMapDestination(World, PackageName, Files.Repo, F.Content); };
    TestTrue(TEXT("Ordinary map without build data needs one path"), ReviewDestination().Paths().Num() == 1 && ReviewDestination().Error.IsEmpty());
    FString DataFile; FPackageName::TryConvertLongPackageNameToFilename(PackageName + TEXT("_BuiltData"), DataFile, TEXT(".uasset"));
    FFileHelper::SaveStringToFile(TEXT("orphan companion data"), *DataFile);
    TestFalse(TEXT("Orphan build data refuses even data-free source"), ReviewDestination().Error.IsEmpty());
    TestTrue(TEXT("Orphan data not removed"), IFileManager::Get().FileExists(*DataFile)); IFileManager::Get().Delete(*DataFile);
    FString External; FPackageName::TryConvertLongPackageNameToFilename(ULevel::GetExternalActorsPath(PackageName), External);
    IFileManager::Get().MakeDirectory(*External, true);
    TestFalse(TEXT("Orphan actor folder refuses destination"), ReviewDestination().Error.IsEmpty()); IFileManager::Get().DeleteDirectory(*External, false, true);
    World->GetWorldSettings()->SetWorldPartition(NewObject<UWorldPartition>(World));
    TestFalse(TEXT("World Partition refused before naming and acquisition"), ReviewDestination().Error.IsEmpty());
    World->GetWorldSettings()->SetWorldPartition(nullptr);
    World->PersistentLevel->SetUseExternalActors(true);
    TestFalse(TEXT("OFPA refused before naming"), ReviewDestination().Error.IsEmpty()); World->PersistentLevel->SetUseExternalActors(false);
    World->WorldComposition = NewObject<UWorldComposition>(World);
    TestFalse(TEXT("World Composition refused before naming"), ReviewDestination().Error.IsEmpty()); World->WorldComposition = nullptr;
    TestFalse(TEXT("Save All cannot open an unreviewed temporary-map naming route"), GitWorkspaceSave::GatherPackageSavePaths({World->GetPackage()}, Files.Repo, F.Content).Error.IsEmpty());
    GitWorkspace::FRepository Repo(Files.Git, Files.Repo); FString Error; GitWorkspaceSession::FLease Lease;
    if (!TestTrue(TEXT("Boundary fixture lease"), Lease.Acquire(Files.Repo, true, Error))) return false;
    const auto Destination = ReviewDestination(); const auto Review = Repo.ReviewAssetSave(Destination.Paths(), TEXT("origin"), Destination.Paths());
    auto Prepared = Repo.PrepareAssetSave(Review, Lease, true); if (!TestTrue(TEXT("Boundary reservation"), Prepared.Result.Ok() && Prepared.Permit)) return false;
    World->PersistentLevel->GetOrCreateMapBuildData();
    UWorld* Written = nullptr;
    TestFalse(TEXT("Build-data association drift refuses before mutation"), GitWorkspaceSave::WriteMapDestination(World, Destination, Repo, *Prepared.Permit, Lease, F.Content, Written).Ok());
    TestTrue(TEXT("Drift leaves current map unnamed and writes nothing"), !Written && FPackageName::IsTempPackage(World->GetPackage()->GetName()) && !IFileManager::Get().FileExists(*Destination.Map.Filename));
    TestTrue(TEXT("Reservation remains held for review/recovery"), Repo.VerifyLocks(TEXT("origin")).State(Destination.Map.Path, true) == GitWorkspace::ELockState::Ours);
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitMapPartialTest, "GitWorkspace.SaveLock.MapPartialFailures", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGitMapPartialTest::RunTest(const FString&)
{
    auto Lifetime = MakeShared<FMapSaveFixture>(); auto& F = *Lifetime; auto& Files = F.Files;
    ADD_LATENT_AUTOMATION_COMMAND(FMapFixtureCleanup(Lifetime)); UWorld* World = F.NewWorld(); const FString SourceName = World->GetPathName();
    GitWorkspace::FRepository Repo(Files.Git, Files.Repo); FString Error; GitWorkspaceSession::FLease Lease;
    if (!TestTrue(TEXT("Partial fixture lease"), Lease.Acquire(Files.Repo, true, Error))) return false;
    const auto Destination = GitWorkspaceSave::ReviewMapDestination(World, F.Mount + TEXT("L_Partial"), Files.Repo, F.Content);
    if (!TestTrue(TEXT("Partial destination valid"), Destination.Error.IsEmpty())) return false;
    const auto Review = Repo.ReviewAssetSave(Destination.Paths(), TEXT("origin"), Destination.Paths());
    FFileHelper::SaveStringToFile(Destination.BuildData.Path, *FPaths::Combine(Files.Root, TEXT("fail-lock-path")));
    auto Prepared = Repo.PrepareAssetSave(Review, Lease, true);
    TestFalse(TEXT("One failed lock cancels preparation"), Prepared.Result.Ok());
    TestTrue(TEXT("Partial acquisition retains map lock without renaming or writing"), Prepared.AcquiredPaths.Num() == 1 && World->GetPathName() == SourceName && !IFileManager::Get().FileExists(*Destination.Map.Filename) && !IFileManager::Get().FileExists(*Destination.BuildData.Filename));
    IFileManager::Get().Delete(*FPaths::Combine(Files.Root, TEXT("fail-lock-path")));
    const auto RetryReview = Repo.ReviewAssetSave(Destination.Paths(), TEXT("origin"), Destination.Paths()); Prepared = Repo.PrepareAssetSave(RetryReview, Lease, true);
    if (!TestTrue(TEXT("Retry completes both reservations"), Prepared.Result.Ok() && Prepared.Permit)) return false;
    auto Guard = FCoreUObjectDelegates::IsPackageOKToSaveDelegate;
    ON_SCOPE_EXIT { FCoreUObjectDelegates::IsPackageOKToSaveDelegate = Guard; };
    FCoreUObjectDelegates::IsPackageOKToSaveDelegate.BindLambda([Guard](UPackage* Package, const FString& File, FOutputDevice* Output)
    { return !File.EndsWith(TEXT(".umap")) && (!Guard.IsBound() || Guard.Execute(Package, File, Output)); });
    UWorld* Named = nullptr; const auto Saved = GitWorkspaceSave::WriteMapDestination(World, Destination, Repo, *Prepared.Permit, Lease, F.Content, Named);
    TestFalse(TEXT("Map writer failure is reported"), Saved.Ok());
    TestTrue(TEXT("Named map with unsaved edits remains available"), Named == World && World->GetPackage()->IsDirty() && World->GetPackage()->GetName() == Destination.Map.PackageName);
    TestTrue(TEXT("Completed dependency retained, map absent"), IFileManager::Get().FileExists(*Destination.BuildData.Filename) && !IFileManager::Get().FileExists(*Destination.Map.Filename));
    const auto Gathered = GitWorkspaceSave::GatherPackageSavePaths({World->GetPackage()}, Files.Repo, F.Content);
    TestTrue(TEXT("Ordinary Save retry reviews both files and only absent map as new"), Gathered.Error.IsEmpty() && Gathered.Paths.Num() == 2 && Gathered.NewPaths.Num() == 1 && Gathered.NewPaths.Contains(Destination.Map.Path));
    TestTrue(TEXT("Both locks retained after partial save"), Repo.VerifyLocks(TEXT("origin")).Locks.Num() == 2);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitSaveLockReviewTest, "GitWorkspace.SaveLock.CancelDriftAndOwnership", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGitSaveLockReviewTest::RunTest(const FString&)
{
    FLockFixture F; if (!TestFalse(TEXT("Save lock server started"), F.Endpoint.IsEmpty())) return false;
    GitWorkspace::FRepository Repo(F.Git, F.Repo); FString Error; GitWorkspaceSession::FLease Lease;
    if (!TestTrue(TEXT("Exclusive fixture lease"), Lease.Acquire(F.Repo, true, Error))) return false;
    const auto Before = Repo.Refresh(); const auto Stashes = Repo.ListStashes().Fingerprint;
    FPlatformFileManager::Get().GetPlatformFile().SetReadOnly(*FPaths::Combine(F.Repo, TEXT("asset.uasset")), true);
    auto Review = Repo.ReviewAssetSave({TEXT("asset.uasset")}, TEXT("origin"));
    if (!TestTrue(TEXT("Unlocked asset review: ") + Review.Error, Review.IsFresh() && Review.NeedsLock.Num() == 1)) return false;
    TestFalse(TEXT("Cancellation cannot acquire a lock"), Repo.PrepareAssetSave(Review, Lease).Result.Ok());
    TestTrue(TEXT("Cancellation keeps asset read only"), IFileManager::Get().IsReadOnly(*FPaths::Combine(F.Repo, TEXT("asset.uasset"))));
    TestTrue(TEXT("Cancellation has no server lock"), Repo.VerifyLocks(TEXT("origin")).Locks.IsEmpty());
    TestEqual(TEXT("Cancellation preserves index"), Repo.Refresh().IndexEntries, Before.IndexEntries);
    TestEqual(TEXT("Cancellation preserves HEAD"), Repo.Refresh().Head, Before.Head);
    TestEqual(TEXT("Cancellation preserves stash list"), Repo.ListStashes().Fingerprint, Stashes);
    auto Expired = Review; Expired.Locks.VerifiedSeconds -= 61;
    TestFalse(TEXT("Expired consent cannot lock"), Repo.PrepareAssetSave(Expired, Lease, true).Result.Ok());
    F.Write(TEXT("asset.uasset"), TEXT("saved bytes changed\n"));
    TestFalse(TEXT("Raw saved byte drift blocks consent"), Repo.PrepareAssetSave(Review, Lease, true).Result.Ok());
    F.Write(TEXT("asset.uasset"), TEXT("base A\n"));
    Review = Repo.ReviewAssetSave({TEXT("asset.uasset")}, TEXT("origin"));
    for (const TCHAR* Mode : {TEXT("foreign"), TEXT("otherclone"), TEXT("offline"), TEXT("auth")})
    {
        F.Mode(Mode);
        TestFalse(FString(Mode) + TEXT(" blocks review"), Repo.ReviewAssetSave({TEXT("asset.uasset")}, TEXT("origin")).IsFresh());
        TestFalse(FString(Mode) + TEXT(" blocks previously reviewed save"), Repo.PrepareAssetSave(Review, Lease, true).Result.Ok());
    }
    F.Mode(TEXT("conflict"));
    TestFalse(TEXT("Acquisition race cancels save"), Repo.PrepareAssetSave(Review, Lease, true).Result.Ok());
    F.Mode(TEXT(""));
    auto Prepared = Repo.PrepareAssetSave(Repo.ReviewAssetSave({TEXT("asset.uasset")}, TEXT("origin")), Lease, true);
    if (!TestTrue(TEXT("Explicit acquisition prepares permit: ") + Prepared.Result.Error, Prepared.Result.Ok() && Prepared.Permit)) return false;
    TestEqual(TEXT("Acquired exactly selected asset"), Prepared.AcquiredPaths.Num(), 1);
    TestTrue(TEXT("Verified owned asset is writable"), !IFileManager::Get().IsReadOnly(*FPaths::Combine(F.Repo, TEXT("asset.uasset"))));
    TestTrue(TEXT("Permit validates saved asset"), Repo.ValidateAssetSave(*Prepared.Permit, TEXT("asset.uasset"), Lease).Ok());
    TestFalse(TEXT("Permit cannot authorize extra paths"), Repo.ValidateAssetSave(*Prepared.Permit, TEXT("extra.uasset"), Lease).Ok());
    F.Write(TEXT("note.txt"), TEXT("staged change\n")); F.Call({TEXT("add"), TEXT("note.txt")});
    TestFalse(TEXT("Index drift blocks prepared write"), Repo.ValidateAssetSave(*Prepared.Permit, TEXT("asset.uasset"), Lease).Ok());
    auto Owned = Repo.ReviewAssetSave({TEXT("asset.uasset")}, TEXT("origin"));
    TestTrue(TEXT("Recorded existing lock needs no new acquisition"), Owned.IsFresh() && Owned.NeedsLock.IsEmpty());
    Prepared = Repo.PrepareAssetSave(Owned, Lease);
    TestTrue(TEXT("Owned file can prepare without lock consent"), Prepared.Result.Ok() && Prepared.AcquiredPaths.IsEmpty());
    F.Mode(TEXT("foreign"));
    TestFalse(TEXT("Final write denies replaced ownership"), Repo.ValidateAssetSave(*Prepared.Permit, TEXT("asset.uasset"), Lease).Ok());
    F.Mode(TEXT("offline"));
    TestFalse(TEXT("Final write denies unavailable verification"), Repo.ValidateAssetSave(*Prepared.Permit, TEXT("asset.uasset"), Lease).Ok());
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitSaveLockPartialTest, "GitWorkspace.SaveLock.PartialAcquisitionAndPointers", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGitSaveLockPartialTest::RunTest(const FString&)
{
    FLockFixture F; if (!TestFalse(TEXT("Save server started"), F.Endpoint.IsEmpty())) return false;
    F.Write(TEXT("second.uasset"), TEXT("second bytes\n")); F.Call({TEXT("add"), TEXT(".")}); F.Call({TEXT("commit"), TEXT("-qm"), TEXT("second")});
    GitWorkspace::FRepository Repo(F.Git, F.Repo); FString Error; GitWorkspaceSession::FLease Lease;
    if (!TestTrue(TEXT("Exclusive lease"), Lease.Acquire(F.Repo, true, Error))) return false;
    auto Review = Repo.ReviewAssetSave({TEXT("asset.uasset"), TEXT("second.uasset")}, TEXT("origin"));
    FFileHelper::SaveStringToFile(TEXT("second.uasset"), *FPaths::Combine(F.Root, TEXT("fail-lock-path")));
    auto Prepared = Repo.PrepareAssetSave(Review, Lease, true);
    TestFalse(TEXT("Partial lock failure does not issue save permit"), Prepared.Result.Ok() || Prepared.Permit.IsValid());
    TestTrue(TEXT("First acquired lock recorded for user"), Prepared.AcquiredPaths == TArray<FString>{TEXT("asset.uasset")});
    auto Locks = Repo.VerifyLocks(TEXT("origin"));
    TestTrue(TEXT("Partial failure retains first lock"), Locks.IsFresh() && Locks.State(TEXT("asset.uasset"), true) == GitWorkspace::ELockState::Ours && !Locks.Locks.Contains(TEXT("second.uasset")));
    IFileManager::Get().Delete(*FPaths::Combine(F.Root, TEXT("fail-lock-path")));
    auto Retry = Repo.ReviewAssetSave({TEXT("asset.uasset"), TEXT("second.uasset")}, TEXT("origin"));
    TestTrue(TEXT("Retry acquires only remaining path"), Retry.IsFresh() && Retry.NeedsLock == TArray<FString>{TEXT("second.uasset")});
    TestTrue(TEXT("Retry completes"), Repo.PrepareAssetSave(Retry, Lease, true).Result.Ok());
    const auto Pointer = F.Call({TEXT("show"), TEXT("HEAD:asset.uasset")}); F.Write(TEXT("asset.uasset"), Pointer.Text());
    TestFalse(TEXT("LFS pointer is not writable asset content"), Repo.ReviewAssetSave({TEXT("asset.uasset")}, TEXT("origin")).IsFresh());
    F.Write(TEXT("asset.uasset"), TEXT("base A\n"));
    const FString Link = FPaths::Combine(F.Repo, TEXT("linked.uasset"));
    symlink("asset.uasset", TCHAR_TO_UTF8(*Link));
    TestFalse(TEXT("Save cannot follow a symlink"), Repo.ReviewAssetSave({TEXT("linked.uasset")}, TEXT("origin")).IsFresh());
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitFirstSaveDestinationTest, "GitWorkspace.SaveLock.FirstSaveDestinations", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGitFirstSaveDestinationTest::RunTest(const FString&)
{
    FLockFixture F; if (!TestFalse(TEXT("First-save server started"), F.Endpoint.IsEmpty())) return false;
    GitWorkspace::FRepository Repo(F.Git, F.Repo); const auto Before = Repo.Refresh(); const auto Stashes = Repo.ListStashes().Fingerprint;
    const FString Path = TEXT("Content/NewFolder/T_First.uasset"), Full = FPaths::Combine(F.Repo, Path);
    FString Error; GitWorkspaceSession::FLease Lease;
    if (!TestTrue(TEXT("First-save lease"), Lease.Acquire(F.Repo, true, Error))) return false;
    auto Review = Repo.ReviewAssetSave({Path}, TEXT("origin"), {Path});
    if (!TestTrue(TEXT("Absent destination reviewed: ") + Review.Error, Review.IsFresh() && Review.NewPaths.Contains(Path) && Review.NeedsLock.Contains(Path))) return false;
    TestTrue(TEXT("Review identifies first save"), Review.Text().Contains(TEXT("First save")));
    TestFalse(TEXT("Review creates no placeholder"), IFileManager::Get().FileExists(*Full));
    TestFalse(TEXT("Review creates no folder"), IFileManager::Get().DirectoryExists(*FPaths::GetPath(Full)));
    TestFalse(TEXT("Cancellation cannot reserve a new path"), Repo.PrepareAssetSave(Review, Lease).Result.Ok());
    TestTrue(TEXT("Cancellation retains empty server lock set"), Repo.VerifyLocks(TEXT("origin")).Locks.IsEmpty());
    TestFalse(TEXT("Missing paths require explicit first-save intent"), Repo.ReviewAssetSave({Path}, TEXT("origin")).IsFresh());
    TestFalse(TEXT("Extra first-save paths refused"), Repo.ReviewAssetSave({Path}, TEXT("origin"), {TEXT("extra.uasset")}).IsFresh());
    FFileHelper::SaveStringToFile(Path, *FPaths::Combine(F.Root, TEXT("forced-lock-path")));
    for (const TCHAR* Mode : {TEXT("foreign"), TEXT("otherclone"), TEXT("offline"), TEXT("auth")})
    {
        F.Mode(Mode);
        TestFalse(FString(Mode) + TEXT(" refuses new destination"), Repo.ReviewAssetSave({Path}, TEXT("origin"), {Path}).IsFresh());
        TestFalse(FString(Mode) + TEXT(" invalidates earlier consent"), Repo.PrepareAssetSave(Review, Lease, true).Result.Ok());
    }
    F.Mode(TEXT("conflict"));
    TestFalse(TEXT("New path acquisition race retains unsaved destination"), Repo.PrepareAssetSave(Review, Lease, true).Result.Ok());
    F.Mode(TEXT(""));
    TestEqual(TEXT("First-save refusals preserve HEAD"), Repo.Refresh().Head, Before.Head);
    TestEqual(TEXT("First-save refusals preserve index"), Repo.Refresh().IndexEntries, Before.IndexEntries);
    TestEqual(TEXT("First-save refusals preserve stashes"), Repo.ListStashes().Fingerprint, Stashes);
    IFileManager::Get().MakeDirectory(*FPaths::GetPath(Full), true); F.Write(Path, TEXT("external client occupied destination\n"));
    TestFalse(TEXT("Appearing file invalidates absence review"), Repo.PrepareAssetSave(Review, Lease, true).Result.Ok());
    TestFalse(TEXT("Occupied path is never treated as new"), Repo.ReviewAssetSave({Path}, TEXT("origin"), {Path}).IsFresh());
    IFileManager::Get().Delete(*Full);
    F.Write(TEXT(".gitignore"), TEXT("Content/NewFolder/\n"));
    TestFalse(TEXT("Ignored future destination refused"), Repo.ReviewAssetSave({Path}, TEXT("origin"), {Path}).IsFresh());
    F.Write(TEXT(".gitignore"), TEXT(""));
    F.Call({TEXT("init"), TEXT("-q"), TEXT("Content/Nested")});
    const FString Nested = TEXT("Content/Nested/T_First.uasset");
    TestFalse(TEXT("Absent path in nested repository refused"), Repo.ReviewAssetSave({Nested}, TEXT("origin"), {Nested}).IsFresh());
    const FString Link = FPaths::Combine(F.Repo, TEXT("Content/Link")); symlink(TCHAR_TO_UTF8(*F.Root), TCHAR_TO_UTF8(*Link));
    const FString Linked = TEXT("Content/Link/T_First.uasset");
    TestFalse(TEXT("Symlink parent cannot reserve new destination"), Repo.ReviewAssetSave({Linked}, TEXT("origin"), {Linked}).IsFresh());
    F.Call({TEXT("update-index"), TEXT("--add"), TEXT("--cacheinfo"), TEXT("160000,") + Before.Head + TEXT(",Content/Sub")});
    const FString Sub = TEXT("Content/Sub/T_First.uasset");
    TestFalse(TEXT("Absent path below submodule refused"), Repo.ReviewAssetSave({Sub}, TEXT("origin"), {Sub}).IsFresh());
    F.Call({TEXT("rm"), TEXT("-f"), TEXT("asset.uasset")});
    TestFalse(TEXT("Staged deletion cannot masquerade as first save"), Repo.ReviewAssetSave({TEXT("asset.uasset")}, TEXT("origin"), {TEXT("asset.uasset")}).IsFresh());
    TestFalse(TEXT("External actor first-save destination refused"), Repo.ReviewAssetSave({TEXT("Content/__ExternalActors__/A.uasset")}, TEXT("origin"), {TEXT("Content/__ExternalActors__/A.uasset")}).IsFresh());
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitFirstSavePartialTest, "GitWorkspace.SaveLock.FirstSavePartialAcquisition", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGitFirstSavePartialTest::RunTest(const FString&)
{
    FLockFixture F; if (!TestFalse(TEXT("New batch server started"), F.Endpoint.IsEmpty())) return false;
    GitWorkspace::FRepository Repo(F.Git, F.Repo); const auto Before = Repo.Refresh();
    const TArray<FString> Paths {TEXT("Content/A_New.uasset"), TEXT("Content/Z_New.uasset")};
    FString Error; GitWorkspaceSession::FLease Lease;
    if (!TestTrue(TEXT("New batch lease"), Lease.Acquire(F.Repo, true, Error))) return false;
    auto Review = Repo.ReviewAssetSave(Paths, TEXT("origin"), Paths);
    if (!TestTrue(TEXT("New batch review: ") + Review.Error, Review.IsFresh())) return false;
    FFileHelper::SaveStringToFile(Paths[1], *FPaths::Combine(F.Root, TEXT("fail-lock-path")));
    auto Prepared = Repo.PrepareAssetSave(Review, Lease, true);
    TestFalse(TEXT("Partial new reservation issues no write permit"), Prepared.Result.Ok() || Prepared.Permit.IsValid());
    TestTrue(TEXT("Partial new reservation reports held path"), Prepared.AcquiredPaths == TArray<FString>{Paths[0]});
    TestFalse(TEXT("Partial failure writes no first asset"), IFileManager::Get().FileExists(*FPaths::Combine(F.Repo, Paths[0])));
    TestFalse(TEXT("Partial failure writes no second asset"), IFileManager::Get().FileExists(*FPaths::Combine(F.Repo, Paths[1])));
    auto Held = Repo.VerifyLocks(TEXT("origin"));
    TestTrue(TEXT("Reserved absent path remains visible and owned"), Held.IsFresh() && Held.State(Paths[0], true) == GitWorkspace::ELockState::Ours);
    IFileManager::Get().Delete(*FPaths::Combine(F.Root, TEXT("fail-lock-path")));
    Review = Repo.ReviewAssetSave(Paths, TEXT("origin"), Paths);
    TestTrue(TEXT("Retry only needs remaining reservation"), Review.IsFresh() && Review.NeedsLock == TArray<FString>{Paths[1]});
    Prepared = Repo.PrepareAssetSave(Review, Lease, true);
    if (!TestTrue(TEXT("Missing-path retry prepares: ") + Prepared.Result.Error, Prepared.Result.Ok() && Prepared.Permit)) return false;
    TestTrue(TEXT("Absent destination passes final verification"), Repo.ValidateAssetSave(*Prepared.Permit, Paths[0], Lease).Ok());
    TestEqual(TEXT("Reservations preserve HEAD"), Repo.Refresh().Head, Before.Head);
    TestEqual(TEXT("Reservations preserve index"), Repo.Refresh().IndexEntries, Before.IndexEntries);
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitFirstSavePackageTest, "GitWorkspace.SaveLock.FirstSaveBlueprintAndTexture", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGitFirstSavePackageTest::RunTest(const FString&)
{
    FLockFixture F; if (!TestFalse(TEXT("First-write server started"), F.Endpoint.IsEmpty())) return false;
    const FString Mount = TEXT("/GitFirstSaveFixture") + FGuid::NewGuid().ToString(EGuidFormats::Digits) + TEXT("/");
    const FString Content = FPaths::Combine(F.Repo, TEXT("Content/"));
    FPackageName::RegisterMountPoint(Mount, Content);
    TStrongObjectPtr<UTexture2D> Texture(NewObject<UTexture2D>(CreatePackage(*(Mount + TEXT("T_First"))), TEXT("T_First"), RF_Public | RF_Standalone));
    TStrongObjectPtr<UBlueprint> Blueprint(FKismetEditorUtilities::CreateBlueprint(AActor::StaticClass(), CreatePackage(*(Mount + TEXT("BP_First"))), TEXT("BP_First"), BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass()));
    ON_SCOPE_EXIT
    {
        for (UObject* Asset : TArray<UObject*>{Texture.Get(), Blueprint.Get()}) if (Asset)
        { Asset->GetPackage()->SetDirtyFlag(false); Asset->GetPackage()->SetFlags(RF_Transient); Asset->ClearFlags(RF_Public | RF_Standalone); }
        FPackageName::UnRegisterMountPoint(Mount, Content);
    };
    if (!TestNotNull(TEXT("Real new Blueprint"), Blueprint.Get())) return false;
    const uint8 Pixel[4] = {0, 255, 0, 255}; Texture->Source.Init(1, 1, 1, 1, TSF_BGRA8, Pixel); Texture->MarkPackageDirty(); Blueprint->MarkPackageDirty();
    const auto Destinations = GitWorkspaceSave::GatherPackageSavePaths({Blueprint->GetPackage(), Texture->GetPackage()}, F.Repo, Content);
    if (!TestTrue(TEXT("Actual new package names yield two first-save destinations"), Destinations.Error.IsEmpty() && Destinations.NewPaths.Num() == 2 && Destinations.Paths.Num() == 2)) return false;
    GitWorkspace::FRepository Repo(F.Git, F.Repo); const auto Before = Repo.Refresh();
    FString Error; GitWorkspaceSession::FLease Lease;
    if (!TestTrue(TEXT("Real first-write lease"), Lease.Acquire(F.Repo, true, Error))) return false;
    const auto Review = Repo.ReviewAssetSave(Destinations.Paths, TEXT("origin"), Destinations.NewPaths);
    if (!TestTrue(TEXT("Real new package review: ") + Review.Error, Review.IsFresh())) return false;
    auto Prepared = Repo.PrepareAssetSave(Review, Lease, true);
    if (!TestTrue(TEXT("First-write permit: ") + Prepared.Result.Error, Prepared.Result.Ok() && Prepared.Permit)) return false;
    auto Locks = Repo.VerifyLocks(TEXT("origin"));
    TestEqual(TEXT("Both new paths reserved before bytes exist"), Locks.Locks.Num(), 2);
    const FString TexturePath = TEXT("Content/T_First.uasset"), BlueprintPath = TEXT("Content/BP_First.uasset");
    const FString TextureFile = FPaths::Combine(F.Repo, TexturePath), BlueprintFile = FPaths::Combine(F.Repo, BlueprintPath);
    TestFalse(TEXT("Preparation makes no texture placeholder"), IFileManager::Get().FileExists(*TextureFile));
    TestFalse(TEXT("Preparation makes no Blueprint placeholder"), IFileManager::Get().FileExists(*BlueprintFile));
    GitWorkspaceSave::InstallGuard(); ON_SCOPE_EXIT { GitWorkspaceSave::RemoveGuard(); };
    FSavePackageArgs Args; Args.TopLevelFlags = RF_Public | RF_Standalone; Args.SaveFlags = SAVE_NoError;
    {
        GitWorkspaceSave::FPreparedScope Scope(Repo, *Prepared.Permit, Lease, F.Repo);
        IFileManager::Get().MakeDirectory(*Content, true); F.Write(TexturePath, TEXT("occupied by external writer\n"));
        TestFalse(TEXT("Final guard refuses appearing first-save file"), UPackage::SavePackage(Texture->GetPackage(), Texture.Get(), *TextureFile, Args));
        FString Occupied; FFileHelper::LoadFileToString(Occupied, *TextureFile);
        TestEqual(TEXT("External file was not overwritten"), Occupied, FString(TEXT("occupied by external writer\n")));
        TestTrue(TEXT("Failed first write keeps dirty memory"), Texture->GetPackage()->IsDirty());
        IFileManager::Get().Delete(*TextureFile);
        TestTrue(TEXT("Native writer first-saves real texture"), UPackage::SavePackage(Texture->GetPackage(), Texture.Get(), *TextureFile, Args));
        F.Mode(TEXT("offline"));
        TestFalse(TEXT("Offline final check refuses first Blueprint write"), UPackage::SavePackage(Blueprint->GetPackage(), Blueprint.Get(), *BlueprintFile, Args));
        TestFalse(TEXT("Offline refusal writes no Blueprint file"), IFileManager::Get().FileExists(*BlueprintFile));
        TestTrue(TEXT("Offline refusal keeps new Blueprint dirty"), Blueprint->GetPackage()->IsDirty());
        F.Mode(TEXT(""));
        TestTrue(TEXT("Native writer first-saves real Blueprint"), UPackage::SavePackage(Blueprint->GetPackage(), Blueprint.Get(), *BlueprintFile, Args));
        TestFalse(TEXT("First-save permit cannot authorize Save As destination"), FCoreUObjectDelegates::IsPackageOKToSaveDelegate.Execute(Texture->GetPackage(), FPaths::Combine(Content, TEXT("T_Unreviewed.uasset")), nullptr));
    }
    UPackage::WaitForAsyncFileWrites();
    TestTrue(TEXT("Saved texture contains bytes"), IFileManager::Get().FileSize(*TextureFile) > 0);
    TestTrue(TEXT("Saved Blueprint contains bytes"), IFileManager::Get().FileSize(*BlueprintFile) > 0);
    TestEqual(TEXT("First save never stages"), Repo.Refresh().IndexEntries, Before.IndexEntries);
    TestEqual(TEXT("First save never commits"), Repo.Refresh().Head, Before.Head);
    const auto AfterLocks = Repo.VerifyLocks(TEXT("origin"));
    for (const auto& Path : Destinations.Paths)
        TestTrue(TEXT("Exact new lock retained: ") + Path, AfterLocks.IsFresh() && AfterLocks.Locks.Contains(Path) && AfterLocks.Locks[Path].Id == Locks.Locks[Path].Id);
    auto Subsequent = Repo.ReviewAssetSave(Destinations.Paths, TEXT("origin"));
    TestTrue(TEXT("Next save uses owned existing assets without acquisition"), Subsequent.IsFresh() && Subsequent.NewPaths.IsEmpty() && Subsequent.NeedsLock.IsEmpty());
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitSaveAsCopiesTest, "GitWorkspace.SaveLock.SaveAsBlueprintAndTexture", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGitSaveAsCopiesTest::RunTest(const FString&)
{
    FLockFixture F; if (!TestFalse(TEXT("Copy server started"), F.Endpoint.IsEmpty())) return false;
    const FString Mount = TEXT("/GitCopyFixture") + FGuid::NewGuid().ToString(EGuidFormats::Digits) + TEXT("/");
    const FString Content = FPaths::Combine(F.Repo, TEXT("Content/"));
    FPackageName::RegisterMountPoint(Mount, Content); IFileManager::Get().MakeDirectory(*Content, true);
    TStrongObjectPtr<UBlueprint> Blueprint(FKismetEditorUtilities::CreateBlueprint(AActor::StaticClass(), CreatePackage(*(Mount + TEXT("BP_Original"))), TEXT("BP_Original"), BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass()));
    TStrongObjectPtr<UTexture2D> Texture(NewObject<UTexture2D>(CreatePackage(*(Mount + TEXT("T_Original"))), TEXT("T_Original"), RF_Public | RF_Standalone));
    TArray<UObject*> Copies;
    ON_SCOPE_EXIT
    {
        GitWorkspaceSave::RemoveGuard();
        TArray<UObject*> Objects = Copies; Objects.Add(Blueprint.Get()); Objects.Add(Texture.Get());
        for (UObject* Object : Objects) if (Object) { Object->GetPackage()->SetDirtyFlag(false); Object->GetPackage()->SetFlags(RF_Transient); Object->ClearFlags(RF_Public | RF_Standalone); }
        FPackageName::UnRegisterMountPoint(Mount, Content);
    };
    if (!TestNotNull(TEXT("Original Blueprint"), Blueprint.Get())) return false;
    Blueprint->BlueprintDescription = TEXT("saved source A");
    Blueprint->GeneratedClass->GetDefaultObject<AActor>()->Tags = {FName(TEXT("SavedSourceA"))};
    const uint8 PixelA[4] = {255, 0, 0, 255}, PixelB[4] = {0, 255, 0, 255}; Texture->Source.Init(1, 1, 1, 1, TSF_BGRA8, PixelA);
    FSavePackageArgs Args; Args.TopLevelFlags = RF_Public | RF_Standalone; Args.SaveFlags = SAVE_NoError;
    const TArray<UObject*> Sources {Blueprint.Get(), Texture.Get()}; TArray<TArray<uint8>> SourceBytes;
    for (UObject* Source : Sources)
    {
        const FString Filename = FPaths::Combine(Content, Source->GetName() + TEXT(".uasset"));
        if (!TestTrue(TEXT("Save source fixture"), UPackage::SavePackage(Source->GetPackage(), Source, *Filename, Args))) return false;
        TArray<uint8> Bytes; FFileHelper::LoadFileToArray(Bytes, *Filename); SourceBytes.Add(Bytes);
    }
    F.Call({TEXT("add"), TEXT("Content")}); F.Call({TEXT("commit"), TEXT("-qm"), TEXT("copy sources A")});
    GitWorkspace::FRepository Repo(F.Git, F.Repo);
    const FString SourcePath = TEXT("Content/BP_Original.uasset");
    if (!TestTrue(TEXT("Acquire original Blueprint lock"), Repo.ChangeLock(Repo.VerifyLocks(TEXT("origin")), SourcePath, false).Ok())) return false;
    const FString OriginalLockId = Repo.VerifyLocks(TEXT("origin")).Locks[SourcePath].Id;
    Blueprint->BlueprintDescription = TEXT("unsaved source B must be copied without saving A"); Blueprint->MarkPackageDirty();
    Blueprint->GeneratedClass->GetDefaultObject<AActor>()->Tags = {FName(TEXT("UnsavedCopyB"))};
    Texture->Source.Init(1, 1, 1, 1, TSF_BGRA8, PixelB); Texture->MarkPackageDirty();
    for (UObject* Source : Sources) FPlatformFileManager::Get().GetPlatformFile().SetReadOnly(*FPaths::Combine(Content, Source->GetName() + TEXT(".uasset")), true);
    const auto Before = Repo.Refresh(); const auto Stashes = Repo.ListStashes().Fingerprint;
    FString Error; GitWorkspaceSession::FLease Lease;
    if (!TestTrue(TEXT("Copy lease"), Lease.Acquire(F.Repo, true, Error))) return false;
    GitWorkspaceSave::InstallGuard();
    for (int32 I = 0; I < Sources.Num(); ++I)
    {
        UObject* Source = Sources[I]; const FString OriginalName = Source->GetPathName();
        const auto Destination = GitWorkspaceSave::ReviewCopyDestination(Source, Mount + Source->GetName() + TEXT("_Copy"), F.Repo, Content);
        if (!TestTrue(TEXT("Absent copy destination: ") + Destination.Error, Destination.Error.IsEmpty())) return false;
        const auto Review = Repo.ReviewAssetSave({Destination.Path}, TEXT("origin"), {Destination.Path});
        const auto Prepared = Repo.PrepareAssetSave(Review, Lease, true);
        if (!TestTrue(TEXT("Copy reservation prepared: ") + Prepared.Result.Error, Prepared.Result.Ok() && Prepared.Permit)) return false;
        TestNull(TEXT("No copy exists before reservation"), FindPackage(nullptr, *Destination.PackageName));
        TestFalse(TEXT("No placeholder before copy"), IFileManager::Get().FileExists(*Destination.Filename));
        const FString CopyLockId = Repo.VerifyLocks(TEXT("origin")).Locks[Destination.Path].Id;
        UObject* Copy = nullptr;
        const auto Written = GitWorkspaceSave::WriteAssetCopy(Source, Destination, Repo, *Prepared.Permit, Lease, Content, Copy);
        if (Copy) Copies.Add(Copy);
        if (!TestTrue(TEXT("Real copy writer: ") + Written.Error, Written.Ok() && Copy)) return false;
        TestTrue(TEXT("Copy file saved"), IFileManager::Get().FileSize(*Destination.Filename) > 0);
        TestFalse(TEXT("Saved copy is clean in memory"), Copy->GetPackage()->IsDirty());
        TestEqual(TEXT("Original name is not changed"), Source->GetPathName(), OriginalName);
        TestTrue(TEXT("Original unsaved memory remains dirty"), Source->GetPackage()->IsDirty());
        if (I == 0)
        {
            const auto* BP = CastChecked<UBlueprint>(Copy);
            TestEqual(TEXT("Copy contains edited Blueprint actor defaults B"), BP->GeneratedClass->GetDefaultObject<AActor>()->Tags, Blueprint->GeneratedClass->GetDefaultObject<AActor>()->Tags);
            TestEqual(TEXT("Original Blueprint description remains edited"), Blueprint->BlueprintDescription, FString(TEXT("unsaved source B must be copied without saving A")));
            TestTrue(TEXT("Unreal's DuplicateTransient description rule is retained"), BP->BlueprintDescription.IsEmpty());
        }
        else
        {
            TArray64<uint8> Pixels;
            TestTrue(TEXT("Texture copy contains edited green source pixels"), CastChecked<UTexture2D>(Copy)->Source.GetMipData(Pixels, 0) && Pixels.Num() == 4 && FMemory::Memcmp(Pixels.GetData(), PixelB, 4) == 0);
        }
        TArray<uint8> After; const FString SourceFile = FPaths::Combine(Content, Source->GetName() + TEXT(".uasset")); FFileHelper::LoadFileToArray(After, *SourceFile);
        TestEqual(TEXT("Original saved bytes A preserved"), After, SourceBytes[I]);
        TestTrue(TEXT("Original file stays read only"), IFileManager::Get().IsReadOnly(*SourceFile));
        const auto Locks = Repo.VerifyLocks(TEXT("origin"));
        TestTrue(TEXT("Exact copy lock remains owned"), Locks.Locks.Contains(Destination.Path) && Locks.Locks[Destination.Path].Id == CopyLockId && Locks.Locks[Destination.Path].bOurs);
        const auto State = Repo.Refresh();
        TestTrue(TEXT("New copy is untracked"), State.Files.ContainsByPredicate([&](const GitWorkspace::FFile& File) { return File.Path == Destination.Path && File.bUntracked; }));
        TestEqual(TEXT("Save As does not stage"), State.IndexEntries, Before.IndexEntries);
        TestEqual(TEXT("Save As does not commit"), State.Head, Before.Head);
        TestEqual(TEXT("Save As preserves stashes"), Repo.ListStashes().Fingerprint, Stashes);
    }
    const auto FinalLocks = Repo.VerifyLocks(TEXT("origin"));
    TestTrue(TEXT("Original Blueprint lock is unchanged"), FinalLocks.Locks.Contains(SourcePath) && FinalLocks.Locks[SourcePath].Id == OriginalLockId);
    TestFalse(TEXT("Copying an unlocked texture does not acquire its source lock"), FinalLocks.Locks.Contains(TEXT("Content/T_Original.uasset")));
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitMaterialCopyTest, "GitWorkspace.SaveLock.SaveAsMaterialPreview", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGitMaterialCopyTest::RunTest(const FString&)
{
    FLockFixture F; if (!TestFalse(TEXT("Material copy server started"), F.Endpoint.IsEmpty())) return false;
    const FString Mount = TEXT("/GitMaterialCopy") + FGuid::NewGuid().ToString(EGuidFormats::Digits) + TEXT("/");
    const FString Content = FPaths::Combine(F.Repo, TEXT("Content/")); IFileManager::Get().MakeDirectory(*Content, true); FPackageName::RegisterMountPoint(Mount, Content);
    TStrongObjectPtr<UMaterial> Source(NewObject<UMaterial>(CreatePackage(*(Mount + TEXT("M_Original"))), TEXT("M_Original"), RF_Public | RF_Standalone));
    auto* SavedColor = CastChecked<UMaterialExpressionConstant3Vector>(UMaterialEditingLibrary::CreateMaterialExpression(Source.Get(), UMaterialExpressionConstant3Vector::StaticClass()));
    SavedColor->Constant = FLinearColor::Red; Source->GetEditorOnlyData()->BaseColor.Connect(0, SavedColor);
    Source->SetUsageByFlag(MATUSAGE_SkeletalMesh, true); Source->SetUsageByFlag(MATUSAGE_StaticMesh, false);
    FSavePackageArgs Args; Args.TopLevelFlags = RF_Public | RF_Standalone; Args.SaveFlags = SAVE_NoError;
    const FString SourcePath = TEXT("Content/M_Original.uasset"), SourceFile = FPaths::Combine(F.Repo, SourcePath);
    if (!TestTrue(TEXT("Save red material fixture"), UPackage::SavePackage(Source->GetPackage(), Source.Get(), *SourceFile, Args))) return false;
    F.Call({TEXT("add"), TEXT("Content")}); F.Call({TEXT("commit"), TEXT("-qm"), TEXT("red material source")});
    TArray<uint8> SourceBytes; FFileHelper::LoadFileToArray(SourceBytes, *SourceFile);
    auto Editor = IMaterialEditorModule::Get().CreateMaterialEditor(EToolkitMode::Standalone, nullptr, Source.Get());
    TArray<UObject*> Copies;
    ON_SCOPE_EXIT
    {
        GitWorkspaceSave::RemoveGuard(); Editor->CloseWindow(EAssetEditorCloseReason::AssetForceDeleted);
        TArray<UObject*> Objects = Copies; Objects.Add(Source.Get());
        for (UObject* Object : Objects) if (Object) { Object->GetPackage()->SetDirtyFlag(false); Object->GetPackage()->SetFlags(RF_Transient); Object->ClearFlags(RF_Public | RF_Standalone); }
        FPackageName::UnRegisterMountPoint(Mount, Content);
    };
    auto* Preview = CastChecked<UMaterial>(Editor->GetMaterialInterface());
    TestEqual(TEXT("Real material editor registers asset, preview and helper"), Editor->GetObjectsCurrentlyBeingEdited()->Num(), 3);
    TestTrue(TEXT("Save As source adapter selects the single persistent original"), GitWorkspaceSave::GetCopySource(*Editor) == Source.Get());
    auto* EditedColor = CastChecked<UMaterialExpressionConstant3Vector>(Preview->GetEditorOnlyData()->BaseColor.Expression);
    EditedColor->Constant = FLinearColor::Green; Preview->TwoSided = true;
    Preview->MaterialGraph->LinkGraphNodesFromMaterial(); Editor->UpdateMaterialAfterGraphChange();
    const auto Apply = FInputBindingManager::Get().FindCommandInContext(TEXT("MaterialEditor"), TEXT("Apply"));
    const auto* ApplyAction = Editor->GetToolkitCommands()->GetActionForCommand(Apply);
    if (!TestTrue(TEXT("Real material editor has unapplied edits"), ApplyAction && ApplyAction->CanExecuteAction.Execute())) return false;
    const auto Save = FInputBindingManager::Get().FindCommandInContext(TEXT("AssetEditor"), TEXT("SaveAsset"));
    const FUIAction OriginalSave = *Editor->GetToolkitCommands()->GetActionForCommand(Save);
    TestTrue(TEXT("Preview data adapter accepts real editor preview"), GitWorkspaceSave::ReviewCopyData(Source.Get(), Preview).IsEmpty());
    GitWorkspace::FRepository Repo(F.Git, F.Repo); const auto Before = Repo.Refresh(); const FString Stashes = Repo.ListStashes().Fingerprint;
    FPlatformFileManager::Get().GetPlatformFile().SetReadOnly(*SourceFile, true);
    const bool bSourceDirty = Source->GetPackage()->IsDirty(); const FString SourceName = Source->GetPathName();
    FString Error; GitWorkspaceSession::FLease Lease; if (!TestTrue(TEXT("Material copy lease"), Lease.Acquire(F.Repo, true, Error))) return false;
    const auto Destination = GitWorkspaceSave::ReviewCopyDestination(Source.Get(), Mount + TEXT("M_Copy"), F.Repo, Content);
    const auto Review = Repo.ReviewAssetSave({Destination.Path}, TEXT("origin"), {Destination.Path});
    TestFalse(TEXT("Cancelled material review issues no permit"), Repo.PrepareAssetSave(Review, Lease).Result.Ok());
    TestTrue(TEXT("Cancel keeps Apply enabled"), ApplyAction->CanExecuteAction.Execute());
    TestEqual(TEXT("Cancel leaves original red"), SavedColor->Constant, FLinearColor::Red);
    TestNull(TEXT("Cancel creates no material package"), FindPackage(nullptr, *Destination.PackageName));
    TestTrue(TEXT("Cancel takes no source or destination lock"), Repo.VerifyLocks(TEXT("origin")).Locks.IsEmpty());
    const auto Prepared = Repo.PrepareAssetSave(Review, Lease, true);
    if (!TestTrue(TEXT("Material destination reserved: ") + Prepared.Result.Error, Prepared.Result.Ok() && Prepared.Permit)) return false;
    const FString CopyLock = Repo.VerifyLocks(TEXT("origin")).Locks[Destination.Path].Id;
    GitWorkspaceSave::InstallGuard(); UObject* Object = nullptr;
    const auto Result = GitWorkspaceSave::WriteAssetCopy(Source.Get(), Destination, Repo, *Prepared.Permit, Lease, Content, Object, Preview);
    if (Object) Copies.Add(Object);
    if (!TestTrue(TEXT("Preview copy written: ") + Result.Error, Result.Ok() && Object)) return false;
    auto* Copy = CastChecked<UMaterial>(Object);
    TestTrue(TEXT("Copy is ordinary UMaterial, not editor-only preview"), Copy->GetClass() == UMaterial::StaticClass() && Copy->IsAsset() && !Copy->bIsPreviewMaterial);
    auto* CopyColor = CastChecked<UMaterialExpressionConstant3Vector>(Copy->GetEditorOnlyData()->BaseColor.Expression);
    TestEqual(TEXT("Copy contains unapplied green graph edit"), CopyColor->Constant, FLinearColor::Green);
    TestTrue(TEXT("Copy contains unapplied property edit"), Copy->TwoSided);
    TestTrue(TEXT("Copied expression belongs to copy"), CopyColor->Material == Copy && CopyColor->GetOutermost() == Copy->GetOutermost() && CopyColor != EditedColor);
    TestTrue(TEXT("Usage flags retained without forcing preview-only static mesh usage"), Copy->GetUsageByFlag(MATUSAGE_SkeletalMesh) && !Copy->GetUsageByFlag(MATUSAGE_StaticMesh));
    TestFalse(TEXT("Copy does not gain special engine material status"), Copy->bUsedAsSpecialEngineMaterial);
    TestFalse(TEXT("Copy is saved clean"), Copy->GetPackage()->IsDirty());
    TestTrue(TEXT("Original editor retains same preview and Apply state"), Editor->GetMaterialInterface() == Preview && ApplyAction->CanExecuteAction.Execute());
    TestEqual(TEXT("Source material data is still red"), SavedColor->Constant, FLinearColor::Red);
    TestFalse(TEXT("Source property is not applied"), Source->TwoSided);
    TestEqual(TEXT("Original dirty state preserved"), Source->GetPackage()->IsDirty(), bSourceDirty);
    TestEqual(TEXT("Original asset name preserved"), Source->GetPathName(), SourceName);
    TArray<uint8> After; FFileHelper::LoadFileToArray(After, *SourceFile); TestEqual(TEXT("Source saved bytes preserved"), After, SourceBytes);
    TestTrue(TEXT("Source stays read only"), IFileManager::Get().IsReadOnly(*SourceFile));
    const auto Locks = Repo.VerifyLocks(TEXT("origin"));
    TestFalse(TEXT("Source is not locked by Save As"), Locks.Locks.Contains(SourcePath));
    TestTrue(TEXT("Exact destination lock retained"), Locks.Locks.Contains(Destination.Path) && Locks.Locks[Destination.Path].Id == CopyLock);
    TestEqual(TEXT("Material copy preserves index"), Repo.Refresh().IndexEntries, Before.IndexEntries);
    TestEqual(TEXT("Material copy preserves HEAD"), Repo.Refresh().Head, Before.Head);
    TestEqual(TEXT("Material copy preserves stashes"), Repo.ListStashes().Fingerprint, Stashes);
    TestTrue(TEXT("Material copy remains untracked"), Repo.Refresh().Files.ContainsByPredicate([&](const GitWorkspace::FFile& File) { return File.Path == Destination.Path && File.bUntracked; }));
    // The ordinary Save action still applies and compiles the editor preview.
    const auto SavePrepared = Repo.PrepareAssetSave(Repo.ReviewAssetSave({SourcePath}, TEXT("origin")), Lease, true);
    if (!TestTrue(TEXT("Prepare regular material Save"), SavePrepared.Result.Ok() && SavePrepared.Permit)) return false;
    // Apply reconstructs the original UObject in place. The editor owns it;
    // a fixture strong reference would prohibit that engine replacement.
    Source.Reset();
    {
        // Match the registered mount's literal /var spelling, as the engine's
        // native writer derives its filename from that mount.
        GitWorkspaceSave::FPreparedScope Scope(Repo, *SavePrepared.Permit, Lease, F.Repo);
        // The unattended harness cancels native checkout dialogs. Unreal's
        // supported scripted-save path still runs the original material action
        // and final package writer under the real prepared lock permit.
        TGuardValue<bool> ScriptMode(GIsRunningUnattendedScript, true);
        OriginalSave.ExecuteAction.Execute();
        auto* Current = CastChecked<UMaterial>((*Editor->GetObjectsCurrentlyBeingEdited())[0]);
        TestFalse(TEXT("Native material writer consumes the prepared one-write permit"), UPackage::SavePackage(Current->GetPackage(), Current, *SourceFile, Args));
    }
    Source.Reset(CastChecked<UMaterial>((*Editor->GetObjectsCurrentlyBeingEdited())[0]));
    TestFalse(TEXT("Regular Save clears Apply state"), ApplyAction->CanExecuteAction.Execute());
    TestEqual(TEXT("Regular Save applies green edit"), CastChecked<UMaterialExpressionConstant3Vector>(Source->GetEditorOnlyData()->BaseColor.Expression)->Constant, FLinearColor::Green);
    TestTrue(TEXT("Regular Save applies property edit"), Source->TwoSided);
    TestFalse(TEXT("Regular Save writes a clean source"), Source->GetPackage()->IsDirty());
    FFileHelper::LoadFileToArray(After, *SourceFile); TestTrue(TEXT("Regular Save writes changed source bytes"), After != SourceBytes);
    TestEqual(TEXT("Regular Save still preserves staged snapshot"), Repo.Refresh().IndexEntries, Before.IndexEntries);
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitMaterialInstanceCopyTest, "GitWorkspace.SaveLock.SaveAsMaterialInstance", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGitMaterialInstanceCopyTest::RunTest(const FString&)
{
    FLockFixture F; if (!TestFalse(TEXT("Instance copy server started"), F.Endpoint.IsEmpty())) return false;
    const FString Mount = TEXT("/GitInstanceCopy") + FGuid::NewGuid().ToString(EGuidFormats::Digits) + TEXT("/");
    const FString Content = FPaths::Combine(F.Repo, TEXT("Content/")); IFileManager::Get().MakeDirectory(*Content, true); FPackageName::RegisterMountPoint(Mount, Content);
    TStrongObjectPtr<UMaterial> Parent(NewObject<UMaterial>(CreatePackage(*(Mount + TEXT("M_Parent"))), TEXT("M_Parent"), RF_Public | RF_Standalone));
    auto* Scalar = CastChecked<UMaterialExpressionScalarParameter>(UMaterialEditingLibrary::CreateMaterialExpression(Parent.Get(), UMaterialExpressionScalarParameter::StaticClass()));
    Scalar->ParameterName = TEXT("Roughness"); Scalar->DefaultValue = 0.25f; Parent->GetEditorOnlyData()->Roughness.Connect(0, Scalar);
    UMaterialEditingLibrary::RecompileMaterial(Parent.Get());
    TStrongObjectPtr<UMaterialInstanceConstant> Source(NewObject<UMaterialInstanceConstant>(CreatePackage(*(Mount + TEXT("MI_Original"))), TEXT("MI_Original"), RF_Public | RF_Standalone));
    Source->SetParentEditorOnly(Parent.Get()); Source->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("Roughness")), 0.25f);
    FSavePackageArgs Args; Args.TopLevelFlags = RF_Public | RF_Standalone; Args.SaveFlags = SAVE_NoError;
    const FString SourceFile = FPaths::Combine(Content, TEXT("MI_Original.uasset"));
    if (!TestTrue(TEXT("Save instance parent"), UPackage::SavePackage(Parent->GetPackage(), Parent.Get(), *FPaths::Combine(Content, TEXT("M_Parent.uasset")), Args)) ||
        !TestTrue(TEXT("Save original instance"), UPackage::SavePackage(Source->GetPackage(), Source.Get(), *SourceFile, Args))) return false;
    F.Call({TEXT("add"), TEXT("Content")}); F.Call({TEXT("commit"), TEXT("-qm"), TEXT("instance source")});
    TArray<uint8> SourceBytes; FFileHelper::LoadFileToArray(SourceBytes, *SourceFile);
    auto Editor = IMaterialEditorModule::Get().CreateMaterialInstanceEditor(EToolkitMode::Standalone, nullptr, Source.Get());
    TArray<UObject*> Copies;
    ON_SCOPE_EXIT
    {
        GitWorkspaceSave::RemoveGuard(); Editor->CloseWindow(EAssetEditorCloseReason::AssetForceDeleted);
        TArray<UObject*> Objects = Copies; Objects.Add(Source.Get()); Objects.Add(Parent.Get());
        for (UObject* Object : Objects) if (Object) { Object->GetPackage()->SetDirtyFlag(false); Object->GetPackage()->SetFlags(RF_Transient); Object->ClearFlags(RF_Public | RF_Standalone); }
        FPackageName::UnRegisterMountPoint(Mount, Content);
    };
    TestTrue(TEXT("Instance editor source selection"), GitWorkspaceSave::GetCopySource(*Editor) == Source.Get());
    TestTrue(TEXT("Instance copy uses source, not rendering preview"), GitWorkspaceSave::GetCopyData(*Editor) == Source.Get());
    UMaterialEditorInstanceConstant* Helper = nullptr;
    for (UObject* Object : *Editor->GetObjectsCurrentlyBeingEdited()) if (auto* Candidate = Cast<UMaterialEditorInstanceConstant>(Object)) Helper = Candidate;
    if (!TestNotNull(TEXT("Real instance editor helper"), Helper)) return false;
    UDEditorScalarParameterValue* Parameter = nullptr;
    for (const auto& Group : Helper->ParameterGroups) for (UDEditorParameterValue* Value : Group.Parameters)
        if (auto* Candidate = Cast<UDEditorScalarParameterValue>(Value); Candidate && Candidate->ParameterInfo.Name == TEXT("Roughness")) Parameter = Candidate;
    if (!TestNotNull(TEXT("Editable scalar override"), Parameter)) return false;
    Parameter->bOverride = true; Parameter->ParameterValue = 0.75f;
    Helper->BasePropertyOverrides.bOverride_TwoSided = true; Helper->BasePropertyOverrides.TwoSided = true;
    FPropertyChangedEvent Changed(nullptr); Helper->PostEditChangeProperty(Changed);
    TestTrue(TEXT("Live instance edit dirties source"), Source->GetPackage()->IsDirty());
    TestTrue(TEXT("Live parameter edit updates source"), Source->ScalarParameterValues.ContainsByPredicate([](const FScalarParameterValue& Value) { return Value.ParameterInfo.Name == TEXT("Roughness") && Value.ParameterValue == 0.75f; }));
    FPlatformFileManager::Get().GetPlatformFile().SetReadOnly(*SourceFile, true);
    GitWorkspace::FRepository Repo(F.Git, F.Repo); const auto Before = Repo.Refresh(); const FString Stashes = Repo.ListStashes().Fingerprint;
    FString Error; GitWorkspaceSession::FLease Lease; if (!TestTrue(TEXT("Instance copy lease"), Lease.Acquire(F.Repo, true, Error))) return false;
    const auto Destination = GitWorkspaceSave::ReviewCopyDestination(Source.Get(), Mount + TEXT("MI_Copy"), F.Repo, Content);
    const auto Review = Repo.ReviewAssetSave({Destination.Path}, TEXT("origin"), {Destination.Path});
    TestFalse(TEXT("Instance cancellation does not reserve"), Repo.PrepareAssetSave(Review, Lease).Result.Ok());
    TestNull(TEXT("Instance cancellation creates no copy"), FindPackage(nullptr, *Destination.PackageName));
    const auto Prepared = Repo.PrepareAssetSave(Review, Lease, true);
    if (!TestTrue(TEXT("Instance destination reserved"), Prepared.Result.Ok() && Prepared.Permit)) return false;
    const FString LockId = Repo.VerifyLocks(TEXT("origin")).Locks[Destination.Path].Id;
    GitWorkspaceSave::InstallGuard(); UObject* Object = nullptr;
    const auto Written = GitWorkspaceSave::WriteAssetCopy(Source.Get(), Destination, Repo, *Prepared.Permit, Lease, Content, Object, GitWorkspaceSave::GetCopyData(*Editor));
    if (Object) Copies.Add(Object);
    if (!TestTrue(TEXT("Instance copy written: ") + Written.Error, Written.Ok() && Object)) return false;
    auto* Copy = CastChecked<UMaterialInstanceConstant>(Object);
    TestTrue(TEXT("Copy retains external material parent"), Copy->Parent == Parent.Get());
    TestTrue(TEXT("Copy retains edited scalar override"), Copy->ScalarParameterValues.ContainsByPredicate([](const FScalarParameterValue& Value) { return Value.ParameterInfo.Name == TEXT("Roughness") && Value.ParameterValue == 0.75f; }));
    TestTrue(TEXT("Copy retains Two Sided override"), Copy->BasePropertyOverrides.bOverride_TwoSided && Copy->BasePropertyOverrides.TwoSided);
    TestTrue(TEXT("Source editor stays open with dirty edits"), GitWorkspaceSave::GetCopySource(*Editor) == Source.Get() && Source->GetPackage()->IsDirty());
    TestFalse(TEXT("Copy saved clean"), Copy->GetPackage()->IsDirty());
    TArray<uint8> After; FFileHelper::LoadFileToArray(After, *SourceFile); TestEqual(TEXT("Instance original saved bytes retained"), After, SourceBytes);
    TestTrue(TEXT("Instance original remains read only"), IFileManager::Get().IsReadOnly(*SourceFile));
    const auto Locks = Repo.VerifyLocks(TEXT("origin"));
    TestFalse(TEXT("No instance source lock acquired"), Locks.Locks.Contains(TEXT("Content/MI_Original.uasset")));
    TestTrue(TEXT("Exact instance destination lock retained"), Locks.Locks.Contains(Destination.Path) && Locks.Locks[Destination.Path].Id == LockId);
    const auto Final = Repo.Refresh();
    TestTrue(TEXT("Instance copy untracked"), Final.Files.ContainsByPredicate([&](const GitWorkspace::FFile& File) { return File.Path == Destination.Path && File.bUntracked; }));
    TestEqual(TEXT("Instance copy preserves index"), Final.IndexEntries, Before.IndexEntries);
    TestEqual(TEXT("Instance copy preserves HEAD"), Final.Head, Before.Head);
    TestEqual(TEXT("Instance copy preserves stashes"), Repo.ListStashes().Fingerprint, Stashes);
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitMaterialFunctionCopyTest, "GitWorkspace.SaveLock.SaveAsMaterialFunctionPreview", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGitMaterialFunctionCopyTest::RunTest(const FString&)
{
    FLockFixture F; if (!TestFalse(TEXT("Function copy server started"), F.Endpoint.IsEmpty())) return false;
    const FString Mount = TEXT("/GitFunctionCopy") + FGuid::NewGuid().ToString(EGuidFormats::Digits) + TEXT("/");
    const FString Content = FPaths::Combine(F.Repo, TEXT("Content/")); IFileManager::Get().MakeDirectory(*Content, true); FPackageName::RegisterMountPoint(Mount, Content);
    TStrongObjectPtr<UMaterialFunction> Source(NewObject<UMaterialFunction>(CreatePackage(*(Mount + TEXT("MF_Original"))), TEXT("MF_Original"), RF_Public | RF_Standalone));
    Source->Description = TEXT("saved red function"); Source->bExposeToLibrary = false;
    auto* SavedColor = CastChecked<UMaterialExpressionConstant3Vector>(UMaterialEditingLibrary::CreateMaterialExpressionInFunction(Source.Get(), UMaterialExpressionConstant3Vector::StaticClass()));
    SavedColor->Constant = FLinearColor::Red;
    auto* SavedOutput = CastChecked<UMaterialExpressionFunctionOutput>(UMaterialEditingLibrary::CreateMaterialExpressionInFunction(Source.Get(), UMaterialExpressionFunctionOutput::StaticClass()));
    SavedOutput->OutputName = TEXT("Color"); SavedOutput->A.Connect(0, SavedColor);
    FSavePackageArgs Args; Args.TopLevelFlags = RF_Public | RF_Standalone; Args.SaveFlags = SAVE_NoError;
    const FString SourcePath = TEXT("Content/MF_Original.uasset"), SourceFile = FPaths::Combine(F.Repo, SourcePath);
    if (!TestTrue(TEXT("Save red function source"), UPackage::SavePackage(Source->GetPackage(), Source.Get(), *SourceFile, Args))) return false;
    F.Call({TEXT("add"), TEXT("Content")}); F.Call({TEXT("commit"), TEXT("-qm"), TEXT("red function source")});
    TArray<uint8> SourceBytes; FFileHelper::LoadFileToArray(SourceBytes, *SourceFile);
    auto Editor = IMaterialEditorModule::Get().CreateMaterialEditor(EToolkitMode::Standalone, nullptr, Source.Get());
    TArray<UObject*> Copies;
    ON_SCOPE_EXIT
    {
        GitWorkspaceSave::RemoveGuard(); Editor->CloseWindow(EAssetEditorCloseReason::AssetForceDeleted);
        TArray<UObject*> Objects = Copies; Objects.Add(Source.Get());
        for (UObject* Object : Objects) if (Object) { Object->GetPackage()->SetDirtyFlag(false); Object->GetPackage()->SetFlags(RF_Transient); Object->ClearFlags(RF_Public | RF_Standalone); }
        FPackageName::UnRegisterMountPoint(Mount, Content);
    };
    auto* Preview = CastChecked<UMaterial>(Editor->GetMaterialInterface());
    auto* FunctionPreview = Preview->MaterialGraph->MaterialFunction.Get();
    TestTrue(TEXT("Function original selected despite preview/helper"), GitWorkspaceSave::GetCopySource(*Editor) == Source.Get());
    TestTrue(TEXT("Function data adapter returns material wrapper"), GitWorkspaceSave::GetCopyData(*Editor) == Preview);
    if (!TestTrue(TEXT("Preview function belongs to original"), FunctionPreview && FunctionPreview->ParentFunction == Source.Get())) return false;
    UMaterialExpressionFunctionOutput* Output = nullptr; UMaterialExpressionConstant3Vector* OldColor = nullptr;
    for (UMaterialExpression* Expression : Preview->GetExpressions())
    { if (auto* Found = Cast<UMaterialExpressionFunctionOutput>(Expression)) Output = Found; if (auto* Found = Cast<UMaterialExpressionConstant3Vector>(Expression)) OldColor = Found; }
    if (!TestNotNull(TEXT("Function preview output"), Output) || !TestNotNull(TEXT("Function preview color"), OldColor)) return false;
    // Exercise added/deleted nodes and new links, not just a value on an old node.
    auto* NewColor = CastChecked<UMaterialExpressionConstant3Vector>(Editor->CreateNewMaterialExpression(UMaterialExpressionConstant3Vector::StaticClass(), FVector2D(-300, 0), false, false));
    NewColor->Constant = FLinearColor::Green; Output->A.Connect(0, NewColor);
    Preview->MaterialGraph->LinkGraphNodesFromMaterial();
    Editor->DeleteNodes({OldColor->GraphNode});
    auto* Comment = Editor->CreateNewMaterialExpressionComment(FVector2D(-400, -100)); Comment->GraphNode->NodeComment = TEXT("Copied green function note");
    FunctionPreview->Description = TEXT("unapplied green function"); FunctionPreview->bExposeToLibrary = true;
    Preview->MaterialGraph->LinkGraphNodesFromMaterial(); Editor->UpdateMaterialAfterGraphChange();
    const auto Apply = FInputBindingManager::Get().FindCommandInContext(TEXT("MaterialEditor"), TEXT("Apply"));
    const auto* ApplyAction = Editor->GetToolkitCommands()->GetActionForCommand(Apply);
    if (!TestTrue(TEXT("Function preview has unapplied edits"), ApplyAction && ApplyAction->CanExecuteAction.Execute())) return false;
    TestTrue(TEXT("Real function preview passes boundary"), GitWorkspaceSave::ReviewCopyData(Source.Get(), Preview).IsEmpty());
    const auto Save = FInputBindingManager::Get().FindCommandInContext(TEXT("AssetEditor"), TEXT("SaveAsset"));
    const FUIAction OriginalSave = *Editor->GetToolkitCommands()->GetActionForCommand(Save);
    FPlatformFileManager::Get().GetPlatformFile().SetReadOnly(*SourceFile, true);
    const bool Dirty = Source->GetPackage()->IsDirty(); const FString Name = Source->GetPathName(); const FGuid SourceState = Source->StateId;
    GitWorkspace::FRepository Repo(F.Git, F.Repo); const auto Before = Repo.Refresh(); const FString Stashes = Repo.ListStashes().Fingerprint;
    FString Error; GitWorkspaceSession::FLease Lease; if (!TestTrue(TEXT("Function copy lease"), Lease.Acquire(F.Repo, true, Error))) return false;
    const auto Destination = GitWorkspaceSave::ReviewCopyDestination(Source.Get(), Mount + TEXT("MF_Copy"), F.Repo, Content);
    const auto Review = Repo.ReviewAssetSave({Destination.Path}, TEXT("origin"), {Destination.Path});
    TestFalse(TEXT("Cancel does not reserve function destination"), Repo.PrepareAssetSave(Review, Lease).Result.Ok());
    TestNull(TEXT("Cancel creates no function package"), FindPackage(nullptr, *Destination.PackageName));
    TestTrue(TEXT("Cancel retains function Apply state"), ApplyAction->CanExecuteAction.Execute());
    const auto Prepared = Repo.PrepareAssetSave(Review, Lease, true);
    if (!TestTrue(TEXT("Function destination reserved"), Prepared.Result.Ok() && Prepared.Permit)) return false;
    const FString LockId = Repo.VerifyLocks(TEXT("origin")).Locks[Destination.Path].Id;
    // Stale/replaced preview association refuses before duplication, even with a permit.
    FunctionPreview->ParentFunction = nullptr;
    GitWorkspaceSave::InstallGuard(); UObject* Object = nullptr;
    TestFalse(TEXT("Wrong function preview refuses before copy"), GitWorkspaceSave::WriteAssetCopy(Source.Get(), Destination, Repo, *Prepared.Permit, Lease, Content, Object, Preview).Ok());
    TestNull(TEXT("Refused function creates no copy object"), Object); TestNull(TEXT("Refused function creates no package"), FindPackage(nullptr, *Destination.PackageName));
    FunctionPreview->ParentFunction = Source.Get();
    const auto Written = GitWorkspaceSave::WriteAssetCopy(Source.Get(), Destination, Repo, *Prepared.Permit, Lease, Content, Object, Preview);
    if (Object) Copies.Add(Object);
    if (!TestTrue(TEXT("Function copy written: ") + Written.Error, Written.Ok() && Object)) return false;
    auto* Copy = CastChecked<UMaterialFunction>(Object);
    UMaterialExpressionFunctionOutput* CopyOutput = nullptr;
    for (UMaterialExpression* Expression : Copy->GetExpressions())
    {
        TestTrue(TEXT("Copied function expressions owned independently"), Expression->GetOutermost() == Copy->GetOutermost() && Expression->Function == Copy && !Expression->Material && !Expression->GraphNode);
        if (auto* Found = Cast<UMaterialExpressionFunctionOutput>(Expression)) CopyOutput = Found;
    }
    if (!TestNotNull(TEXT("Copied function output"), CopyOutput)) return false;
    auto* CopyColor = Cast<UMaterialExpressionConstant3Vector>(CopyOutput->A.Expression);
    if (!TestNotNull(TEXT("Copied output retains new node link"), CopyColor)) return false;
    TestEqual(TEXT("Function graph comment retained"), Copy->GetEditorComments().Num(), 1);
    if (Copy->GetEditorComments().Num() == 1)
    {
        auto* CopyComment = Copy->GetEditorComments()[0].Get();
        TestTrue(TEXT("Function comment copied with independent ownership"), CopyComment->GetOutermost() == Copy->GetOutermost() && CopyComment->Function == Copy && !CopyComment->Material && !CopyComment->GraphNode);
        TestEqual(TEXT("Function graph comment text retained"), CopyComment->Text, FString(TEXT("Copied green function note")));
    }
    TestEqual(TEXT("Function copy uses added green node"), CopyColor->Constant, FLinearColor::Green);
    TestTrue(TEXT("New function node is copied independently"), CopyColor != NewColor && Copy->GetExpressions().Num() == 2);
    TestEqual(TEXT("Copy retains edited function description"), Copy->Description, FString(TEXT("unapplied green function")));
    TestTrue(TEXT("Copy retains library exposure"), Copy->bExposeToLibrary);
    TestTrue(TEXT("Function copy has no preview/editor state"), !Copy->ParentFunction && !Copy->PreviewMaterial && !Copy->EditorMaterial && !Copy->MaterialGraph);
    TestTrue(TEXT("Copied function has independent state identity"), Copy->StateId != SourceState);
    TestFalse(TEXT("Function copy saved clean"), Copy->GetPackage()->IsDirty());
    TestTrue(TEXT("Function editor keeps unapplied preview"), Editor->GetMaterialInterface() == Preview && ApplyAction->CanExecuteAction.Execute());
    TestEqual(TEXT("Original function stays red"), SavedColor->Constant, FLinearColor::Red);
    TestTrue(TEXT("Original function metadata unchanged"), Source->Description == TEXT("saved red function") && !Source->bExposeToLibrary && Source->StateId == SourceState);
    TestEqual(TEXT("Original function dirty state retained"), Source->GetPackage()->IsDirty(), Dirty); TestEqual(TEXT("Original function name retained"), Source->GetPathName(), Name);
    TArray<uint8> After; FFileHelper::LoadFileToArray(After, *SourceFile); TestEqual(TEXT("Original function saved bytes retained"), After, SourceBytes);
    TestTrue(TEXT("Original function remains read only"), IFileManager::Get().IsReadOnly(*SourceFile));
    const auto Locks = Repo.VerifyLocks(TEXT("origin"));
    TestFalse(TEXT("Save As acquires no source function lock"), Locks.Locks.Contains(SourcePath));
    TestTrue(TEXT("Exact function destination reservation retained"), Locks.Locks.Contains(Destination.Path) && Locks.Locks[Destination.Path].Id == LockId);
    const auto Final = Repo.Refresh();
    TestTrue(TEXT("Function copy untracked"), Final.Files.ContainsByPredicate([&](const GitWorkspace::FFile& File) { return File.Path == Destination.Path && File.bUntracked; }));
    TestEqual(TEXT("Function copy preserves index"), Final.IndexEntries, Before.IndexEntries); TestEqual(TEXT("Function copy preserves HEAD"), Final.Head, Before.Head); TestEqual(TEXT("Function copy preserves stashes"), Repo.ListStashes().Fingerprint, Stashes);
    const auto SavePrepared = Repo.PrepareAssetSave(Repo.ReviewAssetSave({SourcePath}, TEXT("origin")), Lease, true);
    if (!TestTrue(TEXT("Prepare ordinary function Save"), SavePrepared.Result.Ok() && SavePrepared.Permit)) return false;
    Source.Reset();
    {
        GitWorkspaceSave::FPreparedScope Scope(Repo, *SavePrepared.Permit, Lease, F.Repo); TGuardValue<bool> ScriptMode(GIsRunningUnattendedScript, true); OriginalSave.ExecuteAction.Execute();
    }
    Source.Reset(CastChecked<UMaterialFunction>(GitWorkspaceSave::GetCopySource(*Editor)));
    TestFalse(TEXT("Regular function Save clears Apply state"), ApplyAction->CanExecuteAction.Execute());
    TestTrue(TEXT("Regular function Save applies metadata and graph"), Source->Description == TEXT("unapplied green function") && Source->bExposeToLibrary && Source->GetExpressions().Num() == 2);
    TestFalse(TEXT("Regular function Save writes clean source"), Source->GetPackage()->IsDirty());
    FFileHelper::LoadFileToArray(After, *SourceFile); TestTrue(TEXT("Regular function Save writes changed bytes"), After != SourceBytes);
    TestEqual(TEXT("Regular function Save preserves staging"), Repo.Refresh().IndexEntries, Before.IndexEntries);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitMaterialCopyDataTest, "GitWorkspace.SaveLock.MaterialCopyDataBoundaries", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGitMaterialCopyDataTest::RunTest(const FString&)
{
    TStrongObjectPtr<UMaterial> Source(NewObject<UMaterial>(GetTransientPackage()));
    TStrongObjectPtr<UMaterial> Other(NewObject<UMaterial>(GetTransientPackage()));
    TStrongObjectPtr<UMaterialFunction> Function(NewObject<UMaterialFunction>(GetTransientPackage()));
    TestTrue(TEXT("Ordinary material is supported"), GitWorkspaceSave::SupportsAssetCopy(Source.Get()));
    TestTrue(TEXT("Direct source copies retain existing behavior"), GitWorkspaceSave::ReviewCopyData(Source.Get(), Source.Get()).IsEmpty());
    TestFalse(TEXT("Unavailable material preview fails closed"), GitWorkspaceSave::ReviewCopyData(Source.Get(), nullptr).IsEmpty());
    TestFalse(TEXT("Unrelated normal material cannot supply preview edits"), GitWorkspaceSave::ReviewCopyData(Source.Get(), Other.Get()).IsEmpty());
    TestTrue(TEXT("Ordinary functions supported"), GitWorkspaceSave::SupportsAssetCopy(Function.Get()));
    TestFalse(TEXT("Material data cannot substitute a function preview"), GitWorkspaceSave::ReviewCopyData(Function.Get(), Other.Get()).IsEmpty());
    TStrongObjectPtr<UMaterialInstanceConstant> Instance(NewObject<UMaterialInstanceConstant>(GetTransientPackage()));
    TStrongObjectPtr<UMaterialInstanceConstant> OtherInstance(NewObject<UMaterialInstanceConstant>(GetTransientPackage()));
    TestTrue(TEXT("Ordinary instance supported"), GitWorkspaceSave::SupportsAssetCopy(Instance.Get()));
    TestTrue(TEXT("Instance requires its actual source"), GitWorkspaceSave::ReviewCopyData(Instance.Get(), Instance.Get()).IsEmpty());
    TestFalse(TEXT("Instance preview proxy cannot replace source"), GitWorkspaceSave::ReviewCopyData(Instance.Get(), OtherInstance.Get()).IsEmpty());
    TStrongObjectPtr<UMaterialFunctionInstance> FunctionInstance(NewObject<UMaterialFunctionInstance>(GetTransientPackage()));
    TStrongObjectPtr<UMaterialFunctionMaterialLayer> Layer(NewObject<UMaterialFunctionMaterialLayer>(GetTransientPackage()));
    TestFalse(TEXT("Function instances still require separate integration"), GitWorkspaceSave::SupportsAssetCopy(FunctionInstance.Get()));
    TestFalse(TEXT("Layer subclasses still require separate integration"), GitWorkspaceSave::SupportsAssetCopy(Layer.Get()));
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitSaveAsRefusalTest, "GitWorkspace.SaveLock.SaveAsCancellationAndRefusal", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGitSaveAsRefusalTest::RunTest(const FString&)
{
    FLockFixture F; if (!TestFalse(TEXT("Copy refusal server started"), F.Endpoint.IsEmpty())) return false;
    const FString Mount = TEXT("/GitCopyRefusal") + FGuid::NewGuid().ToString(EGuidFormats::Digits) + TEXT("/");
    const FString Content = FPaths::Combine(F.Repo, TEXT("Content/")); IFileManager::Get().MakeDirectory(*Content, true); FPackageName::RegisterMountPoint(Mount, Content);
    TStrongObjectPtr<UTexture2D> Source(NewObject<UTexture2D>(CreatePackage(*(Mount + TEXT("T_Original"))), TEXT("T_Original"), RF_Public | RF_Standalone));
    const uint8 Pixel[4] = {0, 255, 0, 255}; Source->Source.Init(1, 1, 1, 1, TSF_BGRA8, Pixel); Source->MarkPackageDirty();
    TArray<UObject*> Copies;
    auto Previous = FCoreUObjectDelegates::IsPackageOKToSaveDelegate;
    ON_SCOPE_EXIT
    {
        GitWorkspaceSave::RemoveGuard(); FCoreUObjectDelegates::IsPackageOKToSaveDelegate = Previous;
        Copies.Add(Source.Get());
        for (UObject* Copy : Copies) if (Copy) { Copy->GetPackage()->SetDirtyFlag(false); Copy->GetPackage()->SetFlags(RF_Transient); Copy->ClearFlags(RF_Public | RF_Standalone); }
        FPackageName::UnRegisterMountPoint(Mount, Content);
    };
    GitWorkspace::FRepository Repo(F.Git, F.Repo); const auto Before = Repo.Refresh();
    const auto D = GitWorkspaceSave::ReviewCopyDestination(Source.Get(), Mount + TEXT("T_Copy"), F.Repo, Content);
    if (!TestTrue(TEXT("Copy destination reviewed: ") + D.Error, D.Error.IsEmpty())) return false;
    TestFalse(TEXT("Same name cannot overwrite source memory"), GitWorkspaceSave::ReviewCopyDestination(Source.Get(), Source->GetPackage()->GetName(), F.Repo, Content).Error.IsEmpty());
    TestFalse(TEXT("Invalid/escaping package name refused"), GitWorkspaceSave::ReviewCopyDestination(Source.Get(), Mount + TEXT("../T_Copy"), F.Repo, Content).Error.IsEmpty());
    TestFalse(TEXT("Destination outside game Content refused"), GitWorkspaceSave::ReviewCopyDestination(Source.Get(), TEXT("/Engine/T_Copy"), F.Repo, Content).Error.IsEmpty());
    const FString LinkTarget = FPaths::Combine(F.Repo, TEXT("OtherFolder")), Link = FPaths::Combine(Content, TEXT("Link"));
    IFileManager::Get().MakeDirectory(*LinkTarget, true); symlink(TCHAR_TO_UTF8(*LinkTarget), TCHAR_TO_UTF8(*Link));
    const auto Linked = GitWorkspaceSave::ReviewCopyDestination(Source.Get(), Mount + TEXT("Link/T_Copy"), Repo.Refresh().Root, Content);
    TestTrue(TEXT("Root normalization keeps an inner symlink literal"), Linked.Error.IsEmpty() && Linked.Path == TEXT("Content/Link/T_Copy.uasset"));
    TestFalse(TEXT("Service still refuses normalized copy through a symlink"), Repo.ReviewAssetSave({Linked.Path}, TEXT("origin"), {Linked.Path}).IsFresh());
    FString Error; GitWorkspaceSession::FLease Lease;
    if (!TestTrue(TEXT("Copy refusal lease"), Lease.Acquire(F.Repo, true, Error))) return false;
    const bool bFolderBeforeReview = IFileManager::Get().DirectoryExists(*Content);
    const auto Review = Repo.ReviewAssetSave({D.Path}, TEXT("origin"), {D.Path});
    if (!TestTrue(TEXT("New copy review: ") + Review.Error, Review.IsFresh())) return false;
    TestFalse(TEXT("Cancelled review issues no permit"), Repo.PrepareAssetSave(Review, Lease).Result.Ok());
    TestNull(TEXT("Cancelled review creates no copy"), FindPackage(nullptr, *D.PackageName));
    TestFalse(TEXT("Cancelled review creates no file"), IFileManager::Get().FileExists(*D.Filename));
    TestEqual(TEXT("Cancelled review preserves existing folder state"), IFileManager::Get().DirectoryExists(*Content), bFolderBeforeReview);
    TestTrue(TEXT("Cancelled review reserves no server lock"), Repo.VerifyLocks(TEXT("origin")).Locks.IsEmpty());
    const auto Prepared = Repo.PrepareAssetSave(Review, Lease, true);
    if (!TestTrue(TEXT("Copy refusal permit: ") + Prepared.Result.Error, Prepared.Result.Ok() && Prepared.Permit)) return false;
    UObject* Copy = nullptr; auto Forged = D; Forged.Filename += TEXT(".wrong");
    TestFalse(TEXT("Forged destination cannot duplicate"), GitWorkspaceSave::WriteAssetCopy(Source.Get(), Forged, Repo, *Prepared.Permit, Lease, Content, Copy).Ok());
    TestNull(TEXT("Forged destination leaves copy absent"), Copy);
    F.Mode(TEXT("offline"));
    TestFalse(TEXT("Offline verification cancels before duplication"), GitWorkspaceSave::WriteAssetCopy(Source.Get(), D, Repo, *Prepared.Permit, Lease, Content, Copy).Ok());
    TestNull(TEXT("Offline verification creates no package"), FindPackage(nullptr, *D.PackageName));
    F.Mode(TEXT("")); IFileManager::Get().MakeDirectory(*Content, true); F.Write(D.Path, TEXT("external occupied bytes\n"));
    TestFalse(TEXT("Appearing file cancels before duplication"), GitWorkspaceSave::WriteAssetCopy(Source.Get(), D, Repo, *Prepared.Permit, Lease, Content, Copy).Ok());
    FString External; FFileHelper::LoadFileToString(External, *D.Filename); TestEqual(TEXT("Occupied bytes preserved"), External, FString(TEXT("external occupied bytes\n"))); IFileManager::Get().Delete(*D.Filename);
    TStrongObjectPtr<UTexture2D> Occupied(NewObject<UTexture2D>(CreatePackage(*D.PackageName), TEXT("T_Copy"), RF_Public | RF_Standalone));
    TestFalse(TEXT("In-memory destination is refused"), GitWorkspaceSave::WriteAssetCopy(Source.Get(), D, Repo, *Prepared.Permit, Lease, Content, Copy).Ok());
    TestNull(TEXT("In-memory collision creates no replacement"), Copy);
    Copies.Add(Occupied.Get());
    const auto Retry = GitWorkspaceSave::ReviewCopyDestination(Source.Get(), Mount + TEXT("T_Retry"), F.Repo, Content);
    const auto RetryPrepared = Repo.PrepareAssetSave(Repo.ReviewAssetSave({Retry.Path}, TEXT("origin"), {Retry.Path}), Lease, true);
    if (!TestTrue(TEXT("Second destination prepared"), RetryPrepared.Result.Ok() && RetryPrepared.Permit)) return false;
    FCoreUObjectDelegates::IsPackageOKToSaveDelegate.BindLambda([](UPackage*, const FString&, FOutputDevice*) { return false; }); GitWorkspaceSave::InstallGuard();
    const auto Failed = GitWorkspaceSave::WriteAssetCopy(Source.Get(), Retry, Repo, *RetryPrepared.Permit, Lease, Content, Copy);
    if (Copy) Copies.Add(Copy);
    TestFalse(TEXT("Engine writer veto is retained"), Failed.Ok());
    TestTrue(TEXT("Failed writer retains unsaved copy for Save retry"), Copy && Copy->GetPackage()->IsDirty() && Failed.Error.Contains(TEXT("use Save to retry")));
    TestFalse(TEXT("Failed writer creates no saved copy"), IFileManager::Get().FileExists(*Retry.Filename));
    TestTrue(TEXT("Original unsaved texture survives all refusals"), Source->GetPackage()->IsDirty());
    TestEqual(TEXT("Refusals preserve original package name"), Source->GetPackage()->GetName(), Mount + TEXT("T_Original"));
    const auto Locks = Repo.VerifyLocks(TEXT("origin")); TestTrue(TEXT("Failed copy retains reservation"), Locks.Locks.Contains(Retry.Path) && Locks.Locks[Retry.Path].bOurs);
    TestEqual(TEXT("Refusals preserve HEAD"), Repo.Refresh().Head, Before.Head); TestEqual(TEXT("Refusals preserve index"), Repo.Refresh().IndexEntries, Before.IndexEntries);
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitSaveLockPackageTest, "GitWorkspace.SaveLock.RealPackageWriteGuard", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGitSaveLockPackageTest::RunTest(const FString&)
{
    FLockFixture F; if (!TestFalse(TEXT("Package lock server started"), F.Endpoint.IsEmpty())) return false;
    const FString Mount = TEXT("/GitSaveFixture") + FGuid::NewGuid().ToString(EGuidFormats::Digits) + TEXT("/");
    const FString Content = FPaths::Combine(F.Repo, TEXT("Content/")); IFileManager::Get().MakeDirectory(*Content, true);
    FPackageName::RegisterMountPoint(Mount, Content);
    TStrongObjectPtr<UTexture2D> Asset(NewObject<UTexture2D>(CreatePackage(*(Mount + TEXT("T_Save"))), TEXT("T_Save"), RF_Public | RF_Standalone));
    ON_SCOPE_EXIT { Asset->GetPackage()->SetDirtyFlag(false); Asset->GetPackage()->SetFlags(RF_Transient); Asset->ClearFlags(RF_Public | RF_Standalone); FPackageName::UnRegisterMountPoint(Mount, Content); };
    const uint8 Pixel[4] = {255, 0, 0, 255}; Asset->Source.Init(1, 1, 1, 1, TSF_BGRA8, Pixel);
    const FString Path = TEXT("Content/T_Save.uasset"), Filename = FPaths::Combine(F.Repo, Path);
    FSavePackageArgs Args; Args.TopLevelFlags = RF_Public | RF_Standalone; Args.SaveFlags = SAVE_NoError;
    if (!TestTrue(TEXT("Initial fixture save"), UPackage::SavePackage(Asset->GetPackage(), Asset.Get(), *Filename, Args))) return false;
    UPackage::WaitForAsyncFileWrites();
    F.Call({TEXT("add"), TEXT(".")}); F.Call({TEXT("commit"), TEXT("-qm"), TEXT("saved texture")});
    GitWorkspace::FRepository Repo(F.Git, F.Repo); const auto Before = Repo.Refresh();
    TArray<uint8> BeforeBytes; FFileHelper::LoadFileToArray(BeforeBytes, *Filename);
    FString Error; GitWorkspaceSession::FLease Lease; if (!TestTrue(TEXT("Save fixture lease"), Lease.Acquire(F.Repo, true, Error))) return false;
    auto Prepared = Repo.PrepareAssetSave(Repo.ReviewAssetSave({Path}, TEXT("origin")), Lease, true);
    if (!TestTrue(TEXT("Texture permit: ") + Prepared.Result.Error, Prepared.Result.Ok() && Prepared.Permit)) return false;
    GitWorkspaceSave::InstallGuard(); ON_SCOPE_EXIT { GitWorkspaceSave::RemoveGuard(); };
    Asset->SRGB = !Asset->SRGB; Asset->MarkPackageDirty();
    F.Write(TEXT(".gitattributes"), TEXT("# attributes changed after preparation\n"));
    {
        GitWorkspaceSave::FPreparedScope Scope(Repo, *Prepared.Permit, Lease, F.Repo);
        TestFalse(TEXT("Attribute changes cannot bypass a reviewed protected save"), UPackage::SavePackage(Asset->GetPackage(), Asset.Get(), *Filename, Args));
    }
    F.Write(TEXT(".gitattributes"), TEXT("*.uasset filter=lfs diff=lfs merge=lfs -text lockable\n"));
    IFileManager::Get().Delete(*Filename);
    {
        GitWorkspaceSave::FPreparedScope Scope(Repo, *Prepared.Permit, Lease, F.Repo);
        TestFalse(TEXT("Deleted reviewed file cannot become an unguarded first save"), UPackage::SavePackage(Asset->GetPackage(), Asset.Get(), *Filename, Args));
    }
    FFileHelper::SaveArrayToFile(BeforeBytes, *Filename);
    F.Mode(TEXT("offline"));
    {
        GitWorkspaceSave::FPreparedScope Scope(Repo, *Prepared.Permit, Lease, F.Repo);
        TestFalse(TEXT("Actual package writer refuses offline final verification"), UPackage::SavePackage(Asset->GetPackage(), Asset.Get(), *Filename, Args));
    }
    UPackage::WaitForAsyncFileWrites(); TArray<uint8> BlockedBytes; FFileHelper::LoadFileToArray(BlockedBytes, *Filename);
    TestTrue(TEXT("Blocked save keeps editor dirty"), Asset->GetPackage()->IsDirty());
    TestEqual(TEXT("Blocked save keeps saved bytes"), BlockedBytes, BeforeBytes);
    TestEqual(TEXT("Blocked save keeps index"), Repo.Refresh().IndexEntries, Before.IndexEntries);
    F.Mode(TEXT(""));
    Prepared = Repo.PrepareAssetSave(Repo.ReviewAssetSave({Path}, TEXT("origin")), Lease);
    if (!TestTrue(TEXT("Retry texture permit: ") + Prepared.Result.Error, Prepared.Result.Ok() && Prepared.Permit)) return false;
    {
        GitWorkspaceSave::FPreparedScope Scope(Repo, *Prepared.Permit, Lease, F.Repo);
        TestTrue(TEXT("Native package writer saves with verified owned lock"), UPackage::SavePackage(Asset->GetPackage(), Asset.Get(), *Filename, Args));
        Asset->MarkPackageDirty();
        TestFalse(TEXT("Same permit cannot be reused for another write"), UPackage::SavePackage(Asset->GetPackage(), Asset.Get(), *Filename, Args));
        // Make Writable cannot authorize an additional saved file in this session.
        FPlatformFileManager::Get().GetPlatformFile().SetReadOnly(*FPaths::Combine(F.Repo, TEXT("asset.uasset")), false);
        TestFalse(TEXT("Make Writable bypass is denied"), FCoreUObjectDelegates::IsPackageOKToSaveDelegate.Execute(Asset->GetPackage(), FPaths::Combine(F.Repo, TEXT("asset.uasset")), nullptr));
    }
    UPackage::WaitForAsyncFileWrites(); TArray<uint8> Saved; FFileHelper::LoadFileToArray(Saved, *Filename);
    TestTrue(TEXT("Texture bytes changed"), Saved != BeforeBytes);
    TestEqual(TEXT("Save never stages texture"), Repo.Refresh().IndexEntries, Before.IndexEntries);
    TestEqual(TEXT("Save never commits"), Repo.Refresh().Head, Before.Head);
    TestTrue(TEXT("Save retains verified lock"), Repo.VerifyLocks(TEXT("origin")).State(Path, true) == GitWorkspace::ELockState::Ours);
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitDiscardLockTest, "GitWorkspace.Discard.RetainsVerifiedLock", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGitDiscardLockTest::RunTest(const FString&)
{
    FLockFixture F; if (!TestFalse(TEXT("Discard lock server started"), F.Endpoint.IsEmpty())) return false;
    const FString Path = TEXT("Content/Discard.uasset"); IFileManager::Get().MakeDirectory(*FPaths::Combine(F.Repo, TEXT("Content")), true);
    F.Write(Path, TEXT("base\n")); F.Call({TEXT("add"), TEXT(".")}); F.Call({TEXT("commit"), TEXT("-qm"), TEXT("discard asset")});
    F.Call({TEXT("push"), TEXT("--no-verify"), TEXT("origin"), TEXT("main")});
    GitWorkspace::FRepository Repo(F.Git, F.Repo);
    if (!TestTrue(TEXT("Acquire reviewed server lock"), Repo.ChangeLock(Repo.VerifyLocks(TEXT("origin")), Path, false).Ok())) return false;
    const auto Locks = Repo.VerifyLocks(TEXT("origin")); const auto* Held = Locks.Locks.Find(Path);
    if (!TestTrue(TEXT("Owned fixture lock verified"), Held && Held->bOurs)) return false;
    const FString LockId = Held->Id;
    F.Write(Path, TEXT("staged A\n")); Repo.Stage({Path}); F.Write(Path, TEXT("working B\n"));
    FString Error; GitWorkspaceSession::FLease Lease;
    if (!TestTrue(TEXT("Exclusive lock fixture lease"), Lease.Acquire(F.Repo, true, Error))) return false;
    const auto Review = Repo.ReviewDiscard({Path});
    if (!TestTrue(TEXT("Locked asset discard review: ") + Review.Error, Review.IsFresh())) return false;
    const auto Result = Repo.ExecuteDiscard(Review, Lease, true);
    if (!TestTrue(TEXT("Discard locked working edits: ") + Result.Error, Result.Ok())) return false;
    TestTrue(TEXT("Verified discard completion"), Repo.CompleteDiscard(Review, Lease).Ok());
    const auto After = Repo.VerifyLocks(TEXT("origin")); const auto* Retained = After.Locks.Find(Path);
    TestTrue(TEXT("Exact server lock stays owned after discard"), After.IsFresh() && Retained && Retained->bOurs && Retained->Id == LockId);
    TestFalse(TEXT("Staged edits still block handoff"), Repo.ReviewUnlock(TEXT("origin"), Path).IsFresh());
    return true;
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
    TestTrue(TEXT("Combined report names the blocking stash"), Review.Text({TEXT("texture.uasset")}).Contains(TEXT("Saved stash: stash@{0}")) && Review.Text({TEXT("texture.uasset")}).Contains(TEXT("pending Blueprint work")));
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
