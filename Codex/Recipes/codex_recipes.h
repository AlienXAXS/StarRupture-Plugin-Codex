#pragma once

#include "plugin_interface.h"

#include <cstdint>
#include <string>
#include <vector>

// Recipe database for the Codex search/detail UI.
//
// Built once per session from the crafting buildings themselves: every
// building's UCrItemRecipeCollection, plus any collections GameFeature
// plugins have linked onto it at runtime (UCrItemRecipeCollectionSubsystem).
// Gated to only run once the player is in the ChimeraMain world, since the
// recipe-owner / technology-keeper actors that list the buildings only exist
// once a save is loaded.
namespace CodexRecipes
{
	// One ingredient/output line within a recipe.
	struct RecipeItemRef
	{
		std::string uniqueItemName;
		std::string displayName;
		int32_t     count = 0;
	};

	// A building that crafts a given recipe - enough to find it again in
	// the building browser (GetBuildingGroups) by name + tier.
	struct BuildingRef
	{
		std::string groupName; // BuildingGroup::displayName, e.g. "Fabricator"
		int         tier = 1;
		std::string label;     // BuildingTier::label, e.g. "Fabricator (Tier 2)"
	};

	struct RecipeInfo
	{
		void*       nativeRecipe = nullptr; // SDK::UCrItemRecipeData* identity key - stable for the session
		std::string displayName;
		std::string buildingName;           // label of the lowest-tier building that crafts this recipe
		std::vector<BuildingRef> buildings; // every building that crafts it, lowest tier first
		int         minTier = 1;            // lowest building tier that crafts this recipe
		bool        hasTierVariants = false; // another recipe shares this display name at a different minTier
		float       buildTimeSeconds = 0.0f;
		float       outputsPerMinute = 0.0f; // (output.count / buildTimeSeconds) * 60, or 0 if buildTimeSeconds <= 0
		RecipeItemRef        output;
		std::vector<RecipeItemRef> inputs;
	};

	// One tier of a building (e.g. Fabricator Tier 2) and the recipes it
	// can craft, sorted by display name.
	struct BuildingTier
	{
		int                tier = 1;
		std::string        label;       // "Fabricator (Tier 2)", or just the name if the group has one tier
		std::vector<void*> recipes;     // RecipeInfo::nativeRecipe keys
	};

	// Every tier of one building, e.g. "Fabricator" -> { Tier 1, Tier 2 }.
	struct BuildingGroup
	{
		std::string               displayName;
		std::vector<BuildingTier> tiers; // ascending by tier
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

	// Snapshot of every crafting building, grouped by tier and sorted by
	// display name.
	std::vector<BuildingGroup> GetBuildingGroups();

	// Finds the recipe whose output item matches uniqueItemName, preferring
	// the one made in the lowest-tier building if several produce it.
	// Returns false if none found.
	bool FindProducerOfItem(const std::string& uniqueItemName, RecipeInfo& outRecipe);

	// Finds every recipe that lists uniqueItemName among its inputs, sorted
	// by display name. Used to show "what uses this?" when clicking an
	// output item in the detail window.
	std::vector<RecipeInfo> FindConsumersOfItem(const std::string& uniqueItemName);

	// Finds a recipe by its native identity pointer (RecipeInfo::nativeRecipe).
	bool FindByNativeRecipe(void* nativeRecipe, RecipeInfo& outRecipe);
}
