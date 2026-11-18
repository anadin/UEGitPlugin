// Copyright (c) 2014-2020 Sebastien Rombauts (sebastien.rombauts@gmail.com)
//
// Distributed under the MIT License (MIT) (See accompanying file LICENSE.txt
// or copy at http://opensource.org/licenses/MIT)

#include "GitWorkspaceSettings.h"
#include "GitProviderIdentity.h"

#include "Misc/ConfigCacheIni.h"
#include "Misc/ScopeLock.h"
#include "SourceControlHelpers.h"

namespace
{
	const TCHAR* const SettingsSection = TEXT("GitWorkspace.GitWorkspaceSettings");
	// Where they were saved while the plugin was named GitSourceControl. Unreal's own Git plugin keeps
	// its Git path in the same section, so the section stays.
	const TCHAR* const EarlierSettingsSection = TEXT("GitSourceControl.GitSourceControlSettings");
}

FString FGitWorkspaceSettings::GetBinaryPath() const
{
	FScopeLock Lock(&CriticalSection);
	return BinaryPath;
}

bool FGitWorkspaceSettings::SetBinaryPath(const FString& InPath)
{
	FScopeLock Lock(&CriticalSection);
	const bool bChanged = BinaryPath != InPath;
	BinaryPath = InPath;
	return bChanged;
}

bool FGitWorkspaceSettings::IsUsingGitLfsLocking() const
{
	FScopeLock Lock(&CriticalSection);
	return bUsingGitLfsLocking;
}

bool FGitWorkspaceSettings::SetUsingGitLfsLocking(bool bInUsing)
{
	FScopeLock Lock(&CriticalSection);
	const bool bChanged = bUsingGitLfsLocking != bInUsing;
	bUsingGitLfsLocking = bInUsing;
	return bChanged;
}

FString FGitWorkspaceSettings::GetLockRemote() const
{
	FScopeLock Lock(&CriticalSection);
	return LockRemote;
}

bool FGitWorkspaceSettings::SetLockRemote(const FString& InRemote)
{
	FScopeLock Lock(&CriticalSection);
	const bool bChanged = LockRemote != InRemote;
	LockRemote = InRemote;
	return bChanged;
}

void FGitWorkspaceSettings::LoadSettings()
{
	LoadSettings(SourceControlHelpers::GetSettingsIni());
}

void FGitWorkspaceSettings::LoadSettings(const FString& IniFile)
{
	FScopeLock Lock(&CriticalSection);
	const FString Keys[] = { TEXT("BinaryPath"), TEXT("UsingGitLfsLocking"), TEXT("LockRemote") };
	if (GitWorkspaceProvider::CopyRenamedSection(EarlierSettingsSection, SettingsSection, Keys, IniFile))
	{
		GConfig->Flush(false, IniFile);
	}
	GConfig->GetString(SettingsSection, TEXT("BinaryPath"), BinaryPath, IniFile);
	GConfig->GetBool(SettingsSection, TEXT("UsingGitLfsLocking"), bUsingGitLfsLocking, IniFile);
	GConfig->GetString(SettingsSection, TEXT("LockRemote"), LockRemote, IniFile);
}

void FGitWorkspaceSettings::SaveSettings() const
{
	FScopeLock Lock(&CriticalSection);
	const FString& IniFile = SourceControlHelpers::GetSettingsIni();
	GConfig->SetString(SettingsSection, TEXT("BinaryPath"), *BinaryPath, IniFile);
	GConfig->SetBool(SettingsSection, TEXT("UsingGitLfsLocking"), bUsingGitLfsLocking, IniFile);
	GConfig->SetString(SettingsSection, TEXT("LockRemote"), *LockRemote, IniFile);
	GConfig->Flush(false, IniFile);
}
