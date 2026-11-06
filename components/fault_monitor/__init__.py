"""fault_monitor: declarative, device-side fault detection and reporting.

See README.md for the full specification.
"""

import re

from esphome import automation
import esphome.codegen as cg
from esphome.components import binary_sensor, text_sensor
import esphome.config_validation as cv
from esphome.const import (
    CONF_ID,
    CONF_NAME,
    CONF_TRIGGER_ID,
    DEVICE_CLASS_PROBLEM,
    DEVICE_CLASS_SAFETY,
    ENTITY_CATEGORY_DIAGNOSTIC,
)

CODEOWNERS = ["@onetxpanda"]
AUTO_LOAD = ["text_sensor", "binary_sensor"]
MULTI_CONF = False

fault_monitor_ns = cg.esphome_ns.namespace("fault_monitor")
FaultMonitor = fault_monitor_ns.class_("FaultMonitor", cg.PollingComponent)
Fault = fault_monitor_ns.class_("Fault")
FaultTrigger = fault_monitor_ns.class_("FaultTrigger", automation.Trigger.template())
Severity = fault_monitor_ns.enum("Severity", is_class=True)
UnknownPolicy = fault_monitor_ns.enum("UnknownPolicy", is_class=True)

FaultIsActiveCondition = fault_monitor_ns.class_("FaultIsActiveCondition", automation.Condition)
AnyActiveCondition = fault_monitor_ns.class_("AnyActiveCondition", automation.Condition)
EvaluateAction = fault_monitor_ns.class_("EvaluateAction", automation.Action)
AcknowledgeAction = fault_monitor_ns.class_("AcknowledgeAction", automation.Action)

CONF_FAULTS = "faults"
CONF_CODE = "code"
CONF_DESCRIPTION = "description"
CONF_SEVERITY = "severity"
CONF_CONDITION = "condition"
CONF_CLEAR_CONDITION = "clear_condition"
CONF_RAISE_AFTER = "raise_after"
CONF_CLEAR_AFTER = "clear_after"
CONF_STARTUP_DELAY = "startup_delay"
CONF_UNKNOWN = "unknown"
CONF_LATCH = "latch"
CONF_ON_RAISE = "on_raise"
CONF_ON_CLEAR = "on_clear"
CONF_WHILE_ACTIVE = "while_active"
CONF_BINARY_SENSOR = "binary_sensor"
CONF_REPORT = "report"
CONF_CRITICAL_REPORT = "critical_report"
CONF_SUMMARY = "summary"
CONF_TITLE = "title"
CONF_MESSAGE = "message"
CONF_FAULT_ENTITIES = "fault_entities"
CONF_ACKNOWLEDGEMENT_NEEDED = "acknowledgement_needed"
CONF_VERSION = "version"
# Generated per-fault entity configs (filled in by validation, not by users).
CONF_AUTO_STATUS = "_auto_status_sensor"
CONF_AUTO_MESSAGE = "_auto_message_sensor"

# Reported by the optional `version` text sensor so consumers (e.g. Home
# Assistant) can tell which contract a device implements.
COMPONENT_VERSION = "1.1.0"

SEVERITIES = {
    "warning": Severity.WARNING,
    "critical": Severity.CRITICAL,
}
UNKNOWN_POLICIES = {
    "hold": UnknownPolicy.HOLD,
    "raise": UnknownPolicy.RAISE,
    "clear": UnknownPolicy.CLEAR,
}

# Home Assistant rejects states longer than 255 characters.
MAX_REPORT_LEN = 255
MAX_FAULTS = 32  # latched-fault persistence uses a 32-bit mask
CODE_RE = re.compile(r"^[a-z][a-z0-9_]{0,47}$")
RESERVED_CODES = {"ok", "unknown", "unavailable", "none", "warning", "critical"}
MAX_DESCRIPTION_LEN = 200
MAX_TITLE_LEN = 60
# The message is published as an entity state; Home Assistant allows 255.
MAX_MESSAGE_LEN = 255


def validate_code(value):
    value = cv.string_strict(value)
    if not CODE_RE.match(value):
        raise cv.Invalid(
            "Fault codes must start with a lowercase letter and contain only "
            "lowercase letters, digits and underscores (max 48 characters)"
        )
    if value in RESERVED_CODES:
        raise cv.Invalid(f"'{value}' is reserved and can't be used as a fault code")
    return value


