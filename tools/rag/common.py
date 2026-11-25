# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Antoine Durand
"""Shared plumbing for the journal/docs RAG: venv re-exec, model registry,
SQLite+sqlite-vec connection, and the fastembed embedder.

Query vs document asymmetry (BUILD-PLAN.md §3) is handled by fastembed's own
``query_embed`` / ``passage_embed``, which apply each model's correct
instruction prefix (bge's "Represent this sentence…", nomic's search_query/
search_document). We don't hardcode prefixes — we ask the model card via the API.
"""

import hashlib
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))

# Per-repo invocation: set RAG_REPO_ROOT=/path/to/other-repo to index a repo
# other than this one. Index + cache then live in ~/.cache/rag-index/<basename>/
# so nothing dirties the target repo. Backward-compatible: if the env is unset,
# indexes this repository (REPO_ROOT = two levels up, DB in HERE).
_env_root = os.environ.get("RAG_REPO_ROOT", "").strip()
if _env_root:
    REPO_ROOT = os.path.abspath(_env_root)
    _data_dir = os.environ.get("RAG_DATA_DIR") or os.path.join(
        os.path.expanduser("~/.cache/rag-index"), os.path.basename(REPO_ROOT.rstrip("/")))
    os.makedirs(_data_dir, exist_ok=True)
    DEFAULT_DB = os.path.join(_data_dir, "index.db")
    DEFAULT_CACHE = os.path.join(_data_dir, "embed_cache.db")
else:
    REPO_ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
    DEFAULT_DB = os.path.join(HERE, "index.db")
    DEFAULT_CACHE = os.path.join(HERE, ".embed_cache.db")

VENV_PY = os.path.join(HERE, ".venv", "bin", "python")


def reexec_into_venv():
    """Re-exec under the bundled venv if we're not already there.

    Lets the documented `python tools/rag/<script>.py` invocation work even
    when the caller's python lacks the deps (the system python has no pip).
    """
    if os.environ.get("RAG_NO_REEXEC"):
        return
    if os.path.abspath(sys.executable) == os.path.abspath(VENV_PY):
        return
    if os.path.exists(VENV_PY):
        os.environ["RAG_NO_REEXEC"] = "1"
        os.execv(VENV_PY, [VENV_PY, *sys.argv])


# name -> {hf model id, embedding dimensions}. Prefixes are handled by fastembed.
MODELS = {
    "bge-large-en-v1.5": {"hf": "BAAI/bge-large-en-v1.5", "dim": 1024},
    "bge-base-en-v1.5": {"hf": "BAAI/bge-base-en-v1.5", "dim": 768},
    "nomic-embed-text-v1.5": {"hf": "nomic-ai/nomic-embed-text-v1.5", "dim": 768},
}
DEFAULT_MODEL = "bge-large-en-v1.5"


def model_config(name):
    if name not in MODELS:
        raise SystemExit(
            f"unknown model {name!r}; choose one of: {', '.join(MODELS)}"
        )
    return MODELS[name]


def connect(db_path, create=False):
    """Open the SQLite DB with the sqlite-vec extension loaded."""
    import sqlite3

    import sqlite_vec

    if not create and not os.path.exists(db_path):
        raise SystemExit(
            f"index not found: {db_path}\nbuild it first: "
            f"python {os.path.relpath(os.path.join(HERE, 'build_index.py'), REPO_ROOT)}"
        )
    db = sqlite3.connect(db_path)
    db.enable_load_extension(True)
    sqlite_vec.load(db)
    db.enable_load_extension(False)
    db.row_factory = sqlite3.Row
    return db


class Embedder:
    """Thin wrapper over fastembed.TextEmbedding with query/doc asymmetry."""

    def __init__(self, model_name):
        from fastembed import TextEmbedding

        cfg = model_config(model_name)
        self.name = model_name
        self.dim = cfg["dim"]
        # trust_remote_code is needed by nomic; harmless for bge.
        self.model = TextEmbedding(model_name=cfg["hf"], trust_remote_code=True)

    def embed_documents(self, texts, batch_size=64):
        import numpy as np

        out = self.model.passage_embed(list(texts), batch_size=batch_size)
        return [np.asarray(v, dtype="float32") for v in out]

    def embed_query(self, text):
        import numpy as np

        vec = next(iter(self.model.query_embed([text])))
        return np.asarray(vec, dtype="float32")


class EmbedCache:
    """Content-addressed embedding cache, persisted across rebuilds.

    Keyed by sha256(model | dim | chunk text), value is the sqlite-vec serialized
    float32 blob (insertable straight into vec_chunks). This is what makes a
    rebuild cheap: only chunk texts that are genuinely new — an edited journal
    section, a new doc, a fresh commit message — miss and get embedded. Unchanged
    content (the other ~99% on a typical edit) is a cache hit. Lives in its own
    file (NOT wiped with index.db) and self-prunes entries unused for a while.
    """

    def __init__(self, path, model, dim):
        import sqlite3

        self.model, self.dim = model, dim
        self.db = sqlite3.connect(path)
        self.db.execute(
            "CREATE TABLE IF NOT EXISTS embed_cache ("
            "text_hash TEXT PRIMARY KEY, model TEXT, dim INTEGER, "
            "vec BLOB, last_used INTEGER)"
        )
        self.db.commit()

    def key(self, text):
        h = hashlib.sha256()
        h.update(f"{self.model}\x00{self.dim}\x00".encode())
        h.update(text.encode("utf-8"))
        return h.hexdigest()

    def get_many(self, keys):
        """Return {key: blob} for the subset of keys already cached."""
        hits = {}
        cur = self.db.cursor()
        for i in range(0, len(keys), 900):  # stay under SQLite's variable cap
            batch = keys[i:i + 900]
            q = "SELECT text_hash, vec FROM embed_cache WHERE text_hash IN (%s)" % (
                ",".join("?" * len(batch))
            )
            for h, blob in cur.execute(q, batch):
                hits[h] = blob
        return hits

    def put_many(self, items, now):
        """items: iterable of (key, blob)."""
        self.db.executemany(
            "INSERT OR REPLACE INTO embed_cache (text_hash, model, dim, vec, "
            "last_used) VALUES (?, ?, ?, ?, ?)",
            [(k, self.model, self.dim, blob, now) for k, blob in items],
        )
        self.db.commit()

    def touch(self, keys, now):
        for i in range(0, len(keys), 900):
            batch = keys[i:i + 900]
            self.db.execute(
                "UPDATE embed_cache SET last_used=? WHERE text_hash IN (%s)" % (
                    ",".join("?" * len(batch))
                ),
                [now, *batch],
            )
        self.db.commit()

    def prune(self, max_age_days, now):
        cutoff = now - max_age_days * 86400
        n = self.db.execute(
            "DELETE FROM embed_cache WHERE last_used < ?", (cutoff,)
        ).rowcount
        self.db.commit()
        return n

    def close(self):
        self.db.close()
