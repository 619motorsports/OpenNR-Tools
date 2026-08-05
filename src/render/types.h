#pragma once

// Renderer-level value types used across the render module.  These are
// modern-C++ analogues of the data the rend_dxg.dll vtable shuffles
// around in 32-bit form (window rects in (top, left, w, h) ints,
// 0xRRGGBB DWORD colours, per-pixel format enums, etc.).
//
// Conventions (matching the rest of the project):
//
//   * Coordinate-system handedness is right-handed, +z up - see
//     src/math/vec.h.  Renderer-internal projection / clip space is
//     whatever the backend chooses; world-space input is always RH+Zup.
//   * Window / viewport rectangles use top-left origin in pixel
//     coordinates, matching Win32.
//   * Colours are linear RGBA floats unless explicitly noted.

#include <cstdint>

namespace opennr::render {

// A window-relative pixel rect.  (left, top) is the inclusive top-left
// pixel; (right, bottom) is exclusive (matching Win32 RECT semantics).
struct Rect {
    int left   = 0;
    int top    = 0;
    int right  = 0;
    int bottom = 0;

    constexpr int width()  const { return right - left; }
    constexpr int height() const { return bottom - top; }

    constexpr bool empty() const { return width() <= 0 || height() <= 0; }
};

// (x, y, w, h) form for the few APIs that prefer that shape - kept as a
// distinct type so a caller can't pass a rect-as-(L,T,R,B) where a
// rect-as-(X,Y,W,H) is expected, or vice versa.
struct Viewport {
    int x      = 0;
    int y      = 0;
    int width  = 0;
    int height = 0;

