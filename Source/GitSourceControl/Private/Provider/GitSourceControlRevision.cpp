// Copyright (c) 2014-2020 Sebastien Rombauts (sebastien.rombauts@gmail.com)
//
// Distributed under the MIT License (MIT) (See accompanying file LICENSE.txt
// or copy at http://opensource.org/licenses/MIT)

#include "GitSourceControlRevision.h"

#include "HAL/FileManager.h"
#include "ISourceControlModule.h"
#include "Misc/Paths.h"

bool FGitSourceControlRevision::Get(FString& InOutFilename, EConcurrency::Type InConcurrency) const
{
	if (!Repository.IsValid())
	{
		return false;
	}
	if (InOutFilename.IsEmpty())
	{
		// Diff tools load the file as a package, so it must live under Saved/ and keep its extension.
		// One folder per repository path keeps same-named files apart; the revision in the name keeps packages apart.
		const FString Folder = FString::Printf(TEXT("%08x"), FCrc::StrCrc32(*PathInCommit));
		const FString Name = FPaths::GetBaseFilename(PathInCommit) + TEXT("-") + ShortCommitId + FPaths::GetExtension(PathInCommit, true);
		InOutFilename = FPaths::ConvertRelativePathToFull(FPaths::DiffDir() / Folder / Name);
		// The name identifies the revision, and exports are renamed into place only when complete.
		if (IFileManager::Get().FileSize(*InOutFilename) > 0)
		{
			return true;
		}
	}
	const GitCore::FOpResult Result = Repository->ExportRevision(CommitId, PathInCommit, InOutFilename);
	if (!Result.bOk)
	{
		UE_LOG(LogSourceControl, Warning, TEXT("%s %s"), *Result.Message, *Result.Details);
	}
	return Result.bOk;
}
