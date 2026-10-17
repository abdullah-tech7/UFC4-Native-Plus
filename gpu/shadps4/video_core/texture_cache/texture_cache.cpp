// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <xxhash.h>

#include "bbport_toggles.h"
#include "bbport_free_check.h"
#include "bbport_clobber.h"
#include "common/assert.h"
#include "common/debug.h"
#include "common/div_ceil.h"
#include "common/hash.h"
#include "common/scope_exit.h"
#include "core/emulator_settings.h"
#include "core/memory.h"
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/page_manager.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/host_compatibility.h"
#include "video_core/texture_cache/texture_cache.h"
#include "video_core/texture_cache/tile_manager.h"

namespace VideoCore {

static constexpr u64 PageShift = 12;
static constexpr u64 NumFramesBeforeRemoval = 32;

TextureCache::TextureCache(const Vulkan::Instance& instance_, Vulkan::Scheduler& scheduler_,
                           Vulkan::Runtime& runtime_, AmdGpu::Liverpool* liverpool_,
                           BufferCache& buffer_cache_, PageManager& tracker_)
    : instance{instance_}, scheduler{scheduler_}, runtime{runtime_}, liverpool{liverpool_},
      buffer_cache{buffer_cache_}, tracker{tracker_}, blit_helper{instance, scheduler},
      tile_manager{instance, scheduler, runtime, buffer_cache.GetStreamBuffer()},
      readback_linear_images{EmulatorSettings.IsReadbackLinearImagesEnabled()} {

    u32 max_samplers = instance.GetMaxSamplerAllocationCount();
    trigger_gc_samplers = max_samplers * 3 / 4;
    pressure_gc_samplers = max_samplers * 7 / 8;
    critical_gc_samplers = max_samplers * 15 / 16;

    // Set up garbage collection parameters.
    if (!instance.CanReportMemoryUsage()) {
        trigger_gc_memory = 0;
        pressure_gc_memory = DEFAULT_PRESSURE_GC_MEMORY;
        critical_gc_memory = DEFAULT_CRITICAL_GC_MEMORY;
        return;
    }

    const s64 device_local_memory = static_cast<s64>(instance.GetTotalMemoryBudget());
    const s64 min_spacing_expected = device_local_memory - 1_GB;
    const s64 min_spacing_critical = device_local_memory - 512_MB;
    const s64 mem_threshold = std::min<s64>(device_local_memory, TARGET_GC_THRESHOLD);
    const s64 min_vacancy_expected = (6 * mem_threshold) / 10;
    const s64 min_vacancy_critical = (2 * mem_threshold) / 10;
    pressure_gc_memory = static_cast<u64>(
        std::max<u64>(std::min(device_local_memory - min_vacancy_expected, min_spacing_expected),
                      DEFAULT_PRESSURE_GC_MEMORY));
    critical_gc_memory = static_cast<u64>(
        std::max<u64>(std::min(device_local_memory - min_vacancy_critical, min_spacing_critical),
                      DEFAULT_CRITICAL_GC_MEMORY));
    trigger_gc_memory = static_cast<u64>((device_local_memory - mem_threshold) / 2);
}

TextureCache::~TextureCache() = default;

namespace {
// Backport of mancelli's 01b32f45 (ufc3 T6a). Games compress textures on the GPU by writing BC
// blocks through an uncompressed storage view (e.g. R32G32B32A32_UINT) of the compressed
// texture's memory. SHADPS4_BC_MODE (default 3): bit 0 = a compressed view of an uncompressed
// image goes through memory (else the old host image copy), bit 1 = storage images over memory
// a compressed image is created on are written back first.
u32 BcMode() {
    static const u32 mode = [] {
        const char* env = std::getenv("SHADPS4_BC_MODE");
        return env && *env ? static_cast<u32>(std::strtoul(env, nullptr, 0)) : 3u;
    }();
    return mode;
}

// ufc3 T6a: "[T6a]" lines, once per address and kind (at most 200), called with the cache mutex
// held. They show whether the game compresses textures on the GPU (Create Fighter skin tone).
template <typename... Args>
void LogT6a(char kind, VAddr address, const char* format, Args... args) {
    static std::unordered_set<u64> seen;
    if (seen.size() >= 200 || !seen.insert(address ^ (u64(kind) << 56)).second) {
        return;
    }
    std::printf(format, args...);
}

bool T4TraceEnabled() {
    static const bool enabled = [] {
        const char* env = std::getenv("BB_T4_TRACE");
        return env && env[0] == '1';
    }();
    return enabled;
}

bool T4Portrait(const ImageInfo& info) {
    return (info.size.width == 864 && info.size.height == 1360) ||
           (info.size.width == 652 && info.size.height == 1080) ||
           (info.size.width == 600 && info.size.height == 1080) ||
           (info.size.width == 1000 && info.size.height == 1200) ||
           (info.size.width == 252 && info.size.height == 400) ||
           (info.size.width == 328 && info.size.height == 280) ||
           (info.size.width == 188 && info.size.height == 160) ||
           (info.size.width == 360 && info.size.height == 260) ||
           (info.size.width == 120 && info.size.height == 100);
}

bool T4PreserveAlias(const Image& source, const ImageInfo& destination) {
    static const bool enabled = [] {
        const char* env = std::getenv("BB_T4_PRESERVE_ALIAS");
        return !env || std::string_view{env} != "0";
    }();
    const auto& src = source.info;
    // UFC-13: only the observed portrait RT -> sampled texture transitions. A CPU/buffer
    // write invalidates SafeToDownload(), so reused pools do not inherit a stale GPU image.
    const bool padded_shape =
        (destination.size.width >= 600 && src.size == Extent3D{1024, 2048, 1}) ||
        (destination.size == Extent3D{252, 400, 1} && src.size == Extent3D{256, 512, 1}) ||
        ((destination.size == Extent3D{328, 280, 1} ||
          destination.size == Extent3D{360, 260, 1}) && src.size == Extent3D{512, 512, 1}) ||
        (destination.size == Extent3D{188, 160, 1} && src.size == Extent3D{256, 256, 1}) ||
        (destination.size == Extent3D{120, 100, 1} && src.size == Extent3D{128, 128, 1});
    return enabled && T4Portrait(destination) && padded_shape &&
           source.SafeToDownload() && source.usage.render_target &&
           src.guest_address == destination.guest_address &&
           destination.size.depth == 1 &&
           src.type == AmdGpu::ImageType::Color2D && destination.type == src.type &&
           src.pixel_format == vk::Format::eR8G8B8A8Unorm &&
           destination.pixel_format == src.pixel_format &&
           src.tile_mode == AmdGpu::TileMode::Thin2DThin &&
           destination.tile_mode == src.tile_mode && src.array_mode == destination.array_mode &&
           src.props.is_tiled && destination.props.is_tiled &&
           src.alt_tile == destination.alt_tile && src.bank_swizzle == destination.bank_swizzle &&
           src.num_bits == 32 && destination.num_bits == 32 &&
           src.num_samples == 1 && destination.num_samples == 1 &&
           src.resources.layers == 1 && destination.resources.layers == 1 &&
           src.resources.levels == 1 && destination.resources.levels >= 1 &&
           src.pitch == src.size.width && destination.pitch == src.pitch &&
           src.mips_layout[0].pitch == destination.mips_layout[0].pitch &&
           src.mips_layout[0].offset == 0 && destination.mips_layout[0].offset == 0 &&
           !((src.micro_tiled_mips | destination.micro_tiled_mips) & 1u);
}

// UFC-13: diagnostics only; never write tiled images back as linear pixels. Called on the
// GPU thread. Group by layout rather than address so per-frame allocations cannot flood logs.
void LogT4Image(const Image& image, const char* event, u64 tick, bool linear_readback,
                const u8* pixels = nullptr, u32 bytes = 0) {
    const auto& info = image.info;
    if (!T4TraceEnabled() || info.size.width < 64 || info.size.height < 64 || info.props.is_depth ||
        info.props.is_block || info.props.is_volume) {
        return;
    }
    static std::unordered_map<std::string, u32> counts;
    const auto layout = fmt::format("{} {}x{} pitch={} fmt={} tile={} tiled={}", event,
                                    info.size.width, info.size.height, info.pitch,
                                    vk::to_string(info.pixel_format), u32(info.tile_mode),
                                    bool(info.props.is_tiled));
    auto it = counts.find(layout);
    if (it == counts.end()) {
        if (counts.size() >= 512) {
            return;
        }
        it = counts.emplace(layout, 0).first;
    }
    if (it->second >= 32) {
        return;
    }
    ++it->second;
    std::string content;
    if (pixels && bytes) {
        // This is the completed host staging copy, never a read of protected guest pages.
        u32 nonzero = 0;
        for (u32 i = 0; i < bytes; ++i) {
            nonzero += pixels[i] != 0;
        }
        content = fmt::format(" host_bytes={} nonzero={} hash={:#x}", bytes, nonzero,
                              XXH3_64bits(pixels, bytes));
        if (info.pixel_format == vk::Format::eR8G8B8A8Unorm ||
            info.pixel_format == vk::Format::eR8G8B8A8Srgb) {
            // Visible level-0 pixels only: padding can contain nonzero staging bytes.
            std::array<u32, 4> channel_nonzero{};
            for (u32 y = 0; y < info.size.height; ++y) {
                for (u32 x = 0; x < info.size.width; ++x) {
                    const u64 offset = (u64(y) * info.pitch + x) * 4;
                    if (offset + 4 <= bytes) {
                        for (u32 c = 0; c < 4; ++c) {
                            channel_nonzero[c] += pixels[offset + c] != 0;
                        }
                    }
                }
            }
            content += fmt::format(" rgba_nonzero={},{},{},{}", channel_nonzero[0],
                                   channel_nonzero[1], channel_nonzero[2], channel_nonzero[3]);
        }
    }
    std::printf("[T4] tick=%llu %s addr=%#llx guest_bytes=%u mips=%u layers=%u "
                "samples=%u flags=%#x linear_eligible=%d%s\n",
                static_cast<unsigned long long>(tick), layout.c_str(),
                static_cast<unsigned long long>(info.guest_address), info.guest_size,
                info.resources.levels, info.resources.layers, info.num_samples, u32(image.flags),
                linear_readback && (!info.props.is_tiled || info.size.width <= 8) &&
                    info.guest_address != 0,
                content.c_str());
}
} // Anonymous namespace

void TextureCache::TraceT4Pixels(Image& image, const char* event, Extent2D region) {
    // Explicit, bounded diagnostic waits, separate from normal BB_T4_TRACE logging. This
    // reads the Vulkan image into host staging; it never changes guest/buffer-cache bytes.
    static const bool pixels_enabled = [] {
        const char* env = std::getenv("BB_T4_TRACE_PIXELS");
        return env && env[0] == '1';
    }();
    const auto& info = image.info;
    if (!T4TraceEnabled() || !pixels_enabled || info.props.is_depth || info.props.is_block ||
        info.size.depth != 1 || info.resources.layers != 1 || info.num_samples != 1 ||
        info.pixel_format != vk::Format::eR8G8B8A8Unorm) {
        return;
    }
    static std::unordered_set<std::string> seen;
    static std::unordered_map<std::string, u32> counts;
    const auto layout = fmt::format("{} {}x{}", event, info.size.width, info.size.height);
    const auto key = fmt::format("{} {:#x} {}", layout, info.guest_address, info.guest_size);
    if (seen.size() >= 16 || counts[layout] >= 2 || !seen.insert(key).second) {
        return;
    }
    ++counts[layout];
    const u64 size = u64(info.pitch) * info.size.height * 4;
    if (size > 8_MB) {
        return;
    }
    const auto download = runtime.GetStagingPool().Request(size, MemoryType::HostCached, 16);
    const vk::BufferImageCopy copy{
        .bufferOffset = download.offset,
        .bufferRowLength = info.pitch,
        .bufferImageHeight = info.size.height,
        .imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
        .imageExtent = {info.size.width, info.size.height, 1},
    };
    runtime.DownloadImage(&image, download.buffer, std::span{&copy, 1});
    scheduler.Finish();
    download.Invalidate();
    LogT4Image(image, event, scheduler.CurrentTick(), readback_linear_images,
               download.mapped, static_cast<u32>(size));
    if (region.width && region.height) {
        // Same completed staging copy: distinguish the requested portrait rectangle from
        // pixels elsewhere in the padded 1024x2048 target. No additional GPU wait or copy.
        const u32 width = std::min(region.width, info.size.width);
        const u32 height = std::min(region.height, info.size.height);
        std::array<u32, 4> channels{};
        for (u32 y = 0; y < height; ++y) {
            for (u32 x = 0; x < width; ++x) {
                const u64 offset = (u64(y) * info.pitch + x) * 4;
                for (u32 c = 0; c < 4; ++c) {
                    channels[c] += download.mapped[offset + c] != 0;
                }
            }
        }
        std::printf("[T4] tick=%llu alias-region-host addr=%#llx region=%ux%u "
                    "rgba_nonzero=%u,%u,%u,%u\n",
                    static_cast<unsigned long long>(scheduler.CurrentTick()),
                    static_cast<unsigned long long>(info.guest_address), width, height,
                    channels[0], channels[1], channels[2], channels[3]);
    }
}

std::string TextureCache::DescribeImagesIn(VAddr addr, u64 size) {
    std::unique_lock lock{mutex, std::try_to_lock}; // the caller may hold it already
    if (!lock.owns_lock()) {
        return " [images: not looked up]";
    }
    std::string out;
    int shown = 0;
    ForEachImageInRegion(addr, size, [&](ImageId, Image& image) {
        if (shown++ < 3) {
            out += fmt::format(" [image {:#x}+{:#x} {}x{} fmt {} tile {}{}{}]", image.info.guest_address,
                               image.info.guest_size, image.info.size.width,
                               image.info.size.height, u32(image.info.pixel_format),
                               u32(image.info.tile_mode), image.usage.render_target ? " RT" : "",
                               True(image.flags & ImageFlagBits::GpuModified) ? " gpu" : "");
        }
    });
    if (shown > 3) {
        out += fmt::format(" +{} more", shown - 3);
    }
    return out;
}

namespace {
// UFC-48 (BB_WRITE_WINDOW): an image download writes over the guest bytes as they were when it
// was requested (PageManager::ReadbackSnapshot); those the CPU wrote since, through a write
// window without a fault, stay (PageManager::WriteReadback), as for buffer readbacks. Written
// straight before: a window's CPU bytes over a GPU-written image were lost to its download.
std::vector<u8> ImageReadbackStart(VAddr address, u64 size) {
    std::vector<u8> at_start;
    if (PageManager::WriteWindowsEnabled()) {
        at_start.resize(size);
        PageManager::ReadbackSnapshot(address, at_start.data(), size);
    }
    return at_start;
}

bool WriteImageReadback(VAddr address, const u8* data, u64 size,
                        const std::vector<u8>& at_start) {
    if (at_start.empty()) {
        return Core::Memory::Instance()->TryWriteBacking(std::bit_cast<u8*>(address), data, size);
    }
    bool written = true;
    PageManager::WriteReadback(
        address, data, at_start.data(), size,
        [](void* context, VAddr at, const u8* bytes, u64 n) {
            *static_cast<bool*>(context) &=
                Core::Memory::Instance()->TryWriteBacking(std::bit_cast<u8*>(at), bytes, n);
        },
        &written);
    PageManager::DiagnoseWindowWrite(address, std::span<const u8>{data, size}, false);
    return written;
}
} // namespace

void TextureCache::ProcessDownloadImages() {
    // bbport: called for every fence (~450 a frame): the lock only when something is queued.
    if (!downloads_queued.load(std::memory_order_acquire)) {
        return;
    }
    std::unique_lock lk{download_images_mutex};
    downloads_queued.store(false, std::memory_order_relaxed);
    for (const ImageId image_id : download_images) {
        DownloadImageMemory(image_id, true);
    }
    download_images.clear();
}

void TextureCache::DownloadImageMemory(ImageId image_id, bool sync) {
    Image& image = slot_images[image_id];
    if (False(image.flags & ImageFlagBits::GpuModified)) {
        return;
    }
    const u32 download_size = image.info.pitch * image.info.size.height * image.info.size.depth *
                              image.info.resources.layers * (image.info.num_bits / 8);
    ASSERT(download_size <= image.info.guest_size);
    vk::BufferImageCopy image_copy = {
        .bufferOffset = 0,
        .bufferRowLength = image.info.pitch,
        .bufferImageHeight = image.info.size.height,
        .imageSubresource =
            {
                .aspectMask = image.info.props.is_depth ? vk::ImageAspectFlagBits::eDepth
                                                        : vk::ImageAspectFlagBits::eColor,
                .mipLevel = 0,
                .baseArrayLayer = 0,
                .layerCount = image.info.resources.layers,
            },
        .imageOffset = {0, 0, 0},
        .imageExtent = {image.info.size.width, image.info.size.height, image.info.size.depth},
    };
    // bbport BB_GUEST_IN_PLACE: the GPU copies the image into the game's memory itself, in stream
    // order: before the fences that follow, no wait here and no CPU copy afterwards.
    if (GuestInPlace() && image.info.guest_address != 0) {
        const auto [arena, offset] =
            buffer_cache.ObtainBuffer(image.info.guest_address, download_size, true);
        if (buffer_cache.IsInPlace(image.info.guest_address, download_size)) {
            LogT4Image(image, "download-in-place", scheduler.CurrentTick(), readback_linear_images);
            image_copy.bufferOffset = offset;
            runtime.DownloadImage(&image, arena, std::span{&image_copy, 1});
            return;
        }
    }
    const auto download =
        runtime.GetStagingPool().Request(download_size, MemoryType::HostCached, 16, !sync);
    auto at_start = ImageReadbackStart(image.info.guest_address, download_size);
    const vk::BufferImageCopy image_download = {
        .bufferOffset = download.offset,
        .bufferRowLength = image.info.pitch,
        .bufferImageHeight = image.info.size.height,
        .imageSubresource =
            {
                .aspectMask = image.info.props.is_depth ? vk::ImageAspectFlagBits::eDepth
                                                        : vk::ImageAspectFlagBits::eColor,
                .mipLevel = 0,
                .baseArrayLayer = 0,
                .layerCount = image.info.resources.layers,
            },
        .imageOffset = {0, 0, 0},
        .imageExtent = {image.info.size.width, image.info.size.height, image.info.size.depth},
    };
    runtime.DownloadImage(&image, download.buffer, std::span{&image_download, 1});
    if (sync) {
        scheduler.Finish();
        download.Invalidate();
        LogT4Image(image, "download-host", scheduler.CurrentTick(), readback_linear_images,
                   download.mapped, download_size);
        BbFreeCheck::Check(image.info.guest_address, download_size, download.mapped,
                           BbFreeCheck::ImageDownload);
        BbClobber::NoteWrite(image.info.guest_address, 0, u32(download_size),
                             BbClobber::ImageDownload);
        const bool written = WriteImageReadback(image.info.guest_address, download.mapped,
                                                download_size, at_start);
        LogT4Image(image, written ? "guest-written" : "guest-write-failed",
                   scheduler.CurrentTick(), readback_linear_images);
        if (!WriteTracking()) {
            buffer_cache.InvalidateMemory(image.info.guest_address, download_size);
        }
    } else {
        scheduler.DeferPriorityOperation(
            [this, device_addr = image.info.guest_address, download, download_size,
             at_start = std::move(at_start)] {
                download.Invalidate();
                BbFreeCheck::Check(device_addr, download_size, download.mapped,
                                   BbFreeCheck::ImageDownload);
                BbClobber::NoteWrite(device_addr, 0, u32(download_size), BbClobber::ImageDownload);
                WriteImageReadback(device_addr, download.mapped, download_size, at_start);
                if (!WriteTracking()) {
                    buffer_cache.InvalidateMemory(device_addr, download_size);
                }
                runtime.GetStagingPool().FreeDeferred(download);
            });
    }
}

void TextureCache::TileImageToBuffer(Image& image) {
    // Stays on the GPU: a CPU write back would have to wait for the GPU and take the memory
    // manager lock, which guest threads hold while waiting for the GPU thread (unmap flushes).
    const VAddr address = image.info.guest_address;
    const u32 size = image.info.guest_size;

    boost::container::small_vector<vk::BufferImageCopy, 14> copies;
    for (u32 mip = 0; mip < image.info.resources.levels; ++mip) {
        const auto& mip_info = image.info.mips_layout[mip];
        copies.push_back({
            .bufferOffset = mip_info.offset,
            .bufferRowLength = mip_info.pitch,
            .bufferImageHeight = mip_info.height,
            .imageSubresource{
                .aspectMask = image.aspect_mask & ~vk::ImageAspectFlagBits::eStencil,
                .mipLevel = mip,
                .baseArrayLayer = 0,
                .layerCount = image.info.resources.layers,
            },
            .imageOffset = {0, 0, 0},
            .imageExtent = {std::max(image.info.size.width >> mip, 1u),
                            std::max(image.info.size.height >> mip, 1u),
                            std::max(image.info.size.depth >> mip, 1u)},
        });
    }
    scheduler.EndRendering();
    const auto [arena, arena_offset] = buffer_cache.ObtainBuffer(address, size, true);
    tile_manager.TileImage(image, copies, arena, arena_offset);

    // Image uploads detile from this buffer without checking for earlier writes to it.
    // bbport: recorded through Record() (threaded recording); the tiler writes from a compute
    // shader, or with a copy for linear images.
    scheduler.Record([](vk::CommandBuffer cmdbuf) {
        const vk::MemoryBarrier2 tiled_barrier = {
            .srcStageMask =
                vk::PipelineStageFlagBits2::eComputeShader | vk::PipelineStageFlagBits2::eCopy,
            .srcAccessMask =
                vk::AccessFlagBits2::eShaderWrite | vk::AccessFlagBits2::eTransferWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .dstAccessMask = vk::AccessFlagBits2::eMemoryRead,
        };
        cmdbuf.pipelineBarrier2(vk::DependencyInfo{
            .memoryBarrierCount = 1u,
            .pMemoryBarriers = &tiled_barrier,
        });
    });
}

void TextureCache::WriteBackImage(ImageId image_id) {
    Image& image = slot_images[image_id];
    TileImageToBuffer(image);
    image.flags |= ImageFlagBits::WrittenBack;
    ForEachImageInRegion(image.info.guest_address, image.info.guest_size,
                         [&](ImageId other_id, Image& other) {
                             if (other_id != image_id && other.info.props.is_block) {
                                 other.flags |= ImageFlagBits::GpuDirty;
                             }
                         });
}

void TextureCache::MarkAsMaybeDirty(ImageId image_id, Image& image) {
    if (image.hash == 0) {
        // Initialize hash
        const u8* addr = std::bit_cast<u8*>(image.info.guest_address);
        image.hash = XXH3_64bits(addr, image.info.guest_size);
    }
    image.flags |= ImageFlagBits::MaybeCpuDirty;
    UntrackImage(image_id);
}

void TextureCache::InvalidateMemory(VAddr addr, size_t size) {
    std::scoped_lock lock{mutex};
    const auto pages_start = PageManager::GetPageAddr(addr);
    const auto pages_end = PageManager::GetNextPageAddr(addr + size - 1);
    ForEachImageInRegion(pages_start, pages_end - pages_start, [&](ImageId image_id, Image& image) {
        const auto image_begin = image.info.guest_address;
        const auto image_end = image.info.guest_address + image.info.guest_size;
        if (image.Overlaps(addr, size)) {
            // Modified region overlaps image, so the image was definitely accessed by this fault.
            // Untrack the image, so that the range is unprotected and the guest can write freely.
            image.flags |= ImageFlagBits::CpuDirty;
            PageManager::NoteImageFault(image.info.guest_address, image.info.guest_size,
                                        image.info.size.width, image.info.size.height,
                                        u32(image.info.pixel_format), u32(image.info.tile_mode));
            UntrackImage(image_id);
        } else if (pages_end < image_end) {
            // This page access may or may not modify the image.
            // We should not mark it as dirty now. If it really was modified
            // it will receive more invalidations on its other pages.
            // Remove tracking from this page only.
            UntrackImageHead(image_id);
        } else if (image_begin < pages_start) {
            // This page access does not modify the image but the page should be untracked.
            // We should not mark this image as dirty now. If it really was modified
            // it will receive more invalidations on its other pages.
            UntrackImageTail(image_id);
        } else {
            // Image begins and ends on this page so it can not receive any more invalidations.
            // We will check it's hash later to see if it really was modified.
            MarkAsMaybeDirty(image_id, image);
        }
    });
}

// bbport BB_IMAGE_WATCH=addr (diagnostics): how the image at that address is used (first 300).
void TextureCache::WatchImage(const Image& image, const char* what) {
    static const u64 watched = [] {
        const char* env = std::getenv("BB_IMAGE_WATCH");
        return env ? std::strtoull(env, nullptr, 0) : 0ull;
    }();
    if (!watched || image.info.guest_address != watched) {
        return;
    }
    static std::atomic<u32> printed{0};
    if (printed.fetch_add(1) < 300) {
        std::printf("Image watch %#llx: %s, flags %#x\n", (unsigned long long)watched, what,
                    u32(image.flags));
    }
}

void TextureCache::InvalidateMemoryFromGPU(VAddr address, size_t max_size) {
    std::scoped_lock lock{mutex};
    // bbport: only an image starting at `address` is affected, so the images over its first byte
    // are all there is to look at (the whole range was walked for every GPU write binding).
    ForEachImageInRegion(address, 1, [&](ImageId image_id, Image& image) {
        static const bool log = std::getenv("BB_IMAGE_SYNC_LOG") != nullptr;
        if (log) {
            static std::atomic<u32> printed{0};
            if (printed.fetch_add(1) < 200) {
                std::printf("Image sync: GPU write %#llx+%#llx over image %#llx %ux%u fmt %u flags %#x%s\n",
                            (unsigned long long)address, (unsigned long long)max_size,
                            (unsigned long long)image.info.guest_address, image.info.size.width,
                            image.info.size.height, u32(image.info.pixel_format), u32(image.flags),
                            image.info.guest_address == address ? "" : " (not at its start: kept)");
            }
        }
        // Only consider images that match base address.
        // TODO: Maybe also consider subresources
        if (image.info.guest_address != address) {
            return;
        }
        // Ensure image is reuploaded when accessed again.
        WatchImage(image, "GPU write (marked for refresh)");
        image.flags |= ImageFlagBits::GpuDirty;
    });
}

void TextureCache::UnmapMemory(VAddr cpu_addr, size_t size) {
    std::scoped_lock lk{mutex};

    ImageIds deleted_images;
    ForEachImageInRegion(cpu_addr, size, [&](ImageId id, Image&) { deleted_images.push_back(id); });
    for (const ImageId id : deleted_images) {
        // TODO: Download image data back to host.
        FreeImage(id);
    }
}

ImageId TextureCache::ResolveDepthOverlap(const ImageInfo& requested_info, BindingType binding,
                                          ImageId cache_image_id) {
    auto& cache_image = slot_images[cache_image_id];

    if (!cache_image.info.props.is_depth && !requested_info.props.is_depth) {
        return {};
    }

    const bool stencil_match =
        requested_info.props.has_stencil == cache_image.info.props.has_stencil;
    const bool bpp_match = requested_info.num_bits == cache_image.info.num_bits;

    // If an image in the cache has less slices we need to expand it
    bool recreate = cache_image.info.resources < requested_info.resources;

    switch (binding) {
    case BindingType::Texture:
        // The guest requires a depth sampled texture, but cache can offer only Rxf. Need to
        // recreate the image.
        recreate |= requested_info.props.is_depth && !cache_image.info.props.is_depth;
        break;
    case BindingType::Storage:
        // If the guest is going to use previously created depth as storage, the image needs to be
        // recreated. (TODO: Probably a case with linear rgba8 aliasing is legit)
        recreate |= cache_image.info.props.is_depth;
        break;
    case BindingType::RenderTarget:
        // Render target can have only Rxf format. If the cache contains only Dx[S8] we need to
        // re-create the image.
        ASSERT(!requested_info.props.is_depth);
        recreate |= cache_image.info.props.is_depth;
        break;
    case BindingType::DepthTarget:
        // The guest has requested previously allocated texture to be bound as a depth target.
        // In this case we need to convert Rx float to a Dx[S8] as requested
        recreate |= !cache_image.info.props.is_depth;

        // The guest is trying to bind a depth target and cache has it. Need to be sure that aspects
        // and bpp match
        recreate |= cache_image.info.props.is_depth && !(stencil_match && bpp_match);
        break;
    default:
        break;
    }

    if (recreate) {
        auto new_info = requested_info;
        new_info.resources = std::max(requested_info.resources, cache_image.info.resources);
        new_info.UpdateSize();
        const auto new_image_id = slot_images.insert(instance, runtime, slot_image_views, new_info);
        RegisterImage(new_image_id);

        // Inherit image usage
        auto& new_image = slot_images[new_image_id];
        new_image.usage = cache_image.usage;
        new_image.flags &= ~ImageFlagBits::Dirty;
        // When creating a depth buffer through overlap resolution don't clear it on first use.
        new_image.info.meta_info.htile_clear_mask = 0;
        runtime.CopyColorAndDepth(&cache_image, &new_image);

        // Free the cache image.
        FreeImage(cache_image_id);
        return new_image_id;
    }

    // Will be handled by view
    return cache_image_id;
}

std::tuple<ImageId, int, int> TextureCache::ResolveOverlap(const ImageInfo& image_info,
                                                           BindingType binding,
                                                           ImageId cache_image_id,
                                                           ImageId merged_image_id) {
    auto& cache_image = slot_images[cache_image_id];
    const bool safe_to_delete =
        scheduler.CurrentTick() - cache_image.tick_accessed_last > NumFramesBeforeRemoval;

    // Equal address
    if (image_info.guest_address == cache_image.info.guest_address) {
        const u32 lhs_block_size = image_info.num_bits * image_info.num_samples;
        const u32 rhs_block_size = cache_image.info.num_bits * cache_image.info.num_samples;
        if (image_info.BlockDim() != cache_image.info.BlockDim() ||
            lhs_block_size != rhs_block_size) {
            static u32 alias_trace_count = 0;
            if (T4TraceEnabled() && T4Portrait(image_info) &&
                cache_image.info.size == Extent3D{1024, 2048, 1} &&
                cache_image.SafeToDownload() && alias_trace_count < 64) {
                ++alias_trace_count;
                std::printf("[T4] tick=%llu alias-rejected-blockdim addr=%#llx "
                            "cached=%ux%u pitch=%u tile=%u mips=%u write_tick=%llu "
                            "requested=%ux%u pitch=%u tile=%u mips=%u binding=%u\n",
                            static_cast<unsigned long long>(scheduler.CurrentTick()),
                            static_cast<unsigned long long>(image_info.guest_address),
                            cache_image.info.size.width, cache_image.info.size.height,
                            cache_image.info.pitch, u32(cache_image.info.tile_mode),
                            cache_image.info.resources.levels,
                            static_cast<unsigned long long>(cache_image.gpu_write_tick),
                            image_info.size.width, image_info.size.height, image_info.pitch,
                            u32(image_info.tile_mode), image_info.resources.levels, u32(binding));
                TraceT4Pixels(cache_image, "alias-before-reject-host",
                              {image_info.size.width, image_info.size.height});
            }
            // Very likely this kind of overlap is caused by allocation from a pool.
            if (safe_to_delete) {
                FreeImage(cache_image_id);
            }
            return {merged_image_id, -1, -1};
        }

        if (const auto depth_image_id = ResolveDepthOverlap(image_info, binding, cache_image_id)) {
            return {depth_image_id, -1, -1};
        }

        // Compressed view of uncompressed image with same block size.
        if (image_info.props.is_block && !cache_image.info.props.is_block) {
            const bool gpu_written = cache_image.SafeToDownload();
            LogT6a('B', image_info.guest_address,
                   "[T6a] BC view %s over %s image %#llx %ux%u mips %u (%s%s%s): %s\n",
                   vk::to_string(image_info.pixel_format).c_str(),
                   vk::to_string(cache_image.info.pixel_format).c_str(),
                   (unsigned long long)cache_image.info.guest_address,
                   cache_image.info.size.width, cache_image.info.size.height,
                   cache_image.info.resources.levels,
                   gpu_written ? "GPU-written" : "not GPU-written",
                   cache_image.usage.storage ? ", storage" : "",
                   cache_image.usage.render_target ? ", render target" : "",
                   (BcMode() & 1) == 0 ? "host copy (SHADPS4_BC_MODE)"
                   : gpu_written       ? "through memory"
                                       : "reupload from memory");
            if ((BcMode() & 1) == 0) {
                return {ExpandImage(image_info, cache_image_id), -1, -1};
            }
            // Backport of 01b32f45: always go through memory, it is correct for any layout. The
            // image is tiled into the buffer cache copy of guest memory and the BC image uploads
            // from there.
            if (gpu_written) {
                TileImageToBuffer(cache_image);
            }
            if (cache_image.binding.is_bound || cache_image.binding.is_target) {
                cache_image.binding.needs_rebind = 1u;
            }
            FreeImage(cache_image_id);
            return {merged_image_id, -1, -1};
        }

        if (image_info.guest_size == cache_image.info.guest_size &&
            (image_info.type == AmdGpu::ImageType::Color3D ||
             cache_image.info.type == AmdGpu::ImageType::Color3D)) {
            return {ExpandImage(image_info, cache_image_id), -1, -1};
        }

        // The same single-level, single-layer image whose guest size differs only in the
        // tiler's padding: a pow2 flag that differs, or (SacredFire) the same metadata sized once
        // by the tiler from a texture descriptor (1920x1080 RGBA8 -> 0x7f8000, rows aligned to 8)
        // and once from the colour buffer's slice register (0x870000, macro-tile alignment of 64
        // rows). The two differ only for heights such as 1080 that are not a multiple of 64 (CTR
        // in PS4 Pro mode with the 1080p patch); the cached image is the same surface.
        const bool padding_only =
            image_info.tile_mode == cache_image.info.tile_mode &&
            image_info.size == cache_image.info.size &&
            image_info.pitch == cache_image.info.pitch && image_info.resources.levels == 1 &&
            cache_image.info.resources.levels == 1 && image_info.resources.layers == 1 &&
            cache_image.info.resources.layers == 1 &&
            (image_info.props.is_pow2 != cache_image.info.props.is_pow2 ||
             image_info.type == cache_image.info.type);

        // Backport of 01b32f45: a compatible format (e.g. UNORM over SRGB) that needs more
        // mips/layers than the cached image: the guest renders level 0 first and then generates
        // the rest through another view. Expand and keep the GPU-written contents instead of
        // dropping the cached image (which would re-upload stale guest memory).
        if (image_info.pixel_format != cache_image.info.pixel_format &&
            image_info.type == cache_image.info.type && image_info.size == cache_image.info.size &&
            image_info.resources > cache_image.info.resources &&
            IsVulkanFormatCompatible(cache_image.info.pixel_format, image_info.pixel_format)) {
            LogT6a('E', image_info.guest_address,
                   "[T6a] expand %#llx %ux%u %s (mips %u) for %s view with mips %u\n",
                   (unsigned long long)cache_image.info.guest_address,
                   cache_image.info.size.width, cache_image.info.size.height,
                   vk::to_string(cache_image.info.pixel_format).c_str(),
                   cache_image.info.resources.levels,
                   vk::to_string(image_info.pixel_format).c_str(), image_info.resources.levels);
            return {ExpandImage(image_info, cache_image_id), -1, -1};
        }

        // ufc3 T6a: the other order, an uncompressed storage view over a cached BC image (the GPU
        // writes compressed blocks into the BC image itself).
        if (binding == BindingType::Storage && cache_image.info.props.is_block &&
            !image_info.props.is_block) {
            LogT6a('S', image_info.guest_address,
                   "[T6a] storage %s view over BC %s image %#llx %ux%u mips %u: %s\n",
                   vk::to_string(image_info.pixel_format).c_str(),
                   vk::to_string(cache_image.info.pixel_format).c_str(),
                   (unsigned long long)cache_image.info.guest_address,
                   cache_image.info.size.width, cache_image.info.size.height,
                   cache_image.info.resources.levels,
                   IsVulkanFormatCompatible(cache_image.info.pixel_format,
                                            image_info.pixel_format)
                       ? "view of the BC image"
                       : "incompatible, separate image");
        }

        // Size and resources are less than or equal, use image view.
        if (image_info.pixel_format != cache_image.info.pixel_format ||
            image_info.guest_size <= cache_image.info.guest_size || padding_only) {
            auto result_id = merged_image_id ? merged_image_id : cache_image_id;
            const auto& result_image = slot_images[result_id];
            const bool is_compatible =
                IsVulkanFormatCompatible(result_image.info.pixel_format, image_info.pixel_format);
            return {is_compatible ? result_id : ImageId{}, -1, -1};
        }

        // Size and resources are greater, expand the image.
        if (image_info.type == cache_image.info.type &&
            image_info.resources > cache_image.info.resources) {
            return {ExpandImage(image_info, cache_image_id), -1, -1};
        }

        // Size is greater but resources are not, because the tiling mode is different.
        // Likely the address is reused for a image with a different tiling mode.
        if (image_info.tile_mode != cache_image.info.tile_mode) {
            if (safe_to_delete) {
                FreeImage(cache_image_id);
            }
            return {merged_image_id, -1, -1};
        }

        // Enhanced debug logging for unreachable case
        // Calculate expected size based on format and dimensions
        u64 expected_size =
            (static_cast<u64>(image_info.size.width) * static_cast<u64>(image_info.size.height) *
             static_cast<u64>(image_info.size.depth) * static_cast<u64>(image_info.num_bits) / 8);
        LOG_ERROR(Render_Vulkan,
                  "Unresolvable image overlap with equal memory address:\n"
                  "=== OLD IMAGE (cached) ===\n"
                  "  Address:        {:#x}\n"
                  "  Size:           {:#x} bytes\n"
                  "  Format:         {}\n"
                  "  Type:           {}\n"
                  "  Width:          {}\n"
                  "  Height:         {}\n"
                  "  Depth:          {}\n"
                  "  Pitch:          {}\n"
                  "  Mip levels:     {}\n"
                  "  Array layers:   {}\n"
                  "  Samples:        {}\n"
                  "  Tile mode:      {:#x}\n"
                  "  Block size:     {} bits\n"
                  "  Is block-comp:  {}\n"
                  "  Guest size:     {:#x}\n"
                  "  Last accessed:  tick {}\n"
                  "  Safe to delete: {}\n"
                  "  isPow2:         {}\n"
                  "  Alt tile:       {}\n"
                  "\n"
                  "=== NEW IMAGE (requested) ===\n"
                  "  Address:        {:#x}\n"
                  "  Size:           {:#x} bytes\n"
                  "  Format:         {}\n"
                  "  Type:           {}\n"
                  "  Width:          {}\n"
                  "  Height:         {}\n"
                  "  Depth:          {}\n"
                  "  Pitch:          {}\n"
                  "  Mip levels:     {}\n"
                  "  Array layers:   {}\n"
                  "  Samples:        {}\n"
                  "  Tile mode:      {:#x}\n"
                  "  Block size:     {} bits\n"
                  "  Is block-comp:  {}\n"
                  "  Guest size:     {:#x}\n"
                  "  isPow2:         {}\n"
                  "  Alt tile:       {}\n"
                  "\n"
                  "=== COMPARISON ===\n"
                  "  Same format:           {}\n"
                  "  Same type:             {}\n"
                  "  Same tile mode:        {}\n"
                  "  Same block size:       {}\n"
                  "  Same BlockDim:         {}\n"
                  "  Same pitch:            {}\n"
                  "  Same pow2:             {}\n"
                  "  Same alt tile:         {}\n"
                  "  Old resources <= new:  {} (old: {}, new: {})\n"
                  "  Old size <= new size:  {}\n"
                  "  Expected size (calc):  {} bytes\n"
                  "  Size ratio (new/expected): {:.2f}x\n"
                  "  Size ratio (new/old):  {:.2f}x\n"
                  "  Old vs expected diff:  {} bytes ({:+.2f}%)\n"
                  "  New vs expected diff:  {} bytes ({:+.2f}%)\n"
                  "  Merged image ID:       {}\n"
                  "  Binding type:          {}\n"
                  "  Current tick:          {}\n"
                  "  Age (ticks since last access): {}",

                  // Old image details
                  cache_image.info.guest_address, cache_image.info.guest_size,
                  vk::to_string(cache_image.info.pixel_format),
                  static_cast<int>(cache_image.info.type), cache_image.info.size.width,
                  cache_image.info.size.height, cache_image.info.size.depth, cache_image.info.pitch,
                  cache_image.info.resources.levels, cache_image.info.resources.layers,
                  cache_image.info.num_samples, static_cast<u32>(cache_image.info.tile_mode),
                  cache_image.info.num_bits, +cache_image.info.props.is_block,
                  cache_image.info.guest_size, cache_image.tick_accessed_last, safe_to_delete,
                  bool(cache_image.info.props.is_pow2), cache_image.info.alt_tile,

                  // New image details
                  image_info.guest_address, image_info.guest_size,
                  vk::to_string(image_info.pixel_format), static_cast<int>(image_info.type),
                  image_info.size.width, image_info.size.height, image_info.size.depth,
                  image_info.pitch, image_info.resources.levels, image_info.resources.layers,
                  image_info.num_samples, static_cast<u32>(image_info.tile_mode),
                  image_info.num_bits, image_info.props.is_block, image_info.guest_size,
                  bool(image_info.props.is_pow2), image_info.alt_tile,

                  // Comparison
                  (image_info.pixel_format == cache_image.info.pixel_format),
                  (image_info.type == cache_image.info.type),
                  (image_info.tile_mode == cache_image.info.tile_mode),
                  (image_info.num_bits == cache_image.info.num_bits),
                  (image_info.BlockDim() == cache_image.info.BlockDim()),
                  (image_info.pitch == cache_image.info.pitch),
                  (image_info.props.is_pow2 == cache_image.info.props.is_pow2),
                  (image_info.alt_tile == cache_image.info.alt_tile),
                  (cache_image.info.resources <= image_info.resources),
                  cache_image.info.resources.levels, image_info.resources.levels,
                  (cache_image.info.guest_size <= image_info.guest_size), expected_size,

                  // Size ratios
                  static_cast<double>(image_info.guest_size) / expected_size,
                  static_cast<double>(image_info.guest_size) / cache_image.info.guest_size,

                  // Difference between actual and expected sizes with percentages
                  static_cast<s64>(cache_image.info.guest_size) - static_cast<s64>(expected_size),
                  (static_cast<double>(cache_image.info.guest_size) / expected_size - 1.0) * 100.0,

                  static_cast<s64>(image_info.guest_size) - static_cast<s64>(expected_size),
                  (static_cast<double>(image_info.guest_size) / expected_size - 1.0) * 100.0,

                  merged_image_id.index, static_cast<int>(binding), scheduler.CurrentTick(),
                  scheduler.CurrentTick() - cache_image.tick_accessed_last);

        UNREACHABLE_MSG("Encountered unresolvable image overlap with equal memory address.");
    }

    // Right overlap, the image requested is a possible subresource of the image from cache.
    if (image_info.guest_address > cache_image.info.guest_address) {
        if (auto mip = image_info.MipOf(cache_image.info); mip >= 0) {
            if (auto slice = image_info.SliceOf(cache_image.info, mip); slice >= 0) {
                return {cache_image_id, mip, slice};
            }
        }

        // Image isn't a subresource but a chance overlap.
        if (safe_to_delete) {
            FreeImage(cache_image_id);
        }

        return {{}, -1, -1};
    } else {
        // Left overlap, the image from cache is a possible subresource of the image requested
        if (auto mip = cache_image.info.MipOf(image_info); mip >= 0) {
            if (auto slice = cache_image.info.SliceOf(image_info, mip); slice >= 0) {
                // We have a larger image created and a separate one, representing a subres of it
                // bound as render target. In this case we need to rebind render target.
                if (cache_image.binding.is_target) {
                    cache_image.binding.needs_rebind = 1u;
                    if (merged_image_id) {
                        GetImage(merged_image_id).binding.is_target = 1u;
                    }

                    FreeImage(cache_image_id);
                    return {merged_image_id, -1, -1};
                }

                // We need to have a larger, already allocated image to copy this one into
                if (merged_image_id) {
                    auto& merged_image = slot_images[merged_image_id];
                    runtime.CopyMip(&cache_image, &merged_image, mip, slice);
                    FreeImage(cache_image_id);
                }
            }
        }
    }

    return {merged_image_id, -1, -1};
}

ImageId TextureCache::ExpandImage(const ImageInfo& info, ImageId image_id) {
    const auto new_image_id = slot_images.insert(instance, runtime, slot_image_views, info);
    RegisterImage(new_image_id);

    auto& src_image = slot_images[image_id];
    auto& new_image = slot_images[new_image_id];

    RefreshImage(new_image);
    runtime.CopyImage(&src_image, &new_image);

    if (src_image.binding.is_bound || src_image.binding.is_target) {
        src_image.binding.needs_rebind = 1u;
    }

    FreeImage(image_id);
    TrackImage(new_image_id);
    return new_image_id;
}

ImageId TextureCache::FindImage(ImageDesc& desc, bool exact_fmt) {
    const auto& info = desc.info;
    ASSERT(info.guest_address != 0);

    std::scoped_lock lock{mutex};

    u64 key_hash = info.guest_address ^ info.guest_size << 17 ^ u64(info.pixel_format) << 40 ^
                   u64(info.size.width) << 24 ^ info.size.height ^ u64(desc.type) << 58 ^
                   u64(exact_fmt) << 63;
    key_hash = (key_hash ^ key_hash >> 33) * 0xFF51AFD7ED558CCDull;
    key_hash = (key_hash ^ key_hash >> 33) * 0xC4CEB9FE1A85EC53ull;
    key_hash ^= key_hash >> 33;
    auto& cached = find_image_cache[key_hash % find_image_cache.size()];
    // An exact match stays valid while that image is registered with the same description.
    // Portraits also check registrations: a newly rendered padded source can supersede an
    // exact alias. Resolved overlaps likewise depend on the registry generation.
    const auto still_exact = [&] {
        if (cached.view_mip >= 0 || cached.view_slice >= 0 || !cached.image_id ||
            (desc.type == BindingType::Texture && T4Portrait(info))) {
            return false;
        }
        const Image& image = slot_images[cached.image_id];
        return True(image.flags & ImageFlagBits::Registered) &&
               image.info.guest_address == info.guest_address &&
               image.info.guest_size == info.guest_size && image.info.size == info.size &&
               image.info.pixel_format == info.pixel_format;
    };
    if ((cached.generation == registry_generation.load(std::memory_order_relaxed) || still_exact()) &&
        cached.address == info.guest_address &&
        cached.size == info.guest_size && cached.extent == info.size &&
        cached.format == info.pixel_format && cached.type == info.type &&
        cached.exact_fmt == exact_fmt && cached.binding == desc.type &&
        cached.levels == info.resources.levels && cached.layers == info.resources.layers &&
        !BbToggle::Disabled(BbToggle::FindImageCache)) {
        Image& image = slot_images[cached.image_id];
        image.tick_accessed_last = scheduler.CurrentTick();
        TouchImage(image);
        if (cached.view_mip > 0) {
            desc.view_info.range.base.level = cached.view_mip;
        }
        if (cached.view_slice > 0) {
            desc.view_info.range.base.layer = cached.view_slice;
        }
        return cached.image_id;
    }

    ImageIds image_ids;
    ForEachImageInRegion(info.guest_address, info.guest_size,
                         [&](ImageId image_id, Image& image) { image_ids.push_back(image_id); });

    ImageId image_id{};

    // A newly registered padded RT must take precedence over an exact sampled alias.
    // Otherwise a second postprocess pass reuses the old alias (even in the same tick).
    // Consume the source once, before mip generation, so later binds keep generated mips.
    ImageId portrait_source{};
    if (desc.type == BindingType::Texture && T4Portrait(info)) {
        for (const auto id : image_ids) {
            if (T4PreserveAlias(slot_images[id], info)) {
                if (portrait_source) { // Ambiguous pool overlap: keep ordinary resolution.
                    portrait_source = {};
                    break;
                }
                portrait_source = id;
            }
        }
    }
    if (portrait_source) {
        for (const auto id : image_ids) {
            const auto& other = slot_images[id].info;
            if (id != portrait_source && other.guest_address == info.guest_address &&
                other.guest_size >= info.guest_size && other.size == info.size &&
                other.pixel_format == info.pixel_format && other.type == info.type &&
                other.resources.levels >= info.resources.levels &&
                other.resources.layers == info.resources.layers &&
                other.props.is_pow2 == info.props.is_pow2 && other.pitch == info.pitch &&
                other.tile_mode == info.tile_mode && other.alt_tile == info.alt_tile &&
                other.bank_swizzle == info.bank_swizzle &&
                T4PreserveAlias(slot_images[portrait_source], other)) {
                // Keep a previously expanded chain as one image: creating a separate
                // mip-0 alias here would fork it from the later storage mip writes.
                image_id = id;
                break;
            }
        }
        const bool reused = bool(image_id);
        if (!image_id) {
            image_id = slot_images.insert(instance, runtime, slot_image_views, info);
            RegisterImage(image_id);
        }
        // Insertion may move slots; reacquire both references afterwards.
        auto& src = slot_images[portrait_source];
        auto& dst = slot_images[image_id];
        TraceT4Pixels(src, "alias-before-copy-host", {info.size.width, info.size.height});
        RefreshImage(dst);
        // Equal macro-tile parameters and effective pitch give identical addresses in
        // the mip-0 origin rectangle. Slice height has no effect on slice 0 (RGBA8, 1x).
        runtime.CopyImageMip0Region(&src, &dst);
        TraceT4Pixels(dst, "alias-preserved-host");
        static u32 preserved_count = 0;
        if (T4TraceEnabled() && preserved_count++ < 128) {
            std::printf("[T4] tick=%llu alias-preserved addr=%#llx region=%ux%u pitch=%u "
                        "tile=%u mip=0 mips=%u requested_mips=%u src_id=%u dst_id=%u reused=%d\n",
                        static_cast<unsigned long long>(scheduler.CurrentTick()),
                        static_cast<unsigned long long>(info.guest_address),
                        info.size.width, info.size.height, info.pitch, u32(info.tile_mode),
                        dst.info.resources.levels, info.resources.levels, portrait_source.index,
                        image_id.index, reused);
        }
        if (src.binding.is_bound || src.binding.is_target) {
            src.binding.needs_rebind = 1u;
        }
        FreeImage(portrait_source);
        TrackImage(image_id);
    }

    // Check for a perfect match unless a live portrait source has already supplied it.
    for (const auto& cache_id : image_ids) {
        if (portrait_source) break;
        auto& cache_image = slot_images[cache_id];
        if (cache_image.info.guest_address != info.guest_address) {
            continue;
        }
        if (cache_image.info.guest_size != info.guest_size) {
            continue;
        }
        if (cache_image.info.size != info.size) {
            continue;
        }
        if (!IsVulkanFormatCompatible(cache_image.info.pixel_format, info.pixel_format) ||
            (cache_image.info.type != info.type && info.size != Extent3D{1, 1, 1})) {
            continue;
        }
        if (exact_fmt && info.pixel_format != cache_image.info.pixel_format) {
            continue;
        }
        image_id = cache_id;
    }

    // Try to resolve overlaps (if any)
    int view_mip{-1};
    int view_slice{-1};
    if (!image_id) {
        for (const auto& cache_id : image_ids) {
            view_mip = -1;
            view_slice = -1;

            const auto& merged_info = image_id ? slot_images[image_id].info : info;
            auto [overlap_image_id, overlap_view_mip, overlap_view_slice] =
                ResolveOverlap(merged_info, desc.type, cache_id, image_id);
            if (overlap_image_id) {
                image_id = overlap_image_id;
                view_mip = overlap_view_mip;
                view_slice = overlap_view_slice;
            }
        }
    }

    if (image_id) {
        Image& image_resolved = slot_images[image_id];
        if (exact_fmt && info.pixel_format != image_resolved.info.pixel_format) {
            // Cannot reuse this image as we need the exact requested format.
            image_id = {};
        } else if (image_resolved.info.resources < info.resources) {
            // The image was clearly picked up wrong.
            FreeImage(image_id);
            image_id = {};
            LOG_WARNING(Render_Vulkan, "Image overlap resolve failed");
        }
    }
    // Create and register a new image
    if (!image_id) {
        if (info.props.is_block && (BcMode() & 2) != 0) {
            // Backport of 01b32f45: the texture may have been compressed on the GPU into memory
            // that a storage image still holds; write it back so the upload picks up the data.
            ImageIds storage_aliases;
            ForEachImageInRegion(
                info.guest_address, info.guest_size, [&](ImageId other_id, Image& other) {
                    if (!other.info.props.is_block && other.usage.storage &&
                        other.SafeToDownload() && False(other.flags & ImageFlagBits::WrittenBack)) {
                        storage_aliases.push_back(other_id);
                    }
                });
            for (const ImageId alias_id : storage_aliases) {
                const Image& alias = slot_images[alias_id];
                LogT6a('W', alias.info.guest_address,
                       "[T6a] write back storage %s %#llx %ux%u before BC %s %#llx %ux%u\n",
                       vk::to_string(alias.info.pixel_format).c_str(),
                       (unsigned long long)alias.info.guest_address, alias.info.size.width,
                       alias.info.size.height, vk::to_string(info.pixel_format).c_str(),
                       (unsigned long long)info.guest_address, info.size.width, info.size.height);
                WriteBackImage(alias_id);
            }
        }
        image_id = slot_images.insert(instance, runtime, slot_image_views, info);
        RegisterImage(image_id);
    }

    Image& image = slot_images[image_id];
    image.tick_accessed_last = scheduler.CurrentTick();
    TouchImage(image);

    // If the image requested is a subresource of the image from cache record its location.
    if (view_mip > 0) {
        desc.view_info.range.base.level = view_mip;
    }
    if (view_slice > 0) {
        desc.view_info.range.base.layer = view_slice;
    }

    cached = FindImageCacheEntry{
        .address = info.guest_address,
        .size = info.guest_size,
        .extent = info.size,
        .format = info.pixel_format,
        .type = info.type,
        .exact_fmt = exact_fmt,
        .binding = desc.type,
        .levels = info.resources.levels,
        .layers = info.resources.layers,
        .generation = registry_generation.load(std::memory_order_relaxed),
        .image_id = image_id,
        .view_mip = view_mip,
        .view_slice = view_slice,
    };
    return image_id;
}

ImageId TextureCache::FindImageFromRange(VAddr address, size_t size, bool ensure_valid) {
    ImageIds image_ids;
    ForEachImageInRegion(address, size, [&](ImageId image_id, Image& image) {
        if (image.info.guest_address != address) {
            return;
        }
        if (ensure_valid && !image.SafeToDownload()) {
            return;
        }
        image_ids.push_back(image_id);
    });
    if (image_ids.size() == 1) {
        // Sometimes image size might not exactly match with requested buffer size
        // If we only found 1 candidate image use it without too many questions.
        return image_ids.back();
    }
    if (!image_ids.empty()) {
        for (s32 i = 0; i < image_ids.size(); ++i) {
            Image& image = slot_images[image_ids[i]];
            if (image.info.guest_size == size) {
                return image_ids[i];
            }
        }
        LOG_WARNING(Render_Vulkan,
                    "Failed to find exact image match for copy addr={:#x}, size={:#x}", address,
                    size);
    }
    return {};
}

ImageView& TextureCache::FindTexture(ImageId image_id, const ImageDesc& desc, ViewMemo* memo,
                                     bool refresh) {
    Image& image = slot_images[image_id];
    WatchImage(image, refresh ? "texture (refresh)" : "texture (no refresh)");
    if (desc.type == BindingType::Storage) {
        image.flags |= ImageFlagBits::GpuModified;
        image.flags &= ~ImageFlagBits::WrittenBack;
        image.gpu_write_tick = scheduler.CurrentTick();
        if (readback_linear_images && (!image.info.props.is_tiled || image.info.size.width <= 8) &&
            image.info.guest_address != 0) {
            std::unique_lock lk{download_images_mutex};
            download_images.emplace(image_id);
            downloads_queued.store(true, std::memory_order_release);
        }
    }
    if (refresh) {
        UpdateImage(image_id);
    }
    if (T4TraceEnabled() && T4Portrait(desc.info)) {
        static u32 view_count = 0;
        if (view_count++ < 256) {
            const auto& view = desc.view_info;
            std::printf("[T4] tick=%llu view addr=%#llx requested=%ux%u "
                        "resolved=%ux%u image_id=%u binding=%u view_fmt=%s mip=%u+%u "
                        "layer=%u+%u swizzle=%u,%u,%u,%u\n",
                        static_cast<unsigned long long>(scheduler.CurrentTick()),
                        static_cast<unsigned long long>(image.info.guest_address),
                        desc.info.size.width, desc.info.size.height,
                        image.info.size.width, image.info.size.height, image_id.index, u32(desc.type),
                        vk::to_string(view.format).c_str(), view.range.base.level,
                        view.range.extent.levels, view.range.base.layer, view.range.extent.layers,
                        u32(view.mapping.r), u32(view.mapping.g),
                        u32(view.mapping.b), u32(view.mapping.a));
        }
        if (desc.type == BindingType::Texture && image.info.props.is_tiled &&
            False(image.flags & (ImageFlagBits::GpuModified | ImageFlagBits::Dirty))) {
            TraceT4Pixels(image, "alias-upload-host");
        }
    }
    LogT4Image(image, desc.type == BindingType::Storage ? "storage" : "sampled",
               scheduler.CurrentTick(), readback_linear_images);
    if (memo && !BbToggle::Disabled(BbToggle::TextureViewMemo)) {
        if (memo->image_id == image_id && memo->backing == image.backing && memo->view_id) {
            return slot_image_views[memo->view_id];
        }
        ImageView& view = image.FindView(desc.view_info);
        const auto& ids = image.backing->image_view_ids;
        const u32 last = image.backing->last_view;
        memo->image_id = image_id;
        memo->backing = image.backing;
        memo->view_id = last < ids.size() && &slot_image_views[ids[last]] == &view ? ids[last]
                                                                                    : ids.back();
        return view;
    }
    return image.FindView(desc.view_info);
}

ImageView& TextureCache::FindRenderTarget(ImageId image_id, const ImageDesc& desc) {
    Image& image = slot_images[image_id];
    WatchImage(image, "render target");
    image.flags |= ImageFlagBits::GpuModified;
    image.flags &= ~ImageFlagBits::WrittenBack;
    image.gpu_write_tick = scheduler.CurrentTick();
    if (readback_linear_images && (!image.info.props.is_tiled || image.info.size.width <= 8)) {
        std::unique_lock lk{download_images_mutex};
        download_images.emplace(image_id);
        downloads_queued.store(true, std::memory_order_release);
    }
    image.usage.render_target = 1u;
    UpdateImage(image_id);
    LogT4Image(image, "render-target", scheduler.CurrentTick(), readback_linear_images);

    // Register meta data for this color buffer
    if (desc.info.meta_info.cmask_addr) {
        surface_metas.emplace(desc.info.meta_info.cmask_addr,
                              MetaDataInfo{.type = MetaType::CMask});
        image.info.meta_info.cmask_addr = desc.info.meta_info.cmask_addr;
    }

    if (desc.info.meta_info.fmask_addr) {
        surface_metas.emplace(desc.info.meta_info.fmask_addr,
                              MetaDataInfo{.type = MetaType::FMask});
        image.info.meta_info.fmask_addr = desc.info.meta_info.fmask_addr;
    }

    return image.FindView(desc.view_info, false);
}

ImageView& TextureCache::FindDepthTarget(ImageId image_id, const ImageDesc& desc) {
    Image& image = slot_images[image_id];
    WatchImage(image, "depth target");
    image.flags |= ImageFlagBits::GpuModified;
    image.gpu_write_tick = scheduler.CurrentTick();
    image.usage.depth_target = 1u;
    UpdateImage(image_id);

    // Register meta data for this depth buffer
    if (desc.info.meta_info.htile_addr) {
        surface_metas.emplace(desc.info.meta_info.htile_addr,
                              MetaDataInfo{.type = MetaType::HTile,
                                           .clear_mask = image.info.meta_info.htile_clear_mask});
        image.info.meta_info.htile_addr = desc.info.meta_info.htile_addr;
    }

    // If there is a stencil attachment, link depth and stencil.
    if (desc.info.stencil_addr != 0) {
        ImageId stencil_id{};
        ForEachImageInRegion(desc.info.stencil_addr, desc.info.stencil_size,
                             [&](ImageId image_id, Image& image) {
                                 if (image.info.guest_address == desc.info.stencil_addr) {
                                     stencil_id = image_id;
                                 }
                             });
        if (!stencil_id) {
            ImageInfo info{};
            info.guest_address = desc.info.stencil_addr;
            info.guest_size = desc.info.stencil_size;
            info.size = desc.info.size;
            stencil_id = slot_images.insert(instance, runtime, slot_image_views, info);
            RegisterImage(stencil_id);
        }
        Image& stencil_image = slot_images[stencil_id];
        TouchImage(stencil_image);
        stencil_image.AssociateDepth(image_id, image.image_uid);
    }

    return image.FindView(desc.view_info, false);
}

void TextureCache::RefreshImage(Image& image) {
    WatchImage(image, "refresh entered");
    if (False(image.flags & ImageFlagBits::Dirty) || image.info.num_samples > 1) {
        return;
    }
    BbStats::Timer timer{BbStats::t_refresh};

    RENDERER_TRACE;
    TRACE_HINT(fmt::format("{:x}:{:x}", image.info.guest_address, image.info.guest_size));

    if (True(image.flags & ImageFlagBits::MaybeCpuDirty) &&
        False(image.flags & ImageFlagBits::CpuDirty)) {
        // The image size should be less than page size to be considered MaybeCpuDirty
        // So this calculation should be very uncommon and reasonably fast
        // For now we'll just check up to 64 first pixels
        const auto addr = std::bit_cast<u8*>(image.info.guest_address);
        const u32 w = std::min(image.info.size.width, u32(8));
        const u32 h = std::min(image.info.size.height, u32(8));

        const u32 s_w = image.info.props.is_block ? Common::DivCeil(w, 4u) : w;
        const u32 s_h = image.info.props.is_block ? Common::DivCeil(h, 4u) : h;
        const u32 size = s_w * s_h * (image.info.num_bits / 8);
        const u64 hash = XXH3_64bits(addr, size);
        if (image.hash == hash) {
            image.flags &= ~ImageFlagBits::MaybeCpuDirty;
            return;
        }
        image.hash = hash;
    }

    const u32 num_layers = image.info.resources.layers;
    const u32 num_mips = image.info.resources.levels;
    const bool is_gpu_modified = True(image.flags & ImageFlagBits::GpuModified);
    const bool is_gpu_dirty = True(image.flags & ImageFlagBits::GpuDirty);
    static const bool sync_log = std::getenv("BB_IMAGE_SYNC_LOG") != nullptr;
    if (sync_log && is_gpu_dirty) {
        static std::atomic<u32> printed{0};
        if (printed.fetch_add(1) < 100) {
            std::printf("Image sync: refresh of %#llx (%ux%u) from memory the GPU wrote, flags %#x\n",
                        (unsigned long long)image.info.guest_address, image.info.size.width,
                        image.info.size.height, u32(image.flags));
        }
    }

    if (sync_log) {
        // BB_IMAGE_SYNC_LOG: the images refreshed most per 5 s (any reason), with their flags.
        struct Refreshes {
            u64 count = 0, bytes = 0;
            u32 width = 0, height = 0, format = 0, tiling = 0, flags = 0;
        };
        static std::mutex refresh_mutex;
        static std::unordered_map<VAddr, Refreshes> refreshes;
        static auto window_start = std::chrono::steady_clock::now();
        std::scoped_lock lk{refresh_mutex};
        auto& r = refreshes[image.info.guest_address];
        r = {r.count + 1,
             r.bytes + image.info.guest_size,
             image.info.size.width,
             image.info.size.height,
             u32(image.info.pixel_format),
             u32(image.info.tile_mode),
             r.flags | u32(image.flags)};
        if (const auto now = std::chrono::steady_clock::now();
            now - window_start >= std::chrono::seconds(5)) {
            std::vector<std::pair<VAddr, Refreshes>> top(refreshes.begin(), refreshes.end());
            std::ranges::sort(top, std::greater{}, [](const auto& e) { return e.second.bytes; });
            std::string text;
            for (size_t i = 0; i < std::min<size_t>(top.size(), 5); ++i) {
                const auto& [address, e] = top[i];
                text += fmt::format(" {:#x} {}x{} fmt {} tile {}: {}x {:.0f} MB flags {:#x};",
                                    address, e.width, e.height, e.format, e.tiling, e.count,
                                    e.bytes / 1e6, e.flags);
            }
            std::printf("Image refreshes (5 s, %zu images):%s\n", refreshes.size(), text.c_str());
            refreshes.clear();
            window_start = now;
        }
    }
    BbStats::image_upload_bytes.fetch_add(image.info.guest_size, std::memory_order_relaxed);
    boost::container::small_vector<vk::BufferImageCopy, 14> image_copies;
    for (u32 m = 0; m < num_mips; m++) {
        const u32 width = std::max(image.info.size.width >> m, 1u);
        const u32 height = std::max(image.info.size.height >> m, 1u);
        const u32 depth =
            image.info.props.is_volume ? std::max(image.info.size.depth >> m, 1u) : 1u;
        const auto [mip_size, mip_pitch, mip_height, mip_offset] = image.info.mips_layout[m];

        // Protect GPU modified resources from accidental CPU reuploads.
        if (is_gpu_modified && !is_gpu_dirty) {
            const u8* addr = std::bit_cast<u8*>(image.info.guest_address);
            const u64 hash = XXH3_64bits(addr + mip_offset, mip_size);
            if (image.mip_hashes[m] == hash) {
                continue;
            }
            image.mip_hashes[m] = hash;
        }

        const u32 extent_width = mip_pitch ? std::min(mip_pitch, width) : width;
        const u32 extent_height = mip_height ? std::min(mip_height, height) : height;
        image_copies.push_back({
            .bufferOffset = mip_offset,
            .bufferRowLength = mip_pitch,
            .bufferImageHeight = mip_height,
            .imageSubresource{
                .aspectMask = image.aspect_mask & ~vk::ImageAspectFlagBits::eStencil,
                .mipLevel = m,
                .baseArrayLayer = 0,
                .layerCount = num_layers,
            },
            .imageOffset = {0, 0, 0},
            .imageExtent = {extent_width, extent_height, depth},
        });
    }

    WatchImage(image, image_copies.empty() ? "refresh: nothing to copy" : "refresh: uploading");
    if (image_copies.empty()) {
        image.flags &= ~ImageFlagBits::Dirty;
        return;
    }

    scheduler.EndRendering();

    const auto [in_buffer, in_offset] =
        buffer_cache.ObtainBufferForImage(image.info.guest_address, image.info.guest_size);
    const auto [buffer, offset] = tile_manager.DetileImage(in_buffer, in_offset, image.info);
    for (auto& copy : image_copies) {
        copy.bufferOffset += offset;
    }

    runtime.UploadImage(&image, buffer, image_copies);
}

vk::Sampler TextureCache::GetSampler(const AmdGpu::Sampler& sampler,
                                     AmdGpu::BorderColorBuffer border_color_base,
                                     const bool is_depth, float extra_lod_bias) {
    // Compare and plain uses of one S# need separate samplers; so do extra LOD biases.
    const u64 hash = HashCombine(HashCombine(XXH3_64bits(&sampler, sizeof(sampler)), is_depth),
                                 u64(std::bit_cast<u32>(extra_lod_bias)));

    std::scoped_lock lock{samplers_mutex};
    const auto [it, new_sampler] = samplers.try_emplace(hash, instance, sampler, border_color_base,
                                                        is_depth, extra_lod_bias);
    if (new_sampler) {
        samplers.at(hash).lru_id = sampler_lru_cache.Insert(hash, gc_tick);
    } else {
        sampler_lru_cache.Touch(it->second.lru_id, gc_tick);
    }

    return it->second.Handle();
}

void TextureCache::RegisterImage(ImageId image_id) {
    BbStats::images_registered.fetch_add(1, std::memory_order_relaxed);
    Image& image = slot_images[image_id];
    ASSERT_MSG(False(image.flags & ImageFlagBits::Registered),
               "Trying to register an already registered image");
    image.flags |= ImageFlagBits::Registered;
    ++registry_generation;
    total_used_memory += Common::AlignUp(image.info.guest_size, 1024);
    BbStats::live_image_bytes.fetch_add(Common::AlignUp(image.info.guest_size, 1024),
                                        std::memory_order_relaxed);
    BbStats::live_images.fetch_add(1, std::memory_order_relaxed);
    image.lru_id = lru_cache.Insert(image_id, gc_tick);
    image.lru_touched_tick = gc_tick;
    ForEachPage(image.info.guest_address, image.info.guest_size,
                [this, image_id](u64 page) { page_table[page].push_back(image_id); });
}

void TextureCache::UnregisterImage(ImageId image_id) {
    Image& image = slot_images[image_id];
    ASSERT_MSG(True(image.flags & ImageFlagBits::Registered),
               "Trying to unregister an already unregistered image");
    image.flags &= ~ImageFlagBits::Registered;
    ++registry_generation;
    lru_cache.Free(image.lru_id);
    total_used_memory -= Common::AlignUp(image.info.guest_size, 1024);
    BbStats::live_image_bytes.fetch_sub(Common::AlignUp(image.info.guest_size, 1024),
                                        std::memory_order_relaxed);
    BbStats::live_images.fetch_sub(1, std::memory_order_relaxed);
    ForEachPage(image.info.guest_address, image.info.guest_size, [this, image_id](u64 page) {
        const auto page_it = page_table.find(page);
        if (page_it == nullptr) {
            UNREACHABLE_MSG("Unregistering unregistered page=0x{:x}", page << PageShift);
            return;
        }
        auto& image_ids = *page_it;
        const auto vector_it = std::ranges::find(image_ids, image_id);
        if (vector_it == image_ids.end()) {
            ASSERT_MSG(false, "Unregistering unregistered image in page=0x{:x}", page << PageShift);
            return;
        }
        image_ids.erase(vector_it);
    });
}

void TextureCache::TrackImage(ImageId image_id) {
    auto& image = slot_images[image_id];
    if (!(image.flags & ImageFlagBits::Registered)) {
        return;
    }
    const auto image_begin = image.info.guest_address;
    const auto image_end = image.info.guest_address + image.info.guest_size;
    if (image_begin == image.track_addr && image_end == image.track_addr_end) {
        return;
    }

    if (!image.IsTracked()) {
        // Re-track the whole image
        image.track_addr = image_begin;
        image.track_addr_end = image_end;
        tracker.UpdatePageWatchers<1>(image_begin, image.info.guest_size);
    } else {
        if (image_begin < image.track_addr) {
            TrackImageHead(image_id);
        }
        if (image.track_addr_end < image_end) {
            TrackImageTail(image_id);
        }
    }
}

void TextureCache::TrackImageHead(ImageId image_id) {
    auto& image = slot_images[image_id];
    if (!(image.flags & ImageFlagBits::Registered)) {
        return;
    }
    const auto image_begin = image.info.guest_address;
    if (image_begin == image.track_addr) {
        return;
    }
    ASSERT(image.track_addr != 0 && image_begin < image.track_addr);
    const auto size = image.track_addr - image_begin;
    image.track_addr = image_begin;
    tracker.UpdatePageWatchers<1>(image_begin, size);
}

void TextureCache::TrackImageTail(ImageId image_id) {
    auto& image = slot_images[image_id];
    if (!(image.flags & ImageFlagBits::Registered)) {
        return;
    }
    const auto image_end = image.info.guest_address + image.info.guest_size;
    if (image_end == image.track_addr_end) {
        return;
    }
    ASSERT(image.track_addr_end != 0 && image.track_addr_end < image_end);
    const auto addr = image.track_addr_end;
    const auto size = image_end - image.track_addr_end;
    image.track_addr_end = image_end;
    tracker.UpdatePageWatchers<1>(addr, size);
}

void TextureCache::UntrackImage(ImageId image_id) {
    auto& image = slot_images[image_id];
    if (!image.IsTracked()) {
        return;
    }
    const auto addr = image.track_addr;
    const auto size = image.track_addr_end - image.track_addr;
    image.track_addr = 0;
    image.track_addr_end = 0;
    if (size != 0) {
        tracker.UpdatePageWatchers<false>(addr, size);
    }
}

void TextureCache::UntrackImageHead(ImageId image_id) {
    auto& image = slot_images[image_id];
    const auto image_begin = image.info.guest_address;
    if (!image.IsTracked() || image_begin < image.track_addr) {
        return;
    }
    const auto addr = tracker.GetNextPageAddr(image_begin);
    const auto size = addr - image_begin;
    image.track_addr = addr;
    if (image.track_addr == image.track_addr_end) {
        // This image spans only 2 pages and both are modified,
        // but the image itself was not directly affected.
        // Cehck its hash later.
        MarkAsMaybeDirty(image_id, image);
    }
    tracker.UpdatePageWatchers<false>(image_begin, size);
}

void TextureCache::UntrackImageTail(ImageId image_id) {
    auto& image = slot_images[image_id];
    const auto image_end = image.info.guest_address + image.info.guest_size;
    if (!image.IsTracked() || image.track_addr_end < image_end) {
        return;
    }
    ASSERT(image.track_addr_end != 0);
    const auto addr = tracker.GetPageAddr(image_end);
    const auto size = image_end - addr;
    image.track_addr_end = addr;
    if (image.track_addr == image.track_addr_end) {
        // This image spans only 2 pages and both are modified,
        // but the image itself was not directly affected.
        // Cehck its hash later.
        MarkAsMaybeDirty(image_id, image);
    }
    tracker.UpdatePageWatchers<false>(addr, size);
}

void TextureCache::GarbageCollectImages() {
    if (instance.CanReportMemoryUsage()) {
        total_used_memory = instance.GetDeviceMemoryUsage();
        // bbport: on integrated GPUs (Steam Deck) the usage covers system-memory heaps holding
        // much more than images (buffers backing guest memory), and the startup budget left
        // ~1 GB after its 8 GB system reserve: usage stayed above the critical mark, so the
        // collector evicted images used two or three frames ago on every submission and wrote
        // GPU-written ones back. Compare with the driver's current budget instead.
        // BB_GC_BUDGET_MB=N: this rule with a fixed budget on any GPU (tests on a desktop).
        static const u64 forced_budget = [] {
            const char* env = std::getenv("BB_GC_BUDGET_MB");
            return env ? std::strtoull(env, nullptr, 10) << 20 : 0;
        }();
        // bbport: GPUs under 8 GB (laptop RTX 3060 6 GB) got their pressure mark at ~40 % of
        // VRAM from the desktop formula, and UFC 3 fights evicted and re-uploaded hundreds of
        // images every few seconds at 2.3 GB used. They use the budget rule as well.
        static const bool small_vram =
            static_cast<s64>(instance.GetTotalMemoryBudget()) < TARGET_GC_THRESHOLD;
        if (instance.IsIntegrated() || forced_budget || small_vram) {
            const u64 budget = forced_budget ? forced_budget : instance.GetDeviceMemoryBudgetNow();
            if (budget != 0) {
                trigger_gc_memory = budget / 10 * 7;
                pressure_gc_memory = budget / 100 * 85;
                critical_gc_memory = budget / 100 * 95;
            }
        }
    }
    BbStats::gc_used_bytes.store(total_used_memory, std::memory_order_relaxed);
    BbStats::gc_trigger_bytes.store(trigger_gc_memory, std::memory_order_relaxed);
    BbStats::gc_critical_bytes.store(critical_gc_memory, std::memory_order_relaxed);
    // bbport: gc_tick (one per guest submission, hundreds a second) at each of the last 64
    // seconds, for ages in seconds. The usage compared is all of our VRAM, not only images: on a
    // discrete GPU it stays over the trigger, where an age of 16 ticks (~3 frames) evicted every
    // texture briefly off screen and uploaded it again. Below the pressure mark an image goes
    // once unused for BB_GC_IDLE_SECONDS (20): the textures of an area left behind.
    const u64 second = u64(std::chrono::duration_cast<std::chrono::seconds>(
                               std::chrono::steady_clock::now().time_since_epoch())
                               .count());
    if (second != gc_second) {
        gc_second = second;
        // bbport: the driver's budget for our process, once a second: near it the collector evicts
        // by submissions (as before 0.5) rather than keeping textures for seconds (NoteVramShort).
        if (instance.CanReportMemoryUsage() && !instance.IsIntegrated()) {
            const u64 budget = instance.GetDeviceMemoryBudgetNow();
            if (budget != 0 && total_used_memory >= budget / 100 * 92 &&
                vram_short_until.load(std::memory_order_relaxed) < second + 2) {
                vram_short_until.store(second + 2, std::memory_order_relaxed);
            }
        }
        BbStats::coarse_second.store(u32(second), std::memory_order_relaxed);
        gc_tick_at_second[second % gc_tick_at_second.size()] = gc_tick;
    }
    if (BbToggle::Disabled(BbToggle::TextureCollector)) {
        return;
    }
    if (total_used_memory < trigger_gc_memory) {
        return;
    }
    static const u64 idle_seconds = [] {
        const char* env = std::getenv("BB_GC_IDLE_SECONDS");
        return std::clamp<u64>(env ? std::strtoull(env, nullptr, 10) : 20, 1, 63);
    }();
    // Under pressure an image goes once unused for BB_GC_PRESSURE_IDLE_SECONDS (5), over the
    // critical mark for 1 s first, and only then by submissions as before (80/160 ticks, a
    // fraction of a second). On cards up to 8 GB the pressure mark is 40 % of the budget: the
    // usage stayed over it, so textures off screen for a moment were evicted and uploaded again
    // when the camera turned back (9000 a minute with a 4.5 GB budget), read over the bus each
    // time: NVIDIA testers saw the frame rate fall the longer they played.
    static const u64 pressure_idle_seconds = [] {
        const char* env = std::getenv("BB_GC_PRESSURE_IDLE_SECONDS");
        return std::clamp<u64>(env ? std::strtoull(env, nullptr, 10) : 5, 0, 63);
    }();
    // The tick at the start of that second (an older one where no submission came then).
    const auto tick_seconds_ago = [&](u64 seconds) {
        return gc_tick_at_second[(second - seconds) % gc_tick_at_second.size()];
    };
    const u64 idle_tick = tick_seconds_ago(idle_seconds);
    std::scoped_lock lock{mutex};
    bool pressured = false;
    bool aggresive = false;
    u64 below_tick = 0;
    size_t num_deletions = 0;
    u32 visited = 0;

    // emergency: over the critical mark after evicting what was unused for a second.
    const auto configure = [&](bool allow_aggressive, bool emergency = false) {
        pressured = total_used_memory >= pressure_gc_memory;
        aggresive = allow_aggressive && total_used_memory >= critical_gc_memory;
        const u64 ticks_to_destroy = std::min<u64>(aggresive ? 160 : pressured ? 80 : 16, gc_tick);
        below_tick = gc_tick - ticks_to_destroy;
        if (!pressured && !aggresive) {
            below_tick = std::min(below_tick, idle_tick);
        } else if (!emergency && second >= vram_short_until.load(std::memory_order_relaxed)) {
            const u64 seconds = aggresive ? 1 : pressure_idle_seconds;
            below_tick = std::min(below_tick, tick_seconds_ago(seconds));
        }
        num_deletions = aggresive ? 40 : pressured ? 20 : 10;
        visited = 0;
    };
    const auto clean_up = [&](ImageId image_id) {
        if (num_deletions == 0 || ++visited > 256) {
            return true;
        }
        auto& image = slot_images[image_id];
        const bool download = image.SafeToDownload();
        const bool tiled = image.info.IsTiled();
        // bbport: images that cannot go now (GPU-written: their contents exist only here, and
        // tiled ones cannot be written back at all) neither use up the deletions nor stay first
        // in line. The oldest ten being such render targets stopped the collector for good, and
        // VRAM grew with every area visited up to the pressure mark (~10 GB on 16 GB).
        if ((tiled && download) || (download && !pressured)) {
            lru_cache.Touch(image.lru_id, gc_tick);
            BbStats::gc_kept_images.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        --num_deletions;
        if (download) {
            // bbport: synchronously, while the image still protects its pages. A deferred
            // write-back landed after FreeImage had unprotected them, over whatever the game
            // had meanwhile stored there (e.g. its heap after unloading an area).
            DownloadImageMemory(image_id, true);
            ++gc_downloads;
        }
        ++gc_evictions;
        BbStats::gc_freed_images.fetch_add(1, std::memory_order_relaxed);
        FreeImage(image_id);
        if (total_used_memory < critical_gc_memory) {
            if (aggresive) {
                num_deletions >>= 2;
                aggresive = false;
                return false;
            }
            if (pressured && total_used_memory < pressure_gc_memory) {
                num_deletions >>= 1;
                pressured = false;
            }
        }
        return false;
    };

    // Try to remove anything old enough and not high priority.
    configure(false);
    lru_cache.ForEachItemBelow(below_tick, clean_up);

    if (total_used_memory >= critical_gc_memory) {
        // If we are still over the critical limit, run an aggressive GC
        configure(true);
        lru_cache.ForEachItemBelow(below_tick, clean_up);
    }
    if (total_used_memory >= critical_gc_memory) {
        // Still over it: what was used a moment ago goes too (out of memory otherwise).
        configure(true, true);
        lru_cache.ForEachItemBelow(below_tick, clean_up);
    }
    // bbport: evictions under memory pressure, at most every 5 s (BB_FRAME_STATS or not).
    if (pressured || gc_downloads != 0) {
        const auto now = std::chrono::steady_clock::now();
        if (now - gc_report_time >= std::chrono::seconds(5)) {
            std::printf("Texture cache: memory pressure, %llu of %llu MiB (critical %llu): "
                        "%llu images evicted, %llu written back since the last report\n",
                        (unsigned long long)(total_used_memory >> 20),
                        (unsigned long long)(pressure_gc_memory >> 20),
                        (unsigned long long)(critical_gc_memory >> 20),
                        (unsigned long long)gc_evictions, (unsigned long long)gc_downloads);
            gc_report_time = now;
            gc_evictions = gc_downloads = 0;
        }
    }
}

void TextureCache::GarbageCollectSamplers() {
    total_used_samplers = samplers.size();
    if (total_used_samplers < trigger_gc_samplers) {
        return;
    }
    std::scoped_lock lock{samplers_mutex};
    bool pressured = false;
    bool aggresive = false;
    u64 ticks_to_destroy = 0;
    size_t num_deletions = 0;

    const auto configure = [&](bool allow_aggressive) {
        pressured = total_used_samplers >= pressure_gc_samplers;
        aggresive = allow_aggressive && total_used_samplers >= critical_gc_samplers;
        ticks_to_destroy = aggresive ? 160 : pressured ? 80 : 16;
        ticks_to_destroy = std::min(ticks_to_destroy, gc_tick);
        num_deletions = aggresive ? 40 : pressured ? 20 : 10;
    };
    const auto clean_up = [&](u64 hash) {
        if (num_deletions == 0) {
            return true;
        }
        --num_deletions;
        const size_t lru_id = samplers.at(hash).lru_id;
        samplers.erase(hash);
        sampler_lru_cache.Free(lru_id);
        return false;
    };

    // Try to remove anything old enough and not high priority.
    configure(false);
    sampler_lru_cache.ForEachItemBelow(gc_tick - ticks_to_destroy, clean_up);

    if (total_used_samplers >= critical_gc_samplers) {
        // If we are still over the critical limit, run an aggressive GC
        configure(true);
        sampler_lru_cache.ForEachItemBelow(gc_tick - ticks_to_destroy, clean_up);
    }
}

void TextureCache::NoteVramShort() {
    const u64 now = u64(std::chrono::duration_cast<std::chrono::seconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count());
    vram_short_until.store(now + 30, std::memory_order_relaxed);
}

void TextureCache::RunGarbageCollector() {
    SCOPE_EXIT {
        ++gc_tick;
    };

    GarbageCollectImages();
    GarbageCollectSamplers();
}

void TextureCache::TouchImage(const Image& image) {
    if (image.lru_touched_tick == gc_tick) {
        return;
    }
    image.lru_touched_tick = gc_tick;
    lru_cache.Touch(image.lru_id, gc_tick);
}

void TextureCache::DeleteImage(ImageId image_id) {
    Image& image = slot_images[image_id];
    ASSERT_MSG(!image.IsTracked(), "Image was not untracked");
    ASSERT_MSG(False(image.flags & ImageFlagBits::Registered), "Image was not unregistered");

    // Remove any registered meta areas.
    const auto& meta_info = image.info.meta_info;
    if (meta_info.cmask_addr) {
        surface_metas.erase(meta_info.cmask_addr);
    }
    if (meta_info.fmask_addr) {
        surface_metas.erase(meta_info.fmask_addr);
    }
    if (meta_info.htile_addr) {
        surface_metas.erase(meta_info.htile_addr);
    }

    {
        std::unique_lock lk{download_images_mutex};
        if (download_images.contains(image_id)) {
            download_images.erase(image_id);
        }
    }

    // Reclaim image and any image views it references.
    scheduler.DeferOperation([this, image_id] {
        Image& image = slot_images[image_id];
        for (auto& backing : image.backing_images) {
            for (const ImageViewId image_view_id : backing.image_view_ids) {
                slot_image_views.erase(image_view_id);
            }
        }
        slot_images.erase(image_id);
    });
}

} // namespace VideoCore
