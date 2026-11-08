import { api, events } from './api';
import { historyView } from './views/history';
import { loginView } from './views/login';
import { settingsView } from './views/settings';
import { uploadView } from './views/upload';

// Hash router: #/upload, #/history, #/settings, #/login. Each view renders into #app and may
// return a cleanup function (history stops polling).
type Cleanup = () => void;

function byId<T extends HTMLElement>(id: string): T {
  const el = document.getElementById(id);
  if (!el) throw new Error(`#${id} is missing from index.html`);
  return el as T;
}

const app = byId('app');
const topbar = byId('topbar');
let cleanup: Cleanup | null = null;
let justUploaded: number[] = [];

const routes: Record<string, () => Cleanup | void> = {
  '/login': () => loginView(app, { onDone: () => go('/upload') }),
  '/upload': () => uploadView(app, { onUploaded: (ids) => { justUploaded = ids; go('/history'); } }),
  '/history': () => {
    const highlight = justUploaded;
    justUploaded = [];
    return historyView(app, { highlight });
  },
  '/settings': () => settingsView(app),
};

function go(path: string): void {
  if (location.hash === `#${path}`) route();
  else location.hash = path;
}

function route(): void {
  cleanup?.();
  const path = location.hash.slice(1) || '/upload';
  const view = routes[path] ?? routes['/upload']!;
  topbar.hidden = path === '/login';
  for (const link of topbar.querySelectorAll('a')) {
    link.classList.toggle('active', link.getAttribute('href') === `#${path}`);
  }
  cleanup = view() || null;
}

events.addEventListener('unauthorized', () => {
  if (location.hash !== '#/login') go('/login');
});

byId('logout').addEventListener('click', async () => {
  await api('/auth/logout', { method: 'POST' }).catch(() => {});
  go('/login');
});

window.addEventListener('hashchange', route);
route();
