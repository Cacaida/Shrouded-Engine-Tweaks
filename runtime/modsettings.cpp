#include "modsettings.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <mutex>

#include "level.h"

namespace {

std::mutex gMx;
std::map<std::string, mset::Page> gPages;               // by mod
std::vector<std::pair<std::string, std::string>> gEdits;  // overlay edits not yet given to onChange

mset::Field* find(const std::string& mod, const std::string& key) {
    auto p = gPages.find(mod);
    if (p == gPages.end()) return nullptr;
    for (mset::Field& f : p->second.fields)
        if (f.key == key) return &f;
    return nullptr;
}

std::string jsonStr(const std::string& s) {
    std::string out = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') (out += '\\') += static_cast<char>(c);
        else if (c < 0x20) {
            char esc[8];
            snprintf(esc, sizeof esc, "\\u%04x", c);
            out += esc;
        } else out += static_cast<char>(c);
    }
    return out + '"';
}

// The shortest of %.14g and %.17g that reads back the same, as json.encode writes numbers.
std::string jsonNum(double v, bool integer) {
    char s[32];
    if (integer) snprintf(s, sizeof s, "%lld", static_cast<long long>(v));
    else if (snprintf(s, sizeof s, "%.14g", v); strtod(s, nullptr) != v) snprintf(s, sizeof s, "%.17g", v);
    return s;
}

std::string jsonValue(const mset::Field& f) {
    switch (f.type) {
    case mset::Bool: return f.num ? "true" : "false";
    case mset::Number: return jsonNum(f.num, false);
    case mset::Int: return jsonNum(f.num, true);
    case mset::Choice: return jsonStr(f.values[static_cast<size_t>(f.num)]);
    default: return jsonStr(f.text);
    }
}

// Written next to the file and swapped in, so an interrupted save never leaves half a file.
// ponytail: no FlushFileBuffers (a game crash can't lose it, a power cut can); add it if that ever matters.
bool writeFile(const std::string& path, const std::string& text) {
    std::string tmp = path + ".tmp";
    FILE* f = nullptr;
    if (fopen_s(&f, tmp.c_str(), "wb") != 0 || !f) return false;
    bool ok = fwrite(text.data(), 1, text.size(), f) == text.size();
    ok = fclose(f) == 0 && ok;
    return ok && MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
}

// Writes the given pages, outside the lock; a failed one is marked changed again, and reported once.
void write(const std::vector<std::pair<std::string, std::string>>& out, mset::Sink sink) {
    for (const auto& [mod, text] : out) {
        std::string path;
        {
            std::lock_guard lock(gMx);
            auto p = gPages.find(mod);
            if (p == gPages.end()) continue;
            path = p->second.path;
        }
        bool ok = writeFile(path, text);
        std::lock_guard lock(gMx);
        auto p = gPages.find(mod);
        if (p == gPages.end() || ok) continue;
        p->second.dirty = true;
        if (!std::exchange(p->second.told, true)) sink((LV_WARN "Settings: Couldn't save " + path + ", trying again").c_str());
    }
}

// The changed pages' text, taken under the lock. mod empty: all of them.
std::vector<std::pair<std::string, std::string>> changed(const std::string& mod) {
    std::lock_guard lock(gMx);
    std::vector<std::pair<std::string, std::string>> out;
    for (auto& [m, p] : gPages)
        if (p.dirty && (mod.empty() || m == mod)) {
            out.emplace_back(m, mset::toJson(p));
            p.dirty = false;
        }
    return out;
}

}  // namespace

void mset::define(Page page) {
    std::lock_guard lock(gMx);
    std::string mod = page.mod;
    gPages[mod] = std::move(page);
}

bool mset::has(const std::string& mod) {
    std::lock_guard lock(gMx);
    return gPages.count(mod) != 0;
}

bool mset::get(const std::string& mod, const std::string& key, Field& out) {
    std::lock_guard lock(gMx);
    const Field* f = find(mod, key);
    if (f) out = *f;
    return f != nullptr;
}

bool mset::assign(Field& f, double num, const std::string& text) {
    if (!std::isfinite(num)) return false;  // JSON has no inf: it would break the file
    if (f.type == Choice && (num < 0 || num >= static_cast<double>(f.values.size()))) return false;
    if (f.type == Number || f.type == Int) num = std::clamp(f.type == Int ? std::round(num) : num, f.lo, f.hi);
    if (f.type == Bool || f.type == Choice) num = f.type == Bool ? num != 0 : std::floor(num);
    size_t n = f.type == Text ? std::min(text.size(), f.maxLen) : 0;
    while (n && n < text.size() && (static_cast<unsigned char>(text[n]) & 0xC0) == 0x80) n--;  // not mid-character
    f.num = num;
    f.text = text.substr(0, n);
    return true;
}

bool mset::set(const std::string& mod, const std::string& key, double num, const std::string& text, bool edit) {
    std::lock_guard lock(gMx);
    Field* f = find(mod, key);
    if (!f) return false;
    Field next = *f;
    if (!assign(next, num, text)) return false;
    if (next.num == f->num && next.text == f->text) return true;
    *f = std::move(next);
    gPages[mod].dirty = true;
    std::pair<std::string, std::string> e{mod, key};
    if (edit && std::find(gEdits.begin(), gEdits.end(), e) == gEdits.end()) gEdits.push_back(std::move(e));
    return true;
}

std::vector<std::pair<std::string, std::string>> mset::takeEdits() {
    std::lock_guard lock(gMx);
    return std::exchange(gEdits, {});
}

std::vector<mset::Page> mset::pages() {
    std::lock_guard lock(gMx);
    std::vector<Page> out;
    for (const auto& [m, p] : gPages) out.push_back(p);
    return out;
}

// One key per line, sorted, so the file reads and diffs well by hand. Keys the mod doesn't define are kept.
std::string mset::toJson(const Page& page) {
    std::map<std::string, std::string> all = page.unknown;
    for (const Field& f : page.fields) all[f.key] = jsonValue(f);
    std::string out = "{";
    for (const auto& [k, v] : all) out += (out.size() > 1 ? ",\n  " : "\n  ") + jsonStr(k) + ": " + v;
    return out + "\n}\n";
}

void mset::save(bool now, Sink sink) {
    static std::chrono::steady_clock::time_point last;
    auto t = std::chrono::steady_clock::now();
    if (!now && t - last < std::chrono::seconds(1)) return;
    auto out = changed("");
    if (out.empty()) return;
    last = t;
    write(out, sink);
}

void mset::drop(const std::string& mod, Sink sink) {
    write(changed(mod), sink);
    std::lock_guard lock(gMx);
    gPages.erase(mod);
    std::erase_if(gEdits, [&](const auto& e) { return e.first == mod; });
}

void mset::dropAll(Sink sink) {
    write(changed(""), sink);
    std::lock_guard lock(gMx);
    gPages.clear();
    gEdits.clear();
}
