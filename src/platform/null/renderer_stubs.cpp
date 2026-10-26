// Stand-ins for the PS2 renderer's retail symbols: the renderer's own
// translation units are not compiled on the host, but the retail data (its vtables and jump
// tables) still refers to its functions by their retail names. These carry those labels so the
// link resolves; anything reached aborts like the rest of the null graphics, except the mesh
// and model constructors and readers the shadows' file loading calls, which make zeroed
// resources like the graphics resources' own constructors and readers do.
#include "platform/graphics.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace
{
[[noreturn]] void AbortStub(const char* name)
{
    std::fprintf(stderr, "null platform: %s not implemented\n", name);
    std::abort();
}
}
// The resources' sizes on the PS2 (their layouts', src/platform/ps2/renderer/renderer.h)
constexpr u32 RigidModelResourceSize = 0x20;
constexpr u32 ModelResourceSize = 0x1C;

extern "C"
{
// A mesh made in the memory given with its ID, like the graphics meshes' own constructor
RigidModel* FUN_001c1e70(void* memory, u32 id)
{
    std::memset(memory, 0, RigidModelResourceSize);
    static_cast<u32*>(memory)[1] = id;
    return static_cast<RigidModel*>(memory);
}

// A model made the same way
RigidModelData* InitGameModel(void* memory, u32 id)
{
    std::memset(memory, 0, ModelResourceSize);
    static_cast<u32*>(memory)[1] = id;
    return static_cast<RigidModelData*>(memory);
}

// A model and a mesh read: nothing on the null platform
void ReadGameModel(RigidModelData* model, Stream* stream)
{
    (void)model;
    (void)stream;
}

void ReadRigidModel2(RigidModel* mesh, Stream* stream)
{
    (void)mesh;
    (void)stream;
}

void FUN_001cd550()
{
    AbortStub("FUN_001cd550");
}

void FUN_001ce0b0()
{
    AbortStub("FUN_001ce0b0");
}

void FUN_001ce4a0()
{
    AbortStub("FUN_001ce4a0");
}

void FUN_001ce860()
{
    AbortStub("FUN_001ce860");
}

void FUN_001cec20()
{
    AbortStub("FUN_001cec20");
}

void FUN_001cef08()
{
    AbortStub("FUN_001cef08");
}

void FUN_001cf2a0()
{
    AbortStub("FUN_001cf2a0");
}

void FUN_001cf450()
{
    AbortStub("FUN_001cf450");
}

void FUN_001cf5e8()
{
    AbortStub("FUN_001cf5e8");
}

void FUN_001cf8a0()
{
    AbortStub("FUN_001cf8a0");
}

void FUN_001cfc88()
{
    AbortStub("FUN_001cfc88");
}

void FUN_001d0368()
{
    AbortStub("FUN_001d0368");
}

void FUN_001d0650()
{
    AbortStub("FUN_001d0650");
}

void FUN_001d0a38()
{
    AbortStub("FUN_001d0a38");
}

void FUN_001d0d20()
{
    AbortStub("FUN_001d0d20");
}

void FUN_001d10f0()
{
    AbortStub("FUN_001d10f0");
}

void FUN_001d14b0()
{
    AbortStub("FUN_001d14b0");
}

void FUN_001d1948()
{
    AbortStub("FUN_001d1948");
}

void FUN_001d1cf0()
{
    AbortStub("FUN_001d1cf0");
}

void FUN_001d2058()
{
    AbortStub("FUN_001d2058");
}

void FUN_001d24d8()
{
    AbortStub("FUN_001d24d8");
}

void FUN_001d2848()
{
    AbortStub("FUN_001d2848");
}

void FUN_001d2b88()
{
    AbortStub("FUN_001d2b88");
}

void FUN_001d2f70()
{
    AbortStub("FUN_001d2f70");
}

void FUN_001d3258()
{
    AbortStub("FUN_001d3258");
}

void FUN_001d3640()
{
    AbortStub("FUN_001d3640");
}

void FUN_001d3928()
{
    AbortStub("FUN_001d3928");
}

void FUN_001d3d10()
{
    AbortStub("FUN_001d3d10");
}

void FUN_001d3ff8()
{
    AbortStub("FUN_001d3ff8");
}

void FUN_001d43e0()
{
    AbortStub("FUN_001d43e0");
}

void FUN_001d46c8()
{
    AbortStub("FUN_001d46c8");
}

void FUN_001d4a88()
{
    AbortStub("FUN_001d4a88");
}

void FUN_001d4d70()
{
    AbortStub("FUN_001d4d70");
}

void FUN_001d5158()
{
    AbortStub("FUN_001d5158");
}

void FUN_001d5440()
{
    AbortStub("FUN_001d5440");
}

void FUN_001d5830()
{
    AbortStub("FUN_001d5830");
}

void FUN_001d5db8()
{
    AbortStub("FUN_001d5db8");
}

void FUN_001d6130()
{
    AbortStub("FUN_001d6130");
}

void FUN_001d6720()
{
    AbortStub("FUN_001d6720");
}

void FUN_001d6aa8()
{
    AbortStub("FUN_001d6aa8");
}

void FUN_001d6fb8()
{
    AbortStub("FUN_001d6fb8");
}

void FUN_001d7378()
{
    AbortStub("FUN_001d7378");
}

void FUN_001d7590()
{
    AbortStub("FUN_001d7590");
}

void FUN_001d7978()
{
    AbortStub("FUN_001d7978");
}

void FUN_001d7b90()
{
    AbortStub("FUN_001d7b90");
}

void FUN_001d7f18()
{
    AbortStub("FUN_001d7f18");
}

void FUN_001d8370()
{
    AbortStub("FUN_001d8370");
}

void FUN_001d8758()
{
    AbortStub("FUN_001d8758");
}

void FUN_001d8970()
{
    AbortStub("FUN_001d8970");
}

void FUN_001d8d30()
{
    AbortStub("FUN_001d8d30");
}

void FUN_001d9070()
{
    AbortStub("FUN_001d9070");
}

void FUN_001d9078()
{
    AbortStub("FUN_001d9078");
}

void FUN_001d9080()
{
    AbortStub("FUN_001d9080");
}

void FUN_001d91e0()
{
    AbortStub("FUN_001d91e0");
}

void FUN_001d9270()
{
    AbortStub("FUN_001d9270");
}

void FUN_001d9278()
{
    AbortStub("FUN_001d9278");
}

void FUN_001d9288()
{
    AbortStub("FUN_001d9288");
}

void FUN_001d92a0()
{
    AbortStub("FUN_001d92a0");
}

void FUN_001d9330()
{
    AbortStub("FUN_001d9330");
}

void FUN_001d9348()
{
    AbortStub("FUN_001d9348");
}

void FUN_001d9360()
{
    AbortStub("FUN_001d9360");
}

void FUN_001d93f0()
{
    AbortStub("FUN_001d93f0");
}

void FUN_001d93f8()
{
    AbortStub("FUN_001d93f8");
}

void FUN_001d9400()
{
    AbortStub("FUN_001d9400");
}

void FUN_001d9408()
{
    AbortStub("FUN_001d9408");
}

void FUN_001d9420()
{
    AbortStub("FUN_001d9420");
}

void FUN_001d94b0()
{
    AbortStub("FUN_001d94b0");
}

void FUN_001d94b8()
{
    AbortStub("FUN_001d94b8");
}

void FUN_001d94e8()
{
    AbortStub("FUN_001d94e8");
}

void FUN_001d9578()
{
    AbortStub("FUN_001d9578");
}

void FUN_001d95a8()
{
    AbortStub("FUN_001d95a8");
}

void FUN_001d9638()
{
    AbortStub("FUN_001d9638");
}

void FUN_001d9640()
{
    AbortStub("FUN_001d9640");
}

void FUN_001d9650()
{
    AbortStub("FUN_001d9650");
}

void FUN_001d9668()
{
    AbortStub("FUN_001d9668");
}

void FUN_001d96f8()
{
    AbortStub("FUN_001d96f8");
}

void FUN_001d9728()
{
    AbortStub("FUN_001d9728");
}

void FUN_001d97b8()
{
    AbortStub("FUN_001d97b8");
}

void FUN_001d97e8()
{
    AbortStub("FUN_001d97e8");
}

void FUN_001d9878()
{
    AbortStub("FUN_001d9878");
}

void FUN_001d98a8()
{
    AbortStub("FUN_001d98a8");
}

void FUN_001d9938()
{
    AbortStub("FUN_001d9938");
}

void FUN_001d9968()
{
    AbortStub("FUN_001d9968");
}

void FUN_001d99f8()
{
    AbortStub("FUN_001d99f8");
}

void FUN_001d9a00()
{
    AbortStub("FUN_001d9a00");
}

void FUN_001d9a10()
{
    AbortStub("FUN_001d9a10");
}

void FUN_001d9a28()
{
    AbortStub("FUN_001d9a28");
}

void FUN_001d9ab8()
{
    AbortStub("FUN_001d9ab8");
}

void FUN_001d9ac0()
{
    AbortStub("FUN_001d9ac0");
}

void FUN_001d9ac8()
{
    AbortStub("FUN_001d9ac8");
}

void FUN_001d9ad8()
{
    AbortStub("FUN_001d9ad8");
}

void FUN_001d9ae0()
{
    AbortStub("FUN_001d9ae0");
}

void FUN_001d9b70()
{
    AbortStub("FUN_001d9b70");
}

void FUN_001d9b78()
{
    AbortStub("FUN_001d9b78");
}

void FUN_001d9b80()
{
    AbortStub("FUN_001d9b80");
}

void FUN_001d9b88()
{
    AbortStub("FUN_001d9b88");
}

void FUN_001d9b98()
{
    AbortStub("FUN_001d9b98");
}

void FUN_001d9bb0()
{
    AbortStub("FUN_001d9bb0");
}

void FUN_001d9c40()
{
    AbortStub("FUN_001d9c40");
}

void FUN_001d9c48()
{
    AbortStub("FUN_001d9c48");
}

void FUN_001d9c50()
{
    AbortStub("FUN_001d9c50");
}

void FUN_001d9c58()
{
    AbortStub("FUN_001d9c58");
}

void FUN_001d9c68()
{
    AbortStub("FUN_001d9c68");
}

void FUN_001d9c80()
{
    AbortStub("FUN_001d9c80");
}

void FUN_001d9d10()
{
    AbortStub("FUN_001d9d10");
}

void FUN_001d9d18()
{
    AbortStub("FUN_001d9d18");
}

void FUN_001d9d20()
{
    AbortStub("FUN_001d9d20");
}

void FUN_001d9d28()
{
    AbortStub("FUN_001d9d28");
}

void FUN_001d9d40()
{
    AbortStub("FUN_001d9d40");
}

void FUN_001d9d58()
{
    AbortStub("FUN_001d9d58");
}

void FUN_001d9de8()
{
    AbortStub("FUN_001d9de8");
}

void FUN_001d9df0()
{
    AbortStub("FUN_001d9df0");
}

void FUN_001d9df8()
{
    AbortStub("FUN_001d9df8");
}

void FUN_001d9e00()
{
    AbortStub("FUN_001d9e00");
}

void FUN_001d9e18()
{
    AbortStub("FUN_001d9e18");
}

void FUN_001d9e20()
{
    AbortStub("FUN_001d9e20");
}

void FUN_001d9eb0()
{
    AbortStub("FUN_001d9eb0");
}

void FUN_001d9eb8()
{
    AbortStub("FUN_001d9eb8");
}

void FUN_001d9ec8()
{
    AbortStub("FUN_001d9ec8");
}

void FUN_001d9ed8()
{
    AbortStub("FUN_001d9ed8");
}

void FUN_001d9f68()
{
    AbortStub("FUN_001d9f68");
}

void FUN_001d9f70()
{
    AbortStub("FUN_001d9f70");
}

void FUN_001d9f80()
{
    AbortStub("FUN_001d9f80");
}

void FUN_001d9f90()
{
    AbortStub("FUN_001d9f90");
}

void FUN_001da020()
{
    AbortStub("FUN_001da020");
}

void FUN_001da028()
{
    AbortStub("FUN_001da028");
}

void FUN_001da038()
{
    AbortStub("FUN_001da038");
}

void FUN_001da050()
{
    AbortStub("FUN_001da050");
}

void FUN_001da0e0()
{
    AbortStub("FUN_001da0e0");
}

void FUN_001da0e8()
{
    AbortStub("FUN_001da0e8");
}

void FUN_001da0f0()
{
    AbortStub("FUN_001da0f0");
}

void FUN_001da0f8()
{
    AbortStub("FUN_001da0f8");
}

void FUN_001da108()
{
    AbortStub("FUN_001da108");
}

void FUN_001da128()
{
    AbortStub("FUN_001da128");
}

void FUN_001da130()
{
    AbortStub("FUN_001da130");
}

void FUN_001da140()
{
    AbortStub("FUN_001da140");
}

void FUN_001da158()
{
    AbortStub("FUN_001da158");
}

void FUN_001da1e8()
{
    AbortStub("FUN_001da1e8");
}

void FUN_001da1f0()
{
    AbortStub("FUN_001da1f0");
}

void FUN_001da1f8()
{
    AbortStub("FUN_001da1f8");
}

void FUN_001da210()
{
    AbortStub("FUN_001da210");
}

void FUN_001da220()
{
    AbortStub("FUN_001da220");
}

void FUN_001da2b0()
{
    AbortStub("FUN_001da2b0");
}

void FUN_001da2b8()
{
    AbortStub("FUN_001da2b8");
}

void FUN_001da2c8()
{
    AbortStub("FUN_001da2c8");
}

void FUN_001da2e8()
{
    AbortStub("FUN_001da2e8");
}

void FUN_001da2f0()
{
    AbortStub("FUN_001da2f0");
}

void FUN_001da300()
{
    AbortStub("FUN_001da300");
}

void FUN_001da318()
{
    AbortStub("FUN_001da318");
}

void FUN_001da3a8()
{
    AbortStub("FUN_001da3a8");
}

void FUN_001da3b0()
{
    AbortStub("FUN_001da3b0");
}

void FUN_001da3b8()
{
    AbortStub("FUN_001da3b8");
}

void FUN_001da3c0()
{
    AbortStub("FUN_001da3c0");
}

void FUN_001da3d0()
{
    AbortStub("FUN_001da3d0");
}

void FUN_001da3f0()
{
    AbortStub("FUN_001da3f0");
}

void FUN_001da3f8()
{
    AbortStub("FUN_001da3f8");
}

void FUN_001da408()
{
    AbortStub("FUN_001da408");
}

void FUN_001da420()
{
    AbortStub("FUN_001da420");
}

void FUN_001da4b0()
{
    AbortStub("FUN_001da4b0");
}

void FUN_001da4b8()
{
    AbortStub("FUN_001da4b8");
}

void FUN_001da4c8()
{
    AbortStub("FUN_001da4c8");
}

void FUN_001da4e0()
{
    AbortStub("FUN_001da4e0");
}

void FUN_001da570()
{
    AbortStub("FUN_001da570");
}

void FUN_001da578()
{
    AbortStub("FUN_001da578");
}

void FUN_001da588()
{
    AbortStub("FUN_001da588");
}

void FUN_001da5a0()
{
    AbortStub("FUN_001da5a0");
}

void FUN_001da630()
{
    AbortStub("FUN_001da630");
}

void FUN_001da638()
{
    AbortStub("FUN_001da638");
}

void FUN_001da648()
{
    AbortStub("FUN_001da648");
}

void FUN_001da660()
{
    AbortStub("FUN_001da660");
}

void FUN_001da6f0()
{
    AbortStub("FUN_001da6f0");
}

void FUN_001da6f8()
{
    AbortStub("FUN_001da6f8");
}

void FUN_001da708()
{
    AbortStub("FUN_001da708");
}

void FUN_001da9e0()
{
    AbortStub("FUN_001da9e0");
}

void FUN_001dab00()
{
    AbortStub("FUN_001dab00");
}

void FUN_001dab50()
{
    AbortStub("FUN_001dab50");
}

void FUN_001dabe0()
{
    AbortStub("FUN_001dabe0");
}

void FUN_001dabe8()
{
    AbortStub("FUN_001dabe8");
}

void FUN_001dabf8()
{
    AbortStub("FUN_001dabf8");
}

void FUN_001dac10()
{
    AbortStub("FUN_001dac10");
}

void FUN_001dac18()
{
    AbortStub("FUN_001dac18");
}

void FUN_001dac38()
{
    AbortStub("FUN_001dac38");
}

void FUN_001dad00()
{
    AbortStub("FUN_001dad00");
}

void FUN_001dade0()
{
    AbortStub("FUN_001dade0");
}

void FUN_001dae00()
{
    AbortStub("FUN_001dae00");
}

void FUN_001db010()
{
    AbortStub("FUN_001db010");
}

void FUN_001db098()
{
    AbortStub("FUN_001db098");
}

void FUN_001db110()
{
    AbortStub("FUN_001db110");
}

void FUN_001db238()
{
    AbortStub("FUN_001db238");
}

void FUN_001db268()
{
    AbortStub("FUN_001db268");
}

void FUN_001db2b8()
{
    AbortStub("FUN_001db2b8");
}

void FUN_001db2e8()
{
    AbortStub("FUN_001db2e8");
}

void FUN_001db460()
{
    AbortStub("FUN_001db460");
}

void FUN_001db4f8()
{
    AbortStub("FUN_001db4f8");
}

void FUN_001db518()
{
    AbortStub("FUN_001db518");
}

void FUN_001db5e0()
{
    AbortStub("FUN_001db5e0");
}

void FUN_001db600()
{
    AbortStub("FUN_001db600");
}

void FUN_001db6c8()
{
    AbortStub("FUN_001db6c8");
}

void FUN_001db6e8()
{
    AbortStub("FUN_001db6e8");
}

void FUN_001db740()
{
    AbortStub("FUN_001db740");
}

void FUN_001db748()
{
    AbortStub("FUN_001db748");
}

void FUN_001db768()
{
    AbortStub("FUN_001db768");
}

void FUN_001db830()
{
    AbortStub("FUN_001db830");
}

void FUN_001db910()
{
    AbortStub("FUN_001db910");
}

void FUN_001db930()
{
    AbortStub("FUN_001db930");
}

void FUN_001db9f8()
{
    AbortStub("FUN_001db9f8");
}

void FUN_001dba80()
{
    AbortStub("FUN_001dba80");
}

void FUN_001dbab0()
{
    AbortStub("FUN_001dbab0");
}

void FUN_001dbb48()
{
    AbortStub("FUN_001dbb48");
}

void FUN_001dbca8()
{
    AbortStub("FUN_001dbca8");
}

void FUN_001dbd30()
{
    AbortStub("FUN_001dbd30");
}

void FUN_001dbd60()
{
    AbortStub("FUN_001dbd60");
}

void FUN_001dbdc8()
{
    AbortStub("FUN_001dbdc8");
}

void FUN_001dbef8()
{
    AbortStub("FUN_001dbef8");
}

void FUN_001dbf18()
{
    AbortStub("FUN_001dbf18");
}

void FUN_001dbfe0()
{
    AbortStub("FUN_001dbfe0");
}

void FUN_001dc000()
{
    AbortStub("FUN_001dc000");
}

void FUN_001dc0c8()
{
    AbortStub("FUN_001dc0c8");
}

void FUN_001dc0e8()
{
    AbortStub("FUN_001dc0e8");
}

void FUN_001dc1b0()
{
    AbortStub("FUN_001dc1b0");
}

void FUN_001dc1d0()
{
    AbortStub("FUN_001dc1d0");
}

void FUN_001dc220()
{
    AbortStub("FUN_001dc220");
}

void FUN_001dc2b0()
{
    AbortStub("FUN_001dc2b0");
}

void FUN_001dc2b8()
{
    AbortStub("FUN_001dc2b8");
}

void FUN_001dc2c8()
{
    AbortStub("FUN_001dc2c8");
}

void FUN_001dc2e0()
{
    AbortStub("FUN_001dc2e0");
}

void FUN_001dc300()
{
    AbortStub("FUN_001dc300");
}

void FUN_001dc3c8()
{
    AbortStub("FUN_001dc3c8");
}

void FUN_001dc3e8()
{
    AbortStub("FUN_001dc3e8");
}

void FUN_001dc438()
{
    AbortStub("FUN_001dc438");
}

void FUN_001dc4c8()
{
    AbortStub("FUN_001dc4c8");
}

void FUN_001dc4f8()
{
    AbortStub("FUN_001dc4f8");
}

void FUN_001dc518()
{
    AbortStub("FUN_001dc518");
}

void FUN_001dc548()
{
    AbortStub("FUN_001dc548");
}

void FUN_001dc630()
{
    AbortStub("FUN_001dc630");
}

void FUN_001dc660()
{
    AbortStub("FUN_001dc660");
}

void FUN_001dc698()
{
    AbortStub("FUN_001dc698");
}

void FUN_001dc740()
{
    AbortStub("FUN_001dc740");
}

void FUN_001dc790()
{
    AbortStub("FUN_001dc790");
}

void FUN_001dc7c8()
{
    AbortStub("FUN_001dc7c8");
}

void FUN_001dc870()
{
    AbortStub("FUN_001dc870");
}

void FUN_001dc878()
{
    AbortStub("FUN_001dc878");
}

void FUN_001dca38()
{
    AbortStub("FUN_001dca38");
}

void FUN_001dca40()
{
    AbortStub("FUN_001dca40");
}

void FUN_001dcb00()
{
    AbortStub("FUN_001dcb00");
}

void FUN_001dcb08()
{
    AbortStub("FUN_001dcb08");
}

void FUN_001dcb38()
{
    AbortStub("FUN_001dcb38");
}

void FUN_001dcb58()
{
    AbortStub("FUN_001dcb58");
}

void FUN_001dcc08()
{
    AbortStub("FUN_001dcc08");
}

void FUN_001dcc10()
{
    AbortStub("FUN_001dcc10");
}

void FUN_001dcc18()
{
    AbortStub("FUN_001dcc18");
}

void FUN_001dcc20()
{
    AbortStub("FUN_001dcc20");
}

void FUN_001dcc50()
{
    AbortStub("FUN_001dcc50");
}

void FUN_001dcc70()
{
    AbortStub("FUN_001dcc70");
}

void FUN_001dcd20()
{
    AbortStub("FUN_001dcd20");
}

void FUN_001dcd28()
{
    AbortStub("FUN_001dcd28");
}

void FUN_001dcd30()
{
    AbortStub("FUN_001dcd30");
}

void FUN_001dcd38()
{
    AbortStub("FUN_001dcd38");
}

void FUN_001dcd68()
{
    AbortStub("FUN_001dcd68");
}

void FUN_001dcd88()
{
    AbortStub("FUN_001dcd88");
}

void FUN_001dce38()
{
    AbortStub("FUN_001dce38");
}

void FUN_001dce40()
{
    AbortStub("FUN_001dce40");
}

void FUN_001dce48()
{
    AbortStub("FUN_001dce48");
}

void FUN_001dce50()
{
    AbortStub("FUN_001dce50");
}

void FUN_001dce80()
{
    AbortStub("FUN_001dce80");
}

void FUN_001dcf30()
{
    AbortStub("FUN_001dcf30");
}

void FUN_001dcf38()
{
    AbortStub("FUN_001dcf38");
}

void FUN_001dcff8()
{
    AbortStub("FUN_001dcff8");
}

void FUN_001dd000()
{
    AbortStub("FUN_001dd000");
}

void FUN_001dd030()
{
    AbortStub("FUN_001dd030");
}

void FUN_001dd050()
{
    AbortStub("FUN_001dd050");
}

void FUN_001dd180()
{
    AbortStub("FUN_001dd180");
}

void FUN_001dd1a0()
{
    AbortStub("FUN_001dd1a0");
}

void FUN_001dd2b8()
{
    AbortStub("FUN_001dd2b8");
}

void FUN_001dd2e8()
{
    AbortStub("FUN_001dd2e8");
}

void FUN_001dd320()
{
    AbortStub("FUN_001dd320");
}

void FUN_001dd3c8()
{
    AbortStub("FUN_001dd3c8");
}

void FUN_001dd3e8()
{
    AbortStub("FUN_001dd3e8");
}

void FUN_001dd4d0()
{
    // InitVuProgramsModule: the programs' table's constructors do nothing, and an unread word is set
    extern s32 g_VuProgramsUnread RETAIL(D_0030A890);
    g_VuProgramsUnread = -1;
}

void GetShaderSettings()
{
    AbortStub("GetShaderSettings");
}

void UpdateShaderScrollAndColor()
{
    AbortStub("UpdateShaderScrollAndColor");
}

void SetShaderSettings()
{
    AbortStub("SetShaderSettings");
}

void SetShaderType()
{
    AbortStub("SetShaderType");
}

void ReadBasePrecompShader_()
{
    AbortStub("ReadBasePrecompShader_");
}

void ReadPrecompShaderType_0x0()
{
    AbortStub("ReadPrecompShaderType_0x0");
}

void ReadPrecompShaderType_0x10()
{
    AbortStub("ReadPrecompShaderType_0x10");
}

void ReadPrecompShaderType_0x11()
{
    AbortStub("ReadPrecompShaderType_0x11");
}

void ReadPrecompShaderType_0x12()
{
    AbortStub("ReadPrecompShaderType_0x12");
}

void ReadPrecompShaderType_0x13_()
{
    AbortStub("ReadPrecompShaderType_0x13_");
}

void ReadPrecompShaderType_0x14()
{
    AbortStub("ReadPrecompShaderType_0x14");
}

void ReadPrecompShaderType_0x15()
{
    AbortStub("ReadPrecompShaderType_0x15");
}

void ReadPrecompShaderType_0x16()
{
    AbortStub("ReadPrecompShaderType_0x16");
}

void ReadPrecompShaderType_0x17()
{
    AbortStub("ReadPrecompShaderType_0x17");
}

void ReadPrecompShaderType_0x19()
{
    AbortStub("ReadPrecompShaderType_0x19");
}

void ReadPrecompShaderType_0x1A()
{
    AbortStub("ReadPrecompShaderType_0x1A");
}

void ReadPrecompShaderType_0x1B()
{
    AbortStub("ReadPrecompShaderType_0x1B");
}

void ReadPrecompShaderType_0x1E()
{
    AbortStub("ReadPrecompShaderType_0x1E");
}

void ReadPrecompShaderType_0x1F()
{
    AbortStub("ReadPrecompShaderType_0x1F");
}

void ReadPrecompShaderType_0x1_()
{
    AbortStub("ReadPrecompShaderType_0x1_");
}

void ReadPrecompShaderType_0x20()
{
    AbortStub("ReadPrecompShaderType_0x20");
}

void ReadPrecompShaderType_0x2_()
{
    AbortStub("ReadPrecompShaderType_0x2_");
}

void ReadPrecompShaderType_0x4()
{
    AbortStub("ReadPrecompShaderType_0x4");
}

void ReadPrecompShaderType_0xA()
{
    AbortStub("ReadPrecompShaderType_0xA");
}

void ReadPrecompShaderType_0xB()
{
    AbortStub("ReadPrecompShaderType_0xB");
}

void ReadPrecompShaderType_0xC()
{
    AbortStub("ReadPrecompShaderType_0xC");
}

void ReadPrecompShaderType_0xD()
{
    AbortStub("ReadPrecompShaderType_0xD");
}

void ReadPrecompShaderType_0xE()
{
    AbortStub("ReadPrecompShaderType_0xE");
}

void ReadPrecompShaderType_0xF()
{
    AbortStub("ReadPrecompShaderType_0xF");
}

void func_001D94C0()
{
    AbortStub("func_001D94C0");
}

void func_001D94D0()
{
    AbortStub("func_001D94D0");
}

void func_001D9580()
{
    AbortStub("func_001D9580");
}

void func_001D9590()
{
    AbortStub("func_001D9590");
}

void func_001D9700()
{
    AbortStub("func_001D9700");
}

void func_001D9710()
{
    AbortStub("func_001D9710");
}

void func_001D97C0()
{
    AbortStub("func_001D97C0");
}

void func_001D97D0()
{
    AbortStub("func_001D97D0");
}

void func_001D9880()
{
    AbortStub("func_001D9880");
}

void func_001D9890()
{
    AbortStub("func_001D9890");
}

void func_001D9940()
{
    AbortStub("func_001D9940");
}

void func_001D9950()
{
    AbortStub("func_001D9950");
}

void func_001DB3C8()
{
    AbortStub("func_001DB3C8");
}

void func_001DC4D0()
{
    AbortStub("func_001DC4D0");
}

void func_001DC4E0()
{
    AbortStub("func_001DC4E0");
}

// The jump-table labels the retail data points at (GetButtonPressure's switch and the agents'
// unreachable switch): addresses only, never read by the compiled game
u32 case_0x8 = 0;
u32 case_0x9 = 0;
u32 case_0xa = 0;
u32 case_0xb = 0;
u32 case_0xc = 0;
u32 case_0xd = 0;
u32 case_0xe = 0;
u32 case_0xf = 0;
u32 case_0x12 = 0;
u32 case_0x15 = 0;
u32 case_0x16 = 0;
u32 case_0x17 = 0;
u32 func_001412EC = 0;
u32 func_00141304 = 0;
}
