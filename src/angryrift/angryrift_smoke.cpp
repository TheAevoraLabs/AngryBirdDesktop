// angryrift_smoke.cpp — twin of SwordigoDesktop tests/filerift_smoke.cpp.
// filerift_smoke: markup -> protobuf -> markup round-trip + error cases.
// angryrift_smoke: encrypted file -> luac -> header/string assertions +
//                   AES error cases + 2058-file corpus sweep (optional env).
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

#include "angryrift/angryrift.h"

namespace fs = std::filesystem;
namespace {

void require(bool cond, const std::string& msg) {
    if (!cond) {
        std::cerr << "angryrift smoke FAIL: " << msg << "\n";
        std::exit(EXIT_FAILURE);
    }
}

void require_contains(const std::string& hay, const std::string& needle) {
    if (hay.find(needle) == std::string::npos) {
        std::cerr << "expected output to contain: " << needle << "\n";
        std::exit(EXIT_FAILURE);
    }
}

std::string asset(const std::string& rel) {
    if (const char* root = std::getenv("ANGRYBIRDS_ASSETS"))
        return std::string(root) + "/" + rel;
    return std::string("/home/quantumcreeper/AngryBirdsDesktop/assets/") +
           rel;
}

}  // namespace

int main() {
    // 1. Golden samples: both naming flavours from the original question.
    const std::string w4scripts = asset(
        "data/vehicles/EggDefender_15/EggDefender_15_W4_V1.scripts.lua");
    const std::string ach = asset("data/scripts/achievements.lua");

    angryrift::DecodeResult r = angryrift::decode_file(w4scripts);
    require(r.ok, "W4 scripts decode: " + r.error);
    require(angryrift::looks_luac(r.luac), "W4 scripts payload is luac");
    require(r.luac.size() == 6165, "W4 scripts luac size == 6165");
    require(r.declaredSize == 6165, "W4 scripts declared size == 6165");
    {
        std::string probe = angryrift::probe_file(w4scripts);
        require_contains(probe, "AES           : ok");
        require_contains(probe, "disableSplit");
        require_contains(probe, "EggDefender_15_W4_V1.scripts.lua");
        std::string cli_out = "/tmp/angryrift_smoke_w4.luac";
        std::string err;
        require(angryrift::write_file_bytes(cli_out, r.luac, err),
                "write smoke luac: " + err);
        angryrift::DecodeResult back =
            angryrift::decode_file(w4scripts);  // idempotent re-decode
        require(back.ok && back.luac == r.luac, "re-decode is idempotent");
    }

    angryrift::DecodeResult a = angryrift::decode_file(ach);
    require(a.ok, "achievements decode: " + a.error);
    require(a.luac.size() == 3910, "achievements luac size == 3910");
    require_contains(angryrift::probe_file(ach), "ACHIEVEMENT_FINISH_WORLD_1");

    // 2. Error cases (mirror filerift_smoke's invalid-markup checks).
    {
        angryrift::DecodeResult bad =
            angryrift::decode_bytes(std::vector<uint8_t>{1, 2, 3});
        require(!bad.ok, "unaligned input must fail");
        require_contains(bad.error, "multiple of 16");
    }
    {
        std::vector<uint8_t> zeros(32, 0);  // valid size, wrong key stream
        angryrift::DecodeResult bad = angryrift::decode_bytes(zeros);
        require(!bad.ok, "zero block must not decode to magic");
    }

    // 3. Full-corpus sweep (2058 files): 2057 LZMA-ALONE + options.lua 7z.
    if (std::getenv("ANGRYRIFT_FULL_SWEEP")) {
        int ok = 0, sevenz = 0, fail = 0;
        for (auto& e : fs::recursive_directory_iterator(asset("data"))) {
            if (e.path().extension() != ".lua") continue;
            std::string err;
            std::vector<uint8_t> raw =
                angryrift::read_file_bytes(e.path().string(), err);
            require(err.empty(), "read " + e.path().string());
            angryrift::DecodeResult d = angryrift::decode_bytes(raw);
            if (d.ok) {
                ++ok;
                require(angryrift::looks_luac(d.luac),
                        "payload is luac: " + e.path().string());
            } else if (d.error.find("7z-archive") != std::string::npos) {
                ++sevenz;
            } else {
                ++fail;
                std::cerr << "CORPUS FAIL " << e.path() << ": " << d.error
                          << "\n";
            }
        }
        std::cout << "corpus: ok=" << ok << " 7z=" << sevenz
                  << " fail=" << fail << "\n";
        require(ok == 2057 && sevenz == 1 && fail == 0,
                "corpus must be 2057+1+0");
    }

    std::cout << "AngryRift smoke checks passed\n";
    return EXIT_SUCCESS;
}
