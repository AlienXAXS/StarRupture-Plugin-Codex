#include "codex_icons.h"
#include "Plugin Core/Helpers/plugin_helpers.h"
#include "Plugin Core/codex_fields.h"

#include "Chimera_classes.hpp"
#include "AuItems_classes.hpp"
#include "Engine_classes.hpp"
#include "AssetRegistry_classes.hpp"
#include "AssetRegistry_parameters.hpp"

#include "icon_requests.h"

#include <atomic>
#include <mutex>
#include <stdexcept>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>

namespace CodexIcons
{
	namespace
	{
		static_assert(std::is_same_v<CodexIconRequests::Handle, PluginTextureHandle>,
			"the request table stores plugin texture handles");

		// What it takes to find an item's icon again: the item's Blueprint asset,
		// and the icon's identity for comparing with another brush's texture.
		// Plain strings, so nothing here can go stale when the game frees objects.
		struct ItemSource
		{
			std::string package;
			std::string asset;
			std::string textureId;
		};

		// Populated on the game thread by RefreshIconsOnGameThread, adopted by
		// Tick() (also game thread, via OnEngineTick).
		std::mutex                                   g_pendingMutex;
		std::unordered_map<std::string, ItemSource>  g_pendingSources;
		bool                                         g_pendingReady = false;
		std::atomic<bool>                            g_refreshInFlight{ false };

		// Game thread only.
		std::unordered_map<std::string, ItemSource> g_sources;
		bool                                        g_iconsLoaded = false;

		// Asked for by the render callback, served by Tick().
		CodexIconRequests::Table g_requests;

		double NowMs()
		{
			static const double perMs = []
			{
				LARGE_INTEGER frequency;
				QueryPerformanceFrequency(&frequency);
				return static_cast<double>(frequency.QuadPart) / 1000.0;
			}();
			LARGE_INTEGER now;
			QueryPerformanceCounter(&now);
			return static_cast<double>(now.QuadPart) / perMs;
		}

		// The item's icon if it is a UTexture2D. ResourceObject might be a
		// UTexture, a UMaterialInterface, etc. - the IsA check is the safe gate.
		// The pointer is good for the tick that fetched it and no longer.
		SDK::UTexture2D* GetItemIconTexture(SDK::UAuItemDataBase* item)
		{
			const auto* brush = CodexFields::ItemIcon.In(item);
			SDK::UObject* resource = brush ? brush->ResourceObject : nullptr;
			if (!resource)
				return nullptr;

			try
			{
				return resource->IsA(SDK::UTexture2D::StaticClass()) ? static_cast<SDK::UTexture2D*>(resource) : nullptr;
			}
			catch (...)
			{
				return nullptr;
			}
		}

		// Every item type is defined as a Blueprint (UAuItemBlueprint asset whose
		// GeneratedClass's CDO is the actual UAuItemDataBase) - mirrors how the
		// game's own UCrUW_CheatItemsTab::DebugGatherAllItems enumerates the full
		// item list via the asset registry.
		SDK::FTopLevelAssetPath MakeItemBlueprintClassPath()
		{
			SDK::FString packagePath(L"/Script/AuItems");
			SDK::FString className(L"AuItemBlueprint");
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
				LOG_WARN("CodexIcons: could not resolve UAssetRegistryHelpers::GetAssetRegistry.");
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
				LOG_WARN("CodexIcons: could not resolve IAssetRegistry::GetAssetsByClass.");
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

		// Raw CoreUObject object/package lookup and loading functions - resolved
		// once via the AOB-scanned addresses the modloader exposes on
		// IPluginEngineEvents (same approach as ProductionViewer's icon loader).
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
				LOG_WARN("CodexIcons: engine events interface unavailable - cannot resolve object/package lookup functions.");
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
				LOG_WARN("CodexIcons: failed to resolve object/package lookup functions - icon list will be incomplete.");
				return false;
			}

			return true;
		}

