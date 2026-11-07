#include "game.h"
#include "hooks.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

struct Range {
    const uint8_t* lo = nullptr;
    const uint8_t* hi = nullptr;
};

const IMAGE_NT_HEADERS64* ntOf(uintptr_t base) {
    return reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
}

const IMAGE_SECTION_HEADER* sectionHeader(uintptr_t base, const char* name) {
    auto nt = ntOf(base);
    auto s = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++, s++) {
        if (strncmp(reinterpret_cast<const char*>(s->Name), name, IMAGE_SIZEOF_SHORT_NAME) == 0) return s;
    }
    return nullptr;
}

Range section(uintptr_t base, const char* name) {
    const IMAGE_SECTION_HEADER* s = sectionHeader(base, name);
    if (!s) return {};
    auto lo = reinterpret_cast<const uint8_t*>(base + s->VirtualAddress);
    return {lo, lo + s->Misc.VirtualSize};
}

// Adds delta to every 64-bit relocation the image's relocation directory lists inside .text (copy holds .text).
void relocate(uintptr_t base, const IMAGE_SECTION_HEADER* tx, std::vector<uint8_t>& copy, uint64_t delta) {
    const IMAGE_DATA_DIRECTORY& dir = ntOf(base)->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
    const uint8_t *p = reinterpret_cast<const uint8_t*>(base + dir.VirtualAddress), *end = p + dir.Size;
    while (dir.VirtualAddress && p + sizeof(IMAGE_BASE_RELOCATION) <= end) {
        auto block = reinterpret_cast<const IMAGE_BASE_RELOCATION*>(p);
        if (block->SizeOfBlock < sizeof *block || p + block->SizeOfBlock > end) break;
        auto e = reinterpret_cast<const WORD*>(block + 1);
        for (size_t i = 0, n = (block->SizeOfBlock - sizeof *block) / 2; i < n; i++) {
            uint64_t rva = block->VirtualAddress + (e[i] & 0xfff), at = rva - tx->VirtualAddress, v;
            if (e[i] >> 12 != IMAGE_REL_BASED_DIR64 || rva < tx->VirtualAddress || at + 8 > copy.size()) continue;
            memcpy(&v, copy.data() + at, 8);
            v += delta;
            memcpy(copy.data() + at, &v, 8);
        }
        p += block->SizeOfBlock;
    }
}

// .text as the image's file on disk holds it, with the loader's relocations applied: the code before any detour,
// SET's or another mod's (a plugin's MinHook on a function SET needs). False if the file isn't this image.
bool readText(uintptr_t base, std::vector<uint8_t>& out) {
    const IMAGE_SECTION_HEADER* tx = sectionHeader(base, ".text");
    wchar_t path[MAX_PATH];
    if (!tx || !GetModuleFileNameW(reinterpret_cast<HMODULE>(base), path, MAX_PATH)) return false;
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    auto readAt = [f](uint64_t off, void* dst, DWORD n) {
        LARGE_INTEGER at{};
        at.QuadPart = static_cast<LONGLONG>(off);
        DWORD got = 0;
        return SetFilePointerEx(f, at, nullptr, FILE_BEGIN) && ReadFile(f, dst, n, &got, nullptr) && got == n;
    };
    std::vector<uint8_t> head(4096);
    auto nt = ntOf(base);
    bool ok = readAt(0, head.data(), DWORD(head.size()));
    LONG lfanew = ok ? reinterpret_cast<const IMAGE_DOS_HEADER*>(head.data())->e_lfanew : 0;
    ok = ok && lfanew > 0 && size_t(lfanew) + sizeof(IMAGE_NT_HEADERS64) <= head.size();
    auto fileNt = ok ? reinterpret_cast<const IMAGE_NT_HEADERS64*>(head.data() + lfanew) : nullptr;
    ok = ok && fileNt->FileHeader.TimeDateStamp == nt->FileHeader.TimeDateStamp &&
         fileNt->OptionalHeader.SizeOfImage == nt->OptionalHeader.SizeOfImage;
    if (ok) {
        out.assign(tx->Misc.VirtualSize, 0);
        ok = readAt(tx->PointerToRawData, out.data(), std::min<DWORD>(tx->SizeOfRawData, tx->Misc.VirtualSize));
    }
    CloseHandle(f);
    if (!ok) return false;
    // The loader writes the real base into the mapped header, so the preferred one comes from the file's.
    uint64_t delta = base - fileNt->OptionalHeader.ImageBase;
    if (delta) relocate(base, tx, out, delta);
    return true;
}

// base's .text for pattern scans, read once per image: the clean file copy, or else the live bytes with SET's own
// detours undone (hk::restoreCode). A match at data + i is at live + i in the running image.
struct Code {
    const uint8_t* data = nullptr;
    size_t size = 0;
    const uint8_t* live = nullptr;
};

Code cleanText(uintptr_t base) {
    static std::mutex mx;  // the init thread and Lua's Hooks.observe both scan
    static std::unordered_map<uintptr_t, std::vector<uint8_t>> copies;
    Range tx = section(base, ".text");
    if (!tx.lo) return {};
    std::lock_guard lock(mx);
    auto [it, fresh] = copies.try_emplace(base);
    if (fresh && !readText(base, it->second)) {
        it->second.assign(tx.lo, tx.hi);
        hk::restoreCode(reinterpret_cast<uintptr_t>(tx.lo), it->second.data(), it->second.size());
    }
    return {it->second.data(), it->second.size(), tx.lo};
}

const uint8_t* findBytes(Range r, const void* needle, size_t len) {
    auto n = static_cast<const uint8_t*>(needle);
    auto it = std::search(r.lo, r.hi, n, n + len);
    return it == r.hi ? nullptr : it;
}

// First 8-aligned qword in r equal to v.
const uint8_t* findQword(Range r, uint64_t v) {
    auto a = reinterpret_cast<const uint8_t*>((reinterpret_cast<uintptr_t>(r.lo) + 7) & ~uintptr_t(7));
    for (; a + 8 <= r.hi; a += 8) {
        if (*reinterpret_cast<const uint64_t*>(a) == v) return a;
    }
    return nullptr;
}

// A descriptor starts with its name pointer, so the first pointer to the name string is the descriptor.
const uint8_t* descriptorNamed(Range rd, const char* zname, size_t len) {
    auto s = findBytes(rd, zname, len);
    return s ? findQword(rd, reinterpret_cast<uintptr_t>(s + 1)) : nullptr;
}

const uint8_t* const* gTable;
size_t gCount;
std::unordered_map<uint32_t, const uint8_t*> gByHash;
std::unordered_map<std::string_view, const uint8_t*> gByName;

refl::Field fieldAt(const uint8_t* fields, uint32_t i) {
    const uint8_t* f = fields + 0x30 * size_t(i);  // StructFieldMetadata: name, type ref, data offset
    auto name = *reinterpret_cast<const char* const*>(f);
    return {{name, *reinterpret_cast<const uint64_t*>(f + 8)},
            {*reinterpret_cast<const uint8_t* const*>(f + 0x10)},
            static_cast<uint32_t>(*reinterpret_cast<const uint64_t*>(f + 0x18))};
}

const uint8_t* fieldArray(refl::Type t) { return *reinterpret_cast<const uint8_t* const*>(t.p + 0x58); }

}  // namespace

std::string_view refl::Type::str(size_t off) const {
    auto s = *reinterpret_cast<const char* const*>(p + off);
    return s ? std::string_view(s, *reinterpret_cast<const uint64_t*>(p + off + 8)) : std::string_view();
}

refl::Type refl::Type::resolved() const {
    Type t = *this;
    for (int i = 0; i < 16 && t && t.prim() == Typedef && t.inner(); i++) t = t.inner();
    return t;
}

// TypeMetadata + 0x70: {bytes, length} (docs/reflection-format.md).
const uint8_t* refl::Type::defaults() const {
    auto d = *reinterpret_cast<const uint8_t* const*>(p + 0x70);
    return d && *reinterpret_cast<const uint64_t*>(p + 0x78) == size() ? d : nullptr;
}

