#include "codex_planner.h"
#include "Plugin Core/Config/plugin_config.h"
#include "Plugin Core/Helpers/plugin_helpers.h"

#include <cctype>
#include <cstring>
#include <unordered_map>

#include <shellapi.h>

namespace CodexPlanner
{
	namespace
	{
		constexpr const char* kCustomPlannerName = "Custom";
		constexpr size_t      kMaxUrlLength = 2048;

		// The StarRupture Planner's item catalog (update2_QoL,
		// public/game-data/update2_QoL/items_catalog.json): planner id and the
		// in-game display name. Game items are matched to it by comparing
		// letters and digits only, against the game's UniqueItemName first and
		// its display name second ("CalciumBlock" and "Calcium Block" are the
		// planner's "block_calcium").
		//
		// The other columns are the same item's id on starruptureplanner.com
		// (its /database page) and starrupturecalc.com (its /items/ pages), found
		// by the display name. starruptureplannerId is the id its calculator takes;
		// starruptureplannerPageId is the id of its /items/ page, filled only where
		// the calculator has no id (it has no recipe for the item). They are empty
		// where that site has no such item, so the planner opens without an item.
		struct PlannerItem
		{
			const char* id;
			const char* name;
			const char* starruptureplannerId;
			const char* starruptureplannerPageId;
			const char* starrupturecalcId;
		};

