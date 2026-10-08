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

### Save As an asset copy

In the standard Blueprint, Texture2D, Material, Material Instance or Material Function editor, use **File > Save As**. **Save asset copy as** chooses a new name under game Content. The destination must be absent on disk and in memory. Then **Lock assets before saving** reviews its exact path; **Lock and save** reserves it before creating the copy or writing a file. Cancel either dialog to leave the original untouched, without creating a copy or acquiring a lock.

The copy includes the current edited asset data using Unreal's normal duplication rules (Blueprint Description/Display Name are DuplicateTransient and reset on the copy). Its file is untracked and its lock remains held. The original editor stays open with its unsaved edits; its saved file, staging and existing lock are preserved. Save As does not rename the original, overwrite another asset, save the original, or release its lock. If the copy writer fails after duplication, the unsaved copy stays in memory with its destination lock held; open that copy and use **Save** to retry. An abandoned copy/reservation still needs separate recovery or handoff handling.

Materials copy the current editor preview, including unapplied graph and property edits, into an ordinary Material asset. Save As does not press Apply or change the original material's use in the world; its original editor retains the preview and Apply state. The copy's expressions belong to the new material, and editor-only preview state is not published. Known preview shader errors refuse the copy; errors found while compiling the new copy retain it in memory with its lock for repair and ordinary Save. Regular Material **Save** retains Unreal's original apply/compile/save action.

Material Instance copies retain the current source's scalar/vector/texture/static overrides, base-property overrides and parent reference. Unreal applies ordinary instance edits directly to that source in memory; its pending edits remain dirty and its saved bytes are not changed by Save As. The rendering preview is not substituted for the source.

Ordinary Material Functions copy their unapplied graph and metadata from the standard Material editor's transient function preview. Graph additions, deletions and links synchronize after lock consent. The new function owns its expressions, preserves the edited library-exposure flag and has an independent state identity. Save As retains the original function's saved data and editor Apply state. Regular function Save still runs Unreal's Apply and save action. Known wrapper-preview shader errors refuse the copy; this adapter does not certify every caller or shader configuration.

These adapters preserve command enable/visibility/check delegates and restore the original action on shutdown. Material Function Instances, Material Layers/Layer Blends, other asset classes and multi-asset editors require separate integration. Exact ordinary asset classes are accepted; derived/custom classes are not. Other providers and non-Git projects keep the inherited Save As behavior.

### Name or copy an ordinary map

In the Level Editor, use **Save Current Level** (Ctrl+S) on an unnamed ordinary map. **Name and save current map** chooses a fresh name under game Content. The lock review includes the `.umap` and, when present, its separate `_BuiltData.uasset`. **Lock and save** reserves and verifies every destination before naming or writing anything. The current world receives the chosen name and stays open. Later Save resolves that named package directly.

For a named map, **Save Current Level As…** (Ctrl+Alt+S) opens **Save map copy as**. This copies current unsaved map edits and its build data to independent packages. UE 5.8 resets the level's non-PIE build-data reference during duplication, so the adapter explicitly duplicates the exact reviewed modern registry when needed. The original stays open with its unsaved edits; open the saved copy from Content Browser when you want to switch maps. The review explicitly describes this behavior. Neither route stages files or releases locks.

Cancel either dialog to preserve the current map without a copy or lock acquisition. Existing map/build-data packages in memory or on disk, and orphan external-package folders at the destination, are refused. The source's build-data relationship and complete destination set are checked again after preparation. Ordinary Save/Save All also review a map's separate build-data package.

Writes are not atomic across files. Build data is written first; if it fails, the map is not written. If the map writer then fails, the completed build-data file, unsaved named map/copy and destination locks remain. Use Save on the current named map to retry, or **Save All** / Git Workspace **Save assets…** to review all unsaved packages and retry an inactive copy. A failed copy without a map file cannot be opened from disk yet. Save All also includes other unsaved edits; review them first. A partial lock-acquisition failure writes and names nothing and reports the locks retained.

This adapter accepts ordinary persistent maps with no streaming levels, World Composition, HLOD proxy actors or external packages. Build data must be the ordinary registry in that map's own `_BuiltData` package. Legacy, shared/custom build data, sublevels and World Partition/OFPA naming remain pending. Name an ordinary temporary map using Save Current Level before Save All; Save All refuses unreviewed map naming.

### Save actors in an existing World Partition or OFPA map

Use **Save Current Level** (Ctrl+S), **Save All**, or Git Workspace **Save assets…** in an already saved map. The review names changed actors and their exact files, together with the dirty map and its modern build-data registry when needed. Clean files stay unlocked and unchanged. Build data and actors save before the map.

New actors appear as **New actor** and **First save** in the review. **Lock and save** reserves their absent files before writing anything. **Cancel** keeps the actors in the editor and creates no file or lock. Saved actor files are untracked until you stage them; later saves verify and retain their existing locks. An occupied destination or a missing existing actor file is refused rather than overwritten or treated as new.

