// E2E test: command executor + UI thread split.
// Connects to the Emacs web display as a fake browser, runs a slow
// command, and verifies (1) display updates stream DURING the command,
// (2) keys typed during the command are queued and applied after,
// (3) C-g interrupts a running command.

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

const events = []; // {t, s}
const ws = new WebSocket("ws://127.0.0.1:8080");

ws.onmessage = async (ev) => {
  let s = typeof ev.data === "string" ? ev.data : await ev.data.text();
  events.push({ t: Date.now(), s });
};

const send = (obj) => ws.send(JSON.stringify(obj) + "\n");
const key = (ch, mods = 0) =>
  send({ type: "key", keycode: 0, mods, char: ch.charCodeAt(0) });

const seenSince = (t, needle) =>
  events.filter((e) => e.t >= t && e.s.includes(needle));

await new Promise((res, rej) => {
  ws.onopen = res;
  ws.onerror = (e) => rej(new Error("connect failed"));
});
send({ type: "request_redraw" });
await sleep(2000);

let pass = 0, fail = 0;
const verdict = (name, ok, detail) => {
  console.log(`${ok ? "PASS" : "FAIL"} ${name}${detail ? ": " + detail : ""}`);
  ok ? pass++ : fail++;
};

// ---- Test A: display streams during a slow command ----
const tBusy = Date.now();
key("c", 4); // C-c
key("z");

// Type while the command grinds.
await sleep(1000);
key("x"); key("x"); key("x");

// Wait for busy-done.
let doneAt = 0;
for (let i = 0; i < 300; i++) {
  await sleep(100);
  const d = seenSince(tBusy, "busy-done");
  if (d.length) { doneAt = d[0].t; break; }
}
verdict("A0 slow command completed", doneAt > 0,
        doneAt ? `after ${doneAt - tBusy}ms` : "timed out");

// Distinct tick values seen BEFORE busy-done arrived = live updates.
const tickVals = new Set();
for (const e of events) {
  if (e.t >= tBusy && doneAt && e.t < doneAt) {
    for (const m of e.s.matchAll(/tick (\d+)/g)) tickVals.add(m[1]);
  }
}
verdict("A1 display streamed during command", tickVals.size >= 5,
        `${tickVals.size} distinct ticks rendered mid-command`);

// Keys queued during the command appear afterwards.
await sleep(1500);
const xs = events.filter((e) => doneAt && e.t >= doneAt && e.s.includes("xxx"));
verdict("A2 keys typed during command applied after", xs.length > 0,
        xs.length ? "found 'xxx' in frame" : "'xxx' never rendered");

// ---- Test B: C-g interrupts a running command ----
const tB = Date.now();
key("c", 4);
key("z");
await sleep(800);
send({ type: "interrupt" }); // proxy delivers SIGINT to Emacs
let quitSeen = false;
for (let i = 0; i < 30; i++) {
  await sleep(100);
  if (seenSince(tB + 800, "Quit").length) { quitSeen = true; break; }
}
verdict("B1 C-g interrupted running command", quitSeen);

// The interrupted command must not have completed.
await sleep(1000);
verdict("B2 command did not run to completion",
        seenSince(tB, "busy-done").length === 0);

// ---- Test C: editor alive afterwards ----
const tC = Date.now();
key("y"); key("y");
await sleep(1500);
verdict("C1 editor responsive after interrupt",
        seenSince(tC, "yy").length > 0);

console.log(`\n${pass} passed, ${fail} failed; ${events.length} frames received`);
process.exit(fail ? 1 : 0);
