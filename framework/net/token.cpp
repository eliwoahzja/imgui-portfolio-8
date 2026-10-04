#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>

#include <cstdio>
#include <cstring>
#include <vector>

#include "token.h"

#pragma comment(lib, "shell32.lib")

namespace
{
    // Must match dl.php: TOKEN_XOR_KEY, OVERLAY_MAGIC_START, OVERLAY_MAGIC_END.
    const unsigned char xor_key[16] = {
        0x6B, 0x39, 0xA2, 0x7F, 0x14, 0xD8, 0x53, 0xE1,
        0xBC, 0x47, 0x90, 0x2E, 0xF5, 0x68, 0x1A, 0xC3
    };
    const unsigned char magic_start[4] = { 0x47, 0x53, 0x4C, 0x44 };  // GSLD
    const unsigned char magic_end[4]   = { 0x44, 0x4C, 0x53, 0x47 };  // DLSG

    bool plausible(const std::string& t)
    {
        if (t.size() < 10 || t.size() > 128)
            return false;

        for (unsigned char c : t)
        {
            if (c < 0x21 || c > 0x7E)
                return false;
        }
        return true;
    }

    bool from_overlay(const std::string& exe_path, std::string& out)
    {
        HANDLE file = CreateFileA(exe_path.c_str(), GENERIC_READ, FILE_SHARE_READ,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            return false;

        LARGE_INTEGER size;
        if (!GetFileSizeEx(file, &size) || size.QuadPart < 14)
        {
            CloseHandle(file);
            return false;
        }

        // The overlay is at most 4 + 2 + 128 + 4 bytes; 256 covers it with room.
        const LONGLONG tail_size = size.QuadPart < 256 ? size.QuadPart : 256;

        LARGE_INTEGER seek;
        seek.QuadPart = size.QuadPart - tail_size;
        SetFilePointerEx(file, seek, nullptr, FILE_BEGIN);

        std::vector<unsigned char> tail((size_t)tail_size);
        DWORD read = 0;
        const bool read_ok = ReadFile(file, tail.data(), (DWORD)tail_size, &read, nullptr)
            && read == (DWORD)tail_size;
        CloseHandle(file);

        if (!read_ok || tail.size() < 10)
            return false;

        if (memcmp(&tail[tail.size() - 4], magic_end, 4) != 0)
            return false;

        int start = -1;
        for (int i = (int)tail.size() - 10; i >= 0; --i)
        {
            if (memcmp(&tail[i], magic_start, 4) == 0)
            {
                start = i;
                break;
            }
        }
        if (start < 0)
            return false;

        const int len = tail[start + 4] | (tail[start + 5] << 8);
        const int body = start + 6;
        if (len <= 0 || len > 128 || body + len + 4 > (int)tail.size())
            return false;

        out.assign((size_t)len, '\0');
        for (int i = 0; i < len; ++i)
            out[i] = (char)(tail[body + i] ^ xor_key[i % sizeof(xor_key)]);

        return plausible(out);
    }

    bool from_command_line(std::string& out)
    {
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (!argv)
            return false;

        bool found = false;
        for (int i = 1; i < argc && !found; ++i)
        {
            const std::wstring arg = argv[i];
            if (arg.rfind(L"--token=", 0) != 0)
                continue;

            // Tokens are ASCII by construction, so anything outside it means
            // the argument is not one and the narrowing would be silent.
            const std::wstring value = arg.substr(8);

            out.clear();
            out.reserve(value.size());
            for (wchar_t wc : value)
            {
                if (wc > 0x7F)
                {
                    out.clear();
                    break;
                }
                out.push_back((char)wc);
            }

            found = plausible(out);
        }

        LocalFree(argv);
        return found;
    }

    bool from_environment(std::string& out)
    {
        char buf[160];
        const DWORD n = GetEnvironmentVariableA("RF_TOKEN", buf, (DWORD)sizeof(buf));
        if (n == 0 || n >= sizeof(buf))
            return false;

        out.assign(buf, n);
        return plausible(out);
    }

    bool from_file(const std::string& exe_path, std::string& out)
    {
        const size_t slash = exe_path.find_last_of("\\/");
        const std::string dir = slash == std::string::npos
            ? std::string() : exe_path.substr(0, slash + 1);

        FILE* f = nullptr;
        if (fopen_s(&f, (dir + "token.txt").c_str(), "rb") != 0 || !f)
            return false;

        char buf[160] = {};
        const size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);

        out.assign(buf, n);
        while (!out.empty() && (out.back() == '\r' || out.back() == '\n' || out.back() == ' '))
            out.pop_back();

        return plausible(out);
    }
}

namespace token
{
    bool read_own(std::string& out)
    {
        char path[MAX_PATH] = {};
        if (GetModuleFileNameA(nullptr, path, MAX_PATH) == 0)
            return false;

        return from_overlay(path, out)
            || from_command_line(out)
            || from_environment(out)
            || from_file(path, out);
    }
}
