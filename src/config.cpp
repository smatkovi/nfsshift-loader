#include "config.h"

#include <algorithm>
#include <fstream>
#include <map>
#include <sstream>

#include "guest.h"

namespace config {
namespace {
std::string g_os = "LINUX";
std::map<std::string, std::string> g_values;      // "group/key" (lower case) -> value
std::map<std::string, std::string> g_overrides;   // forced by the loader

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

std::string trim(const std::string &s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

// Evaluate a "{...}" condition line for this device.
bool condition_matches(const std::string &cond) {
    std::string c = trim(cond);
    if (c.empty()) return true;
    size_t eq = c.find('=');
    if (eq == std::string::npos) return false;
    std::string kind = lower(trim(c.substr(0, eq)));
    std::string rest = lower(c.substr(eq + 1));
    size_t b = rest.find_first_not_of(" \t");
    rest = b == std::string::npos ? "" : rest.substr(b);
    std::string head = rest.substr(0, rest.find_first_of(" \t,"));
    if (head == "any") return kind == "os" || kind == "id" || kind == "class" || kind == "screensize";
    if (kind == "os") return head == lower(g_os) && rest.size() == head.size();
    if (kind == "class") return head == "linux_embed";
    return false;  // device ids and other conditions never match this device
}
}  // namespace

void set_os_name(const std::string &os) { g_os = os; }

void add_text(const std::string &text, const std::string &origin) {
    std::istringstream in(text);
    std::string line, group = "s3e";
    bool active = true;
    int count = 0;
    while (std::getline(in, line)) {
        std::string t = trim(line);
        if (t.empty() || t[0] == '#') continue;
        if (t[0] == '{') {
            size_t close = t.find('}');
            active = condition_matches(t.substr(1, close == std::string::npos ? std::string::npos : close - 1));
            continue;
        }
        if (t[0] == '[') {
            size_t close = t.find(']');
            group = lower(trim(t.substr(1, close == std::string::npos ? std::string::npos : close - 1)));
            continue;
        }
        if (!active) continue;
        size_t eq = t.find('=');
        if (eq == std::string::npos) continue;
        std::string key = lower(trim(t.substr(0, eq)));
        std::string value = trim(t.substr(eq + 1));
        if (!value.empty() && value[0] == '"') {
            size_t q = value.find('"', 1);
            value = value.substr(1, q == std::string::npos ? std::string::npos : q - 1);
        } else {
            size_t cut = std::min(value.find('#'), value.find("//"));
            if (cut != std::string::npos) value = trim(value.substr(0, cut));
        }
        g_values[group + "/" + key] = value;
        ++count;
    }
    logf("[config] %s: %d settings", origin.c_str(), count);
}

bool add_file(const std::string &path) {
    std::ifstream f(path);
    if (!f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    add_text(ss.str(), path);
    return true;
}

void set(const std::string &group, const std::string &key, const std::string &value) {
    g_overrides[lower(group) + "/" + lower(key)] = value;
}

std::optional<std::string> get(const std::string &group, const std::string &key) {
    std::string k = lower(group) + "/" + lower(key);
    auto o = g_overrides.find(k);
    if (o != g_overrides.end()) return o->second;
    auto it = g_values.find(k);
    if (it == g_values.end()) return std::nullopt;
    return it->second;
}

int get_int(const std::string &group, const std::string &key, int fallback) {
    auto v = get(group, key);
    if (!v || v->empty()) return fallback;
    return static_cast<int>(strtol(v->c_str(), nullptr, 0));
}
}  // namespace config
