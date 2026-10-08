// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#include "GitWorkspaceSaveFlow.h"
#include "GitWorkspaceMapCopyLoad.h"
#include "GitSourceControlModule.h"
#include "GitSourceControlSettings.h"
#include "Async/Async.h"
#include "Editor.h"
#include "ContentBrowserModule.h"
#include "IContentBrowserSingleton.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Engine/World.h"
#include "Engine/Level.h"
#include "Engine/MapBuildDataRegistry.h"
#include "GameFramework/Actor.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "FileHelpers.h"
#include "ISourceControlModule.h"
#include "ISourceControlProvider.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Commands/UICommandList.h"
#include "Widgets/SWindow.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/MessageDialog.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopedSlowTask.h"
#include "Misc/ScopeExit.h"
#include "HAL/FileManager.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "WorldPartition/WorldPartition.h"
#if PLATFORM_MAC
#include "GitWorkspaceSession.h"
#include "GitWorkspaceFileGuard.h"
#endif

DEFINE_LOG_CATEGORY_STATIC(LogGitWorkspaceSave, Log, All);

namespace GitWorkspaceSave
{
namespace
{
bool bBusy = false;
FCoreUObjectDelegates::FIsPackageOKToSaveDelegate PreviousGuard;
FDelegateHandle GuardHandle;
#if PLATFORM_MAC
FDelegateHandle CleanupHandle, ProviderHandle;
#endif
TSharedPtr<GitWorkspace::FRepository, ESPMode::ThreadSafe> ProjectRepo;
FString ProjectRepositoryRoot;
FText SaveText(const FString& S) { return FText::FromString(S); }
#if PLATFORM_MAC
struct FActiveSave
{
    GitWorkspace::FRepository* Repo;
    const GitWorkspace::FAssetSavePermit* Permit;
    const GitWorkspaceSession::FLease* Lease;
    FString Root;
    TSet<FString> Attempted;
    const FPackageSavePaths* ActorPlan;
    FString ExpectedWrite;
    TFunction<FString(const FString&, UPackage*)> ValidateBatch;
};
TOptional<FActiveSave> ActiveSave;
#endif
struct FCommandOverride
{
    TWeakPtr<FUICommandList> List;
    TSharedPtr<const FUICommandInfo> Command;
    FUIAction Original;
    FDelegateHandle Handle;
};
TArray<FCommandOverride> Commands;
bool UsesWorkspace()
{
    return HandlesProvider(ISourceControlModule::Get().GetProvider());
}
bool Inside(const FString& File, const FString& Directory)
{ return FPaths::IsUnderDirectory(File, Directory); }
FString Relative(const FString& File, const FString& Root)
{
    FString Path = FPaths::ConvertRelativePathToFull(File); FPaths::MakePathRelativeTo(Path, *(Root + TEXT("/"))); return Path;
}
void Blocked(const FString& Error)
{
    FMessageDialog::Open(EAppMsgType::Ok, SaveText(TEXT("Save cancelled. Unsaved edits remain in the editor.\n\n") + Error), SaveText(TEXT("Git Workspace — save blocked")));
}
bool GuardSave(UPackage* Package, const FString& Filename, FOutputDevice* Output)
{
    if (PreviousGuard.IsBound() && !PreviousGuard.Execute(Package, Filename, Output)) return false;
    const FString File = FPaths::ConvertRelativePathToFull(Filename);
#if PLATFORM_MAC
    // Changing providers cannot turn a known unsafe editor state into a save permit.
    if (Inside(File, FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir())) &&
        GitWorkspaceSession::HasBlockedExternalCleanup(FPaths::ConvertRelativePathToFull(FPaths::ProjectDir())))
    {
        UE_LOG(LogGitWorkspaceSave, Warning, TEXT("Native cleanup recovery must be resolved with the editor closed before saving %s."), *Filename);
        return false;
    }
#endif
    if (!UsesWorkspace()) return true;
    // Never read the game-thread prepared scope from a background save.
    if (!IsInGameThread())
    {
        if (Inside(File, FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir())))
        {
            UE_LOG(LogGitWorkspaceSave, Warning, TEXT("Git Workspace requires a game-thread prepared asset save."));
            return false;
        }
        return true;
    }
#if PLATFORM_MAC
    if (!ActiveSave.IsSet() && ProjectRepositoryRoot.IsEmpty()) return true;
#else
    if (ProjectRepositoryRoot.IsEmpty()) return true;
#endif
    if (!File.EndsWith(TEXT(".uasset")) && !File.EndsWith(TEXT(".umap"))) return true;
    bool bReviewedExistingFile = false;
#if PLATFORM_MAC
    bReviewedExistingFile = ActiveSave.IsSet() && Inside(File, ActiveSave->Root) && ActiveSave->Permit->ContainsPath(Relative(File, ActiveSave->Root));
#endif
    // Both existing and first writes require a prepared destination. In
    // particular, Save As cannot silently write an unreviewed new lockable path.
    bool bInPreparedRepository = false;
#if PLATFORM_MAC
    bInPreparedRepository = ActiveSave.IsSet() && Inside(File, ActiveSave->Root);
#endif
    if (!bReviewedExistingFile && !bInPreparedRepository && !Inside(File, FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir()))) return true;
    GitWorkspace::FResult Result; Result.Code = 0;
