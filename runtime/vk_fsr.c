// AMD FidelityFX Super Resolution 1 on the present path (gfx_present.c present_image): when the window is larger than
// the game's output, EASU scales the game's crop rectangle to the swapchain size and RCAS sharpens it, in place of the
// bilinear vkCmdBlitImage. TOTK_UPSCALER=fsr|bilinear picks it, TOTK_SHARPNESS (0..100, 0 = no RCAS pass) sets RCAS;
// both apply at the next frame.
//
// Per frame: EASU reads the source through a view of its own format (the same colour conversion the blit does) and
// writes an RGBA16F image of the picture's size on the swapchain (all of it, or the part between black bars,
// gfx_window.c fit_rect); RCAS reads that and writes a second one; a 1:1 blit converts the result into its place in
// the swapchain image, which the caller left in TRANSFER_DST. The shaders (runtime/shaders/fsr_*.comp)
// compute AMD's constants from the push constants. Cost: two compute passes over the swapchain size and one copy per
// present; nothing on the game's draws.
//
// The source view of slot s is destroyed when slot s records again: present_image has waited for that slot's fence.
// The two intermediate images are shared by all slots; each frame's first barrier orders it after the previous
// frame's reads (one queue, so the barrier covers earlier submissions).
#include "vk.h"

#include <math.h>
#include <string.h>

#include "cfg.h"
#include "fsr_easu_spv.h"
#include "fsr_rcas_spv.h"
#include "host.h"
#include "smaa_blend_spv.h"
#include "smaa_edge_spv.h"
#include "smaa_neighborhood_spv.h"

#define SLOTS 8

static struct {
    int tried, ok;
    PFN_vkCmdPushDescriptorSetKHR push;
    VkDescriptorSetLayout dsl;
    VkPipelineLayout layout;
    VkPipeline easu, rcas;
    VkSampler sampler;
    VkImage img[2];                // EASU output, RCAS output
    VmaAllocation alloc[2];
    VkImageView view[2];
    uint32_t w, h;                 // their size
    VkImageView src_view[SLOTS];   // the source view each slot's last frame read
    int logged;
} F;

static VkPipeline make_pipeline(VkCtx *c, const uint32_t *code, size_t size) {
    VkShaderModuleCreateInfo mi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = size, .pCode = code};
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreateShaderModule(c->device, &mi, NULL, &m) != VK_SUCCESS) return VK_NULL_HANDLE;
    VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = m, .pName = "main"},
                                      .layout = F.layout};
    VkPipeline p = VK_NULL_HANDLE;
    if (vkCreateComputePipelines(c->device, VK_NULL_HANDLE, 1, &ci, NULL, &p) != VK_SUCCESS) p = VK_NULL_HANDLE;
    vkDestroyShaderModule(c->device, m, NULL);
    return p;
}

static int init(VkCtx *c) {
    F.tried = 1;
    F.push = (PFN_vkCmdPushDescriptorSetKHR)vkGetDeviceProcAddr(c->device, "vkCmdPushDescriptorSetKHR");
    if (!F.push) {
        rt_log("vk: FSR off: no VK_KHR_push_descriptor; the present scales bilinearly");
        return 0;
    }
    VkDescriptorSetLayoutBinding b[2] = {
        {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
        {.binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT}};
    VkDescriptorSetLayoutCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                           .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR,
                                           .bindingCount = 2, .pBindings = b};
    VkPushConstantRange pr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 32};
    VkPipelineLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                     .pSetLayouts = &F.dsl, .pushConstantRangeCount = 1, .pPushConstantRanges = &pr};
    VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_LINEAR,
                              .minFilter = VK_FILTER_LINEAR, .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                              .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .maxLod = 0};
    if (vkCreateDescriptorSetLayout(c->device, &dci, NULL, &F.dsl) != VK_SUCCESS ||
        vkCreatePipelineLayout(c->device, &li, NULL, &F.layout) != VK_SUCCESS ||
        vkCreateSampler(c->device, &si, NULL, &F.sampler) != VK_SUCCESS ||
        !(F.easu = make_pipeline(c, fsr_easu_spv, sizeof fsr_easu_spv)) ||
        !(F.rcas = make_pipeline(c, fsr_rcas_spv, sizeof fsr_rcas_spv))) {
        rt_log("vk: FSR off: its pipelines could not be created; the present scales bilinearly");
        return 0;
    }
    rt_log("vk: FSR 1 (EASU + RCAS) ready for the present");
    return F.ok = 1;
}