		// Forces the Blueprint's package to load and resolves it down to the
		// UAuItemDataBase CDO that actually holds the item's icon: package ->
		// generated "<AssetName>_C" class -> ClassDefaultObject.
		SDK::UAuItemDataBase* ResolveItemFromBlueprintAsset(const std::string& assetName, const std::string& packageName)
		{
			if (!ResolveEngineLookupFunctions())
				return nullptr;

			if (packageName.empty())
				return nullptr;

			const std::wstring packageNameW(packageName.begin(), packageName.end());
			const std::wstring generatedClassNameW = std::wstring(assetName.begin(), assetName.end()) + L"_C";

			SDK::UPackage* package = g_findPackage(nullptr, packageNameW.c_str());
			if (package)
				g_packageFullyLoad(package);
			else
				package = g_loadPackage(nullptr, packageNameW.c_str(), 0, nullptr, nullptr);

			if (!package)
				return nullptr;

			SDK::UObject* generatedObj = g_staticFindObjectByName(SDK::UClass::StaticClass(), package, generatedClassNameW.c_str(), false);
			if (!generatedObj)
				return nullptr;

			SDK::UClass* generatedClass = static_cast<SDK::UClass*>(generatedObj);
			SDK::UObject* cdo = generatedClass->ClassDefaultObject;
			if (!cdo || !cdo->IsA(SDK::UAuItemDataBase::StaticClass()))
				return nullptr;

			return static_cast<SDK::UAuItemDataBase*>(cdo);
		}

		// Enumerates every item type via the asset registry (mirrors
		// UCrUW_CheatItemsTab::DebugGatherAllItems) and keeps, for each one that
		// has a texture icon, the strings that find it again. Runs on the game
		// thread (posted via Init/Tick); publishes its result through
		// g_pendingSources for Tick() to adopt. Copies no texture.
		void RefreshIconsOnGameThread(void* /*context*/)
		{
			std::unordered_map<std::string, ItemSource> sources;

			IPluginSelf* self = GetSelf();
			IPluginSplash* splash = (self && self->hooks) ? self->hooks->Splash : nullptr;
			if (splash && splash->IsVisible())
			{
				splash->SetSubStatus("Codex: scanning item icons...");
				splash->SetSubProgress(0.0f);
			}

			LOG_INFO("CodexIcons: scanning the asset registry for item icons...");

			try
			{
				SDK::UObject* registryObject = CallGetAssetRegistry();
				SDK::IAssetRegistry* registry = registryObject ? reinterpret_cast<SDK::IAssetRegistry*>(registryObject) : nullptr;

				if (!registry)
				{
					LOG_WARN("CodexIcons: could not resolve the asset registry.");
				}
				else
				{
					SDK::TArray<SDK::FAssetData> assetData;
					if (!CallGetAssetsByClass(registry, MakeItemBlueprintClassPath(), assetData))
						LOG_WARN("CodexIcons: IAssetRegistry::GetAssetsByClass failed.");

					const int rawCount = assetData.Num();
					for (int i = 0; i < rawCount; ++i)
					{
						ItemSource source;
						source.asset   = assetData[i].AssetName.ToString();
						source.package = assetData[i].PackageName.GetRawString();

						SDK::UAuItemDataBase* item = ResolveItemFromBlueprintAsset(source.asset, source.package);
						if (!item)
							continue;

						const std::string uniqueName = CodexFields::UniqueNameOf(item);
						if (uniqueName.empty())
							continue;

						SDK::UTexture2D* icon = GetItemIconTexture(item);
						if (!icon)
							continue;

						source.textureId = icon->GetFullName();
						sources.emplace(uniqueName, std::move(source));
					}

					LOG_INFO("CodexIcons: resolved %d item icon(s) from %d Blueprint asset(s).",
						static_cast<int>(sources.size()), rawCount);
				}
			}
			catch (...)
			{
				LOG_DEBUG("CodexIcons: exception while resolving item icons via the asset registry.");
			}

			{
				std::lock_guard<std::mutex> lock(g_pendingMutex);
				g_pendingSources = std::move(sources);
				g_pendingReady = true;
				g_refreshInFlight.store(false, std::memory_order_release);
			}

			if (splash)
			{
				splash->ClearSubBar();
				splash->ReleaseSplashHold();
			}
		}

		// Triggers an asynchronous icon-list refresh on the game thread. Safe to
		// call repeatedly (from Init or every Tick) - coalesces into a single
		// in-flight request via g_refreshInFlight.
		void RequestRefresh()
		{
			bool expected = false;
			if (!g_refreshInFlight.compare_exchange_strong(expected, true))
				return;

			IPluginSelf* self = GetSelf();
			IPluginHooks* hooks = self ? self->hooks : nullptr;
			if (!hooks || !hooks->Engine || !hooks->ImGuiTextures)
			{
				g_refreshInFlight.store(false, std::memory_order_release);
				return;
			}

			hooks->Engine->PostToGameThread(&RefreshIconsOnGameThread, nullptr);
		}

