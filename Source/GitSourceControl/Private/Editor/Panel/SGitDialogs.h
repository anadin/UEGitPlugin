// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#pragma once

#include "CoreMinimal.h"

/** Small modal dialogs used by the Git panel. In unattended runs they answer Cancel. */
namespace GitWorkspaceDialogs
{
	/** One sentence, an optional list of files and one primary button. True when the user chose it. */
	bool Confirm(const FText& Title, const FText& Message, const TArray<FString>& Paths, const FText& PrimaryLabel);
	/** Asks for one line of text. Validate returns an error to show, or empty text when the value is fine. */
	bool AskForText(const FText& Title, const FText& Prompt, const FText& Hint, FString& InOutValue, TFunction<FText(const FString&)> Validate = nullptr);
}
