// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#include "GitHistoryGraph.h"

#include "Brushes/SlateRoundedBoxBrush.h"
#include "Rendering/DrawElements.h"
#include "Styling/StyleColors.h"

namespace GitHistoryGraph
{
	TArray<FRow> Layout(TConstArrayView<const GitCore::FHistoryCommit*> Commits, const FString& HeadTip, const FString& UpstreamTip)
	{
		struct FLane
		{
			/** The commit the lane's line leads to; empty while the lane is free. */
			FString Next;
			int32 Color = 0;
		};
		TArray<FLane> Lanes;
		int32 NextColor = 2;
		auto FreeLane = [&Lanes](int32 Taken)
		{
			for (int32 Index = 0; Index < Lanes.Num(); ++Index)
			{
				if (Index != Taken && Lanes[Index].Next.IsEmpty())
				{
					return Index;
				}
			}
			return Lanes.AddDefaulted();
		};

		TArray<FRow> Rows;
		Rows.Reserve(Commits.Num());
		for (const GitCore::FHistoryCommit* Commit : Commits)
		{
			FRow& Row = Rows.AddDefaulted_GetRef();
			// The lines from the commits above whose parent this is; the others pass by.
			TArray<int32> Arriving;
			for (int32 Index = 0; Index < Lanes.Num(); ++Index)
			{
				if (Lanes[Index].Next.IsEmpty())
				{
					continue;
				}
				if (Lanes[Index].Next.Equals(Commit->Oid, ESearchCase::CaseSensitive))
				{
					Arriving.Add(Index);
				}
				else
				{
					Row.Through.Add({Index, Index, Lanes[Index].Color});
				}
			}
			if (Arriving.IsEmpty())
			{
				// The newest commit of a line of history starts a lane.
				Row.Lane = FreeLane(INDEX_NONE);
				const bool bHead = Commit->Oid.Equals(HeadTip, ESearchCase::CaseSensitive);
				const bool bUpstream = !bHead && Commit->Oid.Equals(UpstreamTip, ESearchCase::CaseSensitive);
				Row.Color = bHead ? 0 : bUpstream ? 1 : NextColor++;
			}
			else
			{
				// Lines meet here: the leftmost goes on, in the lowest color (your branch's first).
				Row.Lane = Arriving[0];
				Row.Color = Lanes[Row.Lane].Color;
				for (const int32 Index : Arriving)
				{
					Row.In.Add({Index, Row.Lane, Lanes[Index].Color});
					Row.Color = FMath::Min(Row.Color, Lanes[Index].Color);
					if (Index != Row.Lane)
					{
						Lanes[Index] = FLane();
					}
				}
			}
			Lanes[Row.Lane].Color = Row.Color;
			if (Commit->Parents.IsEmpty())
			{
				Lanes[Row.Lane] = FLane();
			}
			else
			{
				// The first parent goes on in this lane; a merge's other parents join their lines, or start one.
				Lanes[Row.Lane].Next = Commit->Parents[0];
				Row.Out.Add({Row.Lane, Row.Lane, Row.Color});
				for (int32 Parent = 1; Parent < Commit->Parents.Num(); ++Parent)
				{
					const FString& Oid = Commit->Parents[Parent];
					int32 Lane = Lanes.IndexOfByPredicate([&Oid](const FLane& Each) { return Each.Next.Equals(Oid, ESearchCase::CaseSensitive); });
					if (Lane == INDEX_NONE)
					{
						Lane = FreeLane(Row.Lane);
						Lanes[Lane].Next = Oid;
						Lanes[Lane].Color = NextColor++;
					}
					Row.Out.Add({Row.Lane, Lane, Lanes[Lane].Color});
				}
			}
			while (!Lanes.IsEmpty() && Lanes.Last().Next.IsEmpty())
			{
				Lanes.Pop();
			}
		}
		return Rows;
	}

	int32 CountLanes(TConstArrayView<FRow> Rows)
	{
		int32 Count = 0;
		for (const FRow& Row : Rows)
		{
			Count = FMath::Max(Count, Row.Lane + 1);
			for (const TArray<FLine>* Lines : {&Row.Through, &Row.In, &Row.Out})
			{
				for (const FLine& Line : *Lines)
				{
					Count = FMath::Max(Count, FMath::Max(Line.From, Line.To) + 1);
				}
			}
		}
		return Count;
	}

	FSlateColor LineColor(int32 Color)
	{
		static const FSlateColor Others[] = {FStyleColors::AccentGreen, FStyleColors::AccentPink, FStyleColors::AccentYellow, FStyleColors::AccentOrange};
		if (Color <= 0)
		{
			return FStyleColors::AccentBlue;
		}
		return Color == 1 ? FStyleColors::AccentPurple : Others[(Color - 2) % UE_ARRAY_COUNT(Others)];
	}
}

