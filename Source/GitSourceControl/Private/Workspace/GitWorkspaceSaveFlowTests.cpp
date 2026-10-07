// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#if WITH_DEV_AUTOMATION_TESTS
#include "GitWorkspaceSaveFlow.h"
#include "GitSourceControlModule.h"
#include "ISourceControlModule.h"
#include "Misc/AutomationTest.h"
#include "Styling/AppStyle.h"
#include "Misc/ScopeExit.h"
#include "Framework/Commands/InputBindingManager.h"
#include "Framework/Commands/UICommandInfo.h"
#include "Framework/Commands/UICommandList.h"
#include "UObject/UObjectGlobals.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitSaveCommandTest, "GitWorkspace.SaveLock.CommandLifecycle", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGitSaveCommandTest::RunTest(const FString&)
{
    TestTrue(TEXT("Own provider is recognized by instance despite Git LFS 2 name"), GitWorkspaceSave::HandlesProvider(FGitSourceControlModule::Get().GetProvider()));
    TestTrue(TEXT("Disconnected provider uses Git Workspace protection"), GitWorkspaceSave::HandlesProvider(ISourceControlModule::Get().GetProvider()));
    const FName ContextName(TEXT("GitWorkspaceSaveFixture"));
    auto Context = MakeShared<FBindingContext>(ContextName, FText::FromString(TEXT("Save acceptance fixture")), NAME_None, FAppStyle::GetAppStyleSetName());
    TSharedPtr<FUICommandInfo> Command;
    FUICommandInfo::MakeCommandInfo(Context, Command, TEXT("Save"), FText::FromString(TEXT("Save")), FText(), FSlateIcon(), EUserInterfaceActionType::Button, FInputChord());
    ON_SCOPE_EXIT { GitWorkspaceSave::RestoreCommands(); FInputBindingManager::Get().RemoveContextByName(ContextName); GitWorkspaceSave::RemoveGuard(); };
    auto List = MakeShared<FUICommandList>(); int32 Executions = 0; bool bCanExecute = false;
    FUIAction Original(FExecuteAction::CreateLambda([&Executions] { ++Executions; }), FCanExecuteAction::CreateLambda([&bCanExecute] { return bCanExecute; }));
    Original.GetActionCheckState = FGetActionCheckState::CreateLambda([] { return ECheckBoxState::Checked; });
    Original.IsActionVisibleDelegate = FIsActionButtonVisible::CreateLambda([] { return false; });
    List->MapAction(Command, Original); GitWorkspaceSave::InstallGuard();
    GitWorkspaceSave::WrapCommand(List, Command, [] { return TArray<UPackage*>(); });
    const auto First = *List->GetActionForCommand(Command);
    TestFalse(TEXT("Original CanExecute retained"), First.CanExecuteAction.Execute()); bCanExecute = true;
    TestTrue(TEXT("Dynamic enable state retained"), First.CanExecuteAction.Execute());
    TestTrue(TEXT("Original check state retained"), First.GetActionCheckState.Execute() == ECheckBoxState::Checked);
    TestFalse(TEXT("Original visibility retained"), First.IsActionVisibleDelegate.Execute());
    GitWorkspaceSave::WrapCommand(List, Command, [] { return TArray<UPackage*>(); });
    TestTrue(TEXT("Repeated hook does not wrap twice"), List->GetActionForCommand(Command)->ExecuteAction.GetHandle() == First.ExecuteAction.GetHandle());
    First.ExecuteAction.Execute(); TestEqual(TEXT("Original action executed once"), Executions, 1);
    GitWorkspaceSave::RestoreCommands();
    TestTrue(TEXT("Unregister restores original Save"), List->GetActionForCommand(Command)->ExecuteAction.GetHandle() == Original.ExecuteAction.GetHandle());
    GitWorkspaceSave::WrapCommand(List, Command, [] { return TArray<UPackage*>(); });
    FUIAction Replacement(FExecuteAction::CreateLambda([] {})); List->MapAction(Command, Replacement);
    GitWorkspaceSave::RestoreCommands();
    TestTrue(TEXT("Another plugin replacement is preserved"), List->GetActionForCommand(Command)->ExecuteAction.GetHandle() == Replacement.ExecuteAction.GetHandle());
    List->MapAction(Command, Original);
    GitWorkspaceSave::WrapSaveAsCommand(List, Command, []() -> UObject* { return nullptr; });
    const auto SaveAs = *List->GetActionForCommand(Command);
    TestTrue(TEXT("Save As keeps dynamic enable/check delegates"), SaveAs.CanExecuteAction.Execute() && SaveAs.GetActionCheckState.Execute() == ECheckBoxState::Checked);
    TestFalse(TEXT("Save As keeps original visibility"), SaveAs.IsActionVisibleDelegate.Execute());
    GitWorkspaceSave::WrapSaveAsCommand(List, Command, []() -> UObject* { return nullptr; });
    TestTrue(TEXT("Save As is only wrapped once"), List->GetActionForCommand(Command)->ExecuteAction.GetHandle() == SaveAs.ExecuteAction.GetHandle());
    GitWorkspaceSave::RestoreCommands();
    TestTrue(TEXT("Save As original action restored on shutdown"), List->GetActionForCommand(Command)->ExecuteAction.GetHandle() == Original.ExecuteAction.GetHandle());
    List->MapAction(Command, Original);
    GitWorkspaceSave::WrapMapCommand(List, Command, []() -> UWorld* { return nullptr; }, true);
    const auto Map = *List->GetActionForCommand(Command);
    TestTrue(TEXT("Map naming keeps enable/check delegates"), Map.CanExecuteAction.Execute() && Map.GetActionCheckState.Execute() == ECheckBoxState::Checked);
    TestFalse(TEXT("Map naming keeps visibility"), Map.IsActionVisibleDelegate.Execute());
    GitWorkspaceSave::WrapMapCommand(List, Command, []() -> UWorld* { return nullptr; }, true);
    TestTrue(TEXT("Map naming wraps once"), List->GetActionForCommand(Command)->ExecuteAction.GetHandle() == Map.ExecuteAction.GetHandle());
    GitWorkspaceSave::RestoreCommands();
    TestTrue(TEXT("Map naming restores original action"), List->GetActionForCommand(Command)->ExecuteAction.GetHandle() == Original.ExecuteAction.GetHandle());
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitSaveGuardChainTest, "GitWorkspace.SaveLock.EditorPermissionChain", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGitSaveGuardChainTest::RunTest(const FString&)
{
    auto Previous = FCoreUObjectDelegates::IsPackageOKToSaveDelegate;
    ON_SCOPE_EXIT { GitWorkspaceSave::RemoveGuard(); FCoreUObjectDelegates::IsPackageOKToSaveDelegate = Previous; };
    int32 Calls = 0;
    FCoreUObjectDelegates::IsPackageOKToSaveDelegate.BindLambda([&Calls](UPackage*, const FString&, FOutputDevice*) { ++Calls; return false; });
    const auto Original = FCoreUObjectDelegates::IsPackageOKToSaveDelegate.GetHandle();
    GitWorkspaceSave::InstallGuard();
    TestFalse(TEXT("Editor folder permission veto is preserved"), FCoreUObjectDelegates::IsPackageOKToSaveDelegate.Execute(nullptr, TEXT("/tmp/nonexistent.uasset"), nullptr));
    TestEqual(TEXT("Original permission delegate called once"), Calls, 1);
    GitWorkspaceSave::RemoveGuard();
    TestTrue(TEXT("Original permission delegate restored"), Original == FCoreUObjectDelegates::IsPackageOKToSaveDelegate.GetHandle());
    GitWorkspaceSave::InstallGuard();
    FCoreUObjectDelegates::IsPackageOKToSaveDelegate.BindLambda([](UPackage*, const FString&, FOutputDevice*) { return true; });
    const auto Replacement = FCoreUObjectDelegates::IsPackageOKToSaveDelegate.GetHandle();
    GitWorkspaceSave::RemoveGuard();
    TestTrue(TEXT("Later permission delegate is not clobbered"), Replacement == FCoreUObjectDelegates::IsPackageOKToSaveDelegate.GetHandle());
    return true;
}
#endif
