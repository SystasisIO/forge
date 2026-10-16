# OC phase 0: решения и открытые проверки

Журнал реализации с 2026-10-12, не акт полной приёмки
[плана](../iterations/forge-vm-wasm-optimizing-v1.md). Текущий снимок — 2026-10-14.
Исторические записи ниже относятся только к указанным исходникам и проверкам;
прежний PASS не переносится автоматически на последующее дерево.

EOS-VM и EOS-VM-OC остаются движками; Wasmtime — только донор платформенной
механики. Обязательные платформы — Linux x86-64 и macOS ARM64. Текущий проверенный
срез — отдельная чистая нативная macOS ARM64 сборка на изолированном Clang 23.1.2
с заголовками libc++ 22.1.8, SDK 15.4 и системными linker/libc++.
LLVM-библиотека OC lowering остаётся 22.1.8; старые build/prefix и Homebrew не менялись.

Forge: обычный граф сборки и обе новые установки прошли проверку. Актуальные
11 выбранных VM/DB проверок и 5 package-гейтов — PASS, включая свежие compiler fixtures,
compiler service, compiled execution и реальные relocated consumers/helper.
Независимое review текущего пакета доказательств — CLEAR, 131/131 привязок.
Это ограниченная Darwin-проверка, не полная OC/Spine приёмка.

Spine: все четыре продуктовых бинарника (`spineos`, `spinekd`, `spinead`,
`spinectl`) собраны и слинкованы; три owning test-target также собраны.
Все 12 разрешённых runtime/package групп Spine прошли, включая две package-проверки.
Три безопасных negative config-проверки отдельно завершились ожидаемым отказом.
Независимое review итогового Spine receipt — CLEAR в этом выбранном Darwin-срезе;
23 runtime-привязки и 85 привязок compiled inputs совпали.
Source-reviewed разделение зависимых config/API типов, прямые imports,
тестовая JSON dependency и переход теста на существующий Schema decode не меняют
имена типов, состав и типы полей, defaults или consensus semantics.
Module ownership config-типов изменился: затронутые BMI и consumers пересобраны,
старые и новые objects не смешивались. Точные срезы и ограничения
зафиксированы в локальном [scope](../../build/clang23-clean-acceptance.50fj8B/scope.md).

Generic Forge JSON/Variant ограничение остаётся OPEN: на Darwin для `size_t`
нет read-overload `from_variant`, а Schema-ветка `json::load<T>` не исключает
инстанцирование reflected fallback. Продуктовая конфигурация использует прежний
Schema-путь; тестовый `load_document` → `config::core::decode` сохраняет
parse/decode ошибки и явный отказ на `config.unknown`, но не исправляет generic API.

Локальные доказательства (ignored artifacts, не публикуемые package-материалы):
[Forge receipt](../../build/clang23-forge-native.X5j4mE/acceptance-evidence-v3.json)
SHA-256 `9d4010593f4c444d55e2b4a8b35cff240d13b5cd36e7ef3184e7b39a0ef3a807`;
[Spine product-build receipt](../../../spine-vm-wasm-optimizing-v1/build/spine-clang23-clean-plan-v1.jsAyCr/build-product-only-v1-results.json)
SHA-256 `fe77e5168c56c81da9cb72bb6a4d377c8afcb41fae4fef2dd3d75ed47d3e382f`;
[Spine owning-test build log](../../../spine-vm-wasm-optimizing-v1/build/spine-clang23-clean-plan-v1.jsAyCr/build-owning-tests-v1.log)
SHA-256 `d1fa9f25338609dfeefff1fe40fc49cf9805280185290c0900a0f19954064daa`.
[Spine selected runtime/package receipt](../../../spine-vm-wasm-optimizing-v1/build/spine-clang23-clean-plan-v1.jsAyCr/runtime-results-v1.json)
SHA-256 `02faf99eb67df14ad881ba30373dde1f8bef580f915451d3fb9d774b8c4e6b5e`.
PASS выбранных групп не означает выполнение всех тестов этих бинарников.

Нативный Linux, полная продуктовая интеграция, сравнение производительности
с Spring, CI и итоговое слияние не выполнены. Ранее исключённые
Mach-negative/native-signals проверки в этом новом срезе не запускались.

## Baseline

- Forge: `94714f0ff465435ef317ec1891d44b1e9380c01e`.
- Spine: `1cdc9ab9294996f95c20c91f0810d999d3c82a7f`.
- Spring: `e6a99f68b67abc4d89fe716755b2e1394a4991f7`.
- Read-only `git ls-remote https://github.com/AntelopeIO/spring.git HEAD
  refs/heads/main` подтвердил тот же SHA на 2026-10-12: donor соответствует
  текущему upstream main, незаметно пропущенного более нового OC среза нет.
- Локальная платформа: macOS ARM64, LLVM 22.1.8, CMake 3.31.6,
  Ninja 1.13.2. Нативного Linux x86-64 host нет.
- Пользователь разрешил один целевой ручной native Linux x86-64 CI run после
  локальных проверок и review. Другие workflows/release/tags не разрешены.
- Старые checkout/builds и пользовательские данные остаются нетронутыми.

## Review findings и решения

| Вопрос | Решение | Обязательное доказательство |
|---|---|---|
| Parser спрятан в partition | Использовать существующий публичный `interpreter::parser` alias и typed module/opcodes; отдельный writer facade не нужен | Installed consumer, несколько функций, branch PCs и `br_table` payload |
| Parsing не равен полной подготовке | Сохранить проверки `module.error`, data segment bounds и imports из владельца backend | Ошибочный table/data/import отклоняется до native execution |
| Общий interrupt mapping донора | Interrupt domain принадлежит invocation, не shared artifact | Два concurrent вызова одного artifact: отмена одного не мешает другому |
| Донор хранит только apply entrypoint и process-local type addresses | Typed exports и стабильные signature IDs; никаких compiler-process pointers | `apply`, `__forge_call`, indirect calls, artifact load в новом процессе |
| Завершение фоновой компиляции во время применения блока | По уточнённому donor contract прервать только разрешённое выполнение и откатить всю транзакцию перед единственным повтором | Те же expected receipts и исходный deadline; уже выполненные effects, traces и accounting не дублируются; обычный timeout не разрешает повтор |
| Strict miss обнаруживается после DB effects | Полный enclosing rollback, preparation вне writer, bounded retry | Same-block setcode, dynamic nested target, scheduled/onerror |
| Production session удерживает writer | Прервать всю session перед preparation; сохранить mempool, пропустить слот | Следующий слот продолжает production; run loop не завершается |
| Generic Forge error fault-ит controller, wasm_execution_failed отвергает block | Отдельная typed local operational category и явное mapping | Candidate не invalid, operational retry bounded; integrity fault не маскируется |
| Spine native graph собирает Forge из source | Existing installed Forge leaves + public Forge Contract registry/types | Relocated prefixes без source paths; notices из установленного package |
| Текущие setcode limits строже правил исполнения старого code | Compiler получает execution snapshot; admission остаётся в product setcode path | Existing `execution_does_not_revalidate_static_setcode_limits` на OC |

Источники Spring: `libraries/chain/webassembly/runtimes/eos-vm-oc/executor.cpp`
(interrupt), `compile_trampoline.cpp` (artifact),
`unittests/eosvmoc_interrupt_tests.cpp` (tier-up oracle).
Источники Spine: `libraries/chain/execution/{host,intrinsics}.cpp`,
`plugins/chain/controller/controller_runtime.cpp`, root/execution CMake.
Forge parser boundary:
[`backend.cppm`](../../libraries/vm/wasm/interpret/include/forge/vm/wasm/interpret/backend.cppm),
[`bitcode_writer.cppm`](../../libraries/vm/wasm/interpret/include/forge/vm/wasm/interpret/bitcode_writer.cppm).

## Параметры VM

`default_options` недостаточно: продукт использует собственные compile-time
traits и runtime limits. Нужен owned resolved snapshot, полученный через
существующие default getters; не добавлять второй набор defaults.

Пользователь одобрил расширение существующего `interpret::options`
отсутствующими traits и увеличение `max_linear_memory_init` с uint32 до uint64
в соответствии с parser. Это узкое изменение C++ layout требует пересборки
потребителей; второй дублирующий тип настроек не вводится. WASM/wire и
persisted chain layouts не изменяются. Проверки должны сохранять различия
между отсутствующим trait и явно заданным значением через существующие getters.

## Зафиксированное направление ABI

Reviewer согласовал явный invocation-context pointer как адаптацию механики
донора вместо GS offsets и общей `mprotect`-отмены. Это не подтверждение
готовности исполнения. Перед параллельной реализацией compiler/runtime
обязательны:

- одна версионированная ABI definition для обоих владельцев: layout/alignment,
  raw-bit callbacks, memory growth, globals/table, depth/stack и trap restoration;
- `deadline` и `std::stop_token` в каждом invocation;
- один resolved `interpret::options` в identity/artifact; независимый второй
  набор `max_pages/max_call_depth` в executor не вводится;
- canonical identity через существующие Forge SHA-256/Raw и немедленный
  memory-only ready lookup без disk/IPC/compile wait;
- проверка каждого emitted native frame и безопасный общий stack budget;
- ограниченный shutdown с завершением и reap дочерних процессов.

Уточнения следующего реального API-среза:

- `compile_configuration` совместно используется request и artifact: version,
  существующий `interpret::options`, imports, target и compile limits. Request
  добавляет исходный WASM; artifact хранит его digest, а не дублирует весь WASM
  в закреплённом native image. Canonical identity использует Forge Raw/SHA и
  все поля этой единой configuration; второго options DTO нет.
- `compiler_session` получает supplied IO executor и существующий
  `forge::asio::compute::executor` для bounded disk/decode work. Скрытый thread
  pool не создаётся; `ready` остаётся только in-memory lookup.
- Численные операции публикует существующий backend owner: реальный dispatch
  по существующему opcode вызывает его SoftFloat helpers. Отдельный facade,
  копии алгоритмов или импорт private partition из optimizing запрещены.

Это согласованные контракты разработки, не свидетельство готовых APIs.

## Замечания первого review

1. Concurrency fixture выполнял все вызовы второго context до отмены первого.
   Исправленный fixture требует 50000 пар add/load второго context, пока первый
   удерживается в cancel trap, и ещё 50000 после его выхода; writable memory и
   canaries раздельные. Новый запуск и повторное независимое ревью прошли.
   Этот fixture ещё не проверяет globals, stores или floating-point isolation.
2. Spine notices discovery теперь использует metadata установленного Forge
   package, а не вычисляет фиксированный `lib/../share` layout.
3. Source-private pugixml include исключён из installed export владельцем
   Forge codec/package. Metadata gate проверяет genuine generated export,
   default/alternate relative GNUInstallDirs, relocation и build-tree discovery.
   Это не полная сборка installed production dependency closure.

Package-срезы прошли независимое повторное ревью: no reportable findings.
Forge patch SHA256 `dc096eed2471e6bd4a9c1455540fe68408febd4f298e5ffbae1b841a067c0cc1`,
Spine patch SHA256 `677a0887247bdec66ac3a712b8650a3d77aa6ca5e20809818155424fc5be8788`.
Текущий старый Spine gitlink ещё не содержит metadata, поэтому Docker stage
явно передаёт notices path собственного фиксированного install prefix.
При обновлении exact Forge pin этот временный override удаляется; package
discovery обязан пройти без него. Gitlink в этом срезе не менялся.

## Общий SoftFloat substrate

Независимая проверка обнаружила shared-state race, не зависящую от OC:
SoftFloat `19cbea6c254cdbb5d539584e5696675de997721d` собирает четыре C globals
(`roundingMode`, `detectTininess`, `exceptionFlags`, `extF80_roundingPrecision`)
без `THREAD_LOCAL`. В проверенном Mach-O object они находятся в `__common` и
`__data`, а не thread-local sections. Одновременные inexact operations пишут
общий `exceptionFlags`, даже если rounding mode не изменяется.

Исправление назначено владельцу Forge build: target-scoped согласованное TLS
определение для C/C++ consumers, без изменения донора и без глобальных compiler
flags. Оно относится ко всем VM engines. `softfloat.hpp` задаёт default
`__thread`, но не объявляет эти globals; mismatch деклараций сам по себе не
доказан. Изменение storage ABI требует полной пересборки SoftFloat и consumers.

Нужны deterministic barrier tests адресов/defaults/rounding/flags между
потоками, concurrent float execution, object TLS evidence и installed consumer.
TLS не заменяет invocation-local save/reset/restore при nested execution на
одном потоке. Narrow TLS patch
`c698ead7e89ede5b17598b11de6b36148bed764432f330e1cb304988d9ceb6bb`
прошёл независимое ревью без reportable findings и интегрирован. Подтверждены
barrier-controlled тесты всех четырёх переменных, реальные concurrent VM/LLVM
float calls и TLS sections нового Mach-O object. Fresh root build VM также
прошёл, installed export содержит `THREAD_LOCAL=__thread`.
Installed C/C++ consumer проверен отдельным срезом ниже; native Linux ELF
execution ещё не проверен.

Reviewer согласовал реальный reusable public leaf
`interpret::floating_point_environment`, парные `.cppm/.cpp`, без копирования
численных алгоритмов. Он сохраняет все четыре значения, устанавливает canonical
RNE/afterRounding/flags0/precision80 и восстанавливает исходные значения в
`noexcept` destructor; copy/move запрещены. Scope размещается в двух numeric
execute gateways interpreter/debug и JIT перед signal/trap boundary и в OC
executor, не внутри nonlocal-unwind lambda и не на каждом внутреннем WASM call.
Дополнительно host callback не должен оставлять изменённую среду последующему
guest коду: требуется безопасное восстановление при normal/exception/nested
возврате. Это согласованный контракт следующего среза, не готовая реализация.

Первый FP scope diff собран и проходит узкие тесты, но ещё не принят:
время жизни guard при переходе между outer/inner signal destination требует
отдельного доказательства. C++ exception unwind отличается от raw host memory
fault: `siglongjmp` не вызывает деструкторы пропущенных host frames. Существующее
ограничение нельзя скрыть вложенным signal boundary или объявить исправленным
по trivial-only fault fixture. Доработка принадлежит existing signal owner;
новый параллельный process-wide handler не создаётся.

Первоначальное архитектурное ревью signal proposal SHA256
`dbce2f9e7dfe93fe676ce7e6e565ce7d914fa49739d753d035cc4494226f95e3`
разрешило реализацию, но не засчитало runtime gate. Существующий owner будет
публиковать narrow recovery suspension и native raw-entry boundary через
public backend. Для legacy FP scope порядок обязателен: отключить outer VM
destination → создать guard → inner legacy boundary → вернуться к null
destination → уничтожить guard → восстановить outer destination и mask.
Это требуется и на host, и на обоих numeric entrypoints при nested вызовах.
Для нового native пути raw host fault остаётся foreign/fatal; обычное C++
исключение сохраняется только после unwind callback и выходит из generated
frames после возврата callback. Native fault принадлежит VM лишь при
synchronous signal и PC внутри явного code range; пустые ranges не catch-all.
Alternate signal stack предоставляется владельцем invocation; active
`SS_ONSTACK` отклоняется, setup/teardown ошибки обязаны восстановить прежнее
состояние. Существующее отсутствие managed SIGBUS на Linux не меняется.

