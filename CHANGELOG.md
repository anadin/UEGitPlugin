# Changelog

## Unreleased

- `Scripts/package.py` builds and checks a plugin package on Windows, macOS or Linux, replacing the macOS-only packaging scripts.
- The tests' loopback Git LFS server runs on the engine's own Python first, so a Windows machine needs no separate Python install.
- Windows: fixes for compile errors found by review against the engine's Windows headers and Visual Studio's stricter name-hiding rules, and by the first Windows build; two tests rewritten to work there.
- Windows: helpers git leaves running on purpose, such as its file system monitor, are no longer stopped when a git command finishes.
- Paths handed back to the editor keep the editor's spelling when it differs from Git's only in letter case.
- Git panel: every file list (Changes, a commit's or stash's files, Locks) shows files in their folders. Single-folder chains share a row, World Partition actors sit under their level, and a folder's selection acts on its files. Alt+click (Option+click on macOS) or Shift+click on the arrow opens or closes a folder with everything in it; the folder button next to each search box switches back to one list of paths.
- Git panel: search in every tab (Ctrl+F): commits by summary, author or id, stashes by name, locks by file or owner, and a filter for a commit's or stash's files. Matches are highlighted.
- Git panel: a text file's changes show below its details in Changes, and below a commit's files in History. Diffs are colored (added green, removed red, the changed part of an edited line highlighted), numbered, and copyable; Compare opens the same view in a window. New text files show their content.
- Diffs can wrap long lines (Wrap lines, above each diff and in its right-click menu, saved in Editor Preferences); the changed part of a line stays highlighted across the wrap. A count of added and removed lines sits beside the switch.
- **Claim lock** takes over a lock you took in another clone, worktree or computer, so this copy can save and check in the file. It is in the Content Browser's asset menu, the Git panel's Changes tab (right-click, or the button next to the lock in the details) and its Locks tab, which also counts such locks. Nothing is sent to the server. These locks now read "Locked by you elsewhere" / "You, elsewhere", and Lock in the Changes tab only appears for unlocked files.
- Git panel: Show in Git Panel selects only the files asked for; it used to add them to whatever was already selected.
- Git panel: Show in Content Browser only appears for assets and folders inside a content folder (a World Partition actor shows its level, a deleted asset its folder); it no longer opens Finder or Explorer for other files. **Show in Finder** / **Show in Explorer** is a separate entry for any file or folder, in every list's right-click menu and as a button in the Changes details.
- Git panel: Changes and Staged are separate lists, Staged below Changes (Conflicts above them when there are any), told apart by colored title bars with icons. The Staged title bar is a divider: drag it to share the height between the lists, and the split is remembered. Each title bar has a Stage or Unstage button for the files selected in its list, or for everything it lists when none is selected; selecting in one list clears the other.
- Git panel: one **Commit** button instead of Commit and Commit and push. Hold Alt (Option on macOS) over it to make it **Commit and push**, so pushing is never one stray click away. Ctrl+Shift+Enter still commits and pushes.
- Sync is now **Pull**, the Git term, in the Git panel's toolbar, the status bar and the messages that point to it. Unreal's own menu entries keep their names.
- Git panel: the hint below the commit message wraps to the panel's width; it could stay as narrow as a shorter hint shown before it.
- Git panel: Ctrl+S (Cmd+S on macOS) stages or unstages the selected files, like Space. While you type a search or a commit message it is still the editor's Save.
- Git panel: the notification after a commit no longer offers Push. It appears in the bottom-right corner of the screen, where a second click on Commit could land on it.
- Pushes no longer send a branch's commits to an upstream with another name, such as `origin/main` for a feature branch made from it: like `git push` (unless `push.default` is `upstream`), Git Workspace publishes the branch under its own name instead. A branch that follows a local branch is no longer pushed into that branch.
- Every push that would create a branch on the server, or change the branch yours follows, asks first: Push in the Git panel and the status bar, Commit and push, and Check in (where no checks in nothing). **Publish** in the Git panel is its own confirmation.
- Git panel: **Pull** opens a dialog, like Fork's: the remote and branch to pull (the upstream unless you pick another, so another branch can be merged or rebased into yours), the branch it goes into, **Rebase instead of merge** and **Stash and reapply local changes**. Both choices are remembered (also in Editor Preferences) and used by the status bar's Pull and Unreal's Sync, which still pull without asking.
- Pull keeps a local edit to a file that also changed on the server when the two can be combined, instead of stopping: the edit is stashed and put back on top, staged if it was. It still stops before changing anything when they cannot be combined (assets, the same lines), and a rebase never rewrites commits that are already on the server.
- Git panel: tooltips and hints name shortcuts as Unreal does on each platform: Cmd+S and Option+click on macOS, Ctrl+S and Alt+click elsewhere.
- Git panel: stage, unstage and discard single lines of a text file. Select lines in its diff (drag over them, Shift+click, or click a hunk's header for the whole hunk) and **Stage lines** and **Discard lines…** appear beside them in Changes, **Unstage lines** in Staged; the right-click menu has them too, and Space, Ctrl+S (Cmd+S on macOS) and Delete work on the selected lines while the diff has focus. An edit staged on its own keeps its place among the lines around it. Discarding asks first and keeps a copy of the file in Saved/GitSourceControl/Backups. New files work too; binary files, files stored through Git LFS or another filter, conflicts, submodules and text that is not UTF-8 are staged whole.
- Git panel: a refresh no longer clears the selection in a diff, or scrolls it back to the top, when the diff did not change; after a change to the same file the diff keeps its place.
- Tests compare text exactly. Unreal's TestEqual and FString comparisons ignore letter case, so a check could pass with a letter in the wrong case.
- Push and Check in no longer refuse because of locks on files that a merge or rebase brought into your branch. After merging main into a branch, every push failed with "… is locked by someone else" when a teammate still held a lock on a file main had changed, although other Git clients pushed fine. Only the files your unpushed commits change count now (a merge counts only for what it changed from both sides), as with Git LFS's own check; deleting or renaming someone else's locked file still counts.
- Git panel: **Lean branching**, as in Fork. When `main`, `master` or `develop` follows a branch on the server, the branch menu offers **Start branch on main…** (a branch from the newer of `main` and `origin/main`), **Sync (rebase on origin/main)** (rebases the branch through Unreal's Sync, moving its copy on the server along with a push with lease and leaving unpushed commits unpushed; on main, a Pull with rebase) and **Finish (merge into main)…** (one commit moves main forward, more get a merge commit; no file changes and nothing is pushed). Sync and Finish stop before changing anything when someone else pushed to the branch, its commits are on another server branch, it is behind `origin/main`, or main has unpushed commits.
- Git panel: right-click menus show each entry's keyboard shortcut, as Unreal's own menus do. New shortcuts fill the gaps: Ctrl+C (Cmd+C on macOS) copies the selected paths in every file list and the selected commit's id in History; in History's file list Ctrl+D and Enter compare, as in Changes; Delete deletes the selected stash, after asking.
- Locking and releasing several files show how far they have got. The Git panel's toolbar counts the files ("Locking files: 3 of 12") and Cancel stops after the file in hand; Lock in the panel is now its running action. From a few seconds on, Unreal's status bar shows a progress bar too, also for Check Out from the Content Browser, and for the locks Delete takes and Revert and Check in release.
- Commit messages: the first line is the summary. A line typed right below it used to run into the summary, because Git takes the whole first paragraph; Git Workspace now puts the blank line Git needs between them, in the Git panel and in Submit Content. History shows the first line as the summary for older commits too, and lines starting with # (such as "#42 Fix the hero") are no longer dropped from messages.
- Git panel: History shows where you are. Your branch and its server copy (such as `origin/main`) are labelled on the commits they point to, side by side when they match; commits on the server that are not in your branch yet are listed too, marked ↓, next to your unpushed ones, marked ↑. Uncommitted changes are a row above your branch's commit with a dot (orange while some are not staged, green when all are) and a count; double-click it or press Enter to open the Changes tab. Dates read "Today at 9:15 am", "Yesterday at 2:30 pm" and "3 Nov at 1:15 pm" (with the year when it is not this one), in local time.
- Git panel: committing with Ctrl+Enter (Cmd+Enter on macOS), or Ctrl+Shift+Enter to push as well, now empties the message box as clicking Commit does; the message stayed in the box while it had the keyboard. A message typed while the commit runs is kept for the next one.
- Git panel: History draws the branch graph, as other Git clients do: a line per line of history and a dot per commit, curves where branches split off and merge, your branch's line in blue and its server copy's in purple like their labels, and the uncommitted changes as a ring on your branch's line. Commits are listed with each above its parents (git log --date-order), even when the clocks of the computers that made them disagree. A search hides the graph.

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
