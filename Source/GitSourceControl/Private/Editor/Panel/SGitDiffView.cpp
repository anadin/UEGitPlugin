// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#include "SGitDiffView.h"

#include "Provider/GitSourceControlUserSettings.h"

#include "Fonts/FontMeasure.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "Framework/Text/ILayoutBlock.h"
#include "Framework/Text/ITextDecorator.h"
#include "Framework/Text/SlateTextRun.h"
#include "HAL/PlatformApplicationMisc.h"
#include "Misc/FileHelper.h"
#include "Rendering/DrawElements.h"
#include "Rendering/SlateRenderer.h"
#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBar.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Text/SRichTextBlock.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/STableRow.h"

#define LOCTEXT_NAMESPACE "GitWorkspaceDiff"

namespace
{
	/** Diffs longer than this are cut, so a huge generated file cannot stall the editor. */
	constexpr int32 MaxDiffCharacters = 4 * 1024 * 1024;

	// Tints that read on both the dark and the light editor themes.
	const FLinearColor AddedLine(0.10f, 0.60f, 0.20f, 0.16f);
	const FLinearColor AddedPart(0.10f, 0.70f, 0.25f, 0.45f);
	const FLinearColor RemovedLine(0.85f, 0.15f, 0.15f, 0.16f);
	const FLinearColor RemovedPart(0.90f, 0.20f, 0.20f, 0.45f);
	const FLinearColor HunkLine(0.25f, 0.50f, 0.95f, 0.14f);
	const FLinearColor AddedMarker(0.15f, 0.65f, 0.25f, 1.0f);
	const FLinearColor RemovedMarker(0.85f, 0.25f, 0.25f, 1.0f);

	FString ExpandTabs(const FString& Line)
	{
		int32 Tab = INDEX_NONE;
		if (!Line.FindChar(TEXT('\t'), Tab))
		{
			return Line;
		}
		FString Expanded;
		Expanded.Reserve(Line.Len() + 16);
		for (const TCHAR Character : Line)
		{
			if (Character == TEXT('\t'))
			{
				Expanded += FString::ChrN(4 - Expanded.Len() % 4, TEXT(' '));
			}
			else
			{
				Expanded.AppendChar(Character);
			}
		}
		return Expanded;
	}

	/** "@@ -12,4 +12,5 @@ Section": the first old and new line numbers. */
	void ReadHunkStart(const FString& Header, int32& OutOld, int32& OutNew)
	{
		const int32 Minus = Header.Find(TEXT(" -"));
		const int32 Plus = Header.Find(TEXT(" +"));
		OutOld = Minus != INDEX_NONE ? FCString::Atoi(*Header.Mid(Minus + 2)) : 0;
		OutNew = Plus != INDEX_NONE ? FCString::Atoi(*Header.Mid(Plus + 2)) : 0;
	}

	/** Marks what differs between a removed line and the line that replaced it, when most of it stayed. */
	void MarkChange(FGitDiffLine& Removed, FGitDiffLine& Added)
	{
		const FString& Old = Removed.Text;
		const FString& New = Added.Text;
		const int32 Shorter = FMath::Min(Old.Len(), New.Len());
		int32 Prefix = 0;
		while (Prefix < Shorter && Old[Prefix] == New[Prefix])
		{
			++Prefix;
		}
		int32 Suffix = 0;
		while (Suffix < Shorter - Prefix && Old[Old.Len() - 1 - Suffix] == New[New.Len() - 1 - Suffix])
		{
			++Suffix;
		}
		const int32 Longer = FMath::Max(Old.Len(), New.Len());
		if (Longer == 0 || (Prefix + Suffix) * 3 < Longer)
		{
			// Mostly rewritten: the line colors say enough.
			return;
		}
		Removed.ChangeStart = Prefix;
		Removed.ChangeEnd = Old.Len() - Suffix;
		Added.ChangeStart = Prefix;
		Added.ChangeEnd = New.Len() - Suffix;
	}

