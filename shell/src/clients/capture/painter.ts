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
  host.style.left = `${(ring.x * 100).toFixed(3)}%`;
  host.style.top = `${(ring.y * 100).toFixed(3)}%`;
  host.dataset.target = String(ring.isTarget);
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
  // Kept and reused rather than rebuilt each frame. This runs on every animation frame beside a
  // camera and a WASM core, and replacing the children thirty times a second would churn the DOM
  // for a set of markers that mostly just move a little.
  const rings: HTMLElement[] = [];

  return {
    show(overlay) {
      while (rings.length < overlay.rings.length) {
        const host = ringElement();
        rings.push(host);
        layer.append(host);
      }
      overlay.rings.forEach((ring, index) => {
        const host = rings[index];
        host.hidden = false;
        paintRing(host, ring);
      });
      for (let spare = overlay.rings.length; spare < rings.length; spare += 1) {
        rings[spare].hidden = true;
      }

      arrow.hidden = overlay.arrow === null;
      if (overlay.arrow !== null) {
        arrow.style.transform = `rotate(${overlay.arrow.bearingDeg.toFixed(1)}deg)`;
      }
    },
  };
}