Последующее source review выявило открытый P1: одного отключения recovery
вокруг конструктора/деструктора недостаточно. Весь C++ callback, local catch,
nested invocation, result conversion и exception temporaries должны завершаться
при отключённом ancestor recovery. Первоначальный throwing helper заменяется
предложением raw noexcept/POD boundary; typed error decoding остаётся у
invocation owner вне восстанавливаемых generated frames. Amendment v2 SHA256
`9a6ee7e249cc806fc8b4aa07e9e6ca8cbe0c3edc8e58ae5541d9ff1ef8b2dd7c`
содержит карту двух execution roots, трёх host gateways, двух x86 bridges и
`throw_<E>`, а также ownership существующего exception transport. Reviewer и
координатор первоначально приняли v2 architecture для реализации. Этот
architecture verdict отозван после проверки реального host route (ниже);
source-исправление и runtime не приняты. Borrowed signal stack нельзя
освободить после неудачного OS restore; для невосстановимого нарушения этого
инварианта предусматривается fail-stop, не возврат мнимого успеха. Обычные
trap, typed host failure, deadline и cancellation остаются recoverable.

Повторная сверка исходников выявила противоречие v2 route map:
`execution_context_base::invoke_host` создаёт FP guard при отключённом recovery,
но затем `invoke_with_signal_handler` устанавливает новый checkpoint **до**
вызова `_rhf`. Вложенный VM entry из C++ callback снова получает active
ancestor; локальный `catch` и C++ resource cleanup нельзя считать защищёнными.
Второй whole-callback suspension устраняет этот переход через живые C++ frames,
но меняет исторический перехват аппаратной ошибки внутри самого host callback.
Такая смена поведения не считается автоматически разрешённой портированием OC.

Проверка гостевых аргументов уже принадлежит существующим
`execution_interface::validate_pointer` и `host_function::argument_proxy`:
неверный указатель за пределами явно объявленного диапазона отклоняется до
вызова native host function. Поэтому прежние
reexport tests с UINT32_MAX не доказывают необходимость восстановления после
произвольного raw host fault. Spine integer division builtins также уже
проверяют деление на ноль и signed overflow до C++ arithmetic; их не требуется
дублировать или переписывать. Дополнительный аудит нашёл два обязательных
product closure checks: `prints` передаёт one-byte-validated pointer в
`console.append`, а `eosio_assert` использует `strnlen(..., 1024)` без проверки
остатка WASM memory. Отсутствующий NUL возле конца памяти способен вызвать raw
host fault уже после проверки первого байта. До изменения signal policy нужно
закрыть такие guest-induced ошибки существующими VM validation mechanisms и
проверить остальные host imports. Они не относятся к допустимому foreign/fatal
дефекту C++ реализации. Пользователь явно разрешил выбранную границу:
сохранить software checks, а настоящий аппаратный сбой C++ host callback
считать foreign/fatal, не обычной ошибкой контракта. Это осознанное отличие
от legacy и OC Spring, где memory-address-based recovery исторически мог
перехватить такой fault внутри host. Оно не разрешает завершать процесс на
некорректных гостевых аргументах. Signal/FP wiring возобновляется только с
исправленной whole-callback route map и проверенной software host closure;
P1 остаётся открыт до exact source review и исполнения regression tests.
Отдельный новый signal framework не вводится.

Исправленная amendment v3 SHA256
`00cf69050ac9c6df1b360aa600f8d7928f401015b086857bb91a881d3036752e`
получила независимое разрешение на реализацию. Reviewer сверил оба numeric
roots, общий host gateway, imported-only export, x86 bridges и typed tail.
Из host route удаляется внутренний recovery checkpoint вокруг `_rhf`;
весь callback, включая nested invocation, local catch и деструкторы, должен
завершиться при NULL recovery. Exception outcome принадлежит invocation выше
raw checkpoint. Это архитектурное решение, не PASS исходников или runtime;
полное исправление ещё не интегрировано.

Отдельная диагностика macOS 26.1 arm64 изолировала ошибку alternate-stack
restore: реальная системная реализация читает поле jump buffer, которое её
`setjmp` не заполняет. Standalone POSIX C matrix воспроизводит 6/6 отказов
после jumps с uninitialized automatic buffer и 8/8 успехов с обычной
whole-object инициализацией; повтор без диагностического чтения opaque bytes
даёт тот же результат. Предлагается только `sigjmp_buf destination{};`, без
записи private offsets и без собственного jump implementation. Legacy
regression после этой правки проходит, но новый concurrent native signal
test пока завершается тайм-аутом. Его причина исследуется отдельно; signal
draft и FP scope ещё не интегрированы и P1 не закрыт.

Отдельный bounded differential запуск того же concurrent test с прежними
test objects показал: до инициализации jump buffer — 3 тайм-аута и один отказ
проверки результата; после — 4 тайм-аута. Инициализация не является необходимой
причиной зависания; причина самого зависания пока не установлена. Сборка
ограничилась отдельным diagnostic object/binary, product archives и BMI не
изменены. Raw evidence в runtime worktree:
`build/signal-boundary-diagnostic434-results.log`, SHA256
`dd720a89b35c7c384ed21d909b81e73d9e8dbd789730183e3e6bf9d18b911946`;
команды `build/signal-boundary-diagnostic434-commands.json`, SHA256
`fbf77cb8033ab32449447d0f15482889a7ea7b34657c9b81c2493340b9a5b644`.
Reviewer подтвердил только разделение причин, не успешную concurrent recovery.

## Проверки этого среза

- `git diff --check`: PASS для текущих документальных изменений.
- `python3 tests/structure/check_structure.py .`: PASS,
  2348 first-party source files.
- Координатор повторил исполнение LLVM spike на macOS ARM64: PASS для
  ограниченного набора integer/SoftFloat/host/indirect/memory/trap/interrupt
  fixtures. Повторный fixture и review подтвердили описанную выше ограниченную
  concurrent cancellation проверку; globals/stores ещё не покрыты.
  `llvm-readobj` подтвердил формат
  второго результата `elf64-x86-64`, не его исполнение.
  Исходники, точные команды и ограничения находятся в developer worktree;
  независимое ревью ABI и exact diff продолжается.
- Native Linux x86-64 execution, OC package relocation, Spine integration,
  Docker и финальный CI: NOT_RUN.
- Fresh integration root configure и `forge_vm_wasm_interpret`/`forge_codec_xml`
  build `-j 4`: PASS; metadata gate также PASS. Полный `forge` build выявил
  смешение окружения: библиотеки OpenSSL 3.6.4, но общий Homebrew include
  подставляет заголовки OpenSSL 4.0.3 в `crypto/pki/x509.cpp`.
  Реконфигурация с точными package prefixes устранила общий include из команды;
  полная сборка target `forge` с `-j 4` и установка component `dev`: PASS.
  System packages, PKI source и старые builds не изменялись. Installed package
  скопирован в новый prefix; VM consumer собран и исполнен штатным relocation
  gate: PASS. XML consumer отдельно сконфигурирован, собран и исполнен из
  того же перемещённого package: PASS. Это проверяет текущий interpreter/package,
  не ещё не реализованный OC. Raw logs сохранены под `build/native-integration-*`;
  command/exit manifest — `build/native-integration-package-runs.json`.
- После сборки соответствующих targets основной root graph прошёл 4/4 CTest:
  unit 188, internal 12, spec 2428 cases и spec generator mapping. Options и TLS
  включены в этот unit target. Log SHA256
  `69e57389740a4a32c29c60404697ec25fd83d4e703e938d987aed2274d0c49b9`.
  Новый signal boundary и FP scope в этот проверенный graph ещё не интегрированы.
- Reviewer побайтно сверил 21 интегрированный файл с accepted freezes,
  package exports, TLS symbols и raw test evidence: no reportable findings
  в этом prerequisite-срезе. Отдельный ABI rebuild disclosure SoftFloat
  добавлен в `docs/releases/unreleased-softfloat-tls-abi.md` и принят reviewer;
  SHA256 `a125a70ed3c2a83b915f8e2bd57d95c04a7c7b0c917ce83d93fbc837763e96d2`.
  На момент этого initial prerequisite gate installed C/C++ TLS consumers
  ещё не запускались; их отдельный результат приведён ниже.
- Options/preparation slice: 4/4 CTest targets на macOS ARM64 Release;
  focused 12, штатные unit 187, internal 12 и spec 2428 test cases.
  Координатор сверил raw logs и SHA замороженного diff
  `d252e96d919a8c8e884b7610530f54cb1ae172aa0b18a941c7b701d18a2b12e1`;
  новый regression source SHA
  `9a3c4b8ac73204d98a9ebcced08e6ba3a186df24506b080096f2280c1571392a`.
  Сборка использует реальные owning targets в auxiliary graph, не полный
  Forge root/package build. Независимое функциональное ревью этих exact SHA:
  no reportable findings. Форматирование исправлено; повторный exact-diff
  review нового patch SHA256
  `b0d5ae1884ff9a98e59501f6a7bef8c0ad7dcb991d25c3a49aedd3675e76c970`
  также не имеет reportable findings. Новый regression source SHA256
  `c409814437d0e00a60f43cf177cc65f52fdf65f3adb31c53674cedbddcd077dd`.
  Финальный последовательный auxiliary build `-j 4` и CTest: 5/5,
  options 12, unit 188, internal 12, spec 2428, отдельный TLS testcase.
  Успех TLS testcase не заменяет отдельное ревью его реализации.
  Options и Forge package-срез механически перенесены в integration checkout;
  координатор подтвердил совпадение options patch/test/release hashes,
  structure (2349 files), clang-format нового теста и diff-check.
- Координатор повторил tiny installed interpreter consumer Spine до и после
  перемещения Forge/SDK prefixes: по 1/1 PASS. Full native build и clean
  production dependency closure этим не проверены.
- Для полного Spine consumer Forge повторно собран с RocksDB и установлен:
  PASS. Новый Spine host graph использует installed Forge и Contract SDK;
  notices найдены через package metadata без локального path override.
  Первый полный build остановился на отсутствующих прямых Schema dependencies
  у `chain.admin` и `chain.gateway`. Reviewer принял исправление target dependencies
  SHA256 `e6f1d052c7e0258828c41172be5e3fd580845c12174eea691a7fe2915ee71ee6`.
  Повторная сборка прошла этот участок и выявила отсутствующие прямые зависимости
  тестовых targets. Reviewer отдельно принял DB driver dependency slice SHA256
  `30580a593810b0c1fcaa57daab24607fc939021b8470473cfb19d9c87a6e2f6a`.
  Аудит 35 configured test targets и 4 неактивных test targets завершён:
  138 прямых owning-target dependencies в 26 CMake files. Reviewer выявил
  одну ошибку PUBLIC/PRIVATE у validator fixture; после исправления принял
  exact patch SHA256
  `6b2afffd7b3bb5e2644776c22499db215decbd03a4de5b02b7055bcb7c53b9cb`
  без remaining findings. Все 138 additions имеют PRIVATE scope; неактивные
  targets проверены по исходникам, не собраны. Следующий full build прошёл
  эти места и остановился на недостающем прямом Schema import у Admin console.
  Аналогичный аудит 35 configured production targets завершён: 38 отсутствующих
  owning-leaf dependencies (8 PUBLIC / 30 PRIVATE), 2 подтверждённых public
  module imports переведены PRIVATE → PUBLIC, отдельно добавлен Schema console.
  Exact CMake-only patch SHA256
  `afac9b17091f33f8b3b54e0c869e78363e8fff7854561f00bbaba04f77de85fb`
  принят reviewer без reportable findings. После аудита отсутствующих dependencies,
  неизвестных owners и ошибок scope нет; 2036 import records сверены с исходниками
  и 551 installed module owner. Coordinator reverse check/diff-check PASS;
  повторная полная сборка `cmake --build build/native-integration -j 4`
  завершилась успешно (1067/1067 steps, exit 0). Raw log
  `build/native-integration-production-import-build.log` в consumer worktree:
  SHA256 `24cca9724425bd1428529adc70fca36afa9c9428da46b3a351e1584d26694ffd`.
  Все четыре программы и зарегистрированные test binaries собраны.
  Отдельный прогон 11 библиотечных CTest groups: 11/11 PASS, raw log
  `build/native-integration-library-unit-ctest.log` SHA256
  `103acb9f6107c8fc9e995a5ef6297521f7fdc7e7ec4c36d73ac03ff1e4d14951`.
  После сборки полный nonpackage selection прошёл 40/40 CTest, exit 0,
  152.16 секунды: `ctest --test-dir build/native-integration --output-on-failure
  -j 1 -E '^test_spine_package_'`. Включены 8 static checks, CLI parity,
  зарегистрированные unit/integration и 4 validator groups. Raw log
  `build/native-integration-nonpackage-ctest.log` SHA256
  `abfa576d9f0e4be5e8cf101ab5d723c40907e8f4a1a3fa92867c293312b9c269`.
  Из восьми package gates metadata и dev install прошли отдельно 2/2:
  raw log `build/native-integration-package-metadata-install-ctest.log`
  SHA256 `1cdb55da43b8c7407ef306c9fb11a8a2924a306d8286c3d0a7263d2738835a9f`.
  Все шесть компилируемых external consumers затем прошли отдельно: VM,
  Chain Audit/Protocol/API, Admin Protocol/API. Таким образом, 48/48
  зарегистрированных CTest этой конфигурации прошли частями. Raw logs и hashes
  записаны в consumer note `docs/donors/forge-wasm-optimizing-consumer-phase0.md`.
  Независимый reviewer повторно сверил журналы, hashes и шесть evidence archives:
  prerequisite принят, не runtime/merge approval.
  Их временные build graphs удалены только после сохранения logs, metadata и
  binaries; основные builds и prefixes оставлены. System-contract и frontend
  tests в этой конфигурации отключены, полного acceptance verdict нет.
  Это prerequisite package regression, не проверка OC execution.
- Полный первый OC artifact slice заморожен отдельно от непринятых FP/signals:
  patch SHA256 `284a2dad4b5c5eeecf56326b71bce28fec2e457f0a15484deca06191b530456e`.
  Его auxiliary graph использует существующие Raw/digest/VM owners. CTest 1/1
  (7 cases): PASS; raw log SHA256
  `260a2847f13350a3e0657fe7a297d9dcd05a36c858f469fc644451a92d3c5fd8`.
  Проверены identity, binary roundtrip и отказы повреждённых artifacts.
  Независимое implementation review exact diff: no reportable findings в этом
  ограниченном срезе. Координатор механически интегрировал все 15 файлов;
  reverse patch check и diff-check: PASS. Linux executable mappings,
  lifetime и исполнение relocations остаются NOT_RUN. Root registration и
  installed OC consumer не включены в этот результат.
