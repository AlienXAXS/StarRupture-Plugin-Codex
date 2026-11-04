#pragma once

#include "plugin_interface.h"
#include "Chimera_classes.hpp"
#include "Chimera_structs.hpp"
#include "AuCrafting_classes.hpp"
#include "AuCrafting_structs.hpp"
#include "AuItems_classes.hpp"
#include "AuItems_structs.hpp"
#include "AuActorPlacement_classes.hpp"
#include "UMG_classes.hpp"
#include "plugin_map.h"

#include <string>
#include <vector>

// Every member of a game class Codex reads, taken from the loader's map for the
// running game build (see plugin_map.h). Resolved once in OnPluginLoadHooks;
// read on the game thread with In(object), which is null when the object is
// null. Structs Codex only reads through those members are covered by a size
// check instead of a field per member.

#define CODEX_FIELD(Class, Member) SR_FIELD(decltype(SDK::Class::Member), Class, Member, 0)

namespace CodexFields
{
	inline auto UIItemType        = CODEX_FIELD(UAuItemDataBase, UIItemType);
	inline auto UniqueItemName    = CODEX_FIELD(UAuItemDataBase, UniqueItemName);
	inline auto ItemName          = CODEX_FIELD(UAuItemDataBase, ItemName);
	inline auto ItemIcon          = CODEX_FIELD(UAuItemDataBase, ItemIcon);
	inline auto EntityType        = CODEX_FIELD(UAuActorPlacementData, EntityType);
	inline auto BuildingType      = CODEX_FIELD(UCrBuildingData, Type);
	inline auto BuildingName      = CODEX_FIELD(UCrBuildingData, BuildingName);
	inline auto LinkedCollections = CODEX_FIELD(UCrItemRecipeCollectionSubsystem, LinkedCollections);
	inline auto Recipes           = CODEX_FIELD(UCrItemRecipeCollection, Recipes);
	inline auto RecipeBuildTime   = CODEX_FIELD(UAuItemRecipeData, BuildTime);
	inline auto RecipeDisplayText = CODEX_FIELD(UCrItemRecipeData, DisplayText);
	inline auto CraftingParameters = CODEX_FIELD(UCrBuildingCraftingTrait, CraftingParameters);
	inline auto CrafterToRecipeMap = CODEX_FIELD(ACrCraftingRecipeOwner, CrafterToRecipeMap);

	// Members of the structs the codex steps through
	inline auto ItemCount          = CODEX_FIELD(FAuSimpleItem, Count);
	inline auto ItemData           = CODEX_FIELD(FAuSimpleItem, ItemDataBase);
	inline auto LinkedList         = CODEX_FIELD(FNestedItemRecipeCollectionArray, Collections);
	inline auto EntityConfigPtr    = CODEX_FIELD(FAuAPMassSpawnedEntityType, EntityConfigPtr);
	inline auto EntityConfigRef    = CODEX_FIELD(FAuAPMassSpawnedEntityType, EntityConfig);
	inline auto RecipeCollection   = CODEX_FIELD(FCrCraftingParameters, RecipeCollection);

	inline auto SimpleItemSize        = SR_SIZE_CHECK(FAuSimpleItem);
	inline auto EntityTypeSize        = SR_SIZE_CHECK(FAuAPMassSpawnedEntityType);
	inline auto CraftingParametersSize = SR_SIZE_CHECK(FCrCraftingParameters);
	inline auto NestedCollectionsSize = SR_SIZE_CHECK(FNestedItemRecipeCollectionArray);

	// The game's item tooltip, read when the game-menu pin key is pressed and
	// when the pin hint is added to it. Found by name on first use (Blueprint
	// members have no SDK offset to compile in); a miss leaves that one key and
	// the hint doing nothing and the rest of the codex on.
	inline PluginLazyField<SDK::UUserWidget*> TooltipInfoWidget{ "ItemInfoWidget" }; // WBP_ToolTipWidget_C: the header line
	inline PluginLazyField<SDK::UTextBlock*>  TooltipItemName{ "ItemName" };         // WBP_HeaderLine_C: the item's name
	inline PluginLazyField<SDK::UImage*>      TooltipIcon{ "Icon" };                 // Cr_UW_InventoryToolTip: the item's icon
	inline PluginLazyField<SDK::FSlateBrush>  ImageBrush{ "Brush" };                 // UImage
	inline PluginLazyField<SDK::UMenuAnchor*> SlotTooltipAnchor{ "TooltipAnchor" };  // CrUW_InventorySlot, CrUW_WeaponSlot: opens the item tooltip

