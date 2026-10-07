// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#include "GitWorkspaceSaveFlow.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/Blueprint.h"
#include "Engine/Texture2D.h"
#include "HAL/FileManager.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/StrongObjectPtr.h"
#if PLATFORM_MAC
#include "GitWorkspaceSession.h"
#endif

namespace GitWorkspaceSave
{
bool SupportsAssetCopy(const UObject* Source)
{ return Source && (Source->GetClass() == UBlueprint::StaticClass() || Source->GetClass() == UTexture2D::StaticClass()); }
FAssetCopyDestination ReviewCopyDestination(UObject* Source, const FString& PackageName, const FString& Root, const FString& Content)
{
    check(IsInGameThread());
    FAssetCopyDestination Out;
    if (!SupportsAssetCopy(Source))
    { Out.Error = TEXT("Guarded Save As currently supports one ordinary Blueprint or Texture2D in its standard asset editor."); return Out; }
    if (!FPackageName::IsValidLongPackageName(PackageName, false) || PackageName == Source->GetOutermost()->GetName() ||
        !FPackageName::TryConvertLongPackageNameToFilename(PackageName, Out.Filename, FPackageName::GetAssetPackageExtension()))
    { Out.Error = TEXT("Choose a new valid asset name in game Content. Save As does not overwrite or rename the source."); return Out; }
    Out.Filename = FPaths::ConvertRelativePathToFull(Out.Filename);
    if (!FPaths::IsUnderDirectory(Out.Filename, Content))
    { Out.Error = TEXT("Choose a Save As destination inside this game's Content directory."); return Out; }
    FString CanonicalRoot = FPaths::ConvertRelativePathToFull(Root), RootSpelling = CanonicalRoot;
#if PLATFORM_MAC
    CanonicalRoot = GitWorkspaceSession::CanonicalPath(Root); RootSpelling.Empty();
    // Resolve only the checkout root's spelling (/var versus /private/var).
    // Keep every component below it literal so service checks still see and
    // reject a symlink or nested repository within Content.
    for (FString Ancestor = FPaths::GetPath(Out.Filename); !Ancestor.IsEmpty();)
    {
        if (GitWorkspaceSession::CanonicalPath(Ancestor) == CanonicalRoot) RootSpelling = Ancestor;
        const FString Parent = FPaths::GetPath(Ancestor); if (Parent == Ancestor) break; Ancestor = Parent;
    }
#endif
    if (CanonicalRoot.IsEmpty() || RootSpelling.IsEmpty() || !FPaths::IsUnderDirectory(Out.Filename, RootSpelling))
    { Out.Error = TEXT("Choose a Save As destination inside this checkout."); return Out; }
    Out.Path = Out.Filename; FPaths::MakePathRelativeTo(Out.Path, *(RootSpelling + TEXT("/")));
    Out.Filename = FPaths::Combine(CanonicalRoot, Out.Path);
    if (FindPackage(nullptr, *PackageName) || IFileManager::Get().FileExists(*Out.Filename) || IFileManager::Get().DirectoryExists(*Out.Filename))
    { Out.Error = TEXT("This destination already exists on disk or in memory. Choose another name. To retry an unsaved copy, open it and use Save."); return Out; }
    Out.PackageName = PackageName;
    return Out;
}
#if PLATFORM_MAC
GitWorkspace::FResult WriteAssetCopy(UObject* Source, const FAssetCopyDestination& Destination, GitWorkspace::FRepository& Repository,
    const GitWorkspace::FAssetSavePermit& Permit, const GitWorkspaceSession::FLease& Lease, const FString& Content, UObject*& OutCopy)
{
    check(IsInGameThread()); OutCopy = nullptr;
    auto Fail = [](const FString& Error) { GitWorkspace::FResult R; R.Error = Error; return R; };
    const auto Current = ReviewCopyDestination(Source, Destination.PackageName, Repository.Refresh().Root, Content);
    if (!Destination.Error.IsEmpty() || !Current.Error.IsEmpty() || Current.Path != Destination.Path || Current.Filename != Destination.Filename || !Permit.ContainsNewPath(Current.Path))
        return Fail(Current.Error.IsEmpty() ? TEXT("Save As destination does not match the prepared first-save permit.") : Current.Error);
    // Do this before StaticDuplicateObject: Unreal's inherited Save As creates
    // the copy even when its later save is cancelled or the destination is occupied.
    const auto Ready = Repository.ValidateAssetSave(Permit, Current.Path, Lease);
    if (!Ready.Ok()) return Ready;
    TStrongObjectPtr<UObject> Original(Source);
    FPreparedScope Prepared(Repository, Permit, Lease, Repository.Refresh().Root);
    UPackage* Package = CreatePackage(*Current.PackageName);
    TStrongObjectPtr<UObject> Copy(StaticDuplicateObject(Original.Get(), Package, *FPackageName::GetLongPackageAssetName(Current.PackageName)));
    if (!Copy) return Fail(TEXT("Could not create the asset copy. Its acquired destination lock remains held."));
    Copy->ClearFlags(RF_Transient); Copy->SetFlags(RF_Public | RF_Standalone); Copy->MarkPackageDirty();
    if (Original->GetOutermost()->HasAnyPackageFlags(PKG_DisallowExport)) Package->SetPackageFlags(PKG_DisallowExport);
    FAssetRegistryModule::AssetCreated(Copy.Get()); OutCopy = Copy.Get();
    FSavePackageArgs Args; Args.TopLevelFlags = RF_Public | RF_Standalone; Args.SaveFlags = SAVE_NoError;
    if (!UPackage::SavePackage(Package, Copy.Get(), *Current.Filename, Args))
        return Fail(TEXT("The copy could not be saved. The original and unsaved copy remain in memory; the destination lock remains held. Open the copy and use Save to retry after reviewing the log and verifying locks."));
    UPackage::WaitForAsyncFileWrites();
    GitWorkspace::FResult R; R.Code = 0; return R;
}
#endif
}
