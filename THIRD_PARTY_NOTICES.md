# Third-party components

The MIT license in LICENSE applies to original project code. Qt, OCCT and their dependencies
retain their own licenses.

## Linux packages

The Ubuntu 24.04 build uses Qt 6.4.2 and OCCT 7.6.3 supplied by Ubuntu.
The native viewer needs Qt and OCCT visualization/modeling libraries at runtime.
The CAD module also uses OCCT Data Exchange, meshing, primitives and Boolean operations for single-solid STEP import, display preparation and flat-part thickness analysis.
The manual update adapter also uses system Qt Network/TLS; no TLS library is copied into Linux packages.
The project-file adapter uses the existing Qt Core JSON, SHA-256 and atomic-file APIs; it adds no new third-party library.
Its SDK targets are loaded alongside OCCT Application Framework; no new third-party SDK is added.
The `.deb` records the actual runtime dependencies with dpkg-shlibdeps, not a hand-written list.
System libraries are not copied into it. Their license notices, source packages
and modifications are distributed through Ubuntu (`apt source qt6-base`, `apt source opencascade`).
Installed copyright notices are under `/usr/share/doc/` in the respective distribution packages.

## Windows packages

- Qt base 6.8.3, Widgets/Core/Gui/Network: LGPL-3.0 / GPL and applicable Qt exceptions.
  Exact sources: https://download.qt.io/archive/qt/6.8/6.8.3/submodules/qtbase-everywhere-src-6.8.3.tar.xz
- Open CASCADE Technology 7.9.3, vcpkg port revision 1: LGPL-2.1 with Open CASCADE Exception 1.0.
  Sources: https://github.com/Open-Cascade-SAS/OCCT/tree/V7_9_3
  vcpkg build and patches: https://github.com/microsoft/vcpkg/tree/256310531c1e57ee83f8ac01f7bea7c1e56630f5/ports/opencascade
- FreeType 2.14.3, enabled for OCCT viewer dimensions and navigation labels: FreeType License (FTL).
  This software is based in part on the work of the FreeType Team.
  Portions of this software are copyright © 1996–2026 The FreeType Project
  (https://freetype.org). All rights reserved.
  Sources: https://gitlab.freedesktop.org/freetype/freetype/-/tree/VER-2-14-3
  vcpkg build and patches: https://github.com/microsoft/vcpkg/tree/256310531c1e57ee83f8ac01f7bea7c1e56630f5/ports/freetype
  The FTL option is selected; bundled MIT/X11/zlib notices are also retained.

Windows bundles use shared libraries and keep their DLLs replaceable. No third-party source was
copied into original project code. OCCT optional TBB, VTK and FreeImage features are off
for the foundation build. Enabling them requires updating this inventory.
FreeType uses no optional Brotli/bzip2/PNG/external-zlib/HarfBuzz dependencies.
OCCT's embedded DejaVu Sans Latin fallback retains its Bitstream Vera font notice in
`share/licenses/OCCT-DejaVu-copyright.txt` (DejaVu changes are public domain).
The foundation bundle omits unused SVG/image and generic touch plugins, Mesa software OpenGL,
and the system D3D/DXC compilers. Packaging rejects additional Qt modules until their
source/license inventory is updated.

The native viewer uses the operating system's OpenGL driver. Windows packaging includes the
OCCT `resources/Shaders`, `XSMessage`, `SHMessage`, `XSTEPResource` and `UnitsAPI` directories next to the runtime bundle; the corresponding source archive
and OCCT copyright notices cover these resources. No sample implementation was copied into
the original viewer code.

The matching `dependency-sources` companion contains Qt base sources including its bundled
third-party license texts, OCCT and FreeType sources as built, and their vcpkg ports/patches. Windows packaging
also copies license notices into `share/licenses`. The Microsoft Visual C++ Runtime is a separate
prerequisite and is not redistributed in this ZIP.

This inventory describes the initial build. Changing dependencies, build options, fonts, assets,
or packaging requires checking and updating the notices and corresponding sources.

The three primitive-generated STEP examples and `tools/GenerateExamples.cpp` use the project's
MIT license. `wall-panel-1000mm.step` is derived from a user-supplied CAD model and included at
the user's explicit request; the software MIT license does not relicense that source model.
Its provenance and conversion details are retained in `tests/data/step/README.md`. The converter
and the original SLDPRT are not included in the application.

## Embedded drawing font

GOST 2.304-81 Type A Regular, unmodified v0.7.6 font by Sergey S. Betke (2015).
Source: https://github.com/Metrolog/Font.GOST2.304-81/releases/tag/v0.7.6
The font's embedded name-table license is SIL Open Font License 1.1, with no Reserved Font Name.
The exact notice and license are retained in `src/ui/data/fonts/OFL.txt` and installed in
`share/doc/BendingSimulator/licenses/fonts/OFL.txt` in packaged builds.
The repository's Apache license describes its tooling; this embedded font retains its own OFL.
No glyph outlines were changed. Font embedding does not change the license of generated PDF documents.

## Reference bend-allowance data — D056

The theoretical air-bending presets use numerical K-factor recommendations published in
[Wiz Parts, Sheet Metal Fabrication Design Guide v1.0, September 2024, printed page 4](https://www.wiz-parts.co.uk/wp-content/uploads/2024/08/Wiz-Parts-Sheet-Metal-Fabrication-Design-Guide-v1.0.pdf).
Only numerical reference facts and attribution are included; the PDF, illustrations and third-party source code are not distributed.
The application's interval-boundary policy and limitations are documented in docs/flat-pattern.md.
These reference presets do not constitute calibration or endorsement for the user's press brake.


## Manual updates — D065

Windows Qt Network uses the Qt Schannel TLS plugin (`tls/qschannelbackend.dll`) and the
operating system TLS service. No Qt OpenSSL backend or OpenSSL DLL is distributed.
Qt Network/Schannel sources and notices are part of the matching qtbase companion above.
The native bootstrap and extraction script are original MIT project code. Windows PowerShell
and .NET are operating system prerequisites, not bundled dependencies. The bootstrap uses
the MSVC toolchain static CRT; the GUI still has the existing separate Visual C++ runtime
prerequisite. Local Postman/PowerShell 7 development tools are not shipped with the app.


## Стартовые сечения гибочного инструмента — D128

В runtime включён фиксированный набор технических DXF-снимков, предоставленный владельцем
проекта для стартового комплекта: ST3408.dxf, ST3412.dxf, ST1405.dxf, ST1701.dxf,
2067-1.dxf, TOP.205.85.R08.dxf и 1063.dxf. Сохраняются исходные имена/SHA и захваченные
линии/дуги; исходные заводские DXF/PDF/DWG, изображения, личные пути и складские остатки
не включены. Состав/происхождение перечислены в `src/project/data/README.md` исходного дерева.
MIT приложения относится к собственному программному коду и не подменяет условия
исходных материалов изготовителей. Эти снимки не являются новыми заводскими чертежами
и не подтверждают нагрузку, наличие инструмента или пригодность конкретной установки.
