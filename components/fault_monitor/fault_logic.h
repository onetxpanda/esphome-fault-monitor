#pragma once

// Pure fault state machine: no ESPHome dependencies, so it can be unit tested
// on a desktop compiler (see tests/unit). The ESPHome component feeds it the
// condition results and acts on the transitions it returns.

#include <cstdint>

namespace esphome {
namespace fault_monitor {

/// Result of evaluating a condition: the fault is present, absent, or there
/// is not enough information to say (e.g. a sensor has no reading yet).
enum class Tri : uint8_t { FALSE_ = 0, TRUE_ = 1, UNKNOWN = 2 };

/// What an UNKNOWN evaluation means for a fault.
///  HOLD:  no evidence either way. The settled state (clear or active) is kept
///         and any pending transition is cancelled, so a fault is only raised
///         or cleared after raise_after / clear_after of continuous evidence.
///  RAISE: treat "can't tell" as the fault being present (fail-safe).
///  CLEAR: treat "can't tell" as the fault being absent.
enum class UnknownPolicy : uint8_t { HOLD = 0, RAISE = 1, CLEAR = 2 };

enum class Phase : uint8_t {
  CLEAR = 0,          // not active
  PENDING_RAISE = 1,  // condition true, waiting for raise_after
  ACTIVE = 2,         // active and reported
  PENDING_CLEAR = 3,  // clear condition true, waiting for clear_after (still active)
  LATCHED = 4,        // condition cleared, but held active until acknowledged
};

enum class Transition : uint8_t {
  NONE = 0,
  RAISED = 1,   // CLEAR/PENDING_RAISE -> ACTIVE
  CLEARED = 2,  // -> CLEAR (no longer active)
  LATCHED = 3,  // ACTIVE -> LATCHED (still active, awaiting acknowledgement)
};

struct Timing {
  uint32_t raise_after_ms{0};
  uint32_t clear_after_ms{0};
  uint32_t startup_delay_ms{0};
  bool latch{false};
  UnknownPolicy unknown{UnknownPolicy::HOLD};
};

class FaultLogic {
 public:
  FaultLogic() = default;
  explicit FaultLogic(const Timing &timing) : timing_(timing) {}

  void set_timing(const Timing &timing) { this->timing_ = timing; }
  const Timing &timing() const { return this->timing_; }

  /// Advance the state machine.
  ///  now_ms: monotonic milliseconds (wrap-safe; may wrap after ~49 days).
  ///  raise:  result of the fault condition (TRUE_ = fault present).
  ///  clear:  result of the clear condition (TRUE_ = fault gone). Without a
  ///          separate clear condition, pass the negation of `raise`
  ///          (UNKNOWN stays UNKNOWN).
  Transition update(uint32_t now_ms, Tri raise, Tri clear) {
    if (!this->started_) {
      if (now_ms < this->timing_.startup_delay_ms)
        return Transition::NONE;  // still in the startup grace period
      this->started_ = true;
    }
    switch (this->phase_) {
      case Phase::CLEAR: {
        if (this->resolve_raise_(raise) != Tri::TRUE_)
          return Transition::NONE;
        if (this->timing_.raise_after_ms == 0)
          return this->raise_();
        this->phase_ = Phase::PENDING_RAISE;
        this->since_ms_ = now_ms;
        return Transition::NONE;
      }
      case Phase::PENDING_RAISE: {
        if (this->resolve_raise_(raise) != Tri::TRUE_) {
          this->phase_ = Phase::CLEAR;  // evidence interrupted: start over
          return Transition::NONE;
        }
        if (now_ms - this->since_ms_ >= this->timing_.raise_after_ms)
          return this->raise_();
        return Transition::NONE;
      }
      case Phase::ACTIVE: {
        if (this->resolve_clear_(clear) != Tri::TRUE_)
          return Transition::NONE;
        if (this->timing_.clear_after_ms == 0)
          return this->clear_();
        this->phase_ = Phase::PENDING_CLEAR;
        this->since_ms_ = now_ms;
        return Transition::NONE;
      }
      case Phase::PENDING_CLEAR: {
        if (this->resolve_clear_(clear) != Tri::TRUE_) {
          this->phase_ = Phase::ACTIVE;  // evidence interrupted: stay active
          return Transition::NONE;
        }
        if (now_ms - this->since_ms_ >= this->timing_.clear_after_ms)
          return this->clear_();
        return Transition::NONE;
      }
      case Phase::LATCHED: {
        // Still active. If the condition comes back, it is simply active
        // again (no new RAISED event: it was never reported as cleared), and
        // an earlier acknowledgement no longer applies.
        if (this->resolve_raise_(raise) == Tri::TRUE_) {
          this->phase_ = Phase::ACTIVE;
          this->acknowledged_ = false;
        }
        return Transition::NONE;
      }
    }
    return Transition::NONE;
  }

