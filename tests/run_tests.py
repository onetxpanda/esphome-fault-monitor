#!/usr/bin/env python3
"""Run the fault_monitor test suite.

1. Unit tests of the pure state machine (g++ with sanitizers).
2. Invalid configs: each must be rejected by `esphome config` with the
   expected message.
3. Host-platform integration test: a scripted scenario run in three phases
   (killed mid-run with a latched fault, restarted, restarted again) and
   checked step by step.

Usage:  tests/run_tests.py [--esphome PATH] [--skip-compile]
"""

import argparse
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
FAILURES = []


def check(cond, msg):
    print(("  ok   " if cond else "  FAIL ") + msg)
    if not cond:
        FAILURES.append(msg)


def unit_tests():
    print("== unit tests")
    r = subprocess.run(["make", "-s", "-C", os.path.join(HERE, "unit"), "test"], capture_output=True, text=True)
    print(r.stdout.strip().splitlines()[-1] if r.stdout.strip() else r.stderr)
    check(r.returncode == 0, "state machine unit tests pass")


def invalid_configs(esphome):
    print("== invalid configs are rejected")
    d = os.path.join(HERE, "invalid")
    with open(os.path.join(d, "expected.txt")) as f:
        cases = [line.strip().split("|", 1) for line in f if line.strip()]
    for name, expect in cases:
        r = subprocess.run([esphome, "config", name], cwd=d, capture_output=True, text=True)
        out = r.stdout + r.stderr
        check(r.returncode != 0 and expect in out, f"{name}: rejected with \"{expect}\"")


STATE_RE = re.compile(
    r"STATE step=(\d+) report=(\S+) critical=(\S+) summary=(\S+) heater=(\d) raises=(\d+)"
)
ANSI = re.compile(r"\x1b\[[0-9;]*m")


def run_phase(program, prefdir, seconds, sig):
    env = dict(os.environ, ESPHOME_PREFDIR=prefdir)
    p = subprocess.Popen([program], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env, text=True)
    time.sleep(seconds)
    p.send_signal(sig)
    out, _ = p.communicate(timeout=10)
    out = ANSI.sub("", out)
    states = {}
    for m in STATE_RE.finditer(out):
        states[int(m.group(1))] = {
            "report": m.group(2),
            "critical": m.group(3),
            "summary": m.group(4),
            "heater": int(m.group(5)),
            "raises": int(m.group(6)),
        }
    return out, states


def host_test(esphome, skip_compile):
    print("== host integration test")
    d = os.path.join(HERE, "host")
    yaml = "fault_monitor_host.yaml"
    if not skip_compile:
        r = subprocess.run([esphome, "compile", yaml], cwd=d, capture_output=True, text=True)
        check(r.returncode == 0, "host firmware compiles")
        if r.returncode != 0:
            print(r.stdout[-3000:], r.stderr[-3000:])
            return
    program = os.path.join(d, ".esphome/build/fault-monitor-test/.pioenvs/fault-monitor-test/program")
    prefdir = tempfile.mkdtemp(prefix="fault_monitor_prefs_")
    try:
        # Phase 1: run until the latched fault exists, then kill (power loss).
        out, s = run_phase(program, prefdir, 31.5, signal.SIGKILL)
        g = lambda n, k: s.get(n, {}).get(k)  # noqa: E731
        check(g(4, "report") == "enclosure_overheat", "overheat raised immediately")
        check(g(4, "critical") == "enclosure_overheat" and g(4, "summary") == "critical", "critical report and summary")
        check(g(4, "heater") == 0 and g(4, "raises") == 1, "on_raise ran once; interlock forced heater off")
        check(g(6, "heater") == 0, "while_active re-asserted the interlock after the thermostat turned the heater on")
        check(g(8, "report") == "enclosure_overheat", "still active inside the hysteresis band")
        check(g(10, "report") == "OK", "cleared below the clear threshold")
        check(all(g(n, "report") == "OK" for n in (13, 14, 15)), "unknown=hold cancelled the pending raise")
        check(g(17, "report") == "enclosure_too_cold", "raise_after restarted after the unknown reading")
        check(g(21, "report") == "enclosure_too_cold" and g(23, "report") == "OK", "clear_after honoured")
        check(g(25, "report") == "OK" and g(27, "report") == "no_temperature", "unknown=raise raised after raise_after")
        check(g(29, "report") == "power_meter_fault", "latching fault raised")
        check(g(31, "report") == "power_meter_fault" and "LATCHED until acknowledged" in out, "fault latched after its condition cleared")
        check(g(29, "critical") == "OK" and g(29, "summary") == "warning", "warning does not appear in the critical report")

        # Phase 2: restart. The latched fault must be restored and reported.
        out, s = run_phase(program, prefdir, 37, signal.SIGINT)
        g = lambda n, k: s.get(n, {}).get(k)  # noqa: E731
        check("restored LATCHED from before reboot" in out, "latched fault restored after restart")
        check(g(1, "report") == "power_meter_fault", "restored fault reported from the first state")
        check(g(4, "report") == "enclosure_overheat,power_meter_fault", "report lists critical first, then warnings")
        check("[power_meter_fault] acknowledged" in out, "acknowledge action cleared the latched fault")
        check(g(34, "report") == "OK", "report OK after acknowledgement")

        # Phase 3: restart again. The acknowledgement must have been persisted.
        out, s = run_phase(program, prefdir, 4, signal.SIGINT)
        check("restored LATCHED" not in out and s.get(1, {}).get("report") == "OK", "acknowledgement persisted across restart")
    finally:
        shutil.rmtree(prefdir, ignore_errors=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--esphome", default=shutil.which("esphome") or "esphome")
    ap.add_argument("--skip-compile", action="store_true")
    a = ap.parse_args()
    unit_tests()
    invalid_configs(a.esphome)
    host_test(a.esphome, a.skip_compile)
    print(f"\n{'FAILED' if FAILURES else 'PASSED'}: {len(FAILURES)} failure(s)")
    for f in FAILURES:
        print("  - " + f)
    sys.exit(1 if FAILURES else 0)


if __name__ == "__main__":
    main()
