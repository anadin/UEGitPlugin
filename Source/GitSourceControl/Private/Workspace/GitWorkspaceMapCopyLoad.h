// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#pragma once
#include "GitWorkspaceSaveFlow.h"

namespace GitWorkspaceSave
{
struct FWPMapCopyLoadReview
{
    struct FActor
    {
        TWeakObjectPtr<AActor> Actor;
        FGuid Guid;
        FString Path, Label, Package;
    };
    struct FLoader
    {
        TWeakObjectPtr<UObject> Object;
        const void* Adapter = nullptr;
        TOptional<FBox> Bounds;
        bool bLoaded = false, bUserCreated = false;
        bool operator==(const FLoader& Other) const
        { return Object == Other.Object && Adapter == Other.Adapter && Bounds == Other.Bounds && bLoaded == Other.bLoaded && bUserCreated == Other.bUserCreated; }
    };
    TWeakObjectPtr<UWorld> World;
    FMapSaveDestination Source;
    TArray<FActor> LoadedActors;
    TArray<FLoader> Loaders;
    TMap<FGuid, bool> Pins;
    TArray<FGuid> Missing;
    FString Root, Content, Error;
};
FWPMapCopyLoadReview ReviewWPMapCopyLoad(UWorld* World, const FString& Root, const FString& Content);
// Owns only the missing actors' hard references. Never changes region adapters,
// pins, source packages or dirty flags. Release explicitly to report drift.
class FWPMapCopyLoadScope
{
public:
    FWPMapCopyLoadScope();
    ~FWPMapCopyLoadScope();
    FString Load(const FWPMapCopyLoadReview& Review, bool bConfirmed);
    FString Validate() const;
    FString Release();
private:
    struct FState;
    TUniquePtr<FState> State;
};
bool HasRetainedWPMapCopyLoads(UWorld* World);
void ShutdownWPMapCopyLoads();
}
