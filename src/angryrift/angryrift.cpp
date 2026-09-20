// angryrift.cpp — Angry Birds Classic encrypted-Lua decoder, part 1/3.
// See angryrift.h for the format + RE provenance.
// Pipeline: file --AES-256-CBC(zero IV)+PKCS7--> container
//           container = magic[9] + props[5] + u64 LE size + LZMA stream
//           --LZMA-ALONE--> compiled Lua 5.1 ("\x1bLua\x51").

#include "angryrift/angryrift.h"

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

#include <lzma.h>

#include "angryrift/angryaes.h"

// NOTE: AES comes from angryaes.h (tiny-AES-c, public domain, decrypt path)
// so the 32-bit (-m32) build needs no multilib OpenSSL headers.

namespace angryrift {
namespace {

constexpr char kLuaSig[4] = {'\x1b', 'L', 'u', 'a'};

// Zero IV per the sub_777E20 call chain (CBC context zero-initialised).
inline constexpr uint8_t kZeroIv[kAesBlockSize] = {0};

}  // namespace

const uint8_t kContentKey[kAesKeySize] = {
    // sub_70770 (.text:0x70850..0x708e3 movb immediates):
    // "USCaPQpA4TSNVxMI1v9SK9UC0yZuAnb2"
    'U', 'S', 'C', 'a', 'P', 'Q', 'p', 'A', '4', 'T', 'S', 'N', 'V', 'x',
    'M', 'I', '1', 'v', '9', 'S', 'K', '9', 'U', 'C', '0', 'y', 'Z', 'u',
    'A', 'n', 'b', '2'};

bool aes_cbc_decrypt(const std::vector<uint8_t>& in,
                     std::vector<uint8_t>& container_out,
                     std::string& error,
                     const uint8_t* key) {
    container_out.clear();
    if (in.empty()) {
        error = "empty input";
        return false;
    }
    if (in.size() % kAesBlockSize != 0) {
        // Mirrors sub_777E20: size must be a multiple of 16.
        char buf[128];
        std::snprintf(buf, sizeof(buf),
                      "size %zu is not a multiple of 16 (sub_777E20 rejects)",
                      in.size());
        error = buf;
        return false;
    }
    if (!key) key = kContentKey;

    // Embedded AES-256-CBC decrypt (zero IV). Key expansion + InvCipher
    // live in angryaes.h (tiny-AES-c, public domain) so -m32 builds need
    // no multilib OpenSSL headers.
    std::vector<uint8_t> out(in.size());
    {
        AES_ctx ctx;
        AES_init_ctx_iv(&ctx, key, kZeroIv);
        AES_CBC_decrypt_buffer(&ctx, out.data(), in.data(),
                               static_cast<uint32_t>(in.size()));
    }

    // PKCS#7 unpad: sub_777E20 reads last byte, requires 1..0x10, trims it.
    const uint8_t pad = out.back();
    if (pad == 0 || pad > kAesBlockSize) {
        char buf[128];
        std::snprintf(buf, sizeof(buf),
                      "bad PKCS#7 pad byte 0x%02x (want 0x01..0x10)", pad);
        error = buf;
        return false;
    }
    for (std::size_t i = 0; i < pad; ++i) {
        if (out[out.size() - 1 - i] != pad) {
            error = "PKCS#7 padding bytes inconsistent (wrong key?)";
            return false;
        }
    }
    out.resize(out.size() - pad);
    container_out = std::move(out);
    return true;
}

bool lzma_alone_decompress(const uint8_t* in, std::size_t in_len,
                           std::vector<uint8_t>& out,
                           std::string& error,
                           uint64_t* declared_size,
                           uint8_t props_out[5]) {
    out.clear();
    // Container after 9-byte magic: props[5] + u64 LE unpacked size + stream.
    if (in_len < 13) {
        error = "container too short for LZMA-ALONE header (need >=13)";
        return false;
    }
    const uint8_t* props = in;
    uint64_t unpacked = 0;
    for (int i = 0; i < 8; ++i)
        unpacked |= static_cast<uint64_t>(in[5 + i]) << (8 * i);
    if (declared_size) *declared_size = unpacked;
    if (props_out) std::memcpy(props_out, props, 5);

    lzma_stream strm = LZMA_STREAM_INIT;
    // FORMAT_ALONE expects exactly props[5]+u64 size+stream, which is what
    // the container holds after the 9-byte magic. 2nd arg is the memlimit,
    // NOT the unpacked size (passing unpacked breaks dict alloc).
    lzma_ret ret = lzma_alone_decoder(&strm, UINT64_MAX);
    if (ret != LZMA_OK) {
        error = "lzma_alone_decoder init failed";
        return false;
    }
    // Feed props+size+stream as one buffer (as stored on disk).
    strm.next_in = in;
    strm.avail_in = in_len;
    std::vector<uint8_t> buf;
    buf.reserve(unpacked > 0 && unpacked < (1u << 26)
                    ? static_cast<std::size_t>(unpacked) + 16
                    : 1u << 16);
    uint8_t chunk[1u << 16];
    bool done = false;
    while (!done) {
        strm.next_out = chunk;
        strm.avail_out = sizeof(chunk);
        ret = lzma_code(&strm, LZMA_FINISH);
        const std::size_t produced = sizeof(chunk) - strm.avail_out;
        buf.insert(buf.end(), chunk, chunk + produced);
        if (ret == LZMA_STREAM_END) {
            done = true;
        } else if (ret != LZMA_OK) {
            char tmp[128];
            std::snprintf(tmp, sizeof(tmp), "LZMA decode failed (ret=%d)",
                          static_cast<int>(ret));
            error = tmp;
            lzma_end(&strm);
            return false;
        } else if (produced == 0 && strm.avail_in == 0) {
            break;  // Ran dry without STREAM_END (over-long size field).
        }
    }
    lzma_end(&strm);
    out = std::move(buf);
    return true;
}

// ---- angryrift.cpp part 2/3: full-pipeline decode + sniffers + helpers ----

DecodeResult decode_bytes(const std::vector<uint8_t>& file_bytes) {
    DecodeResult r;
    std::vector<uint8_t> container;
    std::string err;
    if (!aes_cbc_decrypt(file_bytes, container, err)) {
        r.error = "AES stage: " + err;
        return r;
    }
    r.containerSize = container.size();
    // Variant B: 7z archive container (only options.lua of 2058 samples).
    // sub_15BC10's config.json branch shows the loader handles more than
    // the LZMA-ALONE path; report it instead of failing on magic.
    static const uint8_t k7z[6] = {0x37, 0x7a, 0xbc, 0xaf, 0x27, 0x1c};
    if (container.size() >= 6 &&
        std::memcmp(container.data(), k7z, 6) == 0) {
        r.error =
            "7z-archive container (only scripts/options.lua): AES ok, use "
            "`7z x` on the decrypted bytes; luac is the archived options.lua";
        return r;
    }
    if (container.size() < sizeof(kMagic) + 13) {
        r.error = "container too short after AES unpad";
        return r;
    }
    if (std::memcmp(container.data(), kMagic, sizeof(kMagic)) != 0) {
        r.error = "bad container magic (wrong key or not an AB lua asset)";
        return r;
    }
    const uint8_t* lzma_in = container.data() + sizeof(kMagic);
    const std::size_t lzma_len = container.size() - sizeof(kMagic);
    r.compressedSize = lzma_len > 13 ? lzma_len - 13 : 0;
    std::vector<uint8_t> luac;
    uint64_t declared = 0;
    uint8_t props[5] = {0};
    if (!lzma_alone_decompress(lzma_in, lzma_len, luac, err, &declared, props)) {
        r.error = "LZMA stage: " + err;
        return r;
    }
    r.luac = std::move(luac);
    r.declaredSize = declared;
    std::memcpy(r.lzmaProps, props, 5);
    r.ok = true;
    return r;
}

DecodeResult decode_bytes(const std::string& file_bytes) {
    std::vector<uint8_t> v(file_bytes.begin(), file_bytes.end());
    return decode_bytes(v);
}

DecodeResult decode_file(const std::string& path) {
    std::string err;
    std::vector<uint8_t> raw = read_file_bytes(path, err);
    if (!err.empty()) {
        DecodeResult r;
        r.error = "read: " + err;
        return r;
    }
    return decode_bytes(raw);
}

bool looks_encrypted_lua(const std::vector<uint8_t>& bytes) {
    if (bytes.empty() || bytes.size() % kAesBlockSize != 0) return false;
    // Encrypted blobs have ~7.9 bits/byte entropy; plaintext lua/luac never
    // starts with our 9-byte magic.
    if (bytes.size() >= sizeof(kMagic) &&
        std::memcmp(bytes.data(), kMagic, sizeof(kMagic)) == 0)
        return false;
    if (bytes.size() >= 4 &&
        std::memcmp(bytes.data(), kLuaSig, 4) == 0)
        return false;
    return true;
}

bool looks_decrypted_container(const std::vector<uint8_t>& bytes) {
    return bytes.size() >= sizeof(kMagic) &&
           std::memcmp(bytes.data(), kMagic, sizeof(kMagic)) == 0;
}

bool looks_luac(const std::vector<uint8_t>& bytes) {
    return bytes.size() >= 5 &&
           std::memcmp(bytes.data(), kLuaSig, 4) == 0 &&
           bytes[4] == 0x51;  // Lua 5.1 version byte seen in all samples
}

std::vector<uint8_t> read_file_bytes(const std::string& path, std::string& error) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        error = "cannot open " + path;
        return {};
    }
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(f),
                                 std::istreambuf_iterator<char>());
}