- Отдельная root/package/test registration принята reviewer и интегрирована:
  patch SHA256 `1778da272e59bf0ae48d80ab6433f265a2e1f8f6f4978008442839583084888f`.
  Координатор повторил narrow structure regression (3/3) и полный structure gate
  (2359 files): PASS. Явный `FORGE_ENABLE_VM_WASM_OPTIMIZING=ON` на Darwin/arm64
  корректно отклонён до dependency discovery, exit 1 с ожидаемой диагностикой.
  Это negative gate, не успешная OC-сборка. Default остаётся OFF.
  Raw logs: `build/native-integration-artifact-structure.log` SHA256
  `016a8d1e21a062bd338ff3ab6407cc575b839aae179b028b82d95cde451204a2`;
  `build/optimizing-unsupported-coordinator.log` SHA256
  `9b668e9b4262be9a22fb8c894ab816f839c8a4e959af7418203b225265986c25`.
- Registration gate дополнительно требует 64-bit pointers: Linux x86-64
  processor с `-m32` не соответствует native ABI. Reviewer принял full patch
  `5be1ba7bdf0fdf7382a2ceef8013c8c57bd985cdf3c14a45a223e9aa07d333d0`
  и incremental delta
  `caf7108681cee2220b43aa3c615dd382186193b8a74ff15128325fdfc3328294`.
  Координатор интегрировал только delta, reverse check и diff-check PASS;
  повторный Darwin/arm64 configure отклонён ожидаемо, exit 1. Raw log
  `build/optimizing-unsupported-coordinator-v2.log` SHA256
  `5ea7deb0a24248b8279500a1e1f56a5fd1225b1eb9d5752e45668363bd5d1ec4`.
  Linux `-m32`, supported ON build и installed OC package ещё NOT_RUN.
- После успешных VM/XML installed consumer gates координатор удалил только
  воспроизводимые `build/native-package-vm` и `build/native-package-xml`
  (около 575 MiB). Raw command/build/run logs остались вне этих каталогов;
  CMakeCache/build.ninja сохранены в
  `build/native-package-consumer-metadata.tgz`. Installed prefixes, active
  builds и старые сборки других worktrees не изменены. Для повторного запуска
  этих двух consumers требуется пересборка, исходники тестов сохранены.

- Первый compiler-owned slice принят reviewer и механически интегрирован:
  patch SHA256 `c3018d5263a54ba61591dca7e47030e5b8de9425d284b1e5f7a7344ed136d493`.
  Он использует существующий parser с теми же execution options,
  `finalize`/`validate_instantiation`, LLVM 22.1.8 и нормализованный ELF64
  x86-64 output. Поддержанный в этом срезе набор ограничен integer arithmetic
  и direct calls; неподдержанное корректное WASM не объявляется invalid.
  Финальная auxiliary сборка owning target `-j 4`: PASS, raw log
  `compiler-final-build.log` SHA256
  `c6d79d89c4df875c4201db588a7214e8e0db711e4debff8329848516ce971832`.
  CTest 1/1 (3 cases) PASS на macOS arm64: raw log
  `compiler-final-tests.log` SHA256
  `d9b753142435984802db5013426b8aac6fb2426197b76dea99fdd99f22bc3954`;
  raw LastTest SHA256
  `be748875235ee2dc157edc6d068a170f07cfcc3232a7d54d09022f0e442edbee`.
  Exact source/final-evidence review: no reportable findings в этом срезе;
  coordinator reverse patch check и diff-check PASS. Native Linux execution,
  полный lowering, executor, worker, root registration и installed compiler
  package этим не подтверждены. Регистрация/доставка notices проверяются
  отдельным diff, не частью этого source acceptance.
- Compiler root/package/test registration также принята reviewer и
  интегрирована отдельно: patch SHA256
  `f6f48d2a8e45d4b7b29f284945a0e1fbaa0468b825c7c7a9660f508593d61e30`.
  Runtime component не вызывает LLVM discovery; compiler component требует
  LLVM 22.1.8, target владеет этой зависимостью PRIVATE. Default OC OFF.
  Coordinator reverse patch check PASS; narrow structure regression 4/4,
  полный structure gate (2366 source files) и pinned provenance check PASS.
  Raw logs `build/native-integration-compiler-structure.log` SHA256
  `ffc76ea5d6df3f35a67cfc04cc8e1977c352d341fdc1be42063cb66c4261feb6`,
  `build/native-integration-compiler-provenance.log` SHA256
  `7fe6e8ca3762af70dfad711b59bcf4fe00b3ffaa47221bc61423cdfbbcacf64d`.
  Linux ON build и installed compiler consumer остаются NOT_RUN. Будущий
  helper в продуктовой поставке должен включать Spring/WAVM notices;
  расширение existing notice closure готовится отдельным packaging diff.

- Installed C/C++ SoftFloat consumer slice принят reviewer и интегрирован:
  patch SHA256 `6ce449d95110e7acea74501d9a8063d733549e9429b969d2bdfb90088eb48146`.
  Consumer собирает две translation units против настоящего relocated Forge
  package и проверяет PUBLIC TLS definition, cross-language identity всех
  четырёх переменных, изоляцию parent/two workers и 40000 rounding-sensitive
  вызовов. Сохраняется прежний interpreter smoke. Fresh build `-j 4` PASS:
  `tls-installed-consumer-build.log` SHA256
  `f3ba380246aa220ee706c70bc49ad814380cff1aabc0f51e054af84f532d67ec`.
  Developer execution exit 0; координатор независимо повторил сохранённый
  executable SHA256 `37fd63041d4e35c16517b44d877c7a9ef448dd656e306f2fd7cbb57fdb3051af`:
  exit 0. Reviewer проверил source, formatting и реальные TLVP relocations
  обеих translation units: no reportable findings. Coordinator reverse patch
  check/diff-check PASS. Linux TLS/OC и полный relocation driver с OC ON
  этим не подтверждены.
  После сохранения executable, raw logs, CMakeCache, build.ninja и object
  evidence вне graph удалён только воспроизводимый TLS consumer build
  (362 MiB). Sources и installed prefixes не изменены; graph восстанавливается
  стандартной consumer-сборкой.

- Root-level notices metadata slice принят reviewer и интегрирован:
  patch SHA256 `8fe66cf920179581cf59ddd7639e0b058d86eff98476d3408233041ab99ee87c`.
  Одна existing install-source list формирует `Forge_NOTICE_FILES`; это только
  обязательные root-level имена, не полный список third-party подкаталогов.
  Redistributor обязан копировать целиком existing `Forge_NOTICES_DIR`.
  Предыдущий complete-closure claim исправлен по замечанию reviewer, новая
  параллельная система лицензий не создавалась. Coordinator reverse-check и
  diff-check PASS. Existing root reconfigure PASS: raw log
  `build/native-integration-notices-v2-configure.log`, SHA256
  `8009008606915e8989dead8797d898495a13301a22e22262fd656f80d223ffa7`.
  Metadata CTest 1/1 PASS (layout/relocation, build-tree discovery и отказы при
  missing notice/directory вместо файла): raw log
  `build/native-integration-notices-v2-metadata-ctest.log`, SHA256
  `6d597310f58213dbabb673858152e3c0a2bee9c2c616dca0ba3c64171b2d150e`.
  Повтор structure gates: 4/4 VM structure tests и полный обход 2366
  first-party source files PASS, raw log
  `build/native-integration-notices-v2-structure.log`, SHA256
  `9516cd05eaf335a387473f6617d16677f8f69939f8c735eb198a02ef1e590d6e`.
  Это OC OFF configuration без компиляции native-кода; Linux OC ON install
  и product directory redistribution остаются непроверенными. Installed
  prefixes потребителей не изменены.

Открытые пункты не превращаются в заглушки, пропущенные тесты или готовый
public API. Выход из phase 0 — согласованный контракт и проверенный
compiler/consumer срез с явно ограниченной областью доказанного поведения.

## Остановка по запросу пользователя: signal v3 ещё не принят

Работа остановлена на pending-changes без commit, push, merge или запуска CI.
Проверяемый объединённый срез остаётся в runtime worktree и не перенесён
в Forge integration checkout:

- raw signal owner: `build/signal-owner-v3-source.patch`, SHA256
  `936e460cfd8d93186d97c288a82b4f31bc2ff52d167ee19c90ce622eb8e2d21c`;
- FP/host wiring в compiler worktree:
  `artifacts/oc-phase0/build/fp-environment-v3-source.diff`, SHA256
  `8803162832958d80ff886511cb662491d4d482ae9d2065eb3811e58ad1675ada`;
- combined pre-build source snapshot в runtime worktree:
  `build/signal-owner-v3-prebuild.md`, SHA256
  `6938341dbba508eae838e10ee06e1d642eee41c5a6fab54ab4ef50c636607578`.

Existing macOS ARM64 graph собрал internal/native-signals/unit targets с
`-j4`: exit 0. Build log `build/signal-owner-v3-build.log`, SHA256
`8177477beac7210a386fefea5bbee7359a82f3743e8883cbdf528aa8b8685b04`.
Internal CTest 1/1 PASS. Native-signals CTest завершился тайм-аутом 30 секунд;
выборочные 12 cases без concurrent case не заменяют этот отказ. FP hardware
fatal probe давал разные результаты из-за предположения о конкретном сигнале:
последующий выборочный 7-case PASS без изменения исходников не отменяет
предыдущие отказы выбранных тестов и полного unit suite.
Полный unit suite: 195 случаев, один FAIL именно по hardware signal oracle.

Независимое ревью оставило три конкретных замечания:

1. FP guard устанавливается после root argument conversion. Пользовательский
   converter может изменить SoftFloat TLS или бросить исключение до сохранения
   исходного состояния. Нужны guard на входе, повторная canonicalization перед
   guest execution и normal/throwing conversion tests.
2. Hardware fatal probe ошибочно требует только SIGSEGV: Darwin может доставить
   SIGBUS. Следует сбросить обе dispositions и разделить hardware fault oracle
   от точного программно посланного сигнала.
3. NULL-mode direct callback не блокирует SIGPROF. Минимальная доработка остаётся
   в существующем raw suspension, с caller-owned outcome и обычным rethrow
   только при NULL recovery.

Дополнительно не доказана полная безопасность direct interpreter C++ unwind:
ordinary throw начинается при active checkpoint, NULL публикуется в catch.
Guest-reachable corruption не установлена; blanket full-EH safety claim
недопустим. Concurrent native test по-прежнему не принят: debugger подтвердил
вход в Forge handler с SIGBUS/code1 и PC native fixture, но TLS ranges и
ветвь классификации ещё не получены. Linux x86-64/JIT/OC, syscall-failure и
полные transition gates этим срезом не подтверждены.

Точка продолжения: сначала исправить и проверить три замечания, локализовать
concurrent timeout и завершить lifetime review; только затем интегрировать
signals/FP и продолжать полный compiler/runtime/Spine OC pipeline.

Оба разработчика и reviewer подтвердили остановку своих дочерних процессов.
Runtime handoff: `build/signal-owner-v3-stop-handoff.md`, SHA256
`b3b1097e538445ef55a061f755491283526fdb1aa9e0fee6676444b671868844`.
Compiler handoff в его worktree:
`artifacts/oc-phase0/build/compiler-safe-stop-2026-10-13.md`, SHA256
`2c8a710b9f697fe80c16b30351be75e55f3bd1b89e1da39d57a45529c73eb8d5`.
CFG остаётся незавершённым, восемь pending source files не собирались.
В compiler checkout новый signal interface ещё не синхронизирован с парной
реализацией; перед его сборкой обязательно перенести согласованный owner.
Три remediation после freeze не применялись. Исходники и артефакты сохранены.

## Возобновление 2026-10-13

Пользователь возобновил работу тем же составом: координатор, два разработчика
и независимый reviewer GPT-6.1 Sol xHigh. Существующие сессии восстановлены
через compact handoff, без передачи полного чата. Первый шаг — три замечания
выше и локализация concurrent native timeout; ни одно из них пока не закрыто.
Сборки сериализованы, `-j4`, с повторным использованием существующих graphs.

Свежий read-only baseline check:

- Forge `origin/dev`: `03d4c43b900e2f44be4c4102fa30fa047bef4a81`;
  исходный `94714f0ff465435ef317ec1891d44b1e9380c01e` является его предком.
- Spine remote `dev` остаётся
  `1cdc9ab9294996f95c20c91f0810d999d3c82a7f`.
- Рабочие HEAD и Forge gitlink Spine не изменены. Dirty trees не перебазированы.

Новый Forge dev убирает отдельные PubSub/diagnostics plugins и descriptor-only
modules; соответствующие API принадлежат node, фабрика — `plugin::descriptor`.
Spine пока использует прежние package components/imports/factories. Перед
финальным pin требуется отдельная адаптация потребителей и повторная проверка
package graph. Это не разрешение менять сетевые алгоритмы и не причина
смешивать текущий signal regression baseline с обновлением P2P.

Ранее разрешённый пользователем один целевой ручной Linux x86-64 CI-прогон
остаётся отложенным до локальных проверок и review; он не запускался при
возобновлении. Нативная OC-приёмка, полный compiler/runtime и интеграция
SpineOS по-прежнему не завершены.

### Проверки трёх исправлений после возобновления

Оба узких diff получили независимое source review без reportable findings:

- compiler FP delta поверх `880316…`: `5d42fa823357ab462cff732edf122e2bb34cfa6fb5024c5691965cfc0e3e5fc4`;
- NULL-mode signal delta поверх `936e46…`: `fb81171289d7894a74964cf2246a53e19a5022ae258d43101402abc006aa47d5`.

Точные исправления объединены только в runtime worktree. Pre-build snapshot:
`build/signal-p2-combined-prebuild.md`, SHA256
`50b701c54db218db8d8db7c6e85c4f1bf6fb42bd86ca6bf35c742653ac3bb2ad`.
Повторная сборка unit/internal/native targets с `-j4` завершилась exit 0.
Raw build: `build/signal-p2-combined-build.log`, SHA256
`600a435d0ca830e849a2261748f954bf6930bff7ae7b60368a499d8249426819`.

- Unit: 197 cases PASS, 3.12 s.
- Internal: 19 cases PASS, 0.39 s.
- Native-signals: весь CTest снова TIMEOUT, 30.02 s. Общий прогон exit 8.
- Отдельные actual-binary FP 9 cases и NULL-mode 2 cases PASS.

Raw CTest `build/signal-p2-combined-ctest.log`, SHA256
`5036c7d3c99e517b585c4fdf12deb4fdddb7440082d626a88a28da1cf643f71e`;
полный `build/signal-p2-combined-LastTest.log`, SHA256
`476406e5446957f15f44133e03050ad619d0962f3aae2afe0f48837b22418d01`.
Координатор сверил эти артефакты. Они подтверждают узкие исправления, но не
закрывают native concurrency, transition/failure injection или Linux OC.
Signal/FP owner пока не интегрирован в Forge integration checkout.

