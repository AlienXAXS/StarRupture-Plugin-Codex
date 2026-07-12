#include "codex_recipes.h"
#include "Plugin Core/Helpers/plugin_helpers.h"

#include "Chimera_classes.hpp"
#include "Chimera_structs.hpp"
#include "AuCrafting_classes.hpp"
#include "AuCrafting_structs.hpp"
#include "AuItems_classes.hpp"
#include "AuItems_structs.hpp"
#include "AssetRegistry_classes.hpp"
#include "AssetRegistry_parameters.hpp"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

// Enumerates every recipe via the asset registry (mirrors CodexIcons' item
// scan) and, best-effort, resolves which building/factory crafts each one
// via the live ACrCraftingRecipeOwner actor's CrafterToRecipeMap. Runs on
// the game thread, posted from OnSessionLoaded (gated to ChimeraMain by the
// caller in plugin.cpp) since the recipe-owner actor only exists once a
// save is loaded.
namespace CodexRecipes
{
	namespace
	{
		std::mutex              g_mutex;
		std::vector<RecipeInfo> g_recipes;
		bool                    g_ready = false;
		std::atomic<bool>       g_refreshInFlight{ false };

		// Same object/package lookup trampolines as CodexIcons - resolved once
		// via the AOB-scanned addresses the modloader exposes.
		using StaticFindObjectByNameFn = SDK::UObject*  (__fastcall*)(SDK::UClass*, SDK::UObject*, const wchar_t*, bool);
		using FindPackageFn            = SDK::UPackage* (__fastcall*)(SDK::UObject*, const wchar_t*);
		using PackageFullyLoadFn       = void           (__fastcall*)(SDK::UPackage*);
		using LoadPackageFn            = SDK::UPackage* (__fastcall*)(SDK::UPackage*, const wchar_t*, uint32_t, void*, const void*);

		StaticFindObjectByNameFn g_staticFindObjectByName = nullptr;
		FindPackageFn            g_findPackage             = nullptr;
		PackageFullyLoadFn       g_packageFullyLoad        = nullptr;
		LoadPackageFn            g_loadPackage             = nullptr;
		bool                     g_engineFnsResolveTried   = false;

		bool ResolveEngineLookupFunctions()
		{
			if (g_engineFnsResolveTried)
				return g_staticFindObjectByName && g_findPackage && g_packageFullyLoad && g_loadPackage;

			g_engineFnsResolveTried = true;

			IPluginHooks* hooks = GetHooks();
			IPluginEngineEvents* engine = hooks ? hooks->Engine : nullptr;
			if (!engine)
			{
				LOG_WARN("CodexRecipes: engine events interface unavailable - cannot resolve object/package lookup functions.");
				return false;
			}

			if (uintptr_t address = engine->GetStaticFindObjectByNameAddress())
				g_staticFindObjectByName = reinterpret_cast<StaticFindObjectByNameFn>(address);
			if (uintptr_t address = engine->GetFindPackageAddress())
				g_findPackage = reinterpret_cast<FindPackageFn>(address);
			if (uintptr_t address = engine->GetPackageFullyLoadAddress())
				g_packageFullyLoad = reinterpret_cast<PackageFullyLoadFn>(address);
			if (uintptr_t address = engine->GetLoadPackageAddress())
				g_loadPackage = reinterpret_cast<LoadPackageFn>(address);

			if (!g_staticFindObjectByName || !g_findPackage || !g_packageFullyLoad || !g_loadPackage)
			{
				LOG_WARN("CodexRecipes: failed to resolve object/package lookup functions - recipe list will be incomplete.");
				return false;
			}

			return true;
		}

		SDK::FTopLevelAssetPath MakeRecipeClassPath()
		{
			SDK::FString packagePath(L"/Script/Chimera");
			SDK::FString className(L"CrItemRecipeData");
			return SDK::UKismetSystemLibrary::MakeTopLevelAssetPath(packagePath, className);
		}

