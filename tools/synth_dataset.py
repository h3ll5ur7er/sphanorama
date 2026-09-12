#!/usr/bin/env python3
"""Render the frames a phone would have captured from a panorama, with the truth of where it looked.

Phase 2 measures registration accuracy in degrees against known rotations, and this is what knows
them. It takes an equirectangular panorama and a list of camera orientations, and emits one image
per orientation plus a `truth.json` carrying the rotation and the lens that produced each.

**It implements the lens itself rather than calling the core.** `camera_model` in
`core/src/utilities/` does the same arithmetic, and rendering a dataset *through* it would mean any
error the two share cancels out — the harness would certify a broken projection as accurate, which
is the one failure a measuring instrument must not have.

Be precise about what that buys, because the first version of this paragraph was not. This is **not
an independent re-derivation**: the term-by-term form below is the core's, closely enough that a
reviewer could point at matching expression names and a comment copied word for word. What carries
the weight is that both implementations are pinned to the same hand-worked decimals, taken from the
published Brown-Conrady definition and derived from neither of them (see
`test_distortion_terms_are_opencvs_in_opencvs_order` and its twin in the C++ suite). Two
implementations agreeing with an outside number is evidence; one implementation checked against
itself is not.

And the danger of a close port is not hypothetical — it is how the fold defect happened. This file
kept the core's arithmetic and dropped its guards, so it inverted the distortion past the fold and
answered 468 of 3072 pixels with fabricated directions, while the round-trip check that was supposed
to notice passed every one of them. See `inverts_along_the_ray`, and ADR 0050.

Conventions, matching `contracts/cpp/sphanorama/types.h` and ADR 0046 exactly, because a dataset
expressed in a different frame from the core that reads it is worse than no dataset:

- **Camera space**: -Z forward, +Y up, +X right.
- **Image space**: +x right, +y down, origin at the top-left corner, `cx = width / 2`. That last is
  half a pixel away from OpenCV's `(width - 1) / 2`, and a residual is measured in pixels.
- **World space**: the same axes; a `Pose` is device -> world, as `Quat` is.
- **Equirectangular**: longitude 0 is -Z (forward) and increases toward +X; latitude +90 is +Y and
  sits at row 0. So forward lands at exactly `(width / 2, height / 2)` in the panorama's own
  coordinates -- which is a pixel *corner*, not a pixel centre, since an integer here is an edge
  (see `direction_to_equirect`). On a 2048x1024 panorama that is where pixels 1023 and 1024 meet.

Run it: `uv run --group datasets tools/synth_dataset.py --out datasets/ring` (the `datasets` group
is what carries numpy; the checkers stay standard-library only — ADR 0048, ADR 0050).
"""
from __future__ import annotations

import argparse
import json
import math
import shutil
from dataclasses import asdict, dataclass
from pathlib import Path

import numpy as np

# Newton on the distortion, matching the core's solver in shape if not in code. The core measured
# its own budget; this one is generous because it runs offline on whole images at a time and an
# extra pass costs nothing a person would notice.
INVERSE_ITERATIONS = 30

# The settled-step early exit — what the core calls `kSettledStepNormalised`. The name says
# `TOLERANCE` for historical reasons and it is *not* the round-trip bound; that is the constant
# below, which is the one that decides refusals.
INVERSE_TOLERANCE = 1e-12

# The round-trip acceptance bound, in **normalised** units, which is the core's
# `kInverseToleranceNormalised` and the same number. This used to be an unnamed `1e-6` compared
# against *pixels*, so the refusal boundary moved with the focal length: 27 times looser than the
# core at 48x36 and 3 times tighter at 4000x3000. Two implementations pinned to each other should
# not disagree about where a refusal is, and a bound in the wrong unit is how that happens — the
# same lesson the render tolerance taught in colour components (ADR 0050).
INVERSE_ACCEPTANCE_NORMALISED = 1e-9

# How far a step may be halved, and how far a starting guess may be pulled toward the optical
# centre, before the pixel is given up on.
BACKTRACK_STEPS = 30


@dataclass
class Intrinsics:
    """A pinhole lens with Brown-Conrady distortion, in OpenCV's `k1 k2 p1 p2 k3` convention."""

    fx: float = 0.0
    fy: float = 0.0
    cx: float = 0.0
    cy: float = 0.0
    k1: float = 0.0
    k2: float = 0.0
    k3: float = 0.0
    p1: float = 0.0
    p2: float = 0.0
    width: int = 0
    height: int = 0


