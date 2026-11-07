// The 3D rigid-mesh vertex shader, flat shaded: a material whose shader has the GS's IIP bit cleared draws its triangles
// with the provoking vertex's values (varying_draw3dflat.def.sc). The body is vs_draw3d.sc's, included rather than copied so
// the two can never drift apart; only the varyings' interpolation differs.
#include "vs_draw3d.sc"
