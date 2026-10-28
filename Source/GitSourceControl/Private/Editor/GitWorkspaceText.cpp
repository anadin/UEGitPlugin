// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#include "GitWorkspaceText.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "RevisionControlStyle/RevisionControlStyle.h"
#include "Styling/SlateIconFinder.h"
#include "UObject/PrimaryAssetId.h"

#define LOCTEXT_NAMESPACE "GitWorkspaceText"

namespace GitWorkspaceText
{
	FText Age(const FDateTime& Utc)
	{
		const double Seconds = FMath::Max(0.0, (FDateTime::UtcNow() - Utc).GetTotalSeconds());
		if (Seconds < 60.0)
		{
			return LOCTEXT("JustNow", "just now");
		}
		if (Seconds < 3600.0)
		{
			return FText::Format(LOCTEXT("MinutesAgo", "{0} min ago"), FText::AsNumber(FMath::FloorToInt(Seconds / 60.0)));
		}
		if (Seconds < 2 * 86400.0)
		{
			return FText::Format(LOCTEXT("HoursAgo", "{0} h ago"), FText::AsNumber(FMath::FloorToInt(Seconds / 3600.0)));
		}
		return FText::Format(LOCTEXT("DaysAgo", "{0} days ago"), FText::AsNumber(FMath::FloorToInt(Seconds / 86400.0)));
	}

	namespace
	{
		TCHAR Code(const GitCore::FStatusEntry& Entry, bool bStaged)
		{
			if (Entry.bConflict)
			{
				return TEXT('U');
			}
			if (Entry.bUntracked)
			{
				return TEXT('?');
			}
			return bStaged ? Entry.Index : Entry.Working;
		}
	}

	FText ChangeKind(const GitCore::FStatusEntry& Entry, bool bStaged)
	{
		switch (Code(Entry, bStaged))
		{
		case TEXT('U'):
			return LOCTEXT("Conflict", "Conflict");
		case TEXT('?'):
			return LOCTEXT("New", "New");
		case TEXT('A'):
			return LOCTEXT("Added", "Added");
		case TEXT('D'):
			return LOCTEXT("Deleted", "Deleted");
		case TEXT('R'):
			return LOCTEXT("Renamed", "Renamed");
		case TEXT('C'):
			return LOCTEXT("Copied", "Copied");
		case TEXT('T'):
			return LOCTEXT("TypeChanged", "Type changed");
		default:
			return LOCTEXT("Modified", "Modified");
		}
	}

	const FSlateBrush* ChangeIcon(const GitCore::FStatusEntry& Entry, bool bStaged)
	{
		const ISlateStyle& Style = FRevisionControlStyleManager::Get();
		switch (Code(Entry, bStaged))
		{
		case TEXT('U'):
			return Style.GetBrush("RevisionControl.Conflicted");
		case TEXT('?'):
			return Style.GetBrush("RevisionControl.NotInDepot");
		case TEXT('A'):
		case TEXT('C'):
			return Style.GetBrush("RevisionControl.OpenForAdd");
		case TEXT('D'):
			return Style.GetBrush("RevisionControl.MarkedForDelete");
		case TEXT('R'):
			return Style.GetBrush("RevisionControl.Branched");
		default:
			return Style.GetBrush("RevisionControl.ModifiedLocally");
		}
	}

	FText FileList(const TArray<FString>& Paths, int32 Max)
	{
		TArray<FString> Names;
		for (int32 Index = 0; Index < Paths.Num() && Index < Max; ++Index)
		{
			Names.Add(FPaths::GetCleanFilename(Paths[Index]));
		}
		const FText Shown = FText::FromString(FString::Join(Names, TEXT(", ")));
		return Paths.Num() > Max ? FText::Format(LOCTEXT("FileListMore", "{0} and {1} more"), Shown, FText::AsNumber(Paths.Num() - Max)) : Shown;
	}

	FString ShortId(const FString& Commit)
	{
		return Commit.Left(8);
	}

	FText ClaimLockTooltip()
	{
		return LOCTEXT("ClaimLockTooltip", "Locked by you in another clone, worktree or computer. Claim the lock to save and check in the file here; "
			"nothing is sent to the server. Stop editing the file in the other copy.");
	}
}

