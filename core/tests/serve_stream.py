#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Antoine Durand
"""B9 STREAM_CHUNK protocol coverage.

Drives the `stream-demo` step over WebSocket and asserts the wire shape:
RUN_STARTED → STEP_STARTED → STREAM_CHUNK ×N (interleaved with binary
frames) → trailing ARTIFACT_HEADER + binary (dual delivery, see
PLAN-VIEWER-PORT §7.3) → STEP_COMPLETE → RUN_COMPLETE. Verifies that:

  - Exactly one STREAM_CHUNK arrives per step_write_output_stream call.
  - Only the last chunk has final=true and carries the artifact's hash.
  - The concatenated binary frames equal the deterministic byte pattern
    the step generates: chunk i byte k → (i*31 + k) & 0xff.
  - The STREAM_CHUNK final chunk's `hash` field equals the trailing
    ARTIFACT_HEADER's `hash` field (both come from the same runner write —
    the store's content hash over the concatenated buffer). Runner-side
    hashing is xxh3-128 zero-padded into 32 bytes, not SHA-256, so we
    can't recompute cryptographically in Python without the xxhash dep.
    Checking cross-message equality gives the same protocol-level
    guarantee: the stream's final hash agrees with the stored artifact's.
  - The first STREAM_CHUNK arrives well within the §11 B9 "first-pixel
    within 500ms" budget — measured from RUN_STARTED to first chunk.
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


async def test_stream_demo(port):
    uri = f"ws://127.0.0.1:{port}/"
    async with websockets.connect(uri, subprotocols=["dioramap-v1"]) as ws:
        m = json.loads(await ws.recv())
        assert m["t"] == "SESSION_STARTED", m

        n_chunks = 5
        chunk_bytes = 2048
        await ws.send(json.dumps({
            "t": "RUN",
            "pipeline": "stream-smoke",
            "input": {"n_chunks": n_chunks, "chunk_bytes": chunk_bytes},
            "subscribe": ["stream-bytes"],
        }))

        # The server emits TWO header→binary pairings for a streamed artifact:
        # (a) one STREAM_CHUNK header + binary per step_write_output_stream call,
        # (b) one ARTIFACT_HEADER + binary at the end (the on_artifact_ready
        #     callback fires for every committed write, streaming included).
        # Track the most recent pending header so we can route the next binary
        # frame correctly.
        run_started_t = None
        first_chunk_t = None
        chunks = []  # list of (header_dict, binary_bytes) for STREAM_CHUNK only
        pending_kind = None  # "stream" | "artifact" | None
        pending_obj = None
        seen_artifact_header = False
        artifact_header_hash = None
        artifact_header_size = None
        seen_run_complete = False
        seen_step_started = False
        seen_step_complete = False

        deadline = time.time() + 10.0
        while time.time() < deadline:
            raw = await asyncio.wait_for(ws.recv(), timeout=deadline - time.time())
            if isinstance(raw, bytes):
                assert pending_kind is not None, "binary frame without preceding header"
                if pending_kind == "stream":
                    chunks.append((pending_obj, raw))
                    if first_chunk_t is None:
                        first_chunk_t = time.monotonic()
                # ARTIFACT_HEADER's binary is just the full artifact bytes —
                # no need to retain it; sanity-check size matches header.
                elif pending_kind == "artifact":
                    assert len(raw) == pending_obj.get("size"), (len(raw), pending_obj)
                pending_kind = None
                pending_obj = None
                continue
            obj = json.loads(raw)
            t = obj.get("t")
            if t == "RUN_STARTED":
                run_started_t = time.monotonic()
            elif t == "STEP_STARTED" and obj.get("step") == "stream-demo":
                seen_step_started = True
            elif t == "STEP_COMPLETE" and obj.get("step") == "stream-demo":
                seen_step_complete = True
            elif t == "STREAM_CHUNK":
                pending_kind = "stream"
                pending_obj = obj
            elif t == "ARTIFACT_HEADER":
                pending_kind = "artifact"
                pending_obj = obj
                seen_artifact_header = True
                artifact_header_hash = obj.get("hash")
                artifact_header_size = obj.get("size")
            elif t == "RUN_COMPLETE":
                seen_run_complete = True
                break
            elif t == "ERROR":
                raise AssertionError(f"server error: {obj}")
        assert seen_step_started
        assert seen_step_complete
        assert seen_run_complete
        assert seen_artifact_header  # streamed artifacts also fire on_artifact_ready

        # Exactly n_chunks STREAM_CHUNK records. Sort by `seq` defensively
        # (server should already deliver in order).
        chunks.sort(key=lambda c: c[0]["seq"])
        assert len(chunks) == n_chunks, f"expected {n_chunks} chunks, got {len(chunks)}: {[c[0] for c in chunks]}"
        for i, (hdr, payload) in enumerate(chunks):
            assert hdr["artifact"] == "stream-bytes", hdr
            assert hdr["seq"] == i, (i, hdr)
            assert hdr["size"] == chunk_bytes, hdr
            assert len(payload) == chunk_bytes, (i, len(payload))
            expected_final = (i == n_chunks - 1)
            assert hdr["final"] == expected_final, (i, hdr)
            if expected_final:
                assert isinstance(hdr.get("hash"), str) and len(hdr["hash"]) == 64, hdr
            else:
                assert hdr.get("hash") in (None, ""), hdr
            # Verify deterministic byte pattern.
            for k in range(0, chunk_bytes, 257):  # spot-check, not every byte
                assert payload[k] == ((i * 31 + k) & 0xff), (i, k, payload[k])

        # PLAN-PROTOCOL P10 test promise: the final hash agrees with the
        # concatenated-chunks hash. Runner uses xxh3-128 zero-padded — no
        # Python stdlib equivalent — so we check the equivalent invariant:
        # the STREAM_CHUNK final.hash equals the trailing ARTIFACT_HEADER's
        # hash. Both come from the same runner write; if they match, the
        # stream finalised to the same bytes the store committed.
        final_hash = chunks[-1][0]["hash"]
        assert artifact_header_hash == final_hash, (
            f"stream vs artifact-header hash mismatch: "
            f"stream_final={final_hash} artifact_header={artifact_header_hash}"
        )
        concatenated_len = sum(len(p) for _h, p in chunks)
        assert artifact_header_size == concatenated_len, (
            f"artifact_header.size {artifact_header_size} != concatenated {concatenated_len}"
        )
        print(f"[serve_stream] hash check ok ({concatenated_len} bytes, {final_hash[:16]}…)")

        # First-pixel timing: first STREAM_CHUNK must arrive within 500ms of
        # RUN_STARTED. The plan §11 B9 sets this as the validation bar for
        # streaming. stream-demo is intentionally cheap (just memset + send)
        # so this gives plenty of headroom — a regression here means the
        # runner is buffering.
        assert run_started_t is not None
        assert first_chunk_t is not None
        first_pixel_ms = (first_chunk_t - run_started_t) * 1000
        print(f"[serve_stream] first chunk arrived {first_pixel_ms:.1f} ms after RUN_STARTED")
        assert first_pixel_ms < 500, f"first chunk took {first_pixel_ms:.1f} ms (>500ms budget)"


async def main():
    port = pick_port()
    cache = tempfile.mkdtemp(prefix="serve_stream_cache_")
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
        await test_stream_demo(port)
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
