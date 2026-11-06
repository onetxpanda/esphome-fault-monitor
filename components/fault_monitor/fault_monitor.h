#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/core/preferences.h"

#ifdef USE_BINARY_SENSOR
#include "esphome/components/binary_sensor/binary_sensor.h"
#endif
#ifdef USE_TEXT_SENSOR
#include "esphome/components/text_sensor/text_sensor.h"
#endif

#include "fault_logic.h"

namespace esphome {
namespace fault_monitor {

enum class Severity : uint8_t { WARNING = 0, CRITICAL = 1 };

class FaultTrigger : public Trigger<> {};

/// One declared fault: its identity (code, description, severity), its
/// conditions, its timing/latching behaviour and its automations.
class Fault {
 public:
  Fault(const char *code, const char *description, Severity severity)
      : code_(code), description_(description), severity_(severity) {}

  void set_timing(uint32_t raise_after_ms, uint32_t clear_after_ms, uint32_t startup_delay_ms, bool latch,
                  UnknownPolicy unknown) {
    Timing t;
    t.raise_after_ms = raise_after_ms;
    t.clear_after_ms = clear_after_ms;
    t.startup_delay_ms = startup_delay_ms;
    t.latch = latch;
    t.unknown = unknown;
    this->logic_.set_timing(t);
  }
  void set_condition(std::function<std::optional<bool>()> &&f) { this->condition_ = std::move(f); }
  void set_clear_condition(std::function<std::optional<bool>()> &&f) { this->clear_condition_ = std::move(f); }
  void add_on_raise_trigger(FaultTrigger *t) { this->on_raise_.push_back(t); }
  void add_on_clear_trigger(FaultTrigger *t) { this->on_clear_.push_back(t); }
  void add_while_active_trigger(FaultTrigger *t) { this->while_active_.push_back(t); }
#ifdef USE_BINARY_SENSOR
  void set_binary_sensor(binary_sensor::BinarySensor *s) { this->binary_sensor_ = s; }
#endif

  const char *get_code() const { return this->code_; }
  const char *get_description() const { return this->description_; }
  Severity get_severity() const { return this->severity_; }
  bool is_critical() const { return this->severity_ == Severity::CRITICAL; }
  /// Active = reported: raised and not yet cleared (includes latched).
  bool is_active() const { return this->logic_.active(); }
  bool is_latched() const { return this->logic_.latched(); }
  Phase get_phase() const { return this->logic_.phase(); }

 protected:
  friend class FaultMonitor;

  static Tri to_tri_(const std::function<std::optional<bool>()> &f) {
    if (!f)
      return Tri::UNKNOWN;
    std::optional<bool> r = f();
    if (!r.has_value())
      return Tri::UNKNOWN;
    return *r ? Tri::TRUE_ : Tri::FALSE_;
  }

  const char *code_;
  const char *description_;
  Severity severity_;
  FaultLogic logic_;
  std::function<std::optional<bool>()> condition_;
  std::function<std::optional<bool>()> clear_condition_;
  std::vector<FaultTrigger *> on_raise_;
  std::vector<FaultTrigger *> on_clear_;
  std::vector<FaultTrigger *> while_active_;
#ifdef USE_BINARY_SENSOR
  binary_sensor::BinarySensor *binary_sensor_{nullptr};
#endif
};

class FaultMonitor : public PollingComponent {
 public:
  void add_fault(Fault *fault) { this->faults_.push_back(fault); }
#ifdef USE_TEXT_SENSOR
  void set_report_sensor(text_sensor::TextSensor *s) { this->report_ = s; }
  void set_critical_report_sensor(text_sensor::TextSensor *s) { this->critical_report_ = s; }
  void set_summary_sensor(text_sensor::TextSensor *s) { this->summary_ = s; }
#endif

  void setup() override;
  void update() override { this->evaluate(); }
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  /// Evaluate every fault now (also runs every update_interval). Safe to call
  /// from automations, including from this monitor's own triggers.
  void evaluate();
  /// Acknowledge one fault by code, or all faults when `code` is empty.
  /// Returns true if a fault with that code exists (or code is empty).
  bool acknowledge(const std::string &code);

  bool any_active() const;
  bool any_active(Severity severity) const;
  const std::vector<Fault *> &get_faults() const { return this->faults_; }
  Fault *get_fault(const std::string &code) const;
  /// Active codes, critical first then warnings, each in declaration order;
  /// "OK" when nothing is active. `critical_only` restricts to critical.
  std::string build_report(bool critical_only = false) const;

 protected:
  void apply_transition_(Fault *fault, Transition t);
  void persist_();
  void publish_(bool force);
  uint32_t list_hash_() const;

  std::vector<Fault *> faults_;
#ifdef USE_TEXT_SENSOR
  text_sensor::TextSensor *report_{nullptr};
  text_sensor::TextSensor *critical_report_{nullptr};
  text_sensor::TextSensor *summary_{nullptr};
#endif
  ESPPreferenceObject pref_;
  uint32_t persisted_bits_{0};
  bool evaluating_{false};
  bool evaluate_again_{false};
  bool published_once_{false};
  std::vector<std::pair<Fault *, Transition>> pending_;
};

// ---- Conditions / actions -------------------------------------------------

template<typename... Ts> class FaultIsActiveCondition : public Condition<Ts...> {
 public:
  explicit FaultIsActiveCondition(Fault *fault) : fault_(fault) {}
  bool check(const Ts &...x) override { return this->fault_->is_active(); }

 protected:
  Fault *fault_;
};

template<typename... Ts> class AnyActiveCondition : public Condition<Ts...> {
 public:
  explicit AnyActiveCondition(FaultMonitor *parent) : parent_(parent) {}
  void set_severity(Severity s) {
    this->severity_ = s;
    this->filter_ = true;
  }
  bool check(const Ts &...x) override {
    return this->filter_ ? this->parent_->any_active(this->severity_) : this->parent_->any_active();
  }

 protected:
  FaultMonitor *parent_;
  Severity severity_{Severity::WARNING};
  bool filter_{false};
};

template<typename... Ts> class EvaluateAction : public Action<Ts...> {
 public:
  explicit EvaluateAction(FaultMonitor *parent) : parent_(parent) {}
  void play(const Ts &...x) override { this->parent_->evaluate(); }

 protected:
  FaultMonitor *parent_;
};

template<typename... Ts> class AcknowledgeAction : public Action<Ts...> {
 public:
  explicit AcknowledgeAction(FaultMonitor *parent) : parent_(parent) {}
  TEMPLATABLE_VALUE(std::string, code)
  void play(const Ts &...x) override {
    std::string code = this->code_.has_value() ? this->code_.value(x...) : std::string();
    this->parent_->acknowledge(code);
  }

 protected:
  FaultMonitor *parent_;
};

}  // namespace fault_monitor
}  // namespace esphome
