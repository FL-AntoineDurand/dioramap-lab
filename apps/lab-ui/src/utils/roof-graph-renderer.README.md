# Per-step roof-graph viz delegate template

Each boundary-* step that emits a `<step>-roof-graph` artifact
gets a 5-line delegate viz file under
`v3/steps/v7/viz/<step>-roof-graph.viz.js`:

```js
// step-visualizer: <step>-roof-graph — section-aware roof-graph renderer.
import { renderRoofGraph } from '../../../apps/lab-ui/src/utils/roof-graph-renderer.js';

export function render(ctx, data) {
  renderRoofGraph(ctx, data.bytes, '<step>');
}
```

Layers land under "Roof Graph / `<step>` / {ridges|corners|edges|…}",
with the renderer probing which sections the artifact contains and
emitting one layer per primitive.

That's it. Section-by-section visual evolution across the chain
(boundary-signals → boundary-corners → … → boundary-regularize)
appears as a stack of toggleable subcategories in the layer tree;
the user can keep multiple snapshots visible simultaneously to
diff them.
