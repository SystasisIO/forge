# Forge: портирование EOS VM OC

## Статус и границы плана

План принят к реализации 2026-10-12. Реализация compiler/runtime и интеграция
SpineOS находятся в работе; OC ещё не прошёл полную приёмку. Точный статус
срезов и границы ранее выполненных проверок фиксируются в
[журнале реализации](../donors/oc-phase0-review.md).
Принятая идентичность backend описана в
[VM WASM Backend Family](forge-vm-wasm-backend-family-v1.md).
Этот план конкретизирует перенос; новые API, параметры и targets ниже являются
проектируемыми, а не существующими возможностями Forge.

- Ветка: `vm-wasm-optimizing-v1`.
- Проверенный baseline Forge dev: `94714f0ff465435ef317ec1891d44b1e9380c01e`.
- Source donor Spring: `e6a99f68b67abc4d89fe716755b2e1394a4991f7`.
- Текущий EOS VM port: `e5b1fc79c4b8d78f32749afa94a8d4c4d071f67f`.
- Поддерживаемые OC-платформы: Linux x86-64 и macOS ARM64. Расширение
  утверждено 2026-10-13; обе платформы являются обязательными gates этой поставки.
- Платформенный донор Wasmtime: `05e7deebf1ddcc726bfc4c30cd87e3dbb2881072`.
  Это источник проверяемых решений, не новая runtime/build dependency.
- В поставку входит обязательная
  [интеграция и приёмка первого потребителя](../donors/spine-wasm-optimizing-integration-v1.md).

EOS VM OC (optimizing compiler) компилирует WASM в нативный код для исполнения.
Contract SDK компилирует исходники контракта в WASM; это другой этап.
Текущий interpreter и non-OC x86-64 JIT сохраняют свои идентичности.

## 1. Результат и инварианты

Получить дополнительный backend с проверяемой эквивалентностью текущему VM,
ограниченными ресурсами, изолированной компиляцией и установленным package,
который продукт использует без Spring/FC/chainbase.

1. Идентичность: `libraries/vm/wasm/optimizing`,
   `forge_vm_wasm_optimizing`, component `vm_wasm_optimizing`,
   `forge::vm::wasm::optimizing`, `forge.vm.wasm.optimizing.*`.
2. Семейства `vm` и `vm.wasm` остаются пустыми. Общий backend factory,
   aggregate module/target и compatibility aliases не вводятся.
3. Канонические WASM bytes, допустимость модуля, host signatures и
   детерминированная floating-point семантика одинаковы у всех backend.
4. OC не знает chain ID, аккаунты, permissions, DB, resource billing,
   protocol-feature activation, producer или порядок действий.
5. Библиотеки используют Forge Raw, Schema, Crypto, typed exceptions и
   существующие runtime/executor механизмы. FC API и макросы не переносятся.
6. Решение о выборе backend принимает потребитель до начала исполнения.
   Ошибка уже начатого исполнения не является основанием повторить вызов
   другим backend поверх частично изменённого состояния.
7. Linux x86-64 и macOS ARM64 используют общие parser, lowering semantics,
   host ABI, artifact validation и cache/session mechanics. Различия target
   codegen, executable memory и traps локализованы у существующих владельцев.
   Linux ARM64, macOS x86-64, удалённый compiler API и перенос native cache
   между машинами не входят в этот этап. Interpreter не должен регрессировать.
8. Перенос не обещает заданный коэффициент ускорения. Производительность
   измеряется после доказательства корректности.

## 2. Source audit и трассировка донора

До портирования составить file-level manifest: repository/SHA/path,
исходный hash, целевой файл, лицензия, адаптации, тесты и причина исключения.
Расширить существующие provenance/test manifest механизмы, не создавать
параллельную систему учёта. Указать отдельно код Spring, EOS VM, WAVM и LLVM.

Обязательный исходный инвентарь Spring:

