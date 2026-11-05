# Third-party code

Vendored unmodified. Update by replacing the folder and this table, never by editing files in place.

| Folder | Project | Version | Source | Integrity | License |
|---|---|---|---|---|---|
| `lua/` | Lua | 5.4.9 (2026-08-10) | `https://www.lua.org/ftp/lua-5.4.9.tar.gz`, `src/` without `lua.c`, `luac.c`, `Makefile` | SHA-256 `2335b6c582a52654f94612bf10d2f4672805d05329aa6568b1d8cd9e5c6fb8e6` (as published on lua.org) | MIT, text at the end of `lua.h` |
| `minhook/` | MinHook | v1.3.4 | `https://github.com/TsudaKageyu/minhook`, `include/`, `src/`, `LICENSE.txt` | commit `c3fcafdc10146beb5919319d0683e44e3c30d537` | BSD 2-Clause, `LICENSE.txt` |
| `imgui/` | Dear ImGui | v1.92.9b (2026-07-31) | `https://github.com/ocornut/imgui`, root sources and headers, `backends/imgui_impl_{win32,vulkan}.{h,cpp}`, `LICENSE.txt` | commit `f1cc2ae15e53a861a874c3034aae6798fde194ab` | MIT, `LICENSE.txt` |
| `vulkan/` | Vulkan-Headers | vulkan-sdk-1.4.341.0 | `https://github.com/KhronosGroup/Vulkan-Headers`, the C headers of `include/vulkan/` and `include/vk_video/`, `LICENSE.md`, `LICENSES/` | commit `b5c8f996196ba4aa6d8f97e52b5d3b6e70f7e4e2` | Apache-2.0 OR MIT, `LICENSES/` |
