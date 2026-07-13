// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// H.264 decoder via Android NDK MediaCodec — hardware decode on device.
// Used by the Android player (tools/android). Delivers NV12 (or BGRA/IYUV
// if the codec insists) for GPU-friendly compose; no FFmpeg.

#include <ge/VideoDecoder.h>

#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include <spdlog/spdlog.h>

#include <cstring>
#include <mutex>
#include <vector>

namespace ge {

// COLOR_FormatYUV420SemiPlanar (NV12): Y + interleaved UV.
static constexpr int32_t kColorFormatNV12 = 21;
// COLOR_FormatYUV420Planar (I420 / IYUV).
static constexpr int32_t kColorFormatI420 = 19;
// Some OEM BGRA fourccs show up as large buffers; detected by size.

static constexpr int64_t kInputTimeoutUs = 20'000;  // 20 ms
static constexpr int64_t kOutputTimeoutUs = 2'000;  // 2 ms drain

// ── minimal H.264 SPS dimension parse (Annex-B or raw NAL body) ─────────────

namespace {

struct BitReader {
    const uint8_t* data = nullptr;
    size_t size = 0;
    size_t bitPos = 0;

    explicit BitReader(const uint8_t* d, size_t n) : data(d), size(n) {}

    int readBit() {
        if (bitPos / 8 >= size) return 0;
        const int b = (data[bitPos / 8] >> (7 - (bitPos % 8))) & 1;
        ++bitPos;
        return b;
    }
    uint32_t readBits(int n) {
        uint32_t v = 0;
        for (int i = 0; i < n; ++i) v = (v << 1) | uint32_t(readBit());
        return v;
    }
    uint32_t readUE() {
        int zeros = 0;
        while (readBit() == 0 && zeros < 31) ++zeros;
        if (zeros == 0) return 0;
        return (1u << zeros) - 1u + readBits(zeros);
    }
    int32_t readSE() {
        uint32_t v = readUE();
        return (v & 1) ? int32_t((v + 1) / 2) : -int32_t(v / 2);
    }
};

// Strip 0x03 emulation-prevention bytes from RBSP.
std::vector<uint8_t> removeEmulationPrevention(const uint8_t* p, size_t n) {
    std::vector<uint8_t> out;
    out.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        if (i + 2 < n && p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 3) {
            out.push_back(0);
            out.push_back(0);
            i += 2;  // skip 0x03
            continue;
        }
        out.push_back(p[i]);
    }
    return out;
}

// Returns true and sets *outW/*outH on success (luma samples).
bool parseSpsDimensions(const uint8_t* nal, size_t nalSize, int* outW, int* outH) {
    // Accept Annex-B or raw; find NAL header.
    size_t off = 0;
    if (nalSize >= 4 && nal[0] == 0 && nal[1] == 0 && nal[2] == 0 && nal[3] == 1)
        off = 4;
    else if (nalSize >= 3 && nal[0] == 0 && nal[1] == 0 && nal[2] == 1)
        off = 3;
    if (off >= nalSize) return false;
    if ((nal[off] & 0x1F) != 7) return false;  // SPS
    ++off;  // skip NAL header
    if (off >= nalSize) return false;

    auto rbsp = removeEmulationPrevention(nal + off, nalSize - off);
    BitReader br(rbsp.data(), rbsp.size());

    const uint32_t profile_idc = br.readBits(8);
    br.readBits(8);  // constraint flags
    br.readBits(8);  // level_idc
    br.readUE();     // seq_parameter_set_id

    uint32_t chroma_format_idc = 1;
    if (profile_idc == 100 || profile_idc == 110 || profile_idc == 122 ||
        profile_idc == 244 || profile_idc == 44 || profile_idc == 83 ||
        profile_idc == 86 || profile_idc == 118 || profile_idc == 128 ||
        profile_idc == 138 || profile_idc == 139 || profile_idc == 134 ||
        profile_idc == 135) {
        chroma_format_idc = br.readUE();
        if (chroma_format_idc == 3) br.readBit();  // separate_colour_plane_flag
        br.readUE();  // bit_depth_luma_minus8
        br.readUE();  // bit_depth_chroma_minus8
        br.readBit(); // qpprime_y_zero_transform_bypass_flag
        if (br.readBit()) {  // seq_scaling_matrix_present_flag
            const int n = (chroma_format_idc != 3) ? 8 : 12;
            for (int i = 0; i < n; ++i) {
                if (!br.readBit()) continue;
                const int last = (i < 6) ? 16 : 64;
                int32_t nextScale = 8, lastScale = 8;
                for (int j = 0; j < last; ++j) {
                    if (nextScale != 0) {
                        const int32_t delta = br.readSE();
                        nextScale = (lastScale + delta + 256) % 256;
                    }
                    lastScale = (nextScale == 0) ? lastScale : nextScale;
                }
            }
        }
    }

    br.readUE();  // log2_max_frame_num_minus4
    const uint32_t pocType = br.readUE();
    if (pocType == 0) {
        br.readUE();
    } else if (pocType == 1) {
        br.readBit();
        br.readSE();
        br.readSE();
        const uint32_t n = br.readUE();
        for (uint32_t i = 0; i < n; ++i) br.readSE();
    }
    br.readUE();  // max_num_ref_frames
    br.readBit(); // gaps_in_frame_num_value_allowed_flag
    const uint32_t pic_width_in_mbs_minus1 = br.readUE();
    const uint32_t pic_height_in_map_units_minus1 = br.readUE();
    const int frame_mbs_only_flag = br.readBit();
    if (!frame_mbs_only_flag) br.readBit();  // mb_adaptive_frame_field_flag
    br.readBit();  // direct_8x8_inference_flag

    uint32_t cropLeft = 0, cropRight = 0, cropTop = 0, cropBottom = 0;
    if (br.readBit()) {  // frame_cropping_flag
        cropLeft = br.readUE();
        cropRight = br.readUE();
        cropTop = br.readUE();
        cropBottom = br.readUE();
    }

    int width = int(pic_width_in_mbs_minus1 + 1) * 16;
    int height = int(pic_height_in_map_units_minus1 + 1) * 16 *
                 (2 - frame_mbs_only_flag);
    // Crop units depend on chroma; for 4:2:0 (default) subWidth=2, subHeight=2.
    const int subW = (chroma_format_idc == 1 || chroma_format_idc == 2) ? 2 : 1;
    const int subH = (chroma_format_idc == 1) ? 2 : 1;
    width -= int(cropLeft + cropRight) * subW;
    height -= int(cropTop + cropBottom) * subH * (2 - frame_mbs_only_flag);
    if (width < 2 || height < 2) return false;
    *outW = width;
    *outH = height;
    return true;
}

}  // namespace

