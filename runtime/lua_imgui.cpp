// ImGui for mods: a subset of ImGui, with the names and arguments common Lua ImGui bindings use, so UI code ports.
// Every function only works inside a Draw callback (render thread, inside the overlay's frame); ImGui is not
// thread-safe.
// Text never goes through a format string, so a mod's "%" can't read the stack.
#include "lua_api.h"

#include <algorithm>
#include <initializer_list>
#include <string>
#include <utility>

#include "imgui.h"
#include "imgui_internal.h"
#include "lauxlib.h"
#include "lua.h"

namespace {

ImGuiErrorRecoveryState gMark;

void needDraw(lua_State* L) {
    if (!rt::inDraw()) luaL_error(L, "ImGui functions only work inside Draw callbacks");
}

int ret(lua_State* L, bool b) {
    lua_pushboolean(L, b);
    return 1;
}

float num(lua_State* L, int i) { return static_cast<float>(luaL_checknumber(L, i)); }
int optInt(lua_State* L, int i) { return static_cast<int>(luaL_optinteger(L, i, 0)); }

// Begin(name [, open] [, flags]) -> [open,] shouldDraw. End() must follow either way.
int begin(lua_State* L) {
    needDraw(L);
    const char* name = luaL_checkstring(L, 1);
    if (!lua_isboolean(L, 2)) return ret(L, ImGui::Begin(name, nullptr, optInt(L, 2)));
    bool open = lua_toboolean(L, 2);
    bool draw = ImGui::Begin(name, &open, optInt(L, 3));
    lua_pushboolean(L, open);
    lua_pushboolean(L, draw);
    return 2;
}

int end(lua_State* L) {
    needDraw(L);
    ImGui::End();
    return 0;
}

int text(lua_State* L) {
    needDraw(L);
    ImGui::TextUnformatted(luaL_checkstring(L, 1));
    return 0;
}

int textColored(lua_State* L) {
    needDraw(L);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(num(L, 1), num(L, 2), num(L, 3), num(L, 4)));
    ImGui::TextUnformatted(luaL_checkstring(L, 5));
    ImGui::PopStyleColor();
    return 0;
}

int separator(lua_State* L) {
    needDraw(L);
    ImGui::Separator();
    return 0;
}

int sameLine(lua_State* L) {
    needDraw(L);
    ImGui::SameLine(static_cast<float>(luaL_optnumber(L, 1, 0)));
    return 0;
}

int spacing(lua_State* L) {
    needDraw(L);
    ImGui::Spacing();
    return 0;
}

int button(lua_State* L) {
    needDraw(L);
    ImVec2 size(static_cast<float>(luaL_optnumber(L, 2, 0)), static_cast<float>(luaL_optnumber(L, 3, 0)));
    return ret(L, ImGui::Button(luaL_checkstring(L, 1), size));
}

// Checkbox(label, v) -> v, pressed
int checkbox(lua_State* L) {
    needDraw(L);
    bool v = lua_toboolean(L, 2);
    bool pressed = ImGui::Checkbox(luaL_checkstring(L, 1), &v);
    lua_pushboolean(L, v);
    lua_pushboolean(L, pressed);
    return 2;
}

int pushFloat(lua_State* L, float v, bool used) {
    lua_pushnumber(L, v);
    lua_pushboolean(L, used);
    return 2;
}

int pushInt(lua_State* L, int v, bool used) {
    lua_pushinteger(L, v);
    lua_pushboolean(L, used);
    return 2;
}

// SliderFloat(label, v, min, max) -> v, used. Likewise SliderInt, InputFloat(label, v [, step]), InputInt.
int sliderFloat(lua_State* L) {
    needDraw(L);
    float v = num(L, 2);
    bool used = ImGui::SliderFloat(luaL_checkstring(L, 1), &v, num(L, 3), num(L, 4));
    return pushFloat(L, v, used);
}

int sliderInt(lua_State* L) {
    needDraw(L);
    int v = static_cast<int>(luaL_checkinteger(L, 2));
    bool used = ImGui::SliderInt(luaL_checkstring(L, 1), &v, static_cast<int>(luaL_checkinteger(L, 3)),
                                 static_cast<int>(luaL_checkinteger(L, 4)));
    return pushInt(L, v, used);
}

int inputFloat(lua_State* L) {
    needDraw(L);
    float v = num(L, 2);
    bool used = ImGui::InputFloat(luaL_checkstring(L, 1), &v, static_cast<float>(luaL_optnumber(L, 3, 0)));
    return pushFloat(L, v, used);
}

int inputInt(lua_State* L) {
    needDraw(L);
    int v = static_cast<int>(luaL_checkinteger(L, 2));
    bool used = ImGui::InputInt(luaL_checkstring(L, 1), &v, static_cast<int>(luaL_optinteger(L, 3, 1)));
    return pushInt(L, v, used);
}

