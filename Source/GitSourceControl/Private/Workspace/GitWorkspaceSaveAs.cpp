// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#include "GitWorkspaceSaveFlow.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/Blueprint.h"
#include "Engine/Texture2D.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionComment.h"
#include "MaterialEditor/PreviewMaterial.h"
#include "MaterialGraph/MaterialGraph.h"
#include "MaterialEditingLibrary.h"
#include "MaterialShared.h"
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
{ return Source && (Source->GetClass() == UBlueprint::StaticClass() || Source->GetClass() == UTexture2D::StaticClass() || Source->GetClass() == UMaterial::StaticClass() || Source->GetClass() == UMaterialInstanceConstant::StaticClass() || Source->GetClass() == UMaterialFunction::StaticClass()); }
FString ReviewCopyData(UObject* Source, UObject* EditedData)
{
    if (!SupportsAssetCopy(Source) || !EditedData)
        return TEXT("Guarded Save As requires one ordinary Blueprint, Texture2D, Material, Material Instance or Material Function in its standard asset editor.");
    if (Source == EditedData) return FString();
    const auto* Preview = Cast<UMaterial>(EditedData);
    if (Preview && EditedData->GetOutermost() == GetTransientPackage() && Preview->bIsPreviewMaterial && Preview->MaterialGraph && Preview->MaterialGraph->Material == Preview)
    {
        const auto* Function = Preview->MaterialGraph->MaterialFunction.Get();
        if (Source->GetClass() == UMaterial::StaticClass() && EditedData->GetClass() == UPreviewMaterial::StaticClass() && !Function) return FString();
        if (Source->GetClass() == UMaterialFunction::StaticClass() && EditedData->GetClass() == UMaterial::StaticClass() && Function &&
            Function->GetClass() == UMaterialFunction::StaticClass() && Function->GetOutermost() == GetTransientPackage() && Function->ParentFunction == Source) return FString();
    }
    return TEXT("The asset editor's current copy data is unavailable or belongs to another asset. Reopen its standard editor before Save As.");
}
FAssetCopyDestination ReviewCopyDestination(UObject* Source, const FString& PackageName, const FString& Root, const FString& Content)
{
    if (!SupportsAssetCopy(Source))
    { FAssetCopyDestination Out; Out.Error = TEXT("Guarded Save As currently supports one ordinary Blueprint, Texture2D, Material, Material Instance or Material Function in its standard asset editor."); return Out; }
    return ReviewAbsentDestination(PackageName, Source->GetOutermost()->GetName(), Root, Content, false);
}
FAssetCopyDestination ReviewAbsentDestination(const FString& PackageName, const FString& SourcePackage, const FString& Root, const FString& Content, bool bMap)
{
    check(IsInGameThread());
    FAssetCopyDestination Out;
    if (!FPackageName::IsValidLongPackageName(PackageName, false) || PackageName == SourcePackage ||
        !FPackageName::TryConvertLongPackageNameToFilename(PackageName, Out.Filename, bMap ? FPackageName::GetMapPackageExtension() : FPackageName::GetAssetPackageExtension()))
    { Out.Error = TEXT("Choose a fresh valid package name in game Content. Existing destinations are never overwritten."); return Out; }
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
    if (FindPackage(nullptr, *PackageName) || FPackageName::DoesPackageExist(PackageName) || IFileManager::Get().FileExists(*Out.Filename) || IFileManager::Get().DirectoryExists(*Out.Filename))
    { Out.Error = bMap ? TEXT("This map destination already exists on disk or in memory. Choose another name, or use Save / Save All to review an unsaved named map or copy.") : TEXT("This destination already exists on disk or in memory. Choose another name. To retry an unsaved copy, open it and use Save."); return Out; }
    Out.PackageName = PackageName;
    return Out;
}
#if PLATFORM_MAC
GitWorkspace::FResult WriteAssetCopy(UObject* Source, const FAssetCopyDestination& Destination, GitWorkspace::FRepository& Repository,
    const GitWorkspace::FAssetSavePermit& Permit, const GitWorkspaceSession::FLease& Lease, const FString& Content, UObject*& OutCopy, UObject* EditedData)
{
    check(IsInGameThread()); OutCopy = nullptr;
    auto Fail = [](const FString& Error) { GitWorkspace::FResult R; R.Error = Error; return R; };
    if (!EditedData) EditedData = Source;
    const FString DataError = ReviewCopyData(Source, EditedData);
    if (!DataError.IsEmpty()) return Fail(DataError);
    const auto Current = ReviewCopyDestination(Source, Destination.PackageName, Repository.Refresh().Root, Content);
    if (!Destination.Error.IsEmpty() || !Current.Error.IsEmpty() || Current.Path != Destination.Path || Current.Filename != Destination.Filename || !Permit.ContainsNewPath(Current.Path))
        return Fail(Current.Error.IsEmpty() ? TEXT("Save As destination does not match the prepared first-save permit.") : Current.Error);
    // Do this before StaticDuplicateObject: Unreal's inherited Save As creates
    // the copy even when its later save is cancelled or the destination is occupied.
    const auto Ready = Repository.ValidateAssetSave(Permit, Current.Path, Lease);
    if (!Ready.Ok()) return Ready;
    TStrongObjectPtr<UObject> Original(Source);
    TStrongObjectPtr<UObject> Data(EditedData);
    UObject* DuplicateData = Data.Get();
    if (auto* Material = Cast<UMaterial>(EditedData))
    {
        if (const auto* Resource = Material->GetMaterialResource(GMaxRHIShaderPlatform); Resource && !Resource->GetCompileErrors().IsEmpty())
            return Fail(TEXT("The material preview has shader errors. Fix them before Save As; the acquired destination lock remains held."));
        // Synchronize graph links only after consent and final lock validation.
        // This never applies the preview to the original material or the world.
        if (Material->MaterialGraph) Material->MaterialGraph->LinkMaterialExpressionsFromGraph();
        if (Source->GetClass() == UMaterialFunction::StaticClass() && Source != EditedData)
        {
            // The function preview's collection can lag behind added/deleted
            // graph nodes. Synchronize it without Apply or source replacement.
            auto* Function = Material->MaterialGraph->MaterialFunction.Get();
            Function->AssignExpressionCollection(Material->GetExpressionCollection());
            DuplicateData = Function;
        }
    }
    FPreparedScope Prepared(Repository, Permit, Lease, Repository.Refresh().Root);
    UPackage* Package = CreatePackage(*Current.PackageName);
    // A preview material is an editor-only class: the saved copy must be UMaterial.
    TStrongObjectPtr<UObject> Copy(StaticDuplicateObject(DuplicateData, Package, *FPackageName::GetLongPackageAssetName(Current.PackageName), RF_AllFlags, Original->GetClass()));
    if (!Copy) return Fail(TEXT("Could not create the asset copy. Its acquired destination lock remains held."));
    Copy->ClearFlags(RF_Transient); Copy->SetFlags(RF_Public | RF_Standalone); Copy->MarkPackageDirty();
    if (Original->GetOutermost()->HasAnyPackageFlags(PKG_DisallowExport)) Package->SetPackageFlags(PKG_DisallowExport);
    FAssetRegistryModule::AssetCreated(Copy.Get()); OutCopy = Copy.Get();
    if (auto* Material = Cast<UMaterial>(Copy.Get()))
    {
        Material->bIsPreviewMaterial = false; Material->MaterialGraph = nullptr;
        Material->bAllowDevelopmentShaderCompile = CastChecked<UMaterial>(Original.Get())->bAllowDevelopmentShaderCompile;
        for (UMaterialExpression* Expression : Material->GetExpressions()) if (Expression) { Expression->Material = Material; Expression->Function = nullptr; Expression->GraphNode = nullptr; }
        for (UMaterialExpressionComment* Comment : Material->GetEditorComments()) if (Comment) { Comment->Material = Material; Comment->Function = nullptr; Comment->GraphNode = nullptr; }
        const auto Errors = UMaterialEditingLibrary::RecompileMaterial(Material);
        if (!Errors.IsEmpty()) return Fail(TEXT("The material copy has shader errors and was not saved. Its unsaved copy and destination lock remain held. Open the copy and fix the errors, then use Save to retry."));
    }
    else if (auto* Function = Cast<UMaterialFunction>(Copy.Get()))
    {
        Function->ParentFunction = nullptr; Function->PreviewMaterial = nullptr; Function->EditorMaterial = nullptr; Function->MaterialGraph = nullptr;
        // Unreal deliberately clears library exposure when duplicating a
        // function. Save As, like Apply, must retain the edited value.
        Function->bExposeToLibrary = CastChecked<UMaterialFunction>(DuplicateData)->bExposeToLibrary;
        for (UMaterialExpression* Expression : Function->GetExpressions()) if (Expression) { Expression->Material = nullptr; Expression->Function = Function; Expression->GraphNode = nullptr; }
        for (UMaterialExpressionComment* Comment : Function->GetEditorComments()) if (Comment) { Comment->Material = nullptr; Comment->Function = Function; Comment->GraphNode = nullptr; }
        UMaterialEditingLibrary::UpdateMaterialFunction(Function, nullptr);
    }
    FSavePackageArgs Args; Args.TopLevelFlags = RF_Public | RF_Standalone; Args.SaveFlags = SAVE_NoError;
    if (!UPackage::SavePackage(Package, Copy.Get(), *Current.Filename, Args))
        return Fail(TEXT("The copy could not be saved. The original and unsaved copy remain in memory; the destination lock remains held. Open the copy and use Save to retry after reviewing the log and verifying locks."));
    UPackage::WaitForAsyncFileWrites();
    GitWorkspace::FResult R; R.Code = 0; return R;
}
#endif
}
