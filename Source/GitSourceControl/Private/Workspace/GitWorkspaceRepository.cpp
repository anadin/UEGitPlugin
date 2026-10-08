// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#include "GitWorkspaceRepository.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "HAL/PlatformMisc.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#if PLATFORM_MAC
#include "GitWorkspaceSession.h"
#include "Serialization/JsonSerializer.h"
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <crt_externs.h>
#include <string>
#include <vector>
#endif

namespace GitWorkspace
{
namespace
{
constexpr int32 MaxOutput = 32 * 1024 * 1024;
FString Decode(const uint8* Data, int32 Size)
{
    if (!Size) return FString();
    FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Data), Size);
    return FString(Converted.Length(), Converted.Get());
}
FResult Fail(const FString& Message) { FResult R; R.Error = Message; return R; }

bool Records(const TArray<uint8>& Bytes, TArray<FString>& Out)
{
    int32 Start = 0;
    for (int32 I = 0; I < Bytes.Num(); ++I)
    {
        if (Bytes[I] == 0) { Out.Add(Decode(Bytes.GetData() + Start, I - Start)); Start = I + 1; }
    }
    return Start == Bytes.Num();
}
void AppendUtf8(TArray<uint8>& Data, const FString& Value)
{
    FTCHARToUTF8 Utf8(*Value);
    Data.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
}
struct FTemporaryFile
{
    FString Path = FPaths::CreateTempFilename(FPlatformProcess::UserTempDir(), TEXT("uegit-"));
    ~FTemporaryFile() { IFileManager::Get().Delete(*Path); }
};
#if !PLATFORM_MAC
FString Quote(const FString& Arg)
{
    // Windows CRT rules: double backslashes before quotes and at the closing quote.
    FString R = TEXT("\"");
    int32 Slashes = 0;
    for (TCHAR C : Arg)
    {
        if (C == '\\') { ++Slashes; continue; }
        R += FString::ChrN(Slashes * (C == '"' ? 2 : 1), '\\');
        Slashes = 0;
        if (C == '"') R += TEXT("\\");
        R.AppendChar(C);
    }
    R += FString::ChrN(Slashes * 2, '\\');
    return R + TEXT("\"");
}
#endif
}

FString FindGitExecutable(const FString& ConfiguredPath)
{
    if (!ConfiguredPath.IsEmpty() && IFileManager::Get().FileExists(*ConfiguredPath)) return ConfiguredPath;
    TArray<FString> Candidates;
#if PLATFORM_MAC
    Candidates = {TEXT("/opt/homebrew/bin/git"), TEXT("/usr/local/bin/git"), TEXT("/usr/bin/git")};
#elif PLATFORM_WINDOWS
    Candidates = {TEXT("C:/Program Files/Git/cmd/git.exe"), TEXT("C:/Program Files/Git/bin/git.exe")};
#endif
    TArray<FString> SearchPaths;
    FPlatformMisc::GetEnvironmentVariable(TEXT("PATH")).ParseIntoArray(SearchPaths,
#if PLATFORM_WINDOWS
        TEXT(";"),
#else
        TEXT(":"),
#endif
        true);
    for (const FString& Directory : SearchPaths) Candidates.Add(FPaths::Combine(Directory,
#if PLATFORM_WINDOWS
        TEXT("git.exe")
#else
        TEXT("git")
#endif
    ));
    for (const FString& Candidate : Candidates) if (IFileManager::Get().FileExists(*Candidate)) return Candidate;
    return FString();
}

FString FResult::Text() const { return Decode(Out.GetData(), Out.Num()); }

