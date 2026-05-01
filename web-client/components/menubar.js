/* Menu bar component — renders Emacs menu bar as a proper GUI menu bar.
   Extracts menu item names from the text line and renders them as
   clickable buttons.  Clicking sends a mouse event to Emacs at the
   item's position, triggering the popup menu system.  */

export function renderMenuBar (win, ws, h) {
  const lineData = win.lines.get(0);
  if (!lineData || !lineData.runs) {
    return h('div', { class: 'menu-bar' });
  }

  /* Extract the full text and parse menu items.
     Emacs menu bar text looks like: " File Edit Options Buffers Tools Help "
     Each item is a word separated by spaces.  We need to track each item's
     character offset so we can send a click at the right position.  */
  let fullText = '';
  for (const run of lineData.runs) {
    fullText += run.text || '';
  }

  /* Parse items: find contiguous non-space sequences.  */
  const items = [];
  let i = 0;
  while (i < fullText.length) {
    if (fullText[i] === ' ') { i++; continue; }
    const start = i;
    while (i < fullText.length && fullText[i] !== ' ') i++;
    items.push({
      label: fullText.substring(start, i),
      col: start,  /* character offset for click targeting */
    });
  }

  const onClick = (item) => {
    if (!ws || ws.readyState !== WebSocket.OPEN) return;

    /* Measure font to compute pixel position.  */
    const el = document.createElement('span');
    el.style.fontFamily = 'monospace';
    el.style.fontSize = '16px';
    el.style.position = 'absolute';
    el.style.visibility = 'hidden';
    el.style.whiteSpace = 'pre';
    el.textContent = 'M';
    document.body.appendChild(el);
    const charW = Math.ceil(el.getBoundingClientRect().width);
    const charH = Math.ceil(el.getBoundingClientRect().height * 1.2);
    document.body.removeChild(el);

    /* Send mouse down + up at the menu item's position.
       x = column * charW, y = win.y * charH (menu bar row).  */
    const x = (item.col + 1) * charW; /* +1 to hit middle of first char */
    const y = win.y * charH + Math.floor(charH / 2);
    const msg1 = JSON.stringify({ type: 'mouse_down', x, y, button: 0, mods: 0 }) + '\n';
    const msg2 = JSON.stringify({ type: 'mouse_up', x, y, button: 0, mods: 0 }) + '\n';
    ws.send(msg1);
    ws.send(msg2);
  };

  const children = items.map((item) =>
    h('div', {
      class: 'menubar-item',
      onClick: () => onClick(item),
    }, item.label)
  );

  const style = {
    left: `calc(${win.x} * 1ch)`,
    top: `calc(${win.y} * 1.2em)`,
    width: `calc(${win.w} * 1ch)`,
    height: `calc(${win.h} * 1.2em)`,
  };

  return h('div', { class: 'menu-bar', style }, ...children);
}
