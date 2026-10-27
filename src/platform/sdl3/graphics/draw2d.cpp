// 2D drawing: DrawSprite/DrawTurnedSprite/DrawStrip as bgfx textured/coloured quads through
// the host 2D program on view 24 (the UI's bucket), in submission order. Positions arrive in clip space straight
// from the game's fractions (HostMapToClip: the PS2's fraction * width pixels from the frame's corner, the 1/16px
// units cancelling between place and GS coords), colours scaled the GS's way (its RGBAQ and its texture modulate
// both read 0x80 as one, so a byte is 1/128 on every channel, alpha included). Textures are whole fractions of the
// bound texture, sampled from the material's cached upload (SubmitQuad and SubmitStrip resolve it; the white
// texture stands in for untextured materials).
#include "draws_host.h"
#include "host_graphics.h"

#include "game/math.h"

#include <algorithm>

namespace
{
// A draw's vertex: clip-space position, texture fractions, premultiplied RGBA
struct Draw2DVertex
{
    f32 x;
    f32 y;
    f32 z;
    f32 s;
    f32 t;
    f32 red;
    f32 green;
    f32 blue;
    f32 alpha;
};

// The screen the strips' pixels are fractions of (StartFrame keeps it; sprites map onto the view without it)
u32 g_screenWidth = BgfxWindowWidth;
u32 g_screenHeight = BgfxWindowHeight;

// The 2D pass's own state over the material's: the colour and alpha are written, the depth isn't (the UI has no
// depth) and the usual source-over blend stands until a material's own settings say otherwise. HostMaterialStateOf
// folds each material's blending, alpha test and depth mask onto this
constexpr u64 Draw2DPass = BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_BLEND_ALPHA;

bgfx::VertexLayout Draw2DLayout()
{
    bgfx::VertexLayout layout;
    layout.begin()
        .add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
        .add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
        .add(bgfx::Attrib::Color0, 4, bgfx::AttribType::Float)
        .end();
    return layout;
}

// The game's RGBA bytes premultiplied for a texture path: the GS modulates texture * colour at Q=1 with 0x80 the
// identity (draw2d.cpp's Write2DColour ORs RGBAQ with Q=1.0f; 128 leaves the texture as it is), and 0x80 is the
// identity on every channel including alpha (0x80808080 is the game's opaque default), so each byte scales by
// 1/128 -- alpha over 255 would turn that default into half transparency
void MultiplyTextured(u32 colour, f32* red, f32* green, f32* blue, f32* alpha)
{
    auto* bytes = reinterpret_cast<const u8*>(&colour);
    constexpr f32 Scale = 1.0f / 128.0f;
    *red = std::min(static_cast<f32>(bytes[0]) * Scale, 1.0f);
    *green = std::min(static_cast<f32>(bytes[1]) * Scale, 1.0f);
    *blue = std::min(static_cast<f32>(bytes[2]) * Scale, 1.0f);
    *alpha = std::min(static_cast<f32>(bytes[3]) * Scale, 1.0f);
}

// The same for an untextured strip: no texture to modulate, but RGBAQ's bytes reach the GS the same way -- 0x80 is
// 1.0 on every channel and anything above saturates to it (gsvalues.h's White is 0x80808080, and the game's own
// "full" RGB is 192, which the GS clamps), so a byte scales by 1/128 and clamps at 1
void MultiplyFlat(u32 colour, f32* red, f32* green, f32* blue, f32* alpha)
{
    auto* bytes = reinterpret_cast<const u8*>(&colour);
    constexpr f32 Scale = 1.0f / 128.0f;
    *red = std::min(static_cast<f32>(bytes[0]) * Scale, 1.0f);
    *green = std::min(static_cast<f32>(bytes[1]) * Scale, 1.0f);
    *blue = std::min(static_cast<f32>(bytes[2]) * Scale, 1.0f);
    *alpha = std::min(static_cast<f32>(bytes[3]) * Scale, 1.0f);
}

// A quad (a triangle strip: 4 vertices) submitted through the shared program: view 24 sequential (submission
// order), the material's cached texture when it has one (HostSetMaterialTexture), else the white texture (which the
// fragment shader's sample * colour reduces to the vertex colour for untextured materials)
// The view a 2D draw goes in: the bucket its material names when that is one of the 2D buckets -- the UI's, the fonts'
// or the distortion's -- else the UI's view, since retail draws every 2D material in one of those three and a material the
// host made without a bucket must not fall back to the opaque view (that draws the UI before the fog and the shadows)
bgfx::ViewId ViewOf2D(const Platform::Graphics::HostMaterial* material)
{
    const u32 bucket = material != nullptr ? material->bucket : 0;
    if (bucket == 24 || bucket == 26 || bucket == 23)
    {
        return Platform::Graphics::HostViewForBucket(bucket);
    }

    return Platform::Graphics::HostDraw2DView;
}

void SubmitQuad(Platform::Graphics::HostMaterial* material, const Draw2DVertex* vertices)
{
    bgfx::TextureHandle texture = Platform::Graphics::HostMaterialBoundTexture(material);
    if (!bgfx::isValid(texture))
    {
        texture = Platform::Graphics::HostWhiteTexture();
    }
    if (!bgfx::getAvailTransientVertexBuffer(4, Draw2DLayout()))
    {
        return;
    }
    bgfx::TransientVertexBuffer buffer;
    bgfx::allocTransientVertexBuffer(&buffer, 4, Draw2DLayout());
    auto* at = reinterpret_cast<Draw2DVertex*>(buffer.data);
    for (u32 vertex = 0; vertex < 4; vertex++)
    {
        at[vertex] = vertices[vertex];
    }
    // The material's own blending and alpha test over the pass's (the 2D has no depth to test: withDepth false), so
    // a UI material that doesn't blend draws opaque and an alpha-tested one drops its samples
    const Platform::Graphics::HostMaterialState state = Platform::Graphics::HostMaterialStateOf(material, Draw2DPass, false);
    // The material's own bucket (the UI's, the fonts', the distortion's): retail draws 2D in the bucket its material names,
    // so a font page's quads land after the UI's and the gem's hexagons after the particles, as on the console
    const bgfx::ViewId view = ViewOf2D(material);
    bgfx::setViewMode(view, bgfx::ViewMode::Sequential);
    Platform::Graphics::HostSceneSetView(view);
    bgfx::setState(state.state | BGFX_STATE_PT_TRISTRIP);
    bgfx::setUniform(Platform::Graphics::HostDraw2DAlphaUniform(), state.alphaReference, 1);
    // The 2D draws' textures clamp (the reference's BindTexture default, GL_CLAMP_TO_EDGE): the GS CLAMP register's
    // reset REPEAT is overridden because the 2D shaders only ever sample inside the sprite. The filter is the
    // material's own (the GS's TEX1 takes it from the shader's "linear" setting)
    bgfx::setTexture(0, Platform::Graphics::HostDraw2DSampler(), texture,
                     BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP |
                         Platform::Graphics::HostMaterialFilter(reinterpret_cast<const Platform::Graphics::HostMaterial*>(material)));
    bgfx::setVertexBuffer(0, &buffer);
    bgfx::submit(view, Platform::Graphics::HostDraw2DProgram());
}

// A strip of count coloured vertices submitted as a triangle strip (the PS2's first two vertices don't draw: its
// strips repeat them as degenerates; bgfx draws them as a zero-area lead-in, same picture). The material's own
// texture is bound when it has one (a textured strip), else the white texture (which the fragment shader's
// sample * colour reduces to the vertex colour)
void SubmitStrip(Platform::Graphics::HostMaterial* material, const Draw2DVertex* vertices, u32 count)
{
    if (count == 0 || !bgfx::getAvailTransientVertexBuffer(count, Draw2DLayout()))
    {
        return;
    }
    bgfx::TextureHandle texture = Platform::Graphics::HostMaterialBoundTexture(material);
    if (!bgfx::isValid(texture))
    {
        texture = Platform::Graphics::HostWhiteTexture();
    }
    bgfx::TransientVertexBuffer buffer;
    bgfx::allocTransientVertexBuffer(&buffer, count, Draw2DLayout());
    auto* at = reinterpret_cast<Draw2DVertex*>(buffer.data);
    for (u32 vertex = 0; vertex < count; vertex++)
    {
        at[vertex] = vertices[vertex];
    }
    // The material's own blending and alpha test over the pass's (no depth on the 2D: withDepth false). A strip with
    // no material draws through the flat material's own state
    Platform::Graphics::HostMaterial* stripMaterial =
        material != nullptr ? material : reinterpret_cast<Platform::Graphics::HostMaterial*>(Platform::Graphics::FlatMaterial());
    const bgfx::ViewId view = ViewOf2D(stripMaterial);
    bgfx::setViewMode(view, bgfx::ViewMode::Sequential);
    Platform::Graphics::HostSceneSetView(view);
    const Platform::Graphics::HostMaterialState state =
        Platform::Graphics::HostMaterialStateOf(stripMaterial, Draw2DPass, false);
    bgfx::setState(state.state | BGFX_STATE_PT_TRISTRIP);
    bgfx::setUniform(Platform::Graphics::HostDraw2DAlphaUniform(), state.alphaReference, 1);
    bgfx::setTexture(0, Platform::Graphics::HostDraw2DSampler(), texture,
                     BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP |
                         Platform::Graphics::HostMaterialFilter(stripMaterial));
    bgfx::setVertexBuffer(0, &buffer);
    bgfx::submit(view, Platform::Graphics::HostDraw2DProgram());
}

// A textured vertex from fractions of the screen and of the texture (the PS2's order: place corner shows texture
// corner, opposites match)
Draw2DVertex TexturedVertex(f32 fractionX, f32 fractionY, f32 s, f32 t, f32 red, f32 green, f32 blue, f32 alpha)
{
    f32 clipX = 0.0f;
    f32 clipY = 0.0f;
    Platform::Graphics::HostMapToClip(fractionX, fractionY, &clipX, &clipY);
    return {clipX, clipY, 0.0f, s, t, red, green, blue, alpha};
}
}

