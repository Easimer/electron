import { BaseWindow, WebContentsView, Event, OffscreenHostView } from 'electron/main';
import type { OffscreenWindow as OWT } from 'electron/main';
const { OffscreenWindow } = process._linkedBinding('electron_browser_offscreen_window') as { OffscreenWindow: typeof OWT };

Object.setPrototypeOf(OffscreenWindow.prototype, BaseWindow.prototype);

class OffscreenHostWindow extends BaseWindow {
  static _instance: OffscreenHostWindow | null;
  static _hostView: OffscreenHostView | null;

  constructor () {
    if (OffscreenHostWindow._instance) {
      return OffscreenHostWindow._instance;
    }

    super({
      show: false,
      fullscreenable: false,
      maximizable: false,
      minimizable: false
    });

    OffscreenHostWindow._hostView = null;
    OffscreenHostWindow._instance = this;
    return this;
  }

  static instance () {
    return new OffscreenHostWindow();
  }

  get _hostView () {
    return OffscreenHostWindow._hostView;
  }

  set _hostView (value: OffscreenHostView | null) {
    OffscreenHostWindow._hostView = value;
  }

  _init () {
    // Call parent class's _init.
    BaseWindow.prototype._init.call(this);

    this._hostView = new OffscreenHostView();
    // Create WebContentsView.
    this.setContentView(this._hostView);
  }

  addWebContents (webContentsView: WebContentsView) {
    if (this._hostView) {
      this._hostView.addWebContents(webContentsView);
    }
  }

  removeWebContents (id: number) {
    if (this._hostView) {
      this._hostView.removeWebContents(id);
      if (this._hostView.childCount === 0) {
        OffscreenHostWindow._instance = null;
        // this.close();
      }
    }
  }
}

OffscreenWindow.prototype._init = function (this: OWT) {
  // Avoid recursive require.
  const { app } = require('electron');

  // Create WebContentsView.
  OffscreenHostWindow.instance().addWebContents(this.webContentsView);

  this.setOwnerWindow(OffscreenHostWindow.instance());

  // Change window title to page title.
  this.webContents.on('page-title-updated', (event: Event, title: string, ...args: any[]) => {
    // Route the event to BrowserWindow.
    this.emit('page-title-updated', event, title, ...args);
  });

  // Sometimes the webContents doesn't get focus when window is shown, so we
  // have to force focusing on webContents in this case. The safest way is to
  // focus it when we first start to load URL, if we do it earlier it won't
  // have effect, if we do it later we might move focus in the page.
  //
  // Though this hack is only needed on macOS when the app is launched from
  // Finder, we still do it on all platforms in case of other bugs we don't
  // know.
  this.webContents.once('did-finish-load', () => {
    this.focus();
  });

  // Redirect focus/blur event to app instance too.
  this.on('blur', (event: Event) => {
    app.emit('offscreen-window-blur', event, this);
  });
  this.on('focus', (event: Event) => {
    app.emit('offscreen-window-focus', event, this);
  });
  this.on('closed', () => {
    OffscreenHostWindow.instance().removeWebContents(this.webContents.id);
  });

  // Notify the creation of the window.
  const event = process._linkedBinding('electron_browser_event').createEmpty();
  app.emit('offscreen-window-created', event, this);

  Object.defineProperty(this, 'devToolsWebContents', {
    enumerable: true,
    configurable: false,
    get () {
      return this.webContents.devToolsWebContents;
    }
  });
};

const isOffscreenWindow = (win: any) => {
  return win && win.constructor.name === 'OffscreenWindow';
};

OffscreenWindow.prototype.fromId = (id: number) => {
  const win = BaseWindow.fromId(id);
  return isOffscreenWindow(win) ? win as any as OWT : null;
};

OffscreenWindow.prototype.getAllWindows = () => {
  return BaseWindow.getAllWindows().filter(isOffscreenWindow) as any[] as OWT[];
};

// Forwarded to webContents:

OffscreenWindow.prototype.loadURL = function (...args: [url: string, options: ElectronInternal.LoadURLOptions]) {
  return this.webContents.loadURL(...args);
};

OffscreenWindow.prototype.getURL = function () {
  return this.webContents.getURL();
};

OffscreenWindow.prototype.loadFile = function (...args: [filename: string, options: Electron.LoadFileOptions]) {
  return this.webContents.loadFile(...args);
};

OffscreenWindow.prototype.reload = function () {
  return this.webContents.reload();
};

OffscreenWindow.prototype.send = function (...args: [channel: string, ...args: any[]]) {
  return this.webContents.send(...args);
};

OffscreenWindow.prototype.openDevTools = function (...args: any[]) {
  return this.webContents.openDevTools(...args);
};

OffscreenWindow.prototype.closeDevTools = function () {
  return this.webContents.closeDevTools();
};

OffscreenWindow.prototype.isDevToolsOpened = function () {
  return this.webContents.isDevToolsOpened();
};

OffscreenWindow.prototype.isDevToolsFocused = function () {
  return this.webContents.isDevToolsFocused();
};

OffscreenWindow.prototype.toggleDevTools = function () {
  return this.webContents.toggleDevTools();
};

OffscreenWindow.prototype.inspectElement = function (...args: [x: number, y: number]) {
  return this.webContents.inspectElement(...args);
};

OffscreenWindow.prototype.inspectSharedWorker = function () {
  return this.webContents.inspectSharedWorker();
};

OffscreenWindow.prototype.inspectServiceWorker = function () {
  return this.webContents.inspectServiceWorker();
};

OffscreenWindow.prototype.showDefinitionForSelection = function () {
  return this.webContents.showDefinitionForSelection();
};

OffscreenWindow.prototype.capturePage = function (...args: [Electron.Rectangle?]) {
  return this.webContents.capturePage(...args);
};

OffscreenWindow.prototype.getBackgroundThrottling = function () {
  return this.webContents.getBackgroundThrottling();
};

OffscreenWindow.prototype.setBackgroundThrottling = function (allowed: boolean) {
  return this.webContents.setBackgroundThrottling(allowed);
};

module.exports = OffscreenWindow;
