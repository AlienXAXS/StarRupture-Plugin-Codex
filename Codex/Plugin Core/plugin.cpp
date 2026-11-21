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

// What PluginInit actually set up, so PluginShutdown undoes exactly that
// and never reads the live feature flag.
static bool g_callbacksRegistered = false; // the tick and the load callback are on
static bool g_codexStarted = false;        // the recipe codex is up

static void OnExperienceLoadComplete();

// Brings the recipe codex up: the recipe database, the windows, the game-menu pin.
static void StartCodex(IPluginSelf* self)
{
	// Tell the image library the item icons are the ones Codex draws
	CodexIcons::Init(self);

	// Recipe database + search/detail UI
	CodexRecipes::Init(self);
	CodexUI::Init(self);
	CodexTooltipPin::Init(self);
	g_codexStarted = true;

	// A session may already be in progress (hot-reload, or the feature coming
	// back on), so scan now.
	OnExperienceLoadComplete();
}

// Takes the recipe codex down, including the rows it added to the game's tooltips.
static void StopCodex()
{
	CodexTooltipPin::Shutdown(g_self);
	CodexUI::Shutdown(g_self);
	CodexRecipes::Shutdown();
	CodexIcons::Shutdown(g_self);
	g_codexStarted = false;
}

// Drives CodexUI::Tick() so the saved pins are put back once the recipes are ready.
// Also follows the feature flag: a host can switch the codex off and back on while
// the game runs.
static void OnEngineTick(float deltaSeconds)
{
	const bool on = CodexFields::RecipeCodex.On();
	if (on != g_codexStarted)
	{
		if (on)
		{
			LOG_INFO("The recipe codex is back on");
			StartCodex(g_self);
		}
		else
		{
			LOG_INFO("The recipe codex is off");
			StopCodex();
		}
	}

	if (!on)
		return;

	CodexUI::Tick();
	CodexTooltipPin::Tick(deltaSeconds);
}

// Fires once a save is fully loaded into the world — (re)scans the recipe
// database. Recipe->building association depends on a live
// ACrCraftingRecipeOwner instance that only exists once a save is loaded,
// so the scan is gated to the main game world.
static void OnExperienceLoadComplete()
{
	if (!CodexFields::RecipeCodex.On())
		return;

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

		// The tick starts the codex when the feature is on, and stops and restarts
		// it as the flag changes; an off start is the same state.
		if (self->hooks->Engine)
			self->hooks->Engine->RegisterOnTick(&OnEngineTick);

		if (self->hooks->World)
			self->hooks->World->RegisterOnExperienceLoadComplete(&OnExperienceLoadComplete);
		g_callbacksRegistered = true;

		if (CodexFields::RecipeCodex.On())
			StartCodex(self);
		else
			LOG_WARN("The recipe codex is off: the game update moved something it reads, or the host turned it off");

		LOG_INFO("Plugin initialized successfully");

		return true;
	}

	__declspec(dllexport) void PluginShutdown()
	{
		LOG_INFO("Plugin shutting down...");

		if (g_callbacksRegistered)
		{
			if (g_self->hooks->Engine)
				g_self->hooks->Engine->UnregisterOnTick(&OnEngineTick);

			if (g_self->hooks->World)
				g_self->hooks->World->UnregisterOnExperienceLoadComplete(&OnExperienceLoadComplete);
			g_callbacksRegistered = false;
		}

		if (g_codexStarted)
			StopCodex();

		g_self = nullptr;
	}

} // extern "C"