#if PLATFORM_MAC
    FString RecoveryRoot, RecoveryGitDir;
    const FString SaveRoot = ActiveSave.IsSet() ? ActiveSave->Root : ProjectRepositoryRoot;
    if (GitWorkspaceSession::HasBlockedExternalCleanup(SaveRoot))
        Result.Error = TEXT("Native cleanup changed editor package state. Keep unsaved work in memory and resolve its recovery report with the editor closed before saving.");
    if (!SaveRoot.IsEmpty() && GitWorkspaceSession::FindRepository(SaveRoot, RecoveryRoot, RecoveryGitDir) &&
        IFileManager::Get().FileExists(*GitWorkspaceSession::RecoveryFile(RecoveryGitDir)))
        Result.Error = TEXT("Resolve active asset/repository recovery before saving. Your unsaved edits remain in the editor.");
    if (Result.Error.IsEmpty() && ActiveSave.IsSet() && ActiveSave->ActorPlan && Relative(File, ActiveSave->Root) != ActiveSave->ExpectedWrite)
        Result.Error = TEXT("Only the coordinator's current package write is authorized. A save callback cannot expand this batch.");
#endif
    const bool bExternal = File.Contains(TEXT("/__ExternalActors__/")) || File.Contains(TEXT("/__ExternalObjects__/"));
    if (Result.Error.IsEmpty() && (bExternal || NeedsCoordinatedWorldSave(Package)))
    {
#if PLATFORM_MAC
        const FString Path = ActiveSave.IsSet() ? Relative(File, ActiveSave->Root) : FString();
        if (!ActiveSave.IsSet() || !ActiveSave->ActorPlan || !ActiveSave->ValidateBatch || !ActiveSave->Permit->ContainsPath(Path) ||
            (bExternal && !ActiveSave->Permit->ContainsExternalActorPath(Path)))
            Result.Error = TEXT("External actors, their maps and build data require a coordinated save.");
#else
        Result.Error = TEXT("Coordinated external actor saves are currently available on Mac only.");
#endif
    }
#if PLATFORM_MAC
    if (Result.Error.IsEmpty() && ActiveSave.IsSet() && ActiveSave->ValidateBatch)
        Result.Error = ActiveSave->ValidateBatch(Relative(File, ActiveSave->Root), Package);
    if (Result.Error.IsEmpty() && ActiveSave.IsSet() && Inside(File, ActiveSave->Root))
    {
        const FString Path = Relative(File, ActiveSave->Root);
        auto* Repo = ActiveSave->Repo;
        if (ActiveSave->Permit->ContainsPath(Path))
        {
            if (ActiveSave->Attempted.Contains(Path)) Result.Error = TEXT("Asset is outside this prepared save attempt.");
            else
            {
                auto* Permit = ActiveSave->Permit; auto* Lease = ActiveSave->Lease;
                Result = Async(EAsyncExecution::ThreadPool, [Repo, Permit, Lease, Path] { return Repo->ValidateAssetSave(*Permit, Path, *Lease); }).Get();
                if (Result.Ok()) ActiveSave->Attempted.Add(Path);
            }
        }
        else
        {
            const auto Attribute = Async(EAsyncExecution::ThreadPool, [Repo, Path] { return Repo->IsLockableAsset(Path); }).Get();
            if (Attribute.Code == 1) return true;
            Result = Attribute;
            if (Result.Ok()) Result.Error = TEXT("Asset was not included in this prepared save.");
        }
    }
    else
