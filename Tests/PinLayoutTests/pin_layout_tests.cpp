// Pin layout tests. A console program: the layout is a pure function of the
// screen size, the window sizes, the lead position, the arrangement and the
// gap (Codex/UI/pin_layout.h), so no game and no ImGui are needed.
#include "UI/pin_layout.h"
#include "UI/pin_set.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace CodexPinLayout;

static int g_checks = 0;
static int g_failed = 0;

static void Check(bool ok, const char* what, const std::string& where)
{
	++g_checks;
	if (!ok)
	{
		++g_failed;
		printf("    FAIL %s [%s]\n", what, where.c_str());
	}
}

namespace
{
	struct Resolution
	{
		const char* name;
		float w, h;
	};

	const Resolution kResolutions[] = {
		{ "1280x720", 1280, 720 },
		{ "1920x1080", 1920, 1080 },
		{ "2560x1440", 2560, 1440 },
		{ "3840x2160", 3840, 2160 },
		{ "3440x1440", 3440, 1440 },
	};

	// Pin windows of different heights and widths at font scale 1.0.
	const Size kBasePins[kMaxPins] = {
		{ 280, 150 }, { 300, 120 }, { 260, 190 }, { 320, 110 },
		{ 290, 170 }, { 270, 135 }, { 310, 200 }, { 250, 125 },
	};

	struct Case
	{
		const Resolution* res;
		float fontScale;
		Edge edge;
		bool cascade;
		bool gap;
		int count;
		const char* leadName;
		Point lead;
	};

	Params MakeParams(const Case& c)
	{
		Params p;
		p.screenW = c.res->w;
		p.screenH = c.res->h;
		p.frame = 24.0f * c.fontScale;
		p.step = 30.0f * c.fontScale;
		p.edge = c.edge;
		p.cascade = c.cascade;
		p.gap = c.gap ? p.step - p.frame : 0.0f;
		return p;
	}

	std::string Describe(const Case& c)
	{
		char buf[200];
		snprintf(buf, sizeof(buf), "%s font %.1f %s %s gap %s %d pins lead %s",
			c.res->name, c.fontScale, c.edge == Edge::Right ? "Right" : c.edge == Edge::Left ? "Left" : "Top",
			c.cascade ? "cascade" : "tile", c.gap ? "on" : "off", c.count, c.leadName);
		return buf;
	}

	bool Overlap(Size sa, Point a, Size sb, Point b)
	{
		const float eps = 0.01f;
		return a.x + sa.w > b.x + eps && b.x + sb.w > a.x + eps && a.y + sa.h > b.y + eps && b.y + sb.h > a.y + eps;
	}

	int Overlaps(const Size* sizes, const Point* at, int count)
	{
		int n = 0;
		for (int i = 0; i < count; ++i)
			for (int j = i + 1; j < count; ++j)
				n += Overlap(sizes[i], at[i], sizes[j], at[j]) ? 1 : 0;
		return n;
	}

	// Every window inside the screen, the lead where it was asked to be (kept
	// on screen), every size and position finite.
	void CheckOnScreen(const Case& c, const Params& p, const Size* sizes, const Point* at)
	{
		const std::string where = Describe(c);
		bool inside = true;
		for (int k = 0; k < c.count; ++k)
		{
			const float eps = 0.01f;
			if (!std::isfinite(at[k].x) || !std::isfinite(at[k].y) || at[k].x < -eps || at[k].y < -eps ||
				at[k].x + sizes[k].w > p.screenW + eps || at[k].y + sizes[k].h > p.screenH + eps)
			{
				inside = false;
				printf("      pin %d at %.1f,%.1f size %.0fx%.0f\n", k, at[k].x, at[k].y, sizes[k].w, sizes[k].h);
			}
		}
		Check(inside, "every pin fully on screen", where);

		const Point want = ClampToScreen(p, sizes[0], c.lead);
		if (c.cascade)
		{
			// A cascade that does not fit below the lead lifts it, never lowers it.
			Check(at[0].x == want.x && at[0].y <= want.y, "cascade lead only moves up", where);

			// Every title bar shows whenever the stack fits on the screen at all.
			float fitH = 0.0f;
			for (int k = 0; k < c.count; ++k)
				fitH = (std::max)(fitH, sizes[k].h + p.step * static_cast<float>(k));
			if (fitH <= p.screenH)
			{
				bool shown = true;
				for (int k = 0; k + 1 < c.count; ++k)
					shown = shown && at[k + 1].y >= at[k].y + p.frame - 0.01f;
				Check(shown, "no cascade title bar is covered when the stack fits", where);
			}
		}
		else
			Check(at[0].x == want.x && at[0].y == want.y, "lead stays where it was put", where);
	}
}

namespace
{
	// The pins' extent along the edge, and whether the group is centered on it.
	bool Centered(const Params& p, const Size* sizes, const Point* at, int count, float tolerance)
	{
		float lo = 0.0f, hi = 0.0f;
		detail::SpanAlongEdge(p, sizes, at, count, lo, hi);
		const float limit = p.edge == Edge::Top ? p.screenW : p.screenH;
		return std::fabs(lo - (limit - hi)) <= tolerance;
	}

	// Whether the whole group fits in one line along the edge, so it is not wrapped.
	bool FitsInOneLine(const Params& p, const Size* sizes, int count)
	{
		const float limit = p.edge == Edge::Top ? p.screenW : p.screenH;
		float used = 2.0f * p.step;
		for (int k = 0; k < count; ++k)
			used += (p.edge == Edge::Top ? sizes[k].w : sizes[k].h) + p.gap;
		return used <= limit;
	}

	bool AllOnScreen(const Params& p, const Size* sizes, const Point* at, int count)
	{
		bool inside = true;
		for (int k = 0; k < count; ++k)
			inside = inside && std::isfinite(at[k].x) && std::isfinite(at[k].y) && at[k].x >= -0.01f && at[k].y >= -0.01f &&
				at[k].x + sizes[k].w <= p.screenW + 0.01f && at[k].y + sizes[k].h <= p.screenH + 0.01f;
		return inside;
	}

	const char* EdgeName(Edge e)
	{
		return e == Edge::Right ? "Right" : e == Edge::Left ? "Left" : "Top";
	}

