# Changelog

## Unreleased

- `Scripts/package.py` builds and checks a plugin package on Windows, macOS or Linux, replacing the macOS-only packaging scripts.
- The tests' loopback Git LFS server runs on the engine's own Python first, so a Windows machine needs no separate Python install.
- Windows, ahead of its first build: fixes for compile errors found by review against the engine's Windows headers and Visual Studio's stricter name-hiding rules, and two tests rewritten to work there.
- Windows: helpers git leaves running on purpose, such as its file system monitor, are no longer stopped when a git command finishes.
- Paths handed back to the editor keep the editor's spelling when it differs from Git's only in letter case.
- Git panel: every file list (Changes, a commit's or stash's files, Locks) shows files in their folders. Single-folder chains share a row, World Partition actors sit under their level, and a folder's selection acts on its files. Alt+click (Option+click on macOS) or Shift+click on the arrow opens or closes a folder with everything in it; the folder button next to each search box switches back to one list of paths.
- Git panel: search in every tab (Ctrl+F): commits by summary, author or id, stashes by name, locks by file or owner, and a filter for a commit's or stash's files. Matches are highlighted.
- Git panel: a text file's changes show below its details in Changes, and below a commit's files in History. Diffs are colored (added green, removed red, the changed part of an edited line highlighted), numbered, and copyable; Compare opens the same view in a window. New text files show their content.
- Diffs can wrap long lines (Wrap lines, above each diff and in its right-click menu, saved in Editor Preferences); the changed part of a line stays highlighted across the wrap. A count of added and removed lines sits beside the switch.
- **Claim lock** takes over a lock you took in another clone, worktree or computer, so this copy can save and check in the file. It is in the Content Browser's asset menu, the Git panel's Changes tab (right-click, or the button next to the lock in the details) and its Locks tab, which also counts such locks. Nothing is sent to the server. These locks now read "Locked by you elsewhere" / "You, elsewhere", and Lock in the Changes tab only appears for unlocked files.
- Git panel: Show in Git Panel selects only the files asked for; it used to add them to whatever was already selected.
- Git panel: Show in Content Browser only appears for assets and folders inside a content folder (a World Partition actor shows its level, a deleted asset its folder); it no longer opens Finder or Explorer for other files. **Show in Finder** / **Show in Explorer** is a separate entry for any file or folder, in every list's right-click menu and as a button in the Changes details.

## 4.0.0-alpha

A rework of the Git LFS 2 provider around Unreal's own revision control flows. Tested on Unreal Engine 5.8 on macOS.

### Unreal's flows, backed by Git

- **Check Out** takes a Git LFS lock, or takes over a lock you hold in another clone. Saving never contacts the server.
- **Submit Content (Check in)** commits exactly the listed files, uploads Git LFS files, pushes (publishing new branches), and releases the locks unless *Keep files checked out* is ticked. A rejected push keeps the commit and the locks.
- **Sync** fetches, refuses before changing anything when local edits overlap incoming files or unpushed commits would conflict, downloads Git LFS files first, then fast-forwards or rebases. Only the changed packages reload. Changes to code, config or plugins come with a rebuild-and-restart notice.
- **Revert** backs up what it overwrites (pruned automatically) and releases locks no longer needed. Add, Copy and Delete no longer stage anything; new files are committed by Check in.
- Content Browser states show lock owners, locks held in another clone, newer versions on the server, and edits made without a lock.

### Git panel and status bar

- A new Git panel (Window > Git) with Changes, History, Stashes and Locks tabs, branch switching and creation, Fetch / Sync / Push, and a layout that adapts to narrow docks. It replaces the previous workspace window.
- Status bar buttons: the current branch, Sync *n*, Check in *n* and Push *n*.
- Branch switches and stash restores reload affected assets and refuse to overwrite local edits.

### Under the hood

- A cross-platform process runner (no more 60-second kill of long Git LFS transfers; separate time limits for local work, background checks and user-started network commands; cancellation).
- One repository session per worktree with background lanes, immutable snapshots and coalesced refreshes instead of polling on the game thread.
- Lock records that survive branch switches and config edits.
- Git 2.38+ and the system Git LFS are required; the bundled git-lfs binaries were removed.

### Removed

- The custom "Lock and save" flow, the save guard and startup lockout, guarded move/rename, the World Partition HLOD and map-copy tooling, and the separate session module. Unreal's own Check Out, Save and asset tools take their place.
