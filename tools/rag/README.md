# Journal/Docs RAG

Local, CPU-only hybrid retrieval over this repo's knowledge base (the research
journal in `docs/journal/`, the design / plan / quality docs, READMEs,
changelogs, and commit messages). Built for contributors who work with an AI
coding assistant: before re-deriving a past decision, the assistant asks the
index "have we tried / decided this before?". Retrieval only — the assistant
runs the query, gets back matching chunks with metadata, and reasons over
them. No generation, no external API, nothing leaves the machine (the embedding
model is downloaded once from HuggingFace, then runs offline).

Design rationale and locked decisions: [`BUILD-PLAN.md`](BUILD-PLAN.md).

## Query (the thing you'll usually do)

```bash
python3 tools/rag/rag_query.py "is the coplanar-import a bug" --top-k 8
python3 tools/rag/rag_query.py "make-shared cascade" --keywords "planes_dyn RIDGE_TYPE_COPLANAR"
python3 tools/rag/rag_query.py "DOF follows topology" --format text   # human-readable
```

Returns JSON: ranked chunks with `journal_id`, `title`, `date`, `section`,
`tags`, `path`, and the full chunk `text`. Cite the `journal_id` when you use a
result. Pass exact identifiers / commit SHAs via `--keywords` — the BM25 arm
catches those even when the vector arm doesn't.

The scripts auto-run under their bundled `.venv`, so plain `python3` works
regardless of your shell's interpreter (no manual venv activation needed).

## Reindex (after adding/editing journals or docs)

```bash
python3 tools/rag/build_index.py            # full rebuild incl. commit messages
python3 tools/rag/build_index.py --no-commits   # markdown docs only
```

Rebuilds `tools/rag/index.db` from scratch (corpus is small; idempotent).

### It's incremental, and mostly automatic

The cold build embeds the whole corpus on CPU (~minutes). Every rebuild after
that is **seconds**, because embeddings are cached by content hash in
`tools/rag/.embed_cache.db` (persists across rebuilds, gitignored). Only chunk
text that actually changed — a new/edited journal section, a new doc, a fresh
commit message — gets re-embedded. The cache self-prunes entries unused for 60
days. Force a full re-embed with `--rebuild-embeddings` (e.g. after a model swap).

Rebuild after adding or editing journal entries or docs, and after pulling
(new commit messages). Cheap thanks to the cache.

### Telling your assistant about it

Add something like this to your assistant's project instructions (e.g. a
`CLAUDE.md` or `.claude/rules/*.md` in your checkout):

> Before re-deriving a past design decision or asking "have we seen this
> before?", run `python3 tools/rag/rag_query.py "<question>" --top-k 8` and
> cite the returned `journal_id`. Pass exact identifiers via `--keywords`.
> The index covers prose only: check code existence with grep.

## Validate (regression net)

```bash
python3 tools/rag/validate.py               # asserts golden queries hit known entries
```

## Models

Default `bge-large-en-v1.5` (1024-d). Switch with `--model bge-base-en-v1.5`
or `--model nomic-embed-text-v1.5` — but the query model must match the index's
build model (enforced via the `meta` table), so a switch means a rebuild.

## First-time setup

```bash
python3 -m venv --without-pip tools/rag/.venv
curl -sS https://bootstrap.pypa.io/get-pip.py | tools/rag/.venv/bin/python
tools/rag/.venv/bin/python -m pip install -r tools/rag/requirements.txt
python3 tools/rag/build_index.py     # cold build: a few minutes on CPU (model download ~1.3 GB once)
```

## Files

| File | Role |
|------|------|
| `build_index.py` | offline indexer: collect → chunk → embed → SQLite store |
| `rag_query.py` | online query CLI: hybrid vector+BM25, RRF fusion → JSON |
| `chunk.py` | markdown-section-aware chunker (shared) |
| `common.py` | model registry, venv re-exec, sqlite-vec connection, embedder |
| `validate.py` | golden-query regression net |
| `index.db` | built search index — chunks + vectors + FTS (gitignored) |
| `.embed_cache.db` | content-hashed embedding cache; makes rebuilds incremental (gitignored) |
