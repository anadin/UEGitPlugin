// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#include "GitWorkspaceSession.h"
#include "Modules/ModuleManager.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformMisc.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/Parse.h"
#include "Misc/OutputDeviceRedirector.h"
#include <sys/file.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <limits.h>
#include <stdlib.h>

namespace GitWorkspaceSession
{
namespace { FLease EditorLease; FString EditorRoot; }
FString CanonicalPath(const FString& Path)
{
    char Resolved[PATH_MAX];
    return realpath(TCHAR_TO_UTF8(*Path), Resolved) ? FString(UTF8_TO_TCHAR(Resolved)) : FString();
}
bool FindRepository(const FString& Directory, FString& Root, FString& GitDir)
{
    Root = CanonicalPath(Directory); GitDir.Empty();
    while (!Root.IsEmpty())
    {
        const FString DotGit = FPaths::Combine(Root, TEXT(".git"));
        if (IFileManager::Get().DirectoryExists(*DotGit)) { GitDir = CanonicalPath(DotGit); return !GitDir.IsEmpty(); }
        FString Link;
        if (FFileHelper::LoadFileToString(Link, *DotGit) && Link.RemoveFromStart(TEXT("gitdir: ")))
        {
            Link.TrimEndInline();
            GitDir = CanonicalPath(FPaths::IsRelative(Link) ? FPaths::Combine(Root, Link) : Link);
            return !GitDir.IsEmpty();
        }
        const FString Parent = FPaths::GetPath(Root);
        if (Parent == Root) break;
        Root = Parent;
    }
    return false;
}
FString RecoveryFile(const FString& GitDir) { return FPaths::Combine(GitDir, TEXT("uegit/restart-pull/recovery-required.txt")); }
FLease::~FLease() { Release(); }
void FLease::Release()
{
    if (Descriptor >= 0) { flock(Descriptor, LOCK_UN); close(Descriptor); Descriptor = -1; }
    if (WriterDescriptor >= 0) { flock(WriterDescriptor, LOCK_UN); close(WriterDescriptor); WriterDescriptor = -1; }
    Root.Empty(); GitDirectory.Empty(); bExclusiveLease = false;
}
bool FLease::ClaimWriter(FString& Error)
{
    const FString File = FPaths::Combine(GitDirectory, TEXT("uegit/editor-writer.lock"));
    WriterDescriptor = open(TCHAR_TO_UTF8(*File), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    struct stat Info;
    if (WriterDescriptor < 0 || fstat(WriterDescriptor, &Info) || !S_ISREG(Info.st_mode) || flock(WriterDescriptor, LOCK_EX | LOCK_NB))
    {
        if (WriterDescriptor >= 0) close(WriterDescriptor);
        WriterDescriptor = -1; Error = TEXT("Another Pull is preparing to update this checkout."); return false;
    }
    return true;
}
bool FLease::Promote(FString& Error)
{
    if (Descriptor < 0 || bExclusiveLease) { Error = TEXT("An active shared editor lease is required."); return false; }
    if (!ClaimWriter(Error)) return false;
    if (flock(Descriptor, LOCK_EX | LOCK_NB))
    {
        // flock conversion can drop the old shared lock even on failure. The
        // writer gate prevents another writer entering until it is restored.
        if (flock(Descriptor, LOCK_SH)) StopForRecovery(TEXT("Editor coordination was lost. Close other editors before reopening."));
        flock(WriterDescriptor, LOCK_UN); close(WriterDescriptor); WriterDescriptor = -1;
        Error = TEXT("Another editor is using this checkout. Close it before pulling assets."); return false;
    }
    bExclusiveLease = true; Error.Empty(); return true;
}
void FLease::Demote()
{
    if (!bExclusiveLease) return;
    if (flock(Descriptor, LOCK_SH)) StopForRecovery(TEXT("Editor coordination could not be restored after Pull."));
    bExclusiveLease = false;
    flock(WriterDescriptor, LOCK_UN); close(WriterDescriptor); WriterDescriptor = -1;
}
bool FLease::Acquire(const FString& Directory, bool bExclusive, FString& Error)
{
    Release(); FString GitDir;
    if (!FindRepository(Directory, Root, GitDir)) { Error = TEXT("Cannot locate the checkout's Git directory."); return false; }
    const FString Folder = FPaths::Combine(GitDir, TEXT("uegit"));
    if (!IFileManager::Get().MakeDirectory(*Folder, true)) { Error = TEXT("Cannot create the editor coordination directory."); return false; }
    GitDirectory = GitDir;
    if (bExclusive && !ClaimWriter(Error)) { Release(); return false; }
    const FString File = FPaths::Combine(Folder, TEXT("editor-session.lock"));
    Descriptor = open(TCHAR_TO_UTF8(*File), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    struct stat Info;
    if (Descriptor < 0 || fstat(Descriptor, &Info) || !S_ISREG(Info.st_mode) || flock(Descriptor, (bExclusive ? LOCK_EX : LOCK_SH) | LOCK_NB))
    { Release(); Error = TEXT("This checkout is still open in an editor or a restart Pull is running."); return false; }
    bExclusiveLease = bExclusive; Error.Empty(); return true;
}
bool FLease::IsExclusiveFor(const FString& Directory) const
{
    return Descriptor >= 0 && bExclusiveLease && Root == CanonicalPath(Directory);
}
bool HasEditorLease(const FString& Root) { return !EditorRoot.IsEmpty() && EditorRoot == CanonicalPath(Root); }
FEditorWriteScope::~FEditorWriteScope() { if (bAcquired) EditorLease.Demote(); }
bool FEditorWriteScope::Acquire(const FString& Root, FString& Error)
{
    check(IsInGameThread());
    if (bAcquired || !HasEditorLease(Root)) { Error = TEXT("Reopen this project to establish editor coordination before pulling assets."); return false; }
    bAcquired = EditorLease.Promote(Error); return bAcquired;
}
const FLease& FEditorWriteScope::Lease() const { check(bAcquired); return EditorLease; }
void StopForRecovery(const FString& Message)
{
    UE_LOG(LogTemp, Error, TEXT("%s"), *Message);
    if (GLog) { GLog->FlushThreadedLogs(); GLog->Flush(); }
    if (!IsRunningCommandlet()) FPlatformMisc::MessageBoxExt(EAppMsgType::Ok, *Message, TEXT("Git Workspace recovery"));
    _exit(1); // Never offer Save on stale objects after a partial integration.
}
bool NoOtherEditors(uint32 IgnoredPid, FString& Error)
{
    FPlatformProcess::FProcEnumerator Processes;
    while (Processes.MoveNext())
    {
        const auto Process = Processes.GetCurrent(); const auto Pid = Process.GetPID();
        if (Pid == FPlatformProcess::GetCurrentProcessId() || Pid == IgnoredPid) continue;
        const FString Name = Process.GetName();
        if (Name == TEXT("UnrealEditor") || Name.StartsWith(TEXT("UnrealEditor-")) || Name == TEXT("UE4Editor") || Name.StartsWith(TEXT("UE4Editor-")))
        { Error = FString::Printf(TEXT("Close other Unreal editors and commandlets first (process %u). Asset Pull currently requires one editor session on this Mac."), Pid); return false; }
    }
    return true;
}
}

class FGitWorkspaceSessionModule : public IModuleInterface
{
public:
    virtual void StartupModule() override
    {
        FString Root, GitDir;
        if (!GitWorkspaceSession::FindRepository(FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()), Root, GitDir)) return;
        FString Run;
        const bool bHelper = IsRunningCommandlet() && FParse::Param(FCommandLine::Get(), TEXT("GitWorkspacePullHelper")) &&
            FParse::Value(FCommandLine::Get(), TEXT("run="), Run) && Run.Equals(TEXT("GitWorkspacePull"), ESearchCase::IgnoreCase);
        if (bHelper) return;
        FString Error;
        const double Deadline = FPlatformTime::Seconds() + 180;
        while (!GitWorkspaceSession::EditorLease.Acquire(Root, false, Error) && FPlatformTime::Seconds() < Deadline)
            FPlatformProcess::Sleep(0.1f);
        const bool bRecovery = IFileManager::Get().FileExists(*GitWorkspaceSession::RecoveryFile(GitDir));
        if (!Error.IsEmpty() || bRecovery)
        {
            const FString Message = bRecovery ? TEXT("Git Workspace stopped an incomplete repository/asset operation. Project loading is blocked to protect assets. Review the recovery report at:\n") + GitWorkspaceSession::RecoveryFile(GitDir)
                : Error + TEXT("\nReopen the project when the operation finishes.");
            UE_LOG(LogTemp, Error, TEXT("%s"), *Message);
            if (!IsRunningCommandlet()) FPlatformMisc::MessageBoxExt(EAppMsgType::Ok, *Message, TEXT("Git Workspace recovery"));
            // Mac inherits the generic RequestExitWithStatus implementation,
            // which ignores the status and exits zero. Stop during bootstrap
            // with a real failure code; no project packages have been loaded.
            if (GLog) { GLog->FlushThreadedLogs(); GLog->Flush(); }
            _exit(1);
        }
        GitWorkspaceSession::EditorRoot = Root;
    }
    virtual void ShutdownModule() override { GitWorkspaceSession::EditorRoot.Empty(); GitWorkspaceSession::EditorLease.Release(); }
};
IMPLEMENT_MODULE(FGitWorkspaceSessionModule, GitWorkspaceSession)
