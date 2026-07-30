#include "bav.hpp"

#include <fstream>
#include <iostream>
#include <string>
#include <vector>

static std::vector<uint8_t> read_all(const std::string &path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open " + path);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)),
                                std::istreambuf_iterator<char>());
}

static void write_all(const std::string &path, const std::vector<uint8_t> &data) {
    std::ofstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot write " + path);
    if (!data.empty())
        f.write(reinterpret_cast<const char *>(data.data()),
                static_cast<std::streamsize>(data.size()));
}

static int usage() {
    std::cerr <<
        "BAV research compressor — C++ backup port (full research)\n"
        "  bav-cpp compress <in> [-o out] [-m auto|store|deflate|lzma|zstd|brotli|research]\n"
        "  bav-cpp decompress <in> [-o out]\n"
        "  bav-cpp version\n"
        "Default method: auto (all backends + full research paths).\n"
        "Primary port is C# (repo-root bav.ps1); this is the C++ backup.\n";
    return 2;
}

int main(int argc, char **argv) {
    if (argc < 2) return usage();
    std::string cmd = argv[1];
    try {
        if (cmd == "version" || cmd == "--version" || cmd == "-v") {
            std::cout << bav::version_string() << "\n";
            return 0;
        }
        if (cmd == "compress") {
            std::string in, out, mstr = "auto";
            for (int i = 2; i < argc; i++) {
                std::string a = argv[i];
                if ((a == "-o" || a == "--output") && i + 1 < argc)
                    out = argv[++i];
                else if ((a == "-m" || a == "--method") && i + 1 < argc)
                    mstr = argv[++i];
                else if (in.empty() && a[0] != '-')
                    in = a;
            }
            if (in.empty()) return usage();
            auto data = read_all(in);
            auto method = bav::parse_method(mstr);
            auto frame = bav::compress(data, method);
            if (out.empty()) out = in + ".bav";
            write_all(out, frame);
            std::cout << "compressed " << data.size() << " -> " << frame.size()
                      << " bytes (" << out << ")\n";
            return 0;
        }
        if (cmd == "decompress") {
            std::string in, out;
            for (int i = 2; i < argc; i++) {
                std::string a = argv[i];
                if ((a == "-o" || a == "--output") && i + 1 < argc)
                    out = argv[++i];
                else if (in.empty() && a[0] != '-')
                    in = a;
            }
            if (in.empty()) return usage();
            auto frame = read_all(in);
            auto data = bav::decompress(frame);
            if (out.empty()) {
                if (in.size() > 4 && in.substr(in.size() - 4) == ".bav")
                    out = in.substr(0, in.size() - 4);
                else
                    out = in + ".out";
            }
            write_all(out, data);
            std::cout << "decompressed " << frame.size() << " -> " << data.size()
                      << " bytes (" << out << ")\n";
            return 0;
        }
    } catch (const std::exception &e) {
        std::cerr << e.what() << "\n";
        return 1;
    }
    return usage();
}
