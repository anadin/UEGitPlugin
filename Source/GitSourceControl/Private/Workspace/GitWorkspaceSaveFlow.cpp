// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#include "GitWorkspaceSaveFlow.h"
#include "GitSourceControlModule.h"
#include "GitSourceControlSettings.h"
#include "Async/Async.h"
#include "Editor.h"
#include "Engine/World.h"
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
#include "HAL/FileManager.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/Package.h"
#if PLATFORM_MAC
#include "GitWorkspaceSession.h"
#endif

DEFINE_LOG_CATEGORY_STATIC(LogGitWorkspaceSave, Log, All);

namespace GitWorkspaceSave
{
namespace
{
bool bBusy = false;
FCoreUObjectDelegates::FIsPackageOKToSaveDelegate PreviousGuard;
FDelegateHandle GuardHandle;
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
    if (!UsesWorkspace()) return true;
    const FString File = FPaths::ConvertRelativePathToFull(Filename);
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
    if (Inside(File, FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir())) &&
        (File.Contains(TEXT("/__ExternalActors__/")) || File.Contains(TEXT("/__ExternalObjects__/"))))
    {
        UE_LOG(LogGitWorkspaceSave, Warning, TEXT("Git Workspace requires a coordinated external package save workflow."));
        return false;
    }
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
    if (ActiveSave.IsSet() && Inside(File, ActiveSave->Root))
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
    if (UsesWorkspace() && Inside(File, FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir())))
    {
        if (!ProjectRepo) Result.Error = TEXT("Git Workspace save protection is not ready.");
        else
        {
            auto Repo = ProjectRepo;
            const FString Path = Relative(File, ProjectRepositoryRoot);
            Result = Async(EAsyncExecution::ThreadPool, [Repo, Path] { return Repo->IsLockableAsset(Path); }).Get();
            if (Result.Code == 1) return true;
            if (Result.Ok()) Result.Error = TEXT("Use Save, Save All or Git Workspace Save assets to prepare this exact asset destination. Name new assets in game Content before saving. Save As destinations need separate integration; Make Writable cannot grant lock ownership.");
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
bool ConfirmLocks(const GitWorkspace::FAssetSaveReview& Review)
{
    bool bConfirmed = false;
    const auto Window = SNew(SWindow).Title(SaveText(TEXT("Lock assets before saving"))).ClientSize(FVector2D(720, 480)).SupportsMinimize(false).SupportsMaximize(false);
    TWeakPtr<SWindow> Weak = Window;
    TSharedPtr<SButton> CancelButton;
    Window->SetContent(SNew(SBorder).Padding(12)[SNew(SVerticalBox)
        + SVerticalBox::Slot().FillHeight(1)
        [SNew(SMultiLineEditableTextBox).IsReadOnly(true).AutoWrapText(true).Text(SaveText(Review.Text()))]
        + SVerticalBox::Slot().AutoHeight().Padding(0, 12)
        [SNew(SHorizontalBox)
            + SHorizontalBox::Slot().AutoWidth()
            [SNew(SButton).Text(SaveText(TEXT("Lock and save"))).IsEnabled_Lambda([Review] { return Review.IsFresh(); })
                .OnClicked_Lambda([&bConfirmed, Weak] { bConfirmed = true; if (auto W = Weak.Pin()) W->RequestDestroyWindow(); return FReply::Handled(); })]
            + SHorizontalBox::Slot().AutoWidth().Padding(12, 0)
            [SAssignNew(CancelButton, SButton).Text(SaveText(TEXT("Cancel"))).OnClicked_Lambda([Weak] { if (auto W = Weak.Pin()) W->RequestDestroyWindow(); return FReply::Handled(); })]
        ]
    ]);
    Window->SetWidgetToFocusOnActivate(CancelButton);
    FSlateApplication::Get().AddModalWindow(Window, FSlateApplication::Get().GetActiveTopLevelWindow());
    return bConfirmed;
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
    if (Paths.IsEmpty()) { Original.ExecuteIfBound(); return; }
    const FString Remote = LockRemote();
    GitWorkspace::FAssetSaveReview Review;
    {
        FScopedSlowTask Task(1.f, SaveText(TEXT("Verifying asset locks before saving…"))); Task.MakeDialog(false); Task.EnterProgressFrame(1.f); Task.ForceRefresh();
        Review = Async(EAsyncExecution::ThreadPool, [Repo, Paths, NewPaths, Remote] { return Repo->ReviewAssetSave(Paths, Remote, NewPaths); }).Get();
    }
    if (!Review.IsFresh()) { Blocked(Review.Error); return; }
    if (Review.Paths.IsEmpty()) { Original.ExecuteIfBound(); return; }
    if (!Review.NeedsLock.IsEmpty() && !ConfirmLocks(Review)) return;
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
FPackageSavePaths GatherPackageSavePaths(const TArray<UPackage*>& Packages, const FString& Root, const FString& Content)
{
    FPackageSavePaths Out;
    for (UPackage* Package : Packages)
    {
        if (!Package) continue;
        if (UWorld* World = UWorld::FindWorldInPackage(Package); World && World->GetWorldPartition())
        { Out.Error = TEXT("World Partition saves need a coordinated external actor/object lock workflow. Save cancelled."); return Out; }
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
void InstallGuard()
{
    if (GuardHandle.IsValid()) return;
    PreviousGuard = FCoreUObjectDelegates::IsPackageOKToSaveDelegate;
    FCoreUObjectDelegates::IsPackageOKToSaveDelegate.BindStatic(&GuardSave);
    GuardHandle = FCoreUObjectDelegates::IsPackageOKToSaveDelegate.GetHandle();
    ProjectRepo = MakeShared<GitWorkspace::FRepository, ESPMode::ThreadSafe>(GitWorkspace::FindGitExecutable(FGitSourceControlModule::Get().AccessSettings().GetBinaryPath()), FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()));
    auto Repo = ProjectRepo; ProjectRepositoryRoot = Async(EAsyncExecution::ThreadPool, [Repo] { return Repo->Refresh(); }).Get().Root;
}
void RemoveGuard()
{
    if (!GuardHandle.IsValid()) return;
    if (FCoreUObjectDelegates::IsPackageOKToSaveDelegate.GetHandle() == GuardHandle) FCoreUObjectDelegates::IsPackageOKToSaveDelegate = PreviousGuard;
    GuardHandle.Reset(); PreviousGuard.Unbind(); ProjectRepo.Reset(); ProjectRepositoryRoot.Empty();
}
#if PLATFORM_MAC
FPreparedScope::FPreparedScope(GitWorkspace::FRepository& Repo, const GitWorkspace::FAssetSavePermit& Permit, const GitWorkspaceSession::FLease& Lease, const FString& Root)
{
    check(IsInGameThread()); check(!ActiveSave.IsSet());
    ActiveSave.Emplace(FActiveSave{&Repo, &Permit, &Lease, FPaths::ConvertRelativePathToFull(Root), {}}); bInstalled = true;
}
FPreparedScope::~FPreparedScope() { if (bInstalled) ActiveSave.Reset(); }
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
