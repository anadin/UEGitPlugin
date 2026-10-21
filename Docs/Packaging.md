# Building a plugin package (macOS)

From the plugin folder, with Unreal Engine 5.8 installed:

```sh
UE_ENGINE_ROOT='/Users/Shared/Epic Games/UE_5.8' bash Scripts/package-mac.sh /absolute/new-output-directory
```

The script refuses an existing output directory, runs Unreal's `BuildPlugin` for the editor (Apple Silicon), and checks the result with `Scripts/check_mac_package.py`. It writes `BuildPlugin.log` and `PackageAudit.json` next to the packaged `Plugin/` folder and never contacts a Git remote.

To try a package, copy the **contents** of `Plugin/` into a disposable project's `Plugins/UEGitPlugin/`, enable **Git Workspace**, and run the `GitWorkspace` automation tests there.
