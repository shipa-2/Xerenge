#include "runtime_shader_recipe.hpp"

#include <iostream>

int main(int argc, char** argv) {
    if (argc != 5) {
        std::cerr << "usage: make-runtime-shader-recipe guest-image.bin game-dir dump-dir out.recipe\n";
        return 1;
    }
    return MakeRuntimeShaderRecipe(argv[1], argv[2], argv[3], argv[4]);
}
