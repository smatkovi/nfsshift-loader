// Marmalade ICF configuration (embedded s3e.icf/app.icf plus external overrides).
#pragma once

#include <optional>
#include <string>

namespace config {
// OS name used to evaluate {OS=...} blocks.
void set_os_name(const std::string &os);
void add_text(const std::string &text, const std::string &origin);
bool add_file(const std::string &path);
void set(const std::string &group, const std::string &key, const std::string &value);

std::optional<std::string> get(const std::string &group, const std::string &key);
int get_int(const std::string &group, const std::string &key, int fallback);
}  // namespace config
