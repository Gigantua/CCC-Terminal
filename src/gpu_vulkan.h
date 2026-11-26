#pragma once
#include <vulkan/vulkan.h>
#include <algorithm>
#include <cstring>
#include <string>
#include <vector>
#include "cells_ps_spv.h"
#include "cells_vs_spv.h"
#include "gpu_data.h"

class Gpu {
public:
    static constexpr uint32_t FRAMES = 2;
    static constexpr size_t ATLAS_BYTES = 32 << 20;
    enum Slot { CELLS, TEXELS, PICTURES, SLOTS };

    ~Gpu() { destroy(); }

    bool init(const std::vector<const char*>& surfaceExtensions, VkSurfaceKHR (*makeSurface)(VkInstance, void*), void* window, uint32_t w, uint32_t h) {
        VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
        app.pApplicationName = "ccc";
        app.apiVersion = VK_API_VERSION_1_1;
        std::vector<const char*> extensions = surfaceExtensions;
        extensions.push_back(VK_KHR_SURFACE_EXTENSION_NAME);
        VkInstanceCreateInfo ic = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
        ic.pApplicationInfo = &app;
        ic.enabledExtensionCount = (uint32_t)extensions.size();
        ic.ppEnabledExtensionNames = extensions.data();
        if (vkCreateInstance(&ic, nullptr, &instance) != VK_SUCCESS) return false;
        surface = makeSurface(instance, window);
        if (!surface || !pickDevice() || !createDevice()) return false;
        width = w;
        height = h;
        if (!createRenderPass() || !createPipeline() || !createFrames() || !createSwapchain()) return false;
        atlasBuffer = createBuffer(ATLAS_BYTES, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        return atlasBuffer.mapped != nullptr;
    }

    void resize(uint32_t w, uint32_t h) {
        if (w == width && h == height) return;
        width = w;
        height = h;
        stale = true;
    }

    void beginFrame() {
        Frame& f = frames[frame];
        vkWaitForFences(device, 1, &f.fence, VK_TRUE, UINT64_MAX);
    }

    void* upload(Slot slot, size_t bytes) {
        Buffer& b = frames[frame].slots[slot];
        bytes = (std::max)(bytes, (size_t)16);
        if (bytes > b.capacity) {
            release(b);
            b = createBuffer(bytes * 2, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        }
        return b.mapped;
    }

    std::unordered_map<uint64_t, uint32_t>& resident(Slot slot) { return frames[frame].slots[slot].resident; }

    GpuCell* cells(size_t count) { return static_cast<GpuCell*>(upload(CELLS, count * sizeof(GpuCell))); }

    uint32_t* atlas() const { return static_cast<uint32_t*>(atlasBuffer.mapped); }

    void waitIdle() { if (device) vkDeviceWaitIdle(device); }

    void draw(const GpuConstants& k, bool vsync, const std::string& shot = std::string()) {
        if (vsync != vsyncOn) { vsyncOn = vsync; stale = true; }
        if (stale && !createSwapchain()) return;
        if (!width || !height) return;
        Frame& f = frames[frame];
        uint32_t image = 0;
        VkResult acquired = vkAcquireNextImageKHR(device, swapchain, UINT64_MAX, f.acquired, VK_NULL_HANDLE, &image);
        if (acquired == VK_ERROR_OUT_OF_DATE_KHR) { stale = true; return; }
        vkResetFences(device, 1, &f.fence);
        std::memcpy(f.constants.mapped, &k, sizeof k);
        upload(TEXELS, 0);
        upload(PICTURES, 0);
        bindResources(f);
        VkCommandBuffer cmd = f.cmd;
        vkResetCommandBuffer(cmd, 0);
        VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &bi);
        VkRenderPassBeginInfo rp = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
        rp.renderPass = pass;
        rp.framebuffer = targets[image].framebuffer;
        rp.renderArea.extent = extent;
        vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &f.set, 0, nullptr);
        VkViewport vp = { 0, 0, (float)extent.width, (float)extent.height, 0, 1 };
        VkRect2D scissor = { {}, extent };
        vkCmdSetViewport(cmd, 0, 1, &vp);
        vkCmdSetScissor(cmd, 0, 1, &scissor);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRenderPass(cmd);
        Buffer readback;
        if (!shot.empty()) {
            readback = createBuffer((size_t)extent.width * extent.height * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
            copyToBuffer(cmd, targets[image].image, readback.buffer);
        }
        vkEndCommandBuffer(cmd);
        VkPipelineStageFlags stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &f.acquired;
        si.pWaitDstStageMask = &stage;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &targets[image].rendered;
        vkQueueSubmit(queue, 1, &si, f.fence);
        if (readback.mapped) {
            vkWaitForFences(device, 1, &f.fence, VK_TRUE, UINT64_MAX);
            writeShot(shot, readback);
            release(readback);
        }
        VkPresentInfoKHR pi = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = &targets[image].rendered;
        pi.swapchainCount = 1;
        pi.pSwapchains = &swapchain;
        pi.pImageIndices = &image;
        VkResult presented = vkQueuePresentKHR(queue, &pi);
        if (presented == VK_ERROR_OUT_OF_DATE_KHR || presented == VK_SUBOPTIMAL_KHR) stale = true;
        frame = (frame + 1) % FRAMES;
    }

private:
    struct Buffer {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        void* mapped = nullptr;
        size_t capacity = 0;
        std::unordered_map<uint64_t, uint32_t> resident;
    };
    struct Frame {
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        VkSemaphore acquired = VK_NULL_HANDLE;
        VkDescriptorSet set = VK_NULL_HANDLE;
        Buffer constants;
        Buffer slots[SLOTS];
    };
    struct Target {
        VkImage image = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkFramebuffer framebuffer = VK_NULL_HANDLE;
        VkSemaphore rendered = VK_NULL_HANDLE;
    };

    VkInstance instance = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t family = 0;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_B8G8R8A8_UNORM;
    VkExtent2D extent = {};
    std::vector<Target> targets;
    VkRenderPass pass = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkCommandPool commands = VK_NULL_HANDLE;
    Frame frames[FRAMES];
    Buffer atlasBuffer;
    uint32_t frame = 0, width = 0, height = 0;
    bool stale = false, vsyncOn = true;

    bool pickDevice() {
        uint32_t n = 0;
        vkEnumeratePhysicalDevices(instance, &n, nullptr);
        std::vector<VkPhysicalDevice> devices(n);
        vkEnumeratePhysicalDevices(instance, &n, devices.data());
        int best = -1;
        for (VkPhysicalDevice d : devices) {
            uint32_t qn = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(d, &qn, nullptr);
            std::vector<VkQueueFamilyProperties> qs(qn);
            vkGetPhysicalDeviceQueueFamilyProperties(d, &qn, qs.data());
            for (uint32_t q = 0; q < qn; ++q) {
                VkBool32 present = VK_FALSE;
                vkGetPhysicalDeviceSurfaceSupportKHR(d, q, surface, &present);
                if (!(qs[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) || !present) continue;
                VkPhysicalDeviceProperties props;
                vkGetPhysicalDeviceProperties(d, &props);
                int score = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 3 : props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2 : 1;
                if (score > best) { best = score; physical = d; family = q; }
                break;
            }
        }
        return physical != VK_NULL_HANDLE;
    }

    bool createDevice() {
        float priority = 1;
        VkDeviceQueueCreateInfo qc = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
        qc.queueFamilyIndex = family;
        qc.queueCount = 1;
        qc.pQueuePriorities = &priority;
        std::vector<const char*> extensions = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
        uint32_t available = 0;
        vkEnumerateDeviceExtensionProperties(physical, nullptr, &available, nullptr);
        std::vector<VkExtensionProperties> offered(available);
        vkEnumerateDeviceExtensionProperties(physical, nullptr, &available, offered.data());
        for (const VkExtensionProperties& e : offered)
            if (std::strcmp(e.extensionName, "VK_KHR_portability_subset") == 0) extensions.push_back("VK_KHR_portability_subset");
        VkDeviceCreateInfo dc = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
        dc.queueCreateInfoCount = 1;
        dc.pQueueCreateInfos = &qc;
        dc.enabledExtensionCount = (uint32_t)extensions.size();
        dc.ppEnabledExtensionNames = extensions.data();
        if (vkCreateDevice(physical, &dc, nullptr, &device) != VK_SUCCESS) return false;
        vkGetDeviceQueue(device, family, 0, &queue);
        uint32_t n = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &n, nullptr);
        std::vector<VkSurfaceFormatKHR> formats(n);
        vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &n, formats.data());
        format = formats.empty() ? VK_FORMAT_B8G8R8A8_UNORM : formats[0].format;
        for (const VkSurfaceFormatKHR& f : formats)
            if (f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM) { format = f.format; break; }
        return true;
    }

