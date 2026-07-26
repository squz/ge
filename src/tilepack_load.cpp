// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// 🎯T168.2 Runtime tile-pyramid loader. See ge/TilePack.h for the threading
// contract: open()/pump() are render-thread; a background reader thread owns
// file I/O and hands decoded blobs to pump() over a bounded queue.

#include <ge/TilePack.h>

#include <ge/FileIO.h>
#include <ge/Tweak.h>
#include <ge/texture.h>

#include "sokol_gfx.h"
#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <istream>
#include <mutex>
#include <thread>
#include <vector>

namespace ge {

namespace {

// Sanity caps on untrusted header fields — a corrupt/truncated pack must be
// rejected, never crash or allocate unbounded memory.
constexpr uint16_t kMaxPlaneCount = 8;
constexpr uint16_t kMaxLevelCount = 12;
constexpr size_t   kQueueCapacity = 48;

tweak::Tweak<int> tweakUploadsPerFrame{"tilepack.uploads_per_frame", 8};

sg_pixel_format formatForPlaneEncoding(uint16_t enc) {
    switch (static_cast<GeTilePlaneEncoding>(enc)) {
    case GeTilePlaneEncoding::Astc4x4: return SG_PIXELFORMAT_ASTC_4x4_RGBA;
    case GeTilePlaneEncoding::R8:      return SG_PIXELFORMAT_R8;
    case GeTilePlaneEncoding::EacR11:  return SG_PIXELFORMAT_EAC_R11;
    case GeTilePlaneEncoding::Rg8:     return SG_PIXELFORMAT_RG8;
    }
    return SG_PIXELFORMAT_NONE;
}

// One decoded tile blob, read off disk by the reader thread and handed to
// pump() for GPU upload. Mips are kept as separate buffers (mip 0 first) so
// they can be handed straight to sg_image_desc.data.mip_levels.
struct HeapTile {
    int      plane = 0;
    uint8_t  face   = 0;
    uint8_t  level  = 0;
    uint16_t tx = 0, ty = 0;
    std::vector<std::vector<uint8_t>> mips;
};

// Bounded single-producer/single-consumer queue: the reader thread pushes
// (blocking when full), pump() drains non-blockingly. shutdown() wakes both
// sides so the destructor can join the reader without deadlock.
class TileQueue {
public:
    explicit TileQueue(size_t cap) : cap_(cap) {}

    // Returns false if shutdown happened before/while waiting for room.
    bool push(HeapTile&& t) {
        std::unique_lock<std::mutex> lk(mu_);
        cvNotFull_.wait(lk, [&] { return q_.size() < cap_ || shutdown_; });
        if (shutdown_) return false;
        q_.push_back(std::move(t));
        lk.unlock();
        cvNotEmpty_.notify_one();
        return true;
    }

    bool tryPop(HeapTile& out) {
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (q_.empty()) return false;
            out = std::move(q_.front());
            q_.pop_front();
        }
        cvNotFull_.notify_one();
        return true;
    }

    void shutdown() {
        {
            std::lock_guard<std::mutex> lk(mu_);
            shutdown_ = true;
        }
        cvNotFull_.notify_all();
        cvNotEmpty_.notify_all();
    }

private:
    std::mutex              mu_;
    std::condition_variable cvNotFull_;
    std::condition_variable cvNotEmpty_;
    std::deque<HeapTile>    q_;
    size_t                  cap_;
    bool                    shutdown_ = false;
};

} // namespace

// ── M (pImpl) ───────────────────────────────────────────────────────────────

struct TilePyramid::M {
    struct PlaneRuntime {
        GeTilePlaneDesc          desc{};
        std::vector<GeTileEntry> entries;      // flat, geTileEntryIndex-ordered
        std::vector<GpuTexture>  textures;      // same indexing; null until uploaded
        int                      levelCap = 0;  // deepest level this run loads
    };

    std::string               path;
    TilePyramidOptions        opts;
    std::vector<PlaneRuntime> planes;

    uint64_t bytesTotal_ = 0;
    int      tilesTotal_ = 0;
    int      level0Total_ = 0;

