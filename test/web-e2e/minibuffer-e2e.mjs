// Minibuffer test: M-x emacs-version RET — exercises recursive edit,
// minibuffer reading, and completion machinery on the executor thread.
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const events = [];
const ws = new WebSocket("ws://127.0.0.1:8080");
ws.onmessage = async (ev) => {
  let s = typeof ev.data === "string" ? ev.data : await ev.data.text();
  if (!s.includes('"heartbeat"')) events.push({ t: Date.now(), s });
};
const send = (obj) => ws.send(JSON.stringify(obj) + "\n");
const key = (ch, mods = 0) =>
  send({ type: "key", keycode: 0, mods, char: ch.charCodeAt(0) });
await new Promise((res, rej) => { ws.onopen = res; ws.onerror = rej; });
send({ type: "request_redraw" });
await sleep(1000);

key("x", 8); // M-x
await sleep(500);
const mxShown = events.some((e) => e.t > Date.now() - 600 && e.s.includes("M-x"));
console.log("minibuffer prompt shown:", mxShown);
for (const ch of "emacs-version") key(ch);
await sleep(300);
send({ type: "key", keycode: 0xff0d, mods: 0, char: 13 }); // RET
await sleep(1500);
const verShown = events.some((e) => e.s.includes("GNU Emacs"));
console.log("emacs-version output shown:", verShown);

// And C-g cancels an open minibuffer:
key("x", 8);
await sleep(400);
key("g", 4); // C-g as a key event (idle path, not signal path)
await sleep(800);
const t1 = Date.now();
for (const ch of "ok") key(ch);
await sleep(1200);
const aliveAfter = events.some((e) => e.t >= t1 && e.s.includes("ok"));
console.log("editor alive after C-g in minibuffer:", aliveAfter);
process.exit(mxShown && verShown && aliveAfter ? 0 : 1);
