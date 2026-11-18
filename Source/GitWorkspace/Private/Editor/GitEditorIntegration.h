// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#pragma once

#include "CoreMinimal.h"

/** The Git panel, status bar, menus and one-time suggestions. Editor only; never used in commandlets. */
namespace GitEditorIntegration
{
	void Register();
	void Unregister();
	/** Opens the Git panel with the changes of these files selected. */
	void ShowInPanel(const TArray<FString>& AbsoluteFiles);
}
