// Shapes of the API's JSON responses that the views read (src/controllers/*Controller.cpp).

export type ScanState = 'idle' | 'uploaded' | 'scanning' | 'scan_failed';
export type Kind = 'purchase' | 'refund' | 'reimbursement' | 'income' | 'transfer';

// GET /receipts and GET /receipts/:id (ReceiptController toJson).
export interface Receipt {
  id: number;
  store_name_raw: string | null;
  merchant: string | null;
  purchase_date: string | null;
  total_cents: number;
  direction: 'inflow' | 'outflow';
  currency: string;
  kind: Kind;
  scan_state: ScanState;
  tax_deductible: boolean;
  note: string | null;
  source: string;
  payment_method_id: number | null;
  offsets_receipt_id: number | null;
  fx_rate_to_aud: number | null;
  aud_equivalent_cents: number | null;
  proposed_at: string | null;
  reviewed_at: string | null;
  needs_review: boolean;
  is_confirmed: boolean;
  created_at: string;
  updated_at: string;
}

// The body of every non-2xx response; a duplicate_image 409 also names the existing image.
export interface ErrorBody {
  error?: {
    code?: string;
    message?: string;
    duplicate_of?: { receipt_id: number; image_id: number };
  };
}

export interface ScannerNotes {
  scanner_notes: string;
}
