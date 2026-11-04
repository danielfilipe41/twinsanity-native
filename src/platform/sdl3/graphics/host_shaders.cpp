// Host shader registry: one row per HostShader, holding the blob each backend takes. A platform only carries the
// backends its renderers take, which tools/build_shaders.py names per platform: desktop OpenGL builds take GLSL 330,
// Android's GL ES takes GLSL ES 300, and Vulkan takes SPIR-V everywhere. Adding a renderer means adding its profile to
// that platform's backends there and its column here; nothing in the draws changes.
#include "host_shaders.h"

// Only this platform's blobs are compiled in: Android's GL family is GLSL ES 300, a desktop build's is GLSL 330, and
// SPIR-V (Vulkan) is on every native platform
#if defined(__ANDROID__)
#include "shaders/fs_draw2d_gles.bin.h"
#include "shaders/fs_draw3d_gles.bin.h"
#include "shaders/fs_particle_gles.bin.h"
#include "shaders/fs_scenecomposite_gles.bin.h"
#include "shaders/fs_scenefog_gles.bin.h"
#include "shaders/fs_screenfx_gles.bin.h"
#include "shaders/fs_shadowresolve_gles.bin.h"
#include "shaders/fs_shadowvolume_gles.bin.h"
#include "shaders/fs_stq_gles.bin.h"
#include "shaders/vs_draw2d_gles.bin.h"
#include "shaders/vs_draw3d_gles.bin.h"
#include "shaders/vs_draw3dskin_gles.bin.h"
#include "shaders/vs_particle_gles.bin.h"
#include "shaders/vs_scenecomposite_gles.bin.h"
#include "shaders/vs_scenefog_gles.bin.h"
#include "shaders/vs_screenfx_gles.bin.h"
#include "shaders/vs_shadowresolve_gles.bin.h"
#include "shaders/vs_stq_gles.bin.h"
#else
#include "shaders/fs_draw2d.bin.h"
#include "shaders/fs_draw3d.bin.h"
#include "shaders/fs_particle.bin.h"
#include "shaders/fs_scenecomposite.bin.h"
#include "shaders/fs_scenefog.bin.h"
#include "shaders/fs_screenfx.bin.h"
#include "shaders/fs_shadowresolve.bin.h"
#include "shaders/fs_shadowvolume.bin.h"
#include "shaders/fs_stq.bin.h"
#include "shaders/vs_draw2d.bin.h"
#include "shaders/vs_draw3d.bin.h"
#include "shaders/vs_draw3dskin.bin.h"
#include "shaders/vs_particle.bin.h"
#include "shaders/vs_scenecomposite.bin.h"
#include "shaders/vs_scenefog.bin.h"
#include "shaders/vs_screenfx.bin.h"
#include "shaders/vs_shadowresolve.bin.h"
#include "shaders/vs_stq.bin.h"
#endif
#if defined(__EMSCRIPTEN__)
#include "shaders/fs_draw2d_wgsl.bin.h"
#include "shaders/fs_draw3d_wgsl.bin.h"
#include "shaders/fs_particle_wgsl.bin.h"
#include "shaders/fs_scenecomposite_wgsl.bin.h"
#include "shaders/fs_scenefog_wgsl.bin.h"
#include "shaders/fs_screenfx_wgsl.bin.h"
#include "shaders/fs_shadowresolve_wgsl.bin.h"
#include "shaders/fs_shadowvolume_wgsl.bin.h"
#include "shaders/fs_stq_wgsl.bin.h"
#include "shaders/vs_draw2d_wgsl.bin.h"
#include "shaders/vs_draw3d_wgsl.bin.h"
#include "shaders/vs_draw3dskin_wgsl.bin.h"
#include "shaders/vs_particle_wgsl.bin.h"
#include "shaders/vs_scenecomposite_wgsl.bin.h"
#include "shaders/vs_scenefog_wgsl.bin.h"
#include "shaders/vs_screenfx_wgsl.bin.h"
#include "shaders/vs_shadowresolve_wgsl.bin.h"
#include "shaders/vs_stq_wgsl.bin.h"
#endif
#include "shaders/fs_draw2d_spv.bin.h"
#include "shaders/fs_draw3d_spv.bin.h"
#include "shaders/fs_particle_spv.bin.h"
#include "shaders/fs_scenecomposite_spv.bin.h"
#include "shaders/fs_scenefog_spv.bin.h"
#include "shaders/fs_screenfx_spv.bin.h"
#include "shaders/fs_shadowresolve_spv.bin.h"
#include "shaders/fs_shadowvolume_spv.bin.h"
#include "shaders/fs_stq_spv.bin.h"
#include "shaders/vs_draw2d_spv.bin.h"
#include "shaders/vs_draw3d_spv.bin.h"
#include "shaders/vs_draw3dskin_spv.bin.h"
#include "shaders/vs_particle_spv.bin.h"
#include "shaders/vs_scenecomposite_spv.bin.h"
#include "shaders/vs_scenefog_spv.bin.h"
#include "shaders/vs_screenfx_spv.bin.h"
#include "shaders/vs_shadowresolve_spv.bin.h"
#include "shaders/vs_stq_spv.bin.h"