    bool createRenderPass() {
        VkAttachmentDescription color = {};
        color.format = format;
        color.samples = VK_SAMPLE_COUNT_1_BIT;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        color.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        VkAttachmentReference ref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
        VkSubpassDescription sub = {};
        sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = 1;
        sub.pColorAttachments = &ref;
        VkSubpassDependency dep = {};
        dep.srcSubpass = VK_SUBPASS_EXTERNAL;
        dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        VkRenderPassCreateInfo rc = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
        rc.attachmentCount = 1;
        rc.pAttachments = &color;
        rc.subpassCount = 1;
        rc.pSubpasses = &sub;
        rc.dependencyCount = 1;
        rc.pDependencies = &dep;
        return vkCreateRenderPass(device, &rc, nullptr, &pass) == VK_SUCCESS;
    }

    VkShaderModule shaderModule(const uint32_t* code, size_t bytes) {
        VkShaderModuleCreateInfo mc = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
        mc.codeSize = bytes;
        mc.pCode = code;
        VkShaderModule m = VK_NULL_HANDLE;
        vkCreateShaderModule(device, &mc, nullptr, &m);
        return m;
    }

    bool createPipeline() {
        VkDescriptorSetLayoutBinding bindings[5] = {};
        for (uint32_t i = 0; i < 5; ++i) {
            bindings[i].binding = i;
            bindings[i].descriptorType = i == 0 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            bindings[i].descriptorCount = 1;
            bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        }
        VkDescriptorSetLayoutCreateInfo lc = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        lc.bindingCount = 5;
        lc.pBindings = bindings;
        if (vkCreateDescriptorSetLayout(device, &lc, nullptr, &setLayout) != VK_SUCCESS) return false;
        VkPipelineLayoutCreateInfo pl = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        pl.setLayoutCount = 1;
        pl.pSetLayouts = &setLayout;
        if (vkCreatePipelineLayout(device, &pl, nullptr, &layout) != VK_SUCCESS) return false;
        VkShaderModule vs = shaderModule(cells_vs_spv, sizeof(cells_vs_spv)), ps = shaderModule(cells_ps_spv, sizeof(cells_ps_spv));
        VkPipelineShaderStageCreateInfo stages[2] = { { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO }, { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO } };
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vs;
        stages[0].pName = "main";
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = ps;
        stages[1].pName = "main";
        VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
        VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
        vp.viewportCount = 1;
        vp.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode = VK_CULL_MODE_NONE;
        rs.lineWidth = 1;
        VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState blend = {};
        blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
        cb.attachmentCount = 1;
        cb.pAttachments = &blend;
        VkDynamicState dynamic[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
        ds.dynamicStateCount = 2;
        ds.pDynamicStates = dynamic;
        VkGraphicsPipelineCreateInfo pc = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
        pc.stageCount = 2;
        pc.pStages = stages;
        pc.pVertexInputState = &vi;
        pc.pInputAssemblyState = &ia;
        pc.pViewportState = &vp;
        pc.pRasterizationState = &rs;
        pc.pMultisampleState = &ms;
        pc.pColorBlendState = &cb;
        pc.pDynamicState = &ds;
        pc.layout = layout;
        pc.renderPass = pass;
        bool ok = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pc, nullptr, &pipeline) == VK_SUCCESS;
        vkDestroyShaderModule(device, vs, nullptr);
        vkDestroyShaderModule(device, ps, nullptr);
        return ok;
    }