    std::atomic<uint64_t> bytesResident_{0};
    std::atomic<int>      tilesLoaded_{0};
    std::atomic<int>      level0Loaded_{0};

    TileQueue         queue{kQueueCapacity};
    std::atomic<bool> shutdownRequested{false};
    std::thread       readerThread;

    ~M() {
        // Unblock the reader whether it's mid-push or mid-read, then join.
        shutdownRequested.store(true, std::memory_order_relaxed);
        queue.shutdown();
        if (readerThread.joinable()) readerThread.join();
        // planes' GpuTexture vectors destroy themselves; GpuTexture::destroy()
        // is a no-op without a live sokol context, so this is safe in tests.
    }

    void readerLoop() {
        auto stream = ge::openFile(path, /*binary=*/true);
        if (!stream || !*stream) {
            SPDLOG_ERROR("TilePyramid: reader thread failed to reopen '{}'", path);
            return;
        }
        for (uint16_t level = 0; level < kMaxLevelCount; ++level) {
            for (size_t p = 0; p < planes.size(); ++p) {
                PlaneRuntime& pr = planes[p];
                if (level >= pr.desc.levelCount) continue;
                if (int(level) > pr.levelCap) continue;
                readLevelForPlane(*stream, int(p), pr, level);
                if (shutdownRequested.load(std::memory_order_relaxed)) return;
            }
        }
    }

    void readLevelForPlane(std::istream& stream, int planeIdx, PlaneRuntime& pr,
                           uint16_t level) {
        const uint16_t side     = uint16_t(1u << level);
        const uint16_t mipCount = pr.desc.mipCount;
        for (uint8_t face = 0; face < uint8_t(kCubeFaceCount); ++face) {
            for (uint16_t ty = 0; ty < side; ++ty) {
                for (uint16_t tx = 0; tx < side; ++tx) {
                    if (shutdownRequested.load(std::memory_order_relaxed)) return;
                    const uint32_t idx = geTileEntryIndex(level, face, tx, ty);
                    const GeTileEntry& e = pr.entries[idx];
                    if (e.offset == 0) continue; // tile absent from this plane

                    HeapTile t;
                    t.plane = planeIdx;
                    t.face  = face;
                    t.level = uint8_t(level);
                    t.tx = tx;
                    t.ty = ty;

                    stream.seekg(std::streamoff(e.offset), std::ios::beg);
                    if (!stream) {
                        SPDLOG_ERROR("TilePyramid: seek to {} failed", e.offset);
                        stream.clear();
                        continue;
                    }
                    std::vector<uint32_t> mipSizes(mipCount);
                    stream.read(reinterpret_cast<char*>(mipSizes.data()),
                               std::streamsize(mipCount * sizeof(uint32_t)));
                    if (!stream) {
                        SPDLOG_ERROR("TilePyramid: mip-size read failed at offset {}",
                                    e.offset);
                        stream.clear();
                        continue;
                    }
                    t.mips.resize(mipCount);
                    bool ok = true;
                    for (uint16_t mi = 0; mi < mipCount; ++mi) {
                        t.mips[mi].resize(mipSizes[mi]);
                        if (mipSizes[mi] == 0) continue;
                        stream.read(reinterpret_cast<char*>(t.mips[mi].data()),
                                   std::streamsize(mipSizes[mi]));
                        if (!stream) { ok = false; break; }
                    }
                    if (!ok) {
                        SPDLOG_ERROR("TilePyramid: blob read failed (plane {} level {} "
                                    "face {} tx {} ty {})",
                                    planeIdx, level, face, tx, ty);
                        stream.clear();
                        continue;
                    }
                    if (!queue.push(std::move(t))) return; // shutting down
                }
            }
        }
    }

