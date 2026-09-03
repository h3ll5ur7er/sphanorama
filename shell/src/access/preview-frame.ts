/**
 * Turning what the viewfinder is showing into bytes the core can be handed (ADR 0021).
 *
 * This is the page's half of `ICameraAccess::PeekPreviewFrame`. The core reads that port
 * synchronously from the worker, so a frame has to be resident there before it is asked for —
 * the page grabs one and transfers the buffer across, and the worker holds the latest (ADR 0019).
 *
 * A `<video>` element is the only place the frames are: `MediaStreamTrackProcessor` would give
 * the worker the track directly, but Safari does not have it, so the page draws instead.
 */

/**
 * The longest edge a grabbed frame is reduced to.
 *
 * Not an arbitrary quality knob — it is a memory budget. RGBA is four bytes a pixel with no
 * compression anywhere in this path yet, so a phone's 4000×3000 preview is 48 MB per frame and
 * the default five-frame burst would be 240 MB against a heap ceiling of 128. At 1280 the same
 * burst is 22 MB, which fits with room for the sphere's other cells.
 *
 * It costs real resolution and that is a debt, not a design: the frame that gets *stitched*
 * should be the full-resolution one. Paying it properly means encoding to JPEG before the frame
 * crosses — `PixelFormat::EncodedJpeg` is in the contract for exactly this — which is its own
 * change, with a decode on the other side that nothing currently needs.
 */
export const GRAB_MAX_EDGE = 1280;

/**
 * Somewhere to draw a video frame and read the pixels back.
 *
 * An interface rather than a `CanvasRenderingContext2D` so the arithmetic above can be tested
 * without a browser — and because the real implementation may well become an `OffscreenCanvas`
 * once frame acquisition moves off the UI thread (04 §4.1).
 */
export interface DrawTarget {
  width: number;
  height: number;
  draw(source: CanvasImageSource, width: number, height: number): void;
  read(width: number, height: number): Uint8ClampedArray;
}

/** RGBA8, tightly packed: `PixelFormat::RGBA8` and a stride of `width * 4`, as the core reads it. */
export interface GrabbedFrame {
  width: number;
  height: number;
  bytes: Uint8Array;
}

/** HTMLMediaElement.HAVE_CURRENT_DATA — there is a frame at the current position. */
const HAVE_CURRENT_DATA = 2;

export function fitWithin(width: number, height: number, maxEdge: number) {
  const longest = Math.max(width, height);
  if (longest <= maxEdge) return { width, height };
  const scale = maxEdge / longest;
  // Rounded, then floored at one: a 4000×1 frame would otherwise scale to a height of zero and
  // produce an allocation of nothing that still claims to be a frame.
  return {
    width: Math.max(1, Math.round(width * scale)),
    height: Math.max(1, Math.round(height * scale)),
  };
}

export function createFrameGrabber(target: DrawTarget) {
  return (video: HTMLVideoElement): GrabbedFrame | null => {
    // Both guards are the same situation seen from two angles — the stream is open but has not
    // produced a frame. Drawing anyway paints a blank, and a blank frame that gets scored is
    // worse than a missing one: it looks to every count-based check like a capture that happened.
    if (video.readyState < HAVE_CURRENT_DATA) return null;
    if (video.videoWidth <= 0 || video.videoHeight <= 0) return null;

    const { width, height } = fitWithin(video.videoWidth, video.videoHeight, GRAB_MAX_EDGE);
    target.draw(video, width, height);
    const pixels = target.read(width, height);

    // A fresh buffer each time, because the last one was transferred to the worker and is
    // detached here. `Uint8Array` over the same memory rather than a copy: this is per burst
    // frame on the hot path, and the copy that matters is the one into the frame store.
    return { width, height, bytes: new Uint8Array(pixels.buffer, pixels.byteOffset, pixels.length) };
  };
}

/** The real target: a 2D canvas kept for the session, resized as the frame size demands. */
export function canvasDrawTarget(canvas: HTMLCanvasElement): DrawTarget {
  const context = canvas.getContext('2d', { willReadFrequently: true });
  if (context === null) throw new Error('this browser would not give a 2D canvas context');
  return {
    get width() { return canvas.width; },
    set width(value: number) { canvas.width = value; },
    get height() { return canvas.height; },
    set height(value: number) { canvas.height = value; },
    draw(source, width, height) {
      // Resizing clears the canvas, so it is done before the draw and only when it changed —
      // setting the same size every frame would throw the buffer away for nothing.
      if (canvas.width !== width) canvas.width = width;
      if (canvas.height !== height) canvas.height = height;
      context.drawImage(source, 0, 0, width, height);
    },
    read: (width, height) => context.getImageData(0, 0, width, height).data,
  };
}
