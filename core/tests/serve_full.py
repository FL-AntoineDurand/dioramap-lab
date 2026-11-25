#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Antoine Durand
"""Full A7 protocol coverage.

Exercises:
  - SESSION_STARTED on connect
  - CATALOG without pipeline → list of pipelines
  - CATALOG with pipeline → per-write entries (name/kind/producer/visibility/output_hash)
  - RUN → ARTIFACT_HEADER + binary + RUN_COMPLETE
  - Late SUBSCRIBE → SUBSCRIBE_ACK + ARTIFACT_HEADER + binary (from cache)
  - CANCEL of an active RUN → ERROR { msg: "cancelled" }
  - RECONNECT to a preserved session after WS close → RECONNECTED
  - Step failure → ERROR + terminal RUN_FAILED {code, step, msg, input,
    processor_version} (docs/ERRORS.md), and a retry right after is not
    RUN_BUSY
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

# Multiplier for all wall-clock timeouts in this script. Sanitizer builds
# run 5-10× slower (TSan in particular); the run-ctest wrapper sets this
# to 5.0 for the tsan variant so this test stops timing out mid-recv on
# the QA VM. Default 1.0 = normal release/asan builds keep their tight
# 10 s budget so real hangs surface fast.
T_MULT = float(os.environ.get("DIORAMAP_TEST_TIMEOUT_MULT", "1.0"))


def pick_port():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


async def wait_for_port(port, timeout=10.0):
    timeout *= T_MULT
    deadline = time.time() + timeout
    while time.time() < deadline:
        s = socket.socket()
        s.settimeout(0.5)
        try:
            s.connect(("127.0.0.1", port))
            s.close()
            return
        except OSError:
            s.close()
            await asyncio.sleep(0.1)
    raise RuntimeError(f"server didn't come up on :{port}")


async def recv_until(ws, predicate, timeout=10):
    """Collect messages until predicate(msg_obj_or_bytes) returns True.
    Returns list of all collected items (including the terminator).
    """
    collected = []
    deadline = time.time() + timeout * T_MULT
    while True:
        remaining = deadline - time.time()
        if remaining <= 0:
            raise asyncio.TimeoutError(f"recv_until: timed out. collected: {collected}")
        raw = await asyncio.wait_for(ws.recv(), timeout=remaining)
        if isinstance(raw, bytes):
            collected.append(raw)
            if predicate(raw):
                return collected
        else:
            obj = json.loads(raw)
            collected.append(obj)
            if predicate(obj):
                return collected


async def test_session_and_catalog(port):
    uri = f"ws://127.0.0.1:{port}/"
    async with websockets.connect(uri, subprotocols=["dioramap-v1"]) as ws:
        m = json.loads(await ws.recv())
        assert m["t"] == "SESSION_STARTED", m
        print("[case1] SESSION_STARTED")

        # CATALOG without pipeline → list of names
        await ws.send(json.dumps({"t": "CATALOG"}))
        m = json.loads(await ws.recv())
        assert m["t"] == "CATALOG_RESPONSE", m
        assert "demo-chain" in m["pipelines"], m
        assert "gpu-smoke" in m["pipelines"], m
        print("[case1] CATALOG list ok:", m["pipelines"])

        # CATALOG with pipeline → per-write entries
        await ws.send(json.dumps({"t": "CATALOG", "pipeline": "demo-chain"}))
        m = json.loads(await ws.recv())
        assert m["t"] == "CATALOG_RESPONSE", m
        assert m["pipeline"] == "demo-chain", m
        names = {a["name"] for a in m["artifacts"]}
        # demo-chain steps: scale-by-factor, add-bias, multiply, report.
        assert "report" in names, m
        producers = {a["name"]: a["producer"] for a in m["artifacts"]}
        assert "report" in producers, producers
        # D0: every CATALOG_RESPONSE carries an attributions array (possibly
        # empty for pipelines that don't declare any). demo-chain has none.
        assert "attributions" in m, m
        assert isinstance(m["attributions"], list), m
        assert m["attributions"] == [], m
        print("[case1] CATALOG detail ok:", sorted(names))

        # CATALOG with a pipeline that DOES declare attributions → array
        # carries the labels + urls verbatim. viewer-full declares three
        # IGN-derived sources.
        await ws.send(json.dumps({"t": "CATALOG", "pipeline": "viewer-full"}))
        m = json.loads(await ws.recv())
        assert m["t"] == "CATALOG_RESPONSE", m
        assert m["pipeline"] == "viewer-full", m
        attrs = m.get("attributions")
        assert isinstance(attrs, list) and len(attrs) >= 1, m
        labels = [a["label"] for a in attrs]
        assert any("IGN" in label for label in labels), labels
        # Each entry has both keys; url may be null.
        for a in attrs:
            assert "label" in a and isinstance(a["label"], str) and a["label"], a
            assert "url" in a, a
            assert a["url"] is None or isinstance(a["url"], str), a
        print("[case1] CATALOG attributions ok:", labels)


async def test_run_then_late_subscribe(port):
    uri = f"ws://127.0.0.1:{port}/"
    async with websockets.connect(uri, subprotocols=["dioramap-v1"]) as ws:
        await ws.recv()  # SESSION_STARTED
        # First RUN asks only for `report`.
        await ws.send(json.dumps({
            "t": "RUN",
            "pipeline": "demo-chain",
            "input": {"base": 5},
            "subscribe": ["report"],
        }))
        msgs = await recv_until(
            ws,
            lambda m: isinstance(m, dict) and m.get("t") == "RUN_COMPLETE",
        )
        # Must include RUN_STARTED + ARTIFACT_HEADER for report + binary
        types = [m["t"] for m in msgs if isinstance(m, dict)]
        assert "RUN_STARTED" in types, types
        assert "ARTIFACT_HEADER" in types, types
        assert any(isinstance(m, bytes) for m in msgs), "no binary frame"
        print("[case2] initial RUN delivered report")

        # Late SUBSCRIBE for a different artifact from the same pipeline.
        await ws.send(json.dumps({"t": "SUBSCRIBE", "artifact": "scaled"}))
        msgs2 = await recv_until(
            ws,
            lambda m: isinstance(m, dict) and m.get("t") == "SUBSCRIBE_ACK",
        )
        # Verify NO RUN_STARTED / RUN_COMPLETE on the wire for late-sub
        # (the client already has a run; SUBSCRIBE_ACK terminates the
        # late-subscribe sequence). STEP_STARTED / STEP_COMPLETE still
        # fire so the client's timeline has real feedback while the tail
        # of the DAG runs (per the server.c late-subscribe contract).
        types2 = [m["t"] for m in msgs2 if isinstance(m, dict)]
        assert "RUN_STARTED" not in types2, types2
        assert "RUN_COMPLETE" not in types2, types2
        assert "ARTIFACT_HEADER" in types2, types2
        assert "SUBSCRIBE_ACK" in types2, types2
        # Must have binary for scaled.
        assert any(isinstance(m, bytes) for m in msgs2), "no late-subscribe binary"
        print("[case2] late SUBSCRIBE delivered scaled")


async def test_cancel(port):
    uri = f"ws://127.0.0.1:{port}/"
    async with websockets.connect(uri, subprotocols=["dioramap-v1"]) as ws:
        await ws.recv()  # SESSION_STARTED
        await ws.send(json.dumps({
            "t": "RUN",
            "pipeline": "demo-chain",
            "input": {"base": 99},  # distinct base → cache miss, steps actually run
            "subscribe": ["report"],
        }))
        # Wait for RUN_STARTED so the runner has registered the run_id.
        while True:
            m = json.loads(await ws.recv())
            if m["t"] == "RUN_STARTED":
                break
        await ws.send(json.dumps({"t": "CANCEL"}))
        # Collect until we see a terminal marker: either ERROR "cancelled" or
        # RUN_COMPLETE (cancel arrived after the run finished, which can
        # happen for very fast pipelines).
        saw_cancel = False
        saw_complete = False
        deadline = time.time() + 5 * T_MULT
        while time.time() < deadline:
            m = await asyncio.wait_for(ws.recv(), timeout=deadline - time.time())
            if isinstance(m, bytes):
                continue
            obj = json.loads(m)
            if obj.get("t") == "ERROR" and obj.get("msg") == "cancelled":
                saw_cancel = True
                break
            if obj.get("t") == "RUN_COMPLETE":
                saw_complete = True
                break
        assert saw_cancel or saw_complete, "neither ERROR cancelled nor RUN_COMPLETE"
        print(f"[case3] CANCEL terminal = {'ERROR cancelled' if saw_cancel else 'RUN_COMPLETE (race)'}")


async def test_run_failed(port):
    uri = f"ws://127.0.0.1:{port}/"
    async with websockets.connect(uri, subprotocols=["dioramap-v1"]) as ws:
        started = json.loads(await ws.recv())
        assert started["t"] == "SESSION_STARTED"
        assert started.get("processor_version"), started
        # lat out of range: canonicalize-location rejects it before any
        # network access, with INVALID_INPUT.
        for attempt in range(2):
            await ws.send(json.dumps({
                "t": "RUN",
                "pipeline": "viewer-full",
                "input": {"lat": 999.0, "lon": 2.0},
                "subscribe": ["canonical-location"],
            }))
            msgs = await recv_until(
                ws, lambda m: isinstance(m, dict) and m.get("t") in ("RUN_FAILED", "RUN_COMPLETE"))
            texts = [m for m in msgs if isinstance(m, dict)]
            assert not any(m.get("code") == "RUN_BUSY" for m in texts), texts
            errs = [m for m in texts if m.get("t") == "ERROR"]
            fail = texts[-1]
            assert fail["t"] == "RUN_FAILED", fail
            assert fail["code"] == "INVALID_INPUT", fail
            assert fail["step"] == "canonicalize-location", fail
            assert "out of range" in fail["msg"], fail
            assert fail["input"]["lat"] == 999.0, fail
            assert fail.get("processor_version"), fail
            assert fail["detail"]["step_version"] >= 1, fail
            assert errs and errs[0]["msg"].startswith("[INVALID_INPUT]"), errs
        print(f"[case5] RUN_FAILED = {fail['code']} @ {fail['step']}: {fail['msg']}")


async def test_reconnect(port):
    uri = f"ws://127.0.0.1:{port}/"
    ws = await websockets.connect(uri, subprotocols=["dioramap-v1"])
    m = json.loads(await ws.recv())
    assert m["t"] == "SESSION_STARTED", m
    token = m["session_token"]
    # Run once so the session has pins + last_pipeline/input.
    await ws.send(json.dumps({
        "t": "RUN",
        "pipeline": "demo-chain",
        "input": {"base": 1},
        "subscribe": ["report"],
    }))
    await recv_until(ws, lambda x: isinstance(x, dict) and x.get("t") == "RUN_COMPLETE")
    print("[case4] first RUN complete")

    await ws.close()

    # Reconnect — must be within the server's grace window (default 600s,
    # well inside a test's lifetime).
    ws2 = await websockets.connect(uri, subprotocols=["dioramap-v1"])
    m = json.loads(await ws2.recv())
    assert m["t"] == "SESSION_STARTED", m
    fresh_token = m["session_token"]
    assert fresh_token != token
    await ws2.send(json.dumps({"t": "RECONNECT", "session_token": token}))
    m = json.loads(await ws2.recv())
    assert m["t"] == "RECONNECTED", m
    assert m["session_token"] == token, m
    print("[case4] RECONNECTED to preserved session")

    # Issue a late SUBSCRIBE on the reattached session — the last_pipeline /
    # last_input are still set, so this proves the pin-and-preserve story.
    await ws2.send(json.dumps({"t": "SUBSCRIBE", "artifact": "biased"}))
    msgs = await recv_until(ws2, lambda x: isinstance(x, dict) and x.get("t") == "SUBSCRIBE_ACK")
    assert any(isinstance(m, bytes) for m in msgs), "no binary after reconnect+subscribe"
    print("[case4] post-reconnect SUBSCRIBE delivered")
    await ws2.close()


async def main():
    port = pick_port()
    cache = tempfile.mkdtemp(prefix="serve_full_cache_")
    # See serve_smoke.py for the setsid + process-group rationale.
    proc = subprocess.Popen(
        [BIN, "serve",
         "--port", str(port),
         "--cache-dir", cache,
         "--pipelines-dir", PIPELINES_DIR],
        stdout=sys.stderr,
        stderr=sys.stderr,
        start_new_session=True,
    )
    try:
        await wait_for_port(port)
        await test_session_and_catalog(port)
        await test_run_then_late_subscribe(port)
        await test_cancel(port)
        await test_run_failed(port)
        await test_reconnect(port)
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
