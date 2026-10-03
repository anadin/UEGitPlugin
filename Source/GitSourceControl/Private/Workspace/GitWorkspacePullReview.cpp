// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#include "GitWorkspacePullReview.h"
#include "FileHelpers.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#if PLATFORM_MAC
#include <limits.h>
#include <stdlib.h>
#endif

namespace GitWorkspace
{
namespace
{
struct FMountAlias { FString Disk, PackageRoot; };
TArray<FMountAlias> MountAliases()
{
    TArray<FMountAlias> Aliases;
#if PLATFORM_MAC
    // Git canonicalizes /var and symlinked worktrees; Unreal mount points can
    // retain their original spelling. Resolve existing mount roots once, which
    // also lets us identify incoming packages that do not exist on disk yet.
    TArray<FString> Roots; FPackageName::QueryRootContentPaths(Roots, true);
    for (const auto& Root : Roots)
    {
        FString Directory;
        if (!FPackageName::TryConvertLongPackageNameToFilename(Root, Directory)) continue;
        Directory = FPaths::ConvertRelativePathToFull(Directory);
        char Resolved[PATH_MAX];
        if (realpath(TCHAR_TO_UTF8(*Directory), Resolved))
            Aliases.Add({FString(UTF8_TO_TCHAR(Resolved)) + TEXT("/"), Root});
    }
#endif
    return Aliases;
}
bool ResolvePackage(const FString& Filename, const TArray<FMountAlias>& Aliases, FString& PackageName)
{
    if (FPackageName::TryConvertToMountedPath(Filename, nullptr, &PackageName, nullptr, nullptr, nullptr)) return true;
    int32 BestLength = 0; bool bAmbiguous = false;
    for (const auto& Alias : Aliases)
    {
        if (Alias.Disk.Len() < BestLength || !Filename.StartsWith(Alias.Disk, ESearchCase::CaseSensitive)) continue;
        const FString Candidate = Alias.PackageRoot + FPaths::ChangeExtension(Filename.Mid(Alias.Disk.Len()), TEXT(""));
        if (Alias.Disk.Len() == BestLength && Candidate != PackageName) { bAmbiguous = true; continue; }
        if (Alias.Disk.Len() > BestLength) { PackageName = Candidate; BestLength = Alias.Disk.Len(); bAmbiguous = false; }
    }
    if (BestLength && !bAmbiguous && FPackageName::IsValidLongPackageName(PackageName, true)) return true;
    PackageName.Empty(); return false;
}
FString DisplayPath(FString Path)
{
    // Keep control characters in real Git paths from creating misleading rows.
    return Path.Replace(TEXT("\\"), TEXT("\\\\")).Replace(TEXT("\r"), TEXT("\\r")).Replace(TEXT("\n"), TEXT("\\n")).Replace(TEXT("\t"), TEXT("\\t"));
}
FString ChangeName(TCHAR Status)
{
    switch (Status) { case 'A': return TEXT("Add"); case 'D': return TEXT("Delete"); case 'M': return TEXT("Modify"); case 'T': return TEXT("Type change"); default: return TEXT("Unknown"); }
}
}
FPullReview ReviewIncoming(const FRemoteSnapshot& Remote, const FSnapshot& Local)
{
    check(IsInGameThread()); FPullReview Review;
    TArray<FString> Blockers, Lines;
    if (!Remote.IsFresh()) Blockers.Add(TEXT("Fetch again: the remote review is unavailable or expired."));
    if (!Local.bValid || Remote.Root != Local.Root || Remote.Head != Local.Head || Remote.Branch != Local.Branch)
        Blockers.Add(TEXT("Refresh and Fetch again: the repository or branch no longer matches this review."));
    if (Local.bOperationInProgress || Local.HasConflicts()) Blockers.Add(TEXT("Resolve the repository operation/conflicts first."));
    if (!Local.Files.IsEmpty()) Blockers.Add(TEXT("The index and working tree must be clean, including untracked files. Review and preserve local changes first."));
    if (Remote.Ahead && Remote.Behind) Blockers.Add(TEXT("History has diverged. Choose how to reconcile it in an external Git client; fast-forward Pull cannot proceed."));
    else if (!Remote.Behind) Blockers.Add(TEXT("There are no incoming commits to pull."));
    TArray<UPackage*> Dirty;
    FEditorFileUtils::GetDirtyWorldPackages(Dirty);
    FEditorFileUtils::GetDirtyContentPackages(Dirty);
    TArray<FString> DirtyNames;
    for (const auto* Package : Dirty) if (Package) DirtyNames.AddUnique(Package->GetName());
    bool bNeedsHandoff = false;
    const auto Aliases = MountAliases();
    for (const auto& Change : Remote.IncomingChanges)
    {
        FString Detail;
        switch (Change.Kind)
        {
        case EPullPathKind::Documentation: Detail = TEXT("Documentation; eligible for in-editor integration after preflight checks."); break;
        case EPullPathKind::Package:
        {
            FIncomingPackage Item; Item.Path = Change.Path;
            const FString Filename = FPaths::ConvertRelativePathToFull(FPaths::Combine(Remote.Root, Change.Path));
            Item.bMounted = ResolvePackage(Filename, Aliases, Item.PackageName);
            Item.bMap = Change.Path.EndsWith(TEXT(".umap"));
            Item.bExternal = Change.Path.Contains(TEXT("/__ExternalActors__/")) || Change.Path.Contains(TEXT("/__ExternalObjects__/"));
            if (Item.bMounted)
            {
                if (const UPackage* Package = FindPackage(nullptr, *Item.PackageName))
                {
                    Item.bLoaded = true; Item.bDirty = Package->IsDirty(); Item.bMap |= Package->ContainsMap();
                    if (Item.bDirty) DirtyNames.AddUnique(Item.PackageName);
                }
            }
            Detail = Item.bExternal ? TEXT("External actor/object package") : (Item.bMap ? TEXT("Map package") : TEXT("Asset package"));
            Detail += !Item.bMounted ? TEXT("; mount not available in this editor") : (Item.bDirty ? TEXT("; LOADED WITH UNSAVED CHANGES") : (Item.bLoaded ? TEXT("; loaded, saved") : TEXT("; not currently loaded")));
            if (Item.bMounted) Detail += TEXT("; ") + DisplayPath(Item.PackageName);
            Detail += TEXT(". Close the editor before integrating; package reload is not yet coordinated.");
            Review.Packages.Add(MoveTemp(Item)); bNeedsHandoff = true;
            break;
        }
        case EPullPathKind::RestartRequired:
            Detail = TEXT("Code, configuration or other project input. Close the editor before integrating; source/plugin changes may require a rebuild."); bNeedsHandoff = true; break;
        case EPullPathKind::Unsupported:
            Detail = TEXT("Symlink, submodule, executable or unsupported file mode. Review and integrate externally with the editor closed."); bNeedsHandoff = true; break;
        }
        Lines.Add(ChangeName(Change.Status) + TEXT("  ") + DisplayPath(Change.Path) + TEXT("\n    ") + Detail);
    }
    if (DirtyNames.Num()) Blockers.Insert(TEXT("Unsaved editor packages must be saved or deliberately resolved before Pull. Review does not save them."), 0);
    if (bNeedsHandoff) Blockers.Add(TEXT("Incoming files require an editor-close handoff. Automatic asset reload/restart is not enabled."));
    Review.bCanPull = Blockers.IsEmpty();
    Review.Blocker = Blockers.Num() ? Blockers[0] : FString();
    Review.Text = TEXT("INCOMING CHANGE REVIEW\n\nRepository: ") + DisplayPath(Remote.Root) + TEXT("\nBranch: ") + DisplayPath(Remote.Branch)
        + TEXT("\nUpstream: ") + DisplayPath(Remote.Remote) + TEXT(" / ") + DisplayPath(Remote.RemoteRef)
        + TEXT("\nLocal commit: ") + Remote.Head + TEXT("\nFetched commit: ") + Remote.RemoteHead
        + TEXT("\nFetched at: ") + Remote.FetchedAt.ToIso8601()
        + FString::Printf(TEXT("\n%d outgoing, %d incoming commits; %d changed paths.\n"), Remote.Ahead, Remote.Behind, Remote.IncomingChanges.Num());
    Review.Text += TEXT("\nThis is a snapshot. Reopen the review after editing, saving, changing loaded packages or refreshing Git. Pull repeats repository checks.\n");
    if (Remote.Ahead && Remote.Behind) Review.Text += TEXT("\nThe path list is a comparison of the two tips, not a predicted merge result.\n");
    Review.Text += Review.bCanPull ? TEXT("\nDocumentation update is eligible for fast-forward Pull after confirmation.\n") : TEXT("\nPULL BLOCKED\n") + FString::Join(Blockers, TEXT("\n")) + TEXT("\n");
    DirtyNames.Sort();
    if (DirtyNames.Num())
    {
        Review.Text += TEXT("\nUNSAVED PACKAGES\n");
        for (const auto& Name : DirtyNames) Review.Text += DisplayPath(Name) + TEXT("\n");
    }
    Review.Text += TEXT("\nAFFECTED FILES (renames appear as delete + add)\n") + (Lines.Num() ? FString::Join(Lines, TEXT("\n\n")) : TEXT("No tree changes."));
    if (bNeedsHandoff) Review.Text += TEXT("\n\nEDITOR-CLOSE HANDOFF\n1. Save or resolve unsaved packages, then preserve local Git changes explicitly.\n2. Close this project's editor instances.\n3. In an external Git client, fetch and review the upstream again; this snapshot may have expired.\n4. Integrate with fast-forward only. If diverged, stop and choose a reconciliation policy.\n5. Confirm LFS objects are hydrated before reopening. Rebuild first if source or plugins changed.\n6. Reopen the project and Refresh Git Workspace. Locks remain held until an explicit handoff/unlock.\n\nThis review does not execute the handoff, close the editor or replace package files.");
    return Review;
}
}
