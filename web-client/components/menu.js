/* Menu component — renders popup menus from Emacs as HTML dropdowns.  */

export function renderMenu (menu, ws, state, h) {
  if (!menu || !menu.panes) return null;

  const onSelect = (item) => {
    if (!item.enabled) return;
    ws.send(JSON.stringify({ type: 'menu_select', idx: item.idx }) + '\n');
    state.dismissMenu();
  };

  const onCancel = (e) => {
    /* Only cancel if clicking the backdrop itself.  */
    if (e.target === e.currentTarget) {
      ws.send(JSON.stringify({ type: 'menu_cancel' }) + '\n');
      state.dismissMenu();
    }
  };

  const onKeyDown = (e) => {
    if (e.key === 'Escape') {
      e.preventDefault();
      ws.send(JSON.stringify({ type: 'menu_cancel' }) + '\n');
      state.dismissMenu();
    }
  };

  const panes = menu.panes.map((pane, pi) => {
    const items = (pane.items || []).map((item, ii) => {
      if (item.separator) {
        return h('div', { class: 'menu-separator', key: `sep-${pi}-${ii}` });
      }

      const cls = 'menu-item' + (item.enabled ? '' : ' disabled');
      return h('div', {
        class: cls,
        key: `item-${pi}-${ii}`,
        onClick: () => onSelect(item),
      },
        h('span', { class: 'menu-label' }, item.label),
        item.key ? h('span', { class: 'menu-key' }, item.key) : null,
      );
    });

    return h('div', { class: 'menu-pane', key: `pane-${pi}` },
      pane.name ? h('div', { class: 'menu-pane-title' }, pane.name) : null,
      ...items,
    );
  });

  /* Position near the click point.  */
  const style = {
    left: Math.min(menu.x || 0, window.innerWidth - 250) + 'px',
    top: (menu.y || 0) + 'px',
  };

  return h('div', {
    class: 'menu-backdrop',
    onClick: onCancel,
    onKeyDown: onKeyDown,
    tabIndex: -1,
    ref: (el) => el && el.focus(),
  },
    h('div', { class: 'menu-popup', style }, ...panes),
  );
}