		void AdoptPendingIconsIfReady()
		{
			std::unordered_map<std::string, ItemSource> incoming;
			{
				std::lock_guard<std::mutex> lock(g_pendingMutex);
				if (!g_pendingReady)
					return;

				incoming = std::move(g_pendingSources);
				g_pendingSources.clear();
				g_pendingReady = false;
			}

			std::unordered_set<std::string> names;
			for (const auto& [name, source] : incoming)
				names.insert(name);

			g_sources = std::move(incoming);
			g_requests.SetKnown(std::move(names));
			g_iconsLoaded = true;
			LOG_INFO("CodexIcons: %d item icon(s) ready to load on demand.", static_cast<int>(g_sources.size()));
		}

		void LogGaveUp(const std::string& uniqueItemName)
		{
			LOG_WARN("CodexIcons: giving up on the icon for '%s'.", uniqueItemName.c_str());
		}

		// Finds the item's texture again and copies it into the loader. Returns
		// null while the texture is not ready (D3D12 not up, texture still
		// streaming, an object already dying), which Serve() retries on a later
		// tick. When the loader has no free texture slot it throws
		// std::out_of_range, and that icon is given up on at once: a slot does
		// not free up by waiting. A texture another plugin already holds is found
		// by name before the loader looks for a slot, so it still loads from a
		// full table. The texture pointer lives only inside this call.
		PluginTextureHandle LoadIcon(const std::string& uniqueItemName)
		{
			IPluginHooks* hooks = GetHooks();
			const auto source = g_sources.find(uniqueItemName);
			if (!hooks || !hooks->ImGuiTextures || source == g_sources.end())
				return nullptr;

			try
			{
				SDK::UAuItemDataBase* item = ResolveItemFromBlueprintAsset(source->second.asset, source->second.package);
				if (!item || !SDK::UKismetSystemLibrary::IsValid(item))
					return nullptr;
				SDK::UTexture2D* icon = GetItemIconTexture(item);
				if (!icon || !SDK::UKismetSystemLibrary::IsValid(icon))
					return nullptr;

				const std::string texName = icon->GetName();
				return hooks->ImGuiTextures->LoadFromUTexture2D(icon, texName.c_str());
			}
			catch (const std::out_of_range&)
			{
				if (g_requests.GiveUp(uniqueItemName))
					LogGaveUp(uniqueItemName);
				return nullptr;
			}
			catch (...)
			{
				return nullptr;
			}
		}
	}

	void Init(IPluginSelf* self)
	{
		IPluginSplash* splash = (self && self->hooks) ? self->hooks->Splash : nullptr;
		if (splash && splash->IsVisible())
			splash->AcquireSplashHold();

		IPluginHooks* hooks = self ? self->hooks : nullptr;
		if (!hooks || !hooks->Engine || !hooks->ImGuiTextures)
		{
			if (splash && splash->IsVisible())
				splash->ReleaseSplashHold();
			LOG_WARN("CodexIcons: ImGui texture hooks unavailable (server/generic build) - icons disabled.");
			return;
		}

		RequestRefresh();
	}

	void Shutdown()
	{
		IPluginHooks* hooks = GetHooks();
		for (PluginTextureHandle handle : g_requests.Clear())
			if (hooks && hooks->ImGuiTextures)
				hooks->ImGuiTextures->FreeTexture(handle);

		{
			std::lock_guard<std::mutex> lock(g_pendingMutex);
			g_pendingSources.clear();
			g_pendingReady = false;
		}
		g_sources.clear();
		g_iconsLoaded = false;
	}

	void Tick()
	{
		AdoptPendingIconsIfReady();

		// Initial scan hasn't completed yet (e.g. the Init-time post never ran) -
		// keep retrying every tick until it succeeds.
		if (!g_iconsLoaded)
		{
			RequestRefresh();
			return;
		}

		CodexIconRequests::Serve(g_requests, &LoadIcon, &NowMs, CodexIconRequests::kBudgetMs, &LogGaveUp);
	}

	PluginTextureHandle GetIcon(const std::string& uniqueItemName)
	{
		return g_requests.Get(uniqueItemName);
	}

	bool HasIcon(const std::string& uniqueItemName)
	{
		return g_requests.Has(uniqueItemName);
	}

	bool IconIs(const std::string& uniqueItemName, const std::string& textureId)
	{
		const auto it = g_sources.find(uniqueItemName);
		return it != g_sources.end() && it->second.textureId == textureId;
	}
}
