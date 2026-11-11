#include "utilities/gpano_xmp.h"

#include <cstddef>
#include <cstdint>

namespace sphanorama {
namespace {

constexpr const char* kComponent = "GPanoXmp";

// The namespace an XMP APP1 segment opens with, NUL-terminated on the wire, which is how a reader
// tells it from the Exif segment that shares the marker.
constexpr std::string_view kXmpNamespace = "http://ns.adobe.com/xap/1.0/";

// A segment's length is sixteen bits and counts its own two bytes.
constexpr size_t kMostSegmentLength = 0xFFFF;

bool Within(int64_t start, int64_t length, int64_t whole) {
  return start >= 0 && length > 0 && start + length <= whole;
}

std::string Attribute(const char* name, const std::string& value) {
  return std::string("\n    GPano:") + name + "=\"" + value + "\"";
}

}  // namespace

std::optional<std::string> GPanoPacket(const GPanoArea& area) {
  // Twice as wide as it is tall is what an equirectangular sphere is: 360 degrees by 180. One with
  // no height cannot be cropped from, so the bounds below refuse it.
  if (int64_t{area.fullWidth} != 2 * int64_t{area.fullHeight}) return std::nullopt;
  if (!Within(area.croppedLeft, area.croppedWidth, area.fullWidth)
      || !Within(area.croppedTop, area.croppedHeight, area.fullHeight)) {
    return std::nullopt;
  }
  std::string packet =
      "<?xpacket begin=\"\xEF\xBB\xBF\" id=\"W5M0MpCehiHzreSzNTczkc9d\"?>\n"
      "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\">\n"
      " <rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">\n"
      "  <rdf:Description rdf:about=\"\"\n"
      "    xmlns:GPano=\"http://ns.google.com/photos/1.0/panorama/\"";
  packet += Attribute("UsePanoramaViewer", "True");
  packet += Attribute("ProjectionType", "equirectangular");
  packet += Attribute("FullPanoWidthPixels", std::to_string(area.fullWidth));
  packet += Attribute("FullPanoHeightPixels", std::to_string(area.fullHeight));
  packet += Attribute("CroppedAreaImageWidthPixels", std::to_string(area.croppedWidth));
  packet += Attribute("CroppedAreaImageHeightPixels", std::to_string(area.croppedHeight));
  packet += Attribute("CroppedAreaLeftPixels", std::to_string(area.croppedLeft));
  packet += Attribute("CroppedAreaTopPixels", std::to_string(area.croppedTop));
  packet +=
      "/>\n"
      " </rdf:RDF>\n"
      "</x:xmpmeta>\n"
      "<?xpacket end=\"w\"?>";
  return packet;
}

Result<std::vector<uint8_t>> WithXmpPacket(std::span<const uint8_t> jpeg, std::string_view packet) {
  if (jpeg.size() < 2 || jpeg[0] != 0xFF || jpeg[1] != 0xD8) {
    return Err<std::vector<uint8_t>>(StatusCode::InvalidArgument, kComponent,
                                     "these bytes do not start as a JPEG does");
  }
  // After the JFIF segment, which readers expect first when there is one.
  size_t at = 2;
  if (jpeg.size() >= 4 && jpeg[2] == 0xFF && jpeg[3] == 0xE0) {
    if (jpeg.size() < 6) {
      return Err<std::vector<uint8_t>>(StatusCode::InvalidArgument, kComponent,
                                       "the JPEG's JFIF segment is cut off");
    }
    const size_t length = (static_cast<size_t>(jpeg[4]) << 8) | jpeg[5];
    if (length < 2 || 4 + length > jpeg.size()) {
      return Err<std::vector<uint8_t>>(StatusCode::InvalidArgument, kComponent,
                                       "the JPEG's JFIF segment runs past its end");
    }
    at = 4 + length;
  }
  const size_t length = 2 + kXmpNamespace.size() + 1 + packet.size();
  if (packet.size() > kMostSegmentLength || length > kMostSegmentLength) {
    return Err<std::vector<uint8_t>>(StatusCode::InvalidArgument, kComponent,
                                     "the XMP packet is longer than one segment can carry");
  }
  std::vector<uint8_t> out;
  out.reserve(jpeg.size() + 2 + length);
  out.insert(out.end(), jpeg.begin(), jpeg.begin() + static_cast<std::ptrdiff_t>(at));
  out.push_back(0xFF);
  out.push_back(0xE1);
  out.push_back(static_cast<uint8_t>(length >> 8));
  out.push_back(static_cast<uint8_t>(length & 0xFF));
  out.insert(out.end(), kXmpNamespace.begin(), kXmpNamespace.end());
  out.push_back(0);
  out.insert(out.end(), packet.begin(), packet.end());
  out.insert(out.end(), jpeg.begin() + static_cast<std::ptrdiff_t>(at), jpeg.end());
  return Ok(std::move(out));
}

}  // namespace sphanorama