    bool createFrames() {
        VkDescriptorPoolSize sizes[2] = { { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, FRAMES }, { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4 * FRAMES } };
        VkDescriptorPoolCreateInfo dp = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        dp.maxSets = FRAMES;
        dp.poolSizeCount = 2;
        dp.pPoolSizes = sizes;
        if (vkCreateDescriptorPool(device, &dp, nullptr, &pool) != VK_SUCCESS) return false;
        VkCommandPoolCreateInfo cp = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
        cp.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        cp.queueFamilyIndex = family;
        if (vkCreateCommandPool(device, &cp, nullptr, &commands) != VK_SUCCESS) return false;
        for (Frame& f : frames) {
            VkCommandBufferAllocateInfo ca = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
            ca.commandPool = commands;
            ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            ca.commandBufferCount = 1;
            vkAllocateCommandBuffers(device, &ca, &f.cmd);
            VkFenceCreateInfo fc = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
            fc.flags = VK_FENCE_CREATE_SIGNALED_BIT;
            vkCreateFence(device, &fc, nullptr, &f.fence);
            VkSemaphoreCreateInfo sc = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
            vkCreateSemaphore(device, &sc, nullptr, &f.acquired);
            VkDescriptorSetAllocateInfo da = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            da.descriptorPool = pool;
            da.descriptorSetCount = 1;
            da.pSetLayouts = &setLayout;
            if (vkAllocateDescriptorSets(device, &da, &f.set) != VK_SUCCESS) return false;
            f.constants = createBuffer(sizeof(GpuConstants), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
            if (!f.constants.mapped) return false;
        }
        return true;
    }

    void destroyTargets() {
        for (Target& t : targets) {
            vkDestroyFramebuffer(device, t.framebuffer, nullptr);
            vkDestroyImageView(device, t.view, nullptr);
            vkDestroySemaphore(device, t.rendered, nullptr);
        }
        targets.clear();
    }

    bool createSwapchain() {
        waitIdle();
        stale = false;
        VkSurfaceCapabilitiesKHR caps;
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, surface, &caps);
        extent = caps.currentExtent.width != UINT32_MAX ? caps.currentExtent : VkExtent2D{ width, height };
        extent.width = std::clamp(extent.width, caps.minImageExtent.width, caps.maxImageExtent.width);
        extent.height = std::clamp(extent.height, caps.minImageExtent.height, caps.maxImageExtent.height);
        if (!extent.width || !extent.height) return false;
        uint32_t n = 0;
        vkGetPhysicalDeviceSurfacePresentModesKHR(physical, surface, &n, nullptr);
        std::vector<VkPresentModeKHR> modes(n);
        vkGetPhysicalDeviceSurfacePresentModesKHR(physical, surface, &n, modes.data());
        VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
        if (!vsyncOn)
            for (VkPresentModeKHR wanted : { VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR })
                if (mode == VK_PRESENT_MODE_FIFO_KHR && std::find(modes.begin(), modes.end(), wanted) != modes.end()) mode = wanted;
        VkSwapchainCreateInfoKHR sc = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
        sc.surface = surface;
        sc.minImageCount = caps.maxImageCount ? (std::min)(caps.minImageCount + 1, caps.maxImageCount) : caps.minImageCount + 1;
        sc.imageFormat = format;
        sc.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
        sc.imageExtent = extent;
        sc.imageArrayLayers = 1;
        sc.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        sc.preTransform = caps.currentTransform;
        sc.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        sc.presentMode = mode;
        sc.clipped = VK_TRUE;
        sc.oldSwapchain = swapchain;
        VkSwapchainKHR next = VK_NULL_HANDLE;
        if (vkCreateSwapchainKHR(device, &sc, nullptr, &next) != VK_SUCCESS) return false;
        destroyTargets();
        if (swapchain) vkDestroySwapchainKHR(device, swapchain, nullptr);
        swapchain = next;
        vkGetSwapchainImagesKHR(device, swapchain, &n, nullptr);
        std::vector<VkImage> images(n);
        vkGetSwapchainImagesKHR(device, swapchain, &n, images.data());
        for (VkImage image : images) {
            Target t;
            t.image = image;
            VkImageViewCreateInfo vc = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
            vc.image = image;
            vc.viewType = VK_IMAGE_VIEW_TYPE_2D;
            vc.format = format;
            vc.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            vkCreateImageView(device, &vc, nullptr, &t.view);
            VkFramebufferCreateInfo fb = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
            fb.renderPass = pass;
            fb.attachmentCount = 1;
            fb.pAttachments = &t.view;
            fb.width = extent.width;
            fb.height = extent.height;
            fb.layers = 1;
            vkCreateFramebuffer(device, &fb, nullptr, &t.framebuffer);
            VkSemaphoreCreateInfo sem = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
            vkCreateSemaphore(device, &sem, nullptr, &t.rendered);
            targets.push_back(t);
        }
        return true;
    }

