// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#pragma once

#include "CoreMinimal.h"
#include "Core/GitTypes.h"

/** Small modal dialogs used by the Git panel. In unattended runs they answer Cancel. */
namespace GitWorkspaceDialogs
{
	/** One sentence, an optional list of files and one primary button. True when the user chose it. */
	bool Confirm(const FText& Title, const FText& Message, const TArray<FString>& Paths, const FText& PrimaryLabel);
	/**
	 * Asks before a push creates a branch on the server or changes the branch this one follows
	 * (FPushTarget::NeedsAgreement). True when the user agreed. bCommitFirst: the action commits, then pushes.
	 */
	bool ConfirmPush(const GitCore::FPushTarget& Target, bool bCommitFirst);
	/** Asks for one line of text. Validate returns an error to show, or empty text when the value is fine. */
	bool AskForText(const FText& Title, const FText& Prompt, const FText& Hint, FString& InOutValue, TFunction<FText(const FString&)> Validate = nullptr);

	/** While one is alive, Confirm shows nothing: it keeps each question and gives the answer (tests). */
	struct FScopedConfirmAnswer
	{
		explicit FScopedConfirmAnswer(bool bInAnswer);
		~FScopedConfirmAnswer();

		bool bAnswer;
		TArray<FText> Questions;
	};
}
