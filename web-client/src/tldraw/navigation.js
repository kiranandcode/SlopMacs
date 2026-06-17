/* Apply Emacs-issued tldraw commands to a board's Editor.

   Emacs owns the keyboard: `tldraw-mode' binds C-n/C-p/.../RET to elisp
   commands that send { type:"tldraw_cmd", board, verb, args } messages.
   This module turns each verb into tldraw Editor API calls and echoes
   selection/edit state back so the Emacs mode line and the node-edit
   buffer stay in sync.  All geometry (which node is "below" another)
   lives here, where the canvas coordinates are authoritative.  */

import { createShapeId, createBindingId, toRichText,
         AssetRecordType, getHashForString } from 'tldraw';

/* ---- helpers ---- */

function boundsCenter (b) {
  return { x: b.x + b.w / 2, y: b.y + b.h / 2 };
}

function currentId (editor) {
  const sel = editor.getSelectedShapeIds();
  return sel.length ? sel[0] : null;
}

/* Walk ProseMirror-style richText JSON collecting plain text.  */
function richTextToPlain (rt) {
  if (!rt) return '';
  let out = '';
  const walk = (node) => {
    if (!node) return;
    if (typeof node.text === 'string') out += node.text;
    if (Array.isArray(node.content)) {
      node.content.forEach((c, i) => {
        walk(c);
        if (node.type === 'doc' && i < node.content.length - 1) out += '\n';
      });
    }
  };
  walk(rt);
  return out;
}

function shapeText (shape) {
  if (!shape || !shape.props) return '';
  if (shape.props.richText) return richTextToPlain(shape.props.richText);
  if (typeof shape.props.text === 'string') return shape.props.text;
  return '';
}

/* Pick the nearest shape in a cardinal direction from the selection.  */
function pickDirectional (editor, dir) {
  const shapes = editor.getCurrentPageShapes();
  if (!shapes.length) return null;
  const selId = currentId(editor);
  const cur = selId ? editor.getShapePageBounds(selId) : null;
  if (!cur) return shapes[0].id;
  const c = boundsCenter(cur);
  let best = null, bestScore = Infinity;
  for (const s of shapes) {
    if (s.id === selId) continue;
    const b = editor.getShapePageBounds(s.id);
    if (!b) continue;
    const o = boundsCenter(b);
    const dx = o.x - c.x, dy = o.y - c.y;
    let primary, secondary;
    if (dir === 'down')       { if (dy <= 1) continue; primary = dy;  secondary = Math.abs(dx); }
    else if (dir === 'up')    { if (dy >= -1) continue; primary = -dy; secondary = Math.abs(dx); }
    else if (dir === 'right') { if (dx <= 1) continue; primary = dx;  secondary = Math.abs(dy); }
    else /* left */           { if (dx >= -1) continue; primary = -dx; secondary = Math.abs(dy); }
    /* Prefer the aligned axis: weight the off-axis distance heavier.  */
    const score = primary + secondary * 2;
    if (score < bestScore) { bestScore = score; best = s.id; }
  }
  return best;
}

function pickSequential (editor, dir) {
  const shapes = editor.getCurrentPageShapesSorted
    ? editor.getCurrentPageShapesSorted()
    : editor.getCurrentPageShapes();
  if (!shapes.length) return null;
  const selId = currentId(editor);
  let idx = shapes.findIndex(s => s.id === selId);
  if (idx < 0) return shapes[0].id;
  idx = dir === 'prev'
    ? (idx - 1 + shapes.length) % shapes.length
    : (idx + 1) % shapes.length;
  return shapes[idx].id;
}

function focusShape (editor, id, ctx) {
  if (!id) return;
  editor.setSelectedShapes([id]);
  const b = editor.getShapePageBounds(id);
  if (b) editor.centerOnPoint(boundsCenter(b), { animation: { duration: 150 } });
  emitSelection(editor, ctx);
}

function emitSelection (editor, ctx) {
  const id = currentId(editor);
  const shape = id ? editor.getShape(id) : null;
  ctx.send({
    type: 'tldraw_event', board: ctx.boardId, event: 'selection',
    id: id || '', shapeType: shape ? shape.type : '',
    text: shape ? shapeText(shape) : '',
    meta: shape ? (shape.meta || {}) : {},
  });
}

/* Place a node at a position relative to the viewport.  `pos' is
   'center' | 'top' | 'bottom' — like C-l / recenter-top-bottom.  */
