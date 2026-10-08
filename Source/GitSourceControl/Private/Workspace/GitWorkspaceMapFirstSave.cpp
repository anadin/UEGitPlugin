// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#include "GitWorkspaceSaveFlow.h"
#include "Editor.h"
#include "Engine/World.h"
#include "Engine/Level.h"
#include "Engine/MapBuildDataRegistry.h"
#include "Engine/LODActor.h"
#include "GameFramework/Actor.h"
#include "WorldPartition/WorldPartition.h"
#include "WorldPartition/HLOD/HLODActor.h"
#include "WorldPartition/HLOD/HLODLayer.h"
#include "LevelInstance/LevelInstanceInterface.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Interfaces/IMainFrameModule.h"
#include "Modules/ModuleManager.h"
#include "HAL/FileManager.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "Misc/RedirectCollector.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

namespace GitWorkspaceSave
{
FString ReviewExternalFirstMapSource(UWorld* World, bool bTemporary)
{
    if (!World || World->GetClass() != UWorld::StaticClass() || !World->PersistentLevel ||
        (World->WorldType != EWorldType::Editor && World->WorldType != EWorldType::Inactive) ||
        !World->PersistentLevel->IsUsingExternalActors() || World->PersistentLevel->OwningWorld != World ||
        World->GetCurrentLevel() != World->PersistentLevel || World->GetLevels().Num() != 1 ||
        !World->GetStreamingLevels().IsEmpty() || World->WorldComposition ||
        (bTemporary && !FPackageName::IsTempPackage(World->GetPackage()->GetName())))
        return TEXT("First naming supports one never-saved persistent WP/OFPA editor map. Stop Play/Simulate; sublevels and conversion require separate workflows.");
    ULevel* Level = World->PersistentLevel;
    bool bExternalObject = false;
    ForEachObjectWithOuter(Level, [&](UObject* Object)
    { bExternalObject |= Object->IsPackageExternal() && !Object->IsA<AActor>(); return !bExternalObject; }, EGetObjectsFlags::IncludeNestedObjects);
    if (bExternalObject) return TEXT("External objects and actor-folder objects need their own first-naming adapter.");
    if (auto* Partition = World->GetWorldPartition())
    {
        if (Partition->GetActorDescContainerCount() > 1)
            return TEXT("Nested containers and external data layers need a separate map-naming workflow.");
        if (auto* Layer = Partition->GetDefaultHLODLayer(); Layer && FPackageName::IsTempPackage(Layer->GetPackage()->GetName()))
            return TEXT("This template has temporary HLOD layers that need companion naming. First naming of those layers is not supported yet.");
        for (UWorldPartition::TIterator<> It(Partition); It; ++It)
            // GetActor() can resolve a stale soft path using StaticFindObject,
            // which is illegal in the core save validator after a map rename.
            // Bind descriptors to already-loaded level actors by GUID instead.
            if (It->IsChildContainerInstance() || !Level->Actors.ContainsByPredicate([&](AActor* Actor)
                { return IsValid(Actor) && Actor->GetActorGuid() == It->GetGuid(); }))
                return TEXT("Every actor must be loaded in this exact persistent map before its first naming. Unloaded actors and nested containers need separate remapping.");
    }
    if (auto* Data = Level->MapBuildData.Get())
        if (Data->GetClass() != UMapBuildDataRegistry::StaticClass() || Data->IsLegacyBuildData() ||
            Data->GetPackage()->GetName() != World->GetPackage()->GetName() + TEXT("_BuiltData") || !Data->GetPackage()->GetExternalPackages().IsEmpty())
            return TEXT("Legacy, shared or custom build data needs a separate first-naming adapter.");
    TSet<UPackage*> ActorPackages;
    TSet<FGuid> ActorGuids;
    for (AActor* Actor : Level->Actors)
    {
        if (!Actor) continue;
        if (!IsValid(Actor) || Actor->IsA<ALODActor>() || Actor->IsA<AWorldPartitionHLOD>() || Actor->GetExternalDataLayerAsset() ||
            Actor->GetClass()->ImplementsInterface(ULevelInstanceInterface::StaticClass()))
            return TEXT("HLOD, external data-layer actors, level instances and pending actor deletions need separate first-naming adapters.");
        if (!Actor->IsPackageExternal()) continue;
        UPackage* Package = Actor->GetExternalPackage();
        bool bBound = Package && ULevel::GetActorPackageName(World->GetPackage(), Level->GetActorPackagingScheme(), Actor->GetPathName(), Actor) == Package->GetName();
        if (!bBound && bTemporary && Package)
        {
            // Load-as-template instances retain the saved actor's hash under
            // Unreal's temporary instance name until UWorld::Rename remaps it.
            // Bind both original namespaces and the exact engine instance name;
            // no source/template file is written or cleaned up by this flow.
            const FString OriginalMap = World->GetPackage()->GetLoadedPath().GetPackageName();
            const FString OriginalActor = Package->GetLoadedPath().GetPackageName();
            if (!OriginalMap.IsEmpty() && !OriginalActor.IsEmpty() && Actor->GetPathName().StartsWith(World->GetPathName() + TEXT(":")))
            {
                const FString OriginalPath = OriginalMap + TEXT(".") + FPackageName::GetShortName(OriginalMap) + Actor->GetPathName().Mid(World->GetPathName().Len());
                bBound = OriginalActor == ULevel::GetActorPackageName(ULevel::GetExternalActorsPath(OriginalMap), Level->GetActorPackagingScheme(), OriginalPath) &&
                    Package->GetName() == ULevel::GetExternalActorPackageInstanceName(World->GetPackage()->GetName(), OriginalActor);
            }
        }
        if (!Actor->IsMainPackageActor() || Actor->GetOuter() != Level || !Actor->GetActorGuid().IsValid() ||
            Actor->HasAnyFlags(RF_Transient | RF_ClassDefaultObject | RF_ArchetypeObject) || !Package ||
            !bBound || (bTemporary && FPackageName::DoesPackageExist(Package->GetName())))
            return TEXT("Every external actor must have its canonical main-map package. Saved, moved or custom actor packages cannot be renamed through first Save.");
        if (ActorPackages.Contains(Package)) return TEXT("Multiple main actors in one external package need a separate naming adapter.");
        if (ActorGuids.Contains(Actor->GetActorGuid())) return TEXT("External actor GUIDs must be unique before naming this map.");
        ActorPackages.Add(Package);
        ActorGuids.Add(Actor->GetActorGuid());
    }
    // Renaming may leave empty, never-written transient packages. They can be
    // ignored, but never use native cleanup on a saved file or external object.
    for (UPackage* Package : Level->GetLoadedExternalObjectPackages())
    {
        if (ActorPackages.Contains(Package)) continue;
        if (!Package || !UPackage::IsEmptyPackage(Package) || FPackageName::DoesPackageExist(Package->GetName()))
            return TEXT("External objects or saved orphan packages need separate review before naming this map.");
    }
    return FString();
}

void ReviewExternalFirstMapActors(UWorld* World, const FString& Root, const FString& Content, FMapSaveDestination& Destination)
{
    ULevel* Level = World->PersistentLevel;
    const FString NewLevelPath = Destination.Map.PackageName + TEXT(".") + FPackageName::GetLongPackageAssetName(Destination.Map.PackageName) + TEXT(":") + Level->GetName();
    const FString Base = ULevel::GetExternalActorsPath(Destination.Map.PackageName);
    for (AActor* Actor : Level->Actors)
    {
        if (!Actor || !Actor->IsPackageExternal()) continue;
        FMapSaveDestination::FActorDestination Entry;
        Entry.Actor = Actor; Entry.Guid = Actor->GetActorGuid(); Entry.SourcePackage = Actor->GetExternalPackage()->GetName();
        Entry.SourcePath = Actor->GetPathName(); Entry.Label = Actor->GetActorLabel();
        const FString Name = ULevel::GetActorPackageName(Base, Level->GetActorPackagingScheme(), NewLevelPath + TEXT(".") + Actor->GetName());
        Entry.Target = ReviewAbsentDestination(Name, Entry.SourcePackage, Root, Content, false);
        if (!Entry.Target.Error.IsEmpty()) { Destination.Error = TEXT("Actor destination: ") + Entry.Target.Error; return; }
        Destination.Actors.Add(MoveTemp(Entry));
    }
    Destination.Actors.Sort([](const auto& A, const auto& B) { return A.Target.Path < B.Target.Path; });
}

#if PLATFORM_MAC
GitWorkspace::FResult WriteExternalFirstMapDestination(UWorld* Source, const FMapSaveDestination& Destination, GitWorkspace::FRepository& Repository,
    const GitWorkspace::FAssetSavePermit& Permit, const GitWorkspaceSession::FLease& Lease, const FString& Content, UWorld*& OutWorld)
{
    check(IsInGameThread()); OutWorld = nullptr;
    auto Fail = [](const FString& Message) { GitWorkspace::FResult R; R.Error = Message + TEXT("\nThe current world and destination locks remain. Use Save on the named map to review and retry remaining files; nothing was staged or unlocked."); return R; };
    TStrongObjectPtr<UWorld> HoldSource(Source);
    const bool bDisallowExport = Source->GetPackage()->HasAnyPackageFlags(PKG_DisallowExport);
    const auto AccessSpecifier = Source->GetPackage()->GetAssetAccessSpecifier();
    UPackage* Package = CreatePackage(*Destination.Map.PackageName);
    const FSoftObjectPath OldPath(Source);
    const ERenameFlags Flags = REN_NonTransactional | REN_DontCreateRedirectors | REN_AllowPackageLinkerMismatch;
    // Native UWorld::Rename re-embeds/re-externalizes actors and moves modern
    // build data. It does not delete files. No FileHelpers cleanup or Save As.
    if (!Source->Rename(*FPackageName::GetLongPackageAssetName(Destination.Map.PackageName), Package, Flags | REN_Test) ||
        !Source->Rename(*FPackageName::GetLongPackageAssetName(Destination.Map.PackageName), Package, Flags))
        return Fail(TEXT("Could not name this external-actor map. Inspect its current package name before retrying."));
    OutWorld = Source; Package->ThisContainsMap(); Package->MarkAsFullyLoaded();
    if (bDisallowExport) Package->SetPackageFlags(PKG_DisallowExport);
    Package->SetAssetAccessSpecifier(AccessSpecifier);
    Source->ClearFlags(RF_Transient); Source->SetFlags(RF_Public | RF_Standalone); Source->MarkPackageDirty();
    FAssetRegistryModule::AssetCreated(Source);
    GRedirectCollector.AddAssetPathRedirection(OldPath.GetWithoutSubPath(), FSoftObjectPath(Source).GetWithoutSubPath());
    ON_SCOPE_EXIT { GRedirectCollector.RemoveAssetPathRedirection(OldPath.GetWithoutSubPath()); };
    for (const auto& Actor : Destination.Actors)
        if (!Actor.Actor.IsValid() || Actor.Actor->GetActorGuid() != Actor.Guid || !Actor.Actor->GetExternalPackage() ||
            Actor.Actor->GetExternalPackage()->GetName() != Actor.Target.PackageName)
            return Fail(TEXT("Unreal produced an actor package outside the reviewed destination set. No package was saved."));
    const auto Plan = GatherPackageSavePaths({Package}, Repository.Refresh().Root, Content);
    if (!Plan.Error.IsEmpty() || Plan.Paths != Destination.Paths() || Plan.NewPaths != Destination.Paths() || Plan.ExternalActorPaths != Destination.ExternalPaths())
        return Fail(TEXT("The named package set differs from the reviewed map/actor/build-data destinations. No package was saved.\n") + Plan.Error);
    const auto Result = WriteExternalActorSave(Plan, Repository, Permit, Lease, Content);
    // Keep the live named world even on partial failure; next Save reviews only
    // unfinished packages. Do not revert its name, unload it or remove outputs.
    if (GEditor && GEditor->GetEditorWorldContext().World() == Source)
        if (auto* MainFrame = FModuleManager::GetModulePtr<IMainFrameModule>(TEXT("MainFrame"))) MainFrame->SetLevelNameForWindowTitle(Destination.Map.Filename);
    return Result.Ok() ? Result : Fail(Result.Error);
}
#endif
}
