// Material system: fills the game's 0x70-byte material storage with the host material ({u32 program;
// flags; uniforms}) and owns the shared 2D program, the sampler uniform, the 1x1 white texture
// the draws sample and the texture uploads' cache (HostTextureCache: one RGBA8 bgfx texture per transfer key, bound by
// SubmitQuad with the white texture standing in until then). The bgfx objects are process singletons made lazily.
#include "host_graphics.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "shaders/fs_draw2d.bin.h"
#include "shaders/vs_draw2d.bin.h"

namespace
{
// No valid bgfx handle has this index; stored in HostMaterial while it holds nothing
constexpr u32 InvalidHandle = 0xFFFFFFFF;

// The shared 2D objects, made once: the program from the embedded blobs, the alpha test's u_materialAlpha and the
// s_tex sampler and the white texture
bgfx::ProgramHandle g_program = BGFX_INVALID_HANDLE;
bgfx::UniformHandle g_sampler = BGFX_INVALID_HANDLE;
bgfx::UniformHandle g_alpha = BGFX_INVALID_HANDLE;
bgfx::TextureHandle g_white = BGFX_INVALID_HANDLE;
bool g_made = false;

// The UI's flat material: static storage like the PS2's g_FlatMaterial (the game keeps pointers to it)
alignas(8) u8 g_flatStorage[Platform::Graphics::MaterialStorage];

void MakeShared()
{
    if (g_made)
    {
        return;
    }
    g_made = true;
    bgfx::ShaderHandle vs = bgfx::createShader(bgfx::makeRef(vs_draw2d_glsl, sizeof(vs_draw2d_glsl)));
    bgfx::ShaderHandle fs = bgfx::createShader(bgfx::makeRef(fs_draw2d_glsl, sizeof(fs_draw2d_glsl)));
    g_program = bgfx::createProgram(vs, fs, true);
    g_sampler = bgfx::createUniform("s_tex", bgfx::UniformType::Sampler);
    // The fragment shader's alpha test (the material's ATST and AREF): the draws set it before every one, but it is
    // created here so the uniform always exists
    g_alpha = bgfx::createUniform("u_materialAlpha", bgfx::UniformType::Vec4);
    // The 1x1 white texture untextured draws sample (opaque white: the fragment shader's sample * colour
    // reduces to the vertex colour)
    const bgfx::Memory* memory = bgfx::copy("\xff\xff\xff\xff", 4);
    g_white = bgfx::createTexture2D(1, 1, false, 1, bgfx::TextureFormat::RGBA8, BGFX_TEXTURE_NONE, memory);
}
}

