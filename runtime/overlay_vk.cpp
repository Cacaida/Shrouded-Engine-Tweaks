// The overlay's Vulkan half: a layer inside powrprof.dll. ov::setup() points the Vulkan loader at a manifest naming
// this DLL, so the loader puts us into the game's call chain. Every Vulkan call we make goes down the chain through the
// next layer's pointers; VK_NO_PROTOTYPES keeps the loader's own exports out of reach.
#define NOMINMAX
#include <windows.h>

#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

#include "imgui.h"
#include "imgui_impl_vulkan.h"
#include "overlay.h"

#pragma comment(linker, "/export:vkNegotiateLoaderLayerInterfaceVersion")

extern "C" IMAGE_DOS_HEADER __ImageBase;  // this DLL

namespace {

constexpr const char* kLayerName = "VK_LAYER_SHROUDED_ENGINE_TWEAKS";
constexpr uint64_t kTimeout = 1'000'000'000;  // 1 s for our own fences: a stuck one skips frames, never hangs the game

ov::Sink gSink;
std::string gIni;              // imgui.ini, UTF-8
std::atomic<uint64_t> gDrawn;  // frames the overlay drew; read by overlay_selftest through rtOverlayFrames
std::atomic<bool> gInstSeen;   // our CreateInstance ran: the loader put the layer into the game's chain
bool gManifestOk;

// Down-chain device functions, loaded through the next layer's vkGetDeviceProcAddr.
#define DEV_FNS(X)                                                                                                     \
    X(DestroyDevice) X(GetDeviceQueue) X(GetDeviceQueue2) X(CreateSwapchainKHR) X(DestroySwapchainKHR)                 \
    X(GetSwapchainImagesKHR) X(QueuePresentKHR) X(CreateImageView) X(DestroyImageView) X(CreateFramebuffer)            \
    X(DestroyFramebuffer) X(CreateRenderPass) X(DestroyRenderPass) X(CreateCommandPool) X(DestroyCommandPool)          \
    X(AllocateCommandBuffers) X(BeginCommandBuffer) X(EndCommandBuffer) X(CmdBeginRenderPass) X(CmdEndRenderPass)      \
    X(QueueSubmit) X(CreateFence) X(DestroyFence) X(WaitForFences) X(ResetFences) X(CreateSemaphore)                   \
    X(DestroySemaphore) X(DeviceWaitIdle)

struct Inst {
    VkInstance inst;
    PFN_vkGetInstanceProcAddr gipa;
    PFN_vkDestroyInstance DestroyInstance;
    PFN_vkCreateWin32SurfaceKHR CreateWin32SurfaceKHR;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties GetPhysicalDeviceQueueFamilyProperties;
};

struct Dev {
    VkDevice dev;
    VkPhysicalDevice phys;
    VkInstance inst;
    PFN_vkGetInstanceProcAddr gipa;
    PFN_vkGetDeviceProcAddr gdpa;
    PFN_vkSetDeviceLoaderData setData;
    std::vector<VkQueueFamilyProperties> families;
    std::unordered_map<VkQueue, uint32_t> queueFam;
#define X(n) PFN_vk##n n;
    DEV_FNS(X)
#undef X
};

struct Swap {
    Dev* dev;
    HWND wnd;
    VkFormat format;
    VkExtent2D extent;
    uint32_t images;
    bool logged;
    // Made on the first frame the overlay draws into this swapchain: one of each per image.
    std::vector<VkImageView> views;
    std::vector<VkFramebuffer> fbs;
    std::vector<VkSemaphore> done;  // our submit signals it, present waits on it
};

// Drawing state. The ImGui Vulkan backend is a singleton, so the overlay draws on one device and queue: the first
// ones it is opened on. Only the present path and the destroy calls touch it, which the game already serializes.
struct Gfx {
    Dev* dev;
    VkQueue queue;
    VkCommandPool pool;
    VkRenderPass pass;
    VkFormat format;
    bool imgui;
    std::vector<VkCommandBuffer> cmds;  // a ring, one fence per slot; also protects ImGui's per-frame buffers
    std::vector<VkFence> fences;
    uint32_t slot;
} gGfx;
bool gUiHeld;  // guardedDraw holds ov::uiMx around draw(); its SEH handler must release it

// Keyed by the dispatch key: the first pointer of a dispatchable handle, shared by an instance and its physical
// devices, and by a device and its queues.
std::mutex gMx;
std::unordered_map<void*, Inst> gInsts;
std::unordered_map<void*, std::unique_ptr<Dev>> gDevs;
std::unordered_map<VkSurfaceKHR, HWND> gWnds;
std::unordered_map<VkSwapchainKHR, Swap> gSwaps;

void* key(const void* handle) { return *static_cast<void* const*>(handle); }

Inst* findInst(const void* handle) {
    auto it = gInsts.find(key(handle));
    return it == gInsts.end() ? nullptr : &it->second;
}

Dev* findDev(const void* handle) {
    auto it = gDevs.find(key(handle));
    return it == gDevs.end() ? nullptr : it->second.get();
}

// The loader's chain-link entry of a create info: VK_LAYER_LINK_INFO or VK_LOADER_DATA_CALLBACK.
template <class T>
T* findLink(const void* chain, VkStructureType type, VkLayerFunction fn) {
    for (auto* p = static_cast<const T*>(chain); p; p = static_cast<const T*>(p->pNext))
        if (p->sType == type && p->function == fn) return const_cast<T*>(p);
    return nullptr;
}

template <class F>
F load(PFN_vkGetInstanceProcAddr gipa, VkInstance inst, const char* name) {
    return reinterpret_cast<F>(gipa(inst, name));
}

// ---- drawing ----

// Every command buffer we allocate, ImGui's included, gets the loader's dispatch pointer so layers below can use it.
VKAPI_ATTR VkResult VKAPI_CALL AllocCmds(VkDevice dev, const VkCommandBufferAllocateInfo* ai, VkCommandBuffer* out) {
    VkResult r = gGfx.dev->AllocateCommandBuffers(dev, ai, out);
    for (uint32_t i = 0; r == VK_SUCCESS && i < ai->commandBufferCount; i++) gGfx.dev->setData(dev, out[i]);
    return r;
}

PFN_vkVoidFunction imguiLoad(const char* name, void* user) {
    auto* d = static_cast<Dev*>(user);
    if (strcmp(name, "vkAllocateCommandBuffers") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&AllocCmds);
    if (PFN_vkVoidFunction f = d->gdpa(d->dev, name)) return f;
    return d->gipa(d->inst, name);  // instance-level, e.g. vkGetPhysicalDeviceMemoryProperties
}

// Draws over what the game rendered: load, draw, and hand the image back still ready to present.
VkRenderPass makePass(Dev* d, VkFormat format) {
    VkAttachmentDescription a{0, format, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_LOAD,
                              VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                              VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                              VK_IMAGE_LAYOUT_PRESENT_SRC_KHR};
    VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &ref;
    VkSubpassDependency dep{VK_SUBPASS_EXTERNAL, 0, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
                            VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, 0};
    VkRenderPassCreateInfo ci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    ci.attachmentCount = 1;
    ci.pAttachments = &a;
    ci.subpassCount = 1;
    ci.pSubpasses = &sub;
    ci.dependencyCount = 1;
    ci.pDependencies = &dep;
    VkRenderPass pass = VK_NULL_HANDLE;
    d->CreateRenderPass(d->dev, &ci, nullptr, &pass);
    return pass;
}

void waitRing() {
    if (!gGfx.fences.empty())
        gGfx.dev->WaitForFences(gGfx.dev->dev, static_cast<uint32_t>(gGfx.fences.size()), gGfx.fences.data(), VK_TRUE,
                                kTimeout);
}

void freeSwapRes(Swap& s) {
    Dev* d = s.dev;
    for (VkFramebuffer f : s.fbs) d->DestroyFramebuffer(d->dev, f, nullptr);
    for (VkImageView v : s.views) d->DestroyImageView(d->dev, v, nullptr);
    for (VkSemaphore x : s.done) d->DestroySemaphore(d->dev, x, nullptr);
    s.fbs.clear();
    s.views.clear();
    s.done.clear();
}

bool makeSwapRes(Swap& s, VkSwapchainKHR sc) {
    Dev* d = s.dev;
    uint32_t n = 0;
    d->GetSwapchainImagesKHR(d->dev, sc, &n, nullptr);
    std::vector<VkImage> imgs(n);
    d->GetSwapchainImagesKHR(d->dev, sc, &n, imgs.data());
    s.views.assign(n, VK_NULL_HANDLE);
    s.fbs.assign(n, VK_NULL_HANDLE);
    s.done.assign(n, VK_NULL_HANDLE);
    for (uint32_t i = 0; i < n; i++) {
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = imgs[i];
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = s.format;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (d->CreateImageView(d->dev, &vi, nullptr, &s.views[i]) != VK_SUCCESS) return false;
        VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fi.renderPass = gGfx.pass;
        fi.attachmentCount = 1;
        fi.pAttachments = &s.views[i];
        fi.width = s.extent.width;
        fi.height = s.extent.height;
        fi.layers = 1;
        if (d->CreateFramebuffer(d->dev, &fi, nullptr, &s.fbs[i]) != VK_SUCCESS) return false;
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        if (d->CreateSemaphore(d->dev, &si, nullptr, &s.done[i]) != VK_SUCCESS) return false;
    }
    return true;
}

bool makeRing(Dev* d, uint32_t family, uint32_t n) {
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = family;
    if (d->CreateCommandPool(d->dev, &pci, nullptr, &gGfx.pool) != VK_SUCCESS) return false;
    gGfx.cmds.resize(n);
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = gGfx.pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = n;
    if (AllocCmds(d->dev, &ai, gGfx.cmds.data()) != VK_SUCCESS) return false;
    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, nullptr, VK_FENCE_CREATE_SIGNALED_BIT};
    gGfx.fences.assign(n, VK_NULL_HANDLE);
    for (VkFence& f : gGfx.fences)
        if (d->CreateFence(d->dev, &fi, nullptr, &f) != VK_SUCCESS) return false;
    return true;
}

