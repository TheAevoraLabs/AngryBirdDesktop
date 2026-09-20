#pragma once
// angryrift.h — Angry Birds Classic encrypted-Lua decoder ("Angry Rift").
//
// Role model: Swordigo's filerift (src/tools/filerift.h):
//   filerift::decode_protobuf(bytes, filetype)  -> text markup
//   filerift::recode_markup(text, filetype)     -> binary protobuf
//   filerift::extract_lua_generic(bytes)        -> lua chunk
// Angry Rift is the same *shape* of tool, but for Angry Birds Classic
// encrypted .lua assets (NOT protobuf):
//   raw file = AES-256-CBC (zero IV) of [ container ]
//   container = 9-byte magic + LZMA-ALONE stream
//             = 89 4C 5A 4D 41 0D 0A 1A 0A  ("~LZMA~\r\n\x1a\n"-style)
//               + 5-byte LZMA props + 8-byte LE uncompressed size + LZMA data
//   payload after LZMA = compiled Lua 5.1 bytecode ("\x1bLua", version 0x51)
//
// Reverse-engineered from bin/libAngryBirdsClassic.so (x86, 32-bit):
//   key      : sub_70770 builds 32-byte key "USCaPQpA4TSNVxMI1v9SK9UC0yZuAnb2"
//              (movb immediates at .text:0x70850..0x708e3)
//   decryptor: sub_777240 (key 16/24/32 octets -> KeyLengthBits 128/192/256)
//              sub_8EE1A0 (AES key expansion) + sub_8F2D20 (CBC decrypt,
//              a5==1 path). sub_777E20 enforces 16-byte alignment, zero-IV
//              CBC, and PKCS#7 pad check (last byte 1..0x10, trimmed).
//   pipeline : sub_15A4A0 "Failed to decrypt input data"
//              = sub_777D40 (ctor) + sub_777E20 (AES-CBC+unpad)
//              then callers sub_15BC10/sub_15C1E0/sub_15C490 inflate the
//              buffer via sub_75F770/sub_15BAF0 (LZMA) and hand the luac
//              chunk to the Lua VM (loadLuaFile/GameLua.cpp,
//              loadAssetPackLuaFile, ...).
//
// Usage:
//   auto out = angryrift::decode_file("/path/to/file.lua"); // luac bytes
//   std::string info = angryrift::probe_file(path);          // human report
// CLI (tools/angryrift_cli): decode <in.lua> [out.luac], probe, batch.

#include <cstdint>
#include <string>
#include <vector>

namespace angryrift {

// ---------------------------------------------------------------- constants
inline constexpr std::size_t kAesKeySize = 32;
inline constexpr std::size_t kAesBlockSize = 16;
// 9-byte container magic seen on every decrypted payload.
inline constexpr uint8_t kMagic[9] = {
    0x89, 0x4C, 0x5A, 0x4D, 0x41, 0x0D, 0x0A, 0x1A, 0x0A
};
// The single baked-in content key from sub_70770.
extern const uint8_t kContentKey[kAesKeySize];

// ------------------------------------------------------------ result struct
struct DecodeResult {
    bool ok = false;
    std::string error;                 // set when !ok
    std::vector<uint8_t> luac;         // raw compiled-lua payload (post-LZMA)
    uint64_t declaredSize = 0;         // uncompressed size from LZMA header
    uint8_t lzmaProps[5] = {0};
    std::size_t containerSize = 0;     // bytes after AES unpad
    std::size_t compressedSize = 0;    // LZMA stream bytes after 13B header
};

// ------------------------------------------------------------ core pipeline
// AES-256-CBC (zero IV) + PKCS#7 unpad -> container bytes.
bool aes_cbc_decrypt(const std::vector<uint8_t>& in,
                     std::vector<uint8_t>& container_out,
                     std::string& error,
                     const uint8_t* key = kContentKey);

// LZMA-ALONE (props[5] + u64 size + stream) -> raw bytes.
bool lzma_alone_decompress(const uint8_t* in, std::size_t in_len,
                           std::vector<uint8_t>& out,
                           std::string& error,
                           uint64_t* declared_size = nullptr,
                           uint8_t props_out[5] = nullptr);

// Full file pipeline: validate alignment -> decrypt -> magic check -> LZMA.
DecodeResult decode_bytes(const std::vector<uint8_t>& file_bytes);
DecodeResult decode_bytes(const std::string& file_bytes);
DecodeResult decode_file(const std::string& path);

// Header sniffers (no full decompression needed).
bool looks_encrypted_lua(const std::vector<uint8_t>& bytes);
bool looks_decrypted_container(const std::vector<uint8_t>& bytes);
bool looks_luac(const std::vector<uint8_t>& bytes);

// Human-readable one-file report (sizes, header, key check, lua strings).
std::string probe_file(const std::string& path);
std::string probe_bytes(const std::vector<uint8_t>& bytes,
                        const std::string& label);

// Tiny helpers.
std::vector<uint8_t> read_file_bytes(const std::string& path, std::string& error);
bool write_file_bytes(const std::string& path,
                      const std::vector<uint8_t>& data,
                      std::string& error);
std::string to_hex(const uint8_t* data, std::size_t len);
std::string lua_version_string(const std::vector<uint8_t>& luac);

}  // namespace angryrift