  /// Operator acknowledgement.
  ///  LATCHED:                 clears the fault now (returns CLEARED).
  ///  ACTIVE / PENDING_CLEAR:  pre-acknowledges, so the fault clears instead
  ///                           of latching when its condition goes away.
  ///  otherwise:               no effect.
  Transition acknowledge() {
    switch (this->phase_) {
      case Phase::LATCHED:
        this->phase_ = Phase::CLEAR;
        this->acknowledged_ = false;
        return Transition::CLEARED;
      case Phase::ACTIVE:
      case Phase::PENDING_CLEAR:
        this->acknowledged_ = true;
        return Transition::NONE;
      default:
        return Transition::NONE;
    }
  }

  /// Restore a latched fault after a reboot (from persisted state). The fault
  /// is active from the first evaluation and stays so until acknowledged.
  void restore_latched() {
    this->phase_ = Phase::LATCHED;
    this->acknowledged_ = false;
  }

  Phase phase() const { return this->phase_; }
  bool active() const {
    return this->phase_ == Phase::ACTIVE || this->phase_ == Phase::PENDING_CLEAR || this->phase_ == Phase::LATCHED;
  }
  bool latched() const { return this->phase_ == Phase::LATCHED; }
  bool started() const { return this->started_; }
  /// For persistence: a latching fault that is active must survive a reboot
  /// (it may be the reason for the reboot), unless it was pre-acknowledged.
  bool needs_persisting() const { return this->timing_.latch && this->active() && !this->acknowledged_; }

 protected:
  Tri resolve_raise_(Tri raise) const {
    if (raise != Tri::UNKNOWN)
      return raise;
    switch (this->timing_.unknown) {
      case UnknownPolicy::RAISE:
        return Tri::TRUE_;
      case UnknownPolicy::CLEAR:
        return Tri::FALSE_;
      default:
        return Tri::UNKNOWN;
    }
  }
  Tri resolve_clear_(Tri clear) const {
    if (clear != Tri::UNKNOWN)
      return clear;
    switch (this->timing_.unknown) {
      case UnknownPolicy::RAISE:
        return Tri::FALSE_;  // "can't tell" keeps the fault active
      case UnknownPolicy::CLEAR:
        return Tri::TRUE_;
      default:
        return Tri::UNKNOWN;
    }
  }
  Transition raise_() {
    this->phase_ = Phase::ACTIVE;
    this->acknowledged_ = false;
    return Transition::RAISED;
  }
  Transition clear_() {
    if (this->timing_.latch && !this->acknowledged_) {
      this->phase_ = Phase::LATCHED;
      return Transition::LATCHED;
    }
    this->phase_ = Phase::CLEAR;
    this->acknowledged_ = false;
    return Transition::CLEARED;
  }

  Timing timing_{};
  Phase phase_{Phase::CLEAR};
  uint32_t since_ms_{0};
  bool started_{false};
  bool acknowledged_{false};
};

}  // namespace fault_monitor
}  // namespace esphome