		SDK::UObject* CallGetAssetRegistry()
		{
			SDK::UAssetRegistryHelpers* cdo = SDK::UAssetRegistryHelpers::GetDefaultObj();
			if (!cdo)
				return nullptr;

			static SDK::UFunction* func = nullptr;
			if (!func)
				func = SDK::UAssetRegistryHelpers::StaticClass()->GetFunction("AssetRegistryHelpers", "GetAssetRegistry");
			if (!func)
			{
				LOG_WARN("CodexRecipes: could not resolve UAssetRegistryHelpers::GetAssetRegistry.");
				return nullptr;
			}

			SDK::Params::AssetRegistryHelpers_GetAssetRegistry parms{};
			const auto flags = func->FunctionFlags;
			func->FunctionFlags |= 0x400;
			cdo->ProcessEvent(func, &parms);
			func->FunctionFlags = flags;

			return parms.ReturnValue.GetObjectRef();
		}

		bool CallGetAssetsByClass(SDK::IAssetRegistry* registry, const SDK::FTopLevelAssetPath& classPath, SDK::TArray<SDK::FAssetData>& outAssets)
		{
			SDK::UObject* registryObject = registry ? registry->AsUObject() : nullptr;
			if (!registryObject)
				return false;

			static SDK::UFunction* func = nullptr;
			if (!func)
				func = SDK::IAssetRegistry::StaticClass()->GetFunction("AssetRegistry", "GetAssetsByClass");
			if (!func)
			{
				LOG_WARN("CodexRecipes: could not resolve IAssetRegistry::GetAssetsByClass.");
				return false;
			}

			SDK::Params::AssetRegistry_GetAssetsByClass parms{};
			parms.ClassPathName     = classPath;
			parms.bSearchSubClasses = true;

			const auto flags = func->FunctionFlags;
			func->FunctionFlags |= 0x400;
			registryObject->ProcessEvent(func, &parms);
			func->FunctionFlags = flags;

			outAssets = std::move(parms.OutAssetData);
			return parms.ReturnValue;
		}

		// Recipes are plain UDataAsset instances (not Blueprint-generated
		// classes like items), so the asset itself IS the object - no
		// "_C"/CDO indirection needed, just a straight FindObjectByName
		// under the loaded package.
		SDK::UCrItemRecipeData* ResolveRecipeAsset(const SDK::FAssetData& assetData)
		{
			if (!ResolveEngineLookupFunctions())
				return nullptr;

			const std::string assetName   = assetData.AssetName.ToString();
			const std::string packageName = assetData.PackageName.GetRawString();
			if (packageName.empty() || assetName.empty())
				return nullptr;

			const std::wstring packageNameW(packageName.begin(), packageName.end());
			const std::wstring assetNameW(assetName.begin(), assetName.end());

			SDK::UPackage* package = g_findPackage(nullptr, packageNameW.c_str());
			if (package)
				g_packageFullyLoad(package);
			else
				package = g_loadPackage(nullptr, packageNameW.c_str(), 0, nullptr, nullptr);

			if (!package)
				return nullptr;

			SDK::UObject* obj = g_staticFindObjectByName(SDK::UCrItemRecipeData::StaticClass(), package, assetNameW.c_str(), false);
			if (!obj || !obj->IsA(SDK::UCrItemRecipeData::StaticClass()))
				return nullptr;

			return static_cast<SDK::UCrItemRecipeData*>(obj);
		}

		bool LooksBroken(const std::string& text)
		{
			return text.find("MISSING STRING TABLE ENTRY") != std::string::npos;
		}

		// Blueprint/schematic "items" (unlock tokens, not real resources) show
		// up as recipe outputs in the asset registry scan but aren't things a
		// player actually crafts - filter them out of the Codex.
		bool IsBlueprintItem(SDK::UAuItemDataBase* item)
		{
			return item && item->UIItemType == SDK::EUIItemType::BlueprintItem;
		}

		RecipeItemRef MakeItemRef(const SDK::FAuSimpleItem& simpleItem)
		{
			RecipeItemRef ref;
			ref.count = simpleItem.Count;

			if (simpleItem.ItemDataBase)
			{
				ref.uniqueItemName = simpleItem.ItemDataBase->UniqueItemName.ToString();
				ref.displayName    = SDK::UKismetTextLibrary::Conv_TextToString(simpleItem.ItemDataBase->ItemName).ToString();
				if (ref.displayName.empty())
					ref.displayName = ref.uniqueItemName;
			}

			if (ref.uniqueItemName.empty())
			{
				ref.uniqueItemName = "Unknown";
				if (ref.displayName.empty())
					ref.displayName = "Unknown";
			}

			return ref;
		}

