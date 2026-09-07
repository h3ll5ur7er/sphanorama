/**
 * The overlay's elements: a ring per cell in view, and an arrow toward the one out of view.
 *
 * Separated from `planOverlay` because everything here is a drawing detail — element reuse, an SVG
 * arc, a rotation — and none of it is a decision worth asserting. What is worth asserting lives
 * next door and is tested without a browser.
 *
 * Rings are absolutely positioned HTML rather than shapes in one big SVG. A single SVG spanning
 * the viewfinder would have to choose between letterboxing its own coordinate system, which puts
 * markers in the wrong place, and stretching it, which turns every circle into an ellipse.
 * Positioning in percentages sidesteps both: the browser does the arithmetic in the frame the
 * markers were computed in.
 */
import type { Overlay, RingMark } from './overlay';

/** The circle the fill is drawn on, in the ring's own tiny coordinate system. */
const RADIUS = 9;
const CIRCUMFERENCE = 2 * Math.PI * RADIUS;

function ringElement(): HTMLElement {
  const host = document.createElement('div');
  host.className = 'cell-ring';
  host.innerHTML =
    `<svg viewBox="0 0 24 24" aria-hidden="true">`
    + `<circle class="ring-track" cx="12" cy="12" r="${RADIUS}"></circle>`
    // Rotated so the fill starts at the top and runs clockwise, which is where a person expects a
    // progress arc to begin. An arc starting at three o'clock reads as a mistake even when it is
    // measuring the right thing.
    + `<circle class="ring-fill" cx="12" cy="12" r="${RADIUS}" transform="rotate(-90 12 12)"></circle>`
    + `</svg>`;
  return host;
}

function paintRing(host: HTMLElement, ring: RingMark): void {
  // Which cell this element is for. The painter keys its pool by it, and writing it down is what
  // makes that checkable from outside — a pool that handed one cell's element to another looked
  // identical in the DOM without it, and the test that was supposed to catch it passed by accident.
  host.dataset.node = String(ring.node);
  host.style.left = `${(ring.x * 100).toFixed(3)}%`;
  host.style.top = `${(ring.y * 100).toFixed(3)}%`;
  host.dataset.target = String(ring.isTarget);
  // Told apart in the stylesheet rather than by the fill, which says the same thing about a cell
  // that is captured and one the user has just finished holding on.
  host.dataset.captured = String(ring.captured);
  const fill = host.querySelector<SVGCircleElement>('.ring-fill');
  if (fill === null) return;
  // Dash the whole circumference and slide the gap: a fill of zero hides the arc entirely and a
  // fill of one closes it, with every fraction between drawn without a second element.
  fill.style.strokeDasharray = `${CIRCUMFERENCE.toFixed(3)}`;
  fill.style.strokeDashoffset = `${(CIRCUMFERENCE * (1 - ring.fill)).toFixed(3)}`;
}

export interface OverlayPainter {
  show(overlay: Overlay): void;
}

export function createOverlayPainter(layer: HTMLElement, arrow: HTMLElement): OverlayPainter {
  const glyph = arrow.querySelector<SVGElement>('svg');
  const away = arrow.querySelector<HTMLElement>('.arrow-away');
  // Kept and reused rather than rebuilt each frame. This runs on every animation frame beside a
  // camera and a WASM core, and replacing the children thirty times a second would churn the DOM
  // for a set of markers that mostly just move a little.
  //
  // **Keyed by cell, not by position in the list.** A pool indexed by array position hands one
  // frame's element to a different cell the next frame, as soon as the set of visible cells
  // changes — which is every time the phone turns. The element keeps the CSS state it had, and
  // `.ring-fill` has a transition on it, so the new cell's ring is drawn part-way through the
  // *previous* cell's arc: measured at 40 ms after a pan, the colour had already snapped to
  // "captured" while the arc was 72% drawn. A captured cell rendered as a partly filled ring is
  // exactly the ambiguity this change exists to remove — a per-render slot doing a per-cell
  // identity's job.
  const rings = new Map<number, HTMLElement>();

  return {
    show(overlay) {
      const drawn = new Set<number>();
      for (const ring of overlay.rings) {
        const key = ring.node as number;
        let host = rings.get(key);
        if (host === undefined) {
          host = ringElement();
          rings.set(key, host);
          layer.append(host);
        }
        host.hidden = false;
        paintRing(host, ring);
        drawn.add(key);
      }
      // A cell that is no longer in view keeps its element, hidden. The plan is fixed for a
      // session, so the map is bounded by its cell count and never grows past it.
      for (const [key, host] of rings) {
        if (!drawn.has(key)) host.hidden = true;
      }

      arrow.hidden = overlay.arrow === null;
      if (overlay.arrow !== null) {
        // Placed rather than centred: the arrow sits out on the side the cell is on, so it reads
        // as a signpost instead of a compass needle. The glyph alone is rotated — rotating the
        // wrapper would stand the distance beside it on its head.
        arrow.style.left = `${(overlay.arrow.x * 100).toFixed(3)}%`;
        arrow.style.top = `${(overlay.arrow.y * 100).toFixed(3)}%`;
        if (glyph !== null) {
          glyph.style.transform = `rotate(${overlay.arrow.bearingDeg.toFixed(1)}deg)`;
        }
        if (away !== null) away.textContent = `${Math.round(overlay.arrow.awayDeg)}°`;
      }
    },
  };
}
