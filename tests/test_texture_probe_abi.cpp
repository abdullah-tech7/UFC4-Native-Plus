// SPDX-License-Identifier: GPL-2.0-or-later
// Independent C++ host methods: verify the C extension really matches the
// donor's member-function/sret/span ABI, not just a C-to-C mock signature.
#include <cassert>
#include <cstdint>
#include <span>
namespace vk {
struct BufferImageCopy {
    uint64_t offset;
    uint32_t row,height,aspect,mip,layer,layers;
    int32_t x,y,z;
    uint32_t w,h,d;
};
}
namespace VideoCore {
enum class MemoryType:uint8_t { DeviceLocal,HostUncached,HostCached };
struct Image{};
struct Buffer{};
}
namespace Vulkan {
struct StagingBufferRef {
    VideoCore::Buffer *buffer;
    uint64_t offset,size;
    uint8_t *mapped;
    VideoCore::MemoryType type;
};
static_assert(sizeof(StagingBufferRef)==40 && sizeof(vk::BufferImageCopy)==56);
class StagingBufferPool {
public:StagingBufferRef Request(uint64_t,VideoCore::MemoryType,uint64_t,bool);
};
StagingBufferRef StagingBufferPool::Request(uint64_t size,VideoCore::MemoryType type,uint64_t align,bool deferred) {
    assert(reinterpret_cast<uintptr_t>(this)==0x12345678 && size==688128);
    assert(type==VideoCore::MemoryType::HostCached && align==16 && !deferred);
    return {reinterpret_cast<VideoCore::Buffer*>(0x76543210),32,size,
            reinterpret_cast<uint8_t*>(0x45678900),type};
}
class Runtime {
public:void DownloadImage(VideoCore::Image*,const VideoCore::Buffer*,std::span<const vk::BufferImageCopy>);
};
void Runtime::DownloadImage(VideoCore::Image *image,const VideoCore::Buffer *buffer,std::span<const vk::BufferImageCopy> copies) {
    assert(reinterpret_cast<uintptr_t>(this)==0x11111111);
    assert(reinterpret_cast<uintptr_t>(image)==0x22222222);
    assert(reinterpret_cast<uintptr_t>(buffer)==0x76543210);
    assert(copies.size()==2 && copies[0].offset==32 && copies[1].offset==524320);
    assert(copies[0].w==1024 && copies[1].mip==1 && copies[1].h==256);
}
}
