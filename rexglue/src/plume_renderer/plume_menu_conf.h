/**
 * @file        plume_renderer/plume_menu_conf.h
 * @brief       The launcher's xerenge.conf, read and written in place by the menu.
 */
#pragma once

#include <map>
#include <string>

namespace rex::plume_renderer::menu_conf {

// XERENGE_CONF: the xerenge.conf the launcher read its settings from. Empty
// when the game was not started by the launcher.
std::string Path();

// The conf's `key = value` lines (comments and blank lines skipped). The last
// of a repeated key wins, as in the launcher.
std::map<std::string, std::string> Read(const std::string& path);

// Sets one key in place: its line is replaced, everything else - comments,
// the order, other keys - is kept; a key not in the file is appended. The file
// is replaced whole, so a reader never sees half of it. False if it could not
// be written.
bool Write(const std::string& path, const std::string& key, const std::string& value);

}  // namespace rex::plume_renderer::menu_conf
