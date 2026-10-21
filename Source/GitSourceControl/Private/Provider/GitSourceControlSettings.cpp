// Copyright (c) 2014-2020 Sebastien Rombauts (sebastien.rombauts@gmail.com)
//
// Distributed under the MIT License (MIT) (See accompanying file LICENSE.txt
// or copy at http://opensource.org/licenses/MIT)

#include "GitSourceControlSettings.h"

#include "Misc/ConfigCacheIni.h"
#include "Misc/ScopeLock.h"
#include "SourceControlHelpers.h"

namespace
{
	const TCHAR* const SettingsSection = TEXT("GitSourceControl.GitSourceControlSettings");
}

FString FGitSourceControlSettings::GetBinaryPath() const
{
	FScopeLock Lock(&CriticalSection);
	return BinaryPath;
}

bool FGitSourceControlSettings::SetBinaryPath(const FString& InPath)
{
	FScopeLock Lock(&CriticalSection);
	const bool bChanged = BinaryPath != InPath;
	BinaryPath = InPath;
	return bChanged;
}

bool FGitSourceControlSettings::IsUsingGitLfsLocking() const
{
	FScopeLock Lock(&CriticalSection);
	return bUsingGitLfsLocking;
}

bool FGitSourceControlSettings::SetUsingGitLfsLocking(bool bInUsing)
{
	FScopeLock Lock(&CriticalSection);
	const bool bChanged = bUsingGitLfsLocking != bInUsing;
	bUsingGitLfsLocking = bInUsing;
	return bChanged;
}

FString FGitSourceControlSettings::GetLockRemote() const
{
	FScopeLock Lock(&CriticalSection);
	return LockRemote;
}

bool FGitSourceControlSettings::SetLockRemote(const FString& InRemote)
{
	FScopeLock Lock(&CriticalSection);
	const bool bChanged = LockRemote != InRemote;
	LockRemote = InRemote;
	return bChanged;
}

void FGitSourceControlSettings::LoadSettings()
{
	FScopeLock Lock(&CriticalSection);
	const FString& IniFile = SourceControlHelpers::GetSettingsIni();
	GConfig->GetString(SettingsSection, TEXT("BinaryPath"), BinaryPath, IniFile);
	GConfig->GetBool(SettingsSection, TEXT("UsingGitLfsLocking"), bUsingGitLfsLocking, IniFile);
	GConfig->GetString(SettingsSection, TEXT("LockRemote"), LockRemote, IniFile);
}

void FGitSourceControlSettings::SaveSettings() const
{
	FScopeLock Lock(&CriticalSection);
	const FString& IniFile = SourceControlHelpers::GetSettingsIni();
	GConfig->SetString(SettingsSection, TEXT("BinaryPath"), *BinaryPath, IniFile);
	GConfig->SetBool(SettingsSection, TEXT("UsingGitLfsLocking"), bUsingGitLfsLocking, IniFile);
	GConfig->SetString(SettingsSection, TEXT("LockRemote"), *LockRemote, IniFile);
	GConfig->Flush(false, IniFile);
}