	TSharedPtr<FGitDiffLine> MakeLine(FGitDiffLine::EKind Kind, const FString& Text, int32 OldLine = 0, int32 NewLine = 0)
	{
		const TSharedPtr<FGitDiffLine> Line = MakeShared<FGitDiffLine>();
		Line->Kind = Kind;
		Line->Text = Text;
		Line->OldLine = OldLine;
		Line->NewLine = NewLine;
		return Line;
	}

	bool IsLfsPointerLine(const FString& Text)
	{
		return Text.StartsWith(TEXT("version https://git-lfs.github.com/spec/")) || Text.StartsWith(TEXT("oid sha256:")) || Text.StartsWith(TEXT("size "));
	}

	FString EscapeMarkup(const FString& Text)
	{
		return Text.Replace(TEXT("&"), TEXT("&amp;")).Replace(TEXT("<"), TEXT("&lt;")).Replace(TEXT(">"), TEXT("&gt;")).Replace(TEXT("\""), TEXT("&quot;"));
	}

	/** Text on a tinted background; each wrapped piece paints its own. */
	class FTintedRun : public FSlateTextRun
	{
	public:
		static TSharedRef<FTintedRun> Create(const FRunInfo& InRunInfo, const TSharedRef<const FString>& InText, const FTextBlockStyle& InStyle, const FTextRange& InRange,
			const FLinearColor& InTint)
		{
			return MakeShareable(new FTintedRun(InRunInfo, InText, InStyle, InRange, InTint));
		}

		virtual int32 OnPaint(const FPaintArgs& PaintArgs, const FTextArgs& TextArgs, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect,
			FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const override
		{
			// Block sizes and offsets are pre-scaled, as in FSlateTextRun::OnPaint.
			const float InverseScale = Inverse(AllottedGeometry.Scale);
			const TSharedRef<ILayoutBlock>& Block = TextArgs.Block;
			FSlateDrawElement::MakeBox(OutDrawElements, LayerId,
				AllottedGeometry.ToPaintGeometry(TransformVector(InverseScale, Block->GetSize()), FSlateLayoutTransform(TransformPoint(InverseScale, Block->GetLocationOffset()))),
				FAppStyle::GetBrush("WhiteBrush"), ESlateDrawEffect::None, Tint);
			return FSlateTextRun::OnPaint(PaintArgs, TextArgs, AllottedGeometry, MyCullingRect, OutDrawElements, LayerId + 1, InWidgetStyle, bParentEnabled);
		}

		virtual TSharedRef<IRun> Clone() const override
		{
			return MakeShareable(new FTintedRun(*this));
		}

	private:
		FTintedRun(const FRunInfo& InRunInfo, const TSharedRef<const FString>& InText, const FTextBlockStyle& InStyle, const FTextRange& InRange, const FLinearColor& InTint)
			: FSlateTextRun(InRunInfo, InText, InStyle, InRange)
			, Tint(InTint)
		{
		}

		FTintedRun(const FTintedRun& Other) = default;

		FLinearColor Tint;
	};

	/** Makes <add> and <del> runs: the changed part of an added or removed line. */
	class FTintDecorator : public ITextDecorator
	{
	public:
		FTintDecorator(const FString& InName, const FTextBlockStyle& InStyle, const FLinearColor& InTint)
			: Name(InName)
			, Style(InStyle)
			, Tint(InTint)
		{
		}

		virtual bool Supports(const FTextRunParseResults& RunParseResult, const FString& Text) const override
		{
			return RunParseResult.Name == Name;
		}

		virtual TSharedRef<ISlateRun> Create(const TSharedRef<FTextLayout>& TextLayout, const FTextRunParseResults& RunParseResult, const FString& OriginalText,
			const TSharedRef<FString>& InOutModelText, const ISlateStyle* SlateStyle) override
		{
			FTextRange ModelRange;
			ModelRange.BeginIndex = InOutModelText->Len();
			*InOutModelText += OriginalText.Mid(RunParseResult.ContentRange.BeginIndex, RunParseResult.ContentRange.Len());
			ModelRange.EndIndex = InOutModelText->Len();
			return FTintedRun::Create(FRunInfo(RunParseResult.Name), InOutModelText, Style, ModelRange, Tint);
		}

