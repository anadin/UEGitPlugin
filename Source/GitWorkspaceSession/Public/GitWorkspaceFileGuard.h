// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#pragma once
#include "CoreMinimal.h"

namespace GitWorkspaceSession
{
// Installed during PostConfigInit, before project packages/async loading start.
GITWORKSPACESESSION_API void InstallExternalCleanupGuard(const FString& Content);
GITWORKSPACESESSION_API void RemoveExternalCleanupGuard();
GITWORKSPACESESSION_API void EnableExternalCleanupGuard(bool bEnabled);
GITWORKSPACESESSION_API bool IsExternalCleanupProtected(const FString& Filename);
// Called before native cleanup unloads a package; it cannot veto that engine API.
GITWORKSPACESESSION_API FString RecordBlockedExternalCleanup(const FString& Filename, const FString& Reason);
GITWORKSPACESESSION_API bool HasBlockedExternalCleanup(const FString& Root);
GITWORKSPACESESSION_API TArray<FString> TakeBlockedCleanupNotices();
// Only the final package validator grants replacement of one existing file.
// Game-thread-only: the native writer can use its matching Saved temporary
// backup/replacement files. Ordinary actor moves remain refused.
GITWORKSPACESESSION_API void AllowVerifiedExternalReplacement(const FString& Filename);
GITWORKSPACESESSION_API void ClearVerifiedExternalReplacement();
// The repository uses this only after fresh lock/checkout verification.
class GITWORKSPACESESSION_API FVerifiedExternalPermissionScope
{
    FString Previous;
public:
    explicit FVerifiedExternalPermissionScope(const FString& Filename);
    ~FVerifiedExternalPermissionScope();
    FVerifiedExternalPermissionScope(const FVerifiedExternalPermissionScope&) = delete;
};
#if WITH_DEV_AUTOMATION_TESTS
// Adds an isolated fixture root without replacing the live platform-file chain.
class GITWORKSPACESESSION_API FExternalCleanupTestScope
{
    FString Content;
public:
    explicit FExternalCleanupTestScope(const FString& InContent);
    ~FExternalCleanupTestScope();
};
#endif
}
