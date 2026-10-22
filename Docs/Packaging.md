# Building a plugin package

`Scripts/package.py` builds an editor package of the plugin for the platform it runs on, using Unreal's `BuildPlugin`, then checks the result. Run it once on each platform you ship (Windows, macOS, Linux), with Unreal Engine 5.8 installed and any Python 3 (the engine's own works too):

```sh
python3 Scripts/package.py /absolute/new-output-folder
```

On Windows:

```bat
py Scripts\package.py C:\absolute\new-output-folder
```

The output folder must not exist yet. It receives `Plugin/` (the package), `BuildPlugin.log` and `PackageAudit.json`. The engine is found through `UE_ENGINE_ROOT`, the Epic Games Launcher's install record, or the default install folder; set `UE_ENGINE_ROOT` on Linux. Nothing contacts a Git remote. On macOS the package is built for Apple Silicon.

The check fails when the descriptor, the module library for this platform, required files (licence, README, changelog, this guide) or the engine `BuildId` don't match, or when development files (`.git`, `Saved`, Python caches) slipped in. To check an existing package without building one:

```sh
python3 Scripts/package.py --audit-only /path/to/Plugin --report /path/to/PackageAudit.json
```

To try a package, copy the **contents** of `Plugin/` into a disposable project's `Plugins/UEGitPlugin/`, enable **Git Workspace**, and run the `GitWorkspace` automation tests there.