Reviewer отдельно проверил обычный interpreter EH (раскрутку стека при
исключении): конкретного guest-reachable must-fix для разобранных штатных
typed traps не установлено. Это ограниченное обоснование, не generic C++ EH
safety. Private guest callbacks остаются ограниченными; произвольный C++ host
callback, его conversions и cleanup должны целиком выполняться при NULL
recovery. Регрессия сигнала точно во время EH этим выводом не подменяется.

Reviewer независимо сверил точные source/build/test hashes и закрыл все три
P2 замечания выше. Общий owner acceptance остаётся HOLD. Конечный список
оставшихся signal gates зафиксирован без новых product fault-injection API:

1. Recoverable syscall failures: входные маски, query/install altstack и
   каждый install managed handler, включая частичный install. Проверить
   stage/errno, невход в callback, восстановление mask/altstack/dispositions/
   TLS ranges/outcome и успешный следующий вызов.
2. Restore failures в отдельном child: оставшиеся безопасные попытки
   восстановления и обязательный `_Exit`, без return/throw/atexit. Проверить
   block-exit, altstack, handler query/restore/release и final mask.
3. Active legacy host→guest pending SIGPROF normal/throw; полные snapshots
   маски/TLS/outcome при normal, explicit и hardware exits. Имеющиеся native
   pending FPE и NULL-mode SIGPROF tests не заменяют эти сценарии.
4. Actual concurrent native test и bounded повтор, восстановление handlers
   последним владельцем; sync wrong-PC/wrong-address отрицательные случаи и
   owned arithmetic/code classification без catch-all.

Fault injection остаётся test-only linker/interposition механизмом внутри
существующих test owners. Native Linux/JIT/spec gates следуют отдельно;
проверки на macOS arm64 не объявляются их заменой.

### CFG v2: узкая интеграция после review

Проверенный compiler patch `compiler-cfg-source-v2.diff`, SHA256
`42b52c8eba4d79eb082489d21b0fb9d45374ab577a2e0eb53ccbc75ef72f90a6`,
механически перенесён в integration checkout. Координатор сверил SHA256 всех
восьми файлов с `compiler-cfg-manifest-v2.json`, reverse apply check и
`git diff --check`; все проверки PASS. Никакие signal/FP изменения этим
переносом не приняты.

Независимый reviewer не нашёл reportable findings в CFG v2. Production-код
совпадает с ранее проверенным v1; v2 исправляет dependent-template syntax
в тесте и уточняет пределы доказательства. Старый failed build не заменён
задним числом успешным результатом.

Compiler worktree, существующий auxiliary graph, `oc_compiler_tests -j4`:

- build exit 0, SHA256 лога
  `d43daf282c8b718ce52a75a58e5e27d53d58e70d00032ed671f8decfae10922f`;
- CTest 1/1, пять случаев PASS на macOS arm64, SHA256
  `d7c35d5efd8d286fbd8b12140b10f229ce948138f7694198210348e89bf6ac9f`;
- полный LastTest SHA256
  `4d24ed12d4f3b10702dc1de806865dda7c561e57cd63cc5998a88795bd034fa3`.

После переноса integration checkout прошёл:

```sh
python3 tests/structure/check_structure.py .
python3 tests/vm_wasm/verify_donor_sources.py --root . --check \
  --optimizing-donor /Users/vladimirtarnakin/.openclaw/workspace/Projects/Storlane/spring \
  --llvm-include /opt/homebrew/opt/llvm/include
```

Structure: 2368 first-party files; pinned Spring/LLVM provenance PASS.
Это codegen и source evidence, не native Linux acceptance. Выполнение
сериализованного и заново отображённого artifact, большой `br_table` с
реальными jump-table relocations и guarded executor пока NOT_RUN.
Следующий отдельный compiler slice — memory/global access с прежним ABI;
общий signal/OC verdict остаётся HOLD.

### Memory/global v2: узкая интеграция после review

Patch `compiler-memory-source-v2.diff`, SHA256
`490e1274ee04c53f29dd3a1bc0d208b4c3932b791a01ac2bab8a4cc5dbc6410c`,
интегрирован после независимого verdict no reportable findings. Координатор
сверил все шесть SHA256 с `compiler-memory-manifest-v2.json`
(`646f2a1f50594bfef3e796319377fde04c9441b56f0c6ac0605bb290d3cf5df8`),
forward/reverse apply checks и `git diff --check`.

Compiler owner: existing auxiliary build `-j4` PASS; CTest 1/1, восемь
macOS ARM64 codegen/interpreter-oracle cases PASS. Это не native OC execution:
пять Linux execution cases NOT_RUN, signal owner остаётся HOLD. Дополнительный
native gate должен читать впервые доступный диапазон после memory.grow,
а не только проверять перемещение base при чтении прежнего диапазона.
Pinned Spring/LLVM provenance повторно PASS на интегрированном дереве.

2026-10-13 пользователь расширил обязательную цель до macOS ARM64 и уточнил
donor-compatible tier-up/retry в SpineOS. Актуальные требования находятся в
основном плане и consumer integration note; прежние Linux-only и
next-invocation-only формулировки больше не являются acceptance scope.

### Darwin ARM64 target: проверенная узкая интеграция

После независимого SOURCE/EVIDENCE review без reportable findings перенесён
ровно 13-file patch `compiler-target-source-v1.diff`, SHA256
`31beaf36b069874cc4fd454aea42b58305f0e89e81a17d9c208220cee1f28b8c`.
Manifest `compiler-target-manifest-v1.json`, SHA256
`0922919490e0b1b6bc44b5b913d4270df0de54510b371e5c4436aff7fd9eb4f0`,
фиксирует preimages, итоговые исходники и ограничения проверки.
Координатор сверил все 13 preimages и итоговых SHA256, 11 журналов/артефактов,
forward/reverse apply check и `git diff --check`.

Добавлены explicit Darwin OS ABI в существующем target profile, AArch64
relocation validation/patching, проверка host OS до mapping, фиксированных
frame/alignment и public instruction-cache maintenance. Существующий
`native_image` остаётся единственным владельцем executable mapping;
нового loader/cache, MAP_JIT arena либо изменения invocation ABI нет.

В существующем compiler auxiliary graph финальная сборка двух targets с
`-j4` прошла без предупреждений. CTest 2/2 PASS, 1.33 s: 10 artifact cases,
13 compiler cases, включая три группы actual native nonfault execution на
macOS ARM64. Проверены integer/direct-call/control-flow/memory/global
примитивы, dense 16-entry branch table и разные mapping addresses исходного
и сериализованного/декодированного artifact. Это уже не только cross-codegen,
но всё ещё не полноценный guarded executor.

Exact logs в compiler worktree `artifacts/oc-phase0/build/`:

| Артефакт | SHA256 |
|---|---|
| `compiler-target-v3-build.log` | `827070a3b97c3192d791a64587cdc6d6bed0f876554ce7c0405975ce6cf47403` |
| `compiler-target-v3-ctest.log` | `d4c785073af29fe54fa2ac2eea09108a50786ac1bfc2fa0af584f37d245dd5aa` |
| `compiler-target-v3-LastTest.log` | `0053702f452002cab67f74e3ccd2118ef8596cd8aa9290965d8f87d715ab1106` |

NOT_RUN/неготово: Linux native, Mach fault plane, guarded aggregate stack и
host-stack bounce, полный numeric/import/indirect runtime, изолированный
compiler session, persistent cache, root package graph, hardened distribution,
SpineOS tier-up/retry и performance matrix. Root CMake Linux-only guard ещё
требует отдельной согласованной platform/package доработки. Общая приёмка OC
остаётся HOLD; commit/push/merge/CI не выполнялись.

После интеграции повторно PASS: Structure (2368 first-party source files),
pinned Spring/LLVM и пять Wasmtime source hashes. Platform-donor argument
должен указывать на подкаталог точного SHA внутри bounded donor snapshot,
не на его родительский каталог; первоначальный вызов с родителем ошибочно
сообщил missing/hash mismatch и был повторён с правильным путём. Исходники
донора, expected hashes и проверяющий скрипт для этого не менялись.

### Общая numeric операция: source review принят, интеграция отложена

Независимый reviewer не нашёл reportable findings в exact 7-file
`numeric-owner-source-v1.diff`, SHA256
`f30f9a37170dd3d0f292b2b7f066b71763b346ec1a42c58a9c06be7bffb0ae6c`.
Manifest `numeric-owner-manifest-v1.json`, SHA256
`2f005756ce513b9af0dd711c9d86b17ac6de57bae3aee433d9dedeb4dc0d215e`.
Координатор отдельно сверил семь файлов, семь prerequisites и четыре журнала.
Этот патч ещё НЕ перенесён в integration checkout: signal/FP prerequisites
остаются HOLD, четыре tracked preimages отличаются из-за неинтегрированных
предшествующих изменений. Whole-file copy поверх них запрещён; после принятия
владельца зависимостей нужны точные hunks и совместная повторная проверка.

`interpret::execute_numeric` вызывает те же overloads `interpret_visitor`
для 62 FP/comparison/conversion операций. Арифметика SoftFloat не копируется.
Проверка арности и поддержанного opcode выполняется до доступа к слотам;
ошибки преобразования сохраняют существующую типизированную категорию.
Владелец численной операции не выполняет stack switch или signal recovery.

Existing auxiliary build `-j4` без предупреждений; CTest 3/3 PASS, 1.85 s:
4 numeric, 10 artifact и 13 compiler cases. Numeric tests сравнивают raw
результаты с настоящим интерпретатором, проверяют граничные значения,
типизированные отказы и восстановление четырёх SoftFloat TLS полей после
успеха/ошибки через existing NULL-recovery callback.

Это не проверка compiled FP lowering, production host-stack bridge,
совместной Mach recovery, native Linux или installed package. Actual
optimizing callback и совместная интеграция остаются NOT_RUN. Review узкого
патча не снимает общий HOLD и не является разрешением merge.

### FP lowering: следующий замороженный compiler slice

Независимое source/evidence review exact patch
`compiler-fp-lowering-source-v1.diff`, SHA256
`e1f126d170b3f7d434ba0ed66d72b1fd3f97c49c783d43c5118f2e4ca87f676a`,
завершилось без reportable findings. Manifest SHA256
`2d9abd8766e3c5f1f821f8d9f709b522f8c610287e0726352afd773363d34a0b`
фиксирует семь pre/postimages и шесть журналов; reviewer восстановил
postimages в памяти, независимо от последующих import/indirect edits.

62 операции вызывают shared numeric owner, f32/f64 передаются как raw bits,
LLVM FP arithmetic не вводится. Existing auxiliary CTest 3/3 PASS, 1.56 s:
10 artifact, 15 compiler и 4 numeric cases; final `-j4` build без предупреждений.
Проверены оба target profiles и actual native Darwin direct/relay primitives.
Первоначальная ошибка компиляции теста сохранена в отдельном v1 build log;
успешный v2 не подменяет его.

Source coverage 171/172 MVP opcodes не означает готовность полных модулей:
imports/indirect ещё вне этого slice. Guest→host stack bounce, actual Mach
NULL boundary, native returned-trap path, Linux/package/performance не
проверены. Патч пока НЕ интегрирован: он зависит от numeric owner и held
signal/FP prerequisites. Общий integration verdict остаётся HOLD.

### Imports / indirect calls: замороженный compiler slice

Exact patch `compiler-import-indirect-source-v1.diff`, SHA256
`7d43e59f02c8bfd9cba42426043f79339c8cdcb641c9081d20fb579c332646d4`,
manifest SHA256
`a9e45589a88977e0309150d5d600be869fce6912d0108c83b8e42ed70376cc42`.
Координатор сверил восемь source/preimage pairs, четырнадцать prerequisites
и восемь журналов. Reviewer независимо восстановил postimages из frozen
baseline и проверил forward apply. Узкий source/evidence review завершён
без новых reportable findings; рекомендация интеграции — hold_for_evidence.

Используются существующие `type_aliases/fast_functions` и import signatures;
второй реестр типов не добавлен. Проверки индекса, пустого слота, типа и
native entry предшествуют соответствующим dereference/call. Ошибка host
callback обрабатывается до чтения результата. Invocation ABI и poll не менялись.

Existing auxiliary build `oc_compiler_tests -j4` — PASS без предупреждений;
CTest 3/3 PASS за 3.99 s: artifact 10, compiler 18, numeric 4. Девять реальных
Darwin fresh-exec negative controls проверяют typed exit и отсутствие host
effects; сигнал, timeout, неверный exit или обычный возврат считаются отказом.
Positive cases проверяют canonical ordinal, raw FP bits, mixed imported/defined
targets и восстановление depth/frame. Shared test helper SHA256
`357106df9ff0e5ce6937f6081017d9c78d23b8aec5d9c89c69ee10bdaf6de6c7`
принадлежит runtime slice, не скопирован как отдельная реализация compiler.

Source lowering 172/172 MVP opcodes не доказывает готовность executor.
Это обычные native host-stack primitives, без production host registry,
guest/host stack bounce и нового Mach owner. Native Linux, installed package,
session/cache/restart и performance остаются NOT_RUN. Патч НЕ интегрирован;
signal/FP prerequisites всё ещё HOLD.

Параллельный первый полный Mach-owning CTest дал 1/3 PASS: native-signals
прошёл, unit выявил неверный suite filter дочернего теста, internal завершился
SIGSEGV. Crash report PID 41559 показывает default forwarding self-raised
SIGSEGV в donor `signals_tests_line_23`, не аппаратный memory fault.
Последующий live LLDB capture локализовал отказ: при активном private legacy
destination и обоих пустых ranges обычный `raise(SIGSEGV)` пришёл с
`si_code=2`; новый Darwin `synchronous_fault` filter очистил destination и
передал сигнал default handler. Capture SHA256
`43d1c625ca0be11b85e49940e3b688895b65f69e459919dfebbb089faf44eb10`
относится к binary
`5189b48a70d576c7f10a998db89c94db9fad834ba47add0a32c896c613a8f3cb`
и source `609238022588c682f91f20ea54a4d77488e2cfc8402931d598450aeb90466099`;
команды сохранены runtime owner. Успешные повторы не отменяют этот отказ.

Проверен исходный contract `94714f0` (`signals.cpp:194–210`): private legacy
both-empty path прежде имел catchall, без `si_code` discriminator. Совпадающий
с установленной ОС XNU `bbb1b6f9e71b8cdde6e5cd6f4841f207dee3d828`,
`bsd/dev/arm/unix_signal.c:439–518`, также не даёт здесь надёжного признака
происхождения сигнала. Rejected-Mach latch не принят: внешний task handler
может восстановить поток без POSIX delivery и оставить stale marker.

