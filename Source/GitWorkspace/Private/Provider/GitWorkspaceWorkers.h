// Copyright (c) 2014-2020 Sebastien Rombauts (sebastien.rombauts@gmail.com)
//
// Distributed under the MIT License (MIT) (See accompanying file LICENSE.txt
// or copy at http://opensource.org/licenses/MIT)

#pragma once

#include "GitWorkspaceCommand.h"

/**
 * One worker per Unreal source control operation. Execute runs on the repository
 * session's mutation lane; UpdateStates runs on the game thread afterwards.
 */
namespace GitWorkers
{
	/** Creates the worker for an operation name, or null when the operation is not supported. */
	TSharedPtr<IGitWorkspaceWorker, ESPMode::ThreadSafe> Create(FName OperationName);
	bool IsSupported(FName OperationName);
	/**
	 * Copies files that are about to be overwritten (repository paths) into a new timestamped folder of the backup
	 * directory, and prunes old folders. Returns the new folder, or empty when nothing was copied.
	 */
	FString BackUpFiles(const GitCore::FGitRepository& Repository, const TArray<FString>& Paths, const FGitCommandSettings& Settings);
}

class FGitWorkspaceConnectWorker final : public IGitWorkspaceWorker
{
public:
	virtual FName GetName() const override { return "Connect"; }
	virtual bool Execute(FGitWorkspaceCommand& Command) override;
};

class FGitWorkspaceUpdateStatusWorker final : public IGitWorkspaceWorker
{
public:
	virtual FName GetName() const override { return "UpdateStatus"; }
	virtual bool Execute(FGitWorkspaceCommand& Command) override;
	virtual bool UpdateStates(FGitWorkspaceCommand& Command, FGitWorkspaceProvider& Provider) override;

private:
	TMap<FString, TArray<GitCore::FFileRevision>> Histories;
};

class FGitWorkspaceCheckOutWorker final : public IGitWorkspaceWorker
{
public:
	virtual FName GetName() const override { return "CheckOut"; }
	virtual bool Execute(FGitWorkspaceCommand& Command) override;
};

class FGitWorkspaceCheckInWorker final : public IGitWorkspaceWorker
{
public:
	virtual FName GetName() const override { return "CheckIn"; }
	virtual bool Execute(FGitWorkspaceCommand& Command) override;
	virtual bool UpdateStates(FGitWorkspaceCommand& Command, FGitWorkspaceProvider& Provider) override;

private:
	FText SuccessMessage;
};

class FGitWorkspaceRevertWorker final : public IGitWorkspaceWorker
{
public:
	virtual FName GetName() const override { return "Revert"; }
	virtual bool Execute(FGitWorkspaceCommand& Command) override;
	virtual bool UpdateStates(FGitWorkspaceCommand& Command, FGitWorkspaceProvider& Provider) override;

private:
	TArray<FString> DeletedFiles;
};

class FGitWorkspaceSyncPreviewWorker final : public IGitWorkspaceWorker
{
public:
	virtual FName GetName() const override { return "SyncPreview"; }
	virtual bool Execute(FGitWorkspaceCommand& Command) override;
	virtual bool UpdateStates(FGitWorkspaceCommand& Command, FGitWorkspaceProvider& Provider) override;

private:
	TArray<FString> AffectedFiles;
};

class FGitWorkspaceSyncWorker final : public IGitWorkspaceWorker
{
public:
	virtual FName GetName() const override { return "Sync"; }
	virtual bool Execute(FGitWorkspaceCommand& Command) override;
};

class FGitWorkspaceMarkForAddWorker final : public IGitWorkspaceWorker
{
public:
	virtual FName GetName() const override { return "MarkForAdd"; }
	virtual bool Execute(FGitWorkspaceCommand& Command) override;
};

class FGitWorkspaceDeleteWorker final : public IGitWorkspaceWorker
{
public:
	virtual FName GetName() const override { return "Delete"; }
	virtual bool Execute(FGitWorkspaceCommand& Command) override;
};

class FGitWorkspaceCopyWorker final : public IGitWorkspaceWorker
{
public:
	virtual FName GetName() const override { return "Copy"; }
	virtual bool Execute(FGitWorkspaceCommand& Command) override { return true; }
	virtual bool UpdateStates(FGitWorkspaceCommand& Command, FGitWorkspaceProvider& Provider) override { return false; }
};

class FGitWorkspaceResolveWorker final : public IGitWorkspaceWorker
{
public:
	virtual FName GetName() const override { return "Resolve"; }
	virtual bool Execute(FGitWorkspaceCommand& Command) override;
};
