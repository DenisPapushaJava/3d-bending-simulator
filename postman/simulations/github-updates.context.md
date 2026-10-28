# Локальная проверка GitHub update-клиента

2026-10-28. Upstream — публичный GitHub Releases REST API, клиент — `UpdateClient`, endpoint фиксирован в production. Контракт `postman/github-releases.openapi.yaml` описывает используемые поля/GET списка и assets, не весь GitHub API. Основания: [список релизов](https://docs.github.com/en/rest/releases/releases), [assets](https://docs.github.com/en/rest/releases/assets). Orbit discovery дал individual release/asset endpoints, недостающий list endpoint сверен по официальной документации. Поиск опубликованного mock владельца не удался: Postman CLI не авторизован. Вместо входа/публикации используется собственный локальный fixture.

Postman CLI 1.75.0, запуск из корня репозитория, без workspace bind/облачной записи:

```bash
postman simulation run postman/simulations/success.sim.yaml --port auto --output ndjson --no-history
# В другом терминале: адрес /github взять из события listening:
build/dev/bin/update_network_tests --probe-url http://127.0.0.1:PORT/github downloaded
```

Для `rate-limit.sim.yaml` / `timeout.sim.yaml` та же команда probe с последним аргументом `error`. Режим origin внедряется только в тестовом executable; production CLI не принимает адрес обновления. Остановить сервер Ctrl+C.

Фактически проверено: success — три HTTP 200 (list/checksums/package), состояние Downloaded и совпавший SHA-256; rate — управляемый 429 и понятная ошибка; latency 2000 ms — таймаут клиента 500 ms, без утверждения актуальности. NDJSON/error/probe logs сохранены локально в игнорируемом `out/manual-updates/`. Сценарии с внедрённой ошибкой/задержкой CLI могут не выводить обычное request-событие: вывод C++ probe проверяет конечный результат.

Mock предлагает alpha.10 после alpha.1; пакет — строка `portable test archive`, **не настоящий ZIP/EXE**. Это проверка сети/выбора/потоковой записи и SHA, не установки Windows. Упаковщик, System.IO.Compression и launcher проверяются отдельными тестами. Больше ответов/пагинация/неверные хеши/отмена/URL покрываются loopback QTcpServer в `update.network`; Postman не добавляется обязательной зависимостью CI. Нет соединения с живым GitHub, передачи CAD, ключей или настройки фонового опроса.
