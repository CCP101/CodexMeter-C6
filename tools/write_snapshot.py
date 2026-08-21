#!/usr/bin/env python3
"""Write one Codex usage snapshot to the ESP32 over USB CDC serial.

The Codex source is local app-server data. The script never prints or sends
OAuth tokens, cookies, or account credentials; the device receives only the
small v1 display snapshot.
"""

from __future__ import annotations

import argparse
import json
import os
import queue
import random
import shutil
import subprocess
import sys
import threading
import time
from pathlib import Path
from typing import Any


DEFAULT_EXPORTER_SCRIPT = os.environ.get("CODEX_USAGE_EXPORTER_SCRIPT", "")
DEFAULT_REFRESH_SECONDS = 2 * 60 * 60
MINIMUM_REFRESH_SECONDS = 5 * 60


def fail(message: str, code: int = 1) -> int:
    print(f"ERROR: {message}", file=sys.stderr)
    return code


def as_number(value: Any) -> float | None:
    try:
        number = float(value)
    except (TypeError, ValueError):
        return None
    return number if number == number else None


def resolve_codex_command(explicit: str | None) -> str | None:
    candidates = [explicit]
    if not explicit:
        candidates.extend(
            [
                shutil.which("codex.cmd"),
                shutil.which("codex"),
            ]
        )
    for candidate in candidates:
        if candidate and Path(candidate).exists():
            return candidate
    return None


class AppServerClient:
    def __init__(self, command: str):
        self.command = command
        self.process: subprocess.Popen[str] | None = None
        self.messages: queue.Queue[dict[str, Any]] = queue.Queue()
        self.next_id = 1
        self.reader_thread: threading.Thread | None = None

    def start(self) -> None:
        suffix = Path(self.command).suffix.lower()
        runtime_dir = Path(self.command).parent
        bundled_node = runtime_dir / "node.exe"
        bundled_script = runtime_dir / "node_modules" / "@openai" / "codex" / "bin" / "codex.js"
        if suffix in {".cmd", ".bat", ".ps1"} and bundled_node.exists() and bundled_script.exists():
            # Avoid the Windows .cmd/.ps1 wrapper layer. It is suitable for an
            # interactive terminal, but direct stdio JSON-RPC is more reliable
            # when node and the Codex entry script are spawned together.
            argv = [str(bundled_node), str(bundled_script), "app-server", "--stdio"]
        elif suffix in {".cmd", ".bat"}:
            command_line = f'"{self.command}" app-server --stdio'
            argv = [os.environ.get("ComSpec", "cmd.exe"), "/d", "/s", "/c", command_line]
        elif suffix == ".ps1":
            argv = [
                "powershell",
                "-NoProfile",
                "-ExecutionPolicy",
                "Bypass",
                "-File",
                self.command,
                "app-server",
                "--listen",
                "stdio://",
            ]
        else:
            argv = [self.command, "app-server", "--listen", "stdio://"]

        creationflags = getattr(subprocess, "CREATE_NO_WINDOW", 0)
        self.process = subprocess.Popen(
            argv,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            text=True,
            encoding="utf-8",
            errors="replace",
            bufsize=1,
            creationflags=creationflags,
        )

        def read_lines() -> None:
            assert self.process is not None
            assert self.process.stdout is not None
            for line in self.process.stdout:
                try:
                    message = json.loads(line)
                except json.JSONDecodeError:
                    continue
                if isinstance(message, dict):
                    self.messages.put(message)

        self.reader_thread = threading.Thread(target=read_lines, daemon=True)
        self.reader_thread.start()
        self.request(
            "initialize",
            {
                "clientInfo": {
                    "name": "codex_meter_c6_host",
                    "title": "CodexMeter C6 COM bridge",
                    "version": "0.1.0",
                },
                "capabilities": {},
            },
        )
        self.notify("initialized", {})

    def request(self, method: str, params: dict[str, Any] | None = None) -> dict[str, Any]:
        if not self.process or not self.process.stdin:
            raise RuntimeError("Codex app-server is not running")
        request_id = self.next_id
        self.next_id += 1
        message: dict[str, Any] = {"method": method, "id": request_id}
        if params is not None:
            message["params"] = params
        self.process.stdin.write(json.dumps(message, separators=(",", ":")) + "\n")
        self.process.stdin.flush()

        deadline = time.monotonic() + 20
        while time.monotonic() < deadline:
            try:
                response = self.messages.get(timeout=0.25)
            except queue.Empty:
                continue
            if response.get("id") != request_id:
                continue
            if response.get("error"):
                raise RuntimeError("Codex app-server rejected the request")
            result = response.get("result")
            if not isinstance(result, dict):
                raise RuntimeError("Codex app-server returned an unexpected response")
            return result
        raise TimeoutError(f"Codex app-server request timed out: {method}")

    def notify(self, method: str, params: dict[str, Any] | None = None) -> None:
        if not self.process or not self.process.stdin:
            raise RuntimeError("Codex app-server is not running")
        message: dict[str, Any] = {"method": method}
        if params is not None:
            message["params"] = params
        self.process.stdin.write(json.dumps(message, separators=(",", ":")) + "\n")
        self.process.stdin.flush()

    def stop(self) -> None:
        if not self.process:
            return
        if self.process.stdin:
            self.process.stdin.close()
        try:
            self.process.wait(timeout=2)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait(timeout=2)
        self.process = None


