#!/usr/bin/env python3
"""Tests for the synthetic dataset renderer.

The renderer's job is to produce frames a phone *would* have captured, together with the rotation
each was captured at. Everything downstream in Phase 2 is measured against those rotations, so a
renderer that is subtly wrong does not produce a visible defect — it produces a harness that
certifies the wrong answer.

Two of the cases below carry most of the weight.

`test_distortion_terms_are_opencvs_in_opencvs_order` pins this implementation to the same
hand-computed decimals as `Project.TheDistortionTermsAreOpenCVsInOpenCVsOrder` in the C++ suite.
That matters more here than an ordinary agreement test would: this module deliberately implements
the lens itself rather than calling the core, because a dataset rendered *through* the code under
test would hide any error the two share.

They are **not** independent re-derivations, and a round-1 reviewer was right to press the claim
that they were: the arithmetic here is a close port of the core's. What carries the weight is that
both meet the same hand-worked decimals, taken from the published Brown-Conrady form and derived
from neither implementation. ADR 0050 records the withdrawal; this docstring stated the withdrawn
version for a further round, because the correction was made in "the module docstring" and there
are two.

`test_every_rendered_pixel_lands_where_its_own_ray_points` renders from a panorama whose colour
encodes the direction it represents, so each rendered pixel carries its own ground truth and the
whole path — unprojection, rotation, equirectangular lookup, sampling — is checkable arithmetic
rather than something to look at.
"""
from __future__ import annotations

import hashlib
import json
import math
import sys
import tempfile
import unittest
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import synth_dataset  # noqa: E402
from synth_dataset import (  # noqa: E402
    _ring_of_poses,
    _distort,
    distort_at,
    INVERSE_ACCEPTANCE_NORMALISED,
    is_usable_lens,
    Intrinsics,
    _to_bytes,
    Pose,
    defined_at,
    direction_to_equirect,
    inverts_along_the_ray,
    radial_map_increases_up_to,
    lens_from_fov,
    project,
    render_frame,
    sample_equirect,
    unproject,
    write_dataset,
)


def phone_lens() -> Intrinsics:
    return lens_from_fov(66.0, 50.0, 320, 240)


def direction_encoded_panorama(width: int = 512, height: int = 256) -> np.ndarray:
    """A panorama whose pixel value *is* the direction that pixel represents.

    Each channel holds one component of the unit direction, in [-1, 1] and **not** remapped — an
    earlier version of this docstring described a [0, 1] encoding that the code never performed, and
    a `decode` helper that was the identity function, which together implied a round trip nobody
    was doing. Read a rendered pixel and you have the world direction the camera was looking along
    when it drew it, to within the sampling error, with no step in between. That is what turns
    "does this render correctly" into arithmetic.

    Only `write_dataset` maps to bytes, and only on the way to disk, where [-1, 1] becomes 0..255.
    """
    v, u = np.meshgrid(np.arange(height), np.arange(width), indexing="ij")
    longitude = (u + 0.5) / width * 2.0 * math.pi - math.pi
    latitude = math.pi / 2.0 - (v + 0.5) / height * math.pi
    x = np.cos(latitude) * np.sin(longitude)
    y = np.sin(latitude)
    z = -np.cos(latitude) * np.cos(longitude)
    return np.stack([x, y, z], axis=-1)


def worst_angle_deg(a: np.ndarray, b: np.ndarray) -> float:
    """The largest angle between corresponding directions, in degrees.

    The unit the dataset is built to serve, so the assertions are written in it rather than in
    colour components whose relationship to an angle a reader has to work out.
    """
    a = a / np.linalg.norm(a, axis=-1, keepdims=True)
    b = b / np.linalg.norm(b, axis=-1, keepdims=True)
    return float(np.degrees(np.max(np.arccos(np.clip(np.sum(a * b, axis=-1), -1.0, 1.0)))))


class LensFromFieldOfView(unittest.TestCase):
    def test_the_focal_lengths_are_the_half_angle_tangents(self):
        lens = lens_from_fov(90.0, 60.0, 800, 400)
        self.assertAlmostEqual(lens.fx, 400.0 / math.tan(math.radians(45.0)), places=9)
        self.assertAlmostEqual(lens.fy, 200.0 / math.tan(math.radians(30.0)), places=9)
        self.assertAlmostEqual(lens.cx, 400.0)
        self.assertAlmostEqual(lens.cy, 200.0)

    def test_the_principal_point_is_the_centre_not_the_last_pixel(self):
        # width/2, matching the C++ and *not* OpenCV's (width-1)/2. Half a pixel, and it matters
        # for a residual measured in pixels.
        lens = lens_from_fov(66.0, 50.0, 320, 240)
        self.assertEqual(lens.cx, 160.0)
        self.assertEqual(lens.cy, 120.0)

    def test_a_lens_wider_than_a_rectilinear_one_can_be_is_refused(self):
        for horizontal, vertical in ((180.0, 50.0), (50.0, 180.0), (200.0, 50.0), (0.0, 50.0)):
            with self.assertRaises(ValueError):
                lens_from_fov(horizontal, vertical, 320, 240)


class Projection(unittest.TestCase):
    DISTORTION_TABLE = (
        # (k1, k2, k3, p1, p2, xn, yn, xd, yd, dxdx, dydy, cross) -- computed from the published
        # Brown-Conrady form and its derivative in
        # exact decimal arithmetic, calling neither implementation. The C++ suite's
        # `Project.MeetsTheDatasetGeneratorAcrossLensFamilies` asserts this same table.
        # no distortion
        (0.0, 0.0, 0.0, 0.0, 0.0, 0.3, 0.2, 0.3000000000, 0.2000000000, 1.0000000000, 1.0000000000, 0.0000000000),
        (0.0, 0.0, 0.0, 0.0, 0.0, -0.45, 0.12, -0.4500000000, 0.1200000000, 1.0000000000, 1.0000000000, 0.0000000000),
        (0.0, 0.0, 0.0, 0.0, 0.0, 0.05, -0.55, 0.0500000000, -0.5500000000, 1.0000000000, 1.0000000000, 0.0000000000),
        # typical phone
        (-0.28, 0.09, 0.0, 0.002, -0.003, 0.3, 0.2, 0.2888463000, 0.1930842000, 0.9143330000, 0.9451930000, -0.0307920000),
        (-0.28, 0.09, 0.0, 0.002, -0.003, -0.45, 0.12, -0.4266576472, 0.1140356526, 0.8544941149, 0.9407025145, 0.0235034640),
        (-0.28, 0.09, 0.0, 0.002, -0.003, 0.05, -0.55, 0.0451086125, -0.5056497375, 0.9187467500, 0.7798867500, 0.0158805000),
        # strong barrel
        (-0.5, 0.25, -0.05, 0.0, 0.0, 0.3, 0.2, 0.2817345450, 0.1878230300, 0.8603588500, 0.9041123500, -0.0525042000),
        (-0.5, 0.25, -0.05, 0.0, 0.0, -0.45, 0.12, -0.4062605368, 0.1083361431, 0.7413654221, 0.8913213158, 0.0430495389),
        (-0.5, 0.25, -0.05, 0.0, 0.0, 0.05, -0.55, 0.0434668809, -0.4781356903, 0.8675303500, 0.6506581000, 0.0198799562),
        # pincushion
        (0.3, 0.1, 0.01, -0.001, 0.002, 0.3, 0.2, 0.3127135910, 0.2081723940, 1.1026832300, 1.0668325300, 0.0393808400),
        (0.3, 0.1, 0.01, -0.001, 0.002, -0.45, 0.12, -0.4800926713, 0.1279234924, 1.2038771071, 1.0772865943, -0.0358574678),
        (0.3, 0.1, 0.01, -0.001, 0.002, 0.05, -0.55, 0.0557293113, -0.6066174244, 1.1046051800, 1.3246796300, -0.0223084912),
        # tangential heavy
        (-0.1, 0.0, 0.0, 0.05, 0.07, 0.3, 0.2, 0.3238000000, 0.2163000000, 1.1150000000, 1.0810000000, 0.0460000000),
        (-0.1, 0.0, 0.0, 0.05, 0.07, -0.45, 0.12, -0.4021065000, 0.1221222000, 0.7608100000, 0.9484300000, -0.0174000000),
        (-0.1, 0.0, 0.0, 0.05, 0.07, 0.05, -0.55, 0.0674250000, -0.4915750000, 0.9350000000, 0.7510000000, -0.0665000000),
    )

    def test_the_distortion_matches_the_core_across_lens_families(self):
        """Fifteen points, five lens families -- the widening ADR 0050 said was owed.

        The single pinned point below is real and narrow: one lens, one quadrant, both `xn` and `yn`
        positive. It catches a swapped `k2`/`k3` and a swapped `p1`/`p2`, and a round-2 reviewer
        showed it catches a flipped `yn` too. What it cannot speak to is whether the two
        implementations agree *away* from that point -- across sign quadrants, at radii where `k3`
        starts to matter, and on a lens where the tangential terms dominate.

        This is the coverage half. Both suites assert the same table, so agreement is with the
        published form rather than with each other -- which is the whole of what ADR 0050 says
        carries the weight, now that the independence claim has been withdrawn.
        """
        base = lens_from_fov(66.0, 50.0, 64, 48).__dict__
        for k1, k2, k3, p1, p2, xn, yn, xd, yd, dxdx, dydy, cross in self.DISTORTION_TABLE:
            coefficients = {"k1": k1, "k2": k2, "k3": k3, "p1": p1, "p2": p2}
            lens = Intrinsics(**{**base, **coefficients})
            got_x, got_y = _distort(lens, np.array([xn]), np.array([yn]))
            where = f"{coefficients} at ({xn}, {yn})"
            self.assertAlmostEqual(float(got_x[0]), xd, places=9, msg="xd for " + where)
            self.assertAlmostEqual(float(got_y[0]), yd, places=9, msg="yd for " + where)

            # The Jacobian too. A reviewer measured that the table read 2 of the 7 values
            # `distort_at` returns, and the other five are what decide where the fold is -- the
            # exact quantity this module's docstring recounts getting wrong. Four mutations to
            # those five were invisible to the whole suite; two were invisible to the C++ suite as
            # well. Three more columns on a table that already existed closes it.
            at = distort_at(lens, np.array([xn]), np.array([yn]))
            self.assertAlmostEqual(float(at.dxdx[0]), dxdx, places=9, msg="dxdx for " + where)
            self.assertAlmostEqual(float(at.dydy[0]), dydy, places=9, msg="dydy for " + where)
            self.assertAlmostEqual(float(at.cross[0]), cross, places=9, msg="cross for " + where)

    def test_distortion_terms_are_opencvs_in_opencvs_order(self):
        # The same point and the same hand-worked decimals as the C++ suite's
        # Project.TheDistortionTermsAreOpenCVsInOpenCVsOrder. At xn = 0.3, yn = 0.2 with
        # k1..k3 = 0.1, 0.02, 0.003 and p1, p2 = 0.05, 0.07. A k2/k3 swap moves xd by 7.5e-05 and a
        # p1/p2 swap by 3.8e-03, against the 1e-9 asserted here.
        lens = phone_lens()
        lens = Intrinsics(**{**lens.__dict__, "k1": 0.1, "k2": 0.02, "k3": 0.003,
                             "p1": 0.05, "p2": 0.07})

        # yn is the negated camera-space y, so this direction has xn = 0.3 and yn = 0.2 exactly.
        u, v, valid = project(lens, np.array([[0.3, -0.2, -1.0]]))

        self.assertTrue(bool(valid[0]))
        self.assertAlmostEqual((u[0] - lens.cx) / lens.fx, 0.3317033773, places=9)
        self.assertAlmostEqual((v[0] - lens.cy) / lens.fy, 0.2215689182, places=9)

    def test_the_optical_axis_lands_on_the_principal_point(self):
        lens = phone_lens()
        u, v, valid = project(lens, np.array([[0.0, 0.0, -1.0]]))
        self.assertTrue(bool(valid[0]))
        self.assertAlmostEqual(u[0], lens.cx, places=9)
        self.assertAlmostEqual(v[0], lens.cy, places=9)

    def test_the_worlds_up_is_the_images_down(self):
        lens = phone_lens()
        u, v, _ = project(lens, np.array([[0.0, 0.3, -1.0]]))
        self.assertLess(v[0], lens.cy)
        u_right, _, _ = project(lens, np.array([[0.3, 0.0, -1.0]]))
        self.assertGreater(u_right[0], lens.cx)

    def test_a_direction_at_or_behind_the_optical_plane_has_no_image(self):
        lens = phone_lens()
        _, _, valid = project(lens, np.array([[0.0, 0.0, 0.0],
                                              [0.0, 0.0, 1.0],
                                              [1.0, 0.0, 0.0]]))
        self.assertFalse(valid.any())

    def test_projection_and_unprojection_are_inverse(self):
        lens = phone_lens()
        lens = Intrinsics(**{**lens.__dict__, "k1": -0.28, "k2": 0.09, "p1": 0.001, "p2": -0.002})
        us, vs = np.meshgrid(np.linspace(5, 315, 24), np.linspace(5, 235, 18), indexing="ij")
        pixels = np.stack([us.ravel(), vs.ravel()], axis=-1)

        directions, valid = unproject(lens, pixels)
        self.assertTrue(valid.all(), "an in-frame pixel had no direction")

        back_u, back_v, back_valid = project(lens, directions)
        self.assertTrue(back_valid.all())
        np.testing.assert_allclose(back_u, pixels[:, 0], atol=1e-6)
        np.testing.assert_allclose(back_v, pixels[:, 1], atol=1e-6)

    def test_unprojected_directions_are_unit(self):
        lens = phone_lens()
        pixels = np.array([[0.0, 0.0], [319.0, 239.0], [160.0, 120.0], [12.0, 200.0]])
        directions, valid = unproject(lens, pixels)
        self.assertTrue(valid.all())
        np.testing.assert_allclose(np.linalg.norm(directions, axis=-1), 1.0, atol=1e-12)


