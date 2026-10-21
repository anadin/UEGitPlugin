# Changelog

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
