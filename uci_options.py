#!/usr/bin/env python3
"""Inspect and validate the UCI options advertised by an engine."""

import argparse
import re
import subprocess
import sys


OPTION_RE = re.compile(
    r"^option name (?P<name>.+?) type (?P<type>check|spin|string|combo|button)"
    r"(?: default (?P<default>.*?))?(?: min (?P<min>-?\d+))?(?: max (?P<max>-?\d+))?$"
)


def query(engine_path):
    proc = subprocess.run(
        [engine_path], input="uci\nquit\n", text=True, capture_output=True, timeout=10
    )
    if proc.returncode:
        raise RuntimeError(proc.stderr.strip() or f"engine exited with {proc.returncode}")
    options = {}
    for line in proc.stdout.splitlines():
        match = OPTION_RE.match(line.strip())
        if match:
            data = match.groupdict()
            options[data.pop("name")] = data
    if "uciok" not in proc.stdout:
        raise RuntimeError("engine did not complete the UCI handshake")
    return options


def validate(options, assignment):
    if "=" not in assignment:
        return "expected NAME=VALUE"
    name, value = assignment.split("=", 1)
    if name not in options:
        return f"unknown UCI option: {name}"
    option = options[name]
    if option["type"] == "check" and value.lower() not in {"true", "false"}:
        return f"{name} expects true or false"
    if option["type"] == "spin":
        try:
            number = int(value)
        except ValueError:
            return f"{name} expects an integer"
        minimum, maximum = int(option["min"]), int(option["max"])
        if not minimum <= number <= maximum:
            return f"{name} must be between {minimum} and {maximum}"
    return None


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("engine")
    parser.add_argument("assignments", nargs="*")
    args = parser.parse_args()
    try:
        options = query(args.engine)
    except (OSError, subprocess.TimeoutExpired, RuntimeError) as exc:
        parser.error(str(exc))

    if not args.assignments:
        for name, option in options.items():
            details = f"default={option['default']}"
            if option["type"] == "spin":
                details += f" range={option['min']}..{option['max']}"
            print(f"{name} ({option['type']}, {details})")
        return

    errors = [error for item in args.assignments if (error := validate(options, item))]
    if errors:
        print("\n".join(f"Fehler: {error}" for error in errors), file=sys.stderr)
        raise SystemExit(2)


if __name__ == "__main__":
    main()