class ALensHasToBeALens(unittest.TestCase):
    """A default `Intrinsics` is not a camera, and nothing here noticed.

    With `fx = fy = 0` the whole world maps to pixel (0, 0) and every direction reports valid,
    because a constant map is its own inverse everywhere — the round trip agrees with itself
    perfectly. The arithmetic is all finite, so no guard fired.
    """

    def test_a_default_intrinsics_is_refused_by_both_directions(self):
        with self.assertRaises(ValueError):
            project(Intrinsics(), np.array([[0.0, 0.0, -1.0]]))
        with self.assertRaises(ValueError):
            unproject(Intrinsics(), np.array([[0.0, 0.0]]))

    def test_a_lens_with_no_focal_length_or_no_size_is_refused(self):
        base = lens_from_fov(66.0, 50.0, 320, 240)
        for broken in ({"fx": 0.0}, {"fy": -1.0}, {"width": 0}, {"height": -4},
                       {"fx": float("nan")}, {"k1": float("inf")}):
            lens = Intrinsics(**{**base.__dict__, **broken})
            with self.assertRaises(ValueError, msg=str(broken)):
                project(lens, np.array([[0.0, 0.0, -1.0]]))

    def test_a_direction_that_is_not_a_measurement_has_no_pixel(self):
        lens = lens_from_fov(66.0, 50.0, 320, 240)
        # An infinite depth is greater than zero and divides to the principal point, which would
        # have answered a direction nobody measured with the middle of the frame.
        _, _, valid = project(lens, np.array([[0.0, 0.0, -float("inf")],
                                              [float("nan"), 0.0, -1.0],
                                              [0.0, float("inf"), -1.0]]))
        self.assertFalse(valid.any())

    def test_a_zero_vector_names_no_direction(self):
        with self.assertRaises(ValueError):
            direction_to_equirect(np.array([[0.0, 0.0, 0.0]]), 512, 256)
        with self.assertRaises(ValueError):
            direction_to_equirect(np.array([[0.0, float("nan"), -1.0]]), 512, 256)


class PoseArithmetic(unittest.TestCase):
    def test_a_quaternion_off_unit_still_names_its_own_rotation(self):
        """`sphanorama::Rotate` normalises and this did not.

        One percent off unit turned a direction 0.69 degrees while `truth.json` recorded the
        rotation that was *asked* for — so the frames and the ground truth would have disagreed by
        more than the quantity the whole harness exists to measure.
        """
        exact = Pose.from_axis_angle((0.3, 0.8, -0.5), 0.9)
        scaled = Pose(exact.w * 1.01, exact.x * 1.01, exact.y * 1.01, exact.z * 1.01)
        vectors = np.array([[0.0, 0.0, -1.0], [1.0, 0.0, 0.0], [0.2, -0.7, -0.4]])

        np.testing.assert_allclose(scaled.rotate(vectors), exact.rotate(vectors), atol=1e-12)

    def test_rotation_preserves_length(self):
        pose = Pose.from_axis_angle((0.1, -0.9, 0.4), 2.1)
        vectors = np.array([[0.0, 0.0, -1.0], [3.0, 0.0, 0.0]])
        np.testing.assert_allclose(np.linalg.norm(pose.rotate(vectors), axis=-1),
                                   np.linalg.norm(vectors, axis=-1), atol=1e-12)


class RotationIsAnchoredToNumbersNobodyComputed(unittest.TestCase):
    """`Pose.rotate` could be replaced by `return vectors` and every test still passed.

    That is the worst thing that was wrong with this file, and the cause is structural rather than
    careless: every other test worked out its expected value by *calling* `rotate`, so the identity
    satisfied all of them. What it produced is precisely the failure a harness cannot have —
    `truth.json` recording three distinct rotations beside three byte-identical frames, so a
    registration that recovered them perfectly would have been scored 40 and 80 degrees wrong.

    Every other stage had an outside anchor: `project` against OpenCV's hand-worked decimals,
    `unproject` against the round trip, `direction_to_equirect` against hand-written coordinates,
    `sample_equirect` against explicit blends. Rotation had none. These are the numbers it was
    missing, worked out from the right-hand rule and written down rather than computed.
    """

    def test_a_quarter_turn_about_each_axis_sends_forward_where_it_should(self):
        quarter = math.pi / 2.0
        forward = np.array([[0.0, 0.0, -1.0]])

        # About +Y, forward swings to the left of the world frame, which is -X.
        np.testing.assert_allclose(
            Pose.from_axis_angle((0.0, 1.0, 0.0), quarter).rotate(forward),
            [[-1.0, 0.0, 0.0]], atol=1e-12)
        # About +X, forward lifts to +Y.
        np.testing.assert_allclose(
            Pose.from_axis_angle((1.0, 0.0, 0.0), quarter).rotate(forward),
            [[0.0, 1.0, 0.0]], atol=1e-12)
        # About +Z, forward is on the axis and does not move.
        np.testing.assert_allclose(
            Pose.from_axis_angle((0.0, 0.0, 1.0), quarter).rotate(forward),
            [[0.0, 0.0, -1.0]], atol=1e-12)

    def test_a_quarter_turn_about_each_axis_sends_right_where_it_should(self):
        quarter = math.pi / 2.0
        right = np.array([[1.0, 0.0, 0.0]])

        np.testing.assert_allclose(
            Pose.from_axis_angle((0.0, 1.0, 0.0), quarter).rotate(right),
            [[0.0, 0.0, -1.0]], atol=1e-12)      # right swings to forward
        np.testing.assert_allclose(
            Pose.from_axis_angle((0.0, 0.0, 1.0), quarter).rotate(right),
            [[0.0, 1.0, 0.0]], atol=1e-12)       # right lifts to up
        np.testing.assert_allclose(
            Pose.from_axis_angle((1.0, 0.0, 0.0), quarter).rotate(right),
            [[1.0, 0.0, 0.0]], atol=1e-12)       # right is on the axis

    def test_a_half_turn_reverses_the_two_axes_it_is_not_about(self):
        half = Pose.from_axis_angle((0.0, 1.0, 0.0), math.pi)
        np.testing.assert_allclose(
            half.rotate(np.array([[0.0, 0.0, -1.0], [1.0, 0.0, 0.0], [0.0, 1.0, 0.0]])),
            [[0.0, 0.0, 1.0], [-1.0, 0.0, 0.0], [0.0, 1.0, 0.0]], atol=1e-12)

    def test_the_identity_is_the_only_rotation_that_moves_nothing(self):
        vectors = np.array([[0.0, 0.0, -1.0], [0.6, -0.3, 0.2]])
        np.testing.assert_allclose(Pose.identity().rotate(vectors), vectors, atol=1e-15)
        for axis in ((1.0, 0.0, 0.0), (0.0, 1.0, 0.0), (0.0, 0.0, 1.0)):
            moved = Pose.from_axis_angle(axis, 0.4).rotate(vectors)
            self.assertGreater(float(np.max(np.abs(moved - vectors))), 0.05,
                               "a rotation that changes nothing is the defect this class exists for")

    def test_composition_applies_the_second_rotation_in_the_first_ones_frame(self):
        # Both axes have every component non-zero, and that is the point rather than decoration.
        # The first version used a yaw and a pitch — one about +Y, one about +X — so a mutation of
        # `then` that corrupted a term involving `self.x` multiplied by zero and changed nothing.
        a = Pose.from_axis_angle((0.3, 0.8, -0.5), 0.9)
        b = Pose.from_axis_angle((-0.6, 0.2, 0.7), 1.3)
        vectors = np.array([[0.0, 0.0, -1.0], [0.3, 0.5, -0.8], [1.0, 0.0, 0.0]])
        np.testing.assert_allclose(a.then(b).rotate(vectors), a.rotate(b.rotate(vectors)),
                                   atol=1e-12)
        # And the two orders genuinely differ, so the assertion above is not vacuous.
        self.assertGreater(float(np.max(np.abs(a.then(b).rotate(vectors)
                                               - b.then(a).rotate(vectors)))), 0.1)

    def test_an_azimuth_turns_about_up_and_an_elevation_lifts_toward_it(self):
        # The convention `FromAzimuthElevation` states, checked against directions written down
        # rather than computed: a quarter turn of azimuth puts the camera's forward axis at -X.
        np.testing.assert_allclose(
            Pose.from_azimuth_elevation(90.0, 0.0).rotate(np.array([[0.0, 0.0, -1.0]])),
            [[-1.0, 0.0, 0.0]], atol=1e-12)
        np.testing.assert_allclose(
            Pose.from_azimuth_elevation(0.0, 90.0).rotate(np.array([[0.0, 0.0, -1.0]])),
            [[0.0, 1.0, 0.0]], atol=1e-12)

        # Both at once, which is the only case that can see the composition order. With azimuth 0
        # or elevation 0 the two orderings agree, so a `then` reversed between them survives every
        # pure-axis case. Worked out by hand: elevation 45 lifts forward to (0, sin45, -cos45), and
        # a 90-degree azimuth then swings that to -X, giving up-and-left.
        root_half = math.sqrt(0.5)
        np.testing.assert_allclose(
            Pose.from_azimuth_elevation(90.0, 45.0).rotate(np.array([[0.0, 0.0, -1.0]])),
            [[-root_half, root_half, 0.0]], atol=1e-12)


