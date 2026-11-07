#pragma once

#include "plugin_interface.h"

#include <string>

// Item icons (UTexture2D -> ImGui texture handle) for every item type in the
// game, keyed by UAuItemDataBase::UniqueItemName. The loading is the SDK's
// ItemIcons::Loader: an icon is copied into the loader the first time
// something draws it, a few per tick, and the splash screen is held open
// until the item scan completes.
namespace CodexIcons
{
	void Init(IPluginSelf* self);
	void Shutdown();

	// Loads the icons asked for since the last tick. Call once per engine
	// tick, on the game thread.
	void Tick();

	// Draws the item's icon at the cursor and moves on by the gap after it. An
	// icon that is still loading holds its place; an item with no icon takes no
	// room. Any thread.
	void Draw(IModLoaderImGui* imgui, IPluginImGuiTextures* textures,
		const std::string& uniqueItemName, float iconSize);

	// True if the item's icon is the texture with this identity (the texture's
	// UObject::GetFullName). Game thread.
	bool IconIs(const std::string& uniqueItemName, const std::string& textureId);
}