// InputText(label, text, maxLen) -> text, changed. maxLen is capped at 4096 bytes.
int inputText(lua_State* L) {
    needDraw(L);
    size_t len = 0;
    const char* cur = luaL_checklstring(L, 2, &len);
    size_t cap = static_cast<size_t>(std::clamp<lua_Integer>(luaL_optinteger(L, 3, 256), 1, 4096));
    std::string buf(cur, std::min(len, cap));
    buf.resize(cap + 1);
    bool changed = ImGui::InputText(luaL_checkstring(L, 1), buf.data(), buf.size());
    lua_pushstring(L, buf.c_str());
    lua_pushboolean(L, changed);
    return 2;
}

int collapsingHeader(lua_State* L) {
    needDraw(L);
    return ret(L, ImGui::CollapsingHeader(luaL_checkstring(L, 1)));
}

// BeginChild(id [, w, h [, border [, flags]]]) -> shouldDraw: a scrolling region; 0 = fill. EndChild() must follow.
int beginChild(lua_State* L) {
    needDraw(L);
    ImVec2 size(static_cast<float>(luaL_optnumber(L, 2, 0)), static_cast<float>(luaL_optnumber(L, 3, 0)));
    ImGuiChildFlags child = lua_toboolean(L, 4) ? ImGuiChildFlags_Borders : ImGuiChildFlags_None;
    return ret(L, ImGui::BeginChild(luaL_checkstring(L, 1), size, child, optInt(L, 5)));
}

int endChild(lua_State* L) {
    needDraw(L);
    ImGui::EndChild();
    return 0;
}

int setNextWindowPos(lua_State* L) {
    needDraw(L);
    ImGui::SetNextWindowPos(ImVec2(num(L, 1), num(L, 2)), optInt(L, 3));
    return 0;
}

int setNextWindowSize(lua_State* L) {
    needDraw(L);
    ImGui::SetNextWindowSize(ImVec2(num(L, 1), num(L, 2)), optInt(L, 3));
    return 0;
}

int isItemHovered(lua_State* L) {
    needDraw(L);
    return ret(L, ImGui::IsItemHovered());
}

int setTooltip(lua_State* L) {
    needDraw(L);
    ImGui::SetTooltip("%s", luaL_checkstring(L, 1));
    return 0;
}

void constants(lua_State* L, const char* global, std::initializer_list<std::pair<const char*, int>> values) {
    lua_newtable(L);
    for (const auto& [name, v] : values) {
        lua_pushinteger(L, v);
        lua_setfield(L, -2, name);
    }
    lua_setglobal(L, global);
}

}  // namespace

void rt::openImGui(lua_State* L) {
    const luaL_Reg fns[] = {
        {"Begin", begin},           {"End", end},
        {"Text", text},             {"TextColored", textColored},
        {"Separator", separator},   {"SameLine", sameLine},
        {"Spacing", spacing},       {"Button", button},
        {"Checkbox", checkbox},     {"SliderFloat", sliderFloat},
        {"SliderInt", sliderInt},   {"InputFloat", inputFloat},
        {"InputInt", inputInt},     {"InputText", inputText},
        {"CollapsingHeader", collapsingHeader}, {"BeginChild", beginChild},
        {"EndChild", endChild},     {"SetNextWindowPos", setNextWindowPos},
        {"SetNextWindowSize", setNextWindowSize}, {"IsItemHovered", isItemHovered},
        {"SetTooltip", setTooltip}, {nullptr, nullptr}};
    luaL_newlib(L, fns);
    lua_setglobal(L, "ImGui");
    constants(L, "ImGuiCond", {{"Always", ImGuiCond_Always}, {"Once", ImGuiCond_Once},
                               {"FirstUseEver", ImGuiCond_FirstUseEver}, {"Appearing", ImGuiCond_Appearing}});
    constants(L, "ImGuiWindowFlags",
              {{"None", ImGuiWindowFlags_None}, {"NoTitleBar", ImGuiWindowFlags_NoTitleBar},
               {"NoResize", ImGuiWindowFlags_NoResize}, {"NoMove", ImGuiWindowFlags_NoMove},
               {"NoScrollbar", ImGuiWindowFlags_NoScrollbar}, {"NoCollapse", ImGuiWindowFlags_NoCollapse},
               {"AlwaysAutoResize", ImGuiWindowFlags_AlwaysAutoResize},
               {"NoBackground", ImGuiWindowFlags_NoBackground}, {"NoSavedSettings", ImGuiWindowFlags_NoSavedSettings},
               {"NoInputs", ImGuiWindowFlags_NoInputs}});
}

void rt::imguiMark() { ImGui::ErrorRecoveryStoreState(&gMark); }

void rt::imguiRecover() { ImGui::ErrorRecoveryTryToRecoverState(&gMark); }
