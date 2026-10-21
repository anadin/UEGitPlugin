// Copyright (c) 2014-2020 Sebastien Rombauts (sebastien.rombauts@gmail.com)
//
// Distributed under the MIT License (MIT) (See accompanying file LICENSE.txt
// or copy at http://opensource.org/licenses/MIT)

#include "SGitSourceControlSettings.h"

#include "GitSourceControlModule.h"
#include "GitSourceControlProvider.h"

#include "Styling/AppStyle.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "SGitSourceControlSettings"

namespace
{
	FGitSourceControlProvider& Provider()
	{
		return FGitSourceControlModule::Get().GetProvider();
	}

	TSharedRef<SWidget> Row(const FText& Label, const FText& Tooltip, const TSharedRef<SWidget>& Value)
	{
		return SNew(SHorizontalBox)
			.ToolTipText(Tooltip)
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.Padding(2.0f)
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock).Text(Label)
			]
			+ SHorizontalBox::Slot()
			.FillWidth(2.0f)
			.Padding(2.0f)
			[
				Value
			];
	}
}

void SGitSourceControlSettings::Construct(const FArguments& InArgs)
{
	ChildSlot
	[
		SNew(SVerticalBox)
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(2.0f)
		[
			Row(LOCTEXT("GitPath", "Git executable"), LOCTEXT("GitPathTooltip", "Path to git. Leave empty to find it automatically."),
				SNew(SEditableTextBox)
				.Text(this, &SGitSourceControlSettings::GetBinaryPathText)
				.HintText(LOCTEXT("GitPathHint", "Found automatically"))
				.OnTextCommitted(this, &SGitSourceControlSettings::OnBinaryPathCommitted))
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(2.0f)
		[
			Row(LOCTEXT("Repository", "Repository"), LOCTEXT("RepositoryTooltip", "The Git repository that contains this project."),
				SNew(STextBlock).Text(this, &SGitSourceControlSettings::GetRepositoryText))
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(2.0f)
		[
			Row(LOCTEXT("UseLocking", "Use Git LFS locks"), LOCTEXT("UseLockingTooltip", "Check Out locks files marked `lockable` in .gitattributes so only one person edits them at a time."),
				SNew(SCheckBox)
				.IsChecked(this, &SGitSourceControlSettings::IsUsingLocking)
				.OnCheckStateChanged(this, &SGitSourceControlSettings::OnLockingChanged))
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(2.0f)
		[
			Row(LOCTEXT("LockRemote", "Lock server remote"), LOCTEXT("LockRemoteTooltip", "The remote whose Git LFS server holds locks. Leave empty to use the branch's upstream remote."),
				SNew(SEditableTextBox)
				.Text(this, &SGitSourceControlSettings::GetLockRemoteText)
				.HintText(LOCTEXT("LockRemoteHint", "Upstream remote (usually origin)"))
				.OnTextCommitted(this, &SGitSourceControlSettings::OnLockRemoteCommitted))
		]
	];
}

FText SGitSourceControlSettings::GetBinaryPathText() const
{
	return FText::FromString(Provider().AccessSettings().GetBinaryPath());
}

void SGitSourceControlSettings::OnBinaryPathCommitted(const FText& InText, ETextCommit::Type InCommitType)
{
	if (Provider().AccessSettings().SetBinaryPath(InText.ToString().TrimStartAndEnd()))
	{
		Provider().AccessSettings().SaveSettings();
		Provider().Close();
		Provider().Init();
	}
}

FText SGitSourceControlSettings::GetRepositoryText() const
{
	const GitCore::FGitSessionPtr Session = Provider().GetSession();
	if (!Session.IsValid())
	{
		return LOCTEXT("NoRepository", "No Git repository found for this project");
	}
	const GitCore::FSessionSnapshotRef Snapshot = Session->GetSnapshot();
	return Snapshot->Status.Branch.IsEmpty()
		? FText::FromString(Snapshot->Root)
		: FText::Format(LOCTEXT("RepositoryAndBranch", "{0} ({1})"), FText::FromString(Snapshot->Root), FText::FromString(Snapshot->Status.Branch));
}

ECheckBoxState SGitSourceControlSettings::IsUsingLocking() const
{
	return Provider().AccessSettings().IsUsingGitLfsLocking() ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
}

void SGitSourceControlSettings::OnLockingChanged(ECheckBoxState NewState)
{
	Provider().AccessSettings().SetUsingGitLfsLocking(NewState == ECheckBoxState::Checked);
	Provider().AccessSettings().SaveSettings();
}

FText SGitSourceControlSettings::GetLockRemoteText() const
{
	return FText::FromString(Provider().AccessSettings().GetLockRemote());
}

void SGitSourceControlSettings::OnLockRemoteCommitted(const FText& InText, ETextCommit::Type InCommitType)
{
	const FString Remote = InText.ToString().TrimStartAndEnd();
	if (Provider().AccessSettings().SetLockRemote(Remote))
	{
		Provider().AccessSettings().SaveSettings();
		if (const GitCore::FGitSessionPtr Session = Provider().GetSession())
		{
			Session->SetLockRemote(Remote);
			Session->RequestLockRefresh(true);
		}
	}
}

#undef LOCTEXT_NAMESPACE
