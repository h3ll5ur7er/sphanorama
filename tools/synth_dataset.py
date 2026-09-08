#!/usr/bin/env python3
"""Render the frames a phone would have captured from a panorama, with the truth of where it looked.

Phase 2 measures registration accuracy in degrees against known rotations, and this is what knows
them. It takes an equirectangular panorama and a list of camera orientations, and emits one image
per orientation plus a `truth.json` carrying the rotation and the lens that produced each.

**It re-implements the lens rather than calling the core, and that is the point.** `camera_model` in
`core/src/utilities/` does the same arithmetic, and rendering a dataset *through* it would mean any
error the two share cancels out — the harness would certify a broken projection as accurate, which
is the one failure a measuring instrument must not have. So this is a second, independent
implementation, and both are pinned to the same hand-worked decimals (see
`test_distortion_terms_are_opencvs_in_opencvs_order`, and its twin in the C++ suite). Two
implementations that agree with an outside number are evidence; one implementation checked against
itself is not.

Conventions, matching `contracts/cpp/sphanorama/types.h` and ADR 0046 exactly, because a dataset
expressed in a different frame from the core that reads it is worse than no dataset:

- **Camera space**: -Z forward, +Y up, +X right.
- **Image space**: +x right, +y down, origin at the top-left corner, `cx = width / 2`. That last is
  half a pixel away from OpenCV's `(width - 1) / 2`, and a residual is measured in pixels.
- **World space**: the same axes; a `Pose` is device -> world, as `Quat` is.
- **Equirectangular**: longitude 0 is -Z (forward) and increases toward +X; latitude +90 is +Y and
  sits at row 0. So the panorama's centre pixel is the direction a camera at identity looks along.

Run it: `uv run --group datasets tools/synth_dataset.py --out datasets/ring` (the `datasets` group
is what carries numpy; the checkers stay standard-library only — ADR 0048, ADR 0050).
"""
from __future__ import annotations

import argparse
import json
import math
from dataclasses import asdict, dataclass, field
from pathlib import Path

import numpy as np

# Newton on the distortion, matching the core's solver in shape if not in code. The core measured
# its own budget; this one is generous because it runs offline on whole images at a time and an
# extra pass costs nothing a person would notice.
INVERSE_ITERATIONS = 30
INVERSE_TOLERANCE = 1e-12


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

    def rotate(self, vectors: np.ndarray) -> np.ndarray:
        """Rotate an (N, 3) array of vectors from camera space into world space."""
        u = np.array([self.x, self.y, self.z], dtype=float)
        cross = np.cross(u, vectors)
        return vectors + 2.0 * self.w * cross + 2.0 * np.cross(u, cross)


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


def _radial(lens: Intrinsics, r2: np.ndarray) -> np.ndarray:
    return 1.0 + r2 * (lens.k1 + r2 * (lens.k2 + r2 * lens.k3))