		constexpr PlannerItem kPlannerItems[] = {
			{ "accumulator", "Accumulator", "", "comp_accumulator", "accumulator" },
			{ "aerogel", "Aerogel", "", "comp_aerogel", "aerogel" },
			{ "airlock", "Airlock", "", "comp_airlock", "airlock" },
			{ "anti-radiationcovers", "Anti-Radiation Covers", "", "comp_anti_radiation_covers", "anti-radiation-covers" },
			{ "applicator", "Applicator", "comp_applicator", "", "syringe" },
			{ "basic_building", "Basic Building Material", "item_bbm", "", "basic-building-material" },
			{ "basicfuel", "Basic Fuel", "", "item_basic_fuel", "basic-fuel" },
			{ "battery", "Battery", "comp_battery", "", "battery" },
			{ "biofilament", "Biofilament", "", "comp_biofilament", "biofilament" },
			{ "calcite_sheets", "Calcite Sheets", "sheet_calcite", "", "calcite-sheets" },
			{ "block_calcium", "Calcium Block", "block_calcium", "", "calcium-block" },
			{ "ore_calcium", "Calcium Ore", "", "ore_calcium", "calcium-ore" },
			{ "powder_calcium", "Calcium Powder", "powder_calcium", "", "calcium-powder" },
			{ "carbonsonar", "Carbon Sonar", "", "comp_carbon_sonar", "carbon-sonar" },
			{ "ceramics", "Ceramics", "comp_ceramics", "", "ceramics" },
			{ "chemicals", "Chemicals", "comp_chemicals", "", "explosive" },
			{ "condenser", "Condenser", "", "comp_condenser", "condenser" },
			{ "converter", "Converter", "", "comp_converter", "converter" },
			{ "cyclotrone", "Cyclotrone", "", "comp_cyclotrone", "cyclotrone" },
			{ "dome", "Dome", "", "comp_dome", "dome" },
			{ "electromagnet", "Electromagnet", "", "comp_electromagnet", "electromagnet" },
			{ "electromagnetic_coil", "Electromagnetic Coil", "comp_electromagnetic_coil", "", "electromagnetic-coil" },
			{ "electronics", "Electronics", "comp_electronics", "", "electronics" },
			{ "epoxy", "Epoxy", "", "comp_epoxy", "epoxy" },
			{ "grenade", "Explosive Charge", "", "item_explosive_charge", "grenade" },
			{ "generator", "Generator", "", "comp_generator", "generator" },
			{ "glass", "Glass", "comp_glass", "", "glass" },
			{ "goethiteingot", "Goethite Ingot", "", "ingot_goethite", "goethite-ingot" },
			{ "goethitelattice", "Goethite Lattice", "", "comp_goethite_lattice", "" },
			{ "goethiteore", "Goethite Ore", "", "ore_goethite", "goethite-ore" },
			{ "goethitepowder", "Goethite Powder", "", "powder_goethite", "goethite-powder" },
			{ "hardening_agent", "Hardening Agent", "comp_hardening_agent", "", "hardening-agent" },
			{ "heat_sheet", "Heat Resistant Sheet", "sheet_heat_resistant", "", "heat-resistant-sheet" },
			{ "heatshield", "Heat Shield", "", "comp_heat_shield", "heat-shield" },
			{ "heavy_ammo", "Heavy Ammo", "ammo_heavy", "", "heavy-ammo-item" },
			{ "helium", "Helium-3", "", "gas_helium3", "helium-ore" },
			{ "impeller", "Impeller", "comp_impeller", "", "impeller" },
			{ "inductor", "Inductor", "comp_inductor", "", "coil" },
			{ "intermediate_building", "Intermediate Building Material", "item_ibm", "", "intermediate-building-material" },
			{ "ionbomb", "Ion Bomb", "", "item_ion_bomb", "ion-bomb" },
			{ "ion_injector", "Ion Injector", "", "comp_ion_injector", "ion-injector" },
			{ "ionstabilizer", "Ion Stabilizer", "", "comp_ion_stabilizer", "ion-stabilizer" },
			{ "ionthrusters", "Ion Thrusters", "", "comp_ion_thrusters", "ion-thrusters" },
			{ "laser_emitter", "Laser Emitter", "", "comp_laser_emitter", "laser-emitter" },
			{ "lens", "Lens", "", "comp_lens", "lens" },
			{ "liquid_helium", "Liquid Helium", "", "comp_liquid_helium", "liquid-helium" },
			{ "nanofiber", "Nanofiber", "", "", "nanofibre" },
			{ "nanosyringe", "Nanosyringe", "", "comp_nanosyringe", "nanosyringe" },
			{ "nozzle", "Nozzle", "comp_nozzle", "", "nozzle" },
			{ "onboardinstruments", "Onboard Instruments", "", "comp_onboard_instruments", "onboard-instruments" },
			{ "organiccompound", "Organic Compound", "", "comp_organic_compound", "organic-compound" },
			{ "pistol_ammo", "Pistol Ammo", "ammo_pistol", "", "pistol-ammo-item" },
			{ "uberoil", "Plastic", "", "comp_plastic", "uber-oil" },
			{ "plasticdust", "Plastic Dust", "", "item_plastic_dust", "plastic-dust" },
			{ "magicoilore", "Powerium", "", "", "" },
			{ "pressure_tank", "Pressure Tank", "", "comp_pressure_tank", "pressure-tank" },
			{ "pressurized_helium", "Pressurized Helium", "fluid_pressurized_helium", "", "pressurized-helium" },
			{ "pseudorubberinsulator", "Pseudorubber Insulator", "", "comp_pseudorubber_insulator", "pseudorubber-insulator" },
			{ "pump", "Pump", "comp_pump", "", "pump" },
			{ "quantumtranqualizer", "Quantum Tranquilizer", "", "comp_quantum_tranquilizer", "quantum-tranqualizer" },
			{ "rnastabilizer", "RNA Stabilizer", "", "comp_rna_stabilizer", "rna-stabilizer" },
			{ "rtsc", "RTSC", "", "item_rtsc", "rtsc" },
			{ "rtscgenerator", "RTSC Generator", "", "item_rtsc_generator", "rtsc-generator" },
			{ "refinedoil", "Refined Oil", "", "", "" },
			{ "reinforcedframe", "Reinforced Frame", "", "comp_reinforced_frame", "reinforced-frame" },
			{ "reinforcedhousing", "Reinforced Housing", "", "comp_reinforced_housing", "reinforced-housing" },
			{ "resonator", "Resonator", "", "", "resonator" },
			{ "rocketengine", "Rocket Engine", "", "comp_rocket_engine", "rocket-engine" },
			{ "rocketfuel", "Rocket Fuel", "", "comp_rocket_fuel", "rocket-fuel" },
			{ "rotor", "Rotor", "comp_rotor", "", "rotor" },
			{ "satellitebody", "Satellite Body", "", "comp_satellite_body", "satellite-body" },
			{ "scafolding", "Scaffolding", "", "comp_scaffolding", "scafolding" },
			{ "scanner", "Scanner", "", "comp_scanner", "scanner" },
			{ "shotgun_ammo", "Shotgun Ammo", "ammo_shotgun", "", "shotgun-ammo-item" },
			{ "stabilizer", "Stabilizer", "comp_stabilizer", "", "gyroscope" },
			{ "standard_ammo", "Standard Ammo", "ammo_standard", "", "standard-ammo-item" },
			{ "stator", "Stator", "comp_stator", "", "stator" },
			{ "substancefhs-135-493", "Substance #FHS-135-493", "", "comp_fhs_135_493", "substance-fhs-135-493" },
			{ "ore_sulfur", "Sulfur Ore", "", "ore_sulfur", "sulphur-ore" },
			{ "sulfuric_acid", "Sulfuric Acid", "fluid_sulfuric_acid", "", "sulphuric-acid" },
			{ "superconductor", "Superconductor", "", "comp_superconductor", "superconductor" },
			{ "supermagnet", "Supermagnet", "comp_supermagnet", "", "supermagnet" },
			{ "syntheticdna", "Synthetic DNA", "", "comp_synthetic_dna", "synthetic-dna" },
			{ "syntheticprotein", "Synthetic Protein", "", "comp_synthetic_protein", "synthetic-protein" },
			{ "synthetic_resin", "Synthetic Resin", "", "", "synthetic-resin" },
			{ "synthetic_silicon", "Synthetic Silicon", "comp_synthetic_silicon", "", "silica-sand" },
			{ "bar_titanium", "Titanium Bar", "ingot_titanium", "", "titanium-bar" },
			{ "titanium_beam", "Titanium Beam", "beam_titanium", "", "titanium-beam" },
			{ "titanium_housing", "Titanium Housing", "comp_titanium_housing", "", "titanium-housing" },
			{ "ore_titanium", "Titanium Ore", "", "ore_titanium", "titanium-ore" },
			{ "titanium_rod", "Titanium Rod", "rod_titanium", "", "titanium-rod" },
			{ "titanium_sheet", "Titanium Sheet", "sheet_titanium", "", "titanium-sheet" },
			{ "ubertitanium", "Titanoferrite Ingot", "", "ingot_titanoferrite", "uber-titanium" },
			{ "trackingdevice", "Tracking Device", "", "comp_tracking_device", "tracking-device" },
			{ "tube", "Tube", "comp_tube", "", "tube" },
			{ "turbine", "Turbine", "comp_turbine", "", "turbine" },
			{ "uberfilament", "Uberfilament", "", "comp_uberfilament", "uberfilament" },
			{ "valve", "Valve", "comp_valve", "", "valve" },
			{ "bar_wolfram", "Wolfram Bar", "ingot_wolfram", "", "wolfram-bar" },
			{ "ore_wolfram", "Wolfram Ore", "", "ore_wolfram", "wolfram-ore" },
			{ "wolfram_plate", "Wolfram Plate", "plate_wolfram", "", "wolfram-plate" },
			{ "powder_wolfram", "Wolfram Powder", "powder_wolfram", "", "wolfram-powder" },
			{ "uberwolfram", "Wolfram Steel Ingot", "", "comp_wolfram_steel_ingot", "uber-wolfram" },
			{ "wolfram_wire", "Wolfram Wire", "wire_wolfram", "", "wolfram-wire" },
		};

