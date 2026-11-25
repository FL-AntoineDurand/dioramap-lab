#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Antoine Durand
"""Offline indexer for the journal/docs RAG (BUILD-PLAN.md §5).

Collect markdown (journals + reference docs + v3 docs) and optionally commit
messages -> markdown-aware chunks -> bge embeddings -> SQLite store with three
search surfaces: chunks (metadata+text), vec_chunks (sqlite-vec), fts_chunks
(FTS5/BM25), plus a meta table. Rebuilt from scratch each run (corpus is small).
"""

import argparse
import glob
import json
import os
import subprocess
import sys
import time

import common
from common import REPO_ROOT
from chunk import chunk_document

# Markdown corpus globs, relative to REPO_ROOT (BUILD-PLAN.md §1).
# Default is this repository's layout; overridable per-invocation via env
# RAG_DOC_GLOBS (JSON array) or --doc-globs CLI flag. Other repos can reuse
# this tool via `RAG_REPO_ROOT=… `.
DOC_GLOBS = [
    "docs/**/*.md",          # research journal, architecture, plans, quality
    "*.md",                  # README, CONTRIBUTING, CHANGELOG, SECURITY
    "core/**/*.md",          # engine changelog + test READMEs
    "apps/*/*.md",           # app READMEs + changelogs
]


def _active_globs():
    env = os.environ.get("RAG_DOC_GLOBS", "").strip()
    if env:
        try:
            return json.loads(env)
        except json.JSONDecodeError as e:
            raise SystemExit(f"RAG_DOC_GLOBS must be a JSON array: {e}")
    return DOC_GLOBS


def collect_docs(globs=None):
    """Return [(rel_path, text)] for every indexable markdown file."""
    seen, out = set(), []
    for g in (globs if globs is not None else _active_globs()):
        for abs_path in sorted(glob.glob(os.path.join(REPO_ROOT, g), recursive=True)):
            rel = os.path.relpath(abs_path, REPO_ROOT)
            base = os.path.basename(rel)
            if base.startswith("_index") or rel in seen:
                continue
            # universal skip for noisy dirs
            if any(p in rel.split(os.sep) for p in ("node_modules", ".venv", ".git")):
                continue
            seen.add(rel)
            try:
                with open(abs_path, encoding="utf-8") as fh:
                    out.append((rel, fh.read()))
            except (OSError, UnicodeDecodeError) as e:
                print(f"  skip {rel}: {e}", file=sys.stderr)
    return out


def collect_commits():
    """One synthetic doc per non-merge commit (subject + body)."""
    sep, rec = "\x1f", "\x1e"
    fmt = sep.join(["%h", "%ad", "%s", "%b"])
    try:
        raw = subprocess.run(
            ["git", "-C", REPO_ROOT, "log", "--no-merges",
             f"--format={fmt}{rec}", "--date=short"],
            capture_output=True, text=True, check=True,
        ).stdout
    except (subprocess.CalledProcessError, FileNotFoundError) as e:
        print(f"  commits skipped (git error): {e}", file=sys.stderr)
        return []
    docs = []
    for entry in raw.split(rec):
        entry = entry.strip("\n")
        if not entry.strip():
            continue
        sha, date, subject, body = (entry.split(sep) + ["", "", "", ""])[:4]
        text = subject if not body.strip() else f"{subject}\n\n{body.strip()}"
        meta = {"id": sha, "date": date, "title": subject,
                "category": "commit", "status": None, "tags": []}
        docs.append((f"git:{sha}", text, meta))
    return docs


# Per-commit diff size caps — skip huge diffs (vendored files, bulk imports)
# so we don't drown the index in text with no retrieval signal.
MAX_DIFF_CHARS = 20_000
MAX_DIFF_LINES = 400


