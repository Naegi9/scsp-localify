// SCSP Localify for Android - test build 1
// Loads into Song for Prism (via LSPatch), waits for il2cpp to start, then hooks the same
// text functions as the PC plugin and replaces Japanese text from localify.json / local2.json.
#include <jni.h>
#include <android/log.h>
#include <dlfcn.h>
#include <elf.h>
#include <link.h>
#include <sys/stat.h>
#include <shadowhook.h>

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#define TAG "SCSPLocal"
#define BUILD_NAME "android test build 1"

// ------------------------------------------------------------------ logging
static std::string g_dir;  // /storage/emulated/0/Android/data/<pkg>/files/scsp_localify
static std::mutex g_logMtx;

static void logf(const char* fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    __android_log_write(ANDROID_LOG_INFO, TAG, buf);
    std::lock_guard<std::mutex> lock(g_logMtx);
    if (g_dir.empty()) return;
    if (FILE* f = fopen((g_dir + "/scsp_log.txt").c_str(), "a")) {
        fprintf(f, "%s\n", buf);
        fclose(f);
    }
}

static void mkdirs(const std::string& path) {
    std::string cur;
    for (size_t i = 0; i < path.size(); i++) {
        cur += path[i];
        if ((path[i] == '/' && i > 0) || i + 1 == path.size()) mkdir(cur.c_str(), 0775);
    }
}

// ------------------------------------------------------------------ text helpers
struct Il2CppString {
    void* klass;
    void* monitor;
    int32_t length;
    char16_t chars[1];
};

static std::string toUtf8(const Il2CppString* s) {
    std::string out;
    if (!s) return out;
    out.reserve(s->length * 3);
    for (int32_t i = 0; i < s->length; i++) {
        uint32_t c = s->chars[i];
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < s->length) {
            const uint32_t lo = s->chars[i + 1];
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
                i++;
            }
        }
        if (c < 0x80) out += (char)c;
        else if (c < 0x800) { out += (char)(0xC0 | (c >> 6)); out += (char)(0x80 | (c & 0x3F)); }
        else if (c < 0x10000) { out += (char)(0xE0 | (c >> 12)); out += (char)(0x80 | ((c >> 6) & 0x3F)); out += (char)(0x80 | (c & 0x3F)); }
        else { out += (char)(0xF0 | (c >> 18)); out += (char)(0x80 | ((c >> 12) & 0x3F)); out += (char)(0x80 | ((c >> 6) & 0x3F)); out += (char)(0x80 | (c & 0x3F)); }
    }
    return out;
}

static bool hasJapanese(const Il2CppString* s) {
    if (!s) return false;
    for (int32_t i = 0; i < s->length; i++) {
        const char16_t c = s->chars[i];
        if ((c >= 0x3040 && c <= 0x30FF) || (c >= 0x4E00 && c <= 0x9FFF)) return true;
    }
    return false;
}

// ------------------------------------------------------------------ translation data
static std::atomic<bool> g_dataReady{false};
static std::unordered_map<std::string, std::unordered_map<int, std::string>> g_localify;  // category -> id -> text
static std::unordered_map<std::string, std::string> g_local2;                              // jp -> en

static nlohmann::json readJson(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return nullptr;
    std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (s.size() >= 3 && (unsigned char)s[0] == 0xEF) s.erase(0, 3);  // BOM
    return nlohmann::json::parse(s, nullptr, false);
}

static void loadData() {
    const auto t0 = std::chrono::steady_clock::now();
    size_t n1 = 0, n2 = 0;
    auto loc = readJson(g_dir + "/localify.json");
    if (loc.is_object()) {
        for (auto& [cat, ids] : loc.items()) {
            if (!ids.is_object()) continue;
            auto& m = g_localify[cat];
            for (auto& [id, text] : ids.items()) {
                if (!text.is_string()) continue;
                try { m[std::stoi(id)] = text.get<std::string>(); n1++; } catch (...) {}
            }
        }
    } else {
        logf("localify.json missing or invalid");
    }
    auto l2 = readJson(g_dir + "/local2.json");
    if (l2.is_object()) {
        for (auto& [k, v] : l2.items()) {
            if (v.is_string() && !v.get_ref<const std::string&>().empty()) { g_local2[k] = v.get<std::string>(); n2++; }
        }
    } else {
        logf("local2.json missing or invalid");
    }
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    logf("translations loaded: %zu localify entries, %zu local2 entries (%lld ms)", n1, n2, (long long)ms);
    g_dataReady = true;
}

