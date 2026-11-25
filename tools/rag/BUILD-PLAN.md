# Build Plan — Journal/Docs RAG for Claude sessions

**Status:** ready to build. **Audience:** an implementing agent with no prior context.
**Goal:** a tiny, local, CPU-only retrieval tool over this project's growing knowledge base
(journals + reference docs) so that **any Claude session can run one script to pull
relevant, grounded context** ("has this been decided/found before?") instead of grepping
140+ journal entries by hand. The recall gap is real and recurring (we have re-derived
already-decided things mid-analysis).

This is **retrieval only**. There is NO generation step in the tool — the *consumer* is a
Claude session that runs the query script, gets back matching chunks + metadata, and
reasons over them itself.

---

## 0. Locked design decisions (do NOT re-litigate — these were settled deliberately)

| Decision | Choice | Why |
|---|---|---|
| Glue | **A CLI script + a `.claude/rules/` entry.** No MCP server, no framework. | Any session runs `rag_query.py "<q>"` via Bash. The script *is* the product; the rule says when to call it. Minimal deps, nothing to keep running. |
| Corpus (v1) | **Journals (primary) + reference docs.** Commit messages optional. **Code/tests excluded** from v1. | RAG shines on prose rationale ("why/what/when"); code is better served by grep/symbol search. |
| Embedding | **Local, CPU, via `fastembed` (ONNX).** Model: `nomic-embed-text-v2` (768-d) OR `BAAI/bge-large-en-v1.5` (1024-d). | Private repo → data stays local; no GPU needed; both are current (2026) CPU picks, well past the quality bar. Pick ONE; default to `bge-large-en-v1.5` for robustness, `nomic-v2` if you want lighter. |
| Vector store | **`sqlite-vec`** (single local `.db` file). | Matches the project's existing SQLite usage; brute-force/flat search is instant at this corpus size (a few thousand chunks). |
| Retrieval | **HYBRID: vector (sqlite-vec) + keyword BM25 (SQLite FTS5), fused with Reciprocal Rank Fusion (RRF).** | Confirmed 2026 best practice, and *essential* here: our text is full of exact identifiers (`RIDGE_TYPE_COPLANAR`, `planes_dyn`, commit SHAs) where BM25 is undefeated. |
| Chunking | **Markdown-section-aware** (split on `##`/`###` headings), carry frontmatter as metadata, target ~300–500 tokens, merge tiny sections / split huge ones. | Journals are already structured; a section ≈ one idea. Beats fixed-size. |
| Reranking | **Out of scope for v1.** (v2: add BGE-reranker over top-N.) | High ROI later, but hybrid+good-chunking is enough for v1. |
| Language | **Python** (standalone, own `requirements.txt`/venv). | Best embedding + sqlite-vec ecosystem. The repo already has Python research scripts. |

Caveat from the research: **MTEB is a prior, not an oracle** — the model that wins on
*our* data may rank mid-pack. So after building, sanity-check retrieval on the golden
queries in §7; don't chase the leaderboard.

---

## 1. Corpus — what to index

Globs (relative to the repo root; current list in `build_index.py` `DOC_GLOBS`):
- `docs/journal/**/*.md` — **primary.** Skip `_index.md` / `_index.json`.
- `docs/**/*.md` — reference docs (ARCHITECTURE, STEP-CATALOG, quality, etc.). Skip `journal/_index*`.
- `*.md`, `core/**/*.md`, `apps/*/*.md` — READMEs and changelogs.
- **Optional (flag `--include-commits`):** `git log --format=...` bodies → one synthetic "doc" per commit. Off by default in v1.
- **Exclude:** code, tests, `node_modules`, `third_party`, `build*`, `.pyc`, anything binary.

For each file, parse YAML frontmatter when present (journals have `id, date, title, tags,
category, status`). Carry those as chunk metadata.

---

## 2. Chunking — method