struct VideoDecoder::M {
    FrameCallback callback;

    AMediaCodec* codec = nullptr;
    int width = 0;
    int height = 0;
    int colorFormat = kColorFormatNV12;
    int yStride = 0;      // from AMEDIAFORMAT_KEY_STRIDE
    int sliceHeight = 0;  // from AMEDIAFORMAT_KEY_SLICE_HEIGHT

    std::vector<uint8_t> spsData;  // Annex-B with start code
    std::vector<uint8_t> ppsData;

    std::mutex drainMutex;

    ~M() {
        if (codec) {
            AMediaCodec_stop(codec);
            AMediaCodec_delete(codec);
            codec = nullptr;
        }
    }

    bool createCodec();
    bool queueAccessUnit(const uint8_t* data, size_t size);
    void drainOutput();
    void deliverFrame(uint8_t* buf, size_t bufSize, AMediaCodecBufferInfo& info);
    void applyOutputFormat(AMediaFormat* outFmt);
};

void VideoDecoder::M::applyOutputFormat(AMediaFormat* outFmt) {
    if (!outFmt) return;
    int32_t w = 0, h = 0, cf = 0, stride = 0, slice = 0;
    AMediaFormat_getInt32(outFmt, AMEDIAFORMAT_KEY_WIDTH, &w);
    AMediaFormat_getInt32(outFmt, AMEDIAFORMAT_KEY_HEIGHT, &h);
    AMediaFormat_getInt32(outFmt, AMEDIAFORMAT_KEY_COLOR_FORMAT, &cf);
    // String keys: AMEDIAFORMAT_KEY_SLICE_HEIGHT is API 28+; minSdk is 26.
    AMediaFormat_getInt32(outFmt, "stride", &stride);
    AMediaFormat_getInt32(outFmt, "slice-height", &slice);
    if (w > 0) width = w;
    if (h > 0) height = h;
    if (cf > 0) colorFormat = cf;
    yStride = stride > 0 ? stride : width;
    sliceHeight = slice > 0 ? slice : height;
    SPDLOG_INFO(
        "VideoDecoder: output format {}x{} cf={} stride={} sliceHeight={}",
        width, height, colorFormat, yStride, sliceHeight);
}

