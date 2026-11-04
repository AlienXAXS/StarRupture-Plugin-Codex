#pragma once

// Where pinned recipe windows go on screen. A pure function of the screen
// size, the windows' sizes, the lead pin's position, the arrangement and the
// gap: no ImGui, no game, so it can be tested on its own.
//
// Grouped (the default): the first pin is the lead and the only one the
// player drags (or any pin, which carries the lead); the rest tile or cascade under it. Ungrouped: every pin has
// its own spot, and a new pin is placed by PlaceNew. Every window stays fully
// on screen.

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace CodexPinLayout
{
	constexpr int kMaxPins = 8;

	struct Point
	{
		float x = 0.0f;
		float y = 0.0f;
	};

	struct Size
	{
		float w = 0.0f;
		float h = 0.0f;
	};

	enum class Edge { Right, Left, Top };

	// The pin spacing option: the gap between tiled windows, in screen pixels.
	// The default is the loader theme's item spacing.
	constexpr int kGapMin = 1;
	constexpr int kGapMax = 50;
	constexpr int kGapDefault = 6;

	struct Params
	{
		float screenW = 0.0f;
		float screenH = 0.0f;
		float step = 0.0f;  // the theme's frame height with spacing: the margin, the cascade step
		float frame = 0.0f; // the theme's frame height (the cascade overlap)
		Edge  edge = Edge::Right;
		bool  cascade = false;
		float gap = 0.0f;   // tiles: the space between windows, in pixels
		bool  center = false; // keep the group centered along its edge (Arrange, SpawnAll)
	};

	// A window's top-left corner moved the least that puts all of it on
	// screen (a window larger than the screen sits at 0).
	inline Point ClampToScreen(const Params& p, Size size, Point at)
	{
		return { std::clamp(at.x, 0.0f, (std::max)(0.0f, p.screenW - size.w)),
			std::clamp(at.y, 0.0f, (std::max)(0.0f, p.screenH - size.h)) };
	}

	// Where the lead starts when the player has not moved it: one step in
	// from the configured edge, one step down.
	inline Point DefaultLead(const Params& p, Size size)
	{
		const float x = p.edge == Edge::Right ? p.screenW - p.step - size.w : p.step;
		return ClampToScreen(p, size, { x, p.step });
	}

	// The lead's saved spot: its top-left corner as fractions of the screen,
	// so it survives a resolution change, kept on screen at the new size.
	inline Point LeadFromFraction(const Params& p, Size size, float fx, float fy)
	{
		// Rounded: the window is placed on whole pixels, so a spot saved from it
		// comes back to the same pixel.
		return ClampToScreen(p, size, { std::round(fx * p.screenW), std::round(fy * p.screenH) });
	}

	// A window's top-left corner as fractions of the screen, kept inside [0, 1],
	// for saving; LeadFromFraction turns it back into a spot at any resolution.
	inline Point ToFraction(const Params& p, Point at)
	{
		return { std::clamp(at.x / p.screenW, 0.0f, 1.0f), std::clamp(at.y / p.screenH, 0.0f, 1.0f) };
	}

	namespace detail
	{
		// How far a cascade can step in one direction before some window
		// leaves [0, limit]; `at` is the lead's corner, `size(k)` the extent
		// of window k along the axis.
		inline float CascadeReach(const float* extent, int count, float at, float limit, float sign)
		{
			float reach = 1.0e30f;
			for (int k = 1; k < count; ++k)
			{
				const float room = sign > 0.0f ? limit - extent[k] - at : at;
				reach = (std::min)(reach, room / static_cast<float>(k));
			}
			return reach;
		}

		inline void Cascade(const Params& p, const Size* sizes, int count, Point& lead, Point* out)
		{
			float w[kMaxPins], h[kMaxPins];
			for (int k = 0; k < count; ++k)
			{
				w[k] = sizes[k].w;
				h[k] = sizes[k].h;
			}

			// Towards the middle of the screen for the Right edge, outwards from
			// it otherwise; the other way when that runs off the screen.
			const float prefer = p.edge == Edge::Right ? -1.0f : 1.0f;
			const float reachPreferred = CascadeReach(w, count, lead.x, p.screenW, prefer);
			const float reachOther = CascadeReach(w, count, lead.x, p.screenW, -prefer);
			const float sx = reachPreferred >= p.step || reachPreferred >= reachOther ? prefer : -prefer;
			const float stepX = std::clamp(sx == prefer ? reachPreferred : reachOther, 0.0f, p.step);

			// When the stack does not fit below the lead, the whole cascade, lead
			// included, moves up by the shortfall so every title bar stays showing.
			float fitH = 0.0f;
			for (int k = 0; k < count; ++k)
				fitH = (std::max)(fitH, h[k] + p.step * static_cast<float>(k));
			lead.y = (std::max)(0.0f, (std::min)(lead.y, p.screenH - fitH));

			// Downwards only: a later pin sits in front of the earlier ones, so
			// stepping up would hide their title bars. Closer steps when the
			// screen is short.
			const float stepY = std::clamp(CascadeReach(h, count, lead.y, p.screenH, 1.0f), 0.0f, p.step);

			for (int k = 0; k < count; ++k)
			{
				const float fk = static_cast<float>(k);
				out[k] = ClampToScreen(p, sizes[k], { lead.x + sx * stepX * fk, lead.y + stepY * fk });
			}
		}

		// Tiles run in lines. Right and Left lines are columns going down;
		// Top lines are rows going across. A window that would cross the
		// margin starts a new line at the margin, on whichever side of the
		// lines so far has room. `a` runs along a line, `b` across lines.
		inline void Tile(const Params& p, const Size* sizes, int count, Point lead, Point* out)
		{
			const bool columns = p.edge != Edge::Top;
			const float limitA = columns ? p.screenH : p.screenW;
			const float limitB = columns ? p.screenW : p.screenH;
			const float margin = p.step;
			const float gap = p.gap;

			auto extentA = [&](int k) { return columns ? sizes[k].h : sizes[k].w; };
			auto extentB = [&](int k) { return columns ? sizes[k].w : sizes[k].h; };

			// The lead column on the Right edge lines up on the lead's right
			// edge; every other first line lines up on the lead's start.
			bool alignEnd = p.edge == Edge::Right;
			float lineB = columns ? (alignEnd ? lead.x + sizes[0].w : lead.x) : lead.y;
			float cur = columns ? lead.y : lead.x;
			int inLine = 0;
			float lo = 1.0e30f, hi = -1.0e30f; // the span across all lines placed so far
			float lineLo = 1.0e30f, lineHi = -1.0e30f; // the span of the current line
			float dir = 0.0f;                  // which way new lines go: -1 before, +1 after

			for (int k = 0; k < count; ++k)
			{
				if (inLine > 0 && cur + extentA(k) > limitA - margin)
				{
					lo = (std::min)(lo, lineLo);
					hi = (std::max)(hi, lineHi);

					const float prefer = dir != 0.0f ? dir : (p.edge == Edge::Right ? -1.0f : 1.0f);
					// The whole line's width decides which side has room for it.
					float lineExtent = 0.0f, along = margin;
					for (int j = k; j < count && (j == k || along + extentA(j) <= limitA - margin); ++j)
					{
						lineExtent = (std::max)(lineExtent, extentB(j));
						along += extentA(j) + gap;
					}
					const float need = lineExtent + gap;
					const float roomPrefer = prefer < 0.0f ? lo : limitB - hi;
					const float roomOther = prefer < 0.0f ? limitB - hi : lo;
					dir = roomPrefer >= need || roomPrefer >= roomOther ? prefer : -prefer;

					alignEnd = dir < 0.0f;
					lineB = dir < 0.0f ? lo - gap : hi + gap;
					cur = margin;
					lineLo = 1.0e30f;
					lineHi = -1.0e30f;
					inLine = 0;
				}

				const float b = alignEnd ? lineB - extentB(k) : lineB;
				// Kept on screen as it is placed, so the line's span is the real one.
				Point at = ClampToScreen(p, sizes[k], columns ? Point{ b, cur } : Point{ cur, b });
				// The lead's title bar is its handle: a window that would cover it
				// starts past the lead along its line instead.
				if (k > 0 && at.x < lead.x + sizes[0].w && at.x + sizes[k].w > lead.x &&
					at.y < lead.y + p.frame && at.y + sizes[k].h > lead.y)
				{
					cur = columns ? lead.y + p.frame + gap : lead.x + sizes[0].w + gap;
					at = ClampToScreen(p, sizes[k], columns ? Point{ b, cur } : Point{ cur, b });
				}
				out[k] = at;
				cur += extentA(k) + gap;
				const float placedB = columns ? out[k].x : out[k].y;
				lineLo = (std::min)(lineLo, placedB);
				lineHi = (std::max)(lineHi, placedB + extentB(k));
				++inLine;
			}
		}
	}

	// Whether the lead's placement has to be requested this frame. A request
	// still pending (a drag or an anchor) is always answered with the laid-out
	// spot: Arrange can move the lead off the requested one (a cascade lifts
	// it), and comparing with the window's measured spot instead would send
	// the lifted spot only every other frame, so the lead would flicker. With
	// nothing pending, only a window that has drifted from its spot is moved.
	inline bool LeadNeedsRequest(bool pending, Point placed, Point measured, float threshold)
	{
		return pending || std::fabs(placed.x - measured.x) > threshold || std::fabs(placed.y - measured.y) > threshold;
	}

	namespace detail
	{
		// The arrangement for a lead that is already on screen.
		inline void ArrangeRaw(const Params& p, const Size* sizes, int count, Point lead, Point* out)
		{
			out[0] = lead;
			if (p.cascade)
				Cascade(p, sizes, count, lead, out);
			else
				Tile(p, sizes, count, lead, out);
			out[0] = lead;
		}

		// The extent of the windows along the edge: across the screen for Top,
		// down it for Left and Right.
		inline void SpanAlongEdge(const Params& p, const Size* sizes, const Point* at, int count, float& lo, float& hi)
		{
			const bool alongX = p.edge == Edge::Top;
			lo = 1.0e30f;
			hi = -1.0e30f;
			for (int k = 0; k < count; ++k)
			{
				lo = (std::min)(lo, alongX ? at[k].x : at[k].y);
				hi = (std::max)(hi, alongX ? at[k].x + sizes[k].w : at[k].y + sizes[k].h);
			}
		}
	}

	// The lead's spot with its position along the edge replaced by the one that
	// centers the whole group on the screen (the arrangement's own wrapping
	// included); its position across the edge is kept. Horizontal for Top,
	// vertical for Left and Right.
	inline Point CenteredLead(const Params& p, const Size* sizes, int count, Point lead)
	{
		if (count <= 0)
			return lead;
		if (count > kMaxPins)
			count = kMaxPins;

		const bool alongX = p.edge == Edge::Top;
		const float limit = alongX ? p.screenW : p.screenH;
		float& along = alongX ? lead.x : lead.y;

		// From the margin, so a group that fits is not wrapped before it is measured.
		along = p.step;
		lead = ClampToScreen(p, sizes[0], lead);
		Point at[kMaxPins];
		for (int pass = 0; pass < 3; ++pass)
		{
			detail::ArrangeRaw(p, sizes, count, lead, at);
			float lo = 0.0f, hi = 0.0f;
			detail::SpanAlongEdge(p, sizes, at, count, lo, hi);
			const float delta = (limit - (hi - lo)) * 0.5f - lo;
			if (std::fabs(delta) < 0.5f)
				break;
			along += delta;
			lead = ClampToScreen(p, sizes[0], lead);
		}
		along = std::round(along);
		return ClampToScreen(p, sizes[0], lead);
	}

	// Positions for `count` windows (at most kMaxPins) in pin order; sizes[0]
	// and lead are the lead's. The lead is kept on screen; a cascade that would
	// not fit below it lifts the lead too. With p.center the lead's position
	// along the edge is ignored: the group is centered (CenteredLead).
	inline void Arrange(const Params& p, const Size* sizes, int count, Point lead, Point* out)
	{
		if (count <= 0)
			return;
		if (count > kMaxPins)
			count = kMaxPins;

		lead = ClampToScreen(p, sizes[0], lead);
		if (p.center)
			lead = CenteredLead(p, sizes, count, lead);
		detail::ArrangeRaw(p, sizes, count, lead, out);
	}

	namespace detail
	{
		// Where the arrangement would put a window of `size` next to the pin
		// at `ref`: below it in a column (Right lines up on its right edge,
		// Left on its left), after it in a row for Top, one step down and in
		// for a cascade.
		inline Point Beside(const Params& p, Point ref, Size refSize, Size size)
		{
			if (p.cascade)
				return { ref.x + (p.edge == Edge::Right ? -p.step : p.step), ref.y + p.step };
			const float gap = p.gap;
			if (p.edge == Edge::Top)
				return { ref.x + refSize.w + gap, ref.y };
			return { p.edge == Edge::Right ? ref.x + refSize.w - size.w : ref.x, ref.y + refSize.h + gap };
		}

		// Whether a window of `size` at `cand` is fully on screen and covers no
		// pin. A cascade is meant to overlap, so there only the pins' title
		// bars count.
		inline bool Free(const Params& p, const Size* sizes, const Point* at, int count, Point cand, Size size)
		{
			constexpr float eps = 0.01f;
			if (cand.x < -eps || cand.y < -eps || cand.x + size.w > p.screenW + eps || cand.y + size.h > p.screenH + eps)
				return false;
			for (int k = 0; k < count; ++k)
			{
				const float otherH = p.cascade ? (std::min)(sizes[k].h, p.frame) : sizes[k].h;
				if (cand.x < at[k].x + sizes[k].w - eps && cand.x + size.w > at[k].x + eps &&
					cand.y < at[k].y + otherH - eps && cand.y + size.h > at[k].y + eps)
					return false;
			}
			return true;
		}
	}

	// Where a newly pinned window of `size` goes when the pins are ungrouped.
	// `sizes` and `at` are the `count` pins already up, in pin order. It goes
	// where the arrangement would put it next to the most recently placed pin
	// (the last), if that is fully on screen and covers no pin; otherwise at
	// the next free arrangement slot (next to an earlier pin, a new line at
	// the margin on whichever side has room, the cascade's other direction),
	// and last of all at the default spot.
	inline Point PlaceNew(const Params& p, const Size* sizes, const Point* at, int count, Size size)
	{
		if (count <= 0)
			return DefaultLead(p, size);

		// Each candidate is kept on screen first, as the grouped layout does, so a
		// window wider than the room beside a pin still finds its spot.
		for (int k = count - 1; k >= 0; --k)
		{
			const Point cand = ClampToScreen(p, size, detail::Beside(p, at[k], sizes[k], size));
			if (detail::Free(p, sizes, at, count, cand, size))
				return cand;
		}

		const float gap = p.gap;
		float minX = 1.0e30f, minY = 1.0e30f, maxX = -1.0e30f, maxY = -1.0e30f;
		for (int k = 0; k < count; ++k)
		{
			minX = (std::min)(minX, at[k].x);
			minY = (std::min)(minY, at[k].y);
			maxX = (std::max)(maxX, at[k].x + sizes[k].w);
			maxY = (std::max)(maxY, at[k].y + sizes[k].h);
		}

		Point more[2];
		int moreCount = 0;
		if (p.cascade)
		{
			const Point last = at[count - 1];
			more[moreCount++] = { last.x + (p.edge == Edge::Right ? p.step : -p.step), last.y + p.step };
		}
		else if (p.edge == Edge::Top)
		{
			more[moreCount++] = { p.step, minY - gap - size.h };
			more[moreCount++] = { p.step, maxY + gap };
		}
		else
		{
			const Point left{ minX - gap - size.w, p.step };
			const Point right{ maxX + gap, p.step };
			more[moreCount++] = p.edge == Edge::Right ? left : right;
			more[moreCount++] = p.edge == Edge::Right ? right : left;
		}
		for (int i = 0; i < moreCount; ++i)
		{
			more[i] = ClampToScreen(p, size, more[i]);
			if (detail::Free(p, sizes, at, count, more[i], size))
				return more[i];
		}

		return DefaultLead(p, size);
	}

	// Every pin placed one after the other as if just pinned: the first at the
	// default spot, the rest by PlaceNew; with p.center the group is then moved
	// along the edge to the middle of the screen.
	inline void SpawnAll(const Params& p, const Size* sizes, int count, Point* out)
	{
		if (count > kMaxPins)
			count = kMaxPins;
		for (int k = 0; k < count; ++k)
			out[k] = PlaceNew(p, sizes, out, k, sizes[k]);

		if (!p.center || count <= 0)
			return;
		float lo = 0.0f, hi = 0.0f;
		detail::SpanAlongEdge(p, sizes, out, count, lo, hi);
		const bool alongX = p.edge == Edge::Top;
		const float limit = alongX ? p.screenW : p.screenH;
		if (hi - lo >= limit)
			return;
		const float delta = std::round((limit - (hi - lo)) * 0.5f - lo);
		for (int k = 0; k < count; ++k)
			(alongX ? out[k].x : out[k].y) += delta;
	}

	// Switching the PinLayout: where the lead (or a grouped anchor) goes so the
	// pins land at the closest equivalent of where they were. `from` and `to`
	// differ in their edge; `to` holds the new settings, its center flag included.
	//   Right <-> Left: the mirror image, same height and same distance from
	//     the edge.
	//   To Top: lined up along the top, centered across the screen.
	//   From Top: at the new edge's default spot, centered down the screen when
	//     to.center is on.
	inline Point SwitchLead(const Params& from, const Params& to, const Size* sizes, int count, Point lead)
	{
		if (from.edge == to.edge || count <= 0)
			return lead;

		if (from.edge != Edge::Top && to.edge != Edge::Top)
			return ClampToScreen(to, sizes[0], { to.screenW - lead.x - sizes[0].w, lead.y });

		Params centered = to;
		centered.center = to.edge == Edge::Top || to.center;
		const Point start = DefaultLead(to, sizes[0]);
		return centered.center ? CenteredLead(centered, sizes, count, start) : start;
	}

	// The same for pins that each have a spot of their own (ungrouped): a mirror
	// moves every pin to its mirror image; a switch to or from Top places the
	// pins again as if just pinned, centered down or across the screen.
	inline void SwitchAll(const Params& from, const Params& to, const Size* sizes, int count, Point* at)
	{
		if (from.edge == to.edge || count <= 0)
			return;
		if (count > kMaxPins)
			count = kMaxPins;

		if (from.edge != Edge::Top && to.edge != Edge::Top)
		{
			for (int k = 0; k < count; ++k)
				at[k] = ClampToScreen(to, sizes[k], { to.screenW - at[k].x - sizes[k].w, at[k].y });
			return;
		}

		Params centered = to;
		centered.center = to.edge == Edge::Top || to.center;
		SpawnAll(centered, sizes, count, at);
	}

	// Pins that move on their own (a pin added or removed, a layout switch,
	// Clear, Arrange) ease from where they were to their new spot over this
	// long, in seconds.
	constexpr float kGlideSeconds = 0.18f;

	// Ease-out: fast at first, settling at the end. 0 at t = 0, 1 at t = 1.
	inline float EaseOut(float t)
	{
		const float u = 1.0f - std::clamp(t, 0.0f, 1.0f);
		return 1.0f - u * u * u;
	}

	// Where a window is `elapsed` seconds into a glide from `from` to `to`.
	inline Point GlideAt(Point from, Point to, float elapsed)
	{
		const float e = EaseOut(elapsed / kGlideSeconds);
		return { from.x + (to.x - from.x) * e, from.y + (to.y - from.y) * e };
	}

	// The spacing option's value from its config text, kept in 1 to 50; no
	// number is the default spacing.
	inline int GapFromText(const char* text)
	{
		if (!text || !text[0])
			return kGapDefault;
		char* end = nullptr;
		const long value = strtol(text, &end, 10);
		return end == text ? kGapDefault : static_cast<int>(std::clamp(value, static_cast<long>(kGapMin), static_cast<long>(kGapMax)));
	}
}
