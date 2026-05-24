import { app, BrowserWindow, Menu, dialog, nativeImage } from 'electron';
import { spawn } from 'node:child_process';
import { createRequire } from 'node:module';
import fs from 'node:fs';
import net from 'node:net';
import path from 'node:path';
import os from 'node:os';
import { fileURLToPath } from 'node:url';

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const require = createRequire(import.meta.url);

/* Load the native Fn→Option key translation module (macOS only).  */
let fnKey = null;
if (process.platform === 'darwin') {
  try {
    fnKey = require('../native/build/Release/fn_key.node');
    fnKey.start();
  } catch (e) {
    console.warn('fn_key native module not available:', e.message);
  }
}

const SESSION_FILE = path.join(os.homedir(), '.emacs-web-session.json');

let mainWindow = null;
let logStream = null;

function executableExists (file) {
  try {
    fs.accessSync(file, fs.constants.X_OK);
    return true;
  } catch {
    return false;
  }
}

function resolveEmacsRoot () {
  const packagedRoot = path.join(process.resourcesPath, 'emacs-root');
  if (executableExists(path.join(packagedRoot, 'src', 'emacs')))
    return packagedRoot;

  return path.resolve(__dirname, '..', '..');
}

function findFreePort (startPort) {
  const portOpen = (port) => new Promise((resolve) => {
    const socket = net.createConnection({ host: '127.0.0.1', port });
    socket.setTimeout(200);
    socket.once('connect', () => {
      socket.destroy();
      resolve(true);
    });
    socket.once('timeout', () => {
      socket.destroy();
      resolve(false);
    });
    socket.once('error', () => resolve(false));
  });

  const portBindable = (port) => new Promise((resolve) => {
    const server = net.createServer();
    server.once('error', () => {
      server.close();
      resolve(false);
    });
    server.once('listening', () => {
      server.close(() => resolve(true));
    });
    server.listen(port, '127.0.0.1');
  });

  return (async () => {
    for (let port = startPort; port < startPort + 100; port++) {
      if (await portOpen(port)) continue;
      if (await portBindable(port)) return port;
    }
    throw new Error(`No free port found starting at ${startPort}`);
  })();
}

function appendPath (first, rest) {
  return rest ? `${first}${path.delimiter}${rest}` : first;
}

function openLog () {
  const logDir = app.getPath('logs');
  fs.mkdirSync(logDir, { recursive: true });
  logStream = fs.createWriteStream(path.join(logDir, 'emacs.log'), {
    flags: 'a',
  });
}

function log (line) {
  if (!logStream) return;
  logStream.write(`[${new Date().toISOString()}] ${line}\n`);
}

/* Check if a process with the given PID is alive.  */
function processAlive (pid) {
  try {
    process.kill(pid, 0);
    return true;
  } catch {
    return false;
  }
}

/* Probe the WebSocket port to see if the proxy is responsive.  */
function probeWebSocket (port) {
  return new Promise((resolve) => {
    const socket = net.createConnection({ host: '127.0.0.1', port });
    socket.setTimeout(500);
    socket.once('connect', () => {
      socket.destroy();
      resolve(true);
    });
    socket.once('timeout', () => {
      socket.destroy();
      resolve(false);
    });
    socket.once('error', () => resolve(false));
  });
}

/* Read and validate existing session file.  Returns session or null.  */
async function readSession () {
  try {
    const data = JSON.parse(fs.readFileSync(SESSION_FILE, 'utf8'));
    if (!data.proxyPid || !data.wsPort) return null;

    /* Check proxy is alive.  */
    if (!processAlive(data.proxyPid)) {
      log('Session stale: proxy PID not alive');
      return null;
    }

    /* Probe WebSocket.  */
    if (!await probeWebSocket(data.wsPort)) {
      log('Session stale: WebSocket not responsive');
      return null;
    }

    return data;
  } catch {
    return null;
  }
}

function writeSession (data) {
  fs.writeFileSync(SESSION_FILE, JSON.stringify(data, null, 2));
}

function spawnProxy (emacsRoot, wsPort, emacsPort) {
  const proxyBin = path.join(emacsRoot, 'web-display', 'emacs-web-display');
  if (!executableExists(proxyBin))
    throw new Error(`Missing executable: ${proxyBin}`);

  const logDir = app.getPath('logs');
  const logFd = fs.openSync(path.join(logDir, 'proxy.log'), 'a');

  const child = spawn(proxyBin,
    ['--port', String(wsPort), '--emacs-port', String(emacsPort)],
    {
      detached: true,
      stdio: ['ignore', logFd, logFd],
    });

  child.unref();
  fs.closeSync(logFd);
  log(`Spawned proxy PID=${child.pid} wsPort=${wsPort} emacsPort=${emacsPort}`);
  return child.pid;
}