// Japanese text we had no translation for -> dumps/local2.json (same format as the PC dump)
static std::mutex g_missMtx;
static std::set<std::string> g_missing;
static std::atomic<bool> g_missDirty{false};

static void noteMissing(const std::string& s) {
    std::lock_guard<std::mutex> lock(g_missMtx);
    if (g_missing.size() < 20000 && g_missing.insert(s).second) g_missDirty = true;
}

static void dumpThread() {
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(10));
        if (!g_missDirty.exchange(false)) continue;
        nlohmann::ordered_json j = nlohmann::ordered_json::object();
        {
            std::lock_guard<std::mutex> lock(g_missMtx);
            for (auto& s : g_missing) j[s] = "";
        }
        mkdirs(g_dir + "/dumps");
        std::ofstream f(g_dir + "/dumps/local2.json", std::ios::binary);
        f << j.dump(4, ' ', false, nlohmann::json::error_handler_t::replace);
    }
}

static bool lookupLocal2(const Il2CppString* s, std::string* out) {
    if (!g_dataReady || !hasJapanese(s)) return false;
    const auto key = toUtf8(s);
    auto it = g_local2.find(key);
    if (it != g_local2.end()) { *out = it->second; return true; }
    noteMissing(key);
    return false;
}

// ------------------------------------------------------------------ il2cpp api
#define IL2CPP_API(ret, name, args) using name##_t = ret(*) args; static name##_t name = nullptr;
IL2CPP_API(void*, il2cpp_domain_get, ())
IL2CPP_API(void**, il2cpp_domain_get_assemblies, (void*, size_t*))
IL2CPP_API(void*, il2cpp_assembly_get_image, (void*))
IL2CPP_API(const char*, il2cpp_image_get_name, (void*))
IL2CPP_API(void*, il2cpp_class_from_name, (void*, const char*, const char*))
IL2CPP_API(void*, il2cpp_class_get_method_from_name, (void*, const char*, int))
IL2CPP_API(void*, il2cpp_class_get_methods, (void*, void**))
IL2CPP_API(const char*, il2cpp_method_get_name, (void*))
IL2CPP_API(uint32_t, il2cpp_method_get_param_count, (void*))
IL2CPP_API(void*, il2cpp_method_get_param, (void*, uint32_t))
IL2CPP_API(char*, il2cpp_type_get_name, (void*))
IL2CPP_API(Il2CppString*, il2cpp_string_new, (const char*))

// Find an exported symbol of a loaded library by reading its ELF dynamic symbol table.
// (dlopen/dlsym can't see the game's libraries from the module's linker namespace.)
struct LibInfo { std::string name; ElfW(Addr) base = 0; const ElfW(Phdr)* phdr = nullptr; int phnum = 0; };

static int findLibCb(dl_phdr_info* info, size_t, void* data) {
    auto* li = static_cast<LibInfo*>(data);
    if (info->dlpi_name && strstr(info->dlpi_name, li->name.c_str())) {
        li->base = info->dlpi_addr;
        li->phdr = info->dlpi_phdr;
        li->phnum = info->dlpi_phnum;
        return 1;
    }
    return 0;
}

static uint32_t gnuHash(const char* s) {
    uint32_t h = 5381;
    for (; *s; s++) h = (h << 5) + h + (uint8_t)*s;
    return h;
}

static uint32_t sysvHash(const char* s) {
    uint32_t h = 0, g;
    for (; *s; s++) {
        h = (h << 4) + (uint8_t)*s;
        if ((g = h & 0xf0000000)) h ^= g >> 24;
        h &= ~g;
    }
    return h;
}