def snapshot_from_client(
    client: AppServerClient, *, next_poll_in: int = DEFAULT_REFRESH_SECONDS
) -> dict[str, Any]:
    account_result = client.request("account/read", {"refreshToken": False})
    rate_result = client.request("account/rateLimits/read")
    model_label = "MODEL UNKNOWN"
    try:
        config_result = client.request(
            "config/read", {"includeLayers": False, "cwd": str(Path.cwd())}
        )
        config = config_result.get("config", config_result)
        if isinstance(config, dict) and config.get("model"):
            model_label = str(config["model"]).upper()[:31]
    except (RuntimeError, TimeoutError):
        pass
    account = account_result.get("account", account_result)
    if not isinstance(account, dict):
        account = {}

    rate_limits = rate_result.get("rateLimits")
    buckets = rate_result.get("rateLimitsByLimitId")
    if not isinstance(buckets, dict):
        buckets = {}
    if not isinstance(rate_limits, dict):
        rate_limits = buckets.get("codex")
    if not isinstance(rate_limits, dict) and buckets:
        rate_limits = next(
            (value for value in buckets.values() if isinstance(value, dict)),
            None,
        )
    if not isinstance(rate_limits, dict):
        raise RuntimeError("Codex returned no usable rate-limit snapshot")

    windows = [rate_limits.get("primary"), rate_limits.get("secondary")]
    primary = next(
        (
            window
            for window in windows
            if isinstance(window, dict)
            and round(as_number(window.get("windowDurationMins")) or 0) == 7 * 24 * 60
        ),
        None,
    )
    if not isinstance(primary, dict):
        raise RuntimeError("Codex returned no weekly quota window")

    used_value = as_number(primary.get("usedPercent"))
    remaining_value = as_number(primary.get("remainingPercent"))
    if remaining_value is None and used_value is not None:
        remaining_value = 100 - used_value
    if remaining_value is None:
        raise RuntimeError("Codex returned no remaining percentage")

    remaining = max(0, min(100, round(remaining_value)))
    used = 100 - remaining
    reset_at = as_number(primary.get("resetsAt"))
    resets_in = max(0, round(reset_at - time.time())) if reset_at is not None else -1
    plan = str(account.get("planType") or rate_limits.get("planType") or "CODEX")
    plan_label = str(account.get("planLabel") or plan).upper()[:31]
    window_mins_value = as_number(primary.get("windowDurationMins"))
    window_mins = round(window_mins_value) if window_mins_value is not None else 0

    return build_payload(
        remaining=remaining,
        used=used,
        window_mins=window_mins,
        resets_in=resets_in,
        plan=plan,
        plan_label=plan_label,
        model_label=model_label,
        source="CODEX APP-SERVER",
        next_poll_in=next_poll_in,
    )


