// Copyright (c) 2014-2020 Sebastien Rombauts (sebastien.rombauts@gmail.com)
//
// Distributed under the MIT License (MIT) (See accompanying file LICENSE.txt
// or copy at http://opensource.org/licenses/MIT)

#pragma once

#include "GitSourceControlCommand.h"

/**
 * One worker per Unreal source control operation. Execute runs on the repository
 * session's mutation lane; UpdateStates runs on the game thread afterwards.
 */
namespace GitWorkers
{
	/** Creates the worker for an operation name, or null when the operation is not supported. */
	TSharedPtr<IGitSourceControlWorker, ESPMode::ThreadSafe> Create(FName OperationName);
	bool IsSupported(FName OperationName);
}

class FGitConnectWorker final : public IGitSourceControlWorker
{
public:
	virtual FName GetName() const override { return "Connect"; }
	virtual bool Execute(FGitSourceControlCommand& Command) override;
};

class FGitUpdateStatusWorker final : public IGitSourceControlWorker
{
public:
	virtual FName GetName() const override { return "UpdateStatus"; }
	virtual bool Execute(FGitSourceControlCommand& Command) override;
	virtual bool UpdateStates(FGitSourceControlCommand& Command, FGitSourceControlProvider& Provider) override;

private:
	TMap<FString, TArray<GitCore::FFileRevision>> Histories;
};

class FGitCheckOutWorker final : public IGitSourceControlWorker
{
public:
	virtual FName GetName() const override { return "CheckOut"; }
	virtual bool Execute(FGitSourceControlCommand& Command) override;
};

class FGitCheckInWorker final : public IGitSourceControlWorker
{
public:
	virtual FName GetName() const override { return "CheckIn"; }
	virtual bool Execute(FGitSourceControlCommand& Command) override;
	virtual bool UpdateStates(FGitSourceControlCommand& Command, FGitSourceControlProvider& Provider) override;

private:
	FText SuccessMessage;
};

class FGitRevertWorker final : public IGitSourceControlWorker
{
public:
	virtual FName GetName() const override { return "Revert"; }
	virtual bool Execute(FGitSourceControlCommand& Command) override;
	virtual bool UpdateStates(FGitSourceControlCommand& Command, FGitSourceControlProvider& Provider) override;

private:
	TArray<FString> DeletedFiles;
};

class FGitSyncPreviewWorker final : public IGitSourceControlWorker
{
public:
	virtual FName GetName() const override { return "SyncPreview"; }
	virtual bool Execute(FGitSourceControlCommand& Command) override;
	virtual bool UpdateStates(FGitSourceControlCommand& Command, FGitSourceControlProvider& Provider) override;

private:
	TArray<FString> AffectedFiles;
};

class FGitSyncWorker final : public IGitSourceControlWorker
{
public:
	virtual FName GetName() const override { return "Sync"; }
	virtual bool Execute(FGitSourceControlCommand& Command) override;
};

class FGitMarkForAddWorker final : public IGitSourceControlWorker
{
public:
	virtual FName GetName() const override { return "MarkForAdd"; }
	virtual bool Execute(FGitSourceControlCommand& Command) override;
};

class FGitDeleteWorker final : public IGitSourceControlWorker
{
public:
	virtual FName GetName() const override { return "Delete"; }
	virtual bool Execute(FGitSourceControlCommand& Command) override;
};

class FGitCopyWorker final : public IGitSourceControlWorker
{
public:
	virtual FName GetName() const override { return "Copy"; }
	virtual bool Execute(FGitSourceControlCommand& Command) override { return true; }
	virtual bool UpdateStates(FGitSourceControlCommand& Command, FGitSourceControlProvider& Provider) override { return false; }
};

class FGitResolveWorker final : public IGitSourceControlWorker
{
public:
	virtual FName GetName() const override { return "Resolve"; }
	virtual bool Execute(FGitSourceControlCommand& Command) override;
};
