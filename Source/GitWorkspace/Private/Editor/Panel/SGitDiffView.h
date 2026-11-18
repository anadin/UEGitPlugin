// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#pragma once

#include "CoreMinimal.h"
#include "Core/GitPatch.h"
#include "Fonts/SlateFontInfo.h"
#include "Styling/SlateTypes.h"
#include "Types/SlateStructs.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/SListView.h"

class ITextDecorator;
class SOverlay;
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
	/** Rich text for a line: its text escaped, with the changed part in <add> or <del>. */
	FString ToMarkup(const FGitDiffLine& Line);
	/** Whether diffs wrap long lines; one preference for every diff view. */
	bool WrapLines();
	void SetWrapLines(bool bWrap);
}

/**
 * A colored, line-numbered view of a text diff. Drag over lines, or click and Shift+click, to select them; selected
 * lines copy with Ctrl+C, and clicking a hunk's header selects the hunk. When the owner offers line actions, buttons
 * next to the selection stage, unstage or discard its changed lines, as do Space and Delete.
 */
class SGitDiffView : public SCompoundWidget
{
public:
	/** Stage, unstage or discard the changed lines picked in the diff shown (GetDiffText). False when nothing started (a discard the user cancelled). */
	DECLARE_DELEGATE_RetVal_TwoParams(bool, FOnLineAction, GitCore::ELineAction, const GitCore::FLineSelection&);

	SLATE_BEGIN_ARGS(SGitDiffView) {}
		SLATE_EVENT(FOnLineAction, OnLineAction)
	SLATE_END_ARGS()

	using FLinePtr = TSharedPtr<FGitDiffLine>;

	void Construct(const FArguments& InArgs);
	virtual bool SupportsKeyboardFocus() const override { return true; }
	virtual FReply OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent) override;
	virtual FReply OnMouseMove(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;
	virtual FReply OnMouseButtonUp(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;
	virtual void OnMouseCaptureLost(const FCaptureLostEvent& CaptureLostEvent) override;
	virtual void Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime) override;

	/**
	 * Shows a unified diff. The same diff again changes nothing, so a refresh keeps the selection. bKeepPosition keeps
	 * the scroll position of a different one: the same file, reloaded after a change.
	 */
	void SetDiff(const FString& UnifiedDiff, bool bKeepPosition = false);
	/** Shows a message in place of a diff (loading, nothing to compare, an error). */
	void SetMessage(const FText& InMessage);
	/** What the selected lines offer to do; none (the default) shows no buttons. */
	void SetLineActions(const TArray<GitCore::ELineAction>& Actions);
	/** The diff shown, as given to SetDiff; empty while a message shows. */
	const FString& GetDiffText() const { return DiffText; }
	/** The added and removed lines selected. */
	GitCore::FLineSelection GetSelectedLines() const;
	/** Asks the owner to apply Action to the selected lines, as its button does. False when Action is not offered or no changed line is selected. */
	bool RunLineAction(GitCore::ELineAction Action);
	/** Lines shown, for tests. */
	const TArray<FLinePtr>& GetLines() const { return Lines; }
	/** The right-click menu, each entry with its shortcut; null while no diff shows. */
	TSharedPtr<SWidget> OnContextMenu();

private:
	TSharedRef<ITableRow> OnGenerateRow(FLinePtr Line, const TSharedRef<STableViewBase>& Owner);
	void OnSelectionChanged(FLinePtr Line, ESelectInfo::Type SelectInfo);
	void CopySelection() const;
	/** Unwrapped: wide enough for the longest line. Wrapped: the visible width. */
	FOptionalSize GetContentWidth() const;
	float GetViewportWidth() const;

	/** Stage, Unstage and Discard buttons, shown next to the first selected line in view. */
	TSharedRef<SWidget> MakeLineActions();
	bool CanRunLineAction(GitCore::ELineAction Action) const;
	EVisibility GetLineActionsVisibility() const;
	FMargin GetLineActionsPadding() const;
	/** Where the first selected line in view starts, down from the top of the list. */
	TOptional<float> FindSelectedRowTop() const;
	/** The line under a screen position: above the lines in view the first of them, below them the last. */
	int32 FindLineAt(float ScreenY) const;
	/** The range of lines in view, some of them possibly only partly. */
	void GetLinesInView(int32& OutFirst, int32& OutLast) const;

	/** Dragging over lines selects them, from the line the drag started on. */
	FReply OnRowDragDetected(const FGeometry& Geometry, const FPointerEvent& MouseEvent, FLinePtr Line);
	void SelectDraggedLines();

	FOnLineAction OnLineAction;
	TArray<GitCore::ELineAction> LineActions;
	/** A line action was asked for and the diff has not been reloaded since: Space and Delete wait for it. */
	bool bLineActionPending = false;
	/** The selection holds an added or removed line. */
	bool bChangedLinesSelected = false;

	FString DiffText;
	TArray<FLinePtr> Lines;
	TSharedPtr<SListView<FLinePtr>> List;
	TSharedPtr<SScrollBox> Scroll;
	TSharedPtr<SOverlay> ListArea;
	TSharedPtr<SWidget> LineActionsBox;
	FLinePtr DragAnchor;
	bool bDragSelecting = false;
	float DragY = 0.0f;
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
