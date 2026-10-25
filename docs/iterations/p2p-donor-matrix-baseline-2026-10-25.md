# P2P donor matrix: открытая ошибка baseline на 2026-10-25

Статус: **OPEN / static test-registry failure**. Проверка связности матрицы
донорских случаев с реестром тестов завершается с кодом `1` и выдаёт
58 диагностик. Это **не 58 сбоев P2P runtime**: сетевые сценарии этим
валидатором не исполняются. P2P — взаимодействие равноправных сетевых узлов.

Maintainer назначил исправление сетевой команде FORGE. Этот отчёт не означает,
что команда уже приняла задачу, внесла исправление или подтвердила совместимость.
Здесь документируется baseline; checker, матрица, селекторы и runtime не меняются.

## Проверенные исходные состояния

| Состояние | Commit / tree |
| --- | --- |
| Общая база feature и текущего dev | `ace821ee46eb1ae410473415a8f26dd76aa61b8d` |
| Feature с VM-исправлениями | `4f9b571bc8c9d738c04ff7e52002ac2acca25e18` / `eec42e596ed0dcc47674d8d4ca8ab448ef47d3f2` |
| Проверенный dev baseline | `07267d4d13f56167ebba1482009978f0f649be9d` / `490c9f7c44b1b8b8bb2e9c4012fe66913897ec4e` |
| Полный merge-кандидат: dev + feature | `fddf44882b685e7ed562db3e741e38c62a23bad8` / `eaaed92d17d8307ac7709fd22a96ae8163e7db7e` |

Полные диагностические выводы feature и merge-кандидата побайтово совпадают.
Один дополнительный статический запуск на указанном dev baseline дал тот же
вывод: 58 строк `ERROR`, exit `1`, SHA256
`16eead57148548dd0a0555ca87cef3d66ac082649c026d8aeb61adac3028149f`.
Ошибка воспроизводится до объединения VM feature с dev; эти данные не
устанавливают commit первого появления и не доказывают новую регрессию VM
или TLSNotary. Независимая source-классификация имеет SHA256
`beaa3d21b719cda2152d06acfcbe742d7586c823d89402ba6f49b92152c01b9b`.

## Воспроизведение и граница проверки

Из корня репозитория на указанном исходном состоянии:

```sh
python3 -B tests/libp2p_interop/check_donor_matrix.py . tests/libp2p_interop/donor_cases.json
```

[Валидатор](../../tests/libp2p_interop/check_donor_matrix.py) читает
[donor_cases.json](../../tests/libp2p_interop/donor_cases.json), декларации
целей/тестов в [tests/CMakeLists.txt](../../tests/CMakeLists.txt), исходные test-id
и литеральную карту `LIVE_SCENARIO_PROFILES` в
[runner.py](../../tests/libp2p_interop/runner.py). Он сверяет точные пары
profile/scenario, ссылки `target test-id` и владельца test-id. Ни runner,
ни сетевые узлы, ни нативные тестовые бинарники не запускаются. Команда выше
не передаёт optional `DONORS_ROOT` и не подтверждает checkout внешних доноров.

`test_forge_libp2p_donor_matrix` зарегистрирован обычным `add_test` при
включённом корневом `BUILD_TESTING` вне contract-SDK-host профиля. Отдельный
opt-in live interop не отключает эту статическую проверку. Согласно
[AGENTS.md](../../AGENTS.md), failing compatibility tests являются blockers:
скрытая фильтрация, ослабление checker или тихий waiver не являются закрытием.

## Все 58 диагностик: группы и примеры

| Donor case | Диагностик |
| --- | ---: |
| `interop.live_native_mdns` | 4 |
| `autorelay.discovery_reservation_lifecycle` | 4 |
| `dcutr.connect_sync_attempt` | 3 |
| `connections.coordinated_dial_port_reuse` | 1 |
| `interop.live_ping_identify_relay` | 4 |
| `interop.live_dcutr_relay_topology` | 3 |
| `gossipsub.native_score_repair` | 17 |
| `gossipsub.native_extensions` | 22 |
| Всего | 58 |

По форме ошибок: 19 неизвестных live-пар, 6 неизвестных interop-сценариев,
21 отсутствующая точная interop-ссылка, 10 неточных ссылок `target test-id`
и 2 отсутствующие ссылки на зарегистрированный interop target. Один donor
case может давать несколько диагностик; количество не равно числу runtime-тестов.

Примеры из неизменённого вывода:

```text
ERROR: interop.live_native_mdns: unknown live selector 'tcp_stage6'/'mdns_public'
ERROR: autorelay.discovery_reservation_lifecycle: live selector 'autorelay_lifecycle' lacks exact interop test reference
ERROR: dcutr.connect_sync_attempt: evidence requires a target and one exact test/scenario id
ERROR: gossipsub.native_score_repair: live scenarios require the registered interop target
ERROR: gossipsub.native_extensions: live selector 'partial_messages' lacks exact interop test reference
```

## Ожидаемое закрытие сетевым владельцем

1. Согласовать заявленные donor mappings с реальными зарегистрированными
   target/test-id и точными исполняемыми profile/scenario; не подменять
   подтверждение поведения формальной регистрацией имени.
2. Добавить регрессии для установленных причин расхождения, сохранив строгую
   проверку неизвестных/неточных ссылок и прежние assertions/селекторы.
3. На зафиксированном исправленном commit получить PASS той же команды и
   зарегистрированного `test_forge_libp2p_donor_matrix`; сохранить raw и SHA.
4. Отдельно сообщить результаты требуемого live interop. Статический PASS
   сам по себе не доказывает сетевую совместимость и не заменяет runtime gates.

В рамках этого отчёта native/P2P runtime, Docker и CI не запускались;
общий FORGE PASS и разрешение на dev/master promotion не заявляются.
