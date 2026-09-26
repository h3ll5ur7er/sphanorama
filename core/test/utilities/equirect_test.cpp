// Where a panorama's pixel looks. The convention is `tools/synth_dataset.py`'s, and it is pinned
// here to decimals worked by hand rather than to that file: a compositor and a renderer that shared
// a wrong convention would round-trip a dataset perfectly (ADR 0050). `test_synth_dataset.py`
// asserts the same decimals from the other side.
#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <optional>

#include "utilities/equirect.h"

namespace sphanorama {
namespace {

void ExpectDirection(const std::optional<Vec3>& got, double x, double y, double z) {
  ASSERT_TRUE(got.has_value());
  EXPECT_NEAR(got->x, x, 1e-9);
  EXPECT_NEAR(got->y, y, 1e-9);
  EXPECT_NEAR(got->z, z, 1e-9);
}

TEST(Equirect, TheCentreIsForward) {
  ExpectDirection(EquirectDirection(Pixel{256.0, 128.0}, 512, 256), 0.0, 0.0, -1.0);
}

TEST(Equirect, TheTopEdgeIsUpAndTheBottomDown) {
  ExpectDirection(EquirectDirection(Pixel{100.0, 0.0}, 512, 256), 0.0, 1.0, 0.0);
  ExpectDirection(EquirectDirection(Pixel{100.0, 256.0}, 512, 256), 0.0, -1.0, 0.0);
}

// Longitude increases toward +X: three quarters of the way across is a quarter turn right.
TEST(Equirect, AQuarterOfTheWidthIsAQuarterTurnTowardPlusX) {
  ExpectDirection(EquirectDirection(Pixel{384.0, 128.0}, 512, 256), 1.0, 0.0, 0.0);
  ExpectDirection(EquirectDirection(Pixel{128.0, 128.0}, 512, 256), -1.0, 0.0, 0.0);
}

// Off every axis, so a swapped sign or a swapped sine and cosine shows. The centre of pixel (6, 1)
// of an 8 x 4 panorama is at longitude (6.5 / 8 - 0.5) * 360 = 112.5 and latitude
// (0.5 - 1.5 / 4) * 180 = 22.5 degrees, which is
//   x =  cos 22.5 * sin 112.5 = 0.9238795325 * 0.9238795325   = 0.8535533906
//   y =  sin 22.5                                             = 0.3826834324
//   z = -cos 22.5 * cos 112.5 = -0.9238795325 * -0.3826834324 = 0.3535533906
TEST(Equirect, APixelOffEveryAxisIsWhereTheArithmeticPutsIt) {
  ExpectDirection(EquirectDirection(Pixel{6.5, 1.5}, 8, 4), 0.8535533906, 0.3826834324,
                  0.3535533906);
}

TEST(Equirect, TheSeamIsTheSameDirectionFromBothSides) {
  const std::optional<Vec3> left = EquirectDirection(Pixel{0.0, 64.0}, 512, 256);
  const std::optional<Vec3> right = EquirectDirection(Pixel{512.0, 64.0}, 512, 256);
  ASSERT_TRUE(left.has_value() && right.has_value());
  EXPECT_NEAR(left->x, right->x, 1e-12);
  EXPECT_NEAR(left->y, right->y, 1e-12);
  EXPECT_NEAR(left->z, right->z, 1e-12);
  EXPECT_NEAR(left->z, 0.7071067812, 1e-9) << "the seam is behind the viewer";
}

TEST(Equirect, AnswersAreUnit) {
  for (double x = 0.25; x < 16.0; x += 1.5) {
    for (double y = 0.25; y < 8.0; y += 1.25) {
      const std::optional<Vec3> d = EquirectDirection(Pixel{x, y}, 16, 8);
      ASSERT_TRUE(d.has_value());
      EXPECT_NEAR(std::hypot(d->x, d->y, d->z), 1.0, 1e-12) << x << " " << y;
    }
  }
}

// Nothing is answered for a panorama with no area or a coordinate that is not a number; a row
// above the top or below the bottom is off the sphere, not a pole.
TEST(Equirect, NoDirectionIsInventedWhereThereIsNone) {
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const double inf = std::numeric_limits<double>::infinity();
  EXPECT_FALSE(EquirectDirection(Pixel{1.0, 1.0}, 0, 4).has_value());
  EXPECT_FALSE(EquirectDirection(Pixel{1.0, 1.0}, 8, 0).has_value());
  EXPECT_FALSE(EquirectDirection(Pixel{1.0, 1.0}, -8, 4).has_value());
  EXPECT_FALSE(EquirectDirection(Pixel{nan, 1.0}, 8, 4).has_value());
  EXPECT_FALSE(EquirectDirection(Pixel{1.0, inf}, 8, 4).has_value());
  EXPECT_FALSE(EquirectDirection(Pixel{1.0, -0.5}, 8, 4).has_value());
  EXPECT_FALSE(EquirectDirection(Pixel{1.0, 4.5}, 8, 4).has_value());
}

}  // namespace
}  // namespace sphanorama
