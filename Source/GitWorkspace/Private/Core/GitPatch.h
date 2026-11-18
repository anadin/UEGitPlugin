// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#pragma once

#include "CoreMinimal.h"

/** Patches cut from a file's diff, so single lines can be staged, unstaged or discarded. */
namespace GitCore
{
	/** What happens to lines picked in a file's diff. */
	enum class ELineAction : uint8
	{
		/** Copies them from the working tree into the index. */
		Stage,
		/** Takes them back out of the index; they are picked in the staged diff. */
		Unstage,
		/** Puts the working tree's lines back as the index has them. */
		Discard
	};

	/** Lines picked in one file's diff: removed lines by their number in the old version, added lines by their number in the new one. */
	struct FLineSelection
	{
		TSet<int32> Removed;
		TSet<int32> Added;

		bool IsEmpty() const { return Removed.IsEmpty() && Added.IsEmpty(); }
	};

	/**
	 * Cuts one file's `git diff` down to the selected lines, as a patch for `git apply`. The changes left out stay
	 * where they are: applied forwards (staging), an unselected removed line stays as context and an unselected
	 * added line is dropped; applied in reverse (unstaging, discarding), the other way round. False with OutError
	 * when the diff cannot be cut (binary, combined, a submodule or a rename); OutPatch is empty when the
	 * selection changes nothing.
	 */
	bool BuildLinePatch(const FString& Diff, const FLineSelection& Selection, bool bReverse, FString& OutPatch, FString& OutError);

	/** A new file's content as a diff that adds every line; a NUL byte near the start makes it binary. */
	FString NewFileDiff(const TArray<uint8>& Bytes);
	/** New files larger than this are not shown as a diff. */
	constexpr int64 NewFileDiffLimit = 4 * 1024 * 1024;

	/** True for well-formed UTF-8, which decodes to text that encodes back to the same bytes. */
	bool IsValidUtf8(const uint8* Data, int32 Size);
}
