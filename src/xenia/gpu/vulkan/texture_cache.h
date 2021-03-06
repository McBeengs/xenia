/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2016 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_VULKAN_TEXTURE_CACHE_H_
#define XENIA_GPU_VULKAN_TEXTURE_CACHE_H_

#include <unordered_map>

#include "xenia/gpu/register_file.h"
#include "xenia/gpu/sampler_info.h"
#include "xenia/gpu/shader.h"
#include "xenia/gpu/texture_info.h"
#include "xenia/gpu/trace_writer.h"
#include "xenia/gpu/vulkan/vulkan_command_processor.h"
#include "xenia/gpu/xenos.h"
#include "xenia/ui/vulkan/circular_buffer.h"
#include "xenia/ui/vulkan/vulkan.h"
#include "xenia/ui/vulkan/vulkan_device.h"

namespace xe {
namespace gpu {
namespace vulkan {

//
class TextureCache {
 public:
  struct TextureView;

  // This represents an uploaded Vulkan texture.
  struct Texture {
    TextureInfo texture_info;
    std::vector<std::unique_ptr<TextureView>> views;

    // True if we know all info about this texture, false otherwise.
    // (e.g. we resolve to system memory and may not know the full details about
    // this texture)
    bool is_full_texture;
    VkFormat format;
    VkImage image;
    VkImageLayout image_layout;
    VkDeviceMemory image_memory;
    VkDeviceSize memory_offset;
    VkDeviceSize memory_size;

    uintptr_t access_watch_handle;
    bool pending_invalidation;

    // Pointer to the latest usage fence.
    std::shared_ptr<ui::vulkan::Fence> in_flight_fence;
  };

  struct TextureView {
    Texture* texture;
    VkImageView view;

    union {
      struct {
        // FIXME: This only applies on little-endian platforms!
        uint16_t swiz_x : 3;
        uint16_t swiz_y : 3;
        uint16_t swiz_z : 3;
        uint16_t swiz_w : 3;
        uint16_t : 4;
      };

      uint16_t swizzle;
    };
  };

  TextureCache(Memory* memory, RegisterFile* register_file,
               TraceWriter* trace_writer, ui::vulkan::VulkanDevice* device);
  ~TextureCache();

  // Descriptor set layout containing all possible texture bindings.
  // The set contains one descriptor for each texture sampler [0-31].
  VkDescriptorSetLayout texture_descriptor_set_layout() const {
    return texture_descriptor_set_layout_;
  }

  // Prepares a descriptor set containing the samplers and images for all
  // bindings. The textures will be uploaded/converted/etc as needed.
  // Requires a fence to be provided that will be signaled when finished
  // using the returned descriptor set.
  VkDescriptorSet PrepareTextureSet(
      VkCommandBuffer setup_command_buffer,
      std::shared_ptr<ui::vulkan::Fence> completion_fence,
      const std::vector<Shader::TextureBinding>& vertex_bindings,
      const std::vector<Shader::TextureBinding>& pixel_bindings);

  // TODO(benvanik): UploadTexture.
  // TODO(benvanik): Resolve.
  // TODO(benvanik): ReadTexture.

  // Looks for a texture either containing or matching these parameters.
  // Caller is responsible for checking if the texture returned is an exact
  // match or just contains the texture given by the parameters.
  // If offset_x and offset_y are not null, this may return a texture that
  // contains this address at an offset.
  Texture* LookupAddress(uint32_t guest_address, uint32_t width,
                         uint32_t height, TextureFormat format,
                         VkOffset2D* out_offset = nullptr);

  // Demands a texture for the purpose of resolving from EDRAM. This either
  // creates a new texture or returns a previously created texture. texture_info
  // is not required to be completely filled out, just guest_address and all
  // sizes.
  //
  // It's possible that this may return an image that is larger than the
  // requested size (e.g. resolving into a bigger texture) or an image that
  // must have an offset applied. If so, the caller must handle this.
  // At the very least, it's guaranteed that the image will be large enough to
  // hold the requested size.
  Texture* DemandResolveTexture(const TextureInfo& texture_info,
                                TextureFormat format, VkOffset2D* out_offset);

  // Clears all cached content.
  void ClearCache();

  // Frees any unused resources
  void Scavenge();

 private:
  struct UpdateSetInfo;

  // Cached Vulkan sampler.
  struct Sampler {
    SamplerInfo sampler_info;
    VkSampler sampler;
  };

  // Allocates a new texture and memory to back it on the GPU.
  Texture* AllocateTexture(const TextureInfo& texture_info);
  bool FreeTexture(Texture* texture);

  // Demands a texture. If command_buffer is null and the texture hasn't been
  // uploaded to graphics memory already, we will return null and bail.
  Texture* Demand(
      const TextureInfo& texture_info, VkCommandBuffer command_buffer = nullptr,
      std::shared_ptr<ui::vulkan::Fence> completion_fence = nullptr);
  TextureView* DemandView(Texture* texture, uint16_t swizzle);
  Sampler* Demand(const SamplerInfo& sampler_info);

  // Queues commands to upload a texture from system memory, applying any
  // conversions necessary. This may flush the command buffer to the GPU if we
  // run out of staging memory.
  bool UploadTexture2D(VkCommandBuffer command_buffer,
                       std::shared_ptr<ui::vulkan::Fence> completion_fence,
                       Texture* dest, TextureInfo src);

  bool UploadTextureCube(VkCommandBuffer command_buffer,
                         std::shared_ptr<ui::vulkan::Fence> completion_fence,
                         Texture* dest, TextureInfo src);

  bool SetupTextureBindings(
      VkCommandBuffer command_buffer,
      std::shared_ptr<ui::vulkan::Fence> completion_fence,
      UpdateSetInfo* update_set_info,
      const std::vector<Shader::TextureBinding>& bindings);
  bool SetupTextureBinding(VkCommandBuffer command_buffer,
                           std::shared_ptr<ui::vulkan::Fence> completion_fence,
                           UpdateSetInfo* update_set_info,
                           const Shader::TextureBinding& binding);

  Memory* memory_ = nullptr;

  RegisterFile* register_file_ = nullptr;
  TraceWriter* trace_writer_ = nullptr;
  ui::vulkan::VulkanDevice* device_ = nullptr;

  VkDescriptorPool descriptor_pool_ = nullptr;
  VkDescriptorSetLayout texture_descriptor_set_layout_ = nullptr;
  std::list<std::pair<VkDescriptorSet, std::shared_ptr<ui::vulkan::Fence>>>
      in_flight_sets_;

  ui::vulkan::CircularBuffer staging_buffer_;
  std::unordered_map<uint64_t, Texture*> textures_;
  std::unordered_map<uint64_t, Sampler*> samplers_;
  std::vector<Texture*> resolve_textures_;
  std::list<Texture*> pending_delete_textures_;

  std::mutex invalidated_textures_mutex_;
  std::vector<Texture*>* invalidated_textures_;
  std::vector<Texture*> invalidated_textures_sets_[2];

  std::mutex invalidated_resolve_textures_mutex_;
  std::vector<Texture*> invalidated_resolve_textures_;

  struct UpdateSetInfo {
    // Bitmap of all 32 fetch constants and whether they have been setup yet.
    // This prevents duplication across the vertex and pixel shader.
    uint32_t has_setup_fetch_mask;
    uint32_t image_write_count = 0;
    VkWriteDescriptorSet image_writes[32];
    VkDescriptorImageInfo image_infos[32];
  } update_set_info_;
};

}  // namespace vulkan
}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_VULKAN_TEXTURE_CACHE_H_