class ElfSymbols {
public:
    bool open(const char* libName) {
        LibInfo li;
        li.name = libName;
        dl_iterate_phdr(findLibCb, &li);
        if (!li.base) return false;
        base_ = li.base;
        for (int i = 0; i < li.phnum; i++) {
            if (li.phdr[i].p_type != PT_DYNAMIC) continue;
            auto* dyn = reinterpret_cast<const ElfW(Dyn)*>(base_ + li.phdr[i].p_vaddr);
            for (; dyn->d_tag != DT_NULL; dyn++) {
                const ElfW(Addr) p = fix(dyn->d_un.d_ptr);
                switch (dyn->d_tag) {
                    case DT_SYMTAB: symtab_ = reinterpret_cast<const ElfW(Sym)*>(p); break;
                    case DT_STRTAB: strtab_ = reinterpret_cast<const char*>(p); break;
                    case DT_GNU_HASH: gnuhash_ = reinterpret_cast<const uint32_t*>(p); break;
                    case DT_HASH: hash_ = reinterpret_cast<const uint32_t*>(p); break;
                    default: break;
                }
            }
        }
        return symtab_ && strtab_ && (gnuhash_ || hash_);
    }

    void* find(const char* name) const {
        if (gnuhash_) {
            const uint32_t nbuckets = gnuhash_[0], symoffset = gnuhash_[1], bloomSize = gnuhash_[2];
            const auto* bloom = reinterpret_cast<const ElfW(Addr)*>(gnuhash_ + 4);
            const uint32_t* buckets = reinterpret_cast<const uint32_t*>(bloom + bloomSize);
            const uint32_t* chain = buckets + nbuckets;
            const uint32_t h = gnuHash(name);
            uint32_t idx = buckets[h % nbuckets];
            if (idx >= symoffset) {
                for (;; idx++) {
                    const uint32_t h2 = chain[idx - symoffset];
                    if ((h | 1) == (h2 | 1) && strcmp(strtab_ + symtab_[idx].st_name, name) == 0 && symtab_[idx].st_value)
                        return reinterpret_cast<void*>(base_ + symtab_[idx].st_value);
                    if (h2 & 1) break;
                }
            }
        }
        if (hash_) {
            const uint32_t nbucket = hash_[0];
            const uint32_t* bucket = hash_ + 2;
            const uint32_t* chain = bucket + nbucket;
            for (uint32_t idx = bucket[sysvHash(name) % nbucket]; idx; idx = chain[idx]) {
                if (strcmp(strtab_ + symtab_[idx].st_name, name) == 0 && symtab_[idx].st_value)
                    return reinterpret_cast<void*>(base_ + symtab_[idx].st_value);
            }
        }
        return nullptr;
    }

private:
    ElfW(Addr) fix(ElfW(Addr) p) const { return p < base_ ? p + base_ : p; }  // bionic keeps these unrelocated
    ElfW(Addr) base_ = 0;
    const ElfW(Sym)* symtab_ = nullptr;
    const char* strtab_ = nullptr;
    const uint32_t* gnuhash_ = nullptr;
    const uint32_t* hash_ = nullptr;
};

static bool resolveApi() {
    ElfSymbols il2cpp;
    if (!il2cpp.open("libil2cpp.so")) { logf("libil2cpp.so symbols not readable"); return false; }
    bool ok = true;
#define RESOLVE(name) name = reinterpret_cast<name##_t>(il2cpp.find(#name)); if (!name) { logf("missing il2cpp export: %s", #name); ok = false; }
    RESOLVE(il2cpp_domain_get)
    RESOLVE(il2cpp_domain_get_assemblies)
    RESOLVE(il2cpp_assembly_get_image)
    RESOLVE(il2cpp_image_get_name)
    RESOLVE(il2cpp_class_from_name)
    RESOLVE(il2cpp_class_get_method_from_name)
    RESOLVE(il2cpp_class_get_methods)
    RESOLVE(il2cpp_method_get_name)
    RESOLVE(il2cpp_method_get_param_count)
    RESOLVE(il2cpp_method_get_param)
    RESOLVE(il2cpp_type_get_name)
    RESOLVE(il2cpp_string_new)
#undef RESOLVE
    return ok;
}

