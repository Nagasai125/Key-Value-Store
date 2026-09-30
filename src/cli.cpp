#include "kv/store.hpp"
#include <iostream>
#include <stdexcept>
#include <string_view>

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "Usage: kv_cli DIRECTORY put KEY VALUE | get KEY | erase KEY | flush | compact\n";
        return 2;
    }
    try {
        const std::string_view command = argv[2];
        const bool valid = (command == "put" && argc == 5) ||
            ((command == "get" || command == "erase") && argc == 4) ||
            ((command == "flush" || command == "compact") && argc == 3);
        if (!valid) throw std::invalid_argument("unknown command or incorrect argument count");
        kv::Store store(argv[1]);
        if (command == "put") store.put(argv[3], argv[4]);
        else if (command == "erase") store.erase(argv[3]);
        else if (command == "flush") store.flush();
        else if (command == "compact") store.compact();
        else {
            const auto value = store.get(argv[3]);
            if (!value) return 1;
            std::cout << *value << '\n';
        }
    } catch (const std::exception& error) {
        std::cerr << "kv_cli: " << error.what() << '\n';
        return 2;
    }
}
