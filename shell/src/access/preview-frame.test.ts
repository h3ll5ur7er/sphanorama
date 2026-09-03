// Turning what the viewfinder is showing into bytes the core can be handed (ADR 0021).
//
// The browser half — drawImage from a live <video> — is the browser's to get right and is covered
// end to end. What is tested here is the adaptation: the size the frame is reduced to, the state
// where there is nothing to grab yet, and the buffer being handed over rather than retained.
import { describe, expect, it } from 'vitest';

import { GRAB_MAX_EDGE, createFrameGrabber, type DrawTarget } from './preview-frame';

/** A canvas that records what it was asked to draw and returns pixels of a known colour. */
function fakeCanvas(fill = 0x40) {
  const draws: { width: number; height: number }[] = [];
  const target: DrawTarget = {
    width: 0,
    height: 0,
    draw(source, width, height) {
      this.width = width;
      this.height = height;
      draws.push({ width, height });
      void source;
    },
    read(width, height) {
      return new Uint8ClampedArray(width * height * 4).fill(fill);
    },
  };
  return { target, draws };
}

/** A <video> as this file uses one: two dimensions and a readiness flag. */
const video = (videoWidth: number, videoHeight: number, readyState = 2) =>
  ({ videoWidth, videoHeight, readyState }) as HTMLVideoElement;

describe('the frame grabber', () => {
  it('hands back RGBA bytes matching the size it drew', () => {
    const { target } = fakeCanvas();
    const grab = createFrameGrabber(target);

    const frame = grab(video(640, 480));

    expect(frame).not.toBeNull();
    expect(frame!.bytes.length).toBe(frame!.width * frame!.height * 4);
  });

  it('keeps a small frame at its own size rather than scaling it up', () => {
    const { target, draws } = fakeCanvas();
    const grab = createFrameGrabber(target);

    grab(video(320, 240));

    expect(draws[0]).toEqual({ width: 320, height: 240 });
  });

  it('reduces a large frame to the long-edge cap, keeping its aspect ratio', () => {
    // A phone's 4K preview is 33 MB of RGBA per frame, and a burst is five of them. The cap is
    // what keeps a burst inside a heap ceiling measured in the low hundreds of megabytes.
    const { target, draws } = fakeCanvas();
    const grab = createFrameGrabber(target);

    grab(video(4000, 3000));

    expect(draws[0].width).toBe(GRAB_MAX_EDGE);
    expect(draws[0].height).toBe(Math.round((GRAB_MAX_EDGE * 3000) / 4000));
  });

  it('caps the long edge whichever way the phone is held', () => {
    const { target, draws } = fakeCanvas();
    const grab = createFrameGrabber(target);

    grab(video(3000, 4000));

    expect(draws[0].height).toBe(GRAB_MAX_EDGE);
    expect(draws[0].width).toBe(Math.round((GRAB_MAX_EDGE * 3000) / 4000));
  });

  it('reports nothing when the video has no frame yet', () => {
    // readyState below HAVE_CURRENT_DATA means drawImage would paint a blank, and a blank frame
    // scored as a candidate is worse than a missing one: it looks like a capture that happened.
    const { target, draws } = fakeCanvas();
    const grab = createFrameGrabber(target);

    expect(grab(video(640, 480, 0))).toBeNull();
    expect(draws).toHaveLength(0);
  });

  it('reports nothing when the video has no dimensions yet', () => {
    const { target } = fakeCanvas();
    const grab = createFrameGrabber(target);

    expect(grab(video(0, 0))).toBeNull();
  });

  it('gives each grab its own buffer, because the last one was transferred away', () => {
    // The buffer is handed to the worker by transfer, which detaches it here. Reusing one would
    // make the second grab write into memory this side no longer owns.
    const { target } = fakeCanvas();
    const grab = createFrameGrabber(target);

    const first = grab(video(640, 480))!;
    const second = grab(video(640, 480))!;

    expect(second.bytes.buffer).not.toBe(first.bytes.buffer);
  });
});
