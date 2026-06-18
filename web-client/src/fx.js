/* Visual-effects overlay for the Emacs web display.

   A single transparent canvas stacked above the text canvas, used for
   flashy motion feedback that must never touch the text renderer's
   delicate pixel-overlap / dirty-tracking invariants (see the
   rendering-artifact postmortems).  Everything here is purely cosmetic
   and self-contained.

   The animation loop is self-stopping: rAF only runs while at least one
   effect is alive, and stops the instant the last one expires, so the
   overlay costs 0% CPU at idle (a core project value).  */

import { recordFxTick } from './perf.js';

const TRAIL_MS = 320;       /* comet-trail point lifetime */
const TRAIL_MAX = 48;       /* cap on retained trail points */
const BEACON_MS = 560;      /* jump beacon pulse duration */
const FLASH_MS = 460;       /* search/landing flash duration */

function easeOutCubic (t) {
  const u = 1 - t;
  return 1 - u * u * u;
}

export class FxOverlay {
  constructor (root) {
    this.root = root;
    this.dpr = window.devicePixelRatio || 1;
    this.width = 0;
    this.height = 0;

    this.canvas = document.createElement('canvas');
    this.canvas.className = 'emacs-fx-canvas';
    this.ctx = this.canvas.getContext('2d', { alpha: true });
    root.appendChild(this.canvas);

    /* Live effects.  */
    this.trailPoints = [];      /* {x, y, t, color} */
    this.beacons = [];          /* {x, y, t, color} */
    this.flashes = [];          /* {x, y, w, h, t, color} */

    this.raf = 0;
    this._tick = this._tick.bind(this);
  }

  resize (width, height, dpr) {
    this.width = width;
    this.height = height;
    this.dpr = dpr;
    this.canvas.width = Math.ceil(width * dpr);
    this.canvas.height = Math.ceil(height * dpr);
    this.canvas.style.width = width + 'px';
    this.canvas.style.height = height + 'px';
    this.ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  }

  destroy () {
    if (this.raf) cancelAnimationFrame(this.raf);
    this.raf = 0;
    this.trailPoints.length = 0;
    this.beacons.length = 0;
    this.flashes.length = 0;
    this.canvas.remove();
  }

  /* ---- effect emitters ---------------------------------------- */

  /* Push a caret position onto the comet trail.  Successive points
     fade and taper into an Obsidian-style tapering trace.  Tiny while
     typing; a long swoosh on a jump.  */
  cursorTrail (x, y, color) {
    const now = performance.now();
    const pts = this.trailPoints;
    const last = pts[pts.length - 1];
    /* Ignore no-op repeats (cursor redrawn without moving).  */
    if (last && Math.abs(last.x - x) < 0.5 && Math.abs(last.y - y) < 0.5) {
      return;
    }
    pts.push({ x, y, t: now, color });
    if (pts.length > TRAIL_MAX) pts.shift();
    this._wake();
  }

  /* An expanding ring + soft glow centred at (cx, cy).  Used to mark a
     jump destination.  */
  beacon (cx, cy, color) {
    this.beacons.push({ x: cx, y: cy, t: performance.now(), color });
    this._wake();
  }

  /* A fading filled highlight over a rectangle.  Used for search
     matches and the line you land on.  */
  flash (x, y, w, h, color) {
    this.flashes.push({ x, y, w, h, t: performance.now(), color });
    this._wake();
  }

  /* ---- loop --------------------------------------------------- */

  _wake () {
    if (!this.raf) this.raf = requestAnimationFrame(this._tick);
  }

  _alive () {
    return this.trailPoints.length > 0
      || this.beacons.length > 0
      || this.flashes.length > 0;
  }

  _tick () {
    this.raf = 0;
    const now = performance.now();
    const ctx = this.ctx;

    ctx.clearRect(0, 0, this.width, this.height);

    this._drawFlashes(ctx, now);
    this._drawBeacons(ctx, now);
    this._drawTrail(ctx, now);

    recordFxTick(performance.now() - now);

    if (this._alive()) {
      this.raf = requestAnimationFrame(this._tick);
    }
  }

