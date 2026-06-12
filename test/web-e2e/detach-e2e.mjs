// E2E test: detach-on-slow (stage 3).
// A command running longer than thread-detach-ms hands the command
// loop to a fresh executor; typing during the slow command must take
// effect WHILE it still runs, and C-g goes to the foreground, leaving
// the detached background command unharmed.

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
const firstAt = (since, needle) => {
  const e = events.find((e) => e.t >= since && e.s.includes(needle));
  return e ? e.t : 0;
};
const waitFor = async (since, needle, ms) => {
  for (let i = 0; i < ms / 100; i++) {
    const t = firstAt(since, needle);
    if (t) return t;
    await sleep(100);
  }
  return 0;
};

await new Promise((res, rej) => { ws.onopen = res; ws.onerror = rej; });
send({ type: "request_redraw" });
await sleep(1500);

let pass = 0, fail = 0;
const verdict = (name, ok, detail) => {
  console.log(`${ok ? "PASS" : "FAIL"} ${name}${detail ? ": " + detail : ""}`);
  ok ? pass++ : fail++;
};

// ---- Test A: typing takes effect WHILE the slow command runs ----
const tBusy = Date.now();
key("c", 4); key("z");
await sleep(600); // well past thread-detach-ms (100)
for (const ch of "hi") key(ch);
const hiAt = await waitFor(tBusy, "hi", 5000);
const doneAt = await waitFor(tBusy, "busy-done", 30000);
verdict("A1 typed text rendered while command still running",
        hiAt && doneAt && hiAt < doneAt,
        hiAt && doneAt
          ? `'hi' at +${hiAt - tBusy}ms, busy-done at +${doneAt - tBusy}ms`
          : `hiAt=${hiAt} doneAt=${doneAt}`);
const ticksAfterHi = new Set();
for (const e of events)
  if (hiAt && e.t > hiAt && e.t < doneAt)
    for (const m of e.s.matchAll(/tick (\d+)/g)) ticksAfterHi.add(m[1]);
verdict("A2 background command kept running after typing",
        ticksAfterHi.size >= 2, `${ticksAfterHi.size} ticks after 'hi'`);

// ---- Test B: C-g hits the foreground; detached command survives ----
await sleep(500);
const tB = Date.now();
key("c", 4); key("z");
await sleep(600); // detached by now
send({ type: "interrupt" });
const quitAt = await waitFor(tB + 600, "Quit", 3000);
verdict("B1 C-g handled by foreground (Quit echoed)", quitAt > 0);
const doneB = await waitFor(tB, "busy-done", 30000);
verdict("B2 detached command survived C-g and completed", doneB > 0,
        doneB ? `completed at +${doneB - tB}ms` : "never completed");

// ---- Test C: still alive ----
const tC = Date.now();
for (const ch of "ok") key(ch);
verdict("C1 editor responsive at end", (await waitFor(tC, "ok", 3000)) > 0);

console.log(`\n${pass} passed, ${fail} failed; ${events.length} frames`);
process.exit(fail ? 1 : 0);
