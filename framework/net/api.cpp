#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>

#include <cstdio>
#include <cstdlib>
#include <vector>

#include "api.h"
#include "crypto.h"
#include "http.h"
#include "token.h"

#pragma comment(lib, "bcrypt.lib")

namespace
{
    // Bootstrap keys, shared with the backend's aes::$a / aes::$b. They only
    // protect the handshake; everything after runs on per-session keys.
    constexpr char static_key[] = "pogvbufpcusmftrurrszycfefhidvdfs";
    constexpr char static_iv[]  = "kspgywjaldzgwphn";

    constexpr char gateway[] = "/forum/app.php";

    // The handshake nonce doubles as the temporary key material: first 32 chars
    // are the cipher key, the next 16 the IV. The backend slices it the same way.
    constexpr size_t nonce_len = 48;

    std::string random_nonce()
    {
        unsigned char raw[nonce_len];
        if (!BCRYPT_SUCCESS(BCryptGenRandom(nullptr, raw, (ULONG)sizeof(raw),
                BCRYPT_USE_SYSTEM_PREFERRED_RNG)))
            return {};

        static const char alphabet[] = "abcdefghijklmnopqrstuvwxyz0123456789";

        std::string out(nonce_len, '\0');
        for (size_t i = 0; i < nonce_len; ++i)
            out[i] = alphabet[raw[i] % (sizeof(alphabet) - 1)];

        return out;
    }

    void split(const std::string& text, char sep, std::vector<std::string>& out, size_t limit)
    {
        out.clear();

        size_t start = 0;
        while (out.size() + 1 < limit)
        {
            const size_t at = text.find(sep, start);
            if (at == std::string::npos)
                break;

            out.push_back(text.substr(start, at - start));
            start = at + 1;
        }
        out.push_back(text.substr(start));
    }

    // A single-field response is always an error code under the static keys —
    // that is how every endpoint bails out before session keys are established.
    std::string static_error(const std::string& body)
    {
        const std::string decoded = aes::decrypt(body, static_key, static_iv);
        return decoded.empty() ? std::string("bad_response") : decoded;
    }

    struct session
    {
        std::string cipher;
        std::string iv;
        std::string sid;
    };

    bool handshake(session& out, std::string& err)
    {
        const std::string nonce = random_nonce();
        if (nonce.empty())
        {
            err = "rng_failed";
            return false;
        }

        const std::string temp_key = nonce.substr(0, 32);
        const std::string temp_iv  = nonce.substr(32, 16);

        const std::string op = aes::encrypt("authentication", static_key, static_iv);
        const std::string a  = aes::encrypt(nonce, static_key, static_iv);
        const std::string b  = aes::encrypt(nonce, temp_key, temp_iv);

        if (op.empty() || a.empty() || b.empty())
        {
            err = "crypto_failed";
            return false;
        }

        const std::string path = std::string(gateway)
            + "?op=" + http::url_encode(op)
            + "&a="  + http::url_encode(a)
            + "&b="  + http::url_encode(b);

        std::string body;
        if (!http::get(path, body))
        {
            err = "network";
            return false;
        }

        std::vector<std::string> parts;
        split(body, ';', parts, 3);

        if (parts.size() < 2)
        {
            err = static_error(body);
            return false;
        }

        out.cipher = aes::decrypt(parts[0], temp_key, temp_iv);
        out.iv     = aes::decrypt(parts[1], temp_key, temp_iv);

        if (out.cipher.size() != 32 || out.iv.size() != 16)
        {
            err = "bad_session";
            return false;
        }

        // The sid is optional and only accepted at its exact length; anything
        // else means a truncated or corrupted field, and falling back to the
        // cookie is safer than sending garbage as `s=`.
        if (parts.size() >= 3)
        {
            const std::string sid = aes::decrypt(parts[2], temp_key, temp_iv);
            if (sid.size() == 32)
                out.sid = sid;
        }

        return true;
    }

    long long to_int(const std::string& text)
    {
        char* end = nullptr;
        const long long value = _strtoi64(text.c_str(), &end, 10);
        return (end && *end == '\0' && end != text.c_str()) ? value : -1;
    }

    void clamp(std::string& text, size_t max)
    {
        if (text.size() > max)
            text.resize(max);
    }

    bool protocol_text(const std::string& text)
    {
        if (text.empty())
            return false;

        for (unsigned char c : text)
            if (c < 0x20 || c > 0x7E)
                return false;
        return true;
    }

