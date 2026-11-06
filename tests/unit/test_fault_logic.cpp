// Unit tests for the pure fault state machine (fault_logic.h).
// Build and run:  make -C tests/unit

#include "../../components/fault_monitor/fault_logic.h"

#include <cstdio>
#include <cstdlib>

using namespace esphome::fault_monitor;

static int g_failures = 0;
static int g_checks = 0;
#define CHECK(cond)                                                     \
  do {                                                                  \
    g_checks++;                                                         \
    if (!(cond)) {                                                      \
      g_failures++;                                                     \
      std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
    }                                                                   \
  } while (0)

static const Tri T = Tri::TRUE_, F = Tri::FALSE_, U = Tri::UNKNOWN;
static Tri NOT(Tri t) { return t == U ? U : (t == T ? F : T); }

// Feed the same raise value (clear = NOT raise) at 1 s steps from `from` to
// `to` inclusive; return the first non-NONE transition and its time.
struct Hit {
  Transition t{Transition::NONE};
  uint32_t at{0};
};
static Hit run(FaultLogic &f, uint32_t from, uint32_t to, Tri raise, uint32_t step = 1000) {
  for (uint32_t now = from;; now += step) {
    Transition t = f.update(now, raise, NOT(raise));
    if (t != Transition::NONE)
      return {t, now};
    if (now >= to)
      break;
  }
  return {};
}

static void test_immediate_raise_and_clear() {
  std::printf("immediate raise/clear\n");
  FaultLogic f(Timing{});
  CHECK(f.update(0, F, T) == Transition::NONE);
  CHECK(!f.active());
  CHECK(f.update(1, T, F) == Transition::RAISED);
  CHECK(f.active());
  CHECK(f.update(2, T, F) == Transition::NONE);  // no repeated RAISED
  CHECK(f.update(3, F, T) == Transition::CLEARED);
  CHECK(!f.active());
  CHECK(f.update(4, F, T) == Transition::NONE);  // no repeated CLEARED
}

static void test_raise_after() {
  std::printf("raise_after qualification\n");
  Timing t;
  t.raise_after_ms = 10000;
  FaultLogic f(t);
  Hit h = run(f, 0, 60000, T);
  CHECK(h.t == Transition::RAISED);
  CHECK(h.at == 10000);
  // An interruption restarts the timer.
  FaultLogic g(t);
  run(g, 0, 8000, T);
  CHECK(g.phase() == Phase::PENDING_RAISE);
  CHECK(!g.active());  // pending is not active / not reported
  CHECK(g.update(9000, F, T) == Transition::NONE);
  CHECK(g.phase() == Phase::CLEAR);
  Hit h2 = run(g, 10000, 60000, T);
  CHECK(h2.t == Transition::RAISED);
  CHECK(h2.at == 20000);
}

static void test_clear_after_and_hysteresis() {
  std::printf("clear_after and separate clear condition\n");
  Timing t;
  t.clear_after_ms = 5000;
  FaultLogic f(t);
  CHECK(f.update(0, T, F) == Transition::RAISED);
  // Separate clear condition: raise no longer true but clear not yet true
  // (inside the hysteresis band) -> stays active.
  CHECK(f.update(1000, F, F) == Transition::NONE);
  CHECK(f.active());
  CHECK(f.update(2000, F, T) == Transition::NONE);  // pending clear starts
  CHECK(f.phase() == Phase::PENDING_CLEAR);
  CHECK(f.active());  // still reported while pending clear
  CHECK(f.update(4000, F, F) == Transition::NONE);  // interrupted
  CHECK(f.phase() == Phase::ACTIVE);
  CHECK(f.update(5000, F, T) == Transition::NONE);
  CHECK(f.update(9999, F, T) == Transition::NONE);
  CHECK(f.update(10000, F, T) == Transition::CLEARED);
}

static void test_unknown_hold() {
  std::printf("unknown = hold\n");
  Timing t;
  t.raise_after_ms = 5000;
  t.clear_after_ms = 5000;
  FaultLogic f(t);
  // Unknown while clear: stays clear.
  CHECK(f.update(0, U, U) == Transition::NONE);
  CHECK(f.phase() == Phase::CLEAR);
  // Unknown during a pending raise cancels it.
  f.update(1000, T, F);
  CHECK(f.phase() == Phase::PENDING_RAISE);
  f.update(3000, U, U);
  CHECK(f.phase() == Phase::CLEAR);
  // Raise properly.
  Hit h = run(f, 4000, 20000, T);
  CHECK(h.t == Transition::RAISED);
  // Unknown while active: stays active indefinitely.
  CHECK(run(f, 10000, 100000, U).t == Transition::NONE);
  CHECK(f.active());
  // Unknown during a pending clear cancels it (stays active).
  f.update(101000, F, T);
  CHECK(f.phase() == Phase::PENDING_CLEAR);
  f.update(102000, U, U);
  CHECK(f.phase() == Phase::ACTIVE);
}

