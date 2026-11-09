# Git Workspace manual

How to work with Git Workspace in Unreal Editor: the everyday loop first, then the Git panel for when you need more. Setting a project up is in the [README](../README.md#install).

Keys are written as on Windows and Linux. On macOS, use Cmd for Ctrl and Option for Alt.

## The everyday loop

Most days you only need the buttons in Unreal's status bar, at the bottom of the editor:

| Button | Shows when | What it does |
| --- | --- | --- |
| **main** | Always: the branch you are on | Opens the Git panel. |
| **Pull 3** | Teammates pushed 3 commits | Saves your work, gets the commits and reloads the assets they changed. |
| **Check in 2** | You changed 2 files | Opens Unreal's Submit Content dialog. |
| **Push 1** | You have a commit the server does not | Sends it to the server. |

Git Workspace checks the server every five minutes, and when you come back to the editor.

### 1. Get the latest

Click **Pull** before you start, and whenever it shows a number. Unreal saves your work first, and the assets that changed reload by themselves. When C++ code, config or plugins changed, you are told to rebuild and restart.

Pull stops before changing anything when it cannot combine your work with your team's: an asset you edited that also changed on the server, or the same lines of a text file. Check in, stash or revert that file, then Pull again.

### 2. Edit

Start editing an asset and Unreal asks to **check it out**. Say yes: Git Workspace takes the asset's lock, so nobody else can change it until you check it in. When someone else holds the lock, Check Out is refused and the Content Browser shows who has it.

To lock assets before you start, right-click them, or their folder, in the Content Browser and choose **Lock**.

Save as often as you like. Saving never talks to the server.

### 3. Check in

Click **Check in** in the status bar. In the Submit Content dialog:

1. Tick the files to check in.
2. Describe the change. The first line is the summary people see in History; put details on the lines below.
3. Click **Submit**.

Git Workspace commits the ticked files, pushes them and releases their locks. Tick **Keep files checked out** to go on working on them.

If a teammate pushed first, your commit is kept on your computer and your locks stay: **Pull**, then **Push**.

## Undo

- **An asset**: right-click it in the Content Browser and choose **Revision Control > Revert**. It goes back to the last version checked in, and its lock is released.
- **Any file**: in the Git panel's Changes tab, select it and press Delete (**Discard changes**).
- **Some lines of a text file**: select them in the file's diff in the Changes tab and choose **Discard lines…**.

Each asks first, and keeps a copy of what it throws away in `Saved/GitSourceControl/Backups`.

## Locks

- **Who has what**: the Content Browser shows each lock's owner. The Git panel's **Locks** tab lists every lock, yours first.
- **Give a lock back** without checking in: right-click the asset, or its folder, in the Content Browser and choose **Unlock**, or select the lock in the Locks tab and click **Release**. Your changes stay; lock the asset again before you check it in.
- **Locked by you elsewhere**: you took the lock in another copy of the project, such as another clone or computer. Right-click the asset and choose **Claim Lock** to use it in this copy, and stop working on it in the other one.
- **Someone has gone home with a lock**: ask them first. If they cannot release it, right-click it in the Locks tab and choose **Release someone else's lock…**.

## The Git panel

Open it with the branch button in the status bar, or **Window > Git**. Its toolbar has the branch menu, how far you are ahead of or behind the server, and **Fetch** (check the server without changing anything), **Pull** and **Push**. The **⋮** menu has Check in, Refresh, Check locks, the settings and the log.

### Changes

Your edits, in two lists: **Changes**, not staged yet, and **Staged**, what the next commit contains.

- Select files and press Space to move them from one list to the other. To stage part of a text file, select lines in its diff and click **Stage lines**.
- Write a message and click **Commit**, or press Ctrl+Enter. To push as well, hold Alt over the button so it says **Commit and push**, or press Ctrl+Shift+Enter.
- Ctrl+D compares a file with its last committed version; assets open in Unreal's own diff.
- Right-click for Show in Content Browser, Show in Explorer (Finder on macOS), History, Lock, Unlock and more.

Check in from the status bar does all of this in one step for the files you tick: commit, push, and release the locks.

### History

The commits on your branch and on its copy on the server, newest first, with a graph of how they connect.

- The blue label is where your branch is, the purple one where the server's copy is. When they sit side by side, you are in step.
- ↑ marks a commit you have not pushed yet, ↓ one on the server that you do not have yet (Pull gets it).
- **Uncommitted changes**, just above your branch's newest commit, are your edits. Double-click them to see them in the Changes tab.
- Select a commit to see its files, and a text file to see what changed in it.

### Stashes

Put changes aside without checking them in, for example to Pull or switch branches with nothing in the way. Click **Stash changes…** and name the stash. Later, **Restore** brings the changes back and deletes the stash; **Apply** brings them back and keeps it.

### Locks

Every lock on the server; see [Locks](#locks) above.

## Branches

Work on a branch when it should not reach everyone until it is finished. The branch menu is the branch button at the left of the Git panel's toolbar:

- **New branch…** starts a branch where you are and switches to it. Your uncommitted changes come along. A space typed in the name becomes a dash.
- **Publish branch** puts it on the server so others can get it.
- Pick a branch under **Branches** to switch to it, or under **On the server** to get a teammate's.

When your `main` (or `master`, or `develop`) is on the server, the menu's **Lean branching** section runs a feature branch in three steps:

1. **Start branch on main…** creates a branch from the newest main.
2. **Sync** brings your team's new work on main into your branch. Do it often.
3. **Finish (merge into main)…** puts your branch's commits on main and switches to main. Then **Push**.

## When something goes wrong

| Message | What to do |
| --- | --- |
| … is locked by … | Someone else holds the lock. Ask them to check in or release it. |
| Locked by you elsewhere | Claim the lock (see [Locks](#locks)). |
| The server has newer commits | Your commit is safe on your computer. Pull, then Push. |
| Nothing was changed: you have local changes … | Check in, stash or revert those files, then try again. |
| An authentication error on Fetch or Push | Make sure `git fetch` works for the project in a terminal without asking for a password. |

Every message, with its details, is in **Window > Message Log > Revision Control**, or **Show log** in the Git panel's **⋮** menu. More in the README's [troubleshooting](../README.md#troubleshooting).

## Keys in the Git panel

| Key | Does |
| --- | --- |
| Ctrl+F | Search the current tab |
| Space | Stage or unstage the selected files or lines |
| Ctrl+Enter | Commit (with Shift, commit and push) |
| Ctrl+D | Compare the selected file |
| Enter | Open or compare the selected file |
| Delete | Discard changes, or delete a stash (both ask first) |
| Ctrl+C | Copy the selected paths, lines or commit id |
| Ctrl+1 … Ctrl+4 | Changes, History, Stashes, Locks |
| F5 | Refresh |

Right-click menus show each action's key beside it.
