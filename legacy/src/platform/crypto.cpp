#include "platform/crypto.hpp"

#include <array>
#include <span>
#include <stdexcept>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#else
#include <openssl/sha.h>
#include <cerrno>
#include <sys/random.h>
#endif

namespace lay::platform {
namespace {

std::string hex(std::span<const unsigned char> bytes) {
    constexpr std::string_view alphabet = "0123456789abcdef";
    std::string result;
    result.reserve(bytes.size() * 2);
    for (const auto byte : bytes) {
        result += alphabet[byte >> 4];
        result += alphabet[byte & 15];
    }
    return result;
}

} // namespace

std::string sha256(std::string_view bytes) {
    std::array<unsigned char, 32> digest{};
#ifdef _WIN32
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) {
        throw std::runtime_error("could not open the SHA-256 provider");
    }
    const auto status = BCryptHash(algorithm, nullptr, 0,
        reinterpret_cast<PUCHAR>(const_cast<char*>(bytes.data())), static_cast<ULONG>(bytes.size()),
        digest.data(), static_cast<ULONG>(digest.size()));
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (status < 0) {
        throw std::runtime_error("could not calculate SHA-256");
    }
#else
    if (!SHA256(reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size(), digest.data())) {
        throw std::runtime_error("could not calculate SHA-256");
    }
#endif
    return hex(digest);
}

std::string random_suffix() {
    std::array<unsigned char, 16> bytes{};
#ifdef _WIN32
    if (BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()),
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) {
        throw std::runtime_error("could not obtain random bytes");
    }
#else
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto count = ::getrandom(bytes.data() + offset, bytes.size() - offset, 0);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            throw std::runtime_error("could not obtain random bytes");
        }
        offset += static_cast<std::size_t>(count);
    }
#endif
    return hex(bytes);
}

} // namespace lay::platform
