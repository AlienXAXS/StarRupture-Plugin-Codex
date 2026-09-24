#include "codex_recipes.h"
#include "Plugin Core/Helpers/plugin_helpers.h"

#include "Chimera_classes.hpp"
#include "Chimera_structs.hpp"
#include "AuCrafting_classes.hpp"
#include "AuCrafting_structs.hpp"
#include "AuItems_classes.hpp"
#include "AuItems_structs.hpp"
#include "AuActorPlacement_classes.hpp"
#include "MassSpawner_classes.hpp"
#include "AssetRegistry_classes.hpp"
#include "AssetRegistry_parameters.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <map>
#include <mutex>
#include <regex>
#include <unordered_map>
#include <unordered_set>

// Builds the recipe list from the crafting buildings themselves rather than
// an asset-registry scan of every UCrItemRecipeData:
//
//  1. Buildings come from the live ACrCraftingRecipeOwner's
//     CrafterToRecipeMap (building -> base recipe collection), topped up
//     by an asset-registry scan of every UCrBuildingData. The game only
//     builds CrafterToRecipeMap from the technology keeper's current
//     building list, which leaves out upgrade-only buildings (Fabricator
//     Tier 2 etc.), so the registry scan resolves their collections itself
//     from the building's Mass entity config.
//
//  2. Each building's recipes are its base collection's Recipes PLUS every
//     collection a GameFeature has linked onto that base collection via
//     UGameFeatureAction_AddItemRecipeCollection
//     (UCrItemRecipeCollectionSubsystem::LinkedCollections). This mirrors
//     UCrItemRecipeCollectionSubsystem::GatherRecipes, minus its
//     "unlocked by the technology keeper" filter - the Codex lists
//     everything, locked or not. Reading only the base Recipes array is what
//     previously hid every GameFeature-added recipe (Arc Reactor, Neutrino
//     Missile, ...).
//
//  3. Buildings are grouped into tiers by asset name (BD_Crafter,
//     BD_CrafterTier2, BD_Crafter_Variant -> one "Fabricator" group with
//     Tier 1 and Tier 2) for the building browser.
//
// Runs on the game thread, posted from OnSessionLoaded (gated to
// ChimeraMain by the caller in plugin.cpp).
namespace CodexRecipes
{
	namespace
	{
		std::mutex                 g_mutex;
		std::vector<RecipeInfo>    g_recipes;
		std::vector<BuildingGroup> g_buildingGroups;
		bool                       g_ready = false;
		std::atomic<bool>          g_refreshInFlight{ false };

		using CollectionList = std::vector<SDK::UCrItemRecipeCollection*>;
		using LinkedCollectionMap = std::unordered_map<SDK::UCrItemRecipeCollection*, CollectionList>;

		struct CraftingBuilding
		{
			SDK::UCrBuildingData*         building   = nullptr;
			SDK::UCrItemRecipeCollection* collection = nullptr;
		};

