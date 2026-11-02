// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#include "SGitDialogs.h"

#include "Framework/Application/SlateApplication.h"
#include "Misc/App.h"
#include "Misc/Paths.h"
#include "RevisionControlStyle/RevisionControlStyle.h"
#include "Styling/AppStyle.h"
#include "Styling/StyleColors.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SGridPanel.h"
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

	GitWorkspaceDialogs::FScopedConfirmAnswer* GAnswer = nullptr;

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
		if (GAnswer)
		{
			GAnswer->Questions.Add(Message);
			return GAnswer->bAnswer;
		}
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

	bool ConfirmPush(const GitCore::FPushTarget& Target, bool bCommitFirst)
	{
		const FText Branch = FText::FromString(Target.Branch);
		const FText Destination = FText::FromString(Target.Remote + TEXT("/") + Target.Branch);
		FText Message;
		if (Target.PreviousUpstream.IsEmpty())
		{
			Message = FText::Format(LOCTEXT("PublishNew", "The branch {0} is not on {1} yet. Pushing publishes it there, so your team can get it."),
				Branch, FText::FromString(Target.Remote));
		}
		else
		{
			Message = FText::Format(Target.bExists
				? LOCTEXT("PushRenamed", "The branch {0} follows {1}, which has another name, so its commits will not go there. Pushing sends them to {2}, which {0} follows from then on.")
				: LOCTEXT("PublishRenamed", "The branch {0} follows {1}, which has another name, so its commits will not go there. Pushing publishes them as a new branch, {2}, which {0} follows from then on."),
				Branch, FText::FromString(Target.PreviousUpstream), Destination);
		}
		const FText Label = bCommitFirst
			? (Target.bExists ? LOCTEXT("CommitAndPushButton", "Commit and push") : LOCTEXT("CommitAndPublishButton", "Commit and publish"))
			: (Target.bExists ? LOCTEXT("PushButton", "Push") : LOCTEXT("PublishButton", "Publish"));
		return Confirm(Target.bExists ? LOCTEXT("PushTitle", "Push branch") : LOCTEXT("PublishTitle", "Publish branch"), Message, {}, Label);
	}

	FScopedConfirmAnswer::FScopedConfirmAnswer(bool bInAnswer)
		: bAnswer(bInAnswer)
	{
		check(!GAnswer);
		GAnswer = this;
	}

	FScopedConfirmAnswer::~FScopedConfirmAnswer()
	{
		GAnswer = nullptr;
	}

	bool AskPull(const FString& Into, const TMap<FString, TArray<FString>>& RemoteBranches, GitCore::FPullOptions& InOutOptions)
	{
		if (FApp::IsUnattended())
		{
			return false;
		}
		GitCore::FPullOptions Options = InOutOptions;
		TArray<TSharedPtr<FString>> Remotes;
		for (const TPair<FString, TArray<FString>>& Pair : RemoteBranches)
		{
			Remotes.Add(MakeShared<FString>(Pair.Key));
		}
		TArray<TSharedPtr<FString>> Branches;
		TSharedPtr<SComboBox<TSharedPtr<FString>>> BranchCombo;
		// Picking a remote lists its branches and keeps the branch's name when that remote has it too.
		auto ShowBranchesOf = [&RemoteBranches, &Branches, &BranchCombo, &Options, &Into](const FString& Remote)
		{
			Branches.Reset();
			TSharedPtr<FString> Picked;
			if (const TArray<FString>* Names = RemoteBranches.Find(Remote))
			{
				for (const FString& Name : *Names)
				{
					Branches.Add(MakeShared<FString>(Name));
				}
				for (const FString& Wanted : {Options.Branch, Into, FString(TEXT("main")), FString(TEXT("master"))})
				{
					if (const TSharedPtr<FString>* Found = Branches.FindByPredicate([&Wanted](const TSharedPtr<FString>& Name) { return *Name == Wanted; }))
					{
						Picked = *Found;
						break;
					}
				}
			}
			Picked = Picked.IsValid() || Branches.IsEmpty() ? Picked : Branches[0];
			Options.Remote = Remote;
			Options.Branch = Picked.IsValid() ? *Picked : FString();
			if (BranchCombo.IsValid())
			{
				BranchCombo->RefreshOptions();
				BranchCombo->SetSelectedItem(Picked);
			}
		};
		ShowBranchesOf(Options.Remote);
		auto Find = [](const TArray<TSharedPtr<FString>>& Items, const FString& Value)
		{
			const TSharedPtr<FString>* Found = Items.FindByPredicate([&Value](const TSharedPtr<FString>& Item) { return *Item == Value; });
			return Found ? *Found : TSharedPtr<FString>();
		};
		auto Label = [](const FText& Text)
		{
			return SNew(STextBlock).Text(Text).ColorAndOpacity(FSlateColor::UseSubduedForeground());
		};
		auto BranchName = [](const TAttribute<FText>& Text)
		{
			return SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(0.0f, 0.0f, 4.0f, 0.0f)
				[
					SNew(SImage)
					.Image(FRevisionControlStyleManager::Get().GetBrush("RevisionControl.BranchExplorer.BranchList.Branch"))
					.ColorAndOpacity(FSlateColor::UseForeground())
				]
				+ SHorizontalBox::Slot()
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock).Text(Text)
				];
		};

		bool bAccepted = false;
		const TSharedRef<SWindow> Window = MakeWindow(LOCTEXT("PullTitle", "Pull"));
		TWeakPtr<SWindow> WeakWindow = Window;
		Window->SetContent(
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
			.Padding(16.0f)
			[
				SNew(SBox)
				.WidthOverride(460.0f)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot()
					.AutoHeight()
					[
						SNew(STextBlock)
						.Text(LOCTEXT("PullPrompt", "Get commits from the server and bring them into your branch. Unreal saves your work first and reloads the assets that change."))
						.AutoWrapText(true)
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.0f, 12.0f, 0.0f, 0.0f)
					[
						SNew(SGridPanel)
						.FillColumn(1, 1.0f)
						+ SGridPanel::Slot(0, 0)
						.HAlign(HAlign_Right)
						.VAlign(VAlign_Center)
						.Padding(0.0f, 0.0f, 8.0f, 4.0f)
						[
							Label(LOCTEXT("PullRemote", "Remote:"))
						]
						+ SGridPanel::Slot(1, 0)
						.Padding(0.0f, 0.0f, 0.0f, 4.0f)
						[
							SNew(SComboBox<TSharedPtr<FString>>)
							.OptionsSource(&Remotes)
							.InitiallySelectedItem(Find(Remotes, Options.Remote))
							.OnGenerateWidget_Lambda([](const TSharedPtr<FString>& Item) { return SNew(STextBlock).Text(FText::FromString(*Item)); })
							.OnSelectionChanged_Lambda([ShowBranchesOf](const TSharedPtr<FString>& Item, ESelectInfo::Type)
							{
								if (Item.IsValid())
								{
									ShowBranchesOf(*Item);
								}
							})
							[
								SNew(STextBlock).Text_Lambda([&Options]() { return FText::FromString(Options.Remote); })
							]
						]
						+ SGridPanel::Slot(0, 1)
						.HAlign(HAlign_Right)
						.VAlign(VAlign_Center)
						.Padding(0.0f, 0.0f, 8.0f, 4.0f)
						[
							Label(LOCTEXT("PullBranch", "Branch:"))
						]
						+ SGridPanel::Slot(1, 1)
						.Padding(0.0f, 0.0f, 0.0f, 4.0f)
						[
							SAssignNew(BranchCombo, SComboBox<TSharedPtr<FString>>)
							.OptionsSource(&Branches)
							.InitiallySelectedItem(Find(Branches, Options.Branch))
							.OnGenerateWidget_Lambda([](const TSharedPtr<FString>& Item) { return SNew(STextBlock).Text(FText::FromString(*Item)); })
							.OnSelectionChanged_Lambda([&Options](const TSharedPtr<FString>& Item, ESelectInfo::Type)
							{
								if (Item.IsValid())
								{
									Options.Branch = *Item;
								}
							})
							[
								BranchName(MakeAttributeLambda([&Options]() { return FText::FromString(Options.Branch); }))
							]
						]
						+ SGridPanel::Slot(0, 2)
						.HAlign(HAlign_Right)
						.VAlign(VAlign_Center)
						.Padding(0.0f, 0.0f, 8.0f, 8.0f)
						[
							Label(LOCTEXT("PullInto", "Into:"))
						]
						+ SGridPanel::Slot(1, 2)
						.Padding(4.0f, 2.0f, 0.0f, 8.0f)
						[
							BranchName(FText::FromString(Into))
						]
						+ SGridPanel::Slot(1, 3)
						.Padding(0.0f, 0.0f, 0.0f, 4.0f)
						[
							SNew(SCheckBox)
							.IsChecked_Lambda([&Options]() { return Options.bRebase ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
							.OnCheckStateChanged_Lambda([&Options](ECheckBoxState State) { Options.bRebase = State == ECheckBoxState::Checked; })
							.ToolTipText(LOCTEXT("PullRebaseTooltip", "Your unpushed commits are replayed on top of the pulled ones, keeping history in one line. Off: a merge commit joins the two. Without unpushed commits, both just move your branch forward."))
							[
								SNew(STextBlock).Text(LOCTEXT("PullRebase", "Rebase instead of merge"))
							]
						]
						+ SGridPanel::Slot(1, 4)
						[
							SNew(SCheckBox)
							.IsChecked_Lambda([&Options]() { return Options.bStash ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
							.OnCheckStateChanged_Lambda([&Options](ECheckBoxState State) { Options.bStash = State == ECheckBoxState::Checked; })
							.ToolTipText(LOCTEXT("PullStashTooltip", "Local changes in the way are set aside and put back after the pull, combined with the server's version where both changed a file. If they cannot be combined, nothing is pulled. Off: the pull stops instead."))
							[
								SNew(STextBlock).Text(LOCTEXT("PullStash", "Stash and reapply local changes"))
							]
						]
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.0f, 16.0f, 0.0f, 0.0f)
					[
						Buttons(LOCTEXT("PullButton", "Pull"),
							[&bAccepted, WeakWindow]() { bAccepted = true; if (const TSharedPtr<SWindow> Pinned = WeakWindow.Pin()) { Pinned->RequestDestroyWindow(); } },
							[WeakWindow]() { if (const TSharedPtr<SWindow> Pinned = WeakWindow.Pin()) { Pinned->RequestDestroyWindow(); } },
							MakeAttributeLambda([&Options]() { return !Options.Remote.IsEmpty() && !Options.Branch.IsEmpty(); }))
					]
				]
			]);
		FSlateApplication::Get().AddModalWindow(Window, FSlateApplication::Get().GetActiveTopLevelWindow());
		if (bAccepted)
		{
			InOutOptions = Options;
		}
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
