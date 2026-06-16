#ifndef LINUX_SERVER_INCLUDE_SECURITY_PASSWORD_HASHER_H_
#define LINUX_SERVER_INCLUDE_SECURITY_PASSWORD_HASHER_H_

#include <cstdint>
#include <optional>
#include <string>

namespace chat {

class IPasswordHasher {
   public:
    virtual ~IPasswordHasher() = default;

    virtual std::optional<std::string> Hash(const std::string& password) const = 0;
    virtual bool Verify(const std::string& password, const std::string& encoded_hash) const = 0;
    virtual bool NeedsRehash(const std::string& encoded_hash) const = 0;
};

class BcryptPasswordHasher : public IPasswordHasher {
   public:
    explicit BcryptPasswordHasher(std::uint32_t cost = 12);

    std::optional<std::string> Hash(const std::string& password) const override;
    bool Verify(const std::string& password, const std::string& encoded_hash) const override;
    bool NeedsRehash(const std::string& encoded_hash) const override;

   private:
    std::uint32_t cost_;
};

}  // namespace chat

#endif  // LINUX_SERVER_INCLUDE_SECURITY_PASSWORD_HASHER_H_
