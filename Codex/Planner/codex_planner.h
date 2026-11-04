#pragma once

#include <string>

// "Launch planner" button support: opens the player's default browser at a
// production planner for an item.
//
// The planner is chosen in the plugin config (Planner/Planner): the default is
// the StarRupture Planner (https://starrupture.goodstuff.software), which
// adds an item from ?add=<item id>. starruptureplanner.com and
// starrupturecalc.com open their calculator for the item
// (starruptureplanner.com opens the item's page for items its calculator
// has no recipe for); starrupture-planner.com has no item link and opens
// its planner page.
// "Custom" uses Planner/CustomUrlTemplate with {item} replaced by the
// StarRupture Planner's item id, {name} by the display name and {game} by the
// game's UniqueItemName, each URL-encoded.
//
// Planners use their own item ids ("basicfuel", "ore_calcium"), not the
// game's UniqueItemName, so the item is translated through a table built from
// each planner's item list.
namespace CodexPlanner
{
	// Opens the configured planner for the item. If the item has no planner id
	// the planner is still opened, without an item, and the id is logged.
	// Only http:// and https:// URLs are ever handed to the shell.
	void Launch(const std::string& uniqueItemName, const std::string& displayName);
}
