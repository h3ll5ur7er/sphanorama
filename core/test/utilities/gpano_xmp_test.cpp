// What makes an exported JPEG open as a photo sphere rather than a wide photograph: Google's GPano
// XMP properties, in an APP1 segment a viewer finds before the image data (ADR 0087).
#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "utilities/gpano_xmp.h"

namespace sphanorama {
namespace {

GPanoArea Sphere(int32_t width) {
  GPanoArea area;
  area.fullWidth = width;
  area.fullHeight = width / 2;
  area.croppedWidth = width;
  area.croppedHeight = width / 2;
  return area;
}

void ExpectAttribute(const std::string& packet, const std::string& name, const std::string& value) {
  const std::string written = "GPano:" + name + "=\"" + value + "\"";
  EXPECT_NE(packet.find(written), std::string::npos) << written << " in\n" << packet;
}

TEST(GPanoXmp, APacketSaysWhereTheImageSitsInAnEquirectangularSphere) {
  GPanoArea area = Sphere(4096);
  area.croppedWidth = 4096;
  area.croppedHeight = 1500;
  area.croppedTop = 300;
  const std::optional<std::string> packet = GPanoPacket(area);
  ASSERT_TRUE(packet.has_value());
  ExpectAttribute(*packet, "UsePanoramaViewer", "True");
  ExpectAttribute(*packet, "ProjectionType", "equirectangular");
  ExpectAttribute(*packet, "FullPanoWidthPixels", "4096");
  ExpectAttribute(*packet, "FullPanoHeightPixels", "2048");
  ExpectAttribute(*packet, "CroppedAreaImageWidthPixels", "4096");
  ExpectAttribute(*packet, "CroppedAreaImageHeightPixels", "1500");
  ExpectAttribute(*packet, "CroppedAreaLeftPixels", "0");
  ExpectAttribute(*packet, "CroppedAreaTopPixels", "300");
  EXPECT_NE(packet->find("xmlns:GPano=\"http://ns.google.com/photos/1.0/panorama/\""),
            std::string::npos);
  // A packet wrapper a reader can find, and one it may rewrite in place.
  EXPECT_EQ(packet->rfind("<?xpacket begin=", 0), 0u);
  EXPECT_NE(packet->find("<?xpacket end=\"w\"?>"), std::string::npos);
}

// An area that is not part of a sphere is not one a viewer could place: nothing rather than a
// packet that would draw the image somewhere it is not.
TEST(GPanoXmp, AnAreaThatIsNotPartOfASphereHasNoPacket) {
  EXPECT_TRUE(GPanoPacket(Sphere(2048)).has_value());
  GPanoArea none = Sphere(2048);
  none.croppedWidth = 0;
  EXPECT_FALSE(GPanoPacket(none).has_value());
  GPanoArea flat = Sphere(2048);
  flat.fullHeight = 2048;
  EXPECT_FALSE(GPanoPacket(flat).has_value()) << "a full sphere is twice as wide as it is tall";
  GPanoArea odd = Sphere(2048);
  odd.fullWidth = 2049;
  EXPECT_FALSE(GPanoPacket(odd).has_value());
  GPanoArea past = Sphere(2048);
  past.croppedTop = 1;
  EXPECT_FALSE(GPanoPacket(past).has_value()) << "cropped past the bottom of the sphere";
  GPanoArea wide = Sphere(2048);
  wide.croppedLeft = 1;
  EXPECT_FALSE(GPanoPacket(wide).has_value()) << "cropped past its right edge";
  GPanoArea negative = Sphere(2048);
  negative.croppedLeft = -1;
  negative.croppedWidth = 2047;
  EXPECT_FALSE(GPanoPacket(negative).has_value());
}

std::vector<uint8_t> Bytes(std::initializer_list<int> values) {
  std::vector<uint8_t> out;
  for (int v : values) out.push_back(static_cast<uint8_t>(v));
  return out;
}

// SOI, a JFIF APP0 of sixteen bytes, then the start of the rest.
const std::vector<uint8_t> kJfif = Bytes({0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10, 'J', 'F', 'I', 'F', 0,
                                          1, 1, 0, 0, 1, 0, 1, 0, 0, 0xFF, 0xDB, 0x00, 0x02, 0xFF,
                                          0xD9});

constexpr std::string_view kXmpNamespace = "http://ns.adobe.com/xap/1.0/";

// The segment that should sit at `at`, carrying `packet`.
void ExpectApp1(const std::vector<uint8_t>& out, size_t at, std::string_view packet) {
  ASSERT_GE(out.size(), at + 4);
  EXPECT_EQ(out[at], 0xFF);
  EXPECT_EQ(out[at + 1], 0xE1);
  const size_t length = (static_cast<size_t>(out[at + 2]) << 8) | out[at + 3];
  EXPECT_EQ(length, 2 + kXmpNamespace.size() + 1 + packet.size());
  ASSERT_GE(out.size(), at + 2 + length);
  const std::string body(out.begin() + static_cast<std::ptrdiff_t>(at + 4),
                         out.begin() + static_cast<std::ptrdiff_t>(at + 2 + length));
  EXPECT_EQ(body, std::string(kXmpNamespace) + '\0' + std::string(packet));
}

TEST(GPanoXmp, ThePacketGoesAfterTheJfifSegmentAndNothingElseMoves) {
  const std::string packet = "<x:xmpmeta/>";
  auto out = WithXmpPacket(kJfif, packet);
  ASSERT_TRUE(out.ok()) << out.status.detail;
  const size_t app0End = 4 + 0x10;
  EXPECT_TRUE(std::equal(kJfif.begin(), kJfif.begin() + app0End, out.value.begin()));
  ExpectApp1(out.value, app0End, packet);
  const size_t inserted = 2 + 2 + kXmpNamespace.size() + 1 + packet.size();
  ASSERT_EQ(out.value.size(), kJfif.size() + inserted);
  EXPECT_TRUE(std::equal(kJfif.begin() + app0End, kJfif.end(),
                         out.value.begin() + static_cast<std::ptrdiff_t>(app0End + inserted)));
}

TEST(GPanoXmp, WithoutAJfifSegmentThePacketFollowsTheStartOfImage) {
  const std::vector<uint8_t> bare = Bytes({0xFF, 0xD8, 0xFF, 0xDB, 0x00, 0x02, 0xFF, 0xD9});
  auto out = WithXmpPacket(bare, "p");
  ASSERT_TRUE(out.ok()) << out.status.detail;
  ExpectApp1(out.value, 2, "p");
  EXPECT_EQ(out.value.size(), bare.size() + 2 + 2 + kXmpNamespace.size() + 1 + 1);
}

TEST(GPanoXmp, WhatIsNotAJpegIsRefused) {
  EXPECT_EQ(WithXmpPacket({}, "p").status.code, StatusCode::InvalidArgument);
  EXPECT_EQ(WithXmpPacket(Bytes({0x89, 'P', 'N', 'G'}), "p").status.code,
            StatusCode::InvalidArgument);
  // A JFIF segment whose length runs past the end is a torn file, not one to write into.
  const std::vector<uint8_t> torn = Bytes({0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x40, 'J', 'F'});
  EXPECT_EQ(WithXmpPacket(torn, "p").status.code, StatusCode::InvalidArgument);
  const std::vector<uint8_t> shortLength = Bytes({0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x01, 0xFF, 0xD9});
  EXPECT_EQ(WithXmpPacket(shortLength, "p").status.code, StatusCode::InvalidArgument);
}

// A segment's length is sixteen bits and counts itself, so the packet has a ceiling, and one at it
// fits: the bound is exact rather than a margin.
TEST(GPanoXmp, APacketPastWhatOneSegmentHoldsIsRefused) {
  const size_t most = 0xFFFF - 2 - kXmpNamespace.size() - 1;
  auto fits = WithXmpPacket(kJfif, std::string(most, 'x'));
  ASSERT_TRUE(fits.ok()) << fits.status.detail;
  ExpectApp1(fits.value, 4 + 0x10, std::string(most, 'x'));
  EXPECT_EQ(WithXmpPacket(kJfif, std::string(most + 1, 'x')).status.code,
            StatusCode::InvalidArgument);
}

}  // namespace
}  // namespace sphanorama
