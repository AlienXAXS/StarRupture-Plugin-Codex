#include "codex_ui.h"
#include "Recipes/codex_recipes.h"
#include "Icons/codex_icons.h"
#include "Plugin Core/Helpers/plugin_helpers.h"
#include "Plugin Core/Config/plugin_config.h"

#include "Engine_classes.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace CodexUI
{
	namespace
	{
		IPluginSelf* g_self = nullptr;

		WidgetHandle g_searchWidget = nullptr;
		WidgetHandle g_detailWidget = nullptr;

		PluginWindowHints g_searchHints{};
		PluginWindowHints g_detailHints{};
		PluginWidgetDesc  g_searchDesc{};
		PluginWidgetDesc  g_detailDesc{};

		void* g_inputCaptureToken = nullptr;
		bool  g_escapeRegistered = false;

		bool  g_searchVisible = false;
		bool  g_detailVisible = false;
		bool  g_justOpenedSearch = false;
		char  g_searchBuffer[256] = "";
		void* g_selectedNativeRecipe = nullptr;

		// Native recipe of the top-most row currently shown by
		// RenderSearchWidget, refreshed every frame it renders - lets Enter
		// jump straight to it without re-running the search match logic.
		void* g_topSearchResult = nullptr;

		// Set when the user clicks an output item; drives the "used in..."
		// popup listing every recipe that consumes that item as an input.
		std::string g_consumersItemName;
		std::string g_consumersItemDisplayName;
		bool        g_openConsumersPopup = false;

		// ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap
		// Used for the (single-column) search result rows only.
		constexpr int kSelectableFlags = (1 << 1) | (1 << 4);

		// ImGuiSelectableFlags_AllowOverlap - deliberately WITHOUT
		// SpanAllColumns, since these rows live inside a 2-column table and
		// SpanAllColumns would make a click in either column's row cover the
		// whole table width, triggering whichever item happened to be drawn
		// underneath in the other column.
		constexpr int kItemSelectableFlags = (1 << 4);

		constexpr int kColumnFlagsWidthStretch = 1 << 3;

		void OnEscapePressed(EModKey key, EModKeyEvent event);
		void OnSearchKeyPressed(EModKey key, EModKeyEvent event);
		void CloseSearch();
		void CloseDetail();

		std::string ToLower(const std::string& s)
		{
			std::string out = s;
			std::transform(out.begin(), out.end(), out.begin(),
				[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return out;
		}

		bool InChimeraMain()
		{
			SDK::UWorld* world = SDK::UWorld::GetWorld();
			return world && world->GetName() == "ChimeraMain";
		}

		void AcquireCapture()
		{
			if (g_inputCaptureToken)
				return;
			IPluginHooks* hooks = GetHooks();
			if (hooks && hooks->UI)
				g_inputCaptureToken = hooks->UI->AcquireInputCapture();
		}

		void ReleaseCaptureIfIdle()
		{
			if (g_searchVisible || g_detailVisible)
				return;
			if (!g_inputCaptureToken)
				return;
			IPluginHooks* hooks = GetHooks();
			if (hooks && hooks->UI)
				hooks->UI->ReleaseInputCapture(g_inputCaptureToken);
			g_inputCaptureToken = nullptr;
		}

		// Escape should close whichever Codex window is open, so both
		// OpenSearch and OpenDetail register it - reference-counted via
		// g_escapeRegistered so the first one in / last one out owns it.
		void AcquireEscape()
		{
			if (g_escapeRegistered)
				return;
			if (g_self && g_self->hooks->Input)
				g_self->hooks->Input->RegisterKeybindByName("Escape", EModKeyEvent::Pressed, &OnEscapePressed);
			g_escapeRegistered = true;
		}

		void ReleaseEscapeIfIdle()
		{
			if (g_searchVisible || g_detailVisible)
				return;
			if (!g_escapeRegistered)
				return;
			if (g_self && g_self->hooks->Input)
				g_self->hooks->Input->UnregisterKeybindByName("Escape", EModKeyEvent::Pressed, &OnEscapePressed);
			g_escapeRegistered = false;
		}

		void OpenSearch()
		{
			if (g_searchVisible)
				return;

			g_searchVisible = true;
			g_searchBuffer[0] = '\0';
			g_justOpenedSearch = true;

			if (g_self && g_self->hooks->UI && g_searchWidget)
				g_self->hooks->UI->SetWidgetVisible(g_searchWidget, true);

			AcquireEscape();
			AcquireCapture();
		}

		void CloseSearch()
		{
			if (!g_searchVisible)
				return;

			g_searchVisible = false;
			g_topSearchResult = nullptr;

			if (g_self && g_self->hooks->UI && g_searchWidget)
				g_self->hooks->UI->SetWidgetVisible(g_searchWidget, false);

			ReleaseEscapeIfIdle();
			ReleaseCaptureIfIdle();
		}

		void OpenDetail(void* nativeRecipe)
		{
			g_selectedNativeRecipe = nativeRecipe;

			if (!g_detailVisible)
			{
				g_detailVisible = true;
				if (g_self && g_self->hooks->UI && g_detailWidget)
					g_self->hooks->UI->SetWidgetVisible(g_detailWidget, true);
				AcquireEscape();
				AcquireCapture();
			}
		}

		void CloseDetail()
		{
			if (!g_detailVisible)
				return;

			g_detailVisible = false;
			g_selectedNativeRecipe = nullptr;
			g_consumersItemName.clear();

			if (g_self && g_self->hooks->UI && g_detailWidget)
				g_self->hooks->UI->SetWidgetVisible(g_detailWidget, false);

			ReleaseEscapeIfIdle();
			ReleaseCaptureIfIdle();
		}

		void OnEscapePressed(EModKey /*key*/, EModKeyEvent event)
		{
			if (event != EModKeyEvent::Pressed)
				return;
			if (g_searchVisible)
				CloseSearch();
			else if (g_detailVisible)
				CloseDetail();
		}

		void OnSearchKeyPressed(EModKey /*key*/, EModKeyEvent event)
		{
			if (event != EModKeyEvent::Pressed)
				return;
			if (g_searchVisible)
				return;

			if (!InChimeraMain())
			{
				LOG_DEBUG("CodexUI: search key ignored (not in ChimeraMain)");
				return;
			}

			OpenSearch();
		}

		// Renders one ingredient/output row: icon + name + count (and rate,
		// if known). Clicking an input row jumps to the recipe that
		// produces that input, if one is known. Clicking the output row
		// instead opens a popup listing every recipe that consumes it, so
		// the user can browse "what uses this?" before navigating.
		void RenderItemRow(IModLoaderImGui* imgui, IPluginImGuiTextures* textures,
			const CodexRecipes::RecipeItemRef& item, float iconSize, float ratePerMinute, bool isOutput)
		{
			imgui->PushIDStr(item.uniqueItemName.c_str());

			// Look the icon up live (rather than trusting item.icon, which was
			// snapshotted at recipe-scan time and may have been null if the
			// icon scan hadn't finished loading textures yet).
			PluginTextureHandle icon = CodexIcons::GetIcon(item.uniqueItemName);

			bool clicked = imgui->SelectableFull("##item_row", false, kItemSelectableFlags, 0.0f, iconSize + 6.0f);
			imgui->SameLine(0.0f, 0.0f);

			if (icon && textures)
			{
				textures->Image(icon, iconSize, iconSize);
				imgui->SameLine(0.0f, 6.0f);
			}

			imgui->BeginGroup();
			imgui->Text(item.displayName.c_str());

			char sub[64];
			if (ratePerMinute > 0.0f)
				snprintf(sub, sizeof(sub), "x%d  (%.1f/min)", item.count, ratePerMinute);
			else
				snprintf(sub, sizeof(sub), "x%d", item.count);

			// Pull the sub-line up closer to the name; the default item
			// spacing leaves it too low, bleeding past the row's bottom edge.
			imgui->SetCursorPosY(imgui->GetCursorPosY() - 4.0f);
			imgui->TextDisabled(sub);
			imgui->EndGroup();

			imgui->PopID();

			if (clicked)
			{
				if (isOutput)
				{
					g_consumersItemName        = item.uniqueItemName;
					g_consumersItemDisplayName = item.displayName;
					g_openConsumersPopup       = true;
				}
				else
				{
					CodexRecipes::RecipeInfo producer;
					if (CodexRecipes::FindProducerOfItem(item.uniqueItemName, producer) &&
						producer.nativeRecipe != g_selectedNativeRecipe)
					{
						OpenDetail(producer.nativeRecipe);
					}
				}
			}
		}

		// Renders the "used in..." popup opened from clicking an output
		// item: a scrollable list of every recipe that consumes it, letting
		// the user jump straight to one.
		void RenderConsumersPopup(IModLoaderImGui* imgui, IPluginImGuiTextures* textures)
		{
			if (g_openConsumersPopup)
			{
				imgui->OpenPopup("##consumers_popup", 0);
				g_openConsumersPopup = false;
			}

			if (!imgui->BeginPopup("##consumers_popup", 0))
				return;

			char title[128];
			snprintf(title, sizeof(title), "Used in (%s)", g_consumersItemDisplayName.c_str());
			imgui->Text(title);
			imgui->Separator();

			std::vector<CodexRecipes::RecipeInfo> consumers = CodexRecipes::FindConsumersOfItem(g_consumersItemName);
			if (consumers.empty())
			{
				imgui->TextDisabled("Not used by any known recipe.");
			}
			else if (imgui->BeginChild("##consumers_list", 320.0f, 240.0f, false))
			{
				for (const CodexRecipes::RecipeInfo& consumer : consumers)
				{
					imgui->PushIDStr(consumer.displayName.c_str());
					bool clicked = imgui->SelectableFull("##consumer_row", false, kSelectableFlags, 0.0f, 22.0f);
					imgui->SameLine(0.0f, 0.0f);

					PluginTextureHandle icon = CodexIcons::GetIcon(consumer.output.uniqueItemName);
					if (icon && textures)
					{
						textures->Image(icon, 18.0f, 18.0f);
						imgui->SameLine(0.0f, 6.0f);
					}
					imgui->Text(consumer.displayName.c_str());
					imgui->PopID();

					if (clicked)
					{
						imgui->CloseCurrentPopup();
						OpenDetail(consumer.nativeRecipe);
					}
				}
				imgui->EndChild();
			}

			imgui->EndPopup();
		}

		// Lets the search box double as a calculator (à la Satisfactory's
		// search bar): typing "12 * 250" shows "= 3000" beneath the box
		// instead of running a recipe search.
		namespace Calculator
		{
			struct Parser
			{
				const char* p;
				bool ok = true;

				void SkipSpace() { while (*p == ' ' || *p == '\t') ++p; }

				double ParseExpr()
				{
					double lhs = ParseTerm();
					for (;;)
					{
						SkipSpace();
						if (*p == '+') { ++p; lhs += ParseTerm(); }
						else if (*p == '-') { ++p; lhs -= ParseTerm(); }
						else break;
					}
					return lhs;
				}

				double ParseTerm()
				{
					double lhs = ParseUnary();
					for (;;)
					{
						SkipSpace();
						if (*p == '*') { ++p; lhs *= ParseUnary(); }
						else if (*p == '/')
						{
							++p;
							double rhs = ParseUnary();
							if (!ok || rhs == 0.0) { ok = false; return 0.0; }
							lhs /= rhs;
						}
						else break;
					}
					return lhs;
				}

				double ParseUnary()
				{
					SkipSpace();
					if (*p == '-') { ++p; return -ParseUnary(); }
					if (*p == '+') { ++p; return ParseUnary(); }
					return ParseAtom();
				}

				double ParseAtom()
				{
					SkipSpace();
					if (*p == '(')
					{
						++p;
						double v = ParseExpr();
						SkipSpace();
						if (*p != ')') { ok = false; return 0.0; }
						++p;
						return v;
					}

					const char* start = p;
					while ((*p >= '0' && *p <= '9') || *p == '.')
						++p;
					if (p == start) { ok = false; return 0.0; }

					return std::strtod(start, nullptr);
				}
			};

			// Cheap pre-check before running the parser: only digits,
			// arithmetic operators, parens, '.' and whitespace are allowed,
			// and there must be at least one digit - otherwise a plain
			// recipe search term like "Iron Plate" would never reach here,
			// but this keeps the rejection explicit and free of parser cost.
			bool TryEvaluate(const std::string& expr, double& outResult)
			{
				bool hasDigit = false;
				for (char c : expr)
				{
					if (c >= '0' && c <= '9') { hasDigit = true; continue; }
					if (c == '+' || c == '-' || c == '*' || c == '/' ||
						c == '(' || c == ')' || c == '.' || c == ' ' || c == '\t')
						continue;
					return false;
				}
				if (!hasDigit)
					return false;

				Parser parser{ expr.c_str() };
				double result = parser.ParseExpr();
				parser.SkipSpace();
				if (!parser.ok || *parser.p != '\0')
					return false;

				outResult = result;
				return true;
			}

			std::string FormatResult(double value)
			{
				char buf[64];
				if (std::fabs(value - std::round(value)) < 1e-9 && std::fabs(value) < 1e15)
				{
					snprintf(buf, sizeof(buf), "%.0f", value);
					return buf;
				}

				snprintf(buf, sizeof(buf), "%.4f", value);
				std::string s = buf;
				size_t last = s.find_last_not_of('0');
				if (s[last] == '.')
					--last;
				s.erase(last + 1);
				return s;
			}
		}

		// Approximate height of the search box with no results/calc output
		// shown - used to anchor the window's top edge at the same place it
		// would sit if vertically centered while collapsed. The window is
		// then pinned there by its top-left corner (pivot_y = 0), so any
		// growth from results or a calculator answer expands downward only,
		// instead of the box itself drifting as height changes.
		constexpr float kSearchBoxCollapsedHeight = 60.0f;

		void RenderSearchWidget(IModLoaderImGui* imgui)
		{
			float dispW = 1920.0f, dispH = 1080.0f;
			imgui->GetDisplaySize(&dispW, &dispH);
			g_searchHints.pos_x = dispW * 0.5f;
			g_searchHints.pos_y = dispH * 0.5f - kSearchBoxCollapsedHeight * 0.5f;

			if (g_justOpenedSearch)
			{
				imgui->SetKeyboardFocusHere(0);
				g_justOpenedSearch = false;
			}

			imgui->SetNextItemWidth(-1.0f);
			imgui->InputTextWithHint("##Codex_Search_Input", "Search recipes...", g_searchBuffer, sizeof(g_searchBuffer));

			// Detected directly off the InputText widget rather than a
			// global "Enter" keybind: the modloader's hotkey system only
			// forwards keys once no ImGui widget has keyboard focus, and
			// typing Enter into a focused field defocuses it - so a global
			// keybind would only fire on a *second* Enter press. Reading
			// ImGui's own deactivated-after-edit flag (true the instant the
			// field submits) sidesteps that entirely.
			const bool submitted = imgui->IsItemDeactivatedAfterEdit();

			// Refreshed below as results are matched; stale otherwise so
			// Enter can't jump to a result that's no longer shown.
			g_topSearchResult = nullptr;

			// Nothing typed yet - just the search box, nothing else.
			const std::string term = ToLower(g_searchBuffer);
			if (term.empty())
				return;

			// If the box holds a valid arithmetic expression, show its
			// result instead of running a recipe search.
			double calcResult = 0.0;
			if (Calculator::TryEvaluate(g_searchBuffer, calcResult))
			{
				imgui->Separator();
				char line[64];
				snprintf(line, sizeof(line), "= %s", Calculator::FormatResult(calcResult).c_str());
				imgui->Text(line);
				return;
			}

			if (!CodexRecipes::IsReady())
				return;

			std::vector<CodexRecipes::RecipeInfo> all = CodexRecipes::GetAll();
			if (all.empty())
				return;

			IPluginImGuiTextures* textures = GetHooks() ? GetHooks()->ImGuiTextures : nullptr;

			constexpr int kMaxResults = 10;
			int shown = 0;

			imgui->Separator();

			for (const CodexRecipes::RecipeInfo& recipe : all)
			{
				if (shown >= kMaxResults)
					break;

				if (ToLower(recipe.displayName).find(term) == std::string::npos)
					continue;

				if (shown == 0)
					g_topSearchResult = recipe.nativeRecipe;

				imgui->PushIDStr(recipe.displayName.c_str());
				bool clicked = imgui->SelectableFull("##recipe_row", false, kSelectableFlags, 0.0f, 22.0f);
				imgui->SameLine(0.0f, 0.0f);

				PluginTextureHandle icon = CodexIcons::GetIcon(recipe.output.uniqueItemName);
				if (icon && textures)
				{
					textures->Image(icon, 18.0f, 18.0f);
					imgui->SameLine(0.0f, 6.0f);
				}
				imgui->Text(recipe.displayName.c_str());
				imgui->PopID();

				if (clicked)
				{
					CloseSearch();
					OpenDetail(recipe.nativeRecipe);
				}

				++shown;
			}

			if (submitted && g_topSearchResult)
			{
				void* target = g_topSearchResult;
				CloseSearch();
				OpenDetail(target);
			}
		}

		void RenderDetailWidget(IModLoaderImGui* imgui)
		{
			if (imgui->Button("Close"))
			{
				CloseDetail();
				return;
			}

			CodexRecipes::RecipeInfo info;
			if (!g_selectedNativeRecipe || !CodexRecipes::FindByNativeRecipe(g_selectedNativeRecipe, info))
			{
				imgui->TextDisabled("No recipe selected.");
				return;
			}

			imgui->SameLine(0.0f, 12.0f);
			imgui->SeparatorText(info.displayName.c_str());

			char line[256];
			snprintf(line, sizeof(line), "Made in: %s", info.buildingName.c_str());
			imgui->Text(line);

			if (info.buildTimeSeconds > 0.0f)
				snprintf(line, sizeof(line), "Craft time: %.1fs  (%.1f/min)", info.buildTimeSeconds, info.outputsPerMinute);
			else
				snprintf(line, sizeof(line), "Craft time: unknown");
			imgui->Text(line);

			imgui->Separator();

			IPluginImGuiTextures* textures = GetHooks() ? GetHooks()->ImGuiTextures : nullptr;
			constexpr float kIconSize = 32.0f;

			if (imgui->BeginTable("##Codex_Detail_Columns", 2, 0))
			{
				imgui->TableSetupColumn("Inputs", kColumnFlagsWidthStretch, 1.0f);
				imgui->TableSetupColumn("Outputs", kColumnFlagsWidthStretch, 1.0f);
				imgui->TableHeadersRow();
				imgui->TableNextRow(0, 0.0f);

				imgui->TableNextColumn();
				if (info.inputs.empty())
					imgui->TextDisabled("(none)");
				for (const CodexRecipes::RecipeItemRef& input : info.inputs)
				{
					const float rate = info.buildTimeSeconds > 0.0f
						? (static_cast<float>(input.count) / info.buildTimeSeconds) * 60.0f
						: 0.0f;
					RenderItemRow(imgui, textures, input, kIconSize, rate, false);
				}

				imgui->TableNextColumn();
				RenderItemRow(imgui, textures, info.output, kIconSize, info.outputsPerMinute, true);

				imgui->EndTable();
			}

			RenderConsumersPopup(imgui, textures);
		}
	}

	void Init(IPluginSelf* self)
	{
		g_self = self;

		if (!self->hooks->UI)
		{
			LOG_WARN("CodexUI: UI hooks unavailable (server/generic build) - Codex search disabled.");
			return;
		}

		g_searchHints.width  = 440.0f;
		g_searchHints.height = 0.0f;
		g_searchHints.pos_x  = 960.0f;
		g_searchHints.pos_y  = 540.0f - kSearchBoxCollapsedHeight * 0.5f;
		g_searchHints.pivot_x = 0.5f;
		g_searchHints.pivot_y = 0.0f; // Top-anchored so growth expands downward, not from center
		g_searchHints.size_cond = 0; // Always
		g_searchHints.pos_cond  = 0; // Always - recentres every frame via GetDisplaySize
		g_searchHints.extra_window_flags = PluginWindowFlags_NoTitleBar | PluginWindowFlags_NoResize |
			PluginWindowFlags_NoMove | PluginWindowFlags_NoSavedSettings;

		g_searchDesc.name        = "Codex Search";
		g_searchDesc.renderFn    = &RenderSearchWidget;
		g_searchDesc.windowHints = &g_searchHints;
		g_searchWidget = self->hooks->UI->RegisterWidget(&g_searchDesc);
		if (g_searchWidget)
			self->hooks->UI->SetWidgetVisible(g_searchWidget, false);

		g_detailHints.width  = 640.0f;
		g_detailHints.height = 520.0f;
		g_detailHints.pos_x  = 960.0f;
		g_detailHints.pos_y  = 540.0f;
		g_detailHints.pivot_x = 0.5f;
		g_detailHints.pivot_y = 0.5f;
		g_detailHints.size_cond = 1; // FirstUseEver - user can resize afterwards
		g_detailHints.pos_cond  = 1; // FirstUseEver - user can move afterwards
		g_detailHints.extra_window_flags = PluginWindowFlags_NoSavedSettings | PluginWindowFlags_NoResize;

		g_detailDesc.name        = "Codex";
		g_detailDesc.renderFn    = &RenderDetailWidget;
		g_detailDesc.windowHints = &g_detailHints;
		g_detailWidget = self->hooks->UI->RegisterWidget(&g_detailDesc);
		if (g_detailWidget)
			self->hooks->UI->SetWidgetVisible(g_detailWidget, false);

		if (self->hooks->Input)
		{
			const char* searchKey = CodexConfig::Config::GetSearchKey();
			LOG_DEBUG("CodexUI: registering search keybind '%s'", searchKey);
			self->hooks->Input->RegisterKeybindByName(searchKey, EModKeyEvent::Pressed, &OnSearchKeyPressed);
		}

		LOG_INFO("CodexUI: search and detail windows registered");
	}

	void Shutdown(IPluginSelf* self)
	{
		if (!self->hooks->UI)
		{
			g_self = nullptr;
			return;
		}

		CloseSearch();
		CloseDetail();

		if (self->hooks->Input)
		{
			const char* searchKey = CodexConfig::Config::GetSearchKey();
			self->hooks->Input->UnregisterKeybindByName(searchKey, EModKeyEvent::Pressed, &OnSearchKeyPressed);
		}

		if (g_searchWidget)
		{
			self->hooks->UI->UnregisterWidget(g_searchWidget);
			g_searchWidget = nullptr;
		}
		if (g_detailWidget)
		{
			self->hooks->UI->UnregisterWidget(g_detailWidget);
			g_detailWidget = nullptr;
		}

		g_self = nullptr;
	}
}
