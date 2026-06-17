/* spytial-core layout pipeline.

   Emacs (`web-tldraw-spytial') sends a `tldraw_layout' message carrying
   relational DATA (JSONDataInstance format: {atoms, relations}) and a
   YAML CnD SPEC.  We run spytial-core's pipeline to get a qualitative
   InstanceLayout (nodes + edges + ordering constraints — note: tldraw's
   LayoutNode carries no x/y), solve node positions from the
   constraints, and build native tldraw shapes so the result is a real,
   keyboard-navigable board.  The new shapes echo back to the Emacs
   buffer through the normal snapshot path, so layouts persist.

   spytial-core ships as a global IIFE bundle (its package "." export is
   a classic script that assigns `var spytialcore'), so it is loaded via
   a <script> tag rather than a named ESM import.  */

import { createShapeId, createBindingId, toRichText } from 'tldraw';

/* The global IIFE bundle is vendored into public/vendor (see
   tools note in package.json); served as a classic script it defines
   the global `spytialcore' namespace and registers the spytial custom
   elements.  Loaded lazily on first layout request.  */
const SPYTIAL_URL = './vendor/spytial-core.global.js';

let enginePromise = null;

function loadEngine () {
  if (enginePromise) return enginePromise;
  enginePromise = new Promise((resolve, reject) => {
    if (globalThis.spytialcore) { resolve(globalThis.spytialcore); return; }
    const s = document.createElement('script');
    s.src = SPYTIAL_URL;
    s.onload = () => resolve(globalThis.spytialcore);
    s.onerror = () => reject(new Error('failed to load spytial-core'));
    document.head.appendChild(s);
  });
  return enginePromise;
}

/* Longest-path layering from the qualitative ordering constraints.
   TopConstraint {top, bottom}: bottom sits below top (greater y).
   LeftConstraint {left, right}: right sits right of left (greater x).
   Nodes with no incoming ordering get rank 0; others get
   1 + max(predecessor rank).  Falls back to a grid when there are no
   constraints at all.  */
function rank (ids, pairs) {
  const rankOf = new Map(ids.map(id => [id, 0]));
  const succ = new Map(ids.map(id => [id, []]));
  for (const [a, b] of pairs) {
    if (succ.has(a)) succ.get(a).push(b);
  }
  /* Iterate to a fixed point (DAG; bounded by node count).  */
  for (let i = 0; i < ids.length; i++) {
    let changed = false;
    for (const [a, b] of pairs) {
      const want = (rankOf.get(a) || 0) + 1;
      if ((rankOf.get(b) || 0) < want) { rankOf.set(b, want); changed = true; }
    }
    if (!changed) break;
  }
  return rankOf;
}