	// The hint row: the tooltip's text list and the game's action button.
	inline PluginLazyField<SDK::USizeBox*>    TooltipCraftedIn{ "CraftedInSizeBox" };   // WBP_ToolTipWidget_C: the "Crafted in" line
	inline PluginLazyField<SDK::UTextBlock*>  ButtonActionName{ "ActionName" };         // CrUW_ActionButton: the label
	inline PluginLazyField<SDK::UUserWidget*> TooltipPrompt{ "TooltipPrompt" };         // WBP_ToolTipWidget_C: the game's own action prompt
	inline PluginLazyField<SDK::UUserWidget*> PromptExpandAction{ "ExpandAction" };     // WBP_TooltipPrompt_C: its "Expand" row
	inline PluginLazyField<SDK::FDataTableRowHandle> TooltipExpandHandle{ "ExpandHandle" }; // WBP_ToolTipWidget_C: the input action the "Expand" row shows

	// The SDK's helpers, under the names the plugin uses.
	using PluginMap::KeysOf;
	using PluginMap::ValueOr;

	// The whole codex reads these members, so the plugin has one feature. Off,
	// the plugin loads but builds no recipe database and opens no window; the
	// menu and planner config entries are greyed out.
	inline PluginMap::Feature RecipeCodex{ "codex", "Recipe codex",
		KeysOf(UIItemType, UniqueItemName, ItemName, ItemIcon, EntityType, BuildingType, BuildingName,
			LinkedCollections, Recipes, RecipeBuildTime, RecipeDisplayText, CraftingParameters, CrafterToRecipeMap,
			ItemCount, ItemData, LinkedList, EntityConfigPtr, EntityConfigRef, RecipeCollection,
			SimpleItemSize, EntityTypeSize, CraftingParametersSize, NestedCollectionsSize),
		{ "Menu.*", "Planner.*" } };

	// Call before Resolve, so a miss lands on the feature and not on the plugin.
	inline void DeclareFeatures(IPluginSelf* self, IPluginHookScanner* scanner)
	{
		PluginMap::DeclareFeatures(scanner, self, RecipeCodex);
	}

	// Asks the loader for every row above. False when any did not resolve; the
	// loader has then recorded the failure and refuses the plugin.
	inline bool Resolve(IPluginSelf* self, IPluginHookScanner* scanner)
	{
		return PluginMap::ResolveFields(scanner, self,
			UIItemType, UniqueItemName, ItemName, ItemIcon, EntityType, BuildingType, BuildingName,
			LinkedCollections, Recipes, RecipeBuildTime, RecipeDisplayText, CraftingParameters, CrafterToRecipeMap,
			ItemCount, ItemData, LinkedList, EntityConfigPtr, EntityConfigRef, RecipeCollection,
			SimpleItemSize, EntityTypeSize, CraftingParametersSize, NestedCollectionsSize);
	}

	// A stack's item type and its count, or null / zero when unresolved.
	inline SDK::UAuItemDataBase* ItemOf(const SDK::FAuSimpleItem& item)
	{
		return ValueOr<SDK::UAuItemDataBase*>(ItemData, &item);
	}

	inline int32_t CountOf(const SDK::FAuSimpleItem& item)
	{
		return ValueOr<int32_t>(ItemCount, &item);
	}

	// An item's unique name and its localized name. Empty when unresolved.
	inline std::string UniqueNameOf(const SDK::UAuItemDataBase* item)
	{
		const SDK::FName* name = UniqueItemName.In(static_cast<const void*>(item));
		return name ? name->ToString() : std::string();
	}

	inline std::string DisplayNameOf(const SDK::UAuItemDataBase* item)
	{
		const SDK::FText* text = ItemName.In(static_cast<const void*>(item));
		return text ? SDK::UKismetTextLibrary::Conv_TextToString(*text).ToString() : std::string();
	}

	// The building's type, or an out-of-range value when it did not resolve.
	inline auto TypeOf(const SDK::UCrBuildingData* building)
	{
		using Type = decltype(SDK::UCrBuildingData::Type);
		return ValueOr<Type>(BuildingType, building, static_cast<Type>(0xFF));
	}
}