bool VideoDecoder::M::createCodec() {
    if (codec) {
        AMediaCodec_stop(codec);
        AMediaCodec_delete(codec);
        codec = nullptr;
    }

    codec = AMediaCodec_createDecoderByType("video/avc");
    if (!codec) {
        SPDLOG_ERROR("VideoDecoder: AMediaCodec_createDecoderByType failed");
        return false;
    }

    if (width <= 0) width = 512;
    if (height <= 0) height = 512;
    yStride = width;
    sliceHeight = height;

    AMediaFormat* fmt = AMediaFormat_new();
    AMediaFormat_setString(fmt, AMEDIAFORMAT_KEY_MIME, "video/avc");
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_WIDTH, width);
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_HEIGHT, height);
    AMediaFormat_setBuffer(fmt, "csd-0", spsData.data(), spsData.size());
    AMediaFormat_setBuffer(fmt, "csd-1", ppsData.data(), ppsData.size());
    // Byte-buffer output (not Surface) so we can blit into the mosaic.
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_COLOR_FORMAT, kColorFormatNV12);
    // Low-latency when available (API 30+); ignored on older devices.
    AMediaFormat_setInt32(fmt, "low-latency", 1);
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_MAX_INPUT_SIZE,
                          width * height);  // upper bound for tile AUs

    media_status_t status = AMediaCodec_configure(
        codec, fmt, /*surface=*/nullptr, /*crypto=*/nullptr, /*flags=*/0);
    AMediaFormat_delete(fmt);

    if (status != AMEDIA_OK) {
        SPDLOG_ERROR("VideoDecoder: AMediaCodec_configure failed: {}",
                     static_cast<int>(status));
        AMediaCodec_delete(codec);
        codec = nullptr;
        return false;
    }

    status = AMediaCodec_start(codec);
    if (status != AMEDIA_OK) {
        SPDLOG_ERROR("VideoDecoder: AMediaCodec_start failed: {}",
                     static_cast<int>(status));
        AMediaCodec_delete(codec);
        codec = nullptr;
        return false;
    }

    SPDLOG_INFO("VideoDecoder: MediaCodec HW decoder started {}x{} (NV12 request)",
                width, height);
    return true;
}

bool VideoDecoder::M::queueAccessUnit(const uint8_t* data, size_t size) {
    if (!codec || !data || size == 0) return false;

    // Try twice: drain between attempts if the codec is full (common with
    // several concurrent tile decoders).
    for (int attempt = 0; attempt < 2; ++attempt) {
        ssize_t idx = AMediaCodec_dequeueInputBuffer(codec, kInputTimeoutUs);
        if (idx < 0) {
            drainOutput();
            continue;
        }
        size_t bufSize = 0;
        uint8_t* buf = AMediaCodec_getInputBuffer(
            codec, static_cast<size_t>(idx), &bufSize);
        if (!buf || bufSize == 0) return false;
        if (size > bufSize) {
            SPDLOG_ERROR("VideoDecoder: AU {} > input buffer {}", size, bufSize);
            size = bufSize;
        }
        std::memcpy(buf, data, size);
        media_status_t st = AMediaCodec_queueInputBuffer(
            codec, static_cast<size_t>(idx), 0, size,
            /*presentationTimeUs=*/0, /*flags=*/0);
        if (st != AMEDIA_OK) {
            SPDLOG_ERROR("VideoDecoder: queueInputBuffer failed: {}",
                         static_cast<int>(st));
            return false;
        }
        return true;
    }
    static int dropLog = 0;
    if (dropLog++ < 8)
        SPDLOG_WARN("VideoDecoder: dropped AU — no input buffer (codec busy)");
    return false;
}

