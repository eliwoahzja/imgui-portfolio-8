#include "embedded_fonts.h"

// The generated dumps use the Arduino-flavoured `byte` / `PROGMEM` spelling,
// so give those two names a meaning before including them. They are plain
// `static const` arrays, which is why they are compiled here instead of being
// added to the project as separate translation units.
typedef unsigned char byte;
#define PROGMEM

#include "museosansex.cpp"
#include "zendots.cpp"
#include "lucide.cpp"

namespace embedded_fonts
{
    const unsigned char* museo      = MuseoSansEx_500;
    const unsigned int   museo_size = (unsigned int)sizeof(MuseoSansEx_500);

    const unsigned char* zendots      = ZenDots_Regular;
    const unsigned int   zendots_size = (unsigned int)sizeof(ZenDots_Regular);

    const unsigned char* lucide      = ::lucide;
    const unsigned int   lucide_size = (unsigned int)sizeof(::lucide);
}
