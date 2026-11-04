#include "codex_ui.h"
#include "game_tooltip_pin.h"
#include "Recipes/codex_recipes.h"
#include "Planner/codex_planner.h"
#include "Icons/codex_icons.h"
#include "Plugin Core/Helpers/plugin_helpers.h"
#include "Plugin Core/Config/plugin_config.h"
#include "Plugin Core/codex_fields.h"
#include "UI/pin_layout.h"
#include "UI/pin_set.h"

#include "Engine_classes.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
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

		// Invisible one-frame widget that runs render-thread work handed over
		// from elsewhere: the detail panel's close, the saved pins' restore and
		// a pin asked for from a game menu - see RenderCloserWidget.
		WidgetHandle      g_closerWidget = nullptr;
		PluginWindowHints g_closerHints{};
		PluginWidgetDesc  g_closerDesc{};

		// Saved pins, put back once the recipe list is ready (see
		// RestorePinnedRecipes). Waiting is set by Init and cleared by Tick,
		// which runs on the game thread; Render is set by Tick and cleared by
		// the closer widget, so the pins are made on the render thread.
		std::atomic<bool> g_restoreWaiting{ false };
		std::atomic<bool> g_restoreRender{ false };

		// Work handed to the closer widget, which runs it on the render thread:
		// closing the detail panel (CloseDetail) and the item a game menu asked
		// to pin (PinProductOf), which any thread may set.
		std::atomic<bool> g_closeRender{ false };
		std::mutex        g_gamePinMutex;
		std::string       g_gamePinItem;

		// The saved pin set as last read or written, one key per slot, kept so
		// Clear on the config page, which empties the values, can put them back
		// whether or not the pins are up yet. Render thread only.
		std::vector<std::string> g_savedPins;

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

		// Row of the search dropdown that Enter opens, counting recipes first,
		// then buildings. Reset to the top result by every new search; moved
		// by the Up/Down keys and by the mouse.
		int g_searchHighlight = 0;

		// Last mouse position the search box saw, so a resting cursor doesn't
		// fight the keyboard for the highlight.
		float g_searchMouseX = 0.0f;
		float g_searchMouseY = 0.0f;

		// Search box placement. The loader re-applies a widget's position
		// hints every frame, which would pin the box in place, so the box is
		// placed for a frame after opening and then left to ImGui so it can
		// be dragged. g_searchPos* is the top-left corner it was last placed
		// or dropped at; a drop that changes it is saved to the config.
		// g_searchSaved* is that saved spot as screen fractions (negative =
		// centred), kept here so a Clear on the config page applies at once.
		int   g_searchPlaceFrames = 0;
		bool  g_searchPlaced = false;
		float g_searchPosX = 0.0f;
		float g_searchPosY = 0.0f;
		float g_searchSavedX = -1.0f;
		float g_searchSavedY = -1.0f;

		// Last display size a Codex window rendered on - places the search
		// box and pins on the frame they appear, before they can query it.
		float g_displayW = 1920.0f;
		float g_displayH = 1080.0f;

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
		constexpr int kMouseCursorResizeAll = 2;

		void OnEscapePressed(EModKey key, EModKeyEvent event);
		void OnSearchKeyPressed(EModKey key, EModKeyEvent event);
		void OnCodexKeyPressed(EModKey key, EModKeyEvent event);
		void CloseSearch();
		void CloseDetail();
		void OnDetailPanelClosed(PanelHandle handle);
		void PlaceSearchBox(float fontScale);
		void PinRecipe(IModLoaderImGui* imgui, void* nativeRecipe, bool restoring = false);
		void RestorePinnedRecipes(IModLoaderImGui* imgui);
		void CommitPinnedRecipes();
		void ArrangePins(IModLoaderImGui* imgui);
		bool IsPinned(void* nativeRecipe);
		bool IsProductPinned(const std::string& uniqueItemName);
		void PinItemProducer(IModLoaderImGui* imgui, const std::string& uniqueItemName);
		bool HasFreePinSlot();
		bool AnyPinned();
		int  PinCount();

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
			SDK::UWorld* world = PluginSdkRuntime::GetWorld();
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
			g_searchHighlight = 0;
			g_searchPlaceFrames = 1;
			g_searchPlaced = false;
			PlaceSearchBox(1.0f);
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
			{
				g_closeRender = true;
				g_self->hooks->UI->SetWidgetVisible(g_closerWidget, true);
			}
			else
				CloseDetailNow();
		}

		void RenderCloserWidget(IModLoaderImGui* imgui)
		{
			if (g_self && g_self->hooks->UI && g_closerWidget)
				g_self->hooks->UI->SetWidgetVisible(g_closerWidget, false);
			if (g_restoreRender.exchange(false))
				RestorePinnedRecipes(imgui);

			std::string gamePin;
			{
				std::lock_guard<std::mutex> lock(g_gamePinMutex);
				gamePin.swap(g_gamePinItem);
			}
			if (!gamePin.empty())
				PinItemProducer(imgui, gamePin);

			if (g_closeRender.exchange(false))
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

		// Right-click on the item just drawn pins a recipe: `recipe` when the item
		// is that recipe's own product, otherwise the one that makes the item.
		// Items nothing makes (raw resources) do nothing, an item some pin already
		// makes is not pinned again, and PinRecipe skips a recipe that is pinned
		// and a full set of pins.
		void PinOnRightClick(IModLoaderImGui* imgui, const std::string& uniqueItemName, void* recipe = nullptr)
		{
			if (!imgui->IsItemClicked(1))
				return;

			if (recipe)
				PinRecipe(imgui, recipe);
			else
				PinItemProducer(imgui, uniqueItemName);
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
			PinOnRightClick(imgui, item.uniqueItemName, isOutput ? g_selectedNativeRecipe : nullptr);
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
					PinOnRightClick(imgui, consumer.output.uniqueItemName, consumer.nativeRecipe);
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

		// Puts the search box where the player last dropped it, or centred
		// (as if it were collapsed) when it has never been moved. The
		// position is saved as screen fractions, so it survives a resolution
		// change.
		void PlaceSearchBox(float fontScale)
		{
			const float x = g_searchSavedX, y = g_searchSavedY;
			const bool saved = x >= 0.0f && x <= 1.0f && y >= 0.0f && y <= 1.0f;

			g_searchHints.pos_x = (saved ? x : 0.5f) * g_displayW;
			g_searchHints.pos_y = saved
				? y * g_displayH
				: g_displayH * 0.5f - kSearchBoxCollapsedHeight * fontScale * 0.5f;
		}

		// Down presses minus Up presses this frame, repeating while a key is
		// held. Read through ImGui rather than registered as keybinds: the
		// modloader withholds keybinds while an ImGui text field has focus,
		// which the search field does for as long as the player is typing.
		int SearchArrowSteps(IModLoaderImGui* imgui)
		{
			if (!imgui->IsWindowFocused(0))
				return 0;
			return (imgui->IsKeyPressed(VK_DOWN, true) ? 1 : 0) - (imgui->IsKeyPressed(VK_UP, true) ? 1 : 0);
		}

		// Called straight after a result row's Selectable: a moving mouse
		// pulls the highlight to the row under it, and a row reached with
		// the arrow keys is scrolled into view.
		void SyncSearchHighlight(IModLoaderImGui* imgui, int row, bool mouseMoved, bool keyMoved)
		{
			if (mouseMoved && imgui->IsItemHovered())
				g_searchHighlight = row;
			if (keyMoved && row == g_searchHighlight)
				imgui->SetScrollHereY(0.5f);
		}

		// Shows the move cursor over the parts of a window the player can grab
		// and drag: anywhere that is not a control.
		void ShowDragCursor(IModLoaderImGui* imgui)
		{
			if (imgui->IsWindowHovered(0) && !imgui->IsAnyItemHovered())
				imgui->SetMouseCursor(kMouseCursorResizeAll);
		}

		void RenderSearchWidget(IModLoaderImGui* imgui)
		{
			const float fontScale = (std::max)(1.0f, imgui->GetFontSize() / kBaselineFontSize);
			g_searchHints.width = kSearchBoxBaseWidth * fontScale;

			imgui->GetDisplaySize(&g_displayW, &g_displayH);

			if (g_searchPlaceFrames > 0)
			{
				// The opening frame used the last known display size and font
				// scale; this one has the real ones.
				--g_searchPlaceFrames;
				PlaceSearchBox(fontScale);
			}
			else
			{
				g_searchHints.pos_x = -1.0f;
				g_searchHints.pos_y = -1.0f;

				float posX = 0.0f, posY = 0.0f, sizeX = 0.0f, sizeY = 0.0f;
				imgui->GetWindowPos(&posX, &posY);
				imgui->GetWindowSize(&sizeX, &sizeY);
				if (!g_searchPlaced)
				{
					g_searchPosX = posX;
					g_searchPosY = posY;
					g_searchPlaced = true;
				}
				else if (imgui->IsMouseReleased(0) &&
					(std::fabs(posX - g_searchPosX) > 0.5f || std::fabs(posY - g_searchPosY) > 0.5f))
				{
					g_searchPosX = posX;
					g_searchPosY = posY;
					g_searchSavedX = (posX + sizeX * 0.5f) / g_displayW;
					g_searchSavedY = posY / g_displayH;
					CodexConfig::Config::SetSearchBoxPos(g_searchSavedX, g_searchSavedY);

					// Dragging the box takes focus off the field; give it back so
					// typing and Enter keep working.
					g_justOpenedSearch = true;
				}
			}

			ShowDragCursor(imgui);

			if (g_justOpenedSearch)
			{
				imgui->SetKeyboardFocusHere(0);
				g_justOpenedSearch = false;
			}

			imgui->SetNextItemWidth(-1.0f);
			if (imgui->InputTextWithHint("##Codex_Search_Input", g_searchHint.c_str(), g_searchBuffer, sizeof(g_searchBuffer)))
				g_searchHighlight = 0;

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
			// the search and jump to the highlighted result instead. So
			// ignore a deactivation caused by a mouse press and let the
			// row's own click handle it.
			const bool deactivated = imgui->IsItemDeactivatedAfterEdit();
			const bool mousePressed = imgui->IsMouseClicked(0, false) || imgui->IsMouseClicked(1, false);
			const bool submitted = deactivated && !mousePressed;

			float mouseX = 0.0f, mouseY = 0.0f;
			imgui->GetMousePos(&mouseX, &mouseY);
			const bool mouseMoved = mouseX != g_searchMouseX || mouseY != g_searchMouseY;
			g_searchMouseX = mouseX;
			g_searchMouseY = mouseY;

			const int arrowSteps = SearchArrowSteps(imgui);

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
			std::vector<CodexRecipes::BuildingGroup> groups = CodexRecipes::GetBuildingGroups();

			IPluginImGuiTextures* textures = GetHooks() ? GetHooks()->ImGuiTextures : nullptr;

			constexpr size_t kMaxResults = 10;
			constexpr size_t kMaxBuildingResults = 3;

			std::vector<const CodexRecipes::RecipeInfo*> recipes;
			for (const CodexRecipes::RecipeInfo& recipe : all)
			{
				if (recipes.size() >= kMaxResults)
					break;
				if (ToLower(recipe.displayName).find(term) != std::string::npos)
					recipes.push_back(&recipe);
			}

			std::vector<const CodexRecipes::BuildingGroup*> buildings;
			for (const CodexRecipes::BuildingGroup& group : groups)
			{
				if (buildings.size() >= kMaxBuildingResults)
					break;
				if (ToLower(group.displayName).find(term) != std::string::npos)
					buildings.push_back(&group);
			}

			const int rowCount = static_cast<int>(recipes.size() + buildings.size());
			if (rowCount == 0)
				return;

			g_searchHighlight = std::clamp(g_searchHighlight + arrowSteps, 0, rowCount - 1);

			imgui->Separator();

			int row = 0;
			for (const CodexRecipes::RecipeInfo* recipe : recipes)
			{
				PushIDPtr(imgui, recipe->nativeRecipe);
				bool clicked = imgui->SelectableFull("##recipe_row", row == g_searchHighlight, kSelectableFlags, 0.0f, 22.0f);
				PinOnRightClick(imgui, recipe->output.uniqueItemName, recipe->nativeRecipe);
				SyncSearchHighlight(imgui, row, mouseMoved, arrowSteps != 0);
				imgui->SameLine(0.0f, 0.0f);
				RenderRecipeLabel(imgui, textures, *recipe, 18.0f);

				// Tiered recipes share a name - the building tells them apart.
				imgui->SameLine(0.0f, 10.0f);
				imgui->TextDisabled(recipe->buildingName.c_str());
				imgui->PopID();

				if (clicked)
				{
					void* target = recipe->nativeRecipe;
					CloseSearch();
					OpenDetail(target);
					return;
				}

				++row;
			}

			if (!recipes.empty() && !buildings.empty())
				imgui->Separator();

			for (const CodexRecipes::BuildingGroup* group : buildings)
			{
				imgui->PushIDStr(group->displayName.c_str());
				bool clicked = imgui->SelectableFull("##building_row", row == g_searchHighlight, kSelectableFlags, 0.0f, 22.0f);
				SyncSearchHighlight(imgui, row, mouseMoved, arrowSteps != 0);
				imgui->SameLine(0.0f, 0.0f);
				imgui->Text(group->displayName.c_str());
				imgui->SameLine(0.0f, 10.0f);
				imgui->TextDisabled("Building");
				imgui->PopID();

				if (clicked)
				{
					std::string target = group->displayName;
					CloseSearch();
					OpenBuilding(target, 0);
					return;
				}

				++row;
			}

			if (submitted)
			{
				const size_t highlighted = static_cast<size_t>(g_searchHighlight);
				if (highlighted < recipes.size())
				{
					void* target = recipes[highlighted]->nativeRecipe;
					CloseSearch();
					OpenDetail(target);
				}
				else
				{
					std::string target = buildings[highlighted - recipes.size()]->displayName;
					CloseSearch();
					OpenBuilding(target, 0);
				}
			}
		}

		// The Planner button, shared by the recipe tab and the pinned
		// windows.
		void RenderLaunchPlannerButton(IModLoaderImGui* imgui, const CodexRecipes::RecipeInfo& info)
		{
			if (imgui->SmallButton("Planner"))
				CodexPlanner::Launch(info.output.uniqueItemName, info.output.displayName);
			imgui->SetItemTooltip("Open a production planner for this item in your browser.");
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
				PinRecipe(imgui, info.nativeRecipe);
			imgui->EndDisabled();
			if (!pinned && !canPin)
				imgui->SetItemTooltip("All pin slots are in use - unpin a recipe first.");
			else if (!pinned)
				imgui->SetItemTooltip("Keep this recipe on screen while you play. Right-click an item to pin its recipe.");

			imgui->SameLine(0.0f, -1.0f);
			RenderLaunchPlannerButton(imgui, info);

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
					PinRecipe(imgui, nativeRecipe);
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

			// Opens the search box over this window; picking a result then
			// navigates here instead of opening a new window.
			if (imgui->Button("Search"))
				OpenSearch();

			if (AnyPinned())
			{
				imgui->SameLine(0.0f, 6.0f);
				if (imgui->Button("Arrange pins"))
					ArrangePins(imgui);
			}

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
		// ImGui the mouse while something holds capture or the Escape menu is
		// open (there only to windows that opt in, as the pins do) - so with
		// only pins on screen, the game keeps every mouse/keyboard input.
		// Whenever one of those is up (the Codex search/detail, the
		// modloader's F2 overlay, the Escape menu) any pin can be dragged and
		// the others follow it, tiled or cascaded (Grouped), or every pin is
		// dragged on its own (Ungrouped); the
		// Open/Planner buttons and the close X on the title bar only show while
		// the Codex itself or the Escape menu is open.
		//
		// Widget render callbacks carry no user data, so a fixed pool of
		// slots each gets its own template-instantiated callback.
		// ------------------------------------------------------------------
		constexpr int kMaxPins = CodexPinLayout::kMaxPins;

		// Where a pinned window sits on screen: top-left corner and size, in
		// pixels. Measured from the window every frame; valid once it has
		// rendered.
		struct PinRect
		{
			float x = 0.0f;
			float y = 0.0f;
			float w = 0.0f;
			float h = 0.0f;
			bool  valid = false;
		};

		// A pin easing from where it was to its new spot (CodexPinLayout::GlideAt).
		// The spot it is heading for is `to`; the in-between frames are only ever
		// requested as the window's position, never saved.
		struct PinGlide
		{
			bool                   active = false;
			CodexPinLayout::Point  from;
			CodexPinLayout::Point  to;
			float                  elapsed = 0.0f;
		};

		struct PinSlot
		{
			void*             nativeRecipe = nullptr;
			std::string       recipeKey; // the recipe's asset name, as saved between sessions
			WidgetHandle      widget = nullptr;
			PluginWidgetDesc  desc{};
			PluginWindowHints hints{};
			PinRect           rect;
			char              title[192] = "";
			bool              dragging = false;
			float             dragFromX = 0.0f;
			float             dragFromY = 0.0f;
			float             grabX = 0.0f; // where in the window the drag took hold
			float             grabY = 0.0f;
			bool              placePending = false; // ungrouped: a new pin waiting for its size to be placed
			bool              placeWait = false;    // ... and for its window to have been drawn once, so the size is its own
			bool              applyFree = false;    // ungrouped: put the window at its saved spot
			PinGlide          glide;                // easing to a new spot on its own
			bool              fresh = false;        // just pinned: its first spot is taken at once, not eased to
			bool              restoring = false;    // ungrouped: a saved pin, back at its saved spot rather than a new pin's
		};

		PinSlot  g_pins[kMaxPins];

		// How the pins are laid out, from the config. The lead pin (the one
		// pinned first) is the one that moves; a drag of any pin carries it,
		// and the rest follow it.
		// Each arrangement remembers where the lead was last dropped, as a
		// fraction of the screen (its top-left corner) so it survives a
		// resolution change. Written to the config when a drag ends, never
		// per frame.
		struct PinAnchor
		{
			float x = 0.0f;
			float y = 0.0f;
			bool  set = false;
		};

		using PinEdge = CodexPinLayout::Edge;

		struct PinSettings
		{
			bool      cascade = false;
			PinEdge   edge = PinEdge::Right;
			int       gap = CodexPinLayout::kGapDefault; // pixels between tiles
			bool      center = true;
			PinAnchor anchors[2]; // indexed by `cascade`
			bool      free = false; // ungrouped: every pin has its own spot
			PinAnchor freeAnchors[kMaxPins]; // ungrouped: each pin's spot, in pin order
		};

		PinSettings g_pinSettings;

		constexpr int kPinWindowFlags = PluginWindowFlags_AlwaysAutoResize | PluginWindowFlags_NoSavedSettings |
			PluginWindowFlags_PauseMenuMouseInputs;

		// Set when the lead pin has to be put back at its anchor: the layout
		// was changed in the config, the old lead was unpinned, the screen
		// size changed, or the player asked for Arrange pins.
		bool  g_pinReanchor = false;
		float g_pinAnchoredW = 0.0f;
		float g_pinAnchoredH = 0.0f;
		float g_pinAnchoredLeadW = 0.0f; // the lead's width when it was last placed

		// Ungrouped, set when the pin grouping was changed, when the saved spots
		// were edited by hand, or when they were cleared (every pin is placed
		// again as if just pinned). The first pin's render acts on them.
		bool g_pinModeChanged = false;
		bool g_pinApplyFree = false;
		bool g_pinRespawn = false;

		// Ungrouped, set once the player has dragged a pin by hand (or the saved
		// spots came from an earlier session): centering then leaves the pins
		// alone when a pin is added or removed. Clear and Arrange reset it.
		bool g_pinHandPlaced = false;

		// Grouped, a follower was pressed: the first pin takes the drag up on its
		// next render, as if it had been grabbed itself, so the whole group moves
		// by the mouse's move. The mouse position is where the press landed.
		struct PinGrab
		{
			bool  pending = false;
			float mouseX = 0.0f;
			float mouseY = 0.0f;
		};
		PinGrab g_pinGrab;

		// The PinLayout was changed while pins were up (the edge it was before):
		// the first pin's render moves the pins to the closest equivalent spot.
		bool    g_pinSwitchPending = false;
		PinEdge g_pinSwitchFrom = PinEdge::Right;

		// Pins that move on their own ease to their new spot. For this long after
		// such a move was set off (a pin added or removed, a layout switch, Clear,
		// Arrange), a pin whose spot changes glides instead of jumping.
		constexpr float kPinGlideWindow = 0.4f;
		float  g_pinGlideWindow = 0.0f;
		double g_pinLastTime = 0.0;
		float  g_pinDeltaTime = 0.0f;

		// One "0.1234,0.5678" entry, with room to spare.
		constexpr int kPinOffsetBufferSize = 32;

		// All the ungrouped entries, joined with ";".
		constexpr int kPinFreeOffsetsBufferSize = kPinOffsetBufferSize * kMaxPins;

		// One saved pin's key: the config holds a value of up to this many
		// characters, and an asset name takes a fraction of it.
		constexpr int kPinKeyBufferSize = static_cast<int>(CodexPinSet::kMaxValueLen) + 1;

		// The old single value of all the pins, as the first pin save kept it.
		constexpr int kLegacyPinnedRecipesBufferSize = 1024;

		// A drop that moves the window less than this is a click, not a drag.
		constexpr float kPinDragThreshold = 0.5f;

		PinAnchor ParsePinAnchor(const char* text)
		{
			PinAnchor anchor;
			if (sscanf_s(text, "%f,%f", &anchor.x, &anchor.y) == 2)
				anchor.set = anchor.x >= 0.0f && anchor.x <= 1.0f && anchor.y >= 0.0f && anchor.y <= 1.0f;
			return anchor;
		}

		PinAnchor ReadPinAnchor(bool cascade)
		{
			char text[kPinOffsetBufferSize];
			CodexConfig::Config::GetPinOffset(cascade, text, sizeof(text));
			return ParsePinAnchor(text);
		}

		void WritePinAnchor(bool cascade, const PinAnchor& anchor)
		{
			char text[kPinOffsetBufferSize] = "";
			if (anchor.set)
				snprintf(text, sizeof(text), "%.4f,%.4f", anchor.x, anchor.y);
			CodexConfig::Config::SetPinOffset(cascade, text);
		}

		PinEdge ParsePinEdge(const char* layout)
		{
			return _stricmp(layout, "Left") == 0 ? PinEdge::Left
				: _stricmp(layout, "Top") == 0 ? PinEdge::Top
				: PinEdge::Right;
		}

		// The ungrouped spots as "x,y;x,y;..." in pin order; an unset spot is an
		// empty entry, and nothing set at all is an empty string.
		void ParsePinFreeAnchors(const char* text, PinAnchor* out)
		{
			for (int k = 0; k < kMaxPins; ++k)
			{
				out[k] = ParsePinAnchor(text);
				const char* next = strchr(text, ';');
				text = next ? next + 1 : "";
			}
		}

		void FormatPinFreeAnchors(const PinAnchor* anchors, char* text, int size)
		{
			int last = -1;
			for (int k = 0; k < kMaxPins; ++k)
				if (anchors[k].set)
					last = k;

			text[0] = '\0';
			int used = 0;
			for (int k = 0; k <= last && used < size; ++k)
			{
				if (anchors[k].set)
					used += snprintf(text + used, size - used, "%s%.4f,%.4f", k ? ";" : "", anchors[k].x, anchors[k].y);
				else
					used += snprintf(text + used, size - used, "%s", k ? ";" : "");
			}
		}

		bool SamePinAnchor(const PinAnchor& a, const PinAnchor& b)
		{
			return a.set == b.set && a.x == b.x && a.y == b.y;
		}

		// Takes up new pin options. A change to the arrangement, the edge or
		// the current arrangement's anchor moves the lead pin to match.
		void ApplyPinSettings(const PinSettings& next)
		{
			if (!SamePinAnchor(next.anchors[next.cascade ? 1 : 0], g_pinSettings.anchors[next.cascade ? 1 : 0]) ||
				next.cascade != g_pinSettings.cascade || next.edge != g_pinSettings.edge)
				g_pinReanchor = true;

			if (next.edge != g_pinSettings.edge)
			{
				if (!g_pinSwitchPending)
					g_pinSwitchFrom = g_pinSettings.edge;
				g_pinSwitchPending = true;
			}

			if (next.free != g_pinSettings.free)
				g_pinModeChanged = true;

			// Layout changes move the pins on their own: they glide. (A drop saved
			// and read back rounded is not one.)
			const int arrangement = next.cascade ? 1 : 0;
			if (next.cascade != g_pinSettings.cascade || next.edge != g_pinSettings.edge || next.gap != g_pinSettings.gap ||
				next.center != g_pinSettings.center || next.anchors[arrangement].set != g_pinSettings.anchors[arrangement].set)
				g_pinGlideWindow = kPinGlideWindow;

			// Ungrouped, turning centering on is a layout change too: the pins
			// nobody placed by hand are placed again.
			if (next.free && next.center != g_pinSettings.center && !g_pinHandPlaced)
				g_pinRespawn = true;

			bool anySet = false, wasSet = false, changed = false;
			for (int k = 0; k < kMaxPins; ++k)
			{
				anySet = anySet || next.freeAnchors[k].set;
				wasSet = wasSet || g_pinSettings.freeAnchors[k].set;
				changed = changed || !SamePinAnchor(next.freeAnchors[k], g_pinSettings.freeAnchors[k]);
			}
			if (changed && !anySet && wasSet)
			{
				g_pinRespawn = true; // cleared
				g_pinHandPlaced = false;
				g_pinGlideWindow = kPinGlideWindow;
			}
			else if (changed)
				g_pinApplyFree = true; // edited by hand

			g_pinSettings = next;
		}

		// Saves the ungrouped spots. The in-memory copy is read back from the
		// text that was written, so the config's own change notice finds it equal.
		void CommitPinFreeAnchors()
		{
			char text[kPinFreeOffsetsBufferSize];
			FormatPinFreeAnchors(g_pinSettings.freeAnchors, text, sizeof(text));
			ParsePinFreeAnchors(text, g_pinSettings.freeAnchors);
			CodexConfig::Config::SetPinFreeOffsets(text);
		}

		// Reads every pin option from the config, at startup.
		void LoadPinSettings()
		{
			PinSettings next;
			next.cascade = _stricmp(CodexConfig::Config::GetPinArrangement(), "Cascade") == 0;
			next.edge = ParsePinEdge(CodexConfig::Config::GetPinLayout());
			next.gap = CodexConfig::Config::GetPinGap();
			next.center = CodexConfig::Config::GetPinCentering();
			next.anchors[0] = ReadPinAnchor(false);
			next.anchors[1] = ReadPinAnchor(true);
			next.free = _stricmp(CodexConfig::Config::GetPinGrouping(), "Ungrouped") == 0;
			char text[kPinFreeOffsetsBufferSize];
			CodexConfig::Config::GetPinFreeOffsets(text, sizeof(text));
			ParsePinFreeAnchors(text, next.freeAnchors);
			// Spots saved by an earlier session may have been placed by hand.
			for (const PinAnchor& anchor : next.freeAnchors)
				g_pinHandPlaced = g_pinHandPlaced || anchor.set;
			ApplyPinSettings(next);
			// Starting up is not a layout change: nothing is switched or eased.
			g_pinSwitchPending = false;
			g_pinGlideWindow = 0.0f;
		}

		// Live edits arrive here with the new value; the config file itself
		// is only written afterwards.
		void OnConfigChanged(const char* section, const char* key, const char* newValue)
		{
			if (!section || !key || !newValue || strcmp(section, "Menu") != 0)
				return;

			// A cleared or hand-edited search box spot: re-place an open box.
			const bool searchX = strcmp(key, "SearchBoxX") == 0;
			if (searchX || strcmp(key, "SearchBoxY") == 0)
			{
				(searchX ? g_searchSavedX : g_searchSavedY) = static_cast<float>(atof(newValue));
				if (g_searchVisible)
				{
					g_searchPlaceFrames = 1;
					g_searchPlaced = false;
				}
				return;
			}

			// Clear on the config page empties these too, but it only resets
			// positions: the saved pins stay, so put their copy back. The
			// loader writes the table's value to disk after this returns.
			if (strncmp(key, "PinnedRecipe", 12) == 0)
			{
				const int slot = key[12] - '1';
				if (slot >= 0 && slot < kMaxPins && key[13] == '\0' && newValue[0] == '\0' &&
					slot < static_cast<int>(g_savedPins.size()) && !g_savedPins[slot].empty())
					CodexConfig::Config::SetPinnedRecipe(slot, g_savedPins[slot].c_str());
				return;
			}

			PinSettings next = g_pinSettings;
			if (strcmp(key, "PinArrangement") == 0)
				next.cascade = _stricmp(newValue, "Cascade") == 0;
			else if (strcmp(key, "PinLayout") == 0)
				next.edge = ParsePinEdge(newValue);
			else if (strcmp(key, "PinSpacing") == 0)
				next.gap = CodexPinLayout::GapFromText(newValue);
			else if (strcmp(key, "PinCentering") == 0)
				next.center = _stricmp(newValue, "true") == 0 || strcmp(newValue, "1") == 0;
			else if (strcmp(key, "PinTileOffset") == 0)
				next.anchors[0] = ParsePinAnchor(newValue);
			else if (strcmp(key, "PinCascadeOffset") == 0)
				next.anchors[1] = ParsePinAnchor(newValue);
			else if (strcmp(key, "PinGrouping") == 0)
				next.free = _stricmp(newValue, "Ungrouped") == 0;
			else if (strcmp(key, "PinFreeOffsets") == 0)
				ParsePinFreeAnchors(newValue, next.freeAnchors);
			else
				return;

			ApplyPinSettings(next);
		}

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

		// True when a pinned recipe makes this item, whichever tier variant of it.
		bool IsProductPinned(const std::string& uniqueItemName)
		{
			for (const PinSlot& slot : g_pins)
			{
				CodexRecipes::RecipeInfo pinned;
				if (slot.nativeRecipe && CodexRecipes::FindByNativeRecipe(slot.nativeRecipe, pinned) &&
					pinned.output.uniqueItemName == uniqueItemName)
					return true;
			}
			return false;
		}

		// Pins the recipe that makes the item, unless nothing makes it (a raw
		// resource) or a pin already does.
		void PinItemProducer(IModLoaderImGui* imgui, const std::string& uniqueItemName)
		{
			CodexRecipes::RecipeInfo producer;
			if (IsProductPinned(uniqueItemName) || !CodexRecipes::FindProducerOfItem(uniqueItemName, producer))
				return;
			PinRecipe(imgui, producer.nativeRecipe);
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

		bool AnyPinned()
		{
			for (const PinSlot& slot : g_pins)
			{
				if (slot.nativeRecipe)
					return true;
			}
			return false;
		}

		// The pins fill the slots from the first, so slot 0 is the lead and the
		// slots render (and so stack) in the order they were pinned.
		int PinCount()
		{
			int count = 0;
			while (count < kMaxPins && g_pins[count].nativeRecipe)
				++count;
			return count;
		}

		// Saves which recipes are pinned, in pin order. Called when the set
		// changes (a pin, an unpin), never per frame. Clear on the config page does
		// not change it (see OnConfigChanged).
		void CommitPinnedRecipes()
		{
			std::vector<std::string> keys;
			const int count = PinCount();
			for (int k = 0; k < count; ++k)
				keys.push_back(g_pins[k].recipeKey);
			g_savedPins = CodexPinSet::ToSlots(keys, kMaxPins);
			for (int k = 0; k < kMaxPins; ++k)
				CodexConfig::Config::SetPinnedRecipe(k, g_savedPins[k].c_str());
		}

		// Reads the saved pins into g_savedPins. A start that finds them in the old
		// single value instead copies them to the slots once and empties the
		// old value.
		void LoadSavedPins()
		{
			std::vector<std::string> slots(kMaxPins);
			for (int k = 0; k < kMaxPins; ++k)
			{
				char key[kPinKeyBufferSize];
				CodexConfig::Config::GetPinnedRecipe(k, key, sizeof(key));
				slots[k] = key;
			}

			char legacy[kLegacyPinnedRecipesBufferSize];
			CodexConfig::Config::GetLegacyPinnedRecipes(legacy, sizeof(legacy));

			bool migrated = false;
			g_savedPins = CodexPinSet::Migrate(slots, legacy, kMaxPins, migrated);
			if (!migrated)
			{
				// Slots that hold pins win; a leftover old value is stale.
				if (legacy[0] != 0)
					CodexConfig::Config::ClearLegacyPinnedRecipes();
				return;
			}

			for (int k = 0; k < kMaxPins; ++k)
				CodexConfig::Config::SetPinnedRecipe(k, g_savedPins[k].c_str());
			CodexConfig::Config::ClearLegacyPinnedRecipes();
			LOG_INFO("CodexUI: moved %d saved pin(s) to the per-pin settings.",
				static_cast<int>(CodexPinSet::FromSlots(g_savedPins).size()));
		}

		CodexPinLayout::Params PinLayoutParams(IModLoaderImGui* imgui)
		{
			CodexPinLayout::Params params;
			imgui->GetDisplaySize(&g_displayW, &g_displayH);
			params.screenW = g_displayW;
			params.screenH = g_displayH;
			params.step = imgui->GetFrameHeightWithSpacing();
			params.frame = imgui->GetFrameHeight();
			params.edge = g_pinSettings.edge;
			params.cascade = g_pinSettings.cascade;
			params.gap = static_cast<float>(g_pinSettings.gap);
			params.center = g_pinSettings.center;
			return params;
		}

		// Asks for a pin's window to be put at `at`; applied by its next Begin.
		void RequestPinPos(PinSlot& slot, CodexPinLayout::Point at)
		{
			slot.hints.pivot_x = 0.0f;
			slot.hints.pivot_y = 0.0f;
			slot.hints.pos_cond = 0;
			slot.hints.pos_x = at.x;
			slot.hints.pos_y = at.y;
		}

		// Where a pin is, or is heading for while it glides.
		CodexPinLayout::Point PinSpot(const PinSlot& slot)
		{
			return slot.glide.active ? slot.glide.to : CodexPinLayout::Point{ slot.rect.x, slot.rect.y };
		}

		// Starts the short window in which pins whose spot changes glide to it.
		void ArmPinGlide()
		{
			g_pinGlideWindow = kPinGlideWindow;
		}

		// A drag is a move by hand: nothing eases, everything follows at once.
		// Pins still on their way are put at their targets.
		void CancelPinGlides()
		{
			g_pinGlideWindow = 0.0f;
			for (PinSlot& slot : g_pins)
			{
				if (!slot.glide.active)
					continue;
				RequestPinPos(slot, slot.glide.to);
				slot.glide.active = false;
			}
		}

		// Puts a pin the layout moves at `at`: straight away, or, for a short while
		// after a move was set off (ArmPinGlide), by easing there from where it is.
		// A glide already under way is only given its new target. A new pin takes
		// its first spot at once, and a pin being dragged never eases. `pending`
		// asks for the position whatever the window measured (an anchor or a drag
		// in progress, the followers every frame).
		void SetPinTarget(PinSlot& slot, CodexPinLayout::Point at, bool pending)
		{
			if (slot.glide.active)
			{
				slot.glide.to = at;
				return;
			}

			const CodexPinLayout::Point shown{ slot.rect.x, slot.rect.y };
			const bool moves = slot.rect.valid &&
				(std::fabs(at.x - shown.x) > kPinDragThreshold || std::fabs(at.y - shown.y) > kPinDragThreshold);
			if (moves && g_pinGlideWindow > 0.0f && !slot.fresh && !slot.dragging)
			{
				slot.glide = { true, shown, at, 0.0f };
				return;
			}

			slot.fresh = false;
			if (CodexPinLayout::LeadNeedsRequest(pending, at, shown, kPinDragThreshold))
				RequestPinPos(slot, at);
		}

		// Once a frame, from the first pin's render: the time since the last frame
		// and the end of the gliding window.
		void AdvancePinClock(IModLoaderImGui* imgui)
		{
			const double now = imgui->GetTime();
			g_pinDeltaTime = static_cast<float>(std::clamp(now - g_pinLastTime, 0.0, 0.1));
			g_pinLastTime = now;
			g_pinGlideWindow = (std::max)(0.0f, g_pinGlideWindow - g_pinDeltaTime);
		}

		// Advances every glide and asks for the window at its eased spot, once
		// the layout has set this frame's targets. Only the target is ever saved.
		void StepPinGlides()
		{
			for (PinSlot& slot : g_pins)
			{
				PinGlide& glide = slot.glide;
				if (!glide.active)
					continue;

				glide.elapsed += g_pinDeltaTime;
				// The first pin's request is applied by the next frame's Begin and the
				// others' by this frame's, so the first looks one frame ahead and they
				// arrive together.
				const float t = glide.elapsed + (&slot == &g_pins[0] ? g_pinDeltaTime : 0.0f);
				const bool done = t >= CodexPinLayout::kGlideSeconds;
				const CodexPinLayout::Point at = done ? glide.to : CodexPinLayout::GlideAt(glide.from, glide.to, t);
				RequestPinPos(slot, { std::round(at.x), std::round(at.y) });
				if (glide.elapsed >= CodexPinLayout::kGlideSeconds)
					glide.active = false;
			}
		}

		// Puts the lead pin at its anchor: where the player last dropped it
		// for the current arrangement, or else one step in from the configured
		// screen edge. Only requests the position - the slot's render applies
		// it once, after which the player's drag sticks.
		void AnchorLead(IModLoaderImGui* imgui, PinSlot& slot)
		{
			const CodexPinLayout::Params params = PinLayoutParams(imgui);
			g_pinAnchoredW = g_displayW;
			g_pinAnchoredH = g_displayH;
			g_pinAnchoredLeadW = slot.rect.w;
			g_pinReanchor = false;

			const CodexPinLayout::Size size{ slot.rect.w, slot.rect.h };
			const PinAnchor& anchor = g_pinSettings.anchors[g_pinSettings.cascade ? 1 : 0];
			const CodexPinLayout::Point at = anchor.set
				? CodexPinLayout::LeadFromFraction(params, size, anchor.x, anchor.y)
				: CodexPinLayout::DefaultLead(params, size);

			SetPinTarget(slot, at, true);
		}

		// Called from the lead's render, so the followers' hints are in place
		// before they begin in the same frame. Works out where every pin
		// belongs from the lead's position and each window's measured size
		// (CodexPinLayout::Arrange), keeps the lead on screen, and asks for the
		// followers' positions. Waits a frame for a new pin's size.
		void PlacePins(IModLoaderImGui* imgui, PinSlot& lead)
		{
			const int count = PinCount();
			CodexPinLayout::Size sizes[kMaxPins];
			for (int k = 0; k < count; ++k)
			{
				if (!g_pins[k].rect.valid)
					return;
				sizes[k] = { g_pins[k].rect.w, g_pins[k].rect.h };
			}

			// A requested move that has not been applied yet counts as the lead's
			// spot, and so does the spot a gliding lead is heading for. With
			// centering on, Arrange keeps the lead's spot across the edge (the
			// drag) and puts it where the group is centered along the edge.
			const bool moving = !lead.glide.active && lead.hints.pos_x >= 0.0f && lead.hints.pos_y >= 0.0f;
			CodexPinLayout::Point at[kMaxPins];
			CodexPinLayout::Arrange(PinLayoutParams(imgui), sizes, count,
				moving ? CodexPinLayout::Point{ lead.hints.pos_x, lead.hints.pos_y } : PinSpot(lead), at);

			SetPinTarget(lead, at[0], moving);
			for (int k = 1; k < count; ++k)
				SetPinTarget(g_pins[k], at[k], true);
		}

		// Ungrouped: remembers where pin `index` sits, as a screen fraction.
		void SetPinFreeAnchor(const CodexPinLayout::Params& params, int index, CodexPinLayout::Point at)
		{
			const CodexPinLayout::Point fraction = CodexPinLayout::ToFraction(params, at);
			g_pinSettings.freeAnchors[index] = { fraction.x, fraction.y, true };
		}

		// The PinLayout was changed: moves the pins to the closest equivalent of
		// where they were (CodexPinLayout::SwitchLead and SwitchAll). Grouped, that
		// is the lead's anchor, which the lead is then put back at; ungrouped,
		// every pin's own spot. The other arrangement's saved anchor is mirrored
		// along, or forgotten when it has no equivalent.
		void ApplyEdgeSwitch(const CodexPinLayout::Params& to)
		{
			if (!g_pinSwitchPending)
				return;

			const int count = PinCount();
			CodexPinLayout::Size sizes[kMaxPins];
			for (int k = 0; k < count; ++k)
			{
				if (!g_pins[k].rect.valid)
					return; // sizes are not known yet; tried again next frame
				sizes[k] = { g_pins[k].rect.w, g_pins[k].rect.h };
			}
			g_pinSwitchPending = false;
			if (count <= 0)
				return;

			CodexPinLayout::Params from = to;
			from.edge = g_pinSwitchFrom;
			const bool mirror = from.edge != PinEdge::Top && to.edge != PinEdge::Top;
			if (from.edge == to.edge)
				return;

			if (g_pinSettings.free)
			{
				CodexPinLayout::Point at[kMaxPins];
				for (int k = 0; k < count; ++k)
					at[k] = PinSpot(g_pins[k]);
				CodexPinLayout::SwitchAll(from, to, sizes, count, at);
				for (int k = 0; k < count; ++k)
				{
					SetPinFreeAnchor(to, k, at[k]);
					SetPinTarget(g_pins[k], at[k], true);
					g_pins[k].applyFree = false;
				}
				CommitPinFreeAnchors();
				g_pinApplyFree = false;
				// Placed again as if just pinned: nobody's hand has placed them.
				if (!mirror)
					g_pinHandPlaced = false;
			}
			else
			{
				const bool cascade = g_pinSettings.cascade;
				const CodexPinLayout::Point lead = CodexPinLayout::SwitchLead(from, to, sizes, count, PinSpot(g_pins[0]));
				const CodexPinLayout::Point fraction = CodexPinLayout::ToFraction(to, lead);
				g_pinSettings.anchors[cascade ? 1 : 0] = { fraction.x, fraction.y, true };
				WritePinAnchor(cascade, g_pinSettings.anchors[cascade ? 1 : 0]);

				PinAnchor& other = g_pinSettings.anchors[cascade ? 0 : 1];
				if (other.set)
				{
					if (mirror)
						other.x = std::clamp(1.0f - other.x - sizes[0].w / to.screenW, 0.0f, 1.0f);
					else
						other = PinAnchor{};
					WritePinAnchor(!cascade, other);
				}
				g_pinReanchor = true;
			}
			ArmPinGlide();
		}

		// The PinLayout was changed while no pin was up (nothing to measure): the
		// saved spots are mirrored with the last known lead width, or forgotten
		// when the new edge has no equivalent. Done when the next pin is pinned.
		void SwitchSpotsWithoutPins()
		{
			g_pinSwitchPending = false;
			if (g_pinSwitchFrom == g_pinSettings.edge)
				return;

			const bool mirror = g_pinSwitchFrom != PinEdge::Top && g_pinSettings.edge != PinEdge::Top;
			if (!mirror)
				g_pinHandPlaced = false;
			const float width = g_displayW > 0.0f ? g_pinAnchoredLeadW / g_displayW : 0.0f;
			auto move = [&](PinAnchor& anchor) {
				if (!anchor.set)
					return;
				if (mirror)
					anchor.x = std::clamp(1.0f - anchor.x - width, 0.0f, 1.0f);
				else
					anchor = PinAnchor{};
			};
			for (int arrangement = 0; arrangement < 2; ++arrangement)
			{
				move(g_pinSettings.anchors[arrangement]);
				WritePinAnchor(arrangement == 1, g_pinSettings.anchors[arrangement]);
			}
			for (PinAnchor& anchor : g_pinSettings.freeAnchors)
				move(anchor);
			CommitPinFreeAnchors();
		}

		// Ungrouped, first pin's render. Acts on a changed grouping, hand-edited
		// or cleared spots and a new screen size before any pin begins, so the
		// requests are in place for all of them in the same frame.
		void UpdatePinGrouping(IModLoaderImGui* imgui)
		{
			const int count = PinCount();
			const CodexPinLayout::Params params = PinLayoutParams(imgui);

			if (g_pinModeChanged)
			{
				g_pinModeChanged = false;
				if (g_pinSettings.free)
				{
					// Grouped to ungrouped: each pin keeps the spot it has.
					for (int k = 0; k < count; ++k)
					{
						if (g_pins[k].rect.valid)
							SetPinFreeAnchor(params, k, PinSpot(g_pins[k]));
						else
							g_pins[k].placePending = g_pins[k].placeWait = true;
						g_pins[k].applyFree = false;
					}
					CommitPinFreeAnchors();
					g_pinHandPlaced = g_pinHandPlaced || g_pinSettings.anchors[g_pinSettings.cascade ? 1 : 0].set;
				}
				else if (count > 0 && g_pins[0].rect.valid)
				{
					// Ungrouped to grouped: the lead stays where it is, as the
					// arrangement's anchor, and the rest snap back under it (PlacePins).
					const bool cascade = g_pinSettings.cascade;
					const CodexPinLayout::Point fraction = CodexPinLayout::ToFraction(params, PinSpot(g_pins[0]));
					g_pinSettings.anchors[cascade ? 1 : 0] = { fraction.x, fraction.y, true };
					WritePinAnchor(cascade, g_pinSettings.anchors[cascade ? 1 : 0]);
				}
			}

			ApplyEdgeSwitch(params);

			if (!g_pinSettings.free)
			{
				// Edits to the ungrouped spots made meanwhile do not move pins that
				// are about to keep the spots they have.
				g_pinApplyFree = g_pinRespawn = false;
				return;
			}

			// The screen size is tracked here too, so a switch back to grouped
			// does not take it for a change and move the lead.
			const bool resized = g_displayW != g_pinAnchoredW || g_displayH != g_pinAnchoredH;
			g_pinAnchoredW = g_displayW;
			g_pinAnchoredH = g_displayH;
			g_pinAnchoredLeadW = g_pins[0].rect.w;
			g_pinReanchor = false;

			if (g_pinRespawn)
			{
				g_pinRespawn = false;
				CodexPinLayout::Size sizes[kMaxPins];
				CodexPinLayout::Point at[kMaxPins];
				bool known = true;
				for (int k = 0; k < count; ++k)
				{
					known = known && g_pins[k].rect.valid;
					sizes[k] = { g_pins[k].rect.w, g_pins[k].rect.h };
				}
				if (known)
				{
					CodexPinLayout::SpawnAll(params, sizes, count, at);
					for (int k = 0; k < count; ++k)
					{
						SetPinFreeAnchor(params, k, at[k]);
						SetPinTarget(g_pins[k], at[k], true);
						g_pins[k].placePending = false;
						g_pins[k].applyFree = false;
					}
					CommitPinFreeAnchors();
				}
				else
					g_pinRespawn = true;
			}
			else if (g_pinApplyFree || resized)
			{
				g_pinApplyFree = false;
				for (int k = 0; k < count; ++k)
					g_pins[k].applyFree = true;
			}
		}

		// Ungrouped, every pin's render: puts a new pin where PlaceNew says,
		// puts a pin back at its saved spot, or keeps one that was resized
		// (the Codex opening shows the buttons) on screen.
		void PlaceFreePin(IModLoaderImGui* imgui, int index, PinSlot& slot)
		{
			if (!slot.rect.valid)
				return;

			const CodexPinLayout::Params params = PinLayoutParams(imgui);
			const CodexPinLayout::Size size{ slot.rect.w, slot.rect.h };

			// A window's first render still measures an empty window (or the last
			// recipe's): its own size is there from the second.
			if (slot.placePending && slot.placeWait)
			{
				slot.placeWait = false;
				return;
			}

			if (slot.placePending)
			{
				slot.placePending = false;
				slot.applyFree = false;
				CodexPinLayout::Point at;
				const PinAnchor& saved = g_pinSettings.freeAnchors[index];
				if (slot.restoring)
				{
					// A saved pin goes back to the spot it was left at, which is
					// already saved.
					slot.restoring = false;
					if (saved.set)
					{
						slot.fresh = false;
						RequestPinPos(slot, CodexPinLayout::LeadFromFraction(params, size, saved.x, saved.y));
						return;
					}
				}
				if (index == 0)
				{
					at = saved.set ? CodexPinLayout::LeadFromFraction(params, size, saved.x, saved.y)
								   : CodexPinLayout::DefaultLead(params, size);
				}
				else
				{
					CodexPinLayout::Size sizes[kMaxPins];
					CodexPinLayout::Point spots[kMaxPins];
					for (int k = 0; k < index; ++k)
					{
						sizes[k] = { g_pins[k].rect.w, g_pins[k].rect.h };
						spots[k] = { g_pins[k].rect.x, g_pins[k].rect.y };
					}
					at = CodexPinLayout::PlaceNew(params, sizes, spots, index, size);
				}
				SetPinFreeAnchor(params, index, at);
				RequestPinPos(slot, at);
				CommitPinFreeAnchors();

				// With centering on, pins nobody placed by hand are placed again as
				// a group, this one included, so the group stays centered.
				slot.fresh = params.center && !g_pinHandPlaced;
				if (slot.fresh)
					g_pinRespawn = true;
			}
			else if (slot.applyFree)
			{
				slot.applyFree = false;
				const PinAnchor& saved = g_pinSettings.freeAnchors[index];
				if (saved.set)
					SetPinTarget(slot, CodexPinLayout::LeadFromFraction(params, size, saved.x, saved.y), true);
			}
			else if (!slot.dragging)
			{
				const CodexPinLayout::Point at = CodexPinLayout::ClampToScreen(params, size, { slot.rect.x, slot.rect.y });
				if (CodexPinLayout::LeadNeedsRequest(false, at, { slot.rect.x, slot.rect.y }, kPinDragThreshold))
					RequestPinPos(slot, at);
			}
		}

		// Puts the pins back on the configured edge: forgets where the lead
		// was dropped for the current arrangement.
		void ArrangePins(IModLoaderImGui* imgui)
		{
			ArmPinGlide();
			if (g_pinSettings.free)
			{
				g_pinRespawn = true;
				g_pinHandPlaced = false;
				return;
			}

			const bool cascade = g_pinSettings.cascade;
			g_pinSettings.anchors[cascade ? 1 : 0] = PinAnchor{};
			WritePinAnchor(cascade, g_pinSettings.anchors[cascade ? 1 : 0]);

			if (PinCount() > 0)
				AnchorLead(imgui, g_pins[0]);
		}

		// Names the window of slot `index` after the recipe in it; the part
		// after ### is the window's id, so the slot keeps its window.
		void TitleSlot(int index, const char* displayName)
		{
			snprintf(g_pins[index].title, sizeof(g_pins[index].title), "%s###CodexPin%d", displayName, index);
		}

		// Pins a recipe in the next free slot. `restoring` is a pin coming back
		// from the saved set: it is not saved again, nothing glides for it, and
		// ungrouped it returns to its saved spot.
		void PinRecipe(IModLoaderImGui* imgui, void* nativeRecipe, bool restoring)
		{
			if (!nativeRecipe || IsPinned(nativeRecipe))
				return;

			CodexRecipes::RecipeInfo info;
			if (!CodexRecipes::FindByNativeRecipe(nativeRecipe, info))
				return;

			const int index = PinCount();
			if (index >= kMaxPins || !g_pins[index].widget)
				return;

			PinSlot& slot = g_pins[index];
			slot.nativeRecipe = nativeRecipe;
			slot.recipeKey = info.key;
			slot.restoring = restoring && g_pinSettings.free;
			TitleSlot(index, info.displayName.c_str());

			// Nothing to move yet: changes made while no pin was up are already in effect.
			if (index == 0)
			{
				g_pinModeChanged = g_pinApplyFree = g_pinRespawn = false;
				if (g_pinSwitchPending)
					SwitchSpotsWithoutPins();
			}

			// The pins already up ease aside for it; it takes its spot at once.
			slot.fresh = true;
			slot.glide.active = false;
			if (!restoring)
				ArmPinGlide();

			if (g_pinSettings.free)
			{
				// Placed once its window has a size.
				slot.placePending = slot.placeWait = true;
				slot.applyFree = false;
			}
			else if (index == 0)
				AnchorLead(imgui, slot);

			if (g_self && g_self->hooks->UI)
				g_self->hooks->UI->SetWidgetVisible(slot.widget, true);

			if (!restoring)
				CommitPinnedRecipes();
		}

		// Puts the saved pins back, once the recipe list is ready. A saved recipe
		// the game no longer has is dropped with a log line, and the pins after it
		// move up a slot with their saved spots, as when one is unpinned.
		void RestorePinnedRecipes(IModLoaderImGui* imgui)
		{
			const std::vector<std::string> keys = CodexPinSet::FromSlots(g_savedPins);
			if (keys.empty() || AnyPinned())
				return;

			PinAnchor savedSpots[kMaxPins];
			for (int k = 0; k < kMaxPins; ++k)
				savedSpots[k] = g_pinSettings.freeAnchors[k];

			int restored = 0;
			for (size_t i = 0; i < keys.size(); ++i)
			{
				CodexRecipes::RecipeInfo info;
				if (!CodexRecipes::FindByKey(keys[i], info))
				{
					LOG_INFO("CodexUI: saved pin '%s' is no longer a recipe; dropped.", keys[i].c_str());
					continue;
				}

				PinRecipe(imgui, info.nativeRecipe, true);
				if (PinCount() == restored)
					continue;
				g_pinSettings.freeAnchors[restored] = savedSpots[i];
				++restored;
			}

			const int saved = static_cast<int>(keys.size());
			LOG_INFO("CodexUI: restored %d of %d saved pin(s).", restored, saved);

			// Nothing came back: the saved copies are left alone, as the recipe list
			// may be at fault rather than the saved pins. The next pin or unpin
			// saves what is up.
			if (restored > 0 && restored < saved)
			{
				// The saved copies match what is up again.
				if (g_pinSettings.free)
				{
					// Slot 0's spot stays for the next first pin, as in UnpinSlot.
					for (int k = restored > 1 ? restored : 1; k < saved; ++k)
						g_pinSettings.freeAnchors[k] = PinAnchor{};
					CommitPinFreeAnchors();
				}
				CommitPinnedRecipes();
			}
		}

		// Unpins a slot and closes the gap: each later pin moves up a slot, so
		// the slots stay in the order the recipes were pinned and the first is
		// always the lead. The next pin along becomes the lead and takes the
		// anchor.
		void UnpinSlot(int index)
		{
			const int count = PinCount();
			if (index >= count)
				return;
			for (int k = index; k + 1 < count; ++k)
			{
				CodexRecipes::RecipeInfo info;
				g_pins[k].nativeRecipe = g_pins[k + 1].nativeRecipe;
				g_pins[k].recipeKey = g_pins[k + 1].recipeKey;
				g_pins[k].rect = g_pins[k + 1].rect;
				g_pins[k].dragging = false;
				g_pins[k].placePending = g_pins[k + 1].placePending;
				g_pins[k].placeWait = g_pins[k + 1].placeWait;
				g_pins[k].fresh = g_pins[k + 1].fresh;
				g_pins[k].restoring = g_pins[k + 1].restoring;
				g_pins[k].glide.active = false;
				if (g_pinSettings.free)
				{
					// This slot's window moves to the next pin's spot.
					g_pins[k].applyFree = true;
					g_pinSettings.freeAnchors[k] = g_pinSettings.freeAnchors[k + 1];
				}
				if (CodexRecipes::FindByNativeRecipe(g_pins[k].nativeRecipe, info))
					TitleSlot(k, info.displayName.c_str());
				else
					TitleSlot(k, "Pinned Recipe");
			}

			if (index == 0)
				g_pinReanchor = true;

			PinSlot& last = g_pins[count - 1];
			last.nativeRecipe = nullptr;
			last.recipeKey.clear();
			last.restoring = false;
			last.rect = PinRect{};
			last.dragging = false;
			last.placePending = false;
			last.placeWait = false;
			last.applyFree = false;
			last.fresh = false;
			last.glide.active = false;
			ArmPinGlide();
			if (g_pinSettings.free && count > 1)
			{
				// The first pin's spot stays: the next first pin comes back to it.
				g_pinSettings.freeAnchors[count - 1] = PinAnchor{};
				CommitPinFreeAnchors();

				// With centering on, the pins left that nobody placed by hand are
				// placed again as a group.
				if (g_pinSettings.center && !g_pinHandPlaced)
					g_pinRespawn = true;
			}
			if (g_self && g_self->hooks->UI && last.widget)
				g_self->hooks->UI->SetWidgetVisible(last.widget, false);
			CommitPinnedRecipes();
		}

		// Follows a drag of a pin's window and, once, when a drag that moved it
		// ends, saves its position: as the current arrangement's anchor for the
		// lead, or as the pin's own spot when ungrouped.
		void SaveDraggedPin(IModLoaderImGui* imgui, int index, PinSlot& slot)
		{
			float posX = 0.0f, posY = 0.0f;
			imgui->GetWindowPos(&posX, &posY);

			const bool pressed = imgui->IsMouseClicked(0, false);
			if (pressed && imgui->IsWindowHovered(0) && !imgui->IsAnyItemHovered())
			{
				slot.dragging = true;
				CancelPinGlides();
				slot.dragFromX = posX;
				slot.dragFromY = posY;
				imgui->GetMousePos(&slot.grabX, &slot.grabY);
				slot.grabX -= posX;
				slot.grabY -= posY;
			}
			else if (slot.dragging && imgui->IsMouseDown(0) && !pressed)
			{
				// ImGui moves a dragged window to the mouse before Begin, so past an
				// edge it would flick off screen and back on alternate frames. Ask for
				// the clamped spot every frame instead; the request is applied in Begin.
				float mouseX = 0.0f, mouseY = 0.0f;
				imgui->GetMousePos(&mouseX, &mouseY);
				const CodexPinLayout::Point at = CodexPinLayout::ClampToScreen(PinLayoutParams(imgui),
					{ slot.rect.w, slot.rect.h }, { mouseX - slot.grabX, mouseY - slot.grabY });
				slot.hints.pivot_x = 0.0f;
				slot.hints.pivot_y = 0.0f;
				slot.hints.pos_cond = 0;
				slot.hints.pos_x = at.x;
				slot.hints.pos_y = at.y;
			}
			else if (slot.dragging)
			{
				// The button is up, or a new press began elsewhere: either way this
				// drag is over, and only a press on this window starts another.
				slot.dragging = false;
				if (std::fabs(posX - slot.dragFromX) > kPinDragThreshold || std::fabs(posY - slot.dragFromY) > kPinDragThreshold)
				{
					// Clamped so a pin dropped partly off the screen is still restored.
					const CodexPinLayout::Params params = PinLayoutParams(imgui);
					if (g_pinSettings.free)
					{
						SetPinFreeAnchor(params, index, { posX, posY });
						CommitPinFreeAnchors();
						g_pinHandPlaced = true;
					}
					else
					{
						const bool cascade = g_pinSettings.cascade;
						const CodexPinLayout::Point fraction = CodexPinLayout::ToFraction(params, { posX, posY });
						g_pinSettings.anchors[cascade ? 1 : 0] = { fraction.x, fraction.y, true };
						WritePinAnchor(cascade, g_pinSettings.anchors[cascade ? 1 : 0]);
					}
				}
			}
		}

		// The first pin's render: takes up a drag that was started on another
		// pin (g_pinGrab), as if the first pin had been pressed where the mouse
		// went down, so it follows the mouse like a drag of the first pin.
		void GrabLeadFromFollower(IModLoaderImGui* imgui, PinSlot& lead)
		{
			if (!g_pinGrab.pending)
				return;
			g_pinGrab.pending = false;
			if (!imgui->IsMouseDown(0))
				return;

			float posX = 0.0f, posY = 0.0f;
			imgui->GetWindowPos(&posX, &posY);
			CancelPinGlides();
			lead.dragging = true;
			lead.dragFromX = posX;
			lead.dragFromY = posY;
			lead.grabX = g_pinGrab.mouseX - posX;
			lead.grabY = g_pinGrab.mouseY - posY;
		}

		// ImGui's ImGuiCol_ indices for the close button's colors.
		constexpr int kColText          = 0;
		constexpr int kColButtonHovered = 22;
		constexpr int kColButtonActive  = 23;

		// The X at the right end of a pinned window's title bar, drawn and
		// hit-tested where ImGui puts a window's own close button (widget
		// windows are begun without p_open, so they have none). True when it is
		// clicked.
		bool RenderUnpinX(IModLoaderImGui* imgui)
		{
			float winX = 0.0f, winY = 0.0f, winW = 0.0f, winH = 0.0f;
			imgui->GetWindowPos(&winX, &winY);
			imgui->GetWindowSize(&winW, &winH);

			const float size = imgui->GetFontSize();
			const float pad  = (imgui->GetFrameHeight() - size) * 0.5f;
			const float minX = winX + winW - pad - size;
			const float minY = winY + pad;
			const float maxX = minX + size;
			const float maxY = minY + size;

			const bool hovered = imgui->IsWindowHovered(0) && imgui->IsMouseHoveringRect(minX, minY, maxX, maxY, false);
			const bool clicked = hovered && imgui->IsMouseClicked(0, false);

			// The window's draw list is clipped to the content area, below the
			// title bar.
			PluginDrawList dl = imgui->GetWindowDrawList();
			imgui->PushClipRect(winX, winY, winX + winW, winY + winH, false);
			const float cx = (minX + maxX) * 0.5f;
			const float cy = (minY + maxY) * 0.5f;
			if (hovered)
				imgui->DL_AddCircleFilled(dl, cx, cy, size * 0.5f + 1.0f,
					imgui->GetColorU32FromCol(clicked ? kColButtonActive : kColButtonHovered, 1.0f), 12);
			const unsigned int col = imgui->GetColorU32FromCol(kColText, 1.0f);
			const float r = size * 0.5f * 0.5f;
			imgui->DL_AddLine(dl, cx - r, cy - r, cx + r, cy + r, col, 1.0f);
			imgui->DL_AddLine(dl, cx - r, cy + r, cx + r, cy - r, col, 1.0f);
			imgui->PopClipRect();
			if (hovered)
				imgui->SetTooltip("Unpin this recipe.");
			return clicked;
		}

		// Pinned cards show their edit controls (the unpin X, Open and Planner)
		// while the Codex is open, a game screen has the mouse cursor up (the
		// Escape menu, a machine menu, the recipe station, the inventory), or a
		// loader or plugin window holds the cursor (the mod loader menu).
		bool PinsEditable(IModLoaderImGui* imgui)
		{
			if (CodexUIActive(imgui))
				return true;
			IPluginHooks* hooks = GetHooks();
			if (!hooks)
				return false;
			return (hooks->GameMenu && hooks->GameMenu->IsPauseMenuOpen())
				|| CodexTooltipPin::GameCursorUp()
				|| (hooks->UI && hooks->UI->IsOverlayOpen());
		}

		void RenderPinSlot(int index, IModLoaderImGui* imgui)
		{
			PinSlot& slot = g_pins[index];

			// A placement requested last frame has now been applied - stop
			// forcing it.
			slot.hints.pos_x = -1.0f;
			slot.hints.pos_y = -1.0f;

			float winX = 0.0f, winY = 0.0f, winW = 0.0f, winH = 0.0f;
			imgui->GetWindowPos(&winX, &winY);
			imgui->GetWindowSize(&winW, &winH);
			if (winW > 0.0f && winH > 0.0f)
				slot.rect = { winX, winY, winW, winH, true };

			imgui->GetDisplaySize(&g_displayW, &g_displayH);

			// The first pin's render sets up the frame for every pin: the clock, then the
			// grouping and spots, before any pin begins.
			if (index == 0)
			{
				AdvancePinClock(imgui);
				UpdatePinGrouping(imgui);
			}

			// Grouped: dragging any pin moves the whole group. The lead is moved by
			// the window itself; a press on another pin is handed to the lead, which
			// then follows the mouse, and that pin is placed by the layout like every
			// follower. Ungrouped: every pin is dragged on its own.
			const bool follower = !g_pinSettings.free && index != 0;
			slot.hints.extra_window_flags = follower ? kPinWindowFlags | PluginWindowFlags_NoMove : kPinWindowFlags;

			if (g_pinSettings.free)
			{
				// Every pin is moved by hand and never moves another.
				PlaceFreePin(imgui, index, slot);
				SaveDraggedPin(imgui, index, slot);
				ShowDragCursor(imgui);
			}
			else if (!follower)
			{
				slot.applyFree = false;

				// On the Right edge the lead keeps its right edge when its window
				// grows, as it does when the Codex opens and the buttons show.
				const bool edgeAnchored = g_pinSettings.edge == PinEdge::Right &&
					!g_pinSettings.anchors[g_pinSettings.cascade ? 1 : 0].set;
				GrabLeadFromFollower(imgui, slot);
				if (!g_pinSwitchPending && (g_pinReanchor || g_displayW != g_pinAnchoredW || g_displayH != g_pinAnchoredH ||
					(edgeAnchored && slot.rect.w != g_pinAnchoredLeadW)))
					AnchorLead(imgui, slot);
				SaveDraggedPin(imgui, 0, slot);
				PlacePins(imgui, slot);
				ShowDragCursor(imgui);
			}
			else
			{
				// The rest follow the lead, which has already asked for their
				// positions this frame. A press on one starts a drag of the group.
				slot.applyFree = false;
				if (imgui->IsMouseClicked(0, false) && imgui->IsWindowHovered(0) && !imgui->IsAnyItemHovered())
				{
					g_pinGrab.pending = true;
					imgui->GetMousePos(&g_pinGrab.mouseX, &g_pinGrab.mouseY);
				}
				ShowDragCursor(imgui);
			}

			// The layout has set this frame's targets: the glides move towards them.
			if (index == 0)
				StepPinGlides();

			const bool interactive = PinsEditable(imgui);

			CodexRecipes::RecipeInfo info;
			if (!slot.nativeRecipe || !CodexRecipes::FindByNativeRecipe(slot.nativeRecipe, info))
			{
				imgui->TextDisabled("Recipe unavailable.");
				if (interactive && RenderUnpinX(imgui))
					UnpinSlot(index);
				return;
			}

			IPluginImGuiTextures* textures = GetHooks() ? GetHooks()->ImGuiTextures : nullptr;
			constexpr float kIconSize = 20.0f;

			char line[256];
			snprintf(line, sizeof(line), "%s  -  %.1fs", info.buildingName.c_str(), info.buildTimeSeconds);
			imgui->TextDisabled(line);
			const bool unpinClicked = interactive && RenderUnpinX(imgui);

			for (const CodexRecipes::RecipeItemRef& input : info.inputs)
			{
				// One group, so a right-click on the icon or the text pins the ingredient's recipe.
				// (The pin's own product is already pinned, so it has no hook.)
				imgui->BeginGroup();
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
				imgui->EndGroup();
				if (interactive)
					PinOnRightClick(imgui, input.uniqueItemName);
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
				RenderLaunchPlannerButton(imgui, info);
			}

			if (unpinClicked)
				UnpinSlot(index);
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
				slot.hints.extra_window_flags = kPinWindowFlags;

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
				slot.recipeKey.clear();
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
		g_searchHints.pos_x  = -1.0f; // placed by PlaceSearchBox each time it opens
		g_searchHints.pos_y  = -1.0f;
		g_searchHints.pivot_x = 0.5f;
		g_searchHints.pivot_y = 0.0f; // Top-anchored so growth expands downward, not from center
		g_searchHints.size_cond = 0; // Always
		g_searchHints.pos_cond  = 0; // Always - for the frames it is placed on
		g_searchHints.extra_window_flags = PluginWindowFlags_NoTitleBar | PluginWindowFlags_NoResize |
			PluginWindowFlags_NoSavedSettings | PluginWindowFlags_PauseMenuMouseInputs;

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
		// only, to run the work handed to it (RenderCloserWidget) outside the
		// panel render loop.
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

		CodexConfig::Config::GetSearchBoxPos(g_searchSavedX, g_searchSavedY);
		LoadPinSettings();
		RegisterPinSlots(self);
		LoadSavedPins();
		g_restoreWaiting = true;
		self->hooks->UI->RegisterOnConfigChanged(self, &OnConfigChanged);

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

	void Tick()
	{
		if (!g_restoreWaiting || !CodexRecipes::IsReady())
			return;

		// The recipes are known, so the saved pins can be made. That takes the
		// render thread, which the closer widget gives.
		g_restoreWaiting = false;
		g_restoreRender = true;
		if (g_self && g_self->hooks->UI && g_closerWidget)
			g_self->hooks->UI->SetWidgetVisible(g_closerWidget, true);
	}

	void PinProductOf(const std::string& uniqueItemName)
	{
		if (!g_self || !g_self->hooks->UI || !g_closerWidget || uniqueItemName.empty())
			return;

		{
			std::lock_guard<std::mutex> lock(g_gamePinMutex);
			g_gamePinItem = uniqueItemName;
		}
		g_self->hooks->UI->SetWidgetVisible(g_closerWidget, true);
	}

	void Shutdown(IPluginSelf* self)
	{
		g_restoreWaiting = false;
		g_restoreRender = false;
		g_closeRender = false;
		{
			std::lock_guard<std::mutex> lock(g_gamePinMutex);
			g_gamePinItem.clear();
		}

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

		self->hooks->UI->UnregisterOnConfigChanged(self, &OnConfigChanged);
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
