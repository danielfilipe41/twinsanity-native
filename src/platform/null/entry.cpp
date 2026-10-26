// Entry: runs the game on the host with logging stubs behind it.
#include "common.h"

extern "C" int Main(u32 argc, char** argv);

int main(int argc, char** argv)
{
    return Main(static_cast<u32>(argc), argv);
}
