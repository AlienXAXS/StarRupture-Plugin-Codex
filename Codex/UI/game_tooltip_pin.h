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

	// True while a game screen has the mouse cursor up (a machine menu, the
	// recipe station, the inventory, the pause menu). Refreshed every tick on the
	// game thread; safe from any thread.
	bool GameCursorUp();
	void Tick(float deltaSeconds);
	void Shutdown(IPluginSelf* self);
}
