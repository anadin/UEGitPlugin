// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#if WITH_DEV_AUTOMATION_TESTS
#include "GitSourceControlModule.h"
#include "Provider/GitProviderIdentity.h"

#include "Interfaces/IPluginManager.h"
#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitWorkspaceProviderIdentityTest, "GitWorkspace.Provider.IdentityAndMigration", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FGitWorkspaceProviderIdentityTest::RunTest(const FString&)
{
	using namespace GitWorkspaceProvider;
	const FGitSourceControlProvider& Provider = FGitSourceControlModule::Get().GetProvider();
	TestEqual(TEXT("Registered provider uses the public name"), Provider.GetName().ToString(), FString(TEXT("Git Workspace")));
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("GitSourceControl"));
	if (!TestTrue(TEXT("Technical plugin identifier remains compatible"), Plugin.IsValid()))
	{
		return false;
	}
	TestEqual(TEXT("Plugin browser uses the public name"), Plugin->GetDescriptor().FriendlyName, FString(TEXT("Git Workspace")));
	TestTrue(TEXT("Plugin browser preserves upstream credit"),
		Plugin->GetDescriptor().CreatedBy.Contains(TEXT("Project Borealis")) && Plugin->GetDescriptor().CreatedBy.Contains(TEXT("SRombauts")));

	TestTrue(TEXT("Legacy saved selection migrates"), ShouldMigrateLegacySelection(TEXT("Git LFS 2"), TEXT("-unattended")));
	TestFalse(TEXT("Disconnected projects stay disconnected"), ShouldMigrateLegacySelection(TEXT("None"), TEXT("")));
	TestFalse(TEXT("Unconfigured projects stay unconfigured"), ShouldMigrateLegacySelection(TEXT(""), TEXT("")));
	TestFalse(TEXT("Other providers stay selected"), ShouldMigrateLegacySelection(TEXT("Perforce"), TEXT("")));
	TestFalse(TEXT("Current name needs no second migration"), ShouldMigrateLegacySelection(TEXT("Git Workspace"), TEXT("")));
	TestFalse(TEXT("Explicit disconnect overrides legacy saved choice"), ShouldMigrateLegacySelection(TEXT("Git LFS 2"), TEXT("-SCCProvider=None")));
	TestFalse(TEXT("Explicit other provider overrides legacy saved choice"), ShouldMigrateLegacySelection(TEXT("Git LFS 2"), TEXT("-SCCProvider=Perforce")));
	TestFalse(TEXT("Explicit modern name needs no migration"), ShouldMigrateLegacySelection(TEXT("Git LFS 2"), TEXT("-SCCProvider=\"Git Workspace\"")));
	TestTrue(TEXT("Quoted legacy command-line selection remains compatible"), ShouldMigrateLegacySelection(TEXT("None"), TEXT("-SCCProvider=\"Git LFS 2\"")));
	TestFalse(TEXT("Unreal's first explicit provider switch wins"), ShouldMigrateLegacySelection(TEXT("Git LFS 2"), TEXT("-SCCProvider=None -SCCProvider=\"Git LFS 2\"")));
	TestTrue(TEXT("Legacy first switch wins over a later disconnect"), ShouldMigrateLegacySelection(TEXT("None"), TEXT("-SCCProvider=\"Git LFS 2\" -SCCProvider=None")));
	return true;
}
#endif