Координатор разрешил восстановить только прежний private both-empty catchall
с active destination. Public native и production legacy nonempty Darwin
POSIX paths всегда forward; actual guest hardware recovery принадлежит Mach.
Whole-NULL C++ host boundary остаётся fatal. Это не расширение публичной
границы recovery. После узкого исправления existing `signal-boundary` graph
пересобран `-j4`, без предупреждений. Полный CTest — 3/3 PASS за 2.35 s
(unit 197 cases, internal 24, native-signals 18), затем ограниченный
`until-fail:3` прошёл все девять suite executions. Raw полный CTest SHA256:
`caafbda97114c79906c0dee38089e2fc3af1955115fb17e1298d72fe12ecb239`.
Это исправление доказанного prefilter отказа, не попытка выдать случайный
успешный повтор за объяснение причины. Exact frozen source ещё проходит
независимое review; integration, полный Mach forwarding/RPC и production
executor остаются непроверенными. macOS OC целиком остаётся HOLD.

### Prospective cache accounting и локальная интеграция compiler slices

Независимый reviewer не установил новых reportable findings в exact
`artifact-budget-source-v1.diff`, SHA256
`a3bf6b2645c4202b35eef2733135ed3a9a74039f65af59111ee8897bd0068a70`;
manifest SHA256
`a45aa6aea8c20b944816890058c6f77bafc53cff5e5f1f8d8bba439589a2d30a`.
Координатор независимо проверил 30 source/preimage/prerequisite/log hashes
и patch hash. Existing artifact/compiler targets пересобраны `-j4` без
предупреждений; CTest 3/3 PASS за 2.18 s (artifact 13, compiler 18, numeric 4).

`prepared_byte_size()` использует тот же native-image planner, включая полный
alignment reserve и exact-sized native function array. После успешного
`prepare()` значение совпадает с `byte_size()`. Getter не отображает код,
но может выделять временную память: его место — worker compute boundary,
не `ready/enqueue` или DB writer. `is_unique()` лишь advisory shared-pin
snapshot при внешнем исключении конкурирующего доступа к индексу; lifetime
по-прежнему принадлежит shared handles. Это не проверка session enforcement,
peak RSS, Linux или production throughput.

Reviewer явно разделил разрешение механической pending-интеграции для
совместных проверок и общий merge/release verdict HOLD. В integration checkout
перенесены только ранее проверенные exact slices: FP `880316…` + `5d42fa…`,
numeric `f30f9a…`, FP lowering `e1f126…`, import/indirect `7d43e5…`, accounting
`a3bf6b…`. Все hash/forward/reverse checks соответствующих frozen patches
прошли. Итоговые 22 из 23 файлов совпадают с последними manifests; единственное
отличие `backend.cppm` — ещё не применённый signal export из Mach patch.
Полный Mach patch `3b299247…` проходит forward check, но до его независимого
review не применяется. Combined build этого дерева ещё NOT_RUN. Коммиты,
публикация, CI и изменение Spine Forge pin не выполнялись. Проверка
`python3 tests/structure/check_structure.py .` после этой механической
интеграции прошла: 2373 first-party source files. Первоначальный вызов без
обязательного аргумента repository root завершился usage/exit 2 и не был
проверкой дерева; он повторён с корректным аргументом.

### Mach first-slice review: найден конкретный blocker перед интеграцией

Reviewer обнаружил дефект в frozen patch `3b299247…`: Darwin POSIX forwarding
обнуляет локальный `destination`, но оставляет TLS `signal_dest` активным при
вызове предыдущего simple/detailed handler. Если такой foreign C++ handler
обращается к недоступной guest memory внутри legacy nonempty scope, Mach
может классифицировать fault по адресу и выполнить longjmp через его живые
C++ объекты. Прошедшие общие тесты этот путь не покрывали.

Патч Mach в integration checkout НЕ переносился. Ответственному разработчику
возвращено узкое исправление: использовать существующего владельца NULL
recovery на весь foreign callback и cleanup, добавить fresh-process отрицательный
hardware control и проверку normal-return restoration, повторить три owning
группы. Это конкретный source blocker, отдельно от открытых общих platform
gates; новый recovery owner или framework для него не разрешены.

### Mach v2: finding закрыт, combined local gate выполнен

Независимое повторное review разрешило pending-интеграцию exact полного patch
`0808670f07ba0bdee51a559f7073d6c404325fa7465c9ca803784b4e6fe3ad8f`,
manifest `ca0a3bab2b2e6d8ce88d525360fd6b924835fe31f089071328825a6c910278a5`.
Функциональная delta `29c7a7c5…` затрагивает только `signals.cpp` и внутренние
regression tests. Внешние callbacks полностью проходят существующий NULL
recovery bridge; native unexpected throw остаётся fatal C++ defect. Два
неуспешных промежуточных test-oracle запуска сохранены и объяснены в manifest;
публичный TLS/query API ради проверки не добавлялся.

Координатор проверил 14 frozen source hashes и прямую/обратную применимость,
затем перенёс patch через механические hunks. После передачи ранее проверенного
numeric owner runtime checkout содержит ту же реализацию `execute_numeric`.
12 из 14 полных Mach файлов byte-identical между runtime и integration; два
CMake различаются только ранее принятыми `Boost::headers` и регистрацией
compiler/artifact tests. Исходники signals/Mach/FP не переписывались вручную.

На существующем integration `build/native-integration`, LLVM 22.1.8 / Release,
выполнены:

```sh
cmake --build build/native-integration --target \
  test_forge_vm_wasm_interpret_unit \
  test_forge_vm_wasm_interpret_internal \
  test_forge_vm_wasm_interpret_native_signals -j 4
ulimit -c 0
ctest --test-dir build/native-integration \
  -R '^test_forge_vm_wasm_interpret_(unit|internal|native_signals)$' --output-on-failure
```

Build EXIT0; CTest EXIT0, 3/3 PASS за 2.77 s: unit 201 (197 + 4 numeric),
internal 27, native-signals 18. Build содержит предупреждения неизменённых
донорских тестов (unused/pragma/loop unroll), поэтому warning-free не заявляется.
Raw evidence в `build/`:

| Файл | SHA256 |
|---|---|
| `oc-combined-mach-fp-build-v1.log` | `43480ac15554173581b58b5b903c2946cb1de40d0a902d0a971eecd78b391086` |
| `oc-combined-mach-fp-ctest-v1.log` | `a1fc21d01729c8ef318f299ebc2c0472af16765068bec60de6a0ee73dd3aac02` |
| `oc-combined-mach-fp-LastTest-v1.log` | `937bc19de0fe8023286b0e237b78dfa6c4669b2dd5950d8bb3f832af34dc0c79` |

Combined Structure пока FAIL: старый blanket ban `interpret/details` конфликтует
с принятой парой private Mach recovery. Владельцу возвращено точное исправление
checker с positive/negative тестами, без отключения gate или ослабления
private install/public include boundary. Остальные ранее перечисленные platform,
executor/session/cache, package, Spine и performance gates остаются OPEN/NOT_RUN.
OC option в этом root graph всё ещё OFF; этот combined regression не выдаётся за
полную optimizing сборку. Коммитов, push, CI и изменения Forge pin Spine нет.

### Structure gate после Mach-интеграции закрыт

После двух review-remediation итераций reviewer разрешил exact patch
`91142c6602f565acb4fe158992b34b7735da29ea1040c7aa2c981d4d80d48f46`.
Он убирает blanket ban на сам каталог `details`, сохраняя общие layout/pairing
проверки и запрет публикации private headers. Отдельные отрицательные fixtures
покрывают установку всего component root и PUBLIC/INTERFACE
`BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}`; разрешённый `include/forge`
и PRIVATE root остаются допустимыми. Это ограниченная лексическая проверка
обычных CMake forms, не доказательство произвольного раскрытия переменных.

Координатор проверил SHA и preimages, механически перенёс два файла и повторил
проверки уже на integration tree: 8/8 regression methods PASS; полный Structure
PASS, 2377 first-party files; reverse patch check и `git diff --check` PASS.
Raw log: `build/oc-structure-private-boundary-v3.log`, SHA256
`53724bea569e8cf11bed8b7aa5920368150bf708c5d6a87570c7eda39d918f1f`.
Предыдущие v1/v2 patches остаются отклонёнными, не альтернативными решениями.

Свежая read-only сверка remote refs: Forge dev
`411644237490a63a52d1f810336673367c962651`, Spine dev
`1cdc9ab9294996f95c20c91f0810d999d3c82a7f`. Рабочий Forge baseline пока
`94714f0ff465435ef317ec1891d44b1e9380c01e`: frozen slices не перебазированы
во время review. Перед доставкой обязательны согласование обновлённых
AGENTS/skills, root CMake, package config и Structure с новым dev и повтор
итоговых gates. Новое правило завершающего metadata-блока применяется
отдельной механической delta без изменения wire field order.

### ABI/codegen 2: epoch compiler slice интегрирован, executor пока не принят

Reviewer разрешил узкую pending-интеграцию compiler patch
`3c2ba13dd2dacab12ba6901ce7f7f237c175cfadff2b20e7b8384ddaa4322ff6`,
manifest `62be7b608939835b0fb7a4696450a3803a6192ff5cd9af72b17632e0193c11db`.
Координатор независимо сверил 75 хешей source/preimages/prerequisites/logs/donors,
CW reverse check и FW forward check. В FW механически перенесены 13 product,
test и documentation файлов. Auxiliary artifact-consumer CMake не переносился;
незавершённые helper/protocol snapshots не считаются принятыми вместе с этим
срезом. FW reverse check и `git diff --check` проходят.

12 из 13 полных файлов совпадают с manifest. Единственное отличие общего
`tests/vm_wasm/CMakeLists.txt` — сохранённая ранее принятая регистрация
native-signals tests из Mach slice; integrated SHA256:
`a92f22910d0604d0265a3b933855f0faf1d3c05d1d071361855deae5289c4f97`.
Reviewer отдельно подтвердил, что этот 16-line block byte-identical принятому
runtime slice. Повторный Structure на интегрированном ABI2 дереве прошёл:
2378 first-party files. Это source gate без компиляции.
Новый compiler ABI добавляет epoch/deadline/sticky pointers и один acquire
epoch load на входе и backedge. Старые и смешанные ABI/codegen profiles
отвергаются существующей проверкой artifact до публикации executable mapping.

В CW final build и CTest прошли (3/3, artifact 14 / compiler 22 / numeric 4,
2.96 s). Реальный infinite-loop stop-callback oracle находится в owning module;
искусственный вызов ради эмиссии символа стандартной библиотеки удалён.
Проверка использует `_Exit` и доказывает только выход из generated loop,
не unregister/destruction, compute shutdown или Mach/host bridge. Отдельная
проблема ordinary imported `std::stop_callback` linkage остаётся открытым
installed-consumer gate. Combined ABI2 runtime build в FW ещё NOT_RUN.

Сборки этой задачи удерживаются до завершения параллельных сборок других задач
по решению пользователя из-за нехватки места. Исходники и review продолжаются;
чужие сборки, сохранённая сборка Spine и данные не удаляются. Real executor,
session/helper/cache, обе платформы, SpineOS tier-up и donor performance
comparison остаются незавершёнными отдельными gates, не покрытыми этим PASS.

### Уточнение macOS scope после donor preflight

Пользователь подтвердил назначение работы: macOS-порт EOS-VM-OC в Forge;
Wasmtime используется только как platform donor. Общий proxy для чужих
THREAD exception handlers не входит в поставку. Принята текущая явная
граница: непустой прежний per-thread route точно сохраняется/восстанавливается,
затем возвращается `execution_unavailable` до guest entry. Это не перенос
всего Wasmtime и не отказ от обычного macOS ARM64 исполнения. Исторические
упоминания полного forwarding выше больше не являются gate этого порта.

Независимый source preflight подтвердил, что pinned Wasmtime тоже не содержит
готового displaced-THREAD forwarding. Самостоятельный proxy не смог бы
сохранить authenticated kernel sender для произвольного чужого handler.
Остаются обязательными собственные RPC identity/malformed-message controls,
queued receive/retire/re-registration и OS-failure cleanup, реальный executor
с host cleanup, relocated runtime-only consumer и macOS performance.

### Compiler fixtures и первый integrated executor source

Test-only exporter patch
`5e6604022ae10692497453a0eb612f208b6f3703b3536c635eab24abb42d41e3`
и final manifest
`6ec88d6500f824aecac7c4bab372985e0eb87995764f2f78fd8dbae6559d1ca6`
получили narrow CLEAR. Координатор сверил 76 хешей, reviewer — 78 с двумя
предыдущими ABI2 references. Перенесены только три test files, их postimages
совпали; auxiliary CMake и compile-only helper/protocol prerequisites не
переносились. Exporters используют прежние семь fixture factories, настоящий
public compiler и Raw/identity checks: получено 14 request/artifact pairs,
по семь на Darwin ARM64 и Linux x86-64. Оба выбранных exporter cases прошли,
обычный CTest 3/3 PASS за 1.53 s. Это cross-codegen на Darwin, не Linux execution.
Артефакты занимают 22481 serialized bytes; 112 KiB — выделенные блоки диска.

RW runtime source patch
`69a4deaead78a1756c3d67246243ed73aae7b8db3a5e8534cb111dbb8153c178`
принят только вместе с remediation
`477654afa0c5e6b1b79fadd0d3a56d5f0a7e3dddd6aab6ab9a91528a49f1ed8c`.
Первый review выявил две ошибки: локальная нехватка памяти до guest entry
выходила как allocation/bad_alloc вместо operational execution_unavailable;
raw argument conversion читала unchecked variant до проверки active type.
Remediation ограничивает allocation catches подготовительными стадиями и
использует existing is_a для четырёх scalar types. Host exceptions и нормальный
memory.grow failure не переклассифицируются. Reviewer проверил exact v2 и не
нашёл source blockers для pending integration/следующих тестов; это не release
или merge approval.

Координатор механически интегрировал v1+v2. 12/13 полных postimages совпали;
единственное отличие tests/vm_wasm/CMakeLists.txt — сохранённый ранее принятый
27-line artifact/compiler/donor-gate block. Integrated CMake SHA256:
`769facfe4e6d6170587c76390d3174cabc7a8c83afd4486c74ab2defad50591c`.
Structure PASS, 2389 first-party files; git diff --check PASS. Owning runtime
12 epoch +13 executor cases ещё NOT_RUN. Успешный экспорт compiler fixtures
не закрывает реальный compiler→executor, measured stack reserve, Mach RPC/
lifetime, installed consumers, session/helper/cache, SpineOS или performance.

### Cross-slice blocker: local native stack против canonical call depth

После pending integration координатор нашёл, а reviewer подтвердил отдельную
ошибку классификации: generated prologue возвращает один stack_overflow и при
достижении canonical max_call_depth, и при нехватке stack_remaining_bytes.
Последнее значение зависит от LOCAL max_guest_stack_bytes и paused outer SP.
Проверки, гарантирующей достаточность local capacity для всех допустимых call
paths, нет. Конечный call path ниже canonical depth поэтому может завершаться
успешно на большем local stack и выглядеть objective guest failure на меньшем.
Owned physical stack guard также ошибочно отображается в ту же guest reason.
Это новый blocker, не закрытый v2 allocation/argument remediation.

