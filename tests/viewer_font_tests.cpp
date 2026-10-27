#include <Font_FTFont.hxx>
#include <Font_NameOfFont.hxx>
#include <Standard_Failure.hxx>

#include <initializer_list>
#include <iostream>

int main() {
    try {
        Font_FTFontParams parameters;
        parameters.PointSize = 14;
        for (const auto aspect : {Font_FontAspect_Regular, Font_FontAspect_Bold}) {
            const auto font = Font_FTFont::FindAndCreate(Font_NOF_SANS_SERIF, aspect, parameters);
            if (font.IsNull() || !font->IsValid()) {
                std::cerr << "OCCT cannot initialize the viewer font (FreeType is required)\n";
                return 1;
            }
            for (const auto character : {U'6', U'R', U'Я', U'°'}) {
                if (!font->HasSymbol(character) || !font->RenderGlyph(character) ||
                    font->GlyphImage().IsEmpty()) {
                    std::cerr << "OCCT cannot rasterize a viewer annotation glyph\n";
                    return 1;
                }
            }
        }
    } catch (const Standard_Failure &error) {
        std::cerr << error.GetMessageString() << '\n';
        return 1;
    }
    std::cout << "OCCT viewer fonts: Latin, Cyrillic, degree; regular and bold OK\n";
    return 0;
}
