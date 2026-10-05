// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#include "GitWorkspaceRestart.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#if PLATFORM_MAC
#include "GitWorkspaceSession.h"
#include <fcntl.h>
#include <unistd.h>
#endif

namespace GitWorkspace
{
namespace
{
bool WriteRecord(const FString& File, const TSharedRef<FJsonObject>& Record)
{
    FString Json; if (!FJsonSerializer::Serialize(Record, TJsonWriterFactory<>::Create(&Json))) return false;
    const FString Temp = File + TEXT(".") + FGuid::NewGuid().ToString(EGuidFormats::Digits) + TEXT(".tmp");
    if (!FFileHelper::SaveStringToFile(Json, *Temp, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return false;
    const bool Ok = IFileManager::Get().Move(*File, *Temp, true, true);
    if (!Ok) IFileManager::Get().Delete(*Temp);
    return Ok;
}
TSharedPtr<FJsonObject> ReadRecord(const FString& File)
{
    const int64 Size = IFileManager::Get().FileSize(*File); FString Text; TSharedPtr<FJsonObject> Record;
    if (Size < 2 || Size > 65536 || !FFileHelper::LoadFileToString(Text, *File) ||
        !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Record)) return nullptr;
    return Record;
}
bool Status(const FString& Folder, const FString& State, const FString& Message)
{
    auto Record = MakeShared<FJsonObject>(); Record->SetStringField(TEXT("state"), State);
    Record->SetStringField(TEXT("message"), Message); Record->SetStringField(TEXT("at"), FDateTime::UtcNow().ToIso8601());
    Record->SetStringField(TEXT("job"), Folder);
    return WriteRecord(FPaths::Combine(Folder, TEXT("status.json")), Record) && WriteRecord(FPaths::Combine(FPaths::GetPath(Folder), TEXT("last-result.json")), Record);
}
bool LaunchArgument(const FString& Value)
{
    // UE's CreateProc takes a command-line string. Reject values its platform
    // tokenizer cannot roundtrip; these never become shell commands.
    return !Value.IsEmpty() && !Value.Contains(TEXT("\"")) && !Value.Contains(TEXT("'")) && !Value.Contains(TEXT("\\")) && !Value.Contains(TEXT("\n")) && !Value.Contains(TEXT("\r"));
}
FString Quote(const FString& Value) { return TEXT("\"") + Value + TEXT("\""); }
}
FString RestartJobStatus(const FString& Folder, FString& State)
{
    State.Empty(); const auto Record = ReadRecord(FPaths::Combine(Folder, TEXT("status.json"))); FString Message;
    if (Record) { Record->TryGetStringField(TEXT("state"), State); Record->TryGetStringField(TEXT("message"), Message); }
    return Message;
}
FString LastRestartResult(const FString& Directory)
{
#if PLATFORM_MAC
    FString Root, GitDir, Message;
    if (GitWorkspaceSession::FindRepository(Directory, Root, GitDir))
        if (const auto Record = ReadRecord(FPaths::Combine(GitDir, TEXT("uegit/restart-pull/last-result.json")))) Record->TryGetStringField(TEXT("message"), Message);
    return Message;
#else
    return FString();
#endif
}
void CancelRestartPull(const FString& Folder)
{
    if (!Folder.IsEmpty()) FFileHelper::SaveStringToFile(TEXT("cancelled\n"), *FPaths::Combine(Folder, TEXT("cancel")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}
bool ApproveRestartClose(const FString& Folder, FString& Error)
{
    if (!FFileHelper::SaveStringToFile(TEXT("approved\n"), *FPaths::Combine(Folder, TEXT("approved")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
    { Error = TEXT("Cannot approve the prepared restart. The editor will remain open."); return false; }
    return true;
}
FRestartJob StartRestartPull(FRepository& Repository, const FRemoteSnapshot& Reviewed, const FString& Project)
{
    FRestartJob Job;
#if PLATFORM_MAC
    Job.Error = RestartPullPathBlocker(Reviewed); if (!Job.Error.IsEmpty()) return Job;
    if (!GitWorkspaceSession::HasEditorLease(Reviewed.Root)) { Job.Error = TEXT("Restart this editor once to enable safe checkout coordination."); return Job; }
    if (!GitWorkspaceSession::NoOtherEditors(0, Job.Error)) return Job;
    const auto Before = Repository.Refresh();
    if (!Before.bValid || Before.Root != Reviewed.Root || Before.Head != Reviewed.Head || !Before.Files.IsEmpty())
    { Job.Error = TEXT("Save packages and preserve all local Git changes before restart Pull, including untracked files."); return Job; }
    const auto Lfs = Repository.PrepareIncomingLfs(Reviewed);
    if (!Lfs.bVerified) { Job.Error = Lfs.Error; return Job; }
    FString Root, GitDir;
    const FString ProjectPath = GitWorkspaceSession::CanonicalPath(Project);
    const FString Executable = FPlatformProcess::GetApplicationName(FPlatformProcess::GetCurrentProcessId());
    if (!GitWorkspaceSession::FindRepository(Reviewed.Root, Root, GitDir) || Root != Reviewed.Root || !ProjectPath.StartsWith(Root + TEXT("/")) ||
        !ProjectPath.EndsWith(TEXT(".uproject")) || !LaunchArgument(ProjectPath) || !LaunchArgument(Executable) || !LaunchArgument(GitDir))
    { Job.Error = TEXT("Cannot safely launch the helper for this project path. Use external integration."); return Job; }
    Job.Folder = FPaths::Combine(GitDir, TEXT("uegit/restart-pull"), FGuid::NewGuid().ToString(EGuidFormats::Digits));
    if (!IFileManager::Get().MakeDirectory(*Job.Folder, true)) { Job.Error = TEXT("Cannot create the restart request directory."); return Job; }
    auto Record = MakeShared<FJsonObject>(); Record->SetNumberField(TEXT("version"), 1);
    Record->SetStringField(TEXT("root"), Root); Record->SetStringField(TEXT("gitDir"), GitDir); Record->SetStringField(TEXT("project"), ProjectPath);
    Record->SetStringField(TEXT("editor"), Executable); Record->SetStringField(TEXT("head"), Reviewed.Head); Record->SetStringField(TEXT("target"), Reviewed.RemoteHead);
    Record->SetStringField(TEXT("context"), Reviewed.Context); Record->SetStringField(TEXT("remote"), Reviewed.Remote); Record->SetStringField(TEXT("remoteRef"), Reviewed.RemoteRef);
    Record->SetStringField(TEXT("git"), Repository.GitExecutable()); Record->SetStringField(TEXT("created"), FDateTime::UtcNow().ToIso8601());
    Record->SetNumberField(TEXT("parentPid"), FPlatformProcess::GetCurrentProcessId());
    const FString Request = FPaths::Combine(Job.Folder, TEXT("request.json"));
    if (!WriteRecord(Request, Record)) { Job.Error = TEXT("Cannot save the restart request."); return Job; }
    const FString Arguments = Quote(ProjectPath) + TEXT(" -run=GitWorkspacePull -GitWorkspacePullHelper -Request=") + Quote(Request)
        + TEXT(" -unattended -nullrhi -nosound -nosplash -nop4 -abslog=") + Quote(FPaths::Combine(Job.Folder, TEXT("helper.log")));
    auto Process = FPlatformProcess::CreateProc(*Executable, *Arguments, true, true, true, &Job.HelperPid, 0, nullptr, nullptr);
    if (!Process.IsValid()) { Job.Error = TEXT("Could not launch the restart helper. The editor remains open."); return Job; }
    ON_SCOPE_EXIT { FPlatformProcess::CloseProc(Process); };
    const double Deadline = FPlatformTime::Seconds() + 90;
    while (FPlatformProcess::IsProcRunning(Process) && FPlatformTime::Seconds() < Deadline)
    {
        FString State; const FString Message = RestartJobStatus(Job.Folder, State);
        if (State == TEXT("ready")) { Job.bReady = true; return Job; }
        if (State == TEXT("failed")) { Job.Error = Message; return Job; }
        FPlatformProcess::Sleep(0.1f);
    }
    CancelRestartPull(Job.Folder); Job.Error = TEXT("The restart helper did not become ready. The editor remains open. Inspect ") + Job.Folder;
#else
    Job.Error = TEXT("Restart Pull is currently supported on Mac only.");
#endif
    return Job;
}

int32 RunRestartPull(const FString& Request)
{
#if PLATFORM_MAC
    FString Root, GitDir;
    if (!GitWorkspaceSession::FindRepository(FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()), Root, GitDir)) return 1;
    const FString Folder = FPaths::GetPath(Request), Base = FPaths::Combine(GitDir, TEXT("uegit/restart-pull"));
    FGuid Id;
    if (FPaths::GetCleanFilename(Request) != TEXT("request.json") || FPaths::GetPath(Folder) != Base ||
        !FGuid::ParseExact(FPaths::GetCleanFilename(Folder), EGuidFormats::Digits, Id)) return 1;
    auto Fail = [&](const FString& Error) { Status(Folder, TEXT("failed"), Error); return 1; };
    const auto Record = ReadRecord(Request); double Version = 0, Pid = 0;
    FString StoredRoot, StoredDir, Project, Editor, Git, Head, Target, Context, Remote, RemoteRef, Created;
    if (!Record || !Record->TryGetNumberField(TEXT("version"), Version) || Version != 1 || !Record->TryGetNumberField(TEXT("parentPid"), Pid) ||
        Pid <= 0 || Pid > MAX_uint32 || Pid != FMath::FloorToDouble(Pid) ||
        !Record->TryGetStringField(TEXT("root"), StoredRoot) || !Record->TryGetStringField(TEXT("gitDir"), StoredDir) ||
        !Record->TryGetStringField(TEXT("project"), Project) || !Record->TryGetStringField(TEXT("editor"), Editor) || !Record->TryGetStringField(TEXT("git"), Git) ||
        !Record->TryGetStringField(TEXT("head"), Head) || !Record->TryGetStringField(TEXT("target"), Target) || !Record->TryGetStringField(TEXT("context"), Context) ||
        !Record->TryGetStringField(TEXT("remote"), Remote) || !Record->TryGetStringField(TEXT("remoteRef"), RemoteRef) || !Record->TryGetStringField(TEXT("created"), Created))
        return Fail(TEXT("Malformed or unsupported restart request. No integration attempted."));
    FDateTime Time;
    if (StoredRoot != Root || StoredDir != GitDir || Project != GitWorkspaceSession::CanonicalPath(FPaths::GetProjectFilePath()) ||
        !LaunchArgument(Project) || !LaunchArgument(Editor) || Editor != FPlatformProcess::GetApplicationName(FPlatformProcess::GetCurrentProcessId()) || !IFileManager::Get().FileExists(*Git) ||
        !FDateTime::ParseIso8601(*Created, Time) || (FDateTime::UtcNow() - Time).GetTotalSeconds() < 0 || (FDateTime::UtcNow() - Time).GetTotalSeconds() > 600)
        return Fail(TEXT("Restart request expired or does not match this checkout/project. No integration attempted."));
    const int Claim = open(TCHAR_TO_UTF8(*FPaths::Combine(Folder, TEXT("claimed"))), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (Claim < 0) return 1; // Requests are single-use, including failed/cancelled attempts.
    close(Claim);
    if (Pid == FPlatformProcess::GetCurrentProcessId()) return Fail(TEXT("The helper must run in a separate process."));
    auto Parent = FPlatformProcess::OpenProcess(uint32(Pid));
    ON_SCOPE_EXIT { FPlatformProcess::CloseProc(Parent); };
    FString Error;
    if (!Parent.IsValid() || !FPlatformProcess::IsProcRunning(Parent) || !GitWorkspaceSession::NoOtherEditors(uint32(Pid), Error))
        return Fail(TEXT("The original editor is unavailable or another editor is open. ") + Error);
    if (!Status(Folder, TEXT("ready"), TEXT("Restart Pull is prepared. Waiting for the editor to close; cancelling shutdown leaves files unchanged."))) return 1;
    const double Deadline = FPlatformTime::Seconds() + 180;
    while (FPlatformProcess::IsProcRunning(Parent))
    {
        if (IFileManager::Get().FileExists(*FPaths::Combine(Folder, TEXT("cancel"))) || FPlatformTime::Seconds() > Deadline)
            return Fail(TEXT("Restart Pull cancelled or editor shutdown timed out. No integration attempted."));
        FPlatformProcess::Sleep(0.1f);
    }
    if (!IFileManager::Get().FileExists(*FPaths::Combine(Folder, TEXT("approved"))) || IFileManager::Get().FileExists(*FPaths::Combine(Folder, TEXT("cancel"))))
        return Fail(TEXT("Editor close was not approved for this request. No integration attempted."));
    GitWorkspaceSession::FLease Lease;
    if (!Lease.Acquire(Root, true, Error) || !GitWorkspaceSession::NoOtherEditors(0, Error)) return Fail(Error);
    FRepository Repository(Git, Root);
    const auto Reviewed = Repository.Fetch();
    FResult Result;
    if (!Reviewed.IsFresh() || Reviewed.Head != Head || Reviewed.RemoteHead != Target || Reviewed.Context != Context || Reviewed.Remote != Remote || Reviewed.RemoteRef != RemoteRef)
        Result.Error = TEXT("Branch, remote or configuration changed after confirmation. No integration attempted; Fetch and review again.");
    else
    {
        Status(Folder, TEXT("integrating"), TEXT("Editor closed. Verifying and integrating the exact reviewed commit; locks are retained."));
        Result = Repository.PullAfterEditorExit(Reviewed, Lease);
    }
    const bool bRecovery = IFileManager::Get().FileExists(*GitWorkspaceSession::RecoveryFile(GitDir));
    FString Message = Result.Ok() ? TEXT("Restart Pull completed and asset bytes verified. Locks retained. Updated to ") + Target
        : TEXT("Restart Pull stopped: ") + Result.Error;
    if (bRecovery) Message += TEXT("\nThe editor was not reopened. Recovery instructions: ") + GitWorkspaceSession::RecoveryFile(GitDir);
    if (!Status(Folder, Result.Ok() ? TEXT("complete") : TEXT("failed"), Message)) return 1;
    Lease.Release();
    if (!bRecovery && GitWorkspaceSession::NoOtherEditors(0, Error))
    {
        const FString Arguments = Quote(Project) + TEXT(" -ExecCmds=GitWorkspace.Open");
        auto Reopened = FPlatformProcess::CreateProc(*Editor, *Arguments, true, false, false, nullptr, 0, nullptr, nullptr);
        if (!Reopened.IsValid()) return Fail(Message + TEXT("\nAutomatic reopening failed; reopen the project manually."));
        FPlatformProcess::CloseProc(Reopened);
    }
    return Result.Ok() ? 0 : 1;
#else
    return 1;
#endif
}
}
