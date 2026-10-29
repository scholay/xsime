#pragma once
#include <array>
#include <algorithm>
#include <string>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#else
#include <memory>
#include <openssl/bn.h>
#include <openssl/ecdsa.h>
#include <openssl/evp.h>
#include <openssl/x509.h>
#endif

namespace rimes::updates {
inline bool VerifyP256(const std::vector<unsigned char>& public_key, const std::string& payload,
                       const std::vector<unsigned char>& signature) {
    if (public_key.size() != 65 || public_key[0] != 4 || signature.size() != 64) return false;
#ifdef _WIN32
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_KEY_HANDLE key = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0) < 0) return false;
    struct PublicBlob { BCRYPT_ECCKEY_BLOB header{BCRYPT_ECDSA_PUBLIC_P256_MAGIC, 32}; unsigned char coordinates[64]{}; } blob;
    std::copy(public_key.begin() + 1, public_key.end(), blob.coordinates);
    bool verified = false;
    if (BCryptImportKeyPair(algorithm, nullptr, BCRYPT_ECCPUBLIC_BLOB, &key,
            reinterpret_cast<PUCHAR>(&blob), static_cast<ULONG>(sizeof(blob)), 0) >= 0) {
        std::array<unsigned char, 32> hash{};
        if (BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0,
                reinterpret_cast<PUCHAR>(const_cast<char*>(payload.data())), static_cast<ULONG>(payload.size()),
                hash.data(), static_cast<ULONG>(hash.size())) >= 0)
            verified = BCryptVerifySignature(key, nullptr, hash.data(), static_cast<ULONG>(hash.size()),
                const_cast<PUCHAR>(signature.data()), static_cast<ULONG>(signature.size()), 0) >= 0;
        BCryptDestroyKey(key);
    }
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return verified;
#else
    // SubjectPublicKeyInfo for id-ecPublicKey / prime256v1, followed by the
    // uncompressed X9.63 point from the compiled trusted-key registry.
    const std::array<unsigned char, 26> prefix = {0x30,0x59,0x30,0x13,0x06,0x07,0x2a,0x86,0x48,0xce,0x3d,0x02,0x01,
        0x06,0x08,0x2a,0x86,0x48,0xce,0x3d,0x03,0x01,0x07,0x03,0x42,0x00};
    std::vector<unsigned char> encoded_key(prefix.begin(), prefix.end());
    encoded_key.insert(encoded_key.end(), public_key.begin(), public_key.end());
    const auto* cursor = encoded_key.data();
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(d2i_PUBKEY(nullptr, &cursor, static_cast<long>(encoded_key.size())), EVP_PKEY_free);
    std::unique_ptr<ECDSA_SIG, decltype(&ECDSA_SIG_free)> sig(ECDSA_SIG_new(), ECDSA_SIG_free);
    std::unique_ptr<BIGNUM, decltype(&BN_free)> r(BN_bin2bn(signature.data(), 32, nullptr), BN_free);
    std::unique_ptr<BIGNUM, decltype(&BN_free)> s(BN_bin2bn(signature.data() + 32, 32, nullptr), BN_free);
    if (!key || !sig || !r || !s || ECDSA_SIG_set0(sig.get(), r.get(), s.get()) != 1) return false;
    r.release(); s.release();
    const auto length = i2d_ECDSA_SIG(sig.get(), nullptr);
    if (length <= 0) return false;
    std::vector<unsigned char> encoded_signature(static_cast<std::size_t>(length));
    auto* output = encoded_signature.data();
    if (i2d_ECDSA_SIG(sig.get(), &output) != length) return false;
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    return context && EVP_DigestVerifyInit(context.get(), nullptr, EVP_sha256(), nullptr, key.get()) == 1 &&
        EVP_DigestVerify(context.get(), encoded_signature.data(), encoded_signature.size(),
            reinterpret_cast<const unsigned char*>(payload.data()), payload.size()) == 1;
#endif
}
} // namespace rimes::updates