def snapshot_from_codex(
    command: str, *, next_poll_in: int = DEFAULT_REFRESH_SECONDS
) -> dict[str, Any]:
    client = AppServerClient(command)
    try:
        client.start()
        return snapshot_from_client(client, next_poll_in=next_poll_in)
    finally:
        client.stop()


def payload_from_exporter_state(
    state: dict[str, Any], *, next_poll_in: int = DEFAULT_REFRESH_SECONDS
) -> dict[str, Any]:
    weekly_prefix: str | None = None
    for prefix in ("codex_weekly", "codex_5h"):
        window = as_number(state.get(f"{prefix}_window_mins"))
        used = as_number(state.get(f"{prefix}_used_percent"))
        remaining = as_number(state.get(f"{prefix}_remaining_percent"))
        if round(window or 0) == 7 * 24 * 60 and (used is not None or remaining is not None):
            weekly_prefix = prefix
            break
    if weekly_prefix is None:
        raise RuntimeError("MQTT exporter returned no 7-day quota window")

    remaining_value = as_number(state.get(f"{weekly_prefix}_remaining_percent"))
    used_value = as_number(state.get(f"{weekly_prefix}_used_percent"))
    if remaining_value is None and used_value is not None:
        remaining_value = 100 - used_value
    if remaining_value is None:
        raise RuntimeError("MQTT exporter returned no weekly remaining percentage")

    remaining = max(0, min(100, round(remaining_value)))
    used = max(0, min(100, round(used_value))) if used_value is not None else 100 - remaining
    reset_at = as_number(state.get(f"{weekly_prefix}_reset_at_epoch"))
    resets_in = max(0, round(reset_at - time.time())) if reset_at is not None else -1
    window_value = as_number(state.get(f"{weekly_prefix}_window_mins"))
    window_mins = round(window_value) if window_value is not None else 7 * 24 * 60
    plan = str(state.get("codex_plan_type") or "CODEX")
    model_label = str(state.get("codex_model") or "MODEL UNKNOWN").upper()[:31]

    return build_payload(
        remaining=remaining,
        used=used,
        window_mins=window_mins,
        resets_in=resets_in,
        plan=plan,
        plan_label=plan.upper(),
        model_label=model_label,
        source="MQTT EXPORTER",
        next_poll_in=next_poll_in,
    )


def snapshot_from_exporter(
    script: str | Path, *, next_poll_in: int = DEFAULT_REFRESH_SECONDS
) -> dict[str, Any]:
    script_path = Path(script)
    if not script_path.is_file():
        raise RuntimeError(f"MQTT exporter script not found: {script_path}")

    powershell = shutil.which("pwsh") or shutil.which("powershell")
    if not powershell:
        raise RuntimeError("PowerShell is unavailable")

    creationflags = getattr(subprocess, "CREATE_NO_WINDOW", 0)
    try:
        result = subprocess.run(
            [
                powershell,
                "-NoProfile",
                "-ExecutionPolicy",
                "Bypass",
                "-File",
                str(script_path),
                "-DryRun",
            ],
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=90,
            check=False,
            creationflags=creationflags,
        )
    except subprocess.TimeoutExpired as error:
        raise TimeoutError("MQTT exporter dry-run timed out") from error

    if result.returncode != 0:
        raise RuntimeError("MQTT exporter dry-run failed")

    for line in reversed(result.stdout.splitlines()):
        try:
            state = json.loads(line)
        except json.JSONDecodeError:
            continue
        if isinstance(state, dict):
            return payload_from_exporter_state(state, next_poll_in=next_poll_in)
    raise RuntimeError("MQTT exporter dry-run returned no JSON object")


