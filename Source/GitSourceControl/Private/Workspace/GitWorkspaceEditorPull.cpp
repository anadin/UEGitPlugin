// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#include "GitWorkspaceEditorPull.h"
#if PLATFORM_MAC
#include "GitWorkspacePullReview.h"
#include "GitWorkspaceSession.h"
#include "Async/Async.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "HAL/FileManager.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "PackageTools.h"
#include "UObject/Linker.h"
#include "UObject/Package.h"
#include "UObject/PackageReload.h"
#include "UObject/UObjectGlobals.h"

namespace GitWorkspace
{
FEditorPullResult PullAndReload(FRepository& Repository, const FRemoteSnapshot& Reviewed,
    const FIncomingLfsResult& Prepared, const GitWorkspaceSession::FLease& Lease)
{
    check(IsInGameThread()); FEditorPullResult Result;
    auto Review = ReviewIncoming(Reviewed, Repository.Refresh());
    if (!Review.bCanReload) { Result.Message = Review.ReloadBlocker; return Result; }
    if (!Prepared.Matches(Reviewed)) { Result.Message = TEXT("The LFS preparation expired. Fetch and review again."); return Result; }
    if (!Lease.IsExclusiveFor(Reviewed.Root) || !GitWorkspaceSession::NoOtherEditors(0, Result.Message))
    { Result.Message = TEXT("Exclusive editor access is required. ") + Result.Message; return Result; }
    FString CanonicalRoot, GitDir;
    if (!GitWorkspaceSession::FindRepository(Reviewed.Root, CanonicalRoot, GitDir)) { Result.Message = TEXT("Cannot locate recovery storage."); return Result; }
    const FString Recovery = GitWorkspaceSession::RecoveryFile(GitDir);
    if (IFileManager::Get().FileExists(*Recovery)) { Result.Message = TEXT("Resolve the existing recovery report before pulling: ") + Recovery; return Result; }

    // Match Unreal's source-control reload preparation: finish outstanding
    // loads and detach linkers before replacing any backing package files.
    FlushAsyncLoading();
    for (const auto& Item : Review.Packages)
        if (UPackage* Package = FindPackage(nullptr, *Item.PackageName)) Package->FullyLoad();
    FlushAsyncLoading();
    Review = ReviewIncoming(Reviewed, Repository.Refresh());
    if (!Review.bCanReload) { Result.Message = Review.ReloadBlocker; return Result; }
    TArray<UPackage*> Loaded;
    TArray<FString> Files;
    TSet<FString> Expected, Reloaded;
    for (const auto& Item : Review.Packages)
    {
        Files.Add(FPackageName::LongPackageNameToFilename(Item.PackageName, TEXT(".uasset")));
        if (UPackage* Package = FindPackage(nullptr, *Item.PackageName))
        { Loaded.Add(Package); Expected.Add(Item.PackageName); }
    }
    for (UPackage* Package : Loaded) ResetLoaders(Package);

    // Git runs on a worker, but the game thread waits without pumping Slate or
    // editor ticks. No user save, autosave or package load can interleave here.
    const auto Integrated = Async(EAsyncExecution::ThreadPool, [&] { return Repository.PullForReload(Reviewed, Prepared, Lease); }).Get();
    Result.bRecoveryRequired = Integrated.Ok() || IFileManager::Get().FileExists(*Recovery);
    if (!Integrated.Ok()) { Result.Message = Integrated.Error; return Result; }

    // UPackageTools' bool means "some packages changed", not "all succeeded".
    // Require a successful reload event for every originally loaded package.
    const auto Delegate = FCoreUObjectDelegates::OnPackageReloaded.AddLambda([&](EPackageReloadPhase Phase, FPackageReloadedEvent* Event)
    {
        if (Phase == EPackageReloadPhase::PostPackageFixup && Event && Event->GetNewPackage())
            Reloaded.Add(Event->GetNewPackage()->GetName());
    });
    ON_SCOPE_EXIT { FCoreUObjectDelegates::OnPackageReloaded.Remove(Delegate); };
    FText ReloadError;
    if (!Loaded.IsEmpty()) UPackageTools::ReloadPackages(Loaded, ReloadError, EReloadPackagesInteractionMode::AssumeNegative);
    if (!ReloadError.IsEmpty()) { Result.Message = TEXT("Assets changed on disk, but reload failed: ") + ReloadError.ToString(); return Result; }
    for (const auto& Name : Expected)
    {
        UPackage* Package = FindPackage(nullptr, *Name);
        if (!Reloaded.Contains(Name) || !Package || !Package->FindAssetInPackage())
        { Result.Message = TEXT("Could not verify a fresh loaded asset: ") + Name; return Result; }
    }
    auto& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
    Registry.ScanFilesSynchronous(Files, true);
    for (const auto& Item : Review.Packages)
    {
        TArray<FAssetData> Assets;
        Registry.GetAssetsByPackageName(*Item.PackageName, Assets, true);
        if (Assets.IsEmpty()) { Result.Message = TEXT("Content Browser could not confirm the incoming asset: ") + Item.PackageName; return Result; }
    }
    const auto Completed = Async(EAsyncExecution::ThreadPool, [&] { return Repository.CompleteReloadPull(Reviewed, Lease); }).Get();
    if (!Completed.Ok()) { Result.Message = Completed.Error; return Result; }
    Result.bSuccess = true; Result.bRecoveryRequired = false;
    Result.Reloaded = Expected.Num(); Result.Refreshed = Review.Packages.Num();
    Result.Message = FString::Printf(TEXT("Pulled the reviewed commit. %d assets refreshed, %d loaded packages reloaded. Locks retained."), Result.Refreshed, Result.Reloaded);
    return Result;
}
}
#endif
