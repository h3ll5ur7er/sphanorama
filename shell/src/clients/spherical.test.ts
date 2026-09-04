// The one conversion both clients share, and the bound the coverage map's arithmetic rests on.
//
// There is deliberately no test that aimOf and aimOfDirection agree: aimOf *is* a call to
// aimOfDirection, so such a test could not fail and would only look like coverage.
import { describe, expect, it } from 'vitest';

import { aimOf } from './spherical';
import type { Quat } from '../../../contracts/ts/contracts';

/** A cell aimed by azimuth then elevation, as the core's FromAzimuthElevation builds one. */
function attitude(azimuthDeg: number, elevationDeg: number): Quat {
  const yaw = (azimuthDeg * Math.PI) / 360;
  const pitch = (elevationDeg * Math.PI) / 360;
  const [cy, sy] = [Math.cos(yaw), Math.sin(yaw)];
  const [cp, sp] = [Math.cos(pitch), Math.sin(pitch)];
  return { w: cy * cp, x: cy * sp, y: sy * cp, z: -sy * sp };
}

describe('aimOf', () => {
  it('keeps azimuth inside the range the map divides by', () => {
    // mapCoverage wraps with `(azimuthDeg + 180) % 360`, which would go negative for anything
    // below -180 and put a dot off the left edge. atan2 cannot answer below -pi, and -pi lands on
    // exactly -180, so the wrap has no negative branch to take — asserted rather than assumed.
    for (let azimuth = -180; azimuth <= 180; azimuth += 0.25) {
      const { azimuthDeg } = aimOf(attitude(azimuth, 0));
      expect(azimuthDeg).toBeGreaterThanOrEqual(-180);
      expect(azimuthDeg).toBeLessThanOrEqual(180);
      expect(((azimuthDeg + 180) % 360) / 360).toBeGreaterThanOrEqual(0);
    }
  });
});
