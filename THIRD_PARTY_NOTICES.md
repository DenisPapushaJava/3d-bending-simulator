# Third-party components

The MIT license in LICENSE applies to original project code. Qt, OCCT and their dependencies
retain their own licenses.

## Linux packages

The Ubuntu 24.04 build uses Qt 6.4.2 and OCCT 7.6.3 supplied by Ubuntu.
The native viewer needs Qt and OCCT visualization/modeling libraries at runtime.
The CAD module also uses OCCT Data Exchange for single-solid STEP import (M1.2).
Its SDK targets are loaded alongside OCCT Application Framework; no new third-party SDK is added.
The `.deb` records the actual runtime dependencies with dpkg-shlibdeps, not a hand-written list.
System libraries are not copied into it. Their license notices, source packages
and modifications are distributed through Ubuntu (`apt source qt6-base`, `apt source opencascade`).
Installed copyright notices are under `/usr/share/doc/` in the respective distribution packages.

## Windows packages

- Qt base 6.8.3, Widgets/Core/Gui: LGPL-3.0 / GPL and applicable Qt exceptions.
  Exact sources: https://download.qt.io/archive/qt/6.8/6.8.3/submodules/qtbase-everywhere-src-6.8.3.tar.xz
- Open CASCADE Technology 7.9.3, vcpkg port revision 1: LGPL-2.1 with Open CASCADE Exception 1.0.
  Sources: https://github.com/Open-Cascade-SAS/OCCT/tree/V7_9_3
  vcpkg build and patches: https://github.com/microsoft/vcpkg/tree/256310531c1e57ee83f8ac01f7bea7c1e56630f5/ports/opencascade

Windows bundles use shared libraries and keep their DLLs replaceable. No third-party source was
copied into original project code. OCCT optional FreeType, TBB, VTK and FreeImage features are off
for the foundation build. Enabling them requires updating this inventory.
The foundation bundle omits unused SVG/image and generic touch plugins, Mesa software OpenGL,
and the system D3D/DXC compilers. Packaging rejects additional Qt modules until their
source/license inventory is updated.

The native viewer uses the operating system's OpenGL driver. Windows packaging includes the
OCCT `resources/Shaders` directory next to the runtime bundle; the corresponding source archive
and OCCT copyright notices cover these resources. No sample implementation was copied into
the original viewer code.

The matching `dependency-sources` companion contains Qt base sources including its bundled
third-party license texts, the OCCT sources as built, and the vcpkg port/patches. Windows packaging
also copies license notices into `share/licenses`. The Microsoft Visual C++ Runtime is a separate
prerequisite and is not redistributed in this ZIP.

This inventory describes the initial build. Changing dependencies, build options, fonts, assets,
or packaging requires checking and updating the notices and corresponding sources.
