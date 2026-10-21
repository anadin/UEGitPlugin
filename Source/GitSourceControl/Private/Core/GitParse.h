// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#pragma once

#include "GitTypes.h"

/**
 * Strict parsers for machine-readable git output. Every parser rejects incomplete or
 * malformed input rather than returning a partial result that could look valid.
 */
namespace GitCore
{
	/** Splits NUL-terminated records. Fails when the final record is unterminated. */
	bool SplitNulRecords(const TArray<uint8>& Bytes, TArray<FString>& OutRecords);

	/** True for a full SHA-1 or SHA-256 object id. */
	bool IsObjectId(const FString& Value);

	/** True for a normalized repository-relative path with no "." / ".." / ".git" components. */
	bool IsSafeRelativePath(const FString& Path);

	/** Parses `git status --porcelain=v2 -z --branch`. */
	bool ParseStatus(const TArray<uint8>& Bytes, FStatusSnapshot& Out, FString& OutError);

	/** Parses `git lfs locks --verify --json`. Ownership comes only from the server's ours/theirs split. */
	bool ParseVerifiedLocks(const FString& Json, TMap<FString, FLockInfo>& OutLocks, FString& OutError);

	/** Parses the response of `git lfs lock --json` (a single lock or an array of locks). */
	bool ParseLockResponse(const FString& Json, TArray<FLockInfo>& OutLocks, FString& OutError);

	/** Format used with ParseLog: `--format=%H%x00%P%x00%an%x00%aI%x00%s` with `-z`. */
	extern const TCHAR* const LogFormat;
	bool ParseLog(const TArray<uint8>& Bytes, TArray<FHistoryCommit>& OutCommits, FString& OutError);

	/** Format used with ParseFileLog: `--format=%x01%H%x00%an%x00%aI%x00%s` with `-z --name-status --follow`. */
	extern const TCHAR* const FileLogFormat;
	bool ParseFileLog(const TArray<uint8>& Bytes, TArray<FFileRevision>& OutRevisions, FString& OutError);

	/** Parses `git diff --raw -z --no-renames` (or diff-tree) records. */
	bool ParseRawDiff(const TArray<uint8>& Bytes, TArray<FHistoryChange>& OutChanges, FString& OutError);

	/** Parses `--name-only -z` output into a set of paths. */
	bool ParseNameList(const TArray<uint8>& Bytes, TSet<FString>& OutPaths);
}