		// Game UniqueItemNames that spell the item differently from the planner.
		struct ItemAlias
		{
			const char* gameKey; // normalized UniqueItemName
			const char* id;      // planner id
		};

		constexpr ItemAlias kItemAliases[] = {
			{ "goethite",      "goethiteore" },
			{ "magicoil",      "magicoilore" },
			{ "nanofibre",     "nanofiber" },
			{ "sulphur",       "ore_sulfur" },
			{ "sulphuricacid", "sulfuric_acid" },
		};

		// Lowercase letters and digits only.
		std::string Normalize(const std::string& s)
		{
			std::string out;
			for (char c : s)
				if (std::isalnum(static_cast<unsigned char>(c)))
					out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			return out;
		}

		const std::unordered_map<std::string, const PlannerItem*>& ItemIndex()
		{
			static const std::unordered_map<std::string, const PlannerItem*> index = []
			{
				std::unordered_map<std::string, const PlannerItem*> map;
				for (const PlannerItem& item : kPlannerItems)
				{
					map.emplace(Normalize(item.id), &item);
					map.emplace(Normalize(item.name), &item);
				}
				for (const ItemAlias& alias : kItemAliases)
					for (const PlannerItem& item : kPlannerItems)
						if (std::strcmp(item.id, alias.id) == 0)
							map.emplace(alias.gameKey, &item);
				return map;
			}();
			return index;
		}

		// The planner's entry for a game item, or null if the planner has no such item.
		const PlannerItem* FindPlannerItem(const std::string& uniqueItemName, const std::string& displayName)
		{
			const auto& index = ItemIndex();
			for (const std::string* name : { &uniqueItemName, &displayName })
			{
				auto it = index.find(Normalize(*name));
				if (it != index.end())
					return it->second;
			}
			return nullptr;
		}

		// Percent-encodes everything outside the RFC 3986 unreserved set.
		std::string UrlEncode(const std::string& s)
		{
			static constexpr char kHex[] = "0123456789ABCDEF";
			std::string out;
			for (char c : s)
			{
				const unsigned char u = static_cast<unsigned char>(c);
				if (std::isalnum(u) || c == '-' || c == '_' || c == '.' || c == '~')
				{
					out += c;
				}
				else
				{
					out += '%';
					out += kHex[u >> 4];
					out += kHex[u & 15];
				}
			}
			return out;
		}