static void free_images(VkCtx *c) {
    for (int i = 0; i < 2; i++) {
        if (F.view[i]) vkDestroyImageView(c->device, F.view[i], NULL);
        if (F.img[i]) vmaDestroyImage(c->vma, F.img[i], F.alloc[i]);
        F.view[i] = VK_NULL_HANDLE;
        F.img[i] = VK_NULL_HANDLE;
    }
    F.w = F.h = 0;
}

static int ensure_images(VkCtx *c, uint32_t w, uint32_t h) {
    if (F.img[0] && F.w == w && F.h == h) return 1;
    // A swapchain size change: earlier presents may still read the old images. vkDeviceWaitIdle needs every queue
    // externally synchronized, so it holds the queue lock like a submission.
    vk_queue_lock();
    vkDeviceWaitIdle(c->device);
    vk_queue_unlock();
    free_images(c);
    for (int i = 0; i < 2; i++) {
        VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                                .format = VK_FORMAT_R16G16B16A16_SFLOAT, .extent = {w, h, 1}, .mipLevels = 1,
                                .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                                .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                         VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        if (vmaCreateImage(c->vma, &ci, &ai, &F.img[i], &F.alloc[i], NULL) != VK_SUCCESS) {
            F.img[i] = VK_NULL_HANDLE;
            free_images(c);
            return 0;
        }
        VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = F.img[i],
                                    .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R16G16B16A16_SFLOAT,
                                    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        if (vkCreateImageView(c->device, &vi, NULL, &F.view[i]) != VK_SUCCESS) {
            F.view[i] = VK_NULL_HANDLE;
            free_images(c);
            return 0;
        }
    }
    F.w = w;
    F.h = h;
    return 1;
}

static void barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to, VkPipelineStageFlags2 ss,
                    VkAccessFlags2 sa, VkPipelineStageFlags2 ds, VkAccessFlags2 da) {
    VkImageMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, .srcStageMask = ss, .srcAccessMask = sa,
                               .dstStageMask = ds, .dstAccessMask = da, .oldLayout = from, .newLayout = to,
                               .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                               .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = img,
                               .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkDependencyInfo d = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1,
                          .pImageMemoryBarriers = &b};
    vkCmdPipelineBarrier2(cb, &d);
}

static void bind_pass(VkCommandBuffer cb, VkPipeline p, VkImageView in, VkImageView out, const float pc[8]) {
    VkDescriptorImageInfo ii = {F.sampler, in, VK_IMAGE_LAYOUT_GENERAL}, oi = {VK_NULL_HANDLE, out, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet w[2] = {
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 0, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 1, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &oi}};
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, p);
    rec_push(F.push, cb, VK_PIPELINE_BIND_POINT_COMPUTE, F.layout, 0, 2, w);
    vkCmdPushConstants(cb, F.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 32, pc);
}

