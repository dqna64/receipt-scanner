import type { Receipt } from './types';

// The user-facing status trace (spec Capture): uploaded -> scanning -> pending review, or scan
// failed. It is the scan_state lane plus the derived needs_review/is_confirmed flags, never a
// status column of its own.

export const TRACE = ['Uploaded', 'Scanning', 'Pending review'] as const;

export interface Status {
  label: string;
  tone: '' | 'busy' | 'bad' | 'todo' | 'ok';
  step: number; // indexes TRACE for receipts on the scan path, else -1
}

export function isBusy(receipt: Receipt): boolean {
  return receipt.scan_state === 'uploaded' || receipt.scan_state === 'scanning';
}

export function statusOf(receipt: Receipt): Status {
  switch (receipt.scan_state) {
    case 'uploaded':
      return { label: 'Queued for scan', tone: 'busy', step: 0 };
    case 'scanning':
      return { label: 'Scanning', tone: 'busy', step: 1 };
    case 'scan_failed':
      return { label: 'Scan failed', tone: 'bad', step: -1 };
    case 'idle':
      break;
  }
  if (receipt.needs_review) return { label: 'Pending review', tone: 'todo', step: 2 };
  if (receipt.is_confirmed) return { label: 'Reviewed', tone: 'ok', step: -1 };
  return { label: 'Saved', tone: '', step: -1 };
}
