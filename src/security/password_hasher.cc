#include "security/password_hasher.h"

#include <crypt.h>
#include <sys/random.h>

#include <array>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

constexpr std::size_t kBcryptHashLength = 60;
constexpr std::size_t kBcryptSaltBytes = 16;

bool HasEmbeddedNul(const std::string& value) { return value.find('\0') != std::string::npos; }

bool IsBcryptHash(const std::string& encoded_hash) {
    if (encoded_hash.size() != kBcryptHashLength) {
        return false;
    }
    return encoded_hash.compare(0, 4, "$2a$") == 0 || encoded_hash.compare(0, 4, "$2b$") == 0 ||
           encoded_hash.compare(0, 4, "$2y$") == 0;
}

bool IsLegacyDecimalHash(const std::string& encoded_hash) {
    if (encoded_hash.empty() || encoded_hash.size() > 20) {
        return false;
    }
    for (const unsigned char ch : encoded_hash) {
        if (!std::isdigit(ch)) {
            return false;
        }
    }
    return true;
}

bool ConstantTimeEquals(const std::string& left, const std::string& right) {
    if (left.size() != right.size()) {
        return false;
    }
    unsigned char difference = 0;
    for (std::size_t i = 0; i < left.size(); ++i) {
        difference |= static_cast<unsigned char>(left[i]) ^ static_cast<unsigned char>(right[i]);
    }
    return difference == 0;
}

bool FillRandom(std::array<unsigned char, kBcryptSaltBytes>* bytes) {
    std::size_t offset = 0;
    while (offset < bytes->size()) {
        const ssize_t count = getrandom(bytes->data() + offset, bytes->size() - offset, 0);
        if (count > 0) {
            offset += static_cast<std::size_t>(count);
            continue;
        }
        if (count < 0 && errno == EINTR) {
            continue;
        }
        return false;
    }
    return true;
}

std::uint64_t LegacyLibstdcxxHash(const std::string& password) {
    constexpr std::uint64_t kMultiplier = 0xc6a4a7935bd1e995ULL;
    constexpr int kShift = 47;
    constexpr std::uint64_t kSeed = 0xc70f6907ULL;

    const auto* data = reinterpret_cast<const unsigned char*>(password.data());
    std::size_t remaining = password.size();
    std::uint64_t hash = kSeed ^ (static_cast<std::uint64_t>(remaining) * kMultiplier);
    while (remaining >= sizeof(std::uint64_t)) {
        std::uint64_t block = 0;
        std::memcpy(&block, data, sizeof(block));
        block *= kMultiplier;
        block ^= block >> kShift;
        block *= kMultiplier;
        hash ^= block;
        hash *= kMultiplier;
        data += sizeof(block);
        remaining -= sizeof(block);
    }

    switch (remaining) {
        case 7:
            hash ^= static_cast<std::uint64_t>(data[6]) << 48;
            [[fallthrough]];
        case 6:
            hash ^= static_cast<std::uint64_t>(data[5]) << 40;
            [[fallthrough]];
        case 5:
            hash ^= static_cast<std::uint64_t>(data[4]) << 32;
            [[fallthrough]];
        case 4:
            hash ^= static_cast<std::uint64_t>(data[3]) << 24;
            [[fallthrough]];
        case 3:
            hash ^= static_cast<std::uint64_t>(data[2]) << 16;
            [[fallthrough]];
        case 2:
            hash ^= static_cast<std::uint64_t>(data[1]) << 8;
            [[fallthrough]];
        case 1:
            hash ^= static_cast<std::uint64_t>(data[0]);
            hash *= kMultiplier;
            [[fallthrough]];
        case 0:
            break;
    }

    hash ^= hash >> kShift;
    hash *= kMultiplier;
    hash ^= hash >> kShift;
    return hash;
}

std::string LegacyHash(const std::string& password) { return std::to_string(LegacyLibstdcxxHash(password)); }

}  // namespace

namespace chat {

BcryptPasswordHasher::BcryptPasswordHasher(std::uint32_t cost) : cost_(cost) {}

std::optional<std::string> BcryptPasswordHasher::Hash(const std::string& password) const {
    if (password.empty() || password.size() > 72 || HasEmbeddedNul(password) || cost_ < 4 || cost_ > 31) {
        return std::nullopt;
    }

    std::array<unsigned char, kBcryptSaltBytes> random_bytes{};
    if (!FillRandom(&random_bytes)) {
        return std::nullopt;
    }

    std::array<char, CRYPT_GENSALT_OUTPUT_SIZE> setting{};
    if (crypt_gensalt_rn("$2b$", cost_, reinterpret_cast<const char*>(random_bytes.data()), random_bytes.size(),
                         setting.data(), setting.size()) == nullptr) {
        return std::nullopt;
    }

    crypt_data data{};
    const char* encoded = crypt_r(password.c_str(), setting.data(), &data);
    if (encoded == nullptr || !IsBcryptHash(encoded)) {
        return std::nullopt;
    }
    return std::string(encoded);
}

bool BcryptPasswordHasher::Verify(const std::string& password, const std::string& encoded_hash) const {
    if (password.empty() || password.size() > 72 || HasEmbeddedNul(password)) {
        return false;
    }
    if (IsLegacyDecimalHash(encoded_hash)) {
        return ConstantTimeEquals(LegacyHash(password), encoded_hash);
    }
    if (!IsBcryptHash(encoded_hash)) {
        return false;
    }

    crypt_data data{};
    const char* candidate = crypt_r(password.c_str(), encoded_hash.c_str(), &data);
    return candidate != nullptr && ConstantTimeEquals(candidate, encoded_hash);
}

bool BcryptPasswordHasher::NeedsRehash(const std::string& encoded_hash) const {
    if (!IsBcryptHash(encoded_hash)) {
        return true;
    }
    char* end = nullptr;
    const std::string encoded_cost_text = encoded_hash.substr(4, 2);
    const auto encoded_cost = std::strtoul(encoded_cost_text.c_str(), &end, 10);
    return end == nullptr || *end != '\0' || encoded_cost != cost_;
}

}  // namespace chat