class TheRotationConventionMeetsTheCore(unittest.TestCase):
    """The twin of `FromAzimuthElevation.MeetsTheDatasetGeneratorAtNumbersNeitherDerived`.

    This module's docstring promises that a pose written here names the same direction the coverage
    planner would. Until now nothing executable checked it: each side was pinned to hand-derived
    vectors *separately*, which catches a mistake in one implementation but not a convention both
    share. And a shared convention error is the one that matters most here — a dataset rendered in
    the wrong rotation convention registers wrong in exactly the compensating way and scores
    perfect, which is the failure ADR 0050 exists to prevent.

    So both suites now assert these same decimals, worked out from the right-hand rule and derived
    from neither implementation. `from_azimuth_elevation(az, el)` is a yaw about +Y followed by a
    pitch about +X in the yawed frame, so applied to forward (0, 0, -1):

        pitch about +X:     (0, +sin el, -cos el)
        then yaw about +Y:  (-cos el * sin az, sin el, -cos el * cos az)

    and applied to the camera's +X axis, which the pitch leaves alone: (cos az, 0, -sin az).

    The second row is what makes this more than a restatement of the first: a convention with the
    forward axis right and the roll wrong passes on forward alone. Together they also separate a
    reversed composition order, which at (37, -12) differs by 0.0419 in its largest component —
    seven orders over the tolerance here.
    """

    CASES = (
        (37.0, -12.0,
         (-0.5886639210, -0.2079116908, -0.7811834080),
         (0.7986355100, 0.0000000000, -0.6018150232)),
        (120.0, 40.0,
         (-0.6634139482, 0.6427876097, 0.3830222216),
         (-0.5000000000, 0.0000000000, -0.8660254038)),
        (250.0, -63.0,
         (0.4266115225, -0.8910065242, 0.1552738958),
         (-0.3420201433, 0.0000000000, 0.9396926208)),
    )

    def test_from_azimuth_elevation_meets_the_core_at_numbers_neither_derived(self):
        for azimuth, elevation, forward, right in self.CASES:
            pose = Pose.from_azimuth_elevation(azimuth, elevation)
            axes = pose.rotate(np.array([[0.0, 0.0, -1.0], [1.0, 0.0, 0.0]]))
            np.testing.assert_allclose(axes[0], forward, atol=1e-9,
                                       err_msg=f"forward at ({azimuth}, {elevation})")
            np.testing.assert_allclose(axes[1], right, atol=1e-9,
                                       err_msg=f"right at ({azimuth}, {elevation})")

    def test_the_ring_the_cli_writes_uses_that_convention(self):
        """`_ring_of_poses` is the only path a generated dataset's rotations travel.

        The first version of this test could not fail for what it claimed. It used the default
        elevation of zero — where the pitch factor is exactly the identity, so `_ring_of_poses` need
        not call `from_azimuth_elevation` at all — and every assertion was symmetric under azimuth
        negation, because `worst_angle_deg` is unsigned. A reviewer replaced the body with a bare
        `from_axis_angle` about +Y, dropping the elevation argument entirely, and all 80 tests
        stayed green.

        So: a non-zero elevation, and directions asserted against the convention's closed form with
        their signs, rather than separations asserted against a magnitude.
        """
        elevation = 20.0
        poses = _ring_of_poses(6, elevation)
        self.assertEqual(len(poses), 6)

        for index, pose in enumerate(poses):
            azimuth = 360.0 * index / 6
            a, e = math.radians(azimuth), math.radians(elevation)
            forward = (-math.cos(e) * math.sin(a), math.sin(e), -math.cos(e) * math.cos(a))
            # And the +X axis, which the pitch leaves alone. Asserting forward alone was the second
            # thing wrong with this test: a reviewer rolled every cell 17.2 degrees about its own
            # forward axis and all eighty stayed green. The class three tests up says in as many
            # words that a convention with forward right and roll wrong passes on forward alone —
            # so this was the stated principle being broken twelve lines under the statement.
            right = (math.cos(a), 0.0, -math.sin(a))
            got = pose.rotate(np.array([[0.0, 0.0, -1.0], [1.0, 0.0, 0.0]]))
            np.testing.assert_allclose(got[0], forward, atol=1e-12,
                                       err_msg=f"forward of cell {index} at azimuth {azimuth}")
            np.testing.assert_allclose(got[1], right, atol=1e-12,
                                       err_msg=f"+X of cell {index} at azimuth {azimuth}")

        # The elevation is honoured rather than ignored — which the zero-elevation ring could not
        # have shown, since there the pitch factor is the identity. That is asserted by the loop
        # above rather than here: `forward[1]` is `sin(e)` for every cell. A trailing block
        # re-asserting it read as a second guard and was a restatement, which a reviewer pointed
        # out no mutation could reach without failing the loop first.


class SeamSampling(unittest.TestCase):
    """The one column where wrapping and clamping differ, tested where the behaviour lives.

    A render that crosses the seam does *not* reliably reach it: the interpolation only differs on
    the single column whose right-hand neighbour must wrap to column zero, and a 64x48 frame lands
    on that exact column only by luck. Replacing the wrap with a clamp left every other test green
    including one written specifically to look through the seam — so this asserts on the sampler
    directly, which is the unit that actually has the behaviour. (Counts are deliberately not
    quoted: the suite grows, and a stale number reads as a contradiction of the same sabotage
    recorded elsewhere. The claim that carries the meaning is that only this class caught it.)
    """

    def test_the_column_past_the_last_one_is_the_first_one(self):
        # Two columns with nothing in common, so a blend between them is unmistakable.
        panorama = np.zeros((4, 8, 3))
        panorama[:, 0] = [1.0, 0.0, 0.0]     # first column, pure red
        panorama[:, 7] = [0.0, 0.0, 1.0]     # last column, pure blue

        # Halfway between the centre of the last column and the centre of the first, going the
        # short way round through the seam.
        sampled = sample_equirect(panorama, np.array([8.0]), np.array([2.0]))

        np.testing.assert_allclose(sampled[0], [0.5, 0.0, 0.5], atol=1e-12)

    def test_sampling_just_inside_each_edge_returns_that_edge(self):
        panorama = np.zeros((4, 8, 3))
        panorama[:, 0] = [1.0, 0.0, 0.0]
        panorama[:, 7] = [0.0, 0.0, 1.0]

        first = sample_equirect(panorama, np.array([0.5]), np.array([2.0]))
        last = sample_equirect(panorama, np.array([7.5]), np.array([2.0]))

        np.testing.assert_allclose(first[0], [1.0, 0.0, 0.0], atol=1e-12)
        np.testing.assert_allclose(last[0], [0.0, 0.0, 1.0], atol=1e-12)

    def test_a_row_past_the_pole_holds_the_edge_where_a_column_past_the_seam_wraps(self):
        # The poles are not periodic — a ray past the top edge has to hold the top row rather than
        # reappear at the bottom, which is the opposite rule from the one above.
        #
        # Renamed: this was `test_latitude_clamps_where_longitude_wraps`, and the clamp it named is
        # gone. It was bit-for-bit redundant with the row clip that follows it — measured difference
        # 0.0 across v in [-3, 9] — so what this actually pins, and always did, is the *clipping*,
        # which is the thing that produces the behaviour.
        panorama = np.zeros((4, 8, 3))
        panorama[0, :] = [0.0, 1.0, 0.0]
        panorama[3, :] = [1.0, 1.0, 0.0]

        above = sample_equirect(panorama, np.array([4.0]), np.array([-3.0]))
        below = sample_equirect(panorama, np.array([4.0]), np.array([9.0]))

        np.testing.assert_allclose(above[0], [0.0, 1.0, 0.0], atol=1e-12)
        np.testing.assert_allclose(below[0], [1.0, 1.0, 0.0], atol=1e-12)


class UnprojectionRefuses(unittest.TestCase):
    """A lens strong enough to fold has pixels with no ray behind them, and they are refused.

    Nothing exercised this: every other test uses a lens whose whole frame inverts, so removing the
    refusal entirely left every other test green. ADR 0046 makes refusal the rule for the core and
    a dataset renderer that quietly invented directions instead would put fabricated geometry into
    the ground truth everything downstream is measured against.
    """

    def test_nothing_answered_lies_past_the_fold(self):
        """The assertion this class was missing, and the reason it was missing is worth keeping.

        A round trip cannot catch a wrong answer here: past the fold `radial` goes negative, which
        flips the sign of `xd`, so the far-side point really does project back to the pixel asked
        about. ADR 0046 says a round-trip check is structurally blind to this and it was right —
        with only that check, **468 of these 3072 pixels came back with fabricated directions**,
        the frame corner among them, answered 52 degrees off axis pointing the opposite way to the
        one it should. The two tests that were here both derived their expectation from the same
        missing guard, so both passed.
        """
        lens = lens_from_fov(66.0, 50.0, 320, 240)
        lens = Intrinsics(**{**lens.__dict__, "k1": -0.9, "k2": 0.6, "k3": -0.4})

        us, vs = np.meshgrid(np.linspace(0.5, 319.5, 64), np.linspace(0.5, 239.5, 48),
                             indexing="ij")
        pixels = np.stack([us.ravel(), vs.ravel()], axis=-1)
        directions, valid = unproject(lens, pixels)

        self.assertFalse(valid.all(), "a lens this strong should fold somewhere in frame")
        self.assertTrue(valid.any(), "and it should still answer near the optical centre")

        # Every answer has to sit where the forward map is orientation-preserving. This is the
        # check, not the round trip below it.
        #
        # This assertion used to read `defined_at(...)` — which `unproject` had just filtered on, so
        # it asserted the filter against itself and `defined_at` could be replaced by `return True`
        # with the whole suite green. `TheAnswerIsTheNearBranch` is the test that actually pins this
        # behaviour, against an oracle built from the forward map alone; what is left here is the
        # weaker statement that the answers satisfy the *stronger* guard, which at least is not the
        # one the solver was filtered on.
        xn = directions[valid][:, 0] / -directions[valid][:, 2]
        yn = -directions[valid][:, 1] / -directions[valid][:, 2]
        self.assertTrue(inverts_along_the_ray(lens, xn, yn).all(),
                        "an answered pixel came from the far side of the fold")

        back_u, back_v, _ = project(lens, directions[valid])
        np.testing.assert_allclose(back_u, pixels[valid][:, 0], atol=1e-6)
        np.testing.assert_allclose(back_v, pixels[valid][:, 1], atol=1e-6)

    def test_a_lens_that_folds_in_frame_is_refused_rather_than_rendered_with_holes(self):
        panorama = direction_encoded_panorama(512, 256)
        lens = lens_from_fov(66.0, 50.0, 96, 72)
        lens = Intrinsics(**{**lens.__dict__, "k1": -0.9, "k2": 0.6, "k3": -0.4})

        with self.assertRaises(ValueError):
            render_frame(panorama, lens, Pose.identity())
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaises(ValueError):
                write_dataset(Path(directory), panorama, lens, [Pose.identity()])

    def test_a_fold_in_the_middle_of_the_interval_is_caught_by_the_interior_branch(self):
        """The endpoint alone is not enough, and this reaches the branch that says so.

        The slope `1 + 3k1*u + 5k2*u^2 + 7k3*u^3` is a cubic in `u = r^2`, so it can dip below zero
        partway out and come back positive by the endpoint. The previous version of this test used
        k1 = -6.0, k2 = 5.5 and never reached the interior check at all — that lens is caught at the
        endpoint by `radial` going negative, so the endpoint-only sabotage its docstring claimed to
        catch was green. A reviewer found that and found a witness the closed form needs; this uses
        one of the same shape.

        The arrangement is asserted, not assumed: the endpoint must look healthy, or the interior
        branch is not what is being tested.
        """
        lens = Intrinsics(**{**lens_from_fov(66.0, 50.0, 320, 240).__dict__,
                             "k1": -9.847065, "k2": 39.478991, "k3": 44.682183})

        endpoint = np.array([0.35])
        self.assertTrue(bool(radial_map_increases_up_to(lens, endpoint * 0.0).all()),
                        "the optical centre must be sound or nothing here means anything")

        # The endpoint of this interval looks fine on its own terms...
        far = np.array([0.20])
        slope_at_far = 1.0 + far * (3.0 * lens.k1 + far * (5.0 * lens.k2 + far * 7.0 * lens.k3))
        self.assertGreater(float(slope_at_far[0]), 0.0,
                           "the endpoint has to look healthy or this proves nothing")

        # ...and the interval is still refused, because the slope dips inside it.
        self.assertFalse(bool(radial_map_increases_up_to(lens, far).all()),
                         "an interior fold was certified as sound")

    def test_a_non_finite_discriminant_refuses_rather_than_taking_the_no_roots_exit(self):
        """`>= 0.0` on a NaN is false, which is the same branch as "there are no real roots".

        They are not the same thing. `b*b` and `4ac` can each overflow to infinity and `inf - inf`
        is NaN, and treating that as "never turns" skips both interior checks and re-admits exactly
        the folded state this guard exists to close. The core refuses by name here and this dropped
        that along with the rest of the guards.
        """
        lens = Intrinsics(**{**lens_from_fov(66.0, 50.0, 64, 48).__dict__,
                             "k1": 1e200, "k2": 1e200, "k3": 1e200})
        self.assertFalse(bool(radial_map_increases_up_to(lens, np.array([0.1])).any()))

    def test_the_lenses_a_phone_actually_has_are_not_refused(self):
        """The other half: the guard must not buy its correctness by refusing everything.

        The numbers this docstring used to quote — k1 = -0.28 leaving the slope at +0.65 — silently
        needed the k2 beside them, and every negative k1 in the list below is paired with a positive
        k2, which is what hid it. `RealisticLensesAreNotAllAnswerable` pins the correction: k1 alone
        does reach the fold inside a 66 degree frame. So this list is lenses that really are
        answerable throughout, and it is a statement about these lenses rather than about phones.
        """
        panorama = direction_encoded_panorama(256, 128)
        base = lens_from_fov(66.0, 50.0, 48, 36)
        for name, coefficients in (
            ("no distortion", {}),
            ("typical phone", {"k1": -0.28, "k2": 0.09}),
            ("strong barrel", {"k1": -0.5, "k2": 0.25}),
            ("pincushion", {"k1": 0.3, "k2": 0.1}),
            ("mild tangential", {"k1": -0.2, "p1": 0.002, "p2": -0.003}),
        ):
            lens = Intrinsics(**{**base.__dict__, **coefficients})
            us, vs = np.meshgrid(np.arange(lens.width) + 0.5, np.arange(lens.height) + 0.5,
                                 indexing="xy")
            pixels = np.stack([us.ravel(), vs.ravel()], axis=-1)
            _, valid = unproject(lens, pixels)
            self.assertTrue(valid.all(), f"{name}: a lens that does not fold refused a pixel")
            render_frame(panorama, lens, Pose.identity())   # and it renders

    def test_a_refused_pixel_is_reported_as_a_refusal_and_not_as_a_bad_direction(self):
        """`render_frame` must read `valid` before it consumes the directions, not after.

        The refusal is raised where the count is known; `direction_to_equirect` raises on a
        non-finite vector one line earlier in the pipeline. A refused pixel whose solver left NaN
        behind reaches that second raise first and reports "a zero or non-finite vector names no
        direction" — true of the row, and the wrong diagnosis of the frame, because it names a
        direction rather than the lens that could not produce one.

        Forced rather than found: no lens surviving `lens_folds_in_frame` is known to leave a NaN
        here, so the arrangement substitutes an `unproject` that refuses one row with a NaN. That
        is the state the ordering exists for, and constructing it is the only way to reach it.
        """
        panorama = direction_encoded_panorama(256, 128)
        lens = lens_from_fov(66.0, 50.0, 8, 6)
        honest = synth_dataset.unproject

        def refuses_one_row_with_a_nan(lens_, pixels):
            directions, valid = honest(lens_, pixels)
            directions = directions.copy()
            valid = valid.copy()
            directions[0] = np.nan
            valid[0] = False
            return directions, valid

        synth_dataset.unproject = refuses_one_row_with_a_nan
        try:
            with self.assertRaises(ValueError) as raised:
                render_frame(panorama, lens, Pose.identity())
        finally:
            synth_dataset.unproject = honest

        self.assertIn("no ray behind them", str(raised.exception))
        self.assertIn("1 of 48", str(raised.exception))