	// Centering, layout switching, gliding and the spacing option.
	void RunCenteringTests()
	{
		const float kGaps[] = { 1.0f, 6.0f, 50.0f };

		// 12. Centering. The group's span along the edge is centered on the
		//     screen whenever the pins fit in one line; the lead's own position
		//     along the edge is ignored (a drag only moves it across); across the
		//     edge it is kept; adding and removing pins re-centers each time.
		printf("centering\n");
		{
			int centered = 0, ignored = 0, sequences = 0;
			for (const Resolution& res : kResolutions)
			for (float fontScale : { 1.0f, 2.0f })
			for (Edge edge : { Edge::Right, Edge::Left, Edge::Top })
			for (bool cascade : { false, true })
			for (float gap : kGaps)
			{
				Case c{ &res, fontScale, edge, cascade, true, 1, "", {} };
				Params p = MakeParams(c);
				p.gap = gap;
				p.center = true;
				Size sizes[kMaxPins];
				for (int k = 0; k < kMaxPins; ++k)
					sizes[k] = { kBasePins[k].w * fontScale, kBasePins[k].h * fontScale };

				// Pins pinned one after the other, then unpinned one by one.
				for (int step = 0; step < 2 * kMaxPins - 1; ++step)
				{
					const int count = step < kMaxPins ? step + 1 : 2 * kMaxPins - 1 - step;
					c.count = count;
					const Point lead = DefaultLead(p, sizes[0]);
					c.leadName = "default";
					c.lead = lead;
					Point at[kMaxPins];
					Arrange(p, sizes, count, lead, at);
					Check(AllOnScreen(p, sizes, at, count), "every pin fully on screen", Describe(c));

					const bool alongX = edge == Edge::Top;
					const bool fits = cascade
						? [&] { float fitH = 0.0f; for (int k = 0; k < count; ++k) fitH = (std::max)(fitH, sizes[k].h + p.step * static_cast<float>(k)); return fitH <= p.screenH; }()
						: FitsInOneLine(p, sizes, count);
					if (fits && !(cascade && alongX))
					{
						Check(Centered(p, sizes, at, count, 1.0f), "the group is centered along the edge", Describe(c));
						++centered;
					}
					else if (fits)
					{
						// A Top cascade steps sideways: its span is the lead plus the steps.
						Check(Centered(p, sizes, at, count, 1.0f), "the Top cascade is centered across the screen", Describe(c));
						++centered;
					}

					// The lead's position along the edge makes no difference; across it, the lead's is kept.
					Point other[kMaxPins];
					Arrange(p, sizes, count, { alongX ? 17.0f : lead.x, alongX ? lead.y : 23.0f }, other);
					Point again[kMaxPins];
					Arrange(p, sizes, count, { alongX ? res.w - sizes[0].w : lead.x, alongX ? lead.y : res.h - sizes[0].h }, again);
					bool same = true;
					for (int k = 0; k < count; ++k)
						same = same && other[k].x == again[k].x && other[k].y == again[k].y;
					Check(same, "centered: the lead's spot along the edge is ignored", Describe(c));
					Check(alongX ? other[0].y == at[0].y || cascade : other[0].x == at[0].x, "centered: the lead's spot across the edge is kept", Describe(c));
					++ignored;
					++sequences;
				}
			}
			printf("  %d centered groups checked, %d drags checked\n", centered, ignored);
			(void)sequences;
		}

		// With centering off the lead goes where it is put, as before.
		{
			Case c{ &kResolutions[1], 1.0f, Edge::Right, false, true, 3, "free", { 700.0f, 300.0f } };
			Params p = MakeParams(c);
			Size sizes[3] = { { 280, 150 }, { 300, 120 }, { 260, 190 } };
			Point at[3];
			Arrange(p, sizes, 3, c.lead, at);
			Check(at[0].x == 700.0f && at[0].y == 300.0f, "centering off: the lead is free on both axes", Describe(c));
		}

		// 13. Spawning pins one after the other, centered (ungrouped).
		printf("centered spawn\n");
		{
			int checked = 0;
			for (const Resolution& res : kResolutions)
			for (Edge edge : { Edge::Right, Edge::Left, Edge::Top })
			for (bool cascade : { false, true })
			for (int count = 1; count <= kMaxPins; ++count)
			{
				Case c{ &res, 1.0f, edge, cascade, true, count, "default", {} };
				Params p = MakeParams(c);
				p.gap = 6.0f;
				p.center = true;
				Size sizes[kMaxPins];
				for (int k = 0; k < count; ++k)
					sizes[k] = kBasePins[k];
				Point at[kMaxPins];
				SpawnAll(p, sizes, count, at);
				Check(AllOnScreen(p, sizes, at, count), "spawned pins are all on screen", Describe(c));

				// Centered, unless the pins did not fit in one line along the edge.
				float lo = 0.0f, hi = 0.0f;
				detail::SpanAlongEdge(p, sizes, at, count, lo, hi);
				const float limit = edge == Edge::Top ? res.w : res.h;
				if (hi - lo < limit && lo >= 0.0f && hi <= limit)
					Check(Centered(p, sizes, at, count, 1.0f), "spawned pins are centered along the edge", Describe(c));
				++checked;
			}
			printf("  %d spawns checked\n", checked);
		}

		// 14. Switching the layout. Every ordered pair of edges, grouped (the
		//     lead) and ungrouped (every pin).
		printf("layout switch\n");
		{
			int pairs = 0;
			for (const Resolution& res : kResolutions)
			for (Edge from : { Edge::Right, Edge::Left, Edge::Top })
			for (Edge to : { Edge::Right, Edge::Left, Edge::Top })
			for (bool center : { true, false })
			for (int count : { 1, 3, 6 })
			{
				Case cf{ &res, 1.0f, from, false, true, count, "default", {} };
				Case ct{ &res, 1.0f, to, false, true, count, "default", {} };
				Params pf = MakeParams(cf);
				Params pt = MakeParams(ct);
				pf.gap = pt.gap = 6.0f;
				pf.center = pt.center = center;
				Size sizes[kMaxPins];
				for (int k = 0; k < count; ++k)
					sizes[k] = kBasePins[k];
				char what[160];
				snprintf(what, sizeof(what), "%s %s -> %s centering %s %d pins", res.name, EdgeName(from), EdgeName(to), center ? "on" : "off", count);
				const std::string where = what;

				// Grouped: the lead wherever the player left it.
				const Point left{ res.w * 0.62f, res.h * 0.4f };
				const Point lead = SwitchLead(pf, pt, sizes, count, left);
				Check(lead.x >= 0.0f && lead.y >= 0.0f && lead.x + sizes[0].w <= res.w + 0.01f && lead.y + sizes[0].h <= res.h + 0.01f,
					"switch: the lead lands on screen", where);
				if (from == to)
					Check(lead.x == left.x && lead.y == left.y, "switch to the same edge changes nothing", where);
				else if (from != Edge::Top && to != Edge::Top)
				{
					Check(std::fabs(lead.y - left.y) < 0.01f, "mirror: the vertical position is kept", where);
					Check(std::fabs((res.w - lead.x - sizes[0].w) - left.x) < 0.01f, "mirror: the distance from the edge is kept", where);
					const Point back = SwitchLead(pt, pf, sizes, count, lead);
					Check(std::fabs(back.x - left.x) < 0.01f && std::fabs(back.y - left.y) < 0.01f, "mirror twice is where it started", where);
				}
				else if (to == Edge::Top)
				{
					Point at[kMaxPins];
					Params arranged = pt;
					arranged.center = false; // the anchor alone centers it
					Arrange(arranged, sizes, count, lead, at);
					Check(std::fabs(lead.y - DefaultLead(pt, sizes[0]).y) < 0.01f, "to Top: lined up along the top", where);
					if (FitsInOneLine(pt, sizes, count))
						Check(Centered(arranged, sizes, at, count, 1.0f), "to Top: centered across the screen", where);
				}
				else
				{
					Check(std::fabs(lead.x - DefaultLead(pt, sizes[0]).x) < 0.01f, "from Top: at the new edge", where);
					if (center)
					{
						Point at[kMaxPins];
						Params arranged = pt;
						arranged.center = false;
						Arrange(arranged, sizes, count, lead, at);
						if (FitsInOneLine(pt, sizes, count))
							Check(Centered(arranged, sizes, at, count, 1.0f), "from Top: centered down the screen", where);
					}
					else
						Check(std::fabs(lead.y - DefaultLead(pt, sizes[0]).y) < 0.01f, "from Top, centering off: the default spot", where);
				}

				// Ungrouped: every pin its own spot.
				Point free[kMaxPins];
				for (int k = 0; k < count; ++k)
					free[k] = ClampToScreen(pf, sizes[k], { res.w * (0.2f + 0.1f * static_cast<float>(k)), res.h * (0.1f + 0.12f * static_cast<float>(k)) });
				Point moved[kMaxPins];
				for (int k = 0; k < count; ++k)
					moved[k] = free[k];
				SwitchAll(pf, pt, sizes, count, moved);
				bool onScreen = true;
				for (int k = 0; k < count; ++k)
					onScreen = onScreen && moved[k].x >= -0.01f && moved[k].y >= -0.01f && moved[k].x + sizes[k].w <= res.w + 0.01f && moved[k].y + sizes[k].h <= res.h + 0.01f;
				Check(onScreen, "switch: every pin lands on screen", where);
				if (from == to)
				{
					bool kept = true;
					for (int k = 0; k < count; ++k)
						kept = kept && moved[k].x == free[k].x && moved[k].y == free[k].y;
					Check(kept, "switch to the same edge moves no pin", where);
				}
				else if (from != Edge::Top && to != Edge::Top)
				{
					bool mirrored = true;
					for (int k = 0; k < count; ++k)
						mirrored = mirrored && std::fabs(moved[k].y - free[k].y) < 0.01f && std::fabs((res.w - moved[k].x - sizes[k].w) - free[k].x) < 0.01f;
					Check(mirrored, "mirror: every pin keeps its height and its distance from the edge", where);
				}
				else
				{
					float lo = 0.0f, hi = 0.0f;
					Params span = pt;
					detail::SpanAlongEdge(span, sizes, moved, count, lo, hi);
					const float limit = to == Edge::Top ? res.w : res.h;
					if ((to == Edge::Top || center) && hi - lo < limit)
						Check(Centered(span, sizes, moved, count, 1.0f), "pins placed again are centered along the new edge", where);
				}
				++pairs;
			}
			printf("  %d layout switches checked\n", pairs);
		}

		// 15. Gliding. Short, ease-out, lands exactly on its target.
		printf("glide\n");
		{
			Check(EaseOut(0.0f) == 0.0f && EaseOut(1.0f) == 1.0f, "ease-out starts at 0 and ends at 1", "ease");
			Check(EaseOut(-1.0f) == 0.0f && EaseOut(2.0f) == 1.0f, "ease-out is clamped", "ease");
			bool rising = true;
			for (int i = 0; i < 100; ++i)
				rising = rising && EaseOut(static_cast<float>(i + 1) / 100.0f) >= EaseOut(static_cast<float>(i) / 100.0f);
			Check(rising, "ease-out never goes backwards", "ease");
			Check(EaseOut(0.5f) > 0.5f, "ease-out is fast first and settles at the end", "ease");
			Check(kGlideSeconds >= 0.15f && kGlideSeconds <= 0.2f, "a glide lasts 150 to 200 ms", "ease");

			const Point from{ 100.0f, 400.0f }, to{ 1500.0f, 120.0f };
			const Point start = GlideAt(from, to, 0.0f);
			const Point end = GlideAt(from, to, kGlideSeconds);
			const Point past = GlideAt(from, to, 5.0f);
			Check(start.x == from.x && start.y == from.y, "a glide starts at its source", "glide");
			Check(std::fabs(end.x - to.x) < 0.001f && std::fabs(end.y - to.y) < 0.001f, "a glide ends on its target", "glide");
			Check(std::fabs(past.x - to.x) < 0.001f && std::fabs(past.y - to.y) < 0.001f, "a glide stays on its target afterwards", "glide");
			const Point mid = GlideAt(from, to, kGlideSeconds * 0.5f);
			Check(mid.x > from.x && mid.x < to.x && mid.y < from.y && mid.y > to.y, "midway is between source and target", "glide");
		}

		// 16. The spacing option: 1 to 50.
		printf("spacing\n");
		{
			struct Case2 { const char* text; int want; };
			const Case2 cases[] = {
				{ "", kGapDefault },
				{ "1", 1 }, { "6", 6 }, { "25", 25 }, { "50", 50 }, { "0", 1 }, { "-4", 1 }, { "51", 50 },
				{ "900", 50 }, { "nonsense", kGapDefault },
			};
			for (const Case2& c2 : cases)
				Check(GapFromText(c2.text) == c2.want, "spacing text maps into 1 to 50", c2.text);
			Check(GapFromText(nullptr) == kGapDefault, "no text is the default spacing", "null");
			Check(kGapMin == 1 && kGapMax == 50, "the spacing runs from 1 to 50", "range");
		}
	}
}

