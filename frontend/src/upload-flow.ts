import { api, ApiError, uploadImages } from './api';
import { formatMoney, h } from './dom';
import { prepareImage } from './images';
import type { Receipt } from './types';

// Prepares `files` and uploads them as ONE receipt (or adds them to `receiptId`). On an
// exact-duplicate 409 it shows the existing receipt and re-sends with force=true only if the
// user confirms. Returns the new receipt id (or receiptId), or null if the user cancelled.
// Throws an Error with a user-facing message otherwise.
export async function uploadAsOne(
  files: File[],
  { receiptId, onProgress = () => {} }: { receiptId?: number; onProgress?: (text: string) => void } = {},
): Promise<number | null> {
  onProgress(`Preparing ${files.length === 1 ? 'photo' : `${files.length} photos`}…`);
  const blobs = await Promise.all(files.map(prepareImage));
  onProgress('Uploading…');
  try {
    return idOf(await uploadImages(blobs, { receiptId }), receiptId);
  } catch (error) {
    const duplicateOf = error instanceof ApiError ? error.body?.error?.duplicate_of : undefined;
    if (duplicateOf) {
      if (!(await confirmDuplicate(duplicateOf))) return null;
      onProgress('Uploading anyway…');
      return idOf(await uploadImages(blobs, { receiptId, force: true }), receiptId);
    }
    throw new Error(messageFor(error));
  }
}

function idOf(response: { id?: number }, receiptId: number | undefined): number {
  const id = receiptId ?? response.id;
  if (id === undefined) throw new Error('The server did not return the new receipt.');
  return id;
}

function messageFor(error: unknown): string {
  if (!(error instanceof ApiError)) return error instanceof Error ? error.message : String(error);
  switch (error.code) {
    case 'too_many_images':
      return 'That is more photos than one receipt can hold.';
    case 'file_too_large':
      return 'A photo is over 15 MB even after downscaling.';
    case 'invalid_image':
      return 'The server could not read one of the photos.';
    default:
      return error.message;
  }
}

// The "are you sure? [shows existing]" dialog for an exact duplicate (spec Capture, 409).
async function confirmDuplicate({ receipt_id: receiptId, image_id: imageId }: { receipt_id: number; image_id: number }):
    Promise<boolean> {
  let summary = `Receipt #${receiptId}`;
  try {
    const existing = await api<Receipt>(`/receipts/${receiptId}`);
    summary = [existing.merchant || existing.store_name_raw || 'Unknown merchant', existing.purchase_date,
               formatMoney(existing.total_cents, existing.currency)].filter(Boolean).join(' · ');
  } catch {
    // The image alone is enough to decide; the summary is a nicety.
  }

  const dialog = h('dialog', { class: 'dialog' },
    h('h2', {}, 'Already uploaded?'),
    h('p', {}, 'An identical photo is already on this receipt:'),
    h('img', { class: 'dup-image', src: `/api/v1/images/${imageId}`, alt: 'The existing photo' }),
    h('p', { class: 'muted' }, summary),
    h('form', { method: 'dialog', class: 'actions' },
      h('button', { value: 'cancel' }, 'Cancel'),
      h('button', { value: 'force', class: 'primary' }, 'Upload anyway')));
  document.body.append(dialog);
  dialog.showModal();
  const choice = await new Promise<string>((resolve) =>
    dialog.addEventListener('close', () => resolve(dialog.returnValue)));
  dialog.remove();
  return choice === 'force';
}
