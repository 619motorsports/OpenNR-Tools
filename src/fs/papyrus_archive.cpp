#include "fs/papyrus_archive.h"

#include <cstring>
#include <stdexcept>

namespace opennr::papyrus {

// --- ClassRegistry ---------------------------------------------------------

ClassRegistry& ClassRegistry::instance() {
    static ClassRegistry r;
    return r;
}

void ClassRegistry::register_class(std::string_view name, ObjectFactory ctor) {
    entries_[std::string(name)] = ctor;
}

std::shared_ptr<PersistentObject>
ClassRegistry::instantiate(std::string_view name) const {
    auto it = entries_.find(std::string(name));
    if (it == entries_.end()) return nullptr;
    return it->second();
}

bool ClassRegistry::contains(std::string_view name) const {
    return entries_.find(std::string(name)) != entries_.end();
}

void ClassRegistry::clear_for_test() {
    entries_.clear();
}

// --- Archive ---------------------------------------------------------------

Archive::Archive(std::span<const std::uint8_t> bytes) : reader_(bytes) {}

std::uint8_t  Archive::read_u8()  { return reader_.read_u8(); }
std::uint16_t Archive::read_u16() { return reader_.read_u16_le(); }
std::uint32_t Archive::read_u32() { return reader_.read_u32_le(); }
std::int32_t  Archive::read_i32() { return reader_.read_i32_le(); }
float         Archive::read_f32() { return reader_.read_f32_le(); }

double Archive::read_f64() {
    auto bytes = reader_.read_bytes(8);
    double v;
    std::memcpy(&v, bytes.data(), 8);  // host is LE on every platform we ship.
    return v;
}

void Archive::read_bytes(void* dest, std::size_t n) {
    auto src = reader_.read_bytes(n);
    std::memcpy(dest, src.data(), n);
}

std::string Archive::read_lp_string() {
    auto len_with_nul = reader_.read_u32_le();
    if (len_with_nul == 0) return {};
    auto bytes = reader_.read_bytes(len_with_nul);
    // Drop trailing NUL but tolerate a stray non-NUL last byte (we've
    // seen this once or twice in shipped files; keeping permissive
    // matches the runtime behaviour of strncpy-style consumers).
    std::size_t n = len_with_nul;
    if (bytes[n - 1] == 0) --n;
    return std::string(reinterpret_cast<const char*>(bytes.data()), n);
}

std::string_view Archive::resolve_class(std::uint32_t class_handle) {
    if (class_handle == 0) {
        throw std::runtime_error("papyrus::Archive: class_handle 0 is invalid");
    }
    auto it = class_intern_.find(class_handle);
    if (it != class_intern_.end()) return it->second;
    // Fresh class — read the inline class name and intern it.  rts.dll
    // does this via a red-black tree lookup that returns the sentinel
    // when the key isn't found; we do the same with a plain hash map.
    std::string name = read_lp_string();
    auto [ins, _] = class_intern_.emplace(class_handle, std::move(name));
    return ins->second;
}

Archive::ObjectHeader Archive::read_object_header() {
    ObjectHeader h;
    h.obj_handle = reader_.read_u32_le();

    if (h.obj_handle == 0) {
        h.is_null = true;
        return h;
    }
    auto existing = object_intern_.find(h.obj_handle);
    if (existing != object_intern_.end()) {
        // Back-reference to an already-deserialized object.
        h.is_back_ref = true;
        h.class_name = std::string(existing->second->class_name());
        return h;
    }

    auto class_handle = reader_.read_u32_le();
    h.class_name = std::string(resolve_class(class_handle));
    return h;
}

std::shared_ptr<PersistentObject> Archive::read_object() {
    auto h = read_object_header();
    if (h.is_null) return nullptr;
    if (h.is_back_ref) return object_intern_[h.obj_handle];

    auto obj = ClassRegistry::instance().instantiate(h.class_name);
    if (!obj) {
        throw std::runtime_error(
            "papyrus::Archive: no class registered for '" + h.class_name +
            "' (object body would desynchronize without a reader)");
    }
    // Record BEFORE calling read() so self-referential graphs work
    // (the object can reach itself through Archive::read_object during
    // its own body deserialization).
    object_intern_.emplace(h.obj_handle, obj);
    obj->read(*this);
    return obj;
}

}  // namespace opennr::papyrus
