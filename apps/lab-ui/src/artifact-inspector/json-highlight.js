// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// JSON syntax highlighter — single-file swap point.
//
// Public API:
//   highlightJson(jsonString) -> string (HTML, already escaped)
//
// The implementation is a ~20-line dependency-free regex tokenizer
// that's enough for the structured payloads the inspector emits
// (numbers, strings, booleans, null, object keys). If we ever hit
// content that needs richer grammar (comments, custom syntaxes,
// embedded code), drop in prismjs:
//
//   import Prism from 'prismjs';
//   import 'prismjs/components/prism-json';
//   export function highlightJson(s) {
//     return Prism.highlight(s, Prism.languages.json, 'json');
//   }
//
// Token CSS class names match prism's convention (`token property`,
// `token string`, …) so the existing stylesheet keeps working through
// the swap with zero edits. Keep them stable.

const ESC_RE = /[&<>]/g;
const ESC_MAP = { '&': '&amp;', '<': '&lt;', '>': '&gt;' };

function escapeHtml(s) {
  return s.replace(ESC_RE, (c) => ESC_MAP[c]);
}

// Order matters: strings first (so we don't number-match digits inside
// strings), then keywords, then numbers. Strings + immediately-following
// `:` get tagged as `property`, otherwise as `string`.
const TOKEN_RE =
  /("(?:\\u[\da-fA-F]{4}|\\[^u]|[^\\"])*")(\s*:)?|\b(true|false|null)\b|(-?\d+(?:\.\d*)?(?:[eE][+-]?\d+)?)/g;

export function highlightJson(jsonString) {
  const escaped = escapeHtml(jsonString);
  return escaped.replace(TOKEN_RE, (match, str, colon, kw, num) => {
    if (str !== undefined) {
      if (colon) {
        return `<span class="token property">${str}</span>${colon}`;
      }
      return `<span class="token string">${str}</span>`;
    }
    if (kw !== undefined) {
      const cls = kw === 'null' ? 'null' : 'boolean';
      return `<span class="token ${cls}">${kw}</span>`;
    }
    if (num !== undefined) {
      return `<span class="token number">${num}</span>`;
    }
    return match;
  });
}
