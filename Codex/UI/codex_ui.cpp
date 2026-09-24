#include "codex_ui.h"
#include "Recipes/codex_recipes.h"
#include "Icons/codex_icons.h"
#include "Plugin Core/Helpers/plugin_helpers.h"
#include "Plugin Core/Config/plugin_config.h"

#include "Engine_classes.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace CodexUI
{
	namespace
	{
		IPluginSelf* g_self = nullptr;

		WidgetHandle g_searchWidget = nullptr;
		PanelHandle  g_detailPanel = nullptr;

		PluginWindowHints g_searchHints{};
		PluginWidgetDesc  g_searchDesc{};
		PluginPanelDesc   g_detailDesc{};

		// Invisible one-frame widget that performs the detail panel's close -
		// see RequestCloseDetail.
		WidgetHandle      g_closerWidget = nullptr;
		PluginWindowHints g_closerHints{};
		PluginWidgetDesc  g_closerDesc{};

		void* g_inputCaptureToken = nullptr;
		bool  g_escapeRegistered = false;

		bool  g_searchVisible = false;
		bool  g_detailVisible = false;
		bool  g_justOpenedSearch = false;
		char  g_searchBuffer[256] = "";

		// Search box placeholder, naming the full-Codex key. Rebuilt each time
		// the search opens so a rebind in the config is picked up.
		std::string g_searchHint = "Search recipes and buildings...";
		void* g_selectedNativeRecipe = nullptr;

		// ImGui frame the detail panel last rendered on - lets pinned windows
		// tell whether the Codex UI (and so the mouse) is currently up.
		int   g_detailLastFrame = -1000;

		// Top-most row currently shown by RenderSearchWidget, refreshed every
		// frame it renders - lets Enter jump straight to it without re-running
		// the search match logic. A recipe wins over a building.
		void*       g_topSearchResult = nullptr;
		std::string g_topSearchBuilding;

		// Set when the user clicks an output item; drives the "used in..."
		// popup listing every recipe that consumes that item as an input.
		std::string g_consumersItemName;
		std::string g_consumersItemDisplayName;
		bool        g_openConsumersPopup = false;

		// Detail panel tabs. g_requestedTab forces a tab on the next frame
		// (e.g. clicking a recipe in the building browser jumps to Recipe).
		enum class DetailTab { None, Recipe, Buildings };
		DetailTab g_requestedTab = DetailTab::None;

		// Building browser selection, by BuildingGroup::displayName so it
		// survives a recipe rescan. g_requestedTier forces a tier tab.
		std::string g_selectedBuildingGroup;
		int         g_requestedTier = 0;

		// ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap
		// Used for the (single-column) search result rows only.
		constexpr int kSelectableFlags = (1 << 1) | (1 << 4);

		// ImGuiSelectableFlags_AllowOverlap - deliberately WITHOUT
		// SpanAllColumns, since these rows live inside multi-column tables and
		// SpanAllColumns would make a click in either column's row cover the
		// whole table width, triggering whichever item happened to be drawn
		// underneath in the other column.
		constexpr int kItemSelectableFlags = (1 << 4);

		constexpr int kColumnFlagsWidthStretch = 1 << 3;
		constexpr int kTabItemFlagsSetSelected = 1 << 1;
		constexpr int kWindowFlagsAlwaysAutoResize = 1 << 6;

		void OnEscapePressed(EModKey key, EModKeyEvent event);
		void OnSearchKeyPressed(EModKey key, EModKeyEvent event);
		void OnCodexKeyPressed(EModKey key, EModKeyEvent event);
		void CloseSearch();
		void CloseDetail();
		void OnDetailPanelClosed(PanelHandle handle);
		void PinRecipe(void* nativeRecipe);
		bool IsPinned(void* nativeRecipe);
		bool HasFreePinSlot();

		std::string ToLower(const std::string& s)
		{
			std::string out = s;
			std::transform(out.begin(), out.end(), out.begin(),
				[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return out;
		}

		// Rows are keyed by native recipe pointer rather than display name:
		// tiered recipes (e.g. Titanium Rod in Fabricator Tier 1 and Tier 2)
		// share a name, and ImGui would otherwise see duplicate IDs.
		void PushIDPtr(IModLoaderImGui* imgui, const void* ptr)
		{
			const auto bits = reinterpret_cast<uintptr_t>(ptr);
			imgui->PushIDInt(static_cast<int>(bits ^ (bits >> 32)));
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
		// Pinned windows deliberately don't: they stay up until unpinned.
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
			g_searchHint = std::string("Search recipes and buildings (") +
				CodexConfig::Config::GetCodexKey() + " opens full Codex)";

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
			g_topSearchBuilding.clear();

			if (g_self && g_self->hooks->UI && g_searchWidget)
				g_self->hooks->UI->SetWidgetVisible(g_searchWidget, false);

			ReleaseEscapeIfIdle();
			ReleaseCaptureIfIdle();
		}

		void ShowDetailPanel()
		{
			if (g_detailVisible)
				return;

			g_detailVisible = true;
			if (g_self && g_self->hooks->UI && g_detailPanel)
				g_self->hooks->UI->SetPanelOpen(g_detailPanel);
			AcquireEscape();
			AcquireCapture();
		}

		void OpenDetail(void* nativeRecipe)
		{
			g_selectedNativeRecipe = nativeRecipe;
			g_requestedTab = DetailTab::Recipe;
			ShowDetailPanel();
		}

		// Opens the building browser on a group, optionally jumping to a
		// specific tier tab (0 = leave the tier selection alone).
		void OpenBuilding(const std::string& groupName, int tier)
		{
			g_selectedBuildingGroup = groupName;
			g_requestedTier = tier;
			g_requestedTab = DetailTab::Buildings;
			ShowDetailPanel();
		}

		// Requests the host close the panel right now. This runs the shared
		// cleanup via OnDetailPanelClosed (fired synchronously by
		// SetPanelClose), the same path taken when the user clicks the
		// window's own X button - there is only one place that resets our
		// state.
		//
		// Must NOT be called from inside the panel's own render callback (or
		// from another thread while it may be rendering): the host snapshots
		// the panel's open flag before calling renderFn and writes it back
		// afterwards, silently undoing the close. Use CloseDetail() instead.
		void CloseDetailNow()
		{
			if (!g_detailVisible)
				return;

			if (g_self && g_self->hooks->UI && g_detailPanel)
				g_self->hooks->UI->SetPanelClose(g_detailPanel);
			else
				OnDetailPanelClosed(g_detailPanel);
		}

		// Defers the close to the closer widget, which the host renders after
		// every panel on the render thread - so its SetPanelClose can't be
		// overwritten by the panel render loop.
		void CloseDetail()
		{
			if (!g_detailVisible)
				return;

			if (g_self && g_self->hooks->UI && g_closerWidget)
				g_self->hooks->UI->SetWidgetVisible(g_closerWidget, true);
			else
				CloseDetailNow();
		}

		void RenderCloserWidget(IModLoaderImGui* /*imgui*/)
		{
			if (g_self && g_self->hooks->UI && g_closerWidget)
				g_self->hooks->UI->SetWidgetVisible(g_closerWidget, false);
			CloseDetailNow();
		}

		// Fired by the host whenever the panel closes, whether from our own
		// CloseDetail() (via SetPanelClose) or the user clicking the
		// window's titlebar X directly.
		void OnDetailPanelClosed(PanelHandle handle)
		{
			if (handle != g_detailPanel || !g_detailVisible)
				return;

			g_detailVisible = false;
			g_consumersItemName.clear();
			g_detailLastFrame = -1000;

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

		// Toggles the full Codex window. Opens on the last recipe viewed, or
		// the Buildings tab if nothing has been picked yet. The mod loader
		// gives an exact modifier match (Shift+N) priority over a plain bind
		// on the same key (N), so this never also opens the search box.
		void OnCodexKeyPressed(EModKey /*key*/, EModKeyEvent event)
		{
			if (event != EModKeyEvent::Pressed)
				return;

			if (g_detailVisible)
			{
				CloseDetail();
				return;
			}

			if (!InChimeraMain())
			{
				LOG_DEBUG("CodexUI: codex key ignored (not in ChimeraMain)");
				return;
			}

			CloseSearch();
			g_requestedTab = g_selectedNativeRecipe ? DetailTab::Recipe : DetailTab::Buildings;
			ShowDetailPanel();
		}

		// Height of a row showing an icon beside a name + sub-line (the
		// sub-line is pulled up 4px, see RenderItemRow). Sized from whichever
		// is taller, so larger UI font scales don't push the sub-line past the
		// row's clickable area.
		float TwoLineRowHeight(IModLoaderImGui* imgui, float iconSize)
		{
			const float textHeight = imgui->GetTextLineHeightWithSpacing() + imgui->GetTextLineHeight() - 4.0f;
			return (std::max)(iconSize, textHeight) + 4.0f;
		}

		// Icon (if loaded) + name, as one line. Shared by every recipe list.
		void RenderRecipeLabel(IModLoaderImGui* imgui, IPluginImGuiTextures* textures,
			const CodexRecipes::RecipeInfo& recipe, float iconSize)
		{
			PluginTextureHandle icon = CodexIcons::GetIcon(recipe.output.uniqueItemName);
			if (icon && textures)
			{
				textures->Image(icon, iconSize, iconSize);
				imgui->SameLine(0.0f, 6.0f);
			}
			imgui->Text(recipe.displayName.c_str());
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

			bool clicked = imgui->SelectableFull("##item_row", false, kItemSelectableFlags, 0.0f, TwoLineRowHeight(imgui, iconSize));
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
					PushIDPtr(imgui, consumer.nativeRecipe);
					bool clicked = imgui->SelectableFull("##consumer_row", false, kSelectableFlags, 0.0f, 22.0f);
					imgui->SameLine(0.0f, 0.0f);
					RenderRecipeLabel(imgui, textures, consumer, 18.0f);
					if (consumer.hasTierVariants)
					{
						char tierLabel[32];
						snprintf(tierLabel, sizeof(tierLabel), "(Tier %d)", consumer.minTier);
						imgui->SameLine(0.0f, 6.0f);
						imgui->TextDisabled(tierLabel);
					}
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

		// The search box is intentionally non-resizable (it auto-sizes to
		// its content every frame), so at large ImGui text-scale settings
		// its fixed 440px width would clip the input text / result rows.
		// Instead, scale the width (and the collapsed-height estimate used
		// to centre it) against the user's current font size, relative to
		// ImGui's default 13px font.
		constexpr float kBaselineFontSize = 13.0f;
		constexpr float kSearchBoxBaseWidth = 440.0f;

		void RenderSearchWidget(IModLoaderImGui* imgui)
		{
			const float fontScale = (std::max)(1.0f, imgui->GetFontSize() / kBaselineFontSize);
			const float collapsedHeight = kSearchBoxCollapsedHeight * fontScale;
			g_searchHints.width = kSearchBoxBaseWidth * fontScale;

			float dispW = 1920.0f, dispH = 1080.0f;
			imgui->GetDisplaySize(&dispW, &dispH);
			g_searchHints.pos_x = dispW * 0.5f;
			g_searchHints.pos_y = dispH * 0.5f - collapsedHeight * 0.5f;

			if (g_justOpenedSearch)
			{
				imgui->SetKeyboardFocusHere(0);
				g_justOpenedSearch = false;
			}

			imgui->SetNextItemWidth(-1.0f);
			imgui->InputTextWithHint("##Codex_Search_Input", g_searchHint.c_str(), g_searchBuffer, sizeof(g_searchBuffer));

			// Detected directly off the InputText widget rather than a
			// global "Enter" keybind: the modloader's hotkey system only
			// forwards keys once no ImGui widget has keyboard focus, and
			// typing Enter into a focused field defocuses it - so a global
			// keybind would only fire on a *second* Enter press. Reading
			// ImGui's own deactivated-after-edit flag (true the instant the
			// field submits) sidesteps that entirely.
			//
			// That flag also fires when a *click* steals focus from the box,
			// which happens on the mouse-press frame - one frame before the
			// clicked result row (a Selectable, which reports on release)
			// can return true. Left unguarded, clicking any row would close
			// the search and jump to the top result instead. So ignore a
			// deactivation caused by a mouse press and let the row's own
			// click handle it.
			const bool deactivated = imgui->IsItemDeactivatedAfterEdit();
			const bool mousePressed = imgui->IsMouseClicked(0, false) || imgui->IsMouseClicked(1, false);
			const bool submitted = deactivated && !mousePressed;

			// Refreshed below as results are matched; stale otherwise so
			// Enter can't jump to a result that's no longer shown.
			g_topSearchResult = nullptr;
			g_topSearchBuilding.clear();

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
			constexpr int kMaxBuildingResults = 3;
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

				PushIDPtr(imgui, recipe.nativeRecipe);
				bool clicked = imgui->SelectableFull("##recipe_row", false, kSelectableFlags, 0.0f, 22.0f);
				imgui->SameLine(0.0f, 0.0f);
				RenderRecipeLabel(imgui, textures, recipe, 18.0f);

				// Tiered recipes share a name - the building tells them apart.
				imgui->SameLine(0.0f, 10.0f);
				imgui->TextDisabled(recipe.buildingName.c_str());
				imgui->PopID();

				if (clicked)
				{
					CloseSearch();
					OpenDetail(recipe.nativeRecipe);
					return;
				}

				++shown;
			}

			int shownBuildings = 0;
			for (const CodexRecipes::BuildingGroup& group : CodexRecipes::GetBuildingGroups())
			{
				if (shownBuildings >= kMaxBuildingResults)
					break;

				if (ToLower(group.displayName).find(term) == std::string::npos)
					continue;

				if (shownBuildings == 0 && shown > 0)
					imgui->Separator();
				if (shownBuildings == 0 && !g_topSearchResult)
					g_topSearchBuilding = group.displayName;

				imgui->PushIDStr(group.displayName.c_str());
				bool clicked = imgui->SelectableFull("##building_row", false, kSelectableFlags, 0.0f, 22.0f);
				imgui->SameLine(0.0f, 0.0f);
				imgui->Text(group.displayName.c_str());
				imgui->SameLine(0.0f, 10.0f);
				imgui->TextDisabled("Building");
				imgui->PopID();

				if (clicked)
				{
					CloseSearch();
					OpenBuilding(group.displayName, 0);
					return;
				}

				++shownBuildings;
			}

			if (submitted && g_topSearchResult)
			{
				void* target = g_topSearchResult;
				CloseSearch();
				OpenDetail(target);
			}
			else if (submitted && !g_topSearchBuilding.empty())
			{
				std::string target = g_topSearchBuilding;
				CloseSearch();
				OpenBuilding(target, 0);
			}
		}

		void RenderRecipeTab(IModLoaderImGui* imgui, IPluginImGuiTextures* textures)
		{
			CodexRecipes::RecipeInfo info;
			if (!g_selectedNativeRecipe || !CodexRecipes::FindByNativeRecipe(g_selectedNativeRecipe, info))
			{
				imgui->TextDisabled("No recipe selected. Search for one, or pick a building in the Buildings tab.");
				return;
			}

			const bool pinned = IsPinned(info.nativeRecipe);
			const bool canPin = !pinned && HasFreePinSlot();
			imgui->BeginDisabled(!canPin);
			if (imgui->SmallButton(pinned ? "Pinned" : "Pin"))
				PinRecipe(info.nativeRecipe);
			imgui->EndDisabled();
			if (!pinned && !canPin)
				imgui->SetItemTooltip("All pin slots are in use - unpin a recipe first.");
			else if (!pinned)
				imgui->SetItemTooltip("Keep this recipe on screen while you play.");

			imgui->SameLine(0.0f, 12.0f);
			imgui->SeparatorText(info.displayName.c_str());

			// "Made in: Fabricator (Tier 1), Fabricator (Tier 2)" - each
			// building is a link into the building browser.
			imgui->Text("Made in:");
			for (size_t i = 0; i < info.buildings.size(); ++i)
			{
				const CodexRecipes::BuildingRef& building = info.buildings[i];
				imgui->SameLine(0.0f, i == 0 ? 6.0f : 0.0f);
				imgui->PushIDInt(static_cast<int>(i));
				if (imgui->TextLink(building.label.c_str()))
					OpenBuilding(building.groupName, building.tier);
				imgui->PopID();
				if (i + 1 < info.buildings.size())
				{
					imgui->SameLine(0.0f, 0.0f);
					imgui->Text(", ");
				}
			}

			char line[256];
			if (info.buildTimeSeconds > 0.0f)
				snprintf(line, sizeof(line), "Craft time: %.1fs  (%.1f/min)", info.buildTimeSeconds, info.outputsPerMinute);
			else
				snprintf(line, sizeof(line), "Craft time: unknown");
			imgui->Text(line);

			imgui->Separator();

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

		// The recipes one building tier can craft, as a grid of icon rows.
		// Left-click opens the recipe; right-click pins it.
		void RenderTierRecipes(IModLoaderImGui* imgui, IPluginImGuiTextures* textures,
			const CodexRecipes::BuildingTier& tier)
		{
			if (tier.recipes.empty())
			{
				imgui->TextDisabled("This building has no known recipes.");
				return;
			}

			constexpr float kIconSize = 28.0f;
			constexpr float kMinColumnWidth = 240.0f;

			float availW = 0.0f, availH = 0.0f;
			imgui->GetContentRegionAvail(&availW, &availH);
			const float fontScale = (std::max)(1.0f, imgui->GetFontSize() / kBaselineFontSize);
			const int columns = (std::max)(1, static_cast<int>(availW / (kMinColumnWidth * fontScale)));

			if (!imgui->BeginTable("##tier_recipes", columns, 0))
				return;

			for (int c = 0; c < columns; ++c)
				imgui->TableSetupColumn(nullptr, kColumnFlagsWidthStretch, 1.0f);

			for (void* nativeRecipe : tier.recipes)
			{
				CodexRecipes::RecipeInfo recipe;
				if (!CodexRecipes::FindByNativeRecipe(nativeRecipe, recipe))
					continue;

				imgui->TableNextColumn();
				PushIDPtr(imgui, nativeRecipe);

				bool clicked = imgui->SelectableFull("##tier_recipe", recipe.nativeRecipe == g_selectedNativeRecipe,
					kItemSelectableFlags, 0.0f, TwoLineRowHeight(imgui, kIconSize));
				const bool rightClicked = imgui->IsItemClicked(1);
				if (imgui->IsItemHovered())
					imgui->SetTooltip(IsPinned(nativeRecipe)
						? "Click to view (pinned)"
						: "Click to view, right-click to pin");
				imgui->SameLine(0.0f, 0.0f);

				PluginTextureHandle icon = CodexIcons::GetIcon(recipe.output.uniqueItemName);
				if (icon && textures)
				{
					textures->Image(icon, kIconSize, kIconSize);
					imgui->SameLine(0.0f, 6.0f);
				}

				imgui->BeginGroup();
				imgui->Text(recipe.displayName.c_str());
				char sub[64];
				if (recipe.outputsPerMinute > 0.0f)
					snprintf(sub, sizeof(sub), "%.1f/min", recipe.outputsPerMinute);
				else
					snprintf(sub, sizeof(sub), "x%d", recipe.output.count);
				imgui->SetCursorPosY(imgui->GetCursorPosY() - 4.0f);
				imgui->TextDisabled(sub);
				imgui->EndGroup();

				imgui->PopID();

				if (rightClicked && !IsPinned(nativeRecipe))
					PinRecipe(nativeRecipe);
				else if (clicked)
					OpenDetail(nativeRecipe);
			}

			imgui->EndTable();
		}

		void RenderBuildingsTab(IModLoaderImGui* imgui, IPluginImGuiTextures* textures)
		{
			if (!CodexRecipes::IsReady())
			{
				imgui->TextDisabled("Recipes are still loading...");
				return;
			}

			std::vector<CodexRecipes::BuildingGroup> groups = CodexRecipes::GetBuildingGroups();
			if (groups.empty())
			{
				imgui->TextDisabled("No crafting buildings found.");
				return;
			}

			const CodexRecipes::BuildingGroup* selected = nullptr;
			for (const CodexRecipes::BuildingGroup& group : groups)
			{
				if (group.displayName == g_selectedBuildingGroup)
					selected = &group;
			}
			if (!selected)
			{
				selected = &groups.front();
				g_selectedBuildingGroup = selected->displayName;
			}

			const float fontScale = (std::max)(1.0f, imgui->GetFontSize() / kBaselineFontSize);

			if (imgui->BeginChild("##building_list", 190.0f * fontScale, 0.0f, true))
			{
				for (const CodexRecipes::BuildingGroup& group : groups)
				{
					imgui->PushIDStr(group.displayName.c_str());
					if (imgui->Selectable(group.displayName.c_str(), &group == selected))
					{
						g_selectedBuildingGroup = group.displayName;
						g_requestedTier = 0;
					}
					imgui->PopID();
				}
			}
			imgui->EndChild();

			imgui->SameLine(0.0f, 8.0f);

			if (imgui->BeginChild("##building_recipes", 0.0f, 0.0f, false))
			{
				imgui->SeparatorText(selected->displayName.c_str());

				if (selected->tiers.size() == 1)
				{
					RenderTierRecipes(imgui, textures, selected->tiers.front());
				}
				else if (imgui->BeginTabBar("##building_tiers", 0))
				{
					for (const CodexRecipes::BuildingTier& tier : selected->tiers)
					{
						char label[32];
						snprintf(label, sizeof(label), "Tier %d", tier.tier);
						const int flags = g_requestedTier == tier.tier ? kTabItemFlagsSetSelected : 0;
						if (imgui->BeginTabItem(label, nullptr, flags))
						{
							RenderTierRecipes(imgui, textures, tier);
							imgui->EndTabItem();
						}
					}
					imgui->EndTabBar();
				}
				g_requestedTier = 0;
			}
			imgui->EndChild();
		}

		void RenderDetailWidget(IModLoaderImGui* imgui)
		{
			// Opened from the mod loader's own panel list rather than via
			// OpenDetail - adopt it so Escape and our state track it too.
			if (!g_detailVisible)
			{
				g_detailVisible = true;
				AcquireEscape();
				AcquireCapture();
				if (!g_selectedNativeRecipe && g_requestedTab == DetailTab::None)
					g_requestedTab = DetailTab::Buildings;
			}
			g_detailLastFrame = imgui->GetFrameCount();

			if (imgui->Button("Close"))
			{
				CloseDetail();
				return;
			}

			// Opens the search box over this window; picking a result then
			// navigates here instead of opening a new window.
			imgui->SameLine(0.0f, 6.0f);
			if (imgui->Button("Search"))
				OpenSearch();

			IPluginImGuiTextures* textures = GetHooks() ? GetHooks()->ImGuiTextures : nullptr;

			const DetailTab requested = g_requestedTab;
			g_requestedTab = DetailTab::None;

			if (imgui->BeginTabBar("##codex_tabs", 0))
			{
				if (imgui->BeginTabItem("Recipe", nullptr, requested == DetailTab::Recipe ? kTabItemFlagsSetSelected : 0))
				{
					RenderRecipeTab(imgui, textures);
					imgui->EndTabItem();
				}
				if (imgui->BeginTabItem("Buildings", nullptr, requested == DetailTab::Buildings ? kTabItemFlagsSetSelected : 0))
				{
					RenderBuildingsTab(imgui, textures);
					imgui->EndTabItem();
				}
				imgui->EndTabBar();
			}
		}

		// ------------------------------------------------------------------
		// Pinned recipes
		//
		// Each pin is its own always-visible widget window. Widgets never
		// acquire input capture on their own, and the modloader only feeds
		// ImGui the mouse while something holds capture - so with only pins
		// on screen, the game keeps every mouse/keyboard input. While the
		// Codex search/detail is open (which does hold capture) the pins
		// become interactive: drag to move, Unpin to close.
		//
		// Widget render callbacks carry no user data, so a fixed pool of
		// slots each gets its own template-instantiated callback.
		// ------------------------------------------------------------------
		constexpr int kMaxPins = 8;

		struct PinSlot
		{
			void*             nativeRecipe = nullptr;
			WidgetHandle      widget = nullptr;
			PluginWidgetDesc  desc{};
			PluginWindowHints hints{};
			char              title[192] = "";
		};

		PinSlot g_pins[kMaxPins];
		float   g_lastDisplayW = 1920.0f;

		bool CodexUIActive(IModLoaderImGui* imgui)
		{
			return g_searchVisible || imgui->GetFrameCount() - g_detailLastFrame <= 1;
		}

		bool IsPinned(void* nativeRecipe)
		{
			for (const PinSlot& slot : g_pins)
			{
				if (slot.nativeRecipe && slot.nativeRecipe == nativeRecipe)
					return true;
			}
			return false;
		}

		bool HasFreePinSlot()
		{
			for (const PinSlot& slot : g_pins)
			{
				if (!slot.nativeRecipe && slot.widget)
					return true;
			}
			return false;
		}

		void PinRecipe(void* nativeRecipe)
		{
			if (!nativeRecipe || IsPinned(nativeRecipe))
				return;

			CodexRecipes::RecipeInfo info;
			if (!CodexRecipes::FindByNativeRecipe(nativeRecipe, info))
				return;

			for (int i = 0; i < kMaxPins; ++i)
			{
				PinSlot& slot = g_pins[i];
				if (slot.nativeRecipe || !slot.widget)
					continue;

				slot.nativeRecipe = nativeRecipe;
				snprintf(slot.title, sizeof(slot.title), "%s###CodexPin%d", info.displayName.c_str(), i);

				// Cascade new pins down the right-hand edge of the screen;
				// placed once (cond Always for a single frame), after which
				// the slot's render clears pos_x so the user's drag sticks.
				slot.hints.pos_x   = g_lastDisplayW - 24.0f;
				slot.hints.pos_y   = 140.0f + 36.0f * static_cast<float>(i);
				slot.hints.pivot_x = 1.0f;
				slot.hints.pivot_y = 0.0f;
				slot.hints.pos_cond = 0;

				if (g_self && g_self->hooks->UI)
					g_self->hooks->UI->SetWidgetVisible(slot.widget, true);
				return;
			}
		}

		void UnpinSlot(int index)
		{
			PinSlot& slot = g_pins[index];
			slot.nativeRecipe = nullptr;
			if (g_self && g_self->hooks->UI && slot.widget)
				g_self->hooks->UI->SetWidgetVisible(slot.widget, false);
		}

		void RenderPinSlot(int index, IModLoaderImGui* imgui)
		{
			PinSlot& slot = g_pins[index];

			float dispW = 0.0f, dispH = 0.0f;
			imgui->GetDisplaySize(&dispW, &dispH);
			if (dispW > 0.0f)
				g_lastDisplayW = dispW;

			// The initial placement has now been applied - stop forcing it.
			slot.hints.pos_x = -1.0f;
			slot.hints.pos_y = -1.0f;

			// Applied next frame. NoMouseInputs while the Codex is closed is
			// belt-and-braces: ImGui isn't fed the mouse then anyway, but a
			// stale cursor position must never make a pin eat a click.
			const bool interactive = CodexUIActive(imgui);
			slot.hints.extra_window_flags = kWindowFlagsAlwaysAutoResize | PluginWindowFlags_NoSavedSettings |
				(interactive ? 0 : PluginWindowFlags_NoMouseInputs);

			CodexRecipes::RecipeInfo info;
			if (!slot.nativeRecipe || !CodexRecipes::FindByNativeRecipe(slot.nativeRecipe, info))
			{
				imgui->TextDisabled("Recipe unavailable.");
				if (interactive && imgui->SmallButton("Unpin"))
					UnpinSlot(index);
				return;
			}

			IPluginImGuiTextures* textures = GetHooks() ? GetHooks()->ImGuiTextures : nullptr;
			constexpr float kIconSize = 20.0f;

			char line[256];
			snprintf(line, sizeof(line), "%s  -  %.1fs", info.buildingName.c_str(), info.buildTimeSeconds);
			imgui->TextDisabled(line);

			for (const CodexRecipes::RecipeItemRef& input : info.inputs)
			{
				PluginTextureHandle icon = CodexIcons::GetIcon(input.uniqueItemName);
				if (icon && textures)
				{
					textures->Image(icon, kIconSize, kIconSize);
					imgui->SameLine(0.0f, 6.0f);
				}
				const float rate = info.buildTimeSeconds > 0.0f
					? (static_cast<float>(input.count) / info.buildTimeSeconds) * 60.0f
					: 0.0f;
				if (rate > 0.0f)
					snprintf(line, sizeof(line), "%dx %s  (%.1f/min)", input.count, input.displayName.c_str(), rate);
				else
					snprintf(line, sizeof(line), "%dx %s", input.count, input.displayName.c_str());
				imgui->AlignTextToFramePadding();
				imgui->Text(line);
			}

			imgui->Separator();

			PluginTextureHandle outIcon = CodexIcons::GetIcon(info.output.uniqueItemName);
			if (outIcon && textures)
			{
				textures->Image(outIcon, kIconSize, kIconSize);
				imgui->SameLine(0.0f, 6.0f);
			}
			if (info.outputsPerMinute > 0.0f)
				snprintf(line, sizeof(line), "%dx %s  (%.1f/min)", info.output.count, info.output.displayName.c_str(), info.outputsPerMinute);
			else
				snprintf(line, sizeof(line), "%dx %s", info.output.count, info.output.displayName.c_str());
			imgui->AlignTextToFramePadding();
			imgui->Text(line);

			if (interactive)
			{
				if (imgui->SmallButton("Open"))
					OpenDetail(info.nativeRecipe);
				imgui->SameLine(0.0f, 6.0f);
				if (imgui->SmallButton("Unpin"))
					UnpinSlot(index);
			}
		}

		template <int Index>
		void RenderPinSlotThunk(IModLoaderImGui* imgui)
		{
			RenderPinSlot(Index, imgui);
		}

		template <int... Indices>
		constexpr auto MakePinRenderFns(std::integer_sequence<int, Indices...>)
		{
			return std::array<PluginImGuiRenderCallback, sizeof...(Indices)>{ &RenderPinSlotThunk<Indices>... };
		}

		constexpr auto kPinRenderFns = MakePinRenderFns(std::make_integer_sequence<int, kMaxPins>{});

		void RegisterPinSlots(IPluginSelf* self)
		{
			for (int i = 0; i < kMaxPins; ++i)
			{
				PinSlot& slot = g_pins[i];
				snprintf(slot.title, sizeof(slot.title), "Pinned Recipe###CodexPin%d", i);

				slot.hints.width  = 0.0f;
				slot.hints.height = 0.0f;
				slot.hints.pos_x  = -1.0f;
				slot.hints.pos_y  = -1.0f;
				slot.hints.size_cond = 0;
				slot.hints.pos_cond  = 0;
				slot.hints.extra_window_flags = kWindowFlagsAlwaysAutoResize | PluginWindowFlags_NoSavedSettings |
					PluginWindowFlags_NoMouseInputs;

				slot.desc.name        = slot.title;
				slot.desc.renderFn    = kPinRenderFns[i];
				slot.desc.windowHints = &slot.hints;

				slot.widget = self->hooks->UI->RegisterWidget(&slot.desc);
				if (slot.widget)
					self->hooks->UI->SetWidgetVisible(slot.widget, false);
			}
		}

		void UnregisterPinSlots(IPluginSelf* self)
		{
			for (PinSlot& slot : g_pins)
			{
				slot.nativeRecipe = nullptr;
				if (slot.widget)
				{
					self->hooks->UI->UnregisterWidget(slot.widget);
					slot.widget = nullptr;
				}
			}
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

		// Registered as a Panel (not a Widget) so it gets the same
		// freely-resizable/movable window as every other plugin's info
		// window (BetterCheats, ProductionViewer) - Widgets are meant for
		// pinned, non-resizable overlays like the search box above.
		g_detailDesc.buttonLabel = "Codex";
		g_detailDesc.windowTitle = "Codex";
		g_detailDesc.renderFn    = &RenderDetailWidget;
		g_detailPanel = self->hooks->UI->RegisterPanel(&g_detailDesc);
		self->hooks->UI->RegisterOnPanelWindowClosed(&OnDetailPanelClosed);

		// Zero-size, input-less and backgroundless: it exists for one frame
		// only to run CloseDetailNow outside the panel render loop.
		g_closerHints.width   = 1.0f;
		g_closerHints.height  = 1.0f;
		g_closerHints.pos_x   = 0.0f;
		g_closerHints.pos_y   = 0.0f;
		g_closerHints.pivot_x = 0.0f;
		g_closerHints.pivot_y = 0.0f;
		g_closerHints.size_cond = 0;
		g_closerHints.pos_cond  = 0;
		g_closerHints.extra_window_flags = PluginWindowFlags_NoTitleBar | PluginWindowFlags_NoResize |
			PluginWindowFlags_NoMove | PluginWindowFlags_NoScrollbar | PluginWindowFlags_NoBackground |
			PluginWindowFlags_NoSavedSettings | PluginWindowFlags_NoMouseInputs;

		g_closerDesc.name        = "##CodexCloser";
		g_closerDesc.renderFn    = &RenderCloserWidget;
		g_closerDesc.windowHints = &g_closerHints;
		g_closerWidget = self->hooks->UI->RegisterWidget(&g_closerDesc);
		if (g_closerWidget)
			self->hooks->UI->SetWidgetVisible(g_closerWidget, false);

		RegisterPinSlots(self);

		if (self->hooks->Input)
		{
			const char* searchKey = CodexConfig::Config::GetSearchKey();
			LOG_DEBUG("CodexUI: registering search keybind '%s'", searchKey);
			self->hooks->Input->RegisterKeybindByName(searchKey, EModKeyEvent::Pressed, &OnSearchKeyPressed);

			const char* codexKey = CodexConfig::Config::GetCodexKey();
			LOG_DEBUG("CodexUI: registering codex keybind '%s'", codexKey);
			self->hooks->Input->RegisterKeybindByName(codexKey, EModKeyEvent::Pressed, &OnCodexKeyPressed);
		}

		LOG_INFO("CodexUI: search, detail and pin windows registered");
	}

	void Shutdown(IPluginSelf* self)
	{
		if (!self->hooks->UI)
		{
			g_self = nullptr;
			return;
		}

		CloseSearch();
		CloseDetailNow();

		if (g_closerWidget)
		{
			self->hooks->UI->UnregisterWidget(g_closerWidget);
			g_closerWidget = nullptr;
		}

		if (self->hooks->Input)
		{
			const char* searchKey = CodexConfig::Config::GetSearchKey();
			self->hooks->Input->UnregisterKeybindByName(searchKey, EModKeyEvent::Pressed, &OnSearchKeyPressed);

			const char* codexKey = CodexConfig::Config::GetCodexKey();
			self->hooks->Input->UnregisterKeybindByName(codexKey, EModKeyEvent::Pressed, &OnCodexKeyPressed);
		}

		if (g_searchWidget)
		{
			self->hooks->UI->UnregisterWidget(g_searchWidget);
			g_searchWidget = nullptr;
		}

		UnregisterPinSlots(self);

		self->hooks->UI->UnregisterOnPanelWindowClosed(&OnDetailPanelClosed);
		if (g_detailPanel)
		{
			self->hooks->UI->UnregisterPanel(g_detailPanel);
			g_detailPanel = nullptr;
		}

		g_self = nullptr;
	}
}
