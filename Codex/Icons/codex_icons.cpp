#include "codex_icons.h"
#include "Plugin Core/codex_fields.h"

#include "item_icons.h"

namespace CodexIcons
{
	namespace
	{
		std::string UniqueNameOf(SDK::UAuItemDataBase* item)
		{
			return CodexFields::UniqueNameOf(item);
		}

		const SDK::FSlateBrush* IconOf(SDK::UAuItemDataBase* item)
		{
			return CodexFields::ItemIcon.In(item);
		}

		ItemIcons::Loader g_icons;
	}

	void Init(IPluginSelf* self)
	{
		g_icons.Init(self, "CodexIcons", { &UniqueNameOf, &IconOf });
	}

	void Shutdown()
	{
		g_icons.Shutdown();
	}

	void Tick()
	{
		g_icons.Tick();
	}

	void Draw(IModLoaderImGui* imgui, IPluginImGuiTextures* textures,
		const std::string& uniqueItemName, float iconSize)
	{
		g_icons.Draw(imgui, textures, uniqueItemName, iconSize);
	}

	bool IconIs(const std::string& uniqueItemName, const std::string& textureId)
	{
		return g_icons.IconIs(uniqueItemName, textureId);
	}
}
