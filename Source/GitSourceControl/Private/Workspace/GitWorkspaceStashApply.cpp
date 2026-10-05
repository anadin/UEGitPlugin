// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#include "GitWorkspaceRepository.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Misc/SecureHash.h"

namespace GitWorkspace
{
namespace
{
bool StashPathsOverlap(const FString& A, const FString& B)
{
    // Conservative on case-insensitive editor filesystems, including aliases
    // whose spelling differs from the Git index. Treat directory swaps as overlap.
    return !A.IsEmpty() && !B.IsEmpty() && (A.Equals(B, ESearchCase::IgnoreCase) ||
        A.StartsWith(B + TEXT("/"), ESearchCase::IgnoreCase) || B.StartsWith(A + TEXT("/"), ESearchCase::IgnoreCase));
}
}
FResult FRepository::PrepareStashApply(FStashReview& R) const
{
    auto Fail = [](const FString& Error) { FResult Result; Result.Error = Error; return Result; };
    TArray<FString> TrackedPaths;
    int32 PathLength = 0;
    for (const auto& Change : R.Changes)
    {
        PathLength += Change.Path.Len();
        for (const auto& File : R.Local.Files)
            if (StashPathsOverlap(Change.Path, File.Path) || StashPathsOverlap(Change.Path, File.OriginalPath))
                return Fail(TEXT("Apply overlaps local changes at: ") + Change.Path.Replace(TEXT("\n"), TEXT("\\n")) +
                    TEXT(". Preserve or resolve that file first. Unrelated files can stay in place."));
        if (!R.UntrackedPaths.Contains(Change.Path)) TrackedPaths.Add(Change.Path);
    }
    if (R.Changes.Num() > 512 || PathLength > 24000) return Fail(TEXT("This stash is too large for guarded in-editor Apply. Apply it externally with the editor closed."));

    const auto BaseTree = Git({TEXT("rev-parse"), R.Base + TEXT("^{tree}")});
    const auto Captured = Git({TEXT("stash"), TEXT("create")});
    if (!BaseTree.Ok() || !Captured.Ok()) return Fail(TEXT("Cannot capture local tracked work before Apply. ") + Captured.Error);
    R.LocalWorkingTree = R.LocalIndexTree = BaseTree.Text().TrimEnd();
    if (!Captured.Text().TrimEnd().IsEmpty())
    {
        const auto Work = Git({TEXT("rev-parse"), Captured.Text().TrimEnd() + TEXT("^{tree}")});
        const auto Index = Git({TEXT("rev-parse"), Captured.Text().TrimEnd() + TEXT("^2^{tree}")});
        if (!Work.Ok() || !Index.Ok()) return Fail(TEXT("Cannot inspect the preserved local snapshot."));
        R.LocalWorkingTree = Work.Text().TrimEnd(); R.LocalIndexTree = Index.Text().TrimEnd();
    }
    for (const auto& File : R.Local.Files) if (File.bUntracked) R.PreservedUntrackedPaths.Add(File.Path);
    R.PreservedUntrackedPaths.Sort();
    if (R.PreservedUntrackedPaths.Num() > 512) return Fail(TEXT("Too many unrelated untracked files to verify safely. Preserve them before applying."));
    if (!R.PreservedUntrackedPaths.IsEmpty())
    {
        const auto Bytes = UntrackedFingerprint(R.PreservedUntrackedPaths);
        if (!Bytes.Ok()) return Fail(TEXT("Cannot verify unrelated untracked files. ") + Bytes.Error);
        uint8 Hash[20]; FSHA1::HashBuffer(Bytes.Out.GetData(), Bytes.Out.Num(), Hash);
        R.PreservedUntrackedBytes = BytesToHex(Hash, 20);
    }
    // Keep both unrelated versions. Replacing only disjoint stash paths in
    // private indexes yields exact expected results without touching the checkout.
    R.ExpectedWorkingTree = R.LocalWorkingTree; R.ExpectedIndexTree = R.LocalIndexTree;
    if (!TrackedPaths.IsEmpty())
    {
        const auto Work = ProjectStashTree(R.LocalWorkingTree, R.WorkingTree, TrackedPaths);
        if (!Work.Ok()) return Work;
        R.ExpectedWorkingTree = Work.Text().TrimEnd();
        if (R.bRestoreIndex)
        {
            const auto Index = ProjectStashTree(R.LocalIndexTree, R.IndexTree, TrackedPaths);
            if (!Index.Ok()) return Index;
            R.ExpectedIndexTree = Index.Text().TrimEnd();
        }
        // Tracked snapshots that would restore an untracked replacement need a
        // separate lifecycle policy. Ordinary third-parent files are handled below.
        for (const auto& Path : TrackedPaths)
        {
            const auto WorkEntry = Git({TEXT("ls-tree"), TEXT("-z"), R.ExpectedWorkingTree, TEXT("--"), Path});
            const auto IndexEntry = Git({TEXT("ls-tree"), TEXT("-z"), R.ExpectedIndexTree, TEXT("--"), Path});
            if (!WorkEntry.Ok() || !IndexEntry.Ok()) return Fail(TEXT("Cannot inspect a stash restoration path."));
            if (!WorkEntry.Out.IsEmpty() && IndexEntry.Out.IsEmpty()) return Fail(TEXT("This stash contains a working file without a tracked index entry. Restore it externally: ") + Path);
        }
    }
    FString HydratedTree = R.ExpectedWorkingTree;
    if (!R.UntrackedPaths.IsEmpty())
    {
        const auto Union = ProjectStashTree(HydratedTree, R.UntrackedCommit, R.UntrackedPaths);
        if (!Union.Ok()) return Union;
        HydratedTree = Union.Text().TrimEnd();
        R.ApplyWorkingPaths.Append(R.UntrackedPaths);
    }
    const auto Expected = MakeStashCommit(HydratedTree, {R.Base});
    if (!Expected.Ok()) return Expected;
    R.ExpectedWorkingCommit = Expected.Text().TrimEnd();
    R.ApplyWorkingPaths.Sort(); R.ApplyIndexPaths.Sort();
    FResult Result; Result.Code = 0; return Result;
}
#if PLATFORM_MAC
FResult FRepository::RestoreStashPaths(const FStashReview& R) const
{
    const FString PathFile = FPaths::Combine(FPlatformProcess::UserTempDir(), TEXT("uegit-apply-paths-") + FGuid::NewGuid().ToString(EGuidFormats::Digits));
    ON_SCOPE_EXIT { IFileManager::Get().Delete(*PathFile); };
    auto Restore = [&](const TArray<FString>& Paths, const FString& Tree, const TCHAR* Destination)
    {
        FResult Result; Result.Code = 0;
        if (Paths.IsEmpty()) return Result;
        TArray<uint8> Bytes;
        for (const auto& Path : Paths) { FTCHARToUTF8 Utf8(*Path); Bytes.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length()); Bytes.Add(0); }
        if (!FFileHelper::SaveArrayToFile(Bytes, *PathFile)) { Result.Code = -1; Result.Error = TEXT("Cannot write exact Apply path list."); return Result; }
        return HydratedGit({TEXT("restore"), TEXT("--source=") + Tree, Destination,
            TEXT("--pathspec-from-file=") + PathFile, TEXT("--pathspec-file-nul")});
    };
    // Worktree first: deleted paths must still be tracked in the real index so
    // restore can remove them. Never run stash apply's repository-wide index reset.
    auto Result = Restore(R.ApplyWorkingPaths, R.ExpectedWorkingCommit, TEXT("--worktree"));
    if (Result.Ok()) Result = Restore(R.ApplyIndexPaths, R.ExpectedIndexTree, TEXT("--staged"));
    return Result;
}
#endif
}