    void uploadTile(HeapTile& t) {
        PlaneRuntime& pr = planes[t.plane];
        const int stored = int(pr.desc.tileSize) + 2 * int(pr.desc.gutter);
        const sg_pixel_format fmt = formatForPlaneEncoding(pr.desc.encoding);
        if (fmt == SG_PIXELFORMAT_NONE || !sg_query_pixelformat(fmt).sample) {
            SPDLOG_ERROR("TilePyramid: unsupported/non-sampleable format for plane {}",
                        t.plane);
            return; // tile stays permanently non-resident; resolve() keeps
                     // serving its nearest resident ancestor.
        }

        char label[80];
        std::snprintf(label, sizeof(label), "getp.p%d.f%u.l%u.%u.%u", t.plane,
                     t.face, t.level, t.tx, t.ty);

        sg_image_desc d{};
        d.width = stored;
        d.height = stored;
        d.num_mipmaps = int(t.mips.size());
        d.pixel_format = fmt;
        d.label = label;
        for (size_t i = 0; i < t.mips.size(); ++i)
            d.data.mip_levels[i] = sg_range{t.mips[i].data(), t.mips[i].size()};

        const sg_image img = sg_make_image(&d);
        if (img.id == SG_INVALID_ID) {
            SPDLOG_ERROR("TilePyramid: sg_make_image failed for {}", label);
            return; // same fallback as above: permanently non-resident, logged.
        }
        sg_view_desc vd{};
        vd.texture.image = img;
        vd.label = label;
        const sg_view view = sg_make_view(&vd);
        if (view.id == SG_INVALID_ID) {
            SPDLOG_ERROR("TilePyramid: sg_make_view failed for {}", label);
            sg_destroy_image(img);
            return;
        }

        GpuTexture gt;
        gt.image = img;
        gt.view = view;
        gt.width = stored;
        gt.height = stored;
        gt.numMips = int(t.mips.size());
        gt.type = SG_IMAGETYPE_2D;
        gt.format = fmt;

        uint64_t bytes = 0;
        for (const auto& mp : t.mips) bytes += mp.size();

        const uint32_t idx = geTileEntryIndex(t.level, t.face, t.tx, t.ty);
        pr.textures[idx] = std::move(gt);
        bytesResident_.fetch_add(bytes, std::memory_order_relaxed);
        tilesLoaded_.fetch_add(1, std::memory_order_relaxed);
        if (t.level == 0) level0Loaded_.fetch_add(1, std::memory_order_relaxed);
    }
};

// ── TilePyramid ─────────────────────────────────────────────────────────────

TilePyramid::TilePyramid() = default;
TilePyramid::~TilePyramid() = default;
TilePyramid::TilePyramid(TilePyramid&&) noexcept = default;
TilePyramid& TilePyramid::operator=(TilePyramid&&) noexcept = default;

bool TilePyramid::isNull() const { return !m; }