static void* getImage(const char* assemblyName) {
    size_t n = 0;
    void** assemblies = il2cpp_domain_get_assemblies(il2cpp_domain_get(), &n);
    for (size_t i = 0; i < n; i++) {
        void* img = il2cpp_assembly_get_image(assemblies[i]);
        const char* name = img ? il2cpp_image_get_name(img) : nullptr;
        if (name && strcmp(name, assemblyName) == 0) return img;
    }
    return nullptr;
}

static void* getClass(const char* assembly, const char* ns, const char* name) {
    void* img = getImage(assembly);
    if (!img) { logf("assembly not found: %s", assembly); return nullptr; }
    void* k = il2cpp_class_from_name(img, ns, name);
    if (!k) logf("class not found: %s.%s", ns, name);
    return k;
}

static void* getMethod(const char* assembly, const char* ns, const char* klass, const char* method, int argc) {
    void* k = getClass(assembly, ns, klass);
    if (!k) return nullptr;
    void* m = il2cpp_class_get_method_from_name(k, method, argc);
    if (!m) { logf("method not found: %s.%s(%d)", klass, method, argc); return nullptr; }
    return *reinterpret_cast<void**>(m);  // MethodInfo::methodPointer is the first field
}

// TMP_Text.SetText(string, bool) - there are many SetText overloads, pick by parameter types
static void* findSetTextStringBool() {
    void* k = getClass("Unity.TextMeshPro.dll", "TMPro", "TMP_Text");
    if (!k) return nullptr;
    void* iter = nullptr;
    while (void* m = il2cpp_class_get_methods(k, &iter)) {
        if (strcmp(il2cpp_method_get_name(m), "SetText") != 0 || il2cpp_method_get_param_count(m) != 2) continue;
        const char* t0 = il2cpp_type_get_name(il2cpp_method_get_param(m, 0));
        const char* t1 = il2cpp_type_get_name(il2cpp_method_get_param(m, 1));
        if (t0 && t1 && strcmp(t0, "System.String") == 0 && strcmp(t1, "System.Boolean") == 0) return *reinterpret_cast<void**>(m);
    }
    logf("method not found: TMP_Text.SetText(string, bool)");
    return nullptr;
}

// ------------------------------------------------------------------ hooks
static void* GetTextOrNull_orig;
static Il2CppString* GetTextOrNull_hook(void* self, Il2CppString* category, int32_t id, void* mi) {
    if (g_dataReady && category) {
        auto c = g_localify.find(toUtf8(category));
        if (c != g_localify.end()) {
            auto t = c->second.find(id);
            if (t != c->second.end()) return il2cpp_string_new(t->second.c_str());
        }
    }
    return reinterpret_cast<Il2CppString* (*)(void*, Il2CppString*, int32_t, void*)>(GetTextOrNull_orig)(self, category, id, mi);
}

static void* set_text_orig;
static void set_text_hook(void* self, Il2CppString* value, void* mi) {
    std::string tl;
    if (lookupLocal2(value, &tl)) value = il2cpp_string_new(tl.c_str());
    reinterpret_cast<void (*)(void*, Il2CppString*, void*)>(set_text_orig)(self, value, mi);
}

static void* SetText_orig;
static void SetText_hook(void* self, Il2CppString* value, bool syncTextInputBox, void* mi) {
    std::string tl;
    if (lookupLocal2(value, &tl)) value = il2cpp_string_new(tl.c_str());
    reinterpret_cast<void (*)(void*, Il2CppString*, bool, void*)>(SetText_orig)(self, value, syncTextInputBox, mi);
}

static Il2CppString* (*UIText_get_text)(void*, void*);
static void (*UIText_set_text)(void*, Il2CppString*, void*);
static void* UIText_Awake_orig;
static void UIText_Awake_hook(void* self, void* mi) {
    if (UIText_get_text && UIText_set_text) {
        Il2CppString* orig = UIText_get_text(self, nullptr);
        std::string tl;
        if (lookupLocal2(orig, &tl)) UIText_set_text(self, il2cpp_string_new(tl.c_str()), nullptr);
    }
    reinterpret_cast<void (*)(void*, void*)>(UIText_Awake_orig)(self, mi);
}