@dataclass(frozen=True)
class Pose:
    """A camera orientation, device -> world, as a unit quaternion."""

    w: float = 1.0
    x: float = 0.0
    y: float = 0.0
    z: float = 0.0

    @staticmethod
    def identity() -> "Pose":
        return Pose()

    @staticmethod
    def from_axis_angle(axis: tuple[float, float, float], radians: float) -> "Pose":
        length = math.sqrt(sum(component * component for component in axis))
        if length == 0.0:
            return Pose()
        half = radians * 0.5
        scale = math.sin(half) / length
        return Pose(math.cos(half), axis[0] * scale, axis[1] * scale, axis[2] * scale)

    @staticmethod
    def from_azimuth_elevation(azimuth_deg: float, elevation_deg: float) -> "Pose":
        """Azimuth turns about +Y from the forward axis; elevation lifts toward +Y.

        The same convention `FromAzimuthElevation` uses, so a pose written here names the same
        direction the coverage planner would.
        """
        yaw = Pose.from_axis_angle((0.0, 1.0, 0.0), math.radians(azimuth_deg))
        pitch = Pose.from_axis_angle((1.0, 0.0, 0.0), math.radians(elevation_deg))
        return yaw.then(pitch)

    def then(self, other: "Pose") -> "Pose":
        """This rotation followed by `other` in its own frame — quaternion multiply, self * other."""
        return Pose(
            self.w * other.w - self.x * other.x - self.y * other.y - self.z * other.z,
            self.w * other.x + self.x * other.w + self.y * other.z - self.z * other.y,
            self.w * other.y - self.x * other.z + self.y * other.w + self.z * other.x,
            self.w * other.z + self.x * other.y - self.y * other.x + self.z * other.w,
        )

    def normalised(self) -> "Pose":
        """The unit quaternion naming the same rotation, or a refusal.

        Scaled by the largest component before anything is squared. `sqrt(w*w + x*x + ...)`
        overflows on a component near 1e200 and underflows near 1e-200, and in both cases the norm
        comes back useless — infinity or zero — so every component divides to zero or takes an early
        exit, and a 180 degree turn silently renders as the identity. That is PR #49 round 14's
        `Normalize` overflow, which the C++ fixed this way and this file then reintroduced.

        Refusing is the other half. A quaternion of all zeros is not a rotation; it used to render
        as the identity and be *written down* as `{0, 0, 0, 0}`, which `sphanorama::Normalize` turns
        back into the identity on the far side — so nothing anywhere would have said that two
        identical frames carried two different rotations.
        """
        components = (self.w, self.x, self.y, self.z)
        if not all(math.isfinite(component) for component in components):
            raise ValueError(f"a rotation needs four finite components, not {components}")
        largest = max(abs(component) for component in components)
        if largest == 0.0:
            raise ValueError("an all-zero quaternion names no rotation")
        w, x, y, z = (component / largest for component in components)
        norm = math.sqrt(w * w + x * x + y * y + z * z)
        return Pose(w / norm, x / norm, y / norm, z / norm)

    def rotate(self, vectors: np.ndarray) -> np.ndarray:
        """Rotate an (N, 3) array of vectors from camera space into world space.

        Normalised first, which `sphanorama::Rotate` also does and this did not. A quaternion 1%
        off unit rotated a direction 0.69 degrees wrong while `truth.json` recorded the rotation it
        was *asked* for — so the frames and the ground truth would have disagreed by more than the
        thing the harness is built to measure.
        """
        unit = self.normalised()
        w, x, y, z = unit.w, unit.x, unit.y, unit.z
        u = np.array([x, y, z], dtype=float)
        cross = np.cross(u, vectors)
        return vectors + 2.0 * w * cross + 2.0 * np.cross(u, cross)


def lens_from_fov(horizontal_fov_deg: float, vertical_fov_deg: float, width: int,
                  height: int) -> Intrinsics:
    """The lens a camera with this field of view would have, distortion-free.

    Refuses at 180 degrees and wider, where the half-angle's tangent is infinite and a rectilinear
    lens has stopped existing — the same boundary `LensFromFieldOfView` draws.
    """
    for angle in (horizontal_fov_deg, vertical_fov_deg):
        if not math.isfinite(angle) or angle <= 0.0 or angle >= 180.0:
            raise ValueError(f"field of view {angle} is not one a rectilinear lens can have")
    if width <= 0 or height <= 0:
        raise ValueError("a frame needs a positive width and height")

    half_width = width / 2.0
    half_height = height / 2.0
    return Intrinsics(
        fx=half_width / math.tan(math.radians(horizontal_fov_deg) / 2.0),
        fy=half_height / math.tan(math.radians(vertical_fov_deg) / 2.0),
        cx=half_width,
        cy=half_height,
        width=width,
        height=height,
    )


def is_usable_lens(lens: Intrinsics) -> bool:
    """Whether this describes a camera at all — the core's `IsUsableLens`.

    A default `Intrinsics` has `fx = fy = 0`, which maps the entire world onto pixel (0, 0) and
    reports every one of them valid. Nothing here caught that: the arithmetic is all finite and the
    round trip agrees with itself, because a constant map is its own inverse everywhere.
    """
    values = (lens.fx, lens.fy, lens.cx, lens.cy,
              lens.k1, lens.k2, lens.k3, lens.p1, lens.p2)
    if not all(math.isfinite(value) for value in values):
        return False
    if not (lens.fx > 0.0 and lens.fy > 0.0 and lens.width > 0 and lens.height > 0):
        return False
    # The optical centre has to be inside the image — the core requires it and this claimed to be
    # the core's function while dropping the clause. A lens whose centre is outside its own frame is
    # not something the fold reasoning, the field of view or the projection were written to
    # describe, and refusing it up front is cheaper than reasoning about each in turn.
    return (0.0 < lens.cx < float(lens.width)) and (0.0 < lens.cy < float(lens.height))


def _radial(lens: Intrinsics, r2: np.ndarray) -> np.ndarray:
    return 1.0 + r2 * (lens.k1 + r2 * (lens.k2 + r2 * lens.k3))


