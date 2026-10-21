// Copyright (c) 2014-2020 Sebastien Rombauts (sebastien.rombauts@gmail.com)
//
// Distributed under the MIT License (MIT) (See accompanying file LICENSE.txt
// or copy at http://opensource.org/licenses/MIT)

#pragma once

#include "CoreMinimal.h"
#include "ISourceControlRevision.h"
#include "Core/GitRepository.h"

/** One commit in a file's history. Get() exports that version (LFS content included) for diffing. */
class FGitSourceControlRevision final : public ISourceControlRevision
{
public:
	virtual bool Get(FString& InOutFilename, EConcurrency::Type InConcurrency = EConcurrency::Synchronous) const override;
	virtual bool GetAnnotated(TArray<FAnnotationLine>& OutLines) const override { return false; }
	virtual bool GetAnnotated(FString& InOutFilename) const override { return false; }
	virtual const FString& GetFilename() const override { return LocalFilename; }
	virtual int32 GetRevisionNumber() const override { return RevisionNumber; }
	virtual const FString& GetRevision() const override { return ShortCommitId; }
	virtual const FString& GetDescription() const override { return Description; }
	virtual const FString& GetUserName() const override { return UserName; }
	virtual const FString& GetClientSpec() const override { return ClientSpec; }
	virtual const FString& GetAction() const override { return Action; }
	virtual TSharedPtr<ISourceControlRevision, ESPMode::ThreadSafe> GetBranchSource() const override { return nullptr; }
	virtual const FDateTime& GetDate() const override { return Date; }
	virtual int32 GetCheckInIdentifier() const override { return RevisionNumber; }
	virtual int32 GetFileSize() const override { return FileSize; }

	/** Absolute path of the file in the working tree. */
	FString LocalFilename;
	/** Repository-relative path of the file in this commit (differs after renames). */
	FString PathInCommit;
	FString CommitId;
	FString ShortCommitId;
	FString Description;
	FString UserName;
	FString ClientSpec;
	/** "add", "edit", "delete" or "branch" (renames and copies). */
	FString Action;
	/** Oldest revision is 1; higher numbers are newer. */
	int32 RevisionNumber = 0;
	FDateTime Date;
	int32 FileSize = 0;
	GitCore::FGitRepositoryPtr Repository;
};
