// Copyright (c) 2014-2020 Sebastien Rombauts (sebastien.rombauts@gmail.com)
//
// Distributed under the MIT License (MIT) (See accompanying file LICENSE.txt
// or copy at http://opensource.org/licenses/MIT)

#pragma once

#include "CoreMinimal.h"
#include "HAL/CriticalSection.h"

/**
 * Connection settings shown in Unreal's "Connect to Revision Control" dialog, stored in
 * SourceControlSettings.ini. Settings saved before the plugin was renamed to GitWorkspace are copied
 * over the first time they are loaded.
 */
class FGitWorkspaceSettings
{
public:
	FString GetBinaryPath() const;
	bool SetBinaryPath(const FString& InPath);

	/** Whether LFS file locking (Check Out) is used for files marked `lockable`. */
	bool IsUsingGitLfsLocking() const;
	bool SetUsingGitLfsLocking(bool bInUsing);

	/** Remote whose LFS server holds locks; empty means the branch's upstream remote. */
	FString GetLockRemote() const;
	bool SetLockRemote(const FString& InRemote);

	void LoadSettings();
	/** LoadSettings from a given config file rather than Unreal's revision control settings. */
	void LoadSettings(const FString& IniFile);
	void SaveSettings() const;

private:
	mutable FCriticalSection CriticalSection;
	FString BinaryPath;
	bool bUsingGitLfsLocking = true;
	FString LockRemote;
};