// First frame the overlay is open: command ring, render pass and the ImGui Vulkan backend. Caller holds ov::uiMx.
// Limit: a failed setup leaks the objects it made; it happens at most once, then the overlay is off.
bool initGfx(Dev* d, VkQueue q, uint32_t family, const Swap& s) {
    uint32_t n = std::max(s.images, 2u);
    gGfx.dev = d;
    gGfx.queue = q;
    gGfx.format = s.format;
    if (!makeRing(d, family, n) || !(gGfx.pass = makePass(d, s.format))) return false;
    if (!ImGui_ImplVulkan_LoadFunctions(VK_API_VERSION_1_0, imguiLoad, d)) return false;
    ImGui_ImplVulkan_InitInfo ii{};
    ii.ApiVersion = VK_API_VERSION_1_0;
    ii.Instance = d->inst;
    ii.PhysicalDevice = d->phys;
    ii.Device = d->dev;
    ii.QueueFamily = family;
    ii.Queue = q;
    ii.DescriptorPoolSize = IMGUI_IMPL_VULKAN_MINIMUM_SAMPLED_IMAGE_POOL_SIZE;
    ii.MinImageCount = 2;
    ii.ImageCount = n;
    ii.PipelineInfoMain.RenderPass = gGfx.pass;
    return gGfx.imgui = ImGui_ImplVulkan_Init(&ii);
}

