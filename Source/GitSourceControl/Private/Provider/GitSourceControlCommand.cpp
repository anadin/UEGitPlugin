// Copyright (c) 2014-2020 Sebastien Rombauts (sebastien.rombauts@gmail.com)
//
// Distributed under the MIT License (MIT) (See accompanying file LICENSE.txt
// or copy at http://opensource.org/licenses/MIT)

#include "GitSourceControlCommand.h"

#include "Misc/CoreMisc.h"

#define LOCTEXT_NAMESPACE "GitSourceControl"

FText FGitProgressBar::Describe(const GitCore::FLockProgress& Progress)
{
	const int32 Done = Progress.Done;
	const int32 Total = Progress.Total;
	return Progress.bReleasing ? FText::Format(LOCTEXT("ReleasingProgress", "Releasing locks: {0} of {1}"), Done, Total)
		: FText::Format(LOCTEXT("LockingProgress", "Locking files: {0} of {1}"), Done, Total);
}

void FGitProgressBar::Update(const GitCore::FLockProgress& Progress)
{
	const int32 Total = Progress.Total;
	const int32 Done = Progress.Done;
	if (Total < 2 || Done == Shown || IsRunningCommandlet())
	{
		return;
	}
	FSlateNotificationManager& Notifications = FSlateNotificationManager::Get();
	if (!Handle.IsValid())
	{
		Handle = Notifications.StartProgressNotification(Describe(Progress), Total);
	}
	Notifications.UpdateProgressNotification(Handle, Done, Total, Describe(Progress));
	Shown = Done;
}

void FGitProgressBar::Close()
{
	if (Handle.IsValid())
	{
		FSlateNotificationManager::Get().CancelProgressNotification(Handle);
		Handle.Reset();
	}
	Shown = -1;
}

FGitSourceControlCommand::FGitSourceControlCommand(const FSourceControlOperationRef& InOperation, const TSharedRef<IGitSourceControlWorker, ESPMode::ThreadSafe>& InWorker)
	: Operation(InOperation)
	, Worker(InWorker)
	, Cancel(MakeShared<GitCore::FCancelToken, ESPMode::ThreadSafe>())
	, Progress(MakeShared<GitCore::FLockProgress, ESPMode::ThreadSafe>())
{
}

void FGitSourceControlCommand::AddError(const FText& Message)
{
	if (!Message.IsEmpty())
	{
		ErrorMessages.Add(Message);
	}
}

void FGitSourceControlCommand::AddInfo(const FText& Message)
{
	if (!Message.IsEmpty())
	{
		InfoMessages.Add(Message);
	}
}

void FGitSourceControlCommand::AddError(const FString& Message)
{
	AddError(FText::FromString(Message));
}

void FGitSourceControlCommand::AddInfo(const FString& Message)
{
	AddInfo(FText::FromString(Message));
}

#undef LOCTEXT_NAMESPACE
