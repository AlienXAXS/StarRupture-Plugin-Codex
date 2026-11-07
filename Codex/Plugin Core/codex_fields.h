#pragma once

#include "plugin_interface.h"
#include "Chimera_classes.hpp"
#include "Chimera_structs.hpp"
#include "AuCrafting_classes.hpp"
#include "AuCrafting_structs.hpp"
#include "AuCrafting_parameters.hpp"
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
// null. The two structs Codex steps through (an item stack, and the entries of
// the linked-collections map) are taken at the size the running game reflects
// (SR_SIZE): nothing is compiled in for them, so a build that resizes one does
// not turn the codex off.
//
// The structs Codex holds as single members of a game class, and reads only
// through their own member rows, are requested at any size
// (CODEX_FIELD_ANY_SIZE): nothing reads their size, so a build that resizes one
// does not turn the codex off.

#define CODEX_FIELD(Class, Member) SR_FIELD(decltype(SDK::Class::Member), Class, Member, 0)

// SR_FIELD without the size check: the same typed In(object), but the member
// may be any size the game reflects (see SR_FIELD_ANY_SIZE).
#define CODEX_FIELD_ANY_SIZE(Class, Member) \
	PluginField<decltype(SDK::Class::Member)>{ SR_FIELD_ANY_SIZE(Class, Member, 0) }

namespace CodexFields
{
	inline auto UIItemType        = CODEX_FIELD(UAuItemDataBase, UIItemType);
	inline auto UniqueItemName    = CODEX_FIELD(UAuItemDataBase, UniqueItemName);
	inline auto ItemName          = CODEX_FIELD(UAuItemDataBase, ItemName);
	inline auto ItemIcon          = CODEX_FIELD(UAuItemDataBase, ItemIcon);
	inline auto EntityType        = CODEX_FIELD_ANY_SIZE(UAuActorPlacementData, EntityType);
	inline auto BuildingType      = CODEX_FIELD(UCrBuildingData, Type);
	inline auto BuildingName      = CODEX_FIELD(UCrBuildingData, BuildingName);
	inline auto LinkedCollections = CODEX_FIELD(UCrItemRecipeCollectionSubsystem, LinkedCollections);
	inline auto Recipes           = CODEX_FIELD(UCrItemRecipeCollection, Recipes);
	inline auto RecipeBuildTime   = CODEX_FIELD(UAuItemRecipeData, BuildTime);
	inline auto RecipeDisplayText = CODEX_FIELD(UCrItemRecipeData, DisplayText);
	inline auto CraftingParameters = CODEX_FIELD_ANY_SIZE(UCrBuildingCraftingTrait, CraftingParameters);
	inline auto CrafterToRecipeMap = CODEX_FIELD(ACrCraftingRecipeOwner, CrafterToRecipeMap);

	// Members of the structs the codex reads
	inline auto ItemCount          = CODEX_FIELD(FAuSimpleItem, Count);
	inline auto ItemData           = CODEX_FIELD(FAuSimpleItem, ItemDataBase);
	inline auto LinkedList         = CODEX_FIELD(FNestedItemRecipeCollectionArray, Collections);
	inline auto EntityConfigPtr    = CODEX_FIELD(FAuAPMassSpawnedEntityType, EntityConfigPtr);
	inline auto EntityConfigRef    = CODEX_FIELD(FAuAPMassSpawnedEntityType, EntityConfig);
	inline auto RecipeCollection   = CODEX_FIELD(FCrCraftingParameters, RecipeCollection);

	inline auto SimpleItemSize        = SR_SIZE(FAuSimpleItem);
	inline auto NestedCollectionsSize = SR_SIZE(FNestedItemRecipeCollectionArray);

	// The recipe functions' parameter blocks: each one's size and where the game
	// puts its return value (see PluginMap::ParamsBlockOf).
	inline auto OutputItemParamsSize      = SR_PARAMS_SIZE(AuItemRecipeData, GetOutputItem);
	inline auto OutputItemReturn          = SR_PARAM(AuItemRecipeData, GetOutputItem, ReturnValue);
	inline auto NeededResourcesParamsSize = SR_PARAMS_SIZE(AuItemRecipeData, GetNeededResources);
	inline auto NeededResourcesReturn     = SR_PARAM(AuItemRecipeData, GetNeededResources, ReturnValue);

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
			SimpleItemSize, NestedCollectionsSize,
			OutputItemParamsSize, OutputItemReturn, NeededResourcesParamsSize, NeededResourcesReturn),
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
			SimpleItemSize, NestedCollectionsSize,
			OutputItemParamsSize, OutputItemReturn, NeededResourcesParamsSize, NeededResourcesReturn);
	}

	// A stack's item type and its count, or null / zero when unresolved. `item`
	// is the game's FAuSimpleItem, read in place.
	inline SDK::UAuItemDataBase* ItemOf(const void* item)
	{
		return ValueOr<SDK::UAuItemDataBase*>(ItemData, item);
	}

	inline int32_t CountOf(const void* item)
	{
		return ValueOr<int32_t>(ItemCount, item);
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