| Донорские пути | Что сохраняем | Что остаётся за пределами Forge VM |
|---|---|---|
| `libraries/chain/webassembly/runtimes/eos-vm-oc/LLVMEmitIR.cpp`, `LLVMJIT.cpp` | lowering WASM в LLVM IR, native generation, relocations | старые LLVM API и глобальные build flags |
| `compile_trampoline.cpp`, `compile_monitor.cpp`, `ipc_helpers.cpp` | изоляция compilation, лимиты, передача дескрипторов | `__wrap_main`, process globals, FC logging, собственный runtime ноды |
| `executor.cpp`, `memory.cpp`, `stack.cpp`, `switch_stack_linux.s`, `gs_seg_helpers.c` | execution ABI, memory bounds, traps, interrupt, stack switching | `apply_context`, transaction timer и chain exceptions |
| `code_cache.cpp` и парный header | versioned bounded cache, владение executable mappings | `chainbase::database`, `code_object`, account whitelist/write-window policy |
| `intrinsic.cpp`, `intrinsic_mapping.hpp` | typed host call ABI и стабильное разрешение imports | chain-specific intrinsic implementations |
| `wasm_eosio_injection.*`, `libraries/wasm-jit` IR/WASM code | понять необходимые transformations и representation | второй независимо развивающийся WASM parser/validator |
| `wasm_interface.cpp`, runtime `eos-vm-oc.cpp` | выбор tier, cold/warm behaviour, resource boundaries | Spring controller и node configuration |
| `unittests/eosvmoc_interrupt_tests.cpp`, `eosvmoc_limits_tests.cpp`, `wasm_tests.cpp`, `wasm_config_tests.cpp`, `wasm-spec-tests` | донорские входы, результаты, regression cases | donor test runner как runtime dependency |

Современный upstream сравнить с фиксированным донором по OC-исправлениям:
reentry, register restoration, compile scheduling, bounds, traps и shutdown.
Новые source pins или backports указывать явно; не смешивать незаметно версии.
Проверить лицензии каждого перенесённого участка и доставку notices/source
provenance в install и runtime images. Существующий EOS VM audit сам по себе
не подтверждает состав будущего OC-порта.

## 3. Владение и package graph

| Компонент | Ответственность |
|---|---|
| `forge_vm_wasm_interpret` | существующие parsing/validation, typed host mechanics, interpreter/non-OC JIT, SoftFloat |
| `forge_vm_wasm_optimizing` | compiled artifact, native execution, guarded memory, cache mechanics, async client изолированного compiler worker |
| Предлагаемый `forge_tooling_wasm_compiler` | реальный compiler implementation, LLVM lowering/code generation и private worker command |
| Предлагаемый `tools/wasm-compiler` | тонкий executable `forge-wasm-compiler`, вызывающий tooling command |
| Продуктовый runtime owner | конфигурация, жизненный цикл compiler session, выбор/приоритет WASM, cache budgets и политика деградации |

Разделение runtime/compiler targets необходимо, чтобы interpreter-only
потребители и клиент native execution не получали обязательную зависимость от
LLVM. LLVM принадлежит compiler implementation; helper не является сетевым
сервисом и не требует отдельного deployment.

Направление зависимостей:

```text
consumer -> vm.wasm.interpret
consumer -> vm.wasm.optimizing -> existing reusable VM mechanisms
tooling.wasm_compiler -> vm.wasm.optimizing artifact/worker protocol
tooling.wasm_compiler -> existing VM parsing + LLVM
tools/wasm-compiler -> tooling.wasm_compiler command
```

`optimizing` не линкует `tooling.wasm_compiler`; связь с helper выполняется
через локальный процессный канал. Ни один VM leaf не импортирует plugins,
application shell или продуктовые библиотеки. Compiler helper получает owned
bytes/configuration, не DB handles и не указатели host process.

В phase 0 AGENTS явно расширен: optional `tooling.wasm_compiler` является
дополнительным владельцем LLVM наряду с ABI/attribute tooling. `tools/`
остаётся тонким слоем над настоящей библиотекой команды. Это не разрешает
зависимость runtime consumers от LLVM или перенос второго WASM parser.

### Общий parser и host ABI

Нельзя скопировать WAVM parser/validator рядом с существующим Forge parser.
Phase 0 подтвердил существующий публичный `interpreter::parser` alias:
он выдаёт `module` с typed bitcode, не требует импорта private partitions
и не требует нового writer facade. `null_backend` остаётся путём проверки,
но его writer не сохраняет инструкции для lowering. Кроме parsing необходимо
сохранить существующие проверки `module.error`, data segments и imports.
Installed consumer должен проверить этот путь без source include directories.

Compiler-specific LLVM IR является допустимым внутренним representation;
он строится из существующей WASM parsing/validation механики. Донорские
injections классифицировать по смыслу: VM safety/SoftFloat остаются в Forge,
продуктовые ограничения передаются явными параметрами. Двойное injection
и пропуск canonical validation запрещены.

Если эта граница потребует переноса публичных типов между namespaces,
сначала оформить точный dependency/API diff и migration note в phase 0.
Такой source break не считается автоматически разрешённым обычным переносом
OC; основная цель — additive API с сохранением существующего interpret API.

