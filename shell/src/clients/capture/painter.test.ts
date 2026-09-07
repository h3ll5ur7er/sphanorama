// The half of the overlay that reaches the DOM.
//
// `overlay.test.ts` decides which rings should exist, where, and with what fill; nothing asserted
// that any of it became elements. That gap had a measurable size: a one-letter typo in
// `host.dataset.captured` passed the whole gate, because `data-captured` appeared in exactly two
// files — the painter that writes it and the stylesheet that reads it — and in no test.
import { describe, expect, it } from 'vitest';

import { createOverlayPainter } from './painter';
import type { Overlay, RingMark } from './overlay';
import type { NodeId } from '../../../../contracts/ts/contracts';

function ring(over: Partial<RingMark> = {}): RingMark {
  return {
    node: 1 as NodeId, x: 0.5, y: 0.5, fill: 0, isTarget: false, captured: false, ...over,
  };
}

function paint(): { layer: HTMLElement; arrow: HTMLElement; show: (o: Overlay) => void } {
  const layer = document.createElement('div');
  const arrow = document.createElement('div');
  arrow.innerHTML = '<svg></svg><span class="arrow-away"></span>';
  const painter = createOverlayPainter(layer, arrow);
  return { layer, arrow, show: (o) => painter.show(o) };
}

const visible = (layer: HTMLElement) =>
  Array.from(layer.querySelectorAll<HTMLElement>('.cell-ring')).filter((el) => !el.hidden);

describe('the painter', () => {
  it('puts a cell being captured onto the element the stylesheet reads', () => {
    const { layer, show } = paint();
    show({ rings: [ring({ node: 4 as NodeId, captured: true, isTarget: true })], arrow: null });

    const [host] = visible(layer);
    expect(host.dataset.captured).toBe('true');
    expect(host.dataset.target).toBe('true');
  });

  it('says false rather than nothing for a cell that is not captured', () => {
    // The stylesheet matches `[data-captured='true']`, so a missing attribute would look the same
    // as `false` today — and would stop looking the same the moment anyone writes `[data-captured]`.
    const { layer, show } = paint();
    show({ rings: [ring({ captured: false })], arrow: null });
    expect(visible(layer)[0].dataset.captured).toBe('false');
  });

  it('gives a cell the same element every frame, however the visible set changes', () => {
    // The finding this test exists for: a pool indexed by array position hands one frame's element
    // to a different cell the next frame. The element carries its CSS state with it, and
    // `.ring-fill` has a transition — so the new cell's ring is drawn part-way through the
    // previous cell's arc. Measured in Chromium as a captured cell showing a 72%-drawn ring.
    const { layer, show } = paint();
    show({ rings: [ring({ node: 1 as NodeId }), ring({ node: 2 as NodeId, x: 0.7 })], arrow: null });
    const forCellTwo = layer.querySelector<HTMLElement>('.cell-ring[data-node="2"]');
    expect(forCellTwo).not.toBeNull();

    // Cell 1 leaves the view, so cell 2 is now first in the list — the moment an index-keyed pool
    // hands it the element that was drawing cell 1.
    show({ rings: [ring({ node: 2 as NodeId, x: 0.7, captured: true })], arrow: null });

    // Asserted as object identity for *this cell*, not as "some element changed". An earlier
    // version of this test checked that the visible element was not cell 1's, which a pool that
    // painted the wrong element and then hid it satisfied by accident — it left cell 2's stale
    // element on screen and passed. Predicting the sabotage's failure set before running it is
    // what caught that.
    expect(visible(layer)).toHaveLength(1);
    expect(visible(layer)[0]).toBe(forCellTwo);
    expect(visible(layer)[0].dataset.captured).toBe('true');
  });

  it('draws the arc from the fill, and closes it at one', () => {
    const { layer, show } = paint();
    show({ rings: [ring({ fill: 1 })], arrow: null });
    const closed = visible(layer)[0].querySelector<SVGCircleElement>('.ring-fill');
    expect(closed?.style.strokeDashoffset).toBe('0.000');

    show({ rings: [ring({ fill: 0 })], arrow: null });
    const open = visible(layer)[0].querySelector<SVGCircleElement>('.ring-fill');
    expect(Number(open?.style.strokeDashoffset)).toBeCloseTo(Number(open?.style.strokeDasharray), 3);
  });

  it('hides the arrow when there is nothing to point at, and raises it when there is', () => {
    // The property, not the appearance: whether `hidden` actually removes it from the picture is a
    // cascade question the stylesheet answers, and the browser suite asserts that half.
    const { arrow, show } = paint();
    show({ rings: [ring()], arrow: null });
    expect(arrow.hidden).toBe(true);

    show({ rings: [ring()], arrow: { x: 0.9, y: 0.5, bearingDeg: 42, awayDeg: 73 } });
    expect(arrow.hidden).toBe(false);
    expect(arrow.querySelector('svg')?.style.transform).toBe('rotate(42.0deg)');
    expect(arrow.querySelector('.arrow-away')?.textContent).toBe('73°');
  });
});
