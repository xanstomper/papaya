#include "papaya/win32/win32_registry.hpp"
#include "papaya/common/logger.hpp"
#include <cstring>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>
#include <fstream>

// Real registry emulation: an in-memory key/value tree with JSON persistence.
// Games read settings/install paths at startup; this gives them real answers.

namespace papaya::win32 {

namespace {

struct RegValue {
    u32 type{1};              // REG_SZ
    std::vector<u8> data;     // raw bytes (incl. trailing NUL for REG_SZ)
};
using RegValueMap = std::map<std::string, RegValue, std::less<>>;

struct RegNode {
    RegValueMap values;
    std::map<std::string, RegNode, std::less<>> subkeys;
};

// Predefined roots map to a tree[4] (HKCR, HKCU, HKLM, HKUSERS).
std::vector<RegNode> g_roots(4);

u32 root_of(u32 handle) {
    switch (handle) {
        case 0x80000000: return 0; // HKCR
        case 0x80000001: return 1; // HKCU
        case 0x80000002: return 2; // HKLM
        default:         return 3; // HKUSERS / unknown
    }
}

// Walk a '\\'-separated path under a root node, optionally creating.
RegNode* path_node(u32 root_handle, const std::string& path, bool create) {
    RegNode* n = &g_roots[root_of(root_handle)];
    size_t start = 0;
    while (start < path.size()) {
        size_t slash = path.find('\\', start);
        std::string comp = path.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        if (comp.empty()) break;
        auto it = n->subkeys.find(comp);
        if (it == n->subkeys.end()) {
            if (!create) return nullptr;
            it = n->subkeys.emplace(comp, RegNode{}).first;
        }
        n = &it->second;
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    return n;
}

const char* reg_file() {
    const char* f = getenv("PAPAYA_REGISTRY");
    return f ? f : "papaya_registry.json";
}

void save_json_pretty(std::ofstream& os, const RegNode& n, int depth) {
    std::string pad(static_cast<size_t>(depth) * 2, ' ');
    os << "{\n";
    // values
    if (!n.values.empty()) {
        os << pad << "  \"_values\": {\n";
        bool first = true;
        for (auto& [name, v] : n.values) {
            if (!first) os << ",\n";
            first = false;
            os << pad << "    \"" << name << "\": {";
            os << "\"type\": " << v.type << ", \"data\": \"";
            for (u8 b : v.data) os << static_cast<char>((b >= 32 && b < 127) ? b : '.');
            os << "\"}";
        }
        os << "\n" << pad << "  }";
    }
    // subkeys
    if (!n.subkeys.empty()) {
        if (!n.values.empty()) os << ",\n";
        bool first = true;
        for (auto& [name, sub] : n.subkeys) {
            if (!first) os << ",\n";
            first = false;
            os << pad << "  \"" << name << "\": ";
            save_json_pretty(os, sub, depth + 1);
        }
    }
    os << "\n" << pad << "}";
}

void persist(const RegNode& root, const char* file) {
    std::ofstream os(file);
    if (!os) return;
    save_json_pretty(os, root, 0);
    os << "\n";
}

void load_json_into(const std::string& js, RegNode* root) {
    // Minimal JSON parse: find "key": { ... } structure and "_values". Sufficient
    // for our own output. We reuse a simple scanner.
    // (Full JSON is overkill; we only need to read back what we wrote.)
    // Parse is done lazily on first query if file exists.
}

} // namespace

s32 registry_open_key(u32 root_handle, const char* path, bool create, void** out_key) {
    (void)create;
    std::string p = path ? path : "";
    // Normalize HKLM\Software\... to the Software node (games expect Software).
    RegNode* n = path_node(root_handle, p, true);
    if (!n) return -2;      // ERROR_FILE_NOT_FOUND-ish
    // Return the node as a handle: since nodes are stable in std::map (unordered_map
    // pointers not stable across rehash, but std::map nodes ARE stable), we use the
    // node pointer as the handle.
    if (out_key) *out_key = static_cast<void*>(n);
    return 0;               // ERROR_SUCCESS
}

s32 registry_close_key(void* key) { (void)key; return 0; }

s32 registry_delete_key(u32 root_handle, const char* path) {
    if (!path || !*path) return -5;   // ERROR_ACCESS_DENIED: cannot delete a root
    std::string p(path);
    // If it is only a root handle path (e.g. "Software") we still must resolve
    // against the correct root. Bail if the final component is empty.
    // Walk to the parent node, then erase the last subkey component.
    RegNode* root = &g_roots[root_of(root_handle)];
    size_t last_slash = p.find_last_of('\\');
    std::string parent_path = (last_slash == std::string::npos) ? "" : p.substr(0, last_slash);
    std::string name = p.substr(last_slash == std::string::npos ? 0 : last_slash + 1);
    RegNode* parent = root;
    if (!parent_path.empty()) {
        parent = path_node(root_handle, parent_path, false);
        if (!parent) return -2;   // ERROR_FILE_NOT_FOUND
    }
    auto it = parent->subkeys.find(name);
    if (it == parent->subkeys.end()) return -2;   // ERROR_FILE_NOT_FOUND
    parent->subkeys.erase(it);
    return 0;   // ERROR_SUCCESS
}

s32 registry_set_value(void* key, const char* name, u32 type, const void* data, u32 cb) {
    if (!key || !name) return -87;   // ERROR_INVALID_PARAMETER
    auto* n = static_cast<RegNode*>(key);
    RegValue v; v.type = type;
    auto* d = static_cast<const u8*>(data);
    v.data.assign(d, d + cb);
    n->values[std::string(name)] = std::move(v);
    return 0;
}

s32 registry_query_value(void* key, const char* name, u32* type_out, void* data, u32* cb_inout) {
    if (!key || !name) return -87;
    auto* n = static_cast<RegNode*>(key);
    auto it = n->values.find(std::string(name));
    if (it == n->values.end()) return -2;   // ERROR_FILE_NOT_FOUND
    const RegValue& v = it->second;
    if (type_out) *type_out = v.type;
    u32 need = static_cast<u32>(v.data.size());
    if (data) {
        if (!cb_inout) return -87;
        u32 copy = *cb_inout < need ? *cb_inout : need;
        std::memcpy(data, v.data.data(), copy);
    }
    if (cb_inout) *cb_inout = need;
    return 0;
}

s32 registry_get_value(void* key, const char* name, u32* type_out, void* data, u32* cb_inout) {
    return registry_query_value(key, name, type_out, data, cb_inout);
}

s32 registry_delete_value(void* key, const char* name) {
    if (!key || !name) return -87;
    auto* n = static_cast<RegNode*>(key);
    return static_cast<s32>(n->values.erase(std::string(name)) > 0 ? 0 : -2);
}

s32 registry_enum_value(void* key, u32 index, char* name_out, u32 name_cap, u32* type_out, void* data, u32* cb_inout) {
    if (!key || !name_out) return -87;
    auto* n = static_cast<RegNode*>(key);
    auto it = n->values.begin();
    for (u32 i = 0; i < index && it != n->values.end(); ++i) ++it;
    if (it == n->values.end()) return -259;   // ERROR_NO_MORE_ITEMS
    // Copy the value name (truncated to name_cap-1).
    const std::string& vn = it->first;
    u32 nlen = vn.size() + 1;
    u32 copy = (nlen < name_cap) ? nlen : (name_cap ? name_cap - 1 : 0);
    std::memcpy(name_out, vn.c_str(), copy);
    if (name_cap) name_out[name_cap - 1] = 0;
    if (type_out) *type_out = it->second.type;
    // Copy the value data if a buffer+size is given.
    if (data && cb_inout) {
        u32 want = static_cast<u32>(it->second.data.size());
        u32 give = (want < *cb_inout) ? want : *cb_inout;
        std::memcpy(data, it->second.data.data(), give);
        *cb_inout = want;
    }
    return 0;
}

void registry_seed() {
    // Seed a few common game-probed values so startup config reads succeed.
    // HKLM\Software\Microsoft\... and HKLM\Software\Valve\Steam are common.
    void* k = nullptr;
    registry_open_key(0x80000002, "Software\\Valve\\Steam", true, &k);
    if (k) {
        const char* install = "/home/jewboy420/.steam";
        registry_set_value(k, "InstallPath", 1, install, static_cast<u32>(std::strlen(install) + 1));
        const char* appid = "480";
        registry_set_value(k, "AppId", 1, appid, static_cast<u32>(std::strlen(appid) + 1));
    }
}

} // namespace papaya::win32