// Same anchors as tools/reflect_dump.py: the BlobString and uint32 descriptors are the first two table entries,
// and a {types*, count} header in .rdata points at the table.
bool refl::init(uintptr_t base) {
    Range rd = section(base, ".rdata");
    auto a = rd.lo ? descriptorNamed(rd, "\0BlobString\0", 12) : nullptr;
    auto b = rd.lo ? descriptorNamed(rd, "\0uint32\0", 8) : nullptr;
    if (!a || !b) return false;
    const uint64_t pair[2] = {reinterpret_cast<uintptr_t>(a), reinterpret_cast<uintptr_t>(b)};
    auto arr = findBytes(rd, pair, sizeof pair);
    auto hdr = arr ? findQword(rd, reinterpret_cast<uintptr_t>(arr)) : nullptr;
    if (!hdr) return false;
    gTable = *reinterpret_cast<const uint8_t* const* const*>(hdr);
    gCount = reinterpret_cast<const uint64_t*>(hdr)[1];
    if (gCount < 1000 || gCount > 1000000 || gTable[0] != a) return false;
    gByHash.clear();
    gByName.clear();
    for (size_t i = 0; i < gCount; i++) {
        Type t{gTable[i]};
        gByHash.emplace(t.hash(), t.p);
        gByName.emplace(t.qname(), t.p);
    }
    return true;
}

size_t refl::count() { return gCount; }

refl::Type refl::at(size_t i) { return {i < gCount ? gTable[i] : nullptr}; }

refl::Type refl::byHash(uint32_t h) {
    auto it = gByHash.find(h);
    return {it == gByHash.end() ? nullptr : it->second};
}

refl::Type refl::byName(std::string_view name) {
    std::string q = name.find("::") == std::string_view::npos ? "keen::ecs::" + std::string(name) : std::string(name);
    auto it = gByName.find(q);
    return {it == gByName.end() ? nullptr : it->second};
}

// Limit: scans every type per call; a short-name map if mods call it every tick.
refl::Type refl::lookup(std::string_view name) {
    if (Type t = byName(name)) return t;
    Type any, outer;  // outer: not in keen::ds, the serialized twins
    int nAny = 0, nOuter = 0;
    for (size_t i = 0; i < gCount; i++) {
        Type t{gTable[i]};
        if (t.name() != name) continue;
        any = t;
        nAny++;
        if (!t.qname().starts_with("keen::ds::")) {
            outer = t;
            nOuter++;
        }
    }
    return nOuter == 1 ? outer : nAny == 1 ? any : Type{};
}

bool refl::plain(Type t) {
    t = t.resolved();
    uint8_t k = t ? t.prim() : 0;
    if (k == StaticArray) return t.inner() && plain(t.inner());
    if (k != Struct) return (k >= Bool && k <= Bitmask64) || k == ObjectReference || k == Guid;
    bool ok = true;
    forFields(t, [&](const Field& f) { ok = ok && plain(f.type); });
    return ok;
}

bool refl::field(Type t, std::string_view name, Field& out) {
    for (int depth = 0; t && t.prim() == Struct && depth < 32; depth++, t = t.inner()) {
        const uint8_t* fa = fieldArray(t);
        for (uint32_t i = 0; fa && i < t.fieldCount(); i++) {
            Field f = fieldAt(fa, i);
            if (f.name == name) {
                out = f;
                return true;
            }
        }
    }
    return false;
}

void refl::forFields(Type t, const std::function<void(const Field&)>& fn) {
    if (!t || t.prim() != Struct) return;
    forFields(t.inner(), fn);
    const uint8_t* fa = fieldArray(t);
    for (uint32_t i = 0; fa && i < t.fieldCount(); i++) fn(fieldAt(fa, i));
}

void refl::forEnum(Type t, const std::function<void(std::string_view, int64_t)>& fn) {
    auto ea = t ? *reinterpret_cast<const uint8_t* const*>(t.p + 0x60) : nullptr;
    for (uint32_t i = 0; ea && i < t.fieldCount(); i++) {
        const uint8_t* e = ea + 0x28 * size_t(i);  // EnumFieldMetadata: name, then the value at 0x10
        fn({*reinterpret_cast<const char* const*>(e), *reinterpret_cast<const uint64_t*>(e + 8)},
           *reinterpret_cast<const int64_t*>(e + 0x10));
    }
}

// Nested wrappers unwrap too. A wrapper of an array stays a struct, and so does a struct whose one field has a name of
// its own, like the user settings' GraphicsSettings {fov}: 776 types have one field, and only value, id and index
// mark a wrapper (HashKey32, Time, EntityId, handles).
refl::Type refl::unwrap(Type u) {
    for (int i = 0; i < 4 && u.prim() == Struct && !u.inner() && u.fieldCount() == 1; i++) {
        Field only{};
        forFields(u, [&](const Field& f) { only = f; });
        Type v = only.type.resolved();
        bool named = only.name != "value" && only.name != "id" && only.name != "index";
        if (named || only.offset != 0 || !v || v.size() != u.size() || v.prim() == StaticArray) break;
        u = v;
    }
    return u;
}

uint32_t refl::fnv(std::string_view s) {
    uint32_t h = 0x811c9dc5;
    for (unsigned char c : s) h = (h ^ c) * 0x01000193;
    return h;
}

bool refl::enumHas(Type t, int64_t v) {
    bool found = t.fieldCount() == 0;
    forEnum(t, [&](std::string_view, int64_t e) { found |= e == v; });
    return found;
}

std::string refl::text(const uint8_t* p, Type u) {
    uint8_t k = u.prim();
    char buf[64];
    if (k == Bool) return *p ? "true" : "false";
    if (k == F32 || k == F64) {
        double d = 0;
        if (k == F32) {
            float f;
            memcpy(&f, p, 4);
            d = f;
        } else {
            memcpy(&d, p, 8);
        }
        snprintf(buf, sizeof buf, "%g", d);
        return buf;
    }
    uint64_t v = 0;
    uint32_t n = std::min<uint32_t>(u.size(), 8);
    memcpy(&v, p, n);
    bool sign = k == S8 || k == S16 || k == S32 || k == S64;
    if (sign && n < 8 && (v >> (n * 8 - 1) & 1)) v |= ~0ull << (n * 8);
    bool mask = k >= Bitmask8 && k <= Bitmask64;
    snprintf(buf, sizeof buf, mask ? "0x%llx" : sign ? "%lld" : "%llu", static_cast<unsigned long long>(v));
    std::string s = buf;
    if (k == Enum) {
        forEnum(u, [&](std::string_view name, int64_t e) {
            if (uint64_t(e) == v) s = std::string(name) + " (" + s + ")";
        });
    }
    return s;
}

refl::Field refl::badField(Type t, const uint8_t* p) {
    Field bad{};
    forFields(t, [&](const Field& f) {
        Type u = unwrap(f.type.resolved());
        if (!bad.name.empty()) return;
        if (u.prim() == Struct) {
            bad = badField(u, p + f.offset);
            bad.offset += f.offset;
            return;
        }
        uint64_t v = 0;
        memcpy(&v, p + f.offset, std::min<uint32_t>(u.size(), 8));
        if (u.prim() == Bool ? v > 1 : u.prim() == Enum && !enumHas(u, int64_t(v))) bad = f;
    });
    return bad;
}

uint32_t gameBuild(uintptr_t base) {
    auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    return reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew)->FileHeader.TimeDateStamp;
}

uintptr_t findUnique(uintptr_t base, const uint8_t* pat, size_t len) {
    Code c = cleanText(base);
    Range tx{c.data, c.data + c.size};
    const uint8_t* hit = nullptr;
    for (const uint8_t* a = tx.lo; a && (a = findBytes({a, tx.hi}, pat, len)) != nullptr; a++) {
        if (hit) return 0;  // a second match: ambiguous
        hit = a;
    }
    return hit ? reinterpret_cast<uintptr_t>(c.live + (hit - c.data)) : 0;
}

