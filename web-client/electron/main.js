import { app, BrowserWindow, Menu, dialog, nativeImage } from 'electron';
import { spawn } from 'node:child_process';
import fs from 'node:fs';
import net from 'node:net';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const __dirname = path.dirname(fileURLToPath(import.meta.url));

let emacsProcess = null;
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

function startEmacs (emacsRoot, port) {
  const emacsBin = path.join(emacsRoot, 'src', 'emacs');
  const proxyDir = path.join(emacsRoot, 'web-display');
  const proxyBin = path.join(proxyDir, 'emacs-web-display');

  if (!executableExists(emacsBin))
    throw new Error(`Missing executable: ${emacsBin}`);
  if (!executableExists(proxyBin))
    throw new Error(`Missing executable: ${proxyBin}`);

  const env = {
    ...process.env,
    EMACS_WEB_PORT: String(port),
    EMACSDATA: path.join(emacsRoot, 'etc'),
    EMACSPATH: path.join(emacsRoot, 'lib-src'),
    EMACSLOADPATH: path.join(emacsRoot, 'lisp'),
    INFOPATH: path.join(emacsRoot, 'info'),
    LC_CTYPE: process.env.LC_CTYPE || 'UTF-8',
    PATH: appendPath(proxyDir, process.env.PATH || ''),
  };

  emacsProcess = spawn(emacsBin, [], {
    cwd: emacsRoot,
    env,
    stdio: ['ignore', 'pipe', 'pipe'],
  });

  emacsProcess.stdout.on('data', data => log(`stdout: ${data}`.trim()));
  emacsProcess.stderr.on('data', data => log(`stderr: ${data}`.trim()));
  emacsProcess.on('error', err => log(`spawn error: ${err.message}`));
  emacsProcess.on('exit', (code, signal) => {
    log(`emacs exited code=${code} signal=${signal}`);
    emacsProcess = null;
    if (!app.isQuitting) app.quit();
  });
}

function stopEmacs () {
  if (!emacsProcess) return;

  const child = emacsProcess;
  emacsProcess = null;
  let exited = false;
  child.once('exit', () => {
    exited = true;
  });
  child.kill('SIGTERM');
  setTimeout(() => {
    if (!exited) child.kill('SIGKILL');
  }, 3000).unref();
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
    },
  });

  if (process.platform === 'darwin' && !icon.isEmpty()) {
    app.dock.setIcon(icon);
  }

  mainWindow.webContents.setWindowOpenHandler(() => ({ action: 'deny' }));

  const indexHtml = path.join(app.getAppPath(), 'dist', 'index.html');
  mainWindow.loadFile(indexHtml, { search: `port=${port}&electron=1` });

  mainWindow.on('closed', () => {
    mainWindow = null;
  });
}

async function main () {
  openLog();
  const emacsRoot = resolveEmacsRoot();
  const port = await findFreePort(8080);
  log(`using emacs root ${emacsRoot}`);
  log(`using websocket port ${port}`);

  startEmacs(emacsRoot, port);
  createWindow(port);
}

app.on('before-quit', () => {
  app.isQuitting = true;
  stopEmacs();
});

function quitFromSignal () {
  app.isQuitting = true;
  stopEmacs();
  setTimeout(() => app.exit(0), 3500);
}

process.on('SIGTERM', quitFromSignal);
process.on('SIGINT', quitFromSignal);

app.whenReady().then(main).catch((err) => {
  log(err.stack || err.message);
  dialog.showErrorBox('Slopmacs failed to start', err.message);
  app.quit();
});

app.on('window-all-closed', () => {
  app.quit();
});
