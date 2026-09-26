#pragma once

#include <cstdint>
#include <string>
#include <vector>

std::vector<uint8_t> ReadFileBytes(const std::string& path);
std::string ReadFileText(const std::string& path);
void WriteFileBytes(const std::string& path, const std::vector<uint8_t>& data);
void WriteFileText(const std::string& path, const std::string& text);
