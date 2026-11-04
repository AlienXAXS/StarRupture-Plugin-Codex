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
	// screen, a station, the map, the inventory); the build menu is named too in
	// case it ever stops setting it. The menu-layer bits are read from the game
	// every tick once a plugin asks for one.
	inline constexpr uint64_t kPinLayerMenus = PLUGIN_UI_PAUSE_MENU | PLUGIN_UI_ANY_MENU | PLUGIN_UI_BUILD_MENU;
	static_assert((kPinLayerMenus & PLUGIN_UI_CURSOR_UP) == 0, "plain gameplay must not show the pin layer arrow");

	// The bits Codex asks the loader to report.
	inline constexpr uint64_t kSubscribedMask = kUIStateMask | kPinLayerMenus;

	// The loader's UI state as of its last game tick. Only the bits in
	// kSubscribedMask are kept current. Safe from any thread.
	uint64_t UIState();
	void Tick(float deltaSeconds);
	void Shutdown(IPluginSelf* self);
}
