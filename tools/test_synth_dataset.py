#!/usr/bin/env python3
"""Tests for the synthetic dataset renderer.

The renderer's job is to produce frames a phone *would* have captured, together with the rotation
each was captured at. Everything downstream in Phase 2 is measured against those rotations, so a
renderer that is subtly wrong does not produce a visible defect — it produces a harness that
certifies the wrong answer.

Two of the cases below carry most of the weight.

`test_distortion_terms_are_opencvs_in_opencvs_order` pins this implementation to the same
hand-computed decimals as `Project.TheDistortionTermsAreOpenCVsInOpenCVsOrder` in the C++ suite.
That matters more here than an ordinary agreement test would: this module deliberately re-implements
the lens rather than calling the core, because a dataset rendered *through* the code under test
would hide any error the two share. Two independent implementations pinned to the same
independently-derived numbers is what makes that independence worth having.

`test_every_rendered_pixel_lands_where_its_own_ray_points` renders from a panorama whose colour
encodes the direction it represents, so each rendered pixel carries its own ground truth and the
whole path — unprojection, rotation, equirectangular lookup, sampling — is checkable arithmetic
rather than something to look at.
"""
from __future__ import annotations

import json
import math
import sys
import tempfile
import unittest
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from synth_dataset import (  # noqa: E402
    Intrinsics,
    Pose,
    direction_to_equirect,
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

    Each channel holds one component of the unit direction, mapped from [-1, 1] onto [0, 1]. Decode
    a rendered pixel and you recover the world direction the camera was looking along when it drew
    it, to within the sampling error — which turns "does this render correctly" into arithmetic.
    """
    v, u = np.meshgrid(np.arange(height), np.arange(width), indexing="ij")
    longitude = (u + 0.5) / width * 2.0 * math.pi - math.pi
    latitude = math.pi / 2.0 - (v + 0.5) / height * math.pi
    x = np.cos(latitude) * np.sin(longitude)
    y = np.sin(latitude)
    z = -np.cos(latitude) * np.cos(longitude)
    return np.stack([x, y, z], axis=-1)


def decode(pixels: np.ndarray) -> np.ndarray:
    return pixels


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


class SeamSampling(unittest.TestCase):
    """The one column where wrapping and clamping differ, tested where the behaviour lives.

    A render that crosses the seam does *not* reliably reach it: the interpolation only differs on
    the single column whose right-hand neighbour must wrap to column zero, and a 64x48 frame lands
    on that exact column only by luck. Replacing the wrap with a clamp left twenty-two tests green
    including one written specifically to look through the seam — so this asserts on the sampler
    directly, which is the unit that actually has the behaviour.
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

    def test_latitude_clamps_where_longitude_wraps(self):
        # The poles are not periodic — a ray past the top edge has to hold the top row rather than
        # reappear at the bottom, which is the opposite rule from the one above and one line away
        # from it in the implementation.
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
    refusal entirely left all nineteen tests green. ADR 0046 makes refusal the rule for the core and
    a dataset renderer that quietly invented directions instead would put fabricated geometry into
    the ground truth everything downstream is measured against.
    """

    def test_a_folding_lens_refuses_the_pixels_past_its_fold(self):
        lens = lens_from_fov(66.0, 50.0, 320, 240)
        lens = Intrinsics(**{**lens.__dict__, "k1": -0.9, "k2": 0.6, "k3": -0.4})

        us, vs = np.meshgrid(np.linspace(0.5, 319.5, 64), np.linspace(0.5, 239.5, 48),
                             indexing="ij")
        pixels = np.stack([us.ravel(), vs.ravel()], axis=-1)
        _, valid = unproject(lens, pixels)

        self.assertFalse(valid.all(), "a lens this strong should fold somewhere in frame")
        self.assertTrue(valid.any(), "and it should still answer near the optical centre")

        # Whatever it does answer has to be right, not merely present.
        directions, valid = unproject(lens, pixels)
        back_u, back_v, _ = project(lens, directions[valid])
        np.testing.assert_allclose(back_u, pixels[valid][:, 0], atol=1e-6)
        np.testing.assert_allclose(back_v, pixels[valid][:, 1], atol=1e-6)

    def test_a_refused_pixel_is_left_black_rather_than_guessed(self):
        panorama = direction_encoded_panorama(512, 256)
        lens = lens_from_fov(66.0, 50.0, 96, 72)
        lens = Intrinsics(**{**lens.__dict__, "k1": -0.9, "k2": 0.6, "k3": -0.4})

        frame = render_frame(panorama, lens, Pose.identity())

        us, vs = np.meshgrid(np.arange(lens.width) + 0.5, np.arange(lens.height) + 0.5,
                             indexing="xy")
        pixels = np.stack([us.ravel(), vs.ravel()], axis=-1)
        _, valid = unproject(lens, pixels)
        self.assertFalse(valid.all())
        np.testing.assert_array_equal(frame.reshape(-1, 3)[~valid], 0.0)


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

        rendered = decode(frame.reshape(-1, 3))

        # Asserted in degrees, because degrees are what this dataset exists to measure and a
        # tolerance in colour units hides its own meaning. The measured interpolation error at this
        # panorama size is 0.00003 degrees, so the bound below has thirty times the headroom it
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
        # it: clamping the seam instead of wrapping left all nineteen tests green, because every
        # other render looks forward and never crosses longitude ±180.
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
