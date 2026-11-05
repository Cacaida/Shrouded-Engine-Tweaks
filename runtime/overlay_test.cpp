// Offline check of the overlay layer: no game, but it needs a GPU. Enables the layer the way the game gets it (a
// manifest plus the loader's env vars), then drives a real window and swapchain through the system Vulkan loader.
// Usage: overlay_selftest.exe [path\to\powrprof.dll]   (default: the one next to this exe)
#include <windows.h>

#include <vulkan/vulkan.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#define CHECK(c)                                                                                                       \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                                                        \
            exit(1);                                                                                                   \
        }                                                                                                              \
    } while (0)

namespace {

constexpr int kFrames = 120;

#define VK_FNS(X)                                                                                                      \
    X(DestroyInstance) X(EnumeratePhysicalDevices) X(GetPhysicalDeviceQueueFamilyProperties)                           \
    X(GetPhysicalDeviceSurfaceSupportKHR) X(GetPhysicalDeviceSurfaceCapabilitiesKHR)                                   \
    X(GetPhysicalDeviceSurfaceFormatsKHR) X(CreateWin32SurfaceKHR) X(DestroySurfaceKHR) X(CreateDevice)                \
    X(DestroyDevice) X(GetDeviceQueue) X(CreateSwapchainKHR) X(DestroySwapchainKHR) X(GetSwapchainImagesKHR)          \
    X(AcquireNextImageKHR) X(QueuePresentKHR) X(QueueSubmit) X(CreateCommandPool) X(DestroyCommandPool)                \
    X(AllocateCommandBuffers) X(BeginCommandBuffer) X(EndCommandBuffer) X(CmdPipelineBarrier) X(CreateSemaphore)      \
    X(DestroySemaphore) X(CreateFence) X(DestroyFence) X(WaitForFences) X(ResetFences) X(DeviceWaitIdle)

#define X(n) PFN_vk##n n;
VK_FNS(X)
#undef X

VkInstance gInst;
HWND gWnd;
VkSurfaceKHR gSurface;
VkPhysicalDevice gPhys;
uint32_t gFamily;
VkDevice gDev;
VkQueue gQueue;
VkSwapchainKHR gSwap;
std::vector<VkImage> gImages;

// The same manifest and variables ov::setup() writes for the game, pointing at the built DLL.
void enableLayer(const std::wstring& dll) {
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    std::wstring dir = std::wstring(tmp) + L"rt_overlay_test";
    CreateDirectoryW(dir.c_str(), nullptr);
    std::string path;
    for (wchar_t c : dll) path += c == L'\\' ? std::string("\\\\") : std::string(1, static_cast<char>(c));
    std::string json = "{\"file_format_version\": \"1.2.0\", \"layer\": {\"name\": \"VK_LAYER_SHROUDED_ENGINE_TWEAKS\", "
                       "\"type\": \"GLOBAL\", \"library_path\": \"" + path +
                       "\", \"api_version\": \"1.4.341\", \"implementation_version\": \"1\", \"description\": \"test\"}}";
    FILE* f = nullptr;
    CHECK(_wfopen_s(&f, (dir + L"\\shrouded_engine_tweaks.json").c_str(), L"wb") == 0);
    fputs(json.c_str(), f);
    fclose(f);
    SetEnvironmentVariableW(L"VK_ADD_LAYER_PATH", dir.c_str());
    SetEnvironmentVariableW(L"VK_LOADER_LAYERS_ENABLE", L"VK_LAYER_SHROUDED_ENGINE_TWEAKS");
}

void makeWindow() {
    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"rt_overlay_test";
    RegisterClassW(&wc);
    gWnd = CreateWindowW(wc.lpszClassName, L"overlay selftest", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100, 640, 360,
                         nullptr, nullptr, wc.hInstance, nullptr);
    CHECK(gWnd);
}

void makeInstance() {
    HMODULE vk = LoadLibraryW(L"vulkan-1.dll");
    CHECK(vk);
    auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(vk, "vkGetInstanceProcAddr"));
    auto create = reinterpret_cast<PFN_vkCreateInstance>(gipa(nullptr, "vkCreateInstance"));
    const char* exts[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &app;
    ci.enabledExtensionCount = 2;
    ci.ppEnabledExtensionNames = exts;
    CHECK(create(&ci, nullptr, &gInst) == VK_SUCCESS);
#define X(n) n = reinterpret_cast<PFN_vk##n>(gipa(gInst, "vk" #n));
    VK_FNS(X)
#undef X
    VkWin32SurfaceCreateInfoKHR sci{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
    sci.hinstance = GetModuleHandleW(nullptr);
    sci.hwnd = gWnd;
    CHECK(CreateWin32SurfaceKHR(gInst, &sci, nullptr, &gSurface) == VK_SUCCESS);
}

// First GPU with a queue family that does graphics and can present to the window.
void makeDevice() {
    uint32_t n = 8;
    VkPhysicalDevice phys[8];
    EnumeratePhysicalDevices(gInst, &n, phys);
    for (uint32_t p = 0; p < n && !gPhys; p++) {
        uint32_t fn = 16;
        VkQueueFamilyProperties fams[16];
        GetPhysicalDeviceQueueFamilyProperties(phys[p], &fn, fams);
        for (uint32_t f = 0; f < fn && !gPhys; f++) {
            VkBool32 present = VK_FALSE;
            GetPhysicalDeviceSurfaceSupportKHR(phys[p], f, gSurface, &present);
            if (present && (fams[f].queueFlags & VK_QUEUE_GRAPHICS_BIT)) gPhys = phys[p], gFamily = f;
        }
    }
    CHECK(gPhys);
    float prio = 1;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = gFamily;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    const char* ext = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 1;
    dci.ppEnabledExtensionNames = &ext;
    CHECK(CreateDevice(gPhys, &dci, nullptr, &gDev) == VK_SUCCESS);
    GetDeviceQueue(gDev, gFamily, 0, &gQueue);
}

void makeSwapchain() {
    VkSurfaceCapabilitiesKHR caps;
    GetPhysicalDeviceSurfaceCapabilitiesKHR(gPhys, gSurface, &caps);
    uint32_t n = 1;
    VkSurfaceFormatKHR fmt;
    GetPhysicalDeviceSurfaceFormatsKHR(gPhys, gSurface, &n, &fmt);  // VK_INCOMPLETE is fine: the first one will do
    VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.surface = gSurface;
    ci.minImageCount = caps.minImageCount + 1;
    ci.imageFormat = fmt.format;
    ci.imageColorSpace = fmt.colorSpace;
    ci.imageExtent = caps.currentExtent;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;  // no COLOR_ATTACHMENT: the layer must add it itself
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    ci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    ci.clipped = VK_TRUE;
    CHECK(CreateSwapchainKHR(gDev, &ci, nullptr, &gSwap) == VK_SUCCESS);
    GetSwapchainImagesKHR(gDev, gSwap, &n, nullptr);
    gImages.resize(n);
    GetSwapchainImagesKHR(gDev, gSwap, &n, gImages.data());
}

void pump() {
    MSG m;
    while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
}

void recordBarrier(VkCommandBuffer cb, VkImage img) {
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    BeginCommandBuffer(cb, &bi);
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0,
                       nullptr, 0, nullptr, 1, &b);
    EndCommandBuffer(cb);
}

// One frame at a time, like a simple game: acquire, a layout barrier, submit, present, wait.
void presentFrames(int count) {
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = gFamily;
    VkCommandPool pool;
    CreateCommandPool(gDev, &pci, nullptr, &pool);
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = pool;
    ai.commandBufferCount = 1;
    VkCommandBuffer cb;
    AllocateCommandBuffers(gDev, &ai, &cb);
    VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkSemaphore acquired;
    std::vector<VkSemaphore> done(gImages.size());
    VkFence fence;
    CreateSemaphore(gDev, &si, nullptr, &acquired);
    for (VkSemaphore& s : done) CreateSemaphore(gDev, &si, nullptr, &s);
    CreateFence(gDev, &fi, nullptr, &fence);
    for (int i = 0; i < count; i++) {
        pump();
        uint32_t idx = 0;
        CHECK(AcquireNextImageKHR(gDev, gSwap, UINT64_MAX, acquired, VK_NULL_HANDLE, &idx) >= 0);
        recordBarrier(cb, gImages[idx]);
        VkPipelineStageFlags stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo sub{VK_STRUCTURE_TYPE_SUBMIT_INFO, nullptr, 1, &acquired, &stage, 1, &cb, 1, &done[idx]};
        CHECK(QueueSubmit(gQueue, 1, &sub, fence) == VK_SUCCESS);
        VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, nullptr, 1, &done[idx], 1, &gSwap, &idx};
        CHECK(QueuePresentKHR(gQueue, &pi) >= 0);
        WaitForFences(gDev, 1, &fence, VK_TRUE, UINT64_MAX);
        ResetFences(gDev, 1, &fence);
    }
    DeviceWaitIdle(gDev);
    DestroyFence(gDev, fence, nullptr);
    for (VkSemaphore s : done) DestroySemaphore(gDev, s, nullptr);
    DestroySemaphore(gDev, acquired, nullptr);
    DestroyCommandPool(gDev, pool, nullptr);
}

}  // namespace

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);  // a crash must not swallow the progress lines
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring dll = buf;
    dll = argc > 1 ? std::wstring(argv[1], argv[1] + strlen(argv[1])) : dll.substr(0, dll.rfind(L'\\') + 1) + L"powrprof.dll";
    CHECK(GetFileAttributesW(dll.c_str()) != INVALID_FILE_ATTRIBUTES);
    enableLayer(dll);
    makeWindow();
    makeInstance();
    HMODULE layer = GetModuleHandleW(dll.c_str());
    CHECK(layer);  // the loader loaded our DLL as a layer
    wchar_t sysDir[MAX_PATH];
    CHECK(GetSystemDirectoryW(sysDir, MAX_PATH));
    HMODULE sys = LoadLibraryW((std::wstring(sysDir) + L"\\powrprof.dll").c_str());
    CHECK(sys && GetProcAddress(layer, "CallNtPowerInformation") == GetProcAddress(sys, "CallNtPowerInformation"));
    printf("forwarded exports ok\n");
    auto frames = reinterpret_cast<uint64_t (*)()>(GetProcAddress(layer, "rtOverlayFrames"));
    auto openAll = reinterpret_cast<void (*)()>(GetProcAddress(layer, "rtOverlayOpenAll"));
    auto askKey = reinterpret_cast<void (*)(int)>(GetProcAddress(layer, "rtOverlayAskKey"));
    CHECK(frames && openAll && askKey);
    openAll();  // the open frames below draw every built-in window
    makeDevice();
    makeSwapchain();
    printf("layer loaded, swapchain %zu images\n", gImages.size());
    presentFrames(kFrames);
    CHECK(frames() == 0);  // closed: presents pass through untouched
    printf("closed frames ok\n");
    PostMessageW(gWnd, WM_KEYDOWN, VK_OEM_3, 0);  // the toggle key, handled by the layer's window subclass
    presentFrames(kFrames);
    CHECK(frames() == kFrames);  // open: the overlay drew into every frame
    printf("open frames ok\n");
    askKey(1);  // the first-start key window: the toggle key must not close it
    PostMessageW(gWnd, WM_KEYDOWN, VK_OEM_3, 0);
    presentFrames(10);
    uint64_t asked = frames();
    CHECK(asked >= kFrames + 9);  // still drawing; ImGui hides a new auto-resizing window for its first frame
    printf("key window frames ok\n");
    askKey(0);
    PostMessageW(gWnd, WM_KEYDOWN, VK_OEM_3, 0);
    presentFrames(10);
    CHECK(frames() == asked);  // closed again
    DestroySwapchainKHR(gDev, gSwap, nullptr);
    DestroyDevice(gDev, nullptr);
    DestroySurfaceKHR(gInst, gSurface, nullptr);
    DestroyInstance(gInst, nullptr);
    DestroyWindow(gWnd);  // the window subclass outlives the instance: the layer must still be loaded
    printf("overlay selftest ok\n");
    return 0;
}