#endif
    if (Result.Error.IsEmpty() && UsesWorkspace() && Inside(File, FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir())))
    {
        if (!ProjectRepo) Result.Error = TEXT("Git Workspace save protection is not ready.");
        else
        {
            auto Repo = ProjectRepo;
            const FString Path = Relative(File, ProjectRepositoryRoot);
            Result = Async(EAsyncExecution::ThreadPool, [Repo, Path] { return Repo->IsLockableAsset(Path); }).Get();
            if (Result.Code == 1) return true;
            if (Result.Ok()) Result.Error = TEXT("Use Save, Save All or Git Workspace Save assets to prepare this exact asset destination. Standard Blueprint, Texture2D, Material, Material Instance and Material Function editors also support reviewed Save As copies. Other naming/custom routes require separate integration; Make Writable cannot grant lock ownership.");
        }
    }
    if (!Result.Error.IsEmpty() || !Result.Ok())
    {
        // SavePackage may pass GError here; writing to that device can crash the
        // editor even for a deliberate refusal. Log normally and return false.
        UE_LOG(LogGitWorkspaceSave, Warning, TEXT("Git Workspace blocked saving %s: %s"), *Filename, *Result.Error);
        if (!IsRunningCommandlet() && IsInGameThread())
        {
            FNotificationInfo Notice(SaveText(TEXT("Save blocked: ") + Result.Error)); Notice.ExpireDuration = 12.f;
            FSlateNotificationManager::Get().AddNotification(Notice);
        }
        return false;
    }
    return true;
}
bool ConfirmSaveReview(const FString& Title, const FString& Text, const FString& Action, TFunction<bool()> CanConfirm = {})
{
    bool bConfirmed = false;
    const auto Window = SNew(SWindow).Title(SaveText(Title)).ClientSize(FVector2D(720, 480)).SupportsMinimize(false).SupportsMaximize(false);
    TWeakPtr<SWindow> Weak = Window;
    TSharedPtr<SButton> CancelButton;
    Window->SetContent(SNew(SBorder).Padding(12)[SNew(SVerticalBox)
        + SVerticalBox::Slot().FillHeight(1)
        [SNew(SMultiLineEditableTextBox).IsReadOnly(true).AutoWrapText(true).Text(SaveText(Text))]
        + SVerticalBox::Slot().AutoHeight().Padding(0, 12)
        [SNew(SHorizontalBox)
            + SHorizontalBox::Slot().AutoWidth()
            [SNew(SButton).Text(SaveText(Action)).IsEnabled_Lambda([CanConfirm] { return !CanConfirm || CanConfirm(); })
                .OnClicked_Lambda([&bConfirmed, Weak] { bConfirmed = true; if (auto W = Weak.Pin()) W->RequestDestroyWindow(); return FReply::Handled(); })]
            + SHorizontalBox::Slot().AutoWidth().Padding(12, 0)
            [SAssignNew(CancelButton, SButton).Text(SaveText(TEXT("Cancel"))).OnClicked_Lambda([Weak] { if (auto W = Weak.Pin()) W->RequestDestroyWindow(); return FReply::Handled(); })]
        ]
    ]);
    Window->SetWidgetToFocusOnActivate(CancelButton);
    FSlateApplication::Get().AddModalWindow(Window, FSlateApplication::Get().GetActiveTopLevelWindow());
    return bConfirmed;
}
bool ConfirmLocks(const GitWorkspace::FAssetSaveReview& Review, const FString& Introduction = FString())
{
    return ConfirmSaveReview(Review.DeletePaths.IsEmpty() ? TEXT("Lock assets before saving") : TEXT("Review actor deletions and saves"),
        Introduction + Review.Text(), Review.DeletePaths.IsEmpty() ? TEXT("Lock and save") : TEXT("Save and delete reviewed actors"), [Review] { return Review.IsFresh(); });
}
void ExecuteSaveAs(UObject* Source, UObject* EditedData, const FExecuteAction& OriginalAction)
{
    if (!UsesWorkspace()) { OriginalAction.ExecuteIfBound(); return; }
    if (bBusy) return;
    TGuardValue<bool> Busy(bBusy, true);
    if (!ProjectRepo) { Blocked(TEXT("Git Workspace save protection is not ready.")); return; }
    auto Repo = ProjectRepo;
    const auto Local = Async(EAsyncExecution::ThreadPool, [Repo] { return Repo->Refresh(); }).Get();
    if (Local.Root.IsEmpty()) { OriginalAction.ExecuteIfBound(); return; }
    if (!Local.bValid) { Blocked(Local.Error); return; }
    const FString DataError = ReviewCopyData(Source, EditedData);
    if (!DataError.IsEmpty()) { Blocked(DataError); return; }
    TStrongObjectPtr<UObject> HoldSource(Source);
    TStrongObjectPtr<UObject> HoldData(EditedData);
    const FString Content = FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir());
    FSaveAssetDialogConfig Config; Config.DialogTitleOverride = SaveText(TEXT("Save asset copy as"));
    Config.DefaultPath = FPackageName::GetLongPackagePath(Source->GetOutermost()->GetName());
    if (!Config.DefaultPath.StartsWith(TEXT("/Game"))) Config.DefaultPath = TEXT("/Game");
    Config.DefaultAssetName = Source->GetName() + TEXT("_Copy"); Config.AssetClassNames.Add(Source->GetClass()->GetClassPathName());
    Config.ExistingAssetPolicy = ESaveAssetDialogExistingAssetPolicy::Disallow;
    const FString ObjectPath = FModuleManager::LoadModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser")).Get().CreateModalSaveAssetDialog(Config);
    if (ObjectPath.IsEmpty()) return;
    const auto Destination = ReviewCopyDestination(Source, FPackageName::ObjectPathToPackageName(ObjectPath), Local.Root, Content);
    if (!Destination.Error.IsEmpty()) { Blocked(Destination.Error); return; }
    const FString Remote = LockRemote(), Path = Destination.Path;
    GitWorkspace::FAssetSaveReview Review;
    {
        FScopedSlowTask Task(1.f, SaveText(TEXT("Verifying the Save As destination and locks…"))); Task.MakeDialog(false); Task.EnterProgressFrame(1.f); Task.ForceRefresh();
        Review = Async(EAsyncExecution::ThreadPool, [Repo, Path, Remote] { return Repo->ReviewAssetSave({Path}, Remote, {Path}); }).Get();
    }
    if (!Review.IsFresh()) { Blocked(Review.Error); return; }
    if (Review.Paths.IsEmpty()) { Blocked(TEXT("Guarded Save As requires an LFS/lockable destination. Review this project's .gitattributes first.")); return; }
    // Always review the chosen copy destination, including a held absent-path reservation.
    if (!ConfirmLocks(Review, TEXT("Save As creates a new copy of ") + Source->GetPathName() + TEXT(".\nThe original stays open; its unsaved edits are not saved.\n\n"))) return;