// The game switched swapchain formats (an HDR toggle): new render pass and pipeline, framebuffers made again.
void rebuildPass(VkFormat format) {
    waitRing();
    {
        std::lock_guard lock(gMx);
        for (auto& [sc, s] : gSwaps)
            if (!s.fbs.empty()) freeSwapRes(s);
    }
    gGfx.dev->DestroyRenderPass(gGfx.dev->dev, gGfx.pass, nullptr);
    gGfx.pass = makePass(gGfx.dev, format);
    gGfx.format = format;
    ImGui_ImplVulkan_PipelineInfo pi{};
    pi.RenderPass = gGfx.pass;
    ImGui_ImplVulkan_CreateMainPipeline(&pi);
}

// Builds the ImGui frame, records it into the presented image and submits it after the game's own rendering. An
// empty frame (closed, and no mod window showing) records nothing, so the game's present goes through untouched.
bool recordAndSubmit(Swap& s, uint32_t img, const VkPresentInfoKHR* info) {
    ImGui_ImplVulkan_NewFrame();
    ov::buildFrame();
    ImDrawData* dd = ImGui::GetDrawData();
    if (!dd || dd->TotalVtxCount == 0) return false;
    Dev* d = gGfx.dev;
    uint32_t slot = gGfx.slot++ % gGfx.cmds.size();
    VkFence fence = gGfx.fences[slot];
    VkCommandBuffer cb = gGfx.cmds[slot];
    if (d->WaitForFences(d->dev, 1, &fence, VK_TRUE, kTimeout) != VK_SUCCESS) {
        ov::debug("Overlay: Frame skipped, the previous overlay frame was still on the GPU after 1 s");  // Fence timeout
        return false;
    }
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    d->BeginCommandBuffer(cb, &bi);
    VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rp.renderPass = gGfx.pass;
    rp.framebuffer = s.fbs[img];
    rp.renderArea.extent = s.extent;
    d->CmdBeginRenderPass(cb, &rp, VK_SUBPASS_CONTENTS_INLINE);
    ImGui_ImplVulkan_RenderDrawData(dd, cb);
    d->CmdEndRenderPass(cb);
    d->EndCommandBuffer(cb);
    std::vector<VkPipelineStageFlags> stages(info->waitSemaphoreCount, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO, nullptr, info->waitSemaphoreCount, info->pWaitSemaphores,
                    stages.data(), 1, &cb, 1, &s.done[img]};
    d->ResetFences(d->dev, 1, &fence);
    if (d->QueueSubmit(gGfx.queue, 1, &si, fence) != VK_SUCCESS) {
        ov::log(LV_BAD "Overlay: Queue submit failed, overlay off until the game restarts");
        ov::disable();
        return false;
    }
    return true;
}

