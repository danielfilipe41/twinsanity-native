// Software MPEG-2 video core: replaces the IPU's VDEC, BDEC and CSC with portable C++. The surrounding control
// follows retail's shape (PES demux, sequence/GOP/picture/slice headers, picture flow and reference bookkeeping) but
// works on linear Y/Cb/Cr planes and uses the scalar predictors in predict.h.
//
// Scope: MPEG-2 4:2:0 frame pictures with I/P/B coding, the format of the retail movies. Scalable extras retail
// never emits (4:2:2, 4:4:4, field D pictures) are rejected.
#pragma once

#include "common.h"

#include <functional>
#include <vector>

namespace HostMovie
{
// A big-endian bit reader over an elementary stream buffer (lifts the
// IPU's TOP reads in mpegdecode.cpp: the show/get/skip shape, over bytes
// instead of the TOP register). error set when a read runs past the end or
// meets bad bits, so the caller can tell a short buffer from bad data.
struct BitReader
{
    const u8* data = nullptr;
    u32 size = 0; // bytes
    u32 byte = 0;
    u32 bit = 0; // 0-7 within byte, 0 = MSB
    bool error = false;
    bool hitEnd = false;

    u32 ShowBits(u32 n);
    void SkipBits(u32 n);
    u32 GetBits(u32 n);
    u32 GetBit();
    void Align();
    bool AtEnd() const;
    // Whether the next bits are a 23-zero-bit start-code prefix
    bool NextIsStartCode();
    // Consumes an aligned 0x000001 xx code, returning its last byte
    u32 ReadStartCode();
};

// A decoded frame in planar 8-bit YUV (4:2:0), rows left to right top to
// bottom (retail's sceMpegGetPictureRAW8 CopyMacroblocks path, without the CSC).
struct YuvFrame
{
    u32 width = 0;
    u32 height = 0;
    // Y width*height bytes, then Cb and Cr of (width/2)*(height/2) each
    ::std::vector<u8> pixels;
};

// A PES video packet's payload, as the demuxer hands it on (StoreVideo's
// shape: header and data stay separate)
struct VideoPacket
{
    const u8* data;
    u32 length;
    s64 pts;
    s64 dts;
};

// Program-stream demux: walks packs and PES packets, calling video() for
// the video ES (MpegVideoStream ch 0) and audio() for the selected PCM
// sub-stream. Lifts mpegdemux.cpp's parsing (pack headers, PES optional
// fields, private-stream-1 sub-stream ids) over a flat buffer instead of the
// PssReader ring; the sub-stream ids come from the retail table's values
// (asm/data/data.data.s D_002E8198): video 0xE0, PCM the 4 bytes
// FF A0 00 channel (MatchSubStream: the channel ORed into the last byte, one
// stream per language), DATA the 4 bytes FF 00 00 00.
class Demux
{
public:
    Demux(const u8* data, u32 size);
    // Which PCM sub-stream's packets reach audio() (the game's audioChannel:
    // 0 when one language, the language otherwise). Defaults to 0.
    void SelectAudioChannel(u32 channel);
    // Demultiplexes the next pack's packets. Returns false at the end or
    // on a malformed stream.
    bool Next(const ::std::function<void(const VideoPacket&)>& video,
              const ::std::function<void(const u8*, u32)>& audio);
    bool Ended() const { return m_position >= m_size; }

private:
    const u8* m_data;
    u32 m_size;
    u32 m_position;
    u32 m_audioChannel = 0;
};

// The video decoder: fed ES bytes, emits YUV frames in display order.
class Decoder
{
public:
    Decoder();
    // Feeds ES bytes (appended to the pending stream)
    void Feed(const u8* data, u32 length);
    // Decodes the next display-order frame. Returns 1 with frame filled,
    // 0 when more bytes are needed, -1 at the sequence end or on error.
    s32 NextFrame(YuvFrame* frame);
    u32 Width() const { return m_width; }
    u32 Height() const { return m_height; }
    u32 Decoded() const { return m_decoded; }
    // The sequence header's frame_rate_code (6.2.2.1): 1 = 23.976, 2 = 24,
    // 3 = 25, 4 = 29.97, 5 = 30, 6 = 50, 7 = 59.94, 8 = 60. 0 until a
    // sequence header parses (the player paces presentation with it).
    u32 FrameRateCode() const { return m_frameRateCode; }

private:
    // A picture's header values (6.2.3 and the coding extension): the
    // switches the slice syntax reads
    struct PictureInfo
    {
        u32 pictureType = 0; // 1 I, 2 P, 3 B
        s32 fCode[2][2] = {};
        bool fullPelForward = false;
        bool fullPelBackward = false;
        u32 intraDcPrecision = 0;
        bool framePredFrameDct = true;
        bool concealmentVectors = false;
        bool qScaleType = false;
        bool intraVlcFormat = false;
        bool alternateScan = false;
        bool topFieldFirst = false;
        bool progressiveFrame = false;
        bool repeatFirstField = false;
    };
    // An undecoded picture: its header values and its slices' bytes back
    // to back, in coded order (NextFrame reorders to display order).
    struct QueuedPicture
    {
        PictureInfo info;
        ::std::vector<u8> bytes;
    };
    ::std::vector<u8> m_stream;
    u32 m_scan = 0; // scanned bytes of m_stream
    u32 m_width = 0; // display size (sequence display extension or coded)
    u32 m_height = 0;
    u32 m_decoded = 0;
    u32 m_frameRateCode = 0;
    // Reference planes (linear Y/Cb/Cr): past, future, current/B
    ::std::vector<u8> m_past;
    ::std::vector<u8> m_future;
    ::std::vector<u8> m_current;
    ::std::vector<s16> m_blocks;
    ::std::vector<s16> m_idct;
    ::std::vector<s16> m_prediction;
    bool m_havePast = false;
    bool m_haveFuture = false;
    // Sequence state (6.2.2): coded size, macroblock grid, quantiser matrices
    u32 m_codedWidth = 0;
    u32 m_codedHeight = 0;
    u32 m_mbWidth = 0;
    u32 m_mbHeight = 0;
    u8 m_intraMatrix[64];
    u8 m_nonIntraMatrix[64];
    bool m_progressiveSequence = true;
    // The last anchor decoded, held back while earlier B pictures display
    YuvFrame m_heldAnchor;
    bool m_haveAnchor = false;
    bool m_sequenceEnded = false;
    // Scan state: which extensions the next chunk belongs to (0 top, 1
    // sequence, 2 picture), the picture being built and queued pictures
    u32 m_extContext = 0;
    QueuedPicture m_building;
    ::std::vector<u8> m_buildingBytes;
    ::std::vector<QueuedPicture> m_queued;
    u32 m_anchorsDecoded = 0;
    bool m_scanError = false;
    // Dual-prime differentials of the picture being decoded (7.6.3.6)
    s32 m_dmVector[2] = {};

