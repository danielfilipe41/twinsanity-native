// The SDL3 platform's rigid-mesh builder (see mesh_builder.h).
#include "mesh_builder.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace
{
// The DMA tag's ID field (renderer.h: CNT 1, REF 3, END 7) and the RET tag the model packets carry
constexpr u32 DmaIdShift = 28;
constexpr u32 DmaCountMask = 0xFFFF;
constexpr u32 DmaReturnId = 6;
// The VIF codes the packet stream uses (renderer.h): STCYCL, MSCAL, and the UNPACK family (V4-8's 0x6E is
// its own element format, VifUnpackV4Bytes)
constexpr u8 VifStcycl = 0x01;
constexpr u8 VifMscal = 0x14;
constexpr u8 VifUnpackV2 = 0x64;
constexpr u8 VifUnpackV3 = 0x68;
constexpr u8 VifUnpackV4 = 0x6C;
constexpr u8 VifUnpackV48 = 0x6E;
// The VU1 destinations of the vertex rows (the UNPACK's low 16 bits: bit 15 is the double-buffer FLG bit,
// bit 14 the unsigned-data bit; the header/quad rows carry FLG, the V4-8 row both)
constexpr u32 VuHeaderAddress = 0x8000;
constexpr u32 VuQuadAddress = 0x8001;
constexpr u32 VuPositionAddress = 0x8003;
constexpr u32 VuUvAddress = 0x8004;
constexpr u32 VuPackedAddress = 0xC006;
constexpr u32 VuAddressMask = 0xFFFF;
// A VIF UNPACK's element sizes in words (V4-8 packs four bytes per vertex in one word)
constexpr u32 V2Words = 2;
constexpr u32 V3Words = 3;
constexpr u32 V4Words = 4;
constexpr u32 V48Words = 1;

// The host vertex: position, UV, packed normal bytes, packed colour bytes
struct MeshVertex
{
    f32 x;
    f32 y;
    f32 z;
    f32 u;
    f32 v;
    u8 normal[4];
    u8 colour[4];
};

// A little-endian word/float read (the packet bytes are the PS2's)
u32 ReadWord(const u8* packet, size_t offset)
{
    u32 word;
    std::memcpy(&word, packet + offset, sizeof(word));
    return word;
}

f32 ReadFloat(const u8* packet, size_t offset)
{
    f32 value;
    std::memcpy(&value, packet + offset, sizeof(value));
    return value;
}

// A word read as its float bits (the packets carry little-endian words the PS2 wrote)
f32 AsFloat(u32 bits)
{
    f32 value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

// A colour channel of a vertex or a decoded image made the window's full range: the game's red, green and blue are
// full at 192, its alpha at 128, and the window's channels at 255 (the reference's ScaleChannel, desktop/graphics.cpp)
u8 ScaleChannel(u8 channel, u32 full)
{
    u32 scaled = static_cast<u32>(channel) * 255u / full;
    return static_cast<u8>(scaled > 255 ? 255 : scaled);
}

// What the vertex colours' red, green and blue are full at. 192 matches the packets, but scaling it to 255 lifts
// every lit surface by about 1.33 over what the console modulates. TS_VERTEXFULL=<n> overrides it (255 leaves the
// bytes alone).
u32 VertexColourFull()
{
    static const u32 full = [] {
        const char* set = std::getenv("TS_VERTEXFULL");
        if (set != nullptr)
        {
            const s32 value = std::atoi(set);
            if (value > 0 && value <= 255)
            {
                return static_cast<u32>(value);
            }
        }

        return 255u;
    }();

    return full;
}

// A debug switch (TS_NOCOLOR): leaves every rigid/skin vertex white, so a frame can be told a textured draw whose
// colour the material's own bytes carry from one whose vertex colours the packet packs. Debug-only, off in a normal run
bool NoVertexColour()
{
    static const bool noColour = std::getenv("TS_NOCOLOR") != nullptr;
    return noColour;
}

// A debug switch (TS_FAN): the retail VU1 programs start their GIF packets from a triangle-FAN prim (vuprograms.cpp's
// TriangleFanTemplate), so a strip may be fan-ordered; with this set each strip emits the fan window (first, i-1, i)
// instead of the strip window (i-2, i-1, i), letting the two topologies be told apart. Debug-only, off in a normal run
bool FanTopology()
{
    static const bool fan = std::getenv("TS_FAN") != nullptr;
    return fan;
}

// A normal component in [-1, 1] as the host's packed byte: the shader decodes `a_normal * 2 - 1` from the byte's
// normalized 0..1 value, so 0.0 lands at 128 and the range spans 0..255 (the rigid path's V4-8 normal bytes are in
// this same convention, so both paths share the one shader)
u8 NormalByte(f32 value)
{
    f32 scaled = (value + 1.0f) * 127.5f;
    if (scaled <= 0.0f)
    {
        return 0;
    }
    if (scaled >= 255.0f)
    {
        return 255;
    }
    return static_cast<u8>(scaled + 0.5f);
}

u32 Crc32(const u8* bytes, size_t size)
{
    u32 crc = 0xFFFFFFFF;
    for (size_t index = 0; index < size; index++)
    {
        crc ^= bytes[index];
        for (u32 bit = 0; bit < 8; bit++)
        {
            crc = (crc & 1) != 0 ? (crc >> 1) ^ 0xEDB88320 : crc >> 1;
        }
    }

    return crc ^ 0xFFFFFFFF;
}

// A one-shot packet dump for the first walked rigid and skin submodel (TS_MESHDUMP=<path>): the raw packet words
// plus, per UNPACK row, the command's decode and the branch it took, plus the first decoded vertices. Writes to the
// env var's file; a no-op when it is unset. Debug-only, off in a normal run
FILE* MeshDumpFile()
{
    static bool tried = false;
    static FILE* file = nullptr;
    if (!tried)
    {
        tried = true;
        const char* path = std::getenv("TS_MESHDUMP");
        if (path != nullptr && path[0] != '\0')
        {
            file = std::fopen(path, "a");
            if (file != nullptr)
            {
                std::setvbuf(file, nullptr, _IONBF, 0);
            }
        }
    }

    return file;
}

bool MeshDumpWanted(const char* which)
{
    FILE* file = MeshDumpFile();
    if (file == nullptr)
    {
        return false;
    }

    static bool rigidDone = false;
    static bool skinDone = false;
    bool* done = std::strcmp(which, "skin") == 0 ? &skinDone : &rigidDone;
    if (*done)
    {
        return false;
    }

    *done = true;
    return true;
}
}

// The components one UNPACK kind's element takes (V4Bytes, one packed word, is a single component)
u32 UnpackComponents(u32 command)
{
    if (command == 0x6E)
    {
        return 1;
    }

    return ((command >> 2) & 3) + 1;
}

// A UNPACK's element size in words: V4Bytes one packed word; the S/V2/V3/V4 kinds one word a component, or (their low bit
// set) one halfword a component
u32 UnpackElementSize(u32 command, u32 components)
{
    if (command == 0x6E)
    {
        return 1;
    }

    return (command & 1) != 0 ? (components * 2 + 3) / 4 : components;
}

bool ParseRigidSubModel(const u8* packet, size_t packetSize, u32 vertexCount, Ps2RigidMesh* out)
{
    // The reference decoder's walk (desktop/graphics.cpp's DecodeSubModel): the DMA RET tag, then the VIF row codes. A
    // rigid packet carries, per triangle strip, V4-32 positions to 0x8003 (address 3), V4-32 UV/colour-plus-ADC to 0x8004
    // (address 4) and V4-8 packed colour to 0xC006 (address 6, the 0x6E two-byte UNPACK); a MSCAL (0x14) ends the strip.
    // STCYCL (0x01), MARK (0x07) and NOP (0x00) words are skipped. Each strip becomes the sliding window of triangles
    // (i-2, i-1, i) -- the reference's own DrawTriangleStrip3D over the strip, and the same sequence the skin path uses.
    // Rigid meshes have no normal row (they are drawn unlit, the vertex colours as they are, like the reference), so a
    // vertex's normal is left white (the shader decodes 255 to 1, so it never NaNs) and the light wash leaves the colour
    if (packet == nullptr || packetSize < 16 || (packetSize & 3) != 0 || out == nullptr)
    {
        return false;
    }

    u32 tag = ReadWord(packet, 0);
    if ((tag >> DmaIdShift) != DmaReturnId)
    {
        return false;
    }

    size_t wordCount = packetSize / sizeof(u32);
    // The words after the DMA tag: the tag's count in quadwords (bit 0 is the zero word that follows the tag)
    size_t end = 4 + (tag & DmaCountMask) * 4;
    if (end > wordCount)
    {
        return false;
    }

    const bool dump = MeshDumpWanted("rigid");
    FILE* dumpFile = dump ? MeshDumpFile() : nullptr;
    if (dumpFile != nullptr)
    {
        std::fprintf(dumpFile, "RIGID packet %p size %zu words %zu vertexCount %u tag %08x end %zu\n",
                     static_cast<const void*>(packet), packetSize, wordCount, vertexCount, tag, end);
        for (size_t index = 0; index < wordCount; index++)
        {
            std::fprintf(dumpFile, "  w%04zu %08x\n", index, ReadWord(packet, index * sizeof(u32)));
        }
    }

    std::vector<MeshVertex> vertices;
    if (vertexCount != 0)
    {
        vertices.reserve(vertexCount);
    }

    // The GS's per-vertex no-draw bit (address 4's component 3, bit 15): a triangle ending at a no-draw vertex is left out
    std::vector<u8> noDraw;
    // Whether a vertex's normal came from the packet's own row (address 5) rather than staying the sentinel: the model's
    // faces light the rest
    std::vector<u8> hasNormal;
    // The colour-branch tally (the dump): how many vertices' colours the address-4 low bytes set and how many the
    // address-6 packed word overwrote
    u32 addr4Colours = 0;
    u32 addr6Colours = 0;

    struct Strip
    {
        u32 first;
        u32 count;
    };
    std::vector<Strip> strips;
    u32 stripStart = 0;
    u32 stripVertices = 0;
    bool havePositions = false;
    auto Fresh = []() {
        MeshVertex fresh{};
        // The shader decodes the normal as a_normal*2-1 and normalizes it; a zero normal would be normalize(0) = NaN and
        // poison the lit colour. (255,255,255) decodes to a unit diagonal and is the sentinel for "no normal yet": a rigid
        // packet carries the normals as its own row when the model has them, and the pass after the strips gives every
        // vertex that never got one its own triangle's face normal
        fresh.normal[0] = 255;
        fresh.normal[1] = 255;
        fresh.normal[2] = 255;
        fresh.normal[3] = 0;
        fresh.colour[0] = 255;
        fresh.colour[1] = 255;
        fresh.colour[2] = 255;
        fresh.colour[3] = 255;
        return fresh;
    };

    size_t pos = 4;
    while (pos < end)
    {
        u32 code = ReadWord(packet, pos * sizeof(u32));
        u8 command = static_cast<u8>(code >> 24);
        if (command >= 0x60 && command <= 0x6F)
        {
            u32 components = UnpackComponents(command);
            u32 elementSize = UnpackElementSize(command, components);
            u32 count = (code >> 16) & 0xFF;
            u32 address = code & 0x3FFF;
            if (out->rowCodeCount < 8)
            {
                out->rowCodes[out->rowCodeCount++] = code;
            }
            // The address-3 row's element size: 4 words is a V4-32 float row (the layout the host reads), 2 words a
            // V4-16 halfword row (the skins' layout, which reading as floats would explode); anything else is not the
            // rigid layout the builder assumes
            if (address == 3)
            {
                if (elementSize == 4)
                {
                    out->positionRows32++;
                }
                else if (elementSize == 2)
                {
                    out->positionRows16++;
                }
                else
                {
                    out->unknownRow++;
                }
            }
            if (pos + 1 + static_cast<size_t>(elementSize) * count > end)
            {
                return false;
            }

            if (dumpFile != nullptr)
            {
                const char* branch = address == 3 ? "pos" : address == 4 ? "uv/col" : address == 5 ? "normal" :
                                     address == 6 ? "packed-col" : "other";
                std::fprintf(dumpFile,
                             "  UNPACK cmd %02x code %08x addr %u (0x%04x) components %u elementSize %u count %u "
                             "words [%zu..%zu) branch %s\n",
                             command, code, address, code & VuAddressMask, components, elementSize, count, pos + 1,
                             pos + 1 + static_cast<size_t>(elementSize) * count, branch);
                if (address == 4 && components >= 4)
                {
                    for (u32 index = 0; index < count && index < 4; index++)
                    {
                        size_t offset = (pos + 1 + index * elementSize) * sizeof(u32);
                        u32 w0 = ReadWord(packet, offset);
                        u32 w1 = ReadWord(packet, offset + 4);
                        u32 w2 = ReadWord(packet, offset + 8);
                        u32 w3 = ReadWord(packet, offset + 12);
                        std::fprintf(dumpFile,
                                     "    addr4 v%u: U=%f(%08x low %02x) V=%f(%08x low %02x) w2=%08x(low %02x) "
                                     "w3=%08x(low %02x adc %u)\n",
                                     index, AsFloat(w0), w0, w0 & 0xFF, AsFloat(w1), w1, w1 & 0xFF, w2, w2 & 0xFF, w3,
                                     w3 & 0xFF, (w3 & 0x8000) != 0 ? 1 : 0);
                    }
                }
                if (address == 1 || address == 2)
                {
                    for (u32 index = 0; index < count && index < 4; index++)
                    {
                        size_t offset = (pos + 1 + index * elementSize) * sizeof(u32);
                        u32 w0 = ReadWord(packet, offset);
                        u32 w1 = ReadWord(packet, offset + 4);
                        std::fprintf(dumpFile, "    addr%u v%u: %08x %08x -> floats %g %g (ints %u %u)\n", address,
                                     index, w0, w1, static_cast<double>(AsFloat(w0)), static_cast<double>(AsFloat(w1)),
                                     w0, w1);
                    }
                }
            }

            if (address == 3 || address == 4 || address == 5 || address == 6)
            {
                // Each row is a strip of `count` vertices appended to the model's vertex list; the positions come first and
                // fix the strip's start, the UV/colour and normal rows that follow index from it. Every row itself appends
                // (never re-indexes an earlier row): the rows are the packet's own per-strip layout, as the reference decoder
                // (desktop/graphics.cpp's DecodeSubModel) reads them
                u32 base = 0;
                if (address == 3)
                {
                    stripStart = static_cast<u32>(vertices.size());
                    stripVertices = count;
                    havePositions = true;
                    base = static_cast<u32>(vertices.size());
                    vertices.resize(vertices.size() + count, Fresh());
                    noDraw.resize(vertices.size(), 0);
                    hasNormal.resize(vertices.size(), 0);
                }
                else
                {
                    if (!havePositions || stripStart + count > vertexCount)
                    {
                        return false;
                    }

                    base = stripStart;
                }

                if (count == 0 || base + count > vertexCount)
                {
                    return false;
                }

                if (address != 3)
                {
                    // The row must land on vertexes the position row already appended
                    if (base + count > vertices.size())
                    {
                        return false;
                    }
                }

                for (u32 index = 0; index < count; index++)
                {
                    size_t offset = (pos + 1 + index * elementSize) * sizeof(u32);
                    MeshVertex& vertex = vertices[base + index];
                    if (address == 3)
                    {
                        vertex.x = ReadFloat(packet, offset);
                        vertex.y = ReadFloat(packet, offset + 4);
                        vertex.z = ReadFloat(packet, offset + 8);
                    }
                    else if (address == 4)
                    {
                        vertex.u = ReadFloat(packet, offset);
                        vertex.v = ReadFloat(packet, offset + 4);
                        if (index == 0)
                        {
                            out->address4Sample[0] = ReadWord(packet, offset);
                            out->address4Sample[1] = ReadWord(packet, offset + 4);
                            out->address4Sample[2] = ReadWord(packet, offset + 8);
                            out->address4Sample[3] = ReadWord(packet, offset + 12);
                        }
                        if (components >= 4)
                        {
                            addr4Colours++;
                            // The UV/colour row's four words each carry a colour channel in their low byte (the reference
                            // decoder's DecodeSubModel: r the first word, then green, blue and alpha). The colour's red,
                            // green and blue are full at 192, its alpha at 128, scaled to the window's 255 (ScaleChannel)
                            if (!NoVertexColour())
                            {
                                const u32 vertexFull = VertexColourFull();
                                vertex.colour[0] = ScaleChannel(static_cast<u8>(ReadWord(packet, offset) & 0xFF), vertexFull);
                                vertex.colour[1] =
                                    ScaleChannel(static_cast<u8>(ReadWord(packet, offset + 4) & 0xFF), vertexFull);
                                vertex.colour[2] =
                                    ScaleChannel(static_cast<u8>(ReadWord(packet, offset + 8) & 0xFF), vertexFull);
                                vertex.colour[3] = ScaleChannel(static_cast<u8>(ReadWord(packet, offset + 12) & 0xFF), 128);
                            }
                            // The fourth word's bit 15 is the ADC/no-draw flag: the triangle ending at this vertex is dropped
                            noDraw[base + index] =
                                (ReadWord(packet, offset + 12) & 0x8000) != 0 ? 1 : 0;
                        }
                    }
                    else if (address == 5)
                    {
                        // The rigid vertex's own normal row, three 32 bit floats (TT-Lab: TwinVIFCompiler's
                        // NormalAddress row, read back at PS2SubModel.CalculateData; the game's normals are not always
                        // normalized and their lowest bits carry flags). The model has the row or it does not -- TT-Lab
                        // compiles it only when hasNormals -- so what is absent stays the sentinel for the pass below
                        vertex.normal[0] = NormalByte(ReadFloat(packet, offset));
                        vertex.normal[1] = NormalByte(ReadFloat(packet, offset + 4));
                        vertex.normal[2] = NormalByte(ReadFloat(packet, offset + 8));
                        vertex.normal[3] = 0;
                        hasNormal[base + index] = 1;
                    }
                    else
                    {
                        // Address 6 (0x6E): one packed RGBA word a vertex, the colour bytes red lowest (the reference
                        // decoder's own order: r the low byte, then green, blue and alpha), scaled like the UV/colour row's
                        u32 packed = ReadWord(packet, offset);
                        addr6Colours++;
                        if (!NoVertexColour())
                        {
                            const u32 skinFull = VertexColourFull();
                            vertex.colour[0] = ScaleChannel(static_cast<u8>(packed & 0xFF), skinFull);
                            vertex.colour[1] = ScaleChannel(static_cast<u8>((packed >> 8) & 0xFF), skinFull);
                            vertex.colour[2] = ScaleChannel(static_cast<u8>((packed >> 16) & 0xFF), skinFull);
                            vertex.colour[3] = ScaleChannel(static_cast<u8>((packed >> 24) & 0xFF), 128);
                        }
                    }
                }
            }

            pos += 1 + static_cast<size_t>(elementSize) * count;
            continue;
        }

        if (command == VifMscal)
        {
            if (havePositions && stripVertices != 0)
            {
                strips.push_back({stripStart, stripVertices});
            }

            havePositions = false;
            stripVertices = 0;
            pos += 2;  // the MSCAL and the programme's address
            continue;
        }

        if (command == VifStcycl || command == 0x00 || command == 0x07)
        {
            pos += 1;
            continue;
        }

        return false;
    }

    if (havePositions && stripVertices != 0)
    {
        strips.push_back({stripStart, stripVertices});
    }

    if (vertices.empty() || vertices.size() > 0xFFFF)
    {
        return false;
    }

    // A rigid model whose packet carries no normal row is lit flat: every vertex that never got one takes its own
    // triangle's face normal, normalised, which is what TT-Lab's viewer does for such a mesh (MeshBuilder's rigid build
    // generates cross(b - a, c - a) when no vertex has a normal). The face is turned away from the model's centre, because
    // the winding the host's rasterizer sees is a mirror of the one the console's VU1 tests -- the volume pass's cull
    // states had to be swapped for the same reason -- and without the test a whole mesh would light from behind
    {
        f32 centre[3] = {0.0f, 0.0f, 0.0f};
        for (const MeshVertex& vertex : vertices)
        {
            centre[0] += vertex.x;
            centre[1] += vertex.y;
            centre[2] += vertex.z;
        }

        const f32 count = static_cast<f32>(vertices.size());
        centre[0] /= count;
        centre[1] /= count;
        centre[2] /= count;

        for (const Strip& strip : strips)
        {
            for (u32 index = 2; index < strip.count; index++)
            {
                if (noDraw[strip.first + index] != 0)
                {
                    continue;
                }

                const u32 corners[3] = {FanTopology() ? strip.first : strip.first + index - 2, strip.first + index - 1,
                                        strip.first + index};
                const MeshVertex& first = vertices[corners[0]];
                const MeshVertex& second = vertices[corners[1]];
                const MeshVertex& third = vertices[corners[2]];
                const f32 edge0[3] = {second.x - first.x, second.y - first.y, second.z - first.z};
                const f32 edge1[3] = {third.x - first.x, third.y - first.y, third.z - first.z};
                f32 normal[3] = {edge0[1] * edge1[2] - edge0[2] * edge1[1], edge0[2] * edge1[0] - edge0[0] * edge1[2],
                                 edge0[0] * edge1[1] - edge0[1] * edge1[0]};
                const f32 length = std::sqrt(normal[0] * normal[0] + normal[1] * normal[1] + normal[2] * normal[2]);
                if (!(length > 1.0e-6f))
                {
                    continue;
                }

                for (f32& lane : normal)
                {
                    lane /= length;
                }

                const f32 toFace[3] = {(first.x + second.x + third.x) / 3.0f - centre[0],
                                       (first.y + second.y + third.y) / 3.0f - centre[1],
                                       (first.z + second.z + third.z) / 3.0f - centre[2]};
                if (normal[0] * toFace[0] + normal[1] * toFace[1] + normal[2] * toFace[2] < 0.0f)
                {
                    for (f32& lane : normal)
                    {
                        lane = -lane;
                    }
                }

                for (u32 corner = 0; corner < 3; corner++)
                {
                    MeshVertex& vertex = vertices[corners[corner]];
                    if (hasNormal[corners[corner]] != 0)
                    {
                        continue;
                    }

                    vertex.normal[0] = NormalByte(normal[0]);
                    vertex.normal[1] = NormalByte(normal[1]);
                    vertex.normal[2] = NormalByte(normal[2]);
                    vertex.normal[3] = 0;
                }
            }
        }
    }

    // Each strip is the sliding window of triangles (i-2, i-1, i) -- the reference's DrawTriangleStrip3D over one
    // strip, the same ordering the rigid meshes' shared vertex sequence needs. A vertex whose no-draw bit (address 4's
    // fourth word, bit 15) is set takes no triangle ending at it. The compact list drops vertices no triangle used
    std::vector<s32> remap(vertices.size(), -1);
    std::vector<MeshVertex> compact;
    compact.reserve(vertices.size());
    auto Remap = [&](u32 source) {
        if (remap[source] < 0)
        {
            remap[source] = static_cast<s32>(compact.size());
            compact.push_back(vertices[source]);
        }

        return static_cast<u16>(remap[source]);
    };
    for (const Strip& strip : strips)
    {
        if (strip.count < 3)
        {
            continue;
        }

        for (u32 index = 2; index < strip.count; index++)
        {
            if (noDraw[strip.first + index] != 0)
            {
                continue;
            }

            if (FanTopology())
            {
                out->indexes.push_back(Remap(strip.first));
                out->indexes.push_back(Remap(strip.first + index - 1));
                out->indexes.push_back(Remap(strip.first + index));
                continue;
            }

            // A strip's odd triangles are flipped (the GL strip rule): their first two vertices swap so every triangle winds
            // the same way. Without this the list alternates winding and a culled pass (the shadow volumes') keeps and drops
            // half the faces of every strip-built mesh
            if ((index & 1) != 0)
            {
                out->indexes.push_back(Remap(strip.first + index - 1));
                out->indexes.push_back(Remap(strip.first + index - 2));
            }
            else
            {
                out->indexes.push_back(Remap(strip.first + index - 2));
                out->indexes.push_back(Remap(strip.first + index - 1));
            }

            out->indexes.push_back(Remap(strip.first + index));
        }
    }

    if (compact.empty() || out->indexes.empty())
    {
        return false;
    }

    out->vertexCount = static_cast<u32>(compact.size());
    out->indexCount = static_cast<u32>(out->indexes.size());
    out->vertices.resize(compact.size() * sizeof(MeshVertex));
    std::memcpy(out->vertices.data(), compact.data(), out->vertices.size());
    out->boundsMin[0] = compact[0].x;
    out->boundsMin[1] = compact[0].y;
    out->boundsMin[2] = compact[0].z;
    out->boundsMax[0] = compact[0].x;
    out->boundsMax[1] = compact[0].y;
    out->boundsMax[2] = compact[0].z;
    for (const MeshVertex& vertex : compact)
    {
        if (vertex.x < out->boundsMin[0])
        {
            out->boundsMin[0] = vertex.x;
        }

        if (vertex.y < out->boundsMin[1])
        {
            out->boundsMin[1] = vertex.y;
        }

        if (vertex.z < out->boundsMin[2])
        {
            out->boundsMin[2] = vertex.z;
        }

        if (vertex.x > out->boundsMax[0])
        {
            out->boundsMax[0] = vertex.x;
        }

        if (vertex.y > out->boundsMax[1])
        {
            out->boundsMax[1] = vertex.y;
        }

        if (vertex.z > out->boundsMax[2])
        {
            out->boundsMax[2] = vertex.z;
        }
    }

    out->checksum = Crc32(out->vertices.data(), out->vertices.size());
    out->address4Colours = addr4Colours;
    out->address6Colours = addr6Colours;
    if (dumpFile != nullptr)
    {
        std::fprintf(dumpFile, "  RIGID decoded %zu verts %zu indexes bounds [%f %f %f]-[%f %f %f] checksum %08x\n",
                     compact.size(), out->indexes.size(), out->boundsMin[0], out->boundsMin[1], out->boundsMin[2],
                     out->boundsMax[0], out->boundsMax[1], out->boundsMax[2], out->checksum);
        u32 redCount = 0;
        u32 opaqueWhite = 0;
        f32 minU = 1.0e30f;
        f32 maxU = -1.0e30f;
        f32 minV = 1.0e30f;
        f32 maxV = -1.0e30f;
        for (size_t index = 0; index < compact.size(); index++)
        {
            const MeshVertex& v = compact[index];
            minU = v.u < minU ? v.u : minU;
            maxU = v.u > maxU ? v.u : maxU;
            minV = v.v < minV ? v.v : minV;
            maxV = v.v > maxV ? v.v : maxV;
            if (index < 12)
            {
                std::fprintf(dumpFile, "    v%zu pos %f %f %f uv %f %f colour %u %u %u %u normal %u %u %u %u\n", index,
                             v.x, v.y, v.z, v.u, v.v, v.colour[0], v.colour[1], v.colour[2], v.colour[3], v.normal[0],
                             v.normal[1], v.normal[2], v.normal[3]);
            }
            if (v.colour[0] > 200 && v.colour[1] < 40 && v.colour[2] < 40)
            {
                redCount++;
            }
            if (v.colour[0] == 255 && v.colour[1] == 255 && v.colour[2] == 255 && v.colour[3] == 255)
            {
                opaqueWhite++;
            }
        }
        std::fprintf(dumpFile, "    colours: red-ish %u, opaque white %u of %zu\n", redCount, opaqueWhite,
                     compact.size());
        std::fprintf(dumpFile, "    uv range u [%g %g] v [%g %g]\n", static_cast<double>(minU),
                     static_cast<double>(maxU), static_cast<double>(minV), static_cast<double>(maxV));
        u8 minC[4] = {255, 255, 255, 255};
        u8 maxC[4] = {0, 0, 0, 0};
        for (const MeshVertex& v : compact)
        {
            for (u32 channel = 0; channel < 4; channel++)
            {
                minC[channel] = v.colour[channel] < minC[channel] ? v.colour[channel] : minC[channel];
                maxC[channel] = v.colour[channel] > maxC[channel] ? v.colour[channel] : maxC[channel];
            }
        }
        std::fprintf(dumpFile, "    colour bytes min %u %u %u %u max %u %u %u %u (address-4 %u, address-6 %u)\n",
                     minC[0], minC[1], minC[2], minC[3], maxC[0], maxC[1], maxC[2], maxC[3], addr4Colours,
                     addr6Colours);
        if (!compact.empty())
        {
            const u8* raw = reinterpret_cast<const u8*>(&compact[0]);
            std::fprintf(dumpFile, "    v0 raw28:");
            for (size_t byte = 0; byte < sizeof(MeshVertex); byte++)
            {
                std::fprintf(dumpFile, " %02x", raw[byte]);
            }
            std::fprintf(dumpFile, "\n");
        }
    }

    return true;
}

// The skin rows' VU1 destinations (UNPACK low 16 bits, masked of the FLG bit), as the TT-Lab reference decoder's
// DecodeSkinSubModel reads them: address 2 (the
// packet's scales), address 3 (V4-16: positions X/Y/Z and the normal's X), address 4
// (V4-16: UV U/V and the normal's Y/Z), address 5 (V4-32: joint indices and weights +
// the joint count/ADC flags), address 6 (V4-8: the packed RGBA colour). The host walk
// takes the same four rows.
constexpr u32 VuSkinScaleAddress = 0x0002;
constexpr u32 VuSkinPositionAddress = 0x0003;
constexpr u32 VuSkinUvAddress = 0x0004;
constexpr u32 VuSkinJointAddress = 0x0005;
constexpr u32 VuSkinPackedAddress = 0x0006;
constexpr u32 VuSkinAddressMask = 0x3FFF;
constexpr u8 VifUnpackV416 = 0x6D;
// The address-5 lane 3: its low byte is how many joints this vertex uses (1-3), its bit
// 15 the strip-chain ADC flag (the triangle ending at this vertex draws no triangle)
constexpr u32 SkinJointCountMask = 0xFF;
constexpr u32 SkinChainFlag = 0x8000;
constexpr u32 SkinJointsPerVertex = 3;
// The address-3/4 rows' default scales until the packet's address-2 row sets them
// (the VU1 program's init: 1/16384 for positions, 1/2048 for coordinates)
constexpr f32 SkinDefaultPositionScale = 1.0f / 16384.0f;
constexpr f32 SkinDefaultUvScale = 1.0f / 2048.0f;
// The address-3/4 lanes' 16 bit sign extension (VIF's own)
constexpr u32 HalfwordMask = 0xFFFF;
constexpr u32 PackedIndexMask = 0x1FF;
constexpr u32 PackedIndexShift = 2;

bool ParseSkinSubModel(const u8* packet, size_t packetSize, u32 vertexCount, u32 shapeCount, Ps2SkinMesh* out)
{
    if (packet == nullptr || packetSize < 16 || (packetSize & 3) != 0 || out == nullptr)
    {
        return false;
    }

    size_t words = packetSize / sizeof(u32);
    std::vector<Ps2SkinVertex> vertices;
    if (vertexCount != 0)
    {
        vertices.reserve(vertexCount);
    }
    std::vector<u8> noDraw;

    // The reference decoder's walk (DecodeSkinSubModel): the
    // packet's rows are indexed from the current strip's first vertex (`stripStart`); an UNPACK to
    // VU1 address 0 (the strip's GIF header) closes the strip that was open and starts the next,
    // and address 3's row advances `stripStart` by its count (so the UV/normal/colour/joint rows
    // that follow write into the same vertices). STMOD selects offset mode and STROW's four row
    // registers are added to the packed lanes before the scale.
    u32 stripStart = 0;
    u32 row[4] = {0, 0, 0, 0};
    bool offsetMode = false;
    f32 positionScale = SkinDefaultPositionScale;
    f32 uvScale = SkinDefaultUvScale;
    bool started = false;
    bool ok = true;
    // The packet is one RET DMA tag (the size word and its zero) then the VIF1 program, exactly as
    // the reference's own decode starts (its `word = 2`)
    size_t pos = 2;

    const bool dump = MeshDumpWanted("skin");
    FILE* dumpFile = dump ? MeshDumpFile() : nullptr;
    if (dumpFile != nullptr)
    {
        std::fprintf(dumpFile, "SKIN packet %p size %zu words %zu vertexCount %u shapeCount %u\n",
                     static_cast<const void*>(packet), packetSize, words, vertexCount, shapeCount);
        for (size_t index = 0; index < words; index++)
        {
            std::fprintf(dumpFile, "  w%04zu %08x\n", index, ReadWord(packet, index * sizeof(u32)));
        }
    }

    // A strip's four rows are collected before they can be assembled, their counts matched
    std::vector<u16> rowA;
    std::vector<u16> rowNormalX;
    std::vector<u16> rowB;
    std::vector<u32> rowC;
    std::vector<u32> rowD;
    u32 rowCount = 0;
    // The offset the position row and the coordinate row each had when they were read: STMOD/STROW are re-sent
    // between the rows of a strip (a STROW of zeros before the coordinates on the retail packets), so the values
    // in effect at that row -- not the last the walk saw -- are the ones the VU1 adds (the reference's decode adds
    // them inline, row by row)
    u32 positionRow[4] = {0, 0, 0, 0};
    u32 uvRow[4] = {0, 0, 0, 0};
    bool positionOffset = false;
    bool uvOffset = false;

    auto FlushStrip = [&]() {
        if (dumpFile != nullptr)
        {
            std::fprintf(dumpFile,
                         "  FLUSH started %d rowCount %u posOffset %d posRow %08x %08x %08x %08x uvOffset %d "
                         "uvRow %08x %08x %08x %08x scale %g\n",
                         started ? 1 : 0, rowCount, positionOffset ? 1 : 0, positionRow[0], positionRow[1],
                         positionRow[2], positionRow[3], uvOffset ? 1 : 0, uvRow[0], uvRow[1], uvRow[2], uvRow[3],
                         static_cast<double>(positionScale));
        }

        if (!started || rowCount == 0)
        {
            return;
        }
        if (vertices.size() + rowCount > vertexCount && vertexCount != 0)
        {
            ok = false;
            return;
        }
        size_t first = vertices.size();
        noDraw.resize(first + rowCount, 0);
        for (u32 vertex = 0; vertex < rowCount; vertex++)
        {
            Ps2SkinVertex outVertex{};
            s32 x = static_cast<s16>(rowA[vertex * 3 + 0]);
            s32 y = static_cast<s16>(rowA[vertex * 3 + 1]);
            s32 z = static_cast<s16>(rowA[vertex * 3 + 2]);
            s32 normalX = static_cast<s16>(rowNormalX[vertex]);
            s32 u = static_cast<s16>(rowB[vertex * 4 + 0]);
            s32 v = static_cast<s16>(rowB[vertex * 4 + 1]);
            s32 normalY = static_cast<s16>(rowB[vertex * 4 + 2]);
            s32 normalZ = static_cast<s16>(rowB[vertex * 4 + 3]);
            if (positionOffset)
            {
                // The position row's row registers are added to the packed components before the scale
                x += static_cast<s32>(positionRow[0]);
                y += static_cast<s32>(positionRow[1]);
                z += static_cast<s32>(positionRow[2]);
                normalX += static_cast<s32>(positionRow[3]);
            }
            if (uvOffset)
            {
                // The coordinate row's own
                u += static_cast<s32>(uvRow[0]);
                v += static_cast<s32>(uvRow[1]);
                normalY += static_cast<s32>(uvRow[2]);
                normalZ += static_cast<s32>(uvRow[3]);
            }
            outVertex.x = static_cast<f32>(x) * positionScale;
            outVertex.y = static_cast<f32>(y) * positionScale;
            outVertex.z = static_cast<f32>(z) * positionScale;
            outVertex.u = static_cast<f32>(u) * uvScale;
            outVertex.v = static_cast<f32>(v) * uvScale;
            // The normal's X rides the position row's lane 3 (position scale), its Y and Z the
            // coordinate row's lanes 2-3 (UV scale), as the reference unpacks them
            outVertex.normal[0] = NormalByte(static_cast<f32>(normalX) * positionScale);
            outVertex.normal[1] = NormalByte(static_cast<f32>(normalY) * uvScale);
            outVertex.normal[2] = NormalByte(static_cast<f32>(normalZ) * uvScale);
            outVertex.normal[3] = 255;
            // The colour word red lowest, then green, blue and alpha; the reference scales the
            // channels at draw time (192 for RGB, 128 for alpha), so the decode does the same here
            // and shares the fragment shader's modulation with the rigid path
            const u32 packed = vertex < rowC.size() ? rowC[vertex] : 0xFFFFFFFFu;
            const u32 blendFull = VertexColourFull();
            outVertex.colour[0] = ScaleChannel(static_cast<u8>(packed & 0xFF), blendFull);
            outVertex.colour[1] = ScaleChannel(static_cast<u8>((packed >> 8) & 0xFF), blendFull);
            outVertex.colour[2] = ScaleChannel(static_cast<u8>((packed >> 16) & 0xFF), blendFull);
            outVertex.colour[3] = ScaleChannel(static_cast<u8>((packed >> 24) & 0xFF), 128);

            const u32 joint0 = rowD[vertex * V4Words + 0];
            const u32 joint1 = rowD[vertex * V4Words + 1];
            const u32 joint2 = rowD[vertex * V4Words + 2];
            const u32 flags = rowD[vertex * V4Words + 3];
            u32 amount = flags & SkinJointCountMask;
            if (amount == 0)
            {
                amount = 1;
            }
            else if (amount > SkinJointsPerVertex)
            {
                amount = SkinJointsPerVertex;
            }
            const u32 indices[3] = {joint0, joint1, joint2};
            for (u32 joint = 0; joint < SkinJointsPerVertex; joint++)
            {
                // The low 9 bits are the joint index times 4; the upper bits hold the weight float
                // the exporter packed above it (the reference reads it back as `AsFloat(word & ~0x1FF)`).
                // A joint the vertex does not use takes index 0: its weight is zero either way, but the
                // packed word's leftover bits can name any index up to 127 and the shader indexes
                // u_joints[SkinMaxJoints] with it, so an unused lane must never name a slot past the
                // array (the reference skips out-of-range joints outright, desktop/graphics.cpp's DrawSkin)
                outVertex.jointIdx[joint] =
                    joint < amount ? static_cast<f32>((indices[joint] & PackedIndexMask) >> PackedIndexShift) : 0.0f;
                outVertex.jointWeight[joint] = joint < amount ? AsFloat(indices[joint] & ~PackedIndexMask) : 0.0f;
                if (joint < amount && outVertex.jointIdx[joint] >= out->jointCount)
                {
                    out->jointCount = outVertex.jointIdx[joint] + 1;
                }
            }
            noDraw[first + vertex] = (flags & SkinChainFlag) != 0 ? 1 : 0;
            vertices.push_back(outVertex);
        }
        if (out->indexes.capacity() == 0 && vertices.capacity() != 0)
        {
            out->indexes.reserve((vertices.capacity() - 2) * 3);
        }
        // The strip is a run of triangles (i - 2, i - 1, i), like the rigid path and the reference
        // port (desktop/graphics.cpp:1739/1763): the GS never backface-culls and the same vertex
        // sequence the rigid meshes use. A vertex whose address-5 lane-3 bit 15 is set (the GS's
        // ADC) draws no triangle, exactly the reference's skip
        for (u32 vertex = 2; vertex < rowCount; vertex++)
        {
            if (noDraw[first + vertex] != 0)
            {
                continue;
            }

            if (FanTopology())
            {
                out->indexes.push_back(static_cast<u16>(first));
                out->indexes.push_back(static_cast<u16>(first + vertex - 1));
                out->indexes.push_back(static_cast<u16>(first + vertex));
                continue;
            }

            out->indexes.push_back(static_cast<u16>(first + vertex - 2));
            out->indexes.push_back(static_cast<u16>(first + vertex - 1));
            out->indexes.push_back(static_cast<u16>(first + vertex));
        }
    };

    while (pos < words && ok)
    {
        u32 code = ReadWord(packet, pos * sizeof(u32));
        u8 command = static_cast<u8>(code >> 24);
        if (command >= 0x60 && command <= 0x6F)
        {
            u32 components = UnpackComponents(command);
            u32 elementSize = UnpackElementSize(command, components);
            u32 count = (code >> 16) & 0xFF;
            u32 address = code & VuSkinAddressMask;
            if (pos + 1 + static_cast<size_t>(elementSize) * count > words)
            {
                ok = false;
                break;
            }
            const u32* data = reinterpret_cast<const u32*>(packet + (pos + 1) * sizeof(u32));
            if (dumpFile != nullptr)
            {
                const char* branch = address == 0 ? "gif-header" : address == 2 ? "scales" : address == 3 ? "position" :
                                     address == 4 ? "uv/normal" : address == 5 ? "joints" :
                                     address == 6 ? "packed-col" : "other";
                std::fprintf(dumpFile,
                             "  UNPACK cmd %02x code %08x addr %u (0x%04x) components %u elementSize %u count %u "
                             "words [%zu..%zu) branch %s\n",
                             command, code, address, code & VuSkinAddressMask, components, elementSize, count, pos + 1,
                             pos + 1 + static_cast<size_t>(elementSize) * count, branch);
                if (address == 5)
                {
                    for (u32 vertex = 0; vertex < count && vertex < 4; vertex++)
                    {
                        std::fprintf(dumpFile, "    addr5 v%u: j0 %08x j1 %08x j2 %08x flags %08x -> idx %u %u %u "
                                               "count %u chain %u\n",
                                     vertex, data[vertex * 4 + 0], data[vertex * 4 + 1], data[vertex * 4 + 2],
                                     data[vertex * 4 + 3], (data[vertex * 4 + 0] & PackedIndexMask) >> PackedIndexShift,
                                     (data[vertex * 4 + 1] & PackedIndexMask) >> PackedIndexShift,
                                     (data[vertex * 4 + 2] & PackedIndexMask) >> PackedIndexShift,
                                     data[vertex * 4 + 3] & SkinJointCountMask,
                                     (data[vertex * 4 + 3] & SkinChainFlag) != 0 ? 1 : 0);
                    }
                }
            }
            if (address == 0)
            {
                // The strip's GIF header: the open strip is done, the next starts here
                FlushStrip();
                rowA.clear();
                rowNormalX.clear();
                rowB.clear();
                rowC.clear();
                rowD.clear();
                rowCount = 0;
                started = true;
            }
            if (address == VuSkinScaleAddress)
            {
                if (count >= 1)
                {
                    positionScale = AsFloat(data[0]);
                }
                if (count >= 2)
                {
                    uvScale = AsFloat(data[1]);
                }
            }
            else if (address == VuSkinPositionAddress)
            {
                rowA.resize(count * 3);
                rowNormalX.resize(count);
                for (u32 vertex = 0; vertex < count; vertex++)
                {
                    const u32 low = data[vertex * 2];
                    const u32 high = data[vertex * 2 + 1];
                    rowA[vertex * 3 + 0] = static_cast<u16>(low & HalfwordMask);
                    rowA[vertex * 3 + 1] = static_cast<u16>(low >> 16);
                    rowA[vertex * 3 + 2] = static_cast<u16>(high & HalfwordMask);
                    rowNormalX[vertex] = static_cast<u16>(high >> 16);
                }
                rowCount = count;
                if (stripStart + count > vertexCount && vertexCount != 0)
                {
                    ok = false;
                    break;
                }
                positionOffset = offsetMode;
                for (u32 lane = 0; lane < 4; lane++)
                {
                    positionRow[lane] = row[lane];
                }
                stripStart = static_cast<u32>(vertices.size());
            }
            else if (address == VuSkinUvAddress)
            {
                rowB.resize(count * 4);
                uvOffset = offsetMode;
                for (u32 lane = 0; lane < 4; lane++)
                {
                    uvRow[lane] = row[lane];
                }
                for (u32 vertex = 0; vertex < count; vertex++)
                {
                    const u32 low = data[vertex * 2];
                    const u32 high = data[vertex * 2 + 1];
                    rowB[vertex * 4 + 0] = static_cast<u16>(low & HalfwordMask);
                    rowB[vertex * 4 + 1] = static_cast<u16>(low >> 16);
                    rowB[vertex * 4 + 2] = static_cast<u16>(high & HalfwordMask);
                    rowB[vertex * 4 + 3] = static_cast<u16>(high >> 16);
                }
            }
            else if (address == VuSkinPackedAddress)
            {
                rowC.assign(data, data + count);
            }
            else if (address == VuSkinJointAddress)
            {
                rowD.assign(data, data + count * V4Words);
            }

            pos += 1 + static_cast<size_t>(elementSize) * count;
            continue;
        }

        switch (command)
        {
        case VifMscal:
        case 0x00:  // NOP
        case 0x01:  // STCYCL
        case 0x04:  // ITOP
        case 0x07:  // MARK
        case 0x10:  // FLUSHA
        case 0x11:  // FLUSH
        case 0x13:  // FLUSHE
        case 0x15:  // MSCALF
        case 0x17:  // MSCNT
            pos += 1;
            break;
        case 0x05:  // STMOD: the row registers are added to the packed components in offset mode
            offsetMode = (code & 3) == 1;
            pos += 1;
            break;
        case 0x30:  // STROW: the four row registers
            if (pos + 5 > words)
            {
                ok = false;
                break;
            }
            for (u32 lane = 0; lane < 4; lane++)
            {
                row[lane] = ReadWord(packet, (pos + 1 + lane) * sizeof(u32));
            }
            pos += 5;
            break;
        case 0x31:  // STCOL
            pos += 5;
            break;
        case 0x20:  // STMASK
            pos += 2;
            break;
        default:
            ok = false;
            break;
        }
    }

    if (ok)
    {
        FlushStrip();
    }

    if (!ok || vertices.empty() || (vertexCount != 0 && vertices.size() != vertexCount) ||
        vertices.size() > 0xFFFF)
    {
        return false;
    }

    out->vertexCount = static_cast<u32>(vertices.size());
    out->indexCount = static_cast<u32>(out->indexes.size());
    out->shapeCount = shapeCount;
    out->vertices.resize(vertices.size() * sizeof(Ps2SkinVertex));
    std::memcpy(out->vertices.data(), vertices.data(), out->vertices.size());
    out->checksum = Crc32(out->vertices.data(), out->vertices.size());
    if (dumpFile != nullptr)
    {
        std::fprintf(dumpFile, "  SKIN decoded %zu verts %zu indexes joints %u checksum %08x\n", vertices.size(),
                     out->indexes.size(), out->jointCount, out->checksum);
        for (size_t index = 0; index < vertices.size() && index < 12; index++)
        {
            const Ps2SkinVertex& v = vertices[index];
            std::fprintf(dumpFile, "    v%zu pos %f %f %f uv %f %f colour %u %u %u %u normal %u %u %u %u "
                                   "joints %u %u %u weights %f %f %f\n",
                         index, v.x, v.y, v.z, v.u, v.v, v.colour[0], v.colour[1], v.colour[2], v.colour[3],
                         v.normal[0], v.normal[1], v.normal[2], v.normal[3], 
                         static_cast<u32>(v.jointIdx[0]), static_cast<u32>(v.jointIdx[1]), static_cast<u32>(v.jointIdx[2]),
                         v.jointWeight[0], v.jointWeight[1], v.jointWeight[2]);
        }
        f32 minU = 1.0e30f;
        f32 maxU = -1.0e30f;
        f32 minV = 1.0e30f;
        f32 maxV = -1.0e30f;
        for (const Ps2SkinVertex& v : vertices)
        {
            minU = v.u < minU ? v.u : minU;
            maxU = v.u > maxU ? v.u : maxU;
            minV = v.v < minV ? v.v : minV;
            maxV = v.v > maxV ? v.v : maxV;
        }
        std::fprintf(dumpFile, "    uv range u [%g %g] v [%g %g]\n", static_cast<double>(minU),
                     static_cast<double>(maxU), static_cast<double>(minV), static_cast<double>(maxV));
        u8 minC[4] = {255, 255, 255, 255};
        u8 maxC[4] = {0, 0, 0, 0};
        u32 minJoint = 255;
        u32 maxJoint = 0;
        for (const Ps2SkinVertex& v : vertices)
        {
            for (u32 channel = 0; channel < 4; channel++)
            {
                minC[channel] = v.colour[channel] < minC[channel] ? v.colour[channel] : minC[channel];
                maxC[channel] = v.colour[channel] > maxC[channel] ? v.colour[channel] : maxC[channel];
            }
            for (u32 joint = 0; joint < 3; joint++)
            {
                minJoint = v.jointIdx[joint] < minJoint ? v.jointIdx[joint] : minJoint;
                maxJoint = v.jointIdx[joint] > maxJoint ? v.jointIdx[joint] : maxJoint;
            }
        }
        std::fprintf(dumpFile, "    colour bytes min %u %u %u %u max %u %u %u %u joints [%u %u]\n", minC[0], minC[1],
                     minC[2], minC[3], maxC[0], maxC[1], maxC[2], maxC[3], minJoint, maxJoint);
        if (!vertices.empty())
        {
            const u8* raw = reinterpret_cast<const u8*>(&vertices[0]);
            std::fprintf(dumpFile, "    v0 raw44:");
            for (size_t byte = 0; byte < sizeof(Ps2SkinVertex); byte++)
            {
                std::fprintf(dumpFile, " %02x", raw[byte]);
            }
            std::fprintf(dumpFile, "\n");
        }
    }

    return true;
}

bool SkinJointUploadNeeded(u32 jointsAddress)
{
    static u32 lastJoints = 0;
    if (jointsAddress == lastJoints)
    {
        return false;
    }

    lastJoints = jointsAddress;
    return true;
}
