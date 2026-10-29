// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#pragma once

#include "CoreMinimal.h"
#include "Editor/GitWorkspaceText.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SNullWidget.h"
#include "Widgets/Views/STableRow.h"
#include "Widgets/Views/STreeView.h"

class FGitWorkspaceModel;
class FMenuBuilder;

/**
 * Folder trees for the Git panel's file lists. An item type works with these helpers when it has
 * bool bFolder, FString Path (a file's repository path, or a folder's), FString Label (what a
 * folder row shows), int32 FileCount (files below a folder) and TArray<TSharedPtr<Item>> Children.
 */
namespace GitFileTree
{
	/** Whether lists show files in their folders; one preference for the whole panel. */
	bool ShowFolders();
	void SetShowFolders(bool bShow);
	/** The button next to a list's search box that switches between folders and one list of paths. */
	TSharedRef<SWidget> MakeFoldersToggle();

	/** Alt+click on a folder row (its arrow too) opens or closes everything below it, as Shift+click on the arrow does. */
	FReply ToggleOnAltClick(ITableRow& Row, const FPointerEvent& Event);
	/**
	 * Show in Content Browser and Show in Finder / Explorer for the selected rows' own paths (folders
	 * as folders), each only when it has something to show.
	 */
	void AddShowEntries(FMenuBuilder& Menu, const TSharedRef<FGitWorkspaceModel>& Model, const TArray<FString>& Paths);

	/**
	 * The name cell of a row: the expander, an icon (a folder's opens with it), the name and a
	 * subdued note such as a folder's file count. Trailing goes after the note.
	 */
	TSharedRef<SWidget> MakeNameCell(const TSharedRef<ITableRow>& Row, bool bFolder, const FText& Name, const FText& Note, const FSlateBrush* Icon,
		const FText& Tooltip, const TAttribute<FText>& Highlight, const TSharedRef<SWidget>& Trailing = SNullWidget::NullWidget);

	template<typename ItemType>
	void Tidy(TArray<TSharedPtr<ItemType>>& Items)
	{
		for (const TSharedPtr<ItemType>& Item : Items)
		{
			if (!Item->bFolder)
			{
				continue;
			}
			// A folder that only holds a folder is shown with it ("Maps/Main").
			while (Item->Children.Num() == 1 && Item->Children[0]->bFolder)
			{
				const TSharedPtr<ItemType> Only = Item->Children[0];
				Item->Label += TEXT("/") + Only->Label;
				Item->Path = Only->Path;
				Item->Children = MoveTemp(Only->Children);
			}
			Tidy(Item->Children);
		}
		Items.StableSort([](const TSharedPtr<ItemType>& A, const TSharedPtr<ItemType>& B)
		{
			if (A->bFolder != B->bFolder)
			{
				return A->bFolder;
			}
			return A->bFolder && A->Label.Compare(B->Label, ESearchCase::IgnoreCase) < 0;
		});
	}

	/**
	 * Puts Files into their folders, keeping their order, after each level's folders (sorted by
	 * name). Without folders the files come back as they are.
	 */
	template<typename ItemType>
	TArray<TSharedPtr<ItemType>> Build(const TArray<TSharedPtr<ItemType>>& Files, bool bFolders)
	{
		if (!bFolders)
		{
			return Files;
		}
		ItemType Root;
		TMap<FString, TSharedPtr<ItemType>> Folders;
		TArray<FString> Parts;
		for (const TSharedPtr<ItemType>& File : Files)
		{
			ItemType* Parent = &Root;
			FString FolderPath;
			Parts.Reset();
			GitWorkspaceText::TreeFolder(File->Path).ParseIntoArray(Parts, TEXT("/"));
			for (const FString& Part : Parts)
			{
				FolderPath = FolderPath.IsEmpty() ? Part : FolderPath + TEXT("/") + Part;
				TSharedPtr<ItemType>& Folder = Folders.FindOrAdd(FolderPath);
				if (!Folder.IsValid())
				{
					Folder = MakeShared<ItemType>();
					Folder->bFolder = true;
					Folder->Path = FolderPath;
					Folder->Label = Part;
					Parent->Children.Add(Folder);
				}
				Parent = Folder.Get();
				++Parent->FileCount;
			}
			Parent->Children.Add(File);
		}
		Tidy(Root.Children);
		return MoveTemp(Root.Children);
	}

	/** Item if it is a file, else every file below it. */
	template<typename ItemType>
	void CollectFiles(const TSharedPtr<ItemType>& Item, TArray<TSharedPtr<ItemType>>& Out)
	{
		if (!Item.IsValid())
		{
			return;
		}
		if (!Item->bFolder)
		{
			Out.Add(Item);
			return;
		}
		for (const TSharedPtr<ItemType>& Child : Item->Children)
		{
			CollectFiles(Child, Out);
		}
	}

