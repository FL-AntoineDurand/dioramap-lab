# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Antoine Durand
"""Markdown-section-aware chunking for the journal/docs RAG.

See BUILD-PLAN.md §2. A "chunk" is one markdown section (heading + body),
size-normalized: huge sections are split on paragraph breaks (~300-500 tokens
each, heading repeated for context), tiny sections are merged with the next.

Token counting uses the cheap len//4 heuristic (no tokenizer dependency).
"""

import os
import re

try:
    import yaml
except ImportError:  # only needed for frontmatter; build_index pins the dep
    yaml = None

CHARS_PER_TOK = 4
MAX_TOK, MIN_TOK = 500, 80
MAX_CHARS = MAX_TOK * CHARS_PER_TOK  # ~2000
MIN_CHARS = MIN_TOK * CHARS_PER_TOK  # ~320

_HEADING_RE = re.compile(r"^(#{2,4})\s+(.*\S)\s*$")
_FM_RE = re.compile(r"^---\s*\n(.*?)\n---\s*\n?", re.DOTALL)


def parse_frontmatter(text):
    """Return (meta_dict, body). meta is {} when there's no YAML frontmatter."""
    m = _FM_RE.match(text)
    if not m:
        return {}, text
    meta = {}
    if yaml is not None:
        try:
            meta = yaml.safe_load(m.group(1)) or {}
        except yaml.YAMLError:
            meta = {}
    return meta, text[m.end():]


def _slugify(s):
    s = re.sub(r"[^\w\s-]", "", s.lower()).strip()
    s = re.sub(r"[\s_-]+", "-", s)
    return s[:60] or "section"


def _doc_title_line(body):
    """The leading '# Title' line, if the body opens with one."""
    m = re.match(r"^#\s+(.*\S)\s*$", body.lstrip("\n"), re.MULTILINE)
    return m.group(1) if m else None


def _split_sections(body):
    """Split body into [(heading_line, section_body)] on level-2..4 headings.

    Content before the first such heading is the preamble; its heading_line is
    the document's own '# Title' line if present, else ''.
    """
    lines = body.splitlines()
    sections = []
    cur_heading = ""
    cur_buf = []
    started = False
    for ln in lines:
        m = _HEADING_RE.match(ln)
        if m:
            if started or any(s.strip() for s in cur_buf):
                sections.append((cur_heading, "\n".join(cur_buf).strip()))
            cur_heading = ln.strip()
            cur_buf = []
            started = True
        else:
            cur_buf.append(ln)
    if started or any(s.strip() for s in cur_buf):
        sections.append((cur_heading, "\n".join(cur_buf).strip()))
    return sections


def _split_big(body):
    """Split an oversized body into <=MAX_CHARS pieces on paragraph breaks."""
    paras = re.split(r"\n\s*\n", body)
    pieces, cur = [], ""
    for p in paras:
        p = p.strip()
        if not p:
            continue
        if cur and len(cur) + len(p) + 2 > MAX_CHARS:
            pieces.append(cur)
            cur = p
        else:
            cur = f"{cur}\n\n{p}" if cur else p
    if cur:
        pieces.append(cur)
    # hard-split any single paragraph that is still too long
    out = []
    for p in pieces:
        if len(p) <= MAX_CHARS:
            out.append(p)
        else:
            out.extend(p[i:i + MAX_CHARS] for i in range(0, len(p), MAX_CHARS))
    return out or [""]


def _merge_small(sections):
    """Merge sections shorter than MIN_CHARS into the following section."""
    merged = []
    carry = None  # (heading, body) waiting to be prepended to the next section
    for heading, sbody in sections:
        if carry is not None:
            ch, cb = carry
            heading = ch or heading
            sbody = (cb + "\n\n" + sbody).strip() if cb else sbody
            carry = None
        if len(heading) + len(sbody) < MIN_CHARS:
            carry = (heading, sbody)
            continue
        merged.append((heading, sbody))
    if carry is not None:
        if merged:  # fold the trailing scrap into the previous section
            ph, pb = merged[-1]
            merged[-1] = (ph, (pb + "\n\n" + carry[1]).strip())
        else:
            merged.append(carry)
    return merged


def chunk_document(rel_path, raw_text, meta_override=None):
    """Chunk one document into records (see BUILD-PLAN.md §2 for the shape).

    meta_override: when given (e.g. for synthetic commit docs), it is used
    verbatim as the doc metadata and frontmatter parsing is skipped.
    """
    if meta_override is None:
        doc_meta, body = parse_frontmatter(raw_text)
    else:
        doc_meta, body = meta_override, raw_text

    title = doc_meta.get("title") or _doc_title_line(body) or os.path.basename(rel_path)
    tags = doc_meta.get("tags") or []
    if isinstance(tags, str):
        tags = [t.strip() for t in tags.split(",") if t.strip()]

    sections = _merge_small(_split_sections(body))

    records = []
    idx = 0
    for heading_line, sbody in sections:
        heading_text = _HEADING_RE.match(heading_line)
        section_heading = heading_text.group(2) if heading_text else (
            re.sub(r"^#\s+", "", heading_line).strip() or "(preamble)"
        )
        full = f"{heading_line}\n{sbody}".strip() if heading_line else sbody
        pieces = _split_big(full) if len(full) > MAX_CHARS else [full]
        multi = len(pieces) > 1
        for j, piece in enumerate(pieces):
            text = piece
            # repeat the heading on continuation pieces so context survives
            if multi and j > 0 and heading_line:
                text = f"{heading_line}\n{piece}"
            if not text.strip():
                continue
            records.append({
                "chunk_id": f"{rel_path}#{_slugify(section_heading)}#{idx}",
                "text": text,
                "path": rel_path,
                "journal_id": doc_meta.get("id"),
                "date": str(doc_meta["date"]) if doc_meta.get("date") else None,
                "title": title,
                "tags": tags,
                "category": doc_meta.get("category"),
                "status": doc_meta.get("status"),
                "section_heading": section_heading,
                "chunk_index": idx,
            })
            idx += 1
    return records
