// Copyright (c) 2014-2020 Sebastien Rombauts (sebastien.rombauts@gmail.com)
//
// Distributed under the MIT License (MIT) (See accompanying file LICENSE.txt
// or copy at http://opensource.org/licenses/MIT)

#include "GitSourceControlCommand.h"

FGitSourceControlCommand::FGitSourceControlCommand(const FSourceControlOperationRef& InOperation, const TSharedRef<IGitSourceControlWorker, ESPMode::ThreadSafe>& InWorker)
	: Operation(InOperation)
	, Worker(InWorker)
	, Cancel(MakeShared<GitCore::FCancelToken, ESPMode::ThreadSafe>())
{
}

void FGitSourceControlCommand::AddError(const FText& Message)
{
	if (!Message.IsEmpty())
	{
		ErrorMessages.Add(Message);
	}
}

void FGitSourceControlCommand::AddInfo(const FText& Message)
{
	if (!Message.IsEmpty())
	{
		InfoMessages.Add(Message);
	}
}

void FGitSourceControlCommand::AddError(const FString& Message)
{
	AddError(FText::FromString(Message));
}

void FGitSourceControlCommand::AddInfo(const FString& Message)
{
	AddInfo(FText::FromString(Message));
}
