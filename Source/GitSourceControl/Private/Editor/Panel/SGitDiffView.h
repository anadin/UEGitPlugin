// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#pragma once

#include "CoreMinimal.h"
#include "Fonts/SlateFontInfo.h"
#include "Styling/SlateTypes.h"
#include "Types/SlateStructs.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/SListView.h"

class ITextDecorator;
class SScrollBox;

/** One line of a text diff, ready to show. */
struct FGitDiffLine
{
	enum class EKind : uint8
	{
		Context,
		Added,
		Removed,
		/** "@@ -12,4 +12,5 @@" and the function or section it is in. */
		Hunk,
		/** "No newline at end of file", "Binary file", and other remarks. */
		Note
	};

	EKind Kind = EKind::Context;
	/** Line numbers in the old and the new version; 0 where the line is not on that side. */
	int32 OldLine = 0;
	int32 NewLine = 0;
	/** The line without its diff marker, tabs expanded. */
	FString Text;
	/** For an edited line paired with its replacement: the part that differs, [ChangeStart, ChangeEnd). */
	int32 ChangeStart = 0;
	int32 ChangeEnd = 0;

	bool HasChange() const { return ChangeEnd > ChangeStart; }
};

namespace GitDiff
{
	/**
	 * Reads `git diff` or `git show` output for one file. Within a run of removed lines followed
	 * by added lines, each pair is compared to find the part that changed. Binary files and
	 * Git LFS pointers come back as a single note.
	 */
	TArray<TSharedPtr<FGitDiffLine>> Parse(const FString& UnifiedDiff);
	/** A new file's content as a diff that adds every line; binary content becomes a note. */
	FString FromNewFile(const TArray<uint8>& Bytes);
	/** Rich text for a line: its text escaped, with the changed part in <add> or <del>. */
	FString ToMarkup(const FGitDiffLine& Line);
	/** Whether diffs wrap long lines; one preference for every diff view. */
	bool WrapLines();
	void SetWrapLines(bool bWrap);
}

/** A colored, line-numbered view of a text diff. Selected lines copy with Ctrl+C. */
class SGitDiffView : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SGitDiffView) {}
	SLATE_END_ARGS()

	using FLinePtr = TSharedPtr<FGitDiffLine>;

	void Construct(const FArguments& InArgs);
	virtual bool SupportsKeyboardFocus() const override { return true; }
	virtual FReply OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent) override;
	virtual void Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime) override;

	/** Shows a unified diff. */
	void SetDiff(const FString& UnifiedDiff);
	/** Shows a message in place of a diff (loading, nothing to compare, an error). */
	void SetMessage(const FText& InMessage);
	/** Lines shown, for tests. */
	const TArray<FLinePtr>& GetLines() const { return Lines; }

private:
	TSharedRef<ITableRow> OnGenerateRow(FLinePtr Line, const TSharedRef<STableViewBase>& Owner);
	TSharedPtr<SWidget> OnContextMenu();
	void CopySelection() const;
	/** Unwrapped: wide enough for the longest line. Wrapped: the visible width. */
	FOptionalSize GetContentWidth() const;
	float GetViewportWidth() const;

	TArray<FLinePtr> Lines;
	TSharedPtr<SListView<FLinePtr>> List;
	TSharedPtr<SScrollBox> Scroll;
	FText Message;
	FSlateFontInfo Font;
	FTextBlockStyle TextStyle;
	FTextBlockStyle NoteStyle;
	/** Paint the changed part of an added or removed line on a stronger tint, wrapped or not. */
	TSharedPtr<ITextDecorator> AddedDecorator;
	TSharedPtr<ITextDecorator> RemovedDecorator;
	float CharWidth = 7.0f;
	float GutterWidth = 32.0f;
	int32 LongestLine = 0;
	int32 AddedCount = 0;
	int32 RemovedCount = 0;
	/** The wrap setting the rows were made with. */
	bool bBuiltWrapped = false;
};
