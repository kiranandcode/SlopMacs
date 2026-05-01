/* Line component — renders a single line of styled text runs.
   Uses innerHTML (Monaco-style) instead of React elements for text spans.
   This eliminates virtual DOM overhead: one innerHTML write replaces
   20+ createElement + reconciliation operations per line.  */

import React from 'react';

/* ---------- helpers (module-level, zero allocation on hot path) ---------- */

function escapeHtml (s) {
  /* Fast path: most Emacs text has no HTML-special chars.  */
  if (s.indexOf('&') === -1 && s.indexOf('<') === -1 && s.indexOf('>') === -1)
    return s;
  return s.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
}

/* Build a CSS style string for a face, cached on the face object.  */
function faceStyle (face) {
  if (face._css) return face._css;
  let s = 'color:' + face.fg + ';background-color:' + face.bg;
  if (face.bold) s += ';font-weight:bold';
  if (face.italic) s += ';font-style:italic';
  if (face.underline && face.strike) s += ';text-decoration:underline line-through';
  else if (face.underline) s += ';text-decoration:underline';
  else if (face.strike) s += ';text-decoration:line-through';
  if (face.box === 'raised') {
    s += ';border-top:1px solid rgba(255,255,255,0.35)'
       + ';border-left:1px solid rgba(255,255,255,0.35)'
       + ';border-bottom:1px solid rgba(0,0,0,0.35)'
       + ';border-right:1px solid rgba(0,0,0,0.35)';
  } else if (face.box === 'sunken') {
    s += ';border-top:1px solid rgba(0,0,0,0.35)'
       + ';border-left:1px solid rgba(0,0,0,0.35)'
       + ';border-bottom:1px solid rgba(255,255,255,0.35)'
       + ';border-right:1px solid rgba(255,255,255,0.35)';
  } else if (face.box) {
    s += ';outline:1px solid ' + face.fg + ';outline-offset:-1px';
  }
  face._css = s;
  return s;
}

const CURSOR_CLASS = ['cursor-box', 'cursor-hollow', 'cursor-bar', 'cursor-hbar'];

function cursorClass (cursor) {
  if (!cursor.active) return 'cursor-inactive';
  return CURSOR_CLASS[cursor.type] || 'cursor-box';
}

/* Count user-perceived characters.  ASCII fast path.  */
function charLen (text) {
  for (let i = 0; i < text.length; i++) {
    if (text.charCodeAt(i) > 127) return [...text].length;
  }
  return text.length;
}

/* Split text at character index.  ASCII fast path.  */
function splitAt (text, idx) {
  for (let i = 0; i < text.length; i++) {
    if (text.charCodeAt(i) > 127) {
      const chars = [...text];
      return [
        chars.slice(0, idx).join(''),
        chars[idx] || ' ',
        chars.slice(idx + 1).join(''),
      ];
    }
  }
  return [
    text.substring(0, idx),
    text.charAt(idx) || ' ',
    text.substring(idx + 1),
  ];
}

/* ---------- component ---------- */

function lineAreEqual (prev, next) {
  if (!prev.lineData && !next.lineData) return true;
  if (!prev.lineData || !next.lineData) return false;
  if (prev.lineGen !== next.lineGen) return false;
  const prevCursorHere = prev.cursor && prev.cursor.row === prev.lineData.row;
  const nextCursorHere = next.cursor && next.cursor.row === next.lineData.row;
  if (prevCursorHere !== nextCursorHere) return false;
  if (prevCursorHere && nextCursorHere) {
    if (prev.cursorGen !== next.cursorGen) return false;
  }
  return true;
}

function Line ({ lineData, lineGen, cursor, cursorGen, faceResolver }) {
  if (!lineData) {
    return <div className="emacs-line" />;
  }

  const runs = lineData.runs || [];
  const cursorOnLine = cursor && cursor.row === lineData.row;

  /* Build HTML string — one string concat instead of 20+ React elements.  */
  let html = '';
  let col = 0;

  for (let ri = 0; ri < runs.length; ri++) {
    const run = runs[ri];
    const face = faceResolver(run.face_id);
    const text = run.text || '';
    const css = faceStyle(face);
    const len = charLen(text);

    if (cursorOnLine && cursor.col >= col && cursor.col < col + len) {
      const ci = cursor.col - col;
      const [before, ch, after] = splitAt(text, ci);
      const cc = cursorClass(cursor);
      const cursorCss = cursor.active && cursor.type === 0
        ? 'color:' + face.bg + ';background-color:' + face.fg
        : css;

      if (before) html += '<span class="r" style="' + css + '">' + escapeHtml(before) + '</span>';
      html += '<span class="r ' + cc + '" style="' + cursorCss + '">' + escapeHtml(ch) + '</span>';
      if (after) html += '<span class="r" style="' + css + '">' + escapeHtml(after) + '</span>';
    } else {
      html += '<span class="r" style="' + css + '">' + escapeHtml(text) + '</span>';
    }
    col += len;
  }

  /* Cursor beyond all runs.  */
  if (cursorOnLine && cursor.col >= col) {
    const df = faceResolver(0);
    const cc = cursorClass(cursor);
    const cursorCss = cursor.active
      ? 'color:' + df.bg + ';background-color:' + df.fg
      : 'color:' + df.fg + ';background-color:' + df.bg;
    html += '<span class="r ' + cc + '" style="' + cursorCss + '"> </span>';
  }

  const cls = 'emacs-line' + (lineData.mode_line ? ' mode-line' : '');
  return <div className={cls} dangerouslySetInnerHTML={{ __html: html }} />;
}

export default React.memo(Line, lineAreEqual);
