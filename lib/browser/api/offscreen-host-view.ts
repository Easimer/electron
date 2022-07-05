import { View } from 'electron/main';

const { OsrHostView } = process._linkedBinding('electron_browser_osr_host_view');
const OffscreenHostView = OsrHostView;

Object.setPrototypeOf(OffscreenHostView.prototype, View.prototype);

export default OffscreenHostView;