function recenter (editor, pos, id) {
  const sid = id || currentId(editor);
  if (!sid) return;
  const b = editor.getShapePageBounds(sid);
  if (!b) return;
  const c = boundsCenter(b);
  const vp = editor.getViewportPageBounds();
  const margin = (b.h || 64) + 40;
  let ty = c.y;
  if (pos === 'top') ty = c.y + vp.h / 2 - margin;       /* node near top */
  else if (pos === 'bottom') ty = c.y - vp.h / 2 + margin; /* node near bottom */
  editor.centerOnPoint({ x: c.x, y: ty }, { animation: { duration: 200 } });
}

/* ---- arrow / edge support ---- */

function createEdge (editor, fromId, toId) {
  if (!fromId || !toId || fromId === toId) return null;
  const a = editor.getShapePageBounds(fromId);
  const b = editor.getShapePageBounds(toId);
  if (!a || !b) return null;
  const ca = boundsCenter(a), cb = boundsCenter(b);
  const arrowId = createShapeId();
  editor.createShape({
    id: arrowId, type: 'arrow',
    props: {
      start: { x: ca.x, y: ca.y },
      end: { x: cb.x, y: cb.y },
    },
  });
  editor.createBindings([
    { id: createBindingId(), fromId: arrowId, toId: fromId, type: 'arrow',
      props: { terminal: 'start', normalizedAnchor: { x: 0.5, y: 0.5 },
               isExact: false, isPrecise: false } },
    { id: createBindingId(), fromId: arrowId, toId: toId, type: 'arrow',
      props: { terminal: 'end', normalizedAnchor: { x: 0.5, y: 0.5 },
               isExact: false, isPrecise: false } },
  ]);
  return arrowId;
}

/* ---- command dispatch ---- */

