#include "plugin.h"
#include "Helpers/plugin_helpers.h"
#include "Config/plugin_config.h"
#include "Icons/codex_icons.h"
#include "Recipes/codex_recipes.h"
#include "UI/codex_ui.h"
#include "UI/game_tooltip_pin.h"
#include "codex_fields.h"

#include "Engine_classes.hpp"

// Global plugin self pointer — stable for the plugin's lifetime, retained from PluginInit
static IPluginSelf* g_self = nullptr;

IPluginSelf* GetSelf() { return g_self; }

// Drives CodexIcons::Tick() so icon loads/retries happen once per engine tick, and
// CodexUI::Tick() so the saved pins are put back once the recipes are ready.
static void OnEngineTick(float deltaSeconds)
{
	CodexIcons::Tick();
	CodexUI::Tick();
	CodexTooltipPin::Tick(deltaSeconds);
}

// Fires once a save is fully loaded into the world — (re)scans the recipe
// database. Recipe->building association depends on a live
// ACrCraftingRecipeOwner instance that only exists once a save is loaded,
// so the scan is gated to the main game world.
static void OnExperienceLoadComplete()
{
	SDK::UWorld* world = PluginSdkRuntime::GetWorld();
	if (!world || world->GetName() != "ChimeraMain")
	{
		LOG_DEBUG("OnExperienceLoadComplete: ignored (not in ChimeraMain map)");
		return;
	}

	CodexRecipes::OnSessionLoaded();
}

// Plugin metadata
#ifndef MODLOADER_BUILD_TAG
#define MODLOADER_BUILD_TAG "dev"
#endif

static PluginInfo s_pluginInfo = {
	"Codex",
	MODLOADER_BUILD_TAG,
	"AlienX",
	"A minimal example plugin showing the basic structure required by the mod loader",
	PLUGIN_INTERFACE_VERSION,
	PLUGIN_TARGET_CLIENT,
	PLUGIN_GAME_BUILT_CL,
	PLUGIN_MAP_MIGRATED
};

extern "C" {

	__declspec(dllexport) PluginInfo* GetPluginInfo()
	{
		return &s_pluginInfo;
	}

	// Runs after GetPluginInfo and before PluginInit, and is the only context in
	// which the loader accepts a field request. Resolve here, read from PluginInit.
	__declspec(dllexport) void OnPluginLoadHooks(IPluginSelf* self, IPluginHookScanner* scanner)
	{
		CodexFields::DeclareFeatures(self, scanner);
		CodexFields::Resolve(self, scanner);
	}

	__declspec(dllexport) bool PluginInit(IPluginSelf* self)
	{
		// First, before any early return: off its game build the loader refuses a
		// plugin whose PluginInit did not take the globals.
		PluginSdkRuntime::Init(self->hooks);

		// Store the plugin self pointer — valid for the plugin's entire lifetime
		g_self = self;

		LOG_INFO("Plugin initializing...");

		// Initialize config system (optional - creates config file with defaults)
		CodexConfig::Config::Initialize(self);

		// Check if plugin is enabled via config
		if (!CodexConfig::Config::IsEnabled())
		{
			LOG_WARN("Plugin is disabled in config file");
			return true; // Return true so plugin loads but doesn't activate
		}

		if (!CodexFields::RecipeCodex.On())
		{
			LOG_WARN("The recipe codex is off: the game update moved something it reads");
			return true;
		}

		// Kick off the asset-registry scan that pre-loads item/recipe icons
		CodexIcons::Init(self);

		// Recipe database + search/detail UI
		CodexRecipes::Init(self);
		CodexUI::Init(self);
		CodexTooltipPin::Init(self);

		if (self->hooks->Engine)
			self->hooks->Engine->RegisterOnTick(&OnEngineTick);

		if (self->hooks->World)
			self->hooks->World->RegisterOnExperienceLoadComplete(&OnExperienceLoadComplete);

		// Hot-reload: experience-load-complete may have already fired before we
		// registered, so if a session is already in progress, scan now.
		OnExperienceLoadComplete();

		LOG_INFO("Plugin initialized successfully");

		return true;
	}

	__declspec(dllexport) void PluginShutdown()
	{
		LOG_INFO("Plugin shutting down...");

		if (g_self && g_self->hooks->Engine)
			g_self->hooks->Engine->UnregisterOnTick(&OnEngineTick);

		if (g_self && g_self->hooks->World)
			g_self->hooks->World->UnregisterOnExperienceLoadComplete(&OnExperienceLoadComplete);

		CodexTooltipPin::Shutdown(g_self);
		CodexUI::Shutdown(g_self);
		CodexRecipes::Shutdown();
		CodexIcons::Shutdown();

		g_self = nullptr;
	}

} // extern "C"