class TheAnswerIsTheNearBranch(unittest.TestCase):
    """Judged against an oracle that does not call the code under test.

    Round 1's fold tests were their own oracle: they filtered with `defined_at` inside `unproject`
    and then asserted `defined_at` on what survived, so `defined_at` could be replaced by `return
    True` with the whole suite green. A reviewer proved that by doing it. The lesson is one this
    repository had already written down about `Pose.rotate` one round earlier and then repeated
    inside the fix for it: a function that is its own oracle cannot fail.

    So the expectation here is built from the forward map alone, by brute force. `_near_branch`
    tabulates `r -> r * radial(r^2)` on a dense grid, cuts the table at the first radius where the
    map stops increasing, and reads the answer off backwards. It is far too slow to ship and it
    calls nothing this module is testing, which is exactly what makes it an oracle.
    """

    @staticmethod
    def _near_branch(lens, pixels, samples=400_000, r_max=8.0):
        """(directions, exists) for each pixel, from the forward map only.

        **Radial lenses only, and it refuses rather than answering for others.** The table below is
        a function of `r` alone, so `p1` and `p2` appear nowhere in it: on a lens with tangential
        distortion this would return the truth of a *different* lens, and the tests using it would
        compare against a wrong answer that looks entirely reasonable. Both current callers pass
        purely radial lenses, which is the arrangement doing the work of a check — the pattern this
        oracle exists to defend against, one level up in the oracle itself. A reviewer spotted it.

        Refusing keeps the limitation loud. The tangential half is covered by a different route:
        `TheTangentialTermsAreLoadBearing` uses a lens where the closed form and the endpoint both
        pass and only the sampling along the ray refuses.
        """
        if lens.p1 != 0.0 or lens.p2 != 0.0:
            raise ValueError(
                f"this oracle is radial only and cannot describe p1={lens.p1}, p2={lens.p2}; "
                "answering would compare the code against the truth of a different lens")
        xd = (pixels[:, 0] - lens.cx) / lens.fx
        yd = (pixels[:, 1] - lens.cy) / lens.fy
        rd = np.hypot(xd, yd)

        r = np.linspace(0.0, r_max, samples)
        r2 = r * r
        radial = 1.0 + r2 * (lens.k1 + r2 * (lens.k2 + r2 * lens.k3))
        slope = 1.0 + r2 * (3.0 * lens.k1 + r2 * (5.0 * lens.k2 + r2 * 7.0 * lens.k3))
        folded = np.nonzero((slope <= 0.0) | (radial <= 0.0))[0]
        end = folded[0] if len(folded) else len(r)
        r, f = r[:end], (r * radial)[:end]

        exists = (rd <= f.max()) if len(f) else np.zeros_like(rd, dtype=bool)
        solved = np.interp(rd, f, r)                    # f increases on this branch, so this is safe
        scale = np.where(rd > 0.0, solved / np.where(rd > 0.0, rd, 1.0), 1.0)
        directions = np.stack([xd * scale, -yd * scale, -np.ones_like(xd)], axis=-1)
        directions /= np.linalg.norm(directions, axis=-1, keepdims=True)
        return directions, exists

    def _frame_pixels(self, lens):
        us, vs = np.meshgrid(np.arange(lens.width) + 0.5, np.arange(lens.height) + 0.5,
                             indexing="xy")
        return np.stack([us.ravel(), vs.ravel()], axis=-1)

    def test_the_oracle_refuses_a_lens_it_cannot_describe(self):
        """The refusal added last round was itself deletable with the suite green.

        Which is the same defect it was added to prevent, one level up: a guard whose state nothing
        reaches. Both other callers pass radial lenses on purpose, so nothing ever handed the oracle
        a tangential one — and a guard nobody triggers is indistinguishable from a guard that does
        not work.
        """
        tangential = Intrinsics(**{**lens_from_fov(66.0, 50.0, 16, 12).__dict__, "p1": 0.002})
        with self.assertRaises(ValueError):
            self._near_branch(tangential, np.array([[8.0, 6.0]]))

        radial = lens_from_fov(66.0, 50.0, 16, 12)
        directions, exists = self._near_branch(radial, np.array([[8.0, 6.0]]))
        self.assertTrue(exists.all(), "and it still answers for a lens it can describe")
        self.assertEqual(directions.shape, (1, 3))

    def test_a_folding_lens_answers_no_pixel_from_the_far_side(self):
        """The defect round 1 introduced while fixing the defect round 1 found.

        `k1 = -1, k2 = 0.3` folds between r = 0.650 and r = 1.256 — the core's own documented
        example. Before this was fixed, `unproject` accepted 36,036 pixels whose solved radius sat
        past the fold, the frame corner among them at r = 1.5832 where the near branch is 0.6499:
        24.7 degrees apart, both genuine preimages, `valid = True` on the wrong one. The round trip
        cannot separate them and ADR 0046 says why, so the test has to know the right answer
        independently rather than ask the solver to mark its own work.
        """
        lens = Intrinsics(**{**lens_from_fov(66.0, 50.0, 48, 36).__dict__, "k1": -1.0, "k2": 0.3})
        pixels = self._frame_pixels(lens)

        truth, exists = self._near_branch(lens, pixels)
        self.assertTrue(exists.any(), "the arrangement must have answerable pixels")
        self.assertFalse(exists.all(), "and unanswerable ones, or it proves nothing about the fold")

        directions, valid = unproject(lens, pixels)

        self.assertFalse(bool((valid & ~exists).any()),
                         f"{int((valid & ~exists).sum())} pixels answered that have no near-branch "
                         "preimage at all")
        agreed = valid & exists
        self.assertGreater(int(agreed.sum()), 0, "it refused everything, which proves nothing")
        self.assertLess(worst_angle_deg(directions[agreed], truth[agreed]), 0.01,
                        "an answered pixel disagrees with the near branch")

    def test_an_ordinary_lens_is_answered_everywhere_and_correctly(self):
        """The other half: the fix must not buy its correctness by refusing everything.

        A 66x50 lens with k1 = -0.28 *and* the k2 that a real calibration comes with. Note that
        k1 = -0.28 on its own does not belong in this list and ADR 0050 used to claim it did --
        see `RealisticLensesAreNotAllAnswerable`.
        """
        lens = Intrinsics(**{**lens_from_fov(66.0, 50.0, 48, 36).__dict__, "k1": -0.28, "k2": 0.09})
        pixels = self._frame_pixels(lens)
        truth, exists = self._near_branch(lens, pixels)
        self.assertTrue(exists.all(), "the arrangement is meant to be answerable everywhere")

        directions, valid = unproject(lens, pixels)
        self.assertTrue(valid.all(), f"{int((~valid).sum())} pixels refused on an ordinary lens")
        self.assertLess(worst_angle_deg(directions, truth), 0.01)


