# Scripts for non-MCP use

Everything in `tools/` works as a plain PowerShell script — no MCP client
required. These drive the same UevrMcpServer.exe CLI verbs that the MCP
tools call underneath.

## Contents

| Script | What it does |
|---|---|
| [`setup.ps1`](setup.ps1) | One-time setup: verify deps, build plugin + MCP server, register WER dumps |
| [`enable-dumper-mode.ps1`](enable-dumper-mode.ps1) | Write the `dumper_mode` sentinel for a given game so UEVR skips render hooks |
| [`disable-dumper-mode.ps1`](disable-dumper-mode.ps1) | Remove the sentinel — restores normal VR-mode UEVR behaviour |
| [`quick-dump.ps1`](quick-dump.ps1) | Full dump pipeline for a single game: inject → USMAP + UE project + BN/IDA bundle → output summary |
| [`enable-wer-dumps.ps1`](enable-wer-dumps.ps1) | Register per-exe Windows Error Reporting full-memory dumps. Useful for diagnosing crashes in UEVR or plugins |
| [`dumper-mode-recipe.md`](dumper-mode-recipe.md) | Walkthrough: from zero to a complete game dump, no MCP client |
| [`cli-cheatsheet.md`](cli-cheatsheet.md) | Every UevrMcpServer.exe CLI verb with concrete examples |

## Quick start

```powershell
# 1. First-time setup (idempotent — safe to re-run)
.\tools\setup.ps1

# 2. Enable dumper mode for a specific game
.\tools\enable-dumper-mode.ps1 -GameExe "D:\Steam\MyGame\Binaries\Win64\MyGame-Win64-Shipping.exe"

# 3. Launch the game manually (or via Steam), then attach + full dump
.\tools\quick-dump.ps1 -GameExe "D:\Steam\MyGame\Binaries\Win64\MyGame-Win64-Shipping.exe" -OutDir C:\dumps\MyGame
```

Output:
- `C:\dumps\MyGame\MyGame.usmap` — FModel/CUE4Parse mappings
- `C:\dumps\MyGame\MirrorProject\` — buildable UE4/UE5 project
- `C:\dumps\MyGame\REBundle\` — Binary Ninja + IDA import bundle
- Optional: log of the MCP server output + probe diagnostics per run

All scripts accept `-Help` for full parameter documentation.