int vk_fsr_record(VkCommandBuffer cb, unsigned slot, VkImage src, VkFormat fmt, int sw, int sh, const int crop[4],
                  VkImage dst, const int out[4]) {
    int cw = crop[2], ch = crop[3];
    uint32_t dw = (uint32_t)out[2], dh = (uint32_t)out[3];
    if (!fmt || cw <= 0 || ch <= 0 || dw <= 0 || dh <= 0) return 0;

    int sharp = cfg_int("NS2_SHARPNESS");
    if (sharp < 0) sharp = 0;
    if (sharp > 100) sharp = 100;

    // When output is strictly smaller than input (downsampling), or 1:1 with sharpness off, bypass FSR
    if (dw < (uint32_t)cw || dh < (uint32_t)ch || (dw == (uint32_t)cw && dh == (uint32_t)ch && sharp <= 0)) return 0;

    VkCtx *c = vk_ctx();
    if (!F.tried) init(c);
    if (!F.ok || !ensure_images(c, dw, dh)) return 0;
    slot %= SLOTS;
    if (F.src_view[slot]) vkDestroyImageView(c->device, F.src_view[slot], NULL);
    VkImageViewUsageCreateInfo use = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO,
                                      .usage = VK_IMAGE_USAGE_SAMPLED_BIT};
    VkFormat vfmt = (fmt == VK_FORMAT_R8G8B8A8_SRGB) ? VK_FORMAT_R8G8B8A8_UNORM
                  : (fmt == VK_FORMAT_B8G8R8A8_SRGB) ? VK_FORMAT_B8G8R8A8_UNORM
                                                     : fmt;
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .pNext = &use, .image = src,
                                .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = vfmt,
                                .components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                                               VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_ONE},
                                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    if (vkCreateImageView(c->device, &vi, NULL, &F.src_view[slot]) != VK_SUCCESS) {
        F.src_view[slot] = VK_NULL_HANDLE;
        return 0;
    }
    if (!F.logged) {
        F.logged = 1;
        rt_log("vk: FSR %s %dx%d to %ux%u (sharpness %d)",
               (dw == (uint32_t)cw && dh == (uint32_t)ch) ? "RCAS native sharpen" : "upscales",
               cw, ch, dw, dh, sharp);
    }
    const VkPipelineStageFlags2 CS = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, XF = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    // the game's writes to the source become visible to the compute read; the earlier frame's reads of the
    // intermediates finish before this frame writes them
    barrier(cb, src, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            VK_ACCESS_2_MEMORY_WRITE_BIT, CS, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    for (int i = 0; i < 2; i++)
        barrier(cb, F.img[i], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, CS | XF, 0, CS,
                VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    uint32_t gx = (dw + 15) / 16, gy = (dh + 15) / 16;
    int last = 0;
    if (dw == (uint32_t)cw && dh == (uint32_t)ch) {
        // Native 1:1 sharpening: skip EASU spatial filter entirely, run RCAS directly on src
        float rcas[8] = {(float)dw, (float)dh, (float)(100 - sharp) / 50.0f, 0, 0, 0, 0, 0};
        bind_pass(cb, F.rcas, F.src_view[slot], F.view[0], rcas);
        vkCmdDispatch(cb, gx, gy, 1);
        last = 0;
    } else {
        float easu[8] = {(float)cw, (float)ch, (float)sw, (float)sh, (float)dw, (float)dh, (float)crop[0], (float)crop[1]};
        bind_pass(cb, F.easu, F.src_view[slot], F.view[0], easu);
        vkCmdDispatch(cb, gx, gy, 1);
        if (sharp > 0) {
            barrier(cb, F.img[0], VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                    CS, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
            float rcas[8] = {(float)dw, (float)dh, (float)(100 - sharp) / 50.0f, 0, 0, 0, 0, 0};
            bind_pass(cb, F.rcas, F.view[0], F.view[1], rcas);
            vkCmdDispatch(cb, gx, gy, 1);
            last = 1;
        }
    }
    barrier(cb, F.img[last], VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            XF, VK_ACCESS_2_TRANSFER_READ_BIT);
    // later submissions that write the source wait for the compute read
    barrier(cb, src, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, CS, 0, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0);
    VkImageBlit region = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                          .srcOffsets = {{0, 0, 0}, {(int)dw, (int)dh, 1}},
                          .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                          .dstOffsets = {{out[0], out[1], 0}, {out[0] + (int)dw, out[1] + (int)dh, 1}}};
    vkCmdBlitImage(cb, F.img[last], VK_IMAGE_LAYOUT_GENERAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region,
                   VK_FILTER_LINEAR);
    return 1;
}

void vk_fsr_release(void) {
    VkCtx *c = vk_ctx();
    if (!c || !F.tried) return;
    free_images(c);
    for (int i = 0; i < SLOTS; i++)
        if (F.src_view[i]) vkDestroyImageView(c->device, F.src_view[i], NULL);
    if (F.easu) vkDestroyPipeline(c->device, F.easu, NULL);
    if (F.rcas) vkDestroyPipeline(c->device, F.rcas, NULL);
    if (F.sampler) vkDestroySampler(c->device, F.sampler, NULL);
    if (F.layout) vkDestroyPipelineLayout(c->device, F.layout, NULL);
    if (F.dsl) vkDestroyDescriptorSetLayout(c->device, F.dsl, NULL);
    memset(&F, 0, sizeof F);
}

// ---- SMAA 1x Anti-Aliasing on the present path (NS2_SMAA=1)
// Three compute passes (smaa_edge, smaa_blend, smaa_neighborhood) with a procedurally generated analytic
// 160x560 RG8 area LUT. Zero cost when NS2_SMAA=0 (initialized lazily on first enabled frame).
#define SMAA_AREA_W 160u
#define SMAA_AREA_H 560u

static struct {
    int tried, ok, lut_uploaded, logged;
    PFN_vkCmdPushDescriptorSetKHR push;
    VkDescriptorSetLayout dsl[3];     // 0=edge(2), 1=blend(4), 2=neighborhood(3)
    VkPipelineLayout pl[3];
    VkPipeline pipe[3];
    VkSampler sampler;
    VkImage area_img, search_img;
    VmaAllocation area_alloc, search_alloc;
    VkImageView area_view, search_view;
    VkBuffer lut_stage;
    VmaAllocation lut_stage_alloc;
    VkImage edges_img, blend_img, out_img;
    VmaAllocation edges_alloc, blend_alloc, out_alloc;
    VkImageView edges_view, blend_view, out_view;
    uint32_t w, h;
    VkImageView src_view[SLOTS];
} S;

static VkPipeline make_smaa_pipe(VkCtx *c, VkPipelineLayout pl, const uint32_t *code, size_t size) {
    VkShaderModuleCreateInfo mi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = size, .pCode = code};
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreateShaderModule(c->device, &mi, NULL, &m) != VK_SUCCESS) return VK_NULL_HANDLE;
    VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = m, .pName = "main"},
                                      .layout = pl};
    VkPipeline p = VK_NULL_HANDLE;
    if (vkCreateComputePipelines(c->device, VK_NULL_HANDLE, 1, &ci, NULL, &p) != VK_SUCCESS) p = VK_NULL_HANDLE;
    vkDestroyShaderModule(c->device, m, NULL);
    return p;
}