		std::string ToLower(std::string s)
		{
			std::transform(s.begin(), s.end(), s.begin(),
				[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return s;
		}

		bool LooksBroken(const std::string& text)
		{
			return text.find("MISSING STRING TABLE ENTRY") != std::string::npos;
		}

		// Blueprint/schematic "items" (unlock tokens, not real resources) show
		// up as recipe outputs but aren't things a player actually crafts -
		// filter them out of the Codex.
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

		template <typename T>
		T* FindFirstInstance(const char* className)
		{
			IPluginHooks* hooks = GetHooks();
			IPluginObjectWalker* walker = hooks ? hooks->ObjectWalker : nullptr;
			if (!walker || !walker->IsReady())
				return nullptr;

			PluginObjectInfo info{};
			int total = walker->FindObjectsByClassNameInto(className, PluginObjectLookup_InstanceOnly, &info, 1);
			return total > 0 ? reinterpret_cast<T*>(info.object) : nullptr;
		}

		// Snapshot of UCrItemRecipeCollectionSubsystem::LinkedCollections
		// (base collection -> collections GameFeatures added to it). There's
		// one subsystem per world, and stale worlds can linger until GC, so
		// merge every instance rather than trusting the first.
		LinkedCollectionMap CollectLinkedCollections()
		{
			LinkedCollectionMap result;

			IPluginHooks* hooks = GetHooks();
			IPluginObjectWalker* walker = hooks ? hooks->ObjectWalker : nullptr;
			if (!walker || !walker->IsReady())
				return result;

			constexpr int kMaxSubsystems = 8;
			PluginObjectInfo infos[kMaxSubsystems]{};
			int total = walker->FindObjectsByClassNameInto("CrItemRecipeCollectionSubsystem", PluginObjectLookup_InstanceOnly, infos, kMaxSubsystems);
			total = (std::min)(total, kMaxSubsystems);

			for (int i = 0; i < total; ++i)
			{
				auto* subsystem = reinterpret_cast<SDK::UCrItemRecipeCollectionSubsystem*>(infos[i].object);
				if (!subsystem)
					continue;

				try
				{
					for (auto& pair : subsystem->LinkedCollections)
					{
						SDK::UCrItemRecipeCollection* base = pair.Key();
						if (!base)
							continue;

						CollectionList& linked = result[base];
						for (SDK::UCrItemRecipeCollection* extra : pair.Value().Collections)
						{
							if (extra && std::find(linked.begin(), linked.end(), extra) == linked.end())
								linked.push_back(extra);
						}
					}
				}
				catch (...)
				{
					LOG_DEBUG("CodexRecipes: exception while walking LinkedCollections.");
				}
			}

			return result;
		}

		// Object/package lookup trampolines (same as CodexIcons), resolved once
		// via the AOB-scanned addresses the modloader exposes. Used to load
		// building data / entity config packages nothing has loaded yet.
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
				return false;

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
				LOG_WARN("CodexRecipes: failed to resolve object/package lookup functions - tiered buildings may be missing.");
				return false;
			}
			return true;
		}

		// Finds (loading the package if needed) the asset packageName.assetName.
		SDK::UObject* LoadAsset(const std::string& packageName, const std::string& assetName, SDK::UClass* cls)
		{
			if (packageName.empty() || assetName.empty() || !ResolveEngineLookupFunctions())
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

			SDK::UObject* obj = g_staticFindObjectByName(cls, package, assetNameW.c_str(), false);
			return (obj && obj->IsA(cls)) ? obj : nullptr;
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
				return nullptr;

			SDK::Params::AssetRegistryHelpers_GetAssetRegistry parms{};
			const auto flags = func->FunctionFlags;
			func->FunctionFlags |= 0x400;
			cdo->ProcessEvent(func, &parms);
			func->FunctionFlags = flags;

			return parms.ReturnValue.GetObjectRef();
		}

		// Every UCrBuildingData asset in the game, loaded - including
		// upgrade-only tiers that nothing in the session has touched yet.
		std::vector<SDK::UCrBuildingData*> ScanAllBuildingData()
		{
			std::vector<SDK::UCrBuildingData*> result;

			SDK::UObject* registryObject = CallGetAssetRegistry();
			if (!registryObject)
			{
				LOG_WARN("CodexRecipes: could not resolve the asset registry.");
				return result;
			}

			static SDK::UFunction* func = nullptr;
			if (!func)
				func = SDK::IAssetRegistry::StaticClass()->GetFunction("AssetRegistry", "GetAssetsByClass");
			if (!func)
			{
				LOG_WARN("CodexRecipes: could not resolve IAssetRegistry::GetAssetsByClass.");
				return result;
			}

			SDK::Params::AssetRegistry_GetAssetsByClass parms{};
			parms.ClassPathName = SDK::UKismetSystemLibrary::MakeTopLevelAssetPath(
				SDK::FString(L"/Script/Chimera"), SDK::FString(L"CrBuildingData"));
			parms.bSearchSubClasses = true;

			const auto flags = func->FunctionFlags;
			func->FunctionFlags |= 0x400;
			registryObject->ProcessEvent(func, &parms);
			func->FunctionFlags = flags;

			SDK::UClass* buildingClass = SDK::UCrBuildingData::StaticClass();
			std::unordered_set<SDK::UCrBuildingData*> seen;
			for (const SDK::FAssetData& asset : parms.OutAssetData)
			{
				SDK::UObject* obj = LoadAsset(asset.PackageName.GetRawString(), asset.AssetName.ToString(), buildingClass);
				auto* building = static_cast<SDK::UCrBuildingData*>(obj);
				if (building && seen.insert(building).second)
					result.push_back(building);
			}

			LOG_INFO("CodexRecipes: asset registry lists %d building data asset(s), %d loaded.",
				parms.OutAssetData.Num(), static_cast<int>(result.size()));
			return result;
		}