TilePyramid TilePyramid::open(const std::string& path, const TilePyramidOptions& opts) {
    TilePyramid out;

    auto stream = ge::openFile(path, /*binary=*/true);
    if (!stream || !*stream) {
        SPDLOG_ERROR("TilePyramid::open: cannot open '{}'", path);
        return out;
    }

    stream->seekg(0, std::ios::end);
    const std::streamoff endPos = stream->tellg();
    if (!*stream || endPos < 0) {
        SPDLOG_ERROR("TilePyramid::open: cannot size '{}'", path);
        return out;
    }
    const uint64_t fileSize = uint64_t(endPos);
    stream->seekg(0, std::ios::beg);

    GeTilePackHeader hdr{};
    if (fileSize < sizeof(hdr)) {
        SPDLOG_ERROR("TilePyramid::open: '{}' truncated header ({} bytes)", path,
                    fileSize);
        return out;
    }
    stream->read(reinterpret_cast<char*>(&hdr), sizeof(hdr));
    if (!*stream) {
        SPDLOG_ERROR("TilePyramid::open: '{}' header read failed", path);
        return out;
    }
    if (std::memcmp(hdr.magic, kGeTilePackMagic, 4) != 0) {
        SPDLOG_ERROR("TilePyramid::open: '{}' bad magic", path);
        return out;
    }
    if (hdr.version != kGeTilePackVersion) {
        SPDLOG_ERROR("TilePyramid::open: '{}' unsupported version {}", path,
                    hdr.version);
        return out;
    }
    if (hdr.planeCount == 0 || hdr.planeCount > kMaxPlaneCount) {
        SPDLOG_ERROR("TilePyramid::open: '{}' bad planeCount {}", path,
                    hdr.planeCount);
        return out;
    }
    if (hdr.fileSize != fileSize) {
        SPDLOG_ERROR("TilePyramid::open: '{}' header fileSize {} != actual {}",
                    path, hdr.fileSize, fileSize);
        return out;
    }

    const uint64_t descsOff = sizeof(GeTilePackHeader);
    const uint64_t descsBytes = uint64_t(hdr.planeCount) * sizeof(GeTilePlaneDesc);
    if (descsOff + descsBytes > fileSize) {
        SPDLOG_ERROR("TilePyramid::open: '{}' plane descs beyond fileSize", path);
        return out;
    }
    std::vector<GeTilePlaneDesc> descs(hdr.planeCount);
    stream->read(reinterpret_cast<char*>(descs.data()), std::streamsize(descsBytes));
    if (!*stream) {
        SPDLOG_ERROR("TilePyramid::open: '{}' plane descs read failed", path);
        return out;
    }

    auto m = std::make_unique<M>();
    m->planes.resize(hdr.planeCount);

    for (uint16_t p = 0; p < hdr.planeCount; ++p) {
        const GeTilePlaneDesc& d = descs[p];
        if (d.levelCount == 0 || d.levelCount > kMaxLevelCount) {
            SPDLOG_ERROR("TilePyramid::open: '{}' plane {} bad levelCount {}", path,
                        p, d.levelCount);
            return out;
        }
        if (d.mipCount == 0 || d.mipCount > SG_MAX_MIPMAPS) {
            SPDLOG_ERROR("TilePyramid::open: '{}' plane {} bad mipCount {}", path, p,
                        d.mipCount);
            return out;
        }
        if (d.tileSize == 0) {
            SPDLOG_ERROR("TilePyramid::open: '{}' plane {} zero tileSize", path, p);
            return out;
        }
        if (d.encoding > uint16_t(GeTilePlaneEncoding::Rg8)) {
            SPDLOG_ERROR("TilePyramid::open: '{}' plane {} bad encoding {}", path, p,
                        d.encoding);
            return out;
        }

        const uint32_t tileCount = geTilePlaneTileCount(d.levelCount);
        const uint64_t entriesBytes = uint64_t(tileCount) * sizeof(GeTileEntry);
        if (d.indexOffset > fileSize || entriesBytes > fileSize - d.indexOffset) {
            SPDLOG_ERROR("TilePyramid::open: '{}' plane {} index beyond fileSize",
                        path, p);
            return out;
        }

        M::PlaneRuntime& pr = m->planes[p];
        pr.desc = d;
        pr.entries.resize(tileCount);
        stream->seekg(std::streamoff(d.indexOffset), std::ios::beg);
        stream->read(reinterpret_cast<char*>(pr.entries.data()),
                    std::streamsize(entriesBytes));
        if (!*stream) {
            SPDLOG_ERROR("TilePyramid::open: '{}' plane {} index read failed", path,
                        p);
            return out;
        }
        // Presence rule (🎯T168.1 truncation property): the cook writes a FULL
        // index even for a capped/truncated pack, so entries whose blobs lie
        // beyond this file's end are expected — they mark levels this pack
        // simply doesn't carry. Treat them as absent (like offset==0), never
        // as corruption; malformed packs are still caught by the header/desc
        // checks above and by blob reads failing.
        uint32_t beyondEof = 0;
        for (GeTileEntry& e : pr.entries) {
            if (e.offset == 0) continue; // tile absent — valid
            if (e.size == 0 || e.offset >= fileSize || e.offset + e.size > fileSize) {
                e.offset = 0;
                e.size = 0;
                ++beyondEof;
            }
        }
        if (beyondEof > 0) {
            SPDLOG_INFO("TilePyramid::open: '{}' plane {}: {} tile(s) beyond "
                       "fileSize treated as absent (capped/truncated pack)",
                       path, p, beyondEof);
        }

        pr.levelCap = opts.levelCap >= 0
                        ? std::min(int(d.levelCount) - 1, opts.levelCap)
                        : int(d.levelCount) - 1;
        pr.textures.resize(tileCount);

        for (uint16_t level = 0; level <= pr.levelCap; ++level) {
            const uint16_t side = uint16_t(1u << level);
            for (uint8_t face = 0; face < uint8_t(kCubeFaceCount); ++face) {
                for (uint16_t ty = 0; ty < side; ++ty) {
                    for (uint16_t tx = 0; tx < side; ++tx) {
                        const GeTileEntry& e =
                            pr.entries[geTileEntryIndex(level, face, tx, ty)];
                        if (e.offset == 0) continue;
                        m->bytesTotal_ += e.size;
                        ++m->tilesTotal_;
                        if (level == 0) ++m->level0Total_;
                    }
                }
            }
        }
    }

    m->path = path;
    m->opts = opts;
    M* mRaw = m.get();
    m->readerThread = std::thread([mRaw] { mRaw->readerLoop(); });

    out.m = std::move(m);
    SPDLOG_INFO("TilePyramid::open: '{}' {} plane(s), {} tiles / {} bytes to load",
               path, hdr.planeCount, out.m->tilesTotal_, out.m->bytesTotal_);
    return out;
}

