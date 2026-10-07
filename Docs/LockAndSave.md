# Git-native Lock and save

This development branch uses **Window > Git Workspace** for staging, commits, stashes and remote operations. Saving and lock acquisition are separate from staging. The inherited CheckIn, Sync, CheckOut, Revert and MarkForAdd operations are disabled.

## Save an asset

Use the asset editor's **Save** (including its keyboard shortcut), **Save All**, Content Browser **Save** for selected assets, or Git Workspace **Save assets…**.

For a game Content package with effective `filter=lfs` and `lockable` attributes:

1. Git Workspace verifies ownership with the selected **Lock remote**. That remote is shared with the save flow and stored in this project's editor preferences.
2. An unlocked asset opens **Lock assets before saving**, listing exact paths. **Lock and save** acquires those locks and continues the original save action. **Cancel** leaves unsaved edits in the editor. Cancel has initial keyboard focus. After the 60-second review expires, cancel and Save again.
3. An asset already locked by this checkout is verified and saved without another acquisition prompt.
4. A foreign lock, failed verification, changed repository/index/file state or a missing acquisition record cancels the save. A lock reported as yours but acquired by another clone is not adopted automatically.

Only freshly verified, locally recorded owned files are made writable. The package writer verifies ownership and saved bytes again before each protected write. **Make Writable** alone does not authorize saving. Saving retains locks and the existing staged versions. No commit, push or unlock occurs.

### First save of a new asset

Create and name the asset in game Content, then use one of the supported **Save** routes. The review marks its exact destination **First save**. **Lock and save** reserves that absent path with Git LFS and verifies this checkout's acquisition record before Unreal writes the package. No placeholder or staged file is created. After saving, the asset appears as untracked; use **Stage** when ready. Later saves verify and retain the same lock.

**Cancel** keeps the new asset and unsaved edits in memory. Failed lock verification or a destination that becomes occupied also leaves it unsaved. Ignored destinations, nested repositories, submodules, symlinks and deleted tracked files are refused. New assets with an owned lock from another clone are not adopted. If acquisition succeeded but the writer fails, the lock remains held and can be reused by a reviewed retry. Abandoned reservations still require separate handoff/recovery handling; no automatic unlock occurs.

If acquisition succeeds for some files but fails for another, no save permit is issued. The acquired locks remain held and are named in the failure report. Verify locks and retry; the next review lists only the remaining acquisitions. A native multi-package save can still fail partway through; it is not an atomic save transaction, and locks remain held.

## Current scope

This slice targets UE 5.8.3 Mac arm64. It covers ordinary existing `.uasset` and `.umap` files under the game's Content directory, saved untracked assets, and the first save of assets already named in game Content. New Blueprint and texture package writers are covered by the automated acceptance fixtures. **Save As** and unnamed maps that open a destination dialog need separate integration: an unreviewed new lockable destination is refused by the final writer guard. Non-lockable packages retain normal saving. First-save reservations were exercised with Git LFS 3.8; a client/server that refuses absent-path locking cancels the save without creating a placeholder.

World Partition/external actor and object saves are blocked pending a coordinated package-set workflow. Folder save/resave, Choose Files to Save, save-on-close and custom editor commands are not wrapped yet: their protected writes require a prepared save and are refused by the final guard. Use one of the supported Save routes first. Autosave copies under Saved and cooking retain their own engine paths. Other source-control providers and projects without a Git repository retain their normal save behavior.

A local editor lease excludes cooperating editors. External Git clients and remote force-unlocks are not held by that lease; verification occurs immediately before the native writer and cannot provide an atomic server lock plus disk-write transaction. Hosted two-user acceptance, Windows and large-project performance are still pending.