		// Resolves a building's recipe collection the same way
		// ACrCraftingRecipeOwner::CreateCrafterRecipeMap does: find the
		// UCrBuildingCraftingTrait on its Mass entity config (walking parent
		// configs too) and read CraftingParameters.RecipeCollection. Loads
		// the entity config's package if it isn't in memory yet.
		enum class CollectionLookup { Found, NoConfig, NoCraftingTrait, NoCollection };

		CollectionLookup FindBuildingCollection(SDK::UCrBuildingData* building, SDK::UCrItemRecipeCollection*& outCollection)
		{
			outCollection = nullptr;

			SDK::UMassEntityConfigAsset* config = building->EntityType.EntityConfigPtr;
			if (!config)
				config = building->EntityType.EntityConfig.Get();
			if (!config)
			{
				const auto& path = building->EntityType.EntityConfig.ObjectID.AssetPath;
				config = static_cast<SDK::UMassEntityConfigAsset*>(LoadAsset(
					path.PackageName.GetRawString(), path.AssetName.ToString(), SDK::UMassEntityConfigAsset::StaticClass()));
			}
			if (!config)
				return CollectionLookup::NoConfig;

			SDK::UClass* traitClass = SDK::UCrBuildingCraftingTrait::StaticClass();
			for (int depth = 0; config && depth < 8; ++depth)
			{
				for (SDK::UMassEntityTraitBase* trait : config->Config.Traits)
				{
					if (trait && trait->IsA(traitClass))
					{
						outCollection = static_cast<SDK::UCrBuildingCraftingTrait*>(trait)->CraftingParameters.RecipeCollection;
						return outCollection ? CollectionLookup::Found : CollectionLookup::NoCollection;
					}
				}
				config = config->Config.Parent;
			}

			return CollectionLookup::NoCraftingTrait;
		}

		// Buildings that can't craft anything - skipped before touching their
		// entity config so the scan doesn't load hundreds of drone lanes,
		// walls and foundations for nothing.
		bool CanSkipBuildingType(SDK::UCrBuildingData* building)
		{
			switch (building->Type)
			{
			case SDK::ECrBuildingType::Habitat:
			case SDK::ECrBuildingType::Power:
			case SDK::ECrBuildingType::Transport:
			case SDK::ECrBuildingType::Defensive:
			case SDK::ECrBuildingType::CustomBuilding:
			case SDK::ECrBuildingType::TemperatureManagement:
			case SDK::ECrBuildingType::Test:
				return true;
			default:
				return false;
			}
		}

