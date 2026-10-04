#pragma once

#include <cstddef>

namespace config_page
{
    struct state;
}

namespace menu_cloud
{
    bool export_code(const config_page::state& s, char* out, std::size_t out_size,
        char* error, std::size_t error_size);
    bool import_code(const char* code, config_page::state& s, char* error, std::size_t error_size);
}
