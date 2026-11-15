# Project Keeper

Project Keeper is a portable Windows app for keeping local projects, snapshots, notes, and GitHub issue activity in one vault. Its MCP server lets Codex read and update that same vault.

**[Download the 0.6.0 prerelease for Windows x64][download]** · [Release notes and checksums][release]

## Quick start

1. Extract the ZIP to a folder on Windows 10 or 11 (x64). Keep both executables together if you plan to use MCP.
2. Open `ProjectKeeper.exe` and approve the Windows administrator prompt.
3. Choose an empty dedicated folder for your vault, or reopen an existing Project Keeper vault.
4. Select **New project** or **Import folder**. Import copies your source folder into the vault and shows file and byte progress.

## What you can do

- **Organize projects** on a searchable dark tile board. Pin projects to the top and sort by name, size, or last modified.
- **Keep context and notes** with each project. `PROJECT_CONTEXT.md` summarizes the folder and its README. `PROJECT_NOTES.md` holds your decisions and handoff notes.
- **Make snapshots and recover files** from **Backups** or **Trash**. Recovering a backup creates a separate project.
- **Track GitHub activity** by linking a repository. Project tiles display open issues, unread activity, stars, and forks. Sync runs every five minutes while the app is running. For repositories that require authentication, the app stores your optional token in Windows Credential Manager.
- **Stay in the tray** when you close the window. Click the tray icon to reopen it, or right-click it and choose **Exit** to quit. Use `ProjectKeeper.exe --tray` to start there.
- **Hide or passcode-protect projects** inside the app. Vault files remain accessible through Windows file permissions.

## Connect Codex with MCP

In Codex settings, add a **STDIO** MCP server named `project-keeper`. Use the absolute path to `ProjectKeeperMcp.exe` as its command. It uses the vault you selected in the desktop app. To select a vault explicitly, pass `--vault` and the vault path as arguments.

With the Codex CLI, register the server from PowerShell using the full path to your helper.

```powershell
codex mcp add project-keeper -- "C:\path\to\ProjectKeeperMcp.exe"
```

Restart Codex after adding the server. Try asking it to import a local folder, add a handoff note, or summarize cached GitHub issues for a project.

| Tool | What it does |
| --- | --- |
| `list_projects` | Search visible projects and filter to pinned projects. |
| `get_project` | Read project metadata, cached GitHub stats, and optional source file metrics. |
| `read_project_document` | Read notes, generated context, or activity history. |
| `list_project_issues` | Read issues cached by the desktop app's latest GitHub sync. |
| `create_project` | Create an empty project or copy an existing folder into the vault. |
| `write_project_notes` | Append Markdown or replace notes. Pass `expected_revision` from a previous read to catch intervening edits. |
| `update_project` | Edit a description or change a project's pin status. |

The desktop app and MCP helper serialize vault writes. Hidden and passcode-protected projects are excluded from MCP access.

## Build from source

Use CMake 3.20 or newer and MSVC with the **Desktop development with C++** workload and a Windows SDK. Run these commands in an x64 Developer PowerShell.

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

The release uses the static MSVC runtime. Python 3 enables the optional MCP smoke test and the PE import verifier. See [VALIDATION.txt](VALIDATION.txt) for the prerelease checks.

## Upgrade

Exit the desktop app from its tray menu and update both executables together. Reopen the desktop app and select your existing vault if needed. Restart Codex to load the updated MCP tool catalog.

[download]: https://github.com/VaporUnlocked/Project-Keeper/releases/download/v0.6.0-preview.1/ProjectKeeper-0.6.0-windows-x64.zip
[release]: https://github.com/VaporUnlocked/Project-Keeper/releases/tag/v0.6.0-preview.1