namespace
{
	/** Mirrors the editor's changelist views: actor labels, display names and actor folders before object names. */
	FString AssetName(const FAssetData& Asset)
	{
		static const FName ActorLabel(TEXT("ActorLabel"));
		FString Name;
		if (Asset.GetTagValue(ActorLabel, Name) && !Name.IsEmpty())
		{
			return Name;
		}
		if (Asset.GetTagValue(FPrimaryAssetId::PrimaryAssetDisplayNameTag, Name) && !Name.IsEmpty())
		{
			return Name;
		}
		static const FName FolderLabel(TEXT("FolderLabel"));
		if (Asset.GetTagValue(FolderLabel, Name) && !Name.IsEmpty())
		{
			return Name;
		}
		return Asset.AssetName.ToString();
	}

	/**
	 * "Content/__ExternalActors__/Maps/Main/0/AB/XYZ.uasset" belongs to the level "Maps/Main", whose
	 * packages are in "Content/__ExternalActors__/Maps/Main". False for other files.
	 */
	bool SplitExternalPackage(const FString& RelativePath, FString& OutLevelFolder, FString& OutLevel)
	{
		for (const TCHAR* Folder : {TEXT("/__ExternalActors__/"), TEXT("/__ExternalObjects__/")})
		{
			const int32 Start = RelativePath.Find(Folder);
			if (Start != INDEX_NONE)
			{
				const int32 LevelStart = Start + FCString::Strlen(Folder);
				TArray<FString> Parts;
				RelativePath.Mid(LevelStart).ParseIntoArray(Parts, TEXT("/"));
				// Two hash folders and the file name follow the level path.
				if (Parts.Num() > 3)
				{
					Parts.SetNum(Parts.Num() - 3);
					OutLevel = FString::Join(Parts, TEXT("/"));
					OutLevelFolder = RelativePath.Left(LevelStart) + OutLevel;
					return true;
				}
			}
		}
		return false;
	}

	FString ExternalPackageLevel(const FString& RelativePath)
	{
		FString LevelFolder;
		FString Level;
		return SplitExternalPackage(RelativePath, LevelFolder, Level) ? Level : FString();
	}
}

FString GitWorkspaceText::TreeFolder(const FString& RelativePath)
{
	FString LevelFolder;
	FString Level;
	return SplitExternalPackage(RelativePath, LevelFolder, Level) ? LevelFolder : FPaths::GetPath(RelativePath);
}

FGitFileDisplay FGitFileDisplay::Make(const FString& AbsoluteFilename, const FString& RelativePath)
{
	FGitFileDisplay Display;
	Display.Name = FText::FromString(FPaths::GetCleanFilename(RelativePath));
	Display.Folder = FText::FromString(FPaths::GetPath(RelativePath));
	Display.Icon = FRevisionControlStyleManager::Get().GetBrush("RevisionControl.File");
	const FString Extension = FPaths::GetExtension(RelativePath);
	Display.bPackage = FPackageName::IsPackageExtension(*Extension);
	if (!Display.bPackage)
	{
		return Display;
	}
	Display.Name = FText::FromString(FPaths::GetBaseFilename(RelativePath));
	const FString Level = ExternalPackageLevel(RelativePath);
	if (!Level.IsEmpty())
	{
		Display.Folder = FText::Format(NSLOCTEXT("GitWorkspaceText", "InLevel", "in {0}"), FText::FromString(Level));
	}
	// Only the Asset Registry is asked: reading package headers from disk is slow and noisy for
	// files that are not valid packages (for example LFS pointers that were never downloaded).
	FString PackageName;
	TArray<FAssetData> Assets;
	if (!FPackageName::TryConvertFilenameToLongPackageName(AbsoluteFilename, PackageName))
	{
		return Display;
	}
	IAssetRegistry::GetChecked().GetAssetsByPackageName(FName(*PackageName), Assets, true);
	const FAssetData* Asset = Assets.FindByPredicate([](const FAssetData& Candidate) { return !Candidate.IsRedirector(); });
	if (!Asset)
	{
		return Display;
	}
	Display.Name = FText::FromString(AssetName(*Asset));
	Display.ClassName = FText::FromName(Asset->AssetClassPath.GetAssetName());
	if (const UClass* Class = Asset->GetClass())
	{
		Display.Icon = FSlateIconFinder::FindIconForClass(Class).GetIcon();
	}
	return Display;
}

#undef LOCTEXT_NAMESPACE
