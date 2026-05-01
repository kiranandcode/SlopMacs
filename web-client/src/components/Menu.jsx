/* Popup menu component — renders Emacs menus as HTML dropdowns.  */

import React, { useCallback, useRef, useEffect } from 'react';

export default function Menu ({ menu, ws, onDismiss }) {
  const backdropRef = useRef(null);

  useEffect(() => {
    if (backdropRef.current) backdropRef.current.focus();
  }, []);

  const onSelect = useCallback((item) => {
    if (!item.enabled) return;
    ws.send(JSON.stringify({ type: 'menu_select', idx: item.idx }) + '\n');
    onDismiss();
  }, [ws, onDismiss]);

  const onCancel = useCallback((e) => {
    if (e.target === e.currentTarget) {
      ws.send(JSON.stringify({ type: 'menu_cancel' }) + '\n');
      onDismiss();
    }
  }, [ws, onDismiss]);

  const onKeyDown = useCallback((e) => {
    if (e.key === 'Escape') {
      e.preventDefault();
      ws.send(JSON.stringify({ type: 'menu_cancel' }) + '\n');
      onDismiss();
    }
  }, [ws, onDismiss]);

  if (!menu || !menu.panes) return null;

  const style = {
    left: Math.min(menu.x || 0, window.innerWidth - 250) + 'px',
    top: (menu.y || 0) + 'px',
  };

  return (
    <div
      ref={backdropRef}
      className="menu-backdrop"
      onClick={onCancel}
      onKeyDown={onKeyDown}
      tabIndex={-1}
    >
      <div className="menu-popup" style={style}>
        {menu.panes.map((pane, pi) => (
          <div key={pi} className="menu-pane">
            {pane.name && <div className="menu-pane-title">{pane.name}</div>}
            {(pane.items || []).map((item, ii) => {
              if (item.separator) {
                return <div key={ii} className="menu-separator" />;
              }
              return (
                <div
                  key={ii}
                  className={'menu-item' + (item.enabled ? '' : ' disabled')}
                  onClick={() => onSelect(item)}
                >
                  <span className="menu-label">{item.label}</span>
                  {item.key && <span className="menu-key">{item.key}</span>}
                </div>
              );
            })}
          </div>
        ))}
      </div>
    </div>
  );
}