  _drawTrail (ctx, now) {
    const pts = this.trailPoints;
    /* Drop expired points.  */
    while (pts.length && now - pts[0].t > TRAIL_MS) pts.shift();
    if (pts.length < 2) {
      /* A lone fresh point still draws a small glow head.  */
      if (pts.length === 1) {
        const p = pts[0];
        const age = (now - p.t) / TRAIL_MS;
        if (age < 1) this._trailHead(ctx, p, 1 - age);
      }
      return;
    }

    ctx.save();
    ctx.lineCap = 'round';
    ctx.lineJoin = 'round';
    ctx.shadowBlur = 8;
    for (let i = 1; i < pts.length; i++) {
      const a = pts[i - 1];
      const b = pts[i];
      const age = (now - b.t) / TRAIL_MS;
      if (age >= 1) continue;
      const head = i / (pts.length - 1);       /* 0 tail … 1 head */
      const alpha = (1 - age) * (0.10 + 0.55 * head);
      const wpx = 1.5 + 6 * head;
      ctx.strokeStyle = b.color;
      ctx.shadowColor = b.color;
      ctx.globalAlpha = alpha;
      ctx.lineWidth = wpx;
      ctx.beginPath();
      ctx.moveTo(a.x, a.y);
      ctx.lineTo(b.x, b.y);
      ctx.stroke();
    }
    /* Bright head dot.  */
    const headPt = pts[pts.length - 1];
    this._trailHead(ctx, headPt, 1 - (now - headPt.t) / TRAIL_MS);
    ctx.restore();
  }

  _trailHead (ctx, p, k) {
    if (k <= 0) return;
    ctx.save();
    ctx.globalAlpha = 0.6 * k;
    ctx.shadowBlur = 10;
    ctx.shadowColor = p.color;
    ctx.fillStyle = p.color;
    ctx.beginPath();
    ctx.arc(p.x, p.y, 3.5, 0, Math.PI * 2);
    ctx.fill();
    ctx.restore();
  }

  _drawBeacons (ctx, now) {
    const beacons = this.beacons;
    for (let i = beacons.length - 1; i >= 0; i--) {
      const b = beacons[i];
      const t = (now - b.t) / BEACON_MS;
      if (t >= 1) { beacons.splice(i, 1); continue; }
      const e = easeOutCubic(t);
      const radius = 8 + 90 * e;
      const alpha = (1 - t) * (1 - t);

      ctx.save();
      /* Expanding ring.  */
      ctx.globalAlpha = 0.5 * alpha;
      ctx.strokeStyle = b.color;
      ctx.shadowBlur = 16;
      ctx.shadowColor = b.color;
      ctx.lineWidth = 2.5 * (1 - t) + 0.5;
      ctx.beginPath();
      ctx.arc(b.x, b.y, radius, 0, Math.PI * 2);
      ctx.stroke();
      /* Soft inner glow.  */
      const grad = ctx.createRadialGradient(b.x, b.y, 0, b.x, b.y, radius);
      grad.addColorStop(0, b.color);
      grad.addColorStop(1, 'transparent');
      ctx.globalAlpha = 0.18 * alpha;
      ctx.fillStyle = grad;
      ctx.beginPath();
      ctx.arc(b.x, b.y, radius, 0, Math.PI * 2);
      ctx.fill();
      ctx.restore();
    }
  }

  _drawFlashes (ctx, now) {
    const flashes = this.flashes;
    for (let i = flashes.length - 1; i >= 0; i--) {
      const fl = flashes[i];
      const t = (now - fl.t) / FLASH_MS;
      if (t >= 1) { flashes.splice(i, 1); continue; }
      /* Quick bloom in, slow fade out.  */
      const k = t < 0.18 ? t / 0.18 : 1 - (t - 0.18) / 0.82;
      const alpha = Math.max(0, k);
      const r = Math.min(6, fl.h / 3);

      ctx.save();
      ctx.globalAlpha = 0.32 * alpha;
      ctx.fillStyle = fl.color;
      ctx.shadowBlur = 12;
      ctx.shadowColor = fl.color;
      this._roundRect(ctx, fl.x, fl.y, fl.w, fl.h, r);
      ctx.fill();
      ctx.restore();
    }
  }

  _roundRect (ctx, x, y, w, h, r) {
    ctx.beginPath();
    ctx.moveTo(x + r, y);
    ctx.arcTo(x + w, y, x + w, y + h, r);
    ctx.arcTo(x + w, y + h, x, y + h, r);
    ctx.arcTo(x, y + h, x, y, r);
    ctx.arcTo(x, y, x + w, y, r);
    ctx.closePath();
  }
}
