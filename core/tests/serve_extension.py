#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Antoine Durand
"""Processor extension seam (core/include/v3/extension.h), end to end.

Runs `serve` linked against tests/fake_extension.c:

  - no ?key=            → ERROR AUTH_REQUIRED (the extension's code), closed
  - unknown key         → ERROR AUTH_FAILED unknown_key, closed
  - disallowed Origin   → ERROR AUTH_FAILED origin_not_allowed, closed
  - full key            → SESSION_STARTED tier=internal, debug artifacts listed,
                          successful RUN reported (principal, pipeline, success)
  - failing RUN         → reported with success=false, error_code, lat/lon
  - public-only key     → debug artifacts hidden from CATALOG, RUN/SUBSCRIBE
                          on them → FORBIDDEN_ARTIFACT, public ones still work
  - init() refusal      → serve exits non-zero without listening
  - shutdown()          → called on SIGTERM

Invoked from CTest: see core/tests/CMakeLists.txt.
"""
import asyncio, json, os, signal, socket, subprocess, sys, tempfile, time
import websockets

BIN = os.environ["DIORAMAP_BIN"]
PIPELINES_DIR = os.environ["V3_PIPELINES_DIR"] + "/smoke"
WS_KW = dict(subprotocols=["dioramap-v1"], origin="http://localhost:5183")
TERMINAL = ("RUN_COMPLETE", "RUN_FAILED")


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


async def recv_json(ws, timeout=10):
    while True:
        m = await asyncio.wait_for(ws.recv(), timeout=timeout)
        if isinstance(m, str):
            return json.loads(m)


async def recv_until(ws, pred, timeout=30):
    out = []
    while True:
        m = await recv_json(ws, timeout)
        out.append(m)
        if pred(m):
            return out


async def expect_refused(uri, code, reason, origin=None):
    kw = dict(WS_KW)
    if origin:
        kw["origin"] = origin
    async with websockets.connect(uri, **kw) as ws:
        frames = []
        try:
            while True:
                frames.append(json.loads(await asyncio.wait_for(ws.recv(), timeout=2)))
        except (websockets.exceptions.ConnectionClosed, asyncio.TimeoutError):
            pass
    assert len(frames) == 1, frames
    assert frames[0]["t"] == "ERROR" and frames[0]["code"] == code, frames
    assert reason in frames[0]["msg"], frames
    print(f"[refused] {code} {frames[0]['msg']!r} ok")


async def catalog(ws, pipeline):
    await ws.send(json.dumps({"t": "CATALOG", "pipeline": pipeline}))
    m = await recv_until(ws, lambda m: m.get("t") in ("CATALOG_RESPONSE", "ERROR"))
    return m[-1]


def spawn(port, reports, cache):
    env = dict(os.environ)
    if reports is None:
        env.pop("FAKE_EXT_REPORTS", None)
    else:
        env["FAKE_EXT_REPORTS"] = reports
    return subprocess.Popen(
        [BIN, "serve", "--port", str(port), "--cache-dir", cache,
         "--pipelines-dir", PIPELINES_DIR],
        stdout=sys.stderr, stderr=sys.stderr, start_new_session=True, env=env)


def stop(p):
    try: os.killpg(os.getpgid(p.pid), signal.SIGTERM)
    except (ProcessLookupError, PermissionError): pass
    try: p.wait(timeout=10)
    except subprocess.TimeoutExpired:
        os.killpg(os.getpgid(p.pid), signal.SIGKILL)


def read_reports(path):
    with open(path) as f:
        return [json.loads(l) for l in f if l.strip()]


async def wait_reports(path, n, timeout=10):
    deadline = time.time() + timeout
    while time.time() < deadline:
        r = read_reports(path)
        if len(r) >= n:
            return r
        await asyncio.sleep(0.1)
    raise AssertionError(f"expected {n} reports, got {read_reports(path)}")


