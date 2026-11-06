# esphome-fault-monitor

`fault_monitor` is an ESPHome external component for declaring a device's
fault conditions in one place and reporting them reliably. It was built for
safety-relevant controllers (an animal-enclosure thermostat), where:

- the **device** decides what is wrong, because only it knows the cause;
- every fault is **time-qualified**, so blips never become faults;
- "I can't tell" is never silently treated as "everything is fine";
- **safety responses are state-based**, so a later action can't undo them;
- the **report** to Home Assistant is a small, stable contract: fault codes.

```yaml
external_components:
  - source: github://onetxpanda/esphome-fault-monitor@v1.1.0
    components: [fault_monitor]
```

Pin a tag. A push to the repository must never change running firmware unexpectedly.

## Example

```yaml
fault_monitor:
  id: faults
  update_interval: 1s
  report:            # "OK" or active codes, e.g. "enclosure_overheat,switchbot_offline"
    name: "Faults"
  critical_report:   # same, critical faults only
    name: "Critical Faults"
  summary:           # "ok" | "warning" | "critical"
    name: "Fault Summary"
  faults:
    - id: f_overheat
      code: enclosure_overheat
      description: "Enclosure at or above 95F; heater cut off until below 90F"
      severity: critical
      condition: |-
        float t = id(hottest_c);
        if (std::isnan(t)) return {};          // unknown
        return t >= 35.0f;
      clear_condition: |-
        float t = id(hottest_c);
        if (std::isnan(t)) return {};
        return t < 32.2f;                      // hysteresis
      while_active:
        - if:
            condition:
              switch.is_on: heater
            then:
              - switch.turn_off: heater
      binary_sensor:
        name: "Enclosure Overheat"

climate:
  - platform: thermostat
    heat_action:
      - if:
          condition:
            not:
              fault_monitor.is_active: f_overheat   # interlock
          then:
            - switch.turn_on: heater
```

## Fault options

| Option | Default | Meaning |
|---|---|---|
| `code` | required | Stable identifier reported to HA: `^[a-z][a-z0-9_]{0,47}$`, unique, not `ok`/`unknown`/`unavailable`/`none`/`warning`/`critical`. |
| `description` | required | Human-readable cause, used in logs and on-device displays (max 200 characters). |
| `severity` | required | `warning` or `critical`. |
| `condition` | required | Lambda returning `std::optional<bool>`: `true` = fault present, `false` = absent, `{}` = unknown. |
| `clear_condition` | `!condition` | Lambda: `true` = the fault has gone. Use it for hysteresis. Only consulted while the fault is active. |
| `raise_after` | `0s` | The condition must hold continuously this long before the fault is raised. |
| `clear_after` | `0s` | The clear condition must hold continuously this long before the fault clears. |
| `startup_delay` | `0s` | Grace period after boot before this fault is evaluated at all. |
| `unknown` | `hold` | What an unknown result means; see below. |
| `latch` | `false` | After the condition clears, keep the fault active until acknowledged. Latched faults survive reboots. |
| `on_raise` | | Automation, run once when the fault is raised. |
| `on_clear` | | Automation, run once when the fault clears (or is acknowledged). |
| `while_active` | | Automation, run on every evaluation while the fault is active. |
| `title` | | User-facing name of the fault (max 60 characters, unique). Required with `fault_entities: true`. |
| `message` | | User-facing explanation and what to check (max 255 characters). Required with `fault_entities: true`. |
| `binary_sensor` | | Optional hand-configured per-fault binary sensor (`device_class: problem`). Not allowed with `fault_entities: true`. |

### Monitor options

| Option | Default | Meaning |
|---|---|---|
| `fault_entities` | `false` | Generate the per-fault entities described under **Entity contract**. |
| `report` / `critical_report` / `summary` | | Compact text reports, described under **Report contract**. |
| `acknowledgement_needed` | | Binary sensor, on while any fault is latched. Use it to show an acknowledge button. |
| `version` | | Text sensor with the component version (`1.1.0`), so a consumer can tell which contract a device implements. |

The `update_interval` (default `1s`) is how often every fault is evaluated. A
non-zero `raise_after` or `clear_after` shorter than it is rejected.

## Semantics

### States

```
          condition true            raise_after elapsed
 CLEAR ─────────────────► PENDING ─────────────────────► ACTIVE ◄──────────┐
   ▲   ◄───────────────── RAISE                           │  ▲             │
   │    condition false / unknown(hold)     clear true    │  │ clear false │
   │                                                      ▼  │  / unknown  │
   │            clear_after elapsed (not latching)    PENDING CLEAR        │
   └───────────────────────────────────────────────────── │              │
                                                          │ latching     │
                                                          ▼              │
                                                       LATCHED ──────────┘
                                                (acknowledge → CLEAR;
                                                 condition returns → ACTIVE)
```