bool draw(Dev* d, Swap& s, VkSwapchainKHR sc, VkQueue q, uint32_t family, const VkPresentInfoKHR* info) {
    if (!gGfx.dev && !initGfx(d, q, family, s)) {
        ov::log(LV_BAD "Overlay: Vulkan setup failed, overlay off");
        ov::disable();
        return false;
    }
    if (d != gGfx.dev || q != gGfx.queue) {
        // another device or queue: the overlay won't show on these frames, said once
        static bool once;
        if (!std::exchange(once, true))
            ov::log("Overlay: The game presents from another GPU queue too; the overlay draws only on its first one");
        return false;
    }
    if (s.format != gGfx.format) rebuildPass(s.format);
    if (s.fbs.empty() && !makeSwapRes(s, sc)) {
        freeSwapRes(s);
        static bool once;  // Used to fail silently
        if (!std::exchange(once, true))
            ov::log(LV_BAD "Overlay: Making the swapchain's image views or framebuffers failed, no overlay on it");
        return false;
    }
    return recordAndSubmit(s, info->pImageIndices[0], info);
}

// An access violation while drawing turns the overlay off; the game keeps presenting without it.
bool guardedDraw(Dev* d, Swap* s, VkSwapchainKHR sc, VkQueue q, uint32_t family, const VkPresentInfoKHR* info) {
    __try {
        ov::uiMx.lock();
        gUiHeld = true;
        bool ok = draw(d, *s, sc, q, family, info);
        gUiHeld = false;
        ov::uiMx.unlock();
        return ok;
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER
                                                                  : EXCEPTION_CONTINUE_SEARCH) {
        if (gUiHeld) {
            gUiHeld = false;
            ov::uiMx.unlock();
        }
        ov::disable();
        ov::log(LV_BAD "Overlay: Access violation while drawing, overlay off until the game restarts");
        return false;
    }
}

void shutdownGfx() {
    Dev* d = gGfx.dev;
    d->DeviceWaitIdle(d->dev);
    if (gGfx.imgui) {
        std::lock_guard lock(ov::uiMx);
        ImGui_ImplVulkan_Shutdown();
    }
    for (VkFence f : gGfx.fences) d->DestroyFence(d->dev, f, nullptr);
    if (gGfx.pool) d->DestroyCommandPool(d->dev, gGfx.pool, nullptr);
    if (gGfx.pass) d->DestroyRenderPass(d->dev, gGfx.pass, nullptr);
    gGfx = {};
}

// ---- the layer ----

VKAPI_ATTR VkResult VKAPI_CALL CreateInstance(const VkInstanceCreateInfo* ci, const VkAllocationCallbacks* alloc,
                                              VkInstance* out) {
    gInstSeen = true;
    auto* link = findLink<VkLayerInstanceCreateInfo>(ci->pNext, VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO,
                                                     VK_LAYER_LINK_INFO);
    if (!link) return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkGetInstanceProcAddr gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;  // the next layer reads its own link
    VkResult r = load<PFN_vkCreateInstance>(gipa, VK_NULL_HANDLE, "vkCreateInstance")(ci, alloc, out);
    if (r != VK_SUCCESS) return r;
    Inst i{*out, gipa, load<PFN_vkDestroyInstance>(gipa, *out, "vkDestroyInstance"),
           load<PFN_vkCreateWin32SurfaceKHR>(gipa, *out, "vkCreateWin32SurfaceKHR"),
           load<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(gipa, *out, "vkGetPhysicalDeviceQueueFamilyProperties")};
    std::lock_guard lock(gMx);
    gInsts[key(*out)] = i;
    return r;
}

