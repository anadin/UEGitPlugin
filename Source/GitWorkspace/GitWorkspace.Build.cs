// Copyright (c) 2014-2020 Sebastien Rombauts (sebastien.rombauts@gmail.com)
//
// Distributed under the MIT License (MIT) (See accompanying file LICENSE.txt
// or copy at http://opensource.org/licenses/MIT)

using System.IO;
using EpicGames.Core;
using UnrealBuildTool;

public class GitWorkspace : ModuleRules
{
	public GitWorkspace(ReadOnlyTargetRules Target) : base(Target)
	{
		PrivateDependencyModuleNames.AddRange(
			new string[] {
				"Core",
				"CoreUObject",
				"ApplicationCore",
				"Json",
				"Engine",
				"ImageCore",
				"DeveloperSettings",
				"AssetRegistry",
				"AssetDefinition",
				"AssetTools",
				"Slate",
				"SlateCore",
				"InputCore",
				"DesktopWidgets",
				"EditorStyle",
				"UnrealEd",
				"SourceControl",
				"SourceControlWindows",
				"Projects",
				"LevelEditor",
				"ContentBrowser",
				"ToolMenus",
				"WorkspaceMenuStructure"
			}
		);

		ForgetEarlierModuleName();
	}

	/// <summary>
	/// Until the plugin was renamed, this module was called GitSourceControl, like the module of Unreal's own
	/// Git plugin. A build from then leaves that name in the plugin's module lists (Binaries/*/*.modules): the
	/// build tool keeps entries it did not write, though it deletes their library. Unreal reads this plugin's
	/// lists after its own, so it would look for its Git plugin's module here, fail to load it and quit.
	/// </summary>
	private void ForgetEarlierModuleName()
	{
		DirectoryReference Binaries = DirectoryReference.Combine(new DirectoryReference(PluginDirectory), "Binaries");
		if (!DirectoryReference.Exists(Binaries))
		{
			return;
		}
		foreach (FileReference List in DirectoryReference.EnumerateFiles(Binaries, "*.modules", SearchOption.AllDirectories))
		{
			if (ModuleManifest.TryRead(List, out var Manifest) && Manifest.ModuleNameToFileName.Remove("GitSourceControl"))
			{
				Manifest.Write(List);
			}
		}
	}
}
