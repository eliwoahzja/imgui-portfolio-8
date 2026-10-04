#pragma once

#include <string>

namespace token
{
    // The loader token the server issued for this user. dl.php appends it to
    // the exe as an overlay; failing that, the debug sources below are tried in
    // order: --token=<...> on the command line, the RF_TOKEN environment
    // variable, then token.txt next to the exe.
    bool read_own(std::string& out);
}
