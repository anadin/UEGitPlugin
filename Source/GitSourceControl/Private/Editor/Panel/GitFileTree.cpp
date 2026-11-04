// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#include "GitFileTree.h"

#include "Editor/GitWorkspaceModel.h"
#include "Editor/GitWorkspaceText.h"
#include "Provider/GitSourceControlUserSettings.h"

#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "HAL/PlatformApplicationMisc.h"
#include "Styling/AppStyle.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/SExpanderArrow.h"

#define LOCTEXT_NAMESPACE "GitWorkspaceFileTree"

bool GitFileTree::ShowFolders()
{
	return GetDefault<UGitSourceControlUserSettings>()->bShowFolders;
}

void GitFileTree::SetShowFolders(bool bShow)
{
	UGitSourceControlUserSettings* Settings = GetMutableDefault<UGitSourceControlUserSettings>();
	if (Settings->bShowFolders != bShow)
	{
		Settings->bShowFolders = bShow;
		Settings->SaveConfig();
	}
}

TSharedRef<SWidget> GitFileTree::MakeFoldersToggle()
{
	return SNew(SCheckBox)
		.Style(FAppStyle::Get(), "ToggleButtonCheckbox")
		.Padding(FMargin(4.0f, 2.0f))
		.ToolTipText(FText::Format(LOCTEXT("FoldersToggleTooltip", "Show files in their folders (off: one list of paths). {0} a folder to open or close everything in it."),
			GitWorkspaceText::AltClick()))
		.IsChecked_Lambda([]() { return ShowFolders() ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
		.OnCheckStateChanged_Lambda([](ECheckBoxState State) { SetShowFolders(State == ECheckBoxState::Checked); })
		[
			SNew(SImage)
			.Image(FAppStyle::GetBrush("Icons.FolderClosed"))
			.ColorAndOpacity(FSlateColor::UseForeground())
		];
}

FReply GitFileTree::ToggleOnAltClick(ITableRow& Row, const FPointerEvent& Event)
{
	if (Event.GetEffectingButton() == EKeys::LeftMouseButton && Event.IsAltDown() && Row.DoesItemHaveChildren() != 0)
	{
		Row.Private_OnExpanderArrowShiftClicked();
		return FReply::Handled();
	}
	return FReply::Unhandled();
}

void GitFileTree::AddShowEntries(FMenuBuilder& Menu, const TSharedRef<FGitWorkspaceModel>& Model, const TArray<FString>& Paths)
{
	const TWeakPtr<FGitWorkspaceModel> Weak = Model;
	if (Model->CanBrowseTo(Paths))
	{
		Menu.AddMenuEntry(LOCTEXT("BrowseMenu", "Show in Content Browser"),
			LOCTEXT("BrowseMenuTooltip", "Select these assets or folders in the Content Browser. A World Partition actor shows its level."),
			FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.BrowseContent"),
			FUIAction(FExecuteAction::CreateLambda([Weak, Paths]() { if (const TSharedPtr<FGitWorkspaceModel> M = Weak.Pin()) { M->BrowseTo(Paths); } })));
	}
	if (Model->CanReveal(Paths))
	{
		Menu.AddMenuEntry(FGitWorkspaceModel::RevealLabel(),
			FText::Format(LOCTEXT("RevealMenuTooltip", "Show these files or folders in {0}."), FPlatformMisc::GetFileManagerName()),
			FSlateIcon(FAppStyle::GetAppStyleSetName(), "ContentBrowser.ShowInExplorer"),
			FUIAction(FExecuteAction::CreateLambda([Weak, Paths]() { if (const TSharedPtr<FGitWorkspaceModel> M = Weak.Pin()) { M->Reveal(Paths); } })));
	}
}

void GitFileTree::CopyPaths(const TArray<FString>& Paths)
{
	if (!Paths.IsEmpty())
	{
		FPlatformApplicationMisc::ClipboardCopy(*FString::Join(Paths, LINE_TERMINATOR));
	}
}

bool GitFileTree::IsCopyKey(const FKeyEvent& Event)
{
	return Event.GetKey() == EKeys::C && (Event.IsControlDown() || Event.IsCommandDown()) && !Event.IsShiftDown() && !Event.IsAltDown();
}

void GitFileTree::AddCopyPathEntry(FMenuBuilder& Menu, const TArray<FString>& Paths)
{
	GitWorkspaceText::AddMenuEntry(Menu, LOCTEXT("CopyPathMenu", "Copy path"), LOCTEXT("CopyPathMenuTooltip", "Copy the selected rows' repository paths, one per line."),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "GenericCommands.Copy"), FUIAction(FExecuteAction::CreateLambda([Paths]() { CopyPaths(Paths); })),
		GitWorkspaceText::Shortcut(EModifierKey::Control, EKeys::C));
}

void GitFileTree::AddFolderEntries(FMenuBuilder& Menu, TFunction<void(bool bExpand)> SetExpansionRecursive)
{
	// Shift with Right or Left does the same for the focused folder (Unreal's tree views), as Alt+click does.
	GitWorkspaceText::AddMenuEntry(Menu, LOCTEXT("ExpandAllMenu", "Open all inside"),
		FText::Format(LOCTEXT("ExpandAllMenuTooltip", "Open these folders and every folder in them. ({0})"), GitWorkspaceText::AltClick()),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.FolderOpen"), FUIAction(FExecuteAction::CreateLambda([SetExpansionRecursive]() { SetExpansionRecursive(true); })),
		GitWorkspaceText::Shortcut(EModifierKey::Shift, EKeys::Right));
	GitWorkspaceText::AddMenuEntry(Menu, LOCTEXT("CollapseAllMenu", "Close all inside"),
		FText::Format(LOCTEXT("CollapseAllMenuTooltip", "Close these folders and every folder in them. ({0})"), GitWorkspaceText::AltClick()),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.FolderClosed"), FUIAction(FExecuteAction::CreateLambda([SetExpansionRecursive]() { SetExpansionRecursive(false); })),
		GitWorkspaceText::Shortcut(EModifierKey::Shift, EKeys::Left));
}

TSharedRef<SWidget> GitFileTree::MakeNameCell(const TSharedRef<ITableRow>& Row, bool bFolder, const FText& Name, const FText& Note, const FSlateBrush* Icon,
	const FText& Tooltip, const TAttribute<FText>& Highlight, const TSharedRef<SWidget>& Trailing)
{
	const TWeakPtr<ITableRow> WeakRow = Row;
	return SNew(SHorizontalBox)
		.ToolTipText(Tooltip)
		+ SHorizontalBox::Slot()
		.AutoWidth()
		[
			SNew(SExpanderArrow, Row)
			.IndentAmount(12.0f)
		]
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(0.0f, 0.0f, 6.0f, 0.0f)
		[
			SNew(SImage)
			.DesiredSizeOverride(FVector2D(16.0f, 16.0f))
			.ColorAndOpacity(bFolder ? FSlateColor::UseSubduedForeground() : FSlateColor::UseForeground())
			.Image_Lambda([WeakRow, bFolder, Icon]()
			{
				if (!bFolder)
				{
					return Icon;
				}
				const TSharedPtr<ITableRow> Pinned = WeakRow.Pin();
				return FAppStyle::GetBrush(Pinned.IsValid() && Pinned->IsItemExpanded() ? "Icons.FolderOpen" : "Icons.FolderClosed");
			})
		]
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		[
			SNew(STextBlock)
			.Text(Name)
			.HighlightText(Highlight)
		]
		+ SHorizontalBox::Slot()
		.FillWidth(1.0f)
		.VAlign(VAlign_Center)
		.Padding(8.0f, 0.0f, 0.0f, 0.0f)
		[
			SNew(STextBlock)
			.Text(Note)
			.HighlightText(Highlight)
			.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
		]
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		[
			Trailing
		];
}

const FGitFileDisplay& FGitFileDisplays::Get(const FGitWorkspaceModel* Model, const FString& Path)
{
	if (const FGitFileDisplay* Found = Cache.Find(Path))
	{
		return *Found;
	}
	return Cache.Add(Path, FGitFileDisplay::Make(Model ? Model->ToAbsolute(Path) : FString(), Path));
}

#undef LOCTEXT_NAMESPACE