	/** The files behind a selection of files and folders, each once. */
	template<typename ItemType>
	TArray<TSharedPtr<ItemType>> SelectedFiles(const TArray<TSharedPtr<ItemType>>& Selection)
	{
		TArray<TSharedPtr<ItemType>> Collected;
		for (const TSharedPtr<ItemType>& Item : Selection)
		{
			CollectFiles(Item, Collected);
		}
		TSet<TSharedPtr<ItemType>> Seen;
		TArray<TSharedPtr<ItemType>> Files;
		for (const TSharedPtr<ItemType>& File : Collected)
		{
			bool bAlreadySeen = false;
			Seen.Add(File, &bAlreadySeen);
			if (!bAlreadySeen)
			{
				Files.Add(File);
			}
		}
		return Files;
	}

	template<typename ItemType>
	void SetExpansionRecursive(STreeView<TSharedPtr<ItemType>>& Tree, const TSharedPtr<ItemType>& Item, bool bExpand)
	{
		if (Item.IsValid() && !Item->Children.IsEmpty())
		{
			Tree.SetItemExpansion(Item, bExpand);
			for (const TSharedPtr<ItemType>& Child : Item->Children)
			{
				SetExpansionRecursive(Tree, Child, bExpand);
			}
		}
	}

	/** Folders are open unless the user closed them, and all are open while a search filters the list. */
	template<typename ItemType>
	void RestoreExpansion(STreeView<TSharedPtr<ItemType>>& Tree, const TArray<TSharedPtr<ItemType>>& Items, const TSet<FString>& Collapsed, bool bSearching,
		const FString& KeyPrefix = FString())
	{
		for (const TSharedPtr<ItemType>& Item : Items)
		{
			if (Item->bFolder)
			{
				Tree.SetItemExpansion(Item, bSearching || !Collapsed.Contains(KeyPrefix + Item->Path));
				RestoreExpansion(Tree, Item->Children, Collapsed, bSearching, KeyPrefix);
			}
		}
	}

	/** Folders on the way to the first item that Predicate accepts, outermost first; false when none does. */
	template<typename ItemType>
	bool FindPath(const TArray<TSharedPtr<ItemType>>& Items, TFunctionRef<bool(const ItemType&)> Predicate, TArray<TSharedPtr<ItemType>>& OutPath)
	{
		for (const TSharedPtr<ItemType>& Item : Items)
		{
			OutPath.Add(Item);
			if ((!Item->bFolder && Predicate(*Item)) || (Item->bFolder && FindPath(Item->Children, Predicate, OutPath)))
			{
				return true;
			}
			OutPath.Pop();
		}
		return false;
	}

	/** A single-column tree row on which Alt+click opens or closes a folder and everything below it. */
	template<typename ItemType>
	class STreeRow : public STableRow<TSharedPtr<ItemType>>
	{
	public:
		SLATE_BEGIN_ARGS(STreeRow) {}
			SLATE_ARGUMENT(TSharedPtr<ItemType>, Item)
		SLATE_END_ARGS()

		void Construct(const FArguments& InArgs, const TSharedRef<STableViewBase>& Owner)
		{
			Item = InArgs._Item;
			STableRow<TSharedPtr<ItemType>>::Construct(typename STableRow<TSharedPtr<ItemType>>::FArguments(), Owner);
		}

		/** Shows a name cell as the whole row, in place of the expander STableRow adds in trees (the cell has its own). */
		void SetCell(const TSharedRef<SWidget>& Cell)
		{
			this->SetRowContent(SNew(SBox).Padding(FMargin(0.0f, 2.0f))[Cell]);
		}

		virtual FReply OnPreviewMouseButtonDown(const FGeometry& Geometry, const FPointerEvent& Event) override
		{
			return Item.IsValid() && Item->bFolder ? ToggleOnAltClick(*this, Event) : FReply::Unhandled();
		}

	private:
		TSharedPtr<ItemType> Item;
	};
}

/** A file or folder of a plain list of paths (a stash's files). */
struct FGitPathItem
{
	bool bFolder = false;
	FString Path;
	FString Label;
	int32 FileCount = 0;
	TArray<TSharedPtr<FGitPathItem>> Children;
};

/** How repository files are shown (asset names, actor labels, class icons), cached for one list. Game thread only. */
class FGitFileDisplays
{
public:
	const FGitFileDisplay& Get(const FGitWorkspaceModel* Model, const FString& Path);
	void Reset() { Cache.Reset(); }

private:
	TMap<FString, FGitFileDisplay> Cache;
};