		std::vector<CraftingBuilding> CollectCraftingBuildings()
		{
			std::vector<CraftingBuilding> result;
			std::unordered_set<SDK::UCrBuildingData*> seen;

			if (auto* owner = FindFirstInstance<SDK::ACrCraftingRecipeOwner>("CrCraftingRecipeOwner"))
			{
				try
				{
					for (auto& pair : owner->CrafterToRecipeMap)
					{
						SDK::UCrBuildingData*         building   = pair.Key();
						SDK::UCrItemRecipeCollection* collection = pair.Value();
						if (building && collection && seen.insert(building).second)
							result.push_back({ building, collection });
					}
				}
				catch (...)
				{
					LOG_DEBUG("CodexRecipes: exception while walking CrafterToRecipeMap.");
				}
			}
			else
			{
				LOG_DEBUG("CodexRecipes: no CrCraftingRecipeOwner instance found.");
			}

			const size_t fromOwner = result.size();

			int skippedType = 0, noConfig = 0, noTrait = 0, noCollection = 0;

			try
			{
				for (SDK::UCrBuildingData* building : ScanAllBuildingData())
				{
					if (seen.count(building))
						continue;
					if (CanSkipBuildingType(building))
					{
						++skippedType;
						continue;
					}

					SDK::UCrItemRecipeCollection* collection = nullptr;
					CollectionLookup lookup = CollectionLookup::NoConfig;
					try { lookup = FindBuildingCollection(building, collection); }
					catch (...) {}

					switch (lookup)
					{
					case CollectionLookup::NoConfig:        ++noConfig;     continue;
					case CollectionLookup::NoCraftingTrait: ++noTrait;      continue;
					case CollectionLookup::NoCollection:    ++noCollection; continue;
					case CollectionLookup::Found:           break;
					}

					seen.insert(building);
					result.push_back({ building, collection });
					LOG_INFO("CodexRecipes: + %s (type %d) -> %s",
						building->GetName().c_str(), static_cast<int>(building->Type), collection->GetName().c_str());
				}
			}
			catch (...)
			{
				LOG_WARN("CodexRecipes: exception while scanning building data assets.");
			}

			LOG_INFO("CodexRecipes: found %d crafting building(s) (%d from the recipe owner, %d from the asset scan); "
				"skipped %d non-crafting type(s), %d without an entity config, %d without a crafting trait, %d without a collection.",
				static_cast<int>(result.size()), static_cast<int>(fromOwner), static_cast<int>(result.size() - fromOwner),
				skippedType, noConfig, noTrait, noCollection);

			return result;
		}

		// Mirrors UCrItemRecipeCollectionSubsystem::GatherRecipes: linked
		// (GameFeature-added) collections first, then the base collection.
		std::vector<SDK::UCrItemRecipeData*> GatherRecipes(SDK::UCrItemRecipeCollection* collection, const LinkedCollectionMap& linked)
		{
			std::vector<SDK::UCrItemRecipeData*> result;
			std::unordered_set<SDK::UCrItemRecipeData*> seen;

			auto append = [&](SDK::UCrItemRecipeCollection* source)
			{
				if (!source)
					return;
				for (SDK::UCrItemRecipeData* recipe : source->Recipes)
				{
					if (recipe && seen.insert(recipe).second)
						result.push_back(recipe);
				}
			};

			auto it = linked.find(collection);
			if (it != linked.end())
			{
				for (SDK::UCrItemRecipeCollection* extra : it->second)
					append(extra);
			}
			append(collection);

			return result;
		}

		// Returns false for recipes that shouldn't appear in the Codex
		// (blueprint unlocks, placeholder/untranslated entries).
		bool BuildRecipeInfo(SDK::UCrItemRecipeData* recipe, RecipeInfo& info)
		{
			// Buildings without real recipes yet (Chemical Processor, Player, ...)
			// ship a CR_PlaceholderCraftingRecipe - not something to list.
			if (recipe->GetName().find("Placeholder") != std::string::npos)
				return false;

			info.nativeRecipe     = static_cast<void*>(recipe);
			info.buildTimeSeconds = recipe->BuildTime;

			SDK::FAuSimpleItem outputItem;
			try { outputItem = recipe->GetOutputItem(); }
			catch (...) { return false; }

			if (!outputItem.ItemDataBase || IsBlueprintItem(outputItem.ItemDataBase))
				return false;

			info.output = MakeItemRef(outputItem);
			if (LooksBroken(info.output.displayName))
				return false;

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
			if (info.displayName.empty() || LooksBroken(info.displayName))
				info.displayName = info.output.displayName;

			if (info.buildTimeSeconds > 0.0f)
				info.outputsPerMinute = (static_cast<float>(info.output.count) / info.buildTimeSeconds) * 60.0f;

			return true;
		}

