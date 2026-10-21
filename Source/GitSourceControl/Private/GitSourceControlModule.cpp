// Copyright (c) 2014-2020 Sebastien Rombauts (sebastien.rombauts@gmail.com)
//
// Distributed under the MIT License (MIT) (See accompanying file LICENSE.txt
// or copy at http://opensource.org/licenses/MIT)

#include "GitSourceControlModule.h"

#include "Core/GitSession.h"
#include "Editor/GitEditorIntegration.h"
#include "Provider/GitProviderIdentity.h"

#include "Features/IModularFeatures.h"
#include "Modules/ModuleManager.h"

namespace
{
	const FName SourceControlFeature(TEXT("SourceControl"));
}

void FGitSourceControlModule::StartupModule()
{
	Provider.AccessSettings().LoadSettings();
	IModularFeatures::Get().RegisterModularFeature(SourceControlFeature, &Provider);
	GitWorkspaceProvider::MigrateLegacySelection();
	if (!IsRunningCommandlet())
	{
		GitEditorIntegration::Register();
	}
}

void FGitSourceControlModule::ShutdownModule()
{
	if (!IsRunningCommandlet())
	{
		GitEditorIntegration::Unregister();
	}
	Provider.Close();
	IModularFeatures::Get().UnregisterModularFeature(SourceControlFeature, &Provider);
	GitCore::FGitSession::ShutdownAll();
}

FGitSourceControlModule& FGitSourceControlModule::Get()
{
	return FModuleManager::GetModuleChecked<FGitSourceControlModule>("GitSourceControl");
}

IMPLEMENT_MODULE(FGitSourceControlModule, GitSourceControl);