export function applyCommand (editor, verb, args, ctx) {
  switch (verb) {
    case 'focus-move':
      focusShape(editor, pickDirectional(editor, args.dir), ctx);
      break;
    case 'focus-seq':
      focusShape(editor, pickSequential(editor, args.dir), ctx);
      break;
    case 'focus-first': {
      const shapes = editor.getCurrentPageShapesSorted
        ? editor.getCurrentPageShapesSorted() : editor.getCurrentPageShapes();
      if (shapes.length) focusShape(editor, shapes[0].id, ctx);
      break;
    }
    case 'select':
      if (Array.isArray(args.ids)) {
        editor.setSelectedShapes(args.ids);
        emitSelection(editor, ctx);
      }
      break;
    case 'enter-edit': {
      /* Emacs-routed editing: do NOT open tldraw's native editor.
         Emit the node's current text so Emacs can open an edit buffer;
         live updates arrive as `node-text'.  */
      const id = currentId(editor);
      const shape = id ? editor.getShape(id) : null;
      if (shape) {
        ctx.send({ type: 'tldraw_event', board: ctx.boardId,
                   event: 'edit-begin', id, shapeType: shape.type,
                   text: shapeText(shape) });
      }
      break;
    }
    case 'exit-edit':
      editor.setEditingShape(null);
      ctx.send({ type: 'tldraw_event', board: ctx.boardId, event: 'edit-end',
                 id: args.id || '' });
      break;
    case 'node-text': {
      /* Live mirror from the Emacs edit buffer onto the shape.  */
      const id = args.id;
      const shape = id ? editor.getShape(id) : null;
      if (shape) {
        const props = {};
        if (shape.props && 'richText' in shape.props) {
          props.richText = toRichText(args.text || '');
        } else {
          props.text = args.text || '';
        }
        editor.updateShape({ id, type: shape.type, props });
      }
      break;
    }
    case 'create-node': {
      const id = args.id || createShapeId();
      const x = args.x ?? 0, y = args.y ?? 0;
      const shape = {
        id, type: 'geo', x, y,
        props: {
          geo: 'rectangle',
          w: args.w ?? 160, h: args.h ?? 64,
          /* 'solid' draws clean geometric (sharp-cornered) rectangles;
             the default 'draw' is the hand-drawn, round-cornered style.  */
          dash: args.dash || 'solid',
          size: args.size || 'm',
          font: args.font || 'sans',
          richText: toRichText(args.text || ''),
        },
      };
      if (args.meta && typeof args.meta === 'object') shape.meta = args.meta;
      editor.createShape(shape);
      if (!args.quiet) {
        editor.setSelectedShapes([id]);
        /* Centre the camera on a freshly created node so it is in view.  */
        const b = editor.getShapePageBounds(id);
        if (b) editor.centerOnPoint(boundsCenter(b),
                                    { animation: { duration: 200 } });
      }
      ctx.send({ type: 'tldraw_event', board: ctx.boardId, event: 'created',
                 id, shapeType: 'geo', meta: shape.meta || {} });
      break;
    }
    case 'create-text': {
      const id = args.id || createShapeId();
      const shape = {
        id, type: 'text', x: args.x ?? 0, y: args.y ?? 0,
        props: { richText: toRichText(args.text || '') },
      };
      if (args.meta && typeof args.meta === 'object') shape.meta = args.meta;
      editor.createShape(shape);
      if (!args.quiet) editor.setSelectedShapes([id]);
      break;
    }
    case 'set-meta': {
      const id = args.id || currentId(editor);
      const shape = id ? editor.getShape(id) : null;
      if (shape) {
        editor.updateShape({ id, type: shape.type,
                             meta: { ...(shape.meta || {}), ...(args.meta || {}) } });
      }
      break;
    }
    case 'recenter':
      recenter(editor, args.pos || 'center', args.id);
      break;
    case 'nudge-shape': {
      const id = args.id || currentId(editor);
      const shape = id ? editor.getShape(id) : null;
      if (shape) {
        editor.updateShape({ id, type: shape.type,
                             x: shape.x + (args.dx || 0),
                             y: shape.y + (args.dy || 0) });
      }
      break;
    }
    case 'undo': editor.undo(); break;
    case 'redo': editor.redo(); break;
    case 'create-image': {
      /* Embed an image (base64 data URL from Emacs) as a tldraw image
         shape, sizing it from the image's natural dimensions.  */
      const src = args.src;
      if (!src) break;
      const cap = args.maxw || 360;
      const img = new Image();
      img.onload = () => {
        const nw = img.naturalWidth || 200, nh = img.naturalHeight || 200;
        const scale = nw > cap ? cap / nw : 1;
        const w = Math.round(nw * scale), h = Math.round(nh * scale);
        const assetId = AssetRecordType.createId(getHashForString(src));
        editor.createAssets([{
          id: assetId, type: 'image', typeName: 'asset',
          props: { name: args.name || 'image', src, w: nw, h: nh,
                   mimeType: args.mime || 'image/png', isAnimated: false },
          meta: {},
        }]);
        const sid = args.id || createShapeId();
        editor.createShape({
          id: sid, type: 'image', x: args.x ?? 0, y: args.y ?? 0,
          props: { assetId, w, h },
          ...(args.meta ? { meta: args.meta } : {}),
        });
        if (!args.quiet) {
          editor.setSelectedShapes([sid]);
          const b = editor.getShapePageBounds(sid);
          if (b) editor.centerOnPoint(boundsCenter(b),
                                      { animation: { duration: 200 } });
        }
      };
      img.src = src;
      break;
    }
    case 'create-edge':
      createEdge(editor, args.from || currentId(editor), args.to);
      break;
    case 'delete': {
      const ids = args.ids || editor.getSelectedShapeIds();
      if (ids.length) editor.deleteShapes(ids);
      emitSelection(editor, ctx);
      break;
    }
    case 'nudge-bend': {
      const id = args.id || currentId(editor);
      const shape = id ? editor.getShape(id) : null;
      if (shape && shape.type === 'arrow') {
        const bend = (shape.props.bend || 0) + (args.delta || 0);
        editor.updateShape({ id, type: 'arrow', props: { bend } });
      }
      break;
    }
    case 'mouse': {
      /* Toggle pointer-events on this board's container so the mouse can
         drive tldraw directly (default off: keyboard-first).  The
         on-canvas UI is only useful with the mouse, so show it in mouse
         mode and hide it otherwise.  */
      const c = editor.getContainer?.();
      const board = c && c.closest('.tldraw-board');
      if (board) {
        board.classList.toggle('mouse-enabled', !!args.on);
        board.classList.toggle('hide-ui', !args.on);
      }
      break;
    }
    case 'clear': {
      const all = editor.getCurrentPageShapes().map(s => s.id);
      if (all.length) editor.deleteShapes(all);
      break;
    }
    case 'camera':
      if (args.action === 'fit') editor.zoomToFit({ animation: { duration: 150 } });
      else if (args.action === 'reset') editor.resetZoom({ animation: { duration: 150 } });
      else if (args.action === 'in') editor.zoomIn(undefined, { animation: { duration: 120 } });
      else if (args.action === 'out') editor.zoomOut(undefined, { animation: { duration: 120 } });
      else if (args.action === 'center') {
        const id = currentId(editor);
        const b = id ? editor.getShapePageBounds(id) : null;
        if (b) editor.centerOnPoint(boundsCenter(b), { animation: { duration: 150 } });
      }
      break;
    default:
      break;
  }
}