namespace {
int hexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool parsePattern(std::string_view text, std::vector<int>& bytes) {
    bool exact = false;
    for (size_t i = 0; i < text.size();) {
        if (isspace(static_cast<unsigned char>(text[i]))) { ++i; continue; }
        if (i + 2 > text.size() || bytes.size() == 256) return false;
        char a = text[i++], b = text[i++];
        if (i < text.size() && !isspace(static_cast<unsigned char>(text[i]))) return false;
        if (a == '?' && b == '?') bytes.push_back(-1);
        else {
            int hi = hexDigit(a), lo = hexDigit(b);
            if (hi < 0 || lo < 0) return false;
            bytes.push_back(hi * 16 + lo);
            exact = true;
        }
    }
    return exact;
}
}  // namespace

uintptr_t hk::findPattern(uintptr_t base, std::string_view pattern, std::string& err) {
    err.clear();
    std::vector<int> bytes;
    if (!parsePattern(pattern, bytes)) {
        err = "malformed pattern: use 1..256 byte pairs or ??, with at least one exact byte";
        return 0;
    }
    Code code = cleanText(base);
    uintptr_t hit = 0;
    if (code.data && bytes.size() <= code.size) {
        for (const uint8_t* p = code.data; p <= code.data + code.size - bytes.size(); ++p) {
            size_t i = 0;
            while (i < bytes.size() && (bytes[i] < 0 || bytes[i] == p[i])) ++i;
            if (i != bytes.size()) continue;
            if (hit) { err = "ambiguous pattern: more than one match in .text"; return 0; }
            hit = reinterpret_cast<uintptr_t>(code.live) + (p - code.data);
        }
    }
    if (!hit) err = "pattern not found in .text";
    return hit;
}

// A system descriptor in .rdata holds its function pointer; fn + 0x88 holds {entries, count} of the resources it reads
// and fn + 0xa0 those it writes, each entry {char* name, u64 length}. The system's query keeps all of them in one
// group, reads first (admin_player's ecs::GameSettings is 4 of 6).
int writtenResource(uintptr_t base, uintptr_t fn, std::string_view name, uint32_t& total) {
    Range rd = section(base, ".rdata");
    auto slot = rd.lo ? findQword(rd, fn) : nullptr;
    if (!slot || findQword({slot + 8, rd.hi}, fn)) return -1;
    auto reads = static_cast<uint32_t>(*reinterpret_cast<const uint64_t*>(slot + 0x90));
    auto writes = static_cast<uint32_t>(*reinterpret_cast<const uint64_t*>(slot + 0xa8));
    auto list = *reinterpret_cast<const uint8_t* const*>(slot + 0xa0);
    total = reads + writes;
    for (uint32_t i = 0; list && reads < 64 && i < writes && i < 64; i++) {
        const uint8_t* e = list + 0x10 * size_t(i);
        auto s = *reinterpret_cast<const char* const*>(e);
        if (s && std::string_view(s, *reinterpret_cast<const uint64_t*>(e + 8)) == name) return int(reads + i);
    }
    return -1;
}

// A system descriptor starts {char* name, u64 length, fn}: admin_player's gives the admin consumer.
uintptr_t systemFn(uintptr_t base, std::string_view name) {
    Range rd = section(base, ".rdata"), tx = section(base, ".text");
    std::string z = '\0' + std::string(name) + '\0';
    auto s = rd.lo ? findBytes(rd, z.data(), z.size()) : nullptr;
    if (!s || findBytes({s + 1, rd.hi}, z.data(), z.size())) return 0;
    auto d = findQword(rd, reinterpret_cast<uintptr_t>(s + 1));
    if (!d || d + 24 > rd.hi || findQword({d + 8, rd.hi}, reinterpret_cast<uintptr_t>(s + 1))) return 0;
    auto fn = *reinterpret_cast<const uintptr_t*>(d + 16);
    return fn >= reinterpret_cast<uintptr_t>(tx.lo) && fn < reinterpret_cast<uintptr_t>(tx.hi) ? fn : 0;
}

namespace {

std::vector<sys::System> gSys;
std::vector<std::pair<uintptr_t, size_t>> gSysByFn;  // fn, index in gSys; sorted by fn for the runner hook
std::unique_ptr<std::atomic<bool>[]> gSysOff;
std::atomic<size_t> gSysOffN;
// The profiler, per system: calls, QPC ticks and world-kind bits since profiling started.
std::unique_ptr<std::atomic<uint64_t>[]> gSysCalls, gSysTicks;
std::unique_ptr<std::atomic<uint8_t>[]> gSysKinds;
std::atomic<bool> gProf;
std::atomic<int64_t> gProfStart, gProfStop;

int64_t qpc() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

double qpcMs(int64_t ticks) {
    static const int64_t freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f.QuadPart;
    }();
    return double(ticks) * 1000.0 / double(freq);
}

// The job's system is its program's update function, *(*(job + 0x10) + 0x28), as the runner calls it. Its index in
// gSys, or -1 for a function that isn't a named system.
ptrdiff_t sysIndex(const void* job) {
    auto prog = *reinterpret_cast<const uintptr_t*>(reinterpret_cast<uintptr_t>(job) + 0x10);
    auto fn = *reinterpret_cast<const uintptr_t*>(prog + 0x28);
    auto it = std::lower_bound(gSysByFn.begin(), gSysByFn.end(), std::pair<uintptr_t, size_t>(fn, 0));
    return it != gSysByFn.end() && it->first == fn ? ptrdiff_t(it->second) : -1;
}

bool inRange(Range r, uintptr_t p) {
    return p >= reinterpret_cast<uintptr_t>(r.lo) && p < reinterpret_cast<uintptr_t>(r.hi);
}

