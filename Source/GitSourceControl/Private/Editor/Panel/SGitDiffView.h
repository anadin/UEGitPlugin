// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#pragma once

#include "CoreMinimal.h"
#include "Fonts/SlateFontInfo.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/SListView.h"

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
	float GetContentWidth() const;

	TArray<FLinePtr> Lines;
	TSharedPtr<SListView<FLinePtr>> List;
	TSharedPtr<SScrollBox> Scroll;
	FText Message;
	FSlateFontInfo Font;
	float CharWidth = 7.0f;
	float GutterWidth = 32.0f;
	int32 LongestLine = 0;
};
