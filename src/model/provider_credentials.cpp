#include "model/provider.hpp"

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <stdexcept>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wincred.h>
#endif

namespace lay {
namespace {

void validate_key(std::string_view key) {
    if (key.empty() || key.size() > 1024 ||
        !std::ranges::all_of(key, [](unsigned char c) { return c > 32 && c < 127; })) {
        throw std::invalid_argument("The API key must be a non-empty token of at most 1024 bytes.");
    }
}

#ifdef _WIN32
std::wstring target(const Provider& provider) {
    const std::string selected = "LayCode/" + provider.name + "/" + provider.endpoint;
    return std::wstring(selected.begin(), selected.end());
}
#endif

} // namespace

std::string provider_key(const Provider& provider) {
    if (provider.api_key_env.empty()) {
        return {};
    }
    if (const auto* value = std::getenv(provider.api_key_env.c_str()); value && *value) {
        validate_key(value);
        return value;
    }
#ifdef _WIN32
    PCREDENTIALW credential = nullptr;
    if (CredReadW(target(provider).c_str(), CRED_TYPE_GENERIC, 0, &credential)) {
        std::unique_ptr<CREDENTIALW, decltype(&CredFree)> owner(credential, CredFree);
        std::string key(reinterpret_cast<const char*>(credential->CredentialBlob), credential->CredentialBlobSize);
        validate_key(key);
        return key;
    }
#endif
    throw std::invalid_argument("Set " + provider.api_key_env + " or use --set-provider-key --provider " + provider.name + ".");
}

void save_provider_key(const Provider& provider, std::string_view key) {
    if (provider.api_key_env.empty()) {
        throw std::invalid_argument("This provider is configured for unauthenticated access.");
    }
    validate_key(key);
#ifdef _WIN32
    auto name = target(provider);
    CREDENTIALW credential{};
    credential.Type = CRED_TYPE_GENERIC;
    credential.TargetName = name.data();
    credential.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<char*>(key.data()));
    credential.CredentialBlobSize = static_cast<DWORD>(key.size());
    credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
    if (!CredWriteW(&credential, 0)) {
        throw std::runtime_error("Windows Credential Manager could not save the API key.");
    }
#else
    throw std::invalid_argument("Set " + provider.api_key_env + " in the terminal environment on this platform.");
#endif
}

} // namespace lay