function buildFromLayout (editor, layout, ctx) {
  const nodes = (layout && layout.nodes) || [];
  const edges = (layout && layout.edges) || [];
  const constraints = (layout && layout.constraints) || [];
  if (!nodes.length) {
    console.warn('spytial: layout produced no nodes', layout);
    return;
  }

  const nid = (x) => (x && x.id != null ? x.id : x);
  const ids = nodes.map(n => n.id);
  const below = [];   /* [top, bottom] */
  const rightOf = []; /* [left, right] */
  for (const c of constraints) {
    if (c == null) continue;
    if (c.top != null && c.bottom != null) below.push([nid(c.top), nid(c.bottom)]);
    else if (c.left != null && c.right != null) rightOf.push([nid(c.left), nid(c.right)]);
  }

  const groups = (layout && layout.groups) || [];
  const COL = 240, ROW = 120;
  const xRank = rank(ids, rightOf);
  const yRank = rank(ids, below);
  const haveConstraints = below.length || rightOf.length;

  /* Compute every node's box first so groups can be drawn (behind) as
     bounding containers around their members.  */
  const pos = new Map();
  nodes.forEach((n, i) => {
    const x = haveConstraints ? (xRank.get(n.id) || 0) * COL : (i % 6) * COL;
    const y = haveConstraints ? (yRank.get(n.id) || 0) * ROW : Math.floor(i / 6) * ROW;
    pos.set(n.id, { x, y, w: Math.max(80, n.width || 140),
                    h: Math.max(40, n.height || 56) });
  });

  const shapeOf = new Map();
  editor.run(() => {
    /* Groups first so they sit behind the nodes (z-order = creation
       order).  Each group is a labelled dashed container around its
       members' bounding box.  */
    const GPAD = 28;
    for (const g of groups) {
      const members = (g.nodeIds || g.atoms || [])
        .map(id => pos.get(nid(id))).filter(Boolean);
      if (!members.length) continue;
      const x0 = Math.min(...members.map(m => m.x)) - GPAD;
      const y0 = Math.min(...members.map(m => m.y)) - GPAD;
      const x1 = Math.max(...members.map(m => m.x + m.w)) + GPAD;
      const y1 = Math.max(...members.map(m => m.y + m.h)) + GPAD;
      editor.createShape({
        id: createShapeId(), type: 'geo',
        x: x0, y: y0,
        props: { geo: 'rectangle', w: x1 - x0, h: y1 - y0,
                 dash: 'dashed', fill: 'none', color: 'grey', size: 's',
                 labelColor: 'grey', verticalAlign: 'start',
                 richText: toRichText(String(g.name || g.label || 'group')) },
      });
    }

    nodes.forEach((n) => {
      const p = pos.get(n.id);
      const sid = createShapeId();
      shapeOf.set(n.id, sid);
      editor.createShape({
        id: sid, type: 'geo', x: p.x, y: p.y,
        props: {
          geo: 'rectangle', w: p.w, h: p.h, dash: 'solid', size: 's',
          font: 'mono',
          richText: toRichText(String(n.label != null ? n.label : n.id)),
        },
        /* Carry the source atom id so the host (e.g. web-tldraw-dired)
           can correlate a selected shape back to its data node.  */
        meta: { spytialId: String(n.id) },
      });
    });

    for (const e of edges) {
      /* Edges carry full source/target node objects, not ids.  */
      const srcId = e.source && (e.source.id != null ? e.source.id : e.source);
      const dstId = e.target && (e.target.id != null ? e.target.id : e.target);
      const from = shapeOf.get(srcId), to = shapeOf.get(dstId);
      if (!from || !to || from === to) continue;
      const arrowId = createShapeId();
      editor.createShape({ id: arrowId, type: 'arrow' });
      editor.createBindings([
        { id: createBindingId(), fromId: arrowId, toId: from, type: 'arrow',
          props: { terminal: 'start', normalizedAnchor: { x: 0.5, y: 0.5 },
                   isExact: false, isPrecise: false } },
        { id: createBindingId(), fromId: arrowId, toId: to, type: 'arrow',
          props: { terminal: 'end', normalizedAnchor: { x: 0.5, y: 0.5 },
                   isExact: false, isPrecise: false } },
      ]);
    }
  });
  editor.zoomToFit({ animation: { duration: 200 } });
}

export async function applyLayout (editor, msg, ctx) {
  let sc;
  try {
    sc = await loadEngine();
  } catch (e) {
    console.error('spytial: engine load failed:', e);
    return;
  }
  if (!sc || !sc.JSONDataInstance) {
    console.error('spytial: engine missing expected exports', sc && Object.keys(sc));
    return;
  }
  try {
    const { JSONDataInstance, parseLayoutSpec, SGraphQueryEvaluator,
            LayoutInstance } = sc;
    const instance = new JSONDataInstance(msg.data);
    let spec;
    try { spec = parseLayoutSpec(msg.spec || ''); }
    catch (specErr) {
      console.warn('spytial: spec parse failed, using empty spec:', specErr);
      spec = parseLayoutSpec('');
    }
    const evaluator = new SGraphQueryEvaluator();
    evaluator.initialize({ sourceData: instance });
    const result = new LayoutInstance(spec, evaluator).generateLayout(instance);
    if (result && result.error) {
      console.error('spytial: layout error:', result.error);
    }
    /* Re-layouts (e.g. dired fold/unfold) replace the whole board.  */
    if (msg.replace) {
      const all = editor.getCurrentPageShapes().map(s => s.id);
      if (all.length) editor.deleteShapes(all);
    }
    /* generateLayout returns {layout, error, ...}; the InstanceLayout
       (nodes/edges/constraints) is nested under .layout.  */
    buildFromLayout(editor, (result && result.layout) || result, ctx);
  } catch (e) {
    console.error('spytial: pipeline failed:', e);
  }
}