### Публичные контракты

Зафиксировать до большого переноса точные объявления и тестовый consumer:

- compile request: owned WASM, validation/compile limits, typed import
  signature table, explicit target profile и compatibility identity;
- compiled artifact: immutable native code/init data/export metadata,
  relocations и проверяемая identity без process pointers;
- compiler session: async submission с deadline/cancellation, bounded
  admission, stop/drain и отчёт capabilities;
- executor: вызов заданного typed export с invocation-local memory, host
  bindings и execution deadline;
- typed failure categories: invalid module, unsupported target/import,
  queue full, compile limit, cancelled, compiler unavailable, invalid artifact,
  cache I/O/corruption, execution trap и deadline.

Имена конкретных modules/методов фиксируются в phase 0 по реальным consumers.
Не создавать декоративные `api`, `types`, facade и пустые implementation files.
Value types не получают pimpl; resource owners скрывают тяжёлые зависимости.
Объявления/реализации следуют `create-library`, включая парный инвариант.

## 4. LLVM и платформы

Проверенный Spring donor допускает LLVM 7–11 и Linux x86-64. Его legacy ORC
API, typed pointers и pass manager нельзя считать совместимыми с новым LLVM.

Первый исполнимый этап — narrow compiler spike на LLVM 22, согласованном с
целевым modern toolchain: integer/float arithmetic, memory, indirect call,
host call, trap, interruption и relocatable output. Зафиксировать точную
LLVM patch version, target triple и CPU features после прохождения spike.
До этого LLVM 22 является выбранной целью, а не доказанной поддержкой.

- LLVM IR verification включён в проверках; незнакомые relocations отклоняются.
- Старую ORC инфраструктуру заменить поддерживаемыми LLVM API, сохранив
  наблюдаемую WASM семантику и проверив linkage/code layout.
- Не применять fast-math или host-native float замены, нарушающие SoftFloat.
- LLVM остаётся target-scoped; обычная Forge configure/build не собирает LLVM.
- Proposed build option: `FORGE_ENABLE_VM_WASM_OPTIMIZING=OFF` по умолчанию.
  `ON` на неподдерживаемой архитектуре или без совместимого LLVM — ясная
  configure error. Runtime-only installed consumer LLVM headers не требует.
- Отдельно проверить Debug/Release и x86-64 CPU baseline без зависимости
  сгенерированного кода от случайных возможностей build machine.

Native macOS ARM64 и native Linux x86-64 проверяются независимо. Linux ARM64
Docker и эмуляция amd64 дают только дополнительные результаты и не заменяют
эти gates. Performance verdict отдельно измеряется на каждой native platform.

### 4.1. macOS ARM64: платформа, не второй движок

Зафиксировать file-level provenance Wasmtime на указанном SHA, начиная с
`crates/wasmtime/src/runtime/vm/sys/unix/{machports,mmap,signals,traphandlers,vm}.rs`
и используемых ими stack/transition components. Применение конкретного решения
требует source review и проверки публичного Darwin SDK; список не означает,
что все эти реализации уже перенесены или приняты.

1. Compiler owner расширяет существующий target profile и LLVM generation
   для ARM64. Формат входного object image, whitelist relocations, calling
   convention, stack alignment и CPU baseline фиксируются до общего API diff.
   Не создавать второй lowering/parser либо отдельный macOS compiler product.
2. Runtime owner расширяет существующий native image/VM boundary: публичные
   Darwin executable-memory APIs, разделение записи и исполнения (W^X),
   publication с синхронизацией instruction cache и проверенные требования
   подписи executable/entitlements. Требования документировать по реально
   выбранному механизму, не отключать системную защиту для прохождения тестов.
3. Wasmtime Mach exception handling — donor candidate для Darwin guest traps:
   классификация по принадлежащему VM коду/памяти, восстановление faulting
   thread и передача чужих исключений штатному владельцу. Проверить сохранение
   прежних handlers/ports, вложенные и конкурентные вызовы, lifecycle receiver,
   teardown и поведение после fork. Собственный протокол Mach messages не
   изобретать: использовать публичные SDK определения и штатные механизмы.
   Уточнение границы: Wasmtime — донор платформенных решений для наших
   EOS-VM/EOS-VM-OC, не цель общей совместимости. Если другой компонент уже
   установил непустой THREAD handler, точно восстановить его и вернуть
   `execution_unavailable` до WASM. Универсальную переадресацию сторонним
   обработчикам не добавлять; обычные task/default и POSIX пути сохраняются.
