// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#pragma once

#include "CoreMinimal.h"
#include "Core/GitTypes.h"
#include "Widgets/SLeafWidget.h"

/** The History tab's graph: a lane (a column) per line of history, and a dot per commit. */
namespace GitHistoryGraph
{
	/** A line across one row, from lane From to lane To. */
	struct FLine
	{
		int32 From = 0;
		int32 To = 0;
		/** 0 for your branch's line, 1 for its upstream's, then one per line in the order they start. */
		int32 Color = 0;
	};

	/** One commit's row. */
	struct FRow
	{
		/** The lane of the commit's dot. */
		int32 Lane = 0;
		int32 Color = 0;
		/** Lines that pass the dot, top to bottom. */
		TArray<FLine> Through;
		/** Lines from the top of the row into the dot: from the commits above whose parent this is. */
		TArray<FLine> In;
		/** Lines from the dot to the bottom of the row: to the commit's parents below. */
		TArray<FLine> Out;
	};

	/**
	 * Lays out commits listed newest first, each above its parents (as git log --date-order lists them). A commit's line
	 * goes on down to its first parent; a merge's other parents join their own lines, or start one. Where lines meet,
	 * the leftmost goes on, in the color of your branch's line if it is one of them. HeadTip starts your branch's line
	 * and UpstreamTip its upstream's, when no line above leads to them.
	 */
	TArray<FRow> Layout(TConstArrayView<const GitCore::FHistoryCommit*> Commits, const FString& HeadTip, const FString& UpstreamTip);

	/** How many lanes the rows use. */
	int32 CountLanes(TConstArrayView<FRow> Rows);

	/** A line's color: blue for your branch and purple for its upstream, as their labels; the others take turns. */
	FSlateColor LineColor(int32 Color);
}

/** A row's part of the graph: the lines across it and the commit's dot, or a ring for changes not committed yet. */
class SGitGraphCell : public SLeafWidget
{
public:
	SLATE_BEGIN_ARGS(SGitGraphCell) {}
		/** The row to draw; nothing is drawn without one. */
		SLATE_ATTRIBUTE(const GitHistoryGraph::FRow*, Row)
		/** The lanes every row makes room for, so the rows line up. */
		SLATE_ATTRIBUTE(int32, Lanes)
		/** Set for the uncommitted changes: their dot is a ring of this color. */
		SLATE_ATTRIBUTE(FSlateColor, RingColor)
	SLATE_END_ARGS()

	static constexpr float LaneWidth = 14.0f;

	void Construct(const FArguments& InArgs);
	virtual int32 OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect, FSlateWindowElementList& OutDrawElements,
		int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const override;

protected:
	virtual FVector2D ComputeDesiredSize(float LayoutScaleMultiplier) const override;

private:
	TAttribute<const GitHistoryGraph::FRow*> Row;
	TAttribute<int32> Lanes;
	TAttribute<FSlateColor> RingColor;
};
