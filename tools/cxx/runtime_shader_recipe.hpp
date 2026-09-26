#pragma once

#include <string>

// Writes runtime_shaders.recipe from ucode dumps and disc sources. Returns exit code (0 ok).
int MakeRuntimeShaderRecipe(const std::string& image_path, const std::string& game_path,
                            const std::string& dump_dir, const std::string& output_path);
