// Minimal VU1 disassembler for assets/vutext.textbin.bin (6702 instrs, 8B each:
// lower word at +0, upper word at +4). Dispatch transcribed from PCSX2's
// DebugTools/DisVUops.h + DisVUmicro.h (GPL-3.0+, PCSX2 Dev Team):
// lower index = code>>25, upper index = code&0x3F. Enough to find the
// skinning loop's ILW (joint-index loads) and MUL (weight) lanes.
// Build: g++ -m32 -o /tmp/vudis tools/vudis.cpp; run from repo root.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace
{
std::vector<uint8_t> ReadFile(const char* path)
{
    FILE* file = std::fopen(path, "rb");
    if (file == nullptr)
    {
        return {};
    }

    std::fseek(file, 0, SEEK_END);
    long size = std::ftell(file);
    std::fseek(file, 0, SEEK_SET);
    std::vector<uint8_t> bytes(size > 0 ? static_cast<size_t>(size) : 0);
    if (size > 0 && std::fread(bytes.data(), 1, static_cast<size_t>(size), file) != static_cast<size_t>(size))
    {
        std::fclose(file);
        return {};
    }

    std::fclose(file);
    return bytes;
}

uint32_t ReadU32(const std::vector<uint8_t>& bytes, size_t at)
{
    uint32_t word;
    std::memcpy(&word, bytes.data() + at, sizeof(word));
    return word;
}

uint32_t Ft(uint32_t code) { return (code >> 16) & 0x1F; }
uint32_t Fs(uint32_t code) { return (code >> 11) & 0x1F; }
uint32_t Fd(uint32_t code) { return (code >> 6) & 0x1F; }
int32_t Imm11(uint32_t code) { return (int16_t)(code & 0x400 ? 0xFC00 | (code & 0x3FF) : code & 0x3FF); }
int32_t Imm5(uint32_t code)
{
    return (int16_t)((code >> 6) & 0x10 ? 0xFFF0 | ((code >> 6) & 0xF) : (code >> 6) & 0xF);
}

std::string DisLower(uint32_t code)
{
    char out[128];
    uint32_t index = code >> 25;
    uint32_t it = Ft(code) & 15;
    uint32_t is = Fs(code) & 15;
    uint32_t id = Fd(code) & 15;
    switch (index)
    {
        case 0x00:
            std::snprintf(out, sizeof(out), "LQ vf%u,(vi%u+%d)", Ft(code), is, Imm11(code));
            return out;
        case 0x01:
            std::snprintf(out, sizeof(out), "SQ vf%u,(vi%u+%d)", Fs(code), it, Imm11(code));
            return out;
        case 0x04:
            std::snprintf(out, sizeof(out), "ILW.x vi%u,%d(vi%u)", it, Imm11(code), is);
            return out;
        case 0x05:
            std::snprintf(out, sizeof(out), "ISW.x vi%u,%d(vi%u)", it, Imm11(code), is);
            return out;
        case 0x08:
            std::snprintf(out, sizeof(out), "IADDIU vi%u,vi%u,%d", it, is,
                          (int16_t)(((code >> 10) & 0x7800) | (code & 0x7FF)));
            return out;
        case 0x09:
            std::snprintf(out, sizeof(out), "ISUBIU vi%u,vi%u,%d", it, is,
                          (int16_t)(((code >> 10) & 0x7800) | (code & 0x7FF)));
            return out;
        case 0x10:
            std::snprintf(out, sizeof(out), "FCEQ vi%u,0x%x", it, code & 0xFFFFFF);
            return out;
        case 0x11:
            std::snprintf(out, sizeof(out), "FCSET 0x%x", code & 0xFFFFFF);
            return out;
        case 0x12:
            std::snprintf(out, sizeof(out), "FCAND vi%u,0x%x", it, code & 0xFFF);
            return out;
        case 0x13:
            std::snprintf(out, sizeof(out), "FCOR vi%u,0x%x", it, code & 0xFFF);
            return out;
        case 0x14:
            std::snprintf(out, sizeof(out), "FSEQ vi%u,0x%x", it, code & 0xFFF);
            return out;
        case 0x15:
            std::snprintf(out, sizeof(out), "FSSET");
            return out;
        case 0x16:
            std::snprintf(out, sizeof(out), "FSAND vi%u,0x%x", it, code & 0xFFF);
            return out;
        case 0x17:
            std::snprintf(out, sizeof(out), "FSOR vi%u,0x%x", it, code & 0xFFF);
            return out;
        case 0x18:
            std::snprintf(out, sizeof(out), "FMEQ vi%u,vi%u", it, is);
            return out;
        case 0x1A:
            std::snprintf(out, sizeof(out), "FMAND vi%u,vi%u", it, is);
            return out;
        case 0x1B:
            std::snprintf(out, sizeof(out), "FMOR vi%u,vi%u", it, is);
            return out;
        case 0x1C:
            std::snprintf(out, sizeof(out), "FCGET vi%u", it);
            return out;
        case 0x20:
            std::snprintf(out, sizeof(out), "B %d", Imm11(code));
            return out;
        case 0x21:
            std::snprintf(out, sizeof(out), "BAL vi%u,%d", it, Imm11(code));
            return out;
        case 0x24:
            std::snprintf(out, sizeof(out), "JR (vi%u)", is);
            return out;
        case 0x25:
            std::snprintf(out, sizeof(out), "JALR (vi%u),vi%u", is, it);
            return out;
        case 0x40:
        {
            // LowerOP group: sub = code&0x3F picks the integer op or a T3 table
            uint32_t sub = code & 0x3F;
            if (sub == 0x30)
            {
                std::snprintf(out, sizeof(out), "IADD vi%u,vi%u,vi%u", id, is, it);
                return out;
            }
            if (sub == 0x31)
            {
                std::snprintf(out, sizeof(out), "ISUB vi%u,vi%u,vi%u", id, is, it);
                return out;
            }
            if (sub == 0x32)
            {
                std::snprintf(out, sizeof(out), "IADDI vi%u,vi%u,%d", it, is, Imm5(code));
                return out;
            }
            if (sub == 0x34)
            {
                std::snprintf(out, sizeof(out), "IAND vi%u,vi%u,vi%u", id, is, it);
                return out;
            }
            if (sub == 0x35)
            {
                std::snprintf(out, sizeof(out), "IOR vi%u,vi%u,vi%u", id, is, it);
                return out;
            }
            if (sub < 0x3C || sub > 0x3F)
            {
                std::snprintf(out, sizeof(out), "LOP3C.%02x", sub);
                return out;
            }
            const char* t300[32] = {"?", "?", "?", "?", "?", "?", "?", "?", "?", "?", "?", "?",
                                    "MOVE", "LQI", "DIV", "MTIR", "RNEXT", "?", "?", "?", "?", "?", "?",
                                    "?", "?", "MFP", "XTOP", "XGKICK", "ESADD", "EATANxy", "ESQRT", "ESIN"};
            const char* t301[32] = {"?", "?", "?", "?", "?", "?", "?", "?", "?", "?", "?", "?",
                                    "MR32", "SQI", "SQRT", "MFIR", "RGET", "?", "?", "?", "?", "?", "?",
                                    "?", "?", "?", "XITOP", "?", "ERSADD", "EATANxz", "ERSQRT", "EATAN"};
            const char* t310[32] = {"?", "?", "?", "?", "?", "?", "?", "?", "?", "?", "?", "?",
                                    "?", "LQD", "RSQRT", "ILWR", "RINIT", "?", "?", "?", "?", "?", "?",
                                    "?", "ELENG", "ESUM", "ERCPR", "EEXP", "?", "?", "?", "?"};
            const char* t311[32] = {"?", "?", "?", "?", "?", "?", "?", "?", "?", "?", "?", "?",
                                    "?", "SQD", "WAITQ", "ISWR", "RXOR", "?", "?", "?", "?", "?", "?",
                                    "?", "ERLENG", "?", "WAITP", "?", "?", "?", "?", "?"};
            const char** table = sub == 0x3C ? t300 : sub == 0x3D ? t301 : sub == 0x3E ? t310 : t311;
            std::snprintf(out, sizeof(out), "%s it%u,is%u,fd%u", table[Fd(code)], it, is, Fd(code));
            return out;
        }
        default:
            std::snprintf(out, sizeof(out), "LOP%02x", index);
            return out;
    }
}

std::string DisUpper(uint32_t code)
{
    char out[128];
    static const char* table[64] = {
        "ADDx", "ADDy", "ADDz", "ADDw", "SUBx", "SUBy", "SUBz", "SUBw", "MADDx", "MADDy", "MADDz",
        "MADDw", "MSUBx", "MSUBy", "MSUBz", "MSUBw", "MAXx", "MAXy", "MAXz", "MAXw", "MINIx", "MINIy",
        "MINIz", "MINIw", "MULx", "MULy", "MULz", "MULw", "MULq", "MAXi", "MULi", "MINIi", "ADDq",
        "MADDq", "ADDi", "MADDi", "SUBq", "MSUBq", "SUBi", "MSUBi", "ADD", "MADD", "MUL", "MAX", "SUB",
        "MSUB", "OPMSUB", "MINI", "?", "?", "?", "?", "?", "?", "?", "?", "?", "?", "?", "?"};
    uint32_t op = code & 0x3F;
    if (op < 0x3C)
    {
        std::snprintf(out, sizeof(out), "%s vf%u,vf%u,vf%u", table[op], Fd(code), Fs(code), Ft(code));
        return out;
    }

    static const char* fd00[32] = {"ADDAx", "SUBx", "MADDAx", "MSUBAx", "ITOF0", "FTOI0", "MULAx",
                                   "MULAq", "ADDAq", "SUBAq", "ADDA", "SUBA", "?", "?", "?", "?",
                                   "?", "?", "?", "?", "?", "?", "?", "?", "?", "?", "?", "?", "?", "?", "?", "?"};
    static const char* fd01[32] = {"ADDAy", "SUBy", "MADDAy", "MSUBAy", "ITOF4", "FTOI4", "MULAy",
                                   "ABS", "MADDAq", "MSUBAq", "MADDA", "MSUBA", "?", "?", "?", "?",
                                   "?", "?", "?", "?", "?", "?", "?", "?", "?", "?", "?", "?", "?", "?", "?", "?"};
    static const char* fd10[32] = {"ADDAz", "SUBz", "MADDAz", "MSUBAz", "ITOF12", "FTOI12", "MULAz",
                                   "MULAi", "ADDAi", "SUBAi", "MULA", "OPMULA", "?", "?", "?", "?",
                                   "?", "?", "?", "?", "?", "?", "?", "?", "?", "?", "?", "?", "?", "?", "?", "?"};
    static const char* fd11[32] = {"ADDAw", "SUBw", "MADDAw", "MSUBAw", "ITOF15", "FTOI15", "MULAw",
                                   "CLIP", "MADDAi", "MSUBAi", "?", "NOP", "?", "?", "?", "?",
                                   "?", "?", "?", "?", "?", "?", "?", "?", "?", "?", "?", "?", "?", "?", "?", "?"};
    const char* sub = op == 0x3C ? fd00[Fd(code)] : op == 0x3D ? fd01[Fd(code)] : op == 0x3E ? fd10[Fd(code)] : fd11[Fd(code)];
    if (sub[0] == '?' || sub[0] == 'N')
    {
        std::snprintf(out, sizeof(out), "%s", sub);
        return out;
    }

    std::snprintf(out, sizeof(out), "%s vf%u,vf%u,vf%u", sub, Fd(code), Fs(code), Ft(code));
    return out;
}
}

