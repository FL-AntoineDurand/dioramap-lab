// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// error-messages.js — end-user wording for DioramapError codes
// (docs/ERRORS.md). The technical message stays available under
// "Technical details"; this is the sentence a visitor reads first.
//
// Locale: French when the embedding page (or the browser) is French,
// English otherwise. Embedders can force one with init({ locale }).

const TEXT = {
  en: {
    NO_DATA: ['No data for this place yet', 'IGN’s open LiDAR HD coverage does not include this spot yet. Try a nearby place.'],
    INVALID_INPUT: ['This place can’t be shown', 'The requested location is outside the area the engine covers (metropolitan France).'],
    UPSTREAM: ['The IGN data service did not answer properly', 'This is usually temporary. Try again in a few minutes.'],
    TIMEOUT: ['The IGN data service is too slow right now', 'Try again in a few minutes.'],
    CANCELLED: ['Computation cancelled', ''],
    CONNECTION_FAILED: ['Can’t reach the 3D server', 'Check your connection, or try again later.'],
    CONNECTION_LOST: ['Connection to the 3D server was lost', 'Try again.'],
    STALLED: ['The 3D server stopped responding', 'Try again. If it keeps happening, please report it.'],
    AUTH: ['Access refused by the 3D server', 'The access key is missing or not valid for this site.'],
    BUSY: ['A computation is already running', 'Wait for it to finish, then try again.'],
    DEFAULT: ['Something went wrong while building the 3D scene', 'Please report it so it can be fixed.'],
    details: 'Technical details — for developers, include them in a bug report',
    copy: 'Copy', copied: 'Copied', report: 'Report on GitHub', retry: 'Try again',
  },
  fr: {
    NO_DATA: ['Pas encore de données pour ce lieu', 'La couverture LiDAR HD ouverte de l’IGN n’inclut pas encore cet endroit. Essayez un lieu voisin.'],
    INVALID_INPUT: ['Ce lieu ne peut pas être affiché', 'L’emplacement demandé est hors de la zone couverte (France métropolitaine).'],
    UPSTREAM: ['Le service de données de l’IGN n’a pas répondu correctement', 'C’est généralement temporaire. Réessayez dans quelques minutes.'],
    TIMEOUT: ['Le service de données de l’IGN est trop lent en ce moment', 'Réessayez dans quelques minutes.'],
    CANCELLED: ['Calcul annulé', ''],
    CONNECTION_FAILED: ['Impossible de joindre le serveur 3D', 'Vérifiez votre connexion ou réessayez plus tard.'],
    CONNECTION_LOST: ['Connexion au serveur 3D perdue', 'Réessayez.'],
    STALLED: ['Le serveur 3D ne répond plus', 'Réessayez. Si cela se reproduit, merci de le signaler.'],
    AUTH: ['Accès refusé par le serveur 3D', 'La clé d’accès est absente ou non valide pour ce site.'],
    BUSY: ['Un calcul est déjà en cours', 'Attendez qu’il se termine puis réessayez.'],
    DEFAULT: ['Un problème est survenu pendant la construction de la scène 3D', 'Merci de le signaler pour qu’il soit corrigé.'],
    details: 'Détails techniques — pour les développeurs, à joindre au signalement',
    copy: 'Copier', copied: 'Copié', report: 'Signaler sur GitHub', retry: 'Réessayer',
  },
};

const GROUP = {
  UPSTREAM_HTTP: 'UPSTREAM', UPSTREAM_UNREACHABLE: 'UPSTREAM', UPSTREAM_FORMAT: 'UPSTREAM',
  AUTH_REQUIRED: 'AUTH', AUTH_FAILED: 'AUTH', FORBIDDEN_ARTIFACT: 'AUTH',
  RUN_BUSY: 'BUSY',
};

export function resolveLocale(forced) {
  if (forced === 'fr' || forced === 'en') return forced;
  const lang = (typeof document !== 'undefined' && document.documentElement?.lang) ||
               (typeof navigator !== 'undefined' && navigator.language) || 'en';
  return /^fr\b/i.test(lang) ? 'fr' : 'en';
}

/** @returns {{title: string, body: string, ui: Object, retryable: boolean}} */
export function describeError(err, locale) {
  const L = TEXT[locale] ?? TEXT.en;
  const key = GROUP[err?.code] ?? err?.code;
  const [title, body] = L[key] ?? L.DEFAULT;
  const retryable = !['NO_DATA', 'INVALID_INPUT', 'AUTH'].includes(key);
  return { title, body, ui: L, retryable };
}
