#pragma once

#include "plugin_interface.h"

// The Codex search box (opened via a configurable keybind, closed with Esc)
// and the recipe detail window it opens into.
namespace CodexUI
{
	void Init(IPluginSelf* self);
	void Shutdown(IPluginSelf* self);
}
