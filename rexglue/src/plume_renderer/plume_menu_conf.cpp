/**
 * @file        plume_renderer/plume_menu_conf.cpp
 * @brief       The launcher's xerenge.conf, read and written in place by the menu.
 */
#include "plume_renderer/plume_menu_conf.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

namespace rex::plume_renderer::menu_conf {

namespace {

std::string Trimmed(const std::string& text) {
  const size_t first = text.find_first_not_of(" \t\r");
  if (first == std::string::npos) {
    return {};
  }
  return text.substr(first, text.find_last_not_of(" \t\r") - first + 1);
}

// The key of a `key = value` line, empty for any other line.
std::string KeyOf(const std::string& line) {
  const std::string text = Trimmed(line);
  if (text.empty() || text[0] == '#') {
    return {};
  }
  const size_t equals = text.find('=');
  return equals == std::string::npos ? std::string() : Trimmed(text.substr(0, equals));
}

}  // namespace

std::string Path() {
  const char* path = std::getenv("XERENGE_CONF");
  return path ? path : "";
}

std::map<std::string, std::string> Read(const std::string& path) {
  std::map<std::string, std::string> values;
  std::ifstream file(path);
  std::string line;
  while (std::getline(file, line)) {
    const std::string key = KeyOf(line);
    if (!key.empty()) {
      values[key] = Trimmed(line.substr(line.find('=') + 1));
    }
  }
  return values;
}

bool Write(const std::string& path, const std::string& key, const std::string& value) {
  std::vector<std::string> lines;
  {
    std::ifstream file(path);
    std::string line;
    while (std::getline(file, line)) {
      lines.push_back(line);
    }
  }
  const std::string replacement = key + " = " + value;
  // The last line of the key is the one the launcher reads.
  int found = -1;
  for (size_t i = 0; i < lines.size(); ++i) {
    if (KeyOf(lines[i]) == key) {
      found = int(i);
    }
  }
  if (found >= 0) {
    lines[size_t(found)] = replacement;
  } else {
    lines.push_back(replacement);
  }

  const std::string temporary = path + ".tmp";
  {
    std::ofstream out(temporary, std::ios::trunc);
    for (const std::string& line : lines) {
      out << line << '\n';
    }
    if (!out.good()) {
      return false;
    }
  }
  std::error_code error;
  std::filesystem::rename(temporary, path, error);
  if (error) {
    std::filesystem::remove(temporary, error);
    return false;
  }
  return true;
}

}  // namespace rex::plume_renderer::menu_conf