- A fault is **active**, meaning reported, in ACTIVE, PENDING CLEAR and LATCHED.
  Pending raises are not reported.
- RAISED is reported once per occurrence. A latched fault whose condition
  returns becomes ACTIVE again without a second RAISED, because it was never
  reported as cleared.

### Unknown results

A condition returns `{}` when it can't judge, for example because a sensor has
no reading. Each fault chooses:

- `hold` (default): no evidence either way. A settled state is kept and any
  pending transition is cancelled, so raising and clearing both need
  continuous evidence for the full `raise_after`/`clear_after`.
- `raise`: fail-safe. Unknown counts as the fault being present, and never
  clears it.
- `clear`: unknown counts as the fault being absent.

### Latching and acknowledgement

- `fault_monitor.acknowledge` (all faults, or one `code`) clears a LATCHED fault.
- Acknowledging a fault that is still present pre-acknowledges it: when it goes
  away, it clears instead of latching. A new occurrence resets this.
- Latched and active-latching faults are stored in flash, keyed by a hash of
  the declared fault list, and restored as LATCHED on boot, before any startup
  delay. A fault that causes a reboot is therefore still reported after it.
  If the fault list changes, nothing is restored.

### Evaluation order (each update, or `fault_monitor.evaluate`)

1. Every fault's conditions are evaluated and its state machine advanced, in
   declaration order.
2. Latching state is persisted (and synced) **before** any action runs, since an
   `on_raise` may reboot the device.
3. Reports and binary sensors are published, so the new state is visible before
   actions run.
4. `on_raise` / `on_clear` run, in declaration order.
5. `while_active` runs for every active fault.

Calling `fault_monitor.evaluate` from inside these automations is safe: it
schedules one more pass, bounded to 3 passes.

### Report contract

- `report`: active codes, critical first then warnings, each group in
  declaration order, joined by `,`; or `OK`. Configs whose worst case (all
  faults active) exceeds Home Assistant's 255-character limit are rejected.
- `critical_report`: critical codes only, or `OK`.
- `summary`: `ok`, `warning` or `critical`.
- Reports are published at boot (restored state), on every change, and are
  never stale.

## Entity contract (`fault_entities: true`)

ESPHome entities carry a single typed value and no attributes, so each fault
is published as a pair of entities that share the fault's `title` as their name:

| Entity | State | Carries |
|---|---|---|
| `binary_sensor` named *title* | `on` while the fault is active (including latched) | Severity as `device_class`: `safety` for critical, `problem` for warning |
| `text_sensor` named *title* | The fault's `message`, published at boot | The user-facing wording |

Both are diagnostic entities. Because they share a name, Home Assistant gives
them the same object id in the `binary_sensor` and `sensor` domains, so a
consumer can pair them without any per-fault configuration: for each fault
binary sensor on the device, its message is `sensor.<same object id>`.
Adding a fault to the firmware is enough for it to appear in Home Assistant
with its severity and wording.

## Conditions and actions

```yaml
fault_monitor.is_active: f_overheat          # a fault (by id) is active
fault_monitor.any_active:                     # any fault, optionally of one severity
  id: faults
  severity: critical
fault_monitor.evaluate: faults                # evaluate now (e.g. from a sensor's on_value)
fault_monitor.acknowledge:                    # acknowledge all, or one code
  id: faults
  code: power_meter_fault
```

From lambdas: `id(faults).get_faults()`, `Fault::is_active()`,
`get_code()`, `get_description()`, `get_severity()`, `is_latched()`;
`id(faults).build_report()`.

## Design guidance for safety use

- **Interlocks must be state-based.** Gate the dangerous action itself, with
  `fault_monitor.is_active` in `heat_action`, and re-assert with `while_active`.
  Never rely on `on_raise` alone: a later automation can undo a one-shot action.
- Prefer conditions that read the same values the control loop uses, so the
  fault describes what the controller actually sees.
- Use `unknown: raise` when silence itself is the hazard (e.g. "no
  temperature"), and `hold` when another fault already covers missing data.
- Keep the monitor's own logic simple. It is part of the safety system.

## Tests

```
tests/run_tests.py --esphome /path/to/esphome
```

- `tests/unit`: the state machine (`fault_logic.h`, no ESPHome dependency),
  built with address and undefined-behaviour sanitizers.
- `tests/invalid`: configs that must be rejected, with their expected errors.
- `tests/host`: a `platform: host` firmware driven through a scripted scenario
  in three runs: killed mid-run with a latched fault, restarted (restore and
  acknowledge), and restarted again (acknowledgement persisted).
