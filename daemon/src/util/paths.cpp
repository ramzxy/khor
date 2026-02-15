#include "util/paths.h"

#include <cstdlib>
#include <filesystem>

namespace khor {

static std::string env_or_empty(const char* k) {
  const char* v = std::getenv(k);
  return v ? std::string(v) : std::string();
}

std::string path_home_dir() {
  std::string home = env_or_empty("HOME");
  if (!home.empty()) return home;
  // Best-effort fallback.
  return "/tmp";
}

std::string path_xdg_config_home() {
  std::string xdg = env_or_empty("XDG_CONFIG_HOME");
  if (!xdg.empty()) return xdg;
  return (std::filesystem::path(path_home_dir()) / ".config").string();
}

std::string path_default_config_file() {
  return (std::filesystem::path(path_xdg_config_home()) / "khor" / "config.json").string();
}

std::string path_xdg_data_home() {
  std::string xdg = env_or_empty("XDG_DATA_HOME");
  if (!xdg.empty()) return xdg;
  return (std::filesystem::path(path_home_dir()) / ".local" / "share").string();
}

std::string path_default_ui_dir() {
  return (std::filesystem::path(path_xdg_data_home()) / "khor" / "ui").string();
}

} // namespace khor