@dataclass
class Distorted:
    """Where the distortion sends a point, and its Jacobian there, in one evaluation.

    One evaluation because there were four. `_distort`, `defined_at`, the Newton residual and the
    backtracking trial each wrote Brown-Conrady out again, and the core carries a comment recording
    that it removed exactly this duplication for exactly this reason: a fact held in two places will
    drift, and the drift here is the fold test disagreeing with the solver about where the fold is —
    which is the disagreement `camera_model` exists to prevent.
    """
    x: np.ndarray
    y: np.ndarray
    radial: np.ndarray
    dxdx: np.ndarray
    dydy: np.ndarray
    cross: np.ndarray
    determinant: np.ndarray


def distort_at(lens: Intrinsics, xn: np.ndarray, yn: np.ndarray) -> Distorted:
    """The core's `DistortAt`: the distorted point and the Jacobian at it, together."""
    r2 = xn * xn + yn * yn
    radial = _radial(lens, r2)
    x = xn * radial + 2.0 * lens.p1 * xn * yn + lens.p2 * (r2 + 2.0 * xn * xn)
    y = yn * radial + lens.p1 * (r2 + 2.0 * yn * yn) + 2.0 * lens.p2 * xn * yn
    d_radial = lens.k1 + r2 * (2.0 * lens.k2 + r2 * 3.0 * lens.k3)   # d(radial)/d(r2)
    dxdx = radial + 2.0 * xn * xn * d_radial + 2.0 * lens.p1 * yn + 6.0 * lens.p2 * xn
    dydy = radial + 2.0 * yn * yn * d_radial + 6.0 * lens.p1 * yn + 2.0 * lens.p2 * xn
    cross = 2.0 * xn * yn * d_radial + 2.0 * lens.p1 * xn + 2.0 * lens.p2 * yn
    return Distorted(x=x, y=y, radial=radial, dxdx=dxdx, dydy=dydy, cross=cross,
                     determinant=dxdx * dydy - cross * cross)


def defined_at(lens: Intrinsics, xn: np.ndarray, yn: np.ndarray) -> np.ndarray:
    """Whether the forward map is orientation-preserving here — the core's `DefinedAt`.

    Both halves are needed and the first is the one this file went without. A radius past the fold
    has `radial < 0`, which flips the sign of `xd` and makes the point map to a pixel it has no
    business at; the round trip then *passes*, because that wrong answer really is a preimage. ADR
    0046 says in as many words that a round-trip check is structurally blind to this, and it was
    right: without this test, 468 of 3072 grid pixels on a folding lens came back with fabricated
    directions, the frame corner among them, 52 degrees off axis and pointing the opposite way.
    """
    at = distort_at(lens, xn, yn)
    return (at.radial > 0.0) & (at.determinant > 0.0)


def _radial_slope(lens: Intrinsics, u: np.ndarray) -> np.ndarray:
    """d/dr of `r * radial(r^2)`, in `u = r^2` — the core's `RadialSlope`."""
    return 1.0 + u * (3.0 * lens.k1 + u * (5.0 * lens.k2 + u * 7.0 * lens.k3))


def radial_map_increases_up_to(lens: Intrinsics, r2: np.ndarray) -> np.ndarray:
    """Whether `r * radial(r^2)` increases over the *whole* way out to this radius.

    The core's `RadialMapIncreasesUpTo`, ported rather than reinvented — which is the correction
    this function exists to be. What stood here before was a whole-lens check of my own devising
    that fed the frame corner's **distorted** radius to a cubic in the **undistorted** one, so it
    was answering a question about the wrong interval and got the answer wrong in both directions.

    The slope is 1 at the optical centre, so the endpoint and every interior local minimum settle
    it; those sit at the roots of `3k1 + 10k2 u + 21k3 u^2`. A cubic can dip below zero partway out
    and return, which is why the endpoint alone is not enough.
    """
    r2 = np.asarray(r2, dtype=float)
    increases = _radial_slope(lens, r2) > 0.0        # NaN refuses, as the core's `!(x > 0.0)` does

    a, b, c = 21.0 * lens.k3, 10.0 * lens.k2, 3.0 * lens.k1
    if a == 0.0:
        if b == 0.0:
            return increases                          # affine or constant: it cannot turn
        turning_points = [-c / b]
    else:
        discriminant = b * b - 4.0 * a * c
        # A NaN discriminant is "I could not tell", not "no real roots". Taking the no-roots exit
        # on it skips both interior checks and re-admits the folded state this exists to close —
        # the core refuses by name here and says so in as many words, and this dropped that guard
        # along with the rest of them.
        if not math.isfinite(discriminant):
            return np.zeros_like(increases, dtype=bool)
        if discriminant < 0.0:
            return increases                          # never turns: the endpoint settled it
        root = math.sqrt(discriminant)
        turning_points = [(-b + root) / (2.0 * a), (-b - root) / (2.0 * a)]

    for u in turning_points:
        # A turning point outside (0, r2) says nothing about this interval, and a non-finite one
        # compares false against both bounds, which is the same answer and the right one.
        if math.isfinite(u) and u > 0.0:
            increases &= (r2 <= u) | (_radial_slope(lens, u) > 0.0)
    return increases


RAY_SAMPLES = 64


