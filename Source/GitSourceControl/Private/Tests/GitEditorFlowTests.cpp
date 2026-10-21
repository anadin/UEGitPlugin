// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#if WITH_DEV_AUTOMATION_TESTS

#include "GitTestFixtures.h"
#include "Provider/GitProviderIdentity.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetViewUtils.h"
#include "Curves/CurveFloat.h"
#include "FileHelpers.h"
#include "ISourceControlModule.h"
#include "Misc/CommandLine.h"
#include "Misc/PackageName.h"
#include "Misc/Parse.h"
#include "SourceControlHelpers.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

namespace
{
	const TCHAR* const HeroPackage = TEXT("/Game/E2E/Hero");
	const TCHAR* const HeroObject = TEXT("/Game/E2E/Hero.Hero");

	UCurveFloat* FindHero()
	{
		return FindObject<UCurveFloat>(nullptr, HeroObject);
	}

	float HeroValue()
	{
		const UCurveFloat* Curve = FindHero();
		return Curve && Curve->FloatCurve.GetNumKeys() > 0 ? Curve->FloatCurve.GetFirstKey().Value : -1.0f;
	}

	bool SetHeroValue(float Value, bool bSave)
	{
		UCurveFloat* Curve = FindHero();
		if (!Curve)
		{
			return false;
		}
		Curve->FloatCurve.UpdateOrAddKey(0.0f, Value);
		UPackage* Package = Curve->GetPackage();
		Package->MarkPackageDirty();
		if (!bSave)
		{
			return true;
		}
		FSavePackageArgs Args;
		Args.TopLevelFlags = RF_Public | RF_Standalone;
		const FString File = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
		// Saving over a read-only file is fatal here; in the editor, the Check Out dialog comes first.
		if (IFileManager::Get().IsReadOnly(*File))
		{
			return false;
		}
		return UPackage::SavePackage(Package, Curve, *File, Args);
	}

	FString ServerLocks(const FString& Clone)
	{
		return GitTest::Output(GitTest::Git(Clone, {TEXT("lfs"), TEXT("locks"), TEXT("--json")}));
	}
}