// A system name: lowercase letters, digits and underscores ("inventory_actions"). Other text can sit in the same
// {char*, length, fn} shape, such as a resource's description.
bool sysName(std::string_view s) {
    return !s.empty() && s[0] >= 'a' && s[0] <= 'z' &&
           std::all_of(s.begin(), s.end(), [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'; });
}

}  // namespace

// Every .rdata descriptor {char* name, u64 length, fn} whose name is a system name in .rdata, ending in a zero, and
// whose fn is in .text: 861 in the tested builds.
size_t sys::scan(uintptr_t base) {
    Range rd = section(base, ".rdata"), tx = section(base, ".text");
    gSys.clear();
    auto a = reinterpret_cast<const uint8_t*>((reinterpret_cast<uintptr_t>(rd.lo) + 7) & ~uintptr_t(7));
    for (; rd.lo && a + 24 <= rd.hi; a += 8) {
        auto q = reinterpret_cast<const uint64_t*>(a);
        auto s = reinterpret_cast<const char*>(q[0]);
        if (q[1] == 0 || q[1] > 127 || !inRange(rd, q[0]) || !inRange(rd, q[0] + q[1]) || !inRange(tx, q[2]) || s[q[1]])
            continue;
        if (sysName({s, q[1]})) gSys.push_back({{s, q[1]}, q[2]});
    }
    std::sort(gSys.begin(), gSys.end(), [](const System& x, const System& y) { return x.name < y.name; });
    gSysByFn.clear();
    for (size_t i = 0; i < gSys.size(); i++) gSysByFn.emplace_back(gSys[i].fn, i);
    std::sort(gSysByFn.begin(), gSysByFn.end());
    gSysOff = std::make_unique<std::atomic<bool>[]>(gSys.size());
    gSysOffN = 0;
    gSysCalls = std::make_unique<std::atomic<uint64_t>[]>(gSys.size());
    gSysTicks = std::make_unique<std::atomic<uint64_t>[]>(gSys.size());
    gSysKinds = std::make_unique<std::atomic<uint8_t>[]>(gSys.size());
    gProf = false;
    return gSys.size();
}

const std::vector<sys::System>& sys::all() { return gSys; }

const sys::System* sys::find(std::string_view name) {
    auto it = std::lower_bound(gSys.begin(), gSys.end(), name, [](const System& s, std::string_view n) { return s.name < n; });
    return it != gSys.end() && it->name == name ? &*it : nullptr;
}

bool sys::isOn(const System& s) { return !gSysOff[size_t(&s - gSys.data())]; }

void sys::setOn(const System& s, bool on) {
    if (gSysOff[size_t(&s - gSys.data())].exchange(!on) == !on) return;
    if (on) gSysOffN--;
    else gSysOffN++;
}

size_t sys::offCount() { return gSysOffN; }

bool sys::skip(const void* job) {
    if (!gSysOffN.load(std::memory_order_relaxed)) return false;
    ptrdiff_t i = sysIndex(job);
    return i >= 0 && gSysOff[size_t(i)].load(std::memory_order_relaxed);
}

void sys::setProfiling(bool on) {
    if (on) {
        for (size_t i = 0; i < gSys.size(); i++) {
            gSysCalls[i] = 0;
            gSysTicks[i] = 0;
            gSysKinds[i] = 0;
        }
        gProfStart = qpc();
    } else if (gProf) {
        gProfStop = qpc();
    }
    gProf = on;
}

bool sys::profiling() { return gProf.load(std::memory_order_relaxed); }

void sys::record(const void* job, uint8_t worldKind, int64_t ticks) {
    if (!gProf.load(std::memory_order_relaxed)) return;
    ptrdiff_t i = sysIndex(job);
    if (i < 0) return;
    gSysCalls[size_t(i)].fetch_add(1, std::memory_order_relaxed);
    gSysTicks[size_t(i)].fetch_add(uint64_t(ticks), std::memory_order_relaxed);
    if (worldKind < 8) gSysKinds[size_t(i)].fetch_or(uint8_t(1u << worldKind), std::memory_order_relaxed);
}

std::vector<sys::Stat> sys::stats() {
    std::vector<Stat> out;
    for (size_t i = 0; i < gSys.size(); i++) {
        uint64_t n = gSysCalls[i].load(std::memory_order_relaxed);
        if (n) out.push_back({&gSys[i], n, qpcMs(int64_t(gSysTicks[i].load(std::memory_order_relaxed))), gSysKinds[i]});
    }
    std::sort(out.begin(), out.end(), [](const Stat& a, const Stat& b) { return a.ms > b.ms; });
    return out;
}

double sys::profiledSecs() {
    if (!gProfStart) return 0;
    return qpcMs((gProf ? qpc() : gProfStop.load()) - gProfStart) / 1000.0;
}

namespace {

std::string gCmdA;
std::wstring gCmdW;
LPSTR WINAPI cmdLineA() { return gCmdA.data(); }
LPWSTR WINAPI cmdLineW() { return gCmdW.data(); }

}  // namespace

// Walks the exe's import table by name. The originals come through this module's own imports, so they are unchanged.
bool addLaunchFlag(uintptr_t base, const char* flag) {
    gCmdA = std::string(GetCommandLineA()) + ' ' + flag;
    gCmdW = std::wstring(GetCommandLineW()) + L' ' + std::wstring(flag, flag + strlen(flag));
    auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    int swapped = 0;
    auto d = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress);
    for (; dir.VirtualAddress && d->Name; d++) {
        if (!d->OriginalFirstThunk) continue;  // names only in the bound slots: not ours to guess
        auto names = reinterpret_cast<const IMAGE_THUNK_DATA64*>(base + d->OriginalFirstThunk);
        auto slots = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + d->FirstThunk);
        for (; names->u1.AddressOfData; names++, slots++) {
            if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal)) continue;
            const char* fn = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData)->Name;
            uintptr_t to = strcmp(fn, "GetCommandLineA") == 0   ? reinterpret_cast<uintptr_t>(&cmdLineA)
                           : strcmp(fn, "GetCommandLineW") == 0 ? reinterpret_cast<uintptr_t>(&cmdLineW)
                                                                : 0;
            DWORD old;
            if (!to || !VirtualProtect(&slots->u1.Function, 8, PAGE_READWRITE, &old)) continue;
            slots->u1.Function = to;
            VirtualProtect(&slots->u1.Function, 8, old, &old);
            swapped++;
        }
    }
    return swapped == 2;
}

// Rows look like "[I 00:00:08,464] message"; the message decides.
bool logNoise(std::string_view row) {
    constexpr auto npos = std::string_view::npos;
    size_t end = row.starts_with('[') ? row.find(']') : npos;
    if (end != npos) row.remove_prefix(end + 1);
    row.remove_prefix(std::min(row.find_first_not_of(" \t"), row.size()));
    if (row.empty() || row.starts_with("triggered '")) return true;
    return row[0] == '\'' &&
           (row.find("'::Enter #") != npos || row.find("'::Execute #") != npos || row.find("'::Exit #") != npos);
}

// ---- The game's options ----

namespace {

std::atomic<uintptr_t> gOptions;  // the options object, once attach checked it
uint32_t gAaAt;                   // graphics.render.antiAliasing in the machine settings, 0 if not found
std::atomic<bool> gAaOff;
int32_t gAaWas = -1;              // the game's antialiasing while it is kept at None; main thread only
using CamPushFn = void (*)(uintptr_t options, uint8_t* slot);
CamPushFn gCamPush;               // set before the options hook goes in
uint32_t gFovAt;                  // graphics.fov in the user settings, 0 if not found
float gFovWas;                    // the field of view the camera last got; main thread only
char gWhy[160];

uint32_t pathOffset(refl::Type t, std::initializer_list<std::string_view> path) {
    uint32_t off = 0;
    for (std::string_view name : path) {
        refl::Field f;
        if (!refl::field(t, name, f)) return 0;
        off += f.offset;
        t = f.type.resolved();
    }
    return off;
}

// The scalars of the struct t at p as "path = value" lines. d holds the same struct in the game's defaults, laid out
// as dt (the settings type's non-ds twin), matched by field name: a value that differs gets " (default X)", one the
// twin lacks " (default unknown)".
void dumpStruct(const uint8_t* p, refl::Type t, const uint8_t* d, refl::Type dt, const std::string& path,
                std::string_view prefix, size_t& n, const std::function<void(const std::string&)>& fn) {
    refl::forFields(t, [&](const refl::Field& f) {
        refl::Type u = refl::unwrap(f.type.resolved());
        std::string at = path + '.' + std::string(f.name);
        refl::Field df{};
        bool has = d && refl::field(dt, f.name, df);
        refl::Type du = has ? refl::unwrap(df.type.resolved()) : refl::Type{};
        if (u.prim() == refl::Struct) {
            bool sub = has && du.prim() == refl::Struct;
            return dumpStruct(p + f.offset, u, sub ? d + df.offset : nullptr, du, at, prefix, n, fn);
        }
        if (u.prim() < refl::Bool || u.prim() > refl::Bitmask64 || !at.starts_with(prefix)) return;
        std::string line = at + " = " + refl::text(p + f.offset, u);
        if (!has || du.prim() != u.prim() || du.size() != u.size()) line += " (default unknown)";
        else if (memcmp(p + f.offset, d + df.offset, u.size()) != 0) line += " (default " + refl::text(d + df.offset, u) + ")";
        fn(line);
        n++;
    });
}

// The game's defaults for a settings type sit in its twin outside keen::ds.
refl::Type defaultsTwin(refl::Type t) {
    std::string q(t.qname());
    if (!q.starts_with("keen::ds::")) return {};
    return refl::byName(q.erase(6, 4));
}

}  // namespace

refl::Type opts::machineType() { return refl::byName("keen::ds::g38_options::pc::machine::Settings"); }
refl::Type opts::userType() { return refl::byName("keen::ds::g38_options::pc::user::Settings"); }