int main()
{
	// 1. The grid: five resolutions, three lead spots, both arrangements, gap
	//    on and off, three edges, 1 to 8 pins, font scale 1.0 and 2.0.
	int cases = 0;
	int tileOverlapCases = 0;
	int tileOverlapFeasible = 0;
	printf("grid\n");
	for (const Resolution& res : kResolutions)
	for (float fontScale : { 1.0f, 2.0f })
	for (Edge edge : { Edge::Right, Edge::Left, Edge::Top })
	for (bool cascade : { false, true })
	for (bool gap : { true, false })
	for (int count = 1; count <= kMaxPins; ++count)
	for (int spot = 0; spot < 3; ++spot)
	{
		Size sizes[kMaxPins];
		for (int k = 0; k < count; ++k)
			sizes[k] = { kBasePins[k].w * fontScale, kBasePins[k].h * fontScale };

		Case c{ &res, fontScale, edge, cascade, gap, count, "", {} };
		const Params p = MakeParams(c);
		if (spot == 0)
		{
			c.leadName = "default";
			c.lead = DefaultLead(p, sizes[0]);
		}
		else if (spot == 1)
		{
			c.leadName = "top-left";
			c.lead = { 0.0f, 0.0f };
		}
		else
		{
			c.leadName = "bottom-right";
			c.lead = { res.w - sizes[0].w - 4.0f, res.h - sizes[0].h - 4.0f };
		}

		Point at[kMaxPins];
		Arrange(p, sizes, count, c.lead, at);
		CheckOnScreen(c, p, sizes, at);
		++cases;

		// The lead's title bar is its drag handle: where the lead starts by
		// default, no tile covers it.
		if (!cascade && spot == 0)
		{
			bool clear = true;
			for (int k = 1; k < count; ++k)
				clear = clear && !Overlap({ sizes[0].w, p.frame }, at[0], sizes[k], at[k]);
			Check(clear, "tiles leave the lead's title bar uncovered", Describe(c));
		}

		// Tiles do not overlap whenever the screen is big enough to hold them
		// side by side: here, when the pins fill under half of it.
		if (!cascade)
		{
			float area = 0.0f;
			for (int k = 0; k < count; ++k)
				area += sizes[k].w * sizes[k].h;
			if (area < 0.25f * res.w * res.h)
			{
				++tileOverlapFeasible;
				const int n = Overlaps(sizes, at, count);
				if (n != 0)
				{
					++tileOverlapCases;
					printf("      %d overlapping pairs\n", n);
				}
				Check(n == 0, "tiles do not overlap when the screen has room", Describe(c));
			}
		}
	}
	printf("  %d layouts checked, %d tile layouts with room (%d overlapped)\n", cases, tileOverlapFeasible, tileOverlapCases);

	// 2. The gap: one column of tiles under a lead at the default spot is
	//    exactly the item spacing apart with the gap on, touching with it off.
	printf("gap\n");
	for (const Resolution& res : kResolutions)
	for (float fontScale : { 1.0f, 2.0f })
	for (bool gap : { true, false })
	{
		Size sizes[3] = { { 280 * fontScale, 80 * fontScale }, { 300 * fontScale, 70 * fontScale }, { 260 * fontScale, 60 * fontScale } };
		Case c{ &res, fontScale, Edge::Right, false, gap, 3, "default", {} };
		const Params p = MakeParams(c);
		c.lead = DefaultLead(p, sizes[0]);
		Point at[3];
		Arrange(p, sizes, 3, c.lead, at);
		const float spacing = gap ? p.step - p.frame : 0.0f;
		Check(std::fabs(at[1].y - (at[0].y + sizes[0].h + spacing)) < 0.01f, "second tile sits one gap below the lead", Describe(c));
		Check(std::fabs(at[2].y - (at[1].y + sizes[1].h + spacing)) < 0.01f, "third tile sits one gap below the second", Describe(c));
		Check(std::fabs((at[1].x + sizes[1].w) - (at[0].x + sizes[0].w)) < 0.01f, "Right column lines up on the lead's right edge", Describe(c));
	}

	// 3. Cascade: one step down and one step in per pin when there is room.
	printf("cascade\n");
	for (const Resolution& res : kResolutions)
	{
		Size sizes[4] = { { 280, 150 }, { 300, 120 }, { 260, 190 }, { 320, 110 } };
		Case c{ &res, 1.0f, Edge::Right, true, true, 4, "default", {} };
		const Params p = MakeParams(c);
		c.lead = DefaultLead(p, sizes[0]);
		Point at[4];
		Arrange(p, sizes, 4, c.lead, at);
		for (int k = 1; k < 4; ++k)
		{
			Check(std::fabs(at[k].y - (at[0].y + p.step * static_cast<float>(k))) < 0.01f, "cascade steps one title bar down", Describe(c));
			Check(std::fabs(at[k].x - (at[0].x - p.step * static_cast<float>(k))) < 0.01f, "Right cascade steps one title bar in", Describe(c));
		}
	}

	// 4. Wrapping goes to the side with room. A lead in the bottom-right
	//    corner of a 1280x720 screen: the next pin starts a column to its
	//    left; a lead in the top-left corner on the Right edge: to its right.
	printf("wrap\n");
	{
		Size sizes[6] = { { 280, 150 }, { 300, 120 }, { 260, 190 }, { 320, 110 }, { 290, 170 }, { 270, 135 } };
		Case c{ &kResolutions[0], 1.0f, Edge::Right, false, true, 6, "bottom-right", {} };
		const Params p = MakeParams(c);
		Point at[6];
		Arrange(p, sizes, 6, { 1280 - 280 - 4.0f, 720 - 150 - 4.0f }, at);
		Check(at[1].x + sizes[1].w <= at[0].x + 0.01f, "wrap from the bottom-right goes left of the lead column", Describe(c));
		Check(std::fabs(at[1].y - p.step) < 0.01f, "the new column starts at the margin", Describe(c));

		c.leadName = "top-left";
		Arrange(p, sizes, 6, { 0.0f, 0.0f }, at);
		bool rightOfLead = true;
		bool wrapped = false;
		for (int k = 1; k < 6; ++k)
			if (at[k].y < at[k - 1].y)
			{
				wrapped = true;
				rightOfLead = rightOfLead && at[k].x >= at[0].x + sizes[0].w - 0.01f;
			}
		Check(wrapped, "six pins on 720p wrap", Describe(c));
		Check(rightOfLead, "wrap from the top-left goes right of the lead column", Describe(c));

		// Top edge, lead in the bottom row: new rows go up.
		Case t{ &kResolutions[0], 1.0f, Edge::Top, false, true, 6, "bottom-left", {} };
		const Params tp = MakeParams(t);
		Size wide[6] = { { 400, 120 }, { 400, 120 }, { 400, 120 }, { 400, 120 }, { 400, 120 }, { 400, 120 } };
		Arrange(tp, wide, 6, { 0.0f, 720 - 120.0f }, at);
		bool someAbove = false;
		for (int k = 1; k < 6; ++k)
			someAbove = someAbove || at[k].y < at[0].y - 0.01f;
		Check(someAbove, "Top rows wrap upwards when the lead is in the bottom row", Describe(t));
	}

	// 5. Resolution change: a spot saved as a screen fraction at 4K comes back
	//    on screen at 720p, at the same fraction when it fits.
	printf("resolution change\n");
	{
		const Size size{ 300, 200 };
		const float fx = 0.97f, fy = 0.95f; // near the bottom-right corner
		for (const Resolution& res : kResolutions)
		{
			Case c{ &res, 1.0f, Edge::Right, false, true, 1, "saved", {} };
			const Params p = MakeParams(c);
			const Point at = LeadFromFraction(p, size, fx, fy);
			Check(at.x >= 0.0f && at.y >= 0.0f && at.x + size.w <= res.w + 0.01f && at.y + size.h <= res.h + 0.01f,
				"a saved spot is re-clamped to the new screen", Describe(c));
		}
		const Params roomy = MakeParams({ &kResolutions[3], 1.0f, Edge::Right, false, true, 1, "", {} });
		const Point mid = LeadFromFraction(roomy, size, 0.5f, 0.5f);
		Check(mid.x == 0.5f * 3840 && mid.y == 0.5f * 2160, "a spot that fits keeps its fraction", "3840x2160");
		const Params none = MakeParams({ &kResolutions[0], 1.0f, Edge::Right, false, true, 1, "", {} });
		const Point d = DefaultLead(none, size);
		Check(d.x == 1280 - none.step - size.w && d.y == none.step, "the default spot is one step in from the edge", "1280x720");
	}

	// 6. A window bigger than the screen (a tiny window, a huge font) sits at 0
	//    and nothing goes negative or not-a-number.
	printf("oversize\n");
	{
		Case c{ &kResolutions[0], 2.0f, Edge::Right, false, true, 3, "default", {} };
		const Params p = MakeParams(c);
		Size sizes[3] = { { 1400, 900 }, { 1400, 900 }, { 300, 200 } };
		Point at[3];
		Arrange(p, sizes, 3, { 500, 500 }, at);
		bool fine = true;
		for (const Point& pt : at)
			fine = fine && std::isfinite(pt.x) && std::isfinite(pt.y) && pt.x >= 0.0f && pt.y >= 0.0f;
		Check(fine && at[0].x == 0.0f && at[0].y == 0.0f, "an oversize lead sits at the corner", Describe(c));
	}

	// 7. Mixed widths: a line is judged by its widest window, not its first, so
	//    a wide pin late in a line does not get pushed back over the lead.
	printf("mixed widths\n");
	{
		struct Repro { const Resolution* res; Edge edge; bool gap; Point lead; Size sizes[3]; };
		const Resolution r3440{ "3440x1440", 3440, 1440 };
		const Resolution r2560{ "2560x1440", 2560, 1440 };
		const Repro repros[] = {
			{ &r3440, Edge::Right, false, { 567, 796 }, { { 362, 446 }, { 508, 378 }, { 902, 392 } } },
			{ &r2560, Edge::Top, true, { 1039, 709 }, { { 928, 272 }, { 948, 244 }, { 376, 306 } } },
		};
		for (const Repro& r : repros)
		{
			Case c{ r.res, 2.0f, r.edge, false, r.gap, 3, "repro", r.lead };
			const Params p = MakeParams(c);
			Point at[3];
			Arrange(p, r.sizes, 3, r.lead, at);
			Check(Overlaps(r.sizes, at, 3) == 0, "mixed-width tiles do not overlap", Describe(c));
		}

		// A repeatable spread of pin sizes, lead spots, edges and gaps.
		unsigned seed = 12345u;
		auto next = [&seed](float lo, float hi) {
			seed = seed * 1664525u + 1013904223u;
			return lo + (hi - lo) * static_cast<float>((seed >> 8) & 0xFFFF) / 65535.0f;
		};
		int fuzzed = 0;
		for (const Resolution& res : kResolutions)
		for (int round = 0; round < 400; ++round)
		{
			const float fontScale = round % 2 ? 2.0f : 1.0f;
			const int count = 1 + round % kMaxPins;
			Size sizes[kMaxPins];
			float area = 0.0f;
			for (int k = 0; k < count; ++k)
			{
				sizes[k] = { next(120.0f, 950.0f), next(100.0f, 450.0f) };
				area += sizes[k].w * sizes[k].h;
			}
			Case c{ &res, fontScale, static_cast<Edge>(round % 3), round % 5 == 4, round % 2 == 0, count, "random", {} };
			const Params p = MakeParams(c);
			c.lead = { next(0.0f, res.w), next(0.0f, res.h) };
			Point at[kMaxPins];
			Arrange(p, sizes, count, c.lead, at);
			CheckOnScreen(c, p, sizes, at);
			if (!c.cascade && area < 0.2f * res.w * res.h)
				Check(Overlaps(sizes, at, count) == 0, "random tiles do not overlap when the screen has room", Describe(c));
			++fuzzed;
		}
		printf("  %d random layouts checked\n", fuzzed);
	}

	// 8. Dragging the lead with the mouse held still. Each frame the drag asks
	//    for the clamped mouse spot, Arrange may move the lead off it (a cascade
	//    lifts it when the stack does not fit below), and the window sits where
	//    the last request put it. The lead has to settle, not alternate.
	printf("drag\n");
	{
		const Point mice[] = { { 730, 715 }, { 1275, 300 }, { 1275, 715 }, { 640, 5 } };
		for (const Resolution& res : kResolutions)
		for (const Point& mouse : mice)
		for (int arrangement = 0; arrangement < 2; ++arrangement)
		for (int count = 2; count <= 6; count += 2)
		{
			Case c{ &res, 2.0f, Edge::Right, arrangement == 1, true, count, "drag", {} };
			const Params p = MakeParams(c);
			Size sizes[kMaxPins];
			for (int k = 0; k < count; ++k)
				sizes[k] = { kBasePins[k].w * 2.0f, kBasePins[k].h * 2.0f };

			Point pos = ClampToScreen(p, sizes[0], { 700, 100 }); // where the window is
			const Point wanted = ClampToScreen(p, sizes[0], mouse);
			Point history[12];
			for (int frame = 0; frame < 12; ++frame)
			{
				const Point measured = pos;
				Point hint = wanted; // what the drag asks for this frame
				Point at[kMaxPins];
				Arrange(p, sizes, count, hint, at);
				if (LeadNeedsRequest(true, at[0], measured, 0.5f))
					hint = at[0];
				pos = hint;
				history[frame] = pos;
			}
			bool steady = true;
			for (int frame = 4; frame < 12; ++frame)
				steady = steady && history[frame].x == history[11].x && history[frame].y == history[11].y;
			Check(steady, "a held-still drag leaves the lead where it settles", Describe(c));

			Point laidOut[kMaxPins];
			Arrange(p, sizes, count, wanted, laidOut);
			Check(history[11].x == laidOut[0].x && history[11].y == laidOut[0].y, "the lead settles at its laid-out spot", Describe(c));
		}

		// Nothing pending: a window that drifted is moved back, one that did not is left alone.
		Check(!LeadNeedsRequest(false, { 100, 100 }, { 100.2f, 100.2f }, 0.5f), "a lead in place is not asked to move", "no request");
		Check(LeadNeedsRequest(false, { 100, 100 }, { 100, 140 }, 0.5f), "a drifted lead is asked to move", "no request");
	}

	// 9. Ungrouped spawn (PlaceNew). A new pin goes where the arrangement would
	//    put it next to the most recently placed pin, when that is fully on
	//    screen and covers no pin; otherwise at the next free arrangement slot;
	//    otherwise at the default spot.
	printf("ungrouped spawn\n");
	{
		// Next to the last pin: exactly where the arrangement says, on every
		// resolution, edge, arrangement, gap and font scale.
		int roomy = 0;
		for (const Resolution& res : kResolutions)
		for (float fontScale : { 1.0f, 2.0f })
		for (Edge edge : { Edge::Right, Edge::Left, Edge::Top })
		for (bool cascade : { false, true })
		for (bool gap : { true, false })
		{
			Case c{ &res, fontScale, edge, cascade, gap, 2, "mid", {} };
			const Params p = MakeParams(c);
			const Size lead{ 280 * fontScale, 150 * fontScale };
			const Size fresh{ 300 * fontScale, 120 * fontScale };
			const Point leadAt{ res.w * 0.45f, res.h * 0.08f };
			const float spacing = gap ? p.step - p.frame : 0.0f;
			Point want;
			if (cascade)
				want = { leadAt.x + (edge == Edge::Right ? -p.step : p.step), leadAt.y + p.step };
			else if (edge == Edge::Top)
				want = { leadAt.x + lead.w + spacing, leadAt.y };
			else
				want = { edge == Edge::Right ? leadAt.x + lead.w - fresh.w : leadAt.x, leadAt.y + lead.h + spacing };

			const Point got = PlaceNew(p, &lead, &leadAt, 1, fresh);
			const bool fits = want.x >= 0 && want.y >= 0 && want.x + fresh.w <= res.w && want.y + fresh.h <= res.h;
			if (fits)
			{
				++roomy;
				Check(std::fabs(got.x - want.x) < 0.01f && std::fabs(got.y - want.y) < 0.01f, "a new pin goes where the arrangement puts it next to the last pin", Describe(c));
			}
			Check(got.x >= 0 && got.y >= 0 && got.x + fresh.w <= res.w + 0.01f && got.y + fresh.h <= res.h + 0.01f, "a new pin is fully on screen", Describe(c));
		}
		printf("  %d next-to-last placements checked\n", roomy);

		// The last pin is the reference, not the first: three pins, the third
		// one is the one the fourth lines up with.
		for (const Resolution& res : kResolutions)
		{
			Case c{ &res, 1.0f, Edge::Left, false, true, 3, "mid", {} };
			const Params p = MakeParams(c);
			const Size sizes[3] = { { 280, 150 }, { 300, 120 }, { 260, 190 } };
			const Point at[3] = { { 100, 100 }, { 600, 100 }, { 1000, 200 } };
			const Point got = PlaceNew(p, sizes, at, 3, { 320, 110 });
			Check(got.x == std::min(1000.0f, res.w - 320.0f) && std::fabs(got.y - (200 + 190 + (p.step - p.frame))) < 0.01f, "the most recently placed pin is the reference", Describe(c));
		}

		// A Right-edge cascade, the lead at its default spot: a new pin wider than
		// the lead steps in as far as the screen allows and every title bar shows.
		for (const Resolution& res : kResolutions)
		for (float fontScale : { 1.0f, 2.0f })
		for (bool gap : { true, false })
		{
			Case c{ &res, fontScale, Edge::Right, true, gap, 2, "default", {} };
			const Params p = MakeParams(c);
			const Size sizes[2] = { { 213 * fontScale, 139 * fontScale }, { 395 * fontScale, 150 * fontScale } };
			const Point lead = DefaultLead(p, sizes[0]);
			const Point got = PlaceNew(p, sizes, &lead, 1, sizes[1]);
			const Point def = DefaultLead(p, sizes[1]);
			Check(!(got.x == def.x && got.y == def.y), "a wider cascade pin does not fall back to the default spot", Describe(c));
			Check(got.y >= lead.y + p.frame - 0.01f && got.x >= 0.0f && got.x + sizes[1].w <= res.w + 0.01f, "a wider cascade pin leaves the lead's title bar showing", Describe(c));
		}

		// No room below the last pin: the next free slot, not on top of a pin.
		int fallbacks = 0;
		for (const Resolution& res : kResolutions)
		for (Edge edge : { Edge::Right, Edge::Left, Edge::Top })
		for (bool cascade : { false, true })
		{
			Case c{ &res, 1.0f, edge, cascade, true, 2, "bottom-right", {} };
			const Params p = MakeParams(c);
			const Size sizes[2] = { { 280, 150 }, { 300, 120 } };
			const Point at[2] = { { res.w - 280 - 4.0f, 200 }, { res.w - 300 - 4.0f, res.h - 120 - 4.0f } };
			const Size fresh{ 260, 190 };
			const Point direct = detail::Beside(p, at[1], sizes[1], fresh);
			const Point got = PlaceNew(p, sizes, at, 2, fresh);
			Check(!detail::Free(p, sizes, at, 2, direct, fresh), "the setup leaves no room next to the last pin", Describe(c));
			Check(detail::Free(p, sizes, at, 2, got, fresh), "no room next to the last pin: the fallback is on screen and covers no pin", Describe(c));
			Check(got.x != direct.x || got.y != direct.y, "no room next to the last pin: it goes elsewhere", Describe(c));
			++fallbacks;
		}
		printf("  %d no-room fallbacks checked\n", fallbacks);

		// No room anywhere: the default spot, still on screen.
		for (const Resolution& res : kResolutions)
		for (Edge edge : { Edge::Right, Edge::Left, Edge::Top })
		for (bool cascade : { false, true })
		{
			Case c{ &res, 1.0f, edge, cascade, true, 1, "full", {} };
			const Params p = MakeParams(c);
			const Size big{ res.w - 20.0f, res.h - 20.0f };
			const Point where{ 10.0f, 10.0f };
			const Size fresh{ 300, 200 };
			const Point got = PlaceNew(p, &big, &where, 1, fresh);
			const Point def = DefaultLead(p, fresh);
			// A cascade only needs the title bar clear, so it may still find a spot
			// in front of the big window; a tile has none.
			if (!cascade)
				Check(got.x == def.x && got.y == def.y, "no free spot anywhere: the default spot", Describe(c));
			Check(got.x >= 0 && got.y >= 0 && got.x + fresh.w <= res.w + 0.01f && got.y + fresh.h <= res.h + 0.01f, "the default spot is on screen", Describe(c));
		}

		// Pins dragged anywhere: the answer is a free, on-screen spot, or the default.
		unsigned seed = 777u;
		auto next = [&seed](float lo, float hi) {
			seed = seed * 1664525u + 1013904223u;
			return lo + (hi - lo) * static_cast<float>((seed >> 8) & 0xFFFF) / 65535.0f;
		};
		int fuzzed = 0, defaults = 0;
		for (const Resolution& res : kResolutions)
		for (int round = 0; round < 600; ++round)
		{
			const float fontScale = round % 2 ? 2.0f : 1.0f;
			const int count = 1 + round % (kMaxPins - 1);
			Case c{ &res, fontScale, static_cast<Edge>(round % 3), round % 5 == 4, round % 2 == 0, count, "random", {} };
			const Params p = MakeParams(c);
			Size sizes[kMaxPins];
			Point at[kMaxPins];
			for (int k = 0; k < count; ++k)
			{
				sizes[k] = { next(120.0f, 500.0f), next(100.0f, 300.0f) };
				at[k] = ClampToScreen(p, sizes[k], { next(0.0f, res.w), next(0.0f, res.h) });
			}
			const Size fresh{ next(120.0f, 500.0f), next(100.0f, 300.0f) };
			const Point got = PlaceNew(p, sizes, at, count, fresh);
			const Point def = DefaultLead(p, fresh);
			const bool isDefault = got.x == def.x && got.y == def.y;
			defaults += isDefault ? 1 : 0;
			Check(isDefault || detail::Free(p, sizes, at, count, got, fresh), "a new pin lands on a free spot or the default", Describe(c));
			Check(got.x >= -0.01f && got.y >= -0.01f && got.x + fresh.w <= res.w + 0.01f && got.y + fresh.h <= res.h + 0.01f, "a new pin lands on screen", Describe(c));
			++fuzzed;
		}
		printf("  %d random spawns checked (%d fell back to the default spot)\n", fuzzed, defaults);

		// Pinning one after the other with nobody dragged: every pin on screen,
		// no tile covering another when the screen has room.
		int sequences = 0;
		for (const Resolution& res : kResolutions)
		for (float fontScale : { 1.0f, 2.0f })
		for (Edge edge : { Edge::Right, Edge::Left, Edge::Top })
		for (bool cascade : { false, true })
		for (bool gap : { true, false })
		for (int count = 1; count <= kMaxPins; ++count)
		{
			Case c{ &res, fontScale, edge, cascade, gap, count, "default", {} };
			const Params p = MakeParams(c);
			Size sizes[kMaxPins];
			float area = 0.0f;
			for (int k = 0; k < count; ++k)
			{
				sizes[k] = { kBasePins[k].w * fontScale, kBasePins[k].h * fontScale };
				area += sizes[k].w * sizes[k].h;
			}
			Point at[kMaxPins];
			SpawnAll(p, sizes, count, at);
			bool inside = true;
			for (int k = 0; k < count; ++k)
				inside = inside && at[k].x >= -0.01f && at[k].y >= -0.01f && at[k].x + sizes[k].w <= res.w + 0.01f && at[k].y + sizes[k].h <= res.h + 0.01f;
			Check(inside, "pins pinned one after the other are all on screen", Describe(c));
			if (!cascade && area < 0.2f * res.w * res.h)
				Check(Overlaps(sizes, at, count) == 0, "pins pinned one after the other do not overlap when the screen has room", Describe(c));
			++sequences;
		}
		printf("  %d pin-after-pin sequences checked\n", sequences);
	}

	// 10. Ungrouped positions are saved as screen fractions and re-clamped when
	//     the screen size changes, as a grouped lead's are.
	printf("ungrouped saved spots\n");
	{
		unsigned seed = 4242u;
		auto next = [&seed](float lo, float hi) {
			seed = seed * 1664525u + 1013904223u;
			return lo + (hi - lo) * static_cast<float>((seed >> 8) & 0xFFFF) / 65535.0f;
		};
		int checked = 0;
		for (const Resolution& from : kResolutions)
		for (const Resolution& to : kResolutions)
		for (int round = 0; round < 50; ++round)
		{
			Case c{ &from, 1.0f, Edge::Right, false, true, 1, "saved", {} };
			const Params saved = MakeParams(c);
			Case d{ &to, 1.0f, Edge::Right, false, true, 1, "restored", {} };
			const Params now = MakeParams(d);
			const Size size{ next(150.0f, 600.0f), next(100.0f, 400.0f) };
			const Point at = ClampToScreen(saved, size, { next(0.0f, from.w), next(0.0f, from.h) });

			const Point fraction = ToFraction(saved, at);
			Check(fraction.x >= 0.0f && fraction.x <= 1.0f && fraction.y >= 0.0f && fraction.y <= 1.0f, "a saved fraction is inside the screen", Describe(c));

			// Written with four decimals, as the config text is.
			char text[64];
			snprintf(text, sizeof(text), "%.4f,%.4f", fraction.x, fraction.y);
			float fx = 0.0f, fy = 0.0f;
			sscanf(text, "%f,%f", &fx, &fy);

			const Point back = LeadFromFraction(now, size, fx, fy);
			Check(back.x >= 0.0f && back.y >= 0.0f && back.x + size.w <= to.w + 0.01f && back.y + size.h <= to.h + 0.01f, "a saved ungrouped spot is re-clamped to the new screen", Describe(d));
			if (&from == &to)
				Check(std::fabs(back.x - at.x) <= 1.0f && std::fabs(back.y - at.y) <= 1.0f, "a saved spot comes back at the same size", Describe(c));
			++checked;
		}
		printf("  %d saved spots checked\n", checked);
	}

	// 11. Switching groupings. Grouped to ungrouped: each pin keeps the spot it
	//     has. Ungrouped to grouped: the lead stays and the rest snap back under
	//     it.
	printf("switching\n");
	{
		int switched = 0;
		for (const Resolution& res : kResolutions)
		for (float fontScale : { 1.0f, 2.0f })
		for (Edge edge : { Edge::Right, Edge::Left, Edge::Top })
		for (bool cascade : { false, true })
		for (int count = 2; count <= kMaxPins; count += 2)
		{
			Case c{ &res, fontScale, edge, cascade, true, count, "default", {} };
			const Params p = MakeParams(c);
			Size sizes[kMaxPins];
			for (int k = 0; k < count; ++k)
				sizes[k] = { kBasePins[k].w * fontScale, kBasePins[k].h * fontScale };

			// Grouped layout, then switched to ungrouped: every spot is saved as it is.
			Point grouped[kMaxPins];
			Arrange(p, sizes, count, DefaultLead(p, sizes[0]), grouped);
			bool kept = true;
			for (int k = 0; k < count; ++k)
			{
				const Point f = ToFraction(p, grouped[k]);
				char text[64];
				snprintf(text, sizeof(text), "%.4f,%.4f", f.x, f.y);
				float fx = 0.0f, fy = 0.0f;
				sscanf(text, "%f,%f", &fx, &fy);
				const Point back = LeadFromFraction(p, sizes[k], fx, fy);
				kept = kept && std::fabs(back.x - grouped[k].x) < 0.5f && std::fabs(back.y - grouped[k].y) < 0.5f;
			}
			Check(kept, "grouped to ungrouped: each pin keeps its spot", Describe(c));

			// Pins then dragged about, switched back: the lead stays, the rest sit under it.
			Point free[kMaxPins];
			for (int k = 0; k < count; ++k)
				free[k] = ClampToScreen(p, sizes[k], { res.w * (0.1f + 0.1f * static_cast<float>(k)), res.h * (0.7f - 0.07f * static_cast<float>(k)) });
			Point snapped[kMaxPins];
			Arrange(p, sizes, count, free[0], snapped);
			const Point lead = ClampToScreen(p, sizes[0], free[0]);
			Check(snapped[0].x == lead.x && (cascade ? snapped[0].y <= lead.y : snapped[0].y == lead.y),
				"ungrouped to grouped: the lead stays", Describe(c));
			CheckOnScreen({ c.res, c.fontScale, c.edge, c.cascade, c.gap, c.count, c.leadName, free[0] }, p, sizes, snapped);
			// Snapped pins sit exactly where the arrangement puts them for that lead.
			Point again[kMaxPins];
			Arrange(p, sizes, count, snapped[0], again);
			bool same = true;
			for (int k = 0; k < count; ++k)
				same = same && again[k].x == snapped[k].x && again[k].y == snapped[k].y;
			Check(same, "ungrouped to grouped: the pins are placed by the arrangement under the lead", Describe(c));
			++switched;
		}
		printf("  %d switches checked\n", switched);
	}

	// 12. Grouped, dragging any pin moves the whole group by the drag: the lead
	//     moves by the same amount and the layout is kept. With centering on the
	//     group moves only across the edge.
	printf("group drag\n");
	{
		int dragged = 0;
		const Edge edges[] = { Edge::Right, Edge::Left, Edge::Top };
		for (Edge edge : edges)
			for (int cascade = 0; cascade < 2; ++cascade)
				for (int center = 0; center < 2; ++center)
					for (int count = 1; count <= 4; ++count)
					{
						Params p;
						p.screenW = 1920.0f;
						p.screenH = 1080.0f;
						p.frame = 24.0f;
						p.step = 30.0f;
						p.edge = edge;
						p.cascade = cascade != 0;
						p.gap = 6.0f;
						p.center = center != 0;
						Size sizes[kMaxPins];
						for (int k = 0; k < count; ++k)
							sizes[k] = { 260.0f + 10.0f * k, 120.0f + 15.0f * k };

						const Point start = DefaultLead(p, sizes[0]);
						Point before[kMaxPins];
						Arrange(p, sizes, count, start, before);

						const Point drag = { edge == Edge::Right ? -43.0f : 43.0f, 31.0f };
						Point after[kMaxPins];
						Arrange(p, sizes, count, { before[0].x + drag.x, before[0].y + drag.y }, after);

						const bool alongX = edge == Edge::Top;
						const Point moved = p.center ? Point{ alongX ? 0.0f : drag.x, alongX ? drag.y : 0.0f } : drag;
						char what[96];
						snprintf(what, sizeof(what), "edge %d %s %s %d pins", static_cast<int>(edge), cascade ? "cascade" : "tile", center ? "centered" : "free", count);
						bool kept = true;
						for (int k = 0; k < count; ++k)
							kept = kept && std::fabs(after[k].x - before[k].x - moved.x) < 1.01f && std::fabs(after[k].y - before[k].y - moved.y) < 1.01f;
						Check(kept, "every pin moves by the drag and the layout is kept", what);
						++dragged;
					}
		printf("  %d drags checked\n", dragged);
	}
	RunCenteringTests();

	// 13. The saved pin set: keys joined with ";" in pin order, read back the
	//     same, and read safely from a hand-edited or damaged value.
	printf("saved pin set\n");
	{
		using Keys = std::vector<std::string>;
		const Keys three = { "CR_IronPlate", "CR_CopperWire", "CR_Aerogel" };

		// Encode: one key per slot, in pin order, the rest empty.
		const Keys slots = CodexPinSet::ToSlots(three, kMaxPins);
		Check(static_cast<int>(slots.size()) == kMaxPins, "there is always one value per slot", "slots");
		Check(slots[0] == "CR_IronPlate" && slots[1] == "CR_CopperWire" && slots[2] == "CR_Aerogel", "keys fill the slots in pin order", "three");
		bool restEmpty = true;
		for (int k = 3; k < kMaxPins; ++k)
			restEmpty = restEmpty && slots[k].empty();
		Check(restEmpty, "the slots after the last pin are empty", "three");
		Check(CodexPinSet::FromSlots(slots) == three, "saved pins read back in the same order", "three");

		Check(CodexPinSet::FromSlots(CodexPinSet::ToSlots({}, kMaxPins)).empty(), "no pins save as empty slots", "empty");
		Check(CodexPinSet::FromSlots(Keys(kMaxPins)).empty(), "empty slots read as no pins", "empty");

		// Every count from one pin up to the limit survives a round trip, in order.
		for (int count = 1; count <= kMaxPins; ++count)
		{
			Keys keys;
			for (int k = 0; k < count; ++k)
				keys.push_back("CR_Recipe" + std::to_string(k * 7 + 1));
			char what[64];
			snprintf(what, sizeof(what), "%d pins", count);
			Check(CodexPinSet::FromSlots(CodexPinSet::ToSlots(keys, kMaxPins)) == keys, "round trip", what);
		}

		// The reason for a value per pin: eight long asset names are far past the
		// 255 characters one config value holds, and every one of them is kept.
		Keys longKeys;
		for (int k = 0; k < kMaxPins; ++k)
			longKeys.push_back("CR_" + std::string(60, static_cast<char>('A' + k)) + "_Recipe");
		size_t joined = 0;
		for (const std::string& key : longKeys)
			joined += key.size() + 1;
		Check(joined > CodexPinSet::kMaxValueLen, "the long keys would not fit one value", "long");
		const Keys longSlots = CodexPinSet::ToSlots(longKeys, kMaxPins);
		bool eachFits = true;
		for (const std::string& slot : longSlots)
			eachFits = eachFits && slot.size() <= CodexPinSet::kMaxValueLen;
		Check(eachFits && CodexPinSet::FromSlots(longSlots) == longKeys, "eight long keys all survive", "long");

		// A key that cannot be read back whole is not saved; more keys than slots are cut.
		Check(CodexPinSet::FromSlots(CodexPinSet::ToSlots({ "CR_A", "", "CR_B", "CR_C" }, kMaxPins)) == Keys({ "CR_A", "CR_B", "CR_C" }),
			"an empty key is skipped", "bad keys");
		const std::string tooLong(CodexPinSet::kMaxValueLen + 1, 'x');
		Check(CodexPinSet::FromSlots(CodexPinSet::ToSlots({ "CR_A", tooLong, "CR_B" }, kMaxPins)) == Keys({ "CR_A", "CR_B" }),
			"a key longer than a value is skipped, not cut", "too long");
		const std::string exact(CodexPinSet::kMaxValueLen, 'x');
		Check(CodexPinSet::FromSlots(CodexPinSet::ToSlots({ exact }, kMaxPins)) == Keys({ exact }), "a key that exactly fits is kept", "exact");
		Keys many;
		for (int k = 0; k < kMaxPins + 3; ++k)
			many.push_back("CR_" + std::to_string(k));
		Check(static_cast<int>(CodexPinSet::ToSlots(many, kMaxPins).size()) == kMaxPins &&
			CodexPinSet::FromSlots(CodexPinSet::ToSlots(many, kMaxPins)).size() == static_cast<size_t>(kMaxPins),
			"no more pins are saved than there are slots", "limit");

		// A hand-edited file: gaps, spaces and repeats are dropped, order kept.
		Check(CodexPinSet::FromSlots({ " CR_A ", "", "CR_B", "CR_A", "\t", "CR_C" }) == Keys({ "CR_A", "CR_B", "CR_C" }),
			"blank slots, spaces and repeats are dropped", "hand edited");

		// The old single value.
		Check(CodexPinSet::Parse("", kMaxPins).empty(), "empty text reads as no pins", "legacy");
		Check(CodexPinSet::Parse(nullptr, kMaxPins).empty(), "a missing value reads as no pins", "legacy");
		Check(CodexPinSet::Parse("CR_IronPlate;CR_CopperWire;CR_Aerogel", kMaxPins) == three, "old text reads in order", "legacy");
		Check(CodexPinSet::Parse(" CR_A ;;CR_B;CR_A; ;CR_C", kMaxPins) == Keys({ "CR_A", "CR_B", "CR_C" }),
			"blank entries, spaces and repeats are dropped", "legacy");
		Check(CodexPinSet::Parse("CR_A;CR_B;CR_C;CR_D", 2) == Keys({ "CR_A", "CR_B" }), "no more are read than there are slots", "legacy");

		// Migration: the old value moves into the slots once, only if the slots are empty.
		bool migrated = true;
		Keys moved = CodexPinSet::Migrate(Keys(kMaxPins), "CR_IronPlate;CR_CopperWire;CR_Aerogel", kMaxPins, migrated);
		Check(migrated && CodexPinSet::FromSlots(moved) == three && static_cast<int>(moved.size()) == kMaxPins,
			"the old value moves into the slots in order", "migrate");

		migrated = true;
		moved = CodexPinSet::Migrate(CodexPinSet::ToSlots({ "CR_New" }, kMaxPins), "CR_IronPlate;CR_CopperWire", kMaxPins, migrated);
		Check(!migrated && CodexPinSet::FromSlots(moved) == Keys({ "CR_New" }), "slots that hold a pin are not overwritten", "migrate");

		migrated = true;
		moved = CodexPinSet::Migrate(Keys(kMaxPins), "", kMaxPins, migrated);
		Check(!migrated && CodexPinSet::FromSlots(moved).empty(), "an empty old value migrates nothing", "migrate");

		migrated = true;
		moved = CodexPinSet::Migrate(Keys(kMaxPins), nullptr, kMaxPins, migrated);
		Check(!migrated && CodexPinSet::FromSlots(moved).empty(), "a missing old value migrates nothing", "migrate");

		// Run twice: once the old value is emptied nothing moves again, so
		// pins the player has since removed do not come back.
		migrated = false;
		moved = CodexPinSet::Migrate(Keys(kMaxPins), "CR_A;CR_B", kMaxPins, migrated);
		Check(migrated, "first start moves the old value", "twice");
		moved = CodexPinSet::Migrate(Keys(kMaxPins), "", kMaxPins, migrated);
		Check(!migrated && CodexPinSet::FromSlots(moved).empty(), "second start with the old value emptied moves nothing", "twice");

		// An old value that was cut at 255 characters ends in half a key; the rest are whole.
		std::string cut;
		for (int k = 0; k < 6; ++k)
			cut += "CR_" + std::string(35, static_cast<char>('A' + k)) + ";";
		cut += "CR_Half";
		moved = CodexPinSet::Migrate(Keys(kMaxPins), cut.c_str(), kMaxPins, migrated);
		Check(migrated && CodexPinSet::FromSlots(moved).back() == "CR_Half", "a cut old value migrates what it holds", "migrate cut");
	}

	printf("\n%d checks, %d failed\n", g_checks, g_failed);
	return g_failed == 0 ? 0 : 1;
}
