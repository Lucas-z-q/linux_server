#include "security/password_hasher.h"

#include <cassert>
#include <functional>
#include <iostream>
#include <string>

namespace {

void TestHashAndVerify() {
    chat::BcryptPasswordHasher hasher;
    const auto first = hasher.Hash("correct-password");
    const auto second = hasher.Hash("correct-password");

    assert(first.has_value());
    assert(second.has_value());
    assert(*first != "correct-password");
    assert(*first != std::to_string(std::hash<std::string>{}("correct-password")));
    assert(first->size() == 60);
    assert(first->compare(0, 4, "$2b$") == 0);
    assert(*first != *second);
    assert(first->substr(7, 22) != second->substr(7, 22));
    assert(hasher.Verify("correct-password", *first));
    assert(!hasher.Verify("wrong-password", *first));
    assert(!hasher.NeedsRehash(*first));
}

void TestInvalidInputsAndHashesFailClosed() {
    chat::BcryptPasswordHasher hasher;
    const std::string with_nul("abc\0def", 7);
    const std::string malformed_cost = "$2b$xx$" + std::string(53, 'a');
    const std::string malformed_payload = "$2b$12$" + std::string(53, '!');

    assert(!hasher.Hash("").has_value());
    assert(!hasher.Hash(std::string(73, 'x')).has_value());
    assert(!hasher.Hash(with_nul).has_value());
    assert(!hasher.Verify("password", ""));
    assert(!hasher.Verify("password", "$2b$12$broken"));
    assert(!hasher.Verify("password", "$9z$12$unknown-format"));
    assert(!hasher.Verify("password", malformed_cost));
    assert(!hasher.Verify("password", malformed_payload));
    assert(!hasher.Verify(with_nul, "$2b$12$broken"));
    assert(hasher.NeedsRehash(""));
    assert(hasher.NeedsRehash("$2b$12$broken"));
    assert(hasher.NeedsRehash(malformed_cost));
    assert(hasher.NeedsRehash("$9z$12$unknown-format"));
}

void TestLegacyAndOldCostNeedRehash() {
    chat::BcryptPasswordHasher hasher;
    const std::string legacy = std::to_string(std::hash<std::string>{}("legacy-password"));

    assert(hasher.Verify("legacy-password", legacy));
    assert(!hasher.Verify("wrong-password", legacy));
    assert(hasher.NeedsRehash(legacy));

    chat::BcryptPasswordHasher old_cost_hasher(4);
    const auto old_cost_hash = old_cost_hasher.Hash("correct-password");
    assert(old_cost_hash.has_value());
    assert(hasher.Verify("correct-password", *old_cost_hash));
    assert(hasher.NeedsRehash(*old_cost_hash));
}

}  // namespace

int main() {
    TestHashAndVerify();
    TestInvalidInputsAndHashesFailClosed();
    TestLegacyAndOldCostNeedRehash();
    std::cout << "[PASS] password hasher tests passed\n";
    return 0;
}
