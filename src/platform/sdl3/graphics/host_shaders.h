// Host shader registry: the renderer's shaders by name, so a draw never names a bytecode blob. The blobs are the
// outputs of tools/build_shaders.py; HostCreateShader hands back the one the initialized renderer takes (GLSL for
// bgfx's OpenGL renderer, SPIR-V for Vulkan).
#pragma once

#include <bgfx/bgfx.h>

namespace Platform::Graphics
{
// The renderer's programs' shaders, by role. The draws make their programs from these; the order is the blob table's
// in host_shaders.cpp, so the two lists have to move together
enum class HostShader
{
    Draw2DVertex,
    Draw2DFragment,
    Draw3DVertex,
    Draw3DSkinVertex,
    Draw3DFragment,
    ShadowVolumeFragment,
    ParticleVertex,
    ParticleFragment,
    StqVertex,
    StqFragment,
    ScreenFxVertex,
    ScreenFxFragment,
    SceneFogVertex,
    SceneFogFragment,
    ShadowResolveVertex,
    ShadowResolveFragment,
    SceneCompositeVertex,
    SceneCompositeFragment,
    FxaaFragment,
    SmaaEdgeFragment,
    SmaaWeightsFragment,
    SmaaBlendFragment,
    Count,
};

// Creates the shader for the renderer bgfx initialized (the caller owns the handle). Invalid, with one line on stderr,
// for a renderer this build has no blobs for (WebGPU's WGSL, Metal's MSL, Direct3D's DXIL/DXBC today)
bgfx::ShaderHandle HostCreateShader(HostShader shader);
}
