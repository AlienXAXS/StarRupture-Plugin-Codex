#pragma once

#include "plugin_interface.h"

#include <string>

// Item/recipe icons (UTexture2D -> ImGui texture handle) for every item type
// in the game, keyed by UAuItemDataBase::UniqueItemName.
//
// The asset registry scan runs once on the game thread (posted from Init) and
// keeps only strings. An icon is copied into the loader the first time
// something asks to draw it: GetIcon records the ask, Tick serves it within a
// small time budget, and the handle is then held until Shutdown. The splash
// screen is held open (AcquireSplashHold) until the scan completes.
namespace CodexIcons
{
	void Init(IPluginSelf* self);
	void Shutdown();

	// Adopts a freshly-scanned item list and loads the icons asked for since
	// the last tick. Call once per engine tick, on the game thread.
	void Tick();

	// Returns a texture handle for the given UniqueItemName, or nullptr if
	// the item has no icon or its icon hasn't loaded yet. A nullptr for an item
	// that has an icon asks for it to be loaded. Any thread.
	PluginTextureHandle GetIcon(const std::string& uniqueItemName);

	// True if the item has an icon, loaded or still to load. Any thread.
	bool HasIcon(const std::string& uniqueItemName);

	// True if the item's icon is the texture with this identity (the texture's
	// UObject::GetFullName). Game thread.
	bool IconIs(const std::string& uniqueItemName, const std::string& textureId);
}
