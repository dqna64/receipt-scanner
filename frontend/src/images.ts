// Client-side image preparation before upload (spec: Capture, client-side conversion). The
// server only decodes JPEG/PNG/WebP, so HEIC must be converted here; downscaling also cuts the
// upload size. The server normalizes again (EXIF strip, 2048px, WebP), so this matches its
// long edge instead of shrinking further.

const MAX_EDGE = 2048;
const JPEG_QUALITY = 0.85;

type Decoded = ImageBitmap | HTMLImageElement;

// File -> JPEG Blob with its long edge at most MAX_EDGE, orientation applied. Throws an Error
// with a user-facing message if the file can't be read as an image at all.
export async function prepareImage(file: File): Promise<Blob> {
  let image: Decoded;
  try {
    image = await decodeNatively(file);
  } catch {
    // Detection is by decode failure, not the file name: desktop Chrome/Firefox can't decode
    // HEIC, so the libheif WASM decoder is loaded only now, the first time it is needed.
    image = await decodeHeic(file);
  }
  return encode(image);
}

// createImageBitmap first; an <img> element as a fallback for browsers whose
// createImageBitmap rejects a format their <img> decodes (older Safari with HEIC).
async function decodeNatively(blob: Blob): Promise<Decoded> {
  try {
    return await createImageBitmap(blob, { imageOrientation: 'from-image' });
  } catch {
    const url = URL.createObjectURL(blob);
    try {
      const img = new Image();
      img.src = url;
      await img.decode();
      return img;
    } finally {
      URL.revokeObjectURL(url);
    }
  }
}

async function decodeHeic(file: File): Promise<Decoded> {
  let jpeg: Blob;
  try {
    const { heicTo } = await import('heic-to');
    jpeg = await heicTo({ blob: file, type: 'image/jpeg', quality: 0.92 });
  } catch {
    throw new Error(`${file.name || 'This file'} isn't an image this browser can read (JPEG, PNG, WebP or HEIC).`);
  }
  return decodeNatively(jpeg);
}

function encode(image: Decoded): Promise<Blob> {
  const width = image instanceof HTMLImageElement ? image.naturalWidth : image.width;
  const height = image instanceof HTMLImageElement ? image.naturalHeight : image.height;
  const scale = Math.min(1, MAX_EDGE / Math.max(width, height));
  const canvas = document.createElement('canvas');
  canvas.width = Math.round(width * scale);
  canvas.height = Math.round(height * scale);
  const context = canvas.getContext('2d');
  if (!context) throw new Error('This browser cannot draw images.');
  context.drawImage(image, 0, 0, canvas.width, canvas.height);
  if (image instanceof ImageBitmap) image.close();
  return new Promise((resolve, reject) => {
    canvas.toBlob(
      (blob) => (blob ? resolve(blob) : reject(new Error('Could not re-encode the image.'))),
      'image/jpeg',
      JPEG_QUALITY,
    );
  });
}