Согласован следующий узкий контракт, ещё не исполненная проверка: добавить
raw native_stack_exhausted в конец существующего trap_reason, не сдвигая старые
значения; оставить canonical depth как stack_overflow. Runtime после полного
raw/host cleanup переводит новую причину и OWNED physical stack fault в
execution_unavailable. Чужие/C++ host faults и настоящий depth limit не
переклассифицируются. Layout/host ABI остаётся 2; codegen становится 3, старый
codegen 2 явно отклоняется до mapping и не смешивается с новым cache identity.
Compiler/profile часть принадлежит CW, runtime mapping/контроль — RW.

Обязателен один finite call path с неизменной WASM/configuration: малый LOCAL
stack даёт operational unavailable, достаточный — исходный результат; отдельный
case доказывает сохранение canonical depth trap. Старые 14 codegen2 fixtures
сохраняются неизменными и не считаются проверкой будущей codegen3 реализации.

### Первый native runtime gate после test-only repair

В RW production executor/epoch/native-stack собрались; первоначальная сборка
остановилась в тестовой единице: неоднозначный signed/unsigned literal и попытка
Boost.Test печатать exception_ptr. Narrow test-only repair
`0efb3a4d75131c0e3d7aff8630c027fadf055834deb66ed937fd261bf8442108`
содержит пять diff hunks, шесть заменённых строк без изменения production кода.
Reviewer подтвердил прежнюю семантику null checks и wrong-operand negative.
Координатор перенёс exact patch в FW; postimage executor_tests.cpp:
`0d8e39a5943967d4598cb3a74afef20d3387e4420b979897dc2ce3740166a497`.

RW target build -j4 EXIT0, incremental log SHA256:
`177a757814adc14fb4791f6f22df48df67b4eea1366fdda24aea7997110f0751`.
CTest 2/2 PASS за 0.80 s; raw log содержит 12 epoch и 13 executor cases.
CTest log: `43419c4fa7c653cc6e5eac68bdc7fc12caffe2912f491ffb9d96976386505f7d`;
LastTest: `4e4542f38db5f4d0177154a8087621dcd0a0e95c6cd5997a0b407a59e0923eb0`.
Координатор и reviewer независимо сверили hashes и содержание результатов.
Это owning runtime fixtures на native macOS ARM64, не настоящий LLVM-output
через executor, не новая codegen3 taxonomy и не combined FW rebuild.
Новый stack blocker, измерение bridge reserve, Mach RPC/lifetime, installed
consumer, helper/session, Linux, SpineOS и performance остаются открытыми.

### Codegen3 source принят отдельно от runtime mapping

Reviewer дал narrow source CLEAR для frozen девятифайлового diff
`c6224e7c72cd36b3a104c8787f84171c10b8970d4591fa9be9a7925dbee16769`
и preflight manifest
`90974dd34f588c43d27fc2326ae02b7eba89532aeaa9b9145df62f454e197aab`.
79 проверенных hashes включают прежние 14 неизменных artifact/request pairs.
Координатор сверил 9/9 preimages, механически перенёс patch в FW и сверил
9/9 postimages. Три shared ABI/profile файла также перенесены в RW; его
executor mapping остаётся отдельным pending delta. Это source acceptance,
не PASS будущих native codegen3/runtime tests. ABI2/size168 сохраняются,
codegen3 отклоняет старый codegen2 до prepared publication. Ошибка локального
стека остаётся blocker до проверки обеих сторон границы.

### Codegen3 evidence и отдельный runtime source delta

Final compiler manifest
`608cfe665d88ff7e66514d21087176de3825fe665c0382b92c9d0ff0a9af5da0`
сохраняет source c6224e7c неизменным. Target build и numeric target check -j4
прошли. CTest 3/3 PASS за 1.38 s: 14 artifact /23 compiler /4 numeric cases.
Отдельные finite4controls, обе O2 target profiles и old-profile Raw rejection
прошли. В новом каталоге compiler-runtime-fixtures-codegen3-v1-outputs.qLKPbO
получены ровно 14 request/artifact pairs. Reviewer сверил 119 hashes и raw logs;
координатор отдельно сверил 41 hash binary/log/output/patch и CTest/LastTest.
Это native Darwin compiler primitives и cross-codegen, не Linux execution.

Runtime source patch
`06b25c384fa2d5057e41ddcf958e5b94825e382041761a8137685c3935d682f6`
с manifest `7f000f684d63f8614f69c419b6e769d452b425a15fb417778e119cf97c4151b9`
получил narrow CLEAR и перенесён в FW: все три postimages совпали.
Только уже классифицированный OWNED stack fault получает reason12;
перевод reason12 в execution_unavailable происходит после activation.finish.
Новые два controls пока NOT_RUN: ожидается 27 owning cases. Обращение к guard
в тесте доказывает classification/clean next invoke, не exhausted-SP recovery.
Тестовый loader настоящих LLVM fixtures проходит отдельный review; его сборка
и исполнение здесь не заявлены как успешные.

### Открытый invariant failing-entry reserve

Source audit выявил, что machine prologue предшествует IR frame guard.
Spring на pin e6a99f68 связывает max-frame cap16KiB, usable stack
canonical_depth*16KiB+4MiB и PROT_NONE guard16KiB. Forge использовал один
OS page guard (на этом Darwin host16KiB, на обычном Linux x86-64 часто4KiB)
и bridge reserve4096 без доказанного запаса на failing-entry frame.
Это подтверждённое отсутствие инварианта; emitted skip-over defect ещё
не доказан. Одни passing software-budget tests его не закрывают.

Следующая проверка использует SAME compiler IR/object output для first entry,
direct/indirect/import calls и terminal raw tail. Предпочтительная адаптация
донорского headroom pattern остаётся в SAME native_stack/activation owner:
validated max frame сверх logical allowance плюс измеренный raw gateway запас.
Не вводится отдельный stack engine или произвольный pre-call mechanism.
Настоящий finite small/big runtime control использует неизменные WASM/config
и реальные emitted frame metadata, учитывает OS rounding; tiny three-call
primitive не выдаётся за такой контроль. Старые frozen packets не меняются.

### Genuine compiler/executor gate: source integrated, execution pending

Loader patch `f53b3a4a3ef00c6e0b131d22c428c9bc0d6c20b134c7226b601d6028cb9e7b94`
и remediation `f18675f90b66dbeb86df7b0f005a87e71f2d4f7ff5a9d7b51f52d2c70bcca457`
прошли narrow review и перенесены в FW. Семь явных selected controls читают
настоящие checked request/artifact pairs через существующие Raw/OC owners;
LLVM не добавляется в runtime test dependency. No-tick cancellation oracle
проверяет выход guest до освобождения заблокированного IO callback, а не
устаревший флаг, снятый до bounded wait. Отсутствующий fixture — ошибка теста.
Этот selected gate ещё должен стать явной compiler-export → runtime CTest
зависимостью в общей приёмке; default-disabled suite не считается покрытием.

Первая сборка loader отклонила coroutine instantiation из-за отсутствующего
standard `<coroutine>` в test GMF. Единственный include добавлен patch
`0f715e4be2757a3d884b5f06c683ceb8e7c7f363c82cba15af28e80e45ec154b`;
reviewer проверил pre/post, без Asio/shim/flags изменений. Final loader postimage
`4c4231e5d817627c909bfef7e2e90678d798900dd1523c1339b2a4831f9d89b5`.
RW incremental target build EXIT0, log
`23736321c37f1be2deefc44f0f470593a6c19fc70be41c26c096b70711d4343a`.
На момент записи новые 27 owning и 7 genuine controls ещё NOT_RUN.

### Large finite stack evidence preparation

Test-only source `6845c8cce48a454a2a71bfaf15b55bb83e80420ac8fb903d3c89ab02738d7279`
и whitespace delta `2d28de69382486a4073a85e7abdedc7056c540778790536c11f435767e0733a5`
получили narrow source CLEAR и интегрированы с точными тремя postimages.
Linkage исправлен только в manifest v3
`5fbb163096b3b13ea035a3546f0df6213e642bce0aa1809dc979acea5ceb514e`;
старые ошибочные metadata остаются как superseded evidence.
Fixture строится прежним WASM builder и проходит тот же compiler/O2 path.
Три конечные функции с живыми аргументами должны дать реальные большие frames;
это проверяемое ожидание, не подстановка stack metadata. Canonical depth251
и frame cap16KiB не меняются. Сначала compile/object/prologue inspection без
native execution; малый/большой runtime stack — после принятия reserve invariant.

Wasmtime остаётся только platform donor. Generic forwarding чужих THREAD Mach
handlers не входит в задачу EOS-VM/OC; сохранение существующего обработчика и
явный cold refusal не заменяют обязательные identity/lifetime/recovery gates.

### Native Darwin codegen3 runtime results

Новый RW owning run: CTest2/2, 12 epoch +15 executor =27 cases PASS за0.98s.
Logs `0c79676aa9f58b6a9a0ed49017de5dfddf325265ca81ff0d9c97c83a1d949deb`
и LastTest `5b643f8267f04488131e512bad8de31228ebafab406b47469f67573329bb70f6`
проверены координатором и reviewer. Отдельный genuine compiler selected run
содержит все7 cases, EXIT0,44.6ms; raw log
`5ceb567f7e2aa37c8daefb5423763d621f56abc1e3be9412107cb0589ac34d6c`,
executor binary `0040c77b1b9f48bc0fe953326c25f6408bc6ce130a9a4938d486d3e971f11eb2`.
Итоговый RW evidence manifest
`35fdd238b247e47a14dcf32e573a9a61fe1b1e33bd734ecfc9f48fe2e53cd83e`
прошёл независимую сверку: координатор52 и reviewer54 hashes,0mismatch.
Exact argv/cwd/ENV связывают command с прежними28 input files; inherited ENV
и shared fresh-process helper связывают отдельный infinite child с Darwin input.
Этот run исполняет реальные Darwin LLVM fixtures, включая
fresh-process infinite loop, observed native PC и stop без работающего ticker.
Large-frame capacity, Mach adversarial messages, installed consumer, Linux,
helper/session, SpineOS и performance этим run не закрыты.

### Compiler session admission: existing scheduler owner defect

Read-only review выявил отдельный риск existing asio/task.cpp::submit_queued:
callback уже размещён в ready/delayed heap, затем schedule_drain/post может
бросить до возврата handle. Draft compiler_session catch очищает live record,
сохраняя quiescent=true, хотя queued callback удерживает raw this. Последующий
drain способен обратиться к уничтоженной session. Это не исправляется вторым
scheduler, polling/retry или имитацией гарантии admission в compiler leaf.

Назначен узкий fix существующего Asio owner: либо submit возвращает handle
ровно одного принятого задания, либо исключение означает, что задание не может
стартовать. Commit/rollback queue+post должен быть атомарен относительно drain;
callback уничтожается после unlock. До reviewed source и deterministic
failure-path regression эта ветка session не принята. Остальной Asio runtime
не перепроектируется; broad unrelated scheduler audit не входит в срез.

Первый owner delta `cb46c677ad58ab892784d432004f607ec16abddcbee854cd840c50f83ec77cc1`
получил REVISE до сборки. Public Boost associated-allocator diagnostic
`32e26db9b6f1963d1cf1f112f5417b18daaddaa6e11062dbd597b9460c48f06f`
воспроизвёл отказ второй allocation после strand enqueue: ни исходный, ни
следующий исправный handler не исполняются. Raw log
`95792a85f407829cb7488a98c4f65273c87888762a5d6b53fde755cb08109e89`
не является passing scheduler gate. Operator-new-only injection не покрывает
используемый Boost `aligned_alloc`. Согласован узкий следующий delta: weak-only
post в существующий runtime context, тот же mutex для queue/slot/timer state,
реальная aligned-allocation injection и concurrent ready/delayed/stop control.
Новый scheduler, подмена Boost internals и восстановление poisoned strand
не вводятся. Три добавленных unconditional scratch allocations также подлежат
устранению до принятия owner fix.

### Finite v2: compiler gate closed, runtime headroom still open

Test delta `779fec15b66bd45a0abf2ddabdfe7df00eaace49537ce6e3e586e47923ebeae2`
и final evidence `3a14dfd18f0b446d81851974e349922f54a6a4a35e362b42af78ddffbd586097`
приняты независимым reviewer: 123 hashes совпали. Координатор отдельно проверил
новые build/test logs, binaries и finite pairs. CTest 3/3 PASS за 4.91 s
(14 artifact, 24 compiler, 4 numeric cases). Конечные функции с 512/1024
аргументами скомпилированы для обеих платформ; корректный WASM с 2000 аргументами
отклонён именно typed compile_limit 4040004, а не как неверный WASM.
Лимит frame 16 KiB не увеличен. Сохранены 16 request/artifact pairs;
прежние 14 побайтно совпадают с принятым codegen3 output. Старый diagnostic
EXIT201 сохранён как неуспешный первоначальный выбор размера, не переписан в PASS.

Реальные frames для N=1024: Linux 16368/16368/8240 bytes, Darwin
14064/14064/8256 bytes. Дизассемблирование подтверждает выделение полного frame
и обращения к стеку до IR-проверки. Поэтому нужен запас на максимальный
проверенный failing-entry frame F плюс отдельный запас raw gateway G.
Большие функции пока не исполнялись. Донорские 4 MiB рассматриваются как
консервативный локальный резерв guest stack, не как доказанный предел libc,
не как требование к обычному pthread stack и не как новый расход на каждый
nested callback. Изменение reserve и native small/big controls остаются pending.
Отдельно нужен настоящий recursive compiler fixture для canonical-depth
exhaustion при достаточном локальном стеке. Нынешний reason8 classification
control не заменяет эту проверку; менять metadata готового artifact ради неё
нельзя.

### Native stack headroom: reviewed source and local execution gate

Source delta `1195c4b7b311fbd45019060719e6d91aeb795688fc5f392f1d5b272953cdec55`
получил независимый source CLEAR и механически интегрирован: все девять
postimages совпали. Существующий binding один раз вычисляет F из проверенных
descriptors; native_stack округляет usable budget вниз, оставляя внутри него
донорский запас G = 4 MiB. Условия entry/start проверяются до memory reset и
host effects. Размер guards не меньше поддержанного frame cap 16 KiB.
На неизменённом source выполнена warning-free сборка существующих targets
LLVM/Ninja с `-j 4`. Owning CTest: 2/2 PASS за 1.22 s, 12 epoch и 17 executor
cases. Отдельный selected suite: 9/9 PASS на настоящих Darwin compiler artifacts.
SAME finite artifact с N=1024 и frames 14064/14064/8256 bytes получает typed
`execution_unavailable` при G+48 KiB и возвращает 42 при G+64 KiB. Проверен
также отказ G+16 KiB до memory initialization и host effects.

