#pragma once

#include "plugin_interface.h"
#include "UI/pin_layout.h"

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

	// True while the Escape menu is up or a game screen has the game cursor up
	// (a machine menu, the recipe station, the inventory, the build menu), so the
	// pin layer arrow never shows in plain gameplay. The pinned cards' controls
	// follow the same bits (CodexPinLayout::PinLayerMenuUp).
	inline bool PinLayerMenuUp(uint64_t state)
	{
		return CodexPinLayout::PinLayerMenuUp((state & PLUGIN_UI_PAUSE_MENU) != 0, (state & PLUGIN_UI_CURSOR_UP) != 0);
	}

	// The loader's UI state as of its last game tick. Only the bits in
	// kUIStateMask are kept current. Safe from any thread.
	uint64_t UIState();
	void Tick(float deltaSeconds);
	void Shutdown(IPluginSelf* self);
}