class RealisticLensesAreNotAllAnswerable(unittest.TestCase):
    """A correction to ADR 0050, pinned so the prose cannot drift back.

    The ADR said the lenses a phone actually has are nowhere near the fold, and offered
    `k1 = -0.28` leaving the slope at +0.65 as the evidence. That number silently needs the `k2`
    the sentence omits. With `k1 = -0.28` alone, a 66x50 degree frame's corner is *past the fold*:
    the largest distorted radius the lens can produce is 0.7275 and the corner sits at 0.8104, so
    that pixel has no preimage on either branch and refusing it is right.
    """

    def test_a_negative_k1_with_no_k2_folds_inside_a_sixty_six_degree_frame(self):
        lens = Intrinsics(**{**lens_from_fov(66.0, 50.0, 48, 36).__dict__, "k1": -0.28})
        us, vs = np.meshgrid(np.arange(lens.width) + 0.5, np.arange(lens.height) + 0.5,
                             indexing="xy")
        pixels = np.stack([us.ravel(), vs.ravel()], axis=-1)
        _, valid = unproject(lens, pixels)
        self.assertFalse(valid.all(),
                         "k1 = -0.28 alone reaches the fold inside the frame; if this passes, the "
                         "geometry changed and ADR 0050's correction needs revisiting")

    def test_the_same_k1_with_a_real_calibrations_k2_is_answerable_throughout(self):
        lens = Intrinsics(**{**lens_from_fov(66.0, 50.0, 48, 36).__dict__, "k1": -0.28, "k2": 0.09})
        us, vs = np.meshgrid(np.arange(lens.width) + 0.5, np.arange(lens.height) + 0.5,
                             indexing="xy")
        pixels = np.stack([us.ravel(), vs.ravel()], axis=-1)
        _, valid = unproject(lens, pixels)
        self.assertTrue(valid.all(), "the positive k2 is what pulls the fold outside the frame")


class ARotationIsRecordedAsTheOneThatWasRendered(unittest.TestCase):
    """Round 1 normalised `rotate` and left the record alone, which fixed half of one defect.

    The frames were then rendered with a normalised quaternion while `truth.json` wrote down the
    unnormalised one the caller happened to pass. A consumer that believes the file's own
    "unit quaternion" claim is handed a rotation the pixels were never taken at. And the degenerate
    case is worse than a small error: `Pose(0, 0, 0, 0)` renders as the identity and was recorded
    as `{0, 0, 0, 0}`, which `sphanorama::Normalize` turns back into the identity on the C++ side —
    so two byte-identical frames sit beside two different recorded rotations and nothing anywhere
    notices.
    """

    def _rotation_in(self, directory, index=0):
        truth = json.loads((Path(directory) / "truth.json").read_text())
        return truth["frames"][index]["rotation"]

    def test_the_recorded_quaternion_is_the_unit_one_the_frame_was_rendered_with(self):
        panorama = direction_encoded_panorama(256, 128)
        lens = lens_from_fov(66.0, 50.0, 32, 24)
        unit = Pose.from_axis_angle((0.3, 0.5, -0.8), 0.7)
        long_by_one_percent = Pose(unit.w * 1.01, unit.x * 1.01, unit.y * 1.01, unit.z * 1.01)

        with tempfile.TemporaryDirectory() as directory:
            write_dataset(Path(directory), panorama, lens, [long_by_one_percent])
            recorded = self._rotation_in(directory)

        norm = math.sqrt(sum(recorded[k] ** 2 for k in "wxyz"))
        self.assertAlmostEqual(norm, 1.0, places=12,
                               msg="the file says unit quaternion; this one is not")
        for component in "wxyz":
            self.assertAlmostEqual(recorded[component], getattr(unit, component), places=12)

    def test_a_rotation_that_is_not_a_rotation_is_refused_rather_than_recorded(self):
        panorama = direction_encoded_panorama(256, 128)
        lens = lens_from_fov(66.0, 50.0, 32, 24)
        with tempfile.TemporaryDirectory() as directory:
            for name, pose in (
                ("all zero", Pose(0.0, 0.0, 0.0, 0.0)),
                ("not a number", Pose(float("nan"), 0.0, 0.0, 0.0)),
                ("infinite", Pose(float("inf"), 0.0, 0.0, 0.0)),
            ):
                with self.assertRaises(ValueError, msg=name):
                    write_dataset(Path(directory), panorama, lens, [pose])

    def test_a_component_large_enough_to_square_to_infinity_still_rotates(self):
        """`sqrt(w*w + x*x + ...)` overflows before it sums, and underflows the same way.

        `Pose(0, 1e200, 0, 0)` is a 180 degree turn about +X. Squaring 1e200 is infinity, so the
        norm is infinity, every component divides to zero and the rotation silently becomes the
        identity — while the recorded rotation says the frame was turned over. `Pose(0, 1e-200, 0,
        0)` is the same turn and underflows to a zero norm, taking the other early exit to the
        same wrong place. This is PR #49 round 14's `Normalize` overflow, in a second language.
        """
        forward = np.array([[0.0, 0.0, -1.0]])
        turned = np.array([[0.0, 0.0, 1.0]])
        for name, pose in (("huge", Pose(0.0, 1e200, 0.0, 0.0)),
                           ("tiny", Pose(0.0, 1e-200, 0.0, 0.0)),
                           ("ordinary", Pose(0.0, 1.0, 0.0, 0.0))):
            np.testing.assert_allclose(pose.rotate(forward), turned, atol=1e-12,
                                       err_msg=f"{name}: a 180 degree turn came back as something else")


class RefusalsThatCannotBeMistakenForAnswers(unittest.TestCase):
    """Every way of having no answer here has been a colour or a plausible number at some point."""

    def test_a_non_finite_colour_is_refused_rather_than_written_as_black(self):
        """Byte 0 is black, and black is a colour the scene produces.

        This is the sentinel round 1 removed mid-grey 128 for, reintroduced one layer down by the
        cast: `np.round(nan).astype(np.uint8)` is 0 on this platform, silently, with only a numpy
        RuntimeWarning that nothing reads.
        """
        with self.assertRaises(ValueError):
            _to_bytes(np.array([[[math.nan, 0.5, 1.0]]]))
        with self.assertRaises(ValueError):
            _to_bytes(np.array([[[math.inf, 0.5, 1.0]]]))
        # and an ordinary frame still encodes
        np.testing.assert_array_equal(_to_bytes(np.array([[[-1.0, 0.0, 1.0]]])),
                                      np.array([[[0, 128, 255]]], dtype=np.uint8))

    def test_a_panorama_that_is_not_three_channel_is_refused_before_a_p6_header_lies_about_it(self):
        """`P6` means three bytes a pixel. The header was hard-coded and the payload was not."""
        lens = lens_from_fov(66.0, 50.0, 16, 12)
        for channels in (1, 4):
            panorama = np.zeros((64, 128, channels), dtype=float)
            with tempfile.TemporaryDirectory() as directory:
                with self.assertRaises(ValueError, msg=f"{channels} channels"):
                    write_dataset(Path(directory), panorama, lens, [Pose.identity()])

    def test_an_empty_batch_is_answered_emptily_rather_than_raising_from_numpy(self):
        lens = lens_from_fov(66.0, 50.0, 16, 12)
        directions, valid = unproject(lens, np.zeros((0, 2)))
        self.assertEqual(directions.shape, (0, 3))
        self.assertEqual(valid.shape, (0,))
        u, v, ok = project(lens, np.zeros((0, 3)))
        self.assertEqual((u.shape, v.shape, ok.shape), ((0,), (0,), (0,)))

    def test_an_optical_centre_outside_the_image_is_not_a_lens(self):
        """The core's `IsUsableLens` requires it and this claimed to be that function.

        A lens whose optical centre is outside its own image is not something the fold reasoning,
        the field of view or the projection were written to describe.
        """
        base = lens_from_fov(66.0, 50.0, 64, 48)
        for name, centre in (("cx past the right edge", {"cx": 1e9}),
                             ("cx on the edge", {"cx": 64.0}),
                             ("cx at zero", {"cx": 0.0}),
                             ("cy below the bottom", {"cy": -1.0})):
            lens = Intrinsics(**{**base.__dict__, **centre})
            self.assertFalse(is_usable_lens(lens), name)
            with self.assertRaises(ValueError, msg=name):
                project(lens, np.array([[0.0, 0.0, -1.0]]))


class ADatasetIsAllOfItOrNoneOfIt(unittest.TestCase):
    """Frames are written as they render, and `truth.json` last, so a refusal splits the pair.

    The pitch for P6 is that "any consumer can read it in a dozen lines and no consumer needs a
    library" — and the dozen-line consumer globs `frame_*.ppm`. A run that raises part way left it
    frames with no truth at all, and a second, shorter run into the same directory left it frames
    from a *previous* capture, named exactly like the current ones, beside a truth file that does
    not mention them. Ground truth silently attached to the wrong pixels is the one failure this
    whole tool exists to prevent.
    """

    def _lens(self):
        return lens_from_fov(66.0, 50.0, 16, 12)

    def test_a_pose_that_is_not_a_rotation_is_refused_before_any_file_exists(self):
        panorama = direction_encoded_panorama(128, 64)
        good = Pose.identity()
        with tempfile.TemporaryDirectory() as directory:
            out = Path(directory) / "ds"
            with self.assertRaises(ValueError):
                write_dataset(out, panorama, self._lens(),
                              [good, good, Pose(math.nan, 0.0, 0.0, 0.0)])
            written = sorted(child.name for child in out.iterdir()) if out.exists() else []
            self.assertEqual(written, [], f"a refused run left {written} behind")

    def test_a_refusal_during_rendering_also_leaves_nothing_behind(self):
        """The previous test does not reach this, and I only found that by sabotage.

        It refuses on the *pose*, which `normalised()` rejects before anything renders — so making
        the render lazy again left it green. `render_frame`'s own refusal depends on the lens rather
        than the pose, so no list of poses can fail part way through today; the state is reachable
        only if a future modifier (noise, movers, an exposure ramp) can refuse per frame, which is
        exactly what §5.5 has queued.

        So the arrangement forces it, rather than pretending a lens could. Keeping the guard with a
        test that reaches it is the alternative to keeping an untested guard because it feels safer.
        """
        panorama = direction_encoded_panorama(128, 64)
        honest = synth_dataset.render_frame
        calls = {"n": 0}

        def refuses_the_third_frame(*args, **kwargs):
            calls["n"] += 1
            if calls["n"] == 3:
                raise ValueError("a modifier refused this frame")
            return honest(*args, **kwargs)

        synth_dataset.render_frame = refuses_the_third_frame
        try:
            with tempfile.TemporaryDirectory() as directory:
                out = Path(directory) / "ds"
                with self.assertRaises(ValueError):
                    write_dataset(out, panorama, self._lens(), [Pose.identity()] * 4)
                written = sorted(child.name for child in out.iterdir()) if out.exists() else []
                self.assertEqual(written, [],
                                 f"a run that refused its third frame left {written} behind")
                # And nothing beside it either. A sabotage showed the staging cleanup on this path
                # was untested: asserting `out` is empty says nothing about the hidden sibling the
                # render was writing into.
                leaked = [c.name for c in out.parent.iterdir() if c.name.startswith(".")]
                self.assertEqual(leaked, [], f"staging directories leaked: {leaked}")
        finally:
            synth_dataset.render_frame = honest

    def test_a_failed_second_run_does_not_destroy_the_first_ones_dataset(self):
        """The round-2 fix swept stale frames *before* writing, so a failure destroyed both.

        All-or-nothing covered rendering and stopped there: the unlink loop ran between the render
        and the write, so an I/O failure during writing left the previous dataset deleted and the
        new one half-present — `truth.json` describing frames that are gone, beside files from
        neither run. That is "ground truth silently attached to the wrong frames", which the comment
        three lines above the sweep calls the one failure this tool exists to prevent, reintroduced
        by the commit that quoted it.
        """
        panorama = direction_encoded_panorama(128, 64)
        with tempfile.TemporaryDirectory() as directory:
            out = Path(directory) / "ds"
            write_dataset(out, panorama, self._lens(), [Pose.identity()] * 3)
            before = {child.name: child.read_bytes() for child in sorted(out.iterdir())}
            self.assertEqual(len(before), 4, "three frames and a truth file")

            honest = Path.open
            calls = {"n": 0}

            def fails_on_the_second_frame(self_path, *args, **kwargs):
                if "frame_" in self_path.name and "w" in str(args[0] if args else kwargs.get("mode", "")):
                    calls["n"] += 1
                    if calls["n"] == 2:
                        raise OSError("no space left on device")
                return honest(self_path, *args, **kwargs)

            Path.open = fails_on_the_second_frame
            try:
                with self.assertRaises(OSError):
                    write_dataset(out, panorama, self._lens(), [Pose.identity()] * 3)
            finally:
                Path.open = honest

            after = {child.name: child.read_bytes() for child in sorted(out.iterdir())}
            self.assertEqual(after, before,
                             "a failed run changed the dataset that was already there")
            # This is the path where the staging directory exists when the failure happens — the
            # render is complete by then — so it is the one that pins the cleanup. The
            # refused-during-rendering test cannot: nothing is staged yet at that point.
            leaked = [c.name for c in out.parent.iterdir() if c.name.startswith(".")]
            self.assertEqual(leaked, [], f"staging directories leaked: {leaked}")

    def test_a_failure_while_swapping_leaves_the_old_dataset_whole(self):
        """The staging fix moved the window rather than closing it, and a reviewer measured it.

        Files were moved into `out` one at a time, so an `OSError` on the second `replace` left the
        previous dataset partly deleted, `out` holding one new frame and no `truth.json`, and the
        staging directory leaked. Both properties that were supposed to make it safe were
        mutation-green — deleting the cleanup, and deleting the truth-first unlink, each left the
        whole suite passing.

        Two directory renames close it: the old dataset moves aside whole, the new one takes its
        place whole, and the loser is removed afterwards. A failure at any single step leaves one
        or the other complete.
        """
        panorama = direction_encoded_panorama(128, 64)
        with tempfile.TemporaryDirectory() as directory:
            out = Path(directory) / "ds"
            write_dataset(out, panorama, self._lens(), [Pose.identity()] * 3)
            before = {child.name: child.read_bytes() for child in sorted(out.iterdir())}

            honest = Path.replace
            calls = {"n": 0}

            def fails_on_the_second_move(self_path, target):
                calls["n"] += 1
                if calls["n"] == 2:
                    raise OSError("the filesystem went away")
                return honest(self_path, target)

            Path.replace = fails_on_the_second_move
            try:
                with self.assertRaises(OSError):
                    write_dataset(out, panorama, self._lens(), [Pose.identity()] * 2)
            finally:
                Path.replace = honest

            self.assertTrue(out.is_dir(), "the output directory vanished entirely")
            after = {child.name: child.read_bytes() for child in sorted(out.iterdir())}
            self.assertEqual(after, before, "a failed swap left a mixture of two datasets")
            leaked = [c.name for c in out.parent.iterdir() if c.name.startswith(".")]
            self.assertEqual(leaked, [], f"staging directories leaked: {leaked}")

    def test_a_shorter_second_run_does_not_leave_the_first_ones_frames(self):
        panorama = direction_encoded_panorama(128, 64)
        with tempfile.TemporaryDirectory() as directory:
            out = Path(directory) / "ds"
            write_dataset(out, panorama, self._lens(), [Pose.identity()] * 5)
            write_dataset(out, panorama, self._lens(), [Pose.identity()] * 2)

            frames = sorted(child.name for child in out.glob("frame_*.ppm"))
            listed = json.loads((out / "truth.json").read_text())["frames"]
            self.assertEqual(len(frames), len(listed),
                             f"{len(frames)} frames on disk, {len(listed)} in truth.json")