def collect_diffs():
    """One synthetic doc per non-merge commit with its diff hunks.

    Captures the WHAT of each commit (files/lines actually changed), which
    commit messages often hide. Capped per-commit so vendored-file or bulk
    diffs don't dominate the index. Opt-in: pass --diffs.
    """
    try:
        shas = subprocess.run(
            ["git", "-C", REPO_ROOT, "log", "--no-merges",
             "--format=%h\t%ad\t%s", "--date=short"],
            capture_output=True, text=True, check=True,
        ).stdout.strip().splitlines()
    except (subprocess.CalledProcessError, FileNotFoundError) as e:
        print(f"  diffs skipped (git error): {e}", file=sys.stderr)
        return []
    docs = []
    for line in shas:
        parts = line.split("\t", 2)
        if len(parts) < 3:
            continue
        sha, date, subject = parts
        try:
            diff = subprocess.run(
                ["git", "-C", REPO_ROOT, "show", "--no-color", "--format=",
                 "--unified=3", "--diff-filter=AMR", sha],
                capture_output=True, text=True, check=False, timeout=15,
            ).stdout
        except Exception:
            continue
        if not diff.strip():
            continue
        lines = diff.splitlines()
        truncated = len(lines) > MAX_DIFF_LINES
        if truncated:
            lines = lines[:MAX_DIFF_LINES]
        body = "\n".join(lines)
        if len(body) > MAX_DIFF_CHARS:
            body = body[:MAX_DIFF_CHARS]
            truncated = True
        paths = sorted({
            ln.split()[-1].lstrip("b/")
            for ln in lines if ln.startswith("+++ ") and "/" in ln
        })
        paths_str = "files: " + ", ".join(paths[:15]) if paths else ""
        text = f"{subject}\n\n{paths_str}\n\n{body}".strip()
        if truncated:
            text += "\n\n[diff truncated]"
        meta = {"id": sha, "date": date, "title": subject,
                "category": "commit_diff", "status": None, "tags": []}
        docs.append((f"git-diff:{sha}", text, meta))
    return docs


def run_preprocess(cmd):
    """Run a user-supplied preprocess command (e.g. JSON-to-markdown flatten
    for other repos). Executes in REPO_ROOT; its stdout is logged.
    Non-zero exit aborts the build."""
    import shlex
    print(f"Running preprocess: {cmd}")
    r = subprocess.run(shlex.split(cmd), cwd=REPO_ROOT, capture_output=True, text=True)
    if r.stdout.strip():
        print("  " + r.stdout.strip().replace("\n", "\n  "))
    if r.returncode != 0:
        raise SystemExit(f"preprocess failed (exit {r.returncode}): {r.stderr.strip()}")


def embed_with_cache(records, embedder, cache_path, force):
    """Return serialized-float32 blobs for every record, embedding only misses.

    This is the incremental core: identical chunk text (unchanged journals,
    docs, and old commits) is served from the on-disk cache; only new/edited
    text is sent through the CPU embedder. Returns (blobs, n_cached, n_new).
    """
    import sqlite_vec

    cache = common.EmbedCache(cache_path, embedder.name, embedder.dim)
    now = int(time.time())
    keys = [cache.key(r["text"]) for r in records]
    hits = {} if force else cache.get_many(keys)

    miss_idx = [i for i, k in enumerate(keys) if k not in hits]
    if miss_idx:
        miss_vecs = embedder.embed_documents([records[i]["text"] for i in miss_idx])
        new_items = []
        for i, vec in zip(miss_idx, miss_vecs):
            blob = sqlite_vec.serialize_float32(vec.tolist())
            hits[keys[i]] = blob
            new_items.append((keys[i], blob))
        cache.put_many(new_items, now)

    cache.touch(keys, now)
    cache.prune(max_age_days=60, now=now)
    blobs = [hits[k] for k in keys]
    cache.close()
    return blobs, len(records) - len(miss_idx), len(miss_idx)


