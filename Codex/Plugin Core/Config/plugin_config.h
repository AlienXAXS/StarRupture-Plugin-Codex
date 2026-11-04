#pragma once

#include "plugin_interface.h"
#include "UI/pin_layout.h"

#include <cstdio>
#include <cstring>

namespace CodexConfig
{
	static const ConfigEntry CONFIG_ENTRIES[] = {
		{
			"General",
			"Enabled",
			ConfigValueType::Boolean,
			"true",
			"Enable or disable Codex"
		},
		{
			"Menu",
			"SearchKey",
			ConfigValueType::Keybind,
			"N",
			"Key to open the recipe search box"
		},
		{
			"Menu",
			"CodexKey",
			ConfigValueType::Keybind,
			"Shift+N",
			"Key to open (or close) the full Codex window"
		},
		{
			"Menu",
			"GamePinKey",
			ConfigValueType::Keybind,
			"RightMouseButton",
			"Key that pins the item in the game's open item tooltip (recipe station, delivery panel, machine menus)"
		},
		{
			"Menu",
			"GamePlannerKey",
			ConfigValueType::Keybind,
			"Ctrl+RightMouseButton",
			"Key that opens the item in the game's open item tooltip in the production planner"
		},
		{
			"Menu",
			"GameTooltipHints",
			ConfigValueType::Hidden,
			"false",
			"Whether the game's item tooltips get \"Pin to Codex\" and \"Open in Planner\" rows (off until their layout matches the game's own rows)"
		},
		{
			"Menu",
			"PinLayout",
			ConfigValueType::Choice,
			"Right",
			"Screen edge pinned recipes start from|Right|Left|Top"
		},
		{
			"Menu",
			"PinArrangement",
			ConfigValueType::Choice,
			"Tile",
			"How pinned recipes sit under the first one|Tile|Cascade"
		},
		{
			"Menu",
			"PinSpacing",
			ConfigValueType::Integer,
			"6",
			"Space between tiled pinned recipes, in screen pixels",
			1.0f,
			50.0f
		},
		{
			"Menu",
			"PinCentering",
			ConfigValueType::Boolean,
			"true",
			"Keep the pinned recipes centered along their edge: left to right on Top, top to bottom on Left and Right"
		},
		{
			"Menu",
			"PinGrouping",
			ConfigValueType::Choice,
			"Grouped",
			"Whether pinned recipes move together under the first one or each on its own|Grouped|Ungrouped"
		},
		{
			"Menu",
			"SearchBoxX",
			ConfigValueType::Hidden,
			"-1",
			"Horizontal centre of the search box as a fraction of the screen width; -1 centres it. Set by dragging the box"
		},
		{
			"Menu",
			"SearchBoxY",
			ConfigValueType::Hidden,
			"-1",
			"Top edge of the search box as a fraction of the screen height; -1 centres it. Set by dragging the box"
		},
		{
			"Menu",
			"PinTileOffset",
			ConfigValueType::Hidden,
			"",
			"Where the first pinned recipe was last dragged to while tiling, as a screen-fraction \"x,y\" (empty = not moved)"
		},
		{
			"Menu",
			"PinCascadeOffset",
			ConfigValueType::Hidden,
			"",
			"Where the first pinned recipe was last dragged to while cascading, as a screen-fraction \"x,y\" (empty = not moved)"
		},
		{
			"Menu",
			"PinFreeOffsets",
			ConfigValueType::Hidden,
			"",
			"Where each pinned recipe was last placed while ungrouped, as screen-fraction \"x,y\" pairs separated by \";\", in pin order (empty = not placed)"
		},
		{
			"Menu",
			"PinCollapsed",
			ConfigValueType::Hidden,
			"",
			"Which pinned recipes are collapsed to their title bar, one 1 (collapsed) or 0 per pin in pin order (empty = none)"
		},
		{
			"Menu",
			"PinnedRecipe1",
			ConfigValueType::Hidden,
			"",
			"The asset name of pinned recipe 1, which is pinned again once a save has loaded (empty = none)"
		},
		{
			"Menu",
			"PinnedRecipe2",
			ConfigValueType::Hidden,
			"",
			"The asset name of pinned recipe 2, which is pinned again once a save has loaded (empty = none)"
		},
		{
			"Menu",
			"PinnedRecipe3",
			ConfigValueType::Hidden,
			"",
			"The asset name of pinned recipe 3, which is pinned again once a save has loaded (empty = none)"
		},
		{
			"Menu",
			"PinnedRecipe4",
			ConfigValueType::Hidden,
			"",
			"The asset name of pinned recipe 4, which is pinned again once a save has loaded (empty = none)"
		},
		{
			"Menu",
			"PinnedRecipe5",
			ConfigValueType::Hidden,
			"",
			"The asset name of pinned recipe 5, which is pinned again once a save has loaded (empty = none)"
		},
		{
			"Menu",
			"PinnedRecipe6",
			ConfigValueType::Hidden,
			"",
			"The asset name of pinned recipe 6, which is pinned again once a save has loaded (empty = none)"
		},
		{
			"Menu",
			"PinnedRecipe7",
			ConfigValueType::Hidden,
			"",
			"The asset name of pinned recipe 7, which is pinned again once a save has loaded (empty = none)"
		},
		{
			"Menu",
			"PinnedRecipe8",
			ConfigValueType::Hidden,
			"",
			"The asset name of pinned recipe 8, which is pinned again once a save has loaded (empty = none)"
		},
		{
			"Menu",
			"PinnedRecipes",
			ConfigValueType::Hidden,
			"",
			"Old save of all the pinned recipes in one value, replaced by PinnedRecipe1 to PinnedRecipe8; emptied once they are copied over (empty = nothing to copy)"
		},
		{
			"Planner",
			"Planner",
			ConfigValueType::Choice,
			"StarRupture Planner",
			"Planner the Launch planner button opens|StarRupture Planner|starruptureplanner.com|starrupturecalc.com|starrupture-planner.com|Custom"
		},
		{
			"Planner",
			"CustomUrlTemplate",
			ConfigValueType::String,
			"",
			"Custom planner address; {item} becomes the StarRupture Planner's item id, {name} the item's display name and {game} the game's internal item name"
		}
	};

