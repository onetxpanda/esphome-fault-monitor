#include "fault_monitor.h"

#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

namespace esphome {
namespace fault_monitor {

static const char *const TAG = "fault_monitor";

// Persisted state: which latching faults were active, keyed by a hash of the
// declared fault list so a changed list never restores the wrong faults.
struct PersistedState {
  uint32_t list_hash;
  uint32_t bits;
};

static const char *severity_str(Severity s) { return s == Severity::CRITICAL ? "critical" : "warning"; }

static const char *phase_str(Phase p) {
  switch (p) {
    case Phase::CLEAR:
      return "clear";
    case Phase::PENDING_RAISE:
      return "pending raise";
    case Phase::ACTIVE:
      return "active";
    case Phase::PENDING_CLEAR:
      return "pending clear";
    case Phase::LATCHED:
      return "latched";
  }
  return "?";
}

static Tri tri_not(Tri t) {
  if (t == Tri::UNKNOWN)
    return Tri::UNKNOWN;
  return t == Tri::TRUE_ ? Tri::FALSE_ : Tri::TRUE_;
}

uint32_t FaultMonitor::list_hash_() const {
  uint32_t h = fnv1_hash("fault_monitor/v1");
  for (const Fault *f : this->faults_) {
    h = fnv1_hash_extend(h, fnv1_hash(f->get_code()));
    h = fnv1_hash_extend(h, static_cast<uint32_t>(f->logic_.timing().latch));
  }
  return h;
}

void FaultMonitor::setup() {
  this->pending_.reserve(this->faults_.size());
  this->pref_ = global_preferences->make_preference<PersistedState>(fnv1_hash("fault_monitor"), true);
  PersistedState st{};
  if (this->pref_.load(&st) && st.list_hash == this->list_hash_()) {
    this->persisted_bits_ = st.bits;
    for (size_t i = 0; i < this->faults_.size() && i < 32; i++) {
      if (st.bits & (1u << i)) {
        Fault *f = this->faults_[i];
        f->logic_.restore_latched();
        ESP_LOGW(TAG, "[%s] restored LATCHED from before reboot: %s", f->get_code(), f->get_description());
      }
    }
  }
  // Publish the restored state right away; a full evaluation follows on the
  // first update.
  this->publish_(true);
}

void FaultMonitor::dump_config() {
  ESP_LOGCONFIG(TAG, "Fault monitor: %u faults", (unsigned) this->faults_.size());
  LOG_UPDATE_INTERVAL(this);
  for (const Fault *f : this->faults_) {
    const Timing &t = f->logic_.timing();
    ESP_LOGCONFIG(TAG, "  [%s] %s, raise_after %ums, clear_after %ums, startup_delay %ums%s, unknown=%s",
                  f->get_code(), severity_str(f->get_severity()), (unsigned) t.raise_after_ms,
                  (unsigned) t.clear_after_ms, (unsigned) t.startup_delay_ms, t.latch ? ", latching" : "",
                  t.unknown == UnknownPolicy::HOLD ? "hold" : (t.unknown == UnknownPolicy::RAISE ? "raise" : "clear"));
    ESP_LOGCONFIG(TAG, "      %s (now %s)", f->get_description(), phase_str(f->get_phase()));
  }
}

void FaultMonitor::evaluate() {
  if (this->evaluating_) {
    // Called from one of our own triggers: run once more when this pass ends.
    this->evaluate_again_ = true;
    return;
  }
  this->evaluating_ = true;
  // Bounded re-runs, so a trigger that keeps requesting evaluation can't loop.
  for (int pass = 0; pass < 3; pass++) {
    this->evaluate_again_ = false;
    const uint32_t now = millis();
    this->pending_.clear();

    // 1. Advance every fault's state machine.
    for (Fault *f : this->faults_) {
      Tri raise = Fault::to_tri_(f->condition_);
      Tri clear = f->clear_condition_ ? Fault::to_tri_(f->clear_condition_) : tri_not(raise);
      Transition t = f->logic_.update(now, raise, clear);
      if (t != Transition::NONE)
        this->pending_.emplace_back(f, t);
    }
    // 2. Persist latching faults before anything else happens (an on_raise
    //    action may reboot the device).
    this->persist_();
    // 3. Report, so the new state is visible before any action runs.
    this->publish_(false);
    // 4. Log and run on_raise / on_clear, in declaration order.
    for (auto &p : this->pending_)
      this->apply_transition_(p.first, p.second);
    // 5. Re-assert active responses.
    for (Fault *f : this->faults_) {
      if (f->is_active()) {
        for (FaultTrigger *t : f->while_active_)
          t->trigger();
      }
    }
    if (!this->evaluate_again_)
      break;
  }
  this->evaluating_ = false;
}

void FaultMonitor::apply_transition_(Fault *f, Transition t) {
  switch (t) {
    case Transition::RAISED:
      if (f->is_critical()) {
        ESP_LOGE(TAG, "[%s] RAISED (critical): %s", f->get_code(), f->get_description());
      } else {
        ESP_LOGW(TAG, "[%s] RAISED (warning): %s", f->get_code(), f->get_description());
      }
      for (FaultTrigger *tr : f->on_raise_)
        tr->trigger();
      break;
    case Transition::LATCHED:
      ESP_LOGW(TAG, "[%s] condition cleared; LATCHED until acknowledged", f->get_code());
      break;
    case Transition::CLEARED:
      ESP_LOGI(TAG, "[%s] cleared", f->get_code());
      for (FaultTrigger *tr : f->on_clear_)
        tr->trigger();
      break;
    case Transition::NONE:
      break;
  }
}

void FaultMonitor::persist_() {
  uint32_t bits = 0;
  for (size_t i = 0; i < this->faults_.size() && i < 32; i++) {
    if (this->faults_[i]->logic_.needs_persisting())
      bits |= (1u << i);
  }
  if (bits == this->persisted_bits_)
    return;
  PersistedState st{this->list_hash_(), bits};
  if (this->pref_.save(&st) && global_preferences->sync()) {
    this->persisted_bits_ = bits;
  } else {
    ESP_LOGE(TAG, "Failed to persist latched faults");
  }
}

std::string FaultMonitor::build_report(bool critical_only) const {
  std::string out;
  for (int pass = 0; pass < 2; pass++) {
    const bool want_critical = pass == 0;
    if (critical_only && !want_critical)
      break;
    for (const Fault *f : this->faults_) {
      if (f->is_critical() != want_critical || !f->is_active())
        continue;
      if (!out.empty())
        out += ',';
      out += f->get_code();
    }
  }
  return out.empty() ? std::string("OK") : out;
}

void FaultMonitor::publish_(bool force) {
  force = force || !this->published_once_;
#ifdef USE_BINARY_SENSOR
  for (Fault *f : this->faults_) {
    if (f->binary_sensor_ == nullptr)
      continue;
    if (force || !f->binary_sensor_->has_state() || f->binary_sensor_->state != f->is_active())
      f->binary_sensor_->publish_state(f->is_active());
  }
#endif
#ifdef USE_TEXT_SENSOR
  if (this->report_ != nullptr) {
    std::string r = this->build_report(false);
    if (force || !this->report_->has_state() || this->report_->state != r)
      this->report_->publish_state(r);
  }
  if (this->critical_report_ != nullptr) {
    std::string r = this->build_report(true);
    if (force || !this->critical_report_->has_state() || this->critical_report_->state != r)
      this->critical_report_->publish_state(r);
  }
  if (this->summary_ != nullptr) {
    const char *s = this->any_active(Severity::CRITICAL) ? "critical" : (this->any_active() ? "warning" : "ok");
    if (force || !this->summary_->has_state() || this->summary_->state != s)
      this->summary_->publish_state(s);
  }
#endif
  this->published_once_ = true;
}

bool FaultMonitor::acknowledge(const std::string &code) {
  bool found = code.empty();
  bool changed = false;
  for (Fault *f : this->faults_) {
    if (!code.empty() && code != f->get_code())
      continue;
    found = true;
    bool was_persisted = f->logic_.needs_persisting();
    Transition t = f->logic_.acknowledge();
    if (t == Transition::CLEARED) {
      ESP_LOGI(TAG, "[%s] acknowledged", f->get_code());
      // Note: may run inside evaluate() (from a trigger), so it must not
      // touch pending_.
      this->persist_();
      this->publish_(false);
      this->apply_transition_(f, t);
      changed = true;
    } else if (was_persisted != f->logic_.needs_persisting()) {
      ESP_LOGI(TAG, "[%s] acknowledged while active: will clear instead of latching", f->get_code());
      changed = true;
    }
  }
  if (changed)
    this->persist_();
  if (!found)
    ESP_LOGW(TAG, "acknowledge: no fault with code '%s'", code.c_str());
  return found;
}

bool FaultMonitor::any_active() const {
  for (const Fault *f : this->faults_) {
    if (f->is_active())
      return true;
  }
  return false;
}

bool FaultMonitor::any_active(Severity severity) const {
  for (const Fault *f : this->faults_) {
    if (f->is_active() && f->get_severity() == severity)
      return true;
  }
  return false;
}

Fault *FaultMonitor::get_fault(const std::string &code) const {
  for (Fault *f : this->faults_) {
    if (code == f->get_code())
      return f;
  }
  return nullptr;
}

}  // namespace fault_monitor
}  // namespace esphome