VKAPI_ATTR void VKAPI_CALL DestroyInstance(VkInstance inst, const VkAllocationCallbacks* alloc) {
    PFN_vkDestroyInstance next = nullptr;
    {
        std::lock_guard lock(gMx);
        if (Inst* i = findInst(inst)) next = i->DestroyInstance;
        gInsts.erase(key(inst));
    }
    if (next) next(inst, alloc);
}

VKAPI_ATTR VkResult VKAPI_CALL CreateWin32SurfaceKHR(VkInstance inst, const VkWin32SurfaceCreateInfoKHR* ci,
                                                     const VkAllocationCallbacks* alloc, VkSurfaceKHR* out) {
    PFN_vkCreateWin32SurfaceKHR next;
    {
        std::lock_guard lock(gMx);
        next = findInst(inst)->CreateWin32SurfaceKHR;
    }
    VkResult r = next(inst, ci, alloc, out);
    if (r == VK_SUCCESS) {
        std::lock_guard lock(gMx);
        gWnds[*out] = ci->hwnd;
    }
    return r;
}

std::unique_ptr<Dev> makeDev(const Inst* inst, VkPhysicalDevice phys, VkDevice dev, PFN_vkGetDeviceProcAddr gdpa,
                             PFN_vkSetDeviceLoaderData setData) {
    auto d = std::make_unique<Dev>();
    d->dev = dev;
    d->phys = phys;
    d->inst = inst->inst;
    d->gipa = inst->gipa;
    d->gdpa = gdpa;
    d->setData = setData;
#define X(n) d->n = reinterpret_cast<PFN_vk##n>(gdpa(dev, "vk" #n));
    DEV_FNS(X)
#undef X
    uint32_t n = 0;
    inst->GetPhysicalDeviceQueueFamilyProperties(phys, &n, nullptr);
    d->families.resize(n);
    inst->GetPhysicalDeviceQueueFamilyProperties(phys, &n, d->families.data());
    return d;
}

VKAPI_ATTR VkResult VKAPI_CALL CreateDevice(VkPhysicalDevice phys, const VkDeviceCreateInfo* ci,
                                            const VkAllocationCallbacks* alloc, VkDevice* out) {
    auto* link = findLink<VkLayerDeviceCreateInfo>(ci->pNext, VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO,
                                                   VK_LAYER_LINK_INFO);
    auto* cb = findLink<VkLayerDeviceCreateInfo>(ci->pNext, VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO,
                                                 VK_LOADER_DATA_CALLBACK);
    Inst inst;
    {
        std::lock_guard lock(gMx);
        Inst* i = findInst(phys);
        if (!link || !cb || !i) return VK_ERROR_INITIALIZATION_FAILED;
        inst = *i;
    }
    PFN_vkGetInstanceProcAddr gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr gdpa = link->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;
    VkResult r = load<PFN_vkCreateDevice>(gipa, inst.inst, "vkCreateDevice")(phys, ci, alloc, out);
    if (r != VK_SUCCESS) return r;
    auto d = makeDev(&inst, phys, *out, gdpa, cb->u.pfnSetDeviceLoaderData);
    std::lock_guard lock(gMx);
    gDevs[key(*out)] = std::move(d);
    return r;
}

VKAPI_ATTR void VKAPI_CALL DestroyDevice(VkDevice dev, const VkAllocationCallbacks* alloc) {
    std::unique_ptr<Dev> d;
    {
        std::lock_guard lock(gMx);
        auto it = gDevs.find(key(dev));
        if (it == gDevs.end()) return;
        d = std::move(it->second);
        gDevs.erase(it);
        for (auto& [sc, s] : gSwaps)
            if (s.dev == d.get() && !s.fbs.empty()) freeSwapRes(s);
        std::erase_if(gSwaps, [&](const auto& s) { return s.second.dev == d.get(); });
    }
    if (gGfx.dev == d.get()) shutdownGfx();
    d->DestroyDevice(dev, alloc);
}

VKAPI_ATTR void VKAPI_CALL GetDeviceQueue(VkDevice dev, uint32_t family, uint32_t index, VkQueue* out) {
    std::lock_guard lock(gMx);
    Dev* d = findDev(dev);
    d->GetDeviceQueue(dev, family, index, out);
    d->queueFam[*out] = family;
}

VKAPI_ATTR void VKAPI_CALL GetDeviceQueue2(VkDevice dev, const VkDeviceQueueInfo2* info, VkQueue* out) {
    std::lock_guard lock(gMx);
    Dev* d = findDev(dev);
    d->GetDeviceQueue2(dev, info, out);
    d->queueFam[*out] = info->queueFamilyIndex;
}