static float smaa_trap(float x, float d) {
    if (d <= 0.0f) return 0.0f;
    float h0 = 0.5f - x / d;
    float h1 = 0.5f - (x + 1.0f) / d;
    if (h0 < 0.0f) h0 = 0.0f;
    if (h1 < 0.0f) h1 = 0.0f;
    return 0.5f * (h0 + h1);
}

static void smaa_fill_area_lut(uint8_t *dst) {
    memset(dst, 0, SMAA_AREA_W * SMAA_AREA_H * 2u);
    for (uint32_t py = 0; py < 5; py++) {
        for (uint32_t px = 0; px < 5; px++) {
            int e1 = px > 0, e2 = py > 0;
            if (!e1 && !e2) continue;
            for (uint32_t dy = 0; dy < 16; dy++) {
                for (uint32_t dx = 0; dx < 16; dx++) {
                    float left = (float)(dx * dx), right = (float)(dy * dy);
                    float d = left + right;
                    if (d < 1.0f) d = 1.0f;
                    float a1 = e1 ? smaa_trap(left, d) : 0.0f;
                    float a2 = e2 ? smaa_trap(right, d) : 0.0f;
                    if (a1 > 0.5f) a1 = 0.5f;
                    if (a2 > 0.5f) a2 = 0.5f;
                    uint32_t x = px * 16u + dx, y = py * 16u + dy;
                    size_t off = ((size_t)y * SMAA_AREA_W + x) * 2u;
                    dst[off + 0] = (uint8_t)(a1 * 255.0f + 0.5f);
                    dst[off + 1] = (uint8_t)(a2 * 255.0f + 0.5f);
                }
            }
        }
    }
}

static int smaa_make_img(VkCtx *c, uint32_t w, uint32_t h, VkFormat fmt, VkImageUsageFlags usage,
                         VkImage *img, VmaAllocation *alloc, VkImageView *view) {
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                            .format = fmt, .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1,
                            .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (vmaCreateImage(c->vma, &ci, &ai, img, alloc, NULL) != VK_SUCCESS) { *img = VK_NULL_HANDLE; return 0; }
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = *img,
                                .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = fmt,
                                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    if (vkCreateImageView(c->device, &vi, NULL, view) != VK_SUCCESS) {
        *view = VK_NULL_HANDLE;
        vmaDestroyImage(c->vma, *img, *alloc);
        *img = VK_NULL_HANDLE;
        return 0;
    }
    return 1;
}