// The game's user-slot getter: imul eax, eax, slot size; add rax, first slot; ...; cmp [rax + has - 8], cl.
bool opts::findLayout(uintptr_t base) {
    std::string err;
    uintptr_t at = hk::findPattern(base,
                                   "48 85 C9 74 ?? 85 D2 74 ?? 8B C2 83 E0 0F 48 69 C0 ?? ?? ?? ?? 48 05 ?? ?? ?? ?? "
                                   "48 03 C1 39 10 75 ?? 48 85 C0 74 ?? 48 83 C0 08 33 C9 38 88 ?? ?? ?? ??", err);
    if (!at) return false;
    auto u32 = [at](uintptr_t off) { return *reinterpret_cast<const uint32_t*>(at + off); };
    slotSize = u32(17);
    slotsAt = u32(23);
    hasAt = u32(47) + 8;
    return true;
}

const char* opts::attach(uintptr_t o) {
    refl::Type mt = machineType(), ut = userType();
    if (!slotSize) return "the user settings layout wasn't found";
    if (!mt || !ut || mt.size() > slotsAt - kMachine || ut.size() != hasAt - 8) return "the settings types differ";
    if (!o) return "no options object yet";
    auto m = reinterpret_cast<const uint8_t*>(o + kMachine);
    if (refl::Field f = refl::badField(mt, m); !f.name.empty()) {
        snprintf(gWhy, sizeof gWhy, "machine field %.*s = %s", int(f.name.size()), f.name.data(),
                 refl::text(m + f.offset, refl::unwrap(f.type.resolved())).c_str());
        return gWhy;
    }
    gAaAt = pathOffset(mt, {"graphics", "render", "antiAliasing"});
    gFovAt = pathOffset(ut, {"graphics", "fov"});
    gOptions = o;
    return nullptr;
}

bool opts::ready() { return gOptions != 0; }
uint8_t* opts::machine() { return gOptions ? reinterpret_cast<uint8_t*>(gOptions + kMachine) : nullptr; }

// The slot is looked up on every call, so a user signing in again can't leave a stale pointer.
uint8_t* opts::user() {
    refl::Type ut = userType();
    for (uintptr_t i = 0, o = gOptions; o && ut && i < 16; i++) {
        auto slot = reinterpret_cast<uint8_t*>(o + slotsAt + i * slotSize);
        if (*reinterpret_cast<const uint32_t*>(slot) && slot[hasAt] == 1 && refl::badField(ut, slot + 8).name.empty())
            return slot + 8;
    }
    return nullptr;
}

namespace {

// The camera keeps its own copy of the field of view, which only the settings menu's apply refreshes. So a changed
// value goes through the same push the menu runs.
void pushFov() {
    uint8_t* u = gCamPush && gFovAt ? opts::user() : nullptr;
    if (!u) return;
    float fov = *reinterpret_cast<const float*>(u + gFovAt);
    if (fov != gFovWas) gCamPush(gOptions, u - 8);
    gFovWas = fov;
}

}  // namespace

const char* opts::frame(uintptr_t app) {
    if (!gOptions) {
        if (const char* why = attach(*reinterpret_cast<const uintptr_t*>(app + kAppOptions))) return why;
    }
    pushFov();
    if (!gAaAt) return nullptr;
    auto& aa = *reinterpret_cast<int32_t*>(gOptions + kMachine + gAaAt);
    if (gAaOff && aa != 0) {  // also after the game's menu applied another choice
        gAaWas = aa;
        aa = 0;
    } else if (!gAaOff && gAaWas >= 0) {
        if (aa == 0) aa = gAaWas;
        gAaWas = -1;
    }
    return nullptr;
}

void opts::setAaOff(bool off) { gAaOff = off; }
void opts::setCamPush(uintptr_t fn) { gCamPush = reinterpret_cast<CamPushFn>(fn); }

size_t opts::dump(std::string_view prefix, const std::function<void(const std::string&)>& fn) {
    size_t n = 0;
    auto one = [&](const uint8_t* p, refl::Type t, const char* path) {
        refl::Type dt = defaultsTwin(t);
        if (p) dumpStruct(p, t, dt ? dt.defaults() : nullptr, dt, path, prefix, n, fn);
    };
    one(machine(), machineType(), "machine");
    one(user(), userType(), "user");
    return n;
}

// ---- ECS ----

namespace {

constexpr uintptr_t kEntityMap = 0x148, kCompMap = 0x88;  // world + kEntityMap, schema (= *world) + kCompMap
constexpr uintptr_t kRecArch = 0x18, kRecChunk = 0x20, kRecRow = 0x30;
constexpr uintptr_t kArchOffsets = 0x84, kArchStrides = 0xa84;
constexpr int kMaxComp = 1024;  // the archetype bitmask ends at 0x84: whole qwords cover 1024 indices

template <class T>
T rd(uintptr_t a) { return *reinterpret_cast<const T*>(a); }

// keen hash map: +8 occupancy bitmap, +0x10 capacity (power of two), +0x28 u32 keys, +0x40 values, +0x54 count.
struct Map {
    uintptr_t m;
    uint64_t cap() const { return rd<uint64_t>(m + 0x10); }
    uint32_t count() const { return rd<uint32_t>(m + 0x54); }
    bool used(uint64_t s) const { return rd<const uint64_t*>(m + 8)[s >> 6] >> (s & 63) & 1; }
    uint32_t key(uint64_t s) const { return rd<const uint32_t*>(m + 0x28)[s]; }
    template <class V>
    V val(uint64_t s) const { return rd<const V*>(m + 0x40)[s]; }

    // Slot holding k, or -1, probed the way the game's own lookups probe (raw keys, or keys through mix()).
    int64_t find(uint32_t k, bool mixed) const {
        uint64_t c = cap();
        if (!count() || !c || (c & (c - 1))) return -1;
        uint64_t s = (mixed ? ecs::mix(k) : k) & (c - 1);
        for (uint64_t n = 0; n < c && used(s); n++, s = (s + 1) & (c - 1)) {
            if (key(s) == k) return static_cast<int64_t>(s);
        }
        return -1;
    }
};

// Component idx of the entity behind record rec: chunk + offset[idx] + row * stride[idx], if its archetype has it.
uint8_t* inRecord(uintptr_t rec, int idx) {
    auto arch = rd<uintptr_t>(rec + kRecArch);
    auto i = static_cast<uintptr_t>(idx);
    if (!arch || !(rd<uint64_t>(arch + (i >> 6) * 8) >> (i & 63) & 1)) return nullptr;
    uintptr_t off = rd<uint16_t>(arch + kArchOffsets + i * 2), stride = rd<uint16_t>(arch + kArchStrides + i * 2);
    return reinterpret_cast<uint8_t*>(rd<uintptr_t>(rec + kRecChunk) + off + rd<uint32_t>(rec + kRecRow) * stride);
}

}  // namespace

uint32_t ecs::mix(uint32_t k) {
    k = ((k >> 16) ^ k) * 0x45d9f3bu;
    k = ((k >> 16) ^ k) * 0x45d9f3bu;
    return (k >> 16) ^ k;
}

int ecs::compIndex(uintptr_t world, uint32_t typeHash) {
    Map m{rd<uintptr_t>(world) + kCompMap};
    int64_t s = m.find(typeHash, false);
    int idx = s < 0 ? -1 : m.val<uint16_t>(static_cast<uint64_t>(s));
    return idx < kMaxComp ? idx : -1;
}

uint8_t* ecs::component(uintptr_t world, uint32_t entity, int idx) {
    if (idx < 0 || idx >= kMaxComp) return nullptr;
    Map m{world + kEntityMap};
    int64_t s = m.find(entity, true);
    auto rec = s < 0 ? 0 : m.val<uintptr_t>(static_cast<uint64_t>(s));
    return rec ? inRecord(rec, idx) : nullptr;
}

uint32_t ecs::firstWith(uintptr_t world, int idx) {
    Map m{world + kEntityMap};
    if (idx < 0 || idx >= kMaxComp || !m.count()) return 0;
    for (uint64_t s = 0, c = m.cap(); s < c; s++) {
        auto rec = m.used(s) ? m.val<uintptr_t>(s) : 0;
        if (rec && inRecord(rec, idx)) return m.key(s);
    }
    return 0;
}