VKAPI_ATTR VkResult VKAPI_CALL CreateSwapchainKHR(VkDevice dev, const VkSwapchainCreateInfoKHR* ci,
                                                  const VkAllocationCallbacks* alloc, VkSwapchainKHR* out) {
    Dev* d;
    {
        std::lock_guard lock(gMx);
        d = findDev(dev);
    }
    VkSwapchainCreateInfoKHR mine = *ci;
    mine.imageUsage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;  // every surface supports it; we render into the images
    VkResult r = d->CreateSwapchainKHR(dev, &mine, alloc, out);
    if (r != VK_SUCCESS) return r;
    uint32_t n = 0;
    d->GetSwapchainImagesKHR(dev, *out, &n, nullptr);
    HWND wnd;
    {
        std::lock_guard lock(gMx);
        wnd = gWnds[ci->surface];
        gSwaps[*out] = Swap{d, wnd, ci->imageFormat, ci->imageExtent, n, false, {}, {}, {}};
    }
    std::lock_guard lock(ov::uiMx);
    ov::attach(wnd, gIni);
    ov::resize(wnd, ci->imageExtent.height);
    return r;
}

VKAPI_ATTR void VKAPI_CALL DestroySwapchainKHR(VkDevice dev, VkSwapchainKHR sc, const VkAllocationCallbacks* alloc) {
    Dev* d;
    Swap s{};
    {
        std::lock_guard lock(gMx);
        d = findDev(dev);
        auto it = gSwaps.find(sc);
        if (it != gSwaps.end()) s = std::move(it->second), gSwaps.erase(it);
    }
    if (!s.fbs.empty()) {
        waitRing();
        freeSwapRes(s);
    }
    d->DestroySwapchainKHR(dev, sc, alloc);
}

// Logs each swapchain once, on its first present: what the overlay draws into, and from which queue.
void logSwap(Swap& s, uint32_t family) {
    s.logged = true;
    bool gfx = family < s.dev->families.size() && (s.dev->families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT);
    ov::log(LV_OK "Overlay: Swapchain %ux%u, format %d, %u images, present queue family %d (graphics: %s)", s.extent.width,
            s.extent.height, s.format, s.images, static_cast<int>(family), gfx ? "Yes" : "No");
}

std::atomic<int> gFpsCap;  // settings.ini fpsCap, 0 = off

int64_t qpc() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

// The frame rate cap: each present waits for its slot, 1/cap after the previous one. A high-resolution timer (Windows
// 10 1803+) sleeps to 0.5 ms before it, a spin does the rest. Falling behind restarts the schedule, so slow frames are
// never made up with fast ones.
void capFrame() {
    int cap = gFpsCap;
    if (cap <= 0) return;
    static HANDLE timer =
        CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    static const int64_t freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f.QuadPart;
    }();
    static int64_t next;
    int64_t period = freq / cap, now = qpc();
    if (now - next > period) next = now;  // the first frame, a slow one, or a lower cap than before
    int64_t sleep = next - now - freq / 2000;
    if (timer && sleep > 0) {
        LARGE_INTEGER due;
        due.QuadPart = -sleep * 10'000'000 / freq;  // relative, in 100 ns units
        if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) WaitForSingleObject(timer, INFINITE);
    }
    while (qpc() < next) YieldProcessor();
    next += period;
}

// Why a visible overlay isn't drawn on a present, logged (debugLog) only when the reason changes.
void noteSkip(const char* why) {
    static std::atomic<const char*> last;
    if (last.exchange(why) != why && why) ov::debug("Overlay: Not drawn: %s", why);
}

