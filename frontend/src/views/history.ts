import { api } from '../api';
import { formatMoney, h } from '../dom';
import { isBusy, statusOf, TRACE } from '../status';
import type { Receipt } from '../types';
import { uploadAsOne } from '../upload-flow';

const PAGE = 50;
const POLL_MS = 2500;

function messageOf(error: unknown): string {
  return error instanceof Error ? error.message : String(error);
}

// All transactions, newest first, with the live status trace. Polls while any shown receipt is
// still uploaded/scanning. `highlight` holds ids just uploaded. Returns a cleanup function that
// stops polling when the view is left.
export function historyView(container: HTMLElement, { highlight = [] }: { highlight?: number[] } = {}): () => void {
  let receipts: Receipt[] = [];
  let exhausted = false;
  let timer: ReturnType<typeof setTimeout> | undefined;
  let stopped = false;
  const list = h('ul', { class: 'receipts' });
  const more = h('button', { onclick: () => load(receipts.length) }, 'Load more');
  const error = h('p', { class: 'error', role: 'alert' });

  async function load(offset: number): Promise<void> {
    try {
      const page = await api<Receipt[]>('/receipts', { query: { limit: PAGE, offset } });
      receipts = offset === 0 ? page : receipts.concat(page);
      exhausted = page.length < PAGE;
      error.textContent = '';
      render();
    } catch (e) {
      error.textContent = messageOf(e);
    }
    schedule();
  }

  // Re-fetches only the busy rows, so polling costs one small request per scanning receipt.
  async function refreshBusy(): Promise<void> {
    const busy = receipts.filter(isBusy);
    const fresh = await Promise.all(busy.map((r) => api<Receipt>(`/receipts/${r.id}`).catch(() => r)));
    const byId = new Map(fresh.map((r) => [r.id, r]));
    receipts = receipts.map((r) => byId.get(r.id) ?? r);
    render();
    schedule();
  }

  function schedule(): void {
    clearTimeout(timer);
    if (!stopped && receipts.some(isBusy)) {
      timer = setTimeout(refreshBusy, POLL_MS);
    }
  }

  async function addPhotos(receipt: Receipt, input: HTMLInputElement, note: HTMLElement): Promise<void> {
    const files = [...(input.files ?? [])];
    input.value = '';
    if (!files.length) return;
    try {
      await uploadAsOne(files, { receiptId: receipt.id, onProgress: (t) => (note.textContent = t) });
      note.textContent = '';
      await load(0);
    } catch (e) {
      note.textContent = messageOf(e);
    }
  }

  async function retry(receipt: Receipt): Promise<void> {
    try {
      const updated = await api<Receipt>(`/receipts/${receipt.id}/retry-scan`, { method: 'POST' });
      receipts = receipts.map((r) => (r.id === updated.id ? updated : r));
      render();
      schedule();
    } catch (e) {
      error.textContent = messageOf(e);
    }
  }

  function row(receipt: Receipt): HTMLLIElement {
    const status = statusOf(receipt);
    const note = h('span', { class: 'muted small', role: 'status' });
    const picker = h('input', { type: 'file', accept: 'image/*,.heic,.heif', multiple: true, hidden: true });
    picker.addEventListener('change', () => addPhotos(receipt, picker, note));
    const title = receipt.merchant || receipt.store_name_raw || (isBusy(receipt) ? 'Reading receipt…' : 'Unknown');
    return h('li', { class: `receipt ${highlight.includes(receipt.id) ? 'new' : ''}` },
      h('div', { class: 'line' },
        h('strong', {}, title),
        h('span', { class: 'amount' }, receipt.total_cents || !isBusy(receipt)
          ? formatMoney(receipt.total_cents, receipt.currency) : '')),
      h('div', { class: 'line muted small' },
        h('span', {}, [receipt.purchase_date, receipt.kind !== 'purchase' && receipt.kind].filter(Boolean).join(' · ')),
        h('span', { class: `badge ${status.tone}` }, status.label)),
      status.step >= 0 && h('ol', { class: 'trace' },
        TRACE.map((label, i) => h('li', { class: i < status.step ? 'done' : i === status.step ? 'now' : '' }, label))),
      h('div', { class: 'line' },
        note,
        h('span', {},
          receipt.scan_state === 'scan_failed' && h('button', { class: 'link', onclick: () => retry(receipt) }, 'Retry scan'),
          h('button', { class: 'link', onclick: () => picker.click() }, 'Add photo'),
          picker)));
  }

  function render(): void {
    list.replaceChildren(...receipts.map(row));
    more.hidden = exhausted;
    if (!receipts.length) list.replaceChildren(h('li', { class: 'muted' }, 'Nothing yet. Upload a receipt to start.'));
  }

  container.replaceChildren(h('section', { class: 'stack' }, h('h1', {}, 'History'), error, list, more));
  void load(0);
  return () => {
    stopped = true;
    clearTimeout(timer);
  };
}
