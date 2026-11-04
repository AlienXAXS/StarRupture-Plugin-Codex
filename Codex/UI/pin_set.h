#pragma once

// The pinned recipes as they are saved in the config: one config value per
// slot (PinnedRecipe1..8), each holding the recipe's key (its asset name,
// which survives a game update where a runtime pointer does not). The loader's
// config table keeps 255 characters per value, so one value per pin cannot
// lose a pin the way a single joined string could. A pure function of the
// keys: no ImGui, no game, so it can be tested on its own.

#include <algorithm>
#include <string>
#include <vector>

namespace CodexPinSet
{
	constexpr char kSeparator = ';';

	// The most characters one config value holds (the loader's table is 256
	// bytes with the terminator).
	constexpr size_t kMaxValueLen = 255;

	// The keys laid out one per slot, in pin order, `slotCount` slots long; the
	// slots after the last key are empty. A key that is empty, or longer than a
	// value can hold, is skipped (it could not be read back whole), and so is
	// every key past the last slot. Real asset names are neither empty nor long.
	inline std::vector<std::string> ToSlots(const std::vector<std::string>& keys, int slotCount)
	{
		std::vector<std::string> slots;
		for (const std::string& key : keys)
		{
			if (static_cast<int>(slots.size()) >= slotCount)
				break;
			if (key.empty() || key.size() > kMaxValueLen)
				continue;
			slots.push_back(key);
		}
		slots.resize(slotCount > 0 ? slotCount : 0);
		return slots;
	}

	// The keys in the slots, in slot order: blank slots and repeats are dropped
	// (a hand-edited file may have either) and spaces around a key are trimmed.
	inline std::vector<std::string> FromSlots(const std::vector<std::string>& slots)
	{
		std::vector<std::string> keys;
		for (const std::string& slot : slots)
		{
			size_t first = 0, last = slot.size();
			while (first < last && (slot[first] == ' ' || slot[first] == '\t'))
				++first;
			while (last > first && (slot[last - 1] == ' ' || slot[last - 1] == '\t'))
				--last;

			const std::string key = slot.substr(first, last - first);
			if (!key.empty() && std::find(keys.begin(), keys.end(), key) == keys.end())
				keys.push_back(key);
		}
		return keys;
	}

	// The keys in a ";"-joined text, in order, at most `maxKeys` of them, as
	// FromSlots reads them. Only the old single-value save is in this form.
	inline std::vector<std::string> Parse(const char* text, int maxKeys)
	{
		std::vector<std::string> pieces;
		if (!text)
			return pieces;

		const std::string all = text;
		size_t from = 0;
		while (from <= all.size())
		{
			size_t end = all.find(kSeparator, from);
			if (end == std::string::npos)
				end = all.size();
			pieces.push_back(all.substr(from, end - from));
			from = end + 1;
		}

		std::vector<std::string> keys = FromSlots(pieces);
		if (static_cast<int>(keys.size()) > maxKeys)
			keys.resize(maxKeys > 0 ? maxKeys : 0);
		return keys;
	}

	// The slots to start from. The first pin save kept every pin in one value,
	// "PinnedRecipes", joined with ";". If no slot holds a pin yet and that old
	// value does, the pins in it become the slots, and `migrated` is set so the
	// caller can empty the old value; otherwise the slots stand as they are.
	// Pins past the 255th character of the old value were cut off when it was
	// saved, and a half key at the end simply fails to match a recipe.
	inline std::vector<std::string> Migrate(const std::vector<std::string>& slots, const char* legacy,
		int slotCount, bool& migrated)
	{
		migrated = false;
		if (!FromSlots(slots).empty())
			return slots;

		const std::vector<std::string> old = Parse(legacy, slotCount);
		if (old.empty())
			return slots;

		migrated = true;
		return ToSlots(old, slotCount);
	}
}
