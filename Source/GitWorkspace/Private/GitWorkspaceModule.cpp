// Copyright (c) 2014-2020 Sebastien Rombauts (sebastien.rombauts@gmail.com)
//
// Distributed under the MIT License (MIT) (See accompanying file LICENSE.txt
// or copy at http://opensource.org/licenses/MIT)

#include "GitWorkspaceModule.h"

#include "Core/GitSession.h"
#include "Editor/GitEditorIntegration.h"
#include "Provider/GitProviderIdentity.h"
#include "Provider/GitWorkspaceUserSettings.h"

#include "Features/IModularFeatures.h"
#include "Modules/ModuleManager.h"

namespace
{
	const FName SourceControlFeature(TEXT("SourceControl"));
}

void FGitWorkspaceModule::StartupModule()
{
	UGitWorkspaceUserSettings::ImportEarlierSettings();
	Provider.AccessSettings().LoadSettings();
	IModularFeatures::Get().RegisterModularFeature(SourceControlFeature, &Provider);
	GitWorkspaceProvider::MigrateLegacySelection();
	if (!IsRunningCommandlet())
	{
		GitEditorIntegration::Register();
	}
}

void FGitWorkspaceModule::ShutdownModule()
{
	if (!IsRunningCommandlet())
	{
		GitEditorIntegration::Unregister();
	}
	Provider.Close();
	IModularFeatures::Get().UnregisterModularFeature(SourceControlFeature, &Provider);
	GitCore::FGitSession::ShutdownAll();
}

FGitWorkspaceModule& FGitWorkspaceModule::Get()
{
	return FModuleManager::GetModuleChecked<FGitWorkspaceModule>("GitWorkspace");
}

IMPLEMENT_MODULE(FGitWorkspaceModule, GitWorkspace);
