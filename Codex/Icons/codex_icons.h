#pragma once

#include "plugin_interface.h"

#include <cstdint>
#include <string>

namespace SDK { class UObject; }

// Item icons, drawn from the loader's image library (hooks->Images). The
// library reads each icon the first time it is drawn and keeps a small copy for
// the session; nothing is loaded or cached here.
//
// Where the loader has no image table (a server or generic build), nothing is
// drawn and every icon keeps its space.
namespace CodexIcons
{
	// Tells the library the item images are the ones Codex draws, so it can
	// have them ready. Call once from PluginInit.
	void Init(IPluginSelf* self);

	// Gives the item images back, so the library stops having them ready. Safe to
	// call again.
	void Shutdown(IPluginSelf* self);

	// Draws an item's icon (its id from ItemImage) at the cursor, size by size,
	// and moves on by the gap after it. An icon that is still loading, or an item
	// with none, holds its place, so the text after it does not move. Call from a
	// render callback.
	void Draw(IModLoaderImGui* imgui, uint32_t itemImage, float iconSize);

	// The image id of an item by its key (CodexFields::ItemKeyOf), or 0 for no
	// item or no image table. The one place an item's image is chosen. Only
	// names the image; nothing is read until it is drawn.
	uint32_t ItemImage(const std::string& itemKey);

	// The image id of a texture, or 0 when the library cannot name it. Game
	// thread; the texture is not kept.
	uint32_t ImageOfTexture(SDK::UObject* texture);

	// Whether an item id (from ItemImage) and a texture id (from ImageOfTexture)
	// are the same picture. False for an unknown id, and for an item the library
	// has not resolved yet. Any thread.
	bool SameImage(uint32_t itemImage, uint32_t textureImage);
}