def inverts_along_the_ray(lens: Intrinsics, xn: np.ndarray, yn: np.ndarray) -> np.ndarray:
    """Whether the distortion inverts everywhere from the optical centre out to this point.

    The core's `DistortionInvertsAlongTheRay`. **`defined_at` is not this**, and using it as though
    it were is the defect that survived round 1: a pointwise test says the map is fine *here* and
    says nothing about what it did on the way, so a solution that leapt the fold and landed
    somewhere well-behaved on the far side passes it. Measured on `k1 = -1, k2 = 0.3`, 36,036
    pixels were answered that way, the frame corner among them, 24.7 degrees from the truth.

    The radial half is exact and closed-form. The tangential half is not — `p1` and `p2` break the
    reduction to one dimension — so the determinant is sampled along the ray, and a fold thinner
    than a sixty-fourth of it slips through. That is a limit worth stating rather than a guarantee
    worth implying, and it is the core's limit too.
    """
    inverts = radial_map_increases_up_to(lens, xn * xn + yn * yn)
    if lens.p1 == 0.0 and lens.p2 == 0.0:
        return inverts                                # radial only, and that was exact
    for i in range(1, RAY_SAMPLES + 1):
        t = i / RAY_SAMPLES
        inverts &= defined_at(lens, xn * t, yn * t)
    return inverts


