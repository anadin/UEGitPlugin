// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#include "GitWorkspaceFileGuard.h"
#include "GitWorkspaceSession.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/FileManager.h"
#include "Async/AsyncFileHandle.h"
#include "Async/MappedFileHandle.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "Misc/SecureHash.h"
#include "Serialization/JsonSerializer.h"
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>

DEFINE_LOG_CATEGORY_STATIC(LogGitWorkspaceCleanup, Log, All);
namespace GitWorkspaceSession
{
namespace
{
FCriticalSection StateMutex;
TArray<FString> ContentRoots;
TMap<FString, FString> LatchedReports;
TSet<FString> RecordedFiles;
TArray<FString> Notices;
bool bEnabled = true;
thread_local FString WritableFile;
thread_local FString ReplacementFile;
FString Normalized(const FString& Path)
{
    FString Out = FPaths::ConvertRelativePathToFull(Path);
    FPaths::NormalizeFilename(Out); FPaths::CollapseRelativeDirectories(Out);
    Out.RemoveFromEnd(TEXT("/")); return Out;
}
bool Under(const FString& Path, const FString& Root)
{ return Path.Equals(Root, ESearchCase::IgnoreCase) || Path.StartsWith(Root + TEXT("/"), ESearchCase::IgnoreCase); }
bool External(const FString& Path)
{ return Path.Contains(TEXT("/__ExternalActors__/"), ESearchCase::IgnoreCase) || Path.Contains(TEXT("/__ExternalObjects__/"), ESearchCase::IgnoreCase); }
bool NativeReplacement(const FString& File)
{
    if (ReplacementFile.IsEmpty() || Normalized(File) != ReplacementFile) return false;
    return !HasBlockedExternalCleanup(FPaths::GetPath(ReplacementFile));
}
bool NativeTemp(const FString& File)
{
    const FString Path = Normalized(File), Prefix = FPaths::GetBaseFilename(ReplacementFile).Left(32);
    const FString Name = FPaths::GetBaseFilename(Path);
    if (ReplacementFile.IsEmpty() || FPaths::GetPath(Path) != Normalized(FPaths::ProjectSavedDir()) ||
        !Path.EndsWith(TEXT(".tmp")) || !Name.StartsWith(Prefix) || Name.Len() != Prefix.Len() + 32) return false;
    for (TCHAR C : Name.Right(32)) if (!FChar::IsHexDigit(C)) return false;
    return true;
}
bool WriteExclusive(int Parent, const char* Name, const FString& Text)
{
    const int File = openat(Parent, Name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (File < 0) return false;
    FTCHARToUTF8 Data(*Text); const uint8* Bytes = reinterpret_cast<const uint8*>(Data.Get()); int64 Left = Data.Length(); bool Ok = true;
    while (Left)
    {
        const auto N = write(File, Bytes, Left);
        if (N < 0 && errno == EINTR) continue;
        if (N <= 0) { Ok = false; break; } Bytes += N; Left -= N;
    }
    Ok = Ok && fsync(File) == 0; close(File); return Ok && fsync(Parent) == 0;
}
int Directory(int Parent, const FString& Name)
{
    if (mkdirat(Parent, TCHAR_TO_UTF8(*Name), 0700) && errno != EEXIST) return -1;
    const int Fd = openat(Parent, TCHAR_TO_UTF8(*Name), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (Fd >= 0 && fsync(Parent)) { close(Fd); return -1; } return Fd;
}
FString SavedDigest(const FString& Filename, int32& Mode)
{
    Mode = -1; const int Fd = open(TCHAR_TO_UTF8(*Filename), O_RDONLY | O_NOFOLLOW | O_CLOEXEC); struct stat Info;
    if (Fd < 0) return {};
    FSHA1 Sha; uint8 Bytes[65536]; bool Ok = !fstat(Fd, &Info) && S_ISREG(Info.st_mode);
    while (Ok)
    {
        const auto N = read(Fd, Bytes, sizeof(Bytes));
        if (N < 0 && errno == EINTR) continue;
        if (N < 0) { Ok = false; break; } if (!N) break; Sha.Update(Bytes, N);
    }
    close(Fd); if (!Ok) return {}; Mode = Info.st_mode & 0777;
    Sha.Final(); uint8 Hash[20]; Sha.GetHash(Hash); return BytesToHex(Hash, 20);
}
class FExternalCleanupPlatformFile : public IPlatformFile
{
    IPlatformFile* Inner = nullptr;
    bool Deny(const TCHAR* Filename, const FString& Reason)
    {
        if (!IsExternalCleanupProtected(Filename)) return false;
        RecordBlockedExternalCleanup(Filename, Reason); return true;
    }
    bool DirectoryProtected(const TCHAR* Directory)
    {
        const FString Path = Normalized(Directory), Physical = CanonicalPath(Path); TArray<FString> Roots;
        { FScopeLock Lock(&StateMutex); if (!bEnabled) return false; Roots = ContentRoots; }
        bool Candidate = false;
        for (const auto& Root : Roots)
        {
            if (Under(Root, Path) || (!Physical.IsEmpty() && Under(Root, Physical))) return true;
            Candidate |= Under(Path, Root) || (!Physical.IsEmpty() && Under(Physical, Root));
        }
        if (!Candidate) return false;
        // Preflight before deleting anything, including mixed ordinary/external trees.
        bool Found = false;
        const bool Complete = Inner->IterateDirectoryRecursively(Directory, [&Found](const TCHAR* File, bool IsDir)
        { if (!IsDir && IsExternalCleanupProtected(File)) { Found = true; return false; } return true; });
        return Found || !Complete;
    }
public:
    using IPlatformFile::IterateDirectory;
    using IPlatformFile::IterateDirectoryStat;
    bool Initialize(IPlatformFile* In, const TCHAR*) override { Inner = In; return Inner != nullptr; }
    IPlatformFile* GetLowerLevel() override { return Inner; }
    void SetLowerLevel(IPlatformFile* In) override { Inner = In; }
    const TCHAR* GetName() const override { return TEXT("GitWorkspaceExternalCleanup"); }
    bool FileExists(const TCHAR* F) override { return Inner->FileExists(F); }
    int64 FileSize(const TCHAR* F) override { return Inner->FileSize(F); }
    bool IsReadOnly(const TCHAR* F) override { return Inner->IsReadOnly(F); }
    bool DeleteFile(const TCHAR* F) override
    {
        if (IsExternalCleanupProtected(F) && !NativeReplacement(F))
        { RecordBlockedExternalCleanup(F, TEXT("Unreviewed external package deletion")); return false; }
        return Inner->DeleteFile(F);
    }
    bool DeleteFiles(const TArrayView<const TCHAR*>& Files) override
    {
        for (const auto* F : Files)
            if (IsExternalCleanupProtected(F) && !NativeReplacement(F))
            { RecordBlockedExternalCleanup(F, TEXT("Unreviewed bulk external package deletion")); return false; }
        return Inner->DeleteFiles(Files);
    }
    bool MoveFile(const TCHAR* To, const TCHAR* From) override
    {
        if (Inner->DirectoryExists(From) && DirectoryProtected(From))
        { RecordBlockedExternalCleanup(From, TEXT("Unreviewed external package directory move")); return false; }
        if (IsExternalCleanupProtected(From) && !(NativeReplacement(From) && NativeTemp(To)))
        { RecordBlockedExternalCleanup(From, TEXT("Unreviewed external package move")); return false; }
        if (IsExternalCleanupProtected(To) && !(NativeReplacement(To) && NativeTemp(From)))
        { RecordBlockedExternalCleanup(To, TEXT("Unreviewed external package replacement")); return false; }
        return Inner->MoveFile(To, From);
    }
    bool SetReadOnly(const TCHAR* F, bool ReadOnly) override
    {
        if (!ReadOnly && IsExternalCleanupProtected(F) && Normalized(F) != WritableFile && Inner->IsReadOnly(F))
        { RecordBlockedExternalCleanup(F, TEXT("Unverified external package permission change")); return false; }
        return Inner->SetReadOnly(F, ReadOnly);
    }
    bool DeleteDirectory(const TCHAR* D) override
    { if (DirectoryProtected(D)) { RecordBlockedExternalCleanup(D, TEXT("Unreviewed external package directory cleanup")); return false; } return Inner->DeleteDirectory(D); }
    bool DeleteDirectoryRecursively(const TCHAR* D) override
    { if (DirectoryProtected(D)) { RecordBlockedExternalCleanup(D, TEXT("Unreviewed recursive external package cleanup")); return false; } return Inner->DeleteDirectoryRecursively(D); }
    // Reads, ordinary writes and optional IO capabilities retain the existing chain.
    FDateTime GetTimeStamp(const TCHAR* F) override { return Inner->GetTimeStamp(F); }
    void SetTimeStamp(const TCHAR* F, FDateTime T) override { Inner->SetTimeStamp(F, T); }
    FDateTime GetAccessTimeStamp(const TCHAR* F) override { return Inner->GetAccessTimeStamp(F); }
    FString GetFilenameOnDisk(const TCHAR* F) override { return Inner->GetFilenameOnDisk(F); }
    ESymlinkResult IsSymlink(const TCHAR* F) override { return Inner->IsSymlink(F); }
    IFileHandle* OpenRead(const TCHAR* F, bool W = false) override { return Inner->OpenRead(F, W); }
    FFileOpenResult OpenRead(const TCHAR* F, EOpenReadFlags Flags) override { return Inner->OpenRead(F, Flags); }
    IFileHandle* OpenWrite(const TCHAR* F, bool A = false, bool R = false) override { return Inner->OpenWrite(F, A, R); }
    FFileOpenResult OpenWrite(const TCHAR* F, EOpenWriteFlags Flags) override { return Inner->OpenWrite(F, Flags); }
    FFileOpenAsyncResult OpenAsyncRead(const TCHAR* F, EOpenReadFlags Flags) override { return Inner->OpenAsyncRead(F, Flags); }
    IAsyncReadFileHandle* OpenAsyncRead(const TCHAR* F, bool W = false) override { return Inner->OpenAsyncRead(F, W); }
    FOpenMappedResult OpenMappedEx(const TCHAR* F, EOpenReadFlags Flags = EOpenReadFlags::None, int64 Size = 0) override { return Inner->OpenMappedEx(F, Flags, Size); }
    FOpenMappedResult OpenMappedEx2(const TCHAR* F, EOpenMappedFlags Flags = EOpenMappedFlags::None, int64 Size = 0) override { return Inner->OpenMappedEx2(F, Flags, Size); }
    bool DirectoryExists(const TCHAR* D) override { return Inner->DirectoryExists(D); }
    bool CreateDirectory(const TCHAR* D) override { return Inner->CreateDirectory(D); }
    FFileStatData GetStatData(const TCHAR* F) override { return Inner->GetStatData(F); }
    bool IterateDirectory(const TCHAR* D, FDirectoryVisitor& V) override { return Inner->IterateDirectory(D, V); }
    bool IterateDirectoryStat(const TCHAR* D, FDirectoryStatVisitor& V) override { return Inner->IterateDirectoryStat(D, V); }
    bool SendMessageToServer(const TCHAR* M, IFileServerMessageHandler* H) override { return Inner->SendMessageToServer(M, H); }
};
TUniquePtr<FExternalCleanupPlatformFile> FileGuard;
}
void InstallExternalCleanupGuard(const FString& Content)
{
    check(IsInGameThread());
    if (!FileGuard)
    {
        FileGuard = MakeUnique<FExternalCleanupPlatformFile>();
        FileGuard->Initialize(&FPlatformFileManager::Get().GetPlatformFile(), TEXT(""));
        FPlatformFileManager::Get().SetPlatformFile(*FileGuard);
    }
    FScopeLock Lock(&StateMutex); ContentRoots.AddUnique(Normalized(Content));
}
void RemoveExternalCleanupGuard()
{
    if (FileGuard) FPlatformFileManager::Get().RemovePlatformFile(FileGuard.Get());
    // Keep the wrapper alive through shutdown; it is never dynamically reloaded.
}
void EnableExternalCleanupGuard(bool Enabled) { FScopeLock Lock(&StateMutex); bEnabled = Enabled || !LatchedReports.IsEmpty(); }
bool IsExternalCleanupProtected(const FString& Filename)
{
    const FString Path = Normalized(Filename), Physical = CanonicalPath(Path);
    auto IsPackage = [](const FString& P) { return P.EndsWith(TEXT(".uasset"), ESearchCase::IgnoreCase) || P.EndsWith(TEXT(".umap"), ESearchCase::IgnoreCase); };
    if ((!IsPackage(Path) || !External(Path)) && (!IsPackage(Physical) || !External(Physical))) return false;
    FScopeLock Lock(&StateMutex);
    if (!bEnabled) return false;
    for (const auto& Root : ContentRoots) if (Under(Path, Root) || (!Physical.IsEmpty() && Under(Physical, Root))) return true;
    return false;
}
FString RecordBlockedExternalCleanup(const FString& Filename, const FString& Reason)
{
    FString Root, GitDir;
    // Directories may contain protected packages; record them without inventing a saved digest.
    if (!FindRepository(Normalized(Filename), Root, GitDir) && !FindRepository(FPaths::GetPath(Normalized(Filename)), Root, GitDir))
    {
        TArray<FString> Roots; { FScopeLock Lock(&StateMutex); Roots = ContentRoots; }
        for (const auto& Content : Roots)
            if (Under(Content, Normalized(Filename)) && FindRepository(Content, Root, GitDir)) break;
        if (Root.IsEmpty() || GitDir.IsEmpty()) return TEXT("Native cleanup blocked; cannot persist the checkout recovery report.");
    }
    FScopeLock Lock(&StateMutex);
    const FString Recorded = Normalized(Filename);
    if (RecordedFiles.Contains(Recorded)) return LatchedReports.FindRef(Root);
    RecordedFiles.Add(Recorded);
    const FString Id = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString Folder = FPaths::Combine(GitDir, TEXT("uegit/native-cleanup"), Id);
    const FString Message = TEXT("Native external-package cleanup blocked. Saved files remain on disk, but Unreal may already have cleared package/Undo state. Preserve other unsaved work before closing. With the editor closed, verify the report's saved files, write resolved.json from report.json, remove only its matching shared marker, and reopen the original map. Recovery report: ") + Folder;
    if (!LatchedReports.Contains(Root)) { LatchedReports.Add(Root, Message); Notices.Add(Message); }
    int32 Mode; const FString Hash = SavedDigest(Normalized(Filename), Mode);
    auto Json = MakeShared<FJsonObject>(); Json->SetStringField(TEXT("operation"), TEXT("native-external-cleanup-blocked"));
    Json->SetStringField(TEXT("id"), Id); Json->SetStringField(TEXT("root"), Root); Json->SetStringField(TEXT("path"), Normalized(Filename));
    Json->SetStringField(TEXT("reason"), Reason); Json->SetStringField(TEXT("saved_raw_sha1"), Hash); Json->SetNumberField(TEXT("original_mode"), Mode);
    Json->SetStringField(TEXT("report_folder"), Folder); Json->SetStringField(TEXT("recovery"), Message);
    FString Text; bool Saved = FJsonSerializer::Serialize(Json, TJsonWriterFactory<>::Create(&Text));
    const int Git = open(TCHAR_TO_UTF8(*GitDir), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    const int UeGit = Git < 0 ? -1 : Directory(Git, TEXT("uegit")); const int Store = UeGit < 0 ? -1 : Directory(UeGit, TEXT("native-cleanup"));
    const int Report = Store < 0 ? -1 : Directory(Store, Id); const int Marker = UeGit < 0 ? -1 : Directory(UeGit, TEXT("restart-pull"));
    Saved = Saved && Report >= 0 && WriteExclusive(Report, "report.json", Text);
    // Never overwrite another operation's marker. The report folder independently gates startup.
    if (Saved && Marker >= 0) WriteExclusive(Marker, "recovery-required.txt", Text);
    for (int Fd : {Report, Marker, Store, UeGit, Git}) if (Fd >= 0) close(Fd);
    UE_LOG(LogGitWorkspaceCleanup, Warning, TEXT("%s%s"), *Message, Saved ? TEXT("") : TEXT(" Report persistence failed; keep this editor open until the saved files are inspected."));
    return Message;
}
bool HasBlockedExternalCleanup(const FString& Root)
{
    FString Repo, GitDir; if (!FindRepository(Root, Repo, GitDir)) return false;
    { FScopeLock Lock(&StateMutex); if (LatchedReports.Contains(Repo)) return true; }
    const FString Store = FPaths::Combine(GitDir, TEXT("uegit/native-cleanup")); TArray<FString> Folders;
    IFileManager::Get().FindFiles(Folders, *FPaths::Combine(Store, TEXT("*")), false, true);
    for (const auto& Folder : Folders)
        if (IFileManager::Get().FileExists(*FPaths::Combine(Store, Folder, TEXT("report.json"))) &&
            !IFileManager::Get().FileExists(*FPaths::Combine(Store, Folder, TEXT("resolved.json")))) return true;
    return false;
}
TArray<FString> TakeBlockedCleanupNotices() { FScopeLock Lock(&StateMutex); TArray<FString> Out = MoveTemp(Notices); Notices.Empty(); return Out; }
void AllowVerifiedExternalReplacement(const FString& Filename) { check(IsInGameThread()); ReplacementFile = Normalized(Filename); }
void ClearVerifiedExternalReplacement() { check(IsInGameThread()); ReplacementFile.Empty(); }
FVerifiedExternalPermissionScope::FVerifiedExternalPermissionScope(const FString& Filename) : Previous(WritableFile) { WritableFile = Normalized(Filename); }
FVerifiedExternalPermissionScope::~FVerifiedExternalPermissionScope() { WritableFile = MoveTemp(Previous); }
#if WITH_DEV_AUTOMATION_TESTS
FExternalCleanupTestScope::FExternalCleanupTestScope(const FString& In) : Content(Normalized(CanonicalPath(In)))
{ FScopeLock Lock(&StateMutex); check(FileGuard && !ContentRoots.Contains(Content)); ContentRoots.Add(Content); }
FExternalCleanupTestScope::~FExternalCleanupTestScope()
{
    FString Root, GitDir; FindRepository(Content, Root, GitDir); FScopeLock Lock(&StateMutex);
    ContentRoots.Remove(Content); LatchedReports.Remove(Root);
    for (auto It = RecordedFiles.CreateIterator(); It; ++It) if (Under(*It, Root)) It.RemoveCurrent();
    Notices.Empty();
}
#endif
}
