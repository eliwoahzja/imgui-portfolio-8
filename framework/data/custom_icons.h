#pragma once

// ---------------------------------------------------------------------------
// Custom SVG icons.
//
// Drop raw SVG markup in here and give it a name. Anything drawn through
// icons::draw(name, ...) looks in this table first, so a name defined here
// overrides the built-in Lucide icon of the same name.
//
//     { "gear-icon", R"SVG(
//         <svg viewBox="0 0 24 24" fill="none" stroke="currentColor"
//              stroke-width="2" stroke-linecap="round">
//           <circle cx="12" cy="12" r="3"/>
//           <path d="M19.4 15a1.65 1.65 0 0 0 .33 1.82l..."/>
//         </svg>)SVG" },
//
// Supported: <path> (M L H V C S Q T A Z, absolute and relative), <circle>,
// <ellipse>, <rect> (incl. rx/ry), <line>, <polyline>, <polygon>, <g>.
// stroke-width, fill and stroke are read per element and inherited from <svg>.
// fill="none" (the Lucide default) means outline only; any other fill value
// fills the shape. Colours in the markup are ignored - the icon is drawn in
// the colour the caller passes in.
//
// The viewBox is fitted into the target rect, so any canvas size works.
// ---------------------------------------------------------------------------
namespace custom_icons
{
    struct entry
    {
        const char* name;
        const char* svg;
    };

    inline constexpr entry table[] = {
        // Example - delete or replace. Draw it with icons::draw(dl, "example-star", ...)
        { "example-star", R"SVG(
            <svg viewBox="0 0 24 24" fill="none" stroke="currentColor"
                 stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
              <path d="M11.5 2.5 14 8l6 .9-4.3 4.2 1 6-5.2-2.8L6.3 19l1-6L3 8.9 9 8z"/>
            </svg>)SVG" },
    };

    inline constexpr int table_count = (int)(sizeof(table) / sizeof(table[0]));
}