// Direct3D 11 and 12 take DXBC, which only a shaderc built for Windows can make (tools/build_shaders.py
// --platform windows --d3d). configure.py defines TWINSANITY_HAVE_DXBC only when the generated headers are there, so a
// build without them carries no Direct3D renderer and says so
#if defined(TWINSANITY_HAVE_DXBC)
#include "shaders/fs_draw2d_dxbc.bin.h"
#include "shaders/fs_draw3d_dxbc.bin.h"
#include "shaders/fs_particle_dxbc.bin.h"
#include "shaders/fs_scenecomposite_dxbc.bin.h"
#include "shaders/fs_scenefog_dxbc.bin.h"
#include "shaders/fs_screenfx_dxbc.bin.h"
#include "shaders/fs_shadowresolve_dxbc.bin.h"
#include "shaders/fs_shadowvolume_dxbc.bin.h"
#include "shaders/fs_stq_dxbc.bin.h"
#include "shaders/vs_draw2d_dxbc.bin.h"
#include "shaders/vs_draw3d_dxbc.bin.h"
#include "shaders/vs_draw3dskin_dxbc.bin.h"
#include "shaders/vs_particle_dxbc.bin.h"
#include "shaders/vs_scenecomposite_dxbc.bin.h"
#include "shaders/vs_scenefog_dxbc.bin.h"
#include "shaders/vs_screenfx_dxbc.bin.h"
#include "shaders/vs_shadowresolve_dxbc.bin.h"
#include "shaders/vs_stq_dxbc.bin.h"
#endif

#include <cstddef>
#include <cstdio>

// The GL-family blob this platform's GL renderer takes: a desktop build's GL is OpenGL (GLSL 330), Android's is GL ES
// (GLSL ES 300). Only that family's rows are referenced, so a build carries one GL profile and no more
#if defined(__ANDROID__)
#define HOST_SHADER_GL_BLOB(stem) stem##_gles, sizeof(stem##_gles)
#else
#define HOST_SHADER_GL_BLOB(stem) stem##_glsl, sizeof(stem##_glsl)
#endif

// The web is the one platform with two GL-family-adjacent renderers of its own: WebGL takes the GLSL ES 300 blobs above,
// and WebGPU takes WGSL (a blob of its own, so a web build can offer either)
#if defined(__EMSCRIPTEN__)
#define HOST_SHADER_WEBGPU_BLOB(stem) , stem##_wgsl, sizeof(stem##_wgsl)
#else
#define HOST_SHADER_WEBGPU_BLOB(stem)
#endif

// Windows' two Direct3D renderers take the DXBC blob, when the build has it
#if defined(TWINSANITY_HAVE_DXBC)
#define HOST_SHADER_D3D_BLOB(stem) , stem##_dxbc, sizeof(stem##_dxbc)
#else
#define HOST_SHADER_D3D_BLOB(stem)
#endif