// Overlay visible: our submit waits on the game's semaphores and present waits on ours. Otherwise a plain
// pass-through.
VKAPI_ATTR VkResult VKAPI_CALL QueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* info) {
    capFrame();
    Dev* d;
    Swap* s = nullptr;
    uint32_t family = UINT32_MAX;
    {
        std::lock_guard lock(gMx);
        d = findDev(queue);
        auto fam = d->queueFam.find(queue);
        if (fam != d->queueFam.end()) family = fam->second;
        for (uint32_t i = 0; i < info->swapchainCount; i++) {
            auto it = gSwaps.find(info->pSwapchains[i]);
            if (it != gSwaps.end() && !it->second.logged) logSwap(it->second, family);
        }
        if (info->swapchainCount == 1 && gSwaps.count(info->pSwapchains[0])) s = &gSwaps[info->pSwapchains[0]];
    }
    bool gfx = family < d->families.size() && (d->families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT);
    if (ov::visible())
        noteSkip(!s    ? (info->swapchainCount != 1 ? "The present has several swapchains" : "Unknown swapchain")
                 : !gfx ? "The present queue can't do graphics"
                        : nullptr);
    if (!s || !gfx || !ov::visible() || !guardedDraw(d, s, info->pSwapchains[0], queue, family, info))
        return d->QueuePresentKHR(queue, info);
    gDrawn++;
    VkPresentInfoKHR mine = *info;
    mine.waitSemaphoreCount = 1;
    mine.pWaitSemaphores = &s->done[info->pImageIndices[0]];
    return d->QueuePresentKHR(queue, &mine);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GetDeviceProcAddr(VkDevice dev, const char* name);
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GetInstanceProcAddr(VkInstance inst, const char* name);

struct Hook {
    const char* name;
    PFN_vkVoidFunction fn;
};

#define HOOK(n) {"vk" #n, reinterpret_cast<PFN_vkVoidFunction>(&n)}
const Hook kInstHooks[] = {HOOK(GetInstanceProcAddr), HOOK(CreateInstance), HOOK(DestroyInstance),
                           HOOK(CreateWin32SurfaceKHR), HOOK(CreateDevice)};
const Hook kDevHooks[] = {HOOK(GetDeviceProcAddr), HOOK(DestroyDevice), HOOK(GetDeviceQueue), HOOK(GetDeviceQueue2),
                          HOOK(CreateSwapchainKHR), HOOK(DestroySwapchainKHR), HOOK(QueuePresentKHR)};
#undef HOOK

template <size_t N>
PFN_vkVoidFunction ours(const Hook (&hooks)[N], const char* name) {
    for (const Hook& h : hooks)
        if (strcmp(h.name, name) == 0) return h.fn;
    return nullptr;
}

// Ours where we intercept, else the next layer's. A function the chain below doesn't have stays null, even if we
// would intercept it.
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GetDeviceProcAddr(VkDevice dev, const char* name) {
    PFN_vkGetDeviceProcAddr gdpa;
    {
        std::lock_guard lock(gMx);
        Dev* d = findDev(dev);
        if (!d) return nullptr;
        gdpa = d->gdpa;
    }
    PFN_vkVoidFunction next = gdpa(dev, name);
    PFN_vkVoidFunction mine = ours(kDevHooks, name);
    return next && mine ? mine : next;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GetInstanceProcAddr(VkInstance inst, const char* name) {
    if (!inst) return ours(kInstHooks, name);  // vkCreateInstance and the other global commands
    PFN_vkGetInstanceProcAddr gipa;
    {
        std::lock_guard lock(gMx);
        Inst* i = findInst(inst);
        if (!i) return nullptr;
        gipa = i->gipa;
    }
    PFN_vkVoidFunction next = gipa(inst, name);
    PFN_vkVoidFunction mine = ours(kInstHooks, name);
    if (!mine) mine = ours(kDevHooks, name);
    return next && mine ? mine : next;
}

// Layer manifest naming this DLL. library_path is absolute, so the loader gets back the module already loaded.
bool writeManifest(const std::wstring& dir) {
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(reinterpret_cast<HMODULE>(&__ImageBase), self, MAX_PATH);
    std::string path;
    for (char c : ov::utf8(self)) path += c == '\\' ? std::string("\\\\") : std::string(1, c);
    std::string json = std::string("{\"file_format_version\": \"1.2.0\", \"layer\": {\"name\": \"") + kLayerName +
                       "\", \"type\": \"GLOBAL\", \"library_path\": \"" + path +
                       "\", \"api_version\": \"1.4.341\", \"implementation_version\": \"1\", "
                       "\"description\": \"Shrouded Engine Tweaks overlay\"}}\n";
    HANDLE f = CreateFileW((dir + L"\\shrouded_engine_tweaks.json").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD w = 0;
    bool ok = WriteFile(f, json.data(), static_cast<DWORD>(json.size()), &w, nullptr) && w == json.size();
    CloseHandle(f);
    return ok;
}

void appendEnv(const wchar_t* name, const std::wstring& value, wchar_t sep) {
    wchar_t old[4096];
    DWORD n = GetEnvironmentVariableW(name, old, 4096);
    std::wstring v = n > 0 && n < 4096 ? std::wstring(old) + sep + value : value;
    SetEnvironmentVariableW(name, v.c_str());
}

// The loader's own test: above medium integrity it ignores VK_ADD_LAYER_PATH, so the layer is never found.
bool highIntegrity() {
    HANDLE t;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &t)) return false;
    uint8_t buf[SECURITY_MAX_SID_SIZE + sizeof(DWORD)];
    DWORD n = 0;
    bool high = false;
    if (GetTokenInformation(t, TokenIntegrityLevel, buf, sizeof buf, &n)) {
        PSID sid = reinterpret_cast<TOKEN_MANDATORY_LABEL*>(buf)->Label.Sid;
        high = *GetSidSubAuthority(sid, *GetSidSubAuthorityCount(sid) - 1) > SECURITY_MANDATORY_MEDIUM_RID;
    }
    CloseHandle(t);
    return high;
}

// The game's Vulkan loader, e.g. "1.4.341". It comes with the GPU driver, so it differs between PCs.
std::string loaderVersion() {
    wchar_t path[MAX_PATH];
    HMODULE m = GetModuleHandleW(L"vulkan-1.dll");
    if (!m || !GetModuleFileNameW(m, path, MAX_PATH)) return "not loaded";
    DWORD n = GetFileVersionInfoSizeW(path, nullptr);
    std::vector<char> buf(n);
    VS_FIXEDFILEINFO* fi = nullptr;
    UINT len = 0;
    if (!n || !GetFileVersionInfoW(path, 0, n, buf.data()) ||
        !VerQueryValueW(buf.data(), L"\\", reinterpret_cast<void**>(&fi), &len) || !fi)
        return "unknown";
    char s[32];
    snprintf(s, sizeof s, "%u.%u.%u", HIWORD(fi->dwFileVersionMS), LOWORD(fi->dwFileVersionMS),
             HIWORD(fi->dwFileVersionLS));
    return s;
}

}  // namespace