		// Best-effort building/factory lookup: walks the live
		// ACrCraftingRecipeOwner's CrafterToRecipeMap (building -> recipe
		// collection -> recipes) and returns a map from recipe pointer to
		// building display name. Empty if the actor or map can't be
		// resolved - callers fall back to "Unknown".
		std::unordered_map<void*, std::string> BuildRecipeToBuildingMap()
		{
			std::unordered_map<void*, std::string> result;

			IPluginHooks* hooks = GetHooks();
			IPluginObjectWalker* walker = hooks ? hooks->ObjectWalker : nullptr;
			if (!walker || !walker->IsReady())
				return result;

			PluginObjectInfo info{};
			int total = walker->FindObjectsByClassNameInto("CrCraftingRecipeOwner", PluginObjectLookup_InstanceOnly, &info, 1);
			if (total <= 0 || !info.object)
			{
				LOG_DEBUG("CodexRecipes: no CrCraftingRecipeOwner instance found - buildings will show as Unknown.");
				return result;
			}

			auto* owner = reinterpret_cast<SDK::ACrCraftingRecipeOwner*>(info.object);

			try
			{
				for (auto& pair : owner->CrafterToRecipeMap)
				{
					SDK::UCrBuildingData*        building   = pair.Key();
					SDK::UCrItemRecipeCollection* collection = pair.Value();
					if (!building || !collection)
						continue;

					std::string buildingName = SDK::UKismetTextLibrary::Conv_TextToString(building->BuildingName).ToString();
					if (buildingName.empty())
						buildingName = building->GetName();

					for (SDK::UCrItemRecipeData* recipe : collection->Recipes)
					{
						if (recipe)
							result.emplace(static_cast<void*>(recipe), buildingName);
					}
				}
			}
			catch (...)
			{
				LOG_DEBUG("CodexRecipes: exception while walking CrafterToRecipeMap.");
			}

			return result;
		}

