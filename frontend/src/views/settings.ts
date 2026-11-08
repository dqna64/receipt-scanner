import { api } from '../api';
import { h } from '../dom';
import type { ScannerNotes } from '../types';

const MAX_CHARS = 4000; // the server's limit (users.scanner_notes CHECK)

// The user's scanner notes: free text that every scan adds to its prompt.
export function settingsView(container: HTMLElement): void {
  const notes = h('textarea', { rows: 10, maxlength: MAX_CHARS,
                                placeholder: 'e.g. Beem screenshots: the merchant is "@handle - Beem".' });
  const status = h('p', { class: 'muted small', role: 'status' });
  const save = h('button', { class: 'primary', type: 'submit' }, 'Save');
  const show = (e: unknown) => (status.textContent = e instanceof Error ? e.message : String(e));

  api<ScannerNotes>('/scanner-notes').then((body) => (notes.value = body.scanner_notes)).catch(show);

  async function onSubmit(event: SubmitEvent): Promise<void> {
    event.preventDefault();
    save.disabled = true;
    try {
      const body = await api<ScannerNotes>('/scanner-notes', { method: 'PUT', json: { scanner_notes: notes.value } });
      notes.value = body.scanner_notes;
      status.textContent = 'Saved. New scans use these notes.';
    } catch (e) {
      show(e);
    } finally {
      save.disabled = false;
    }
  }

  container.replaceChildren(
    h('form', { class: 'card stack', onsubmit: onSubmit },
      h('h1', {}, 'Scanner notes'),
      h('p', { class: 'muted small' },
        'Facts about your own receipts that the scanner should know. A note applies only to documents it clearly ' +
        'describes, so name the shop or app. Keep them short: every scan reads them.'),
      notes, save, status));
}
