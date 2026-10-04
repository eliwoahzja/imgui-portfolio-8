#pragma once

#include <string>
#include <string_view>

// AES-256-CFB128 + Base64, matching the backend's
// openssl_encrypt($str, 'aes-256-cfb', $key, true, $iv) exactly.
//
// Windows CNG rather than CryptoPP: the whole surface is two functions, and
// bcrypt is already on the machine. Both return an empty string on failure.
namespace aes
{
    std::string encrypt(std::string_view data, std::string_view key, std::string_view iv);
    std::string decrypt(std::string_view b64, std::string_view key, std::string_view iv);
}

namespace base64
{
    std::string encode(std::string_view data);
    std::string decode(std::string_view text);
}