class ARefusedRowDoesNotLookLikeAnAnswer(unittest.TestCase):
    """The flag was closed in round 1 and the value beside it was not.

    `safe_depth = 1.0` is not a neutral placeholder — it is "pretend this direction is one unit in
    front of the camera", which for a direction lying in or behind the optical plane lands on the
    principal point: the single most plausible-looking pixel available. And a refused `unproject`
    row came back as a *unit* vector, indistinguishable by inspection from an answered one.

    That the one in-tree caller reads the mask is not the argument for leaving it: this file already
    shipped a caller that consumed the directions before reading the mask, and the only reason that
    was survivable is that the fabricated rows happened to be NaN. Under a lens where they are not,
    the same mistake reads as a rendered frame.
    """

    def test_a_refused_projection_is_not_answered_at_the_principal_point(self):
        lens = lens_from_fov(66.0, 50.0, 320, 240)
        behind_and_worse = np.array([[0.0, 0.0, 1.0],          # behind the camera
                                     [0.0, 0.0, 0.0],          # in the optical plane
                                     [0.3, -0.2, math.inf]])   # not a measurement
        u, v, valid = project(lens, behind_and_worse)
        self.assertFalse(valid.any(), "the arrangement must be refused, or this tests nothing")
        self.assertTrue(np.isnan(u).all() and np.isnan(v).all(),
                        f"refused pixels came back as {list(zip(u, v))}, which are pixels")

    def test_a_refused_unprojection_is_not_answered_as_a_unit_direction(self):
        lens = Intrinsics(**{**lens_from_fov(66.0, 50.0, 48, 36).__dict__, "k1": -1.0, "k2": 0.3})
        us, vs = np.meshgrid(np.arange(lens.width) + 0.5, np.arange(lens.height) + 0.5,
                             indexing="xy")
        pixels = np.stack([us.ravel(), vs.ravel()], axis=-1)
        directions, valid = unproject(lens, pixels)
        self.assertFalse(valid.all(), "the arrangement must refuse something")
        self.assertTrue(np.isnan(directions[~valid]).all(),
                        "a refused direction came back finite, so it reads as an answer")
        self.assertTrue(np.isfinite(directions[valid]).all(),
                        "and an answered one must still be a direction")


class TheAcceptanceToleranceIsNamedAndInTheCoresUnit(unittest.TestCase):
    """An unnamed `1e-6` in pixels made the refusal boundary move with the focal length.

    The core accepts a round trip within `kInverseToleranceNormalised = 1e-9`, in normalised units.
    This file compared pixels against a literal, so the same lens family drew the line in different
    places depending on frame size — 27x looser than the core at 48x36, 3x tighter at 4000x3000.
    Two implementations ADR 0050 pins to each other should not disagree about where a refusal is.

    `INVERSE_TOLERANCE` was also named for a job it does not do: it is the settled-step early exit,
    which the core calls `kSettledStepNormalised`.
    """

    def test_the_constant_is_the_cores_number(self):
        """**This pins the constant, not a behaviour, and that is a real limitation.**

        Say so plainly, because a sabotage proved it: putting the old unnamed `1e-6`-in-pixels back
        leaves all 58 tests green. The two bounds differ by 27x at 48x36 and 3x the other way at
        4000x3000, but the solver converges around 1e-12 normalised, which is four orders inside
        both — so no input I could construct lands in the gap between them.

        The fix is therefore latent correctness rather than a behaviour change, and this test is
        documentation with an assertion attached. It still earns its place: it fails if someone
        retunes the constant without meeting the core, which is the drift ADR 0050's pinning exists
        to catch, and the docstring is the only place that records why a stronger test is not here.
        """
        self.assertEqual(INVERSE_ACCEPTANCE_NORMALISED, 1e-9,
                         "the core's kInverseToleranceNormalised is what this pins to")
        # And the bound is applied in normalised units, so it is the same statement at every size.
        for width, height in ((48, 36), (320, 240), (1600, 1200)):
            lens = lens_from_fov(66.0, 50.0, width, height)
            _, valid = unproject(lens, np.array([[lens.cx, lens.cy]]))
            self.assertTrue(valid.all(), f"{width}x{height}: the principal point must invert")


class GuardsThatAreReachedByTheTestsNamedForThem(unittest.TestCase):
    """Round 2 deleted four guards one at a time and the suite stayed green on each.

    That is a different complaint from "there is no test": there were tests, and each proved
    something adjacent. The pattern is worth naming because it has now appeared three times on this
    branch — a test reaches the behaviour it is named for only if nothing *else* refuses the input
    first, and the earlier refusal is usually the more obvious one.
    """

    def test_unproject_refuses_an_unusable_lens_itself_rather_than_via_project(self):
        """`unproject` calls `project` to adjudicate, and `project` has its own usability guard.

        So deleting `unproject`'s guard left the suite green: with `fx = fy = 0` the arithmetic runs
        to completion on infinities and the *adjudicator* raises, which the old test could not tell
        apart. This one asserts that `project` is never reached, which is the only statement that
        distinguishes them.
        """
        honest = synth_dataset.project
        reached = {"yes": False}

        def note_that_it_was_reached(*args, **kwargs):
            reached["yes"] = True
            return honest(*args, **kwargs)

        synth_dataset.project = note_that_it_was_reached
        try:
            with self.assertRaises(ValueError):
                unproject(Intrinsics(), np.array([[1.0, 1.0]]))
        finally:
            synth_dataset.project = honest
        self.assertFalse(reached["yes"],
                         "unproject reached its adjudicator, so its own guard did nothing")

    def test_a_direction_whose_normalisation_overflows_is_refused(self):
        """The `isfinite(xn) & isfinite(yn)` conjunct, reached on its own.

        Every other arrangement in the suite refuses earlier — a NaN component is caught by the
        camera-space test one line up. This one is finite in every component and has a positive,
        finite depth; it is the *division* that overflows, which is the only way to reach this line
        without another guard having fired first.
        """
        lens = lens_from_fov(66.0, 50.0, 64, 48)
        overflowing = np.array([[1e300, 0.0, -1e-300]])
        with np.errstate(over="ignore", invalid="ignore"):
            u, v, valid = project(lens, overflowing)
        self.assertFalse(valid.any(), "an overflowing normalisation was answered")
        self.assertTrue(np.isnan(u).all() and np.isnan(v).all())

        # And the conjunct earns its place by *where* it sits, which is the part a refusal test
        # cannot see: the `isfinite(u) & isfinite(v)` test further down refuses the same row, so
        # deleting this one changes no answer. What it changes is whether an infinity reaches
        # `inverts_along_the_ray`, which iterates a cubic and samples a Jacobian on it. Asserting
        # the outcome alone left the guard deletable with the suite green; asserting that nothing
        # invalid is *computed* is what pins it.
        with np.errstate(over="ignore", invalid="raise"):
            project(lens, overflowing)


class TheTangentialTermsAreLoadBearing(unittest.TestCase):
    """Nothing in the suite had tangential distortion strong enough to matter.

    Measured by sabotage at `0cef5fe`: `RAY_SAMPLES = 1` and zeroing the Jacobian's off-diagonal
    `cross` term each left every test green. Both are specifically about `p1` and `p2` — the radial
    part factors exactly and is settled in closed form, so the 64 samples along the ray exist *only*
    for the tangential half, and `cross` is the coupling that half introduces.

    A lens list where every entry is radial, or where the tangential coefficients are the 0.002 of a
    real calibration, cannot reach either. So this uses one that is frankly pathological, and says
    so: the point is not that a phone has this lens, it is that these two lines have a job and no
    arrangement in the file asked them to do it.
    """

    def _pathological(self):
        return Intrinsics(**{**lens_from_fov(66.0, 50.0, 64, 48).__dict__,
                             "k1": -0.1, "p1": 1.2, "p2": 1.2})

    def _grid(self):
        xs = np.linspace(-0.7, 0.7, 21)
        ys = np.linspace(-0.55, 0.55, 21)
        gx, gy = np.meshgrid(xs, ys, indexing="ij")
        return gx.ravel(), gy.ravel()

    def test_a_tangential_fold_is_caught_only_by_sampling_along_the_ray(self):
        lens = self._pathological()
        xn, yn = self._grid()

        radial_sound = radial_map_increases_up_to(lens, xn * xn + yn * yn)
        endpoint_sound = defined_at(lens, xn, yn)
        refused = ~inverts_along_the_ray(lens, xn, yn)

        # Points the closed form calls sound and the endpoint calls sound, which the ray sampling
        # still refuses: the fold is *between* the optical centre and the point.
        only_the_ray = radial_sound & endpoint_sound & refused
        self.assertGreater(int(only_the_ray.sum()), 0,
                           "the arrangement no longer has a tangential-only fold, so this proves "
                           "nothing about the ray sampling")

    def test_the_jacobians_off_diagonal_term_changes_the_answer(self):
        """`cross` is what tangential distortion contributes to the determinant.

        Zeroing it left the whole suite green, because no lens in the file had a `p1` or `p2` large
        enough for the coupling to decide anything.
        """
        lens = self._pathological()
        xn, yn = self._grid()
        at = distort_at(lens, xn, yn)

        self.assertGreater(float(np.max(np.abs(at.cross))), 0.5,
                           "the arrangement must have a substantial off-diagonal term")
        without_the_coupling = at.dxdx * at.dydy
        self.assertFalse(np.array_equal((at.determinant > 0.0), (without_the_coupling > 0.0)),
                         "dropping the off-diagonal term changed no verdict, so this lens does not "
                         "exercise it")


