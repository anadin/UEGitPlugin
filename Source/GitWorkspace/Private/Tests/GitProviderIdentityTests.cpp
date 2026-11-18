// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#if WITH_DEV_AUTOMATION_TESTS
#include "GitWorkspaceModule.h"
#include "Provider/GitProviderIdentity.h"
#include "Provider/GitWorkspaceSettings.h"
#include "Provider/GitWorkspaceUserSettings.h"

#include "Features/IModularFeatures.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "ISourceControlProvider.h"
#include "Misc/AutomationTest.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitWorkspaceProviderIdentityTest, "GitWorkspace.Provider.IdentityAndMigration", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FGitWorkspaceProviderIdentityTest::RunTest(const FString&)
{
	using namespace GitWorkspaceProvider;
	const FGitWorkspaceProvider& Provider = FGitWorkspaceModule::Get().GetProvider();
	TestEqualSensitive(TEXT("Registered provider uses the public name"), Provider.GetName().ToString(), FString(TEXT("Git Workspace")));
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("GitWorkspace"));
	if (!TestTrue(TEXT("The plugin is named GitWorkspace"), Plugin.IsValid()))
	{
		return false;
	}
	TestEqualSensitive(TEXT("Plugin browser uses the public name"), Plugin->GetDescriptor().FriendlyName, FString(TEXT("Git Workspace")));
	TestTrue(TEXT("Plugin browser preserves upstream credit"),
		Plugin->GetDescriptor().CreatedBy.Contains(TEXT("Project Borealis"), ESearchCase::CaseSensitive) && Plugin->GetDescriptor().CreatedBy.Contains(TEXT("SRombauts"), ESearchCase::CaseSensitive));

	// Unreal's own Git plugin keeps the name GitSourceControl, and this plugin no longer hides it.
	const TSharedPtr<IPlugin> UnrealGit = IPluginManager::Get().FindPlugin(TEXT("GitSourceControl"));
	if (!UnrealGit.IsValid())
	{
		AddInfo(TEXT("This engine has no GitSourceControl plugin; skipped the checks that both plugins load."));
	}
	else
	{
		TestTrue(TEXT("GitSourceControl is Unreal's own plugin"), UnrealGit->GetType() == EPluginType::Engine);
		if (!UnrealGit->IsEnabled())
		{
			AddInfo(TEXT("Unreal's own Git plugin is off in this project; skipped the check that both providers load."));
		}
		else
		{
			// Both providers are registered, and each answers with its own name. Classes that had the
			// same name in both modules could end up running each other's code.
			TArray<FString> Names;
			{
				IModularFeatures::FScopedLockModularFeatureList ListLock;
				for (ISourceControlProvider* Each : IModularFeatures::Get().GetModularFeatureImplementations<ISourceControlProvider>(TEXT("SourceControl")))
				{
					Names.Add(Each->GetName().ToString());
				}
			}
			auto Count = [&Names](const TCHAR* Name)
			{
				return Names.FilterByPredicate([Name](const FString& Each) { return Each.Equals(Name, ESearchCase::CaseSensitive); }).Num();
			};
			TestEqual(TEXT("Unreal's Git provider is registered once"), Count(TEXT("Git")), 1);
			TestEqual(TEXT("Git Workspace is registered once"), Count(TEXT("Git Workspace")), 1);
		}
	}

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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitWorkspaceRenamedSettingsTest, "GitWorkspace.Provider.SettingsFromGitSourceControl", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FGitWorkspaceRenamedSettingsTest::RunTest(const FString&)
{
	// A config file of the test's own, so the editor's settings are not touched.
	const FString Ini = FPaths::ConvertRelativePathToFull(FPaths::CreateTempFilename(*FPaths::AutomationTransientDir(), TEXT("GitWorkspaceRename"), TEXT(".ini")));
	const TCHAR* const SavedByEarlierBuild =
		TEXT("[GitSourceControl.GitSourceControlSettings]\n")
		TEXT("BinaryPath=/opt/git/bin/git\n")
		TEXT("UsingGitLfsLocking=False\n")
		TEXT("LockRemote=upstream\n")
		TEXT("\n")
		TEXT("[/Script/GitSourceControl.GitSourceControlUserSettings]\n")
		TEXT("BackgroundFetchSeconds=0\n")
		TEXT("bWrapDiffLines=True\n")
		TEXT("Retired=1\n");
	if (!TestTrue(TEXT("Writes the settings an earlier build saved"), FFileHelper::SaveStringToFile(SavedByEarlierBuild, *Ini)))
	{
		return false;
	}
	GConfig->LoadFile(Ini);
	ON_SCOPE_EXIT
	{
		GConfig->UnloadFile(Ini);
		IFileManager::Get().Delete(*Ini);
	};

	FGitWorkspaceSettings Connection;
	Connection.LoadSettings(Ini);
	TestEqualSensitive(TEXT("The Git path carries over"), Connection.GetBinaryPath(), FString(TEXT("/opt/git/bin/git")));
	TestFalse(TEXT("Whether to use locks carries over"), Connection.IsUsingGitLfsLocking());
	TestEqualSensitive(TEXT("The lock remote carries over"), Connection.GetLockRemote(), FString(TEXT("upstream")));
	FString Value;
	TestTrue(TEXT("The earlier section stays for Unreal's own Git plugin"), GConfig->GetString(TEXT("GitSourceControl.GitSourceControlSettings"), TEXT("BinaryPath"), Value, Ini));

	GConfig->SetString(TEXT("GitSourceControl.GitSourceControlSettings"), TEXT("BinaryPath"), TEXT("/usr/bin/git"), Ini);
	FGitWorkspaceSettings Later;
	Later.LoadSettings(Ini);
	TestEqualSensitive(TEXT("Copied only once: later changes there are Unreal's own plugin's"), Later.GetBinaryPath(), FString(TEXT("/opt/git/bin/git")));

	const FString Section = UGitWorkspaceUserSettings::StaticClass()->GetPathName();
	TestEqualSensitive(TEXT("Preferences have a section of their own"), Section, FString(TEXT("/Script/GitWorkspace.GitWorkspaceUserSettings")));
	TestTrue(TEXT("Preferences are copied"), UGitWorkspaceUserSettings::CopyEarlierSettings(Ini));
	TestTrue(TEXT("The background fetch interval carries over"), GConfig->GetString(*Section, TEXT("BackgroundFetchSeconds"), Value, Ini) && Value.Equals(TEXT("0"), ESearchCase::CaseSensitive));
	TestTrue(TEXT("Diff wrapping carries over"), GConfig->GetString(*Section, TEXT("bWrapDiffLines"), Value, Ini) && Value.Equals(TEXT("True"), ESearchCase::CaseSensitive));
	TestFalse(TEXT("Only preferences that still exist are copied"), GConfig->GetString(*Section, TEXT("Retired"), Value, Ini));
	TestFalse(TEXT("Preferences are copied only once"), UGitWorkspaceUserSettings::CopyEarlierSettings(Ini));
	return true;
}
#endif
