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
namespace
{
FEditorPullResult RunWithPackageReload(const FString& Root, const TArray<FIncomingChange>& Changes,
    const GitWorkspaceSession::FLease& Lease, TFunctionRef<FResult()> Execute, TFunctionRef<FResult()> Complete, bool bAllowRemoval = false)
{
    check(IsInGameThread()); FEditorPullResult Result; TArray<FIncomingPackage> Packages;
    Result.Message = ReviewPackageChanges(Root, Changes, Packages, bAllowRemoval);
    if (!Result.Message.IsEmpty()) return Result;
    if (!Lease.IsExclusiveFor(Root) || !GitWorkspaceSession::NoOtherEditors(0, Result.Message))
    { Result.Message = TEXT("Exclusive editor access is required. ") + Result.Message; return Result; }
    FString CanonicalRoot, GitDir;
    if (!GitWorkspaceSession::FindRepository(Root, CanonicalRoot, GitDir)) { Result.Message = TEXT("Cannot locate recovery storage."); return Result; }
    const FString Recovery = GitWorkspaceSession::RecoveryFile(GitDir);
    if (IFileManager::Get().FileExists(*Recovery)) { Result.Message = TEXT("Resolve the existing recovery report first: ") + Recovery; return Result; }
    // Match Unreal's source-control reload preparation: finish outstanding
    // loads and detach linkers before replacing any backing package files.
    FlushAsyncLoading();
    for (const auto& Item : Packages)
        if (UPackage* Package = FindPackage(nullptr, *Item.PackageName)) Package->FullyLoad();
    FlushAsyncLoading();
    Result.Message = ReviewPackageChanges(Root, Changes, Packages, bAllowRemoval);
    if (!Result.Message.IsEmpty()) return Result;
    // New saved assets may disappear only after the package and all references
    // have actually gone. UnloadPackages returning true is not that guarantee.
    TArray<UPackage*> ToUnload;
    for (const auto& Item : Packages) if (Item.Status == 'D')
        if (UPackage* Package = FindPackage(nullptr, *Item.PackageName)) ToUnload.Add(Package);
    if (!ToUnload.IsEmpty())
    {
        UPackageTools::FUnloadPackageParams Params(ToUnload);
        UPackageTools::UnloadPackages(Params);
        for (const auto& Item : Packages) if (Item.Status == 'D' && FindPackage(nullptr, *Item.PackageName))
        {
            Result.Message = TEXT("A reference is keeping this new asset in memory: ") + Item.PackageName +
                TEXT(". Its saved file is intact. Close referencing editors or resolve references before stashing; Undo and selection may have reset.");
            return Result;
        }
        Result.Unloaded = ToUnload.Num();
    }
    // Unload observers may change editor state. Refuse any newly dirty package
    // before touching files, and reacquire live pointers after garbage collection.
    Result.Message = ReviewPackageChanges(Root, Changes, Packages, bAllowRemoval);
    if (!Result.Message.IsEmpty()) return Result;
    TArray<UPackage*> Loaded;
    TArray<FString> Files, RemovedFiles;
    TSet<FString> Expected, Reloaded;
    for (const auto& Item : Packages)
    {
        const FString Filename = FPackageName::LongPackageNameToFilename(Item.PackageName, TEXT(".uasset"));
        if (Item.Status == 'D') { RemovedFiles.Add(Filename); continue; }
        Files.Add(Filename);
        if (UPackage* Package = FindPackage(nullptr, *Item.PackageName))
        { Loaded.Add(Package); Expected.Add(Item.PackageName); }
    }
    for (UPackage* Package : Loaded) ResetLoaders(Package);

    // Git runs on a worker, but the game thread waits without pumping Slate or
    // editor ticks. No user save, autosave or package load can interleave here.
    const auto Integrated = Async(EAsyncExecution::ThreadPool, [&] { return Execute(); }).Get();
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
    if (!Files.IsEmpty()) Registry.ScanFilesSynchronous(Files, true);
    if (!RemovedFiles.IsEmpty()) Registry.ScanModifiedAssetFiles(RemovedFiles);
    for (const auto& Item : Packages)
    {
        TArray<FAssetData> Assets;
        Registry.GetAssetsByPackageName(*Item.PackageName, Assets, true);
        if (Item.Status == 'D')
        {
            if (!Assets.IsEmpty() || FindPackage(nullptr, *Item.PackageName))
            { Result.Message = TEXT("Could not confirm removed asset left the editor: ") + Item.PackageName; return Result; }
            continue;
        }
        if (Assets.IsEmpty()) { Result.Message = TEXT("Content Browser could not confirm the incoming asset: ") + Item.PackageName; return Result; }
    }
    const auto Completed = Async(EAsyncExecution::ThreadPool, [&] { return Complete(); }).Get();
    if (!Completed.Ok()) { Result.Message = Completed.Error; return Result; }
    Result.bSuccess = true; Result.bRecoveryRequired = false;
    Result.Reloaded = Expected.Num(); Result.Refreshed = Packages.Num();
    Result.Message = FString::Printf(TEXT("%d assets refreshed, %d loaded packages reloaded, %d new packages unloaded. Locks retained."), Result.Refreshed, Result.Reloaded, Result.Unloaded);
    return Result;
}
}
FEditorPullResult PullAndReload(FRepository& Repository, const FRemoteSnapshot& Reviewed,
    const FIncomingLfsResult& Prepared, const GitWorkspaceSession::FLease& Lease)
{
    FEditorPullResult Result;
    const auto Review = ReviewIncoming(Reviewed, Repository.Refresh());
    if (!Review.bCanReload) { Result.Message = Review.ReloadBlocker; return Result; }
    if (!Prepared.Matches(Reviewed)) { Result.Message = TEXT("The LFS preparation expired. Fetch and review again."); return Result; }
    Result = RunWithPackageReload(Reviewed.Root, Reviewed.IncomingChanges, Lease,
        [&] { return Repository.PullForReload(Reviewed, Prepared, Lease); }, [&] { return Repository.CompleteReloadPull(Reviewed, Lease); });
    if (Result.bSuccess) Result.Message = TEXT("Pulled the reviewed commit. ") + Result.Message;
    return Result;
}
FEditorPullResult StashAndReload(FRepository& Repository, const FStashReview& Reviewed, const FString& Name, const GitWorkspaceSession::FLease& Lease)
{
    FEditorPullResult Result;
    if (!Reviewed.IsFresh()) { Result.Message = TEXT("Stash review expired. Review again."); return Result; }
    Result = RunWithPackageReload(Reviewed.Local.Root, Reviewed.Changes, Lease,
        [&] { return Repository.ExecuteStash(Reviewed, Name, Lease); }, [&] { return Repository.CompleteStash(Reviewed, Lease); }, Reviewed.bCreate);
    if (Result.bSuccess) Result.Message = (Reviewed.bCreate ? TEXT("Named stash created. Reviewed files preserved in the stash. ") : TEXT("Stash applied and retained. ")) + Result.Message;
    return Result;
}
}
#endif