int main(int argc, char** argv)
{
    const char* path = argc > 1 ? argv[1] : "assets/vutext.textbin.bin";
    std::vector<uint8_t> bytes = ReadFile(path);
    if (bytes.size() % 8 != 0 || bytes.empty())
    {
        std::fprintf(stderr, "cannot read %s\n", path);
        return 1;
    }

    size_t count = bytes.size() / 8;
    bool brief = argc > 2 && std::strcmp(argv[2], "--brief") == 0;
    for (size_t index = 0; index < count; index++)
    {
        uint32_t lower = ReadU32(bytes, index * 8);
        uint32_t upper = ReadU32(bytes, index * 8 + 4);
        bool eBit = (upper & 0x40000000) != 0;
        std::string lo = DisLower(lower);
        std::string up = DisUpper(upper);
        bool interesting = !brief;
        if (brief)
        {
            interesting = lo.rfind("ILW", 0) == 0 || lo.rfind("LQ", 0) == 0 || lo.rfind("MOVE", 0) == 0 ||
                          lo.rfind("MTIR", 0) == 0 || lo.rfind("MFIR", 0) == 0 || up.rfind("MUL", 0) == 0 ||
                          up.rfind("MADD", 0) == 0 || up.rfind("ADD", 0) == 0 || eBit;
        }

        if (interesting)
        {
            std::printf("%5zu: L=%08x [%-22s] U=%08x [%-24s]%s\n", index, lower, lo.c_str(), upper,
                        up.c_str(), eBit ? " E" : "");
        }
    }

    return 0;
}
