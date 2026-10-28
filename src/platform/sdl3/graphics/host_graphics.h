// Host graphics internals: the layout the game's 0x70-byte material storage holds and the
// shared 2D objects the draws submit through. Only the SDL3 platform's own files include this; the game sees the
// opaque Material through include/platform/graphics.h.
#pragma once

#include "common.h"
#include "platform/graphics.h"
#include "bgfx_context.h"
#include "texture_decoder.h"

#include <bgfx/bgfx.h>

#include <vector>

namespace Platform::Graphics
{
// How many of a shader's setting bytes the host keeps (the file's base shader: the reading's 30 fields, the same
// bytes shaderclasses.cpp's ReadShader reads into ShaderSettings)
constexpr u32 HostShaderSettings = 30;
// The setting bytes the draws read (renderer.h's ShaderSettings: byte N of the union's u64 is field N, so these
// offsets are the bitfield's own). The rest of the 30 (textured, STQ, fog, the scrolls, linear, the animation, AFAIL
// and the unused fields) is either not a blend/depth state or has no host equivalent
constexpr u32 HostShaderSettingBlends = 0;
constexpr u32 HostShaderSettingPreset = 1;
constexpr u32 HostShaderSettingAlphaTest = 2;
constexpr u32 HostShaderSettingAlphaMethod = 3;
constexpr u32 HostShaderSettingAlphaReference = 4;
// The destination alpha test and its mode (the GS's DATE and DATM; ordinal 5 is AFAIL's own byte, which the host never
// reads). MakeSkidMaterial's shader is the one host material that asks for them (DATE 1, DATM 0) and HostMaterialStateOf
// maps them as far as the host can
constexpr u32 HostShaderSettingDestinationTest = 6;
constexpr u32 HostShaderSettingDestinationMode = 7;
constexpr u32 HostShaderSettingDepthTest = 8;
// Gouraud shading (IIP): the marks' material carries the 1 ShaderConstruct leaves, the host's 3D program always shading
// with the vertexes' colours
constexpr u32 HostShaderSettingGouraud = 10;
constexpr u32 HostShaderSettingOwnAlpha = 17;
constexpr u32 HostShaderSettingAlphaA = 18;
constexpr u32 HostShaderSettingAlphaB = 19;
constexpr u32 HostShaderSettingAlphaC = 20;
constexpr u32 HostShaderSettingAlphaD = 21;
constexpr u32 HostShaderSettingAlphaFix = 22;
// The 30 setting bytes are one byte per ShaderSettings field (shaderclasses.cpp's ReadShader): blends 0, preset 1,
// alphaTest 2, alphaMethod 3, alphaReference 4, alphaFail 5, destinationTest 6, destinationMode 7, depthTest 8,
// unused 9, gouraud 10, textured 11, stq 12, fog 13, secondContext 14, uScroll 15, vScroll 16, ownAlpha 17,
// alphaA-D 18-21, alphaFix 22, linear 23, noFba 24, unused 25, antiAliased 26, noDepthWrites 27,
// animatedColour 28, hasAnimation 29.
constexpr u32 HostShaderSettingLinear = 23;
constexpr u32 HostShaderSettingNoDepthWrites = 27;
// The scroll modes (renderer.h's ShaderScroll: 0 none, 1 the animation's offset, 2 a phase going round, 3 its sine)
// and the animated-colour flag
constexpr u32 HostShaderSettingUScroll = 15;
constexpr u32 HostShaderSettingVScroll = 16;
constexpr u32 HostShaderSettingAnimatedColour = 28;
constexpr u32 HostShaderSettingHasAnimation = 29;

// The GS's alpha presets (renderer.h's AlphaPreset, InitAlphaPresets' table: (A - B)·C + D over the GS's ALPHA
// selectors). The draws turn one into bgfx factors: the source and destination colours, and the source alpha
enum HostAlphaPreset : u8
{
    HostPresetMix = 0,       // Cs·As + Cd·(1 - As)
    HostPresetAdd = 1,       // Cs·As + Cd
    HostPresetSubtract = 2,  // Cd - Cs·As
    HostPresetBrighten = 3,  // Cd - Cs·As (the host reaches the mix)
    HostPresetDarken = 4,    // Cd·(1 - As)
    HostPresetScale = 5,     // Cd·As
};

// The GS's ATST as the fragment shaders' u_materialAlpha comparison, in the GS's own numbering (renderer.h reads
// the field straight from the file and the GS uses it as its test): 0 never, 1 always, 2 less, 3 less or equal,
// 4 equal, 5 greater or equal, 6 greater, 7 not equal
enum HostAlphaMethod : u32
{
    HostAlphaNever = 0,
    HostAlphaAlways = 1,
    HostAlphaLess = 2,
    HostAlphaLessEqual = 3,
    HostAlphaEqual = 4,
    HostAlphaGreaterEqual = 5,
    HostAlphaGreater = 6,
    HostAlphaNotEqual = 7,
};

// How many shaders a material may carry (renderer.h's MaxMaterialShaders: the retail reader fills at most four)
constexpr u32 HostMaterialPasses = 4;

// The host material: what ConstructMaterial fills the 0x70 bytes of storage with ({u32 program; flags; uniforms}:
// the program's handle, the kind flags and the sampler + texture uniforms' handles; invalid handles are 0xFFFFFFFF)
struct HostMaterial
{
    u32 program;
    u32 flags;
    u32 sampler;
    u32 texture;
    // The particle pages' blend mode (HostParticleBlend*: the GS preset their page materials draw with); unused by
    // the rest
    u32 blend;
    // The material's render bucket from its file (renderer.h's FrameBucket: 2 opaque, 3 the startup chunk's objects and
    // the skid marks, 5..19 blended, 20 decals, 22 particles, 23 distortion, 24 UI, 26 fonts, 27 movies). The 3D and 2D
    // draws put a material's draws in the bgfx view that stands for its bucket, which is what keeps the game's draw
    // order: retail writes each material's packets into its bucket's DMA chain and the chains run in bucket order,
    // where the host's views do the same job. 0 when the file named none (a page material, a make-* material): the
    // draws then fall back to the opaque bucket
    u32 bucket;
    // The texture's size in texels (the particles' and decals' texture rectangles are fractions of it); 0 until it
    // is set with the texture
    u16 textureWidth;
    u16 textureHeight;
    // The game texture the material's first textured shader samples (ReadMaterial stashes it; SubmitQuad resolves it
    // through HostTextures, its upload having been made when ReadTexture read it). nullptr when its shaders
    // reference none: the draws then leave the white texture bound
    GameTexture* gameTexture;
    // The material's first shader's scroll state (renderer.h's Shader::scrollPhases/scrollSpeeds/scroll): the phase
    // the mode's own function advances each frame, the speed it advances by, and the resulting texture offset the
    // draws add to every UV. ReadMaterial takes them; StepAnimations advances them (the retail
    // AnimateMaterials -> UpdateShader path); the 3D draws upload scroll to the shader. The floats come before the
    // byte arrays so the struct needs no padding (the reserved tail is sized to the exact remainder)
    f32 scrollPhases[2];
    f32 scrollSpeeds[2];
    f32 scroll[2];
    // The material's first shader's GS settings, byte for byte (the file's 30 setting bytes: HostShaderSettings),
    // taken while the material is read (ReadMaterialShaders). The draws map them onto the bgfx state before each
    // submodel's strip, so an opaque shader writes its colour as it is, a blended one mixes it and an alpha-tested
    // one drops its samples (the set state the PS2's GS reads from the same bytes)
    u8 shaderSettings[HostShaderSettings];
    // Straight off the settings, for the draws that only need one field: the GS's ABE (does the shader blend at
    // all) and its ZTST (0 never, 1 always, 2 gequal, 3 greater). Both 0 until ReadMaterial stashes the shader,
    // which an opaque material without a depth test is not (HostMaterialDepthState's default has the test on)
    u8 blends;
    u8 depthTest;
    // A font page's material: which page of its font it draws (SetFontPage, the retail Shader::fontPage). Each page has a
    // material and a texture of its own, so the glyph draws find the page's texture through gameTexture
    u8 fontPage;
    // The material's first shader's type (the material file's leading s32 of each shader): the retail's shader classes
    // pick the VU1 program and the per-type behaviour (needsEye, billboard, the vertex layout) by it, so the draws
    // can tell a shader type apart from the one the host's programs were written for
    u8 shaderType;
    // A material whose file carries more than one shader: the console draws every one of them (materials.cpp's
    // FlushMaterial -> WriteShaders), each pass sampling the first shader's texture. Only the fields a later pass
    // differs by are kept, because the game's material storage is a fixed 0x70 bytes.
    u8 passCount;
    u8 passBlends[HostMaterialPasses - 1];
    u8 passPreset[HostMaterialPasses - 1];
    u8 passDepth[HostMaterialPasses - 1];
    u8 passFlags[HostMaterialPasses - 1];   // bit 0: noDepthWrites, bit 1: alphaTest
    // The first shader's alpha-failure action (the file's setting byte 5: the GS's AFAIL, 0 KEEP, 1 FB_ONLY,
    // 2 ZB_ONLY, 3 RGB_ONLY). The fragment shader drops a failing sample, which is KEEP; FB_ONLY instead writes its
    // colour and leaves the depth, so the draws issue a second pass for it
    u8 alphaFail;
    // The first shader's own colour (renderer.h's Shader::shaderColour, the file's four floats) as bytes: the tint a
    // type-0x1F material multiplies its lit vertex colour by (that type's VU1 program loads the colour words, ITOF0's
    // them, multiplies them in and clamps). White for a file that names none, and unused by every other type
    u8 colour[4];
    // The 4-aligned fields end at the scroll block; the bytes after it are these and the tail
    static constexpr u32 HostFixedBytes = 6 * sizeof(u32) + 2 * sizeof(u16) + sizeof(GameTexture*) +
                                          sizeof(f32) * 6 + HostShaderSettings + 4 + 1 + 4 * (HostMaterialPasses - 1) + 1 +
                                          sizeof(u8) * 4;
    u8 reserved[MaterialStorage - HostFixedBytes];
};
static_assert(sizeof(HostMaterial) == MaterialStorage, "a host material fills the game's material storage");

// The sampler flags a material's draw takes: the GS's TEX1 takes the magnifying and the minifying filter from the
// shader's "linear" setting (the retail's TextureFiltering: MMAG and MMIN both take it, the minifying one the mipmap
// variant when the type minifies through its mips), so a material that asks for it is sampled bilinearly and one that
// does not is sampled by the nearest texel. The mip level itself is the nearest (the console's minifying filter takes
// the nearest level and blends inside it).
//
// The particle pages' material asks for it, and it matters there: a puff is a small region of the page blown up, and
// the nearest texel shows the region's own texels as hard blocks (the mist's streaks, the nitro's spikes) where the
// console's blend makes the soft round shape the data holds
inline u64 HostMaterialFilter(const HostMaterial* material)
{
    const bool linear = material != nullptr && (material->shaderSettings[HostShaderSettingLinear] & 1) != 0;
    return linear ? BGFX_SAMPLER_MIP_POINT
                  : (BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT | BGFX_SAMPLER_MIP_POINT);
}

// Flags of the host material: whether it samples a texture (real textures land with the texture upload; until then
// the white texture stands in), whether it's the UI's flat material, and whether its GS settings were captured (a
// material ReadMaterial read; the host's own flat/particle materials leave them unset and draw with the pass's state)
enum HostMaterialFlags : u32
{
    HostMaterialTextured = 1 << 0,
    HostMaterialFlat = 1 << 1,
    HostMaterialSettings = 1 << 2,
    // The material's shader makes its model a billboard (shaderclasses.cpp: the type-0x1B shader's ShaderMakesBillboard,
    // the retail ReadRigidModelData's AnyShader(model, ShaderBillboardSlot)): the placed draw turns the model to face the
    // camera. The flag is taken while the material's shaders are read, where the shader's type still is
    HostMaterialBillboard = 1 << 3,
};

// The blend a particle page's material draws with (ParticleBlendMode, include/game/particles.h: additive,
// subtractive, the page material's own and cutout)
enum HostParticleBlend : u32
{
    HostParticleBlendPage = 0,
    HostParticleBlendAdditive = 1,
    HostParticleBlendSubtractive = 2,
    HostParticleBlendCutout = 3,
};
// No valid bgfx handle has this index; stored in HostMaterial while it holds nothing
constexpr u32 HostInvalidHandle = 0xFFFFFFFF;

// A cached upload's key: the transfer bytes' identity plus the format and geometry telling reuses of the same bytes
// apart (a reallocated block under the same address, the same image under another palette). The GS slot's address is
// never a member: the ring aliases many textures onto few slots
struct HostTextureKey
{
    const void* transfer;
    u8 pixelFormat;
    u16 widthPower;
    u16 heightPower;
    u32 levelOffset0;
    u32 paletteOffset;
};

// A cached upload's result: the bgfx handle and whether it was already in the cache
struct HostTextureUpload
{
    bgfx::TextureHandle texture;
    bool hit;
};

// The host's texture uploads: one RGBA8 bgfx texture per GameTexture, made when its bytes are read and held until
// the GameTexture dies. Nothing is evicted to make room. The game keeps hundreds of textures live at once, so a
// fixed-size cache would destroy uploads that queued draws still reference, leaving flat untextured scenery.
class HostTextureCache
{
public:
    HostTextureCache();
    HostTextureUpload GetOrUpload(const HostTextureKey& key, const Ps2TextureHeader* header, const u8* packet,
                                  size_t packetSize);
    // The upload of a key's transfer bytes (invalid when there is none): how the draws find a material's texture
    // again, its key's transfer being the GameTexture*
    bgfx::TextureHandle Find(const void* transfer) const;
    // The texels of a key's uploaded texture (false when there is none). The font pages' glyph rectangles are in the
    // page's texels, so the text drawing needs the size as well as the handle
    bool FindSize(const void* transfer, u32* width, u32* height) const;
    void InvalidateTransfer(const void* transfer);
    void Clear();

private:
    // The uploads held, one per live GameTexture (grows with the game's texture set; shrinks as one is freed)
    struct Entry
    {
        HostTextureKey key;
        bgfx::TextureHandle texture;
        u16 width;
        u16 height;
        bool used;
    };
    std::vector<Entry> entries;
};

// The uploads (used once bgfx is up, like the shared 2D objects)
HostTextureCache& HostTextures();

// Points a material at an upload: its texture handle with the textured flag (SubmitQuad samples it, falling back to
// the white texture without the flag or with no handle stored)
void HostSetMaterialTexture(HostMaterial* material, bgfx::TextureHandle texture);

// The same with the texture's size in texels, which the particles' and decals' texture rectangles are fractions of
// (the page load knows it; a material without one samples the whole texture)
void HostSetMaterialTextureSize(HostMaterial* material, bgfx::TextureHandle texture, u16 width, u16 height);

// The texture a material samples (invalid without the textured flag or with no handle stored)
inline bgfx::TextureHandle HostMaterialTexture(const HostMaterial* material)
{
    bgfx::TextureHandle handle = BGFX_INVALID_HANDLE;
    if (material != nullptr && (material->flags & HostMaterialTextured) != 0 &&
        material->texture != HostInvalidHandle)
    {
        handle.idx = static_cast<u16>(material->texture);
    }
    return handle;
}

// The texture a draw binds for a material (invalid when it samples none): its stored handle, else the upload
// HostTextures holds for the game texture its first textured shader reads (ReadTexture's). SubmitQuad binds the
// white texture in its place when this is invalid
bgfx::TextureHandle HostMaterialBoundTexture(const HostMaterial* material);

// A material's state as the 3D/2D draws set it (BgfxStateOfMaterial): the bgfx state flags a draw with the material
// submits, its alpha test's comparison and reference (the fragment shader's u_materialAlpha) and its pass state the
// caller keeps (whether the belt-and-braces alpha state is on). The draws build one of these from each submodel's
// material, so a plain material leaves the caller's own state as it is and a blended, alpha-tested or depth-masked
// one overrides it
struct HostMaterialState
{
    // The bgfx state the draw submits (the caller's pass state with the material's blending, depth test and write
    // and culling folded in)
    u64 state;
    // The alpha test: x the comparison (HostAlphaMethod), y the reference the sample's alpha is tested against (the
    // GS's AREF / 128, the game's convention full at 1). The draws set these as the u_materialAlpha uniform; a
    // material without an alpha test gets HostAlphaAlways, whose comparison drops nothing whatever the reference
    f32 alphaReference[4];
};

// The state a submodel's material draws with: its first shader's settings (the material's shaderSettings, or a
// default when it has none) mapped onto the bgfx state the caller's pass already has (baseState: a pass's write and
// cull flags, kept where the reference has no counterpart). The alpha test rides the fragment shader's
// u_materialAlpha because bgfx has no fixed-function one. withDepth tells whether the pass depth-tests (the 3D
// passes do, the 2D one doesn't: a UI quad has no depth to test and only its blending and alpha test apply)
HostMaterialState HostMaterialStateOf(const HostMaterial* material, u64 baseState, bool withDepth);
// The screen effects' fullscreen filter goes through bgfx view 4 (the ScreenEffects bucket), kept sequential so
// the filter's pass draws in submission order over the frame. Its readback blits on view 34 (bgfx
// runs a view's blits before its draws, so the blit needs a later view than the filter's pass; 34 keeps it out of the
// buckets' range, where 26 is the fonts')
constexpr bgfx::ViewId HostScreenFxView = 4;
constexpr bgfx::ViewId HostScreenFxBlitView = 34;
// The scene fog's own view: retail draws the colour filter in the ScreenEffects bucket (FrameBucket: 4), right after
// the opaque buckets (2, 3) and before the blended buckets, the decals, the shadows, the particles and the UI, so the
// filter tints the opaque scene and the sky and everything after draws over it untinted. Retail's FilterColours and the
// gem filter are one bucket but the host draws them into different targets (the frame and the filter's own proof
// buffer), so the fog takes a view of its own, 32, which OrderScreenViews places between 4 and 5 -- outside the
// buckets' 0..27 range, which is now one to one with the retail numbering
constexpr bgfx::ViewId HostSceneFogView = 32;
// The filter's palette bank: the game's eight screen-effect palettes of 256 RGBA8 colours (the chunks'
// colourFilterPalette selects one as the screen-effect id; effect.cpp's FilterPaletteCount)
constexpr u32 ScreenFxPaletteCount = 8;
constexpr u32 ScreenFxPaletteColours = 0x100;

// The bank's entry uploaded (its colours kept for reselection), the game's screen-effect id selected (the live
// entry's upload follows it), and the live id read back
void ScreenFxUploadPalette(u32 palette, const u32* colours);
void ScreenFxSelectEffect(u32 palette);
u32 ScreenFxSelectedEffect();
// The depth pre-pass's upload: normalized depths (the PS2's CopyDepthBuffer as R32F), the filter's source
void ScreenFxUploadDepth(u32 width, u32 height, const f32* depths);
// The colour filter's fullscreen pass over the uploaded depth through the selected palette (FinishScene's work
// when the game asks for effects)
void ScreenFxDrawFilter();
// The filter's output read back as RGBA8 rows (false when nothing was drawn or the sizes mismatch)
bool ScreenFxReadOutput(u32 width, u32 height, u8* out);

// The particles' bgfx view (the Particles bucket): the page materials' quads in submission order, keeping the
// game's own draw order
constexpr bgfx::ViewId HostParticlesView = 22;
// The distorting hexagons' view (the Distortion bucket): the hexagons sampling the frame copy
constexpr bgfx::ViewId HostDistortionView = 23;
// The decals' view (the Decals bucket): the aged decals' quads through their type materials
constexpr bgfx::ViewId HostDecalsView = 20;

// A particle block's host header: the 0x20 bytes the game's block spends on the DMA packet (Platform::
// ParticleBlockBytes / HexagonBlockBytes). The game's ParticleRecords follow at ParticleBlockHeader (0x20), which
// the game writes and reads itself; only this header is the platform's
constexpr u32 HostParticleBlockMagic = 0x31544250; // 'PBT1'
struct HostParticleBlockHead
{
    // The chain's DMA tag words the game's ChainParticleBlocks/EndParticleBlock keep: [0]'s id (NextTag/ReturnTag)
    // says whether another block follows, [1] is the next block's address
    u32 tag[2];
    u32 magic;    // HostParticleBlockMagic
    u32 hexagons; // 1: a hexagon block (12 records), 0: a particle block (32)
    u32 count;    // how many records of the block the game uses (32 or 12)
    u32 unused[3];
};
static_assert(sizeof(HostParticleBlockHead) == 0x20, "a particle block's header is its packet");

// A render table's host form (Platform::ParticleRenderTableBytes): the look the game builds (a particle system's
// curves at each of the 64 steps of a particle's life) as WriteParticleRenderTable leaves it
constexpr u32 HostParticleTableMagic = 0x31545250; // 'PRT1'
struct HostParticleStep
{
    f32 shape[6];  // the quad's corner, the edge from it and the far corner
    u8 colour[4];  // the texture's modulation at this step (128 leaves it as it is)
    u8 unused[4];
};
struct HostParticleTable
{
    u32 magic;
    u32 hexagons;            // DrawsHexagons of the system the table was built for
    f32 gravity;
    s32 texture[4];          // the texture's rectangle in whole pixels (start x, start y, end x, end y)
    f32 hexagonCorners[12];  // the hexagons' six corners (a quarter long)
    f32 time;                // the particles' clock of the last draw
    HostParticleStep steps[Platform::Graphics::ParticleRenderSteps];
};
static_assert(sizeof(HostParticleTable) <= Platform::Graphics::ParticleRenderTableBytes,
              "a host render table fits the game's table");

// A vertex of a host particle/decal/distortion draw (the draws build these on the CPU and submit them, so a
// headless run can read back exactly what a frame drew)
struct HostFxVertex
{
    f32 x;
    f32 y;
    f32 z;
    f32 s;
    f32 t;
    u32 colour;
};

// The vertices and an FNV-1a checksum of them from the last draw of each kind (for the headless proof;
// particles_host.cpp and effects.cpp keep them)
u32 HostFxDebug_ParticleVertices();
u32 HostFxDebug_ParticleChecksum();
// The last draw's first record's projected place (its clip-space columns, w the projection's), what the placement
// proof reads
Vector4 HostFxDebug_ParticlePlace();
// The last draw's first record's modulate colour (the GS's: the colour's channels over 255 and the alpha over 128,
// the alpha held to what the GS's alpha register takes), what the colour proof reads
Vector4 HostFxDebug_ParticleColour();
u32 HostFxDebug_DecalVertices();
u32 HostFxDebug_DecalChecksum();
u32 HostFxDebug_DistortionVertices();
u32 HostFxDebug_DistortionChecksum();

// The display the game's screen coordinates are measured against (StartFrame's screen size: the units the
// particles' shapes and the game's to-screen matrices are in; the particles' draw needs it, the mid-point of those
// coordinates being 2048 in 1/16th-pixel units)
void HostSetDisplaySize(u32 width, u32 height);
u32 HostDisplayWidth();
u32 HostDisplayHeight();

// How many frames FinishFrame has presented (the debug probes' gate: a per-draw log can pick one frame of a scene)
u32 HostFrameCounter();

// The distortion's frame copy: the image its hexagons sample, made from the frame's pixels (RGBA8, rows of pitch
// bytes; the PS2's type 0x18 copies the frame first). Owned by the process, remade when the size changes
void HostDistortionFrameCopy(u32 width, u32 height, const void* rgba, u32 pitch);
bgfx::TextureHandle HostDistortionFrameTexture();

// The particles' wave (the PS2's type 0x1C shader: speed 1 and amplitude 1.5 at start-up): its sixteen rows of
// three wave phases (ShaderWaveRows), moved on by the frame's seconds and read back as their sines
constexpr u32 HostWaveRows = 16;
void HostParticleWaveInit(f32 speed, f32 amplitude);
void HostParticleWaveUpdate(f32 seconds);
f32 HostParticleWaveAmplitude();
void HostParticleWaveSines(f32* out /* HostWaveRows * 3 */);

// The UI's 2D drawing goes through bgfx view 24 (the UI's bucket), kept sequential so the
 // quads draw in submission order over the frame view 0 cleared
 constexpr bgfx::ViewId HostDraw2DView = 24;

// The shadows' pass (R6): a half the screen's size offscreen alpha+depth target the shadow volumes draw into,
// resolved over the frame. The resolve is the documented Shadows bucket, view 21; the volumes' offscreen pass
// takes view 25 (the third chain's spare -- view 1 is the sky's copy) and shadows.cpp calls bgfx::setViewOrder
// once to run it just before 21: a bgfx view has one framebuffer, so the offscreen pass and the resolve cannot
// share a view, and the volumes must finish before the resolve samples them
constexpr bgfx::ViewId HostShadowVolumeView = 25;
constexpr bgfx::ViewId HostShadowView = 21;
// The palette-equivalent LUT's entries: the 256 the GS palette holds (white, the alpha ramp 255 down to 0)
constexpr u32 HostShadowLutEntries = 0x100;

// The LUT as SetUpShadowPass uploaded it (entry i is white with alpha 255 - i); null before the set-up ran
const u32* HostShadowLut();

// The frame the resolve darkens, with its size: the shadow pass sizes its offscreen targets and its overdraw by it (its
// coverage target carries that frame's scene depth as its depth attachment)
void HostShadowSetFrameTarget(bgfx::FrameBufferHandle frame, u32 width, u32 height, bool sceneTarget);

// The shared 2D program, its s_tex sampler and the 1x1 white texture untextured draws sample (made lazily on first
// use, after BgfxInit; owned by the process, torn down with bgfx itself)
bgfx::ProgramHandle HostDraw2DProgram();
bgfx::UniformHandle HostDraw2DSampler();
// The 2D fragment shader's u_materialAlpha (its alpha test: the comparison and the reference, HostMaterialStateOf's)
bgfx::UniformHandle HostDraw2DAlphaUniform();
// The 3D fragment shaders' u_materialAlpha (fs_draw3d: the same pair, for the rigid and skinned programs)
bgfx::UniformHandle HostDraw3DAlphaUniform();
bgfx::TextureHandle HostWhiteTexture();

// The program the shadow volumes draw through: the rigid vertex shader with a fragment shader writing one constant colour, so
// a shell contributes the pass's blend factor and not a fraction of the mesh's colours (see fs_shadowvolume.sc)
bgfx::ProgramHandle HostShadowVolumeProgram();

// A particle page's per-mode materials (the PS2's ReadParticlePageData makes one material a blend mode): all
// through the STQ particle program, each with the page's texture and its mode's blend. graphics.cpp's
// ReadParticlePageData calls it; the materials are owned by that page's callers
void MakePageModeMaterials(struct ParticlePage* page);

// The screen the strips' pixels are fractions of (StartFrame keeps it: the frame's size, the window's while no frame
// started). Sprites' fractions map onto the view directly without it
void HostSetScreenSize(u32 width, u32 height);

// The same size read back (the shadow pass sizes its half-res target from it)
void HostScreenSize(u32* width, u32* height);

// The display's offset (Platform::Graphics::MoveDisplay): how far the finished picture moves, a fraction of the frame
// each way. MoveDisplay (graphics.cpp) hands it over; HostCompositeScene moves the composite of the scene over the
// backbuffer by it, since the host has no display window to move the way the PS2's GS has
void HostSetDisplayOffset(f32 across, f32 down);

// A chunk's view for the frame's particles (Platform::Graphics::LoadParticleView): the frustum's six planes in the
// chunk's space and the camera's place in it, kept as the PS2 keeps them in VU0's memory. Read back by the maths the
// particles' distances and culling go by (Platform::Math::ViewDistance and ParticleBlockView, whose CPU versions in
// src/platform/null/math.cpp are the null platform's and so cannot include this header); the accessors are null for an
// id no view was loaded for this frame. Both live in culling.cpp
void HostStoreParticleView(s32 view, const Matrix4x4* matrices);
const Vector4* HostParticleViewPlanes(s32 view);
const Vector4* HostParticleViewCamera(s32 view);
const Matrix4x4* HostParticleViewMatrix(s32 view);

// The view's matrix to the screen (the frame's view, the one the culling loaded): the particle billboards take their
// size from it, the way the console's VU1 takes it from the view's own matrix (see particle_billboard.h). Every view
// of a frame shares it, as they share the camera. It lives in culling.cpp
const Matrix4x4& HostParticleToScreen();

// The projection's aspect: the one the projection's x is compressed by (the lens's pixel aspect times the TV's)
f32 HostParticleInverseAspect();

// A fraction of the screen checked against draw2d.cpp's coordinate math: the PS2 maps fraction f of a width W to
// f * W pixels from the frame's corner (place.x * width * GsSubpixels + FrameCorner, the 1/16px units cancelling),
// which is NDC 2f - 1; Y grows down on both, so NDC y is 1 - 2f
inline void HostMapToClip(f32 fractionX, f32 fractionY, f32* clipX, f32* clipY)
{
    *clipX = fractionX * 2.0f - 1.0f;
    *clipY = 1.0f - fractionY * 2.0f;
}

// The sky's half-size buffer (sky.cpp): DrawChunkSky draws the chunk's sky into it (bgfx view 0, the Sky bucket)
// and copies it over the screen (view 1). HostSkySetCopyTarget redirects the copy (the headless proof reads its
// offscreen target back); HostSkyReadBuffer reads the half-size buffer itself back as RGBA8 rows after a frame
void HostSkySetCopyTarget(bgfx::FrameBufferHandle framebuffer);
bool HostSkyReadBuffer(u32 width, u32 height, u8* out);
}
