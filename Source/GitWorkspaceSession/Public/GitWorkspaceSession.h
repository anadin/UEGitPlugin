// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#pragma once
#include "CoreMinimal.h"

namespace GitWorkspaceSession
{
GITWORKSPACESESSION_API FString CanonicalPath(const FString& Path);
GITWORKSPACESESSION_API bool FindRepository(const FString& Directory, FString& Root, FString& GitDir);
GITWORKSPACESESSION_API FString RecoveryFile(const FString& GitDir);
// Exclusive acquisition cannot succeed while any cooperating editor/commandlet
// holds its early-startup shared lease. A process exit releases the OS lock.
class GITWORKSPACESESSION_API FLease
{
public:
    FLease() = default;
    ~FLease();
    FLease(const FLease&) = delete;
    FLease& operator=(const FLease&) = delete;
    bool Acquire(const FString& Directory, bool bExclusive, FString& Error);
    bool IsExclusiveFor(const FString& Directory) const;
    bool Promote(FString& Error);
    void Demote();
    void Release();
private:
    int Descriptor = -1;
    int WriterDescriptor = -1;
    FString GitDirectory;
    bool ClaimWriter(FString& Error);
    FString Root;
    bool bExclusiveLease = false;
};
class GITWORKSPACESESSION_API FEditorWriteScope
{
public:
    FEditorWriteScope() = default;
    FEditorWriteScope(const FEditorWriteScope&) = delete;
    FEditorWriteScope& operator=(const FEditorWriteScope&) = delete;
    ~FEditorWriteScope();
    bool Acquire(const FString& Root, FString& Error);
    const FLease& Lease() const;
private:
    bool bAcquired = false;
};
GITWORKSPACESESSION_API void StopForRecovery(const FString& Message);
GITWORKSPACESESSION_API bool HasEditorLease(const FString& Root);
// Conservative Mac boundary: older editors do not have the startup lease, so
// refuse other Unreal editor processes, including unrelated project editors.
GITWORKSPACESESSION_API bool NoOtherEditors(uint32 IgnoredPid, FString& Error);
}
