// Export is the one place bytes leave the device, so its fake records rather than discards.
#include <gtest/gtest.h>

#include <memory>

#include "support/fake_export_access.h"
#include "support/fake_image_codec_access.h"

namespace sphanorama {
namespace {

TEST(FakeExport, RecordsWhatWasSaved) {
  FakeExportAccess exporter;
  const std::vector<uint8_t> bytes{1, 2, 3};
  ASSERT_TRUE(exporter.Save("sphere.jpg", "image/jpeg", bytes).ok());
  ASSERT_EQ(exporter.artifacts().size(), 1u);
  EXPECT_EQ(exporter.artifacts()[0].filename, "sphere.jpg");
  EXPECT_EQ(exporter.artifacts()[0].bytes, bytes);
  EXPECT_FALSE(exporter.artifacts()[0].shared);
}

TEST(FakeExport, DistinguishesSharingFromSaving) {
  FakeExportAccess exporter;
  ASSERT_TRUE(exporter.Share("sphere.jpg", "image/jpeg", std::vector<uint8_t>{9}).ok());
  EXPECT_TRUE(exporter.artifacts()[0].shared);
}

TEST(FakeExport, ReportsWhenThereIsNoShareTarget) {
  // Desktop browsers and older iOS have no share sheet; the manager has to fall back to a
  // download rather than surfacing a failure to the user.
  FakeExportAccess exporter(/*canShare=*/false);
  EXPECT_FALSE(exporter.CanShare().value);
  EXPECT_EQ(exporter.Share("s.jpg", "image/jpeg", std::vector<uint8_t>{1}).code,
            StatusCode::Unsupported);
}

TEST(FakeExport, RejectsAnEmptyFilename) {
  FakeExportAccess exporter;
  EXPECT_EQ(exporter.Save("", "image/jpeg", std::vector<uint8_t>{1}).code,
            StatusCode::InvalidArgument);
}

TEST(FakeCodec, RoundTripsPixelsThroughEncodeAndDecode) {
  auto store = std::make_shared<FakeFrameStoreAccess>();
  FakeImageCodecAccess codec(store);

  auto frame = store->Allocate(4, 1, PixelFormat::RGBA8);
  ASSERT_TRUE(frame.ok());
  auto pinned = store->Pin(frame.value);
  ASSERT_TRUE(pinned.ok());
  std::fill(pinned.value.begin(), pinned.value.end(), 0x7E);
  ASSERT_TRUE(store->Release(frame.value).ok());

  auto encoded = codec.Encode(frame.value, EncodeSpec{});
  ASSERT_TRUE(encoded.ok());
  auto decoded = codec.Decode(encoded.value);
  ASSERT_TRUE(decoded.ok());
  EXPECT_EQ(store->ContentHash(decoded.value).value, store->ContentHash(frame.value).value);
}

TEST(FakeCodec, RemembersWhetherGPanoMetadataWasRequested) {
  // Without the GPano block an export is a wide photo, not a sphere — the difference between
  // the product working and not, and invisible in the pixels.
  auto store = std::make_shared<FakeFrameStoreAccess>();
  FakeImageCodecAccess codec(store);
  auto frame = store->Allocate(4, 1, PixelFormat::RGBA8);
  ASSERT_TRUE(frame.ok());

  EncodeSpec spec;
  spec.attachGPanoXmp = true;
  spec.format = EncodeFormat::Avif;
  ASSERT_TRUE(codec.Encode(frame.value, spec).ok());
  EXPECT_TRUE(codec.LastEncodeCarriedGPano());
  EXPECT_EQ(codec.LastFormat(), EncodeFormat::Avif);
}

TEST(FakeCodec, RefusesAPayloadThatIsNotAWholeNumberOfPixels) {
  // The magic matches, so the length is the only thing standing between the copy and the end of
  // the buffer: floor(size/4) pixels are allocated and size bytes are written into them. ASan
  // catches it the moment a test tries, which is exactly what a fake is for.
  auto store = std::make_shared<FakeFrameStoreAccess>();
  FakeImageCodecAccess codec(store);
  auto frame = store->Allocate(4, 1, PixelFormat::RGBA8);
  ASSERT_TRUE(frame.ok());
  auto encoded = codec.Encode(frame.value, EncodeSpec{});
  ASSERT_TRUE(encoded.ok());

  std::vector<uint8_t> ragged = encoded.value;
  ragged.push_back(0xFF);   // one byte more than a whole pixel
  EXPECT_EQ(codec.Decode(ragged).status.code, StatusCode::CodecFailure);
}

TEST(FakeCodec, RefusesBytesItDidNotProduce) {
  FakeImageCodecAccess codec;
  const std::vector<uint8_t> junk{0, 1, 2, 3, 4};
  EXPECT_EQ(codec.Decode(junk).status.code, StatusCode::CodecFailure);
}

}  // namespace
}  // namespace sphanorama
