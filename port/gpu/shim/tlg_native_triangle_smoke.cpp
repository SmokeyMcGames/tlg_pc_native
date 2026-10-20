// SPDX-License-Identifier: GPL-2.0-or-later
#include "tlg_native_triangle_smoke.h"
#include "tlg_native_device_resources.h"
#include "tlg_native_vk_draw.h"
#include "video_core/host_shaders/fs_tri_vert.h"
#include "video_core/host_shaders/native_triangle_frag.h"
#include "video_core/host_shaders/native_textured_frag.h"
#include <span>
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include <vk_mem_alloc.h>
#include <array>
#include <cstdio>
#include <cstdint>
#include <cstring>

namespace TlgNativeVk {
namespace {
constexpr std::uint64_t SmokeTargetId = 0xfffffffffffe1001ull;
constexpr std::uint64_t SmokePipelineId = 0xfffffffffffe1002ull;
constexpr std::uint64_t SmokeIndexId = 0xfffffffffffe1003ull;
constexpr std::uint64_t SmokeContextId = 0xfffffffffffe1004ull;
constexpr std::uint64_t SmokeTextureId = 0xfffffffffffe1005ull;
constexpr std::uint64_t SmokeSamplerId = 0xfffffffffffe1006ull;
constexpr vk::Extent2D SmokeSize{64, 64};
}
struct TriangleSmoke::State {
    const Vulkan::Instance& instance;
    Vulkan::Scheduler& scheduler;
    DeviceResources& resources;
    ResourceRegistry& registry;
    bool textured = false;
    bool tiled = false; // exercises verified PS4 1D thin detile before Vulkan upload
    bool macro = false; // exercises verified 2D macro tile bank/pipe permutation
    vk::ImageView color_view{};
    VkImage color_image = VK_NULL_HANDLE;
    VkBuffer readback_buffer = VK_NULL_HANDLE;
    VmaAllocation readback_allocation = VK_NULL_HANDLE;
    void* readback_mapped = nullptr;
    bool target_created = false;
    bool initialized = false;
    bool emitted = false;
};

TriangleSmoke::TriangleSmoke(const Vulkan::Instance& instance,
                              Vulkan::Scheduler& scheduler, DeviceResources& resources,
                              ResourceRegistry& registry, bool texture_test,
                              bool tiled_texture_test, bool macro_texture_test)
    : state_{std::make_unique<State>(
          instance, scheduler, resources, registry,
          texture_test || tiled_texture_test || macro_texture_test,
          tiled_texture_test && !macro_texture_test, macro_texture_test)} {
TriangleSmoke::~TriangleSmoke() {
    if (!state_->target_created) return;
    // A deferred Scheduler::Record() closure may not have reached the GPU yet.
    // Finish recording and wait for execution before destroying pipeline/image.
    // This only happens for the opt-in native smoke at teardown.
    state_->scheduler.EndRendering();
    state_->scheduler.Finish();
    state_->registry.Unbind(SmokeContextId);
    state_->resources.RetireGraphicsPipeline(SmokePipelineId);
    state_->resources.RetireIndexBuffer(SmokeIndexId);
    if (state_->readback_buffer != VK_NULL_HANDLE) {
        vmaDestroyBuffer(state_->instance.GetAllocator(), state_->readback_buffer,
                         state_->readback_allocation);
        state_->readback_buffer = VK_NULL_HANDLE;
        state_->readback_allocation = VK_NULL_HANDLE;
        state_->readback_mapped = nullptr;
    }
    if (state_->textured) {
        state_->resources.RetireSampledTexture(SmokeTextureId);
        state_->resources.RetireSampler(SmokeSamplerId);
    }
    state_->resources.RetireColorTarget(SmokeTargetId);
    state_->resources.CollectRetiredAfterIdle();
}

bool TriangleSmoke::Init() {
    if (state_->initialized) return true;
    if (!state_->resources.CreateColorTarget(SmokeTargetId, SmokeSize.width,
                                               SmokeSize.height,
                                               VK_FORMAT_R8G8B8A8_UNORM))
        return false;
    state_->target_created = true;
    DeviceResources::ColorTargetInfo target{};
    if (!state_->resources.QueryColorTarget(SmokeTargetId, target)) return false;
    VkImageView raw_view = VK_NULL_HANDLE;
    static_assert(sizeof(raw_view) == sizeof(target.view));
    std::memcpy(&raw_view, &target.view, sizeof(raw_view));
    state_->color_view = vk::ImageView{raw_view};
    static_assert(sizeof(state_->color_image) == sizeof(target.image));
    std::memcpy(&state_->color_image, &target.image, sizeof(state_->color_image));

    // Real GPU -> CPU verification: a host-visible readback buffer is filled
    // only via vkCmdCopyImageToBuffer after the native triangle has rendered.
    constexpr VkDeviceSize readback_size = SmokeSize.width * SmokeSize.height * 4u;
    const VkBufferCreateInfo bi{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = readback_size,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    const VmaAllocationCreateInfo ai{
        .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
                 VMA_ALLOCATION_CREATE_MAPPED_BIT |
                 VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
    };
    VmaAllocationInfo allocation_info{};
    if (vmaCreateBuffer(state_->instance.GetAllocator(), &bi, &ai,
                        &state_->readback_buffer, &state_->readback_allocation,
                        &allocation_info) != VK_SUCCESS)
        return false;
    state_->readback_mapped = allocation_info.pMappedData;
    if (!state_->readback_mapped) return false;
    // The readback buffer is deliberately initialized to a diagnostic pattern.
    std::memset(state_->readback_mapped, 0xa5, static_cast<std::size_t>(readback_size));
    if (vmaFlushAllocation(state_->instance.GetAllocator(),
                           state_->readback_allocation, 0, readback_size) != VK_SUCCESS)
        return false;

    // Independent native Vulkan texture proof, never touching a game asset.
    // The tiled variant first exercises the exact verified 1D thin microtile
    // permutation, then independently binds the resulting VkImageView.
    if (state_->textured) {
        bool uploaded = false;
        if (state_->macro) {
            // A synthetic 128x64 pixel PS4 macro tile with independently
            // varied R/G channels, bank swizzle 7. All 8192 32-bit pixels
            // are explicitly laid out using the same address constraints
            // that the detiler validates before Vulkan upload.
            constexpr std::uint32_t pitch = 128, height = 64, swizzle = 7;
            std::array<std::uint8_t, pitch * height * 4u> macro_pixels{};
            MacroRgba8Layout macro_layout{};
            const bool valid = Validate2DMacroRgba8(
                pitch, height, pitch, height, swizzle, macro_pixels.size(),
                macro_pixels.data(), Rgba8MacroTiling::Thin2DThin, macro_layout);
            if (!valid)
                return false;
            for (std::uint32_t y = 0; y < height; ++y) {
                for (std::uint32_t x = 0; x < pitch; ++x) {
                    const auto offset = MacroPixelByteOffsetRgba8(x, y, macro_layout);
                    macro_pixels[offset] = static_cast<std::uint8_t>(x * 3u);
                    macro_pixels[offset + 1] = static_cast<std::uint8_t>(y * 5u);
                    macro_pixels[offset + 2] = 96;
                    macro_pixels[offset + 3] = 255;
                }
            }
            uploaded = state_->resources.CreateSampledTexture2DMacroRgba8(
                SmokeTextureId, pitch, height, pitch, height, swizzle,
                Rgba8MacroTiling::Thin2DThin,
                macro_pixels.data(), macro_pixels.size());
        } else if (state_->tiled) {
            std::array<std::uint8_t, 8u * 8u * 4u> tiled_pixels{};
            for (std::uint32_t y = 0; y < 8; ++y) {
                for (std::uint32_t x = 0; x < 8; ++x) {
                    const auto pixel = PixelInMicroTile1DRgba8(
                        x, y, Rgba8Tiling::Thin1DThin) * 4u;
                    tiled_pixels[pixel] = static_cast<std::uint8_t>(x * 31u);
                    tiled_pixels[pixel + 1] = static_cast<std::uint8_t>(y * 31u);
                    tiled_pixels[pixel + 2] =
                        static_cast<std::uint8_t>((x + y) * 14u);
                    tiled_pixels[pixel + 3] = 255;
                }
            }
            uploaded = state_->resources.CreateSampledTexture1DTiledRgba8(
                SmokeTextureId, 8, 8, 8, 8, Rgba8Tiling::Thin1DThin,
                tiled_pixels.data(), tiled_pixels.size());
        } else {
            std::array<std::uint8_t, 4u * 4u * 4u> pixels{};
            for (std::size_t i = 0; i < pixels.size(); i += 4) {
                pixels[i] = 16;
                pixels[i + 1] = 64;
                pixels[i + 2] = 128;
                pixels[i + 3] = 255;
            }
            uploaded = state_->resources.CreateSampledTextureRGBA8(
                SmokeTextureId, 4, 4, pixels.data(), 16, pixels.size());
        }
        if (!uploaded ||
            !state_->resources.CreateNearestClampSampler(SmokeSamplerId))
            return false;
    }
    // Build actual standalone Vulkan pipeline through independent native
    // resource owner, without Gnm/PM4 shader state or pipeline cache.
    // The textured variant has TWO explicit Vulkan descriptor bindings.
    constexpr std::array descriptors{
        NativeDescriptorBinding{
            .binding = 0, .descriptor_count = 1,
            .kind = NativeDescriptorKind::SampledImage,
        },
        NativeDescriptorBinding{
            .binding = 1, .descriptor_count = 1,
            .kind = NativeDescriptorKind::Sampler,
        },
    };
    const bool compiled = state_->textured
        ? state_->resources.CreateGraphicsPipeline(
              SmokePipelineId, FS_TRI_VERT, NATIVE_TEXTURED_FRAG,
              VK_FORMAT_R8G8B8A8_UNORM, descriptors)
        : state_->resources.CreateGraphicsPipeline(
              SmokePipelineId, FS_TRI_VERT, NATIVE_TRIANGLE_FRAG,
              VK_FORMAT_R8G8B8A8_UNORM);
    if (!compiled)
        return false;
    // A real native index buffer participates in the same resource registry
    // and GPU read barriers that game-level indexed draws will need.
    constexpr std::uint16_t indices[]{0, 1, 2};
    if (!state_->resources.CreateIndexBuffer(SmokeIndexId, 3, 2,
                                              indices, sizeof(indices)))
        return false;
    state_->initialized = true;
    return true;
}

bool TriangleSmoke::RecordOnce() {
    if (!state_->initialized || state_->emitted) return false;
    // Vulkan forbids changing an attachment image's layout while another
    // dynamic rendering instance is active. End the guest's current pass first.
    state_->scheduler.EndRendering();
    if (state_->textured &&
        !state_->resources.RecordSampledTextureUpload(
            SmokeTextureId, state_->scheduler))
        return false;
    if (!state_->resources.RecordColorAttachmentTransition(SmokeTargetId, state_->scheduler) ||
        !state_->resources.RecordIndexReadBarrier(SmokeIndexId, state_->scheduler))
        return false;

    // End-to-end semantic Js draw -> native Vulkan registry -> VkDrawIndexed.
    // No Gnm or PM4 code participates in these draw arguments or bindings.
    if (!state_->registry.Bind(SmokeContextId, SmokePipelineId, SmokeIndexId,
                               SmokeTargetId, 0, 0) ||
        !state_->registry.AcknowledgeBarriers(SmokeContextId, 1))
        return false;
    const TlgJsNativeCommand semantic{
        .ready = 1, .kind = TLG_JS_NATIVE_INDEXED,
        .context = SmokeContextId,
        .arg0 = 3, .arg1 = 0, .arg2 = 0,
    };
    TlgJsNativeDrawCandidate requested{};
    TlgNativeVk::NativeBoundDraw resolved{};
    if (!tlg_js_native_decode_draw(&semantic, &requested) ||
        !state_->registry.TryResolve(requested, resolved))
        return false;
    VkPipeline raw_pipeline = VK_NULL_HANDLE;
    VkBuffer raw_indices = VK_NULL_HANDLE;
    static_assert(sizeof(raw_pipeline) == sizeof(resolved.pipeline));
    static_assert(sizeof(raw_indices) == sizeof(resolved.index_buffer));
    std::memcpy(&raw_pipeline, &resolved.pipeline, sizeof(raw_pipeline));
    std::memcpy(&raw_indices, &resolved.index_buffer, sizeof(raw_indices));
    const vk::Pipeline pipeline{raw_pipeline};
    const vk::Buffer indices{raw_indices};
    const TlgNativeVk::Draw draw = resolved.command;
    // Exercise the same native descriptor validation used by future
    // high-level Js draw bindings, instead of a smoke-only hardcoded Vk
    // descriptor writer. Only exact registered generations can be submitted.
    if (state_->textured) {
        DeviceResources::SampledImageInfo sampled{};
        if (!state_->resources.QuerySampledBinding(
                SmokeTextureId, SmokeSamplerId, sampled))
            return false;
        const std::array<NativeGuestDescriptorBinding, 2> assignments{{
            NativeGuestDescriptorBinding{
                .binding = 0, .kind = NativeDescriptorKind::SampledImage,
                .resources = {{SmokeTextureId, sampled.image_generation}},
            },
            NativeGuestDescriptorBinding{
                .binding = 1, .kind = NativeDescriptorKind::Sampler,
                .resources = {{SmokeSamplerId, sampled.sampler_generation}},
            },
        }};
        if (!state_->resources.RecordNativeImageDescriptors(
                SmokePipelineId, NativePipelineKind::Graphics,
                assignments, state_->scheduler))
            return false;
    }
    const auto color = state_->color_view;
    const auto native_image = vk::Image{state_->color_image};
    const auto staging = vk::Buffer{state_->readback_buffer};
    state_->scheduler.Record([pipeline,indices,draw,color,native_image,staging]
                              (vk::CommandBuffer cmd) {
        const vk::RenderingAttachmentInfo attachment{
            .imageView=color,
            .imageLayout=vk::ImageLayout::eColorAttachmentOptimal,
            .loadOp=vk::AttachmentLoadOp::eClear,
            .storeOp=vk::AttachmentStoreOp::eStore,
            .clearValue=vk::ClearValue{
                .color = {.float32 = std::array<float,4>{0.0f,0.0f,0.0f,1.0f}},
            },
        };
        const vk::RenderingInfo pass{
            .renderArea={.extent=SmokeSize},
            .layerCount=1,
            .colorAttachmentCount=1,
            .pColorAttachments=&attachment,
        };
        cmd.beginRendering(pass);
        cmd.bindIndexBuffer(indices, 0, vk::IndexType::eUint16);
        cmd.setViewport(0, vk::Viewport{
            .width=float(SmokeSize.width),.height=float(SmokeSize.height),
            .minDepth=0.0f,.maxDepth=1.0f,
        });
        cmd.setScissor(0, vk::Rect2D{.extent=SmokeSize});
        TlgNativeVk::EmitGraphicsDraw(cmd, vk::PipelineBindPoint::eGraphics,
                                      pipeline, vk::Pipeline{}, draw);
        cmd.endRendering();

        // Native Vulkan Sync2 image transition: color-write -> copy source.
        const vk::ImageMemoryBarrier2 to_transfer{
            .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
            .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
            .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
            .oldLayout = vk::ImageLayout::eColorAttachmentOptimal,
            .newLayout = vk::ImageLayout::eTransferSrcOptimal,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = native_image,
            .subresourceRange = {
                .aspectMask = vk::ImageAspectFlagBits::eColor,
                .baseMipLevel = 0, .levelCount = 1,
                .baseArrayLayer = 0, .layerCount = 1,
            },
        };
        const vk::DependencyInfo image_dependency{
            .imageMemoryBarrierCount = 1,
            .pImageMemoryBarriers = &to_transfer,
        };
        cmd.pipelineBarrier2(image_dependency);

        const vk::BufferImageCopy copy{
            .bufferOffset = 0,
            .bufferRowLength = 0,
            .bufferImageHeight = 0,
            .imageSubresource = {
                .aspectMask = vk::ImageAspectFlagBits::eColor,
                .mipLevel = 0, .baseArrayLayer = 0, .layerCount = 1,
            },
            .imageOffset = {0, 0, 0},
            .imageExtent = {SmokeSize.width, SmokeSize.height, 1},
        };
        cmd.copyImageToBuffer(native_image, vk::ImageLayout::eTransferSrcOptimal,
                              staging, copy);

        // Native Vulkan Sync2 buffer barrier: transfer-write -> host-read.
        const vk::BufferMemoryBarrier2 to_cpu{
            .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
            .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eHost,
            .dstAccessMask = vk::AccessFlagBits2::eHostRead,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = staging,
            .offset = 0,
            .size = SmokeSize.width * SmokeSize.height * 4u,
        };
        const vk::DependencyInfo cpu_dependency{
            .bufferMemoryBarrierCount = 1,
            .pBufferMemoryBarriers = &to_cpu,
        };
        cmd.pipelineBarrier2(cpu_dependency);
    });

    state_->emitted = true; // never accidentally issue the smoke twice
    // One-shot GPU readback is intentionally synchronous and opt-in. Submit
    // the command before inspecting the mapped staging allocation.
    state_->scheduler.Finish();
    if (vmaInvalidateAllocation(state_->instance.GetAllocator(),
                                state_->readback_allocation, 0, VK_WHOLE_SIZE) != VK_SUCCESS) {
        std::puts("Native Vulkan triangle: readback memory invalidation FAILED");
        return false;
    }
    const auto* rgba = static_cast<const std::uint8_t*>(state_->readback_mapped);
    const auto middle = (SmokeSize.height / 2u * SmokeSize.width +
                         SmokeSize.width / 2u) * 4u;
    const std::uint8_t r = rgba[middle], g = rgba[middle + 1];
    const std::uint8_t b = rgba[middle + 2], a = rgba[middle + 3];
    // fs_tri.vert supplies UV ~0.5 at the center; native_triangle.frag
    // outputs (uv.x, uv.y, 0.25, 1). Tolerance allows rasterization rounding.
    const bool pass = state_->macro
        ? (r >= 180 && r <= 205 && g >= 145 && g <= 175 &&
           b >= 90 && b <= 105 && a >= 250)
        : state_->tiled
            ? (r >= 80 && r <= 140 && g >= 80 && g <= 140 &&
               b >= 70 && b <= 135 && a >= 250)
        : state_->textured
            ? (r >= 10 && r <= 22 && g >= 58 && g <= 70 &&
               b >= 122 && b <= 134 && a >= 250)
            : (r >= 90 && r <= 165 && g >= 90 && g <= 165 &&
               b >= 50 && b <= 85 && a >= 250);
    std::printf("Native Vulkan %s triangle GPU readback: %s "
                "(center RGBA %u,%u,%u,%u; expected %s)\n",
                state_->macro ? "2D-macro sampled-texture"
                    : state_->tiled ? "1D-tiled sampled-texture"
                    : state_->textured ? "sampled-texture" : "registry-indexed",
                pass ? "PASS" : "FAIL", unsigned(r), unsigned(g),
                unsigned(b), unsigned(a),
                state_->macro ? "center ~192,160,96,255"
                    : state_->tiled ? "center ~109,109,98,255"
                    : state_->textured ? "16,64,128,255" : "128,128,64,255");
    return pass;
}
} // namespace TlgNativeVk