FResult Run(const FString& Executable, const FString& Directory, const TArray<FString>& Arguments, double TimeoutSeconds)
{
    FResult Result;
    TArray<uint8> Errors;
    bool bAborted = false;
    const double Started = FPlatformTime::Seconds();
#if PLATFORM_MAC
    // UE's macOS command-line string parser cannot roundtrip arbitrary filenames.
    // Use argv directly: no shell, quoting, or global environment changes.
    int OutputPipe[2] = {-1, -1}, ErrorPipe[2] = {-1, -1};
    if (pipe(OutputPipe) || pipe(ErrorPipe))
    {
        for (int Fd : OutputPipe) if (Fd >= 0) close(Fd);
        for (int Fd : ErrorPipe) if (Fd >= 0) close(Fd);
        return Fail(TEXT("Unable to create Git process pipes."));
    }
    for (int Fd : OutputPipe) fcntl(Fd, F_SETFD, FD_CLOEXEC);
    for (int Fd : ErrorPipe) fcntl(Fd, F_SETFD, FD_CLOEXEC);
    posix_spawn_file_actions_t Actions;
    posix_spawn_file_actions_init(&Actions);
    posix_spawn_file_actions_adddup2(&Actions, OutputPipe[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&Actions, ErrorPipe[1], STDERR_FILENO);
    posix_spawn_file_actions_addopen(&Actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    const int ChdirResult = posix_spawn_file_actions_addchdir_np(&Actions, TCHAR_TO_UTF8(*Directory));
    posix_spawnattr_t Attr;
    posix_spawnattr_init(&Attr);
    posix_spawnattr_setflags(&Attr, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&Attr, 0);
    std::vector<std::string> Strings;
    Strings.emplace_back(TCHAR_TO_UTF8(*Executable));
    for (const FString& Arg : Arguments) Strings.emplace_back(TCHAR_TO_UTF8(*Arg));
    std::vector<char*> Argv;
    for (std::string& Str : Strings) Argv.push_back(Str.data());
    Argv.push_back(nullptr);
    std::vector<std::string> Environment;
    for (char** E = *_NSGetEnviron(); *E; ++E)
    {
        const std::string Entry(*E);
        if (Entry.rfind("GIT_TERMINAL_PROMPT=", 0) && Entry.rfind("LC_ALL=", 0) && Entry.rfind("PATH=", 0)) Environment.push_back(Entry);
    }
    Environment.emplace_back("GIT_TERMINAL_PROMPT=0");
    Environment.emplace_back("LC_ALL=C");
    Environment.emplace_back(std::string("PATH=/opt/homebrew/bin:/usr/local/bin:") + (getenv("PATH") ? getenv("PATH") : "/usr/bin:/bin"));
    std::vector<char*> Env;
    for (std::string& E : Environment) Env.push_back(E.data());
    Env.push_back(nullptr);
    pid_t Pid = 0;
    const int SpawnResult = ChdirResult ? ChdirResult : posix_spawn(&Pid, Strings[0].c_str(), &Actions, &Attr, Argv.data(), Env.data());
    posix_spawnattr_destroy(&Attr);
    posix_spawn_file_actions_destroy(&Actions);
    close(OutputPipe[1]); close(ErrorPipe[1]);
    if (SpawnResult)
    {
        close(OutputPipe[0]); close(ErrorPipe[0]);
        return Fail(FString::Printf(TEXT("Unable to launch Git (OS error %d)."), SpawnResult));
    }
    fcntl(OutputPipe[0], F_SETFL, O_NONBLOCK);
    fcntl(ErrorPipe[0], F_SETFL, O_NONBLOCK);
    auto Drain = [&](int Fd, TArray<uint8>& Destination)
    {
        uint8 Buffer[8192];
        for (ssize_t Count; (Count = read(Fd, Buffer, sizeof(Buffer))) > 0; )
        {
            if (Destination.Num() + Count > MaxOutput) { bAborted = true; break; }
            Destination.Append(Buffer, static_cast<int32>(Count));
        }
    };
    int Status = 0;
    bool bExited = false;
    while (!bExited)
    {
        Drain(OutputPipe[0], Result.Out); Drain(ErrorPipe[0], Errors);
        const pid_t WaitResult = waitpid(Pid, &Status, WNOHANG);
        bExited = WaitResult == Pid;
        if (WaitResult < 0 && errno != EINTR) { bAborted = true; }
        if (bAborted || FPlatformTime::Seconds() - Started > TimeoutSeconds)
        {
            bAborted = true;
            kill(-Pid, SIGKILL);
            while (waitpid(Pid, &Status, 0) < 0 && errno == EINTR) {}
            break;
        }
        if (!bExited) FPlatformProcess::Sleep(0.005f);
    }
    Drain(OutputPipe[0], Result.Out); Drain(ErrorPipe[0], Errors);
    close(OutputPipe[0]); close(ErrorPipe[0]);
    if (!bAborted && WIFEXITED(Status)) Result.Code = WEXITSTATUS(Status);
#else
    void *ReadOut = nullptr, *WriteOut = nullptr, *ReadErr = nullptr, *WriteErr = nullptr;
    if (!FPlatformProcess::CreatePipe(ReadOut, WriteOut) || !FPlatformProcess::CreatePipe(ReadErr, WriteErr))
    {
        FPlatformProcess::ClosePipe(ReadOut, WriteOut); FPlatformProcess::ClosePipe(ReadErr, WriteErr);
        return Fail(TEXT("Unable to create Git process pipes."));
    }
    FString CommandLine;
    for (const FString& Arg : Arguments) { CommandLine += Quote(Arg) + TEXT(" "); }
    FProcHandle Handle = FPlatformProcess::CreateProc(*Executable, *CommandLine, false, true, true, nullptr, 0, *Directory, WriteOut, nullptr, WriteErr);
    if (Handle.IsValid())
    {
        auto Drain = [&](void* Pipe, TArray<uint8>& Destination)
        {
            TArray<uint8> Chunk;
            FPlatformProcess::ReadPipeToArray(Pipe, Chunk);
            if (Destination.Num() + Chunk.Num() > MaxOutput) bAborted = true;
            else Destination.Append(Chunk);
            return Chunk.Num() > 0;
        };
        do
        {
            Drain(ReadOut, Result.Out); Drain(ReadErr, Errors);
            if (bAborted || FPlatformTime::Seconds() - Started > TimeoutSeconds)
            {
                bAborted = true; FPlatformProcess::TerminateProc(Handle, true); break;
            }
            FPlatformProcess::Sleep(0.005f);
        } while (FPlatformProcess::IsProcRunning(Handle));
        while (!bAborted && (Drain(ReadOut, Result.Out) | Drain(ReadErr, Errors))) {}
        if (!bAborted) FPlatformProcess::GetProcReturnCode(Handle, &Result.Code);
        FPlatformProcess::CloseProc(Handle);
    }
    FPlatformProcess::ClosePipe(ReadOut, WriteOut); FPlatformProcess::ClosePipe(ReadErr, WriteErr);
#endif
    Result.Error = Decode(Errors.GetData(), Errors.Num());
    if (bAborted) { Result.Code = -1; Result.Error = TEXT("Git exceeded the time/output limit. Its outcome may be partial; refresh and inspect before retrying.\n") + Result.Error; }
    if (!Result.Ok() && Result.Error.IsEmpty()) Result.Error = Result.Out.Num() ? Result.Text() : TEXT("Git did not complete successfully. Refresh before retrying.");
    return Result;
}

int32 FSnapshot::StagedCount() const { int32 Count = 0; for (const FFile& F : Files) Count += F.HasStaged(); return Count; }
bool FSnapshot::HasConflicts() const { for (const FFile& F : Files) if (F.bConflict) return true; return false; }

bool ParseStatus(const TArray<uint8>& Bytes, FSnapshot& Out, FString& Error)
{
    TArray<FString> Lines;
    if (!Records(Bytes, Lines)) { Error = TEXT("Incomplete Git status output."); return false; }
    Out.Files.Reset();
    for (int32 I = 0; I < Lines.Num(); ++I)
    {
        const FString& Line = Lines[I];
        if (Line.StartsWith(TEXT("# branch.oid "))) { Out.Head = Line.Mid(13); Out.bUnborn = Out.Head == TEXT("(initial)"); continue; }
        if (Line.StartsWith(TEXT("# branch.head "))) { Out.Branch = Line.Mid(14); continue; }
        if (Line.StartsWith(TEXT("# branch.upstream "))) { Out.Upstream = Line.Mid(18); continue; }
        if (Line.StartsWith(TEXT("# "))) continue; // Extensible porcelain headers.
        FFile File;
        if (Line.StartsWith(TEXT("? "))) { File.Path = Line.Mid(2); File.bUntracked = true; File.Working = '?'; }
        else if (Line.StartsWith(TEXT("! "))) continue;
        else if (Line.StartsWith(TEXT("1 ")) || Line.StartsWith(TEXT("2 ")) || Line.StartsWith(TEXT("u ")))
        {
            const int32 FieldCount = Line[0] == '1' ? 8 : (Line[0] == '2' ? 9 : 10);
            TArray<FString> Fields;
            int32 Start = 0;
            for (int32 Field = 0; Field < FieldCount; ++Field)
            {
                const int32 End = Line.Find(TEXT(" "), ESearchCase::CaseSensitive, ESearchDir::FromStart, Start);
                if (End == INDEX_NONE) { Error = TEXT("Malformed Git status record."); return false; }
                Fields.Add(Line.Mid(Start, End - Start)); Start = End + 1;
            }
            if (Fields[1].Len() != 2 || Fields[2].Len() != 4) { Error = TEXT("Malformed status flags."); return false; }
            File.Index = Fields[1][0]; File.Working = Fields[1][1];
            File.Path = Line.Mid(Start); File.bConflict = Line[0] == 'u'; File.bSubmodule = Fields[2][0] == 'S';
            if (Line[0] == '2')
            {
                if (++I >= Lines.Num()) { Error = TEXT("Missing rename source path."); return false; }
                File.OriginalPath = Lines[I];
            }
        }
        else { Error = TEXT("Unrecognized Git status record."); return false; }
        if (File.Path.IsEmpty()) { Error = TEXT("Git returned an empty path."); return false; }
        Out.Files.Add(MoveTemp(File));
    }
    return true;
}

FRepository::FRepository(FString InGit, FString InDirectory) : GitBinary(MoveTemp(InGit)), RequestedDirectory(MoveTemp(InDirectory)), Root(RequestedDirectory) {}
FResult FRepository::Git(const TArray<FString>& Args) const
{
    TArray<FString> All {TEXT("--no-optional-locks"), TEXT("--literal-pathspecs"), TEXT("-c"), TEXT("color.ui=false"), TEXT("-c"), TEXT("core.quotepath=false")};
    All.Append(Args);
    return Run(GitBinary, Root, All);
}
FSnapshot FRepository::Refresh() { FScopeLock Guard(&Mutex); return RefreshInternal(); }
FSnapshot FRepository::RefreshInternal()
{
    FSnapshot Snapshot;
    FResult R = Git({TEXT("rev-parse"), TEXT("--show-toplevel")});
    if (!R.Ok()) { Snapshot.Error = R.Error; return Snapshot; }
    Root = R.Text().TrimEnd(); Snapshot.Root = Root;
    // The index must remain stable while we read status + attributes. External clients
    // are not covered by our mutex, so discard an inconsistent refresh.
    R = Git({TEXT("ls-files"), TEXT("--stage"), TEXT("-z")});
    if (!R.Ok()) { Snapshot.Error = R.Error; return Snapshot; }
    Snapshot.IndexEntries = R.Out;
    R = Git({TEXT("status"), TEXT("--porcelain=v2"), TEXT("-z"), TEXT("--branch"), TEXT("--untracked-files=all"), TEXT("--ignore-submodules=none")});
    if (!R.Ok() || !ParseStatus(R.Out, Snapshot, Snapshot.Error)) { if (!R.Ok()) Snapshot.Error = R.Error; return Snapshot; }
    if (Snapshot.Head.IsEmpty()) { Snapshot.Error = TEXT("Git status omitted HEAD identity."); return Snapshot; }
    // Resolve effective attributes in one call; paths returned by -z never become shell text.
    if (Snapshot.Files.Num())
    {
        // ls-files --stage supplies binary-safe identity; check-attr accepts path lists
        // through argv here (native argv on macOS, CRT quoting on Windows).
        for (int32 Start = 0; Start < Snapshot.Files.Num(); Start += 32)
        {
            TArray<FString> Args {TEXT("check-attr"), TEXT("-z"), TEXT("filter"), TEXT("lockable"), TEXT("--")};
            const int32 End = FMath::Min(Start + 32, Snapshot.Files.Num());
            for (int32 I = Start; I < End; ++I) Args.Add(Snapshot.Files[I].Path);
            R = Git(Args);
            TArray<FString> Attrs;
            if (!R.Ok() || !Records(R.Out, Attrs) || Attrs.Num() != (End - Start) * 6) { Snapshot.Error = TEXT("Could not resolve file attributes. ") + R.Error; return Snapshot; }
            for (int32 I = Start; I < End; ++I)
            {
                const int32 Offset = (I - Start) * 6;
                Snapshot.Files[I].bLfs = Attrs[Offset + 2] == TEXT("lfs");
                Snapshot.Files[I].bLockable = Attrs[Offset + 5] == TEXT("set");
            }
        }
    }
    for (const TCHAR* State : {TEXT("MERGE_HEAD"), TEXT("CHERRY_PICK_HEAD"), TEXT("REVERT_HEAD"), TEXT("rebase-merge"), TEXT("rebase-apply")})
    {
        R = Git({TEXT("rev-parse"), TEXT("--path-format=absolute"), TEXT("--git-path"), State});
        if (!R.Ok()) { Snapshot.Error = R.Error; return Snapshot; }
        const FString StatePath = R.Text().TrimEnd();
        Snapshot.bOperationInProgress |= IFileManager::Get().FileExists(*StatePath) || IFileManager::Get().DirectoryExists(*StatePath);
    }
#if PLATFORM_MAC
    FString RecoveryRoot, GitDir;
    if (GitWorkspaceSession::FindRepository(Root, RecoveryRoot, GitDir))
    {
        const FString Marker = GitWorkspaceSession::RecoveryFile(GitDir);
        if (IFileManager::Get().FileExists(*Marker))
        {
            FString Text, Operation; TSharedPtr<FJsonObject> Json;
            const bool bKnown = FFileHelper::LoadFileToString(Text, *Marker) && FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Json) &&
                Json && Json->TryGetStringField(TEXT("operation"), Operation);
            // The other asset operations clear their own marker only after
            // repository verification. Deletion also needs a live-editor gate:
            // retain unsaved work while incomplete recovery is inspected.
            if (bKnown && Operation == TEXT("external-actor-delete"))
            { Snapshot.bOperationInProgress = true; Snapshot.Error = TEXT("Resolve active actor deletion recovery before changing this checkout: ") + Marker; }
        }
        // A changed/missing shared marker must not hide an unfinished removal.
        // Failed backups with no moved file and completed retained backups do
        // not block; legacy stash/pull recovery formats keep their own checks.
        const FString Store = FPaths::Combine(GitDir, TEXT("uegit/actor-delete")); TArray<FString> Folders;
        IFileManager::Get().FindFiles(Folders, *FPaths::Combine(Store, TEXT("*")), false, true);
        for (const auto& Folder : Folders)
            if (IFileManager::Get().FileExists(*FPaths::Combine(Store, Folder, TEXT("removed.uasset"))) &&
                !IFileManager::Get().FileExists(*FPaths::Combine(Store, Folder, TEXT("complete.json"))))
            { Snapshot.bOperationInProgress = true; Snapshot.Error = TEXT("Resolve unfinished actor deletion recovery before changing this checkout: ") + FPaths::Combine(Store, Folder); break; }
    }
#endif
    R = Git({TEXT("ls-files"), TEXT("--stage"), TEXT("-z")});
    if (!R.Ok() || R.Out != Snapshot.IndexEntries) { Snapshot.Error = TEXT("The index changed during refresh. Refresh again."); return Snapshot; }
    const FResult Head = Git({TEXT("rev-parse"), TEXT("--verify"), TEXT("HEAD")});
    if ((!Snapshot.bUnborn && (!Head.Ok() || Head.Text().TrimEnd() != Snapshot.Head)) || (Snapshot.bUnborn && Head.Ok()))
    { Snapshot.Error = TEXT("HEAD changed during refresh. Refresh again."); return Snapshot; }
    Snapshot.bValid = true;
    return Snapshot;
}

FResult FRepository::Stage(const TArray<FString>& Paths) { FScopeLock Guard(&Mutex); return ChangeIndex(Paths, true); }
FResult FRepository::Unstage(const TArray<FString>& Paths) { FScopeLock Guard(&Mutex); return ChangeIndex(Paths, false); }
FResult FRepository::ChangeIndex(const TArray<FString>& Paths, bool bStage)
{
    FSnapshot Current = RefreshInternal();
    if (!Current.bValid) return Fail(Current.Error);
    if (Current.HasConflicts() || Current.bOperationInProgress) return Fail(TEXT("Resolve the in-progress Git operation externally before staging here."));
    if (Paths.IsEmpty()) return Fail(TEXT("Select at least one file."));
    TArray<uint8> PathBytes;
    TSet<FString> Added;
    for (const FString& Path : Paths)
    {
        const FFile* File = Current.Files.FindByPredicate([&](const FFile& F) { return F.Path == Path; });
        if (!File || File->bSubmodule) return Fail(TEXT("Selection changed or contains a submodule. Refresh; handle submodule pointers externally in this first slice."));
        for (const FString& P : {File->Path, File->OriginalPath})
        {
            if (!P.IsEmpty() && !Added.Contains(P)) { AppendUtf8(PathBytes, P); PathBytes.Add(0); Added.Add(P); }
        }
    }
    FTemporaryFile PathsFile;
    if (!FFileHelper::SaveArrayToFile(PathBytes, *PathsFile.Path)) return Fail(TEXT("Unable to write the temporary path list."));
    TArray<FString> Args;
    if (bStage) Args = {TEXT("add"), TEXT("-A")};
    else if (Current.bUnborn) Args = {TEXT("rm"), TEXT("--cached"), TEXT("-r"), TEXT("-f"), TEXT("--ignore-unmatch")};
    else Args = {TEXT("restore"), TEXT("--staged")};
    Args.Add(TEXT("--pathspec-from-file=") + PathsFile.Path);
    Args.Add(TEXT("--pathspec-file-nul"));
    return Git(Args);
}

FResult FRepository::Commit(const FSnapshot& Reviewed, const FString& Message)
{
    FScopeLock Guard(&Mutex);
    if (Message.TrimStartAndEnd().IsEmpty()) return Fail(TEXT("Enter a commit message."));
    FSnapshot Current = RefreshInternal();
    if (!Current.bValid) return Fail(Current.Error);
    if (!Reviewed.bValid || Reviewed.Root != Current.Root || Reviewed.Head != Current.Head || Reviewed.IndexEntries != Current.IndexEntries)
        return Fail(TEXT("The staged snapshot or HEAD changed. Review the refreshed changes before committing."));
    if (Current.HasConflicts() || Current.bOperationInProgress) return Fail(TEXT("Finish the in-progress Git operation externally before committing here."));
    if (!Current.StagedCount()) return Fail(TEXT("There are no staged changes to commit."));
    for (const FFile& F : Current.Files) if (F.bSubmodule && F.HasStaged()) return Fail(TEXT("Staged submodule pointer changes need external review in this first slice."));
    const FResult Tree = Git({TEXT("write-tree")});
    if (!Tree.Ok()) return Tree;
    FTemporaryFile MessageFile;
    if (!FFileHelper::SaveStringToFile(Message, *MessageFile.Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return Fail(TEXT("Unable to write commit message."));
    // Intentionally no add, pathspec, amend, push, pull or unlock. Normal hooks/signing run.
    FResult Result = Git({TEXT("-c"), TEXT("core.editor=false"), TEXT("commit"), TEXT("--file=" ) + MessageFile.Path});
    if (Result.Ok())
    {
        const FResult AfterTree = Git({TEXT("rev-parse"), TEXT("HEAD^{tree}")});
        if (!AfterTree.Ok() || AfterTree.Text().TrimEnd() != Tree.Text().TrimEnd())
            Result.Error += TEXT("\nA commit was created, but its tree differs from the reviewed index (a hook or external process may have changed it). Inspect history before continuing.");
    }
    return Result;
}

FResult FRepository::Diff(const FString& Path, bool bStaged)
{
    FScopeLock Guard(&Mutex);
    TArray<FString> Args {TEXT("diff"), TEXT("--no-ext-diff"), TEXT("--no-textconv"), TEXT("--no-color")};
    if (bStaged) Args.Add(TEXT("--cached"));
    Args.Append({TEXT("--"), Path});
    return Git(Args);
}
}
