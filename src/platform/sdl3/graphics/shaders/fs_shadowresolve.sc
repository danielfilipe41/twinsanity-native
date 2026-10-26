// Shadow resolve: the PS2's ApplyShadows (ps2/renderer/shadows.cpp), the half-res coverage buffer drawn over the frame on
// ALPHA's Cd - Cs * 32 / 128. s_shadow = the volumes' shells counted, the away ones in R and the facing ones in G (see the
// volumes' pass in shadows.cpp); s_depth = the scene's depth, whose
// near-to-far position picks the palette entry as retail indexes it by the frame-depth copy's top byte (alpha ramp 255 -> 0);
// s_frame = the scene colour, whose destination alpha gates coverage the way TEST{DEST=1, ZTST=GEQUAL} gates a volume fragment.
$input v_texcoord0

#include <bgfx_shader.sh>

SAMPLER2D(s_shadow, 0);
SAMPLER2D(s_depth, 1);
SAMPLER2D(s_palette, 2);
SAMPLER2D(s_frame, 3);

// x: how much of the frame a fully covered pixel loses (the retail 32 / 128)
// y: how many shells a crossed ground takes to reach a full shadow (1: every pixel of a shadow as dark as the host has always
//    drawn it. Retail darkens by the count instead, count * 8 / 255 through x)
uniform vec4 u_shadowAmount;
// x: near plane, y: far plane; the ramp turns the frame's perspective depth into a distance through them
uniform vec4 u_shadowLinear;
// x: 1 draws the factors instead of darkening - R coverage, G depth, B ramp, A gate - so whichever reads zero where a
// shadow belongs is the one at fault
uniform vec4 u_shadowDebug;
// x: 1 forces the ramp to y, taking the sampled depth out of the picture: a shadow that still appears means only that
// depth is wrong
uniform vec4 u_shadowRamp;

void main()
{
    // The volumes' shells as retail counts them: the ones away from the camera in R, the ones facing it in G, 8/255 per shell
    // (the GS's per-face constant). The count of shells with the surface inside the volume is their DIFFERENCE - a pass that
    // instead adds one kind into one channel and takes the other back out of it saturates, and a caster's own stack of proxies
    // (a capsule per bone) then loses the ground it crossed: two shells one way and one the other is a count of ONE
    vec4 shells = texture2D(s_shadow, v_texcoord0);
    float count = abs(shells.r - shells.g) * (255.0 / 8.0);
    // ONE shell is a whole shadow. Any less than that and the coverage grades by how many volumes cover a pixel, which
    // for a character - a stack of one capsule per bone - draws the pills of the individual bones rather than one
    // silhouette: one capsule half, two capsules most of the way. Any gradient here shows the bones, so the strength is
    // the amount's job instead (u_shadowAmount.x), and the only thing that varies across a shadow is the depth ramp
    float coverage = clamp(count, 0.0, 1.0);
    // Scene depth in [0, 1]. The GS's copy puts the FAR end at index 0, so the host's near-0 depth must be turned round to index
    // the palette the same way; raw depth put gameplay pixels (near 1) on the dark end and the ground took no shadow
    float depth = texture2D(s_depth, v_texcoord0).x;
    // The frame's perspective depth as a distance, near to far, spread logarithmically. Raw depth packs the visible world into
    // its last few percent, landing the ground at the caster's feet and the void beyond on one entry
    float near = u_shadowLinear.x;
    float far = u_shadowLinear.y;
    float distance = near * far / max(far - depth * (far - near), 1e-6);
    // Two ways to index the palette, switchable so they can be compared in one build:
    //  * u_shadowLinear.z > 0: a DISTANCE the host sets (not retail - retail has no such number), for the reason above;
    //  * u_shadowLinear.z <= 0: the retail form, the frame-depth byte near 255 / far 0, which indexes the PS2's 256-entry
    //    white palette with alpha ramping 255 down to 0
    float slot = u_shadowLinear.z > 0.0
                     ? clamp(distance / u_shadowLinear.z, 0.0, 1.0)
                     : clamp(1.0 - depth, 0.0, 1.0);
    float ramp = texture2D(s_palette, vec2(slot, 0.5)).a;
    if (u_shadowRamp.x > 0.5)
    {
        ramp = u_shadowRamp.y;
    }
    // The frame's alpha: only pixels the scene drew opaque take a shadow. Switchable (TS_NOGATE=1), though the depth test plus the
    // take-back already spare casters (where the whole volume lies behind the body the two passes cancel), so it may be redundant;
    // as a per-pixel mask it also cuts the shadow into the caster's own silhouette
    float frameAlpha = u_shadowLinear.w > 0.5 ? (texture2D(s_frame, v_texcoord0).a >= 0.5 ? 1.0 : 0.0) : 1.0;
    if (u_shadowDebug.x > 0.5)
    {
        gl_FragColor = vec4(coverage, depth, ramp, frameAlpha);
        return;
    }

    float amount = coverage * ramp * frameAlpha * u_shadowAmount.x;
    gl_FragColor = vec4(amount, amount, amount, 0.0);
}
