#pragma once

#include "rpy_replay.h"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <optional>
#include <vector>

namespace opennr {

enum class RpyEditorEventType : std::uint16_t {
    Stamp = 19, Sound, Text, Fade, Camera, Car, Playback, Marker, Toggle, Volume
};

struct RpyEditorOperation {
    RpyEditorEventType type{};
    std::uint32_t frame_block = 0;
    std::uint32_t event_index = 0;
    std::string summary;
};

// Camera/focus state produced by replay-editor Camera (type 23) and Car
// (type 24) events. Camera is the native drivingView ID written by the stock
// tape (not the nine-name UI selector ordinal); car is a zero-based slot.
struct RpyEditorCameraState {
    std::uint8_t camera = 1;
    std::uint8_t car = 0;
};

// User-facing fields encoded by the stock replay-editor dialogs.  Several
// on-disk records also retain the value that was active immediately before
// the operation so playback can undo/seek through it deterministically.
struct RpyTextPayload {
    float fade_time = 0.0f;
    float lifespan = 0.0f;
    std::uint16_t x = 0;
    std::uint16_t y = 0;
    std::uint16_t max_pixel_width = 300;
    bool enabled = false;
    std::u16string text;
};
struct RpyStampPayload {
    float fade_time = 0.0f;
    float lifespan = 0.0f;
    std::uint16_t x = 0;
    std::uint16_t y = 0;
    bool lesson_resource = false;
    std::string name;
};
struct RpySoundPayload {
    std::uint8_t flags = 0;
    bool lesson_resource = false;
    std::string name;
};
struct RpyFadePayload {
    float lifespan = 0.0f;
    bool fade_in = true;
    bool previous_fade_in = false;
};
struct RpyPlaybackPayload {
    float lifespan = 0.0f;
    std::uint8_t rate_index = 1;
    bool slow_motion = false;
    std::uint8_t previous_packed_rate = 2;
};
struct RpyTogglePayload {
    std::uint8_t item = 0;
    bool enabled = false;
    bool previous_enabled = false;
};
struct RpyVolumePayload {
    float lifespan = 0.0f;
    float volume = 1.0f;
    float previous_volume = 1.0f;
};

// Analysis-only input for a controlled type-11 replay fixture.  This is kept
// separate from editor operations (which are the stock UI's event types
// 19..28) so the replay editor cannot accidentally author simulation data.
struct RpyType11FixtureEntry {
    std::uint32_t frame_block = 0;
    RpyRptpType11DebrisState state{};
};

std::vector<RpyEditorOperation> enumerate_rpy_editor_operations(const RpyReplay& replay);
RpyEditorCameraState replay_editor_camera_state(
    const RpyReplay& replay, std::uint32_t through_frame);
void apply_replay_editor_camera_events(
    const RpyReplay& replay, std::uint32_t first_frame,
    std::uint32_t through_frame, RpyEditorCameraState& state);
bool validate_rpy_editor_payload(RpyEditorEventType type,
                                 std::span<const std::uint8_t> payload) noexcept;
std::string decode_rpy_editor_payload(RpyEditorEventType type,
                                      std::span<const std::uint8_t> payload);

std::vector<std::uint8_t> make_rpy_stamp_payload(
    const std::array<std::uint8_t, 13>& prefix, const std::string& name);
std::vector<std::uint8_t> make_rpy_stamp_payload(const RpyStampPayload& value);
std::vector<std::uint8_t> make_rpy_sound_payload(std::uint8_t flags,
                                                 const std::string& name);
std::vector<std::uint8_t> make_rpy_text_payload(
    const std::array<std::uint8_t, 15>& prefix, const std::u16string& text);
std::vector<std::uint8_t> make_rpy_text_payload(const RpyTextPayload& value);
std::vector<std::uint8_t> make_rpy_fade_payload(float value, std::uint8_t value2);
std::vector<std::uint8_t> make_rpy_fade_payload(const RpyFadePayload& value);
std::vector<std::uint8_t> make_rpy_camera_payload(std::uint8_t a, std::uint8_t b);
std::vector<std::uint8_t> make_rpy_car_payload(std::uint8_t a, std::uint8_t b);
std::vector<std::uint8_t> make_rpy_playback_payload(float value, std::uint8_t a,
                                                     std::uint8_t b);
std::vector<std::uint8_t> make_rpy_playback_payload(
    const RpyPlaybackPayload& value);
std::vector<std::uint8_t> make_rpy_marker_payload();
std::vector<std::uint8_t> make_rpy_toggle_payload(std::uint8_t a, std::uint8_t b,
                                                   std::uint8_t c);
std::vector<std::uint8_t> make_rpy_toggle_payload(const RpyTogglePayload& value);
std::vector<std::uint8_t> make_rpy_volume_payload(float a, float b, float c);
std::vector<std::uint8_t> make_rpy_volume_payload(const RpyVolumePayload& value);

std::optional<RpyTextPayload> parse_rpy_text_payload(
    std::span<const std::uint8_t> payload);
std::optional<RpyStampPayload> parse_rpy_stamp_payload(
    std::span<const std::uint8_t> payload);
std::optional<RpySoundPayload> parse_rpy_sound_payload(
    std::span<const std::uint8_t> payload);
std::optional<RpyFadePayload> parse_rpy_fade_payload(
    std::span<const std::uint8_t> payload);
std::optional<RpyPlaybackPayload> parse_rpy_playback_payload(
    std::span<const std::uint8_t> payload);
std::optional<RpyTogglePayload> parse_rpy_toggle_payload(
    std::span<const std::uint8_t> payload);
std::optional<RpyVolumePayload> parse_rpy_volume_payload(
    std::span<const std::uint8_t> payload);

// Replaces the fixed UTF-16LE description field beginning at RPHD body +0x3c.
// No RPTP byte is touched; overlong text is rejected rather than truncated.
std::vector<std::uint8_t> replace_rpy_summary(
    std::span<const std::uint8_t> original, const std::u16string& summary);

std::vector<std::uint8_t> insert_rpy_editor_event(
    std::span<const std::uint8_t> original, std::uint32_t frame_block,
    RpyEditorEventType type, std::span<const std::uint8_t> payload);
std::vector<std::uint8_t> delete_rpy_editor_event(
    std::span<const std::uint8_t> original, std::uint32_t global_event_index);
std::vector<std::uint8_t> replace_rpy_editor_event(
    std::span<const std::uint8_t> original, std::uint32_t global_event_index,
    RpyEditorEventType type, std::span<const std::uint8_t> payload);

// Encodes the fixed 16-byte payload written by the original type-11 producer.
// It rejects values outside the observed bit domains rather than truncating.
std::array<std::uint8_t, 16> encode_rpy_type11_fixture_payload(
    const RpyRptpType11DebrisState& state);

// Rebuilds a replay with validated type-11 records appended to the selected
// physical frame blocks.  Intended only for controlled original-reader tests;
// it never writes a file and preserves every unrelated RPTP record byte.
std::vector<std::uint8_t> insert_rpy_type11_fixture_events(
    std::span<const std::uint8_t> original,
    std::span<const RpyType11FixtureEntry> entries);

}  // namespace opennr