    float min_depth = 0.0f;
    float max_depth = 1.0f;
};

struct ColorRGBA {
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    float a = 1.0f;
};

// Pixel formats that the renderer can be asked to allocate for textures
// and render-target surfaces.  This mirrors the conceptual set the
// engine asks the legacy DLL for; we don't try to cover every D3D8
// format because the engine only uses a handful.
enum class PixelFormat : std::uint16_t {
    unknown = 0,
    rgba8,        // 32-bit, 8 bits/channel, with alpha
    rgb8,         // 24-bit, no alpha (rare in practice)
    bgra4,        // 16-bit 4:4:4:4
    rgb565,       // 16-bit colour, no alpha (NR2003's default at
                  // 16bpp display depth)
    rgb5a1,       // 16-bit 5:5:5:1
    a8,           // 8-bit alpha-only (for shadow / lightmap masks)
    l8,           // 8-bit luminance
    dxt1,         // S3TC compressed (no alpha)
    dxt3,         // S3TC compressed (4-bit alpha)
    dxt5,         // S3TC compressed (interpolated alpha)
    depth16,
    depth24_stencil8,
    depth32f,
};

enum class TextureKind : std::uint8_t {
    image_2d   = 0,
    cube_map   = 1,  // env-maps for car bodies
    volume_3d  = 2,  // unused by NR2003 in practice
};

enum class FilterMode : std::uint8_t {
    none          = 0,
    point         = 1,
    bilinear      = 2,  // strings in rend_dxg.dll: MinFilter=2, MagFilter=2
    trilinear     = 3,
    anisotropic   = 4,
};

enum class AddressMode : std::uint8_t {
    wrap   = 0,
    mirror = 1,
    clamp  = 2,
    border = 3,
};

struct TextureAddress {
    AddressMode u = AddressMode::clamp;
    AddressMode v = AddressMode::clamp;
};

// Display configuration the engine asks the renderer to come up in.
// Values match the keys in app.ini's [Graphics] section.
struct DisplayConfig {
    int  width        = 1920;
    int  height       = 1080;
    int  bits_per_pixel = 32;
    bool fullscreen   = true;
    int  board_index  = 0;     // adapter index (multi-GPU)
    bool vsync        = false; // app.ini's vSyncMode=0 by default
};

// Opaque handles for renderer-owned resources.  Ownership stays with
// the renderer: callers must release through the renderer's destroy
// methods, not by deleting the handle.  Zero is the "null handle"
// sentinel, never returned by a successful create.
enum class TextureHandle      : std::uint32_t { null = 0 };
enum class RenderTargetHandle : std::uint32_t { null = 0 };
enum class BufferHandle       : std::uint32_t { null = 0 };
enum class ShaderHandle       : std::uint32_t { null = 0 };
enum class MaterialHandle     : std::uint32_t { null = 0 };  // legacy slot 37
enum class EffectHandle       : std::uint32_t { null = 0 };  // legacy slot 84

// Per-draw blend mode shared by the D3D11 and Vulkan effect paths.
enum class MeshBlend : std::uint8_t { opaque, alpha, additive };

// Compiled scene-graph node. The legacy renderer's compile-step cluster
// (vtable slots 68-80, see docs/reverse/renderer_pipeline.md) accepts a
// parsed *Descriptor and produces a Compiled* runtime node. We surface
// that as an opaque, ref-counted handle so the asset pipeline can reach
// the renderer without exposing its internal class hierarchy.
enum class CompiledNodeHandle : std::uint32_t { null = 0 };

// Camera projection state.  Maps to the renderer-DLL fields at
// `+0x9DBC` (near), `+0x9DC0` (FOV), `+0x9DD2` (far), with the same
// defaults the constructor uses (50, 75, 1000) - see
// docs/reverse/renderer_layout.md.
struct CameraProjection {
    float fov_degrees = 75.0f;
    float near_plane  = 50.0f;
    float far_plane   = 1000.0f;
};

// Renderer feature toggles.  Stored in the legacy renderer at
// `+0x9D9E` as a bitfield with default value `7` (all three on).
// We use a strongly-typed flag enum here so the engine can't pass
// arbitrary ints by mistake.
enum class FeatureFlags : std::uint32_t {
    none           = 0,
    fog            = 1u << 0,
    specular       = 1u << 1,
    stencil        = 1u << 2,
    all_default    = fog | specular | stencil,
};

constexpr FeatureFlags operator|(FeatureFlags a, FeatureFlags b) {
    return FeatureFlags(static_cast<std::uint32_t>(a) | static_cast<std::uint32_t>(b));
}
constexpr FeatureFlags operator&(FeatureFlags a, FeatureFlags b) {
    return FeatureFlags(static_cast<std::uint32_t>(a) & static_cast<std::uint32_t>(b));
}
constexpr bool any(FeatureFlags v) { return static_cast<std::uint32_t>(v) != 0; }

// Exponential vertex fog, mirroring rend_dxg.dll's model (see
// docs/reverse/fog_and_stage_cascade.md): fog is armed when
// `density > 0` AND `FeatureFlags::fog` is set — there is no separate
// enable bit on the legacy hot path (the density global DAT_100c58ac
// doubles as the switch).  The factor is `f = exp(-density * depth)`
// with view-space depth (the original upgrades to radial eye distance
// when the device reports the range-fog cap; we always use view
// depth).  Final color = f * lit + (1 - f) * fog color.
struct FogParams {
    float density = 0.f;                  // 0 = fog off
    float r = 0.f, g = 0.f, b = 0.f;      // fog color
};

// Authored global directional light.  `direction` points from the surface
// toward the light, matching the c16 vector consumed by NR2003's car shader.
struct DirectionalLightParams {
    float direction_x = 0.4f, direction_y = 0.2f, direction_z = 1.0f;
    float ambient_r = 0.35f, ambient_g = 0.35f, ambient_b = 0.35f;
    float diffuse_r = 0.65f, diffuse_g = 0.65f, diffuse_b = 0.65f;
};

// Stencil-shadow darkening factor recovered from rend_dxg's resolve
// pass (`FUN_10059920`, fed by `FUN_100599e0` with the scene ambient
// and direct light colours — docs/reverse/car_shadow_system.md): a
// shadowed pixel keeps only its ambient share of the illumination,
//
//   f = max(ambient.rgb) / (max(ambient.rgb) + max(diffuse.rgb))
//
// clamped to [0,1]; degenerate lighting (sum < 1e-4) leaves the frame
// untouched (f = 1).
inline float shadow_darkening_from_light(const DirectionalLightParams& l) {
    auto max3 = [](float a, float b, float c) {
        float m = a > b ? a : b;
        return m > c ? m : c;
    };
    const float amb = max3(l.ambient_r, l.ambient_g, l.ambient_b);
    const float dir = max3(l.diffuse_r, l.diffuse_g, l.diffuse_b);
    if (amb + dir < 1e-4f) return 1.f;
    const float f = amb / (amb + dir);
    return f < 0.f ? 0.f : (f > 1.f ? 1.f : f);
}

// Material description for the multi-texture appearance path.  Maps
// the subset of CompiledAppearance's seven stages that the original
// pipeline actually blends (Base modulate-diffuse is the universal
// fixed-function program; Detail/Light modulate and Env/Specular add
// ride the car mega-shader path — docs/reverse/fog_and_stage_cascade.md).
// Stage handles left null are skipped.  Specular follows the legacy
// D3DMCS_MATERIAL sourcing: color + power come from here, gated by
// FeatureFlags::specular.
struct MaterialDesc {
    TextureHandle base      = TextureHandle::null;  // stage 0 (falls back to mesh texture)
    TextureHandle detail    = TextureHandle::null;  // modulates base color
    TextureHandle light_map = TextureHandle::null;  // modulates base color
    TextureHandle bump      = TextureHandle::null;  // EMBM du/dv source
    TextureHandle env       = TextureHandle::null;  // sphere-mapped additive reflection
    // Gloss mask (the 7-slot cascade's Specular stage): modulates the
    // env reflection per-texel, mirroring the mega PS's
    // `t1_shiny · envFade · t0_env` term (docs/reverse/car_megashader.md).
    TextureHandle specular_mask = TextureHandle::null;
    float specular_r = 0.f, specular_g = 0.f, specular_b = 0.f;
    float shininess       = 16.f;   // Blinn-Phong power
    float reflectivity    = 0.f;    // env-stage add strength (0 = off)
    float detail_uv_scale = 1.f;    // detail stage samples uv * scale
    // D3D8 environmental bump-map stage parameters recovered from
    // AppearanceDescriptor (+0x90..+0x98).  The legacy shader bridge uses
    // these to lower texbem/texbeml and the built-in EMBM techniques.
    float embm_texcoord_scale = 1.f;
    float embm_normal_scale = 1.f;
    float embm_luminosity_scale = 1.f;
    float opacity         = 1.f;    // multiplies texture and vertex alpha
    float alpha_cutoff    = 0.f;    // discard output alpha below this value
    // Down-facing env fade slope: the mega VS computes
    // `oD1.w = max((saturate(R.z + 1) - 0.80) · fadeRange, 0)` so
    // reflections pointing at the ground fade out.  5.0 maps
    // R.z ∈ [-0.2, 0] onto fade ∈ [0, 1].
    float env_fade_range  = 5.f;
};

// Mip-streaming budget.  Both values are clamped to `1024` by the
// legacy renderer at slot 114 (a hard limit baked into the DLL) and
// to the per-pool budget at `+0x9DAB`/+`0x9DAF` (default 128 each).
struct MipBudget {
    std::uint32_t min_kb = 0;
    std::uint32_t max_kb = 0;
};

// Camera context — one of N (we expose 16, matching the legacy
// renderer's 16-slot context array at `+0x7C..+0x968C`).  Each context
// is an independent (projection, view-matrix, viewport) triple that
// the engine selects between for different camera modes (driver,
// mirror, replay TV, in-car onboard, paint-shop preview, etc.).
//
// `view` is column-major, world-space → camera-space.  When the
// caller provides their own composed view-projection through the
// renderer's `set_view_proj()`, that overrides the camera context's
// (projection × view) for the current frame.  Otherwise the renderer
// composes `proj_from_camera_params × view` on the fly.
struct CameraContext {
    CameraProjection projection{};
    float            view[16] = {
        1.f, 0.f, 0.f, 0.f,
        0.f, 1.f, 0.f, 0.f,
        0.f, 0.f, 1.f, 0.f,
        0.f, 0.f, 0.f, 1.f,
    };
    bool             has_view = false;   // true when set_view_matrix was called
    Viewport         viewport{};
};

constexpr int kCameraContextCount = 16;

}  // namespace opennr::render