		// The only URLs the shell is ever given: http(s), printable ASCII, and
		// none of quote, angle brackets, backslash, caret, backtick, pipe or
		// braces, which could break out of the browser's command line.
		bool IsSafeUrl(const std::string& url)
		{
			const bool https = _strnicmp(url.c_str(), "https://", 8) == 0;
			const bool http = _strnicmp(url.c_str(), "http://", 7) == 0;
			if ((!https && !http) || url.size() > kMaxUrlLength)
				return false;

			for (char c : url)
			{
				const unsigned char u = static_cast<unsigned char>(c);
				if (u <= 0x20 || u >= 0x7F || std::strchr("\"<>\\^`|{}", c))
					return false;
			}
			return true;
		}

		// Replaces every occurrence of a placeholder in the URL template.
		void ReplacePlaceholder(std::string& url, const char* placeholder, const std::string& value)
		{
			const size_t placeholderLength = std::strlen(placeholder);
			for (size_t pos = url.find(placeholder); pos != std::string::npos; pos = url.find(placeholder, pos + value.size()))
				url.replace(pos, placeholderLength, value);
		}

		// A planner the Launch planner button can open. itemId picks the item's
		// id on that planner. A planner with no item link leaves itemId and
		// itemParam null and always opens at url. A planner whose calculator
		// cannot target every item can name a second form: an item with no
		// itemId but with a pageId opens at pageUrl with that id appended.
		struct PlannerSite
		{
			const char* name;      // value of Planner/Planner; the first site is the default
			const char* url;       // opened as is, or with itemParam and the item id appended
			const char* itemParam;
			const char* PlannerItem::* itemId;
			const char* pageUrl;
			const char* PlannerItem::* pageId;
		};

		constexpr PlannerSite kPlannerSites[] = {
			{ "StarRupture Planner",     "https://starrupture.goodstuff.software/",       "?add=",  &PlannerItem::id,                   nullptr, nullptr },
			{ "starruptureplanner.com",  "https://starruptureplanner.com/calculator",     "?item=", &PlannerItem::starruptureplannerId, "https://starruptureplanner.com/items/", &PlannerItem::starruptureplannerPageId },
			{ "starrupturecalc.com",     "https://www.starrupturecalc.com/",              "?item=", &PlannerItem::starrupturecalcId,    nullptr, nullptr },
			{ "starrupture-planner.com", "https://www.starrupture-planner.com/planner/",  nullptr,  nullptr,                            nullptr, nullptr },
		};

		std::string BuildUrl(const PlannerItem* item, const std::string& uniqueItemName, const std::string& displayName)
		{
			const char* planner = CodexConfig::Config::GetPlanner();
			if (_stricmp(planner, kCustomPlannerName) == 0)
			{
				std::string url = CodexConfig::Config::GetPlannerUrlTemplate();
				ReplacePlaceholder(url, "{item}", UrlEncode(item ? item->id : ""));
				ReplacePlaceholder(url, "{name}", UrlEncode(displayName));
				ReplacePlaceholder(url, "{game}", UrlEncode(uniqueItemName));
				return url;
			}

			const PlannerSite* site = &kPlannerSites[0];
			for (const PlannerSite& candidate : kPlannerSites)
				if (_stricmp(planner, candidate.name) == 0)
					site = &candidate;
			if (_stricmp(planner, site->name) != 0)
				LOG_WARN("CodexPlanner: unknown Planner '%s'; using the %s.", planner, site->name);

			std::string url = site->url;
			if (!site->itemId)
				return url;

			const char* itemId = item ? item->*site->itemId : "";
			const char* pageItemId = (item && site->pageId) ? item->*site->pageId : "";
			if (*itemId)
				url += site->itemParam + UrlEncode(itemId);
			else if (*pageItemId)
				url = site->pageUrl + UrlEncode(pageItemId);
			else
				LOG_INFO("CodexPlanner: no %s id for item '%s' (%s); opening the planner without an item.",
					site->name, uniqueItemName.c_str(), displayName.c_str());
			return url;
		}
	}

	void Launch(const std::string& uniqueItemName, const std::string& displayName)
	{
		const std::string url = BuildUrl(FindPlannerItem(uniqueItemName, displayName), uniqueItemName, displayName);
		if (!IsSafeUrl(url))
		{
			LOG_WARN("CodexPlanner: not opening '%s'; the planner URL must be http:// or https:// with printable ASCII and no spaces, quotes or braces. Check Planner/CustomUrlTemplate.",
				url.c_str());
			return;
		}

		// The URL is printable ASCII, so widening byte by byte is exact.
		const std::wstring wideUrl(url.begin(), url.end());
		const INT_PTR result = reinterpret_cast<INT_PTR>(
			ShellExecuteW(nullptr, L"open", wideUrl.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
		if (result <= 32)
		{
			LOG_WARN("CodexPlanner: could not open the browser (ShellExecute error %d) for %s", static_cast<int>(result), url.c_str());
		}
		else
		{
			LOG_DEBUG("CodexPlanner: opened %s", url.c_str());
		}
	}
}