namespace Platform::Graphics
{
bgfx::ProgramHandle HostDraw2DProgram()
{
    MakeShared();
    return g_program;
}

bgfx::UniformHandle HostDraw2DSampler()
{
    MakeShared();
    return g_sampler;
}

bgfx::UniformHandle HostDraw2DAlphaUniform()
{
    MakeShared();
    return g_alpha;
}

bgfx::TextureHandle HostWhiteTexture()
{
    MakeShared();
    return g_white;
}
HostTextureCache::HostTextureCache()
{
}

namespace
{
// Whether two keys name the same source image (the transfer block plus the format and geometry telling reuses apart)
bool TextureKeysMatch(const HostTextureKey& first, const HostTextureKey& second)
{
    return first.transfer == second.transfer && first.pixelFormat == second.pixelFormat &&
           first.widthPower == second.widthPower && first.heightPower == second.heightPower &&
           first.levelOffset0 == second.levelOffset0 && first.paletteOffset == second.paletteOffset;
}

// The mip chain of a decoded image: level 0 as it is, each level after it the 2x2 average of the one before down to
// 1x1. The retail texture carries its own chain (the GS header's levels; the proof's 64x32 has 3), and a texture
// sampled without one aliases into per-pixel noise wherever it is minified -- the island's terrain and sky, drawn at
// a distance, came out as a field of random colours rather than a picture. The GS samples mips too when its shader's
// MMIN selects one (the shader settings' linear flag), so the host has to give it the chain
void BuildMipChain(const std::vector<u8>& rgba, u32 width, u32 height, std::vector<u8>& chain, u32* mipCount)
{
    chain = rgba;
    u32 levels = 1;
    u32 sourceWidth = width;
    u32 sourceHeight = height;
    std::vector<u8> source = rgba;
    while (sourceWidth > 1 || sourceHeight > 1)
    {
        const u32 destWidth = sourceWidth > 1 ? sourceWidth / 2 : 1;
        const u32 destHeight = sourceHeight > 1 ? sourceHeight / 2 : 1;
        std::vector<u8> dest(static_cast<size_t>(destWidth) * destHeight * 4);
        for (u32 y = 0; y < destHeight; y++)
        {
            for (u32 x = 0; x < destWidth; x++)
            {
                const u32 x0 = x * 2;
                const u32 y0 = y * 2;
                const u32 x1 = x0 + 1 < sourceWidth ? x0 + 1 : x0;
                const u32 y1 = y0 + 1 < sourceHeight ? y0 + 1 : y0;
                for (u32 channel = 0; channel < 4; channel++)
                {
                    const u32 sum = source[(static_cast<size_t>(y0) * sourceWidth + x0) * 4 + channel] +
                                    source[(static_cast<size_t>(y0) * sourceWidth + x1) * 4 + channel] +
                                    source[(static_cast<size_t>(y1) * sourceWidth + x0) * 4 + channel] +
                                    source[(static_cast<size_t>(y1) * sourceWidth + x1) * 4 + channel];
                    dest[(static_cast<size_t>(y) * destWidth + x) * 4 + channel] = static_cast<u8>(sum / 4);
                }
            }
        }

        chain.insert(chain.end(), dest.begin(), dest.end());
        source = std::move(dest);
        sourceWidth = destWidth;
        sourceHeight = destHeight;
        levels++;
    }

    *mipCount = levels;
}
}

HostTextureUpload HostTextureCache::GetOrUpload(const HostTextureKey& key, const Ps2TextureHeader* header,
                                                const u8* packet, size_t packetSize)
{
    HostTextureUpload miss = {BGFX_INVALID_HANDLE, false};
    for (const Entry& entry : entries)
    {
        if (entry.used && TextureKeysMatch(entry.key, key))
        {
            HostTextureUpload hit = {entry.texture, true};
            return hit;
        }
    }

    // The texture's one image (the 3D path's mips ride the vertex shader later)
    Ps2DecodedTexture decoded;
    if (!DecodePs2Texture(header, packet, packetSize, &decoded) || decoded.width > 0xFFFF ||
        decoded.height > 0xFFFF)
    {
        return miss;
    }

    // TS_TEXDUMP=<dir>: dumps each new upload as <dir>/tex<handle>_<w>x<h>.pam, for tracing a garbled picture to a
    // decode or a draw. The alpha is kept (a PPM has no room for it, and whether a cell holds a soft puff or a hard
    // shape is a question only the alpha can answer)
    static const char* dumpDir = std::getenv("TS_TEXDUMP");
    if (dumpDir != nullptr && *dumpDir != '\0')
    {
        char path[512];
        std::snprintf(path, sizeof(path), "%s/tex_%p_%ux%u.pam", dumpDir, key.transfer, decoded.width,
                      decoded.height);
        FILE* out = std::fopen(path, "wb");
        if (out != nullptr)
        {
            std::fprintf(out, "P7\nWIDTH %u\nHEIGHT %u\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n", decoded.width,
                          decoded.height);
            std::fwrite(decoded.rgba.data(), 1, decoded.rgba.size(), out);
            std::fclose(out);
        }
    }

    // The texture's mip chain. The console uploads the file's own levels (the MIPTBP registers and the level's own
    // offsets and widths), so a texture the file gives one level has no mips at all on the console: the particle
    // pages are such textures, and a puff read from a mip is a flat shape where the console's is soft dust. The
    // file's own levels are used as they are now; a chain has to reach 1x1 for bgfx to accept it, so levels past the
    // ones the file declares are the 2x2 average of the last level it does have (TS_NOMIPS leaves the chain off for
    // the A/B)
    static const bool noMips = std::getenv("TS_NOMIPS") != nullptr;
    std::vector<u8> chain;
    u32 mipCount = 1;
    u32 chainWidth = decoded.width;
    u32 chainHeight = decoded.height;
    if (!noMips && header->levels > 1)
    {
        if (!decoded.mips.empty())
        {
            // The file's own levels first, then the average of the last of them down to 1x1
            chain = decoded.rgba;
            mipCount = 1;
            for (const Ps2DecodedLevel& mip : decoded.mips)
            {
                chain.insert(chain.end(), mip.rgba.begin(), mip.rgba.end());
                chainWidth = mip.width;
                chainHeight = mip.height;
                mipCount++;
            }
            if (chainWidth > 1 || chainHeight > 1)
            {
                std::vector<u8> tail;
                u32 tailCount = 1;
                BuildMipChain(decoded.mips.back().rgba, decoded.mips.back().width, decoded.mips.back().height, tail,
                              &tailCount);
                // BuildMipChain repeats the level it was given as the chain's first image, so its tail skips it
                const size_t levelSize = static_cast<size_t>(decoded.mips.back().width) *
                                         decoded.mips.back().height * 4;
                chain.insert(chain.end(), tail.begin() + static_cast<long>(levelSize), tail.end());
                mipCount += tailCount - 1;
            }
        }
        else
        {
            BuildMipChain(decoded.rgba, decoded.width, decoded.height, chain, &mipCount);
        }
    }

    const std::vector<u8>& image = mipCount > 1 ? chain : decoded.rgba;
    const bgfx::Memory* memory = bgfx::copy(image.data(), static_cast<u32>(image.size()));
    // The GS's sampling wrap: the CLAMP register's reset value is REPEAT, and the retail level's own UVs run past
    // 1.0 (they tile a texture across a large face), so the 3D materials sample with the texture's own addressing
    // -- bgfx's default (U/V unset), which the reference's 3D strip path binds too (sdl_gfx.cpp BindTexture(...,
    // true) = GL_REPEAT). Clamping instead steals the tile and smears the edge texel over the whole face, which is
    // the flat, detail-less look level frames had. The 2D, particle, decal and screen-copy draws override this per
    // draw with BGFX_SAMPLER_U_CLAMP|V_CLAMP (their reference BindTexture clamps). Min/mag stay at bgfx's default
    // (linear), matching the 2D and 3D shaders' GS_TEX_LINEAR; the mip filter with it, so a minified texture reads
    // its chain rather than aliasing
    constexpr u64 SamplerState = BGFX_SAMPLER_MIP_POINT;
    bgfx::TextureHandle handle =
        bgfx::createTexture2D(static_cast<u16>(decoded.width), static_cast<u16>(decoded.height), mipCount > 1, 1,
                              bgfx::TextureFormat::RGBA8, SamplerState, memory);
    if (!bgfx::isValid(handle))
    {
        return miss;
    }

    // The resource's upload, held until its GameTexture dies: the game keeps hundreds live at once, so nothing is
    // ever evicted to make room (see host_graphics.h's HostTextureCache)
    Entry slot;
    slot.key = key;
    slot.texture = handle;
    slot.width = static_cast<u16>(decoded.width);
    slot.height = static_cast<u16>(decoded.height);
    slot.used = true;
    entries.push_back(slot);
    HostTextureUpload uploaded = {handle, false};
    return uploaded;
}

bgfx::TextureHandle HostTextureCache::Find(const void* transfer) const
{
    for (const Entry& entry : entries)
    {
        if (entry.used && entry.key.transfer == transfer)
        {
            return entry.texture;
        }
    }

    return BGFX_INVALID_HANDLE;
}

bool HostTextureCache::FindSize(const void* transfer, u32* width, u32* height) const
{
    for (const Entry& entry : entries)
    {
        if (entry.used && entry.key.transfer == transfer)
        {
            if (width != nullptr)
            {
                *width = entry.width;
            }
            if (height != nullptr)
            {
                *height = entry.height;
            }

            return true;
        }
    }

    return false;
}

void HostTextureCache::InvalidateTransfer(const void* transfer)
{
    // The disk manager reused the block: every upload decoded from its old bytes dies
    for (Entry& entry : entries)
    {
        if (entry.used && entry.key.transfer == transfer)
        {
            bgfx::destroy(entry.texture);
            entry.texture.idx = bgfx::kInvalidHandle;
            entry.used = false;
        }
    }
}

void HostTextureCache::Clear()
{
    for (Entry& entry : entries)
    {
        if (entry.used)
        {
            bgfx::destroy(entry.texture);
            entry.texture.idx = bgfx::kInvalidHandle;
            entry.used = false;
        }
    }
}

HostTextureCache& HostTextures()
{
    static HostTextureCache cache;
    return cache;
}

void HostSetMaterialTexture(HostMaterial* material, bgfx::TextureHandle texture)
{
    if (material == nullptr)
    {
        return;
    }
    if (bgfx::isValid(texture))
    {
        material->texture = texture.idx;
        material->flags |= HostMaterialTextured;
    }
    else
    {
        material->texture = HostInvalidHandle;
        material->flags &= ~static_cast<u32>(HostMaterialTextured);
    }
}

bgfx::TextureHandle HostMaterialBoundTexture(const HostMaterial* material)
{
    bgfx::TextureHandle texture = HostMaterialTexture(material);
    if (!bgfx::isValid(texture) && material != nullptr && material->gameTexture != nullptr)
    {
        texture = HostTextures().Find(material->gameTexture);
    }
    return texture;
}

namespace
{
// The GS's ZTST as bgfx's depth test, the same order the working SDL2 port uses (its DepthFunc3D: the two depths
// run the other way about -- the GS's 0 is the far plane, the host's 0 the near one -- so the retail opaque test
// (ZTST 2, every material the game ships) is the host's plain less). ZTST 1 is the sky's and the shadow's own pass
// (no depth test at all)
u64 DepthState(u8 method)
{
    // TS_DEPTHFLIP: uses the material's comparison as it is, testing whether the GS's and the host's depth conventions
    // run the same way rather than opposite (the GS's ZTST 2, GEQUAL, stays GEQUAL)
    static const bool flipped = std::getenv("TS_DEPTHFLIP") != nullptr;
    if (flipped)
    {
        switch (method)
        {
        case 0:
            return BGFX_STATE_DEPTH_TEST_NEVER;
        case 1:
            return BGFX_STATE_DEPTH_TEST_ALWAYS;
        case 2:
            return BGFX_STATE_DEPTH_TEST_GEQUAL;
        case 3:
            return BGFX_STATE_DEPTH_TEST_GREATER;
        case 4:
            return BGFX_STATE_DEPTH_TEST_EQUAL;
        case 5:
            return BGFX_STATE_DEPTH_TEST_NOTEQUAL;
        case 6:
            return BGFX_STATE_DEPTH_TEST_LESS;
        case 7:
            return BGFX_STATE_DEPTH_TEST_LEQUAL;
        default:
            return BGFX_STATE_DEPTH_TEST_GEQUAL;
        }
    }

    switch (method)
    {
    case 0:
        return BGFX_STATE_DEPTH_TEST_NEVER;
    case 1:
        return BGFX_STATE_DEPTH_TEST_ALWAYS;
    case 2:
        // GEQUAL counts an equal depth, so its mirror does too: LESS would drop the case a face drawn onto an existing
        // surface (a decal, a second layer, an overlay) needs
        return BGFX_STATE_DEPTH_TEST_LEQUAL;
    case 3:
        return BGFX_STATE_DEPTH_TEST_LESS;
    case 4:
        return BGFX_STATE_DEPTH_TEST_EQUAL;
    case 5:
        return BGFX_STATE_DEPTH_TEST_NOTEQUAL;
    case 6:
        return BGFX_STATE_DEPTH_TEST_GREATER;
    case 7:
        return BGFX_STATE_DEPTH_TEST_GEQUAL;
    default:
        return BGFX_STATE_DEPTH_TEST_LESS;
    }
}

// The blend a material's preset asks for, as the host's factors: the GS ALPHA formula InitAlphaPresets writes
// (A - B)·C + D over the source and destination colours and the source alpha), turned into bgfx's source and
// destination factors. All six presets are of the form Cs·(As) + Cd·(1 - As) with the two coefficients the source
// and destination factors: the mix is As/1-As, the add As/1, the subtract 0/1 with the blend equation reversed
// (Cd - Cs·As), the darken 0/1-As, the scale 0/As, the brighten 1-As/1 (the host's factor can't make a source
// colour, so the brighten draws what the PS2's mix does rather than the retail Cd - Cs, exactly as the working SDL2
// port does). A preset past the table (or the shader's own formula, which the host doesn't reproduce) is the mix
u64 PresetBlend(u8 preset)
{
    switch (preset)
    {
    case HostPresetAdd: // Cs·As + Cd
        return BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_SRC_ALPHA, BGFX_STATE_BLEND_ONE);
    case HostPresetSubtract: // Cd - Cs·As: the source subtracted from the destination
        return BGFX_STATE_BLEND_EQUATION(BGFX_STATE_BLEND_EQUATION_REVSUB) |
               BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_SRC_ALPHA, BGFX_STATE_BLEND_ONE);
    case HostPresetBrighten: // Cd - Cs·As (the host reaches the add: the reference's own mapping)
        return BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_SRC_ALPHA, BGFX_STATE_BLEND_ONE);
    case HostPresetDarken: // Cd·(1 - As)
        return BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_ZERO, BGFX_STATE_BLEND_INV_SRC_ALPHA);
    case HostPresetScale: // Cd·As
        return BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_ZERO, BGFX_STATE_BLEND_SRC_ALPHA);
    default: // the mix, and everything else: Cs·As + Cd·(1 - As)
        return BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_SRC_ALPHA, BGFX_STATE_BLEND_INV_SRC_ALPHA);
    }
}
}