Compact evidence `b7afcddb0e0ae77b75a75cef3bce8e0af297640f1ae82357369f73eebb381bf6`
в runtime worktree `build/runtime-stack-headroom-evidence-v1.md` связывает
точные source, 32 input files, команды, binaries и raw output. Reviewer
проверил новое доказательство и дал narrow CLEAR; координатор отдельно прочитал
selected log `d7cfcc6271dc59ec714aa926aa8777d4037890ae83e2b9da0984300bbae30267`
и сверил основные hashes. Это локальная проверка finite stack margin,
не benchmark. Actual canonical-depth exhaustion, genuine nested large frames,
exhausted-SP recovery, Linux, installed consumer и Spine integration остаются
отдельными незакрытыми проверками.

### Darwin compiler resource-limit diagnostic

Отдельный public-API control на Darwin 25.1.0 ARM64 подтвердил, что absolute
RLIMIT_AS применяется, а не игнорируется. Уже при старте небольшого test process
virtual_size составляет около 445746790400 bytes. Поэтому 512 MiB и 32 GiB
отвергаются setrlimit с EINVAL до компиляции. При явно заданном диагностическом
1 TiB limit маленький PROT_NONE mapping проходит, mapping на весь cap отклоняется
с ENOMEM; parent limits не изменены, физические страницы больших mappings не
затрагивались. Source `1906a73238f9dc04fc14bff52628ceb6e6c6c5d0468c43b764f3a1ff563c1ac1`,
raw log `9cbe64a9c57355fd084cb86c8c2330471cafa35019afa8b43ae39fe5a83b40f4`.

Это не измерение LLVM helper и не production default: 1 TiB нельзя выдавать за
маленький предел RSS. Существующий resource_limits требует явных значений;
512 MiB остаётся непроверенным примером проектируемой Spine configuration.
Настоящий helper baseline и практически пригодный ограниченный resource policy
нужно проверить до публикации рабочего примера. Заданный оператором cap не
увеличивается автоматически. Spring использует absolute RLIMIT_AS на Linux;
его unchecked setrlimit не переносится как образец обработки отказа.

### Asio admission: actual owning gate

Итоговый runtime source `d3ebb0da36aa45929036a6ab8d6de3dd68f311e53c599402085b62024a723682`
и test source `eab4f343ea285a791a059b1b884c62b9486b8d3bcbfeadc2542bcb46b91f1f2b`
собраны целевым LLVM/Ninja с `-j 4`. Первоначальный SDK declaration failure и
промежуточный warning сохранены; финальная сборка v4 warning-free, EXIT0.
Public declaration тестового aligned_alloc согласуется с реальным платформенным
exception specification; production allocator или hook не добавлен.

Owning CTest: 2/2 PASS за 2.11 s, фактически 71 прежний и 3 новых test cases.
Raw build log `481342732c6c9a386ce5d11b42f6a5aeeddac24f59230c0892754d51520895e0`,
CTest `257866da4fb65da42ab47325ff47e2366cc72f34bc7bc6feb4553b3517b2c95d`,
LastTest `d16da3fcb4ad3eda7c76ba81772a993e90b8ae24c94743a88fc9f1a5a8cc0e8c`.
Координатор сверил source, logs и оба binaries; reviewer отдельно прочитал
фактическое исполнение и обязательные assertions. Каждый из восьми admission
маршрутов требует реального post allocation failure kind2, нулевого исполнения
непринятого callback и однократного успешного последующего задания. Проверены
reentrant destruction и конкурентные ready/delayed/stop с четырьмя workers.
Это узкая гарантия синхронного admission, не обещание восстановления после любого
асинхронного OOM. Compiler session, headroom, Linux и installed consumer остаются
отдельными gates. Интеграционный Structure scan: 2391 files PASS.

Read-only ref refresh перед следующими этапами: Forge dev
`9eac8072575da0ccc1f1337aa456a1f0caecf25c`. В upstream нет изменений Asio task
от нашего baseline, но появились prepared-module validate и связанные VM tests,
а также package/Structure изменения. Они должны сохраниться при дальнейшей
интеграции и повторной проверке итогового дерева; текущие checkout/pins не менялись.

### Compiler session/helper: initial smoke and review blocker

Frozen source delta `3ca1439f8a2a46957760048030dc2e29c6aefeba141b33f4f49060d04b2d4604`
и manifest `a7207add2c57d9324913b4b82156e74571667209a9a3c0404e7e339c95c3b461`
первоначально получили REVISE; исправление и текущий статус приведены ниже.
Diagnostic graph собрал session и реальный
helper; прошли четыре Raw protocol cases, ранний выход helper, реальный обмен
с prepared result/cache и новой сессией в том же процессе, а также deadline
case. Перезапуск продукта через fresh exec пока не проверен. Семь smoke cases используют
явный диагностический AS cap 1 TiB и не подтверждают production memory policy,
root/package closure, Linux или concurrent stop/launch correctness.

Независимый reviewer обнаружил source-backed interleaving: публикация
`quiescent=false` перед PID/endpoint позволяет cleanup увидеть ещё отсутствующий
PID и снова установить quiescent. Затем уже запущенный child может оказаться
за границей заявленного clean shutdown. Дефект пока не воспроизведён отдельным
тестом; обычные smoke tests его не исключают. Исправление и детерминированная
проверка late spawn против истёкшего shutdown deadline переданы владельцу
существующего child lifecycle. Новая подсистема управления процессами не нужна.

Итог read-only review исходного frozen packet: REVISE из-за этого окна;
других конкретных source blockers в 28 файлах reviewer не установил.
Проверки количества запусков, остановки на границах handshake, неверного hint,
повреждения кэша, OOM, priority/saturation и resource calibration остаются OPEN.
Source review не подменяет эти тесты и не подтверждает ещё не собранную
интеграционную CMake/package projection.

Отдельный незакрытый пункт: memory/index limits не ограничивают накопление
`.foc` на диске. В начальном session packet отсутствует полный disk bytes/entries
budget с проверкой после рестарта. До его реализации нельзя заявлять bounded
persistent cache или завершённую интеграцию SpineOS.

### Ordinary consumer and canonical recursion fixture

Независимый reviewer принял ordinary consumer delta `500461ef837af735b125ea8bef605201a9e9223ecf72b8133fcc4eb9bac16488`
с test-only correction `0e92c8926633500b33d34b510208d3c0f0546e31d4598a6490e9d14e8ddb99c9`
и evidence `cde60938d1e94e280588567129d220a55e6bcc926c5d4cc4feb6ee7ba8c6411a`.
Обычный imported C++ TU собран и выполнен через normal public dependency link,
без LLVM, owning-test objects, whole-archive и std symbol shim. Проверены
stop_callback lifetime, add(7,9)=16, already-stopped cancellation и drain.
Первоначальная ошибка отсутствующего registry Cls сохранена; после применения
существующего standalone_function_t сборка warning-free, bounded run EXIT0.
Естественные production Asio objects обеспечили определение libc++ DATA symbol;
это не доказательство исправления всех прежних compiler-only consumers.
Installed/relocated package и Linux всё ещё не проверены.

Compiler fixture delta `684bf617592aeba8b60e6b1dc7a4776d461782e1b3f4b03aff6ad7866fb2fd58`
с evidence `f477d193f9f6c726ca5af0b41023e17555792a6af5ee213f48b01f69935ee18d`
также принят reviewer. SAME factory добавляет реальную рекурсивную функцию с
неизменяемым max_call_depth=251. Interpreter reference и compiler/codec для двух
платформ прошли, frame=48; общий compiler CTest выполнил 25 cases за 4.60 s.
Native recursion этим не подтверждается: её runtime test остаётся отдельным.

Оба принятых test-only изменения механически интегрированы. Три consumer files
и compiler test postimages совпали; CMake дополнен только отдельным target.
Координатор выполнил `python3 tests/structure/check_structure.py <integration-root>`:
2392 first-party source files PASS; `git diff --check` PASS.

### 2026-10-14: lifecycle remediation and actual recursive execution

Remediation source `837f34c18a46326732a97b2f092743c0eb5e9e867a49b14fd9d13e947cc685ff`
и evidence rollup `f1c3d9ef506deecca9074fe61cdd1ea59d17745b631ea6aa9e84797971ba19af`
получили independent narrow CLEAR. Существующий child owner теперь публикует
endpoint, передаёт владение FD из scope guard, публикует PID и только последним
release-store устанавливает non-quiescence. Acquire-наблюдение этого состояния
не может предшествовать передаче PID/FD. Cleanup до commit сохраняет sticky
stop/deadline, а удерживаемая scheduler/compute operation завершает adoption,
единственное ожидание дочернего процесса и drain.

Изолированный test-only control использовал настоящий posix_spawn и задержку
между успешным OS launch и возвратом владельцу. Shutdown с уже истёкшим deadline
не завершился до снятия задержки; после неё подтверждены cancellation и
waitpid=-1/ECHILD после заявленного cleanup. Это соседний детерминированный
late-spawn сценарий, не точное воспроизведение прежнего межинструкционного окна.
Все семь исходных smoke cases повторены с исправленной библиотекой:
CTest 1/1 PASS, 3.31 s. Диагностический AS cap 1 TiB не стал production default.

Runtime recursive test `387eb0a09bdc6c2a9bba4255e1717241c3e283de8ff1e579c53f9273880a968c`
и actual evidence `e04381ff3d59b8bc00fabd0755de2791d17a09910a9fefe5844bb77ad3200caf`
также получили narrow CLEAR. На macOS ARM64 SAME compiler artifact с frame=48
и canonical depth=251 выполнил 250 -> i32 42, 251 -> canonical trap 8,
затем снова 250 -> i32 42. Независимо измеренное usable stack allowance 262064
bytes превышает требуемые 12096; локальный physical limit не подменяет этот
канонический отказ. Selected genuine suite: 10/10, 143/143 assertions PASS;
17 остальных owning cases этим selector не запускались. Linux bytes проверены
на целостность, не выполнены.

Координатор механически интегрировал coherent session source 3ca + remediation
837f и recursive test. Все 27 неизменяемых postimages из 28 session files
совпали; единственное отличие CMake — ранее принятый ordinary-consumer target.
Auxiliary diagnostic CMake не переносился в product tree. Combined Structure:
2412 first-party source files PASS; `git diff --check` PASS. Полная combined
сборка, installed package, постоянная регистрация новых execution controls,
disk quota/eviction/restart, реальные resource limits, Spine и performance
остаются OPEN. Эти локальные результаты не являются разрешением на merge.

### Normal root graph and finite helper control

Combined evidence `e8e9b1455c3884c853102938e26198c941ca9b109bdd664dfe418c8817a95f2c`
в integration worktree `build/oc-root-combined-evidence-v1.md` фиксирует новую
проверку обычного root graph с OC=ON, без auxiliary CMake. Семь выбранных targets,
включая настоящий `forge-wasm-compiler`, собраны с `-j 4`, EXIT0. Capture build
output частично обрезан инструментом; warning-free не заявляется. Binaries,
Ninja records и полные test logs сохранены.

На этом дереве owning CTest 4/4 PASS (14 artifact + 25 compiler + 12 epoch +
17 executor cases); отдельно session 7/7, genuine compiled suite 10/10 и обычный
публичный consumer PASS. Session использовал явный diagnostic AS cap
448085884928 bytes; genuine suite — immutable reviewed compiler fixtures.
Fresh separate process/disk checks выполнены перед запуском; после завершения
процессы не остались. Independent reviewer подтвердил focused normal-graph
CLEAR: 18 artifact bindings и combined source postimages без расхождений.
Это не install/relocation или whole-product gate.

Test-only resource delta `e79dac257dad38d94d76420031d10a7442dd64cfda4a3948ce1ec1107834260e`
и evidence `6eeaf36c882bdbc454cd0187d69e431a6e7e8590c088bade0a2990f865ed6506`
получили independent narrow CLEAR. Один и тот же настоящий helper/request
при absolute AS 446206836736 дал typed compiler_unavailable (4040006), без ready;
при 448085884928 дал compiled/prepared exact identity. Обе ветки завершили
session/child/compute cleanup до проверки результата. Место конкретного отказа
allocation не инструментировано: precise compile_limit, RSS bound, sandbox и
production default этим не доказываются. Product resource policy не менялась.
Guarded test-only source механически интегрирован; auxiliary diagnostic CMake
не переносился. Постоянная root-регистрация resource/lifecycle controls остаётся
отдельным незавершённым пунктом.

Следующая необходимая product boundary: существующий Forge compiler_session
должен выполнить owned async preflight HELLO/target/cache до допуска production,
а не впервые при реальном контракте. Реализация startup и bounded persistent
cache остаётся в том же Forge owner. В Spine отдельно принят source-only
close-order fix: execution host/allocator освобождаются после compute drain;
runtime lifecycle test этого изменения ещё не выполнен.

### Base EOS-VM regression on the combined native tree

Coordinator evidence `build/oc-root-base-evidence-v1.md` сохраняет normal-root
сборку перед запуском и actual CTest 4/4 PASS за 4.32 s: unit 201 cases,
internal 27, spec 2428 и проверка spec generator. Полный CTest LastTest log
сохранён byte-for-byte; SHA исходников и binaries привязаны в evidence.
Build EXIT0 с предупреждениями в donor tests; warning-free не заявляется.
Это macOS ARM64 base-interpreter regression, не проверка неподдерживаемого JIT.
Native-signals target и отдельный заблокированный Mach-negative scenario здесь
не запускались. Installed package, Linux, SpineOS и performance остаются OPEN.

Evidence `6ab6a0992347f7ac116a48296a7ea2af8ea6c05da2f88793ac7715a6cbc33506`
получил independent narrow CLEAR: 9 completion bindings и 196 source hashes
совпали. Сохранённый LastTest остаётся самостоятельным immutable artifact;
следующая проверка закономерно заменила текущий CTest LastTest.

Отдельно существующий `test_forge_package_metadata` при OC=ON прошёл 1/1,
1.92 s; raw log `build/oc-root-metadata-v1.log`, SHA256
`b65f9633faddf5ecf918396ea9cf094c265aa7d14a9094b6ce99cec837dafc25`.
Это metadata-only проверка notice closure, её отказов и перемещения package
metadata, не сборка/исполнение установленного optimizing consumer.

### Spine selection review and partial worker startup

Первый Spine selection/cache slice зафиксирован отдельно от controller WIP:
`build/spine-optimizing-selection-source-v1.patch`, SHA256
`c72851ab9a38c397ed88badb9d7e35238f53222fae88c8539c925e4e1ac7a0f7`.
Reviewer обнаружил source-level parity defect: eager signature validation
неиспользуемого optional `__forge_call` могла отклонить корректный `apply`
только после готовности OC. BASE admission требует сигнатуру `apply`, но не
такой дополнительной функции. Remediation и отдельная проверка фактического
nested-call return behavior возвращены владельцу Spine. Runtime этого slice
ещё NOT_RUN; ни исходный diff, ни исправление не объявлены принятыми.

