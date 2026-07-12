#pragma once

#include "plugin_interface.h"

#include <cstdint>
#include <string>
#include <vector>

// Recipe database for the Codex search/detail UI.
//
// Recipes are enumerated once per session via an asset-registry scan of
// UCrItemRecipeData (mirrors CodexIcons' UAuItemBlueprint scan), gated to
// only run once the player is in the ChimeraMain world (recipe->building
// association depends on a live ACrCraftingRecipeOwner instance that only
// exists once a save is loaded).
namespace CodexRecipes
{
	// One ingredient/output line within a recipe.
	struct RecipeItemRef
	{
		std::string uniqueItemName;
		std::string displayName;
		int32_t     count = 0;
	};

	struct RecipeInfo
	{
		void*       nativeRecipe = nullptr; // SDK::UCrItemRecipeData* identity key - stable for the session
		std::string displayName;
		std::string buildingName;           // factory/building that crafts this recipe, or "Unknown"
		float       buildTimeSeconds = 0.0f;
		float       outputsPerMinute = 0.0f; // (output.count / buildTimeSeconds) * 60, or 0 if buildTimeSeconds <= 0
		RecipeItemRef        output;
		std::vector<RecipeItemRef> inputs;
	};

	void Init(IPluginSelf* self);
	void Shutdown();

	// Drives icon-handle retries; call once per engine tick.
	void Tick();

	// Fires when a save/session finishes loading - (re)scans recipes and
	// their building associations if in the ChimeraMain world. Safe to call
	// at any time; a no-op outside ChimeraMain.
	void OnSessionLoaded();

	// True once at least one successful scan has completed this session.
	bool IsReady();

	// Snapshot of every known recipe, sorted by display name. Safe to call
	// from the render thread; returns a copy to avoid locking across frames.
	std::vector<RecipeInfo> GetAll();

	// Finds the recipe whose output item matches uniqueItemName (first
	// match if multiple recipes produce the same item). Returns false if
	// none found.
	bool FindProducerOfItem(const std::string& uniqueItemName, RecipeInfo& outRecipe);

	// Finds every recipe that lists uniqueItemName among its inputs, sorted
	// by display name. Used to show "what uses this?" when clicking an
	// output item in the detail window.
	std::vector<RecipeInfo> FindConsumersOfItem(const std::string& uniqueItemName);

	// Finds a recipe by its native identity pointer (RecipeInfo::nativeRecipe).
	bool FindByNativeRecipe(void* nativeRecipe, RecipeInfo& outRecipe);
}
