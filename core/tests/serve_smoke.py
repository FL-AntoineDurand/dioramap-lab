#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Antoine Durand
"""End-to-end serve-mode smoke test.

Spawns `dioramap-processor serve`, connects via WS, sends a RUN for the
demo-chain pipeline, and asserts the expected protocol messages arrive.

Invoked from CTest: see v3/core/tests/CMakeLists.txt.
"""

import asyncio
import json
import os
import signal
import socket
import subprocess
import sys
import tempfile
import time

import websockets


BIN = os.environ["DIORAMAP_BIN"]
PIPELINES_DIR = os.environ["V3_PIPELINES_DIR"] + "/smoke"


def pick_port():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


async def wait_for_port(port, timeout=10.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.settimeout(0.5)
        try:
            s.connect(("127.0.0.1", port))
            s.close()
            return
        except OSError:
            s.close()
            await asyncio.sleep(0.1)
    raise RuntimeError(f"server didn't come up on :{port}")


async def run_client(port):
    uri = f"ws://127.0.0.1:{port}/"
    async with websockets.connect(uri, subprotocols=["dioramap-v1"]) as ws:
        # First message must be SESSION_STARTED.
        raw = await asyncio.wait_for(ws.recv(), timeout=5)
        session_msg = json.loads(raw)
        assert session_msg["t"] == "SESSION_STARTED", session_msg
        assert len(session_msg["session_token"]) >= 32, session_msg
        print("[client] SESSION_STARTED ok")

        await ws.send(json.dumps({
            "t": "RUN",
            "pipeline": "demo-chain",
            "input": {"base": 5},
            "subscribe": ["report"],
        }))

        got_run_started = False
        got_artifact_header = False
        got_artifact_binary = False
        got_run_complete = False
        binary_payload = None

        while True:
            raw = await asyncio.wait_for(ws.recv(), timeout=10)
            if isinstance(raw, bytes):
                got_artifact_binary = True
                binary_payload = raw
                print(f"[client] binary frame {len(raw)} bytes")
                continue
            msg = json.loads(raw)
            t = msg["t"]
            print(f"[client] {t} {msg}")
            if t == "RUN_STARTED":
                got_run_started = True
                assert "step_plan" in msg
            elif t == "ARTIFACT_HEADER":
                got_artifact_header = True
                assert msg["artifact"] == "report"
                assert msg["size"] > 0
            elif t == "RUN_COMPLETE":
                got_run_complete = True
                break
            elif t == "ERROR":
                raise RuntimeError(f"server error: {msg}")

        assert got_run_started, "no RUN_STARTED"
        assert got_artifact_header, "no ARTIFACT_HEADER"
        assert got_artifact_binary, "no binary artifact frame"
        assert got_run_complete, "no RUN_COMPLETE"

        # Payload is JSON produced by the `report` step.
        body = binary_payload.decode("utf-8")
        report = json.loads(body)
        # demo-chain math: base=5 → scaled=15, biased=12, product=180.
        assert report["scaled"] == 15, report
        assert report["biased"] == 12, report
        assert report["product"] == 180, report
        print("[client] report payload ok:", report)


async def main():
    port = pick_port()
    cache = tempfile.mkdtemp(prefix="serve_smoke_cache_")
    env = os.environ.copy()
    # start_new_session=True (setsid) puts the processor in its own
    # process group. On cleanup we signal the entire group with
    # `os.killpg(...)` so the child can't be left orphaned even if the
    # test crashes hard (SIGKILL the parent etc.) — without this, a
    # brutal parent exit leaves dioramap-processor running on init,
    # consuming the chosen port until the host reboots.
    proc = subprocess.Popen(
        [BIN, "serve",
         "--port", str(port),
         "--cache-dir", cache,
         "--pipelines-dir", PIPELINES_DIR],
        env=env,
        stdout=sys.stderr,
        stderr=sys.stderr,
        start_new_session=True,
    )
    try:
        await wait_for_port(port)
        await run_client(port)
    finally:
        try: os.killpg(os.getpgid(proc.pid), signal.SIGTERM)
        except (ProcessLookupError, PermissionError): pass
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            try: os.killpg(os.getpgid(proc.pid), signal.SIGKILL)
            except (ProcessLookupError, PermissionError): pass


if __name__ == "__main__":
    asyncio.run(main())
