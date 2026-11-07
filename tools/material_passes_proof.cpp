// Headless material-pass proof: what a material's submodel draw submits, for materials built by hand (no disc needed).
//
// The console draws a material's EVERY shader over its geometry, each with the registers that shader itself sends
// (materials.cpp's WriteShaders -> ShaderPacketSlot, shadersettings.cpp's PixelTest/DepthBuffer), and a sample that fails
// a shader's alpha test is written with that shader's own AFAIL -- all of which the host reproduces as a list of
// submissions (graphics.cpp's MaterialDrawPassesOf). This proof builds materials with one to four shaders and checks, for
// each: how many submissions come out, which shader each samples, and the state it draws with. The four-shader case is the
// one the list used to overflow: four shaders each asking for their rejected samples is eight submissions.
//
// Build: ninja material-pass-proof && ./build/host/material_passes_proof  (from the repo root)
#include "../src/platform/sdl3/graphics/host_graphics.h"

#include <bgfx/bgfx.h>

#include <cstdio>
#include <cstring>

namespace
{
// The GS's settings bytes the checks fill, by the names host_graphics.h's layout comment gives them (the file's 30 bytes in
// shadersettings.cpp's ShaderSettings order)
constexpr u32 SettingBlends = Platform::Graphics::HostShaderSettingBlends;
constexpr u32 SettingPreset = Platform::Graphics::HostShaderSettingPreset;
constexpr u32 SettingAlphaTest = Platform::Graphics::HostShaderSettingAlphaTest;
constexpr u32 SettingAlphaMethod = Platform::Graphics::HostShaderSettingAlphaMethod;
constexpr u32 SettingAlphaReference = Platform::Graphics::HostShaderSettingAlphaReference;
constexpr u32 SettingAlphaFail = 5;
constexpr u32 SettingDepthTest = Platform::Graphics::HostShaderSettingDepthTest;
constexpr u32 SettingGouraud = Platform::Graphics::HostShaderSettingGouraud;
constexpr u32 SettingNoDepthWrites = Platform::Graphics::HostShaderSettingNoDepthWrites;
constexpr u32 SettingDestinationTest = Platform::Graphics::HostShaderSettingDestinationTest;
constexpr u32 SettingUScroll = Platform::Graphics::HostShaderSettingUScroll;

// The pass state the model draws pass in (DrawRigidModel's), so the material's own fields are what the checks see change
constexpr u64 PassState = BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_WRITE_Z | BGFX_STATE_DEPTH_TEST_LESS;

// A material with nothing but its shaders' settings: flags says the block is real (HostMaterialStateOf reads it), bucket 2 is
// the opaque bucket, and the colour stays white so no check trips over a tint
void MakeMaterial(Platform::Graphics::HostMaterial* material, u32 shaders)
{
    std::memset(material, 0, sizeof(*material));
    material->flags = Platform::Graphics::HostMaterialSettings;
    material->bucket = 2;
    material->passCount = static_cast<u8>(shaders > 1 ? shaders : 0);
    material->shaderSettings[SettingBlends] = 0;
    material->shaderSettings[SettingDepthTest] = 2; // the GS's ZTST gequal, ShaderConstruct's default
    material->shaderSettings[SettingGouraud] = 1;   // and its IIP, so the shader interpolates unless a case clears it
    for (u32 channel = 0; channel < 4; channel++)
    {
        material->colour[channel] = 0xFF;
    }
}

// Every later shader's settings: the same block, with `afail` as its failure action and `blends`/`preset` its blend
void FillLaterShaders(Platform::Graphics::HostMaterial* material, u32 shaders, u8 afail, u8 blends, u8 preset)
{
    for (u32 shader = 1; shader < shaders; shader++)
    {
        u8 settings[Platform::Graphics::HostShaderSettings] = {};
        settings[SettingBlends] = blends;
        settings[SettingPreset] = preset;
        settings[SettingDepthTest] = 2;
        settings[SettingGouraud] = 1;
        settings[SettingAlphaTest] = 1;
        settings[SettingAlphaMethod] = Platform::Graphics::HostAlphaGreaterEqual;
        settings[SettingAlphaReference] = 100;
        settings[SettingAlphaFail] = afail;
        Platform::Graphics::HostMaterialSetLaterShader(material, shader, settings, nullptr, nullptr);
    }
}

bool Check(bool condition, const char* what)
{
    if (!condition)
    {
        std::printf("  FAIL %s\n", what);
    }

    return condition;
}

// The blend the state carries, and whether it is the additive preset (Cs·As + Cd) the later shaders in these materials ask for
bool Additive(const Platform::Graphics::HostMaterialState& state)
{
    const u64 factors = state.state & BGFX_STATE_BLEND_MASK & ~BGFX_STATE_BLEND_EQUATION_MASK;
    return factors == BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_SRC_ALPHA, BGFX_STATE_BLEND_ONE) &&
           (state.state & BGFX_STATE_BLEND_EQUATION_MASK) == 0;
}
}