    Buffer createBuffer(size_t bytes, VkBufferUsageFlags usage) {
        Buffer b;
        VkBufferCreateInfo bc = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bc.size = bytes;
        bc.usage = usage;
        if (vkCreateBuffer(device, &bc, nullptr, &b.buffer) != VK_SUCCESS) return Buffer();
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(device, b.buffer, &req);
        VkPhysicalDeviceMemoryProperties mem;
        vkGetPhysicalDeviceMemoryProperties(physical, &mem);
        VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        uint32_t type = UINT32_MAX;
        for (uint32_t i = 0; i < mem.memoryTypeCount && type == UINT32_MAX; ++i)
            if ((req.memoryTypeBits & (1u << i)) && (mem.memoryTypes[i].propertyFlags & want) == want) type = i;
        VkMemoryAllocateInfo ma = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        ma.allocationSize = req.size;
        ma.memoryTypeIndex = type;
        if (type == UINT32_MAX || vkAllocateMemory(device, &ma, nullptr, &b.memory) != VK_SUCCESS) { vkDestroyBuffer(device, b.buffer, nullptr); return Buffer(); }
        vkBindBufferMemory(device, b.buffer, b.memory, 0);
        vkMapMemory(device, b.memory, 0, VK_WHOLE_SIZE, 0, &b.mapped);
        b.capacity = bytes;
        return b;
    }