def validate_description(value):
    value = cv.string_strict(value)
    if not value.strip():
        raise cv.Invalid("description must not be empty")
    if len(value) > MAX_DESCRIPTION_LEN:
        raise cv.Invalid(f"description must be at most {MAX_DESCRIPTION_LEN} characters")
    return value


def validate_title(value):
    value = cv.string_strict(value)
    if not value.strip():
        raise cv.Invalid("title must not be empty")
    if len(value) > MAX_TITLE_LEN:
        raise cv.Invalid(f"title must be at most {MAX_TITLE_LEN} characters")
    return value


def validate_message(value):
    value = cv.string_strict(value)
    if not value.strip():
        raise cv.Invalid("message must not be empty")
    if len(value) > MAX_MESSAGE_LEN:
        raise cv.Invalid(
            f"message must be at most {MAX_MESSAGE_LEN} characters "
            "(Home Assistant's limit for an entity state)"
        )
    return value


FAULT_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(Fault),
        cv.Required(CONF_CODE): validate_code,
        cv.Required(CONF_DESCRIPTION): validate_description,
        # User-facing wording, published by the per-fault entities.
        cv.Optional(CONF_TITLE): validate_title,
        cv.Optional(CONF_MESSAGE): validate_message,
        cv.Required(CONF_SEVERITY): cv.enum(SEVERITIES, lower=True),
        cv.Required(CONF_CONDITION): cv.returning_lambda,
        cv.Optional(CONF_CLEAR_CONDITION): cv.returning_lambda,
        cv.Optional(CONF_RAISE_AFTER, default="0s"): cv.positive_time_period_milliseconds,
        cv.Optional(CONF_CLEAR_AFTER, default="0s"): cv.positive_time_period_milliseconds,
        cv.Optional(CONF_STARTUP_DELAY, default="0s"): cv.positive_time_period_milliseconds,
        cv.Optional(CONF_UNKNOWN, default="hold"): cv.enum(UNKNOWN_POLICIES, lower=True),
        cv.Optional(CONF_LATCH, default=False): cv.boolean,
        cv.Optional(CONF_ON_RAISE): automation.validate_automation(
            {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(FaultTrigger)}
        ),
        cv.Optional(CONF_ON_CLEAR): automation.validate_automation(
            {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(FaultTrigger)}
        ),
        cv.Optional(CONF_WHILE_ACTIVE): automation.validate_automation(
            {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(FaultTrigger)}
        ),
        cv.Optional(CONF_BINARY_SENSOR): binary_sensor.binary_sensor_schema(
            binary_sensor.BinarySensor,
            device_class=DEVICE_CLASS_PROBLEM,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
    }
)


def validate_fault_list(faults):
    if not faults:
        raise cv.Invalid("At least one fault is required")
    if len(faults) > MAX_FAULTS:
        raise cv.Invalid(f"At most {MAX_FAULTS} faults are supported")
    seen = {}
    for i, f in enumerate(faults):
        code = f[CONF_CODE]
        if code in seen:
            raise cv.Invalid(
                f"Duplicate fault code '{code}' (also used by fault #{seen[code] + 1})",
                path=[i, CONF_CODE],
            )
        seen[code] = i
    # Worst case: every fault active at once. The report must still fit.
    worst = sum(len(f[CONF_CODE]) for f in faults) + len(faults) - 1
    if worst > MAX_REPORT_LEN:
        raise cv.Invalid(
            f"With every fault active the report would be {worst} characters; "
            f"Home Assistant allows {MAX_REPORT_LEN}. Shorten the fault codes."
        )
    return faults


def _text_sensor(icon):
    return text_sensor.text_sensor_schema(
        text_sensor.TextSensor,
        entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        icon=icon,
    )