#if PLATFORM_MAC
    GitWorkspaceSession::FEditorWriteScope Access; FString Error;
    if (!Access.Acquire(Review.Local.Root, Error)) { Blocked(Error); return; }
    GitWorkspace::FAssetSavePreparation Prepared;
    {
        FScopedSlowTask Task(1.f, SaveText(TEXT("Reserving the Save As destination before copying…"))); Task.MakeDialog(false); Task.EnterProgressFrame(1.f); Task.ForceRefresh();
        Prepared = Async(EAsyncExecution::ThreadPool, [Repo, Review, &Access] { return Repo->PrepareAssetSave(Review, Access.Lease(), true); }).Get();
    }
    if (!Prepared.Result.Ok() || !Prepared.Permit) { Blocked(Prepared.Result.Error); return; }
    UObject* Copy = nullptr;
    const auto Written = WriteAssetCopy(Source, Destination, *Repo, *Prepared.Permit, Access.Lease(), Content, Copy, HoldData.Get());
    // Keep the original editor and its edits open. A failed copy is also kept
    // in memory and can use the ordinary, already-locked Save retry.
    if (Copy && GEditor) GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()->OpenEditorForAsset(Copy);
    if (!Written.Ok()) { Blocked(Written.Error); return; }
    FNotificationInfo Notice(SaveText(TEXT("Asset copy saved and locked. The original stays open. Stage the new file when ready.")));
    Notice.ExpireDuration = 8.f; FSlateNotificationManager::Get().AddNotification(Notice);
#else
    Blocked(TEXT("Guarded Save As is currently available on Mac only."));
#endif
}
void ExecuteMapSaveAs(UWorld* World, const FExecuteAction& Original)
{
    if (!UsesWorkspace()) { Original.ExecuteIfBound(); return; }
    if (bBusy) return;
    TGuardValue<bool> Busy(bBusy, true);
    if (!ProjectRepo) { Blocked(TEXT("Git Workspace save protection is not ready.")); return; }
    auto Repo = ProjectRepo;
    const auto Local = Async(EAsyncExecution::ThreadPool, [Repo] { return Repo->Refresh(); }).Get();
    if (Local.Root.IsEmpty()) { Original.ExecuteIfBound(); return; }
    if (!Local.bValid) { Blocked(Local.Error); return; }
    const FString Content = FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir());
    FWPMapCopyLoadScope ActorLoad;
    ON_SCOPE_EXIT
    {
        const FString Error = ActorLoad.Release();
        if (!Error.IsEmpty()) FMessageDialog::Open(EAppMsgType::Ok, SaveText(Error), SaveText(TEXT("Git Workspace — WP copy loading")));
    };
    if (World && World->GetWorldPartition() && !FPackageName::IsTempPackage(World->GetPackage()->GetName()))
    {
        const auto Loading = ReviewWPMapCopyLoad(World, Local.Root, Content);
        if (!Loading.Error.IsEmpty()) { Blocked(Loading.Error); return; }
        if (!Loading.Missing.IsEmpty())
        {
            FString Text = FString::Printf(TEXT("Load %d missing WP actors to review a complete map copy?\n\nThe map stays open. This temporarily loads these actors and can increase memory use or run their construction scripts. It does not change loaded-region adapters, pins, source files or locks. Cancelling a later naming/lock review releases the temporary references. Actors edited by callbacks remain loaded so their unsaved work is kept.\n\n"), Loading.Missing.Num());
            for (const auto& Descriptor : Loading.Source.SourceDescriptors) if (Loading.Missing.Contains(Descriptor.Guid)) Text += Descriptor.Label + TEXT("\n") + Descriptor.Package + TEXT("\n\n");
            if (!ConfirmSaveReview(TEXT("Load WP actors to review copy"), Text, TEXT("Load actors"))) return;
            FString Error;
            {
                FScopedSlowTask Task(1.f, SaveText(TEXT("Loading the reviewed WP actors for the copy…"))); Task.MakeDialog(false); Task.EnterProgressFrame(1.f);
                Error = ActorLoad.Load(Loading, true);
            }
            if (!Error.IsEmpty()) { Blocked(Error); return; }
        }
    }
    const FString Error = ReviewMapSource(World); if (!Error.IsEmpty()) { Blocked(Error); return; }
    TStrongObjectPtr<UWorld> HoldWorld(World);
    const bool bNameCurrent = FPackageName::IsTempPackage(World->GetPackage()->GetName());
    FSaveAssetDialogConfig Config; Config.DialogTitleOverride = SaveText(bNameCurrent ? TEXT("Name and save current map") : TEXT("Save map copy as"));
    Config.DefaultPath = FPackageName::GetLongPackagePath(World->GetPackage()->GetName());
    if (!Config.DefaultPath.StartsWith(TEXT("/Game/")) && Config.DefaultPath != TEXT("/Game")) Config.DefaultPath = TEXT("/Game");
    Config.DefaultAssetName = bNameCurrent ? TEXT("L_NewMap") : World->GetName() + TEXT("_Copy");
    Config.AssetClassNames.Add(UWorld::StaticClass()->GetClassPathName()); Config.ExistingAssetPolicy = ESaveAssetDialogExistingAssetPolicy::Disallow;
    const FString ObjectPath = FModuleManager::LoadModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser")).Get().CreateModalSaveAssetDialog(Config);
    if (ObjectPath.IsEmpty()) return;
    const auto Destination = ReviewMapDestination(World, FPackageName::ObjectPathToPackageName(ObjectPath), Local.Root, Content);
    if (!Destination.Error.IsEmpty()) { Blocked(Destination.Error); return; }
    const auto Paths = Destination.Paths(); const FString Remote = LockRemote();
    GitWorkspace::FAssetSaveReview Review;
    {
        FScopedSlowTask Task(1.f, SaveText(TEXT("Verifying the complete map destination and locks…"))); Task.MakeDialog(false); Task.EnterProgressFrame(1.f); Task.ForceRefresh();
        Review = Async(EAsyncExecution::ThreadPool, [Repo, Paths, Remote, External = Destination.ExternalPaths()] { return Repo->ReviewAssetSave(Paths, Remote, Paths, External); }).Get();
    }
    if (!Review.IsFresh()) { Blocked(Review.Error); return; }
    if (Review.Paths.Num() != Paths.Num()) { Blocked(TEXT("Every map/actor/build-data destination must be LFS and lockable. Review .gitattributes first.")); return; }
    FString Introduction = bNameCurrent ? TEXT("This names and saves the current map and its build data. You keep editing this map.\n\n") :
        TEXT("This saves a new map copy and its build data. The original stays open with its unsaved edits. Open the copy from Content Browser to switch maps.\n\n");
    if (Destination.bExternalFirstSave || Destination.bExternalCopy)
    {
        Introduction = Destination.bExternalCopy ?
            TEXT("This copies the complete loaded WP/OFPA map, external actors and modern build data into new locked files. Copied actors receive new identities. The original stays open with its unsaved edits; temporary actor-load references are released afterwards. Its original loaded regions, pins, files and locks are preserved. Open the copy from Content Browser to switch maps. Locks remain held.\n\n") :
            TEXT("This names the current WP/OFPA map and remaps its external actors. Every new file is locked before naming; actors and build data are saved before the map. You keep editing this map. Locks remain held.\n\n");
        for (const auto& Actor : Destination.Actors) Introduction += TEXT("New actor: ") + Actor.Label + TEXT("\n") + Actor.Target.Path + TEXT("\n\n");
    }
    if (!ConfirmLocks(Review, Introduction)) return;