static void test_unknown_raise() {
  std::printf("unknown = raise (fail-safe)\n");
  Timing t;
  t.unknown = UnknownPolicy::RAISE;
  t.raise_after_ms = 3000;
  FaultLogic f(t);
  Hit h = run(f, 0, 10000, U);
  CHECK(h.t == Transition::RAISED);
  CHECK(h.at == 3000);
  // Unknown never clears it.
  CHECK(run(f, 4000, 50000, U).t == Transition::NONE);
  CHECK(f.active());
  CHECK(f.update(51000, F, T) == Transition::CLEARED);
}

static void test_unknown_clear() {
  std::printf("unknown = clear\n");
  Timing t;
  t.unknown = UnknownPolicy::CLEAR;
  FaultLogic f(t);
  CHECK(f.update(0, U, U) == Transition::NONE);
  CHECK(!f.active());
  CHECK(f.update(1, T, F) == Transition::RAISED);
  CHECK(f.update(2, U, U) == Transition::CLEARED);
}

static void test_latch() {
  std::printf("latching and acknowledgement\n");
  Timing t;
  t.latch = true;
  FaultLogic f(t);
  CHECK(f.update(0, T, F) == Transition::RAISED);
  CHECK(f.needs_persisting());
  CHECK(f.update(1, F, T) == Transition::LATCHED);
  CHECK(f.active());
  CHECK(f.latched());
  CHECK(f.needs_persisting());
  CHECK(run(f, 2, 100000, F).t == Transition::NONE);  // stays latched
  CHECK(f.latched());
  // Condition returns while latched: active again, no duplicate RAISED.
  CHECK(f.update(100001, T, F) == Transition::NONE);
  CHECK(f.phase() == Phase::ACTIVE);
  CHECK(f.update(100002, F, T) == Transition::LATCHED);
  // Acknowledge clears a latched fault.
  CHECK(f.acknowledge() == Transition::CLEARED);
  CHECK(!f.active());
  CHECK(!f.needs_persisting());
  // Acknowledge while clear: no effect.
  CHECK(f.acknowledge() == Transition::NONE);
}

static void test_pre_acknowledge() {
  std::printf("acknowledge while still active\n");
  Timing t;
  t.latch = true;
  FaultLogic f(t);
  CHECK(f.update(0, T, F) == Transition::RAISED);
  CHECK(f.acknowledge() == Transition::NONE);  // still present: stays active
  CHECK(f.active());
  CHECK(!f.needs_persisting());
  CHECK(f.update(1, F, T) == Transition::CLEARED);  // clears instead of latching
  // A new occurrence resets the acknowledgement.
  CHECK(f.update(2, T, F) == Transition::RAISED);
  CHECK(f.update(3, F, T) == Transition::LATCHED);
}

static void test_restore_latched() {
  std::printf("restore latched after reboot\n");
  Timing t;
  t.latch = true;
  t.startup_delay_ms = 60000;
  FaultLogic f(t);
  f.restore_latched();
  CHECK(f.active());  // active immediately, even during startup grace
  CHECK(f.update(0, F, T) == Transition::NONE);
  CHECK(f.update(70000, F, T) == Transition::NONE);
  CHECK(f.latched());
  CHECK(f.acknowledge() == Transition::CLEARED);
}

static void test_startup_delay() {
  std::printf("startup grace period\n");
  Timing t;
  t.startup_delay_ms = 30000;
  FaultLogic f(t);
  CHECK(run(f, 0, 29000, T).t == Transition::NONE);
  CHECK(!f.started());
  CHECK(f.update(30000, T, F) == Transition::RAISED);
  CHECK(f.started());
}

static void test_wraparound() {
  std::printf("millis() wrap-around\n");
  Timing t;
  t.raise_after_ms = 10000;
  t.clear_after_ms = 10000;
  t.startup_delay_ms = 5000;
  FaultLogic f(t);
  f.update(6000, F, T);  // started
  // Approach the 32-bit wrap with a pending raise straddling it.
  uint32_t start = 0xFFFFFFFFu - 4000u;
  CHECK(f.update(start, T, F) == Transition::NONE);
  CHECK(f.phase() == Phase::PENDING_RAISE);
  CHECK(f.update(start + 9000u, T, F) == Transition::NONE);  // wrapped, 9 s elapsed
  CHECK(f.update(start + 10000u, T, F) == Transition::RAISED);
  // After the wrap `now` is small again: must NOT re-enter the startup grace.
  CHECK(f.update(1000, F, T) == Transition::NONE);
  CHECK(f.phase() == Phase::PENDING_CLEAR);
  CHECK(f.update(12000, F, T) == Transition::CLEARED);
}

int main() {
  test_immediate_raise_and_clear();
  test_raise_after();
  test_clear_after_and_hysteresis();
  test_unknown_hold();
  test_unknown_raise();
  test_unknown_clear();
  test_latch();
  test_pre_acknowledge();
  test_restore_latched();
  test_startup_delay();
  test_wraparound();
  std::printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
