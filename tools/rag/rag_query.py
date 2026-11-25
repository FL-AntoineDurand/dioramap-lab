#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Antoine Durand
"""Online query CLI for the journal/docs RAG (BUILD-PLAN.md §6).

Hybrid retrieval: vector (sqlite-vec, cosine) + keyword (FTS5/BM25), fused with
Reciprocal Rank Fusion. Retrieval only — prints matching chunks as JSON for a
Claude session to read and reason over. No generation here.
"""

import argparse
import json
import re
import sys

import common

RRF_K = 60          # RRF damping constant (BUILD-PLAN.md §6)
CANDIDATES = 50     # depth pulled from each search arm before fusion


def _check_model(db, requested):
    row = db.execute(
        "SELECT value FROM meta WHERE key='embedding_model'"
    ).fetchone()
    built_with = row["value"] if row else None
    if built_with and requested and requested != built_with:
        raise SystemExit(
            f"model mismatch: index built with {built_with!r} but --model is "
            f"{requested!r}. Rebuild, or query with --model {built_with}."
        )
    return built_with


def vector_search(db, qvec, limit):
    import sqlite_vec

    rows = db.execute(
        "SELECT rowid FROM vec_chunks WHERE embedding MATCH ? AND k = ? "
        "ORDER BY distance",
        (sqlite_vec.serialize_float32(qvec.tolist()), limit),
    ).fetchall()
    return [r["rowid"] for r in rows]


def _fts_match(query, keywords):
    """Build a safe FTS5 MATCH string: OR over query tokens + quoted keywords."""
    terms = [t for t in re.findall(r"[A-Za-z0-9_]+", query) if len(t) > 1]
    parts = [f'"{t}"' for t in terms]
    for kw in keywords:
        kw = kw.strip().replace('"', "")
        if kw:
            parts.append(f'"{kw}"')  # phrase: identifiers match across _ splits
    return " OR ".join(dict.fromkeys(parts))  # dedupe, keep order


def keyword_search(db, query, keywords, limit):
    match = _fts_match(query, keywords)
    if not match:
        return []
    try:
        rows = db.execute(
            "SELECT rowid, bm25(fts_chunks) AS rank FROM fts_chunks "
            "WHERE fts_chunks MATCH ? ORDER BY rank LIMIT ?",
            (match, limit),
        ).fetchall()
    except Exception as e:  # malformed MATCH — degrade to vector-only
        print(f"  (fts skipped: {e})", file=sys.stderr)
        return []
    return [r["rowid"] for r in rows]


def rrf_fuse(*ranked_lists, k=RRF_K):
    scores = {}
    for lst in ranked_lists:
        for rank, rowid in enumerate(lst, start=1):
            scores[rowid] = scores.get(rowid, 0.0) + 1.0 / (k + rank)
    return sorted(scores.items(), key=lambda kv: kv[1], reverse=True)


def query(db_path, q, keywords, top_k, model):
    db = common.connect(db_path)
    built_with = _check_model(db, model)
    embedder = common.Embedder(built_with)
    qvec = embedder.embed_query(q)

    vec_ids = vector_search(db, qvec, CANDIDATES)
    kw_ids = keyword_search(db, q, keywords, CANDIDATES)
    fused = rrf_fuse(vec_ids, kw_ids)[:top_k]

    results = []
    for rank, (rowid, score) in enumerate(fused, start=1):
        row = db.execute(
            "SELECT chunk_id, text, path, journal_id, date, title, tags, "
            "category, status, section_heading FROM chunks WHERE rowid=?",
            (rowid,),
        ).fetchone()
        if row is None:
            continue
        results.append({
            "rank": rank,
            "score": round(score, 6),
            "text": row["text"],
            "journal_id": row["journal_id"],
            "title": row["title"],
            "date": row["date"],
            "category": row["category"],
            "status": row["status"],
            "section": row["section_heading"],
            "tags": json.loads(row["tags"] or "[]"),
            "path": row["path"],
        })
    db.close()
    return {"query": q, "keywords": keywords, "results": results}


def print_text(out):
    print(f"query: {out['query']}")
    if out["keywords"]:
        print(f"keywords: {' '.join(out['keywords'])}")
    print(f"{len(out['results'])} results\n")
    for r in out["results"]:
        ident = r["journal_id"] or r["path"]
        print(f"#{r['rank']}  [{r['score']}]  {ident}  ({r['date'] or '-'})")
        print(f"     {r['title']}  ›  {r['section']}")
        snippet = " ".join(r["text"].split())[:280]
        print(f"     {snippet}")
        print(f"     {r['path']}\n")


def main():
    common.reexec_into_venv()
    ap = argparse.ArgumentParser(description="Query the journal/docs RAG index.")
    ap.add_argument("query", help="natural-language question")
    ap.add_argument("--keywords", default="",
                    help="extra exact terms/identifiers/SHAs to boost (BM25)")
    ap.add_argument("--top-k", type=int, default=8, help="results to return")
    ap.add_argument("--db", default=common.DEFAULT_DB, help="index path")
    ap.add_argument("--model", default=None,
                    help="override (must match the index's build model)")
    ap.add_argument("--format", choices=["json", "text"], default="json")
    args = ap.parse_args()

    keywords = args.keywords.split() if args.keywords else []
    out = query(args.db, args.query, keywords, args.top_k, args.model)
    if args.format == "text":
        print_text(out)
    else:
        print(json.dumps(out, indent=2, ensure_ascii=False))


if __name__ == "__main__":
    main()