bool write_file_bytes(const std::string& path,
                      const std::vector<uint8_t>& data,
                      std::string& error) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) {
        error = "cannot write " + path;
        return false;
    }
    f.write(reinterpret_cast<const char*>(data.data()),
            static_cast<std::streamsize>(data.size()));
    if (!f) {
        error = "short write " + path;
        return false;
    }
    return true;
}

std::string to_hex(const uint8_t* data, std::size_t len) {
    static const char* kDigits = "0123456789abcdef";
    std::string s;
    s.reserve(len * 2);
    for (std::size_t i = 0; i < len; ++i) {
        s.push_back(kDigits[data[i] >> 4]);
        s.push_back(kDigits[data[i] & 0xF]);
    }
    return s;
}

std::string lua_version_string(const std::vector<uint8_t>& luac) {
    if (!looks_luac(luac)) return "not-luac";
    // luac header: 1b 4c 75 61 51 00 01 04 04 04 ... (Lua 5.1, LE)
    char buf[64];
    std::snprintf(buf, sizeof(buf), "Lua 5.1 (0x%02x) hdr=%s", luac[4],
                  to_hex(luac.data(), luac.size() < 12 ? luac.size() : 12).c_str());
    return buf;
}

std::string probe_bytes(const std::vector<uint8_t>& bytes,
                        const std::string& label) {
    std::ostringstream o;
    o << "angryrift probe: " << label << "\n";
    o << "  raw size      : " << bytes.size();
    if (!bytes.empty())
        o << " (mod16=" << (bytes.size() % 16) << ", head="
          << to_hex(bytes.data(), bytes.size() < 16 ? bytes.size() : 16) << ")";
    o << "\n";
    DecodeResult r = decode_bytes(bytes);
    if (!r.ok) {
        o << "  decode        : FAIL: " << r.error << "\n";
        return o.str();
    }
    o << "  AES           : ok (zero IV, key \"USCaPQpA4TSNVxMI1v9SK9UC0yZuAnb2\")\n";
    o << "  container     : " << r.containerSize << " bytes, magic 894c5a4d410d0a1a0a ok\n";
    o << "  lzma props    : " << to_hex(r.lzmaProps, 5)
      << " declared=" << r.declaredSize
      << " stream=" << r.compressedSize << "\n";
    o << "  luac payload  : " << r.luac.size() << " bytes, "
      << lua_version_string(r.luac) << "\n";
    // First printable strings inside the luac (function/chunk names).
    o << "  strings       : ";
    int shown = 0;
    std::string cur;
    for (std::size_t i = 0; i < r.luac.size() && shown < 8; ++i) {
        const unsigned char c = r.luac[i];
        if (c >= 32 && c < 127) {
            cur.push_back(static_cast<char>(c));
        } else {
            if (cur.size() >= 4) {
                if (shown++) o << " | ";
                o << "'" << cur << "'";
            }
            cur.clear();
        }
    }
    if (cur.size() >= 4 && shown < 8) {
        if (shown++) o << " | ";
        o << "'" << cur << "'";
    }
    o << "\n";
    return o.str();
}

std::string probe_file(const std::string& path) {
    std::string err;
    std::vector<uint8_t> raw = read_file_bytes(path, err);
    if (!err.empty()) return "angryrift probe: " + path + " FAIL: " + err + "\n";
    return probe_bytes(raw, path);
}

}  // namespace angryrift
