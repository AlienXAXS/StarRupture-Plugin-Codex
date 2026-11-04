#pragma once

#include "plugin_interface.h"

// Pinning from the game's own menus (recipe station, company delivery panel,
// machine menus). While the game's item tooltip is open, the pin key
// (right-click by default, set in the config) pins the recipe that makes the
// item the tooltip shows, the same pin a right-click on that item gives in a
// Codex window. Ctrl+right-click (GamePlannerKey) opens the item in the
// production planner, the same page the Planner button opens. With the hidden
// setting Menu.GameTooltipHints on, the tooltip also ends with a "Pin to Codex"
// row and an "Open in Planner" row, in the game's own action-button style.
namespace CodexTooltipPin
{
	void Init(IPluginSelf* self);

	// The loader's UI state bits Codex follows: the pause menu, and a game screen
	// having the mouse cursor up (a machine menu, the recipe station, the inventory).
	inline constexpr uint64_t kUIStateMask = PLUGIN_UI_PAUSE_MENU | PLUGIN_UI_CURSOR_UP;

	// The menus the pin layer arrow shows in: every game menu. ANY_MENU is the
	// loader's menu-layer bit, set for each screen on the menu layer (a machine's
	// screen, a station, the map, the inventory). The loader reads the menu-layer
	// bits from the game every tick while a plugin asks for them, so Codex asks
	// only while a pin is up (WatchPinMenus).
	inline constexpr uint64_t kPinLayerMenus = PLUGIN_UI_PAUSE_MENU | PLUGIN_UI_ANY_MENU;

	// True while a game menu is up and the game cursor with it, so the pin layer
	// arrow never shows in plain gameplay.
	inline constexpr bool PinLayerMenuUp(uint64_t state)
	{
		return (state & kPinLayerMenus) != 0 && (state & kUIStateMask) != 0;
	}

	// Starts or stops asking the loader for the menu-layer bits. Idempotent.
	// Any thread.
	void WatchPinMenus(bool on);

	// The loader's UI state as of its last game tick. Only the bits in
	// kUIStateMask, and kPinLayerMenus while WatchPinMenus is on, are kept
	// current. Safe from any thread.
	uint64_t UIState();
	void Tick(float deltaSeconds);
	void Shutdown(IPluginSelf* self);
}