class APixelThatIsNotAMeasurementGetsNoDirection(unittest.TestCase):
    """A non-finite pixel is refused. **This pins the contract, not a mechanism.**

    Said explicitly because a round-3 reviewer reported the core's two input guards as missing from
    the port, and porting them turned out to change nothing: with them or without, a non-finite
    pixel comes back `valid = False` with a NaN direction, and no arithmetic on the way raises even
    under `np.errstate(all="raise")`. The infinity propagates to NaN and the round trip refuses it.

    So the guards are deliberately not here and the reason is in `unproject`. What is worth pinning
    is the promise a caller depends on — a pixel that is not a measurement gets no direction — and
    that is what this asserts, on whatever refuses it.
    """

    def test_a_non_finite_pixel_is_refused_without_computing_on_it(self):
        """Refused **and** quietly, which is the half I got wrong.

        I removed the core's input guards on a measurement that said they change nothing here, and
        a round-4 reviewer showed the measurement was false: `unproject([[inf, 24.0]])` raises
        `FloatingPointError` under `errstate(all="raise")` and emits 311 RuntimeWarnings from that
        single pixel by default. My probe had suppressed exactly the categories it was meant to be
        looking for, so it reported silence it had itself arranged.

        `errstate(all="raise")` is therefore the assertion, not a convenience: the contract is that
        a pixel which is not a measurement is refused without the solver ever computing on it, and
        that is a property only this can express.
        """
        lens = lens_from_fov(66.0, 50.0, 64, 48)
        for name, pixel in (("infinite", [math.inf, 24.0]),
                            ("not a number", [32.0, math.nan]),
                            ("both", [math.nan, math.inf])):
            with np.errstate(all="raise"):
                directions, valid = unproject(lens, np.array([pixel]))
            self.assertFalse(valid.any(), name)
            self.assertTrue(np.isnan(directions).all(), name)

    def test_a_refused_pixel_does_not_make_the_others_noisy(self):
        """One bad row must not drag the whole vectorised batch through invalid arithmetic."""
        lens = lens_from_fov(66.0, 50.0, 64, 48)
        batch = np.array([[32.0, 24.0], [math.inf, 24.0], [10.0, 8.0]])
        with np.errstate(all="raise"):
            directions, valid = unproject(lens, batch)
        self.assertTrue(bool(valid[0]) and bool(valid[2]), "the good rows must still be answered")
        self.assertFalse(bool(valid[1]))

    def test_a_finite_but_enormous_pixel_is_refused_rather_than_answered(self):
        """A pixel at 1e300 normalises to a finite 2.03e298, so the guard above does not fire.

        It is refused further down, by the round trip, which is also what the core does — its
        `isfinite(xd)` check passes on this input too. The reviewer who found this reported it as
        `unproject` computing on infinities, and the refusal is right; what was wrong was only the
        missing *input* guard above, for a pixel that is itself non-finite.

        Deliberately not asserted under `errstate(all="raise")`: the intermediate overflow is real
        and the C++ has it too, where it produces an infinity rather than an exception. Promising
        that no floating-point event occurs would be a stricter contract than the thing this file
        is pinned to.
        """
        lens = lens_from_fov(66.0, 50.0, 64, 48)
        with np.errstate(over="ignore", invalid="ignore"):
            directions, valid = unproject(lens, np.array([[1e300, 24.0]]))
        self.assertFalse(valid.any())
        self.assertTrue(np.isnan(directions).all())


class AProjectedPixelIsAFiniteNumber(unittest.TestCase):
    """`camera_model.cpp` names this guard as the one with no backstop, and the port had no test.

    Its own witness, reproduced here: a `k3` large enough that `radial` overflows the distorted
    coordinate while every earlier guard passes. `is_usable_lens` says yes, the fold test says yes,
    and `u` comes out infinite — so without this the answer is `valid = True` at pixel `(inf, cy)`.
    """

    def test_a_lens_whose_radial_term_overflows_the_pixel_is_refused(self):
        lens = Intrinsics(**{**lens_from_fov(66.0, 50.0, 64, 48).__dict__, "k3": 1e305})
        direction = np.array([[1.8, 0.0, -1.0]])
        self.assertTrue(is_usable_lens(lens), "the arrangement must survive the earlier guards")
        with np.errstate(over="ignore", invalid="ignore"):
            self.assertTrue(inverts_along_the_ray(lens, np.array([1.8]), np.array([0.0])).all(),
                            "the fold test must pass, or this proves nothing about finiteness")
            u, v, valid = project(lens, direction)
        self.assertFalse(valid.any(), f"answered pixel ({u[0]}, {v[0]})")
        self.assertTrue(np.isnan(u).all() and np.isnan(v).all())


class TheCommandLineRefusesBeforeItSpendsAnything(unittest.TestCase):
    """A smoke test over `main()`, which two rounds of deferral had left with none.

    Round 1 deferred this: `_ring_of_poses` and the defaults are placeholders that §5.5's
    trajectory work replaces, and the risk being weighed was `from_azimuth_elevation`, which round 1
    then anchored. That reasoning has expired. The risk that remains is in the argument plumbing the
    deferral left uncovered, and a reviewer measured both halves of it: `--out` naming a file spends
    the entire render before raising `FileExistsError` — ten seconds at twelve frames, hours at the
    scale the roadmap plans — and `--frames 0` deletes an existing dataset, prints "wrote 0 frames"
    and exits 0.

    These assertions are about the CLI's contract rather than the ring's shape, so they survive the
    trajectory rewrite that the deferral was waiting for.
    """

    def _run(self, *argv):
        argv = ["synth_dataset.py", *argv]
        old = sys.argv
        sys.argv = argv
        try:
            return synth_dataset.main()
        finally:
            sys.argv = old

    def test_an_out_that_is_a_file_is_refused_before_a_single_frame_renders(self):
        with tempfile.TemporaryDirectory() as directory:
            occupied = Path(directory) / "not-a-directory"
            occupied.write_text("something else lives here\n")

            rendered = {"n": 0}
            honest = synth_dataset.render_frame

            def count(*args, **kwargs):
                rendered["n"] += 1
                return honest(*args, **kwargs)

            synth_dataset.render_frame = count
            try:
                with self.assertRaises(SystemExit):
                    self._run("--out", str(occupied), "--frames", "2")
            finally:
                synth_dataset.render_frame = honest

            self.assertEqual(rendered["n"], 0, "it rendered before checking where the output goes")
            self.assertEqual(occupied.read_text(), "something else lives here\n")

    def test_every_shape_of_unusable_out_is_refused_before_rendering(self):
        """The first version of this check covered one shape and a reviewer found three more.

        `out.exists()` is False for a **dangling symlink**, so that slipped through; a `--out` whose
        parent is a file, and a `.<name>.partial` already occupied by a file, were not considered at
        all. Each spent the entire render and then raised the very error the check was added to
        pre-empt — measured at two renders each here, hours at the scale the roadmap plans.

        The lesson is the one this branch keeps relearning in different clothes: a guard written
        against the example in a bug report covers the example. `Path.exists()` follows symlinks and
        answers a question about the *target*, which is not the question being asked.
        """
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "afile").write_text("not a directory\n")
            (root / ".occupied.partial").write_text("in the way\n")
            dangling = root / "dangling"
            dangling.symlink_to(root / "does-not-exist")

            cases = {
                "a file": root / "afile",
                "a dangling symlink": dangling,
                "under a file": root / "afile" / "ds",
                "staging occupied": root / "occupied",
            }
            for name, out in cases.items():
                rendered = {"n": 0}
                honest = synth_dataset.render_frame

                def count(*args, **kwargs):
                    rendered["n"] += 1
                    return honest(*args, **kwargs)

                synth_dataset.render_frame = count
                try:
                    with self.assertRaises(SystemExit, msg=name):
                        self._run("--out", str(out), "--frames", "2",
                                  "--width", "16", "--height", "12")
                finally:
                    synth_dataset.render_frame = honest
                self.assertEqual(rendered["n"], 0, f"{name}: rendered before refusing")

    def test_a_lens_that_is_not_a_lens_is_refused_by_the_parser(self):
        """`--hfov 0` used to reach `lens_from_fov` and traceback rather than say what was wrong."""
        with tempfile.TemporaryDirectory() as directory:
            out = Path(directory) / "ds"
            for flag, value in (("--hfov", "0"), ("--vfov", "-10"), ("--hfov", "180"),
                                ("--width", "0"), ("--height", "-4")):
                with self.assertRaises(SystemExit, msg=f"{flag} {value}"):
                    self._run("--out", str(out), flag, value)

    def test_a_frame_count_that_is_not_a_count_is_refused(self):
        with tempfile.TemporaryDirectory() as directory:
            out = Path(directory) / "ds"
            self._run("--out", str(out), "--frames", "2")
            before = sorted(child.name for child in out.iterdir())

            for count in ("0", "-3"):
                with self.assertRaises(SystemExit, msg=count):
                    self._run("--out", str(out), "--frames", count)

            self.assertEqual(sorted(child.name for child in out.iterdir()), before,
                             "a refused run changed the dataset that was there")

    def test_an_ordinary_invocation_writes_what_it_says_it_wrote(self):
        with tempfile.TemporaryDirectory() as directory:
            out = Path(directory) / "ds"
            self.assertEqual(self._run("--out", str(out), "--frames", "3",
                                       "--width", "32", "--height", "24"), 0)
            frames = sorted(out.glob("frame_*.ppm"))
            truth = json.loads((out / "truth.json").read_text())
            self.assertEqual(len(frames), 3)
            self.assertEqual(len(truth["frames"]), 3)
            self.assertEqual(truth["intrinsics"]["width"], 32)


class TheSeamWrapsOnBothSides(unittest.TestCase):
    """`SeamSampling` tested the right-hand wrap and the left-hand one was never reached.

    Measured across the whole suite: 28,071 sampled coordinates, and `x0` was negative in **none**
    of them. The half-pixel strip `u < 0.5`, where `x0 = -1` and the left neighbour must wrap to the
    last column, is not visited even by the frame rendered straight through the seam — which is the
    same "lands on that exact column only by luck" that put `SeamSampling` here in the first place,
    one side over.
    """

    def test_the_column_before_the_first_one_is_the_last_one(self):
        panorama = np.zeros((1, 4, 3), dtype=float)
        panorama[0, 0] = [1.0, 0.0, 0.0]
        panorama[0, 3] = [0.0, 0.0, 1.0]        # the column a left wrap has to reach

        # u = 0.25 sits a quarter pixel left of the first pixel centre, so the left neighbour is
        # column -1, which is column 3.
        sampled = sample_equirect(panorama, np.array([0.25]), np.array([0.5]))
        np.testing.assert_allclose(sampled[0], [0.75, 0.0, 0.25], atol=1e-12)


