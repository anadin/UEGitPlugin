// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#pragma once
#include "GitWorkspaceRepository.h"

namespace GitWorkspace
{
struct FIncomingPackage
{
    FString Path, PackageName;
    bool bMounted = false, bLoaded = false, bDirty = false, bMap = false, bExternal = false;
};
struct FPullReview
{
    FString Text, Blocker;
    TArray<FIncomingPackage> Packages;
    bool bCanPull = false;
};
// Game thread only. Observes package state without loading, saving or unloading.
FPullReview ReviewIncoming(const FRemoteSnapshot& Remote, const FSnapshot& Local);
}