def build_payload(
    *,
    remaining: int,
    used: int | None,
    window_mins: int,
    resets_in: int,
    plan: str,
    plan_label: str,
    model_label: str = "MODEL UNKNOWN",
    source: str = "USB CDC",
    next_poll_in: int = DEFAULT_REFRESH_SECONDS,
) -> dict[str, Any]:
    remaining = max(0, min(100, int(remaining)))
    used = 100 - remaining if used is None else max(0, min(100, int(used)))
    return {
        "v": 1,
        "status": "ok",
        "source": source[:19],
        "capturedAt": int(time.time()),
        "nextPollIn": max(MINIMUM_REFRESH_SECONDS, int(next_poll_in)),
        "plan": plan[:31],
        "planLabel": plan_label[:31],
        "modelLabel": model_label[:31],
        "preferred": {
            "primary": {
                "used": used,
                "remaining": remaining,
                "windowMins": max(0, int(window_mins)),
                "resetsIn": int(resets_in),
            }
        },
        "extras": [],
    }


def write_payload_to_connection(
    connection: Any, payload: dict[str, Any], timeout: float
) -> None:
    encoded = json.dumps(payload, separators=(",", ":"), ensure_ascii=True).encode("ascii")
    if len(encoded) + 1 > 2016:
        raise RuntimeError("snapshot exceeds the reference device payload budget")

    connection.write(encoded + b"\n")
    connection.flush()

    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        raw = connection.readline()
        if not raw:
            continue
        line = raw.decode("utf-8", errors="replace").strip()
        if line.startswith("CODEX_ACK"):
            return
        if line.startswith("CODEX_NACK"):
            raise RuntimeError("ESP32 rejected the snapshot")
    raise TimeoutError("no CODEX_ACK received from the ESP32")


def open_serial(port: str) -> Any:
    try:
        import serial  # type: ignore
    except ImportError as error:
        raise RuntimeError("pyserial is unavailable; install it with: python -m pip install pyserial") from error

    connection = serial.Serial(
        port=port,
        baudrate=115200,
        timeout=0.2,
        write_timeout=2,
        dsrdtr=False,
        rtscts=False,
    )
    connection.dtr = False
    connection.rts = False
    time.sleep(1.5)
    connection.reset_input_buffer()
    return connection


def write_to_serial(port: str, payload: dict[str, Any], timeout: float) -> None:
    with open_serial(port) as connection:
        write_payload_to_connection(connection, payload, timeout)


def print_write_result(port: str, payload: dict[str, Any]) -> None:
    primary = payload["preferred"]["primary"]
    print(
        f"Wrote Codex snapshot to {port}: "
        f"remaining={primary['remaining']}% "
        f"window={primary['windowMins']}min"
    )


def watch_codex(
    *,
    command: str,
    port: str,
    interval: int,
    timeout: float,
) -> int:
    client = AppServerClient(command)
    failures = 0
    try:
        client.start()
        print(f"Watching Codex allowance; refresh interval={interval}s. Press Ctrl+C to stop.")
        while True:
            try:
                payload = snapshot_from_client(client, next_poll_in=interval)
                write_to_serial(port, payload, timeout)
                print_write_result(port, payload)
                failures = 0
                delay = interval + random.randint(0, 5)
            except (RuntimeError, TimeoutError, OSError) as error:
                failures += 1
                delay = min(interval * (2**min(failures, 4)), 15 * 60)
                print(f"WARN: refresh failed ({error}); retrying in {delay}s", file=sys.stderr)
                try:
                    client.stop()
                    client.start()
                except (RuntimeError, TimeoutError, OSError) as restart_error:
                    print(f"WARN: Codex app-server restart failed ({restart_error})", file=sys.stderr)

            time.sleep(delay)
    except KeyboardInterrupt:
        print("Stopped Codex allowance watcher.")
        return 0
    finally:
        client.stop()