#if PLATFORM_MAC
    GitWorkspaceSession::FEditorWriteScope Access; FString AccessError;
    if (!Access.Acquire(Review.Local.Root, AccessError)) { Blocked(AccessError); return; }
    GitWorkspace::FAssetSavePreparation Prepared;
    {
        FScopedSlowTask Task(1.f, SaveText(TEXT("Reserving all map destinations before naming or copying…"))); Task.MakeDialog(false); Task.EnterProgressFrame(1.f); Task.ForceRefresh();
        Prepared = Async(EAsyncExecution::ThreadPool, [Repo, Review, &Access] { return Repo->PrepareAssetSave(Review, Access.Lease(), true); }).Get();
    }
    if (!Prepared.Result.Ok() || !Prepared.Permit)
    {
        for (const FString& Path : Prepared.AcquiredPaths) Prepared.Result.Error += TEXT("\nLock retained: ") + Path;
        Blocked(Prepared.Result.Error); return;
    }
    UWorld* WrittenWorld = nullptr;
    const auto Written = WriteMapDestination(World, Destination, *Repo, *Prepared.Permit, Access.Lease(), Content, WrittenWorld, [&ActorLoad] { return ActorLoad.Validate(); });
    if (!Written.Ok()) { Blocked(Written.Error); return; }
    FNotificationInfo Notice(SaveText(Destination.bNameCurrent ? TEXT("Current map named and saved. Locks remain held. Stage the new files when ready.") : TEXT("Map copy saved. The original stays open. Open the copy from Content Browser to switch maps. Locks remain held; stage when ready."))); Notice.ExpireDuration = 12.f; FSlateNotificationManager::Get().AddNotification(Notice);
#else
    Blocked(TEXT("Guarded map naming is currently available on Mac only."));