    // Sequence/GOP/picture/slice/macroblock flow (mpegheaders.cpp and
    // mpegdecode.cpp over linear planes)
    void ScanStream();
    void FlushBuilding();
    bool ParseSequence(BitReader* reader);
    bool ParseExtension(BitReader* reader);
    bool ParsePictureHeader(BitReader* reader, PictureInfo* info);
    bool DecodeAnchor(QueuedPicture* picture);
    bool DecodeBPicture(QueuedPicture* picture, YuvFrame* frame);
    bool DecodeSlices(BitReader* reader, const PictureInfo* info, u8* target);
    bool DecodeMacroblock(BitReader* reader, const PictureInfo* info, u32 address, s32* quantScale,
                          s32 predictors[2][2][2], s32 dcPredictors[3], s32 fieldSelect[2][2], u32* motionType,
                          u32* dctType, u32* mbType);
    void DecodeVectors(BitReader* reader, s32 predictors[2][2][2], s32* differential, s32 fieldSelect[2][2],
                       u32 direction, u32 count, u32 fieldFormat, const s32* fCode, bool dualPrime, bool scale,
                       bool fullPel);
    void DecodeVector(BitReader* reader, s32* predictor, s32* differential, s32 fCodeH, s32 fCodeV, bool dualPrime,
                      bool scale, bool fullPel);
    void DecodeComponent(s32* predictor, u32 component, s32 residualSize, s32 code, s32 residual, bool fullPel);
    bool DecodeBlock(BitReader* reader, const PictureInfo* info, s16* coefficients, bool intra, s32* dcPredictor,
                     u32 component, s32 quantScale);
    void FormPrediction(const PictureInfo* info, u32 address, u32 mbType, u32 motionType, s32 predictors[2][2][2],
                        s32 fieldSelect[2][2], s32 differential[2]);
    void PredictDirection(u32 mbX, u32 mbY, const u8* ref, u32 motionType, const s32* first, const s32* second,
                         const s32* fieldSelect, const s32* differential, bool average);
    void PredictField(const u8* ref, u32 width, u32 height, u32 mbX, u32 mbY, u32 srcField, u32 dstField, s32 vectorX,
                      s32 vectorY, s16* out, bool average);
    void PredictPlane(const u8* plane, u32 stride, u32 planeWidth, u32 planeHeight, u32 x0, u32 y0, s32 vectorX,
                      s32 vectorY, u32 width, u32 height, s16* out, u32 outStride, bool average);
    void StoreMacroblock(u8* target, u32 address, u32 mbType, u32 dctType);
    void CropFrame(const ::std::vector<u8>& planes, YuvFrame* frame);
    // The picture being predicted (P reads past implicitly, like retail)
    u32 m_pictureType = 0;
    bool m_topFieldFirst = false;
};
}