4. Не переносить дефект текущего draft: Darwin `siglongjmp(save_mask=1)`
   меняет маски соседних потоков на проверенном host. Простая замена на
   `save_mask=0` не закрывает lifecycle активного alternate signal stack.
   Private Apple helpers, правка opaque `jmp_buf`, неподтверждённые syscall
   константы и глобальная сериализация всех VM вызовов запрещены.
5. Shared host boundary сохраняет C++ cleanup. Ошибка WASM-памяти является
   typed trap; настоящий SIGSEGV/SIGBUS внутри C++ host остаётся fatal process
   defect по согласованному решению. Отмена одной invocation не прерывает другую.
6. Compiler helper запускается через exec/spawn; продолжение VM в child после
   fork с унаследованным Mach/runtime состоянием не поддерживается. Это не
   разрешает новый ad-hoc scheduler либо parallel signal owner.
7. Artifact/cache identity различает OS, architecture, target features и ABI.
   Linux machine code никогда не исполняется на macOS и наоборот. Общий WASM
   corpus обязан давать одинаковые результаты, а не одинаковые native bytes.

Сначала команда фиксирует platform contract и donor traceability, затем
compiler и runtime owners реализуют непересекающиеся slices. Reviewer проверяет
точные diffs и native evidence; support нельзя объявить по одной cross-compilation.

## 5. Изолированная компиляция и lifecycle

Предлагаемый путь — явный exec/spawn установленного helper по проверенному
package path. В отличие от Spring startup через `__wrap_main`, этот путь
совместим с уже запущенным application runtime. Продукт не делает `fork()` с
продолжением C++ работы внутри многопоточной ноды.

Worker использует донорскую процессную изоляцию компиляций: один чистый
coordinator/helper и ограниченное число дочерних compile jobs. Начальный
default — одна одновременная компиляция. Не переносить донорскую неограниченную
очередь как допустимый production default.

1. Локальный закрытый канал на унаследованном descriptor; без TCP/listener,
   bearer token или нового публичного Forge transport.
2. Version/build/target handshake до запуска LLVM-компиляции. Parent передаёт
   ограниченный существующий compile request; child проверяет framing,
   canonical configuration/identity и фактически поддерживаемый target, затем
   возвращает hello с тем же target profile и существующей Forge version/git
   revision. Это не echo непроверенного target и не новая capabilities registry.
   Parent проверяет hello целиком и отправляет header-only `hello_ack` с той же
   exchange/request identity. Только после точного ack и EOF child входит в
   compiler. Нулевая длина body разрешена только для ack; повтор, неверная
   identity/version, лишние bytes или отсутствие ack не запускают LLVM. Это
   дополнительная фаза того же закрытого Raw protocol, не новый транспорт.
   Несовместимый helper отклоняется; поиск executable через PATH запрещён.
3. IPC (межпроцессный обмен) использует OS descriptors и Forge Raw с явным
   versioned envelope, лимитами размера до allocation/decode, request IDs и
   проверкой числа/типа переданных descriptors. Spring wire не является
   публичным совместимым контрактом нового private channel.
4. Ограничить jobs, queued requests, суммарные queued WASM bytes, wall time,
   CPU time, address space, generated code и compiler stack budget.
5. Child не наследует signer keys, открытые wallets, chain DB descriptors,
   произвольные окружение/дескрипторы или продуктовые секреты. Проверить
   close-on-exec и минимальный передаваемый environment.
6. Асинхронные ожидания parent используют существующий Asio executor;
   scheduler/executor предоставляется владельцем. Нет скрытого thread pool,
   detached tasks, polling loop или `std::async`.
7. Отмена waiter не отменяет работу, нужную другому waiter того же artifact.
   Единственная compilation на identity, bounded dedup state.
8. Stop закрывает admission, отменяет очередь, завершает/ограниченно ждёт
   children и reaps их. После завершения shutdown нет orphan/zombie processes
   и обращений callback к уничтоженным объектам.
9. Crash/timeout/EOF helper превращается в typed failure для всех связанных
   запросов. Рестарт ограничен budget/backoff; без горячего restart loop.

Process separation ограничивает последствия сбоя и расход ресурсов; не
объявлять его полной OS sandbox без отдельных подтверждённых механизмов.

## 6. Native execution и code cache

Runtime сохраняет донорские bounds checks, call stack checks, trap handling,
memory/global reset и deadline interruption. Для каждого concurrent invocation
используются собственные mutable execution context и linear memory.

