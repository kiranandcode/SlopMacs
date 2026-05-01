/* Line component — renders a single line of styled text runs.
   Uses a vnode cache to skip re-rendering unchanged lines.  */

const lineCache = new Map();

export function renderLine (lineData, cursor, faceResolver, h, rowKey) {
  if (!lineData) {
    return h('div', { class: 'emacs-line', key: rowKey });
  }

  /* Check if cursor is on this line — if so, always re-render.  */
  const cursorOnLine = cursor && cursor.row === lineData.row;

  /* Cache key: generation stamp from state.js.  */
  const gen = lineData.gen || 0;
  const cacheKey = rowKey + ':' + gen + ':' + (cursorOnLine ? cursor.col + ':' + cursor.active : 'nc');

  const cached = lineCache.get(rowKey);
  if (cached && cached.key === cacheKey) {
    return cached.vnode;
  }

  const children = [];
  const runs = lineData.runs || [];
  let colOffset = 0;

  for (let ri = 0; ri < runs.length; ri++) {
    const run = runs[ri];
    const face = faceResolver(run.face_id);
    const text = run.text || '';

    const style = {
      color: face.fg,
      backgroundColor: face.bg,
    };
    if (face.bold) style.fontWeight = 'bold';
    if (face.italic) style.fontStyle = 'italic';
    if (face.underline) style.textDecoration = 'underline';
    if (face.strike) {
      style.textDecoration = (style.textDecoration || '') + ' line-through';
    }
    if (face.box) {
      style.outline = '1px solid ' + face.fg;
      style.outlineOffset = '-1px';
    }

    /* Count characters efficiently (ASCII fast path).  */
    let textLen = text.length;
    let isAscii = true;
    for (let i = 0; i < text.length; i++) {
      if (text.charCodeAt(i) > 127) { isAscii = false; break; }
    }
    if (!isAscii) {
      textLen = 0;
      for (const _c of text) textLen++;
    }

    /* Check if cursor falls within this run.  */
    if (cursorOnLine
        && cursor.col >= colOffset
        && cursor.col < colOffset + textLen) {
      const cursorIdx = cursor.col - colOffset;

      let before, cursorChar, after;
      if (isAscii) {
        before = text.substring(0, cursorIdx);
        cursorChar = text.charAt(cursorIdx) || ' ';
        after = text.substring(cursorIdx + 1);
      } else {
        const chars = Array.from(text);
        before = chars.slice(0, cursorIdx).join('');
        cursorChar = chars[cursorIdx] || ' ';
        after = chars.slice(cursorIdx + 1).join('');
      }

      if (before) {
        children.push(h('span', { class: 'emacs-run', style }, before));
      }

      const cursorClass = cursor.active
        ? (cursor.type === 0 ? 'cursor-box'
          : cursor.type === 1 ? 'cursor-hollow'
          : cursor.type === 2 ? 'cursor-bar'
          : cursor.type === 3 ? 'cursor-hbar'
          : 'cursor-box')
        : 'cursor-inactive';

      const cursorStyle = { ...style };
      if (cursor.active && cursor.type === 0) {
        cursorStyle.color = face.bg;
        cursorStyle.backgroundColor = face.fg;
      }

      children.push(h('span', { class: `emacs-run ${cursorClass}`, style: cursorStyle }, cursorChar));

      if (after) {
        children.push(h('span', { class: 'emacs-run', style }, after));
      }
    } else {
      children.push(h('span', { class: 'emacs-run', style }, text));
    }

    colOffset += textLen;
  }

  /* If cursor is beyond all runs, render it.  */
  if (cursorOnLine && cursor.col >= colOffset) {
    const defFace = faceResolver(0);
    const cursorClass = cursor.active ? 'cursor-box' : 'cursor-inactive';
    const cursorStyle = cursor.active
      ? { color: defFace.bg, backgroundColor: defFace.fg }
      : { color: defFace.fg, backgroundColor: defFace.bg };
    children.push(h('span', { class: `emacs-run ${cursorClass}`, style: cursorStyle }, ' '));
  }

  const lineClass = 'emacs-line' + (lineData.mode_line ? ' mode-line' : '');
  const vnode = h('div', { class: lineClass, key: rowKey }, ...children);

  lineCache.set(rowKey, { key: cacheKey, vnode });

  /* Evict old cache entries if it grows too large.  */
  if (lineCache.size > 500) {
    const iter = lineCache.keys();
    for (let i = 0; i < 100; i++) lineCache.delete(iter.next().value);
  }

  return vnode;
}
