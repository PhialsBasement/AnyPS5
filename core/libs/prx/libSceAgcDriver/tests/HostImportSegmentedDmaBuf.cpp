#include "prx/libSceAgcDriver/Graphics/include/GuestBufferMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#ifndef _WIN32
#include "prx/libc/include/GuestArena.hpp"
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#endif
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
int main() {
    std::printf("skipped, udmabuf imports are Linux only\n");
    return 77;
}
#else
using namespace AgcDriver::Graphics;

namespace {

int failures = 0;

void Expect(bool condition, const std::string& what) {
    if (condition) return;
    std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    ++failures;
}

constexpr std::uint64_t PieceBytes = 65536;
constexpr std::uint64_t FilePieces = 8;

struct Imported {
    int file = -1;
    void* view = nullptr;
    std::uint64_t bytes = 0;
};

struct MockDevice {
    std::uintptr_t next = 0x1000;
    std::set<VkBuffer> buffers;
    std::map<VkBuffer, VkDeviceSize> sizes;
    std::map<VkDeviceMemory, Imported> memories;
    std::uint64_t nextAddress = 0x500000000ull;
};

MockDevice mock;

VKAPI_ATTR VkResult VKAPI_CALL mockGetMemoryHostPointerProperties(VkDevice, VkExternalMemoryHandleTypeFlagBits, const void*, VkMemoryHostPointerPropertiesEXT*) {
    return VK_ERROR_INVALID_EXTERNAL_HANDLE;
}

VKAPI_ATTR VkResult VKAPI_CALL mockGetMemoryFdProperties(VkDevice, VkExternalMemoryHandleTypeFlagBits, int, VkMemoryFdPropertiesKHR* properties) {
    properties->memoryTypeBits = 1;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL mockCreateBuffer(VkDevice, const VkBufferCreateInfo* info, const VkAllocationCallbacks*, VkBuffer* buffer) {
    *buffer = reinterpret_cast<VkBuffer>(mock.next++);
    mock.buffers.insert(*buffer);
    mock.sizes[*buffer] = info->size;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL mockDestroyBuffer(VkDevice, VkBuffer buffer, const VkAllocationCallbacks*) {
    mock.buffers.erase(buffer);
}

VKAPI_ATTR void VKAPI_CALL mockGetBufferMemoryRequirements(VkDevice, VkBuffer buffer, VkMemoryRequirements* requirements) {
    requirements->size = (mock.sizes.at(buffer) + PieceBytes - 1) / PieceBytes * PieceBytes;
    requirements->alignment = PieceBytes;
    requirements->memoryTypeBits = 1;
}

VKAPI_ATTR VkResult VKAPI_CALL mockAllocateMemory(VkDevice, const VkMemoryAllocateInfo* info, const VkAllocationCallbacks*, VkDeviceMemory* memory) {
    int file = -1;
    for (auto* chain = static_cast<const VkBaseInStructure*>(info->pNext); chain != nullptr; chain = chain->pNext) {
        if (chain->sType == VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR) file = reinterpret_cast<const VkImportMemoryFdInfoKHR*>(chain)->fd;
    }
    if (file < 0) return VK_ERROR_INVALID_EXTERNAL_HANDLE;
    void* view = mmap(nullptr, info->allocationSize, PROT_READ, MAP_SHARED, file, 0);
    if (view == MAP_FAILED) return VK_ERROR_INVALID_EXTERNAL_HANDLE;
    *memory = reinterpret_cast<VkDeviceMemory>(mock.next++);
    mock.memories[*memory] = {file, view, info->allocationSize};
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL mockFreeMemory(VkDevice, VkDeviceMemory memory, const VkAllocationCallbacks*) {
    const auto found = mock.memories.find(memory);
    if (found == mock.memories.end()) return;
    munmap(found->second.view, found->second.bytes);
    close(found->second.file);
    mock.memories.erase(found);
}

VKAPI_ATTR VkResult VKAPI_CALL mockBindBufferMemory(VkDevice, VkBuffer, VkDeviceMemory, VkDeviceSize) {
    return VK_SUCCESS;
}

VKAPI_ATTR VkDeviceAddress VKAPI_CALL mockGetBufferDeviceAddress(VkDevice, const VkBufferDeviceAddressInfo* info) {
    const auto address = mock.nextAddress;
    mock.nextAddress += (mock.sizes.at(info->buffer) + PieceBytes - 1) / PieceBytes * PieceBytes;
    return address;
}

PFN_vkVoidFunction VKAPI_CALL mockProc(VkDevice, const char* name) {
    static const std::map<std::string_view, PFN_vkVoidFunction> table{
        {"vkGetMemoryHostPointerPropertiesEXT", reinterpret_cast<PFN_vkVoidFunction>(mockGetMemoryHostPointerProperties)},
        {"vkGetMemoryFdPropertiesKHR", reinterpret_cast<PFN_vkVoidFunction>(mockGetMemoryFdProperties)},
        {"vkCreateBuffer", reinterpret_cast<PFN_vkVoidFunction>(mockCreateBuffer)},
        {"vkDestroyBuffer", reinterpret_cast<PFN_vkVoidFunction>(mockDestroyBuffer)},
        {"vkGetBufferMemoryRequirements", reinterpret_cast<PFN_vkVoidFunction>(mockGetBufferMemoryRequirements)},
        {"vkAllocateMemory", reinterpret_cast<PFN_vkVoidFunction>(mockAllocateMemory)},
        {"vkFreeMemory", reinterpret_cast<PFN_vkVoidFunction>(mockFreeMemory)},
        {"vkBindBufferMemory", reinterpret_cast<PFN_vkVoidFunction>(mockBindBufferMemory)},
        {"vkGetBufferDeviceAddressKHR", reinterpret_cast<PFN_vkVoidFunction>(mockGetBufferDeviceAddress)},
    };
    const auto it = table.find(name);
    return it == table.end() ? nullptr : it->second;
}

Context mockContext() {
    static const auto device = reinterpret_cast<VkDevice>(mock.next++);
    static const auto queue = reinterpret_cast<VkQueue>(mock.next++);
    Context context{};
    context.device = device;
    context.queue = queue;
    context.deviceProc = mockProc;
    context.hostImportAlignment = PieceBytes;
    context.dmaBufImport = true;
    return context;
}

struct Piece {
    std::uintptr_t address;
    std::uint64_t fileOffset;
    bool shared;
};

int backingFile = -1;
std::vector<Piece> pieces;

bool Resolve(std::uintptr_t address, std::size_t bytes, int* file, std::uint64_t* offset) {
    for (std::size_t first = 0; first < pieces.size(); ++first) {
        if (address < pieces[first].address || address >= pieces[first].address + PieceBytes) continue;
        if (!pieces[first].shared) return false;
        auto runEnd = pieces[first].address + PieceBytes;
        for (auto next = first + 1; next < pieces.size() && pieces[next].shared && pieces[next].address == runEnd && pieces[next].fileOffset == pieces[next - 1].fileOffset + PieceBytes; ++next) runEnd += PieceBytes;
        if (address + bytes > runEnd) return false;
        *file = dup(backingFile);
        *offset = pieces[first].fileOffset + (address - pieces[first].address);
        return *file >= 0;
    }
    return false;
}

struct ScatteredRange {
    void* reservation = nullptr;
    std::uint64_t reserved = 0;
    std::uintptr_t base = 0;
    std::uint64_t bytes = 0;

    ScatteredRange(const std::vector<std::uint64_t>& filePieces, std::size_t anonymousPiece) {
        bytes = filePieces.size() * PieceBytes;
        reserved = bytes + PieceBytes;
        reservation = mmap(nullptr, reserved, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (reservation == MAP_FAILED) throw std::runtime_error("cannot reserve the guest range");
        base = (reinterpret_cast<std::uintptr_t>(reservation) + PieceBytes - 1) / PieceBytes * PieceBytes;
        pieces.clear();
        for (std::size_t index = 0; index < filePieces.size(); ++index) {
            const auto address = base + index * PieceBytes;
            const bool shared = index != anonymousPiece;
            void* mapped = shared ? mmap(reinterpret_cast<void*>(address), PieceBytes, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, backingFile, static_cast<off_t>(filePieces[index] * PieceBytes))
                                  : mmap(reinterpret_cast<void*>(address), PieceBytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
            if (mapped == MAP_FAILED) throw std::runtime_error("cannot map a piece of the guest range");
            pieces.push_back({address, filePieces[index] * PieceBytes, shared});
        }
        auto* bytesAt = reinterpret_cast<std::uint8_t*>(base);
        for (std::uint64_t offset = 0; offset < bytes; ++offset) bytesAt[offset] = static_cast<std::uint8_t>((offset / PieceBytes) * 37u + offset % 251u);
        GuestAllocations::Mutation mutation;
        mutation.Add(reinterpret_cast<void*>(base), static_cast<std::size_t>(bytes), true, true, true);
    }

    ~ScatteredRange() {
        {
            GuestAllocations::Mutation mutation;
            mutation.Remove(reinterpret_cast<const void*>(base));
        }
        pieces.clear();
        munmap(reservation, reserved);
    }
};

const Imported* ImportedMemory(const HostImport& import) {
    const auto found = mock.memories.find(import.memory);
    return found == mock.memories.end() ? nullptr : &found->second;
}

void ImportsScatteredPiecesAsOneDmaBuf() {
    ScatteredRange range({3, 0, 5, 6, 1}, ~std::size_t{0});
    const auto context = mockContext();
    std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
    const auto* import = HostImportFor(context, range.base, static_cast<std::size_t>(range.bytes));
    Expect(import != nullptr && import->dmaBuf, "a range mapped from scattered pieces of its backing file was not imported as a dma-buf");
    if (import == nullptr) return;
    Expect(import->base == range.base && import->bytes == range.bytes, "the import does not cover exactly the scattered range");
    const auto* memory = ImportedMemory(*import);
    Expect(memory != nullptr && memory->bytes >= range.bytes, "the import's memory is not the whole dma-buf");
    if (memory == nullptr) return;
    Expect(std::memcmp(memory->view, reinterpret_cast<const void*>(range.base), static_cast<std::size_t>(range.bytes)) == 0, "the dma-buf does not hold the guest range's pieces in guest order");
    auto* guest = reinterpret_cast<std::uint8_t*>(range.base);
    guest[2 * PieceBytes + 17] = 0xa5;
    guest[4 * PieceBytes + PieceBytes - 1] = 0x5a;
    const auto* view = static_cast<const std::uint8_t*>(memory->view);
    Expect(view[2 * PieceBytes + 17] == 0xa5 && view[4 * PieceBytes + PieceBytes - 1] == 0x5a, "a guest store after the import is not seen through the dma-buf");
    ClearHostImports(context.device);
    Expect(mock.memories.empty(), "clearing the imports left the dma-buf memory alive");
}

void LeavesARangeWithAnAnonymousPieceUnimported() {
    ScatteredRange range({2, 7, 4}, 1);
    const auto context = mockContext();
    std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
    const auto* import = HostImportFor(context, range.base, static_cast<std::size_t>(range.bytes));
    Expect(import == nullptr, "a range with a piece outside the backing file was imported");
    Expect(mock.memories.empty(), "a refused range left dma-buf memory alive");
    ClearHostImports(context.device);
}

}

int main() {
    const int udmabuf = open("/dev/udmabuf", O_RDWR | O_CLOEXEC);
    if (udmabuf < 0) {
        std::printf("skipped, host imports are unavailable: /dev/udmabuf is not accessible\n");
        return 77;
    }
    close(udmabuf);
    backingFile = memfd_create("anyps5-segmented-import", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (backingFile < 0 || ftruncate(backingFile, static_cast<off_t>(FilePieces * PieceBytes)) != 0 || fcntl(backingFile, F_ADD_SEALS, F_SEAL_SHRINK) != 0) {
        std::fprintf(stderr, "cannot make the sealed backing file\n");
        return 1;
    }
    setenv("APS5_WRITE_WATCH_IMPORTS", "unwatch", 1);
    SetHostImportBudget(~std::uint64_t{0});
    GuestArena::GuestArenaSetSharedBacking_nid_postfix(Resolve);
    try {
        ImportsScatteredPiecesAsOneDmaBuf();
        LeavesARangeWithAnAnonymousPieceUnimported();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
    if (failures != 0) {
        std::fprintf(stderr, "%d segmented dma-buf import checks failed\n", failures);
        return 1;
    }
    std::printf("host import segmented dma-buf tests passed\n");
    return 0;
}
#endif
