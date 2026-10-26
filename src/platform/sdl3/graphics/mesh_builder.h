// Rigid-mesh builder: parses a retail model packet (src/platform/ps2/renderer/resources.cpp ModelRead) into bgfx
// vertex/index data. The parse is cached by packet handle; instance data stays per draw.
//
// A packet is one RET DMA tag around a VIF1 program. Each triangle strip has a V4-32 header at VU1 0x8000, a V2-32
// quadword at 0x8001, then STCYCL 4,1 with three NUM-matched unpacks: V3-32 positions to 0x8003, V4-32 (u, v, 1,
// flags) to 0x8004, and V4-8 packed normals/colours to 0xC006, finished by MSCAL 0. The strip vertex total matches
// the submodel's vertexCount.
//
// Host vertex: position 3x f32, UV 2x f32, normal 4x u8 (signed) and colour 4x u8. The V4-32's fourth word carries
// the GS's ADC flag (0x7F starts a strip, 0x7FFF chains past it). Strips become indexed triangles, each emitting
// count - 2 triangles with alternating winding.
#pragma once

#include "common.h"

#include <cstddef>
#include <cstdint>
#include <vector>

// A rigid submodel's host mesh: interleaved vertex bytes under MeshVertexLayout() plus triangle indexes.
// The instance block's matrix/lights (models.cpp RigidInstance) are per draw, not stored here.
struct Ps2RigidMesh
{
    std::vector<u8> vertices;
    std::vector<u16> indexes;
    u32 vertexCount = 0;
    u32 indexCount = 0;
    f32 boundsMin[3] = {0.0f, 0.0f, 0.0f};
    f32 boundsMax[3] = {0.0f, 0.0f, 0.0f};
    u32 checksum = 0;
    // The colour-branch tally (the debug probes): how many vertices' colours the address-4 UV row's low bytes set
    // and how many the address-6 packed RGBA row overwrote. A packet whose colours all come from address 4 has no
    // packed colour row
    u32 address4Colours = 0;
    u32 address6Colours = 0;
    // The first address-4 row's first element, raw (the debug probes): the words the host reads as U, V, and the two
    // the address-4 branch turns into a colour when the unpack has four components
    u32 address4Sample[4] = {0, 0, 0, 0};
    // The packet's leading UNPACK command words (the debug probes): the row layout (each command's kind, count and
    // VU1 address) a draw decoded with, so a packet whose rows differ from the usual layout can be told apart
    u32 rowCodes[8] = {0};
    u32 rowCodeCount = 0;
    // How many position rows were 32-bit words and how many were 16-bit halfwords (the address-3 rows), and whether a
    // non-float element size was seen: a rigid packet whose address-3 row isn't a 32-bit float row the host reads as
    // floats would explode
    u32 positionRows32 = 0;
    u32 positionRows16 = 0;
    u32 unknownRow = 0;
};

// Parses one submodel packet (packetSize bytes at packet) into out. Returns false when the DMA/VIF walk
// fails or the strip vertex total overflows the caller's vertexCount budget (0 = no budget).
bool ParseRigidSubModel(const u8* packet, size_t packetSize, u32 vertexCount, Ps2RigidMesh* out);

// A skin submodel's host mesh: the rigid fields plus per-vertex joint data and the metadata the shader path needs.
// Per strip the packet has a GIF header at 0x8000, rows at 0x8001 and 0x8002 (position and UV scales), STMOD/STROW/
// STCYCL(4,1), then four NUM-matched rows: positions and normal X at 0x8003 (16-bit fixed point, 1/16384), UVs and
// normal Y/Z at 0x8004, joint indices and weights at 0x8005 (index * 4 in the low 9 bits, the joint count in the
// fourth word's low byte, bit 15 the ADC chain flag), and packed RGBA colours at 0x8006, finished by MSCAL.
//
// STROW is resent between rows, so each row's offsets apply to that row alone; the decode captures them as it reads
// (ParseSkinSubModel's positionRow/uvRow). Joint matrices use WriteJoints' VIF REF and are uploaded as a uniform array;
// blend-shape weights go in as shader uniforms.
struct Ps2SkinMesh
{
    std::vector<u8> vertices;
    std::vector<u16> indexes;
    u32 vertexCount = 0;
    u32 indexCount = 0;
    u32 jointCount = 0;
    u32 shapeCount = 0;
    u32 checksum = 0;
};

// The host skin vertex, decoded as the TT-Lab reference decoder's DecodeSkinSubModel
// reads the VU1 packet: position 3x f32 (the address-3 row,
// 16 bit lanes * the packet's position scale; the row's lane-3 halfword is the
// normal's X), UV 2x f32 (the address-4 row's lanes 0-1 * the UV scale; lanes 2-3
// are the normal's Y and Z), the address-6 packed RGBA colour word, 3 joint
// indices (address-5 lanes 0-2, the low 9 bits / 4) and 3 float weights (the same
// lanes' upper bits, the float the exporter packed above the index).
struct Ps2SkinVertex
{
    f32 x;
    f32 y;
    f32 z;
    f32 u;
    f32 v;
    u8 normal[4];
    u8 colour[4];
    f32 jointIdx[3];
    f32 jointWeight[3];
};

// Parses one skin submodel packet (packetSize bytes at packet) into out.
// shapeCount is the blend skin's shape total (0 for plain skins). Returns
// false when the DMA/VIF walk fails or the strip vertex total overflows the
// caller's vertexCount budget (0 = no budget).
bool ParseSkinSubModel(const u8* packet, size_t packetSize, u32 vertexCount, u32 shapeCount, Ps2SkinMesh* out);

// The skin shader's uniforms. Joint matrices upload as one mat4 array
// (WriteJoints dedups on bucket.lastJoints == Address(joints): same address
// skips the upload); blend-shape weights upload as one float array of
// shapeCount (DrawBlendSkin's weights/shapes per part). A vertex's joint lane
// stores a 7-bit index with a 5-bit weight (mesh_builder's decode), so an index
// can name any slot up to 127: the array is sized to the whole range, since the
// vertex shader indexes it without a clamp and an index past the array would
// read another uniform. The retail skins' indices reaching 61 held only for the
// one file that survey walked, not for every model on the disc
constexpr u32 SkinMaxJoints = 128;
constexpr char SkinJointsUniform[] = "u_joints";
constexpr char SkinBlendWeightsUniform[] = "u_blendWeights";

// WriteJoints' dedup for the host: true when jointsAddress differs from the
// last uploaded address (records it), false when the upload can be skipped
bool SkinJointUploadNeeded(u32 jointsAddress);