Spring прерывает исполнение через `mprotect(PROT_NONE)` на code mapping.
Один общий mapping нельзя использовать как interrupt domain нескольких
concurrent invocation. Выбранный механизм прерывания должен быть изолирован
на invocation; это проверяется двумя одновременными вызовами одного artifact,
когда отмена одного не мешает завершению другого.

Особенно проверить GS-register/stack restoration, RAII cleanup при trap,
host exception и nested/reentrant вызовах. Новые handlers должны сосуществовать
с имеющимся Forge VM signal/watchdog механизмом и внешними handlers.
Прямое копирование process-global signal ownership из Spring запрещено.

Cache identity включает WASM digest, codegen/cache version, LLVM/target/CPU
profile, host ABI identity и compile-affecting options. Product activation
policy проверяется на каждом выборе artifact, а не считается доказанной cache hit.

- Механика хранения/валидации artifact — Forge; перечень нужных contracts,
  приоритет, admission и budgets задаёт потребитель.
- Cache bounded по bytes и entries; используется ограниченное eviction,
  активный artifact удерживается handle до окончания исполнения.
- Проверять offsets, sizes, alignment, relocations и compatibility до
  executable mapping. Не принимать native bytes от внешнего сетевого клиента.
- Запись atomic; incomplete output никогда не публикуется как ready.
  Не использовать RWX mapping как штатный режим; описать переход W^X и
  ограничения прав cache directory/files.
- Exclusive cache ownership исключает два несогласованных writer.
- Несовместимый/повреждённый cache можно безопасно пересоздать по WASM;
  это не consensus state и не signed/checkpoint history.
- Compile failures не превращаются в бесконечный permanent blacklist.
  Negative cache bounded и различает детерминированную неподдерживаемую
  возможность и временный отказ ресурсов/worker.

## 7. Порядок реализации

| Этап | Работа | Критерий выхода |
|---|---|---|
| 0. Контракты | donor map, LLVM spike, parser/import boundary, helper packaging, exact API и consumer skeleton | dependency DAG и устанавливаемый sample согласованы; LLVM viability доказана; необходимые изменения AGENTS/migration перечислены |
| 1. Compiler | lowering/IR, codegen, artifact validation, resource limits | детерминированные fixture results, LLVM verification, invalid-input/limit tests |
| 2. Executor | memory/stack/host calls, traps, signals, deadlines, nested calls | donor/spec corpus и interpreter/JIT/OC differential tests проходят |
| 3. Session/cache | isolated helper, queues, cancellation, cache, restart/shutdown | crash/timeout/corruption/concurrency tests и relocation consumer проходят |
| 4. Consumer integration | реальная конфигурация, lifecycle и общий execution host | все execution entrypoints покрыты, никакого обхода validation/billing |
| 5. Runtime acceptance | replay, production, mixed backends, failure injection, cache restart и benchmarks | проверены canonical state/receipts и native Linux x86-64/macOS ARM64 processes |
| 6. Delivery | независимое ревью итоговых деревьев, docs, exact dependency pin | результаты и source hashes связаны; оба репозитория готовы вместе |

После каждого крупного этапа ревью diff и тестовых доказательств; findings
исправляются до перехода к следующему зависимому этапу. Consumer API нельзя
откладывать до завершения порта: его пробная интеграция начинается в этапе 0.

### Организация работы

Сохраняется согласованный состав: основной агент координирует работу,
разработчики и независимый reviewer — GPT-6.1 Sol с reasoning `xhigh`.
Перед первым handoff координатор фиксирует API, baseline и владение файлами.
Разработчики не редактируют один checkout одновременно; Forge и Spine slices
получают отдельные worktrees. Reviewer проверяет точный интегрированный diff
и артефакты, а не только отчёт разработчика.

Субагенты создаются с `fork_turns="none"`: только цель, worktree, разрешённые
файлы, обязательные AGENTS/skills, контракты, SHA и критерии готовности.
Полный чат не передаётся. Недоступная сессия заменяется той же ролью/model/effort.
Самостоятельные commit/push/merge/release субагентам не разрешаются.
Публикацию выполняет координатор после review loop и локальной приёмки,
когда пользователь разрешит реализацию и доставку этого плана.

## 8. Проверки

### Forge

- Текущие donor manifests, interpreter, non-OC JIT, watchdog, signals,
  SoftFloat и package relocation сохраняются зелёными.
