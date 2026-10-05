// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#include "GitWorkspaceRepository.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"

namespace GitWorkspace
{
FResult FRepository::ProjectStashTree(const FString& Initial, const FString& Target, const TArray<FString>& Paths) const
{
    FResult Failure;
#if PLATFORM_MAC
    const FString Folder = FPaths::Combine(FPlatformProcess::UserTempDir(), TEXT("uegit-stash-index-") + FGuid::NewGuid().ToString(EGuidFormats::Digits));
    if (!IFileManager::Get().MakeDirectory(*Folder, true)) { Failure.Error = TEXT("Cannot create private stash index."); return Failure; }
    ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*Folder, false, true); };
    auto InIndex = [&](const TArray<FString>& Args)
    {
        TArray<FString> All {TEXT("GIT_INDEX_FILE=") + FPaths::Combine(Folder, TEXT("index")), GitBinary,
            TEXT("--no-optional-locks"), TEXT("--literal-pathspecs"), TEXT("-c"), TEXT("core.splitIndex=false"),
            TEXT("-c"), TEXT("core.sparseCheckout=false"), TEXT("-c"), TEXT("core.hooksPath=") + Folder};
        All.Append(Args); return Run(TEXT("/usr/bin/env"), Root, All);
    };
    auto Result = InIndex({TEXT("read-tree"), Initial});
    if (!Result.Ok()) return Result;
    TArray<FString> Args {TEXT("diff"), TEXT("--binary"), TEXT("--full-index"), TEXT("--no-renames"),
        TEXT("--no-ext-diff"), TEXT("--no-textconv"), TEXT("--src-prefix=a/"), TEXT("--dst-prefix=b/"), Initial, Target, TEXT("--")};
    Args.Append(Paths);
    const auto Patch = Git(Args);
    if (!Patch.Ok()) return Patch;
    if (!Patch.Out.IsEmpty())
    {
        const FString PatchFile = FPaths::Combine(Folder, TEXT("selection.patch"));
        if (!FFileHelper::SaveArrayToFile(Patch.Out, *PatchFile)) { Failure.Error = TEXT("Cannot write stash projection."); return Failure; }
        Result = InIndex({TEXT("apply"), TEXT("--cached"), TEXT("--whitespace=nowarn"), PatchFile});
        if (!Result.Ok()) return Result;
    }
    // Only immutable tree objects leave this helper. The real index and all
    // working files are untouched, including separately staged unselected work.
    return InIndex({TEXT("write-tree")});
#else
    Failure.Error = TEXT("Selected-file stash creation is currently available on Mac only."); return Failure;
#endif
}
}