function spawnEmacsWrapper (emacsRoot, wsPort, emacsPort) {
  const emacsBin = path.join(emacsRoot, 'src', 'emacs');
  const wrapperScript = path.join(emacsRoot, 'web-display', 'emacs-wrapper.sh');

  if (!executableExists(emacsBin))
    throw new Error(`Missing executable: ${emacsBin}`);

  const proxyDir = path.join(emacsRoot, 'web-display');
  const env = {
    ...process.env,
    EMACS_WEB_PORT: String(wsPort),
    EMACS_WEB_EMACS_PORT: String(emacsPort),
    EMACS_ROOT: emacsRoot,
    EMACSDATA: path.join(emacsRoot, 'etc'),
    EMACSPATH: path.join(emacsRoot, 'lib-src'),
    EMACSLOADPATH: path.join(emacsRoot, 'lisp'),
    INFOPATH: path.join(emacsRoot, 'info'),
    LC_CTYPE: process.env.LC_CTYPE || 'UTF-8',
    PATH: appendPath(proxyDir, process.env.PATH || ''),
  };

  const logDir = app.getPath('logs');
  const logFd = fs.openSync(path.join(logDir, 'emacs.log'), 'a');

  let child;
  if (executableExists(wrapperScript)) {
    /* Use wrapper script for auto-restart on exit code 42.  */
    child = spawn(wrapperScript, [emacsBin], {
      cwd: emacsRoot,
      env,
      detached: true,
      stdio: ['ignore', logFd, logFd],
    });
  } else {
    /* Fallback: run Emacs directly.  */
    child = spawn(emacsBin, [], {
      cwd: emacsRoot,
      env,
      detached: true,
      stdio: ['ignore', logFd, logFd],
    });
  }

  child.unref();
  fs.closeSync(logFd);
  log(`Spawned Emacs wrapper PID=${child.pid}`);
  return child.pid;
}

function createWindow (port) {
  Menu.setApplicationMenu(null);

  const iconPath = path.join(__dirname, 'icon.png');
  const icon = nativeImage.createFromPath(iconPath);

  mainWindow = new BrowserWindow({
    width: 1280,
    height: 900,
    minWidth: 640,
    minHeight: 480,
    title: 'Slopmacs',
    icon,
    backgroundColor: '#1a0a2e',
    webPreferences: {
      contextIsolation: true,
      nodeIntegration: false,
      sandbox: true,
      preload: path.join(__dirname, 'preload.js'),
    },
  });

  if (process.platform === 'darwin' && !icon.isEmpty()) {
    app.dock.setIcon(icon);
  }

  mainWindow.webContents.setWindowOpenHandler(() => ({ action: 'deny' }));

  /* Support Vite HMR dev mode.  */
  const devUrl = process.env.VITE_DEV_SERVER_URL;
  if (devUrl) {
    const url = new URL(devUrl);
    url.searchParams.set('port', port);
    url.searchParams.set('electron', '1');
    mainWindow.loadURL(url.toString());
  } else {
    const indexHtml = path.join(app.getAppPath(), 'dist', 'index.html');
    mainWindow.loadFile(indexHtml, { search: `port=${port}&electron=1` });
  }

  mainWindow.on('closed', () => {
    mainWindow = null;
  });
}

async function main () {
  openLog();
  const emacsRoot = resolveEmacsRoot();
  log(`using emacs root ${emacsRoot}`);

  /* Check for existing session.  */
  const session = await readSession();
  if (session) {
    log(`Reusing existing session: wsPort=${session.wsPort} proxyPid=${session.proxyPid}`);
    createWindow(session.wsPort);
    return;
  }

  /* No valid session — start fresh.  */
  try { fs.unlinkSync(SESSION_FILE); } catch {}

  const wsPort = await findFreePort(8080);
  const emacsPort = wsPort + 2;
  log(`using websocket port ${wsPort}, emacs port ${emacsPort}`);

  const proxyPid = spawnProxy(emacsRoot, wsPort, emacsPort);

  /* Give proxy a moment to bind its ports.  */
  await new Promise(r => setTimeout(r, 200));

  const wrapperPid = spawnEmacsWrapper(emacsRoot, wsPort, emacsPort);

  writeSession({
    proxyPid,
    wrapperPid,
    wsPort,
    emacsPort,
    startedAt: new Date().toISOString(),
  });

  createWindow(wsPort);
}

/* Cmd-Q / window close: just exit Electron.
   Proxy + Emacs stay alive for next launch.  */
app.on('before-quit', () => {
  app.isQuitting = true;
});

app.whenReady().then(main).catch((err) => {
  log(err.stack || err.message);
  dialog.showErrorBox('Slopmacs failed to start', err.message);
  app.quit();
});

app.on('window-all-closed', () => {
  app.quit();
});
