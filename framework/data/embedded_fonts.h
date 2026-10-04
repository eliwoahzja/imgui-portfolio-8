#pragma once

// Byte arrays exported from the .cpp dumps in this folder (museosansex.cpp,
// zendots.cpp, lucide.cpp). Those dumps declare `static const byte ...[]`, so
// they are pulled into embedded_fonts.cpp and re-exported through here.
namespace embedded_fonts
{
    // MuseoSansEx 500 - the UI face.
    extern const unsigned char* museo;
    extern const unsigned int   museo_size;

    // Zen Dots
    extern const unsigned char* zendots;
    extern const unsigned int   zendots_size;

    // Lucide icon font (glyphs live in the private use area).
    extern const unsigned char* lucide;
    extern const unsigned int   lucide_size;
}