If some lock reservations fail, no package is written and acquired locks remain held. A failed write reports completed and remaining files; completed files stay on disk, unfinished actors/maps stay unsaved, and every acquired lock remains held. Use Save again to review the remaining packages and reuse their verified reservations. Nothing is staged, committed, pushed or unlocked automatically. Only explicitly reviewed deleted actor files can be removed. Callback changes to the reviewed package set stop the save rather than acquiring more locks silently.

This workflow supports one persistent editor map and its main external actors. Never-saved empty actor packages, actor conversion/move cleanup, external objects, external data layers, nested containers/level instances, HLOD, streaming/sublevels and World Partition/OFPA first map naming or Save As require separate adapters. The map must already exist in game Content. Modern build data must belong to that map's own `_BuiltData` registry; a new companion is reviewed with its existing map.

## Current scope

This slice targets UE 5.8.3 Mac arm64. It covers ordinary existing `.uasset` and `.umap` files under the game's Content directory, saved untracked assets, the first save of assets already named in game Content, standard Blueprint/Texture2D/Material/Material Instance/Material Function Save As copies, and ordinary map naming/copies with coordinated build data into new LFS/lockable destinations. Other Save As/naming routes need separate integration: an unreviewed new lockable destination is refused by the final writer guard. Non-lockable packages retain normal saving through the existing Save routes; this Save As adapter requires a lockable destination. First-save reservations were exercised with Git LFS 3.8; a client/server that refuses absent-path locking cancels the save without creating a placeholder.

Existing World Partition/OFPA maps, modern build data, existing/new main external actors and reviewed saved-actor deletions use the coordinated workflow above; external objects remain blocked. Folder save/resave, Choose Files to Save, save-on-close and custom editor commands are not wrapped yet: their protected writes require a prepared save and are refused by the final guard. Use one of the supported Save routes first. The package-save guard cannot veto ObjectTools file cleanup. Unwrapped native routes that clean empty packages (including native save-on-close or custom FileHelpers calls) are outside this deletion adapter and can remove files without this review/recovery step. Use the wrapped Save routes for pending actor deletions. Covering those cleanup entry points remains required before claiming universal deletion protection. Autosave copies under Saved and cooking retain their own engine paths. Other source-control providers and projects without a Git repository retain their normal save behavior.

### Save a deleted external actor

Delete a previously saved main actor in the editor, then use Save Current Level or Save All. **Review actor deletions and saves** shows **DELETE actor**, its saved label, owning map and exact file. **Save and delete reviewed actors** requires confirmation even if you already own every lock. **Cancel** leaves the file on disk and the deletion pending in the editor; use Unreal Undo if you want to put the unsaved actor back.

Before removing any file, Git Workspace verifies its server lock and this checkout's acquisition record, exact empty package/map binding, saved actor descriptor, hydrated bytes and unchanged staging. It binds saved metadata synchronously before the core save scope. Live conversion/move targets, missing files, ambiguous metadata, pointers, symlinks and hardlinked actor files are refused. Working files and Git recovery storage must be on the same volume for the verified move. Deletion does not run Unreal's normal ObjectTools cleanup or its source-control revert/delete routes.

A durable backup under the checkout's Git directory, `uegit/actor-delete/<id>`, retains `payload.uasset`, the original file moved as `removed.uasset`, and `manifest.json` with the exact path, actor GUID/label, mode, checksum and restore instructions. Deletions finish before other actor/build-data writes and the map. Locks and staged versions remain unchanged. A partial save lists **Deleted with recovery**, **Saved** and **Not completed**, plus each backup folder; repeat Save for the unfinished files.

After success, a recovery dialog shows the backup location. To recover the **saved working version**, close the editor without saving, inspect the manifest, confirm the destination is absent and no newer work needs keeping, and copy `payload.uasset` to its listed repository path. Apply the recorded mode if needed, then reopen the map. This does not recover edits that were never saved; staging remains a separate version. No automatic restore or backup cleanup occurs. Undo after a completed deletion cannot overwrite/recreate an indexed path through First save: restore the saved file with the editor closed first.

An incomplete move, registry completion or changed recovery identity leaves an active recovery marker. The editor stays open to retain other unsaved edits, while package writes and workspace mutations are blocked through the guarded routes. Project loading also stops on the next startup until that report is resolved. Preserve other unsaved work before closing. Keep both recovery files and any competing destination; do not overwrite newer work. `complete.json` marks a verified completed deletion, whose backup remains available without blocking normal editor startup.

A local editor lease excludes cooperating editors. External Git clients and remote force-unlocks are not held by that lease; verification occurs immediately before the native writer and cannot provide an atomic server lock plus disk-write transaction. Hosted two-user acceptance, Windows and large-project performance are still pending.

For an incomplete deletion, after verifying the chosen disk result and preserved staging with the editor closed, copy that backup's `manifest.json` to `complete.json` to record resolution. Remove `recovery-required.txt` only if it still contains this exact backup ID. Keep the payload, moved file and manifest. Removing the shared marker alone does not bypass an unfinished backup; its folder also blocks workspace mutations. A dedicated in-editor recovery browser remains future work.