Для controller согласована узкая startup adaptation без изменения Forge Asio:
существующий compute pool при исключении `on_worker_start` останавливает и
соединяет потоки, но не гарантирует `on_worker_stop`. Spine startup hook поэтому
сохраняет ошибку подготовки, не выпускает её из hook и не допускает execution.
После завершения создания pool controller проверяет сохранённую ошибку;
при отказе явно завершает pool со всеми stop hooks до уничтожения OC owner,
затем сообщает startup failure. Forge `release_thread` уже допускает поток,
который не был подготовлен. Нужен regression test частично успешного старта;
согласование этого порядка не является результатом его запуска.

Независимый review исходного c728 подтвердил этот единственный present-code
blocker. Exact remediation `056c628dd3cac1f8d83b4d373cb8f4c5bbe8436ad2a1ba88a1f590b00a898552`
после проверки обоих pre/postimages и reverse patch получил composed
source-only CLEAR: optional export лишь классифицируется, `apply` остаётся
void; фактический call с двумя i64 и 0/1 numeric result игнорирует operand
как BASE, не затрагивая отдельные return bytes. Unsupported call shape
выбирает прежний BASE path до native invocation; fallback после effects нет.
Malformed v1 patch не использовался. Реальные regression fixtures этих
четырёх ветвей, cold/warm/nested execution и controller lifecycle ещё NOT_RUN.

Отдельный controller source slice `3faff22ccdda0c60dbffe8522406bf2a56848b953ee223a4594b332ecabb59e0`
передан на independent review с immutable pre/postimages 15 файлов и manifest
`5c30f998b4c718f745eb5390a7c6bc630f9405f7acb0f3148057acaaa5374044`.
Он добавляет explicit finite configuration, owned preflight до допуска
controller, lifetime ресурсов и partial-start cleanup; transaction retry и
runtime-проверки не входят в этот результат. Зависимые compiler declarations
зафиксированы отдельно, их startup/cache реализация ещё не интегрирована.

Compiler owner уточнил lifecycle contract: `compute.request_stop()` до
`compiler_session::shutdown()` допустим, если объекты compute и Asio runtime
сохраняются живыми. Shutdown не должен отправлять новые compute jobs; он
дренирует уже учтённую работу, child strands/reaping и лишь затем освобождает
cache lock. Это не равнозначно уничтожению compute pool до session drain.
Отдельная проверка остановки compute при незавершённых startup/spawn/cache
операциях обязательна и пока NOT_RUN.

Controller review 3faff: REVISE по одному подтверждённому дефекту интеграции.
Один compute pool обслуживал и VM/authority, и compiler preparation/cache I/O.
При одном worker BASE apply попадал в ту же FIFO-очередь за blocking cache
work; неблокирующего enqueue недостаточно для принятой donor boundary.
Согласовано исправление в том же controller owner: отдельный bounded Forge
compute pool только для подготовки, без VM hooks и нового scheduler framework.
Session drain выполняется при живом preparation executor; после него join
обоих pools и освобождение native/host owners. Число preparation workers и
обе очереди задаются явными конечными параметрами, отдельно от child budgets.
Обязательный детерминированный тест: один VM worker завершает настоящий BASE
apply, пока отдельный preparation worker удерживается на конечном barrier.
Это архитектурный контрпример, не измеренная величина regression на текущем Mac.
Remediation ещё не прошёл review/runtime gate.

Подготовка genuine installed package использует обычные 93 Forge archive
targets. Два запуска SDK build были остановлены координатором с EXIT130 из-за
одновременной внешней сборки и интенсивной подкачки, без compile-error verdict.
Evidence: `build/oc-root-sdk-evidence-v1.md`; captures первого запуска частичные.
Incremental outputs сохранены, чужие процессы/данные не тронуты. Возобновление
после устойчивого свободного окна само по себе не является package PASS.

### Preparation isolation review

Composed controller source `3faff + d60c + e130` получил narrow source-only
CLEAR. Remediation `d60c2ada8079f3d207958b85afc5bab83c099d151d370fdaf7f85dc22f933b42`
отделяет preparation pool от VM/authority pool и сохраняет его доступным до
`compiler_session::shutdown()`. Затем выполняются VM join/stop hooks,
preparation stop/join и native shutdown; ошибка не сбрасывает владельцев.
Reviewer сверил 13 pre/post snapshots и обратную применимость patch.

Пять новых runtime scenarios используют настоящий helper/WASM, но пока NOT_RUN.
В их исходнике reviewer нашёл отдельную ошибку: `apply_context::action` хранится
по значению, а fixture обновлял только исходный action и transaction. Test-only
remediation `e13031d229f167922c773935c44e755c146ac036ba7aed6698848900965179f9`
явно обновляет `context.action` перед проверяемым вызовом. Его точная однострочная
поправка и postimage также проверены. Это не изменение production или WASM.

Полный transaction retry пока не реализован. До него требуется определить
нейтральную Forge-owned границу внешнего прерывания BASE: generic timeout не
доказывает успешную готовность конкретного OC artifact. Продуктовый повтор
должен оставаться в controller после полного rollback, без копирования
watchdog/TLS/memory-protection механики в Spine.

Compiler startup/cache source freeze передан на независимое review:
`compiler-session-startup-source-v1.diff`, SHA256
`f31834e3d4fcf11551dacedb51c617c1677ef24a50cdb72837756deddaef7a0c`;
manifest `9117e4cefc9b394cf877bea749531cd35e14221f3d6a48bfc5e04c5ec78dde58`.
Координатор проверил 99 path/hash bindings без расхождений. Все preimages
совпали с integration tree, кроме двух ожидаемых CMake files, где уже есть
принятые runtime/consumer additions; при переносе нужно сохранить обе стороны.
23 product files включают eager owned initialization, настоящий HELLO preflight,
bounded persistent cache и исходники 19 session/protocol tests плюс resource
control. Сохранённый lazy waiter не должен обращаться к уничтоженному owner.
Пакет пока UNBUILT/NOT_RUN и не интегрирован; auxiliary diagnostic graph не
является частью продуктового patch.

Первый concrete finding для frozen f318: `finish_initialize` выставляет
`setup.finalized`, даже если non-quiescent child требует сохранения live record.
После последующего успешного reap/drain повторный вызов немедленно возвращается
по этому флагу и не выполняет `release_job`; финальная проверка live entries
снова сообщает `shutdown_incomplete`, cache lock не освобождается. Пакет REVISE.
Владельцу возвращено разделение once-only outcome/registration finalization и
повторяемого освобождения уже quiescent live record, с owning regression test.
Это source-proven recovery-path defect; runtime reproduction пока NOT_RUN.

Полный first-pass review f318 завершён REVISE с двумя lifetime findings.
Второй: lazy initialization waiter удерживает `shared_ptr<job>`, а через него
уже quiescent child со socket/timer/process_handle старого Asio runtime.
Сохранение waiter после уничтожения session и runtime поэтому не соответствует
заявленной независимости outcome. Существующий pre-cancel control этого не
проверяет: в нём child ещё не создавался. Требуется независимый terminal outcome
и настоящий successful-initialize retained-waiter control после уничтожения
старого runtime, с сохранением pending-cancel completion. Остальные проверенные
contracts не получили concrete findings; header changes не меняют raw field
order, ABI или codegen identity. Coherent remediation обоих дефектов ещё pending.

Independent performance-plan audit не нашёл платформенного противоречия:
Spring LLVM 11 против Forge LLVM 22 сравниваются парными native Linux runs;
macOS ARM64 отдельно сравнивает Spine BASE/OC, эмуляция не заменяет эти замеры.
До performance verdict обязательны следующие раздельные измерения:

- Cold BASE end-to-end при занятом preparation/helper и полной очереди, включая
  стоимость загрузки/copy/hash и admission. Pending miss пока снова получает
  owned WASM; неблокирующий enqueue сам по себе не доказывает малую задержку.
- Первый native bind каждого VM worker отдельно от steady warm dispatch:
  prepared cache является thread-local. Проверить короткие host-heavy,
  loop/FP workloads, pool/host переходы и epoch cost, не только один тёплый поток.
- Fresh-process disk hit: startup HELLO/cache scan, первый loaded/prepared
  invocation и steady warm разделены; считать compile jobs, а не число запусков
  helper. Новый session в прежнем PID не заменяет новый процесс.
- Concurrent identity churn/restart: фактические encoded bytes/entries,
  old+temporary/orphans, resident/pinned mappings, unverified/ticket charges;
  p95/p99, block lag и rejected/offered work при ограничениях. RLIMIT smoke
  не заменяет проверку quota. Общий corpus и не менее пяти пар без co-build
  остаются требованиями; таких runtime artifacts пока нет.

### Installed helper dependency boundary

Source/install-script inspection and `otool -L` distinguish two package gates:
the existing `dev` component installs actual archives/module file sets, while
the compiler executable is currently installed by `Unspecified`. A complete
local SDK plus helper therefore needs both normal component installations; no
synthetic export or copied private object is an acceptable substitute.

The current macOS helper links absolute Homebrew Boost, OpenSSL and LLVM 22.1.8
dylib install names. Installation removes the build-only LLVM Cellar rpath,
but does not bundle or relocate those external dependencies. Moving the Forge
prefix on the same provisioned host tests package-prefix relocation only, not
a self-contained macOS distribution. Runtime `vm_wasm_optimizing` consumers
remain LLVM-free; `tooling_wasm_compiler` owns LLVM discovery. The helper path
is explicit session configuration, not an exported executable target.
Actual relocated HELLO/compile and installed consumers remain NOT_RUN.

### Neutral BASE interruption source freeze

The separate additive interpreter boundary is frozen as
`base-interruption-v1.patch`, SHA256
`bfe29f0ab605a3ecc02b0352ac38fb509353a9f843e38c2716fed0ae51e45c2b`,
manifest `b687917c08a8b738b5e2eac6188ec8601be45ee65d3f7b527e00793462594193`.
Coordinator verified all 27 pre/post/live bindings and forward applicability
to the integration tree; the only preimage difference is existing README prose,
whose unrelated changes remain preserved. Seven approved production files,
the owning watchdog regression test and README are included. The explicit
absolute-deadline overload reuses the existing watchdog/protection owner and
adds typed canceled/interrupted outcomes without assigning product retry policy
to Forge. Six bounded source scenarios are not executed yet. Independent
source review is pending; this packet is not integrated or accepted at runtime.
No Mach-negative WIP, Mach forwarding mechanism or CMake graph change is part
of this packet.

The complete first BASE review found two concrete source issues. Separate
timeout count/boolean publication admits
an inner-retirement/outer-callback race that leaves protected outer code with
the boolean cleared. Independent nested mappings need a single proven source
of interruption state, not a widened fault catcher. Also, the new guard
destructor can invoke a legacy duration watchdog callback on the destroying
thread when its timer has not run yet; this violates the existing asynchronous
callback contract. Preserve the original runner ownership and add a thread
identity regression. Both findings are returned to the runtime owner; bfe29f
is REVISE, not an integration candidate.

The agreed narrow remedy uses the existing atomic timeout count as the sole
state. The redundant boolean is outside the exported namespace; removing it
does not change the documented consumer API. The private Darwin recovery view
will borrow the count instead of the boolean, with no classifier, Mach message,
port or forwarding change. Publication precedes disabling code; reenable
precedes retirement. Do not add a permanent TLS test hook to force the former
split-store gap. Bounded actual nested overlap tests plus a source ordering proof
are appropriate, but must not be called deterministic reproduction of that gap.

The proposed downstream full-transaction retry also needs a fresh DB savepoint
after rollback: Forge Core consumes both rollback and release boundaries. The
original expected receipt, transaction ordinal and absolute block deadline must
survive unchanged, while execution-local traces, resources and queues are
created afresh. This remains a source-bound design, not implemented retry or
runtime acceptance.

### Integrated reviewed source — startup/lifetime and BASE interruption

Independent review cleared the composed startup/cache source `f31834e3` plus
lifetime remediation `87a890ab`, and BASE interruption `bfe29f0a` plus remediation
`58491235`. These four immutable patches are now integrated. Coordinator checked
34 composed paths: 31 match reviewed postimages exactly (including the intended
removed file). Three pre-existing integration differences are retained: the
optimizing and test CMake files keep already integrated executor/consumer targets,
and the interpreter README keeps its prior documentation shape. The stale Darwin
forwarding comment now matches the reviewed, approved conflict-refusal boundary.
No blocked Mach-negative test or forwarding implementation was introduced.

The lifetime remedy separates retained outcomes from runtime/child ownership,
allows retry of a genuinely incomplete child drain, and preserves conservative
resource accounting. The BASE remedy uses one atomic interruption count and
preserves timer-thread ownership of old duration callbacks. Source review is not
runtime acceptance: new startup/lifetime controls, eight interruption controls,
installed/relocated consumers and platform/performance gates remain NOT_RUN.

Coordinator structural check passed over 2412 first-party source files after
integration; `git diff --check` passed. The third SDK build attempt was deliberately
terminated before these source changes, EXIT130, with all verified own processes
gone and incremental outputs retained. No stale paused process should be resumed.
Other tasks' builds and files were not changed; no commit, push, merge or CI ran.

The exact downstream retry design `57ee724c` and its 17 source-owner bindings
`81b2f370` received independent design CLEAR. Implementation is assigned to the
runtime developer. A watcher must use the injected scheduler without controller
gate, DB or VM/preparation-pool dependencies; cancellation and terminal drain
precede destruction of borrowed state. The ready artifact/identity/generation,
caller cancellation and original deadline are checked again before replay.
Unexpected await/allocation failure is not evidence of successful drain.

A separate compiler cache-cost source slice `75f87bdc` received independent source
CLEAR and is integrated; all three postimage hashes match. Under the existing
exclusive, cooperating-owner directory contract, the bounded startup scan builds
the sole disk catalog; subsequent operations validate their selected entry rather
than decoding unrelated files repeatedly. Unknown files remain charged and are
not deleted; old-plus-temporary reservations and confirmed-release requirements
remain. Noncooperating same-UID mutation is not an advisory-lock security promise.
The two new owning controls bring expected counts to 25 Darwin / 24 Linux plus
one resource case; these counts are source-derived and all remain NOT_RUN.
This source improvement has no runtime/performance PASS yet. The additional
eight WASM structure-policy tests passed; they do not execute native VM code.

### Mechanical style and provenance checks

The focused style check initially rejected 22 of 29 composed C++ files.
Coordinator applied only clang-format 22.1.8 to the exact reviewed preimages;
all 29 subsequently pass dry-run/Werror and diff whitespace checks. Independent
format review `7bd58135` / `0a8f98e6` confirmed all 22 postimages exactly equal
formatter output, with unchanged quoted/raw literals and non-whitespace bytes,
and no preprocessor/include/import/module/export changes. Historic snapshots
remain intact; the current 282-file inventory is recorded separately in build
evidence. No runtime result is carried over across this source integration.

The existing provenance gate passes against pinned Spring blobs, EOS VM owner,
LLVM 22.1.8 headers/notices and the exact Wasmtime platform/epoch snapshots.
An initial check used the runtime developer's intentionally narrower snapshot
and failed on an absent compile-environment donor file; the final check used
the already existing compiler developer snapshots, without modifying donor
hashes, downloading another source tree or changing the product.