def build(db_path, model_name, include_commits, include_diffs, cache_path,
          force_embed, preprocess_cmd, globs_override):
    t0 = time.time()
    embedder = common.Embedder(model_name)
    dim = embedder.dim

    if preprocess_cmd:
        run_preprocess(preprocess_cmd)

    print(f"Collecting corpus from {REPO_ROOT}…")
    docs = collect_docs(globs_override)
    records = []
    for rel, text in docs:
        records.extend(chunk_document(rel, text))
    n_md_docs = len(docs)

    n_commits = 0
    if include_commits:
        commit_docs = collect_commits()
        n_commits = len(commit_docs)
        for rel, text, meta in commit_docs:
            records.extend(chunk_document(rel, text, meta_override=meta))

    n_diffs = 0
    if include_diffs:
        diff_docs = collect_diffs()
        n_diffs = len(diff_docs)
        for rel, text, meta in diff_docs:
            records.extend(chunk_document(rel, text, meta_override=meta))

    if not records:
        raise SystemExit("no chunks produced — check the corpus globs")
    print(f"  {n_md_docs} md docs + {n_commits} commit msgs + {n_diffs} commit diffs"
          f" -> {len(records)} chunks")

    print(f"Embedding with {model_name} ({dim}-d) on CPU…")
    blobs, n_cached, n_new = embed_with_cache(
        records, embedder, cache_path, force_embed)
    print(f"  embeddings: {n_cached} cached, {n_new} new")

    print(f"Writing {db_path}…")
    if os.path.exists(db_path):
        os.remove(db_path)
    db = common.connect(db_path, create=True)
    db.executescript(f"""
        CREATE TABLE chunks (
            rowid INTEGER PRIMARY KEY,
            chunk_id TEXT, text TEXT, path TEXT,
            journal_id TEXT, date TEXT, title TEXT, tags TEXT,
            category TEXT, status TEXT, section_heading TEXT, chunk_index INTEGER
        );
        CREATE VIRTUAL TABLE vec_chunks USING vec0(
            embedding float[{dim}] distance_metric=cosine
        );
        CREATE VIRTUAL TABLE fts_chunks USING fts5(
            text, title, section_heading, content=''
        );
        CREATE TABLE meta (key TEXT PRIMARY KEY, value TEXT);
    """)

    for i, (r, blob) in enumerate(zip(records, blobs), start=1):
        db.execute(
            "INSERT INTO chunks (rowid, chunk_id, text, path, journal_id, date, "
            "title, tags, category, status, section_heading, chunk_index) "
            "VALUES (?,?,?,?,?,?,?,?,?,?,?,?)",
            (i, r["chunk_id"], r["text"], r["path"], r["journal_id"], r["date"],
             r["title"], json.dumps(r["tags"]), r["category"], r["status"],
             r["section_heading"], r["chunk_index"]),
        )
        db.execute(
            "INSERT INTO vec_chunks (rowid, embedding) VALUES (?, ?)",
            (i, blob),
        )
        db.execute(
            "INSERT INTO fts_chunks (rowid, text, title, section_heading) "
            "VALUES (?,?,?,?)",
            (i, r["text"], r["title"] or "", r["section_heading"] or ""),
        )

    git_sha = subprocess.run(
        ["git", "-C", REPO_ROOT, "rev-parse", "--short", "HEAD"],
        capture_output=True, text=True,
    ).stdout.strip()
    meta = {
        "embedding_model": model_name,
        "embedding_dim": str(dim),
        "build_unixtime": str(int(t0)),
        "repo_root": REPO_ROOT,
        "doc_globs": json.dumps(_active_globs()),
        "include_commits": str(bool(include_commits)),
        "include_diffs": str(bool(include_diffs)),
        "git_sha": git_sha,
        "n_docs": str(n_md_docs),
        "n_commits": str(n_commits),
        "n_diffs": str(n_diffs),
        "n_chunks": str(len(records)),
    }
    db.executemany("INSERT INTO meta (key, value) VALUES (?, ?)", meta.items())
    db.commit()
    db.close()

    print(
        f"Done: {n_md_docs} docs + {n_commits} commit msgs + {n_diffs} commit diffs, "
        f"{len(records)} chunks, model={model_name} dim={dim}, "
        f"{time.time() - t0:.1f}s"
    )


def main():
    common.reexec_into_venv()
    ap = argparse.ArgumentParser(description="Build the journal/docs RAG index.")
    ap.add_argument("--model", default=common.DEFAULT_MODEL,
                    choices=list(common.MODELS), help="embedding model")
    ap.add_argument("--db", default=common.DEFAULT_DB, help="output index path")
    ap.add_argument("--cache", default=common.DEFAULT_CACHE,
                    help="embedding cache path (persists across rebuilds)")
    ap.add_argument("--rebuild-embeddings", action="store_true",
                    help="ignore the cache and re-embed every chunk")
    ap.add_argument("--no-commits", dest="commits", action="store_false", default=True,
                    help="skip commit messages (default: include)")
    ap.add_argument("--diffs", dest="diffs", action="store_true", default=False,
                    help="include commit diffs (per-file hunks, capped)")
    ap.add_argument("--preprocess", default=None,
                    help="shell command to run in REPO_ROOT before indexing "
                         "(e.g. a JSON→MD flatten for per-entity files)")
    ap.add_argument("--doc-globs", default=None,
                    help="JSON array of globs overriding DOC_GLOBS (or set env RAG_DOC_GLOBS)")
    args = ap.parse_args()
    globs_override = json.loads(args.doc_globs) if args.doc_globs else None
    build(args.db, args.model, args.commits, args.diffs, args.cache,
          args.rebuild_embeddings, args.preprocess, globs_override)


if __name__ == "__main__":
    main()
