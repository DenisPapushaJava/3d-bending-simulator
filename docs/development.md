# Разработка и проверки

Дата: 2026-10-17. Создан технический каркас, а не работающий симулятор гибки.

## Начальный набор

- C++20, CMake 3.24+, Ninja.
- Qt Widgets 6.4+; в Ubuntu 24.04 используется системная 6.4.2, в Windows CI — 6.8.3.
- OCCT 7.6–7.9: Ubuntu 24.04 — 7.6.3, Windows — 7.9.3#1 из зафиксированного vcpkg.
- Windows x64 / MSVC 2022; Linux x64 / GCC 13, базовый дистрибутив Ubuntu 24.04, также Mint 22.x.
- Python 3.10+ нужен только инструментам проверки версии и упаковки; в приложение не входит.

Здесь указан набор стартового каркаса. Разные версии зависимостей на двух ОС проверяют совместимость API; точное воспроизведение системных Ubuntu-пакетов зависит от обновлений репозитория. OCCT 8 намеренно не принимается до отдельной проверки миграции.

## Linux

На Ubuntu 24.04 / Mint 22.x:

```bash
sudo bash scripts/install-linux-deps.sh
cmake --preset dev
cmake --build --preset dev --parallel 2
ctest --preset dev
./build/dev/bin/3d-bending-simulator
```

Без изменения системных пакетов можно использовать Docker:

```bash
docker build -f tools/Dockerfile -t 3d-bending-dev:ubuntu24.04 .
docker run --rm --user "$(id -u):$(id -g)" -v "$PWD:/workspace" \
  3d-bending-dev:ubuntu24.04 bash -c \
  'cmake --preset release && cmake --build --preset release --parallel 2 && ctest --preset release'
```

Контейнер проверяет сборку и создание окна с Qt offscreen. Он не подключает дисплей или видеокарту и не подтверждает работу будущего 3D-просмотрщика на реальном GPU.

## Windows

Установить Visual Studio 2022 с Desktop development with C++, CMake/Ninja, Python и Qt 6.8.3 MSVC 2022 x64. Использовать **Developer PowerShell for VS 2022**, чтобы `cl` был доступен. `Qt6_DIR` или `CMAKE_PREFIX_PATH` должен указывать на выбранную Qt.

```powershell
git clone https://github.com/microsoft/vcpkg .deps/vcpkg
git -C .deps/vcpkg checkout 256310531c1e57ee83f8ac01f7bea7c1e56630f5
./.deps/vcpkg/bootstrap-vcpkg.bat -disableMetrics
cmake --preset release "-DCMAKE_TOOLCHAIN_FILE=$pwd/.deps/vcpkg/scripts/buildsystems/vcpkg.cmake" -DVCPKG_TARGET_TRIPLET=x64-windows-release "-DVCPKG_OVERLAY_TRIPLETS=$pwd/cmake/triplets" "-DVCPKG_INSTALLED_DIR=$pwd/vcpkg_installed"
cmake --build --preset release --parallel 2
ctest --preset release
./build/release/bin/3d-bending-simulator.exe
```

Triplet собирает общие DLL и только Release для зависимостей. Он не требует компилировать Qt из исходников: Qt поставляется отдельно официальной сборкой. Для локальной Debug-конфигурации OCCT нужно отдельно согласовать triplet/CRT, не использовать Release-only triplet как обещание поддержки всех Debug-комбинаций.

## Структура

| Каталог / цель | Ответственность |
| --- | --- |
| `src/core`, `bending_core` | Независимый C++: стабильные ID гибов и перестановка с определением первого затронутого шага |
| `src/cad`, `bending_cad` | Граница OCCT, пока только сведения о версии; отдельный тест создаёт и измеряет реальный CAD-примитив |
| `src/viewer`, `bending_viewer` | Изолированная область просмотра; сейчас QWidget-заглушка, без OpenGL/OCCT-рендеринга |
| `src/ui`, `src/app` | Окно QMainWindow, три dock-панели, меню, запуск, диагностический `--smoke-test` |
| `tests` | Перестановка/ошибки без изменения данных, связь OCCT, создание окна, правила версий |
| `scripts`, `cmake` | Сборка, упаковка и согласованность версий |

Пустые модули planning/simulation/project и плагины заранее не создаются. Они появятся вместе с предметными задачами, сохраняя границы из архитектуры. UI не показывает искусственных успешных проверок и не содержит демонстрационных данных Figma как результатов расчёта.

## Проверки перед коммитом

```bash
python3 scripts/version.py
python3 -m unittest discover -s tests -p 'test_*.py' -v
find src tests -type f \( -name '*.h' -o -name '*.cpp' \) -print0 | xargs -0 clang-format-18 --dry-run --Werror
cmake --preset release
cmake --build --preset release --parallel 2
ctest --preset release
```

Для форматирования использовать clang-format 18: файл `.clang-format` общий с CI. Строгие предупреждения относятся к нашему коду; заголовки OCCT подключены как SYSTEM. Новые расчётные правила проверять на предметных допустимых/недопустимых случаях.

## CI

Один workflow `.github/workflows/ci.yml`:

1. Проверка VERSION, совпадения с vcpkg.json и тегом, тесты правил версии и clang-format.
2. Две сборки: Ubuntu 24.04 и Windows 2022. Сборка Release, CTest, проверка старта окна.
3. Linux .deb с автоматически определёнными зависимостями; Windows ZIP с DLL, Qt-плагинами, лицензиями и исходниками зависимостей. Проверяется запуск из собранного комплекта.
4. Артефакты для main и тегов хранятся 7 дней; по тегу создаётся черновик релиза.

Push проверяется только в main, остальные ветки — через PR, чтобы не дублировать один и тот же запуск. Правки только Markdown/docs/design не запускают тяжёлые сборки. Старые запуски веток отменяются; запуски тегов не отменяются. Разрешение записи есть только у задачи черновика релиза. Actions закреплены SHA, их обновления группирует Dependabot раз в месяц. OCCT имеет отдельный двоичный кэш; первый Windows-запуск будет заметно дольше следующих.

Нет develop-ветки, обязательного числа ревью, автоматического изменения версии или сложного release-бота. Изменения удобно вести короткими ветками и PR в main. Графические проверки, установщик, подпись Windows и универсальный AppImage — последующие задачи.

## Первоисточники

[Qt deployment](https://doc.qt.io/qt-6.8/qt-generate-deploy-app-script.html), [CPack DEB](https://cmake.org/cmake/help/latest/cpack_gen/deb.html), [vcpkg port](https://github.com/microsoft/vcpkg/tree/256310531c1e57ee83f8ac01f7bea7c1e56630f5/ports/opencascade), [install-qt-action](https://github.com/jurplel/install-qt-action), [GitHub token permissions](https://docs.github.com/en/actions/tutorials/authenticate-with-github_token).
