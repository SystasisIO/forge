# Runtime integration: ограниченная квалификация pending-кандидата

Дата: 2026-10-26. Статус: **PARTIAL_QUALIFICATION / NOT_FULL_PASS**.
Платформа текущих интеграционных результатов — macOS arm64.
**Точная ревизия — коммит, содержащий этот отчёт.** Это не объявление выпуска
или полной runtime-приёмки.
[Безопасные статусы и счётчики](safe-validation.json) отделены от private evidence.

## Точная граница поставки

База — `1e396b54dc8b81b3eb23e6e2b386482e29c497c8`. Наблюдавшийся
`origin/dev` `97ffc1bbbe7e43ca47fdba0a471e5f081e654230` уже является
её предком. Runtime-кандидат содержит 72 pending paths — пути изменений до
commit: 50 отслеживаемых и 22 необходимых новых файла у 12 существующих
владельцев. Hash manifest совпал с каждым из 72 текущих файлов. SHA-256 исходной описи:
`a452b444c2db9b18b278e21b5b14cdb025ca62e0389c554ebaaa1bd0338d661b`.
Base commit и эта опись определяют проверенный source cut — срез исходников;
коммит, содержащий отчёт, фиксирует поставленные исходники и сам отчёт.
Первоначальные
18 prerequisite paths — обязательных предварительных изменений — не описывают
весь текущий patch; дополнительные изменения имеют собственных владельцев.
Новая документация квалификации учитывается отдельно от этих 72 путей.

| Владелец | Pending paths |
| --- | ---: |
| Инженерные правила | 2 |
| Композиция пакетов и проверок | 4 |
| Пассивные TLSNotary values и отражение полей | 6 |
| Void-результаты API Core | 6 |
| Порядок API Stream | 5 |
| Допуск в lifecycle — жизненный цикл | 4 |
| Завершение и отмена задач Asio | 12 |
| Регистрация Object Store и snapshot read — чтение снимка | 18 |
| HTTP wire serializer — сетевой сериализатор | 5 |
| Кодеки идентичности P2P — одноранговой сети | 5 |
| Типизированная диагностика TCP/TLSNotary | 3 |
| Реестр Crypto Exchange leaf — отдельного компонента | 2 |

Это учёт владельцев точного кандидата, не одобрение полного runtime по числу
файлов. Source review — проверка исходников — и фактическое исполнение проверок
имеют отдельные результаты.

## VM и миграция потребителя

WAMR — WebAssembly Micro Runtime — уже входит в базовый commit. Эти 72 пути
не меняют VM, WAMR или гостевой API. Текущие владельцы —
`forge.vm.wasm.execution` и `forge.api.wasm`; дополнительной VM нет.
Возврат на наблюдавшийся dev не является обновлением. Данный кандидат не
объявляет новую WAMR-миграцию и не закрывает отдельно ведущуюся VM-квалификацию.

Потребитель фиксирует SHA коммита, содержащего этот отчёт, пересобирает нужные
leaf targets — отдельные библиотеки — и проверяет их через установленный package.
Такое закрепление не закрывает отдельно перечисленные открытые проверки. Состав текущих изменений не разрешает добавлять aliases
или второй runtime. Поведение конкретного API описано у его владельца:
[Asio](../../../libraries/asio/README.md),
[App](../../../libraries/app/README.md),
[Object Store](../../../libraries/db/object/README.md),
[API Core](../../../libraries/api/core/README.md),
[API Stream](../../../libraries/api/stream/README.md),
[HTTP](../../../libraries/net/http/README.md),
[P2P](../../../libraries/net/p2p/README.md),
[TLSNotary](../../../libraries/net/tlsnotary/README.md).

## Подтверждённая интеграционная проверка

- Сборка композиции потребителя: 25 targets PASS.
- Выбранный набор CTest потребителя: 77/77 PASS; это не полный Forge CTest.
- Отдельная регрессия существующего native/WASM/CEL testhost: PASS, 89.79 s.

Эти результаты привязаны к pending source manifests и замороженным бинарникам
R3. Точная ревизия поставки определяется содержащим отчёт коммитом;
интеграционные результаты не заменяют fresh installed consumer — нового
потребителя установленного пакета.