    void release(Buffer& b) {
        if (!b.buffer) return;
        vkDestroyBuffer(device, b.buffer, nullptr);
        vkFreeMemory(device, b.memory, nullptr);
        b = Buffer();
    }

    void bindResources(Frame& f) {
        VkDescriptorBufferInfo infos[5] = {
            { f.constants.buffer, 0, VK_WHOLE_SIZE }, { f.slots[CELLS].buffer, 0, VK_WHOLE_SIZE }, { atlasBuffer.buffer, 0, VK_WHOLE_SIZE },
            { f.slots[TEXELS].buffer, 0, VK_WHOLE_SIZE }, { f.slots[PICTURES].buffer, 0, VK_WHOLE_SIZE },
        };
        VkWriteDescriptorSet writes[5] = {};
        for (uint32_t i = 0; i < 5; ++i) {
            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet = f.set;
            writes[i].dstBinding = i;
            writes[i].descriptorCount = 1;
            writes[i].descriptorType = i == 0 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[i].pBufferInfo = &infos[i];
        }
        vkUpdateDescriptorSets(device, 5, writes, 0, nullptr);
    }

    static void barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to, VkAccessFlags src, VkAccessFlags dst) {
        VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        b.oldLayout = from;
        b.newLayout = to;
        b.srcAccessMask = src;
        b.dstAccessMask = dst;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = image;
        b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    }

    void copyToBuffer(VkCommandBuffer cmd, VkImage image, VkBuffer to) {
        barrier(cmd, image, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        VkBufferImageCopy region = {};
        region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        region.imageExtent = { extent.width, extent.height, 1 };
        vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, to, 1, &region);
        barrier(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_TRANSFER_READ_BIT, 0);
    }

    void writeShot(const std::string& path, const Buffer& b) {
        size_t n = (size_t)extent.width * extent.height * 4;
        std::vector<uint8_t> pixels(static_cast<const uint8_t*>(b.mapped), static_cast<const uint8_t*>(b.mapped) + n);
        if (format == VK_FORMAT_R8G8B8A8_UNORM)
            for (size_t i = 0; i < n; i += 4) std::swap(pixels[i], pixels[i + 2]);
        saveBmp(path, extent.width, extent.height, pixels);
    }

    void destroy() {
        if (!instance) return;
        if (device) {
            vkDeviceWaitIdle(device);
            destroyTargets();
            if (swapchain) vkDestroySwapchainKHR(device, swapchain, nullptr);
            for (Frame& f : frames) {
                release(f.constants);
                for (Buffer& b : f.slots) release(b);
                vkDestroyFence(device, f.fence, nullptr);
                vkDestroySemaphore(device, f.acquired, nullptr);
            }
            release(atlasBuffer);
            vkDestroyCommandPool(device, commands, nullptr);
            vkDestroyDescriptorPool(device, pool, nullptr);
            vkDestroyPipeline(device, pipeline, nullptr);
            vkDestroyPipelineLayout(device, layout, nullptr);
            vkDestroyDescriptorSetLayout(device, setLayout, nullptr);
            vkDestroyRenderPass(device, pass, nullptr);
            vkDestroyDevice(device, nullptr);
        }
        if (surface) vkDestroySurfaceKHR(instance, surface, nullptr);
        vkDestroyInstance(instance, nullptr);
        instance = VK_NULL_HANDLE;
    }
};
