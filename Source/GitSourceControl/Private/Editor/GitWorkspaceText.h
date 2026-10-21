// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#pragma once

#include "CoreMinimal.h"
#include "Core/GitTypes.h"

struct FSlateBrush;

/** Short, localizable strings and file presentation shared by the status bar and the Git panel. */
namespace GitWorkspaceText
{
	/** "just now", "5 min ago", "3 h ago", "2 days ago". */
	FText Age(const FDateTime& Utc);
	/** "Modified", "Added", "Deleted", "Renamed", "New", "Conflict" for one side of a status entry. */
	FText ChangeKind(const GitCore::FStatusEntry& Entry, bool bStaged);
	/** Icon from the revision control style for one side of a status entry. */
	const FSlateBrush* ChangeIcon(const GitCore::FStatusEntry& Entry, bool bStaged);
	/** "Hero.uasset, Map.umap and 3 more". */
	FText FileList(const TArray<FString>& Paths, int32 Max = 3);
	/** Commit ids are shown with eight characters. */
	FString ShortId(const FString& Commit);
}

/** How a repository file is shown: the asset's name (an actor label for World Partition files), its class and icon. */
struct FGitFileDisplay
{
	FText Name;
	FText Folder;
	FText ClassName;
	const FSlateBrush* Icon = nullptr;
	/** True when the file is an Unreal package. */
	bool bPackage = false;

	/** Game thread only: reads the Asset Registry (or the package header of a file it does not know). */
	static FGitFileDisplay Make(const FString& AbsoluteFilename, const FString& RelativePath);
};