namespace Platform::Graphics
{
void HostSetScreenSize(u32 width, u32 height)
{
    if (width != 0)
    {
        g_screenWidth = width;
    }
    if (height != 0)
    {
        g_screenHeight = height;
    }
}

// The display the game's own screen coordinates are measured against (the frame start's screen size, which is what
// the particles' shapes and the to-screen matrices are in; see HostDisplayWidth's comment in host_graphics.h)
u32 g_displayWidth = BgfxWindowWidth;
u32 g_displayHeight = BgfxWindowHeight;

void HostSetDisplaySize(u32 width, u32 height)
{
    if (width != 0)
    {
        g_displayWidth = width;
    }
    if (height != 0)
    {
        g_displayHeight = height;
    }
}

u32 HostDisplayWidth()
{
    return g_displayWidth;
}

u32 HostDisplayHeight()
{
    return g_displayHeight;
}

void HostScreenSize(u32* width, u32* height)
{
    if (width != nullptr)
    {
        *width = g_screenWidth;
    }
    if (height != nullptr)
    {
        *height = g_screenHeight;
    }
}

void DrawSprite(Material* material, u32 colour, const Rectangle& place, const Rectangle& texture)
{
    // draw2d.cpp's DrawSprite: left = place.x * width + frame corner, checked in HostMapToClip's comment. The
    // frame's offset shifts every corner equally, so fractions relative to the corner map onto the view directly
    f32 red = 0.0f;
    f32 green = 0.0f;
    f32 blue = 0.0f;
    f32 alpha = 0.0f;
    MultiplyTextured(colour, &red, &green, &blue, &alpha);
    f32 textureRight = texture.x + texture.width;
    f32 textureBottom = texture.y + texture.height;
    // The PS2's vertex order (top-left, bottom-left, top-right, bottom-right) is a triangle strip already
    const Draw2DVertex vertices[4] = {
        TexturedVertex(place.x, place.y, texture.x, texture.y, red, green, blue, alpha),
        TexturedVertex(place.x, place.y + place.height, texture.x, textureBottom, red, green, blue, alpha),
        TexturedVertex(place.x + place.width, place.y, textureRight, texture.y, red, green, blue, alpha),
        TexturedVertex(place.x + place.width, place.y + place.height, textureRight, textureBottom, red, green,
                       blue, alpha),
    };
    SubmitQuad(reinterpret_cast<HostMaterial*>(material), vertices);
}

void DrawTurnedSprite(Material* material, u32 colour, const Vector2* corners, const Rectangle& texture)
{
    // draw2d.cpp's DrawTurnedSprite: corners' fractions straight from the screen's corner (no frame offset), same
    // texture-corner mapping as DrawSprite
    f32 red = 0.0f;
    f32 green = 0.0f;
    f32 blue = 0.0f;
    f32 alpha = 0.0f;
    MultiplyTextured(colour, &red, &green, &blue, &alpha);
    f32 textureRight = texture.x + texture.width;
    f32 textureBottom = texture.y + texture.height;
    const Draw2DVertex vertices[4] = {
        TexturedVertex(corners[0].x, corners[0].y, texture.x, texture.y, red, green, blue, alpha),
        TexturedVertex(corners[1].x, corners[1].y, texture.x, textureBottom, red, green, blue, alpha),
        TexturedVertex(corners[2].x, corners[2].y, textureRight, texture.y, red, green, blue, alpha),
        TexturedVertex(corners[3].x, corners[3].y, textureRight, textureBottom, red, green, blue, alpha),
    };
    SubmitQuad(reinterpret_cast<HostMaterial*>(material), vertices);
}

void DrawStrip(Material* material, u32 count, const Vector2* places, const u32* colours)
{
    // draw2d.cpp's DrawStrip: places in pixels from the frame's corner (x * 16 + corner in 1/16px units), so pixels
    // are fractions of the frame's display size (the same target->displayWidth FrameCorner measures the corner
    // from); colours per vertex. The material's texture is bound when it has one
    if (count == 0 || places == nullptr || colours == nullptr)
    {
        return;
    }
    // The game's strips hold at most 256 vertexes (Strip::MostVertexes, the count is a byte), so one staging buffer
    // covers every draw; longer strips would split, each chunk after the first repeating the previous chunk's last
    // two vertexes so the triangle strip keeps its continuity across the seam (vertex N needs N-1 and N-2)
    constexpr u32 ChunkVertices = 256;
    constexpr u32 SeamVertexes = 2;
    Draw2DVertex chunk[ChunkVertices];
    u32 done = 0;
    while (done < count)
    {
        u32 remaining = count - done;
        u32 chunkCount = remaining > ChunkVertices ? ChunkVertices : remaining;
        for (u32 vertex = 0; vertex < chunkCount; vertex++)
        {
            f32 red = 0.0f;
            f32 green = 0.0f;
            f32 blue = 0.0f;
            f32 alpha = 0.0f;
            MultiplyFlat(colours[done + vertex], &red, &green, &blue, &alpha);
            f32 clipX = 0.0f;
            f32 clipY = 0.0f;
            HostMapToClip(places[done + vertex].x / static_cast<f32>(HostDisplayWidth()),
                          places[done + vertex].y / static_cast<f32>(HostDisplayHeight()), &clipX, &clipY);
            chunk[vertex] = {clipX, clipY, 0.0f, 0.0f, 0.0f, red, green, blue, alpha};
        }
        SubmitStrip(reinterpret_cast<HostMaterial*>(material), chunk, chunkCount);
        if (chunkCount == remaining)
        {
            break;
        }
        // The next chunk starts on this one's last two vertexes, so its first triangle is the one the split strip
        // needs next (no gap and no double-drawn triangle at the seam)
        done += chunkCount - SeamVertexes;
    }
}

// A font glyph: the same quad as DrawSprite, but the material's texture area is given in the page's texels and its rows
// are taken the other way up. A font page stores its glyphs flipped (the PS2's text.cpp draws them with the page's rows
// the other way up), so the quad's top edge shows the area's bottom row: without that the text draws upside down. The
// page's size, which the texels are fractions of, is the one the upload the material's texture was made with had
void DrawGlyph(Material* material, u32 colour, const Rectangle& place, const Rectangle& texture)
{
    auto* host = reinterpret_cast<HostMaterial*>(material);
    if (host == nullptr || texture.width <= 0.0f || texture.height <= 0.0f)
    {
        return;
    }

    u32 pageWidth = 0;
    u32 pageHeight = 0;
    if (!HostTextures().FindSize(host->gameTexture, &pageWidth, &pageHeight) || pageWidth == 0 || pageHeight == 0)
    {
        return;
    }

    f32 red = 0.0f;
    f32 green = 0.0f;
    f32 blue = 0.0f;
    f32 alpha = 0.0f;
    MultiplyTextured(colour, &red, &green, &blue, &alpha);
    f32 u0 = texture.x / static_cast<f32>(pageWidth);
    f32 u1 = (texture.x + texture.width) / static_cast<f32>(pageWidth);
    f32 sourceTop = texture.y / static_cast<f32>(pageHeight);
    f32 sourceBottom = (texture.y + texture.height) / static_cast<f32>(pageHeight);
    // Flipped: the place's top edge shows the area's bottom row, its bottom edge the area's top
    const Draw2DVertex vertices[4] = {
        TexturedVertex(place.x, place.y, u0, sourceBottom, red, green, blue, alpha),
        TexturedVertex(place.x, place.y + place.height, u0, sourceTop, red, green, blue, alpha),
        TexturedVertex(place.x + place.width, place.y, u1, sourceBottom, red, green, blue, alpha),
        TexturedVertex(place.x + place.width, place.y + place.height, u1, sourceTop, red, green, blue, alpha),
    };
    SubmitQuad(host, vertices);
}
}
