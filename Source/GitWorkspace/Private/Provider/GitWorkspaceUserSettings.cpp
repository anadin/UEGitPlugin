// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#include "GitWorkspaceUserSettings.h"
#include "GitProviderIdentity.h"

#include "Misc/ConfigCacheIni.h"
#include "UObject/UnrealType.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(GitWorkspaceUserSettings)

UGitWorkspaceUserSettings::UGitWorkspaceUserSettings()
{
	SectionName = TEXT("GitWorkspace");
}

void UGitWorkspaceUserSettings::ImportEarlierSettings()
{
	const FString Ini = StaticClass()->GetConfigName();
	if (CopyEarlierSettings(Ini))
	{
		GConfig->Flush(false, Ini);
		GetMutableDefault<UGitWorkspaceUserSettings>()->LoadConfig();
	}
}

bool UGitWorkspaceUserSettings::CopyEarlierSettings(const FString& Ini)
{
	// A config class's section is named after its module and class, so it moved with the rename.
	TArray<FString> Keys;
	for (TFieldIterator<FProperty> It(StaticClass(), EFieldIteratorFlags::ExcludeSuper); It; ++It)
	{
		if (It->HasAnyPropertyFlags(CPF_Config))
		{
			Keys.Add(It->GetName());
		}
	}
	return GitWorkspaceProvider::CopyRenamedSection(TEXT("/Script/GitSourceControl.GitSourceControlUserSettings"), *StaticClass()->GetPathName(), Keys, Ini);
}