		// Splits a building asset name into a tier-independent key and a tier:
		//   BD_Crafter          -> BD_Crafter, 1
		//   BD_CrafterTier2     -> BD_Crafter, 2
		//   BD_Crafter_Variant  -> BD_Crafter, 1
		//   BD_Something_T3     -> BD_Something, 3
		void ParseBuildingAssetName(const std::string& assetName, std::string& outKey, int& outTier)
		{
			static const std::regex kVariantSuffix(R"(_?variants?$)", std::regex::icase);
			// Case-sensitive: asset names are PascalCase, and this must not
			// match inside words like "Frontier".
			static const std::regex kTierWord(R"(_?Tier_?(\d*))");
			static const std::regex kTierShort(R"(_T(\d+)(?=_|$))");

			std::string key = std::regex_replace(assetName, kVariantSuffix, "");
			int tier = 1;

			std::smatch match;
			if (std::regex_search(key, match, kTierWord) || std::regex_search(key, match, kTierShort))
			{
				if (match[1].matched && match[1].length() > 0)
					tier = (std::max)(1, std::atoi(match[1].str().c_str()));
				key = match.prefix().str() + match.suffix().str();
			}

			outKey  = ToLower(key);
			outTier = tier;
		}

		// "Fabricator T2" / "Fabricator Tier 2" / "Fabricator Mk.2" /
		// "Fabricator II" -> "Fabricator". Only used when a group has no tier-1
		// building to borrow the base name from.
		std::string StripTierFromDisplayName(const std::string& name)
		{
			static const std::regex kTierSuffix(
				R"((\s+|\s*[\(\[]\s*)(tier|t|mk\.?|mark)\s*\d+\s*[\)\]]?\s*$|\s+(ii|iii|iv)\s*$)",
				std::regex::icase);
			std::string stripped = std::regex_replace(name, kTierSuffix, "");
			return stripped.empty() ? name : stripped;
		}

		std::string GetBuildingDisplayName(SDK::UCrBuildingData* building)
		{
			std::string name = SDK::UKismetTextLibrary::Conv_TextToString(building->BuildingName).ToString();
			if (name.empty() || LooksBroken(name))
				name = building->GetName();
			return name;
		}

