// Copyright (c) 2014-2020 Sebastien Rombauts (sebastien.rombauts@gmail.com)
//
// Distributed under the MIT License (MIT) (See accompanying file LICENSE.txt
// or copy at http://opensource.org/licenses/MIT)

#pragma once

#include "CoreMinimal.h"
#include "Core/GitSession.h"
#include "Framework/Notifications/NotificationManager.h"
#include "HAL/ThreadSafeBool.h"
#include "ISourceControlProvider.h"

class FGitSourceControlProvider;
class IGitSourceControlWorker;

/**
 * The progress bar Unreal's status bar shows for locking or releasing two or more files. It appears
 * after a few seconds, so quick work never shows one. Game thread only.
 */
class FGitProgressBar
{
public:
	FGitProgressBar() = default;
	FGitProgressBar(const FGitProgressBar&) = delete;
	FGitProgressBar& operator=(const FGitProgressBar&) = delete;
	~FGitProgressBar() { Close(); }

	/** "Locking files: 3 of 12", or "Releasing locks: 3 of 12". */
	static FText Describe(const GitCore::FLockProgress& Progress);
	/** Starts the bar once Progress counts two or more files, then keeps it up to date. */
	void Update(const GitCore::FLockProgress& Progress);
	void Close();

private:
	FProgressNotificationHandle Handle;
	int32 Shown = -1;
};

/** Settings captured on the game thread when a command is created. */
struct FGitCommandSettings
{
	/** The user allows LFS locking. Workers check the repository's lockable rules themselves, after refreshing. */
	bool bLockingEnabled = false;
	bool bPushOnSubmit = true;
	/** The user agreed to a push that creates a branch on the server or changes the branch this one follows. */
	bool bPushAgreed = false;
	/** Where Sync pulls from and how. */
	GitCore::FPullOptions Pull;
	int32 BackupsToKeep = 20;
	int32 BackupDays = 14;
	FString BackupDirectory;
};

/** One editor operation: created on the game thread, run on the session's mutation lane, finished on the game thread. */
class FGitSourceControlCommand
{
public:
	FGitSourceControlCommand(const FSourceControlOperationRef& InOperation, const TSharedRef<IGitSourceControlWorker, ESPMode::ThreadSafe>& InWorker);

	void AddError(const FText& Message);
	void AddInfo(const FText& Message);
	/** Messages from the git layer, which is not localized. */
	void AddError(const FString& Message);
	void AddInfo(const FString& Message);

	FSourceControlOperationRef Operation;
	TSharedRef<IGitSourceControlWorker, ESPMode::ThreadSafe> Worker;
	FSourceControlOperationComplete CompleteDelegate;
	GitCore::FGitSessionPtr Session;
	FGitCommandSettings Settings;

	/** Absolute filenames as passed by the editor. */
	TArray<FString> AbsoluteFiles;
	/** Repository-relative paths of the files that are inside the repository. */
	TArray<FString> RelativeFiles;
	/** The editor passed directories or no files: the operation covers the whole repository. */
	bool bWholeRepository = false;
	GitCore::FCancelTokenPtr Cancel;
	/** Counted by workers that take or release the locks of several files. */
	GitCore::FLockProgressRef Progress;
	/** Shows Progress in the status bar; the provider updates it on the game thread. */
	FGitProgressBar ProgressBar;

	bool bSucceeded = false;
	TArray<FText> InfoMessages;
	TArray<FText> ErrorMessages;
	FThreadSafeBool bDone;
};

using FGitSourceControlCommandRef = TSharedRef<FGitSourceControlCommand, ESPMode::ThreadSafe>;

class IGitSourceControlWorker
{
public:
	virtual ~IGitSourceControlWorker() = default;
	virtual FName GetName() const = 0;
	/** Runs on the session's mutation lane. Returns whether the operation succeeded. */
	virtual bool Execute(FGitSourceControlCommand& Command) = 0;
	/** Runs on the game thread after Execute. Returns true when file states may have changed. */
	virtual bool UpdateStates(FGitSourceControlCommand& Command, FGitSourceControlProvider& Provider) { return true; }
};

using FGitSourceControlWorkerRef = TSharedRef<IGitSourceControlWorker, ESPMode::ThreadSafe>;