namespace
{
	constexpr float GraphPadding = 3.0f;
	constexpr float DotRadius = 4.0f;
	constexpr float LineThickness = 2.0f;

	float LaneX(int32 Lane)
	{
		return GraphPadding + SGitGraphCell::LaneWidth * (static_cast<float>(Lane) + 0.5f);
	}
}

void SGitGraphCell::Construct(const FArguments& InArgs)
{
	Row = InArgs._Row;
	Lanes = InArgs._Lanes;
	RingColor = InArgs._RingColor;
	// Lanes beyond the room made for them are cut off.
	SetClipping(EWidgetClipping::ClipToBounds);
}

FVector2D SGitGraphCell::ComputeDesiredSize(float) const
{
	const int32 Count = Lanes.Get();
	return FVector2D(Count > 0 ? 2.0f * GraphPadding + LaneWidth * static_cast<float>(Count) : 0.0f, 0.0f);
}

int32 SGitGraphCell::OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect, FSlateWindowElementList& OutDrawElements,
	int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const
{
	const GitHistoryGraph::FRow* Shown = Row.Get();
	if (Shown == nullptr)
	{
		return LayerId;
	}
	// Each row draws from its top edge to its bottom edge, so the lines of neighbouring rows join.
	const FVector2f Size = AllottedGeometry.GetLocalSize();
	const float Middle = FMath::RoundToFloat(Size.Y * 0.5f);
	const ESlateDrawEffect Effects = ShouldBeEnabled(bParentEnabled) ? ESlateDrawEffect::None : ESlateDrawEffect::DisabledEffect;
	const FLinearColor Tint = InWidgetStyle.GetColorAndOpacityTint();
	const FPaintGeometry Geometry = AllottedGeometry.ToPaintGeometry();
	auto Draw = [&](const GitHistoryGraph::FLine& Line, float FromY, float ToY)
	{
		const FVector2f Start(LaneX(Line.From), FromY);
		const FVector2f End(LaneX(Line.To), ToY);
		const FLinearColor Color = GitHistoryGraph::LineColor(Line.Color).GetColor(InWidgetStyle) * Tint;
		if (Line.From == Line.To)
		{
			FSlateDrawElement::MakeLines(OutDrawElements, LayerId, Geometry, TArray<FVector2f>{Start, End}, Effects, Color, true, LineThickness);
			return;
		}
		// A curve that leaves and arrives upright, as in other Git clients.
		const FVector2f Tangent(0.0f, (ToY - FromY) * 1.5f);
		FSlateDrawElement::MakeSpline(OutDrawElements, LayerId, Geometry, Start, Tangent, End, Tangent, LineThickness, Effects, Color);
	};
	for (const GitHistoryGraph::FLine& Line : Shown->Through)
	{
		Draw(Line, 0.0f, Size.Y);
	}
	for (const GitHistoryGraph::FLine& Line : Shown->In)
	{
		Draw(Line, 0.0f, Middle);
	}
	for (const GitHistoryGraph::FLine& Line : Shown->Out)
	{
		Draw(Line, Middle, Size.Y);
	}

	const FVector2f Center(LaneX(Shown->Lane), Middle);
	if (RingColor.IsSet())
	{
		// Not a commit yet: a ring.
		TArray<FVector2f> Ring;
		constexpr int32 Steps = 24;
		for (int32 Step = 0; Step <= Steps; ++Step)
		{
			const float Angle = 2.0f * UE_PI * static_cast<float>(Step) / static_cast<float>(Steps);
			Ring.Add(Center + FVector2f(FMath::Cos(Angle), FMath::Sin(Angle)) * DotRadius);
		}
		FSlateDrawElement::MakeLines(OutDrawElements, LayerId + 1, Geometry, MoveTemp(Ring), Effects, RingColor.Get().GetColor(InWidgetStyle) * Tint, true, LineThickness);
		return LayerId + 1;
	}
	static const FSlateRoundedBoxBrush Dot(FLinearColor::White, DotRadius);
	const FLinearColor Color = GitHistoryGraph::LineColor(Shown->Color).GetColor(InWidgetStyle) * Tint;
	FSlateDrawElement::MakeBox(OutDrawElements, LayerId + 1, AllottedGeometry.ToPaintGeometry(FVector2f(2.0f * DotRadius, 2.0f * DotRadius),
		FSlateLayoutTransform(Center - FVector2f(DotRadius, DotRadius))), &Dot, Effects, Color);
	return LayerId + 1;
}
