// The host particles' vertex shader: one quad instanced per particle. The CPU worked the particle out of its block's
// record (start, velocity, spawn time, life scale) and the system's render table at the particle's life step: the
// particle's four corners are projected through the block's matrix already (the shape is in the particle's own units,
// so it cannot be added after the divide), and they arrive per instance with the colour and the texture's rectangle.
// This maps each corner's game screen coordinates (1/16th of a pixel, middle 2048, Y down, and the GS depth the
// clip-to-screen matrix leaves) onto bgfx's clip space.
$input a_position, i_data0, i_data1, i_data2, i_data3, i_data4, i_data5
$output v_color0, v_texcoord0

#include <bgfx_shader.sh>

// x: 1 / (the display's width in 1/16th-pixel units: width * 8), y: the same for the height, z: 32768 (2048 in
// 1/16th-pixel units, the middle the game's coordinates are around), w: 1 / the GS depth range (8388607)
uniform vec4 u_particle;

void main()
{
    // The unit quad's four vertices (0,0), (0,1), (1,0), (1,1): (0,0) takes i_data0, (0,1) i_data1, (1,0) i_data3
    // and (1,1) i_data2; the texels mix i_data5 the same way
    vec4 gs = mix(mix(i_data0, i_data1, a_position.y), mix(i_data3, i_data2, a_position.y), a_position.x);

    // The corner is a clip-space point of the game's: dividing by w gives its place in those units, which the middle
    // and the scales map onto bgfx's [-1, 1]. Each corner keeps its own w, so the quad is the projection of four
    // points and not a screen-space rectangle stretched by the centre's distance.
    vec2 ndc = vec2((gs.x - u_particle.z * gs.w) * u_particle.x,
                    (u_particle.z * gs.w - gs.y) * u_particle.y);

    // The other way about from the screen's x and y, the GS depth runs LARGER NEARER (0 at the far plane, twice the
    // depth range at the near, the way the console's z buffer tests it), where bgfx's clip z runs from -1 at the near
    // to 1 at the far, as the scene's own toClip draws it. The GS depth over the range is therefore 1 - ndc z, and
    // ndc z times the w is what gl_Position wants.
    float depth = gs.w - gs.z * u_particle.w;

    gl_Position = vec4(ndc, depth, gs.w);

    v_texcoord0 = vec2(mix(i_data5.x, i_data5.z, a_position.x), mix(i_data5.y, i_data5.w, a_position.y));
    v_color0 = i_data4;
}
