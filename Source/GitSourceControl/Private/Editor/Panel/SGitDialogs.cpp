// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#include "SGitDialogs.h"

#include "Framework/Application/SlateApplication.h"
#include "Misc/App.h"
#include "Misc/Paths.h"
#include "Styling/AppStyle.h"
#include "Styling/StyleColors.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "GitWorkspaceDialogs"

namespace
{
	TSharedRef<SWidget> Buttons(const FText& PrimaryLabel, TFunction<void()> OnPrimary, TFunction<void()> OnCancel, TAttribute<bool> IsPrimaryEnabled = true)
	{
		return SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.Padding(4.0f, 0.0f)
			[
				SNew(SButton)
				.Text(LOCTEXT("Cancel", "Cancel"))
				.OnClicked_Lambda([OnCancel]() { OnCancel(); return FReply::Handled(); })
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			[
				SNew(SButton)
				.ButtonStyle(FAppStyle::Get(), "PrimaryButton")
				.Text(PrimaryLabel)
				.IsEnabled(IsPrimaryEnabled)
				.OnClicked_Lambda([OnPrimary]() { OnPrimary(); return FReply::Handled(); })
			];
	}

	TSharedRef<SWindow> MakeWindow(const FText& Title)
	{
		return SNew(SWindow)
			.Title(Title)
			.SizingRule(ESizingRule::Autosized)
			.SupportsMinimize(false)
			.SupportsMaximize(false);
	}
}

namespace GitWorkspaceDialogs
{
	bool Confirm(const FText& Title, const FText& Message, const TArray<FString>& Paths, const FText& PrimaryLabel)
	{
		if (FApp::IsUnattended())
		{
			return false;
		}
		constexpr int32 MaxListed = 12;
		TArray<FString> Lines;
		for (int32 Index = 0; Index < Paths.Num() && Index < MaxListed; ++Index)
		{
			Lines.Add(Paths[Index]);
		}
		if (Paths.Num() > MaxListed)
		{
			Lines.Add(FText::Format(LOCTEXT("MoreFiles", "… and {0} more"), Paths.Num() - MaxListed).ToString());
		}

		bool bAccepted = false;
		const TSharedRef<SWindow> Window = MakeWindow(Title);
		TWeakPtr<SWindow> WeakWindow = Window;
		Window->SetContent(
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
			.Padding(16.0f)
			[
				SNew(SBox)
				.MaxDesiredWidth(560.0f)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot()
					.AutoHeight()
					[
						SNew(STextBlock)
						.Text(Message)
						.AutoWrapText(true)
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.0f, 8.0f, 0.0f, 0.0f)
					[
						SNew(SBox)
						.MaxDesiredHeight(220.0f)
						.Visibility(Lines.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible)
						[
							SNew(SScrollBox)
							+ SScrollBox::Slot()
							[
								SNew(STextBlock)
								.Text(FText::FromString(FString::Join(Lines, TEXT("\n"))))
								.ColorAndOpacity(FSlateColor::UseSubduedForeground())
							]
						]
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.0f, 16.0f, 0.0f, 0.0f)
					[
						Buttons(PrimaryLabel,
							[&bAccepted, WeakWindow]() { bAccepted = true; if (const TSharedPtr<SWindow> Pinned = WeakWindow.Pin()) { Pinned->RequestDestroyWindow(); } },
							[WeakWindow]() { if (const TSharedPtr<SWindow> Pinned = WeakWindow.Pin()) { Pinned->RequestDestroyWindow(); } })
					]
				]
			]);
		FSlateApplication::Get().AddModalWindow(Window, FSlateApplication::Get().GetActiveTopLevelWindow());
		return bAccepted;
	}

	bool AskForText(const FText& Title, const FText& Prompt, const FText& Hint, FString& InOutValue, TFunction<FText(const FString&)> Validate)
	{
		if (FApp::IsUnattended())
		{
			return false;
		}
		bool bAccepted = false;
		FString Value = InOutValue;
		const TSharedRef<SWindow> Window = MakeWindow(Title);
		TWeakPtr<SWindow> WeakWindow = Window;
		auto Error = [&Value, Validate]() { return Validate ? Validate(Value.TrimStartAndEnd()) : FText::GetEmpty(); };
		auto Accept = [&bAccepted, WeakWindow, Error]()
		{
			if (Error().IsEmpty())
			{
				bAccepted = true;
				if (const TSharedPtr<SWindow> Pinned = WeakWindow.Pin())
				{
					Pinned->RequestDestroyWindow();
				}
			}
		};
		TSharedPtr<SEditableTextBox> Input;
		Window->SetContent(
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
			.Padding(16.0f)
			[
				SNew(SBox)
				.WidthOverride(420.0f)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot()
					.AutoHeight()
					[
						SNew(STextBlock)
						.Text(Prompt)
						.AutoWrapText(true)
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.0f, 8.0f, 0.0f, 0.0f)
					[
						SAssignNew(Input, SEditableTextBox)
						.Text(FText::FromString(Value))
						.HintText(Hint)
						.SelectAllTextWhenFocused(true)
						.OnTextChanged_Lambda([&Value](const FText& Text) { Value = Text.ToString(); })
						.OnTextCommitted_Lambda([&Value, Accept](const FText& Text, ETextCommit::Type Commit)
						{
							Value = Text.ToString();
							if (Commit == ETextCommit::OnEnter)
							{
								Accept();
							}
						})
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.0f, 4.0f, 0.0f, 0.0f)
					[
						SNew(STextBlock)
						.Text_Lambda(Error)
						.ColorAndOpacity(FStyleColors::Error)
						.AutoWrapText(true)
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.0f, 12.0f, 0.0f, 0.0f)
					[
						Buttons(LOCTEXT("OK", "OK"), Accept,
							[WeakWindow]() { if (const TSharedPtr<SWindow> Pinned = WeakWindow.Pin()) { Pinned->RequestDestroyWindow(); } },
							MakeAttributeLambda([Error]() { return Error().IsEmpty(); }))
					]
				]
			]);
		Window->SetWidgetToFocusOnActivate(Input);
		FSlateApplication::Get().AddModalWindow(Window, FSlateApplication::Get().GetActiveTopLevelWindow());
		if (bAccepted)
		{
			InOutValue = Value.TrimStartAndEnd();
		}
		return bAccepted;
	}
}

#undef LOCTEXT_NAMESPACE