VKAPI_ATTR VkResult VKAPI_CALL vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface* v) {
    if (v->sType != LAYER_NEGOTIATE_INTERFACE_STRUCT || v->loaderLayerInterfaceVersion < 2)
        return VK_ERROR_INITIALIZATION_FAILED;
    v->loaderLayerInterfaceVersion = 2;
    v->pfnGetInstanceProcAddr = GetInstanceProcAddr;
    v->pfnGetDeviceProcAddr = GetDeviceProcAddr;
    v->pfnGetPhysicalDeviceProcAddr = nullptr;
    return VK_SUCCESS;
}

extern "C" __declspec(dllexport) uint64_t rtOverlayFrames() { return gDrawn; }

std::string ov::utf8(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    return s;
}

// The loader reads both variables when the game creates its Vulkan instance, long after DllMain. They are ignored
// only for elevated processes. VK_LOADER_LAYERS_ENABLE is a filter, so a missing manifest just means no layer.
void ov::setup(const std::wstring& game, Sink sink) {
    gSink = sink;
    gameDir = game;
    std::wstring dir = game + L"shrouded_engine_tweaks";
    gIni = utf8(dir + L"\\imgui.ini");
    CreateDirectoryW(dir.c_str(), nullptr);
    dir += L"\\layer";
    CreateDirectoryW(dir.c_str(), nullptr);
    if (!(gManifestOk = writeManifest(dir))) return;
    appendEnv(L"VK_ADD_LAYER_PATH", dir, L';');
    appendEnv(L"VK_LOADER_LAYERS_ENABLE", std::wstring(kLayerName, kLayerName + strlen(kLayerName)), L',');
}

bool ov::layerLoaded() { return gInstSeen; }

bool ov::manifestOk() { return gManifestOk; }

void ov::setFpsCap(int fps) { gFpsCap = fps; }

bool ov::elevated() { return highIntegrity(); }

std::string ov::loaderVersion() { return ::loaderVersion(); }

namespace {
std::atomic<bool> gDebug;  // settings.ini debugLog

// logOnly: the line goes to set.log only (LV_LOG).
void vlog(const char* fmt, va_list ap, bool logOnly = false) {
    if (!gSink) return;
    char buf[1200] = LV_LOG;
    vsnprintf(buf + logOnly, sizeof buf - 1, fmt, ap);
    gSink(buf);
}
}  // namespace

void ov::setDebugLog(bool on) { gDebug = on; }

void ov::debug(const char* fmt, ...) {
    if (!gDebug) return;
    va_list ap;
    va_start(ap, fmt);
    vlog(fmt, ap, true);
    va_end(ap);
}

void ov::log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vlog(fmt, ap);
    va_end(ap);
}