def _add_fault_entities(config):
    """With fault_entities: true, give every fault two generated entities:

    - a binary sensor named after the fault's title, on while the fault is
      active, whose device_class carries the severity (critical -> safety,
      warning -> problem);
    - a text sensor with the same name whose state is the fault's message.

    Both share the name (so Home Assistant gives them the same object id in
    the binary_sensor and sensor domains) and are diagnostic entities.
    """
    if not config[CONF_FAULT_ENTITIES]:
        for i, f in enumerate(config[CONF_FAULTS]):
            for key in (CONF_TITLE, CONF_MESSAGE):
                if key in f:
                    raise cv.Invalid(
                        f"'{key}' is only used with fault_entities: true",
                        path=[CONF_FAULTS, i, key],
                    )
        return config
    titles = {}
    for i, f in enumerate(config[CONF_FAULTS]):
        for key in (CONF_TITLE, CONF_MESSAGE):
            if key not in f:
                raise cv.Invalid(
                    f"fault_entities: true requires a '{key}' on every fault",
                    path=[CONF_FAULTS, i],
                )
        if CONF_BINARY_SENSOR in f:
            raise cv.Invalid(
                "binary_sensor can't be combined with fault_entities: true "
                "(the fault's status entity is generated)",
                path=[CONF_FAULTS, i, CONF_BINARY_SENSOR],
            )
        key = f[CONF_TITLE].strip().lower()
        if key in titles:
            raise cv.Invalid(
                f"Duplicate fault title '{f[CONF_TITLE]}' (also used by fault #{titles[key] + 1})",
                path=[CONF_FAULTS, i, CONF_TITLE],
            )
        titles[key] = i
        # cv.enum yields the validated key (a str subclass); compare as text.
        critical = str(f[CONF_SEVERITY]) == "critical"
        f[CONF_AUTO_STATUS] = binary_sensor.binary_sensor_schema(
            binary_sensor.BinarySensor,
            device_class=DEVICE_CLASS_SAFETY if critical else DEVICE_CLASS_PROBLEM,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        )({CONF_NAME: f[CONF_TITLE]})
        f[CONF_AUTO_MESSAGE] = text_sensor.text_sensor_schema(
            text_sensor.TextSensor,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            icon="mdi:clipboard-text-outline",
        )({CONF_NAME: f[CONF_TITLE]})
    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(FaultMonitor),
            cv.Required(CONF_FAULTS): cv.All(cv.ensure_list(FAULT_SCHEMA), validate_fault_list),
            cv.Optional(CONF_FAULT_ENTITIES, default=False): cv.boolean,
            cv.Optional(CONF_REPORT): _text_sensor("mdi:alert-circle-outline"),
            cv.Optional(CONF_CRITICAL_REPORT): _text_sensor("mdi:alert-octagon-outline"),
            cv.Optional(CONF_SUMMARY): _text_sensor("mdi:shield-check-outline"),
            cv.Optional(CONF_VERSION): _text_sensor("mdi:tag-outline"),
            cv.Optional(CONF_ACKNOWLEDGEMENT_NEEDED): binary_sensor.binary_sensor_schema(
                binary_sensor.BinarySensor,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
                icon="mdi:check-circle-outline",
            ),
        }
    ).extend(cv.polling_component_schema("1s")),
    _add_fault_entities,
)


def _final_validate(config):
    interval = config.get("update_interval")
    if interval is None or not hasattr(interval, "total_milliseconds"):
        return config
    step = interval.total_milliseconds
    for f in config[CONF_FAULTS]:
        for key in (CONF_RAISE_AFTER, CONF_CLEAR_AFTER):
            ms = f[key].total_milliseconds
            if 0 < ms < step:
                raise cv.Invalid(
                    f"{key} ({ms} ms) of fault '{f[CONF_CODE]}' is shorter than the "
                    f"monitor's update_interval ({step} ms) and can't be honoured",
                )
    return config


FINAL_VALIDATE_SCHEMA = _final_validate