static void smaa_free_rt(VkCtx *c) {
    if (S.edges_view) vkDestroyImageView(c->device, S.edges_view, NULL);
    if (S.edges_img) vmaDestroyImage(c->vma, S.edges_img, S.edges_alloc);
    if (S.blend_view) vkDestroyImageView(c->device, S.blend_view, NULL);
    if (S.blend_img) vmaDestroyImage(c->vma, S.blend_img, S.blend_alloc);
    if (S.out_view) vkDestroyImageView(c->device, S.out_view, NULL);
    if (S.out_img) vmaDestroyImage(c->vma, S.out_img, S.out_alloc);
    S.edges_view = S.blend_view = S.out_view = VK_NULL_HANDLE;
    S.edges_img = S.blend_img = S.out_img = VK_NULL_HANDLE;
    S.w = S.h = 0;
}

static int smaa_ensure_rt(VkCtx *c, uint32_t w, uint32_t h) {
    if (S.out_img && S.w == w && S.h == h) return 1;
    vk_queue_lock();
    vkDeviceWaitIdle(c->device);
    vk_queue_unlock();
    smaa_free_rt(c);
    VkImageUsageFlags u = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    if (!smaa_make_img(c, w, h, VK_FORMAT_R8G8_UNORM, u, &S.edges_img, &S.edges_alloc, &S.edges_view) ||
        !smaa_make_img(c, w, h, VK_FORMAT_R8G8B8A8_UNORM, u, &S.blend_img, &S.blend_alloc, &S.blend_view) ||
        !smaa_make_img(c, w, h, VK_FORMAT_R16G16B16A16_SFLOAT, u | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                       &S.out_img, &S.out_alloc, &S.out_view)) {
        smaa_free_rt(c);
        return 0;
    }
    S.w = w, S.h = h;
    return 1;
}

static int smaa_init(VkCtx *c) {
    S.tried = 1;
    S.push = (PFN_vkCmdPushDescriptorSetKHR)vkGetDeviceProcAddr(c->device, "vkCmdPushDescriptorSetKHR");
    if (!S.push) return 0;
    VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_LINEAR,
                              .minFilter = VK_FILTER_LINEAR, .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                              .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .maxLod = 0};
    if (vkCreateSampler(c->device, &si, NULL, &S.sampler) != VK_SUCCESS) return 0;
    static const uint32_t counts[3] = {2, 4, 3};
    static const uint32_t pc_sz[3] = {16, 32, 32};
    const uint32_t *spvs[3] = {smaa_edge_spv, smaa_blend_spv, smaa_neighborhood_spv};
    const size_t spv_sz[3] = {sizeof smaa_edge_spv, sizeof smaa_blend_spv, sizeof smaa_neighborhood_spv};
    for (int i = 0; i < 3; i++) {
        VkDescriptorSetLayoutBinding b[4];
        for (uint32_t k = 0; k < counts[i]; k++) {
            b[k] = (VkDescriptorSetLayoutBinding){
                .binding = k,
                .descriptorType = (k + 1 == counts[i]) ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                                                       : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                .descriptorCount = 1,
                .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
        }
        VkDescriptorSetLayoutCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                               .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR,
                                               .bindingCount = counts[i], .pBindings = b};
        VkPushConstantRange pr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, pc_sz[i]};
        VkPipelineLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                         .pSetLayouts = &S.dsl[i], .pushConstantRangeCount = 1, .pPushConstantRanges = &pr};
        if (vkCreateDescriptorSetLayout(c->device, &dci, NULL, &S.dsl[i]) != VK_SUCCESS ||
            vkCreatePipelineLayout(c->device, &li, NULL, &S.pl[i]) != VK_SUCCESS ||
            !(S.pipe[i] = make_smaa_pipe(c, S.pl[i], spvs[i], spv_sz[i]))) {
            rt_log("vk: SMAA 1x off: pipeline %d creation failed", i);
            return 0;
        }
    }
    VkImageUsageFlags lu = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (!smaa_make_img(c, SMAA_AREA_W, SMAA_AREA_H, VK_FORMAT_R8G8_UNORM, lu,
                       &S.area_img, &S.area_alloc, &S.area_view) ||
        !smaa_make_img(c, 1, 1, VK_FORMAT_R8_UNORM, lu,
                       &S.search_img, &S.search_alloc, &S.search_view)) {
        rt_log("vk: SMAA 1x off: LUT image creation failed");
        return 0;
    }
    size_t area_bytes = SMAA_AREA_W * SMAA_AREA_H * 2u;
    VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = area_bytes + 16u,
                              .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT};
    VmaAllocationCreateInfo bai = {.usage = VMA_MEMORY_USAGE_AUTO,
                                   .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                                            VMA_ALLOCATION_CREATE_MAPPED_BIT};
    VmaAllocationInfo info = {0};
    if (vmaCreateBuffer(c->vma, &bci, &bai, &S.lut_stage, &S.lut_stage_alloc, &info) != VK_SUCCESS || !info.pMappedData) {
        rt_log("vk: SMAA 1x off: LUT staging buffer failed");
        return 0;
    }
    smaa_fill_area_lut((uint8_t *)info.pMappedData);
    memset((uint8_t *)info.pMappedData + area_bytes, 0, 16u);
    vmaFlushAllocation(c->vma, S.lut_stage_alloc, 0, area_bytes + 16u);
    rt_log("vk: SMAA 1x morphological anti-aliasing ready");
    return S.ok = 1;
}