	static const ConfigSchema SCHEMA = {
		CONFIG_ENTRIES,
		sizeof(CONFIG_ENTRIES) / sizeof(ConfigEntry)
	};

	// Type-safe config accessor class
	class Config
	{
	public:
		static void Initialize(IPluginSelf* self)
		{
			s_self = self;

			// Initialize config from schema - creates file with defaults if missing
			if (s_self)
			{
				s_self->config->InitializeFromSchema(s_self, &SCHEMA);
			}
		}

		static bool IsEnabled()
		{
			return s_self ? s_self->config->ReadBool(s_self, "General", "Enabled", true) : true;
		}

		// Returns the current search-box keybind string (e.g. "N", "Ctrl+F").
		// The modloader re-registers the keybind automatically when the user changes it.
		static const char* GetSearchKey()
		{
			static char buffer[64];
			if (s_self && s_self->config->ReadString(s_self, "Menu", "SearchKey", buffer, sizeof(buffer), "N"))
				return buffer;
			return "N";
		}

		// Returns the current full-Codex-window keybind string (default "Shift+N").
		static const char* GetCodexKey()
		{
			static char buffer[64];
			if (s_self && s_self->config->ReadString(s_self, "Menu", "CodexKey", buffer, sizeof(buffer), "Shift+N"))
				return buffer;
			return "Shift+N";
		}

		// Returns the keybind that pins the item in the game's open item tooltip
		// (default "RightMouseButton").
		static const char* GetGamePinKey()
		{
			static char buffer[64];
			if (s_self && s_self->config->ReadString(s_self, "Menu", "GamePinKey", buffer, sizeof(buffer), "RightMouseButton"))
				return buffer;
			return "RightMouseButton";
		}

		// Returns the keybind that opens the item in the game's open item
		// tooltip in the production planner (default "Ctrl+RightMouseButton").
		static const char* GetGamePlannerKey()
		{
			static char buffer[64];
			if (s_self && s_self->config->ReadString(s_self, "Menu", "GamePlannerKey", buffer, sizeof(buffer), "Ctrl+RightMouseButton"))
				return buffer;
			return "Ctrl+RightMouseButton";
		}

		// Returns whether the game's item tooltips get the pin and planner hint
		// rows (default off).
		static bool GetGameTooltipHints()
		{
			return s_self ? s_self->config->ReadBool(s_self, "Menu", "GameTooltipHints", false) : false;
		}

		// Returns the screen edge pins start from: "Right" (default), "Left" or "Top".
		static const char* GetPinLayout()
		{
			static char buffer[16];
			if (s_self && s_self->config->ReadString(s_self, "Menu", "PinLayout", buffer, sizeof(buffer), "Right"))
				return buffer;
			return "Right";
		}

		// Saved search-box position as fractions of the screen (x = centre, y = top
		// edge); a negative value means "not placed yet, centre it".
		static void GetSearchBoxPos(float& x, float& y)
		{
			x = s_self ? s_self->config->ReadFloat(s_self, "Menu", "SearchBoxX", -1.0f) : -1.0f;
			y = s_self ? s_self->config->ReadFloat(s_self, "Menu", "SearchBoxY", -1.0f) : -1.0f;
		}

		static void SetSearchBoxPos(float x, float y)
		{
			if (!s_self)
				return;
			s_self->config->WriteFloat(s_self, "Menu", "SearchBoxX", x);
			s_self->config->WriteFloat(s_self, "Menu", "SearchBoxY", y);
		}

		// Returns which planner the Launch planner button opens: "StarRupture Planner" (default),
		// "starruptureplanner.com", "starrupturecalc.com", "starrupture-planner.com" or "Custom".
		static const char* GetPlanner()
		{
			static char buffer[64];
			if (s_self && s_self->config->ReadString(s_self, "Planner", "Planner", buffer, sizeof(buffer) - 1, "StarRupture Planner"))
				return buffer;
			return "StarRupture Planner";
		}