	private:
		FString Name;
		FTextBlockStyle Style;
		FLinearColor Tint;
	};
}

TArray<TSharedPtr<FGitDiffLine>> GitDiff::Parse(const FString& UnifiedDiff)
{
	TArray<FString> Raw;
	UnifiedDiff.ParseIntoArray(Raw, TEXT("\n"), false);
	TArray<TSharedPtr<FGitDiffLine>> Lines;
	bool bInHunk = false;
	bool bCombined = false;
	bool bBinary = false;
	int32 OldLine = 0;
	int32 NewLine = 0;
	for (FString& Text : Raw)
	{
		if (Text.EndsWith(TEXT("\r")))
		{
			Text.LeftChopInline(1);
		}
		if (Text.StartsWith(TEXT("@@")))
		{
			// Merge commits show a combined diff ("@@@"), with one marker column per parent.
			bInHunk = true;
			bCombined = Text.StartsWith(TEXT("@@@"));
			ReadHunkStart(Text, OldLine, NewLine);
			Lines.Add(MakeLine(FGitDiffLine::EKind::Hunk, Text));
			continue;
		}
		if (Text.StartsWith(TEXT("diff --git ")) || Text.StartsWith(TEXT("diff --cc ")))
		{
			bInHunk = false;
			continue;
		}
		if (!bInHunk)
		{
			// File headers: index, ---/+++, modes, renames.
			bBinary |= Text.StartsWith(TEXT("Binary files ")) || Text.StartsWith(TEXT("GIT binary patch"));
			continue;
		}
		if (Text.IsEmpty())
		{
			continue;
		}
		if (Text.StartsWith(TEXT("\\")))
		{
			Lines.Add(MakeLine(FGitDiffLine::EKind::Note, Text.Mid(2)));
			continue;
		}
		const int32 MarkerWidth = bCombined ? 2 : 1;
		const FString Marker = Text.Left(MarkerWidth);
		const FString Content = ExpandTabs(Text.Mid(MarkerWidth));
		int32 Ignored = INDEX_NONE;
		if (Marker.FindChar(TEXT('-'), Ignored))
		{
			Lines.Add(MakeLine(FGitDiffLine::EKind::Removed, Content, OldLine++, 0));
		}
		else if (Marker.FindChar(TEXT('+'), Ignored))
		{
			Lines.Add(MakeLine(FGitDiffLine::EKind::Added, Content, 0, NewLine++));
		}
		else
		{
			Lines.Add(MakeLine(FGitDiffLine::EKind::Context, Content, OldLine++, NewLine++));
		}
	}

	const bool bChanged = Lines.ContainsByPredicate([](const TSharedPtr<FGitDiffLine>& Line)
	{
		return Line->Kind == FGitDiffLine::EKind::Added || Line->Kind == FGitDiffLine::EKind::Removed;
	});
	const bool bPointer = bChanged && !Lines.ContainsByPredicate([](const TSharedPtr<FGitDiffLine>& Line)
	{
		return (Line->Kind == FGitDiffLine::EKind::Added || Line->Kind == FGitDiffLine::EKind::Removed) && !IsLfsPointerLine(Line->Text);
	});
	if (bPointer)
	{
		return {MakeLine(FGitDiffLine::EKind::Note, LOCTEXT("LfsFile", "Stored in Git LFS: there is no text to compare.").ToString())};
	}
	if (bBinary && !bChanged)
	{
		return {MakeLine(FGitDiffLine::EKind::Note, LOCTEXT("BinaryFile", "Binary file: there is no text to compare.").ToString())};
	}

	// Pair each run of removed lines with the added lines right after it.
	for (int32 Index = 0; Index < Lines.Num();)
	{
		if (Lines[Index]->Kind != FGitDiffLine::EKind::Removed)
		{
			++Index;
			continue;
		}
		const int32 RemovedStart = Index;
		while (Index < Lines.Num() && Lines[Index]->Kind == FGitDiffLine::EKind::Removed)
		{
			++Index;
		}
		const int32 AddedStart = Index;
		while (Index < Lines.Num() && Lines[Index]->Kind == FGitDiffLine::EKind::Added)
		{
			++Index;
		}
		const int32 Pairs = FMath::Min(AddedStart - RemovedStart, Index - AddedStart);
		for (int32 Pair = 0; Pair < Pairs; ++Pair)
		{
			MarkChange(*Lines[RemovedStart + Pair], *Lines[AddedStart + Pair]);
		}
	}
	return Lines;
}

FString GitDiff::FromNewFile(const TArray<uint8>& Bytes)
{
	// Git's own test: a NUL byte near the start means binary.
	const int32 Sniff = FMath::Min(Bytes.Num(), 8000);
	for (int32 Index = 0; Index < Sniff; ++Index)
	{
		if (Bytes[Index] == 0)
		{
			return TEXT("Binary files /dev/null and b/file differ\n");
		}
	}
	FString Content;
	FFileHelper::BufferToString(Content, Bytes.GetData(), Bytes.Num());
	TArray<FString> Lines;
	Content.ParseIntoArray(Lines, TEXT("\n"), false);
	if (!Lines.IsEmpty() && Lines.Last().IsEmpty())
	{
		Lines.Pop();
	}
	FString Diff = FString::Printf(TEXT("@@ -0,0 +1,%d @@\n"), Lines.Num());
	for (const FString& Line : Lines)
	{
		Diff += TEXT("+") + Line + TEXT("\n");
	}
	return Diff;
}

FString GitDiff::ToMarkup(const FGitDiffLine& Line)
{
	if (!Line.HasChange() || (Line.Kind != FGitDiffLine::EKind::Added && Line.Kind != FGitDiffLine::EKind::Removed))
	{
		return EscapeMarkup(Line.Text);
	}
	const TCHAR* Tag = Line.Kind == FGitDiffLine::EKind::Added ? TEXT("add") : TEXT("del");
	return EscapeMarkup(Line.Text.Left(Line.ChangeStart))
		+ FString::Printf(TEXT("<%s>"), Tag) + EscapeMarkup(Line.Text.Mid(Line.ChangeStart, Line.ChangeEnd - Line.ChangeStart)) + TEXT("</>")
		+ EscapeMarkup(Line.Text.Mid(Line.ChangeEnd));
}

bool GitDiff::WrapLines()
{
	return GetDefault<UGitSourceControlUserSettings>()->bWrapDiffLines;
}

void GitDiff::SetWrapLines(bool bWrap)
{
	UGitSourceControlUserSettings* Settings = GetMutableDefault<UGitSourceControlUserSettings>();
	if (Settings->bWrapDiffLines != bWrap)
	{
		Settings->bWrapDiffLines = bWrap;
		Settings->SaveConfig();
	}
}

void SGitDiffView::Construct(const FArguments& InArgs)
{
	Font = FCoreStyle::GetDefaultFontStyle("Mono", 9);
	TextStyle = FTextBlockStyle(FAppStyle::GetWidgetStyle<FTextBlockStyle>("NormalText")).SetFont(Font).SetColorAndOpacity(FSlateColor::UseForeground());
	NoteStyle = FTextBlockStyle(TextStyle).SetColorAndOpacity(FSlateColor::UseSubduedForeground());
	AddedDecorator = MakeShared<FTintDecorator>(TEXT("add"), TextStyle, AddedPart);
	RemovedDecorator = MakeShared<FTintDecorator>(TEXT("del"), TextStyle, RemovedPart);
	bBuiltWrapped = GitDiff::WrapLines();
	if (FSlateApplication::IsInitialized() && FSlateApplication::Get().GetRenderer())
	{
		CharWidth = FSlateApplication::Get().GetRenderer()->GetFontMeasureService()->Measure(TEXT("0000000000"), Font).X / 10.0f;
	}
	const TSharedRef<SScrollBar> Vertical = SNew(SScrollBar).Orientation(Orient_Vertical);
	const TSharedRef<SScrollBar> Horizontal = SNew(SScrollBar).Orientation(Orient_Horizontal);

	ChildSlot
	[
		SNew(SOverlay)
		+ SOverlay::Slot()
		[
			SNew(SVerticalBox)
			.Visibility_Lambda([this]() { return Message.IsEmpty() ? EVisibility::Visible : EVisibility::Collapsed; })
			// How much changed, and the wrap switch.
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(4.0f, 2.0f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Font(Font)
					.ColorAndOpacity(AddedMarker)
					.Text_Lambda([this]() { return FText::Format(INVTEXT("+{0}"), AddedCount); })
					.ToolTipText(LOCTEXT("AddedCountTooltip", "Lines added"))
				]
				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				.VAlign(VAlign_Center)
				.Padding(8.0f, 0.0f, 0.0f, 0.0f)
				[
					SNew(STextBlock)
					.Font(Font)
					.ColorAndOpacity(RemovedMarker)
					.Text_Lambda([this]() { return FText::Format(INVTEXT("\u2212{0}"), RemovedCount); })
					.ToolTipText(LOCTEXT("RemovedCountTooltip", "Lines removed"))
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				[
					SNew(SCheckBox)
					.ToolTipText(LOCTEXT("WrapTooltip", "Wrap long lines instead of scrolling sideways. Applies to every diff."))
					.IsChecked_Lambda([]() { return GitDiff::WrapLines() ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
					.OnCheckStateChanged_Lambda([](ECheckBoxState State) { GitDiff::SetWrapLines(State == ECheckBoxState::Checked); })
					[
						SNew(STextBlock)
						.Text(LOCTEXT("WrapLines", "Wrap lines"))
					]
				]
			]
			+ SVerticalBox::Slot()
			.FillHeight(1.0f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				[
					// Long lines scroll sideways unless they wrap; the gutter scrolls with them.
					SAssignNew(Scroll, SScrollBox)
					.Orientation(Orient_Horizontal)
					.ExternalScrollbar(Horizontal)
					+ SScrollBox::Slot()
					[
						SNew(SBox)
						.WidthOverride(this, &SGitDiffView::GetContentWidth)
						[
							SAssignNew(List, SListView<FLinePtr>)
							.ListItemsSource(&Lines)
							.SelectionMode(ESelectionMode::Multi)
							.OnGenerateRow(this, &SGitDiffView::OnGenerateRow)
							.OnContextMenuOpening(this, &SGitDiffView::OnContextMenu)
							.ExternalScrollbar(Vertical)
							.ConsumeMouseWheel(EConsumeMouseWheel::Always)
						]
					]
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				[
					Vertical
				]
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(SBox)
				.Visibility_Lambda([]() { return GitDiff::WrapLines() ? EVisibility::Collapsed : EVisibility::Visible; })
				[
					Horizontal
				]
			]
		]
		+ SOverlay::Slot()
		.HAlign(HAlign_Center)
		.VAlign(VAlign_Center)
		.Padding(8.0f)
		[
			SNew(STextBlock)
			.Visibility_Lambda([this]() { return Message.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible; })
			.Text_Lambda([this]() { return Message; })
			.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			.AutoWrapText(true)
			.Justification(ETextJustify::Center)
		]
	];
}

void SGitDiffView::SetDiff(const FString& UnifiedDiff)
{
	const bool bCut = UnifiedDiff.Len() > MaxDiffCharacters;
	Lines = GitDiff::Parse(bCut ? UnifiedDiff.Left(MaxDiffCharacters) : UnifiedDiff);
	if (bCut)
	{
		Lines.Add(MakeLine(FGitDiffLine::EKind::Note, LOCTEXT("DiffCut", "The rest of this comparison is too long to show.").ToString()));
	}
	int32 HighestLine = 0;
	LongestLine = 0;
	AddedCount = 0;
	RemovedCount = 0;
	for (const FLinePtr& Line : Lines)
	{
		HighestLine = FMath::Max3(HighestLine, Line->OldLine, Line->NewLine);
		LongestLine = FMath::Max(LongestLine, Line->Text.Len());
		AddedCount += Line->Kind == FGitDiffLine::EKind::Added ? 1 : 0;
		RemovedCount += Line->Kind == FGitDiffLine::EKind::Removed ? 1 : 0;
	}
	GutterWidth = (FString::FromInt(FMath::Max(HighestLine, 99)).Len() + 1) * CharWidth + 6.0f;
	Message = Lines.IsEmpty() ? LOCTEXT("NoDifferences", "No differences.") : FText::GetEmpty();
	List->ClearSelection();
	List->RequestListRefresh();
	List->ScrollToTop();
}

void SGitDiffView::SetMessage(const FText& InMessage)
{
	Message = InMessage;
	Lines.Reset();
	List->RequestListRefresh();
}

float SGitDiffView::GetViewportWidth() const
{
	return Scroll.IsValid() ? static_cast<float>(Scroll->GetCachedGeometry().GetLocalSize().X) : 0.0f;
}

FOptionalSize SGitDiffView::GetContentWidth() const
{
	const float Viewport = GetViewportWidth();
	if (bBuiltWrapped)
	{
		// Before the first layout the width is unknown; the rows wrap once it is.
		return Viewport > 0.0f ? FOptionalSize(Viewport) : FOptionalSize();
	}
	return FMath::Max(GutterWidth * 2.0f + CharWidth * (LongestLine + 4), Viewport);
}

void SGitDiffView::Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime)
{
	SCompoundWidget::Tick(AllottedGeometry, InCurrentTime, InDeltaTime);
	// The wrap preference is shared by every diff view.
	if (GitDiff::WrapLines() != bBuiltWrapped)
	{
		bBuiltWrapped = GitDiff::WrapLines();
		List->RebuildList();
		Scroll->ScrollToStart();
	}
}

TSharedRef<ITableRow> SGitDiffView::OnGenerateRow(FLinePtr Line, const TSharedRef<STableViewBase>& Owner)
{
	FLinearColor Background = FLinearColor::Transparent;
	FText Marker;
	FSlateColor MarkerColor = FSlateColor::UseSubduedForeground();
	bool bNote = false;
	switch (Line->Kind)
	{
	case FGitDiffLine::EKind::Added:
		Background = AddedLine;
		Marker = INVTEXT("+");
		MarkerColor = AddedMarker;
		break;
	case FGitDiffLine::EKind::Removed:
		Background = RemovedLine;
		Marker = INVTEXT("-");
		MarkerColor = RemovedMarker;
		break;
	case FGitDiffLine::EKind::Hunk:
		Background = HunkLine;
		bNote = true;
		break;
	case FGitDiffLine::EKind::Note:
		bNote = true;
		break;
	default:
		break;
	}
	auto Number = [this](int32 Value)
	{
		return SNew(SBox)
			.WidthOverride(GutterWidth)
			.HAlign(HAlign_Right)
			.Padding(FMargin(0.0f, 0.0f, 6.0f, 0.0f))
			[
				SNew(STextBlock)
				.Font(Font)
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				.Text(Value > 0 ? FText::AsNumber(Value, &FNumberFormattingOptions::DefaultNoGrouping()) : FText::GetEmpty())
			];
	};
	// Rich text, so the changed part keeps its tint when the line wraps.
	const FString Markup = GitDiff::ToMarkup(*Line);
	const TSharedRef<SWidget> Content = SNew(SRichTextBlock)
		.Text(FText::FromString(Markup.IsEmpty() ? FString(TEXT(" ")) : Markup))
		.TextStyle(bNote ? &NoteStyle : &TextStyle)
		.AutoWrapText(bBuiltWrapped)
		.WrappingPolicy(ETextWrappingPolicy::AllowPerCharacterWrapping)
		+ SRichTextBlock::Decorator(AddedDecorator.ToSharedRef())
		+ SRichTextBlock::Decorator(RemovedDecorator.ToSharedRef());
	return SNew(STableRow<FLinePtr>, Owner)
		.Padding(0.0f)
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
			.BorderBackgroundColor(Background)
			.Padding(FMargin(0.0f, 1.0f))
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.AutoWidth()
				[
					Number(Line->OldLine)
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				[
					Number(Line->NewLine)
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				[
					SNew(SBox)
					.WidthOverride(CharWidth * 2.0f)
					[
						SNew(STextBlock)
						.Font(Font)
						.ColorAndOpacity(MarkerColor)
						.Text(Marker)
					]
				]
				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				[
					Content
				]
			]
		];
}

TSharedPtr<SWidget> SGitDiffView::OnContextMenu()
{
	if (Lines.IsEmpty())
	{
		return nullptr;
	}
	FMenuBuilder Menu(true, nullptr);
	Menu.AddMenuEntry(LOCTEXT("CopyLines", "Copy"), LOCTEXT("CopyLinesTooltip", "Copy the selected lines without their + and - markers. (Ctrl+C)"),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "GenericCommands.Copy"),
		FUIAction(FExecuteAction::CreateSP(this, &SGitDiffView::CopySelection), FCanExecuteAction::CreateLambda([this]() { return List->GetNumItemsSelected() > 0; })));
	Menu.AddMenuEntry(LOCTEXT("SelectAllLines", "Select all"), FText::GetEmpty(), FSlateIcon(FAppStyle::GetAppStyleSetName(), "GenericCommands.SelectAll"),
		FUIAction(FExecuteAction::CreateLambda([this]() { List->SetItemSelection(Lines, true); })));
	Menu.AddMenuSeparator();
	Menu.AddMenuEntry(LOCTEXT("WrapLinesMenu", "Wrap lines"), LOCTEXT("WrapLinesMenuTooltip", "Wrap long lines instead of scrolling sideways."), FSlateIcon(),
		FUIAction(FExecuteAction::CreateLambda([]() { GitDiff::SetWrapLines(!GitDiff::WrapLines()); }), FCanExecuteAction(),
			FIsActionChecked::CreateLambda([]() { return GitDiff::WrapLines(); })),
		NAME_None, EUserInterfaceActionType::ToggleButton);
	return Menu.MakeWidget();
}

void SGitDiffView::CopySelection() const
{
	TArray<FString> Copied;
	for (const FLinePtr& Line : Lines)
	{
		if (List->IsItemSelected(Line) && Line->Kind != FGitDiffLine::EKind::Hunk && Line->Kind != FGitDiffLine::EKind::Note)
		{
			Copied.Add(Line->Text);
		}
	}
	if (!Copied.IsEmpty())
	{
		FPlatformApplicationMisc::ClipboardCopy(*FString::Join(Copied, LINE_TERMINATOR));
	}
}

FReply SGitDiffView::OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent)
{
	if (InKeyEvent.IsControlDown() || InKeyEvent.IsCommandDown())
	{
		if (InKeyEvent.GetKey() == EKeys::C)
		{
			CopySelection();
			return FReply::Handled();
		}
		if (InKeyEvent.GetKey() == EKeys::A)
		{
			List->SetItemSelection(Lines, true);
			return FReply::Handled();
		}
	}
	return SCompoundWidget::OnKeyDown(MyGeometry, InKeyEvent);
}

#undef LOCTEXT_NAMESPACE
