#pragma once

// Where the time goes: QueryPerformanceCounter timers around SkyCraft's per-frame work (and the
// Havok hooks, which run on Skyrim's physics threads), summed and logged as one line every minute
// (every 10 seconds with bDiagnostics), with Skyrim's own frame times for comparison.
namespace skycraft::Perf
{
	enum Slot : int
	{
		kFrame,  // Skyrim's frame: from one player update to the next
		kUpdate,
		kCollision,
		kDigMeshes,
		kDigChanged,
		kLights,
		kWater,
		kDraw,
		kInFrame,
		kGrass,
		kReadbacks,
		kHookCharContacts,
		kHookContacts,
		kHookPlanes,
		kHookLand,
		kCount
	};

	inline constexpr const char* kNames[kCount] = { "frame", "update", "collision", "dig meshes", "dig changed", "lights", "water", "draw",
		"in-frame draw", "grass", "readbacks", "havok char contacts", "havok contacts", "havok planes", "land height" };

	struct Stat
	{
		std::atomic<std::int64_t> ticks{ 0 };
		std::atomic<std::int64_t> max{ 0 };
		std::atomic<std::int64_t> calls{ 0 };
	};
	inline Stat stats[kCount];

	inline std::int64_t Now()
	{
		LARGE_INTEGER t;
		::QueryPerformanceCounter(&t);
		return t.QuadPart;
	}

	inline double TicksPerMs()
	{
		static const double f = [] {
			LARGE_INTEGER q;
			::QueryPerformanceFrequency(&q);
			return double(q.QuadPart) / 1000.0;
		}();
		return f;
	}

	inline void Add(Slot a_slot, std::int64_t a_ticks)
	{
		auto& s = stats[a_slot];
		s.ticks.fetch_add(a_ticks, std::memory_order_relaxed);
		s.calls.fetch_add(1, std::memory_order_relaxed);
		auto m = s.max.load(std::memory_order_relaxed);
		while (a_ticks > m && !s.max.compare_exchange_weak(m, a_ticks, std::memory_order_relaxed)) {
		}
	}

	struct Scope
	{
		explicit Scope(Slot a_slot) :
			slot(a_slot), start(Now()) {}
		~Scope() { Add(slot, Now() - start); }
		Scope(const Scope&) = delete;
		Scope& operator=(const Scope&) = delete;

		Slot         slot;
		std::int64_t start;
	};

	// Once per frame, on the main thread: now and then, the totals as one log line.
	inline void Report()
	{
		static std::int64_t last = 0, windowStart = 0;
		const auto          now = Now();
		if (last) {
			Add(kFrame, now - last);
		}
		last = now;
		if (!windowStart) {
			windowStart = now;
			return;
		}
		const double windowMs = double(now - windowStart) / TicksPerMs();
		if (windowMs < (DiagnosticsEnabled() ? 10000.0 : 60000.0)) {
			return;
		}
		windowStart = now;
		const auto frames = std::max<std::int64_t>(1, stats[kFrame].calls.exchange(0));
		const auto frameTicks = stats[kFrame].ticks.exchange(0);
		const auto frameMax = stats[kFrame].max.exchange(0);
		std::string line = std::format("perf: {} frames in {:.1f} s ({:.0f} fps), frame avg {:.1f} ms, worst {:.1f} ms", frames, windowMs / 1000.0,
			double(frames) * 1000.0 / windowMs, double(frameTicks) / TicksPerMs() / double(frames), double(frameMax) / TicksPerMs());
		for (int i = kFrame + 1; i < kCount; ++i) {
			const auto calls = stats[i].calls.exchange(0);
			const auto ticks = stats[i].ticks.exchange(0);
			const auto max = stats[i].max.exchange(0);
			if (calls) {
				line += std::format(" | {} {:.2f} ms/frame (worst call {:.1f} ms, {:.1f} calls/frame)", kNames[i], double(ticks) / TicksPerMs() / double(frames),
					double(max) / TicksPerMs(), double(calls) / double(frames));
			}
		}
		logger::info("{}", line);
	}
}
