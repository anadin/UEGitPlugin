// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "GitSourceControlUserSettings.generated.h"

/** Per-user preferences for Git Workspace (Editor Preferences > Revision Control > Git Workspace). */
UCLASS(config = EditorPerProjectUserSettings, meta = (DisplayName = "Git Workspace"))
class UGitSourceControlUserSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	UGitSourceControlUserSettings();

	virtual FName GetContainerName() const override { return TEXT("Editor"); }
	virtual FName GetCategoryName() const override { return TEXT("Revision Control"); }

	/** Seconds between automatic status refreshes while the editor is in focus. */
	UPROPERTY(config, EditAnywhere, Category = "Refresh", meta = (ClampMin = "10", ClampMax = "3600"))
	int32 StatusRefreshSeconds = 60;

	/** Seconds between automatic checks of who holds which locks. */
	UPROPERTY(config, EditAnywhere, Category = "Refresh", meta = (ClampMin = "30", ClampMax = "3600"))
	int32 LockRefreshSeconds = 180;

	/** Seconds between automatic fetches from the remote; 0 turns background fetching off. */
	UPROPERTY(config, EditAnywhere, Category = "Refresh", meta = (ClampMin = "0", ClampMax = "86400"))
	int32 BackgroundFetchSeconds = 300;

	/** Submit (Check In) pushes the new commit after committing it. */
	UPROPERTY(config, EditAnywhere, Category = "Submit")
	bool bPushOnSubmit = true;

	/** How many revert backups to keep in Saved/GitSourceControl/Backups. */
	UPROPERTY(config, EditAnywhere, Category = "Backups", meta = (ClampMin = "1", ClampMax = "200"))
	int32 BackupsToKeep = 20;

	/** Days to keep revert backups. */
	UPROPERTY(config, EditAnywhere, Category = "Backups", meta = (ClampMin = "1", ClampMax = "365"))
	int32 BackupDays = 14;

	/** The Git panel lists files in their folders. Off: one list of paths. */
	UPROPERTY(config, EditAnywhere, Category = "Git Panel")
	bool bShowFolders = true;

	/** Set once the "prompt for check out" suggestion has been shown. */
	UPROPERTY(config)
	bool bOfferedCheckoutPrompt = false;
};
