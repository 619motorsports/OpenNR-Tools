#pragma once

// Walker for the Papyrus typed-stream object format used by .3do (3D
// objects) and .ptf (track layouts).  See docs/formats/3do_object_stream.md
// for the full spec.
//
// The walker scans the byte stream for class-name strings and reports
// each one with its offset and length.  It does NOT yet decode each
// class's body field-by-field - that's a per-class job (see the
// "Strategy for full geometry decoding" section in the format doc).

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace opennr {

struct ObjectToken {
    std::size_t   offset;        // file offset of the u32 length prefix
    std::uint32_t length;        // value of the u32 (string length INCLUDING NUL)
    std::string   name;          // decoded ASCII (without trailing NUL)
    bool          is_class;      // matches one of the known *Descriptor names
};

struct ObjectStream {
    std::uint32_t            stream_version_a = 0; // observed = 1
    std::uint32_t            stream_version_b = 0; // observed = 2
    std::vector<ObjectToken> tokens;

    // Convenience accessors on the parsed token list.
    std::vector<std::string> texture_refs() const;        // *.mip referenced
    std::vector<std::string> object_refs()  const;        // *.3do referenced
    std::vector<std::string> classes_in_order() const;

    // Returns true if this stream's first class is a known root
    // (TrackDescriptor / LodSwitchDescriptor / GroupDescriptor / Empty).
    bool has_known_root() const;

    static ObjectStream parse(std::span<const std::uint8_t> bytes);
};

// True if `name` matches one of the recognized *Descriptor classes.
bool is_known_object_class(std::string_view name);

}  // namespace opennr
