// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// Address search bar — French BAN API geocoding.

const BAN_URL = 'https://api-adresse.data.gouv.fr/search/';

/**
 * Create a search bar overlay inside containerEl.
 * On result select, calls onSelect({ lat, lon, label }).
 */
export function createSearch(containerEl, { onSelect, onError }) {
  const fr = /^fr\b/i.test((typeof document !== 'undefined' && document.documentElement?.lang) ||
                           (typeof navigator !== 'undefined' && navigator.language) || '');
  const wrapper = document.createElement('div');
  wrapper.className = 'dioramap-picker-search';

  const input = document.createElement('input');
  input.type = 'text';
  input.placeholder = 'Search address…';
  input.className = 'dioramap-picker-search-input';

  const dropdown = document.createElement('div');
  dropdown.className = 'dioramap-picker-search-dropdown';

  wrapper.appendChild(input);
  wrapper.appendChild(dropdown);
  containerEl.appendChild(wrapper);

  let debounceTimer = null;

  input.addEventListener('input', () => {
    clearTimeout(debounceTimer);
    const q = input.value.trim();
    if (q.length < 3) { dropdown.classList.remove('is-active'); return; }
    debounceTimer = setTimeout(() => fetchResults(q), 300);
  });

  input.addEventListener('keydown', (e) => {
    if (e.key === 'Escape') {
      dropdown.classList.remove('is-active');
      input.blur();
    }
  });

  let seq = 0;
  async function fetchResults(query) {
    const mine = ++seq;
    const url = `${BAN_URL}?q=${encodeURIComponent(query)}&limit=5`;
    let why;
    try {
      const res = await fetch(url);
      if (res.ok) {
        const data = await res.json();
        if (mine === seq) renderResults(data.features || []);
        return;
      }
      let body = '';
      try { body = (await res.text()).slice(0, 300); } catch { /* ignore */ }
      why = `HTTP ${res.status} from api-adresse.data.gouv.fr${body ? `: ${body}` : ''}`;
    } catch (e) {
      why = `api-adresse.data.gouv.fr unreachable: ${e?.message ?? e}`;
    }
    if (mine !== seq) return;
    // Visible, not silent: the user otherwise just sees "nothing happens".
    console.warn('[location-picker] address search failed:', why);
    renderMessage(fr ? 'Recherche d’adresse indisponible pour le moment — cliquez sur la carte.'
                     : 'Address search is unavailable right now — click on the map instead.', why);
    if (onError) {
      const err = new Error(why);
      err.code = 'GEOCODER_FAILED';
      err.detail = { url, query };
      try { onError(err); } catch { /* embedder handler */ }
    }
  }

  function renderMessage(text, title) {
    dropdown.innerHTML = '';
    const item = document.createElement('div');
    item.className = 'dioramap-picker-search-item is-message';
    item.textContent = text;
    if (title) item.title = title;
    dropdown.appendChild(item);
    dropdown.classList.add('is-active');
  }

  function renderResults(features) {
    if (features.length === 0) {
      renderMessage(fr ? 'Aucune adresse trouvée' : 'No address found');
      return;
    }
    dropdown.innerHTML = '';
    dropdown.classList.add('is-active');

    for (const f of features) {
      const item = document.createElement('div');
      item.textContent = f.properties.label;
      item.className = 'dioramap-picker-search-item';
      // Hover background is in CSS (.dioramap-picker-search-item:hover)
      // so embedders can theme via --dioramap-picker-hover-bg.
      item.addEventListener('click', () => {
        const [lon, lat] = f.geometry.coordinates;
        input.value = f.properties.label;
        dropdown.classList.remove('is-active');
        onSelect({ lat, lon, label: f.properties.label });
      });
      dropdown.appendChild(item);
    }
  }

  return { input, focus: () => input.focus() };
}
