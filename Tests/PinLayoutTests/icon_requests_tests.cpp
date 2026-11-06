// Icon request tests. The table and the serving loop in
// Codex/Icons/icon_requests.h take no game types, so a fake clock and a fake
// loader stand in for the game.
#include "Icons/icon_requests.h"
#include "icon_requests_tests.h"

#include <cstdio>
#include <initializer_list>
#include <string>
#include <unordered_set>
#include <vector>

using namespace CodexIconRequests;

namespace
{
	Handle H(int n) { return reinterpret_cast<Handle>(static_cast<size_t>(n)); }

	std::unordered_set<std::string> Known(std::initializer_list<const char*> names)
	{
		return std::unordered_set<std::string>(names.begin(), names.end());
	}

	// A clock that moves only when the loader runs.
	struct FakeClock
	{
		double now = 0.0;
		double operator()() const { return now; }
	};
}

void RunIconRequestTests(CheckFn check)
{
	printf("icon requests\n");

	{
		Table t;
		t.SetKnown(Known({ "a", "b" }));
		check(t.Get("a") == nullptr, "an unloaded icon gives null", "get");
		t.Get("a");
		t.Get("a");
		check(t.Pending() == 1, "asking again for the same icon queues it once", "dedupe");
		t.Get("b");
		check(t.Pending() == 2, "a second icon queues too", "dedupe");
		check(t.Has("a") && t.Has("b"), "a queued icon counts as having one", "has");
	}

	{
		Table t;
		check(t.Get("a") == nullptr && t.Pending() == 0, "nothing queues before the item list is known", "unknown");
		t.SetKnown(Known({ "a" }));
		check(t.Get("nope") == nullptr && t.Pending() == 0, "an unknown name never queues", "unknown");
		check(!t.Has("nope"), "an unknown name has no icon", "unknown");
		check(t.Has("a"), "a known name has one", "unknown");
		t.Get("a");
		check(t.Pending() == 1, "a known name queues once the list is known", "unknown");
	}

	{
		Table t;
		t.SetKnown(Known({ "a", "b", "c", "d", "e" }));
		for (const char* n : { "a", "b", "c", "d", "e" })
			t.Get(n);

		FakeClock clock;
		const auto load = [&](const std::string&) { clock.now += 5.0; return H(1); };

		check(Serve(t, load, clock, 8.0) == 2, "the budget stops the loads once it is spent", "budget");
		check(t.Pending() == 3, "the rest wait for the next tick", "budget");
		check(Serve(t, load, clock, 8.0) == 2, "the next tick serves the next ones", "budget");
		check(Serve(t, load, clock, 8.0) == 1, "the last one is served alone", "budget");
		check(Serve(t, load, clock, 8.0) == 0, "an empty queue serves nothing", "budget");
		check(t.Get("a") == H(1), "a loaded icon is returned", "budget");
		check(t.Pending() == 0, "a loaded icon is not queued again", "budget");
	}

	{
		Table t;
		t.SetKnown(Known({ "a", "b" }));
		t.Get("a");
		t.Get("b");
		FakeClock clock;
		const auto slow = [&](const std::string&) { clock.now += 100.0; return H(1); };
		check(Serve(t, slow, clock, 8.0) == 1, "one load is served even when it alone exceeds the budget", "budget");
		const auto instant = [&](const std::string&) { return H(2); };
		check(Serve(t, instant, clock, 0.0) == 1, "a budget of zero still serves one", "budget");
	}

	{
		Table t;
		t.SetKnown(Known({ "a", "b", "c" }));
		for (const char* n : { "a", "b", "c" })
			t.Get(n);
		FakeClock clock;
		int calls = 0;
		const auto fail = [&](const std::string&) { ++calls; return Handle(nullptr); };
		check(Serve(t, fail, clock, 8.0) == 3 && calls == 3, "each queued icon is tried once per tick", "retry");
		check(t.Pending() == 3, "failed icons stay queued for a later tick", "retry");
		check(t.Get("a") == nullptr && t.Pending() == 3, "asking for a queued icon does not queue it again", "retry");
	}

	{
		Table t;
		t.SetKnown(Known({ "a" }));
		t.Get("a");
		FakeClock clock;
		int calls = 0;
		const auto lateLoad = [&](const std::string&) { return ++calls < 40 ? Handle(nullptr) : H(7); };
		for (int i = 0; i < 100; ++i)
			Serve(t, lateLoad, clock, 8.0);
		check(calls == 40, "a failing icon is retried until it loads, then left alone", "retry");
		check(t.Get("a") == H(7), "the retried icon ends up loaded", "retry");
	}

	{
		Table t;
		t.SetKnown(Known({ "a", "b" }));
		t.Get("a");
		FakeClock clock;
		int calls = 0;
		const auto never = [&](const std::string&) { ++calls; return Handle(nullptr); };
		for (int i = 0; i < kMaxAttempts + 50; ++i)
			Serve(t, never, clock, 8.0);
		check(calls == kMaxAttempts, "a failing icon is tried kMaxAttempts times and no more", "give up");
		check(t.Pending() == 0, "an icon given up on leaves the queue", "give up");
		check(!t.Has("a"), "an icon given up on counts as having none", "give up");
		check(t.Get("a") == nullptr && t.Pending() == 0, "asking for it again does not queue it", "give up");
		check(t.Has("b"), "other icons are not affected", "give up");
	}

	{
		Table t;
		t.SetKnown(Known({ "a", "b" }));
		t.Get("a");
		t.Get("b");
		FakeClock clock;
		int calls = 0;
		std::vector<std::string> logged;
		const auto noSlot = [&](const std::string& n)
		{
			++calls;
			if (n == "a")
				t.GiveUp(n);
			return Handle(nullptr);
		};
		const auto note = [&](const std::string& n) { logged.push_back(n); };
		Serve(t, noSlot, clock, 8.0, note);
		check(calls == 2 && t.Pending() == 1, "an icon with no free slot is not queued again", "no slot");
		check(!t.Has("a") && t.Has("b"), "only the icon without a slot counts as having none", "no slot");
		for (int i = 0; i < 20; ++i)
			Serve(t, noSlot, clock, 8.0, note);
		check(calls == 22, "an icon with no free slot is tried once", "no slot");
		check(!t.GiveUp("a"), "giving up twice reports once", "no slot");
		check(logged.empty(), "a terminal give-up is not also counted as running out of attempts", "no slot");
	}

	{
		Table t;
		t.SetKnown(Known({ "a" }));
		t.Get("a");
		FakeClock clock;
		std::vector<std::string> logged;
		const auto never = [](const std::string&) { return Handle(nullptr); };
		for (int i = 0; i < kMaxAttempts + 5; ++i)
			Serve(t, never, clock, 8.0, [&](const std::string& n) { logged.push_back(n); });
		check(logged.size() == 1 && logged[0] == "a", "running out of attempts is reported once", "give up");
	}

	{
		Table t;
		t.SetKnown(Known({ "a", "b", "c" }));
		t.Get("a");
		t.Get("b");
		t.Get("c");
		FakeClock clock;
		int n = 0;
		const auto load = [&](const std::string&) { return H(++n); };
		Serve(t, load, clock, 0.0);
		Serve(t, load, clock, 0.0);
		check(t.Pending() == 1, "two of three are loaded", "clear");
		const std::vector<Handle> freed = t.Clear();
		check(freed.size() == 2, "clearing hands back the loaded handles to free", "clear");
		check(t.Pending() == 0, "clearing drops the queue", "clear");
		check(!t.Has("a") && t.Get("a") == nullptr && t.Pending() == 0, "clearing forgets the item list", "clear");
		t.SetKnown(Known({ "a" }));
		check(t.Get("a") == nullptr && t.Pending() == 1, "after a clear the table works again", "clear");
	}

	{
		// The loader runs without the table locked: the render callback asks
		// for icons while a load is in flight, and a lock held across the load
		// would stall it (and deadlock here).
		Table t;
		t.SetKnown(Known({ "a", "b" }));
		t.Get("a");
		FakeClock clock;
		bool asked = false;
		const auto load = [&](const std::string&)
		{
			t.Get("b");
			asked = t.Has("a") && t.Pending() == 1;
			return H(1);
		};
		Serve(t, load, clock, 8.0);
		check(asked, "the table can be used while an icon loads", "unlocked");
	}
}
