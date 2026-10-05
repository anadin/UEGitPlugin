// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#include "GitWorkspaceRepository.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#if PLATFORM_MAC
#include <sys/stat.h>
#include <errno.h>
#endif

namespace GitWorkspace
{
FResult FRepository::CheckStashPaths(const TArray<FString>& Paths, bool bExisting) const
{
    FResult Result;
#if PLATFORM_MAC
    for (const auto& Path : Paths)
    {
        TArray<FString> Parts; Path.ParseIntoArray(Parts, TEXT("/"), false);
        FString Current = Root;
        for (int32 I = 0; I < Parts.Num(); ++I)
        {
            if (Parts[I].IsEmpty() || Parts[I] == TEXT(".") || Parts[I] == TEXT("..") || Parts[I].Equals(TEXT(".git"), ESearchCase::IgnoreCase))
            { Result.Error = TEXT("Invalid stash path: ") + Path; return Result; }
            Current = FPaths::Combine(Current, Parts[I]);
            struct stat Info;
            const int Status = lstat(TCHAR_TO_UTF8(*Current), &Info);
            const bool bLast = I == Parts.Num() - 1;
            if (Status != 0)
            {
                if (!bExisting && errno == ENOENT) continue;
                Result.Error = TEXT("Cannot inspect stash path: ") + Path; return Result;
            }
            if ((!bLast && !S_ISDIR(Info.st_mode)) || (bLast && (!bExisting || !S_ISREG(Info.st_mode) || (Info.st_mode & 0111))))
            { Result.Error = TEXT("Stash path is occupied or is not a regular non-executable file: ") + Path; return Result; }
        }
        if (Parts.IsEmpty()) { Result.Error = TEXT("Empty stash path."); return Result; }
    }
    Result.Code = 0;
#else
    Result.Error = TEXT("Untracked stash operations are currently available on Mac only.");
#endif
    return Result;
}
FResult FRepository::UntrackedFingerprint(const TArray<FString>& Paths) const
{
    auto Result = CheckStashPaths(Paths, true);
    if (!Result.Ok()) return Result;
    // Hash saved bytes without clean filters. LFS pointers alone cannot prove
    // that the file being removed still contains the reviewed hydrated bytes.
    for (int32 Start = 0; Start < Paths.Num(); Start += 32)
    {
        TArray<FString> Args {TEXT("hash-object"), TEXT("--no-filters"), TEXT("--")};
        const int32 End = FMath::Min(Start + 32, Paths.Num());
        for (int32 I = Start; I < End; ++I) Args.Add(Paths[I]);
        const auto Hash = Git(Args);
        TArray<FString> Lines; Hash.Text().ParseIntoArrayLines(Lines);
        if (!Hash.Ok() || Lines.Num() != End - Start) { Result.Code = -1; Result.Error = TEXT("Cannot hash all saved untracked files. ") + Hash.Error; return Result; }
        Result.Out.Append(Hash.Out);
    }
    return Result;
}
FResult FRepository::CaptureUntrackedTree(const TArray<FString>& Paths) const
{
    auto Result = CheckStashPaths(Paths, true);
    if (!Result.Ok()) return Result;
#if PLATFORM_MAC
    const FString Folder = FPaths::Combine(FPlatformProcess::UserTempDir(), TEXT("uegit-untracked-index-") + FGuid::NewGuid().ToString(EGuidFormats::Digits));
    if (!IFileManager::Get().MakeDirectory(*Folder, true)) { Result.Code = -1; Result.Error = TEXT("Cannot create private untracked index."); return Result; }
    ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*Folder, false, true); };
    auto InIndex = [&](const TArray<FString>& Args)
    {
        TArray<FString> All {TEXT("GIT_INDEX_FILE=") + FPaths::Combine(Folder, TEXT("index")), GitBinary,
            TEXT("--no-optional-locks"), TEXT("--literal-pathspecs"), TEXT("-c"), TEXT("core.splitIndex=false"),
            TEXT("-c"), TEXT("core.sparseCheckout=false"), TEXT("-c"), TEXT("core.hooksPath=") + Folder};
        All.Append(Args); return Run(TEXT("/usr/bin/env"), Root, All);
    };
    Result = InIndex({TEXT("read-tree"), TEXT("--empty")});
    if (!Result.Ok()) return Result;
    if (!Paths.IsEmpty())
    {
        TArray<uint8> Bytes;
        for (const auto& Path : Paths) { FTCHARToUTF8 Utf8(*Path); Bytes.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length()); Bytes.Add(0); }
        const FString PathFile = FPaths::Combine(Folder, TEXT("paths"));
        if (!FFileHelper::SaveArrayToFile(Bytes, *PathFile)) { Result.Code = -1; Result.Error = TEXT("Cannot write untracked path list."); return Result; }
        Result = InIndex({TEXT("add"), TEXT("--pathspec-from-file=") + PathFile, TEXT("--pathspec-file-nul")});
        if (!Result.Ok()) return Result;
    }
    return InIndex({TEXT("write-tree")});
#else
    return Result;
#endif
}
FResult FRepository::MakeStashCommit(const FString& Tree, const TArray<FString>& Parents, const FString& Message) const
{
    TArray<FString> Args {TEXT("-c"), TEXT("commit.gpgSign=false"), TEXT("commit-tree"), Tree};
    for (const auto& Parent : Parents) { Args.Add(TEXT("-p")); Args.Add(Parent); }
    Args.Append({TEXT("-m"), Message});
    return Git(Args);
}
}
