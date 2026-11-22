# @dioramap/viewer-sdk

Production embed for the Dioramap viewer. Closed allow-list consumer of
`@dioramap/scene-base` and `@dioramap/protocol`.

Source plan: [`docs/plan/PLAN-VIEWER-SDK-PORT.md`](../../docs/plan/PLAN-VIEWER-SDK-PORT.md).

## Status

V2 (scaffold + WS + sun-only controls). Composer rendering, models system,
loading bar, sun-direction artifact init all land in subsequent milestones —
see `CHANGELOG.md`.

## Local dev

```sh
cd apps/viewer-sdk
npm install
npm run dev   # → http://127.0.0.1:5183
```

The dev page connects to `ws://<host>/ws` by default. Override via:

```
http://127.0.0.1:5183/?lat=48.85&lon=2.34&backend=ws://127.0.0.1:8080/ws&radius=75
```

## Library build

```sh
npm run build
```

Outputs `dist/dioramap-viewer.js` (ES + UMD `window.DioramapViewer`). Three.js is
bundled in (one canonical copy) so customer sites can drop a single `<script>`.

## Public API

`DioramapViewer.init({...})` — see `src/sdk.js` for the full signature, frozen
per PLAN-VIEWER-SDK-PORT §5.7.
