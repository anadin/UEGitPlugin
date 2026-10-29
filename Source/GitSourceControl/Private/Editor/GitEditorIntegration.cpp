// Copyright UEGitPlugin contributors. Distributed under the MIT license.
#include "GitEditorIntegration.h"

#include "GitSourceControlModule.h"
#include "GitWorkspaceModel.h"
#include "GitWorkspaceText.h"
#include "Panel/SGitPanel.h"
#include "Provider/GitSourceControlUserSettings.h"

#include "ContentBrowserMenuContexts.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
#include "Framework/Notifications/NotificationManager.h"
#include "ISourceControlModule.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "RevisionControlStyle/RevisionControlStyle.h"
#include "Settings/EditorLoadingSavingSettings.h"
#include "SourceControlHelpers.h"
#include "Styling/AppStyle.h"
#include "ToolMenus.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Text/STextBlock.h"
#include "WorkspaceMenuStructure.h"
#include "WorkspaceMenuStructureModule.h"

#define LOCTEXT_NAMESPACE "GitEditorIntegration"

namespace
{
	const FName MenuOwner(TEXT("GitWorkspace"));
	FDelegateHandle ModelChangedHandle;
	FDelegateHandle MenusStartupHandle;
	TWeakPtr<SGitPanel> LivePanel;
	bool bCheckoutPromptOffered = false;

	TSharedRef<FGitWorkspaceModel> Model()
	{
		return FGitWorkspaceModel::Get()->AsShared();
	}

	TSharedRef<SDockTab> SpawnPanel(const FSpawnTabArgs& Args)
	{
		TSharedPtr<SGitPanel> Panel;
		const TSharedRef<SDockTab> Tab = SNew(SDockTab)
			.TabRole(ETabRole::NomadTab)
			[
				SAssignNew(Panel, SGitPanel, Model())
			];
		LivePanel = Panel;
		// Opening the panel is a good moment to catch up with the working tree.
		if (const GitCore::FGitSessionPtr Session = Model()->GetSession())
		{
			Session->RequestStatusRefresh();
			Session->RequestStashesAndBranches();
		}
		return Tab;
	}