    bool cloud_call(const char* operation, const std::string& argument,
        std::string& payload, std::string& err)
    {
        std::string tok;
        if (!token::read_own(tok))
        {
            err = "no_token";
            return false;
        }

        session ses;
        if (!handshake(ses, err))
            return false;

        const std::string op = aes::encrypt(operation, static_key, static_iv);
        const std::string a  = aes::encrypt(tok, ses.cipher, ses.iv);
        const std::string b  = aes::encrypt("rf", ses.cipher, ses.iv);
        const std::string c  = argument.empty()
            ? std::string()
            : aes::encrypt(argument, ses.cipher, ses.iv);

        if (op.empty() || a.empty() || b.empty() || (!argument.empty() && c.empty()))
        {
            err = "crypto_failed";
            return false;
        }

        std::string path = std::string(gateway)
            + "?op=" + http::url_encode(op)
            + "&a="  + http::url_encode(a)
            + "&b="  + http::url_encode(b);

        if (!c.empty())
            path += "&c=" + http::url_encode(c);

        if (!ses.sid.empty())
            path += "&s=" + http::url_encode(ses.sid);

        std::string body;
        if (!http::get(path, body))
        {
            err = "network";
            return false;
        }

        payload = aes::decrypt(body, ses.cipher, ses.iv);
        if (!protocol_text(payload))
        {
            err = static_error(body);
            return false;
        }

        return true;
    }
}

namespace api
{
    bool fetch_profile(profile_data& out, std::string& err)
    {
        std::string tok;
        if (!token::read_own(tok))
        {
            err = "no_token";
            return false;
        }

        session ses;
        if (!handshake(ses, err))
            return false;

        const std::string op = aes::encrypt("profile", static_key, static_iv);
        const std::string a  = aes::encrypt(tok, ses.cipher, ses.iv);

        if (op.empty() || a.empty())
        {
            err = "crypto_failed";
            return false;
        }

        std::string path = std::string(gateway)
            + "?op=" + http::url_encode(op)
            + "&a="  + http::url_encode(a);

        if (!ses.sid.empty())
            path += "&s=" + http::url_encode(ses.sid);

        std::string body;
        if (!http::get(path, body))
        {
            err = "network";
            return false;
        }

        const std::string payload = aes::decrypt(body, ses.cipher, ses.iv);
        if (!protocol_text(payload))
        {
            err = static_error(body);
            return false;
        }

        // The response carries no MAC, so every field is validated before it
        // reaches the UI: fixed arity, `ok` marker, strict numeric parses.
        std::vector<std::string> f;
        split(payload, ';', f, 6);

        if (f.size() != 6 || f[0] != "ok")
        {
            err = f.empty() ? "bad_response" : f[0];
            return false;
        }

        const long long uid    = to_int(f[1]);
        const long long end_ts = to_int(f[2]);
        const long long avatar = to_int(f[3]);

        if (uid <= 0 || end_ts < 0 || avatar < 0)
        {
            err = "bad_response";
            return false;
        }

        out.uid         = (int)uid;
        out.end_ts      = end_ts;
        out.avatar_date = avatar;
        out.username    = f[5];   // f[4] is the group title, which the UI drops

        clamp(out.username, 48);

        if (out.username.empty())
        {
            err = "bad_response";
            return false;
        }

        return true;
    }

    bool save_cloud_config(const std::string& name, const std::string& content,
        std::string& out_code, std::string& err)
    {
        out_code.clear();
        err.clear();

        if (name.empty() || content.empty())
        {
            err = "bad_request";
            return false;
        }

        const std::string encoded_name = base64::encode(name);
        const std::string encoded_body = base64::encode(content);
        if (encoded_name.empty() || encoded_body.empty())
        {
            err = "crypto_failed";
            return false;
        }

        std::string payload;
        if (!cloud_call("cloudcfg_save", encoded_name + "." + encoded_body,
                payload, err))
            return false;

        std::vector<std::string> fields;
        split(payload, ';', fields, 2);
        if (fields.size() != 2 || fields[0] != "ok"
            || fields[1].size() < 16 || fields[1].size() > 24)
        {
            err = fields.empty() ? "bad_response" : fields[0];
            return false;
        }

        for (unsigned char c : fields[1])
        {
            if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')))
            {
                err = "bad_response";
                return false;
            }
        }

        out_code = fields[1];
        return true;
    }

    bool load_cloud_config(const std::string& code, cloud_config_data& out,
        std::string& err)
    {
        out = {};
        err.clear();

        if (code.size() < 16 || code.size() > 24)
        {
            err = "bad_request";
            return false;
        }

        for (unsigned char c : code)
        {
            if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')))
            {
                err = "bad_request";
                return false;
            }
        }

        std::string payload;
        if (!cloud_call("cloudcfg_load", code, payload, err))
            return false;

        std::vector<std::string> fields;
        split(payload, ';', fields, 6);
        if (fields.size() != 6 || fields[0] != "ok")
        {
            err = fields.empty() ? "bad_response" : fields[0];
            return false;
        }

        const long long uid = to_int(fields[2]);
        if (uid <= 0)
        {
            err = "bad_response";
            return false;
        }

        out.code     = fields[1];
        out.uid      = (int)uid;
        out.name     = base64::decode(fields[3]);
        out.username = base64::decode(fields[4]);
        out.content  = base64::decode(fields[5]);

        if (out.code != code || out.name.empty() || out.username.empty()
            || out.content.empty())
        {
            out = {};
            err = "bad_response";
            return false;
        }

        clamp(out.name, 64);
        clamp(out.username, 50);
        return true;
    }