		void RefreshRecipesOnGameThread(void* /*context*/)
		{
			std::vector<RecipeInfo> recipes;

			LOG_INFO("CodexRecipes: scanning the asset registry for recipes...");

			try
			{
				SDK::UObject* registryObject = CallGetAssetRegistry();
				SDK::IAssetRegistry* registry = registryObject ? reinterpret_cast<SDK::IAssetRegistry*>(registryObject) : nullptr;

				if (!registry)
				{
					LOG_WARN("CodexRecipes: could not resolve the asset registry.");
				}
				else
				{
					SDK::TArray<SDK::FAssetData> assetData;
					if (!CallGetAssetsByClass(registry, MakeRecipeClassPath(), assetData))
						LOG_WARN("CodexRecipes: IAssetRegistry::GetAssetsByClass failed.");

					std::unordered_map<void*, std::string> buildingByRecipe = BuildRecipeToBuildingMap();

					// The asset registry can list the same recipe asset more than
					// once (e.g. redirectors) - resolving always yields the same
					// native UObject*, so dedupe on that.
					std::unordered_set<void*> seenRecipes;

					// Some distinct recipe assets translate to the exact same
					// display name (localization overlap) - ImGui then sees
					// duplicate widget IDs (rows are keyed by displayName) and
					// refuses to render them. Keep only the first recipe for
					// each title.
					std::unordered_set<std::string> seenDisplayNames;

					const int rawCount = assetData.Num();
					for (int i = 0; i < rawCount; ++i)
					{
						SDK::UCrItemRecipeData* recipe = ResolveRecipeAsset(assetData[i]);
						if (!recipe)
							continue;

						if (!seenRecipes.insert(static_cast<void*>(recipe)).second)
							continue;

						RecipeInfo info;
						info.nativeRecipe     = static_cast<void*>(recipe);
						info.buildTimeSeconds = recipe->BuildTime;

						SDK::FAuSimpleItem outputItem;
						try { outputItem = recipe->GetOutputItem(); }
						catch (...) { continue; }

						// Blueprint/schematic unlock recipes aren't real crafting
						// recipes - skip them rather than showing a bogus entry.
						if (IsBlueprintItem(outputItem.ItemDataBase))
							continue;

						info.output = MakeItemRef(outputItem);
						if (LooksBroken(info.output.displayName))
							continue;

						try
						{
							for (const SDK::FAuSimpleItem& resource : recipe->GetNeededResources())
							{
								if (IsBlueprintItem(resource.ItemDataBase))
									continue;
								RecipeItemRef inputRef = MakeItemRef(resource);
								if (LooksBroken(inputRef.displayName))
									continue;
								info.inputs.push_back(std::move(inputRef));
							}
						}
						catch (...) {}

						info.displayName = SDK::UKismetTextLibrary::Conv_TextToString(recipe->DisplayText).ToString();
						if (info.displayName.empty())
							info.displayName = info.output.displayName;
						if (LooksBroken(info.displayName))
							continue;

						if (!seenDisplayNames.insert(info.displayName).second)
							continue;

						if (info.buildTimeSeconds > 0.0f)
							info.outputsPerMinute = (static_cast<float>(info.output.count) / info.buildTimeSeconds) * 60.0f;

						// No known building crafts this recipe - not something a
						// player can actually make, so drop it entirely.
						auto buildingIt = buildingByRecipe.find(info.nativeRecipe);
						if (buildingIt == buildingByRecipe.end())
							continue;
						info.buildingName = buildingIt->second;

						recipes.push_back(std::move(info));
					}

					LOG_INFO("CodexRecipes: resolved %d recipe(s) from %d asset(s), %d building association(s).",
						static_cast<int>(recipes.size()), rawCount, static_cast<int>(buildingByRecipe.size()));
				}
			}
			catch (...)
			{
				LOG_DEBUG("CodexRecipes: exception while resolving recipes via the asset registry.");
			}

			std::sort(recipes.begin(), recipes.end(),
				[](const RecipeInfo& a, const RecipeInfo& b) { return a.displayName < b.displayName; });

			{
				std::lock_guard<std::mutex> lock(g_mutex);
				g_recipes = std::move(recipes);
				g_ready = true;
			}

			g_refreshInFlight.store(false, std::memory_order_release);
		}
	}

	void Init(IPluginSelf* /*self*/)
	{
	}

	void Shutdown()
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		g_recipes.clear();
		g_ready = false;
	}

	void Tick()
	{
	}

	void OnSessionLoaded()
	{
		bool expected = false;
		if (!g_refreshInFlight.compare_exchange_strong(expected, true))
			return;

		IPluginHooks* hooks = GetHooks();
		if (!hooks || !hooks->Engine)
		{
			g_refreshInFlight.store(false, std::memory_order_release);
			return;
		}

		hooks->Engine->PostToGameThread(&RefreshRecipesOnGameThread, nullptr);
	}

	bool IsReady()
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		return g_ready;
	}

	std::vector<RecipeInfo> GetAll()
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		return g_recipes;
	}

	bool FindProducerOfItem(const std::string& uniqueItemName, RecipeInfo& outRecipe)
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		for (const RecipeInfo& recipe : g_recipes)
		{
			if (recipe.output.uniqueItemName == uniqueItemName)
			{
				outRecipe = recipe;
				return true;
			}
		}
		return false;
	}

	std::vector<RecipeInfo> FindConsumersOfItem(const std::string& uniqueItemName)
	{
		std::vector<RecipeInfo> result;

		std::lock_guard<std::mutex> lock(g_mutex);
		for (const RecipeInfo& recipe : g_recipes)
		{
			for (const RecipeItemRef& input : recipe.inputs)
			{
				if (input.uniqueItemName == uniqueItemName)
				{
					result.push_back(recipe);
					break;
				}
			}
		}
		return result;
	}

	bool FindByNativeRecipe(void* nativeRecipe, RecipeInfo& outRecipe)
	{
		if (!nativeRecipe)
			return false;

		std::lock_guard<std::mutex> lock(g_mutex);
		for (const RecipeInfo& recipe : g_recipes)
		{
			if (recipe.nativeRecipe == nativeRecipe)
			{
				outRecipe = recipe;
				return true;
			}
		}
		return false;
	}
}
