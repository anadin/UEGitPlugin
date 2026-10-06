# Git-native Lock and save

This development branch uses **Window > Git Workspace** for staging, commits, stashes and remote operations. Saving and lock acquisition are separate from staging. The inherited CheckIn, Sync, CheckOut, Revert and MarkForAdd operations are disabled.

## Save an existing asset

Use the asset editor's **Save** (including its keyboard shortcut), **Save All**, Content Browser **Save** for selected assets, or Git Workspace **Save assets…**.

For an existing game Content package with effective `filter=lfs` and `lockable` attributes:

1. Git Workspace verifies ownership with the selected **Lock remote**. That remote is shared with the save flow and stored in this project's editor preferences.
2. An unlocked asset opens **Lock assets before saving**, listing exact paths. **Lock and save** acquires those locks and continues the original save action. **Cancel** leaves unsaved edits in the editor. Cancel has initial keyboard focus. After the 60-second review expires, cancel and Save again.
3. An asset already locked by this checkout is verified and saved without another acquisition prompt.
4. A foreign lock, failed verification, changed repository/index/file state or a missing acquisition record cancels the save. A lock reported as yours but acquired by another clone is not adopted automatically.

Only freshly verified, locally recorded owned files are made writable. The package writer verifies ownership and saved bytes again before each protected write. **Make Writable** alone does not authorize saving. Saving retains locks and the existing staged versions. No commit, push or unlock occurs.

If acquisition succeeds for some files but fails for another, no save permit is issued. The acquired locks remain held and are named in the failure report. Verify locks and retry; the next review lists only the remaining acquisitions. A native multi-package save can still fail partway through; it is not an atomic save transaction, and locks remain held.

## Current scope

This first slice is validated on UE 5.8.3 Mac arm64. It covers ordinary existing `.uasset` and `.umap` files under the game's Content directory, including saved untracked assets. A new asset's first save/Save As is not automatically locked; subsequent saves use this flow. Non-lockable packages retain normal saving.

World Partition/external actor and object saves are blocked pending a coordinated package-set workflow. Folder save/resave, Choose Files to Save, save-on-close and custom editor commands are not wrapped yet: their protected writes require a prepared save and are refused by the final guard. Use one of the supported Save routes first. Autosave copies under Saved and cooking retain their own engine paths. Other source-control providers and projects without a Git repository retain their normal save behavior.

A local editor lease excludes cooperating editors. External Git clients and remote force-unlocks are not held by that lease; verification occurs immediately before the native writer and cannot provide an atomic server lock plus disk-write transaction. Hosted two-user acceptance, Windows and large-project performance are still pending.