def _distort(lens: Intrinsics, xn: np.ndarray, yn: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    """Brown-Conrady forward. Through `distort_at`, which *is* `DistortAt`."""
    at = distort_at(lens, xn, yn)
    return at.x, at.y


def project(lens: Intrinsics, camera_space: np.ndarray) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    """Camera-space directions to pixels. Returns (u, v, valid), one entry per input row."""
    if not is_usable_lens(lens):
        raise ValueError("this is not a lens: fx, fy, width and height must all be positive")

    camera_space = np.asarray(camera_space, dtype=float)
    depth = -camera_space[:, 2]

    # `> 0` rather than `<= 0`, so a NaN refuses rather than passes — the same shape as the core's
    # guard, and for the same reason. Finiteness is separate and was missing: an infinite depth is
    # greater than zero, divides to `xn = yn = 0`, and answers the principal point for a direction
    # that is not a measurement.
    valid = np.isfinite(depth) & (depth > 0.0)
    valid &= np.isfinite(camera_space).all(axis=-1)
    safe_depth = np.where(valid, depth, 1.0)
    xn = camera_space[:, 0] / safe_depth
    yn = -camera_space[:, 1] / safe_depth      # the world's up is the image's down
    valid &= np.isfinite(xn) & np.isfinite(yn)

    # The core's `Project` refuses a direction past the fold, and this did not — which mattered more
    # here than anywhere, because `unproject` adjudicates its own answer by projecting it back. Two
    # implementations that share a blind spot agree by construction rather than by being right.
    # Nothing downstream computes on a row that has already been refused. Substituting here rather
    # than only at the ray test is what makes the finiteness conjunct above load-bearing: without
    # it an infinity reaches the cubic and the Jacobian, and the only thing that noticed was a numpy
    # warning nobody reads. A guard whose effect is invisible is one the next reviewer deletes.
    safe_xn = np.where(valid, xn, 0.0)
    safe_yn = np.where(valid, yn, 0.0)
    valid &= inverts_along_the_ray(lens, safe_xn, safe_yn)

    xd, yd = _distort(lens, safe_xn, safe_yn)
    u = lens.fx * xd + lens.cx
    v = lens.fy * yd + lens.cy
    valid &= np.isfinite(u) & np.isfinite(v)

    # Last, and after the finiteness test above has read the real values. A refused row then carries
    # no number that could be mistaken for a pixel: `safe_depth = 1.0` is not a neutral placeholder
    # — for a direction in or behind the optical plane it lands on the principal point, the most
    # plausible-looking answer available. Round 1 closed the flag and left the value beside it, and
    # this file has already shipped a caller that read the value before the flag.
    return np.where(valid, u, np.nan), np.where(valid, v, np.nan), valid


def unproject(lens: Intrinsics, pixels: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    """Pixels to unit camera-space directions, by Newton on the distortion.

    Returns (directions, valid). A pixel whose distortion does not invert is refused rather than
    answered with the nearest thing that iterated, which is the rule ADR 0046 sets for the core.
    """
    if not is_usable_lens(lens):
        raise ValueError("this is not a lens: fx, fy, width and height must all be positive")

    pixels = np.asarray(pixels, dtype=float)

    # The core refuses a non-finite pixel up front, and a non-finite `xd`/`yd` after the division.
    # Both are ported, and the story of how they briefly were not is worth keeping.
    #
    # I removed them once, on a measurement that said they change nothing in numpy — the infinity
    # propagates to NaN and the round trip refuses the row either way. The refusal part is true.
    # The quietness is not: without these, one infinite pixel raises `FloatingPointError` under
    # `errstate(all="raise")` and emits 311 RuntimeWarnings by default, because a vectorised solver
    # drags the whole batch through invalid arithmetic on its behalf. My probe had suppressed
    # exactly the two warning categories it was looking for, so it reported a silence it had
    # arranged itself, and a reviewer caught it. `project` five lines up is clean on the same input,
    # which should have been the clue.
    # **One test, not two.** The core has a pixel check and then a normalised-coordinate check;
    # ported literally that is two guards which shadow each other here — measured, either was
    # individually deletable with the suite green and only both together failed. That is round 3's
    # finding about the ray guards, which I reintroduced while fixing this one, and caught by
    # sabotaging each separately rather than the pair.
    #
    # The second subsumes the first in numpy: a non-finite pixel gives a non-finite `xd`, and so
    # does a finite pixel that divides to one, and neither subtraction nor division raises on the
    # way. So this is the whole of it, and deleting it fails the tests below.
    xd = (pixels[:, 0] - lens.cx) / lens.fx
    yd = (pixels[:, 1] - lens.cy) / lens.fy
    usable_pixel = np.isfinite(xd) & np.isfinite(yd)
    xd = np.where(usable_pixel, xd, 0.0)
    yd = np.where(usable_pixel, yd, 0.0)

    xn = xd.copy()
    yn = yd.copy()

    # Pull the starting guess in until it is somewhere the map is defined. Without this a pincushion
    # lens starts outside its own answer and the first step leaps the fold — ADR 0046 records the
    # core refusing pixels its own `Project` had just produced for exactly that reason.
    for _ in range(BACKTRACK_STEPS):
        outside = ~defined_at(lens, xn, yn)
        if not outside.any():
            break
        xn = np.where(outside, xn * 0.5, xn)
        yn = np.where(outside, yn * 0.5, yn)

    for _ in range(INVERSE_ITERATIONS):
        at = distort_at(lens, xn, yn)
        fx_ = at.x - xd
        fy_ = at.y - yd
        residual = fx_ * fx_ + fy_ * fy_
        dxdx, dydy, cross, determinant = at.dxdx, at.dydy, at.cross, at.determinant

        usable = determinant > 0.0        # not `abs(...) > eps`: a negative one is the fold
        safe = np.where(usable, determinant, 1.0)
        full_x = np.where(usable, -(dydy * fx_ - cross * fy_) / safe, 0.0)
        full_y = np.where(usable, -(dxdx * fy_ - cross * fx_) / safe, 0.0)

        # Arrived. Tested on the **full** step, before the damping, which is where the core puts it
        # and where the port did not. When Newton converges exactly the full step is zero; every
        # halving of zero puts the trial exactly where the iterate already stands, so its residual
        # is not *smaller*, the strict test below rejects all thirty in turn, and the exit that
        # exists for this sat after the loop where it could never be reached. `camera_model.cpp`
        # describes this defect in its own words, with its own measurement — and the port kept the
        # loop and dropped the exit, which is the fourth incomplete port this branch has found.
        #
        # No test pins it and none should pretend to. The core says removing its line changes no
        # answer; here that is **not quite true** and the difference is worth stating rather than
        # inheriting the C++'s claim. On an undistorted lens the accepted directions are
        # bit-identical. On `k1=-0.28, k2=0.09, p1=0.002` at 640x480 — the lens is named because a
        # reviewer rightly could not reproduce the count without it, and got a different one —
        # 87,368 of 307,200 rows move by one ULP: 3.33e-16 in a component, 1.7e-06 degrees, because
        # the iterate stops a refinement earlier. The count is a property of the lens, not a
        # constant. That is
        # four orders inside the 0.001-degree bound the render tests assert and nine inside the
        # acceptance tolerance, so it is noise rather than a behaviour change; it is simply not the
        # word "identical".
        if float(np.max(full_x * full_x + full_y * full_y, initial=0.0)) <= (
                INVERSE_TOLERANCE * INVERSE_TOLERANCE):
            break

        # Damped, and the damping is what keeps a step from crossing the fold: halve until the
        # trial point is both defined and strictly closer than where it stands.
        scale = np.ones_like(xn)
        taken = np.zeros_like(xn, dtype=bool)
        step_x = np.zeros_like(xn)
        step_y = np.zeros_like(yn)
        for _ in range(BACKTRACK_STEPS):
            trial_x = np.where(taken, xn + step_x, xn + scale * full_x)
            trial_y = np.where(taken, yn + step_y, yn + scale * full_y)
            trial = distort_at(lens, trial_x, trial_y)
            t_fx = trial.x - xd
            t_fy = trial.y - yd
            better = ((trial.radial > 0.0) & (trial.determinant > 0.0)
                      & ((t_fx * t_fx + t_fy * t_fy) < residual))
            accept = better & ~taken
            step_x = np.where(accept, scale * full_x, step_x)
            step_y = np.where(accept, scale * full_y, step_y)
            taken |= accept
            if taken.all():
                break
            scale = np.where(taken, scale, scale * 0.5)

        xn = xn + step_x
        yn = yn + step_y
        if (np.max(np.abs(step_x), initial=0.0) < INVERSE_TOLERANCE
                and np.max(np.abs(step_y), initial=0.0) < INVERSE_TOLERANCE):
            break

    directions = np.stack([xn, -yn, -np.ones_like(xn)], axis=-1)
    directions /= np.linalg.norm(directions, axis=-1, keepdims=True)

    # The fold test lives in `project`, and this asks `project`. That is the core's shape: its
    # `Unproject` has no ray check of its own, because `Project` carries one and the round trip
    # goes through it.
    #
    # A second copy stood here for one round and had to go. Not because it was wrong — because the
    # two shadowed each other, so either was individually deletable with the whole suite green,
    # *including* replacing this one with `defined_at`, which is the round-1 defect verbatim. A
    # guard that cannot be tested alone is a guard nobody can maintain, and the test written this
    # round specifically so a fold check could not be its own oracle was only ever exercising the
    # pair. One check, in the place the core puts it, and deleting it now fails that test.
    back_u, back_v, back_valid = project(lens, directions)
    # Compared in normalised units, so the bound means the same thing at every frame size.
    landed = ((np.abs(back_u - pixels[:, 0]) / lens.fx < INVERSE_ACCEPTANCE_NORMALISED)
              & (np.abs(back_v - pixels[:, 1]) / lens.fy < INVERSE_ACCEPTANCE_NORMALISED))
    finite = np.isfinite(xn) & np.isfinite(yn) & usable_pixel
    valid = back_valid & landed & finite

    # A refused direction comes back as NaN rather than as a unit vector. It used to have norm
    # exactly 1.0, so nothing about the value itself said it was not an answer — see `project`.
    return np.where(valid[:, None], directions, np.nan), valid


def direction_to_equirect(directions: np.ndarray, width: int,
                          height: int) -> tuple[np.ndarray, np.ndarray]:
    """World directions to floating-point panorama coordinates.

    Longitude 0 is -Z and increases toward +X; latitude +90 is +Y at row 0. Returns coordinates in
    pixels, where an integer value is a pixel *edge* — so the centre of pixel (0, 0) is (0.5, 0.5).
    """
    directions = np.asarray(directions, dtype=float)
    norms = np.linalg.norm(directions, axis=-1, keepdims=True)
    if not np.all(np.isfinite(norms) & (norms > 0.0)):
        raise ValueError("a zero or non-finite vector names no direction, so it has no pixel")
    unit = directions / norms

    longitude = np.arctan2(unit[:, 0], -unit[:, 2])
    latitude = np.arcsin(np.clip(unit[:, 1], -1.0, 1.0))
    u = (longitude / (2.0 * math.pi) + 0.5) * width
    v = (0.5 - latitude / math.pi) * height
    return u, v


def sample_equirect(panorama: np.ndarray, u: np.ndarray, v: np.ndarray) -> np.ndarray:
    """Bilinear sample, wrapping in longitude and clamping in latitude.

    Wrapping is not a nicety: a frame straddling the seam is the ordinary case for a sphere, and
    clamping there would draw a smear down the one column every such frame contains.
    """
    height, width = panorama.shape[:2]
    x = u - 0.5
    # `v - 0.5` without a clamp: the row indices are clipped four lines down, and a
    # measurement across v in [-3, 9] on a four-row panorama put the difference between
    # clamping here and not at exactly 0.0 — top and bottom collapse to the same row past
    # either edge, so the blend weight cannot matter. A reviewer deleted the clamp and the
    # whole suite stayed green, which is the definition this repository uses for a guard
    # that should go rather than be kept because it feels safer.
    y = v - 0.5

    x0 = np.floor(x).astype(np.int64)
    y0 = np.floor(y).astype(np.int64)
    fx = (x - x0)[:, None]
    fy = (y - y0)[:, None]

    x0m = x0 % width
    x1m = (x0 + 1) % width
    y0c = np.clip(y0, 0, height - 1)
    y1c = np.clip(y0 + 1, 0, height - 1)

    top = panorama[y0c, x0m] * (1.0 - fx) + panorama[y0c, x1m] * fx
    bottom = panorama[y1c, x0m] * (1.0 - fx) + panorama[y1c, x1m] * fx
    return top * (1.0 - fy) + bottom * fy


def render_frame(panorama: np.ndarray, lens: Intrinsics, pose: Pose) -> np.ndarray:
    """One frame, as a (height, width, channels) float array.

    **A lens with any pixel that has no ray behind it is refused, and the frame is the check.**
    There is no colour that can mean "no ray": black is a colour the scene produces, and the byte a
    refusal wrote before this was 128 — mid-grey, which an ordinary checkerboard pixel hits. A
    sentinel only means something if nothing else can produce it, and in an image nothing can be
    reserved. So refusing is the only honest answer, and it names the count.

    There used to be a separate whole-lens test in front of this, `lens_folds_in_frame`, and it is
    gone rather than fixed. It sampled the Jacobian on a 33x33 grid and tested the frame corner's
    *distorted* radius against a cubic in the *undistorted* one, so it was both unsound and asking
    about the wrong interval — measured, it passed lenses whose frames have thousands of rayless
    pixels and refused lenses that are answerable throughout. Asking `unproject` about every pixel
    of the actual frame is exact where that was a hope about resolution, and it is the same work
    the render does anyway. The core has no whole-lens check either, for the same reason.
    """
    us, vs = np.meshgrid(np.arange(lens.width) + 0.5, np.arange(lens.height) + 0.5, indexing="xy")
    pixels = np.stack([us.ravel(), vs.ravel()], axis=-1)

    camera_directions, valid = unproject(lens, pixels)
    # Before the directions are used for anything. A refused row's direction can be non-finite, and
    # `direction_to_equirect` refuses those too — with a message that names the vector rather than
    # the lens, which is the wrong diagnosis of the frame and the wrong count in it.
    if not valid.all():
        raise ValueError(
            f"{int((~valid).sum())} of {valid.size} pixels of this frame have no ray behind them: "
            "the lens stops being invertible inside its own frame, and no colour could honestly "
            "stand for a missing direction")

    world = pose.rotate(camera_directions)
    u, v = direction_to_equirect(world, panorama.shape[1], panorama.shape[0])
    colours = sample_equirect(panorama, u, v)
    return colours.reshape(lens.height, lens.width, panorama.shape[2])


def _to_bytes(frame: np.ndarray) -> np.ndarray:
    """Map a signed unit-range frame onto bytes: `b = round((value + 1) / 2 * 255)`.

    The docstring used to say "shared with the test that reads a file back", which named the one
    caller that must never share it: `test_a_written_frame_reads_back_as_the_pixels_that_were_
    rendered` re-derives this arithmetic by hand on purpose, so that four byte-path mutations fail
    it. A future reader taking the sentence at its word would have replaced that derivation with a
    call and quietly removed the only check on this function.
    """
    # `np.round(nan).astype(np.uint8)` is 0 — black, which is a colour the scene produces, and the
    # exact sentinel collision mid-grey 128 was removed for. The clip would hide it: NaN survives
    # `np.clip` unchanged and only the cast turns it into a plausible pixel.
    if not np.isfinite(frame).all():
        raise ValueError(
            f"{int((~np.isfinite(frame)).sum())} colour components are not finite, and every byte "
            "this could encode them as is a colour an ordinary scene produces")
    return np.round(np.clip((frame + 1.0) * 0.5, 0.0, 1.0) * 255.0).astype(np.uint8)


def write_dataset(out: Path, panorama: np.ndarray, lens: Intrinsics,
                  poses: list[Pose]) -> list[Path]:
    """Render every pose and write the frames beside the truth that describes them.

    Binary Netpbm rather than PNG, deliberately: a P6 file is a header and the pixels, which any
    consumer can read in a dozen lines and no consumer needs a library for. Measurement datasets are
    regenerated rather than committed, so the size is a cost nobody carries for long.

    One output of this function *is* committed, and it is the exception that proves the rule:
    `core/test/data/synthetic-ring-4` is four 48x36 frames and their `truth.json`, 22,570 bytes of
    content altogether (20,788 of frames, 1,782 of JSON). Not "on disk": `du` reports 40 KiB, of
    which 36 is the five files rounded up to 4 KiB blocks and the fortieth is the directory entry.
    It is read by the C++ loader's tests, and it is a **format** fixture rather than a measurement —
    it exists so that loader is read against bytes this writer produced rather than against its
    author's idea of the format (ADR 0053).
    The stronger claim, that this catches bugs a hand-written fixture would miss, was
    written here and then disproved by a reviewer; the ADR records what survives of it.
    """
    if panorama.ndim != 3 or panorama.shape[2] != 3:
        raise ValueError(
            f"a P6 file is three bytes a pixel and this panorama has shape {panorama.shape}; the "
            "header would describe a frame the payload is not")

    # Normalised here, not only in `rotate`. Round 1 put it in the renderer and left the record
    # alone, so the frames were made with a unit quaternion and the file wrote down whatever it was
    # handed — while claiming "unit quaternion" in its own convention block.
    poses = [pose.normalised() for pose in poses]

    # Everything renders before anything is written, so a refusal part way through leaves no files
    # rather than frames with no truth to describe them. A dozen-line consumer globs
    # `frame_*.ppm` — which is the whole pitch for P6 — and cannot tell a half-written dataset from
    # a whole one.
    rendered = [_to_bytes(render_frame(panorama, lens, pose)) for pose in poses]

    # Written into a staging directory first, and moved into place only once every byte is on
    # disk. The previous version swept the old frames *before* the write loop, so all-or-nothing
    # covered rendering and stopped there: a failure while writing left the earlier dataset deleted
    # and this one half-present — a `truth.json` describing frames that are gone, beside files from
    # neither run. That is the failure the paragraph below is about, reintroduced by the commit
    # that wrote the paragraph.
    out.parent.mkdir(parents=True, exist_ok=True)
    staging = out.parent / f".{out.name}.partial"
    if staging.exists():
        shutil.rmtree(staging)
    staging.mkdir(parents=True)

    frames = []
    try:
        for index, (pose, frame) in enumerate(zip(poses, rendered)):
            name = f"frame_{index:04d}.ppm"
            with (staging / name).open("wb") as handle:
                handle.write(b"P6\n%d %d\n255\n" % (lens.width, lens.height))
                handle.write(frame.tobytes())
            frames.append({
                "file": name,
                "rotation": {"w": pose.w, "x": pose.x, "y": pose.y, "z": pose.z},
            })

        truth = {
            # Prose, and nothing parses it — a reviewer is right that it cannot drift-check itself. It
            # is here because a consumer that reads these frames in another language needs the frame
            # conventions written down somewhere, and the equirectangular one below exists nowhere else:
            # the other three are mirrored from `types.h`, so a C++ reader already has them.
            "convention": {
                "camera_space": "-Z forward, +Y up, +X right",
                "image_space": "+x right, +y down, origin at the top-left corner",
                "principal_point": "this generator's lenses are built with cx = width / 2, half a pixel "
                                   "from OpenCV's (width - 1) / 2; read the value from intrinsics rather "
                                   "than assuming it",
                "equirectangular": "longitude 0 is -Z and increases toward +X; latitude +90 is +Y at "
                                   "row 0; an integer coordinate is a pixel edge, so forward lands on "
                                   "the corner at (width / 2, height / 2)",
                "rotation": "device -> world, unit quaternion, matching sphanorama::Quat",
                "pixel_encoding": "each byte b is a signed component: value = b / 255 * 2 - 1, so 0 is "
                                  "-1.0, 128 is +0.00392 and 255 is +1.0; there is no gamma and no "
                                  "colour space. A consumer that assumes unsigned [0, 1] reads every "
                                  "frame with its contrast halved and its zero in the wrong place",
            },
            "intrinsics": asdict(lens),
            "frames": frames,
        }
        (staging / "truth.json").write_text(json.dumps(truth, indent=2) + "\n")
    except BaseException:
        shutil.rmtree(staging, ignore_errors=True)
        raise

    # Everything is on disk. The swap is **two directory renames**, not a file-by-file move: the
    # old dataset steps aside whole and the new one takes its place whole, so a failure at any
    # single step leaves one or the other complete rather than a mixture of both.
    #
    # Moving the files one at a time was the previous attempt and it only moved the window: an
    # error on the second `replace` left the old dataset partly deleted, `out` holding one new
    # frame and no `truth.json`, and the staging directory leaked. A reviewer measured that, and
    # both properties meant to make it safe were mutation-green — which is what a window looks like
    # when it has been narrowed rather than closed.
    displaced = out.parent / f".{out.name}.replaced"
    shutil.rmtree(displaced, ignore_errors=True)
    try:
        if out.exists():
            out.replace(displaced)
        staging.replace(out)
    except BaseException:
        # Put back whatever was there and leave nothing else behind. Keeping the staged copy "for
        # inspection" was the first version of this and it is the wrong trade: it leaves a hidden
        # directory the caller did not ask for and cannot easily interpret, to save a render that
        # costs seconds at the size this tool is usable at.
        if displaced.exists() and not out.exists():
            displaced.replace(out)
        shutil.rmtree(staging, ignore_errors=True)
        raise
    shutil.rmtree(displaced, ignore_errors=True)
    return [out / f"frame_{index:04d}.ppm" for index in range(len(frames))]


def _checkerboard_panorama(width: int, height: int, squares: int = 64) -> np.ndarray:
    """A stand-in until real panoramas are wired in — enough texture for features to exist."""
    v, u = np.meshgrid(np.arange(height), np.arange(width), indexing="ij")
    cell = ((u * squares // width) + (v * squares // height)) % 2
    noise = np.sin(u * 0.11) * np.cos(v * 0.07)
    value = cell * 0.6 + noise * 0.4
    return np.stack([value, value * 0.8, value * 0.6], axis=-1)


def _ring_of_poses(count: int, elevation_deg: float = 0.0) -> list[Pose]:
    return [Pose.from_azimuth_elevation(360.0 * i / count, elevation_deg) for i in range(count)]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--out", type=Path, required=True, help="directory to write the dataset to")
    parser.add_argument("--frames", type=int, default=12, help="how many frames in the ring")
    parser.add_argument("--width", type=int, default=640)
    parser.add_argument("--height", type=int, default=480)
    parser.add_argument("--hfov", type=float, default=66.0)
    parser.add_argument("--vfov", type=float, default=50.0)
    args = parser.parse_args()

    # Checked before anything is rendered, because rendering is the expensive part and these are
    # the two ways to get it wrong that cost the whole of it. `--out` naming a file used to raise
    # `FileExistsError` *after* the full render — ten seconds at twelve frames and hours at the
    # scale the roadmap plans — and `--frames 0` deleted the dataset that was there, printed
    # "wrote 0 frames" and exited 0.
    if args.frames < 1:
        parser.error(f"--frames must be at least 1, not {args.frames}")
    for name, value in (("--width", args.width), ("--height", args.height)):
        if value < 1:
            parser.error(f"{name} must be at least 1, not {value}")
    for name, value in (("--hfov", args.hfov), ("--vfov", args.vfov)):
        if not 0.0 < value < 180.0:
            parser.error(f"{name} must be between 0 and 180 degrees, not {value}")

    # Every shape of unusable `--out`, not just the one the first version thought of. A reviewer
    # found three more, each of which spent the whole render and then raised the very error this
    # exists to pre-empt. `Path.exists()` follows symlinks, so it answers a question about the
    # *target* — False for a dangling one — which is not the question being asked.
    if args.out.is_symlink() and not args.out.exists():
        parser.error(f"--out is a symlink to nothing: {args.out}")
    if args.out.exists() and not args.out.is_dir():
        parser.error(f"--out must be a directory, and {args.out} is not one")
    if not args.out.parent.is_dir():
        parser.error(f"--out's parent must be an existing directory, and {args.out.parent} is not")
    staging = args.out.parent / f".{args.out.name}.partial"
    if staging.exists() and not staging.is_dir():
        parser.error(f"{staging} is in the way and is not a directory this can clear")

    panorama = _checkerboard_panorama(2048, 1024)
    lens = lens_from_fov(args.hfov, args.vfov, args.width, args.height)
    written = write_dataset(args.out, panorama, lens, _ring_of_poses(args.frames))
    print(f"wrote {len(written)} frames and truth.json to {args.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