		// Returns the custom planner URL template; {item} stands for the StarRupture Planner's item id,
		// {name} for the display name and {game} for the game's UniqueItemName.
		static const char* GetPlannerUrlTemplate()
		{
			static char buffer[512];
			if (s_self && s_self->config->ReadString(s_self, "Planner", "CustomUrlTemplate", buffer, sizeof(buffer) - 1, ""))
				return buffer;
			return "";
		}

		// Returns how pins sit under the first one: "Tile" (default) or "Cascade".
		static const char* GetPinArrangement()
		{
			static char buffer[16];
			if (s_self && s_self->config->ReadString(s_self, "Menu", "PinArrangement", buffer, sizeof(buffer), "Tile"))
				return buffer;
			return "Tile";
		}

		// Returns the space between tiled pins, in screen pixels (1 to 50).
		static int GetPinGap()
		{
			if (!s_self)
				return CodexPinLayout::kGapDefault;
			char text[16] = "";
			s_self->config->ReadString(s_self, "Menu", "PinSpacing", text, sizeof(text), "");
			return CodexPinLayout::GapFromText(text);
		}

		// Returns whether the pins are kept centered along their edge.
		static bool GetPinCentering()
		{
			return s_self ? s_self->config->ReadBool(s_self, "Menu", "PinCentering", true) : true;
		}

		// Returns the current pin grouping ("Grouped" or "Ungrouped").
		static const char* GetPinGrouping()
		{
			static char buffer[32];
			if (s_self && s_self->config->ReadString(s_self, "Menu", "PinGrouping", buffer, sizeof(buffer), "Grouped"))
				return buffer;
			return "Grouped";
		}

		// Saved positions of the pins while ungrouped: "x,y" screen fractions of each
		// pin's top-left corner, separated by ";", in pin order; empty means none saved.
		static void GetPinFreeOffsets(char* out, int maxLen)
		{
			if (!s_self || !s_self->config->ReadString(s_self, "Menu", "PinFreeOffsets", out, maxLen, ""))
				out[0] = '\0';
		}

		static void SetPinFreeOffsets(const char* offsets)
		{
			if (s_self)
				s_self->config->WriteString(s_self, "Menu", "PinFreeOffsets", offsets);
		}

		// Which pins are collapsed to their title bar: one "1" (collapsed) or "0" per
		// pin, in pin order; empty means none.
		static void GetPinCollapsed(char* out, int maxLen)
		{
			if (!s_self || !s_self->config->ReadString(s_self, "Menu", "PinCollapsed", out, maxLen, ""))
				out[0] = '\0';
		}

		static void SetPinCollapsed(const char* flags)
		{
			if (s_self)
				s_self->config->WriteString(s_self, "Menu", "PinCollapsed", flags);
		}

		// The pinned recipes' keys (asset names), one per slot (1 to 8) in pin order,
		// kept so the pins come back at the next start; empty means the slot is free.
		// One value each because the loader's config table keeps 255 characters per
		// value, which eight asset names in a single value could overrun.
		static void GetPinnedRecipe(int slot, char* out, int maxLen)
		{
			char key[32];
			snprintf(key, sizeof(key), "PinnedRecipe%d", slot + 1);
			if (!s_self || !s_self->config->ReadString(s_self, "Menu", key, out, maxLen, ""))
				out[0] = '\0';
		}

		static void SetPinnedRecipe(int slot, const char* key)
		{
			char name[32];
			snprintf(name, sizeof(name), "PinnedRecipe%d", slot + 1);
			if (s_self)
				s_self->config->WriteString(s_self, "Menu", name, key);
		}

		// The old single-value save of the pins, read once to move its pins
		// into the slots and then emptied (the loader cannot delete a key).
		static void GetLegacyPinnedRecipes(char* out, int maxLen)
		{
			if (!s_self || !s_self->config->ReadString(s_self, "Menu", "PinnedRecipes", out, maxLen, ""))
				out[0] = '\0';
		}

		static void ClearLegacyPinnedRecipes()
		{
			if (s_self)
				s_self->config->WriteString(s_self, "Menu", "PinnedRecipes", "");
		}

		// Saved position of the first pin for the tile or cascade arrangement: an "x,y"
		// screen fraction of its top-left corner; empty means it was never moved.
		static void GetPinOffset(bool cascade, char* out, int maxLen)
		{
			if (!s_self || !s_self->config->ReadString(s_self, "Menu", cascade ? "PinCascadeOffset" : "PinTileOffset", out, maxLen, ""))
				out[0] = '\0';
		}

		static void SetPinOffset(bool cascade, const char* offset)
		{
			if (s_self)
				s_self->config->WriteString(s_self, "Menu", cascade ? "PinCascadeOffset" : "PinTileOffset", offset);
		}

	private:
		static IPluginSelf* s_self;
	};
}
