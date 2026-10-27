# Git Workspace

Git and Git LFS for Unreal Editor, working the way Unreal's revision control already works:

- **Check out** an asset and Git Workspace takes its Git LFS lock, so nobody else edits it at the same time.
- **Check in** (Submit Content) commits the files you choose, pushes them and releases their locks.
- **Sync** brings in your team's commits and reloads the assets that changed.
- **Revert** throws away your changes (keeping a backup) and gives the lock back.

Artists never need to leave those flows. For everything else there is the **Git panel**: staging, commits, history, branches, stashes and locks.

> **Status: 4.0.0-alpha.** Built and tested on Unreal Engine 5.8 on macOS (Apple Silicon). Windows and Linux share the same code but have not been built or tested yet.

## Requirements

- Unreal Engine 5.8.
- [Git](https://git-scm.com/) 2.38 or newer. Git Workspace checks the version when it connects.
- [Git LFS](https://git-lfs.com/) 3.x installed and set up once with `git lfs install`. Git Workspace uses the Git LFS that comes with your Git installation; it does not ship its own.
- For locking: a Git LFS server with the [locking API](https://github.com/git-lfs/git-lfs/blob/main/docs/api/locking.md), such as GitHub, GitLab, Gitea or Bitbucket.
- Credentials that work without prompting in a terminal (a credential manager, or SSH keys). Background checks never ask for a password; actions you start yourself, such as Push, may show your credential manager.

## Install

1. Put the plugin in your project's `Plugins/UEGitPlugin` folder (for example as a Git submodule) and build the editor.
2. Enable **Git Workspace** in **Edit > Plugins** and restart the editor.
3. In the status bar, open **Revision Control > Connect to Revision Control**, choose **Git Workspace** and accept. Git is found automatically; set its path in the same dialog if needed.

Projects that used the older **Git LFS 2** provider switch to Git Workspace automatically.

## Set up the project

Track Unreal packages with Git LFS and mark them `lockable` in `.gitattributes`:

```gitattributes
*.uasset filter=lfs diff=lfs merge=lfs -text lockable
*.umap filter=lfs diff=lfs merge=lfs -text lockable
```

Ignore generated folders in `.gitignore` (`Binaries/`, `DerivedDataCache/`, `Intermediate/`, `Saved/`).

`lockable` is what turns on Check Out: files without it stay freely editable. When the project uses locks, Git Workspace offers once to turn on **Editor Preferences > Loading & Saving > Prompt for checkout on asset modification**, so Unreal asks to check out an asset the moment you start editing it. Turn it on for everyone on the team.

## Everyday use

The status bar shows what needs doing, next to Unreal's own revision control status:

| Button | What it does |
| --- | --- |
| **main** (your branch) | Opens the Git panel. |
| **Sync 3** | Gets 3 new commits. Unreal saves your work, updates the files and reloads the assets that changed. |
| **Check in 2** | Opens Unreal's Submit Content dialog for your 2 changed assets. |
| **Push 1** | Publishes a commit you made earlier (for example from the Git panel). |

**Editing.** When you start editing a lockable asset, Unreal asks to check it out; Git Workspace takes the lock. If someone else holds it, the Content Browser shows who, and Check Out is refused. Saving never talks to the server: when you save an asset you have not checked out, Unreal's own dialog offers Check Out or Make Writable.

**Checking in.** Check in commits exactly the files you leave ticked in the Submit dialog, using your description as the commit message, then pushes and releases their locks. Tick **Keep files checked out** to keep the locks. New assets are added when you check them in. If a teammate pushed first, your commit is kept and Git Workspace tells you to Sync and push again; your locks stay.

**Syncing.** Sync fetches, then fast-forwards, or rebases your unpushed commits on top of the new ones. It stops before changing anything when you have local edits to files that also changed on the server, or when your unpushed commits would conflict. When C++ code, config or plugins changed, it tells you to rebuild and restart.

**Reverting.** Revert restores the last committed version, keeps a copy of what it overwrote in `Saved/GitSourceControl/Backups` and releases the lock unless a commit you have not pushed still needs it.

**Badges.** The Content Browser shows checked-out (locked by you), locked by someone else, locked by you in another clone, newer version on the server, new, modified and deleted files.

## The Git panel

Open it with **Window > Git**, the branch button in the status bar, **Revision Control > Git Panel**, or **Show in Git Panel** on assets in the Content Browser.

- **Toolbar**: switch, create or publish branches; see how far ahead or behind you are and when the server was last checked; **Fetch**, **Sync** and **Push**. Running Git commands show here with a Cancel button, and failures show in a banner with a link to the log.
- **Changes**: your changes grouped into **Staged** and **Changes** (plus **Conflicts** when there are any), with asset and actor names, who holds each lock, and details for the selected file; a text file's changes show below its details. Stage what you want to commit, describe it, and choose **Commit** or **Commit and push**. Right-click for Compare, History, Show in Content Browser, Lock, Release lock and Discard changes.
- **History**: commits on your branch (unpushed ones are marked), searchable by summary, author or commit id; select one to see its files, and a text file to see how the commit changed it.
- **Stashes**: put your changes aside (optionally with new files), then restore, apply or delete stashes. Search them by name.
- **Locks**: every lock on the server, yours first, searchable by file or owner. Release your own locks, or someone else's when they cannot.

Every file list shows files in their folders. Folders that only hold one folder share a row (`Source/Demo`), and World Partition actors sit under their level, without the hash folders. Selecting a folder selects its files: Stage, Discard, Lock and Release act on all of them. Alt+click a folder (Option+click on macOS), or Shift+click its arrow, to open or close everything in it. The folder button next to each search box switches every list between folders and one list of paths.

Each tab has a search box (Ctrl+F); matches are highlighted and their folders open. The files of a commit or stash have their own filter.

Text changes are colored: added lines green, removed lines red, and in an edited line the part that changed is highlighted. Each line shows its old and new line number; select lines and press Ctrl+C to copy them. **Compare** (Ctrl+D) opens the same view in its own window. For Unreal assets, Compare opens Unreal's own diff instead: Blueprints (including Animation and Widget Blueprints), materials, data tables, curves, behavior trees and other types with a diff view show their own comparison, and other assets open as text in the **Tool for diffing text** set under Editor Preferences > Loading & Saving. New and deleted assets, and World Partition actors on their own, cannot be compared.

Switching branches and restoring stashes refuse to overwrite local edits, download Git LFS files first, and reload any open assets that change.

| Key | Action |
| --- | --- |
| Ctrl+F | Search the current tab |
| Space | Stage or unstage the selected files |
| Enter / double-click | Compare an edited file, or open a new one; open or close a folder |
| Left / Right | Close or open a folder (with Shift: everything in it) |
| Ctrl+D | Compare with the last commit, in its own window |
| Ctrl+C | Copy the selected lines of a diff |
| Delete | Discard changes to the selected files |
| Ctrl+Enter | Commit (Ctrl+Shift+Enter: commit and push) |
| F5 | Refresh |
| Ctrl+1 … Ctrl+4 | Changes, History, Stashes, Locks |

On macOS, use Cmd instead of Ctrl.

## Settings

**Revision Control > Change Revision Control Settings** (per project):

- **Git executable**: leave empty to find it automatically.
- **Use Git LFS locks**: turn Check Out locking on or off for this project.
- **Lock server remote**: the remote whose LFS server holds locks; empty uses the branch's upstream remote (usually `origin`).

**Editor Preferences > Revision Control > Git Workspace** (per user): how often status, locks and the server are checked (background fetching can be turned off), whether Check in pushes, how many revert backups to keep, and whether the Git panel shows folders.

## Troubleshooting

- **"… is locked by …"**: someone else holds the lock. Ask them to check in or release it; the Git panel's Locks tab can force-release it if they cannot.
- **"Locked by you in another clone"**: you hold the lock from another copy of the project. Check Out takes it over for this copy.
- **"The server has newer commits"**: your commit was kept locally. Sync, then Push.
- **"Nothing was changed: you have local changes …"** during Sync or a branch switch: check in, stash or revert those files first.
- **Fetch or push fails with an authentication error**: make sure `git fetch` works in a terminal for this repository without prompting. Background checks never prompt; they back off and retry.
- **"Another git process seems to be running"** or a stale `index.lock`: close other Git tools working on this repository. If none are running, delete `.git/index.lock`.
- **Offline**: editing and saving keep working. Lock owners shown are the last known ones, with their age; Check Out and Sync fail with a clear message until the server is back.
- **Git LFS not installed**: install Git LFS and run `git lfs install`. The revision control status tooltip shows the Git and Git LFS versions in use.

Detailed messages are in **Window > Message Log > Revision Control** (or via **Show log** in notifications).

## Known limitations

- Tested on macOS only so far.
- Conflicts are not resolved inside Unreal: use a Git client, then mark files resolved.
- No partial (line-by-line) staging, cherry-pick, interactive rebase or history graph.
- Syncing to a specific revision is not supported; use branches instead.
- Submodules show up as changes but are not managed.

## Development

The plugin's automation tests are named `GitWorkspace.*`. They create disposable repositories (and a loopback Git LFS server, which runs on the engine's own Python) and never touch your project's repository. Run them from the Session Frontend, or headless:

```bash
UnrealEditor-Cmd YourProject.uproject -unattended -nullrhi -ExecCmds="Automation RunTests GitWorkspace" -TestExit="Automation Test Queue Empty"
```

`GitWorkspace.Panel.Screenshots` only runs in an editor that renders; it saves images of the Git panel to `Saved/Automation/GitWorkspace/Screenshots`. `GitWorkspace.Editor.NativeFlows` drives Unreal's own check out, check in, revert and sync helpers; it only runs in a disposable project with its own repository and lock server (the development project's `Scripts/dev.sh e2e`, or `Scripts\dev.cmd e2e` on Windows, prepares one).

To build a plugin package for your platform (Windows, macOS or Linux), see [Packaging](Docs/Packaging.md).

## Credits and licence

Git Workspace continues the work of:

- [Sébastien Rombauts](https://github.com/SRombauts/UE4GitPlugin), the original Unreal Git integration.
- [Project Borealis](https://github.com/ProjectBorealis/UEGitPlugin), the Git LFS locking provider.
- [Mighty Games Group](https://github.com/MightyGamesGroup/UEGitPlugin), whose Unreal Engine 5.8 fork this version started from.

MIT licence; see [LICENSE.txt](LICENSE.txt). Please report issues on [GitHub](https://github.com/anadin/UEGitPlugin/issues).
