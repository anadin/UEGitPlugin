// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#pragma once
#include "GitWorkspaceRepository.h"

namespace GitWorkspace
{
struct FRestartJob
{
    FString Folder, Error;
    uint32 HelperPid = 0;
    bool bReady = false;
};
FRestartJob StartRestartPull(FRepository& Repository, const FRemoteSnapshot& Reviewed, const FString& Project);
bool ApproveRestartClose(const FString& Folder, FString& Error);
void CancelRestartPull(const FString& Folder);
FString RestartJobStatus(const FString& Folder, FString& State);
FString LastRestartResult(const FString& Directory);
int32 RunRestartPull(const FString& Request);
}