- Донорские OC interrupt/limits и WASM spec cases; malformed modules,
  arithmetic edge cases, NaNs, bounds, start/data/table initialization,
  indirect calls, host signatures, missing imports, recursion и memory growth.
- Донорский interrupt test сохраняет продуктовый смысл: при разрешённом
  контексте готовность OC прерывает baseline execution, Spine полностью
  откатывает транзакцию и допускает один повтор с готовым OC. Продолжать
  прерванный native/WASM stack другим backend запрещено. Forge предоставляет
  нейтральное прерывание, но не знает undo/transaction/retry policy.
- Native macOS ARM64: executable publication, relocations, instruction cache,
  traps и чужие faults, host cleanup, stack overflow, nested calls, concurrent
  invocation, deadline/interrupt и teardown; полный interpreter regression.
- Lowering учитывает преобразованный bitcode: resolved branch PCs, удалённые
  block/loop markers и payload slots `br_table`; покрыть несколько функций,
  условные/табличные ветви и перенос значения результата через branch.
- Один fixture прогоняется interpreter/JIT/OC: возвращаемые bytes, memory,
  host call sequence и typed failure совпадают в рамках фиксированной policy.
- Отмена до очереди/в очереди/во время compilation; full queue и byte limits;
  worker crash, hang, truncated IPC, wrong descriptor и shutdown под нагрузкой.
- Cache hit/miss, eviction активного artifact, compiler upgrade, incompatible
  CPU/ABI, incomplete write, corruption, full disk и exclusive writer.
- Публичный compiled API не экспортирует LLVM/FC/WAVM/chainbase types.
- Installed consumer после перемещения prefix работает без source tree,
  находит helper и notices; mismatch/missing helper диагностируется.
- Interpreter-only consumer не требует LLVM и запускается на ARM64/macOS.

### Команды и доказательства

Использовать CMake + Ninja + согласованный LLVM; build `-j 4`.
Перед CTest собирать targets. У текущего VM проверки живут в `tests/vm_wasm`,
installed consumer — в `tests/package_vm_wasm_interpret_component`.
Добавить OC suite/consumer в штатную CMake регистрацию, а не отдельный runner.

```sh
cmake --build <forge-build> --target <affected-targets> -j 4
ctest --test-dir <forge-build> -N
ctest --test-dir <forge-build> --output-on-failure -R <verified-test-selection>
```

Placeholders заменяются точными командами после регистрации targets.
Перед прогоном сверить обнаруженный test list с матрицей: пустой selection,
skipped/NOT_RUN и только interpreter run не являются OC PASS.

Сохранять SHA обоих деревьев и donor, dirty diff hash, compiler versions,
CPU/kernel/architecture, команды, exit codes, durations, original logs,
fixture hashes, peak RSS, cache statistics и image digests. Benchmarks
разделяют compile latency, cold execution, warm execution и end-to-end replay;
результат не выводится из compile time одного пустого контракта.

## 9. Диагностика и поставка

Логирование использует `ilog/wlog/elog/dlog` и Schema. `ilog` — один startup
capability/config summary; `wlog` — ограниченная деградация/отказ worker;
`elog` — нарушение integrity/runtime fault; `dlog` — compile/cache/request
details. Cache hit и каждый вызов контракта не логируются на info.
Payload, native code, содержимое memory и секреты не выводятся.
Метрики: queue depth/bytes, compile duration/result, worker restarts,
cache bytes/hits/evictions, execution backend и причина fallback.

Implementation завершается согласованной поставкой библиотеки и потребителя.
Перед интеграцией освежить dev, проверить upstream изменения, повторить
затронутые проверки и review на окончательном дереве. Точный dependency SHA
обновляется после публикации Forge. Обычный push без force.

Пользователь разрешил реализацию и доставку после проверок и review.
Отдельное уточнение от 2026-10-12: после локальных проверок и review разрешён
один целевой ручной CI-прогон OC на native Linux x86-64 для Forge и Spine.
Локальный host — ARM64; Docker amd64 под эмуляцией не заменяет этот native gate.
Один acceptance workflow должен фиксировать точные SHA обоих репозиториев
и не запускаться автоматически от push/PR. Другие workflows, release/tags
не разрешены. Перед публикацией проверить triggers; branch protection
не обходить. Неуспешный или незапущенный обязательный gate блокирует merge.

Build directories и Docker resources учитывать по владельцу. Сохранять
исходные логи и небольшие доказательства, затем очищать только task-owned
воспроизводимые сборки; используемую сборку продукта и пользовательские
volumes не удалять. Не создавать копии donor/build trees ради каждого этапа.
