// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#include "GitWorkspaceSaveFlow.h"
#include "Engine/World.h"
#include "Engine/Level.h"
#include "GameFramework/Actor.h"
#include "WorldPartition/WorldPartition.h"
#include "WorldPartition/WorldPartitionRuntimeHash.h"
#include "WorldPartition/HLOD/HLODLayer.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "UObject/Package.h"
#include "Serialization/ArchiveReplaceObjectRef.h"

namespace GitWorkspaceSave
{
FString CaptureHLODLayerChain(UWorld* World, TArray<FHLODLayerBinding>& Out)
{
    Out.Reset();
    auto* Partition = World ? World->GetWorldPartition() : nullptr;
    TSet<UHLODLayer*> Seen; TSet<FString> Names;
    for (UHLODLayer* Layer = Partition ? Partition->GetDefaultHLODLayer() : nullptr; Layer; Layer = Layer->GetParentLayer())
    {
        if (!IsValid(Layer) || Layer->GetClass() != UHLODLayer::StaticClass() || Seen.Contains(Layer) || Names.Contains(Layer->GetName()) ||
            Layer->GetOuter() != Layer->GetPackage() || Layer->IsPackageExternal() || !Layer->GetPackage()->GetExternalPackages().IsEmpty() ||
            Layer->GetLinkedLayer() || Layer->GetLayerType() == EHLODLayerType::CustomHLODActor)
            return TEXT("HLOD companion naming requires a finite default/parent layer chain with distinct names. Custom/linked layers and external layer packages need separate review.");
        Seen.Add(Layer); Names.Add(Layer->GetName());
        FHLODLayerBinding Binding; Binding.Layer = Layer; Binding.Parent = Layer->GetParentLayer();
        Binding.PackageName = Layer->GetPackage()->GetName(); Binding.ObjectName = Layer->GetName(); Out.Add(Binding);
    }
    return FString();
}
void ReviewHLODCompanions(UWorld* World, const FString& Root, const FString& Content, FMapSaveDestination& Destination)
{
    TArray<FHLODLayerBinding> Chain; Destination.Error = CaptureHLODLayerChain(World, Chain); if (!Destination.Error.IsEmpty()) return;
    for (const auto& Layer : Chain)
    {
        FMapSaveDestination::FHLODCompanion Entry; Entry.Source = Layer;
        // Use Unreal's DuplicateHLODLayersSetup naming, without invoking its
        // mutation until the whole destination set has been reviewed/reserved.
        Entry.ObjectName = FPackageName::GetShortName(Destination.Map.PackageName) + TEXT("_") + Layer.ObjectName;
        Entry.Target = ReviewAbsentDestination(Destination.Map.PackageName + TEXT("_") + Layer.ObjectName, Layer.PackageName, Root, Content, false);
        if (!Entry.Target.Error.IsEmpty()) { Destination.Error = TEXT("HLOD companion destination: ") + Entry.Target.Error; return; }
        Destination.HLODCompanions.Add(MoveTemp(Entry));
    }
}
FString CreateHLODCompanions(UWorld* World, const FMapSaveDestination& Destination)
{
    TArray<FHLODLayerBinding> Before; FString Error = CaptureHLODLayerChain(World, Before); if (!Error.IsEmpty()) return Error;
    if (Before.Num() != Destination.HLODCompanions.Num()) return TEXT("The HLOD default/parent chain changed after review.");
    for (int32 I = 0; I < Before.Num(); ++I) if (!(Before[I] == Destination.HLODCompanions[I].Source)) return TEXT("An HLOD layer binding changed after review.");
    if (Before.IsEmpty()) return FString();
    auto* Partition = World->GetWorldPartition();
    auto* NewRoot = UHLODLayer::DuplicateHLODLayersSetup(CastChecked<UHLODLayer>(Before[0].Layer.Get()), Destination.Map.PackageName, FPackageName::GetShortName(Destination.Map.PackageName));
    TMap<UHLODLayer*, UHLODLayer*> Replacements; UHLODLayer* New = NewRoot;
    for (int32 I = 0; I < Before.Num(); ++I)
    {
        const auto& Target = Destination.HLODCompanions[I];
        if (!New || New->GetPackage()->GetName() != Target.Target.PackageName || New->GetName() != Target.ObjectName)
            return TEXT("Unreal produced HLOD companions outside the reviewed destinations. No package was saved.");
        Replacements.Add(CastChecked<UHLODLayer>(Before[I].Layer.Get()), New);
        New->ClearFlags(RF_Transient); New->SetFlags(RF_Public | RF_Standalone); New->GetPackage()->MarkAsFullyLoaded(); New->MarkPackageDirty();
        FAssetRegistryModule::AssetCreated(New); New = New->GetParentLayer();
    }
    if (New) return TEXT("Unreal produced an extra HLOD parent outside the reviewed set. No package was saved.");
    Partition->SetDefaultHLODLayer(NewRoot);
    if (Partition->RuntimeHash) FArchiveReplaceObjectRef<UHLODLayer> Replace(Partition->RuntimeHash, Replacements, EArchiveReplaceObjectFlags::IgnoreOuterRef | EArchiveReplaceObjectFlags::IgnoreArchetypeRef);
    for (AActor* Actor : World->PersistentLevel->Actors) if (Actor)
        if (auto* Replacement = Replacements.Find(Actor->GetHLODLayer())) { Actor->SetHLODLayer(*Replacement); Actor->MarkPackageDirty(); }
    World->MarkPackageDirty(); return FString();
}
FString GatherOwnedHLODCompanions(UWorld* World, const FString& Root, const FString& Content, FPackageSavePaths::FOwner& Owner, FPackageSavePaths& Out, bool& bSaveMap)
{
    FString Error = CaptureHLODLayerChain(World, Owner.HLODLayers); if (!Error.IsEmpty()) return Error;
    const FString Prefix = Owner.Name + TEXT("_"); const FString ObjectPrefix = FPackageName::GetShortName(Owner.Name) + TEXT("_");
    bool bOwned = !Owner.HLODLayers.IsEmpty() && Owner.HLODLayers[0].PackageName.StartsWith(Prefix);
    for (int32 I = 0; I < Owner.HLODLayers.Num(); ++I)
    {
        const auto& Binding = Owner.HLODLayers[I];
        if (FPackageName::IsTempPackage(Binding.PackageName) || Binding.PackageName.StartsWith(Prefix) != bOwned)
            return TEXT("Temporary or mixed shared/map-owned HLOD layers need a new companion review before saving this map.");
        if (!bOwned) continue; // Shared named layers use the ordinary asset-save flow.
        if (Binding.ObjectName != ObjectPrefix + Binding.PackageName.Mid(Prefix.Len())) return TEXT("The map-owned HLOD layer name no longer matches its owning map.");
        UPackage* Package = Binding.Layer->GetPackage(); FString Filename;
        const bool bExists = FPackageName::DoesPackageExist(Binding.PackageName, &Filename);
        const bool bNew = Package->HasAnyPackageFlags(PKG_NewlyCreated);
        if (!bExists && !bNew) return TEXT("An existing HLOD companion file is missing. Restore/hydrate it before saving; it cannot become a first save.");
        if (bExists && bNew) return TEXT("A first-save HLOD companion destination is occupied; nothing was overwritten.");
        if (!bNew && !Package->IsDirty()) continue;
        if (bNew && !FPackageName::TryConvertLongPackageNameToFilename(Binding.PackageName, Filename, TEXT(".uasset"))) return TEXT("Cannot resolve the HLOD companion destination.");
        Filename = FPaths::ConvertRelativePathToFull(Filename);
        if ((bNew && (IFileManager::Get().FileExists(*Filename) || IFileManager::Get().DirectoryExists(*Filename))) ||
            !FPaths::IsUnderDirectory(Filename, Content) || !FPaths::IsUnderDirectory(Filename, Root)) return TEXT("The HLOD companion destination is occupied or outside this checkout's game Content.");
        FPackageSavePaths::FEntry Entry; Entry.Package = Package; Entry.PackageName = Binding.PackageName; Entry.Filename = Filename;
        Entry.Path = Filename.Mid(Root.Len() + 1);
        Entry.World = World; Entry.WorldName = Owner.Name; Entry.WorldFilename = Owner.Filename; Entry.WorldHash = Owner.Hash;
        Entry.Kind = FPackageSavePaths::EKind::HLODLayer; Entry.CompanionDepth = I; Out.Entries.Add(Entry); Out.Paths.AddUnique(Entry.Path);
        if (bNew) { Out.NewPaths.AddUnique(Entry.Path); bSaveMap = true; }
    }
    return FString();
}
}