_OPTIONAL_BOOL = cg.optional.template(cg.bool_)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    for conf in config[CONF_FAULTS]:
        fault = cg.new_Pvariable(
            conf[CONF_ID],
            conf[CONF_CODE],
            conf[CONF_DESCRIPTION],
            conf[CONF_SEVERITY],
        )
        cg.add(
            fault.set_timing(
                conf[CONF_RAISE_AFTER].total_milliseconds,
                conf[CONF_CLEAR_AFTER].total_milliseconds,
                conf[CONF_STARTUP_DELAY].total_milliseconds,
                conf[CONF_LATCH],
                conf[CONF_UNKNOWN],
            )
        )
        cond = await cg.process_lambda(conf[CONF_CONDITION], [], return_type=_OPTIONAL_BOOL)
        cg.add(fault.set_condition(cond))
        if CONF_CLEAR_CONDITION in conf:
            clear = await cg.process_lambda(
                conf[CONF_CLEAR_CONDITION], [], return_type=_OPTIONAL_BOOL
            )
            cg.add(fault.set_clear_condition(clear))
        for key, adder in (
            (CONF_ON_RAISE, "add_on_raise_trigger"),
            (CONF_ON_CLEAR, "add_on_clear_trigger"),
            (CONF_WHILE_ACTIVE, "add_while_active_trigger"),
        ):
            for auto_conf in conf.get(key, []):
                trig = cg.new_Pvariable(auto_conf[CONF_TRIGGER_ID])
                cg.add(getattr(fault, adder)(trig))
                await automation.build_automation(trig, [], auto_conf)
        if CONF_BINARY_SENSOR in conf:
            bs = await binary_sensor.new_binary_sensor(conf[CONF_BINARY_SENSOR])
            cg.add(fault.set_binary_sensor(bs))
        if CONF_AUTO_STATUS in conf:
            bs = await binary_sensor.new_binary_sensor(conf[CONF_AUTO_STATUS])
            cg.add(fault.set_binary_sensor(bs))
            msg = await text_sensor.new_text_sensor(conf[CONF_AUTO_MESSAGE])
            cg.add(fault.set_message_sensor(msg))
        if CONF_TITLE in conf:
            cg.add(fault.set_title(conf[CONF_TITLE]))
        if CONF_MESSAGE in conf:
            cg.add(fault.set_message(conf[CONF_MESSAGE]))
        cg.add(var.add_fault(fault))

    for key, setter in (
        (CONF_REPORT, "set_report_sensor"),
        (CONF_CRITICAL_REPORT, "set_critical_report_sensor"),
        (CONF_SUMMARY, "set_summary_sensor"),
        (CONF_VERSION, "set_version_sensor"),
    ):
        if key in config:
            sens = await text_sensor.new_text_sensor(config[key])
            cg.add(getattr(var, setter)(sens))
    if CONF_ACKNOWLEDGEMENT_NEEDED in config:
        bs = await binary_sensor.new_binary_sensor(config[CONF_ACKNOWLEDGEMENT_NEEDED])
        cg.add(var.set_acknowledgement_needed_sensor(bs))
    cg.add(var.set_version(COMPONENT_VERSION))


# ---- Conditions -------------------------------------------------------------


@automation.register_condition(
    "fault_monitor.is_active",
    FaultIsActiveCondition,
    automation.maybe_simple_id({cv.Required(CONF_ID): cv.use_id(Fault)}),
)
async def is_active_to_code(config, condition_id, template_arg, args):
    fault = await cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(condition_id, template_arg, fault)


@automation.register_condition(
    "fault_monitor.any_active",
    AnyActiveCondition,
    automation.maybe_simple_id(
        {
            cv.Required(CONF_ID): cv.use_id(FaultMonitor),
            cv.Optional(CONF_SEVERITY): cv.enum(SEVERITIES, lower=True),
        }
    ),
)
async def any_active_to_code(config, condition_id, template_arg, args):
    parent = await cg.get_variable(config[CONF_ID])
    var = cg.new_Pvariable(condition_id, template_arg, parent)
    if CONF_SEVERITY in config:
        cg.add(var.set_severity(config[CONF_SEVERITY]))
    return var


# ---- Actions ----------------------------------------------------------------


@automation.register_action(
    "fault_monitor.evaluate",
    EvaluateAction,
    automation.maybe_simple_id({cv.Required(CONF_ID): cv.use_id(FaultMonitor)}),
    synchronous=True,
)
async def evaluate_to_code(config, action_id, template_arg, args):
    parent = await cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(action_id, template_arg, parent)


@automation.register_action(
    "fault_monitor.acknowledge",
    AcknowledgeAction,
    automation.maybe_simple_id(
        {
            cv.Required(CONF_ID): cv.use_id(FaultMonitor),
            cv.Optional(CONF_CODE): cv.templatable(cv.string),
        }
    ),
    synchronous=True,
)
async def acknowledge_to_code(config, action_id, template_arg, args):
    parent = await cg.get_variable(config[CONF_ID])
    var = cg.new_Pvariable(action_id, template_arg, parent)
    if CONF_CODE in config:
        code = await cg.templatable(config[CONF_CODE], args, cg.std_string)
        cg.add(var.set_code(code))
    return var
