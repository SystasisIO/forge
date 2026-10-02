# Единый macro-first logger и диагностическая сериализация

## Зафиксированный baseline и scope

- Forge: `bbe5f2bedfc98b37be3b468ceda1f15b7bbf2c65`, свежий `origin/dev`.
- Рабочая ветка Forge: `log-schema-macros-v1`, nested checkout существующего
  потребителя. Это source-API clean break по явно согласованному
  pre-stabilization MINOR exception; release/version/tag не входят в задачу.
- Потребитель Spine остаётся на своей существующей рабочей ветке. Его прежние
  pending changes сохраняются; этот срез не разрешает их автоматический commit.
- CI, workflow dispatch, PR и release не запускаются. Перед обычным push в
  Forge dev повторно проверяются remote refs и workflow triggers.

## Согласованный пользовательский контракт

```cpp
ilog("chain.bootstrap", "Bootstrap completed",
     ("profile", profile)("transactions", count));
ilog(log, "Block ${number} produced", ("number", number));
```

Обычные именованные пары не требуют `log_ctx`, `make_log_fields` или явного
`logger::get`. Допускается короткий default-logger вызов без контекста.
Строковый logger name не определяется эвристикой содержимого message.
Редкие metadata/full-record случаи должны сохранять возможности `log_record`:
component, source location, timestamp/thread, exception chain и stacktrace.
Уровень выбирает macro, маршрут выбирает logger; общая реализация принадлежит
существующей библиотеке `forge_log`.

Макросы обеспечивают lazy evaluation после фильтра уровня, ровно одно
вычисление logger expression и source location пользовательского вызова.
`FORGE_DISABLE_LOGGING` отключает все уровни без вычисления аргументов.
Сохраняются `${field}`, синхронная маршрутизация, parent routing без дублей,
console/file/JSONL и существующий OTLP sink. Новая очередь/runtime не создаётся.

Legacy `forge_tlog/dlog/ilog/wlog/elog`, `forge_log`, `FORGE_LOG_MESSAGE`,
`log_message`, appender factories и compatibility bridge удаляются;
первые-party consumers и конфигурация logging переводятся на record/sinks.
Aliases и второй параллельный logging API не добавляются. Механический migration
путь должен быть описан в unreleased note.

## Диагностическое кодирование

Согласованная точка потребления logger:
`forge::variant_schema::encode_diagnostic(const T&) -> forge::variant` в
существующем модуле `forge.variant.schema`. Интерфейс добавлен этим срезом;
в исходном baseline его нет.

- Известные secret-types всегда получают безопасное представление.
- Для объекта с Schema применяются его canonical field names и `.secret()`.
  Секретное поле не проходит через обычную serialization callback.
- Вложенные objects/containers обрабатываются рекурсивно, в том числе когда
  родитель имеет Describe/PFR, а вложенный тип имеет Schema.
- Без Schema повторно используются существующие scalar/container/custom
  conversion, Boost.Describe и совместимые PFR aggregate mechanisms.
- Отсутствие Describe не означает unsupported. Нет нового сериализатора в
  logger; PFR member mechanics принадлежат Reflect, value mapping — Variant.
- Неизвестный непрозрачный тип получает явное unsupported представление.
  Ошибка диагностического кодирования не включает raw fallback.
- Явно пустая Schema не заменяется сериализацией всех полей. Неотражаемые
  nonsecret-поля Schema получают unsupported; `.secret()` применяется и для
  них до доступа к значению. Generic typed field-list или второй serializer
  для обхода runtime type erasure не создаётся.
- PFR используется в пределах поддерживаемых Boost агрегатов. Его public
  potential trait не доказывает поддержку произвольного C++ типа. Для
  несовместимых форм (например, bitfields/references/inheritance) применяется
  существующий Boost opt-out либо Describe; собственный field-count probe
  не вводится. Обычный совместимый aggregate с `std::string` не требует opt-in.
- Обычная строка без metadata не считается секретом по её имени или содержимому.
- Обычный JSON/YAML/network/Raw encode не меняется. Redacted diagnostic output
  не используется как wire DTO или persisted/config replacement.
- Рекурсивный обход ограничен глубиной 64 и общим бюджетом посещений
  `MAX_NUM_ARRAY_ELEMENTS` (1 048 576), с проверкой pointer-cycle на текущем
  пути. Этот общий бюджет отдельно ограничивает и допустимые большие byte
  sequences; исчерпание бюджета не открывает raw/custom
  fallback. Эти ограничения относятся к собственному обходу Forge; произвольный
  пользовательский `to_variant` может выполнять стороннюю работу и не получает
  гарантии ограниченного времени выполнения.
- Проверка пользовательского converter видит доступные Schema/Describe/PFR
  поля. За секреты в скрытом внутреннем состоянии непрозрачного типа отвечает
  автор converter; их нельзя обнаружить без metadata или neutral secret hook.

## Команда и владение

- `sol61_schema_dev`, GPT-6.1 Sol xHigh: Reflect/Variant/Schema, связанные tests,
  локальные README этих библиотек. Не редактирует Log и Spine.
- `sol61_logger_dev`, GPT-6.1 Sol xHigh: Log, logger consumers/OTLP tests/package
  consumer в Forge и macro-only миграция logging calls в Spine. Не редактирует
  Reflect/Variant/Schema или их tests.
- `sol61_logger_reviewer`, GPT-6.1 Sol xHigh: независимое read-only review точного
  интегрированного diff и acceptance evidence, без истории чата.
- Координатор: baseline, границы/API, сборочные gates, документация поставки,
  remediation loop, проверка и обычная доставка Forge dev.

Разработчики не commit/push/merge/release. Общий nested checkout используется
с непересекающимся владением файлами; дополнительные source/build worktrees не
создаются. Сборочный каталог и запуск Ninja координирует основной агент.

## Приёмка

- Macro forms, поля/template, metadata/full-record parity, lazy arguments,
  logger-once, disabled compile mode и корректная source location.
- Schema secret redaction до интерполяции/сериализации, вложенные aggregates и
  контейнеры, Describe/custom conversion/PFR, unsupported и failure behavior.
- Успешный Raw golden/regression и обычный JSON/YAML encode без изменений.
- Console/JSONL/OTLP содержат одинаковое событие без секретов и дублей routes.
- Нет удалённых macros/modules/appender consumers, есть migration note.
- Local Structure/format и затронутые Log/Schema/Variant/Raw/Codec/Config/OTLP
  tests, installed consumer. Targets собираются перед CTest, LLVM/Ninja `-j 4`.
- Независимый reviewer: no reportable findings после remediation.
- После доставки Forge — exact pin в Spine и возврат к полным отложенным
  signer/wallet/program/three-node Docker gates на новых изолированных данных.
  NOT_RUN, scoped PASS и старая Docker image не означают завершённую приёмку.
