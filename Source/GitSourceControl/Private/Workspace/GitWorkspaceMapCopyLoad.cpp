// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#include "GitWorkspaceMapCopyLoad.h"
#include "Engine/World.h"
#include "Engine/Level.h"
#include "Engine/MapBuildDataRegistry.h"
#include "GameFramework/Actor.h"
#include "WorldPartition/WorldPartition.h"
#include "WorldPartition/WorldPartitionHandle.h"
#include "WorldPartition/WorldPartitionEditorLoaderAdapter.h"
#include "UObject/Package.h"
#include "Misc/PackageName.h"

namespace GitWorkspaceSave
{
namespace
{
TMap<TWeakObjectPtr<UWorld>, TArray<FWorldPartitionReference>> RetainedLoads;
FDelegateHandle MapCopyLoadWorldCleanupHandle;
TArray<FWPMapCopyLoadReview::FLoader> Loaders(UWorldPartition* Partition)
{
    TArray<FWPMapCopyLoadReview::FLoader> Out;
    for (UWorldPartitionEditorLoaderAdapter* Object : Partition->GetRegisteredEditorLoaderAdapters())
    {
        FWPMapCopyLoadReview::FLoader Entry; Entry.Object = Object;
        if (auto* Adapter = Object->GetLoaderAdapter())
        { Entry.Adapter = Adapter; Entry.Bounds = Adapter->GetBoundingBox(); Entry.bLoaded = Adapter->IsLoaded(); Entry.bUserCreated = Adapter->GetUserCreated(); }
        Out.Add(MoveTemp(Entry));
    }
    Out.Sort([](const auto& A, const auto& B) { return A.Object.Get() < B.Object.Get(); }); return Out;
}
TMap<FGuid, bool> Pins(UWorldPartition* Partition)
{
    TMap<FGuid, bool> Out;
    for (UWorldPartition::TIterator<> It(Partition); It; ++It) Out.Add(It->GetGuid(), Partition->IsActorPinned(It->GetGuid()));
    return Out;
}
FWPMapCopyLoadReview::FActor ActorState(AActor* Actor)
{
    FWPMapCopyLoadReview::FActor Out; Out.Actor = Actor; Out.Guid = Actor->GetActorGuid(); Out.Path = Actor->GetPathName();
    Out.Label = Actor->GetActorLabel(); Out.Package = Actor->GetPackage()->GetName(); return Out;
}
bool Matches(const FWPMapCopyLoadReview::FActor& Entry)
{
    return Entry.Actor.IsValid() && Entry.Actor->GetActorGuid() == Entry.Guid && Entry.Actor->GetPathName() == Entry.Path &&
        Entry.Actor->GetActorLabel() == Entry.Label && Entry.Actor->GetPackage()->GetName() == Entry.Package;
}
FString Layout(const FWPMapCopyLoadReview& Review)
{
    UWorld* World = Review.World.Get();
    if (!World || World->PersistentLevel != Review.Source.SourceLevel.Get() || World->GetWorldPartition() != Review.Source.SourcePartition.Get())
        return TEXT("The source world/partition changed while loading actors for a copy.");
    auto* Partition = World->GetWorldPartition();
    if (!Partition || !Partition->IsInitialized() || Partition->GetActorDescContainerInstance() != Review.Source.SourceContainer.Get() ||
        Loaders(Partition) != Review.Loaders || !Pins(Partition).OrderIndependentCompareEqual(Review.Pins))
        return TEXT("The source WP container, loaded-region adapters or actor pins changed. Review the copy again.");
    return FString();
}
bool SameFile(const FMapSaveDestination::FSourceFile& Before, const FMapSaveDestination::FSourceFile& After)
{
    return Before.Name == After.Name && Before.Filename == After.Filename && Before.Hash == After.Hash && Before.Mode == After.Mode &&
        Before.bExists == After.bExists && Before.bReadOnly == After.bReadOnly && Before.bDirty == After.bDirty &&
        (!Before.bLoaded || (After.bLoaded && Before.Package == After.Package));
}
void Retain(UWorld* World, TArray<FWorldPartitionReference>& References)
{
    RetainedLoads.FindOrAdd(World).Append(MoveTemp(References));
    if (!MapCopyLoadWorldCleanupHandle.IsValid())
        MapCopyLoadWorldCleanupHandle = FWorldDelegates::OnWorldCleanup.AddLambda([](UWorld* Closing, bool, bool)
        { RetainedLoads.Remove(Closing); });
}
}
bool HasRetainedWPMapCopyLoads(UWorld* World) { return RetainedLoads.Contains(World); }
void ShutdownWPMapCopyLoads()
{
    FWorldDelegates::OnWorldCleanup.Remove(MapCopyLoadWorldCleanupHandle); MapCopyLoadWorldCleanupHandle.Reset();
    FWorldPartitionLoadingContext::FDeferred Loading; RetainedLoads.Empty();
}
FWPMapCopyLoadReview ReviewWPMapCopyLoad(UWorld* World, const FString& Root, const FString& Content)
{
    check(IsInGameThread()); FWPMapCopyLoadReview Out; Out.World = World; Out.Root = Root; Out.Content = Content;
    if (!World || !World->GetWorldPartition() || !World->GetWorldPartition()->IsInitialized() || FPackageName::IsTempPackage(World->GetPackage()->GetName()))
    { Out.Error = TEXT("Loading for a copy requires an initialized, saved named WP map."); return Out; }
    if (HasRetainedWPMapCopyLoads(World))
    { Out.Error = TEXT("Edited temporary actors from an earlier copy remain loaded. Save/reconcile those edits, then reopen this source map before another copy."); return Out; }
    Out.Source.SourcePackage = World->GetPackage()->GetName(); Out.Source.SourceLevel = World->PersistentLevel;
    Out.Source.SourcePartition = World->GetWorldPartition(); Out.Source.SourceBuildData = World->PersistentLevel ? World->PersistentLevel->MapBuildData.Get() : nullptr;
    Out.Error = CaptureExternalMapCopySource(World, Root, Content, Out.Source, true); if (!Out.Error.IsEmpty()) return Out;
    Out.Loaders = Loaders(World->GetWorldPartition()); Out.Pins = Pins(World->GetWorldPartition());
    for (AActor* Actor : World->PersistentLevel->Actors) if (Actor) Out.LoadedActors.Add(ActorState(Actor));
    for (const auto& Descriptor : Out.Source.SourceDescriptors)
        if (!Out.LoadedActors.ContainsByPredicate([&](const auto& Actor) { return Actor.Guid == Descriptor.Guid; }))
        {
            auto* Instance = World->GetWorldPartition()->GetActorDescInstance(Descriptor.Guid);
            if (!Instance || (Instance->GetActor() && Instance->GetActor()->GetPackage()->IsDirty()))
            { Out.Error = TEXT("A WP descriptor has unresolved loaded/reference state. Reconcile this map before copying."); return Out; }
            Out.Missing.Add(Descriptor.Guid);
        }
    return Out;
}
struct FWPMapCopyLoadScope::FState
{
    FWPMapCopyLoadReview Review;
    FMapSaveDestination LoadedSource;
    TArray<FWorldPartitionReference> References;
    TArray<FWPMapCopyLoadReview::FActor> Temporary;
    bool bLoaded = false;
};
FWPMapCopyLoadScope::FWPMapCopyLoadScope() : State(MakeUnique<FState>()) {}
FWPMapCopyLoadScope::~FWPMapCopyLoadScope() { Release(); }
FString FWPMapCopyLoadScope::Load(const FWPMapCopyLoadReview& Review, bool bConfirmed)
{
    check(IsInGameThread());
    if (!bConfirmed) return TEXT("Loading actors for the copy was cancelled. No actor reference or lock was acquired.");
    if (!Review.Error.IsEmpty()) return Review.Error;
    if (State->bLoaded || !State->References.IsEmpty()) return TEXT("This actor-load scope is already in use.");
    FString Error = Layout(Review); if (!Error.IsEmpty()) return Error;
    Error = ValidateExternalMapCopySource(Review.World.Get(), Review.Source, true); if (!Error.IsEmpty()) return Error;
    for (const auto& Actor : Review.LoadedActors) if (!Matches(Actor)) return TEXT("A loaded source actor changed after the loading review.");
    State->Review = Review;
    auto* World = Review.World.Get(); auto* Partition = World->GetWorldPartition();
    {
        // The deferred engine context batches registrations and construction
        // scripts. Only these exact reviewed missing GUIDs get a hard reference.
        FWorldPartitionLoadingContext::FDeferred Loading;
        for (const auto& Guid : Review.Missing) State->References.Emplace(Partition->GetActorDescContainerInstance(), Guid);
    }
    for (const auto& Guid : Review.Missing)
    {
        const auto* Actor = World->PersistentLevel->Actors.FindByPredicate([&](AActor* A) { return IsValid(A) && A->GetActorGuid() == Guid; });
        if (Actor) State->Temporary.Add(ActorState(*Actor));
    }
    Error = Layout(Review); if (!Error.IsEmpty()) return Error;
    if (State->Temporary.Num() != Review.Missing.Num()) return TEXT("Unreal could not load every reviewed actor. No copy destination was reserved or written.");
    auto& Loaded = State->LoadedSource; Loaded.SourcePackage = Review.Source.SourcePackage; Loaded.SourceLevel = Review.Source.SourceLevel;
    Loaded.SourcePartition = Review.Source.SourcePartition; Loaded.SourceBuildData = Review.Source.SourceBuildData;
    Error = CaptureExternalMapCopySource(World, Review.Root, Review.Content, Loaded); if (!Error.IsEmpty()) return Error;
    TSet<AActor*> ExpectedActors, ActualActors;
    for (const auto& Actor : Review.LoadedActors) ExpectedActors.Add(Actor.Actor.Get());
    for (const auto& Actor : State->Temporary) ExpectedActors.Add(Actor.Actor.Get());
    for (AActor* Actor : World->PersistentLevel->Actors) if (IsValid(Actor)) ActualActors.Add(Actor);
    if (ExpectedActors.Num() != ActualActors.Num() || ExpectedActors.Difference(ActualActors).Num())
        return TEXT("Loading added or replaced actors outside the reviewed descriptor set. Unsaved source work remains in the editor.");
    if (Loaded.SourceDescriptors != Review.Source.SourceDescriptors || Loaded.SourceFiles.Num() != Review.Source.SourceFiles.Num())
        return TEXT("Loading changed the reviewed source descriptor/package set. No copy was started.");
    for (int32 I = 0; I < Loaded.SourceFiles.Num(); ++I)
        if (!SameFile(Review.Source.SourceFiles[I], Loaded.SourceFiles[I])) return TEXT("Loading changed source bytes, permissions, dirty state or package bindings. Unsaved edits remain loaded.");
    for (const auto& Actor : Review.LoadedActors) if (!Matches(Actor)) return TEXT("Loading changed a previously loaded source actor. Its edits remain in the editor.");
    State->bLoaded = true; return Validate();
}
FString FWPMapCopyLoadScope::Validate() const
{
    if (!State->bLoaded) return State->Review.World.IsValid() ? TEXT("The WP copy load scope is not ready.") : FString();
    const FString Error = Layout(State->Review); if (!Error.IsEmpty()) return Error;
    for (const auto& Actor : State->Review.LoadedActors) if (!Matches(Actor)) return TEXT("A previously loaded source actor changed during the copy.");
    for (const auto& Actor : State->Temporary) if (!Matches(Actor)) return TEXT("A temporarily loaded source actor changed during the copy.");
    return ValidateExternalMapCopySource(State->Review.World.Get(), State->LoadedSource);
}
FString FWPMapCopyLoadScope::Release()
{
    check(IsInGameThread()); if (!State || !State->Review.World.IsValid()) { if (State) State->References.Reset(); return FString(); }
    const auto Review = State->Review; UWorld* World = Review.World.Get();
    bool bEdited = false;
    for (const auto& Reference : State->References)
        if (Reference.IsValid())
            if (AActor* Actor = Reference->GetActor(); IsValid(Actor))
            {
                const auto* Original = State->Temporary.FindByPredicate([&](const auto& Entry) { return Entry.Guid == Actor->GetActorGuid(); });
                bEdited |= Actor->GetPackage()->IsDirty() || !Original || !Matches(*Original);
            }
    if (bEdited)
    {
        Retain(World, State->References); State->References.Reset(); State->Review = {}; State->bLoaded = false;
        return TEXT("Temporarily loaded WP actors were edited. They remain loaded so their edits are preserved. Save/reconcile those actors, then reopen the source map to restore its loading layout. Source files and locks were not changed by loading or releasing.");
    }
    {
        FWorldPartitionLoadingContext::FDeferred Loading; State->References.Reset();
    }
    State->bLoaded = false; State->Review = {};
    FString Error = Layout(Review); if (!Error.IsEmpty()) return Error;
    TSet<AActor*> Before, After;
    for (const auto& Actor : Review.LoadedActors) Before.Add(Actor.Actor.Get());
    for (AActor* Actor : World->PersistentLevel->Actors) if (IsValid(Actor)) After.Add(Actor);
    if (Before.Contains(nullptr) || Before.Num() != After.Num() || Before.Difference(After).Num())
        return TEXT("WP actor references were released, but the editor's loaded actor set changed. Inspect the source map; no region adapter or pin was changed by this copy flow.");
    return FString();
}
}