    bool list_named_configs(std::vector<named_config_meta>& out, std::string& err)
    {
        out.clear();
        err.clear();

        std::string payload;
        if (!cloud_call("cloudcfg_list", "", payload, err))
            return false;

        std::vector<std::string> fields;
        split(payload, ';', fields, 102);
        if (fields.size() < 2 || fields[0] != "ok")
        {
            err = fields.empty() ? "bad_response" : fields[0];
            return false;
        }

        const long long count = to_int(fields[1]);
        if (count < 0 || count > 50 || fields.size() != (size_t)(2 + count * 2))
        {
            err = "bad_response";
            return false;
        }

        out.reserve((size_t)count);
        for (long long i = 0; i < count; ++i)
        {
            named_config_meta item;
            item.name = base64::decode(fields[(size_t)(2 + i * 2)]);
            item.updated_ts = to_int(fields[(size_t)(3 + i * 2)]);
            if (item.name.empty() || item.updated_ts < 0)
            {
                out.clear();
                err = "bad_response";
                return false;
            }
            out.push_back(std::move(item));
        }
        return true;
    }

    bool get_named_config(const std::string& name, std::string& out_content,
        long long& out_updated_ts, std::string& err)
    {
        out_content.clear();
        out_updated_ts = 0;
        err.clear();

        const std::string encoded_name = base64::encode(name);
        if (encoded_name.empty())
        {
            err = "bad_request";
            return false;
        }

        std::string payload;
        if (!cloud_call("cloudcfg_get", encoded_name, payload, err))
            return false;

        std::vector<std::string> fields;
        split(payload, ';', fields, 4);
        if (fields.size() != 4 || fields[0] != "ok"
            || base64::decode(fields[1]) != name)
        {
            err = fields.empty() ? "bad_response" : fields[0];
            return false;
        }

        out_updated_ts = to_int(fields[2]);
        out_content = base64::decode(fields[3]);
        if (out_updated_ts < 0 || out_content.empty())
        {
            out_content.clear();
            err = "bad_response";
            return false;
        }
        return true;
    }

    bool put_named_config(const std::string& name, const std::string& content,
        long long& out_updated_ts, std::string& err)
    {
        out_updated_ts = 0;
        err.clear();

        const std::string encoded_name = base64::encode(name);
        const std::string encoded_content = base64::encode(content);
        if (encoded_name.empty() || encoded_content.empty())
        {
            err = "bad_request";
            return false;
        }

        std::string payload;
        if (!cloud_call("cloudcfg_put", encoded_name + "." + encoded_content,
                payload, err))
            return false;

        std::vector<std::string> fields;
        split(payload, ';', fields, 2);
        if (fields.size() != 2 || fields[0] != "ok")
        {
            err = fields.empty() ? "bad_response" : fields[0];
            return false;
        }
        out_updated_ts = to_int(fields[1]);
        if (out_updated_ts <= 0)
        {
            err = "bad_response";
            return false;
        }
        return true;
    }

    bool delete_named_config(const std::string& name, std::string& err)
    {
        err.clear();
        std::string payload;
        if (!cloud_call("cloudcfg_delete", base64::encode(name), payload, err))
            return false;
        if (payload != "ok")
        {
            err = payload.empty() ? "bad_response" : payload;
            return false;
        }
        return true;
    }

    bool rename_named_config(const std::string& old_name, const std::string& new_name,
        long long& out_updated_ts, std::string& err)
    {
        out_updated_ts = 0;
        err.clear();
        const std::string old_b64 = base64::encode(old_name);
        const std::string new_b64 = base64::encode(new_name);
        if (old_b64.empty() || new_b64.empty())
        {
            err = "bad_request";
            return false;
        }

        std::string payload;
        if (!cloud_call("cloudcfg_rename", old_b64 + "." + new_b64, payload, err))
            return false;

        std::vector<std::string> fields;
        split(payload, ';', fields, 2);
        if (fields.size() != 2 || fields[0] != "ok")
        {
            err = fields.empty() ? "bad_response" : fields[0];
            return false;
        }
        out_updated_ts = to_int(fields[1]);
        if (out_updated_ts <= 0)
        {
            err = "bad_response";
            return false;
        }
        return true;
    }

    std::string avatar_path(int uid, long long avatar_date)
    {
        if (uid <= 0 || avatar_date <= 0)
            return {};

        char buf[96];
        sprintf_s(buf, "/forum/data/avatars/l/%d/%d.jpg", uid / 1000, uid);
        return buf;
    }
}
