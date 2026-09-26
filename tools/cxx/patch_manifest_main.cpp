#include "file_util.hpp"

#include <iostream>
#include <string>

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "usage: patch-burnout-manifest manifest.toml game-directory\n";
        return 1;
    }
    try {
        std::string text = ReadFileText(argv[1]);
        const std::string game = argv[2];
        const std::string game_line = "game_root = \"" + game + "\"";
        const std::string file_line = "file_path = \"" + game + "/default.xex\"";
        auto replace_line = [](std::string* body, const char* key, const std::string& line) {
            std::string out;
            size_t pos = 0;
            while (pos < body->size()) {
                const size_t end = body->find('\n', pos);
                const size_t line_end = end == std::string::npos ? body->size() : end;
                const std::string current = body->substr(pos, line_end - pos);
                if (current.rfind(key, 0) == 0) {
                    out += line;
                } else {
                    out += current;
                }
                if (end == std::string::npos) {
                    break;
                }
                out += '\n';
                pos = end + 1;
            }
            *body = out;
        };
        replace_line(&text, "game_root = ", game_line);
        replace_line(&text, "file_path = ", file_line);
        WriteFileText(argv[1], text);
    } catch (const std::exception& error) {
        std::cerr << error.what() << "\n";
        return 1;
    }
    return 0;
}
