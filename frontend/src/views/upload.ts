import { h } from '../dom';
import { uploadAsOne } from '../upload-flow';

// Pick photos (camera or library), then upload them as ONE receipt, or each as its own
// receipt (a client-side loop over the same POST /receipts; spec decided 1a). Calls
// onUploaded(ids) with the receipts created.
export function uploadView(container: HTMLElement, { onUploaded }: { onUploaded: (ids: number[]) => void }): void {
  let files: File[] = [];
  const status = h('p', { class: 'muted', role: 'status' });
  const list = h('ul', { class: 'picked' });
  const asOne = h('button', { class: 'primary', onclick: () => run(false) }, 'Upload as one receipt');
  const separately = h('button', { onclick: () => run(true) }, 'Upload each as its own receipt');

  const camera = h('input', { type: 'file', accept: 'image/*', capture: 'environment', hidden: true });
  const library = h('input', { type: 'file', accept: 'image/*,.heic,.heif', multiple: true, hidden: true });
  for (const input of [camera, library]) {
    input.addEventListener('change', () => add(input));
  }

  function add(input: HTMLInputElement): void {
    files = files.concat([...(input.files ?? [])]);
    input.value = '';
    render();
  }

  function render(): void {
    list.replaceChildren(...files.map((file, i) =>
      h('li', {},
        h('span', {}, file.name || `Photo ${i + 1}`),
        h('button', { class: 'link', 'aria-label': `Remove ${file.name}`,
                      onclick: () => { files.splice(i, 1); render(); } }, 'Remove'))));
    asOne.hidden = files.length === 0;
    separately.hidden = files.length < 2;
    asOne.textContent = files.length > 1 ? `Upload ${files.length} photos as one receipt` : 'Upload';
  }

  async function run(eachSeparately: boolean): Promise<void> {
    const groups = eachSeparately ? files.map((f) => [f]) : [files];
    asOne.disabled = separately.disabled = true;
    const ids: number[] = [];
    try {
      for (const [i, group] of groups.entries()) {
        const prefix = groups.length > 1 ? `Receipt ${i + 1} of ${groups.length}: ` : '';
        const id = await uploadAsOne(group, { onProgress: (text) => (status.textContent = prefix + text) });
        if (id !== null) ids.push(id);
      }
      files = [];
      render();
      status.textContent = '';
      if (ids.length) onUploaded(ids);
    } catch (error) {
      status.textContent = error instanceof Error ? error.message : String(error);
      status.className = 'error';
    } finally {
      asOne.disabled = separately.disabled = false;
    }
  }

  container.replaceChildren(
    h('section', { class: 'card stack' },
      h('h1', {}, 'Add a receipt'),
      h('div', { class: 'pickers' },
        h('button', { class: 'big', onclick: () => camera.click() }, 'Take photo'),
        h('button', { class: 'big', onclick: () => library.click() }, 'Choose photos')),
      camera, library, list, asOne, separately, status,
      h('p', { class: 'muted small' },
        'Photos picked together become one receipt, e.g. a long receipt in two shots.')));
  render();
}
