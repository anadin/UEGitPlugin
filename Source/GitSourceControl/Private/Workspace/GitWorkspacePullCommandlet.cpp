// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#include "GitWorkspacePullCommandlet.h"
#include "GitWorkspaceRestart.h"
#include "Misc/Parse.h"

UGitWorkspacePullCommandlet::UGitWorkspacePullCommandlet()
{
    IsClient = false; IsEditor = true; IsServer = false; LogToConsole = true; ShowErrorCount = false;
}
int32 UGitWorkspacePullCommandlet::Main(const FString& Params)
{
    FString Request;
    if (!FParse::Param(*Params, TEXT("GitWorkspacePullHelper")) || !FParse::Value(*Params, TEXT("Request="), Request)) return 1;
    return GitWorkspace::RunRestartPull(Request);
}
