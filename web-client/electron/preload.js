const { contextBridge, webUtils, ipcRenderer } = require('electron');

contextBridge.exposeInMainWorld('electronAPI', {
  getPathForFile: (file) => webUtils.getPathForFile(file),
  /* Write the OS clipboard via the main process.  The renderer's
     navigator.clipboard.writeText is rejected ("Document is not
     focused") when Emacs pushes a copy outside a user gesture, so M-w
     never reached the system clipboard; the main-process clipboard
     module has no such restriction.  */
  writeClipboard: (text) => ipcRenderer.invoke('clipboard-write', text),
  /* Read the OS clipboard via the main process; the renderer syncs it
     into Emacs on window focus so C-y sees copies made in other apps.
     navigator.clipboard.readText needs a user gesture/permission and is
     unreliable here.  */
  readClipboard: () => ipcRenderer.invoke('clipboard-read'),
});
