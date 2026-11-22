#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Antoine Durand
"""serve auth matrix: optional shared token.

  No token configured (default lab mode)
    - plain connect              → SESSION_STARTED, CATALOG works.
  Token configured (--token)
    - no ?token=                 → ERROR AUTH_FAILED, connection closes.
    - wrong ?token=              → ERROR AUTH_FAILED, connection closes.
    - RUN sent right after a rejected connect is NEVER dispatched
      (regression test: messages must not run before auth succeeds).
    - correct ?token=            → SESSION_STARTED, CATALOG works.

Invoked from CTest: see core/tests/CMakeLists.txt.
"""
import asyncio, json, os, signal, socket, subprocess, sys, tempfile, time
import websockets

BIN = os.environ["DIORAMAP_BIN"]
PIPELINES_DIR = os.environ["V3_PIPELINES_DIR"] + "/smoke"
TOKEN = "serve-auth-test-token-0123456789"
WS_KW = dict(subprotocols=["dioramap-v1"], origin="http://localhost:5183")


def pick_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


async def wait_for_port(port, timeout=10.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.5):
                return
        except OSError:
            await asyncio.sleep(0.1)
    raise RuntimeError(f"server didn't come up on :{port}")


async def expect_session_and_catalog(uri, label):
    async with websockets.connect(uri, **WS_KW) as ws:
        m = json.loads(await asyncio.wait_for(ws.recv(), timeout=5))
        assert m["t"] == "SESSION_STARTED", f"{label}: {m}"
        await ws.send(json.dumps({"t": "CATALOG", "pipeline": "demo-chain"}))
        m = json.loads(await asyncio.wait_for(ws.recv(), timeout=5))
        names = {a["name"] for a in m["artifacts"]}
        assert {"scaled", "biased", "product", "report"} <= names, names
    print(f"[{label}] session + catalog ok")


async def expect_rejected(uri, label, send_run_first=False):
    """Expect ERROR AUTH_FAILED then close. Optionally fire a RUN immediately
    after connecting: nothing but the ERROR may come back."""
    async with websockets.connect(uri, **WS_KW) as ws:
        if send_run_first:
            await ws.send(json.dumps({"t": "RUN", "pipeline": "demo-chain",
                                      "input": {"x": 1}}))
        frames = []
        try:
            while True:
                frames.append(json.loads(await asyncio.wait_for(ws.recv(), timeout=2)))
        except (websockets.exceptions.ConnectionClosed, asyncio.TimeoutError):
            pass
    assert frames, f"{label}: no frames"
    assert frames[0]["t"] == "ERROR" and frames[0].get("code") == "AUTH_FAILED", f"{label}: {frames}"
    assert len(frames) == 1, f"{label}: unexpected frames after AUTH_FAILED: {frames[1:]}"
    print(f"[{label}] AUTH_FAILED + close, nothing dispatched ok")


def spawn(port, extra):
    cache = tempfile.mkdtemp(prefix="serve_auth_")
    return subprocess.Popen(
        [BIN, "serve", "--port", str(port), "--cache-dir", cache,
         "--pipelines-dir", PIPELINES_DIR, *extra],
        stdout=sys.stderr, stderr=sys.stderr, start_new_session=True)


def kill_group(p):
    try: os.killpg(os.getpgid(p.pid), signal.SIGTERM)
    except (ProcessLookupError, PermissionError): pass
    try: p.wait(timeout=5)
    except subprocess.TimeoutExpired:
        os.killpg(os.getpgid(p.pid), signal.SIGKILL)


async def main():
    port = pick_port()
    p = spawn(port, [])
    try:
        await wait_for_port(port)
        await expect_session_and_catalog(f"ws://127.0.0.1:{port}/", "no-token-mode")
    finally:
        kill_group(p)

    port = pick_port()
    p = spawn(port, ["--token", TOKEN])
    try:
        await wait_for_port(port)
        base = f"ws://127.0.0.1:{port}/"
        await expect_rejected(base, "token-missing")
        await expect_rejected(base + "?token=wrong-token-value-xxxxxxxx", "token-wrong")
        await expect_rejected(base, "token-missing+RUN", send_run_first=True)
        await expect_session_and_catalog(base + f"?token={TOKEN}", "token-ok")
    finally:
        kill_group(p)
    print("[serve_auth] all cases passed")


if __name__ == "__main__":
    asyncio.run(main())