async def full_access(base, reports):
    async with websockets.connect(base + "?key=full-key-0001", **WS_KW) as ws:
        m = await recv_json(ws)
        assert m["t"] == "SESSION_STARTED" and m.get("tier") == "internal", m
        cat = await catalog(ws, "demo-chain")
        names = {a["name"] for a in cat["artifacts"]}
        assert {"scaled", "biased", "product", "report"} <= names, cat

        await ws.send(json.dumps({"t": "RUN", "pipeline": "demo-chain",
                                  "input": {"base": 2}, "subscribe": ["report"]}))
        done = (await recv_until(ws, lambda m: m.get("t") in TERMINAL))[-1]
        assert done["t"] == "RUN_COMPLETE", done

        await ws.send(json.dumps({"t": "RUN", "pipeline": "viewer-full",
                                  "input": {"lat": 999.0, "lon": 2.0},
                                  "subscribe": ["canonical-location"]}))
        fail = (await recv_until(ws, lambda m: m.get("t") in TERMINAL))[-1]
        assert fail["t"] == "RUN_FAILED" and fail["code"] == "INVALID_INPUT", fail

    r = await wait_reports(reports, 2)
    ok, bad = r[0], r[1]
    assert ok["event"] == "scene_load" and ok["success"] is True, ok
    assert ok["principal"] == "full-key-0001" and ok["account"] == "org-full", ok
    assert ok["pipeline"] == "demo-chain" and ok["input_hash"], ok
    assert ok["n_executed"] + ok["n_cache_hits"] >= 4, ok
    assert bad["success"] is False and bad["error_code"] == "INVALID_INPUT", bad
    assert "out of range" in bad["error_message"], bad
    assert bad["lat"] == 999.0 and bad["lon"] == 2.0, bad
    print("[full] tier, catalog, success + failure reports ok")


async def public_only(base, reports):
    async with websockets.connect(base + "?key=public-key-0001", **WS_KW) as ws:
        m = await recv_json(ws)
        assert m["t"] == "SESSION_STARTED" and m.get("tier") == "public", m
        cat = await catalog(ws, "demo-chain")
        assert cat["artifacts"] == [] and cat["steps"] == [], cat

        await ws.send(json.dumps({"t": "RUN", "pipeline": "demo-chain",
                                  "input": {"base": 2}, "subscribe": ["report"]}))
        err = await recv_json(ws)
        assert err["t"] == "ERROR" and err["code"] == "FORBIDDEN_ARTIFACT", err

        cat = await catalog(ws, "stream-smoke")
        assert [a["name"] for a in cat["artifacts"]] == ["stream-bytes"], cat
        await ws.send(json.dumps({"t": "RUN", "pipeline": "stream-smoke",
                                  "input": {"n_chunks": 2, "chunk_bytes": 64},
                                  "subscribe": ["stream-bytes"]}))
        done = (await recv_until(ws, lambda m: m.get("t") in TERMINAL))[-1]
        assert done["t"] == "RUN_COMPLETE", done

    r = await wait_reports(reports, 3)
    assert r[2]["principal"] == "public-key-0001" and r[2]["pipeline"] == "stream-smoke", r[2]
    print("[public-only] hidden, forbidden, public artifact ok")


async def main():
    work = tempfile.mkdtemp(prefix="serve_ext_")
    reports = os.path.join(work, "reports.jsonl")

    # init() refusal: no FAKE_EXT_REPORTS → serve must exit non-zero.
    p = spawn(pick_port(), None, tempfile.mkdtemp(dir=work))
    rc = p.wait(timeout=20)
    assert rc != 0, "serve started although the extension refused to initialise"
    print(f"[init-refused] exit {rc} ok")

    port = pick_port()
    p = spawn(port, reports, tempfile.mkdtemp(dir=work))
    try:
        await wait_for_port(port)
        base = f"ws://127.0.0.1:{port}/"
        await expect_refused(base, "AUTH_REQUIRED", "key required")
        await expect_refused(base + "?key=nope", "AUTH_FAILED", "unknown_key")
        await expect_refused(base + "?key=full-key-0001", "AUTH_FAILED",
                             "origin_not_allowed", origin="http://evil.example")
        await full_access(base, reports)
        await public_only(base, reports)
    finally:
        stop(p)
    last = read_reports(reports)[-1]
    assert last == {"event": "shutdown"}, last
    print("[shutdown] ok")
    print("[serve_extension] all cases passed")


if __name__ == "__main__":
    asyncio.run(main())
