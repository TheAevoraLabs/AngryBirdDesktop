// angryrift_cli.cpp — minimal CLI twin of filerift_smoke's role:
//   filerift_smoke: recode_markup -> decode_protobuf round-trip checks.
//   angryrift cli : decode (AES+LZMA->luac), probe (report), batch.
// Build: g++ -m32 -std=c++17 -I src src/angryrift/angryrift.cpp
//        src/angryrift/angryrift_cli.cpp -lcrypto -llzma -o bin/angryrift
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "angryrift/angryrift.h"

namespace {

int cmd_decode(const char* in, const char* out) {
    angryrift::DecodeResult r = angryrift::decode_file(in);
    if (!r.ok) {
        std::fprintf(stderr, "angryrift decode FAIL %s: %s\n", in,
                     r.error.c_str());
        return 1;
    }
    std::string err;
    if (!angryrift::write_file_bytes(out, r.luac, err)) {
        std::fprintf(stderr, "angryrift write FAIL: %s\n", err.c_str());
        return 1;
    }
    std::printf("angryrift decode ok: %s -> %s (%zu bytes, %s)\n", in, out,
                r.luac.size(),
                angryrift::lua_version_string(r.luac).c_str());
    return 0;
}

int cmd_probe(int argc, char** argv) {
    int rc = 0;
    for (int i = 0; i < argc; ++i) {
        std::puts(angryrift::probe_file(argv[i]).c_str());
    }
    return rc;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf(
            "angryrift — Angry Birds Classic encrypted-lua decoder\n"
            "usage:\n"
            "  angryrift probe <file.lua> [more...]   # report, no output\n"
            "  angryrift decode <in.lua> <out.luac>   # full AES+LZMA decode\n");
        return 2;
    }
    const std::string cmd = argv[1];
    if (cmd == "probe" && argc >= 3) return cmd_probe(argc - 2, argv + 2);
    if (cmd == "decode" && argc == 4) return cmd_decode(argv[2], argv[3]);
    std::fprintf(stderr, "bad args; run with no args for usage\n");
    return 2;
}
