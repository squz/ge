// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include <ge/CmdStream.h>

#include <lz4.h>

#include <cstring>
#include <vector>

namespace ge::cmdstream {
namespace {

// FNV-1a 128-bit (two independent 64-bit FNV streams with different offsets).
Hash fnv1a128(const void* data, size_t n) {
    const auto* p = static_cast<const uint8_t*>(data);
    uint64_t h0 = 0xcbf29ce484222325ull;
    uint64_t h1 = 0x100000001b3ull ^ 0x84222325cbf29ce4ull;
    constexpr uint64_t prime = 0x100000001b3ull;
    for (size_t i = 0; i < n; ++i) {
        h0 ^= p[i];
        h0 *= prime;
        h1 ^= p[i] ^ 0xa5u;
        h1 *= prime;
    }
    Hash out{};
    for (int i = 0; i < 8; ++i) {
        out[i] = static_cast<uint8_t>((h0 >> (i * 8)) & 0xff);
        out[8 + i] = static_cast<uint8_t>((h1 >> (i * 8)) & 0xff);
    }
    return out;
}

std::string keyOf(const Hash& h) {
    return std::string(reinterpret_cast<const char*>(h.data()), h.size());
}

} // namespace

Hash hashBytes(const void* data, size_t n) {
    return fnv1a128(data, n);
}

std::string hashHex(const Hash& h) {
    static const char* hex = "0123456789abcdef";
    std::string s;
    s.resize(32);
    for (int i = 0; i < 16; ++i) {
        s[i * 2] = hex[h[i] >> 4];
        s[i * 2 + 1] = hex[h[i] & 0xf];
    }
    return s;
}

// ── Cache ─────────────────────────────────────────────────────────

bool Cache::contains(const Hash& h) const {
    return map_.find(HashKey{h}) != map_.end();
}

const std::vector<uint8_t>* Cache::get(const Hash& h) const {
    auto it = map_.find(HashKey{h});
    if (it == map_.end()) return nullptr;
    return &it->second;
}

bool Cache::put(const Hash& h, std::vector<uint8_t> bytes) {
    HashKey k{h};
    auto it = map_.find(k);
    if (it != map_.end()) {
        totalBytes_ -= it->second.size();
        totalBytes_ += bytes.size();
        it->second = std::move(bytes);
        return false;
    }
    totalBytes_ += bytes.size();
    map_.emplace(k, std::move(bytes));
    return true;
}

bool Cache::put(const Hash& h, const void* data, size_t n) {
    std::vector<uint8_t> v(static_cast<const uint8_t*>(data),
                           static_cast<const uint8_t*>(data) + n);
    return put(h, std::move(v));
}

void Cache::clear() {
    map_.clear();
    totalBytes_ = 0;
}

// ── Writer ────────────────────────────────────────────────────────

Writer::Writer(Cache* serverCache) : cache_(serverCache) {
    buf_.reserve(4096);
}

void Writer::putU8(uint8_t v) { buf_.push_back(v); }
void Writer::putU16(uint16_t v) {
    buf_.push_back(static_cast<uint8_t>(v));
    buf_.push_back(static_cast<uint8_t>(v >> 8));
}
void Writer::putU32(uint32_t v) {
    buf_.push_back(static_cast<uint8_t>(v));
    buf_.push_back(static_cast<uint8_t>(v >> 8));
    buf_.push_back(static_cast<uint8_t>(v >> 16));
    buf_.push_back(static_cast<uint8_t>(v >> 24));
}
void Writer::putI32(int32_t v) { putU32(static_cast<uint32_t>(v)); }
void Writer::putHash(const Hash& h) {
    buf_.insert(buf_.end(), h.begin(), h.end());
}
void Writer::putBytes(const void* p, size_t n) {
    auto* b = static_cast<const uint8_t*>(p);
    buf_.insert(buf_.end(), b, b + n);
}
void Writer::putOp(Op op) {
    putU16(static_cast<uint16_t>(op));
    ++stats_.opCount;
}

void Writer::frameBegin(uint32_t seq, bool fullState) {
    putOp(Op::FrameBegin);
    putU32(seq);
    putU8(fullState ? 1 : 0);
}

void Writer::frameEnd() { putOp(Op::FrameEnd); }

Hash Writer::emitBlob(const void* data, size_t n) {
    Hash h = hashBytes(data, n);
    const std::string k = keyOf(h);
    const bool already =
        sentFull_.count(k) || (cache_ && cache_->contains(h));
    if (already) {
        putOp(Op::BlobRef);
        putHash(h);
        ++stats_.refBlobCount;
        return h;
    }
    putOp(Op::Blob);
    putHash(h);
    putU32(static_cast<uint32_t>(n));
    putBytes(data, n);
    sentFull_[k] = true;
    if (cache_) cache_->put(h, data, n);
    stats_.fullBlobBytes += n;
    ++stats_.fullBlobCount;
    return h;
}

void Writer::makeBuffer(uint32_t id, uint32_t size, uint32_t usage,
                        const void* data, size_t n) {
    Hash h = emitBlob(data, n);
    putOp(Op::MakeBuffer);
    putU32(id);
    putU32(size);
    putU32(usage);
    putHash(h);
}

void Writer::makeImage(uint32_t id, uint16_t w, uint16_t h, uint32_t pixelFormat,
                       const void* pixels, size_t n) {
    Hash content = emitBlob(pixels, n);
    putOp(Op::MakeImage);
    putU32(id);
    putU16(w);
    putU16(h);
    putU32(pixelFormat);
    putHash(content);
}

void Writer::updateBuffer(uint32_t id, const void* data, size_t n) {
    Hash h = emitBlob(data, n);
    putOp(Op::UpdateBuffer);
    putU32(id);
    putHash(h);
}

void Writer::updateImage(uint32_t id, const void* pixels, size_t n) {
    Hash h = emitBlob(pixels, n);
    putOp(Op::UpdateImage);
    putU32(id);
    putHash(h);
}

void Writer::destroyBuffer(uint32_t id) {
    putOp(Op::DestroyBuffer);
    putU32(id);
}

void Writer::destroyImage(uint32_t id) {
    putOp(Op::DestroyImage);
    putU32(id);
}

void Writer::beginPass(std::span<const float> clearRgba, bool clearDepth) {
    putOp(Op::BeginPass);
    const uint8_t n = static_cast<uint8_t>(clearRgba.size() / 4);
    putU8(n);
    for (float f : clearRgba) {
        uint32_t bits = 0;
        static_assert(sizeof(float) == 4);
        std::memcpy(&bits, &f, 4);
        putU32(bits);
    }
    putU8(clearDepth ? 1 : 0);
}

void Writer::applyPipeline(uint32_t pipelineId) {
    putOp(Op::ApplyPipeline);
    putU32(pipelineId);
}

void Writer::applyBindings(std::span<const uint32_t> vbufs, uint32_t ibuf,
                           std::span<const uint32_t> images) {
    putOp(Op::ApplyBindings);
    putU8(static_cast<uint8_t>(vbufs.size()));
    for (uint32_t id : vbufs) putU32(id);
    putU32(ibuf);
    putU8(static_cast<uint8_t>(images.size()));
    for (uint32_t id : images) putU32(id);
}

void Writer::applyUniforms(uint8_t slot, const void* data, size_t n) {
    Hash h = emitBlob(data, n);
    putOp(Op::ApplyUniforms);
    putU8(slot);
    putHash(h);
}

void Writer::draw(int32_t base, int32_t numElements, int32_t numInstances) {
    putOp(Op::Draw);
    putI32(base);
    putI32(numElements);
    putI32(numInstances);
}

void Writer::endPass() { putOp(Op::EndPass); }
void Writer::commit() { putOp(Op::Commit); }

void Writer::present(uint16_t w, uint16_t h, uint8_t format,
                     const void* pixels, size_t rawBytes) {
    // Prefer LZ4 when it beats raw (typical for flat/UI-ish game frames).
    uint8_t encoding = kPresentEncRaw;
    const void* blobPtr = pixels;
    size_t blobSize = rawBytes;
    std::vector<uint8_t> compressed;
    if (rawBytes > 0 && rawBytes < static_cast<size_t>(LZ4_MAX_INPUT_SIZE)) {
        const int bound = LZ4_compressBound(static_cast<int>(rawBytes));
        if (bound > 0) {
            compressed.resize(static_cast<size_t>(bound));
            const int n = LZ4_compress_default(
                static_cast<const char*>(pixels),
                reinterpret_cast<char*>(compressed.data()),
                static_cast<int>(rawBytes), bound);
            if (n > 0 && static_cast<size_t>(n) < rawBytes) {
                compressed.resize(static_cast<size_t>(n));
                encoding = kPresentEncLz4;
                blobPtr = compressed.data();
                blobSize = compressed.size();
            }
        }
    }
    Hash content = emitBlob(blobPtr, blobSize);
    putOp(Op::Present);
    putU16(w);
    putU16(h);
    putU8(format);
    putU8(encoding);
    putU32(static_cast<uint32_t>(rawBytes));
    putHash(content);
}

std::vector<uint8_t> Writer::take() {
    std::vector<uint8_t> out;
    out.swap(buf_);
    return out;
}

// ── Reader ────────────────────────────────────────────────────────

Reader::Reader(Cache* playerCache) : cache_(playerCache) {}

uint8_t Reader::Cursor::u8() {
    if (!ok || p >= end) {
        ok = false;
        return 0;
    }
    return *p++;
}
uint16_t Reader::Cursor::u16() {
    uint8_t a = u8(), b = u8();
    return static_cast<uint16_t>(a | (uint16_t(b) << 8));
}
uint32_t Reader::Cursor::u32() {
    uint8_t a = u8(), b = u8(), c = u8(), d = u8();
    return uint32_t(a) | (uint32_t(b) << 8) | (uint32_t(c) << 16) | (uint32_t(d) << 24);
}
int32_t Reader::Cursor::i32() { return static_cast<int32_t>(u32()); }
Hash Reader::Cursor::hash() {
    Hash h{};
    auto s = bytes(16);
    if (s.size() == 16) std::memcpy(h.data(), s.data(), 16);
    return h;
}
std::span<const uint8_t> Reader::Cursor::bytes(size_t n) {
    if (!ok || static_cast<size_t>(end - p) < n) {
        ok = false;
        return {};
    }
    auto s = std::span<const uint8_t>(p, n);
    p += n;
    return s;
}

bool Reader::decode(std::span<const uint8_t> payload,
                    bool (*visit)(Op op, Cursor& c, void* user),
                    void* user) {
    Cursor c{payload.data(), payload.data() + payload.size(), true};
    while (c.remain()) {
        uint16_t raw = c.u16();
        if (!c.ok) return false;
        auto op = static_cast<Op>(raw);
        ++stats_.opCount;

        if (op == Op::Blob) {
            Hash h = c.hash();
            uint32_t n = c.u32();
            auto body = c.bytes(n);
            if (!c.ok) return false;
            if (cache_) cache_->put(h, body.data(), body.size());
            stats_.fullBlobBytes += n;
            ++stats_.fullBlobCount;
            // Re-wrap as a mini cursor for the visitor (empty payload; blob
            // side-effect is the cache). Pass a cursor positioned after the op
            // header fields already consumed — visitor for Blob sees nothing.
            Cursor empty{c.p, c.p, true};
            if (visit && !visit(op, empty, user)) return false;
            continue;
        }
        if (op == Op::BlobRef) {
            Hash h = c.hash();
            if (!c.ok) return false;
            if (!cache_ || !cache_->contains(h)) {
                ++stats_.cacheMisses;
                return false;
            }
            ++stats_.refBlobCount;
            Cursor empty{c.p, c.p, true};
            if (visit && !visit(op, empty, user)) return false;
            continue;
        }
        if (op == Op::End) break;

        // Hand the rest of this op to the visitor: it must consume exactly
        // the op's fields. We don't know lengths for every op here, so the
        // visitor reads via Cursor until the op is done. For sequential
        // well-formed streams the visitor's reads advance c.
        if (visit && !visit(op, c, user)) return false;
        if (!c.ok) return false;
    }
    return c.ok || c.p == c.end;
}

// ── Synthetic scene ───────────────────────────────────────────────

namespace {

std::vector<uint8_t> solidRgba(int w, int h, uint8_t r, uint8_t g, uint8_t b) {
    std::vector<uint8_t> px(static_cast<size_t>(w) * h * 4);
    for (size_t i = 0; i < px.size(); i += 4) {
        px[i] = r;
        px[i + 1] = g;
        px[i + 2] = b;
        px[i + 3] = 255;
    }
    // Sprinkle noise so hashes differ between textures / churn frames.
    for (size_t i = 0; i < px.size(); i += 64)
        px[i] = static_cast<uint8_t>(px[i] ^ (i * 17u));
    return px;
}

} // namespace

SyntheticScene SyntheticScene::tiltbuggyLike() {
    SyntheticScene s;
    // ~512×512 RGBA dirt/asphalt stand-ins (~1 MB each) + modest mesh.
    s.texDirt = solidRgba(512, 512, 140, 100, 60);
    s.texAsphalt = solidRgba(512, 512, 50, 50, 55);
    s.meshVerts.resize(256 * 20); // 256 MeshVertex-ish
    s.meshIdx.resize(256 * 3 * sizeof(uint16_t));
    for (size_t i = 0; i < s.meshVerts.size(); ++i)
        s.meshVerts[i] = static_cast<uint8_t>(i * 3);
    for (size_t i = 0; i < s.meshIdx.size(); ++i)
        s.meshIdx[i] = static_cast<uint8_t>(i);
    return s;
}

void SyntheticScene::writeFrame(Writer& w, uint32_t seq, bool firstFrame,
                                bool forceTextureChurn) const {
    w.frameBegin(seq, firstFrame);
    if (firstFrame) {
        w.makeImage(1, 512, 512, /*RGBA8*/ 0x10, texDirt.data(), texDirt.size());
        w.makeImage(2, 512, 512, 0x10, texAsphalt.data(), texAsphalt.size());
        w.makeBuffer(1, static_cast<uint32_t>(meshVerts.size()), /*vertex*/ 1,
                     meshVerts.data(), meshVerts.size());
        w.makeBuffer(2, static_cast<uint32_t>(meshIdx.size()), /*index*/ 2,
                     meshIdx.data(), meshIdx.size());
    } else if (forceTextureChurn) {
        // Create-tax trial: re-upload a modified dirt texture.
        auto churn = texDirt;
        churn[0] ^= 0xff;
        w.updateImage(1, churn.data(), churn.size());
    }

    float clear[4] = {0.5f, 0.7f, 0.9f, 1.f};
    w.beginPass(clear, true);
    w.applyPipeline(/*sprite-like*/ 1);
    uint32_t vbufs[] = {1};
    uint32_t imgs[] = {1, 2};
    w.applyBindings(vbufs, 2, imgs);

    // Per-frame uniforms: MVP + a few floats (~128 B), changing every frame.
    alignas(16) uint8_t ubo[128]{};
    std::memcpy(ubo, &seq, sizeof(seq));
    float t = static_cast<float>(seq) * 0.016f;
    std::memcpy(ubo + 16, &t, sizeof(t));
    w.applyUniforms(0, ubo, sizeof(ubo));

    // ~dozen draws (terrain chunks + buggy + UI).
    for (int i = 0; i < 12; ++i)
        w.draw(i * 100, 100, 1);

    w.endPass();
    w.commit();
    w.frameEnd();
}

size_t H264Estimate::bytesForFrames(int n, int keyEvery) const {
    if (n <= 0) return 0;
    size_t total = 0;
    for (int i = 0; i < n; ++i) {
        if (i % keyEvery == 0)
            total += keyframeBytes;
        else
            total += pframeBytes;
    }
    return total;
}

H264Estimate estimateH264FullRes(int width, int height) {
    // Empirical-ish floor from the tiled H.264 path at full res on LAN/Wi-Fi
    // sessions: keyframes are large; P-frames smaller but still non-trivial.
    // Scale by megapixels from a 1920×1080 baseline of ~80 KB key / ~12 KB P
    // at CRF~36 (order-of-magnitude, not a codec model).
    const double mp = (double)width * (double)height / (1920.0 * 1080.0);
    H264Estimate e;
    e.keyframeBytes = static_cast<size_t>(80000 * mp);
    e.pframeBytes = static_cast<size_t>(12000 * mp);
    if (e.keyframeBytes < 20000) e.keyframeBytes = 20000;
    if (e.pframeBytes < 3000) e.pframeBytes = 3000;
    return e;
}

} // namespace ge::cmdstream