namespace
{
// A shader's bytecode per backend, in HostShader order. The GL blob is also what bgfx's Noop renderer takes (it never
// reads it, which is what lets the headless proofs run without a GPU)
struct HostShaderBlobs
{
    // The platform's GL family: GLSL 330 (OpenGL) or GLSL ES 300 (Android's GL ES), HOST_SHADER_GL_BLOB
    const uint8_t* gl;
    uint32_t glSize;
    const uint8_t* spirv;
    uint32_t spirvSize;
#if defined(__EMSCRIPTEN__)
    // WGSL, for the web's WebGPU renderer
    const uint8_t* wgsl;
    uint32_t wgslSize;
#endif
#if defined(TWINSANITY_HAVE_DXBC)
    // DXBC, for the Windows build's Direct3D 11 and 12 renderers
    const uint8_t* dxbc;
    uint32_t dxbcSize;
#endif
};

constexpr HostShaderBlobs ShaderBlobs[] = {
    {HOST_SHADER_GL_BLOB(vs_draw2d), vs_draw2d_spv, sizeof(vs_draw2d_spv) HOST_SHADER_WEBGPU_BLOB(vs_draw2d) HOST_SHADER_D3D_BLOB(vs_draw2d)},
    {HOST_SHADER_GL_BLOB(fs_draw2d), fs_draw2d_spv, sizeof(fs_draw2d_spv) HOST_SHADER_WEBGPU_BLOB(fs_draw2d) HOST_SHADER_D3D_BLOB(fs_draw2d)},
    {HOST_SHADER_GL_BLOB(vs_draw3d), vs_draw3d_spv, sizeof(vs_draw3d_spv) HOST_SHADER_WEBGPU_BLOB(vs_draw3d) HOST_SHADER_D3D_BLOB(vs_draw3d)},
    {HOST_SHADER_GL_BLOB(vs_draw3dskin), vs_draw3dskin_spv, sizeof(vs_draw3dskin_spv) HOST_SHADER_WEBGPU_BLOB(vs_draw3dskin) HOST_SHADER_D3D_BLOB(vs_draw3dskin)},
    {HOST_SHADER_GL_BLOB(fs_draw3d), fs_draw3d_spv, sizeof(fs_draw3d_spv) HOST_SHADER_WEBGPU_BLOB(fs_draw3d) HOST_SHADER_D3D_BLOB(fs_draw3d)},
    {HOST_SHADER_GL_BLOB(fs_shadowvolume), fs_shadowvolume_spv, sizeof(fs_shadowvolume_spv) HOST_SHADER_WEBGPU_BLOB(fs_shadowvolume) HOST_SHADER_D3D_BLOB(fs_shadowvolume)},
    {HOST_SHADER_GL_BLOB(vs_particle), vs_particle_spv, sizeof(vs_particle_spv) HOST_SHADER_WEBGPU_BLOB(vs_particle) HOST_SHADER_D3D_BLOB(vs_particle)},
    {HOST_SHADER_GL_BLOB(fs_particle), fs_particle_spv, sizeof(fs_particle_spv) HOST_SHADER_WEBGPU_BLOB(fs_particle) HOST_SHADER_D3D_BLOB(fs_particle)},
    {HOST_SHADER_GL_BLOB(vs_stq), vs_stq_spv, sizeof(vs_stq_spv) HOST_SHADER_WEBGPU_BLOB(vs_stq) HOST_SHADER_D3D_BLOB(vs_stq)},
    {HOST_SHADER_GL_BLOB(fs_stq), fs_stq_spv, sizeof(fs_stq_spv) HOST_SHADER_WEBGPU_BLOB(fs_stq) HOST_SHADER_D3D_BLOB(fs_stq)},
    {HOST_SHADER_GL_BLOB(vs_screenfx), vs_screenfx_spv, sizeof(vs_screenfx_spv) HOST_SHADER_WEBGPU_BLOB(vs_screenfx) HOST_SHADER_D3D_BLOB(vs_screenfx)},
    {HOST_SHADER_GL_BLOB(fs_screenfx), fs_screenfx_spv, sizeof(fs_screenfx_spv) HOST_SHADER_WEBGPU_BLOB(fs_screenfx) HOST_SHADER_D3D_BLOB(fs_screenfx)},
    {HOST_SHADER_GL_BLOB(vs_scenefog), vs_scenefog_spv, sizeof(vs_scenefog_spv) HOST_SHADER_WEBGPU_BLOB(vs_scenefog) HOST_SHADER_D3D_BLOB(vs_scenefog)},
    {HOST_SHADER_GL_BLOB(fs_scenefog), fs_scenefog_spv, sizeof(fs_scenefog_spv) HOST_SHADER_WEBGPU_BLOB(fs_scenefog) HOST_SHADER_D3D_BLOB(fs_scenefog)},
    {HOST_SHADER_GL_BLOB(vs_shadowresolve), vs_shadowresolve_spv, sizeof(vs_shadowresolve_spv) HOST_SHADER_WEBGPU_BLOB(vs_shadowresolve) HOST_SHADER_D3D_BLOB(vs_shadowresolve)},
    {HOST_SHADER_GL_BLOB(fs_shadowresolve), fs_shadowresolve_spv, sizeof(fs_shadowresolve_spv) HOST_SHADER_WEBGPU_BLOB(fs_shadowresolve) HOST_SHADER_D3D_BLOB(fs_shadowresolve)},
    {HOST_SHADER_GL_BLOB(vs_scenecomposite), vs_scenecomposite_spv, sizeof(vs_scenecomposite_spv) HOST_SHADER_WEBGPU_BLOB(vs_scenecomposite) HOST_SHADER_D3D_BLOB(vs_scenecomposite)},
    {HOST_SHADER_GL_BLOB(fs_scenecomposite), fs_scenecomposite_spv, sizeof(fs_scenecomposite_spv) HOST_SHADER_WEBGPU_BLOB(fs_scenecomposite) HOST_SHADER_D3D_BLOB(fs_scenecomposite)},
};
static_assert(sizeof(ShaderBlobs) / sizeof(ShaderBlobs[0]) == static_cast<std::size_t>(Platform::Graphics::HostShader::Count),
              "every host shader has a blob row");
}