	/**
	 * What Unreal's Submit dialog would list: packages and project config that can be checked
	 * in or added, and are current. Read from the provider's cached states.
	 */
	int32 CountSubmittable(const GitCore::FSessionSnapshot& Snapshot, const GitCore::FGitSession& Session)
	{
		ISourceControlProvider& Provider = ISourceControlModule::Get().GetProvider();
		const FString ConfigDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectConfigDir());
		const FString ProjectFile = FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath());
		int32 Count = 0;
		for (const GitCore::FStatusEntry& Entry : Snapshot.Status.Entries)
		{
			const FString File = Session.GetRepository().ToAbsolute(Entry.Path);
			const FString Clean = FPaths::GetCleanFilename(File);
			FString PackageName;
			const bool bPackage = FPackageName::IsPackageExtension(*FPaths::GetExtension(File)) && FPackageName::TryConvertFilenameToLongPackageName(File, PackageName);
			const bool bConfig = File.EndsWith(TEXT(".ini")) && FPaths::IsUnderDirectory(File, ConfigDir) && Clean != TEXT("DefaultEditorPerProjectUserSettings.ini") && !Clean.StartsWith(TEXT("User"));
			if (!bPackage && !bConfig && !File.Equals(ProjectFile, ESearchCase::IgnoreCase))
			{
				continue;
			}
			const FSourceControlStatePtr State = Provider.GetState(File, EStateCacheUsage::Use);
			if (State.IsValid() && State->IsCurrent() && (State->CanCheckIn() || (!State->IsSourceControlled() && State->CanAdd())))
			{
				++Count;
			}
		}
		return Count;
	}

	/** Branch, Pull, Check in and Push, next to Unreal's own revision control status. */
	class SGitStatusBar : public SCompoundWidget
	{
	public:
		SLATE_BEGIN_ARGS(SGitStatusBar) {}
		SLATE_END_ARGS()

		void Construct(const FArguments& InArgs)
		{
			WeakModel = Model();
			ChangedHandle = Model()->OnChanged().AddSP(this, &SGitStatusBar::Update);
			Update();
			ChildSlot
			[
				SNew(SHorizontalBox)
				.Visibility_Lambda([this]() { const TSharedPtr<FGitWorkspaceModel> M = WeakModel.Pin(); return M.IsValid() && M->IsActive() ? EVisibility::Visible : EVisibility::Collapsed; })
				+ SHorizontalBox::Slot()
				.AutoWidth()
				[
					MakeButton(TEXT("RevisionControl.BranchExplorer.BranchList.Branch"), TAttribute<FText>::CreateLambda([this]() { return Branch; }),
						LOCTEXT("PanelTooltip", "Open the Git panel: changes, history, stashes and locks."), []() { return true; },
						[]() { FGlobalTabmanager::Get()->TryInvokeTab(SGitPanel::TabId); })
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				[
					MakeButton(TEXT("RevisionControl.StatusBar.NotAtLatestRevision"), TAttribute<FText>::CreateLambda([this]() { return FText::Format(LOCTEXT("PullButton", "Pull {0}"), Behind); }),
						TAttribute<FText>::CreateLambda([this]() { return FText::Format(LOCTEXT("PullTooltip", "Get {0} new {0}|plural(one=commit,other=commits) from {1}. Unreal saves first and reloads the changed assets."), Behind, FText::FromString(Upstream)); }),
						[this]() { return Behind > 0; },
						[this]() { if (const TSharedPtr<FGitWorkspaceModel> M = WeakModel.Pin()) { M->Sync(); } })
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				[
					MakeButton(TEXT("RevisionControl.Actions.Submit"), TAttribute<FText>::CreateLambda([this]() { return FText::Format(LOCTEXT("CheckInButton", "Check in {0}"), Submittable); }),
						LOCTEXT("CheckInTooltip", "Submit your changed assets: commit, push and release their locks."),
						[this]() { return Submittable > 0; },
						[this]() { if (const TSharedPtr<FGitWorkspaceModel> M = WeakModel.Pin()) { M->CheckIn(); } })
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				[
					MakeButton(TEXT("RevisionControl.StatusBar.HasLocalChanges"), TAttribute<FText>::CreateLambda([this]() { return FText::Format(LOCTEXT("PushButton", "Push {0}"), Ahead); }),
						LOCTEXT("PushTooltip", "Publish your local commits. Locks are kept."),
						[this]() { return Ahead > 0; },
						[this]() { if (const TSharedPtr<FGitWorkspaceModel> M = WeakModel.Pin()) { M->Push(); } })
				]
			];
		}

		virtual ~SGitStatusBar() override
		{
			if (const TSharedPtr<FGitWorkspaceModel> M = WeakModel.Pin())
			{
				M->OnChanged().Remove(ChangedHandle);
			}
		}

	private:
		/** Counts are refreshed when the repository changes, not every frame. */
		void Update()
		{
			const TSharedPtr<FGitWorkspaceModel> M = WeakModel.Pin();
			const GitCore::FGitSessionPtr Session = M.IsValid() ? M->GetSession() : nullptr;
			if (!Session.IsValid())
			{
				Behind = Ahead = Submittable = 0;
				return;
			}
			const GitCore::FSessionSnapshotRef Snapshot = Session->GetSnapshot();
			Behind = FMath::Max(0, Snapshot->Remote.Behind);
			Ahead = FMath::Max(0, Snapshot->Remote.Ahead);
			Upstream = Snapshot->Remote.Upstream;
			Branch = Snapshot->Status.Branch.IsEmpty() ? LOCTEXT("NoBranch", "No branch") : FText::FromString(Snapshot->Status.Branch);
			Submittable = CountSubmittable(*Snapshot, *Session);
		}

		static TSharedRef<SWidget> MakeButton(const TCHAR* Icon, const TAttribute<FText>& Label, const TAttribute<FText>& Tooltip, TFunction<bool()> IsShown, TFunction<void()> Action)
		{
			return SNew(SButton)
				.ButtonStyle(FAppStyle::Get(), "SimpleButton")
				.ToolTipText(Tooltip)
				.Visibility_Lambda([IsShown]() { return IsShown() ? EVisibility::Visible : EVisibility::Collapsed; })
				.IsEnabled_Lambda([]() { const FGitWorkspaceModel* M = FGitWorkspaceModel::Get(); return M && !M->IsBusy(); })
				.OnClicked_Lambda([Action]() { Action(); return FReply::Handled(); })
				.ContentPadding(FMargin(6.0f, 0.0f))
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					.Padding(0.0f, 0.0f, 4.0f, 0.0f)
					[
						SNew(SImage)
						.Image(FRevisionControlStyleManager::Get().GetBrush(Icon))
						.ColorAndOpacity(FSlateColor::UseForeground())
					]
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					[
						SNew(STextBlock).Text(Label)
					]
				];
		}

		TWeakPtr<FGitWorkspaceModel> WeakModel;
		FDelegateHandle ChangedHandle;
		FText Branch;
		FString Upstream;
		int32 Behind = 0;
		int32 Ahead = 0;
		int32 Submittable = 0;
	};

	void AddContentBrowserEntry(UToolMenu* Menu)
	{
		const UContentBrowserAssetContextMenuContext* Context = Menu->FindContext<UContentBrowserAssetContextMenuContext>();
		const FGitWorkspaceModel* Workspace = FGitWorkspaceModel::Get();
		if (!Context || !Workspace || !Workspace->IsActive() || Context->SelectedAssets.IsEmpty())
		{
			return;
		}
		TArray<FString> Files;
		for (const FAssetData& Asset : Context->SelectedAssets)
		{
			Files.Add(USourceControlHelpers::PackageFilename(Asset.PackageName.ToString()));
		}
		FToolMenuSection& Section = Menu->FindOrAddSection("AssetContextSourceControl");
		Section.AddMenuEntry("GitShowInPanel", LOCTEXT("ShowInPanel", "Show in Git Panel"), LOCTEXT("ShowInPanelTooltip", "Select these assets' changes in the Git panel."),
			FSlateIcon(FRevisionControlStyleManager::GetStyleSetName(), "RevisionControl.Icon"),
			FUIAction(FExecuteAction::CreateLambda([Files]()
			{
				GitEditorIntegration::ShowInPanel(Files);
			})));

		const GitCore::FGitSessionPtr Session = Workspace->GetSession();
		const GitCore::FSessionSnapshotRef Snapshot = Workspace->GetSnapshot();
		TArray<FString> Claimable;
		for (const FString& File : Files)
		{
			const FString Path = Session.IsValid() ? Session->GetRepository().ToRelative(File) : FString();
			const GitCore::FLockInfo* Lock = Path.IsEmpty() ? nullptr : Snapshot->Locks.Find(Path);
			if (Lock && Snapshot->IsOursElsewhere(*Lock))
			{
				Claimable.Add(Path);
			}
		}
		if (!Claimable.IsEmpty())
		{
			Section.AddMenuEntry("GitClaimLock", LOCTEXT("ClaimLock", "Claim Lock"), GitWorkspaceText::ClaimLockTooltip(),
				FSlateIcon(FRevisionControlStyleManager::GetStyleSetName(), "RevisionControl.Actions.CheckOut"),
				FUIAction(FExecuteAction::CreateLambda([Claimable]()
				{
					if (FGitWorkspaceModel* Model = FGitWorkspaceModel::Get())
					{
						Model->ClaimLocks(Claimable);
					}
				}),
				FCanExecuteAction::CreateLambda([]()
				{
					const FGitWorkspaceModel* Model = FGitWorkspaceModel::Get();
					return Model && !Model->IsBusy();
				})));
		}
	}

	void RegisterMenus()
	{
		FToolMenuOwnerScoped OwnerScoped(MenuOwner);
		if (UToolMenu* StatusBar = UToolMenus::Get()->ExtendMenu("LevelEditor.StatusBar.ToolBar"))
		{
			FToolMenuSection& Section = StatusBar->AddSection("GitWorkspace", FText::GetEmpty(), FToolMenuInsert("SourceControl", EToolMenuInsertType::Before));
			Section.AddEntry(FToolMenuEntry::InitWidget("GitWorkspaceStatus", SNew(SGitStatusBar), FText::GetEmpty(), true, false));
		}
		if (UToolMenu* Menu = UToolMenus::Get()->ExtendMenu("StatusBar.ToolBar.SourceControl"))
		{
			FToolMenuSection& Section = Menu->AddSection("GitWorkspace", LOCTEXT("GitSection", "Git"));
			const FToolMenuCanExecuteAction IsActive = FToolMenuCanExecuteAction::CreateLambda([](const FToolMenuContext&)
			{
				const FGitWorkspaceModel* M = FGitWorkspaceModel::Get();
				return M && M->IsActive() && !M->IsBusy();
			});
			auto Add = [&Section, &IsActive](FName Name, const FText& Label, const FText& Tooltip, const TCHAR* Icon, TFunction<void(FGitWorkspaceModel&)> Action)
			{
				FToolUIAction UIAction;
				UIAction.ExecuteAction = FToolMenuExecuteAction::CreateLambda([Action](const FToolMenuContext&) { if (FGitWorkspaceModel* M = FGitWorkspaceModel::Get()) { Action(*M); } });
				UIAction.CanExecuteAction = IsActive;
				Section.AddMenuEntry(Name, Label, Tooltip, FSlateIcon(FRevisionControlStyleManager::GetStyleSetName(), Icon), UIAction);
			};
			Section.AddMenuEntry("GitOpenPanel", LOCTEXT("OpenPanelMenu", "Git Panel"), LOCTEXT("OpenPanelMenuTooltip", "Changes, history, stashes and locks."),
				FSlateIcon(FRevisionControlStyleManager::GetStyleSetName(), "RevisionControl.Icon"),
				FUIAction(FExecuteAction::CreateLambda([]() { FGlobalTabmanager::Get()->TryInvokeTab(SGitPanel::TabId); })));
			Add("GitPush", LOCTEXT("PushMenu", "Push"), LOCTEXT("PushMenuTooltip", "Publish your local commits. Locks are kept."), TEXT("RevisionControl.Actions.Submit"),
				[](FGitWorkspaceModel& M) { M.Push(); });
			Add("GitFetch", LOCTEXT("FetchMenu", "Check for New Commits"), LOCTEXT("FetchMenuTooltip", "Fetch from the server now."), TEXT("RevisionControl.Actions.Refresh"),
				[](FGitWorkspaceModel& M) { M.Fetch(); });
			Add("GitLocks", LOCTEXT("LocksMenu", "Check Locks"), LOCTEXT("LocksMenuTooltip", "Ask the server who holds which locks."), TEXT("RevisionControl.Locked"),
				[](FGitWorkspaceModel& M) { M.CheckLocks(); });
		}
		if (UToolMenu* Menu = UToolMenus::Get()->ExtendMenu("ContentBrowser.AssetContextMenu"))
		{
			Menu->AddDynamicSection("GitWorkspace", FNewToolMenuDelegate::CreateStatic(&AddContentBrowserEntry));
		}
	}

	/** Once per project: Unreal ships with "prompt for checkout on modify" off, which hides locking from artists. */
	void OfferCheckoutPrompt()
	{
		const FGitWorkspaceModel* Workspace = FGitWorkspaceModel::Get();
		if (bCheckoutPromptOffered || !Workspace || !Workspace->IsLockingEnabled())
		{
			return;
		}
		bCheckoutPromptOffered = true;
		UGitSourceControlUserSettings* User = GetMutableDefault<UGitSourceControlUserSettings>();
		const UEditorLoadingSavingSettings* Saving = GetDefault<UEditorLoadingSavingSettings>();
		if (User->bOfferedCheckoutPrompt || Saving->bPromptForCheckoutOnAssetModification)
		{
			return;
		}
		User->bOfferedCheckoutPrompt = true;
		User->SaveConfig();

		FNotificationInfo Info(LOCTEXT("CheckoutPromptOffer", "This project locks assets. Ask to check out (lock) an asset when you start editing it?"));
		Info.bFireAndForget = false;
		Info.ExpireDuration = 0.0f;
		TSharedPtr<TWeakPtr<SNotificationItem>> Handle = MakeShared<TWeakPtr<SNotificationItem>>();
		Info.ButtonDetails.Add(FNotificationButtonInfo(LOCTEXT("TurnOn", "Turn on"), LOCTEXT("TurnOnTooltip", "Prompt for check out when an asset is modified."),
			FSimpleDelegate::CreateLambda([Handle]()
			{
				UEditorLoadingSavingSettings* Settings = GetMutableDefault<UEditorLoadingSavingSettings>();
				Settings->bPromptForCheckoutOnAssetModification = true;
				Settings->SaveConfig();
				if (TSharedPtr<SNotificationItem> Item = Handle->Pin())
				{
					Item->ExpireAndFadeout();
				}
			}), SNotificationItem::CS_None));
		Info.ButtonDetails.Add(FNotificationButtonInfo(LOCTEXT("NotNow", "Not now"), FText::GetEmpty(),
			FSimpleDelegate::CreateLambda([Handle]()
			{
				if (TSharedPtr<SNotificationItem> Item = Handle->Pin())
				{
					Item->ExpireAndFadeout();
				}
			}), SNotificationItem::CS_None));
		*Handle = FSlateNotificationManager::Get().AddNotification(Info);
	}
}

namespace GitEditorIntegration
{
	void Register()
	{
		FGitWorkspaceModel::Startup();
		ModelChangedHandle = Model()->OnChanged().AddStatic(&OfferCheckoutPrompt);
		FGlobalTabmanager::Get()->RegisterNomadTabSpawner(SGitPanel::TabId, FOnSpawnTab::CreateStatic(&SpawnPanel))
			.SetDisplayName(LOCTEXT("PanelTitle", "Git"))
			.SetTooltipText(LOCTEXT("PanelTabTooltip", "Changes, history, stashes and locks for this project's Git repository."))
			.SetGroup(WorkspaceMenu::GetMenuStructure().GetToolsCategory())
			.SetIcon(FSlateIcon(FRevisionControlStyleManager::GetStyleSetName(), "RevisionControl.Icon"));
		MenusStartupHandle = UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateStatic(&RegisterMenus));
	}

	void Unregister()
	{
		if (FSlateApplication::IsInitialized())
		{
			FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(SGitPanel::TabId);
		}
		if (UObjectInitialized() && UToolMenus::TryGet())
		{
			UToolMenus::UnRegisterStartupCallback(MenusStartupHandle);
			UToolMenus::UnregisterOwner(MenuOwner);
		}
		if (FGitWorkspaceModel* Workspace = FGitWorkspaceModel::Get())
		{
			Workspace->OnChanged().Remove(ModelChangedHandle);
		}
		FGitWorkspaceModel::Shutdown();
	}

	void ShowInPanel(const TArray<FString>& AbsoluteFiles)
	{
		const FGitWorkspaceModel* Workspace = FGitWorkspaceModel::Get();
		const GitCore::FGitSessionPtr Session = Workspace ? Workspace->GetSession() : nullptr;
		if (!Session.IsValid())
		{
			return;
		}
		TArray<FString> Paths;
		for (const FString& File : AbsoluteFiles)
		{
			const FString Relative = Session->GetRepository().ToRelative(File);
			if (!Relative.IsEmpty())
			{
				Paths.Add(Relative);
			}
		}
		FGlobalTabmanager::Get()->TryInvokeTab(SGitPanel::TabId);
		if (const TSharedPtr<SGitPanel> Panel = LivePanel.Pin())
		{
			Panel->ShowPaths(Paths);
		}
	}
}

#undef LOCTEXT_NAMESPACE
