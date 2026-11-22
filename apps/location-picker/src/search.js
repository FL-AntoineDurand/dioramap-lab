// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// Address search bar — French BAN API geocoding.

const BAN_URL = 'https://api-adresse.data.gouv.fr/search/';

/**
 * Create a search bar overlay inside containerEl.
 * On result select, calls onSelect({ lat, lon, label }).
 */
export function createSearch(containerEl, { onSelect }) {
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

  async function fetchResults(query) {
    try {
      const res = await fetch(`${BAN_URL}?q=${encodeURIComponent(query)}&limit=5`);
      if (!res.ok) return;
      const data = await res.json();
      renderResults(data.features || []);
    } catch { /* network error — silently ignore */ }
  }

  function renderResults(features) {
    if (features.length === 0) { dropdown.classList.remove('is-active'); return; }
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