std::vector<uint32_t> ecs::allWith(uintptr_t world, const int* idx, size_t n) {
    std::vector<uint32_t> out;
    Map m{world + kEntityMap};
    if (!m.count() || std::any_of(idx, idx + n, [](int i) { return i < 0 || i >= kMaxComp; })) return out;
    for (uint64_t s = 0, c = m.cap(); s < c; s++) {
        auto rec = m.used(s) ? m.val<uintptr_t>(s) : 0;
        if (rec && std::all_of(idx, idx + n, [&](int i) { return inRecord(rec, i) != nullptr; })) out.push_back(m.key(s));
    }
    return out;
}

uint32_t ecs::entityCount(uintptr_t world) { return Map{world + kEntityMap}.count(); }

// The layout the game's query fill writes. The query is the job, then per-entity slots (counts at sys + 0x4b0, 0x4b8,
// and 0x4ba for six-slot entries), then four groups of the system's resource pointers: *(sys + 0xc0) with its count
// byte 0x400 past that array, sys + 0x7a8 (count byte sys + 0x7f0), sys + 0x4c0 (count byte sys + 0x7a0), and
// *(sys + 0x1a0) (count sys + 0x1a8). sys is the system instance, *job.
bool ecs::querySlot(const void* job, uint32_t slot, ResSlot& out) {
    auto sys = rd<uintptr_t>(reinterpret_cast<uintptr_t>(job));
    uint64_t i = 1ull + rd<uint8_t>(sys + 0x4b0) + rd<uint16_t>(sys + 0x4b8) + 6ull * rd<uint16_t>(sys + 0x4ba);
    auto first = rd<uintptr_t>(sys + 0xc0);
    const std::pair<uintptr_t, uint64_t> groups[] = {{first, rd<uint8_t>(first + 0x400)},
                                                     {sys + 0x7a8, rd<uint8_t>(sys + 0x7f0)},
                                                     {sys + 0x4c0, rd<uint8_t>(sys + 0x7a0)},
                                                     {rd<uintptr_t>(sys + 0x1a0), rd<uint64_t>(sys + 0x1a8)}};
    for (auto [arr, n] : groups) {
        if (slot >= i && slot < i + n) {
            out = {rd<uintptr_t>(arr + 8 * (slot - i)), n, slot - i};
            return out.ptr != 0;
        }
        i += n;
    }
    return false;
}

// ---- Events ----

namespace {

uintptr_t gEventMap, gStreams;  // schema + gEventMap: hash -> event index; world + gStreams: the streams the session reads
// From world + gStreams: the stream count, and the pending-event buffers (poster 0 = the session's).
constexpr uintptr_t kStreamCount = 8, kPosters = 0x70;
constexpr uintptr_t kStreamStride = 0x48, kPageSize = 0x800, kPageEnd = 0x7f8;
constexpr uintptr_t kTickLength = 0x4c0, kTickIndex = 0x4dc;  // world: sim time = length * index

// Stream of event index idx: +0x14 element size, +0x18 count, +0x30 page pool, +0x40 first page (1-based).
uintptr_t stream(uintptr_t world, int idx) { return rd<uintptr_t>(world + gStreams) + uintptr_t(idx) * kStreamStride; }

}  // namespace

// One function of the game loads both next to each other: lea r14, [rdi + map]; lea rdi, [r13 + streams - 0x28].
bool ecs::findEvents(uintptr_t base) {
    std::string err;
    uintptr_t at =
        hk::findPattern(base, "4C 8B FB 0F 86 ?? ?? ?? ?? 48 8B 45 40 4C 8D B7 ?? ?? ?? ?? 49 8D BD ?? ?? ?? ??", err);
    if (!at) return false;
    gEventMap = rd<uint32_t>(at + 16);
    gStreams = rd<uint32_t>(at + 23) + 0x28;
    return true;
}

int ecs::eventIndex(uintptr_t world, uint32_t typeHash) {
    if (!gEventMap) return -1;
    Map m{rd<uintptr_t>(world) + gEventMap};
    int64_t s = m.find(typeHash, false);
    int idx = s < 0 ? -1 : m.val<uint16_t>(static_cast<uint64_t>(s));
    return idx >= 0 && uint64_t(idx) < rd<uint64_t>(world + gStreams + kStreamCount) ? idx : -1;
}

// The walk the session's own reader does: events packed from page + 8, next page index at page + 2.
void ecs::forEachEvent(uintptr_t world, int idx, const std::function<void(const uint8_t* ev)>& fn) {
    if (idx < 0 || !gStreams || uint64_t(idx) >= rd<uint64_t>(world + gStreams + kStreamCount)) return;
    uintptr_t st = stream(world, idx), pool = rd<uintptr_t>(st + 0x30);
    uint32_t size = rd<uint32_t>(st + 0x14), count = rd<uint32_t>(st + 0x18), off = 0;
    uint16_t page = rd<uint16_t>(st + 0x40);
    if (size == 0 || size >= 0x100) return;
    for (uint32_t i = 0; i < count && page; i++) {
        if (off + size > kPageEnd) {
            page = rd<uint16_t>(pool + (page - 1u) * kPageSize + 2);
            off = 0;
            if (!page) return;
        }
        fn(reinterpret_cast<const uint8_t*>(pool + (page - 1u) * kPageSize + 8 + off));
        off += size;
    }
}

// The record the game's poster writes, on poster 0 ({buf, cap, used}): {u32 hash, u16 tag 0, u8 size, u8 0}, then the
// payload rounded up to 8 bytes. A hash the world doesn't know would crash the game's distributor, hence the index
// check.
uint8_t* ecs::postEvent(uintptr_t world, uint32_t typeHash, uint32_t size) {
    int idx = eventIndex(world, typeHash);
    uint32_t padded = (size + 7) & ~7u;
    if (idx < 0 || size == 0 || size >= 0x100 || rd<uint32_t>(stream(world, idx) + 0x14) > padded) return nullptr;
    auto poster = rd<uintptr_t>(world + gStreams + kPosters);
    auto buf = poster ? rd<uint8_t*>(poster) : nullptr;
    uint32_t used = poster ? rd<uint32_t>(poster + 0x10) : 0, end = used + 8 + padded;
    if (!buf || end > rd<uint64_t>(poster + 8)) return nullptr;
    uint8_t* rec = buf + used;
    memcpy(rec, &typeHash, 4);
    rec[4] = rec[5] = rec[7] = 0;
    rec[6] = static_cast<uint8_t>(size);
    memset(rec + 8, 0, padded);
    *reinterpret_cast<uint32_t*>(poster + 0x10) = end;
    return rec + 8;
}

int64_t ecs::simTime(uintptr_t world) {
    auto len = rd<int64_t>(world + kTickLength);
    uint32_t n = rd<uint32_t>(world + kTickIndex);
    if (!n) return 0;
    return len == INT64_MAX || len == INT64_MIN ? len : len * n;  // saturated values stay as they are
}

// ---- Resources ----