1. Strip & capture frontmatter as `doc_meta`.
2. Split the body on markdown headings (`^#{2,4} `). Each heading + its body = a section.
3. Normalize section size:
   - If a section > ~500 tokens (≈ ~2000 chars), split it further on paragraph breaks into ~300–500-token pieces (keep a 1-sentence overlap or repeat the heading line at the top of each piece so context isn't lost).
   - If a section < ~80 tokens, merge it with the next section.
4. Emit one **chunk record** per piece:
   ```
   { id: "<path>#<section-slug>#<n>",
     text: "<heading line>\n<body piece>",
     path, journal_id, date, title, tags[], category, status, section_heading,
     chunk_index }
   ```
   (`journal_id`/`date`/etc. null for non-journal docs.)

Token counting: a rough `len(text)//4` heuristic is fine (no tokenizer dependency needed);
or use the embedding model's tokenizer if convenient.

---

## 3. Embedding — model + runtime

- **Library:** `fastembed` (ONNX, CPU). Fallback: `sentence-transformers`.
- **Model:** `BAAI/bge-large-en-v1.5` (1024-d) — default. (Swap to `nomic-ai/nomic-embed-text-v1.5` 768-d via a `--model` flag; keep the dim in a config so the store matches.)
- **Important:** some models need an instruction prefix for queries vs documents (e.g. BGE wants `"Represent this sentence for searching relevant passages: "` on the *query* side; nomic uses `search_query:` / `search_document:` prefixes). **Read the chosen model's card and apply the correct asymmetric prefixes** — this is a real quality lever, easy to miss.
- Embed all chunk texts in batches; store vectors as float32.
- Record the model name + dim in a `meta` table so query-time uses the same model and a mismatch is caught.

---

## 4. Store — schema (`tools/rag/index.db`, gitignored)

SQLite with three tables:
1. `chunks` — `rowid, chunk_id TEXT, text TEXT, path, journal_id, date, title, tags, category, status, section_heading, chunk_index` (metadata + raw text).
2. `vec_chunks` — `sqlite-vec` virtual table (`vec0`), `embedding float[<DIM>]`, keyed by `chunks.rowid`.
3. `fts_chunks` — FTS5 virtual table over `text` (+ `title`, `section_heading`) for BM25, keyed by `chunks.rowid`.
4. `meta` — `key, value` (embedding model, dim, build timestamp, corpus globs, git SHA at build).

---

## 5. `build_index.py` — the offline indexer

CLI: `python build_index.py [--model ...] [--include-commits] [--db index.db]`
Flow: collect (§1) → chunk (§2) → embed (§3) → write `chunks` + `vec_chunks` + `fts_chunks`
+ `meta` (§4). Idempotent: rebuild from scratch each run (corpus is small; simplest correct
thing). Print a summary (`N docs, M chunks, model, dim, elapsed`).

---

## 6. `rag_query.py` — the online query CLI (THE interface Claude sessions call)

**Input (argv/flags):**
- positional `query` — the natural-language question (required).
- `--keywords "a b c"` — optional extra exact terms to boost the BM25 side (the calling agent passes identifiers/SHAs it cares about).
- `--top-k N` (default 8) — how many chunks to return.
- `--db index.db`.

**Method:**
1. Embed the query (with the correct *query* prefix, §3).
2. **Vector search:** top-50 from `vec_chunks` by cosine/L2.
3. **Keyword search:** top-50 from `fts_chunks` via FTS5 BM25 on `query` + `--keywords`.
4. **Fuse with RRF:** `score(d) = Σ 1/(k + rank_i(d))` over the two lists, `k=60`. Sort; take top-`k`.
5. Output.

**Output (stdout, JSON — stable contract; this is what the rule documents):**
```json
{
  "query": "...",
  "results": [
    {
      "rank": 1,
      "score": 0.031,
      "text": "<the chunk text>",
      "journal_id": "2026-06-24-coplanar-import-plane-ids-audit",
      "title": "...",
      "date": "2026-06-24",
      "section": "The resolution: the consumer already applies the lateral gate",
      "tags": ["boundary-corners","plane_ids","..."],
      "path": "docs/journal/2026/06/2026-06-24-....md"
    }
  ]
}
```
Keep `text` full (don't truncate) — the consumer is an LLM that wants the passage. Also
support `--format text` for a human-readable dump (path + section + snippet).

---

## 7. Validation — golden queries (must return the right entry in top-k)

After building, run these and confirm the named entry/section appears in the top results
(they're things we KNOW are in the corpus):
- "why was the flashing ridge type renamed" → `2026-06-24-coplanar-import...` is wrong; expect the F2 commit/journal discussing coplanar rename + the `2026-06-20` close-out. (coplanar ≠ flashing-rename source — verify it finds the *DOF/coplanar* discussion.)
- "is the coplanar-import a bug" → `2026-06-24-coplanar-import-plane-ids-audit` (top-1 ideally).
- "what is the make-shared cascade" → `2026-06-20-in-loop-make-shared-cascade-complete` + `2026-06-14-in-loop-make-shared-design`.
- "DOF follows topology not proximity" → the §4.6 / F3 resolution.
- "planes_dyn deleted single source of truth" → the Phase B design §2.4 / cascade entry.
- Exact-identifier test (BM25 path): `--keywords "RIDGE_TYPE_COPLANAR"` → the F2 entry.
Write these as a `make test` / `python validate.py` that asserts the expected `journal_id`
is within top-k; this is the regression net.

---

## 8. The Claude rule — `.claude/rules/rag-journal-search.md`

Create a project rule (checked in) so every session knows the tool exists and when to use
it. Draft content:

> # Project knowledge search (RAG over journals + docs)
> When you need context on a past decision, finding, design rationale, or "have we seen
> this before?" — BEFORE re-deriving it or grepping blind — query the local knowledge index:
>
> ```bash
> python tools/rag/rag_query.py "<your question>" [--keywords "Identifier1 SHA2"] [--top-k 8]
> ```
> Returns JSON: relevant journal/doc **chunks with metadata** (`journal_id`, `title`,
> `date`, `section`, `path`). Cite the `journal_id` when you use a result. Pass exact
> identifiers/SHAs via `--keywords` (BM25 catches those). The index covers
> `docs/journal/**` + reference docs; rebuild after adding journals
> with `python tools/rag/build_index.py`. It is retrieval only — read the chunks and reason
> yourself.

---

## 9. File layout to create

```
tools/rag/
  BUILD-PLAN.md          # this file
  requirements.txt       # fastembed, sqlite-vec, pyyaml  (+ numpy)
  build_index.py         # §5
  rag_query.py           # §6
  chunk.py               # §2 (shared by both)
  validate.py            # §7
  index.db               # built artifact — GITIGNORE it
  README.md              # 5-line how-to (build, query, reindex)
.claude/rules/rag-journal-search.md   # §8
.gitignore               # add tools/rag/index.db and the model cache dir
```

---

## 10. Out of scope for v1 (note for whoever extends it)
- Reranking (BGE-reranker over top-N) — the next quality lever if recall feels weak.
- A second, code-aware index (semantic code search) — separate concern; v1 is prose only.
- MCP server wrapper — the CLI + rule is sufficient; only build MCP if non-Bash consumers need it.
- Auto-reindex git hook — for now reindex manually (or wire into the existing
  `rebuild-journal-index.js` flow).
- Embedding-model A/B on the golden set — worth doing if v1 retrieval disappoints
  (remember: MTEB rank ≠ in-domain performance).

## Acceptance criteria
- `python tools/rag/build_index.py` builds `index.db` from the corpus, prints a summary, CPU-only, no GPU.
- `python tools/rag/rag_query.py "is the coplanar-import a bug"` returns the F4 audit entry in the top results as JSON.
- `python tools/rag/validate.py` passes all golden queries.
- `.claude/rules/rag-journal-search.md` exists; `index.db` is gitignored.
- No external API calls, no data leaves the machine.