static int g_hooksOk = 0;
static void hook(const char* name, void* addr, void* fn, void** orig) {
    if (!addr) return;
    if (shadowhook_hook_func_addr(addr, fn, orig)) {
        g_hooksOk++;
        logf("hooked %s at %p", name, addr);
    } else {
        const int e = shadowhook_get_errno();
        logf("hook FAILED %s: %d %s", name, e, shadowhook_to_errmsg(e));
    }
}

static void installHooks() {
    if (!resolveApi()) return;

    hook("LocalizationManager.GetTextOrNull",
         getMethod("PRISM.Legacy.dll", "ENTERPRISE.Localization", "LocalizationManager", "GetTextOrNull", 2),
         (void*)GetTextOrNull_hook, &GetTextOrNull_orig);
    hook("TMP_Text.set_text",
         getMethod("Unity.TextMeshPro.dll", "TMPro", "TMP_Text", "set_text", 1),
         (void*)set_text_hook, &set_text_orig);
    hook("TMP_Text.SetText", findSetTextStringBool(), (void*)SetText_hook, &SetText_orig);

    UIText_get_text = reinterpret_cast<decltype(UIText_get_text)>(getMethod("PRISM.Legacy.dll", "ENTERPRISE.UI", "UITextMeshProUGUI", "get_text", 0));
    UIText_set_text = reinterpret_cast<decltype(UIText_set_text)>(getMethod("PRISM.Legacy.dll", "ENTERPRISE.UI", "UITextMeshProUGUI", "set_text", 1));
    hook("UITextMeshProUGUI.Awake",
         getMethod("PRISM.Legacy.dll", "ENTERPRISE.UI", "UITextMeshProUGUI", "Awake", 0),
         (void*)UIText_Awake_hook, &UIText_Awake_orig);

    logf("%d/4 hooks installed", g_hooksOk);
}

// il2cpp_init runs once when Unity starts the game code; after it returns all assemblies are loaded.
static void* il2cpp_init_orig;
static int il2cpp_init_hook(const char* domainName) {
    const int r = reinterpret_cast<int (*)(const char*)>(il2cpp_init_orig)(domainName);
    logf("il2cpp_init done (%s)", domainName ? domainName : "");
    try {
        installHooks();
    } catch (std::exception& e) {
        logf("hook setup error: %s", e.what());
    }
    return r;
}

// ------------------------------------------------------------------ entry
extern "C" JNIEXPORT void JNICALL
Java_io_github_naegi9_scsplocal_Entry_nativeInit(JNIEnv* env, jclass, jstring jdir, jstring jpkg) {
    const char* dir = env->GetStringUTFChars(jdir, nullptr);
    const char* pkg = env->GetStringUTFChars(jpkg, nullptr);
    g_dir = dir;
    mkdirs(g_dir);
    logf("==== SCSP Localify %s, package %s ====", BUILD_NAME, pkg);
    env->ReleaseStringUTFChars(jdir, dir);
    env->ReleaseStringUTFChars(jpkg, pkg);

    const int r = shadowhook_init(SHADOWHOOK_MODE_UNIQUE, false);
    if (r != 0) logf("shadowhook_init failed: %d %s", r, shadowhook_to_errmsg(r));

    std::thread(loadData).detach();
    std::thread(dumpThread).detach();

    // libil2cpp.so is usually not loaded yet: shadowhook installs this hook as soon as it is.
    if (shadowhook_hook_sym_name("libil2cpp.so", "il2cpp_init", (void*)il2cpp_init_hook, &il2cpp_init_orig)) {
        logf("il2cpp_init hooked");
    } else {
        const int e = shadowhook_get_errno();
        if (e == SHADOWHOOK_ERRNO_PENDING) logf("il2cpp_init hook pending (waiting for libil2cpp.so)");
        else logf("il2cpp_init hook FAILED: %d %s", e, shadowhook_to_errmsg(e));
    }
}
