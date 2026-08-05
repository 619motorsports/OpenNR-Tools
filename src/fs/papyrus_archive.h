#pragma once

// Clean-room reimplementation of Papyrus rts.dll's typed-object stream
// (`Archive` + `PersistentObject` + `PersistentLoadingInfo`).  Read side
// only for now — write is a future addition.
//
// On-disk per-object layout (confirmed against rts.dll
// Archive::transferPersistentObject @ 0x10002000):
//
//   u32  obj_handle                     // 0 = null reference
//   [if obj_handle was previously seen]:
//       (back-reference; no further bytes for this object)
//   [else]:
//       u32  class_handle
//       [if class_handle was previously seen]:
//           (className from intern table)
//       [else]:
//           u32  name_length            // includes trailing NUL
//           char className[name_length]
//       <body — class-specific, via PersistentObject::read()>
//
// Handles are u32s — rts.dll uses a single monotonic counter for both
// obj_handles and class_handles on the WRITE side, so a stock-built
// file's handles go 1, 2, 3, ... in increasing order.  The reader
// doesn't enforce this: each handle is looked up by value in its own
// intern table, and absence means "this is the new definition".
//
// The class-name lookup is keyed on string equality, mirroring
// rts.dll's PersistentLoadingInfo::instantiatePersistentObj.

#include "core/byte_reader.h"

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace opennr::papyrus {

class Archive;

// Base class for every typed-stream object.  Each subclass implements
// `read()` to consume its body bytes from the archive.
class PersistentObject {
public:
    virtual ~PersistentObject() = default;
    virtual std::string_view class_name() const = 0;
    virtual void read(Archive& ar) = 0;
};

// Default factory signature: instantiate an empty object.  The Archive
// fills it in by calling `read()` afterwards.
using ObjectFactory = std::shared_ptr<PersistentObject>(*)();

// Process-global class registry.  Mirrors rts.dll's
// `PersistentLoadingInfo` singly-linked list, but keyed by string.
class ClassRegistry {
public:
    static ClassRegistry& instance();

    // Register a class.  Last registration wins; idempotent for same ctor.
    void register_class(std::string_view name, ObjectFactory ctor);

    // Returns nullptr when no class is registered under that name.
    std::shared_ptr<PersistentObject> instantiate(std::string_view name) const;

    bool contains(std::string_view name) const;

    // Test/RAII helper: snapshot, clear, restore.
    void clear_for_test();
    std::size_t size() const { return entries_.size(); }

private:
    ClassRegistry() = default;
    std::unordered_map<std::string, ObjectFactory> entries_;
};

// Reader-only Archive over an in-memory byte buffer.
class Archive {
public:
    explicit Archive(std::span<const std::uint8_t> bytes);

    // Primitive reads (correspond to rts.dll Archive::transferBytes).
    std::uint8_t  read_u8();
    std::uint16_t read_u16();
    std::uint32_t read_u32();
    std::int32_t  read_i32();
    float         read_f32();
    double        read_f64();
    void          read_bytes(void* dest, std::size_t n);

    // u32 length-prefixed string. Length includes the trailing NUL byte.
    // Returns the bytes WITHOUT the NUL.  Mirrors transferString /
    // transferPointer behavior on the read side.
    std::string read_lp_string();

    // Read one PersistentObject from the stream.  Returns:
    //   - nullptr if the next u32 is 0 (encoded null reference)
    //   - a back-referenced object when its obj_handle has been seen
    //   - a newly-instantiated object after dispatching to its read()
    //
    // Unknown classes (no registry entry) are an explicit error here
    // because skipping a body of unknown length would desynchronize
    // the stream — callers should register every class they expect.
    std::shared_ptr<PersistentObject> read_object();

    // Read the obj_handle + class header WITHOUT dispatching to read().
    // Returns the resolved class name and whether this is a back-ref.
    // Useful for tooling that wants to enumerate objects without owning
    // all the class implementations.
    struct ObjectHeader {
        std::uint32_t obj_handle = 0;
        std::string   class_name;
        bool          is_null = false;     // obj_handle == 0
        bool          is_back_ref = false; // obj_handle previously seen
    };
    ObjectHeader read_object_header();

    // Inspection / cursor.
    std::size_t position() const { return reader_.position(); }
    std::size_t remaining() const { return reader_.remaining(); }

    // Intern tables, keyed by handle value (handles are not necessarily
    // dense, though in stock files they are).  Useful for tests and
    // diagnostic dumps of a parse in flight.
    const std::unordered_map<std::uint32_t, std::string>& class_intern() const {
        return class_intern_;
    }
    const std::unordered_map<std::uint32_t, std::shared_ptr<PersistentObject>>&
    object_intern() const { return object_intern_; }

private:
    ByteReader reader_;
    std::unordered_map<std::uint32_t, std::string> class_intern_;
    std::unordered_map<std::uint32_t, std::shared_ptr<PersistentObject>> object_intern_;

    // Resolve a class_handle to a class name, reading and interning the
    // inline class name when this handle is fresh.
    std::string_view resolve_class(std::uint32_t class_handle);
};

}  // namespace opennr::papyrus

// Convenience macro: register a class with the global registry at
// translation-unit static-init time.  Type must expose
//   - static constexpr const char* kClassName
//   - a public default constructor
//   - virtual class_name() and read() (inherited from PersistentObject)
#define OPENNR_PAPYRUS_REGISTER(Type)                                          \
    namespace {                                                                \
    const bool _opennr_papy_reg_##Type =                                       \
        (::opennr::papyrus::ClassRegistry::instance().register_class(          \
             Type::kClassName,                                                 \
             []() -> std::shared_ptr<::opennr::papyrus::PersistentObject> {    \
                 return std::make_shared<Type>();                              \
             }),                                                               \
         true);                                                                \
    }