int main()
{
    using namespace Platform::Graphics;

    // No bgfx instance at all: the pass list is worked out from the material's own bytes, before any draw is submitted, so
    // the proof needs no renderer (the state flags are the header's constants)
    HostMaterialState states[HostMaterialPasses * 2];
    u8 shaders[HostMaterialPasses * 2];
    bool ok = true;

    // One shader, no alpha test: one submission, the first shader's, and nothing rejected to draw
    {
        HostMaterial material;
        MakeMaterial(&material, 1);
        material.shaderSettings[SettingDepthTest] = 2;
        const u32 count = HostMaterialDrawPasses(&material, PassState, states, shaders, HostMaterialPasses * 2);
        ok &= Check(count == 1, "one shader draws once");
        ok &= Check(count == 1 && shaders[0] == 0, "the single shader is shader 0");
        ok &= Check(count == 1 && (states[0].state & BGFX_STATE_BLEND_MASK) == 0, "an unblended shader draws opaque");
    }

    // One shader, AFAIL FB_ONLY (1): the test drops the samples it rejects, and the second submission draws them with the
    // colour write and no depth write (the GS's FB_ONLY)
    {
        HostMaterial material;
        MakeMaterial(&material, 1);
        material.shaderSettings[SettingAlphaTest] = 1;
        material.shaderSettings[SettingAlphaMethod] = HostAlphaGreaterEqual;
        material.shaderSettings[SettingAlphaReference] = 100;
        material.shaderSettings[SettingAlphaFail] = 1;
        const u32 count = HostMaterialDrawPasses(&material, PassState, states, shaders, HostMaterialPasses * 2);
        ok &= Check(count == 2, "AFAIL FB_ONLY adds the rejected samples' submission");
        ok &= Check(count == 2 && shaders[0] == 0 && shaders[1] == 0, "both submissions are the first shader's");
        ok &= Check(count == 2 && states[1].alphaReference[2] == 1.0f, "the rejected pass turns the test round");
        ok &= Check(count == 2 && (states[1].state & BGFX_STATE_WRITE_Z) == 0, "FB_ONLY writes no depth");
        ok &= Check(count == 2 && (states[1].state & BGFX_STATE_WRITE_RGB) != 0, "FB_ONLY writes the colour");
    }

    // Two shaders, the second one additive with its own AFAIL: the first shader's submission, then the second's, then the
    // second's rejected samples -- all three of them, in that order
    {
        HostMaterial material;
        MakeMaterial(&material, 2);
        FillLaterShaders(&material, 2, 1, 1, HostPresetAdd);
        const u32 count = HostMaterialDrawPasses(&material, PassState, states, shaders, HostMaterialPasses * 2);
        ok &= Check(count == 3, "two shaders with AFAIL draw three times (the first, the second, its rejections)");
        ok &= Check(count == 3 && shaders[0] == 0 && shaders[1] == 1 && shaders[2] == 1,
                    "each submission names the shader it draws");
        ok &= Check(count == 3 && (states[0].state & BGFX_STATE_BLEND_MASK) == 0, "the first shader is opaque");
        ok &= Check(count == 3 && Additive(states[1]) && Additive(states[2]),
                    "the second shader's own blend reaches both of its submissions");
    }

    // Two shaders, the second with its own destination test and no depth writes: the state is the shader's, not the first's
    {
        HostMaterial material;
        MakeMaterial(&material, 2);
        u8 settings[HostShaderSettings] = {};
        settings[SettingDepthTest] = 3; // a ZTST the first shader does not have (it keeps 2)
        settings[SettingNoDepthWrites] = 1;
        settings[SettingDestinationTest] = 1;
        HostMaterialSetLaterShader(&material, 1, settings, nullptr, nullptr);
        const u32 count = HostMaterialDrawPasses(&material, PassState, states, shaders, HostMaterialPasses * 2);
        ok &= Check(count == 2, "a shader with no writing AFAIL adds no rejected pass");
        ok &= Check(count == 2 && (states[1].state & BGFX_STATE_WRITE_Z) == 0, "its no-depth-writes reaches the state");
        ok &= Check(count == 2 && (states[0].state & BGFX_STATE_WRITE_Z) != 0, "the first shader still writes depth");
        // The two shaders' ZTST bytes differ, so their depth tests must too: a later pass drawn with the first shader's test
        // was the bug this proves gone
        ok &= Check(count == 2 && (states[1].state & BGFX_STATE_DEPTH_TEST_MASK) !=
                                  (states[0].state & BGFX_STATE_DEPTH_TEST_MASK),
                    "the later pass draws with its own depth test");
    }

    // Four shaders, every one of them asking for its rejected samples: eight submissions, two a shader, in order (the case
    // the list was one entry short of)
    {
        HostMaterial material;
        MakeMaterial(&material, 4);
        material.shaderSettings[SettingAlphaTest] = 1;
        material.shaderSettings[SettingAlphaMethod] = HostAlphaGreaterEqual;
        material.shaderSettings[SettingAlphaReference] = 100;
        material.shaderSettings[SettingAlphaFail] = 3; // RGB_ONLY
        FillLaterShaders(&material, 4, 3, 1, HostPresetAdd);
        const u32 count = HostMaterialDrawPasses(&material, PassState, states, shaders, HostMaterialPasses * 2);
        ok &= Check(count == 2 * HostMaterialPasses, "four shaders with AFAIL draw eight times");
        bool ordered = count == 2 * HostMaterialPasses;
        for (u32 pass = 0; ordered && pass < count; pass++)
        {
            ordered = shaders[pass] == pass / 2;
        }
        ok &= Check(ordered, "the submissions run shader 0, 0, 1, 1, 2, 2, 3, 3");
        ok &= Check(count == 2 * HostMaterialPasses && (states[7].state & BGFX_STATE_WRITE_A) == 0,
                    "RGB_ONLY's rejected samples write no alpha");
    }

    // Flat shading: the GS's IIP bit is the shader's own, so a material whose later shader clears it draws that pass with the
    // flat-qualified programs while the first shader keeps interpolating (the draws read HostMaterialState::flat)
    {
        HostMaterial material;
        MakeMaterial(&material, 2);
        u8 settings[HostShaderSettings] = {};
        settings[SettingGouraud] = 0; // IIP cleared: the flat-shaded shader
        HostMaterialSetLaterShader(&material, 1, settings, nullptr, nullptr);
        const u32 count = HostMaterialDrawPasses(&material, PassState, states, shaders, HostMaterialPasses * 2);
        ok &= Check(count == 2, "the flat shader draws its own pass");
        ok &= Check(count == 2 && !states[0].flat, "a shader with IIP set draws interpolated");
        ok &= Check(count == 2 && states[1].flat, "a shader with IIP cleared draws flat");
    }

    // The later shaders' scroll blocks: each shader's own offset comes back for its own index, which is what the draws add
    // to that pass's UVs
    {
        HostMaterial material;
        MakeMaterial(&material, 2);
        const f32 phases[2] = {0.25f, 0.75f};
        const f32 speeds[2] = {1.0f, 2.0f};
        u8 settings[HostShaderSettings] = {};
        settings[SettingUScroll] = 2; // a phase going round 0 to 1
        HostMaterialSetLaterShader(&material, 1, settings, phases, speeds);
        material.scroll[0] = 0.5f;
        material.scroll[1] = 0.125f;
        f32 first[2] = {};
        f32 second[2] = {};
        ok &= Check(HostMaterialShaderScroll(&material, 0, first), "the first shader's scroll is kept");
        ok &= Check(HostMaterialShaderScroll(&material, 1, second), "the second shader's scroll is kept");
        ok &= Check(first[0] == 0.5f && first[1] == 0.125f, "the first shader's own offset comes back");
        ok &= Check(second[0] == 0.0f && second[1] == 0.0f, "the second shader's block starts where it was left");
        ok &= Check(!HostMaterialShaderScroll(&material, 3, second), "a shader the material does not have reports none");
    }

    std::printf("material-pass-proof: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