#endif
}
void ExecuteSave(const TArray<UPackage*>& Packages, const FExecuteAction& Original)
{
    if (!UsesWorkspace()) { Original.ExecuteIfBound(); return; }
    if (bBusy) return;
    TGuardValue<bool> Busy(bBusy, true);
    if (!ProjectRepo) { Blocked(TEXT("Git Workspace save protection is not ready.")); return; }
    auto Repo = ProjectRepo;
    const auto Local = Async(EAsyncExecution::ThreadPool, [Repo] { return Repo->Refresh(); }).Get();
    if (Local.Root.IsEmpty()) { Original.ExecuteIfBound(); return; }
    if (!Local.bValid) { Blocked(Local.Error); return; }
    const auto Destinations = GatherPackageSavePaths(Packages, Local.Root, FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir()));
    if (!Destinations.Error.IsEmpty()) { Blocked(Destinations.Error); return; }
    const auto& Paths = Destinations.Paths; const auto& NewPaths = Destinations.NewPaths;
    if (Paths.IsEmpty()) { if (!Destinations.bCoordinatedActors) Original.ExecuteIfBound(); return; }
    const FString Remote = LockRemote();
    GitWorkspace::FAssetSaveReview Review;
    {
        FScopedSlowTask Task(1.f, SaveText(TEXT("Verifying asset locks before saving…"))); Task.MakeDialog(false); Task.EnterProgressFrame(1.f); Task.ForceRefresh();
        Review = Async(EAsyncExecution::ThreadPool, [Repo, Paths, NewPaths, Remote, External = Destinations.ExternalActorPaths, Delete = Destinations.DeletePaths] { return Repo->ReviewAssetSave(Paths, Remote, NewPaths, External, Delete); }).Get();
    }
    if (!Review.IsFresh()) { Blocked(Review.Error); return; }
    if (Review.Paths.IsEmpty())
    { if (Destinations.bCoordinatedActors) Blocked(TEXT("Every package in a coordinated map save must use LFS and lockable attributes.")); else Original.ExecuteIfBound(); return; }
    FString Introduction;
    if (Destinations.bCoordinatedActors)
    {
        Introduction = TEXT("Saving the reviewed map, build data and actor edits. Clean files are not locked by this save.\nNew actor files are locked before their first save. Reviewed deleted actors get a durable recovery copy before removal. Locks remain held. External objects are not supported yet.\n\n");
        for (const auto& Entry : Destinations.Entries)
        {
            if (Entry.Kind == FPackageSavePaths::EKind::DeleteActor) Introduction += TEXT("DELETE actor: ") + Entry.ActorLabel + TEXT(" — ") + Entry.World->GetName() + TEXT("\n") + Entry.Path + TEXT("\nSaved working bytes will be backed up; staged versions remain unchanged.\n\n");
            else if (Entry.Actor.IsValid() && Entry.World.IsValid()) Introduction += (Destinations.NewPaths.Contains(Entry.Path) ? FString(TEXT("New actor: ")) : FString()) + Entry.Actor->GetActorLabel() + TEXT(" — ") + Entry.World->GetName() + TEXT("\n") + Entry.Path + TEXT("\n\n");
            else if (Entry.Kind == FPackageSavePaths::EKind::Map || Entry.Kind == FPackageSavePaths::EKind::BuildData)
                Introduction += FString(Entry.Kind == FPackageSavePaths::EKind::Map ? TEXT("Map: ") : TEXT("Build data: ")) + Entry.Path + TEXT("\n\n");
        }
    }
    if ((!Review.NeedsLock.IsEmpty() || !Review.DeletePaths.IsEmpty()) && !ConfirmLocks(Review, Introduction)) return;
#if PLATFORM_MAC
    GitWorkspaceSession::FEditorWriteScope Access; FString Error;
    if (!Access.Acquire(Review.Local.Root, Error)) { Blocked(Error); return; }
    GitWorkspace::FAssetSavePreparation Prepared;
    {
        FScopedSlowTask Task(1.f, SaveText(TEXT("Preparing verified locks for saving…"))); Task.MakeDialog(false); Task.EnterProgressFrame(1.f); Task.ForceRefresh();
        Prepared = Async(EAsyncExecution::ThreadPool, [Repo, Review, &Access] { return Repo->PrepareAssetSave(Review, Access.Lease(), true); }).Get();
    }
    if (!Prepared.Result.Ok() || !Prepared.Permit)
    {
        for (const auto& Path : Prepared.AcquiredPaths) Prepared.Result.Error += TEXT("\nLock retained: ") + Path;
        Blocked(Prepared.Result.Error); return;
    }
    const auto CurrentDestinations = GatherPackageSavePaths(Packages, Local.Root, FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir()));
    if (!CurrentDestinations.Error.IsEmpty() || CurrentDestinations.Paths != Paths || CurrentDestinations.NewPaths != NewPaths || CurrentDestinations.DeletePaths != Destinations.DeletePaths)
    { Blocked(TEXT("The package set changed after lock review. Review saving again. Locks remain held.")); return; }
    if (Destinations.bCoordinatedActors)
    {
        const auto Result = WriteExternalActorSave(Destinations, *Repo, *Prepared.Permit, Access.Lease(), FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir()));
        if (!Result.Ok())
        {
            FString Root, GitDir;
            if (GitWorkspaceSession::FindRepository(Local.Root, Root, GitDir) && IFileManager::Get().FileExists(*GitWorkspaceSession::RecoveryFile(GitDir)))
                Blocked(Result.Error + TEXT("\nThe editor stays open to preserve other unsaved edits. Saving and workspace changes are blocked until this recovery report is resolved: ") + GitWorkspaceSession::RecoveryFile(GitDir));
            else Blocked(Result.Error);
            return;
        }
        if (!Result.Text().IsEmpty()) FMessageDialog::Open(EAppMsgType::Ok, SaveText(Result.Text()), SaveText(TEXT("Actor deletion recovery")));
        FNotificationInfo Notice(SaveText(TEXT("Reviewed map/asset edits saved. Locks remain held; stage and commit when ready."))); Notice.ExpireDuration = 8.f;
        FSlateNotificationManager::Get().AddNotification(Notice); return;
    }
    FPreparedScope Permit(*Repo, *Prepared.Permit, Access.Lease(), Review.Local.Root);
    Original.ExecuteIfBound();
    FNotificationInfo Notice(SaveText(TEXT("Asset locks remain held. Saving does not stage files.")));
    Notice.ExpireDuration = 6.f;
    FSlateNotificationManager::Get().AddNotification(Notice);
#else
    Blocked(TEXT("Guarded Lock and save is currently available on Mac only."));
