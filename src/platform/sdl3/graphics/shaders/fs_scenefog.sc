// The scene fog's fragment shader: the retail screen effects' colour filter (ps2/renderer/effects.cpp's
// FilterColours over CopyDepthBuffer). Retail copies the depth buffer as a 16 entry texture whose pixels index a 256
// entry CLUT -- the chunk's colourFilterPalette -- and draws that over the frame.
//
// The blend is the retail ALPHA formula (effects.cpp's OverSource, GS_ALPHA{a=1} = (Cd - Cs)*As + Cs), which the
// draw sets as bgfx's INV_SRC_ALPHA / SRC_ALPHA factors, so only the palette colour and its alpha are written here.
// The palette's own entries make that work out: entry 255 has the palette's haze colour at alpha 0x80 (1.0, the
// mix's identity: those pixels come through unmisted) and entry 0 has it at a lower alpha (about 0.7 for the beach's
// sunset orange), so the palettes ramp the mist from nothing to about 40%.
//
// WHICH ENTRY, from the retail code and data:
//  * the game's clip-to-screen matrix leaves Z = 0 at the far plane and 2 * GsDepthRange (0xFFFFFE) at the near
//    (renderer.h's GsDepthRange, sky.cpp's HalfSizeScreenMatrix), so Z = 0xFFFFFE * (1 - depth) for a frame depth
//    in [0, 1];
//  * CopyDepthBuffer copies the Z buffer as a 16 bit texture and FilterColours reads the copy as 8 bit indexes
//    (PSMT8H, "their pixels' top bytes"), i.e. the copy's byte is the Z value's bits 8..15 -- the 16 bit copy's high
//    byte;
//  * FilterColours draws the sprite at z = 0x10000 with a GEQUAL depth test, so the filter covers only the pixels
//    whose Z is at or below 0x10000.
// Those two numbers fit each other exactly: bits 8..15 of Z run out precisely at Z = 0x10000, so the test keeps the
// whole byte's monotonic range (0 at the far plane up to 255 at Z = 0x10000) and throws away the depths nearer than
// that, where the byte would wrap instead of ramp. With the lens's own planes (camerarig.cpp's 0.1 and 1500) the cut
// sits at about 26 units: no mist closer than that, then 255 -> 0 as the distance runs to the far plane.
//
// That is why the index is not simply 255 * (1 - depth): the perspective packs the visible world into the top 1% of
// the range, where that form lands every pixel on entries 0..25 and turns the whole frame into one flat wash. Retail
// steps past the packed leading byte and uses the next one, which is linear in 1/distance over exactly the span the
// palette was authored for. u_fog.x = 1 restores the flat form, for comparing the two in one build.
$input v_texcoord0

#include <bgfx_shader.sh>

SAMPLER2D(s_depth, 0);
SAMPLER2D(s_palette, 1);

// x: 0 the copy's byte (retail), 1 the packed leading byte (255 * (1 - depth), what this did before)
uniform vec4 u_fog;

void main()
{
    float depth = texture2D(s_depth, v_texcoord0).x;
    // Nothing was drawn here (the sky's own colour), so there is nothing to mist. bgfx clears depth to exactly 1.0
    if (depth >= 1.0)
    {
        discard;
    }

    // The GS depth the game's matrix leaves, in entries of the copy's byte: bits 8..15 of 0xFFFFFE * (1 - depth)
    float slot = (1.0 - depth) * (16777214.0 / 256.0);
    if (u_fog.x > 0.5)
    {
        // The packed leading byte: the whole visible world in entries 0..25
        slot = (1.0 - depth) * 255.0;
    }
    else if (slot > 256.0)
    {
        // Retail's GEQUAL test at z = 0x10000: nearer pixels take no filter at all
        discard;
    }

    // The CLUT is point sampled: the entry the byte names, never a blend of two
    float index = floor(clamp(slot, 0.0, 255.0));
    vec2 uv = vec2((index + 0.5) / 256.0, 0.5);
    gl_FragColor = texture2D(s_palette, uv);
}