Финальная проверка применимости предыдущих source reviews и дельты R3/legacy
R2 — **CLEAN_SCOPED_SOURCE_WITH_RUNTIME_LIMITS**, новых подтверждённых замечаний
в этой области нет. Проверены 23/23 production R3, 6/6 test-only и 72/72 Forge
hashes; прежнее исследование VM — 19/19, pending diff VM/WAMR/API пуст.
Это проверка применимости scoped reviews, не повторный семантический анализ
всех pending files и не новый runtime-прогон. Хеши исходных заключений указаны
в безопасной сводке.

## Открытые проверки

OFF producer — сборка поставщика с выключенными необязательными компонентами —
прошёл configure и build. Первая install вернула exit 0, но не создала prefix и
ForgeConfig: это **NOT_INSTALLED**. Установка родительской сборки не обошла
исключённый vendor-каталог. Исправлена только внешняя команда: она устанавливает
тот же configured Forge sub-build, без изменения исходников. Исправленная
install — **PASS**: 862 файла, ForgeConfig SHA-256
`3e3df03a8b7f67f1129b63afa60fc2fd399ae1f4bd69bb1049de5e7e102abbcd`.
Исходный пустой manifest no-op установки сохранён. Фактическая установка
подтверждена независимой описью файлов; ожидание общего generated manifest
в архивных assertions не являлось дефектом продукта.

Исходная последовательность 39 fresh installed consumer шагов остановилась
на шаге 26: первые восемь потребителей, 24 шага, **PASS**; runtime configure
**PASS**, runtime build **FAIL**. Оригинальный `libp2p_identity_fixture.cpp`
импортирует `forge.crypto.asymmetric`; самостоятельный artifact-only CMake
потребителя не объявлял эту прямую публичную зависимость. Producer и исходные
тесты не менялись. Первоначальный FAIL сохранён. Минимальное исправление
artifact-only композиции принято после review и применено без изменения
producer, исходных тестов, registrations, filters или timeouts. Исправленные
runtime configure/build PASS; оригинальные CTest **7/7 PASS** за 2.24 s.
В совокупности **12 обычных fresh installed consumers PASS**.

Отдельные оставшиеся 12 шагов завершены: HTTP/API WASM/VM WASM
configure/build/run **PASS**, lifecycle configure/build **PASS**. Lifecycle run
содержит два PASS — Asio 2.92 s и App 1.03 s — и один **FAIL** оригинального
`test_forge_asio_task_admission`: известный отказ Boost 1.90 воспроизведён в
fresh installed consumer, checkpoint `tests/asio/task_admission_tests.cpp:490`.
Итого этой отдельной последовательности: 11 шагов PASS, один шаг FAIL.

Исправленная runtime-композиция принята в своей области; известный lifecycle
allocation-failure остаётся FAILED. Полная package acceptance не принята.
Таймеры и guards не ослаблены; исходный FAIL и новое воспроизведение сохранены,
без патча Boost и исключения из требований.

Оригинальный изолированный Asio task_admission test с искусственным отказом
выделения памяти на Boost 1.90 остаётся **FAILED**. Boost не изменён, проверка
не пропущена и исключение из требований не принято.

Внешний static donor registry gate остаётся открыт с 58 диагностическими
сообщениями. Это проверка соответствия реестров и ссылок, а не 58 runtime
сбоев. Владелец — `Forge:jit_profile`; исторический
[baseline](../../iterations/p2p-donor-matrix-baseline-2026-10-25.md)
сохраняется. Promotion — включение в dev — **BLOCKED** этим внешним gate и
известным allocation-failure. Этот отчёт не отменяет данные gates.

Продуктовая стоимость, денежные расчёты и выбор коммерческого профиля принадлежат
потребителю. Они не являются обязательными проверками нейтрального Forge runtime.
Live chain, upstream service, Linux и JIT/AOT acceptance этим отчётом не заявлены.

[Unreleased summary](../../releases/unreleased-runtime-integration.md) описывает
тот же ограниченный срез; полная приёмка остаётся открытой.
