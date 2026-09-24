# Codex -- StarRupture Plugin

A StarRupture plugin built on the StarRupture ModLoader that adds an in-game recipe codex:
search any item or recipe and jump straight to a detail view of what it's crafted from and
what it produces.

**Target:** Game client only

---

## Features

- **Recipe search** -- press the configurable keybind (default `N`) to open a search box,
  type an item, recipe or building name, and jump to its detail view. Close with `Esc`.
- **Full Codex window** -- press `Shift+N` (configurable) to open the Codex straight to the
  last recipe you viewed, or the building browser if you haven't picked one yet.
- **Recipe detail window** -- shows a recipe's inputs and outputs side by side, each with
  icon, name, count, and craft rate (items/min). Click an input to jump to the recipe that
  produces it; click an output to see every recipe that consumes it. Click a building under
  "Made in" to browse everything that building makes.
- **Building browser** -- the Buildings tab lists every crafting building and the recipes it
  can produce. Tiered buildings are grouped under one name (e.g. "Fabricator") with a
  Tier 1 / Tier 2 tab for each tier. Click a recipe to open it, right-click to pin it.
- **Pinned recipes** -- pin up to 8 recipes to keep them on screen while you play. Pinned
  windows never block game input; open the Codex (`N`) to drag them around or unpin them.
- **Configurable** -- enable/disable the plugin and rebind the search key from the mod
  loader's config file.

## Configuration

Settings are written to the plugin's config file on first run and can be edited there:

| Section | Key | Default | Description |
| --- | --- | --- | --- |
| `General` | `Enabled` | `true` | Enable or disable Codex |
| `Menu` | `SearchKey` | `N` | Key to open the recipe search box |
| `Menu` | `CodexKey` | `Shift+N` | Key to open (or close) the full Codex window |

---

## Building

1. Clone this repo (with submodules, or set `GameSDKRoot` / `PluginSDKInclude` in `Shared.props`)
2. Open `StarRupture-Plugin-Codex.sln` in Visual Studio 2022
3. Build the `Client Release` configuration (x64)
4. The output DLL is written to `build\Client Release\Plugins\Codex.dll`
5. Drop it into `Binaries\Win64\Plugins\` alongside `dwmapi.dll` and launch the game

See [PluginDevelopment.md](PluginDevelopment.md) for the full plugin API reference.
