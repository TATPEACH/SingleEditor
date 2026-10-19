#include "UI/Skins/SkinPainters.h"

namespace singleeditor::skins
{
namespace
{
// D  Layered Tiles: neutral canvas, white tiles, teal signal, coral for action.
constexpr ModernGlassTokens::Palette lightPalette {
    0xffe8ebee, 0xffffffff, 0xfff1f3f6, 0xff15181d, 0xff5d6675, 0xffdfe3e8,
    0xfff25355, 0xfffde4e4, 0xff15181d,
    { 0xff0c7882, 0xff13848e, 0xff0a6b74, 0xff1a8f99, 0xff075f68, 0xff23969f }
};
constexpr ModernGlassTokens::Palette nightPalette {
    0xff0a0c0f, 0xff14171b, 0xff1c2026, 0xffeceef1, 0xff9aa3b0, 0xff262b32,
    0xffff5e61, 0xff3a1d21, 0xffeceef1,
    { 0xff4bbfc7, 0xff5cc8cf, 0xff3fb2ba, 0xff6bd0d6, 0xff36a6ae, 0xff7ad6db }
};
constexpr SkinExtras lightExtras {
    0xfff3f5f7, 0x16151820, 0xff0c7882, 0xffd98a1e, 0xffd92d20, 0x00000000, 0xffffffff, 0x00000000, 0x14101820
};
constexpr SkinExtras nightExtras {
    0xff0f1215, 0x14ffffff, 0xff4bbfc7, 0xfff0ae4a, 0xffff4d3d, 0x00000000, 0x12ffffff, 0x00000000, 0x80000000
};

// The flat base already is the Layered Tiles look: tone-separated tiles, one
// hairline in Light, sunken wells, a transport capsule and coral for action.
struct TilesPainter final : Painter
{
};
} // namespace

const Definition& tilesSkin()
{
    static const TilesPainter painter;
    static const Definition skin { lightPalette, &nightPalette, lightExtras, &nightExtras, { 12.0f, 8.0f, 8, 8 }, painter };
    return skin;
}
} // namespace singleeditor::skins