namespace {

std::vector<res::Entry> gEntries;  // sorted by guid, type, part
std::unordered_map<uint32_t, std::vector<const res::Entry*>> gOfType;
uint64_t gDataSize;  // the uncompressed size of all resources
uintptr_t gData;
uint32_t gProtect;
std::atomic<bool> gResReady;
std::mutex gWriteMx;  // two writers on one page must not put its protection back under each other

// The .dat content map from enshrouded.kfc: ContentHash {size, hash0-2} -> where the content starts.
struct Content {
    uint8_t hash[16];
    uint32_t off;
    uint16_t container;  // enshrouded_<container>.dat
};
std::vector<Content> gContent;
size_t gContainers;

// The English text table, a LocaTagCollectionResourceData blob: an array of LocaTagResource {id, text, ...}.
std::vector<uint8_t> gText;
uint32_t gTextAt, gTextCount, gTextRec, gTextId, gTextStr;  // array start, records, record size, field offsets
std::atomic<bool> gTextReady;

bool entryLess(const res::Entry& a, const res::Entry& b) {
    int c = memcmp(a.guid, b.guid, 16);
    return c ? c < 0 : a.type != b.type ? a.type < b.type : a.part < b.part;
}

// Header location i: {u32 offset from the field itself, u32 count}. False unless count elements of elem bytes fit.
bool kfcLoc(const uint8_t* f, size_t n, int i, size_t elem, size_t& off, size_t& count) {
    size_t at = 16 + size_t(i) * 8;
    uint32_t rel, cnt;
    memcpy(&rel, f + at, 4);
    memcpy(&cnt, f + at + 4, 4);
    off = at + rel;
    count = cnt;
    return rel && off <= n && count <= (n - off) / elem;
}

// [a, a + n) committed and readable. prot gets the first region's protection.
bool readable(uintptr_t a, uint64_t n, uint32_t& prot) {
    constexpr DWORD kRead = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                            PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    prot = 0;
    for (uintptr_t p = a; p < a + n;) {
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery(reinterpret_cast<void*>(p), &mi, sizeof mi) || mi.State != MEM_COMMIT ||
            !(mi.Protect & kRead) || (mi.Protect & PAGE_GUARD))
            return false;
        if (!prot) prot = mi.Protect;
        p = reinterpret_cast<uintptr_t>(mi.BaseAddress) + mi.RegionSize;
    }
    return true;
}

// Counts e's top-level strings at data: good if in bounds and text, bad if not. Empty ones count as neither.
void checkStrings(uintptr_t data, const res::Entry& e, int& good, int& bad) {
    auto p = reinterpret_cast<const uint8_t*>(data + e.off);
    refl::forFields(refl::byHash(e.type), [&](const refl::Field& f) {
        if (f.type.resolved().prim() != refl::BlobString || uint64_t(f.offset) + 8 > e.size) return;
        res::Blob b = res::blob(p + f.offset, refl::BlobString);
        if (!b.count) return;
        uint64_t at = uint64_t(f.offset) + b.rel;
        bool ok = at + b.count <= e.size && std::none_of(p + at, p + at + b.count, [](uint8_t c) {
                      return c < 0x20 && c != '\t' && c != '\n' && c != '\r';
                  });
        (ok ? good : bad)++;
    });
}

int hexVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    c = char(c | 0x20);
    return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

// GUID text is the Windows layout: the first three groups are little-endian numbers, the rest bytes in order.
void swapGroups(uint8_t* g) {
    std::reverse(g, g + 4);
    std::reverse(g + 4, g + 6);
    std::reverse(g + 6, g + 8);
}

// Header locations 1 (containers, 16 bytes each), 7 (ContentHash keys) and 8 ({u32 offset, u16 flags, u16 container,
// 8 padding} values). Optional: without them only the English names are missing.
void loadContent(const uint8_t* f, size_t n) {
    size_t co, cc, ko, kc, vo, vc;
    gContent.clear();
    if (!kfcLoc(f, n, 1, 16, co, cc) || !kfcLoc(f, n, 7, 16, ko, kc) || !kfcLoc(f, n, 8, 16, vo, vc) || kc != vc) return;
    gContent.resize(kc);
    for (size_t i = 0; i < kc; i++) {
        memcpy(gContent[i].hash, f + ko + 16 * i, 16);
        memcpy(&gContent[i].off, f + vo + 16 * i, 4);
        memcpy(&gContent[i].container, f + vo + 16 * i + 6, 2);
    }
    gContainers = cc;
}

// Where the hash of LocaTagCollectionResource e's En_Us text is: its languages {language, data} entry, else its
// keenglishData. Both are ResourceContentReferences, which start with the ContentHash.
const uint8_t* englishHash(const res::Entry& e, refl::Type t) {
    refl::Field langs, eng, lang, dat;
    const uint8_t* p = res::data(e);
    if (!refl::field(t, "languages", langs) || !refl::field(t, "keenglishData", eng)) return nullptr;
    const uint8_t* hash = p + eng.offset;
    refl::Type lt = langs.type.resolved().inner();
    if (!lt || !refl::field(lt, "language", lang) || !refl::field(lt, "data", dat) || !res::inside(e, p + langs.offset, 8))
        return hash;
    int64_t enUs = -1;
    refl::forEnum(lang.type.resolved(), [&](std::string_view name, int64_t v) {
        if (name == "En_Us") enUs = v;
    });
    res::Blob b = res::blob(p + langs.offset, refl::BlobArray);
    for (uint32_t i = 0; i < b.count; i++) {
        const uint8_t* l = p + langs.offset + b.rel + size_t(i) * lt.size();
        uint32_t id;
        if (!res::inside(e, l, lt.size())) break;
        memcpy(&id, l + lang.offset, 4);
        if (id == enUs) hash = l + dat.offset;
    }
    return hash;
}

}  // namespace

// The index is three arrays of the KFC4 header: one ResourceLocation {uncompressed size, compressed size, count},
// ResourceId keys {guid, type hash, part, 0, 0} and values {offset, size} into the uncompressed data.
bool res::load(const uint8_t* f, size_t n) {
    gResReady = false;
    size_t lo, lc, ko, kc, vo, vc;
    if (n < 0x90 || memcmp(f, "KFC4", 4) != 0 || !kfcLoc(f, n, 4, 12, lo, lc) || lc != 1 ||
        !kfcLoc(f, n, 10, 32, ko, kc) || !kfcLoc(f, n, 11, 8, vo, vc) || kc != vc)
        return false;
    uint32_t total;
    memcpy(&total, f + lo, 4);
    std::vector<Entry> es(kc);
    for (size_t i = 0; i < kc; i++) {
        const uint8_t* k = f + ko + 32 * i;
        memcpy(es[i].guid, k, 16);
        memcpy(&es[i].type, k + 16, 4);
        memcpy(&es[i].part, k + 20, 4);
        memcpy(&es[i].off, f + vo + 8 * i, 4);
        memcpy(&es[i].size, f + vo + 8 * i + 4, 4);
        if (uint64_t(es[i].off) + es[i].size > total) return false;
    }
    std::sort(es.begin(), es.end(), entryLess);
    gEntries = std::move(es);
    gOfType.clear();
    for (const Entry& e : gEntries) gOfType[e.type].push_back(&e);
    gDataSize = total;
    loadContent(f, n);
    return true;
}

// The data is there once the first resources' strings read as text. All-empty strings (not loaded yet) don't pass.
bool res::attach(uintptr_t data) {
    uint32_t prot = 0;
    if (gEntries.empty() || !readable(data, gDataSize, prot)) return false;
    int good = 0, bad = 0;
    for (size_t i = 0; i < gEntries.size() && i < 256; i++) checkStrings(data, gEntries[i], good, bad);
    if (bad || !good) return false;
    gData = data;
    gProtect = prot;
    gResReady = true;
    return true;
}

bool res::ready() { return gResReady; }

// The pages are read-write while the game loads them, then it protects them, so every write makes its bytes writable
// and puts the protection back.
bool res::write(void* dst, const void* src, size_t n, uint32_t& prot) {
    std::lock_guard lock(gWriteMx);
    DWORD old = 0, ours = 0;
    if (!VirtualProtect(dst, n, PAGE_READWRITE, &old)) return false;
    memcpy(dst, src, n);
    VirtualProtect(dst, n, old, &ours);
    prot = old;
    return true;
}
size_t res::count() { return gEntries.size(); }
uint32_t res::protection() { return gProtect; }
uint8_t* res::data(const Entry& e) { return reinterpret_cast<uint8_t*>(gData + e.off); }

const res::Entry* res::find(const uint8_t guid[16], uint32_t type, uint32_t part) {
    auto it = std::lower_bound(gEntries.begin(), gEntries.end(), guid,
                               [](const Entry& e, const uint8_t* g) { return memcmp(e.guid, g, 16) < 0; });
    for (; it != gEntries.end() && memcmp(it->guid, guid, 16) == 0; ++it) {
        if ((!type || it->type == type) && it->part == part) return &*it;
    }
    return nullptr;
}

