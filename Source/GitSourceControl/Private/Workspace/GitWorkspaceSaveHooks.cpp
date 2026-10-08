// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#include "GitWorkspaceSaveFlow.h"
#include "GitWorkspaceMapCopyLoad.h"
#include "Editor.h"
#include "FileHelpers.h"
#include "Containers/Ticker.h"
#include "Framework/Commands/InputBindingManager.h"
#include "Framework/Commands/UICommandList.h"
#include "Toolkits/ToolkitManager.h"
#include "Toolkits/AssetEditorToolkit.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Interfaces/IMainFrameModule.h"
#include "ContentBrowserModule.h"
#include "ContentBrowserDelegates.h"
#include "AssetRegistry/AssetData.h"
#include "Modules/ModuleManager.h"
#include "IMaterialEditor.h"
#include "Materials/Material.h"
#include "LevelEditor.h"
#include "ILevelEditor.h"
#include "Engine/World.h"

namespace GitWorkspaceSave
{
UObject* GetCopySource(const FAssetEditorToolkit& Editor)
{
    // EditingObjects also includes the material preview and helper. Count
    // persistent assets using the public API; refuse multiple real assets.
    UObject* Source = nullptr;
    if (const auto* Objects = Editor.GetObjectsCurrentlyBeingEdited())
        for (UObject* Object : *Objects)
            if (Object && Object->IsAsset() && !Object->HasAnyFlags(RF_Transient) && Object->GetOutermost() != GetTransientPackage())
            { if (Source && Source != Object) return nullptr; Source = Object; }
    return Source;
}
UObject* GetCopyData(FAssetEditorToolkit& Editor)
{
    if (Editor.GetToolkitFName() == FName(TEXT("MaterialEditor"))) return static_cast<IMaterialEditor&>(Editor).GetMaterialInterface();
    // Instances edit their source directly; function-instance proxies are
    // excluded by SupportsAssetCopy. Never substitute their preview instance.
    return GetCopySource(Editor);
}
namespace
{
FTSTicker::FDelegateHandle TickHandle;
FDelegateHandle BrowserHandle;
bool bInitialized = false;
TArray<UPackage*> DirtyPackages()
{
    TArray<UPackage*> Packages; FEditorFileUtils::GetDirtyContentPackages(Packages); FEditorFileUtils::GetDirtyWorldPackages(Packages); return Packages;
}
bool UpdateHooks(float)
{
    ShowCleanupNotices();
    if (!GEditor) return true;
    if (!bInitialized) { InstallGuard(); bInitialized = true; }
    if (auto* MainFrame = FModuleManager::GetModulePtr<IMainFrameModule>(TEXT("MainFrame")))
        WrapCommand(MainFrame->GetMainFrameCommandBindings(), FInputBindingManager::Get().FindCommandInContext(TEXT("MainFrame"), TEXT("SaveAll")), &DirtyPackages);
    if (auto* LevelEditor = FModuleManager::GetModulePtr<FLevelEditorModule>(TEXT("LevelEditor")))
    {
        auto Hook = [](TSharedRef<FUICommandList> List)
        {
            auto CurrentWorld = []() -> UWorld* { return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr; };
            WrapMapCommand(List, FInputBindingManager::Get().FindCommandInContext(TEXT("LevelEditor"), TEXT("Save")), CurrentWorld, false);
            WrapMapCommand(List, FInputBindingManager::Get().FindCommandInContext(TEXT("LevelEditor"), TEXT("SaveAs")), CurrentWorld, true);
            WrapCommand(List, FInputBindingManager::Get().FindCommandInContext(TEXT("LevelEditor"), TEXT("SaveAllLevels")), []
            { TArray<UPackage*> Packages; FEditorFileUtils::GetDirtyWorldPackages(Packages); return Packages; });
        };
        Hook(LevelEditor->GetGlobalLevelEditorActions());
        if (auto Level = LevelEditor->GetLevelEditorInstance().Pin(); Level && Level->GetLevelEditorActions()) Hook(Level->GetLevelEditorActions().ToSharedRef());
    }
    if (auto* Editors = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>())
    {
        for (UObject* Asset : Editors->GetAllEditedAssets())
        {
            auto Toolkit = FToolkitManager::Get().FindEditorForAsset(Asset);
            if (!Toolkit || !Toolkit->IsAssetEditor()) continue;
            auto Editor = StaticCastSharedPtr<FAssetEditorToolkit>(Toolkit); TWeakPtr<FAssetEditorToolkit> Weak = Editor;
            WrapCommand(Editor->GetToolkitCommands(), FInputBindingManager::Get().FindCommandInContext(TEXT("AssetEditor"), TEXT("SaveAsset")), [Weak]
            {
                TArray<UPackage*> Packages;
                if (auto Live = Weak.Pin())
                {
                    if (const TArray<UObject*>* Objects = Live->GetObjectsCurrentlyBeingEdited())
                        for (UObject* Object : *Objects) if (Object) Packages.AddUnique(Object->GetOutermost());
                }
                return Packages;
            });
            const bool bMaterialEditor = Editor->GetToolkitFName() == FName(TEXT("MaterialEditor"));
            const bool bMaterialInstanceEditor = Editor->GetToolkitFName() == FName(TEXT("MaterialInstanceEditor"));
            // Material copies read the live preview without applying it to the
            // original. Ordinary Save retains the editor's apply/compile action.
            if (bMaterialEditor || bMaterialInstanceEditor || Editor->GetToolkitFName() == FName(TEXT("BlueprintEditor")) || Editor->GetToolkitFName() == FName(TEXT("TextureEditor")))
                WrapSaveAsCommand(Editor->GetToolkitCommands(), FInputBindingManager::Get().FindCommandInContext(TEXT("AssetEditor"), TEXT("SaveAssetAs")), [Weak]() -> UObject*
                {
                    if (auto Live = Weak.Pin()) return GetCopySource(*Live);
                    return nullptr;
                }, [Weak]() -> UObject*
                {
                    if (auto Live = Weak.Pin()) return GetCopyData(*Live);
                    return nullptr;
                });
        }
    }
    return true;
}
}
void Register()
{
    if (TickHandle.IsValid()) return;
    // Polling also picks up subclass commands remapped after toolkit initialization.
    // No UObject is touched by the worker-side repository checks.
    TickHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateStatic(&UpdateHooks), 0.25f);
    auto& Extenders = FModuleManager::LoadModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser")).GetAllContentBrowserCommandExtenders();
    auto Extender = FContentBrowserCommandExtender::CreateLambda([](TSharedRef<FUICommandList> List, FOnContentBrowserGetSelection Selection)
    {
        WrapCommand(List, FInputBindingManager::Get().FindCommandInContext(TEXT("ContentBrowser"), TEXT("SaveSelectedAsset")), [Selection]
        {
            TArray<FAssetData> Assets; TArray<FString> Folders; Selection.ExecuteIfBound(Assets, Folders);
            TArray<UPackage*> Packages;
            for (const auto& Asset : Assets) if (UPackage* Package = Asset.GetPackage()) Packages.AddUnique(Package);
            return Packages;
        });
    });
    BrowserHandle = Extender.GetHandle(); Extenders.Add(Extender);
}
void Unregister()
{
    ShutdownWPMapCopyLoads();
    if (TickHandle.IsValid()) FTSTicker::GetCoreTicker().RemoveTicker(TickHandle); TickHandle.Reset();
    if (auto* Browser = FModuleManager::GetModulePtr<FContentBrowserModule>(TEXT("ContentBrowser")))
        Browser->GetAllContentBrowserCommandExtenders().RemoveAll([](const FContentBrowserCommandExtender& E) { return E.GetHandle() == BrowserHandle; });
    BrowserHandle.Reset(); RestoreCommands(); RemoveGuard(); bInitialized = false;
}
}