void TilePyramid::pump() {
    if (!m) return;
    if (!sg_isvalid()) return; // no GPU context (e.g. unit tests) — inert
    const int cfgBudget = m->opts.uploadsPerPump;
    const int budget = cfgBudget > 0 ? cfgBudget : tweakUploadsPerFrame.get();
    for (int i = 0; i < budget; ++i) {
        HeapTile t;
        if (!m->queue.tryPop(t)) break;
        m->uploadTile(t);
    }
}

int TilePyramid::planeIndex(const char* name) const {
    if (!m) return -1;
    const std::string want(name);
    for (size_t i = 0; i < m->planes.size(); ++i) {
        const char* n = m->planes[i].desc.name;
        const size_t cap = sizeof(m->planes[i].desc.name);
        const void* nul = std::memchr(n, '\0', cap);
        const size_t len = nul ? size_t(static_cast<const char*>(nul) - n) : cap;
        const std::string have(n, len);
        if (have == want) return int(i);
    }
    return -1;
}

const GeTilePlaneDesc& TilePyramid::planeDesc(int plane) const {
    static const GeTilePlaneDesc kEmpty{};
    if (!m || plane < 0 || size_t(plane) >= m->planes.size()) return kEmpty;
    return m->planes[plane].desc;
}

bool TilePyramid::baseResident() const {
    return m && m->level0Loaded_.load(std::memory_order_acquire) >= m->level0Total_;
}

bool TilePyramid::fullyResident() const {
    return m && m->tilesLoaded_.load(std::memory_order_acquire) >= m->tilesTotal_;
}

uint64_t TilePyramid::bytesResident() const {
    return m ? m->bytesResident_.load(std::memory_order_acquire) : 0;
}

uint64_t TilePyramid::bytesTotal() const { return m ? m->bytesTotal_ : 0; }

TileView TilePyramid::resolve(int plane, CubeFace face, uint8_t level, uint16_t tx,
                              uint16_t ty) const {
    TileView view;
    if (!m || plane < 0 || size_t(plane) >= m->planes.size()) return view;
    const M::PlaneRuntime& pr = m->planes[plane];
    if (level >= pr.desc.levelCount) return view;

    uint8_t curLevel = level;
    uint16_t curTx = tx, curTy = ty;
    while (true) {
        const uint32_t idx = geTileEntryIndex(curLevel, uint8_t(face), curTx, curTy);
        const GpuTexture& gt = pr.textures[idx];
        if (!gt.isNull()) {
            const TileAncestorRemap remap = tileAncestorRemap(level, tx, ty, curLevel);
            view.view = gt.view;
            view.level = curLevel;
            view.uvScale = remap.uvScale;
            view.uvBias = remap.uvBias;
            return view;
        }
        if (curLevel == 0) break;
        --curLevel;
        curTx = uint16_t(curTx >> 1);
        curTy = uint16_t(curTy >> 1);
    }
    return view; // no resident ancestor — invalid TileView
}

} // namespace ge
