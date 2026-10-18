# Исследовательская гибка с отверстием

Собственный MIT-прототип внешнего FEM-расчёта. Генерирует плоскую пластину и инструмент, запускает CalculiX, читает результаты, создаёт научный график и самостоятельные HTML-анимации без сетевых библиотек. Производственные STEP и интерфейс приложения не используются. [Постановка, допущения и результаты](../../docs/fem-bend-hole.md).

Из корня репозитория на Linux с Docker:

```bash
docker build -t bending-fem:ubuntu24.04 experiments/bend-hole
docker run --rm --init --user "$(id -u):$(id -g)" -v "$PWD:/workspace" \
  bending-fem:ubuntu24.04 python3 experiments/bend-hole/study.py out/fem-study
docker run --rm --user "$(id -u):$(id -g)" -v "$PWD:/workspace" \
  bending-fem:ubuntu24.04 python3 experiments/bend-hole/report.py out/fem-study
```

`out/fem-study/report/index.html` — оглавление отчёта; `comparison.png` — сравнение формы, силы и сеток; `center-medium.html`, `center-fine.html`, `offset.html` — готовые анимации, открываемые обычным браузером без сервера; `summary.json` — числа и происхождение. Каталог вывода для повторного опыта должен быть новым. Исходные `.inp`, `.msh`, `.frd`, `.dat`, `.sta` и журналы остаются в каждом каталоге случая.

Дополнительные отдельные пробы в существующем наборе (имена ещё не должны существовать):

```bash
docker run --rm --init --user "$(id -u):$(id -g)" -v "$PWD:/workspace" \
  bending-fem:ubuntu24.04 python3 experiments/bend-hole/study.py out/fem-study \
  --case small-step --case stiff-contact --case unloading
```

Завершение решателя не подтверждает точность изготовления. Материал условный, базовый контакт без трения, модель без повреждения/разрыва. Сходимость или отказ конкретного расчёта не являются общим разрешением или запретом гибки. До применения к производственной детали нужны её заготовка, материал, условия процесса и сравнение с измерениями. Разгрузка исследуется отдельно от основного набора.