HostMaterialState HostMaterialStateOf(const HostMaterial* material, u64 baseState, bool withDepth)
{
    // A material whose shader the host read carries its GS settings; one it made itself (the UI's flat material, a
    // particle page's mode) leaves them zero, and the pass's own state stands (the GS defaults: opaque, no alpha
    // test, the gequal depth test every shader starts with, ShaderConstruct)
    const bool hasSettings = material != nullptr && (material->flags & HostMaterialSettings) != 0;
    if (!hasSettings)
    {
        HostMaterialState passed;
        // A material the host made itself has no settings block but does carry its blend mode (host_graphics.h's blend:
        // the UI's 2D particles, which retail builds with the additive preset; materials.cpp's MakeParticleMaterial sets
        // shader->settings.preset = PresetAdd). Without this the 2D path drew them with the pass's plain alpha blend, so a
        // sparkle covered what was behind it instead of adding to it: solid bright blobs where the console glows
        u64 state = baseState;
        switch (material != nullptr ? material->blend : 0)
        {
        case HostParticleBlendAdditive:
            state = (state & ~BGFX_STATE_BLEND_MASK) | PresetBlend(HostPresetAdd);
            break;
        case HostParticleBlendSubtractive:
            state = (state & ~BGFX_STATE_BLEND_MASK) | PresetBlend(HostPresetSubtract);
            break;
        case HostParticleBlendCutout:
            state &= ~BGFX_STATE_BLEND_MASK;
            break;
        default:
            break;
        }
        passed.state = state;
        passed.alphaReference[0] = static_cast<f32>(HostAlphaAlways);
        passed.alphaReference[1] = 0.0f;
        passed.alphaReference[2] = 0.0f;
        passed.alphaReference[3] = 0.0f;
        return passed;
    }

    const u8* settings = material->shaderSettings;
    const u8 blends = settings[HostShaderSettingBlends];
    const u8 preset = settings[HostShaderSettingPreset];
    const u8 ownAlpha = settings[HostShaderSettingOwnAlpha];
    const u8 alphaTest = settings[HostShaderSettingAlphaTest];
    const u8 alphaMethod = settings[HostShaderSettingAlphaMethod];
    const u8 alphaReference = settings[HostShaderSettingAlphaReference];
    const u8 depthTest = settings[HostShaderSettingDepthTest];
    const u8 noDepthWrites = settings[HostShaderSettingNoDepthWrites];
    // The destination alpha test and its mode (the GS's DATE and DATM: the skid marks' material asks for DATE=1, DATM=0).
    // It gates a pass on the alpha the frame buffer already holds, which no bgfx state can read, so the host runs no test
    // of its own for it: the destination the PS2 tests is the scene the gated strip is drawn over, and the host's
    // stand-in for that gate is the depth test the same material's own ZTST asks for -- a DATE-gated material keeps it
    // even in a pass that has no depth test of its own, leaving the strip nowhere to land but the scene. DATE's AREF is
    // deliberately not turned into the fragment alpha test: the GS shares that field with its alpha test, and the marks'
    // fragments are gated by their own alpha through the material's blend preset instead. DATM 0 and 1 choose the
    // polarity of the comparison against the destination; the game asks for 0 and neither polarity reaches a host state
    const u8 destinationTest = settings[HostShaderSettingDestinationTest];
    const u8 destinationMode = settings[HostShaderSettingDestinationMode];
    const bool destinationGated = destinationTest != 0;
    (void)destinationMode;

    // The caller's pass state over the material's own: the blend (or none when the shader doesn't blend), the depth
    // test and its write, so an opaque shader leaves the state the pass set and a blended one is drawn as the GS
    // would
    u64 state = baseState & ~(BGFX_STATE_BLEND_MASK | BGFX_STATE_BLEND_EQUATION_MASK);
    if (withDepth || destinationGated)
    {
        state &= ~BGFX_STATE_DEPTH_TEST_MASK;
        state |= DepthState(depthTest);
    }
    if (blends != 0)
    {
        // A shader with its own alpha formula (ownAlpha) has no preset of its own; the host has no equivalent of the
        // GS's A/B/C/D selectors, so it falls back to the mix, exactly as the working SDL2 port does
        state |= PresetBlend(ownAlpha != 0 ? static_cast<u8>(HostPresetMix) : preset);
    }
    else
    {
        // Opaque: no blend at all (the alpha one the 2D pass sets would leave a semi-transparent material showing
        // through)
        state &= ~BGFX_STATE_BLEND_MASK;
    }
    if (noDepthWrites != 0)
    {
        state &= ~BGFX_STATE_WRITE_Z;
    }
    // The GS's FBA, byte 24 inverted: "noFba" clear means the alpha channel IS written to the framebuffer, and set
    // means it is not (TEST's FBA bit). The shadow pass marks its casters through that same alpha, so a material that
    // leaves it off on the console must not mark here either
    if (settings[24] != 0)
    {
        state &= ~BGFX_STATE_WRITE_A;
    }

    // The alpha test the fragment shaders run (bgfx has no fixed-function one): the material's ATST and AREF, or
    // ALWAYS when it has none, so an unset uniform's never comparison is never even reached
    HostMaterialState result;
    result.state = state;
    result.alphaReference[0] = alphaTest != 0 ? static_cast<f32>(alphaMethod) : static_cast<f32>(HostAlphaAlways);
    result.alphaReference[1] = static_cast<f32>(alphaReference) / 128.0f;
    result.alphaReference[2] = 0.0f;
    // The alpha test's failure action (AFAIL), carried for the draws' second pass over the rejected samples
    result.alphaReference[3] = static_cast<f32>(settings[5]);
    result.alphaReference[3] = 0.0f;
    return result;
}