#undef HOST_SHADER_GL_BLOB
#undef HOST_SHADER_WEBGPU_BLOB
#undef HOST_SHADER_D3D_BLOB

namespace Platform::Graphics
{
bgfx::ShaderHandle HostCreateShader(HostShader shader)
{
    const HostShaderBlobs& blobs = ShaderBlobs[static_cast<std::size_t>(shader)];
    const bgfx::RendererType::Enum renderer = bgfx::getRendererType();
    const uint8_t* data = nullptr;
    uint32_t size = 0;
    switch (renderer)
    {
    case bgfx::RendererType::Vulkan:
        data = blobs.spirv;
        size = blobs.spirvSize;
        break;
#if defined(__EMSCRIPTEN__)
    case bgfx::RendererType::WebGPU:
        data = blobs.wgsl;
        size = blobs.wgslSize;
        break;
    case bgfx::RendererType::OpenGLES:
#elif defined(__ANDROID__)
    case bgfx::RendererType::OpenGLES:
#else
    case bgfx::RendererType::OpenGL:
#endif
#if defined(TWINSANITY_HAVE_DXBC)
    case bgfx::RendererType::Direct3D11:
    case bgfx::RendererType::Direct3D12:
        data = blobs.dxbc;
        size = blobs.dxbcSize;
        break;
#endif
    // Noop never reads the bytecode: it keeps the headless proofs and their object counts renderer-independent
    case bgfx::RendererType::Noop:
        data = blobs.gl;
        size = blobs.glSize;
        break;
    default:
        std::fprintf(stderr, "bgfx: no shader blobs for the %s renderer\n", bgfx::getRendererName(renderer));
        return BGFX_INVALID_HANDLE;
    }
    return bgfx::createShader(bgfx::makeRef(data, size));
}
}