const std::vector<const res::Entry*>& res::ofType(uint32_t type) {
    static const std::vector<const Entry*> none;
    auto it = gOfType.find(type);
    return it == gOfType.end() ? none : it->second;
}

std::vector<std::pair<refl::Type, size_t>> res::types() {
    std::vector<std::pair<refl::Type, size_t>> out;
    for (auto& [hash, list] : gOfType) {
        if (refl::Type t = refl::byHash(hash)) out.emplace_back(t, list.size());
    }
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.first.qname() < b.first.qname(); });
    return out;
}

refl::Type res::typeByName(std::string_view name) {
    for (auto& [hash, list] : gOfType) {
        refl::Type t = refl::byHash(hash);
        if (t && (t.qname() == name || t.name() == name)) return t;
    }
    return {};
}

std::string_view res::label(const Entry& e) {
    refl::Type t = refl::byHash(e.type);
    const uint8_t* p = data(e);
    for (const char* name : {"debugName", "name"}) {
        refl::Field f;
        if (!refl::field(t, name, f) || f.type.resolved().prim() != refl::BlobString || !inside(e, p + f.offset, 8))
            continue;
        Blob b = blob(p + f.offset, refl::BlobString);
        const uint8_t* s = p + f.offset + b.rel;
        if (b.count && inside(e, s, b.count)) return {reinterpret_cast<const char*>(s), b.count};
    }
    return {};
}

// Arrays and strings {rel, count}, optionals {rel}, variants {type hash, rel from its own field, size in bytes}.
res::Blob res::blob(const uint8_t* p, uint8_t prim) {
    uint32_t a, b, c;
    memcpy(&a, p, 4);
    if (prim == refl::BlobOptional) return {a, a ? 1u : 0u, 0};
    memcpy(&b, p + 4, 4);
    if (prim != refl::BlobVariant) return a ? Blob{a, b, 0} : Blob{};
    memcpy(&c, p + 8, 4);
    return a && b && b <= UINT32_MAX - 4 ? Blob{4 + b, c, a} : Blob{};
}

bool res::inside(const Entry& e, const uint8_t* p, uint64_t n) {
    auto lo = reinterpret_cast<uintptr_t>(data(e)), at = reinterpret_cast<uintptr_t>(p);
    return at >= lo && at - lo <= e.size && n <= e.size - (at - lo);
}

std::vector<res::EnumField> res::enumFields(refl::Type t) {
    std::vector<EnumField> out;
    auto isEnum = [](refl::Type u) { return u && u.prim() == refl::Enum && u.size() >= 1 && u.size() <= 8; };
    refl::forFields(t.resolved(), [&](const refl::Field& f) {
        refl::Type u = f.type.resolved();
        if (isEnum(u)) out.push_back({std::string(f.name), u, f.offset});
        else if (u && u.prim() == refl::Struct)
            refl::forFields(u, [&](const refl::Field& g) {
                refl::Type v = g.type.resolved();
                if (isEnum(v)) out.push_back({std::string(f.name) + '.' + std::string(g.name), v, f.offset + g.offset});
            });
    });
    return out;
}

bool res::enumValue(const Entry& e, const EnumField& f, uint64_t& v) {
    const uint8_t* p = data(e) + f.offset;
    if (!inside(e, p, f.type.size())) return false;
    v = 0;
    memcpy(&v, p, f.type.size());
    return true;
}

bool res::textSource(uint32_t& container, uint32_t& offset, uint32_t& size) {
    refl::Type t = typeByName("LocaTagCollectionResource");
    if (!ready() || !t || ofType(t.hash()).empty()) return false;
    const Entry& e = *ofType(t.hash())[0];
    const uint8_t* hash = englishHash(e, t);
    if (!hash || !inside(e, hash, 16)) return false;
    auto c = std::find_if(gContent.begin(), gContent.end(), [&](const Content& c) { return memcmp(c.hash, hash, 16) == 0; });
    if (c == gContent.end() || c->container >= gContainers) return false;
    container = c->container;
    offset = c->off;
    memcpy(&size, hash, 4);  // a ContentHash starts with the content's size
    return size >= 8;
}

// Checked whole once (every text in bounds, ids strictly ascending), so text() only searches.
bool res::setText(std::vector<uint8_t> d) {
    gTextReady = false;
    refl::Type rt = refl::byName("keen::LocaTagResource");
    refl::Field id, str;
    if (!rt || !refl::field(rt, "id", id) || !refl::field(rt, "text", str) || id.offset + 4 > rt.size() ||
        str.offset + 8 > rt.size() || d.size() < 8)
        return false;
    Blob b = blob(d.data(), refl::BlobArray);
    if (!b.count || b.rel > d.size() || b.count > (d.size() - b.rel) / rt.size()) return false;
    for (uint32_t i = 0, prev = 0; i < b.count; i++) {
        size_t r = b.rel + size_t(i) * rt.size();
        uint32_t v;
        memcpy(&v, &d[r + id.offset], 4);
        Blob s = blob(&d[r + str.offset], refl::BlobString);
        if ((i && v <= prev) || (s.count && r + str.offset + uint64_t(s.rel) + s.count > d.size())) return false;
        prev = v;
    }
    gText = std::move(d);
    gTextAt = b.rel;
    gTextCount = b.count;
    gTextRec = rt.size();
    gTextId = id.offset;
    gTextStr = str.offset;
    gTextReady = true;
    return true;
}

size_t res::textCount() { return gTextReady ? gTextCount : 0; }

bool res::isText(refl::Type t) {
    refl::Type u = t.resolved();
    return u && u.prim() == refl::ObjectReference && u.inner() && u.inner().qname() == "keen::LocaTag";
}

// A LocaTag reference's text id is FNV-1a over its 16 GUID bytes.
std::string_view res::text(const uint8_t guid[16]) {
    static constexpr uint8_t kNone[16] = {};
    if (!gTextReady || memcmp(guid, kNone, 16) == 0) return {};
    uint32_t want = refl::fnv({reinterpret_cast<const char*>(guid), 16}), lo = 0, hi = gTextCount;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2, v;
        const uint8_t* r = gText.data() + gTextAt + size_t(mid) * gTextRec;
        memcpy(&v, r + gTextId, 4);
        if (v < want) {
            lo = mid + 1;
        } else if (v > want) {
            hi = mid;
        } else {
            Blob s = blob(r + gTextStr, refl::BlobString);
            return s.count ? std::string_view(reinterpret_cast<const char*>(r + gTextStr + s.rel), s.count) : std::string_view();
        }
    }
    return {};
}

std::string_view res::nameText(const Entry& e) {
    refl::Field f;
    if (!refl::field(refl::byHash(e.type), "name", f) || !isText(f.type) || !inside(e, data(e) + f.offset, 16)) return {};
    return text(data(e) + f.offset);
}

bool res::parseGuid(std::string_view s, uint8_t out[16]) {
    uint8_t g[16] = {};
    if (s.size() != 36) return false;
    for (size_t i = 0, n = 0; i < s.size(); i++) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (s[i] != '-') return false;
            continue;
        }
        int v = hexVal(s[i]);
        if (v < 0) return false;
        g[n / 2] = uint8_t(g[n / 2] << 4 | v);
        n++;
    }
    swapGroups(g);
    memcpy(out, g, 16);
    return true;
}

std::string res::guidStr(const uint8_t g[16]) {
    static constexpr char kHex[] = "0123456789abcdef";
    uint8_t b[16];
    memcpy(b, g, 16);
    swapGroups(b);
    std::string s;
    for (int i = 0; i < 16; i++) {
        if (i == 4 || i == 6 || i == 8 || i == 10) s += '-';
        s += kHex[b[i] >> 4];
        s += kHex[b[i] & 15];
    }
    return s;
}
