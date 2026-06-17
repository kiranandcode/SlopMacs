const { contextBridge, webUtils, ipcRenderer } = require('electron');

contextBridge.exposeInMainWorld('electronAPI', {
  getPathForFile: (file) => webUtils.getPathForFile(file),
  /* Write the OS clipboard via the main process.  The renderer's
     navigator.clipboard.writeText is rejected ("Document is not
     focused") when Emacs pushes a copy outside a user gesture, so M-w
     never reached the system clipboard; the main-process clipboard
     module has no such restriction.  */
  writeClipboard: (text) => ipcRenderer.invoke('clipboard-write', text),
});