def _distort(lens: Intrinsics, xn: np.ndarray, yn: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    """Brown-Conrady forward, in the same term order as `DistortAt`."""
    r2 = xn * xn + yn * yn
    radial = _radial(lens, r2)
    xd = xn * radial + 2.0 * lens.p1 * xn * yn + lens.p2 * (r2 + 2.0 * xn * xn)
    yd = yn * radial + lens.p1 * (r2 + 2.0 * yn * yn) + 2.0 * lens.p2 * xn * yn
    return xd, yd


def project(lens: Intrinsics, camera_space: np.ndarray) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    """Camera-space directions to pixels. Returns (u, v, valid), one entry per input row."""
    camera_space = np.asarray(camera_space, dtype=float)
    depth = -camera_space[:, 2]

    # `> 0` rather than `<= 0`, so a NaN refuses rather than passes — the same shape as the core's
    # guard, and for the same reason.
    valid = depth > 0.0
    safe_depth = np.where(valid, depth, 1.0)
    xn = camera_space[:, 0] / safe_depth
    yn = -camera_space[:, 1] / safe_depth      # the world's up is the image's down
    valid &= np.isfinite(xn) & np.isfinite(yn)

    xd, yd = _distort(lens, xn, yn)
    u = lens.fx * xd + lens.cx
    v = lens.fy * yd + lens.cy
    valid &= np.isfinite(u) & np.isfinite(v)
    return u, v, valid


def unproject(lens: Intrinsics, pixels: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    """Pixels to unit camera-space directions, by Newton on the distortion.

    Returns (directions, valid). A pixel whose distortion does not invert is refused rather than
    answered with the nearest thing that iterated, which is the rule ADR 0046 sets for the core.
    """
    pixels = np.asarray(pixels, dtype=float)
    xd = (pixels[:, 0] - lens.cx) / lens.fx
    yd = (pixels[:, 1] - lens.cy) / lens.fy

    xn = xd.copy()
    yn = yd.copy()
    for _ in range(INVERSE_ITERATIONS):
        r2 = xn * xn + yn * yn
        radial = _radial(lens, r2)
        d_radial = lens.k1 + r2 * (2.0 * lens.k2 + r2 * 3.0 * lens.k3)

        fx_ = xn * radial + 2.0 * lens.p1 * xn * yn + lens.p2 * (r2 + 2.0 * xn * xn) - xd
        fy_ = yn * radial + lens.p1 * (r2 + 2.0 * yn * yn) + 2.0 * lens.p2 * xn * yn - yd

        dxdx = radial + 2.0 * xn * xn * d_radial + 2.0 * lens.p1 * yn + 6.0 * lens.p2 * xn
        dydy = radial + 2.0 * yn * yn * d_radial + 6.0 * lens.p1 * yn + 2.0 * lens.p2 * xn
        cross = 2.0 * xn * yn * d_radial + 2.0 * lens.p1 * xn + 2.0 * lens.p2 * yn
        determinant = dxdx * dydy - cross * cross

        usable = np.abs(determinant) > 1e-15
        safe = np.where(usable, determinant, 1.0)
        step_x = np.where(usable, -(dydy * fx_ - cross * fy_) / safe, 0.0)
        step_y = np.where(usable, -(dxdx * fy_ - cross * fx_) / safe, 0.0)
        xn = xn + step_x
        yn = yn + step_y
        if np.max(np.abs(step_x)) < INVERSE_TOLERANCE and np.max(np.abs(step_y)) < INVERSE_TOLERANCE:
            break

    directions = np.stack([xn, -yn, -np.ones_like(xn)], axis=-1)
    directions /= np.linalg.norm(directions, axis=-1, keepdims=True)

    # The answer has to survive being projected again, which is what turns a solver that wandered
    # into a refusal rather than a plausible-looking direction.
    back_u, back_v, back_valid = project(lens, directions)
    landed = (np.abs(back_u - pixels[:, 0]) < 1e-6) & (np.abs(back_v - pixels[:, 1]) < 1e-6)
    return directions, back_valid & landed & np.isfinite(xn) & np.isfinite(yn)


def direction_to_equirect(directions: np.ndarray, width: int,
                          height: int) -> tuple[np.ndarray, np.ndarray]:
    """World directions to floating-point panorama coordinates.

    Longitude 0 is -Z and increases toward +X; latitude +90 is +Y at row 0. Returns coordinates in
    pixels, where an integer value is a pixel *edge* — so the centre of pixel (0, 0) is (0.5, 0.5).
    """
    directions = np.asarray(directions, dtype=float)
    norms = np.linalg.norm(directions, axis=-1, keepdims=True)
    unit = directions / np.where(norms > 0.0, norms, 1.0)

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
    y = np.clip(v - 0.5, 0.0, height - 1.0)

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

    A pixel whose direction the lens cannot produce is left black rather than guessed at — with a
    distortion strong enough to fold, part of the frame genuinely has no ray behind it.
    """
    us, vs = np.meshgrid(np.arange(lens.width) + 0.5, np.arange(lens.height) + 0.5, indexing="xy")
    pixels = np.stack([us.ravel(), vs.ravel()], axis=-1)

    camera_directions, valid = unproject(lens, pixels)
    world = pose.rotate(camera_directions)
    u, v = direction_to_equirect(world, panorama.shape[1], panorama.shape[0])
    colours = sample_equirect(panorama, u, v)
    colours[~valid] = 0.0
    return colours.reshape(lens.height, lens.width, panorama.shape[2])


def _to_bytes(frame: np.ndarray) -> np.ndarray:
    """Map a signed unit-range frame onto bytes. Shared with the test that reads a file back."""
    return np.round(np.clip((frame + 1.0) * 0.5, 0.0, 1.0) * 255.0).astype(np.uint8)


def write_dataset(out: Path, panorama: np.ndarray, lens: Intrinsics,
                  poses: list[Pose]) -> list[Path]:
    """Render every pose and write the frames beside the truth that describes them.

    Binary Netpbm rather than PNG, deliberately: a P6 file is a header and the pixels, which any
    consumer can read in a dozen lines and no consumer needs a library for. Datasets are
    regenerated rather than committed, so the size is a cost nobody carries for long.
    """
    out.mkdir(parents=True, exist_ok=True)
    written: list[Path] = []
    frames = []

    for index, pose in enumerate(poses):
        frame = _to_bytes(render_frame(panorama, lens, pose))
        name = f"frame_{index:04d}.ppm"
        path = out / name
        with path.open("wb") as handle:
            handle.write(b"P6\n%d %d\n255\n" % (lens.width, lens.height))
            handle.write(frame.tobytes())
        written.append(path)
        frames.append({
            "file": name,
            "rotation": {"w": pose.w, "x": pose.x, "y": pose.y, "z": pose.z},
        })

    truth = {
        "convention": {
            "camera_space": "-Z forward, +Y up, +X right",
            "image_space": "+x right, +y down, origin at the top-left corner, cx = width / 2",
            "rotation": "device -> world, unit quaternion, matching sphanorama::Quat",
        },
        "intrinsics": asdict(lens),
        "frames": frames,
    }
    (out / "truth.json").write_text(json.dumps(truth, indent=2) + "\n")
    return written


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

    panorama = _checkerboard_panorama(2048, 1024)
    lens = lens_from_fov(args.hfov, args.vfov, args.width, args.height)
    written = write_dataset(args.out, panorama, lens, _ring_of_poses(args.frames))
    print(f"wrote {len(written)} frames and truth.json to {args.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