class EquirectangularMapping(unittest.TestCase):
    def test_forward_is_the_centre_of_the_panorama(self):
        u, v = direction_to_equirect(np.array([[0.0, 0.0, -1.0]]), 512, 256)
        self.assertAlmostEqual(u[0], 256.0, places=9)
        self.assertAlmostEqual(v[0], 128.0, places=9)

    def test_up_is_the_top_edge_and_down_is_the_bottom(self):
        u, v = direction_to_equirect(np.array([[0.0, 1.0, 0.0], [0.0, -1.0, 0.0]]), 512, 256)
        self.assertAlmostEqual(v[0], 0.0, places=9)
        self.assertAlmostEqual(v[1], 256.0, places=9)

    def test_a_quarter_turn_is_a_quarter_of_the_width(self):
        # +X is a quarter turn from forward, about +Y.
        u, _ = direction_to_equirect(np.array([[1.0, 0.0, 0.0]]), 512, 256)
        self.assertAlmostEqual(u[0], 384.0, places=9)

    def test_the_seam_wraps_rather_than_clamping(self):
        just_before = direction_to_equirect(np.array([[-1e-9, 0.0, 1.0]]), 512, 256)[0][0]
        just_after = direction_to_equirect(np.array([[1e-9, 0.0, 1.0]]), 512, 256)[0][0]
        self.assertLess(just_before, 1e-4)
        self.assertGreater(just_after, 512.0 - 1e-4)


class ADirectionNeedNotArriveNormalised(unittest.TestCase):
    """`direction_to_equirect` normalises, and nothing in the suite ever gave it a reason to.

    Every caller hands it unit vectors — `unproject` normalises, `Pose.rotate` preserves length,
    and the hand-written cases all use unit axes — so replacing the normalisation with a pass-through
    left the whole suite green. Longitude is `arctan2(x, -z)`, invariant under positive scaling, so
    it never notices; latitude is `arcsin(clip(y, -1, 1))` and very much does.
    """

    def test_a_long_vector_names_the_same_pixel_as_its_unit_direction(self):
        up_thirty = np.array([[0.0, math.sin(math.radians(30.0)), -math.cos(math.radians(30.0))]])
        unit_u, unit_v = direction_to_equirect(up_thirty, 512, 256)
        long_u, long_v = direction_to_equirect(up_thirty * 2.0, 512, 256)

        np.testing.assert_allclose(long_u, unit_u, atol=1e-12)
        # Without the normalisation `y = 1.0` clips to the pole and this lands on row 0 instead.
        np.testing.assert_allclose(long_v, unit_v, atol=1e-12)
        self.assertGreater(float(unit_v[0]), 1.0, "the arrangement must not already be at the pole")


class TheDistortionIsWrittenDownOnce(unittest.TestCase):
    """Brown-Conrady was written out four times in this file and the Jacobian twice more.

    The core hit this first and consolidated to one `DistortAt` returning a struct, with a comment
    on `Project`'s use of it saying why: that is the copy which *adjudicates* the solver's answer,
    so of the places the arithmetic lived it was the one that could least afford to drift. This
    file's copies were the solver's residual, the backtracking trial, `_distort`, and `defined_at`.

    A fact held in two places will drift, and the drift here would be silent in the worst way — the
    fold test disagreeing with the solver about where the fold is, which is precisely the
    disagreement `camera_model` exists to prevent. So this asserts the pieces agree, and it is
    written to fail if a future edit re-splits them.
    """

    def test_every_consumer_reads_the_same_distortion(self):
        lens = Intrinsics(**{**lens_from_fov(66.0, 50.0, 64, 48).__dict__,
                            "k1": -0.31, "k2": 0.14, "k3": -0.02, "p1": 0.003, "p2": -0.004})
        xn = np.array([0.21, -0.44, 0.05, 0.63])
        yn = np.array([-0.17, 0.38, 0.02, -0.51])

        at = distort_at(lens, xn, yn)

        # `_distort` is the same map.
        xd, yd = _distort(lens, xn, yn)
        np.testing.assert_allclose(at.x, xd, rtol=0, atol=0)
        np.testing.assert_allclose(at.y, yd, rtol=0, atol=0)

        # `defined_at` is the same Jacobian, to the bit.
        np.testing.assert_array_equal(defined_at(lens, xn, yn),
                                      (at.radial > 0.0) & (at.determinant > 0.0))

        # And the determinant really is the quantity the guard tests, rather than a lookalike.
        np.testing.assert_allclose(at.determinant,
                                   at.dxdx * at.dydy - at.cross * at.cross, rtol=0, atol=0)


class Rendering(unittest.TestCase):
    def test_every_rendered_pixel_lands_where_its_own_ray_points(self):
        # The panorama encodes direction as colour, so each rendered pixel can be checked against
        # the ray that drew it — no fixture, no eyeballing, and it exercises unprojection, the
        # rotation and the equirectangular lookup at once.
        panorama = direction_encoded_panorama(2048, 1024)
        lens = lens_from_fov(66.0, 50.0, 64, 48)
        pose = Pose.from_axis_angle((0.0, 1.0, 0.0), math.radians(37.0))

        frame = render_frame(panorama, lens, pose)

        us, vs = np.meshgrid(np.arange(lens.width) + 0.5, np.arange(lens.height) + 0.5,
                             indexing="xy")
        pixels = np.stack([us.ravel(), vs.ravel()], axis=-1)
        camera_directions, valid = unproject(lens, pixels)
        self.assertTrue(valid.all())
        expected = pose.rotate(camera_directions)

        rendered = frame.reshape(-1, 3)

        # Asserted in degrees, because degrees are what this dataset exists to measure and a
        # tolerance in colour units hides its own meaning. The measured interpolation error at this
        # panorama size is 2.54419e-05 degrees, so the bound below has 39 times the headroom it
        # needs — and the first version of this test used `atol=2e-3` on the components instead,
        # which was three orders of magnitude looser than the real error and let a render wrong by
        # 0.086 degrees pass. For a harness whose whole job is measuring rotation error in degrees,
        # that was the one blind spot it could not afford.
        self.assertLess(worst_angle_deg(rendered, expected), 0.001)

    def test_a_frame_looking_forward_has_the_panoramas_centre_at_its_principal_point(self):
        panorama = direction_encoded_panorama(1024, 512)
        lens = lens_from_fov(60.0, 45.0, 65, 49)   # odd, so a pixel centre sits on the axis
        frame = render_frame(panorama, lens, Pose.identity())

        centre = frame[lens.height // 2, lens.width // 2]
        self.assertLess(worst_angle_deg(centre[None, :], np.array([[0.0, 0.0, -1.0]])), 0.001)

    def test_turning_the_camera_moves_the_scene_the_other_way(self):
        panorama = direction_encoded_panorama(1024, 512)
        lens = lens_from_fov(60.0, 45.0, 65, 49)
        turned = render_frame(panorama, lens,
                              Pose.from_axis_angle((0.0, 1.0, 0.0), math.radians(20.0)))

        centre = turned[lens.height // 2, lens.width // 2]
        expected = Pose.from_axis_angle((0.0, 1.0, 0.0), math.radians(20.0)).rotate(
            np.array([[0.0, 0.0, -1.0]]))
        self.assertLess(worst_angle_deg(centre[None, :], expected), 0.001)

    def test_a_frame_straddling_the_seam_renders_as_correctly_as_any_other(self):
        # The ordinary case for a sphere, and the one the sampler's wrap exists for. Nothing tested
        # it: clamping the seam instead of wrapping left every other test green, because every
        # other render looks forward and never crosses longitude ±180 — see `SeamSampling`, which
        # is where that sabotage now dies.
        panorama = direction_encoded_panorama(2048, 1024)
        lens = lens_from_fov(66.0, 50.0, 64, 48)
        looking_back = Pose.from_axis_angle((0.0, 1.0, 0.0), math.pi)   # straight through the seam

        frame = render_frame(panorama, lens, looking_back)

        us, vs = np.meshgrid(np.arange(lens.width) + 0.5, np.arange(lens.height) + 0.5,
                             indexing="xy")
        pixels = np.stack([us.ravel(), vs.ravel()], axis=-1)
        camera_directions, valid = unproject(lens, pixels)
        self.assertTrue(valid.all())
        expected = looking_back.rotate(camera_directions)

        self.assertLess(worst_angle_deg(frame.reshape(-1, 3), expected), 0.001)

    def test_rendering_is_deterministic(self):
        panorama = direction_encoded_panorama(512, 256)
        lens = lens_from_fov(66.0, 50.0, 32, 24)
        pose = Pose.from_axis_angle((0.3, 0.8, -0.5), 0.9)
        first = render_frame(panorama, lens, pose)
        second = render_frame(panorama, lens, pose)
        np.testing.assert_array_equal(first, second)


class GroundTruth(unittest.TestCase):
    def test_the_truth_file_carries_every_pose_it_rendered(self):
        panorama = direction_encoded_panorama(256, 128)
        lens = lens_from_fov(66.0, 50.0, 32, 24)
        poses = [Pose.from_axis_angle((0.0, 1.0, 0.0), math.radians(a)) for a in (0.0, 40.0, 80.0)]

        with tempfile.TemporaryDirectory() as directory:
            written = write_dataset(Path(directory), panorama, lens, poses)

            truth = json.loads((Path(directory) / "truth.json").read_text())
            self.assertEqual(len(truth["frames"]), 3)
            self.assertEqual(len(written), 3)
            for entry, pose in zip(truth["frames"], poses):
                self.assertAlmostEqual(entry["rotation"]["w"], pose.w, places=12)
                self.assertAlmostEqual(entry["rotation"]["x"], pose.x, places=12)
                self.assertAlmostEqual(entry["rotation"]["y"], pose.y, places=12)
                self.assertAlmostEqual(entry["rotation"]["z"], pose.z, places=12)
                self.assertTrue((Path(directory) / entry["file"]).exists())

            # The lens travels with the frames, because a rotation is not enough to reproject one.
            self.assertAlmostEqual(truth["intrinsics"]["fx"], lens.fx, places=12)
            self.assertEqual(truth["intrinsics"]["width"], lens.width)

            # And each file holds the frame its own entry claims. Checking only that the files
            # exist let two separate mutations through — rendering every frame from `poses[0]`,
            # and writing all three to one name — either of which produces a dataset whose images
            # and ground truth describe different captures.
            digests = [hashlib.sha256((Path(directory) / entry["file"]).read_bytes()).hexdigest()
                       for entry in truth["frames"]]
            self.assertEqual(len(set(digests)), 3, "distinct poses produced identical frames")
            for entry, pose in zip(truth["frames"], poses):
                expected = _to_bytes(render_frame(panorama, lens, pose)).tobytes()
                body = (Path(directory) / entry["file"]).read_bytes().split(b"255\n", 1)[1]
                self.assertEqual(body, expected, entry["file"])

    def test_a_written_frame_reads_back_as_the_pixels_that_were_rendered(self):
        panorama = direction_encoded_panorama(256, 128)
        lens = lens_from_fov(66.0, 50.0, 24, 16)
        pose = Pose.from_axis_angle((0.0, 1.0, 0.0), 0.2)

        with tempfile.TemporaryDirectory() as directory:
            write_dataset(Path(directory), panorama, lens, [pose])
            raw = (Path(directory) / "frame_0000.ppm").read_bytes()

        header, _, body = raw.partition(b"255\n")
        self.assertTrue(header.startswith(b"P6\n"))
        self.assertIn(b"24 16", header)
        self.assertEqual(len(body), 24 * 16 * 3)

        rendered = render_frame(panorama, lens, pose)
        expected = np.clip((rendered + 1.0) * 0.5, 0.0, 1.0)
        expected = np.round(expected * 255.0).astype(np.uint8)
        np.testing.assert_array_equal(np.frombuffer(body, dtype=np.uint8).reshape(16, 24, 3),
                                      expected)


if __name__ == "__main__":
    unittest.main()