void VideoDecoder::M::deliverFrame(uint8_t* buf, size_t /*bufSize*/,
                                   AMediaCodecBufferInfo& info) {
    if (info.size <= 0 || !buf || width <= 0 || height <= 0) return;

    uint8_t* src = buf + info.offset;
    const size_t frameBytes = static_cast<size_t>(info.size);
    const int stride = yStride > 0 ? yStride : width;
    const int slice = sliceHeight > 0 ? sliceHeight : height;

    VideoFrame f;
    f.width = width;
    f.height = height;

    const size_t nv12Min =
        size_t(stride) * size_t(slice) + size_t(stride) * size_t(height) / 2;
    const size_t i420Min =
        size_t(stride) * size_t(slice) + 2 * (size_t(stride / 2) * size_t(height / 2));
    const size_t bgraMin = size_t(width) * size_t(height) * 4;

    // Prefer declared color format; fall back to size heuristics.
    if (colorFormat == kColorFormatI420 && frameBytes >= i420Min) {
        f.format = VideoFrame::Format::IYUV;
        f.planes[0] = src;
        f.planes[1] = src + size_t(stride) * size_t(slice);
        f.planes[2] = f.planes[1] + size_t(stride / 2) * size_t((slice + 1) / 2);
        f.strides[0] = stride;
        f.strides[1] = stride / 2;
        f.strides[2] = stride / 2;
    } else if (frameBytes >= bgraMin &&
               frameBytes >= size_t(width) * height * 4 &&
               colorFormat != kColorFormatNV12 &&
               colorFormat != kColorFormatI420) {
        f.format = VideoFrame::Format::BGRA;
        f.planes[0] = src;
        f.strides[0] = width * 4;
    } else if (frameBytes >= nv12Min || frameBytes >= size_t(width) * height * 3 / 2) {
        // NV12 (or close enough): Y plane uses stride×sliceHeight.
        f.format = VideoFrame::Format::NV12;
        f.planes[0] = src;
        f.planes[1] = src + size_t(stride) * size_t(slice);
        f.strides[0] = stride;
        f.strides[1] = stride;
    } else {
        static int warn = 0;
        if (warn++ < 5) {
            SPDLOG_WARN(
                "VideoDecoder: unexpected buffer size {} ({}x{} cf={} stride={})",
                frameBytes, width, height, colorFormat, stride);
        }
        return;
    }

    if (callback) callback(f);
}

void VideoDecoder::M::drainOutput() {
    if (!codec) return;
    std::lock_guard<std::mutex> lock(drainMutex);

    for (int n = 0; n < 8; ++n) {
        AMediaCodecBufferInfo info{};
        ssize_t idx =
            AMediaCodec_dequeueOutputBuffer(codec, &info, kOutputTimeoutUs);

        if (idx == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
            AMediaFormat* outFmt = AMediaCodec_getOutputFormat(codec);
            applyOutputFormat(outFmt);
            if (outFmt) AMediaFormat_delete(outFmt);
            continue;
        }
        if (idx == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) continue;
        if (idx == AMEDIACODEC_INFO_TRY_AGAIN_LATER || idx < 0) break;

        size_t bufSize = 0;
        uint8_t* out = AMediaCodec_getOutputBuffer(
            codec, static_cast<size_t>(idx), &bufSize);
        if (out && !(info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM)) {
            deliverFrame(out, bufSize, info);
        }
        AMediaCodec_releaseOutputBuffer(codec, static_cast<size_t>(idx),
                                        /*render=*/false);
        if (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) break;
    }
}

// ── public API ──────────────────────────────────────────────────────────────

VideoDecoder::VideoDecoder(FrameCallback onFrame)
    : m(std::make_unique<M>()) {
    m->callback = std::move(onFrame);
}

VideoDecoder::~VideoDecoder() = default;

void VideoDecoder::setParameterSets(const uint8_t* sps, size_t spsSize,
                                    const uint8_t* pps, size_t ppsSize) {
    static const uint8_t kStartCode[] = {0x00, 0x00, 0x00, 0x01};
    auto ensureStartCode = [](const uint8_t* data, size_t size) {
        std::vector<uint8_t> out;
        const bool has = size >= 4 && data[0] == 0 && data[1] == 0 &&
                         data[2] == 0 && data[3] == 1;
        if (!has) out.insert(out.end(), kStartCode, kStartCode + 4);
        out.insert(out.end(), data, data + size);
        return out;
    };

    m->spsData = ensureStartCode(sps, spsSize);
    m->ppsData = ensureStartCode(pps, ppsSize);

    int w = 0, h = 0;
    if (parseSpsDimensions(m->spsData.data(), m->spsData.size(), &w, &h)) {
        m->width = w;
        m->height = h;
    } else if (m->width == 0) {
        m->width = 512;
        m->height = 512;
    }

    SPDLOG_INFO("VideoDecoder: setParameterSets sps={} pps={} → {}x{}",
                m->spsData.size(), m->ppsData.size(), m->width, m->height);
    m->createCodec();
}

void VideoDecoder::decode(const uint8_t* nalData, size_t nalSize) {
    if (!m->codec || !nalData || nalSize == 0) return;
    m->drainOutput();
    m->queueAccessUnit(nalData, nalSize);
    m->drainOutput();
}

void VideoDecoder::flush() {
    if (!m->codec) return;
    m->drainOutput();
    media_status_t status = AMediaCodec_flush(m->codec);
    if (status != AMEDIA_OK) {
        SPDLOG_WARN("VideoDecoder: AMediaCodec_flush failed: {}",
                    static_cast<int>(status));
    }
}

}  // namespace ge
