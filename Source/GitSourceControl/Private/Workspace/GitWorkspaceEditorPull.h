// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#pragma once
#include "GitWorkspaceRepository.h"

namespace GitWorkspace
{
struct FEditorPullResult
{
    bool bSuccess = false, bRecoveryRequired = false;
    int32 Reloaded = 0, Refreshed = 0, Unloaded = 0;
    FString Message;
};
#if PLATFORM_MAC
// Game thread only, with editor input quiesced. Does not pump editor ticks while
// files change. A recovery result MUST close the editor without saving.
FEditorPullResult PullAndReload(FRepository& Repository, const FRemoteSnapshot& Reviewed,
    const FIncomingLfsResult& Prepared, const GitWorkspaceSession::FLease& Lease);
FEditorPullResult StashAndReload(FRepository& Repository, const FStashReview& Reviewed, const FString& Name, const GitWorkspaceSession::FLease& Lease);
FEditorPullResult DiscardAndReload(FRepository& Repository, const FDiscardReview& Reviewed, const GitWorkspaceSession::FLease& Lease);
// Deletes only the inspected entry, after Apply, package reload and completion
// verification succeed. A deletion failure reports that Apply already completed;
// bRecoveryRequired refers only to uncertain file/package restoration.
FEditorPullResult ApplyStashAndDelete(FRepository& Repository, const FStashReview& Reviewed,
    const FStashInspection& Inspected, const GitWorkspaceSession::FLease& Lease);
#endif
}