VkImage vk_smaa_record(VkCommandBuffer cb, unsigned slot, VkImage src, VkFormat fmt, int sw, int sh, const int crop[4]) {
    if (!cfg_bool("NS2_SMAA")) return VK_NULL_HANDLE;
    int cw = crop[2], ch = crop[3];
    if (!fmt || crop[0] != 0 || crop[1] != 0 || cw <= 0 || ch <= 0 || sw <= 0 || sh <= 0) return VK_NULL_HANDLE;
    VkCtx *c = vk_ctx();
    if (!S.tried) smaa_init(c);
    if (!S.ok || !smaa_ensure_rt(c, (uint32_t)cw, (uint32_t)ch)) return VK_NULL_HANDLE;

    const VkPipelineStageFlags2 CS = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, XF = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    if (!S.lut_uploaded) {
        S.lut_uploaded = 1;
        barrier(cb, S.area_img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0, XF, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        barrier(cb, S.search_img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0, XF, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        VkBufferImageCopy ca = {.bufferOffset = 0, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                .imageExtent = {SMAA_AREA_W, SMAA_AREA_H, 1}};
        VkBufferImageCopy cs = {.bufferOffset = SMAA_AREA_W * SMAA_AREA_H * 2u,
                                .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                .imageExtent = {1, 1, 1}};
        vkCmdCopyBufferToImage(cb, S.lut_stage, S.area_img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &ca);
        vkCmdCopyBufferToImage(cb, S.lut_stage, S.search_img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cs);
        barrier(cb, S.area_img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                XF, VK_ACCESS_2_TRANSFER_WRITE_BIT, CS, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        barrier(cb, S.search_img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                XF, VK_ACCESS_2_TRANSFER_WRITE_BIT, CS, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    }

    slot %= SLOTS;
    if (S.src_view[slot]) vkDestroyImageView(c->device, S.src_view[slot], NULL);
    VkImageViewUsageCreateInfo use = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO,
                                      .usage = VK_IMAGE_USAGE_SAMPLED_BIT};
    VkFormat vfmt = (fmt == VK_FORMAT_R8G8B8A8_SRGB) ? VK_FORMAT_R8G8B8A8_UNORM
                  : (fmt == VK_FORMAT_B8G8R8A8_SRGB) ? VK_FORMAT_B8G8R8A8_UNORM
                                                     : fmt;
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .pNext = &use, .image = src,
                                .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = vfmt,
                                .components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                                               VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_ONE},
                                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    if (vkCreateImageView(c->device, &vi, NULL, &S.src_view[slot]) != VK_SUCCESS) {
        S.src_view[slot] = VK_NULL_HANDLE;
        return VK_NULL_HANDLE;
    }
    if (!S.logged) {
        S.logged = 1;
        rt_log("vk: SMAA 1x active at %dx%d", cw, ch);
    }

    barrier(cb, src, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
            VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT, CS, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    barrier(cb, S.edges_img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, CS, 0, CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    barrier(cb, S.blend_img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, CS, 0, CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    barrier(cb, S.out_img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, CS | XF, 0, CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);

    uint32_t gx = ((uint32_t)cw + 7u) / 8u, gy = ((uint32_t)ch + 7u) / 8u;

    // Pass 1: Luma edge detection
    {
        struct { int32_t w, h; float threshold, dummy; } pc = {cw, ch, 0.08f, 0.0f};
        VkDescriptorImageInfo ii = {S.sampler, S.src_view[slot], VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorImageInfo oi = {VK_NULL_HANDLE, S.edges_view, VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet w[2] = {
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 0, .descriptorCount = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii},
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 1, .descriptorCount = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &oi}};
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, S.pipe[0]);
        rec_push(S.push, cb, VK_PIPELINE_BIND_POINT_COMPUTE, S.pl[0], 0, 2, w);
        vkCmdPushConstants(cb, S.pl[0], VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
        vkCmdDispatch(cb, gx, gy, 1);
    }

    barrier(cb, S.edges_img, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
            CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, CS, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);

    // Pass 2: Blending weight calculation
    struct { int32_t w, h, p0, p1; float inv_w, inv_h, fw, fh; } mpc = {
        cw, ch, 0, 0, 1.0f / (float)sw, 1.0f / (float)sh, (float)sw, (float)sh};
    {
        VkDescriptorImageInfo i0 = {S.sampler, S.edges_view, VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorImageInfo i1 = {S.sampler, S.area_view, VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorImageInfo i2 = {S.sampler, S.search_view, VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorImageInfo o0 = {VK_NULL_HANDLE, S.blend_view, VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet w[4] = {
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 0, .descriptorCount = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &i0},
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 1, .descriptorCount = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &i1},
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 2, .descriptorCount = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &i2},
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 3, .descriptorCount = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &o0}};
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, S.pipe[1]);
        rec_push(S.push, cb, VK_PIPELINE_BIND_POINT_COMPUTE, S.pl[1], 0, 4, w);
        vkCmdPushConstants(cb, S.pl[1], VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof mpc, &mpc);
        vkCmdDispatch(cb, gx, gy, 1);
    }

    barrier(cb, S.blend_img, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
            CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, CS, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);

    // Pass 3: Neighborhood blending
    {
        VkDescriptorImageInfo i0 = {S.sampler, S.src_view[slot], VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorImageInfo i1 = {S.sampler, S.blend_view, VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorImageInfo o0 = {VK_NULL_HANDLE, S.out_view, VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet w[3] = {
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 0, .descriptorCount = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &i0},
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 1, .descriptorCount = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &i1},
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 2, .descriptorCount = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &o0}};
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, S.pipe[2]);
        rec_push(S.push, cb, VK_PIPELINE_BIND_POINT_COMPUTE, S.pl[2], 0, 3, w);
        vkCmdPushConstants(cb, S.pl[2], VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof mpc, &mpc);
        vkCmdDispatch(cb, gx, gy, 1);
    }

    // Order subsequent FSR/blit reads after out_img write, and later guest writes after src read.
    barrier(cb, S.out_img, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
            CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, CS | XF, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_TRANSFER_READ_BIT);
    barrier(cb, src, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, CS, 0, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0);
    return S.out_img;
}

void vk_smaa_release(void) {
    VkCtx *c = vk_ctx();
    if (!c || !S.tried) return;
    smaa_free_rt(c);
    for (int i = 0; i < SLOTS; i++)
        if (S.src_view[i]) vkDestroyImageView(c->device, S.src_view[i], NULL);
    if (S.lut_stage) vmaDestroyBuffer(c->vma, S.lut_stage, S.lut_stage_alloc);
    if (S.area_view) vkDestroyImageView(c->device, S.area_view, NULL);
    if (S.area_img) vmaDestroyImage(c->vma, S.area_img, S.area_alloc);
    if (S.search_view) vkDestroyImageView(c->device, S.search_view, NULL);
    if (S.search_img) vmaDestroyImage(c->vma, S.search_img, S.search_alloc);
    for (int i = 0; i < 3; i++) {
        if (S.pipe[i]) vkDestroyPipeline(c->device, S.pipe[i], NULL);
        if (S.pl[i]) vkDestroyPipelineLayout(c->device, S.pl[i], NULL);
        if (S.dsl[i]) vkDestroyDescriptorSetLayout(c->device, S.dsl[i], NULL);
    }
    if (S.sampler) vkDestroySampler(c->device, S.sampler, NULL);
    memset(&S, 0, sizeof S);
}