void HostSetMaterialTextureSize(HostMaterial* material, bgfx::TextureHandle texture, u16 width, u16 height)
{
    HostSetMaterialTexture(material, texture);
    if (material == nullptr)
    {
        return;
    }
    material->textureWidth = width;
    material->textureHeight = height;
}

void ConstructMaterial(Material* material)
{
    auto* host = reinterpret_cast<HostMaterial*>(material);
    std::memset(host, 0, MaterialStorage);
    host->program = InvalidHandle;
    host->sampler = InvalidHandle;
    host->texture = InvalidHandle;
}

void DestroyMaterial(Material* material)
{
    // The program, sampler and white texture are process singletons shared by every material (torn down with
    // bgfx itself): freeing a material releases its references, which is zeroing its storage
    std::memset(material, 0, MaterialStorage);
}

Material* MakeFlatMaterial()
{
    auto* material = reinterpret_cast<Material*>(g_flatStorage);
    ConstructMaterial(material);
    auto* host = reinterpret_cast<HostMaterial*>(material);
    MakeShared();
    host->program = g_program.idx;
    host->flags = HostMaterialFlat;
    host->sampler = g_sampler.idx;
    host->texture = g_white.idx;
    // The UI's bucket, as retail's MakeFlatMaterial names it (materials.cpp: g_FlatMaterial.bucket = BucketUi): the 2D
    // draws follow the material's bucket now, so the flat material's quads and strips land in the UI's view
    host->bucket = 24;
    return material;
}

Material* FlatMaterial()
{
    return reinterpret_cast<Material*>(g_flatStorage);
}
}