#endif
}
}
FPackageSavePaths GatherOrdinaryPackageSavePaths(const TArray<UPackage*>& Packages, const FString& Root, const FString& Content)
{
    FPackageSavePaths Out;
    for (UPackage* Package : Packages)
    {
        if (!Package) continue;
        if (UWorld* World = UWorld::FindWorldInPackage(Package))
        {
            const FString Error = ReviewMapSource(World); if (!Error.IsEmpty()) { Out.Error = Error; return Out; }
            if (FPackageName::IsTempPackage(Package->GetName()))
            { Out.Error = TEXT("Name the current ordinary map using Save Current Level first, then retry Save All."); return Out; }
            if (auto* Data = World->PersistentLevel->MapBuildData.Get())
            {
                const auto Companion = GatherOrdinaryPackageSavePaths({Data->GetPackage()}, Root, Content);
                if (!Companion.Error.IsEmpty()) { Out.Error = Companion.Error; return Out; }
                for (const FString& Path : Companion.Paths) Out.Paths.AddUnique(Path);
                for (const FString& Path : Companion.NewPaths) Out.NewPaths.AddUnique(Path);
            }
        }
        FString Filename;
        const bool bExists = FPackageName::DoesPackageExist(Package->GetName(), &Filename);
        if (!bExists && FPackageName::IsValidLongPackageName(Package->GetName(), false))
            FPackageName::TryConvertLongPackageNameToFilename(Package->GetName(), Filename, Package->ContainsMap() ? FPackageName::GetMapPackageExtension() : FPackageName::GetAssetPackageExtension());
        if (Filename.IsEmpty()) continue; // Naming dialogs still require their own integration; the writer fails closed.
        Filename = FPaths::ConvertRelativePathToFull(Filename);
        if (!Inside(Filename, Content)) continue;
        const FString Path = Relative(Filename, Root); Out.Paths.AddUnique(Path);
        if (!bExists || Package->HasAnyPackageFlags(PKG_NewlyCreated)) Out.NewPaths.AddUnique(Path);
    }
    return Out;
}
bool HandlesProvider(const ISourceControlProvider& Provider)
{
    return Provider.GetName() == NAME_None || Provider.GetName() == FName(TEXT("None")) || &Provider == &FGitSourceControlModule::Get().GetProvider();
}
FString LockRemote()
{
    FString Remote; GConfig->GetString(TEXT("GitWorkspace"), TEXT("LockRemote"), Remote, GEditorPerProjectIni);
    return Remote.IsEmpty() ? TEXT("origin") : Remote;
}
void SetLockRemote(const FString& Remote)
{ GConfig->SetString(TEXT("GitWorkspace"), TEXT("LockRemote"), *Remote, GEditorPerProjectIni); }
void ShowCleanupNotices()
{
#if PLATFORM_MAC
    for (const auto& Message : GitWorkspaceSession::TakeBlockedCleanupNotices())
        if (!IsRunningCommandlet()) FMessageDialog::Open(EAppMsgType::Ok, SaveText(Message), SaveText(TEXT("Git Workspace — native cleanup blocked")));
#endif
}
void InstallGuard()
{
    if (GuardHandle.IsValid()) return;
    PreviousGuard = FCoreUObjectDelegates::IsPackageOKToSaveDelegate;
    FCoreUObjectDelegates::IsPackageOKToSaveDelegate.BindStatic(&GuardSave);
    GuardHandle = FCoreUObjectDelegates::IsPackageOKToSaveDelegate.GetHandle();
    ProjectRepo = MakeShared<GitWorkspace::FRepository, ESPMode::ThreadSafe>(GitWorkspace::FindGitExecutable(FGitSourceControlModule::Get().AccessSettings().GetBinaryPath()), FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()));
    auto Repo = ProjectRepo; ProjectRepositoryRoot = Async(EAsyncExecution::ThreadPool, [Repo] { return Repo->Refresh(); }).Get().Root;
#if PLATFORM_MAC
    GitWorkspaceSession::EnableExternalCleanupGuard(UsesWorkspace());
    ProviderHandle = ISourceControlModule::Get().RegisterProviderChanged(FSourceControlProviderChanged::FDelegate::CreateLambda([](ISourceControlProvider&, ISourceControlProvider& New)
    { GitWorkspaceSession::EnableExternalCleanupGuard(HandlesProvider(New)); }));
    CleanupHandle = FEditorDelegates::OnPackageDeleted.AddLambda([](UPackage* Package)
    {
        if (!UsesWorkspace() || !Package) return;
        FString File;
        if (FPackageName::DoesPackageExist(Package->GetName(), &File) && GitWorkspaceSession::IsExternalCleanupProtected(File))
            GitWorkspaceSession::RecordBlockedExternalCleanup(File, TEXT("Unreviewed native package cleanup has already notified the registry; editor state needs reopening"));
    });
