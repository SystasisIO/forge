# Unreleased: bounded runtime integration candidate

Статус на 2026-10-26: **PARTIAL_QUALIFICATION / NOT_FULL_PASS**.
Точная ревизия — коммит, содержащий этот отчёт.
База `1e396b54dc8b81b3eb23e6e2b386482e29c497c8` и SHA-256 source manifest
`a452b444c2db9b18b278e21b5b14cdb025ca62e0389c554ebaaa1bd0338d661b`
идентифицируют проверенный срез исходников.

Срез объединяет 72 пути у 12 существующих владельцев: API Core/Stream,
Asio/App lifecycle, Object Store, HTTP wire, P2P identity codecs, пассивные
TLSNotary values, типизированную диагностику, Crypto Exchange registry и
композицию пакетов/тестов. Это расширение существующих границ. Подробные
контракты, зависимости и примеры остаются в README владельцев; отчёт
[квалификации](../validation/runtime-integration-2026-10-26/README.md)
содержит точные счётчики и ограничения.

WAMR уже входит в базовый pin; VM, WAMR и guest API этим runtime-кандидатом
не меняются. Сохраняются текущие `forge.vm.wasm.execution` и `forge.api.wasm`.
Новая VM, перенос в другой namespace и новый VM acceptance не заявлены.

Потребители фиксируют SHA содержащего этот отчёт коммита и выполняют
собственную пересборку через установленный package с проверками нужных API. Наличие новых API или успешной
сборки source tree не доказывает установленную поставку. Существующие
исторические migration notes не становятся новыми результатами этого среза.

На macOS arm64 прошли интеграционная сборка потребителя 25 targets,
77 выбранных тестов и один native/WASM/CEL testhost regression. OFF producer
configure/build PASS. Первая install — NOT_INSTALLED при exit 0: package prefix
и ForgeConfig не появились; raw result сохранён. Исправленная внешняя install
команда выбирает Forge sub-build, исходники не менялись; corrected install PASS,
862 файла и ForgeConfig проверены независимой описью. Пустой исходный manifest
не подменяет фактическую установленную опись. Fresh installed sequence39:
первые 24 шага восьми потребителей и runtime configure PASS, исходный runtime
build FAIL на шаге 26 из-за необъявленной прямой зависимости artifact-only CMake.
Producer и исходные тесты не менялись; FAIL сохранён. Минимальное исправление
artifact-only композиции принято после review; configure/build PASS,
оригинальные runtime CTest 7/7 PASS за 2.24 s. Итого 12 обычных fresh installed
consumers PASS. Отдельные оставшиеся
12 шагов завершены: 11 PASS, lifecycle run FAIL с двумя PASS и свежим
воспроизведением известного Asio/Boost allocation-failure. Оригинальные тесты
и ограничения сохранены. Полная package acceptance остаётся открытой, без waiver. Оригинальный изолированный
allocation-failure Asio/Boost 1.90 FAILED; Boost не патчили и waiver —
исключение из требований — не принимали. Dev promotion остаётся BLOCKED
внешним static donor gate с 58 диагностическими сообщениями и известным
allocation-failure. Полная runtime acceptance не объявляется.

Финансовая политика и цена конкретного потребителя не входят в нейтральный
Forge runtime. Linux, live chain, upstream providers и JIT/AOT квалификация
этой заметкой не подтверждаются.