		void RefreshRecipesOnGameThread(void* /*context*/)
		{
			std::vector<RecipeInfo>    recipes;
			std::vector<BuildingGroup> groups;

			LOG_INFO("CodexRecipes: scanning crafting buildings for recipes...");

			try
			{
				const LinkedCollectionMap linked = CollectLinkedCollections();
				const std::vector<CraftingBuilding> buildings = CollectCraftingBuildings();

				int linkedCount = 0;
				for (const auto& entry : linked)
					linkedCount += static_cast<int>(entry.second.size());

				// Per building: tier key/tier/display name/recipes. Built first so
				// group names (which borrow the tier-1 display name) can be
				// resolved before any recipe learns which building it's made in.
				struct ParsedBuilding
				{
					std::string key;
					int         tier = 1;
					std::string displayName;
					std::vector<SDK::UCrItemRecipeData*> recipes;
				};
				std::vector<ParsedBuilding> parsed;
				parsed.reserve(buildings.size());

				for (const CraftingBuilding& entry : buildings)
				{
					ParsedBuilding pb;
					try
					{
						ParseBuildingAssetName(entry.building->GetName(), pb.key, pb.tier);
						pb.displayName = GetBuildingDisplayName(entry.building);
						pb.recipes     = GatherRecipes(entry.collection, linked);
					}
					catch (...)
					{
						continue;
					}
					if (!pb.recipes.empty())
						parsed.push_back(std::move(pb));
				}

				std::unordered_map<std::string, std::string> tierOneNameByKey;
				for (const ParsedBuilding& pb : parsed)
				{
					if (pb.tier == 1)
						tierOneNameByKey.emplace(pb.key, pb.displayName);
				}

				// groupName -> tier -> recipes (deduped, in first-seen order).
				// Keyed by display name too, so e.g. two different "Food
				// Processor" assets end up as one entry in the browser.
				std::map<std::string, std::map<int, std::vector<SDK::UCrItemRecipeData*>>> grouped;
				for (const ParsedBuilding& pb : parsed)
				{
					auto nameIt = tierOneNameByKey.find(pb.key);
					const std::string groupName = nameIt != tierOneNameByKey.end()
						? nameIt->second
						: StripTierFromDisplayName(pb.displayName);

					std::vector<SDK::UCrItemRecipeData*>& tierRecipes = grouped[groupName][pb.tier];
					for (SDK::UCrItemRecipeData* recipe : pb.recipes)
					{
						if (std::find(tierRecipes.begin(), tierRecipes.end(), recipe) == tierRecipes.end())
							tierRecipes.push_back(recipe);
					}
				}

				std::unordered_map<SDK::UCrItemRecipeData*, size_t> recipeIndex;
				std::unordered_set<SDK::UCrItemRecipeData*> rejected;

				for (auto& [groupName, tiers] : grouped)
				{
					BuildingGroup group;
					group.displayName = groupName;

					for (auto& [tier, tierRecipes] : tiers)
					{
						BuildingTier bt;
						bt.tier  = tier;
						bt.label = tiers.size() > 1
							? groupName + " (Tier " + std::to_string(tier) + ")"
							: groupName;

						for (SDK::UCrItemRecipeData* recipe : tierRecipes)
						{
							if (rejected.count(recipe))
								continue;

							auto it = recipeIndex.find(recipe);
							if (it == recipeIndex.end())
							{
								RecipeInfo info;
								bool ok = false;
								try { ok = BuildRecipeInfo(recipe, info); }
								catch (...) { ok = false; }
								if (!ok)
								{
									rejected.insert(recipe);
									continue;
								}
								it = recipeIndex.emplace(recipe, recipes.size()).first;
								recipes.push_back(std::move(info));
							}

							recipes[it->second].buildings.push_back({ groupName, tier, bt.label });
							bt.recipes.push_back(static_cast<void*>(recipe));
						}

						if (!bt.recipes.empty())
							group.tiers.push_back(std::move(bt));
					}

					if (!group.tiers.empty())
						groups.push_back(std::move(group));
				}

				for (RecipeInfo& info : recipes)
				{
					std::stable_sort(info.buildings.begin(), info.buildings.end(),
						[](const BuildingRef& a, const BuildingRef& b) { return a.tier < b.tier; });
					info.minTier      = info.buildings.front().tier;
					info.buildingName = info.buildings.front().label;
				}

				// Flag recipes whose name also appears at another tier (e.g.
				// Calcite Sheets in Fabricator Tier 1 and Tier 2) so lists can
				// label them "(Tier N)" to tell them apart.
				std::unordered_map<std::string, std::unordered_set<int>> tiersByName;
				for (const RecipeInfo& info : recipes)
					tiersByName[info.displayName].insert(info.minTier);
				for (RecipeInfo& info : recipes)
					info.hasTierVariants = tiersByName[info.displayName].size() > 1;

				LOG_INFO("CodexRecipes: resolved %d recipe(s) across %d building group(s); %d GameFeature-linked collection(s).",
					static_cast<int>(recipes.size()), static_cast<int>(groups.size()), linkedCount);
			}
			catch (...)
			{
				LOG_DEBUG("CodexRecipes: exception while resolving recipes.");
			}

			std::sort(recipes.begin(), recipes.end(),
				[](const RecipeInfo& a, const RecipeInfo& b)
				{
					if (a.displayName != b.displayName)
						return a.displayName < b.displayName;
					return a.minTier < b.minTier;
				});

			// Recipes within each tier, sorted by display name for the browser.
			std::unordered_map<void*, const RecipeInfo*> byNative;
			for (const RecipeInfo& info : recipes)
				byNative.emplace(info.nativeRecipe, &info);

			for (BuildingGroup& group : groups)
			{
				for (BuildingTier& bt : group.tiers)
				{
					std::stable_sort(bt.recipes.begin(), bt.recipes.end(),
						[&](void* a, void* b) { return byNative[a]->displayName < byNative[b]->displayName; });
				}
			}

			{
				std::lock_guard<std::mutex> lock(g_mutex);
				g_recipes        = std::move(recipes);
				g_buildingGroups = std::move(groups);
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
		g_buildingGroups.clear();
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

	std::vector<BuildingGroup> GetBuildingGroups()
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		return g_buildingGroups;
	}

	bool FindProducerOfItem(const std::string& uniqueItemName, RecipeInfo& outRecipe)
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		const RecipeInfo* best = nullptr;
		for (const RecipeInfo& recipe : g_recipes)
		{
			if (recipe.output.uniqueItemName != uniqueItemName)
				continue;
			if (!best || recipe.minTier < best->minTier)
				best = &recipe;
		}
		if (!best)
			return false;
		outRecipe = *best;
		return true;
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
