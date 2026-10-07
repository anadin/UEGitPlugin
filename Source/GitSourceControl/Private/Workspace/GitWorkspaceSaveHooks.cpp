// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#include "GitWorkspaceSaveFlow.h"
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

namespace GitWorkspaceSave
{
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
    if (!GEditor) return true;
    if (!bInitialized) { InstallGuard(); bInitialized = true; }
    if (auto* MainFrame = FModuleManager::GetModulePtr<IMainFrameModule>(TEXT("MainFrame")))
        WrapCommand(MainFrame->GetMainFrameCommandBindings(), FInputBindingManager::Get().FindCommandInContext(TEXT("MainFrame"), TEXT("SaveAll")), &DirtyPackages);
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
            // These built-in editors use the base Save As action. Editors with
            // subclass apply/compile/rename behavior need their own integration.
            if (Editor->GetToolkitFName() == FName(TEXT("BlueprintEditor")) || Editor->GetToolkitFName() == FName(TEXT("TextureEditor")))
                WrapSaveAsCommand(Editor->GetToolkitCommands(), FInputBindingManager::Get().FindCommandInContext(TEXT("AssetEditor"), TEXT("SaveAssetAs")), [Weak]() -> UObject*
                {
                    if (auto Live = Weak.Pin())
                        if (const auto* Objects = Live->GetObjectsCurrentlyBeingEdited(); Objects && Objects->Num() == 1) return (*Objects)[0];
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
    if (TickHandle.IsValid()) FTSTicker::GetCoreTicker().RemoveTicker(TickHandle); TickHandle.Reset();
    if (auto* Browser = FModuleManager::GetModulePtr<FContentBrowserModule>(TEXT("ContentBrowser")))
        Browser->GetAllContentBrowserCommandExtenders().RemoveAll([](const FContentBrowserCommandExtender& E) { return E.GetHandle() == BrowserHandle; });
    BrowserHandle.Reset(); RestoreCommands(); RemoveGuard(); bInitialized = false;
}
}
