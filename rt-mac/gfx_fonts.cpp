#include "gfx_fonts.h"

namespace rt_mac::gfx {

namespace {

using GFXglyph = Glyph;
using GFXfont = Font;

#define PROGMEM
#include "lgfx/Fonts/GFXFF/FreeSans12pt7b.h"
#include "lgfx/Fonts/GFXFF/FreeSans18pt7b.h"
#include "lgfx/Fonts/GFXFF/FreeSans24pt7b.h"
#include "lgfx/Fonts/GFXFF/FreeSans9pt7b.h"
#include "lgfx/Fonts/GFXFF/FreeSansBold12pt7b.h"
#include "lgfx/Fonts/GFXFF/FreeSansBold18pt7b.h"
#include "lgfx/Fonts/GFXFF/FreeSansBold24pt7b.h"
#include "lgfx/Fonts/GFXFF/FreeSansBold9pt7b.h"
#undef PROGMEM

}  // namespace

const Face kFaces[4] = {
    {&FreeSans9pt7b, &FreeSansBold9pt7b, 18},
    {&FreeSans12pt7b, &FreeSansBold12pt7b, 24},
    {&FreeSans18pt7b, &FreeSansBold18pt7b, 36},
    {&FreeSans24pt7b, &FreeSansBold24pt7b, 48},
};

}  // namespace rt_mac::gfx
