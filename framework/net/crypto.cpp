#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#include <wincrypt.h>

#include <vector>

#include "crypto.h"

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "crypt32.lib")

namespace
{
    constexpr size_t block_size = 16;

    // CNG's BCRYPT_CHAIN_MODE_CFB is CFB8; OpenSSL's aes-256-cfb is CFB128.
    // So the mode is built by hand on top of a raw ECB block encryption, which
    // is the primitive both directions of CFB need.
    struct aes_ecb
    {
        BCRYPT_ALG_HANDLE alg = nullptr;
        BCRYPT_KEY_HANDLE key = nullptr;

        bool open(std::string_view k)
        {
            if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_AES_ALGORITHM, nullptr, 0)))
                return false;

            if (!BCRYPT_SUCCESS(BCryptSetProperty(alg, BCRYPT_CHAINING_MODE,
                    (PUCHAR)BCRYPT_CHAIN_MODE_ECB, sizeof(BCRYPT_CHAIN_MODE_ECB), 0)))
                return false;

            return BCRYPT_SUCCESS(BCryptGenerateSymmetricKey(alg, &key, nullptr, 0,
                (PUCHAR)k.data(), (ULONG)k.size(), 0));
        }

        bool block(const unsigned char* in, unsigned char* out) const
        {
            ULONG written = 0;
            return BCRYPT_SUCCESS(BCryptEncrypt(key, (PUCHAR)in, (ULONG)block_size,
                nullptr, nullptr, 0, out, (ULONG)block_size, &written, 0))
                && written == block_size;
        }

        ~aes_ecb()
        {
            if (key) BCryptDestroyKey(key);
            if (alg) BCryptCloseAlgorithmProvider(alg, 0);
        }
    };

    // One pass of CFB128. The feedback register takes the ciphertext block
    // either way round, which is the input when decrypting and the output when
    // encrypting — the only difference between the two directions.
    bool cfb128(std::string_view key, std::string_view iv,
        const unsigned char* in, size_t len, unsigned char* out, bool encrypting)
    {
        if (key.size() != 32 || iv.size() != block_size)
            return false;

        aes_ecb aes;
        if (!aes.open(key))
            return false;

        unsigned char feedback[block_size];
        unsigned char stream[block_size];
        memcpy(feedback, iv.data(), block_size);

        for (size_t off = 0; off < len; off += block_size)
        {
            if (!aes.block(feedback, stream))
                return false;

            const size_t n = (len - off < block_size) ? len - off : block_size;
            for (size_t i = 0; i < n; ++i)
                out[off + i] = in[off + i] ^ stream[i];

            const unsigned char* next = encrypting ? out + off : in + off;
            memcpy(feedback, next, n);
        }
        return true;
    }

    std::string b64_encode(const unsigned char* data, size_t len)
    {
        DWORD chars = 0;
        if (!CryptBinaryToStringA(data, (DWORD)len,
                CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &chars))
            return {};

        std::string out(chars, '\0');
        if (!CryptBinaryToStringA(data, (DWORD)len,
                CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, out.data(), &chars))
            return {};

        out.resize(strnlen(out.c_str(), out.size()));
        return out;
    }

    std::vector<unsigned char> b64_decode(std::string_view text)
    {
        DWORD bytes = 0;
        if (!CryptStringToBinaryA(text.data(), (DWORD)text.size(),
                CRYPT_STRING_BASE64, nullptr, &bytes, nullptr, nullptr))
            return {};

        std::vector<unsigned char> out(bytes);
        if (!CryptStringToBinaryA(text.data(), (DWORD)text.size(),
                CRYPT_STRING_BASE64, out.data(), &bytes, nullptr, nullptr))
            return {};

        out.resize(bytes);
        return out;
    }
}

namespace aes
{
    std::string encrypt(std::string_view data, std::string_view key, std::string_view iv)
    {
        if (data.empty())
            return {};

        std::vector<unsigned char> ct(data.size());
        if (!cfb128(key, iv, (const unsigned char*)data.data(), data.size(), ct.data(), true))
            return {};

        return b64_encode(ct.data(), ct.size());
    }

    std::string decrypt(std::string_view b64, std::string_view key, std::string_view iv)
    {
        const std::vector<unsigned char> ct = b64_decode(b64);
        if (ct.empty())
            return {};

        std::string out(ct.size(), '\0');
        if (!cfb128(key, iv, ct.data(), ct.size(), (unsigned char*)out.data(), false))
            return {};

        return out;
    }
}

namespace base64
{
    std::string encode(std::string_view data)
    {
        if (data.empty())
            return {};

        return b64_encode((const unsigned char*)data.data(), data.size());
    }

    std::string decode(std::string_view text)
    {
        if (text.empty())
            return {};

        const std::vector<unsigned char> bytes = b64_decode(text);
        return bytes.empty() ? std::string() : std::string(bytes.begin(), bytes.end());
    }
}
