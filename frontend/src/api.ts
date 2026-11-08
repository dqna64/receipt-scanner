import type { ErrorBody } from './types';

// Thin fetch wrapper over the REST API (/api/v1). Errors carry the server's
// {"error": {"code", "message", ...}} body; a 401 also fires "unauthorized" so the router can
// send the user to the login screen from wherever they are.

export class ApiError extends Error {
  readonly status: number;
  readonly code: string | undefined;
  readonly body: ErrorBody | null;

  constructor(status: number, body: ErrorBody | null) {
    super(body?.error?.message || `HTTP ${status}`);
    this.status = status;
    this.code = body?.error?.code;
    this.body = body;
  }
}

export const events = new EventTarget();

interface RequestOptions {
  method?: string;
  json?: unknown; // sent as a JSON body
  form?: FormData; // sent as a multipart body
  query?: Record<string, string | number>;
}

// Returns the parsed JSON body as T; the caller names the response type it expects.
export async function api<T = unknown>(path: string, { method = 'GET', json, form, query }: RequestOptions = {}):
    Promise<T> {
  const url = new URL(`/api/v1${path}`, location.origin);
  for (const [key, value] of Object.entries(query ?? {})) {
    url.searchParams.set(key, String(value));
  }
  const init: RequestInit = { method, credentials: 'same-origin' };
  if (json !== undefined) {
    init.headers = { 'Content-Type': 'application/json' };
    init.body = JSON.stringify(json);
  } else if (form) {
    init.body = form;
  }

  const response = await fetch(url, init);
  const text = await response.text();
  let body: unknown = null;
  try {
    body = text ? JSON.parse(text) : null;
  } catch {
    body = { error: { message: text.slice(0, 200) } };
  }
  if (!response.ok) {
    if (response.status === 401) {
      events.dispatchEvent(new Event('unauthorized'));
    }
    throw new ApiError(response.status, body as ErrorBody | null);
  }
  return body as T;
}

// Sends prepared image blobs as one multipart request: a new receipt, or more images on
// `receiptId`. `force` stores an exact duplicate anyway (after the user confirmed it).
export function uploadImages(blobs: Blob[], { receiptId, force = false }: { receiptId?: number; force?: boolean } = {}):
    Promise<{ id?: number }> {
  const form = new FormData();
  blobs.forEach((blob, i) => form.append('images', blob, `photo-${i + 1}.jpg`));
  const path = receiptId ? `/receipts/${receiptId}/images` : '/receipts';
  return api(path, { method: 'POST', form, query: force ? { force: 'true' } : {} });
}