#endif
}
void RemoveGuard()
{
    if (!GuardHandle.IsValid()) return;
    if (FCoreUObjectDelegates::IsPackageOKToSaveDelegate.GetHandle() == GuardHandle) FCoreUObjectDelegates::IsPackageOKToSaveDelegate = PreviousGuard;
    GuardHandle.Reset(); PreviousGuard.Unbind(); ProjectRepo.Reset(); ProjectRepositoryRoot.Empty();
#if PLATFORM_MAC
    FEditorDelegates::OnPackageDeleted.Remove(CleanupHandle); CleanupHandle.Reset();
    ISourceControlModule::Get().UnregisterProviderChanged(ProviderHandle); ProviderHandle.Reset();
    GitWorkspaceSession::ClearVerifiedExternalReplacement();
#endif
}
#if PLATFORM_MAC
FPreparedScope::FPreparedScope(GitWorkspace::FRepository& Repo, const GitWorkspace::FAssetSavePermit& Permit, const GitWorkspaceSession::FLease& Lease, const FString& Root,
    const FPackageSavePaths* ActorPlan, TFunction<FString(const FString&, UPackage*)> ValidateBatch)
{
    check(IsInGameThread()); check(!ActiveSave.IsSet());
    ActiveSave.Emplace(FActiveSave{&Repo, &Permit, &Lease, FPaths::ConvertRelativePathToFull(Root), {}, ActorPlan, {}, MoveTemp(ValidateBatch)}); bInstalled = true;
}
FPreparedScope::~FPreparedScope() { if (bInstalled) { GitWorkspaceSession::ClearVerifiedExternalReplacement(); ActiveSave.Reset(); } }
void FPreparedScope::ExpectActorBatchWrite(const FString& Path)
{ check(IsInGameThread()); check(bInstalled && ActiveSave.IsSet() && ActiveSave->ActorPlan); GitWorkspaceSession::ClearVerifiedExternalReplacement(); ActiveSave->ExpectedWrite = Path; }
#endif
void WrapCommand(TSharedRef<FUICommandList> List, TSharedPtr<const FUICommandInfo> Command, TFunction<TArray<UPackage*>()> GetPackages)
{
    if (!Command) return;
    const FUIAction* Action = List->GetActionForCommand(Command);
    if (!Action || !Action->ExecuteAction.IsBound()) return;
    for (const auto& Entry : Commands) if (Entry.List.Pin() == List && Entry.Command == Command && Entry.Handle == Action->ExecuteAction.GetHandle()) return;
    const FUIAction Original = *Action; FUIAction Wrapper = Original;
    Wrapper.ExecuteAction = FExecuteAction::CreateLambda([GetPackages, Execute = Original.ExecuteAction] { ExecuteSave(GetPackages(), Execute); });
    Commands.Add({List, Command, Original, Wrapper.ExecuteAction.GetHandle()}); List->MapAction(Command, Wrapper);
}
void WrapSaveAsCommand(TSharedRef<FUICommandList> List, TSharedPtr<const FUICommandInfo> Command, TFunction<UObject*()> GetSource, TFunction<UObject*()> GetEditedData)
{
    if (!Command) return;
    const FUIAction* Action = List->GetActionForCommand(Command);
    if (!Action || !Action->ExecuteAction.IsBound()) return;
    for (const auto& Entry : Commands) if (Entry.List.Pin() == List && Entry.Command == Command && Entry.Handle == Action->ExecuteAction.GetHandle()) return;
    const FUIAction Original = *Action; FUIAction Wrapper = Original;
    Wrapper.ExecuteAction = FExecuteAction::CreateLambda([GetSource, GetEditedData, Execute = Original.ExecuteAction]
    { UObject* Source = GetSource(); ExecuteSaveAs(Source, GetEditedData ? GetEditedData() : Source, Execute); });
    Commands.Add({List, Command, Original, Wrapper.ExecuteAction.GetHandle()}); List->MapAction(Command, Wrapper);
}
void WrapMapCommand(TSharedRef<FUICommandList> List, TSharedPtr<const FUICommandInfo> Command, TFunction<UWorld*()> GetWorld, bool bSaveAs)
{
    if (!Command) return;
    const FUIAction* Action = List->GetActionForCommand(Command);
    if (!Action || !Action->ExecuteAction.IsBound()) return;
    for (const auto& Entry : Commands) if (Entry.List.Pin() == List && Entry.Command == Command && Entry.Handle == Action->ExecuteAction.GetHandle()) return;
    const FUIAction Original = *Action; FUIAction Wrapper = Original;
    Wrapper.ExecuteAction = FExecuteAction::CreateLambda([GetWorld, bSaveAs, Execute = Original.ExecuteAction]
    {
        UWorld* World = GetWorld();
        if (bSaveAs || (World && FPackageName::IsTempPackage(World->GetPackage()->GetName()))) ExecuteMapSaveAs(World, Execute);
        else ExecuteSave(World ? TArray<UPackage*>{World->GetPackage()} : TArray<UPackage*>{}, Execute);
    });
    Commands.Add({List, Command, Original, Wrapper.ExecuteAction.GetHandle()}); List->MapAction(Command, Wrapper);
}
void RestoreCommands()
{
    // Restore in reverse order if an editor remapped its command after opening.
    for (int32 I = Commands.Num() - 1; I >= 0; --I)
    {
        const auto& Entry = Commands[I]; if (auto List = Entry.List.Pin())
            if (const auto* Action = List->GetActionForCommand(Entry.Command); Action && Action->ExecuteAction.GetHandle() == Entry.Handle) List->MapAction(Entry.Command, Entry.Original);
    }
    Commands.Empty();
}
void SaveDirtyAssets()
{
    TArray<UPackage*> Packages; FEditorFileUtils::GetDirtyContentPackages(Packages); FEditorFileUtils::GetDirtyWorldPackages(Packages);
    ExecuteSave(Packages, FExecuteAction::CreateLambda([] { FEditorFileUtils::SaveDirtyPackages(false, true, true); }));
}
}