def watch_exporter(
    *,
    script: str | Path,
    port: str,
    interval: int,
    timeout: float,
) -> int:
    failures = 0
    print(f"Watching weekly Codex allowance; refresh interval={interval}s. Press Ctrl+C to stop.")
    try:
        while True:
            try:
                payload = snapshot_from_exporter(script, next_poll_in=interval)
                write_to_serial(port, payload, timeout)
                print_write_result(port, payload)
                failures = 0
                delay = interval + random.randint(0, 5 * 60)
            except (RuntimeError, TimeoutError, OSError) as error:
                failures += 1
                delay = min(interval * (2**min(failures, 4)), 12 * 60 * 60)
                print(f"WARN: refresh failed ({error}); retrying in {delay}s", file=sys.stderr)
            time.sleep(delay)
    except KeyboardInterrupt:
        print("Stopped Codex allowance watcher.")
        return 0


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default="COM3")
    parser.add_argument(
        "--source",
        choices=("exporter", "codex", "manual"),
        default="exporter",
        help="data source; exporter reuses publish-codex-usage-mqtt.ps1 -DryRun",
    )
    parser.add_argument("--codex-executable")
    parser.add_argument("--exporter-script", default=str(DEFAULT_EXPORTER_SCRIPT))
    parser.add_argument("--remaining", type=int)
    parser.add_argument("--window-mins", type=int, default=300)
    parser.add_argument("--resets-in", type=int, default=3600)
    parser.add_argument("--plan", default="codex")
    parser.add_argument("--plan-label", default="CODEX")
    parser.add_argument("--model-label", default="MODEL UNKNOWN")
    parser.add_argument(
        "--watch",
        action="store_true",
        help="keep reading Codex and refreshing the device until Ctrl+C",
    )
    parser.add_argument(
        "--interval",
        type=int,
        default=DEFAULT_REFRESH_SECONDS,
        help="minimum Codex refresh interval in seconds (300-86400; default 7200)",
    )
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--timeout", type=float, default=8.0)
    return parser.parse_args()


def main() -> int:
    args = parse_args()

    try:
        if args.watch:
            if args.source == "manual" or args.remaining is not None:
                return fail("--watch requires --source exporter or --source codex", 2)
            if args.interval < MINIMUM_REFRESH_SECONDS or args.interval > 24 * 60 * 60:
                return fail("--interval must be between 300 and 86400 seconds", 2)
            if args.source == "exporter":
                return watch_exporter(
                    script=args.exporter_script,
                    port=args.port,
                    interval=args.interval,
                    timeout=args.timeout,
                )
            command = resolve_codex_command(args.codex_executable)
            if not command:
                return fail("could not locate codex.cmd for watch mode", 2)
            return watch_codex(
                command=command,
                port=args.port,
                interval=args.interval,
                timeout=args.timeout,
            )

        if args.source == "manual" or args.remaining is not None:
            if args.remaining is None:
                return fail("manual source requires --remaining", 2)
            payload = build_payload(
                remaining=args.remaining,
                used=None,
                window_mins=args.window_mins,
                resets_in=args.resets_in,
                plan=args.plan,
                plan_label=args.plan_label.upper(),
                model_label=args.model_label.upper(),
                source="MANUAL",
                next_poll_in=args.interval,
            )
        elif args.source == "exporter":
            payload = snapshot_from_exporter(
                args.exporter_script, next_poll_in=args.interval
            )
        else:
            command = resolve_codex_command(args.codex_executable)
            if not command:
                return fail(
                    "could not locate codex.cmd; use --remaining for a manual snapshot",
                    2,
                )
            payload = snapshot_from_codex(command, next_poll_in=args.interval)

        if args.dry_run:
            print(json.dumps(payload, separators=(",", ":")))
            return 0

        write_to_serial(args.port, payload, args.timeout)
        print_write_result(args.port, payload)
        return 0
    except (RuntimeError, TimeoutError, OSError) as error:
        return fail(str(error))


if __name__ == "__main__":
    raise SystemExit(main())
