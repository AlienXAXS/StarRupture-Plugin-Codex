#pragma once

#include "plugin_interface.h"

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

	private:
		static IPluginSelf* s_self;
	};
}
