#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

std::string Sha256Hex(std::string_view data);
bool Sha256FileHex(const std::string& path, std::string* out_hex, int* progress_percent);