/**
 * Unreal's own revision control helpers with Git Workspace as the active provider: check in a new
 * asset, check out (lock) and check in, revert with reload, and Sync of a teammate's change with
 * reload. Runs only in the disposable project that the host project's Scripts/e2e-native-flows.sh
 * prepares; skipped everywhere else.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGitEditorNativeFlowsTest, "GitWorkspace.Editor.NativeFlows", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FGitEditorNativeFlowsTest::RunTest(const FString&)
{
	FString Bob;
	FString Remote;
	if (!FParse::Value(FCommandLine::Get(), TEXT("GitWorkspaceE2EBob="), Bob) || !FParse::Value(FCommandLine::Get(), TEXT("GitWorkspaceE2ERemote="), Remote))
	{
		AddInfo(TEXT("Runs in the disposable project prepared by Scripts/e2e-native-flows.sh; skipped here."));
		return true;
	}
	ISourceControlProvider& Provider = ISourceControlModule::Get().GetProvider();
	if (!TestEqual(TEXT("Git Workspace is the active provider"), Provider.GetName(), GitWorkspaceProvider::Name()) || !TestTrue(TEXT("The provider is connected"), Provider.IsAvailable()))
	{
		return false;
	}
	const FString Project = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	const FString File = FPaths::ConvertRelativePathToFull(FPackageName::LongPackageNameToFilename(HeroPackage, FPackageName::GetAssetPackageExtension()));
	auto RemoteSubject = [&Remote]() { return GitTest::Output(GitTest::Git(Remote, {TEXT("log"), TEXT("-1"), TEXT("--format=%s"), TEXT("main")})); };

	// A new asset: saved, marked for add, checked in (committed and pushed).
	UPackage* Package = CreatePackage(HeroPackage);
	UCurveFloat* Curve = NewObject<UCurveFloat>(Package, TEXT("Hero"), RF_Public | RF_Standalone);
	Curve->FloatCurve.AddKey(0.0f, 1.0f);
	FAssetRegistryModule::AssetCreated(Curve);
	TestTrue(TEXT("The new asset saves"), SetHeroValue(1.0f, true));
	TestTrue(TEXT("Mark for add"), USourceControlHelpers::MarkFileForAdd(File, true));
	FSourceControlStatePtr State = Provider.GetState(File, EStateCacheUsage::ForceUpdate);
	TestTrue(TEXT("The new asset reads as added"), State.IsValid() && State->IsAdded());
	TestTrue(TEXT("Check in the new asset"), USourceControlHelpers::CheckInFiles({File}, TEXT("Add the hero curve"), true));
	TestEqual(TEXT("The commit reached the server"), RemoteSubject(), FString(TEXT("Add the hero curve")));

	// Edit: Unreal's Check Out takes the lock, Check in commits, pushes and releases it.
	TestTrue(TEXT("Lockable assets use Check Out"), Provider.UsesCheckout());
	State = Provider.GetState(File, EStateCacheUsage::ForceUpdate);
	TestTrue(TEXT("The asset can be checked out"), State.IsValid() && State->CanCheckout());
	SetHeroValue(2.0f, false);
	TestEqual(TEXT("Check Out succeeds"), FEditorFileUtils::CheckoutPackages(TArray<UPackage*>{FindHero()->GetPackage()}), ECommandResult::Succeeded);
	TestTrue(TEXT("Checked out means locked"), Provider.GetState(File, EStateCacheUsage::Use)->IsCheckedOut());
	TestTrue(TEXT("Teammates see the lock"), ServerLocks(Bob).Contains(TEXT("Content/E2E/Hero.uasset")));
	TestTrue(TEXT("The edit saves"), SetHeroValue(2.0f, true));
	TestTrue(TEXT("Check in the edit"), USourceControlHelpers::CheckInFiles({File}, TEXT("Hero v2"), true));
	TestEqual(TEXT("The edit reached the server"), RemoteSubject(), FString(TEXT("Hero v2")));
	TestFalse(TEXT("Check in released the lock"), ServerLocks(Bob).Contains(TEXT("Content/E2E/Hero.uasset")));

	// Git LFS keeps lockable files read-only unless you hold their lock.
	TestTrue(TEXT("Unlocked lockable files are read-only"), IFileManager::Get().IsReadOnly(*File));

	// A teammate's newer version (produced here, committed and pushed from Bob's clone).
	// Make Writable, as Unreal's Check Out dialog offers, so the version can be written without a lock.
	FPlatformFileManager::Get().GetPlatformFile().SetReadOnly(*File, false);
	TestTrue(TEXT("Write version 3"), SetHeroValue(3.0f, true));
	GitTest::Git(Bob, {TEXT("pull"), TEXT("-q"), TEXT("--ff-only")});
	TestEqual(TEXT("Bob's copy is replaced"), IFileManager::Get().Copy(*(Bob / TEXT("Content/E2E/Hero.uasset")), *File, true, true), static_cast<uint32>(COPY_OK));
	GitTest::CommitAll(Bob, TEXT("Hero v3 from Bob"));
	GitTest::Git(Bob, {TEXT("push"), TEXT("-q"), TEXT("origin"), TEXT("main")});
	TestEqual(TEXT("Bob's version reached the server"), RemoteSubject(), FString(TEXT("Hero v3 from Bob")));

	// Revert throws the local copy away and reloads the committed version.
	TestTrue(TEXT("Revert and reload"), USourceControlHelpers::RevertAndReloadPackages({File}));
	TestEqual(TEXT("The loaded asset shows the committed version"), HeroValue(), 2.0f);

	// Sync: fetch, preview, integrate and reload only what changed.
	TestTrue(TEXT("Sync succeeds"), AssetViewUtils::SyncRevisionFromSourceControl(TEXT("")));
	TestEqual(TEXT("The loaded asset shows the teammate's version"), HeroValue(), 3.0f);
	TestEqual(TEXT("The project is at the server's commit"),
		GitTest::Output(GitTest::Git(Project, {TEXT("rev-parse"), TEXT("HEAD")})), GitTest::Output(GitTest::Git(Remote, {TEXT("rev-parse"), TEXT("main")})));

	// Revert of a checked-out, saved edit restores the file and gives the lock back.
	SetHeroValue(4.0f, false);
	TestEqual(TEXT("Check Out again"), FEditorFileUtils::CheckoutPackages(TArray<UPackage*>{FindHero()->GetPackage()}), ECommandResult::Succeeded);
	TestTrue(TEXT("Save the experiment"), SetHeroValue(4.0f, true));
	TestTrue(TEXT("Revert the experiment"), USourceControlHelpers::RevertAndReloadPackages({File}));
	TestEqual(TEXT("The loaded asset is back to the synced version"), HeroValue(), 3.0f);
	TestFalse(TEXT("Revert released the lock"), ServerLocks(Bob).Contains(TEXT("Content/E2E/Hero.uasset")));
	return true;
}

#endif
