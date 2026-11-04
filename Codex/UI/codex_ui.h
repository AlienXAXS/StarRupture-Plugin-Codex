#pragma once

#include "plugin_interface.h"

#include <string>

// The Codex search box (opened via a configurable keybind, closed with Esc)
// and the recipe detail window it opens into.
namespace CodexUI
{
	void Init(IPluginSelf* self);

	// Once per engine tick: puts the saved pins back as soon as the recipe list is ready.
	void Tick();

	// Pins the recipe that makes the item, as a right-click on it in a Codex
	// window does. Any thread: the pin is made on the next rendered frame.
	void PinProductOf(const std::string& uniqueItemName);

	void Shutdown(IPluginSelf* self);
}
