# SpineOS: интеграция Forge optimizing backend

## Статус и baseline

Consumer integration plan принят к реализации 2026-10-12; обязательная часть
[плана EOS VM OC](../iterations/forge-vm-wasm-optimizing-v1.md).
Документ размещён в donor/consumer notes, чтобы продуктовые сведения не
попадали в нейтральный публичный контракт Forge.

- Spine dev: `1cdc9ab9294996f95c20c91f0810d999d3c82a7f`.
- Текущий runtime pin Forge: `c3bf6f358c682df88d825aa960346551457f2f74`.
- Forge feature baseline: `94714f0ff465435ef317ec1891d44b1e9380c01e`.
- Foundation main: `e75fdfe8d598c87901dd660b15139d8c65024251`.
- Spring donor: `e6a99f68b67abc4d89fe716755b2e1394a4991f7`.

Перед реализацией повторно сверить refs. Spine integration выполняется в
отдельной ветке от актуального dev; действующие worktrees, данные и build
сохраняются. Созданы Forge feature branch и отдельная Spine integration branch
`codex/vm-wasm-optimizing-v1`; runtime изменения ещё не интегрированы.

Уточнения пользователя 2026-10-13: native macOS ARM64 добавлен в обязательную
поставку вместе с Linux x86-64; Wasmtime — донор платформы Forge. Поведение
tier-up и cache в SpineOS должно следовать проверенному Spring donor, включая
разделение base runtime/activation policy и безопасный transaction retry.
Прежнее ограничение «OC только со следующего вызова, без interruption» снято.

## 1. Проверенное текущее состояние

- `libraries/chain/execution/include/spine/chain/execution/host.cppm`
  объявляет `wasm_backend::{interpreter,jit}` и `runtime_policy` с default
  interpreter и prepared-cache capacity 32.
- `plugins/chain/controller/plugin_impl.cpp` создаёт `make_host` с default
  policy. Полноценного runtime выбора OC в spineos нет.
- `libraries/chain/execution/intrinsics.cpp` содержит canonical setcode
  validation, prepared execution cache и dispatch исполнения.
- `FORGE_CONTRACT_INTRINSICS` является общей регистрацией signatures;
  `validate_wasm` проверяет imports, whitelist и `apply(i64,i64,i64)`.
- Nested `call` исполняет `__forge_call`; поддержка только `apply` недостаточна.
- Controller владеет DB transactions/savepoints/revisions, execution
  использует borrowed handles. `programs/spineos/main.cpp` — тонкая композиция.
- Native CMake сейчас вопреки AGENTS добавляет `vendor/forge` через
  `add_subdirectory`. Устранение этого baseline mismatch входит в integration:
  `find_package(Forge COMPONENTS ...)`, imported leaf targets и установленная
  единственная registry/types из Forge Contract SDK; без копий из source tree.

Source documents: Spine `docs/architecture/12-wasm-execution.md`,
`docs/donors/spring-wasm-execution.md`,
`docs/roadmap/spine-implementation-blocks.md`; Forge
[backend family](../iterations/forge-vm-wasm-backend-family-v1.md).
Foundation `docs/products/spine/README.md` пока отражает более ранний baseline;
отдельного принятого OC implementation plan в Foundation main нет.

## 2. Архитектурная граница

| Слой | Ответственность |
|---|---|
| Forge VM/tooling | canonical VM mechanics, LLVM compiler, artifact/cache mechanics, native executor, process client |
| `spine.chain.execution` | contract selection, host intrinsics, chain policy, billing, deadline mapping, выбор backend для invocation |
| `spine.plugins.chain.controller` | compiler/cache lifetime и configuration, existing compute runtime, DB ownership, block apply/production |
| `spineos` | composition и install/deployment; не владелец нового VM framework |

Не вводить `spine::wasm`, локальный EOS VM adapter, вторую intrinsic registry,
FC/chainbase dependency, новый chain state store или общий backend facade.
Продуктовая функция выбора engine допустима внутри execution; она не
реализует повторно parser, allocator, signals, worker или cache storage.

Controller создаёт один bounded compiler session/cache owner и передаёт
execution типизированные Forge handles. Compiler получает immutable owned
WASM bytes. Background compile не удерживает borrowed DB transaction,
writer lock, `apply_context` или ссылку на mutable contract row.

## 3. Точки изменения Spine

| Пути | Изменение |
|---|---|
| `libraries/chain/execution/host.cpp`, `host.cppm` | расширить policy и capability validation; явно передавать доступные Forge resources |
| `libraries/chain/execution/intrinsics.cpp` и owning private headers | единый выбор backend для apply/notifications/inline/deferred/read-only/nested call; разделить большой файл по существующим компонентам только при необходимости |
| `plugins/chain/controller/plugin.cpp`, `plugin_impl.cpp`, `details/plugin_impl.hxx` | configure/initialize/stop/shutdown владельца compiler; передача policy/resources в execution |
| `plugins/chain/controller/include/.../types.cppm` и новый узкий `execution_config.cppm` при реальной необходимости | Schema конфигурации execution; не расширять catch-all DTO произвольными runtime types |
| `libraries/chain/execution/CMakeLists.txt`, root CMake/dependency configuration | optional installed Forge optimizing component; никакой сборки LLVM в host graph Spine |
| `programs/spineos/CMakeLists.txt`, `main.cpp` | минимальная composition/install wiring, если требуется; без собственного process manager |
| Dockerfile, compose/config examples, install/package definitions | helper и runtime dependencies только там, где включён OC; отдельный writable cache directory |
| execution/controller/unit/integration/deployment tests | backend matrix, failure/restart, native Linux x86-64 и macOS ARM64 приёмка |
| архитектурные/donor/deployment docs и AGENTS | обновить точные границы и фактические capabilities вместе с реализацией |

Новые component paths являются proposal. Перед кодом применить актуальные
Forge `create-library` и `create-plugin`; не копировать устаревший descriptor
layout, если параллельная Forge работа уже изменила его в dev.

## 4. Runtime configuration

Предлагается локальная секция `plugins.chain.controller.execution`.
Она не является полем genesis и не меняет chain ID.

```yaml
plugins:
  chain:
    controller:
      execution:
        backend: interpreter
        prepared-cache-entries: 32
        optimizing:
          mode: auto
          cache-directory: /var/lib/spine/wasm-cache
          cache-max-bytes: 1073741824
          cache-max-entries: 4096
          compile-workers: 1
          max-queued: 64
          max-queued-bytes: 67108864
          compile-timeout-ms: 30000
          compile-cpu-seconds: 20
          compile-memory-bytes: 536870912
          generated-code-max-bytes: 16777216
          shutdown-timeout-ms: 5000
```

Это проектируемая Schema, пример не является готовым конфигом текущей версии.
Числа — начальные проверяемые defaults/budgets, подлежащие калибровке на
реальном корпусе. Они ограничивают compilation локально и не заменяют
consensus WASM limits или transaction/block deadlines.

Выявлено до запуска helper: пример `compile-memory-bytes: 536870912` пока
непригоден как лимит виртуального адресного пространства. Используемый
каноническим parser allocator резервирует 1 GiB через `use_default_memory()`;
дополнительно нужны mapping LLVM/loader и рабочие данные. Перед публикацией
рабочего примера требуется измерить реальный baseline и отказ по лимиту на
обеих платформах либо использовать уже существующий подходящий allocator
path. Нельзя молча увеличить заданный оператором лимит или назвать `RLIMIT_AS`
жёстким ограничением RSS (физической памяти процесса).

Base runtime и tier-up — независимые настройки, как у Spring. `backend`
выбирает `interpreter` или существующий non-OC `jit` там, где он поддерживается.
`optimizing.mode` выбирает политику `auto`, `all` или `none`:

- `none`: только base runtime; compiler не запускается.
- `all`: OC рассматривается для всех contract executions; cold miss запускает
  bounded background compilation, вызов пока исполняет base runtime.
- `auto`: OC разрешён для system/явно разрешённых контрактов, read-only и
  применения блоков на ноде, не являющейся producer. Остальное использует
  base runtime. Это donor role/context policy, не эвристика количества вызовов.

Spring `xsat`/`vaulta` account suffix defaults являются донорской продуктовой
политикой и в Spine не копируются. Точные Spine config names/default whitelist
фиксируются с Schema и тестами. `mode: auto` в примере — целевое поведение
OC-enabled поставки после приёмки, не заявленная возможность текущего бинарника.
Interpreter-only build поддерживает `none`; явно запрошенный недоступный OC
даёт configuration error, а не молчаливое игнорирование настройки.

Forced OC — отдельная developer/test возможность, а не основной production
сценарий (Spring также обозначает `eos-vm-oc-forced` development-only).
В нём cold miss требует bounded preparation до исполнения без скрытого fallback.

Следующий порядок относится только к forced OC / explicit preparation-required
miss. Весь такой compile/preparation wait ограничен исходным deadline. Нельзя
ждать compiler, удерживая DB writer/apply slot на неопределённый срок:
готовый artifact запрашивается без ожидания внутри execution. При таком miss
откатывается весь enclosing attempt; owned preparation request обрабатывается
вне DB writer, после чего возможен ограниченный повтор с исходного состояния.
Предварительное чтение только pre-block state недостаточно: same-block
`setcode` и динамический nested target могут обнаружиться уже после effects.
Если forced OC включён в тесте production, сначала завершается вся production session с rollback,
сохраняется mempool и пропускается текущий слот; одного savepoint недостаточно.
В штатных `auto`/`all` cold miss этого rollback/slot skip нет: запускается
background compilation, а текущий вызов продолжает base runtime. Отдельный
completion interrupt допускается только по условиям раздела 5.

Нужна явная локальная operational category в controller, production и
scheduled/deferred paths. Нельзя маскировать её как deadline либо обычный
`wasm_execution_failed`: последний считается объективным отказом, а неизвестная
Forge exception сейчас fault-ит controller. Сбой compiler не доказывает
недопустимость блока и не помечает candidate invalid в fork graph.

Успешность tiered compilation не влияет на validity/consensus. Для warm/cold
ветвей сохранить Spring CPU billing exclusions; block deadline не ставится
на паузу. Конфиг проверяется до старта production, включая budgets, доступный
backend/helper/CPU и cache path. Недоступный явно выбранный backend даёт
понятный отказ конфигурации, а не подменяется interpreter.

Base runtime сохраняет interpreter на ARM64; существующий non-OC JIT не
становится ARM64 JIT автоматически. OC-enabled Linux/macOS поставка получает
донорскую tier-up policy независимо от base runtime. Default changes должны
быть явно отражены в generated config/release notes после приёмки; одно
наличие compiler executable в image не является runtime policy.

## 5. Execution flow и совместимость

1. Admission/setcode использует прежнюю canonical validation Forge и chain
   feature/whitelist policy. Компилятор не разрешает дополнительные opcodes
   или imports от собственного имени.
2. Execution находит code identity, snapshots compile-affecting policy и
   получает owned WASM для cache miss.
3. Backend выбирается до host side effects. При разрешённом tier-up cold miss
   получает ограниченный по числу и размеру admission, текущий вызов идёт в
   base runtime. После worker-side проверки identity допускается не более
   одной helper compilation для этого ключа; повторные ещё не проверенные
   admissions и последующие aliases отдельно учитываются в общих лимитах.
4. Artifact ready допускается после проверки identity/ABI/options и только
   для ещё актуального code. Поздний результат после setcode/reorg может
   остаться reusable cache entry, но не меняет active contract binding.
5. Исполнение использует те же intrinsics, authorization, context-free,
   privileged/read-only ограничения и ресурсы, что текущий host.
6. Если OC стал ready для той же исполняемой code identity во время долгого baseline execution, применяется
   донорский completion interrupt только при applying-block с ожидаемыми
   billed receipts (`attempt.expected` в Spine), доступном reversible
   savepoint, не read-only, implicit или scheduled/deferred/onerror transaction.
   Наличие block context или DB writer само по себе недостаточно. Порог Spring — не раньше
   500 ms от постановки compilation в очередь; compiler completion сам по
   себе не делает обычный execution timeout основанием для retry.
7. Специальный operational outcome обрабатывает владелец транзакции: полный
   rollback/reset effects, traces и accounting, затем не более одного retry
   всей транзакции с теми же expected CPU/net receipts и исходным абсолютным
   block deadline; replay не становится measured-CPU исполнением. Не только retry
   последней action; не повтор на уже изменённой БД; не пометка блока invalid.
   Отсутствие undo, обычный trap/host exception и исчерпанный retry не обходятся.
8. Результат проходит прежний receipt/state path. Controller выполняет
   commit/rollback/revision. Optimizer ничего не фиксирует сам.

Exact donor trace: Spring `wasm_interface_private.hpp:115-193` (выбор и
completion interrupt), `include/eosio/chain/apply_context.hpp:603`
(`is_applying_block()` = `explicit_billed_cpu_time`),
`transaction_context.cpp:58-92,348-385` (reset/one retry),
`apply_context.cpp:1087-1105` (auto policy),
`webassembly/runtimes/eos-vm-oc/code_cache.cpp:152-222,270-431` (async admission,
disk cache и restart). Все пути относительно `libraries/chain/` на указанном SHA.

Покрыть `apply`, notifications, inline/context-free actions, deferred/onerror,
simulation/read-only и nested `__forge_call`, в том числе return data и
ошибки вложенного вызова. Донорский artifact хранит `apply_offset`; его
механическое копирование не покрывает exports, уже поддержанные Spine.
Forge executor должен иметь нейтральный typed export interface.

Переключение backend между вызовами не должно сохранять mutable memory/globals
прошлого исполнения. Trap/host exception/deadline после начала OC возвращается
в прежний error/rollback path; повтор action через interpreter запрещён.

Побайтовая эквивалентность относится к консенсусным данным и результатам
контрактов. Wall-clock duration, generated machine code и локальная величина
измеренного CPU времени между engines не обязаны совпадать. Отдельно проверить
правила billing, limits и воспроизведения уже записанных block receipts.

### Производительность: обязательная донорская граница

Уточнение пользователя 2026-10-13: поддержка OC не считается готовой, если
фоновые compilation jobs ухудшают обработку блоков из-за ошибок интеграции.
Сходство API или успешная компиляция не доказывают требуемую производительность.

- Штатные `auto`/`all` не ожидают LLVM, свободного compiler worker или места
  в compilation queue на execution/producer пути. Cache miss, переполнение
  очереди и временная недоступность compiler оставляют текущий вызов на base
  runtime; эта деградация ограниченно диагностируется. Ожидание существующего
  prepared-module baseline не следует путать с ожиданием optimizing compiler.
- Не более одной verified helper compilation на compatible code identity.
  До worker-side проверки допускаются повторные bounded unverified admissions;
  их validation CPU/bytes и alias tickets учитываются отдельно, а не выдаются
  за бесплатный cache hit. Начальное число concurrent workers
  — один, как у Spring. Приоритет разрешённых контрактов при applying block
  переносится из donor scheduling; собственный общий scheduler не создаётся.
- Compiler работает вне controller compute critical path и не держит DB
  handles/locks. CPU/memory/output budgets ограничивают helper. Фоновый процесс
  всё равно конкурирует за CPU, memory bandwidth и диск: слово «async» не
  является доказательством отсутствия этой цены.
- Warm hit не запускает parser/LLVM или disk write заново; active artifact
  защищён от eviction. Restart с compatible cache не вызывает массовую
  компиляцию уже сохранённых contracts. Не копировать весь cache для invocation.
- Не отключать проверку authority, WASM validation, billing, deadline или
  state verification ради скорости. Ускорение измеряется после parity.
- Отдельный hot-loop gate проверяет стоимость проверки deadline. Ранний draft
  с C callback на каждом taken backedge заменён reviewed ABI/codegen 2:
  generated code делает один acquire-load epoch и сравнение на входе и
  backedge, а callback оставляет редкому пути. Это принятие compiler slice,
  не доказательство runtime lifetime или производительности. Проверить цену
  этой проверки, ограниченное время отмены и независимость concurrent
  invocations на реальном executor и затем Spine.
- Системная подготовка Darwin recovery принадлежит execution worker startup,
  а не каждому `apply`: warm invocation не создаёт receiver/thread и не
  переустанавливает Mach ports. Использовать имеющиеся compute-pool
  `on_worker_start`/`on_worker_stop`; partial startup failure требует cleanup
  при выходе thread, поскольку stop hook в этом случае не гарантирован.
- Текущий baseline `run_with_budget` создаёт `interpret::watchdog`, чей guard
  запускает отдельный `std::thread`. Это существующая стоимость, не образец
  нового OC lifecycle: для интеграции использовать существующее планирование
  Forge, не добавлять второй watchdog/pool и не повторять запуск потока на
  каждый OC invocation. Изменение проверяется также на baseline interpreter,
  чтобы новая interruption policy не ухудшила его поведение.

Donor anchors на фиксированном Spring SHA:
`code_cache.cpp:97-108,152-222` — worker cap, dedup и queue priority;
`config.hpp:16-35` — budgets и default one worker;
`compile_trampoline.cpp:190-209` — child resource limits. Первые и последние
пути относятся к `libraries/chain/webassembly/runtimes/eos-vm-oc/`, header —
к `libraries/chain/include/eosio/chain/webassembly/eos-vm-oc/`.

Приоритет донора имеет конкретный смысл: `wasm_interface_private.hpp:143-149`
выделяет разрешённый контракт при применении блока, чтобы validators успевали
подготовить его до ускорения producer. `code_cache.cpp:207-216` ставит такую
работу впереди обычной очереди, не увеличивая число compiler workers. В Spine
роль ноды и разрешённость контракта остаются продуктовой политикой; Forge
получает нейтральный приоритет в существующем scheduler/session owner, не
вторую очередь. Тест насыщения должен проверять не только возврат к base
runtime, но и выбор приоритетной ожидающей работы без превышения общего
лимита jobs/bytes. Это требование интеграции, пока не реализованное поведение.

Для interruption проверен также конкретный hot path донора:
`LLVMEmitIR.cpp:428-444` создаёт loop без перехода в C++ callback на каждой
итерации; `executor.cpp:228-245` по expiration снимает execute permission
через `mprotect(PROT_NONE)` с mapping конкретного executor. `checktime()`
вызывается перед входом и после соответствующего выхода (`:247,270`), а не
из каждой итерации. Это объясняет отсутствие цены callback на обычном loop,
но не разрешает буквально менять защиту общей Forge artifact mapping:
параллельный invocation не должен получить чужую отмену. Точная механика
Forge должна сохранять этот lifetime/ownership инвариант. Inline epoch-проверка
ABI2 прошла отдельное compiler review; runtime и измерения остаются отдельными
незавершёнными gates.

Для следующего отдельного interruption contract проверен Wasmtime на
`05e7deebf1ddcc726bfc4c30cd87e3dbb2881072`:
[`func_environ.rs:808-959`](https://github.com/bytecodealliance/wasmtime/blob/05e7deebf1ddcc726bfc4c30cd87e3dbb2881072/crates/cranelift/src/func_environ.rs#L808)
проверяет epoch при входе в функцию и в loop, оставляя callback редкому пути;
[`engine.rs:886-922`](https://github.com/bytecodealliance/wasmtime/blob/05e7deebf1ddcc726bfc4c30cd87e3dbb2881072/crates/wasmtime/src/engine.rs#L886)
содержит общий atomic counter, обновляемый embedder. Проверки только loop
недостаточно для глубокой разветвлённой цепочки вызовов. Этот узкий pattern принят
для Forge ABI2 после согласования compiler/runtime владельцев и review.
Wasmtime остаётся донором механики, не вторым движком или целью полной
совместимости. ABI2 compiler tests не заменяют приёмку runtime lifecycle.

В Forge tick должен использовать существующий Asio runtime/scheduler, без
нового потока на invocation. Plain deadline/stack limit не становятся
cross-thread flags. Адресная отмена и lifetime invocation должны сохраняться
независимо от общего tick. В частности, `libraries/app/application_shell.cpp`
в `stop_execution()` вызывает scheduler `request_stop()` до compute shutdown:
остановленный tick не может быть единственным способом завершить бесконечный
guest. Обязательны независимая публикация stop до join, сохранение состояния
до выхода последнего guest, tests nested/parallel cancellation и shutdown.

Донорский `unittests/eosvmoc_interrupt_tests.cpp::wasm_interrupt_test`
проверяет ровно одно compilation-completion interruption и сохранение
обычного transaction deadline после повторного исполнения. Перенести этот
смысл в Spine harness без донорского tester runtime; дополнить проверкой
отката уже выполненных DB effects и отсутствия повторных trace/receipt
записей, отдельными отрицательными cases для scheduled/implicit/no-undo и
обычного timeout. Одного успешного warm-вызова для этого недостаточно.

Обязательная benchmark matrix на одинаковом hardware, corpus, compiler build
type и budgets: base-only; cold tier-up; warm tier-up; restart с cache;
множество новых code identities под concurrent block load; compiler crash и
queue saturation. Записывать throughput, p50/p95/p99 latency, block-apply lag,
пропуски production slots, compile queue/wait, CPU/RSS, cache hit/eviction и
end-to-end replay. Отдельно сравнивать Linux Spine/Spring на общем совместимом
corpus и внутри Spine на Linux/macOS; Spring Linux-only donor не превращается
в неподтверждённый macOS benchmark.

Указывать raw samples, разброс, warmup, число повторов и условия измерения.
Падение вне установленного экспериментом шума разбирается до принятия;
нельзя объявить «не хуже Spring» по ускорению одного пустого контракта,
только warm-case либо эмулированному Linux. Допустимый trade-off нельзя скрыть
произвольным post-hoc порогом: он требует явного согласования до acceptance.

### 5.1. Исполнимый порядок сравнений

Сравнение сначала отделяет стоимость VM от стоимости всей ноды. На одном и том
же неизменённом WASM/входе проверить конечный вычислительный цикл, частые host
calls, memory growth, косвенные вызовы и численные операции. Затем запускать
contract/transaction/replay нагрузку: быстрый VM microbenchmark не доказывает
скорость controller, DB или propagation. Бесконечный `test_checktime` донора
служит oracle отмены, а не образцом полезной пропускной способности.

Spring `tests/PerformanceHarness/performance_test_basic.py` и `log_reader.py`
дают донорскую схему producer/validator, заданной нагрузки и проверки числа
завершённых транзакций. Их `nodeos`/`cleos` orchestration, разбор строк журналов
и собственный критерий допуска TPS не переносятся в Spine как новый runtime
или универсальный transport adapter. Использовать существующие Spine execution,
validator и deployment harnesses; отсутствующая измеренная метрика остаётся
NOT_RUN, не вычисляется из предположения о формате донорского лога.

Для каждого сравнительного прогона:

1. Зафиксировать SHA исходников и binaries, compiler/build type, CPU/OS,
   одинаковые WASM bytes и action inputs, лимиты VM/CPU и состояние кэша.
   Локально изменённый donor checkout не является чистым pin: его посторонние
   изменения не сбрасываются и не включаются незаметно в эталон.
2. Прогнать correctness oracle до timing: результаты, ошибки, host effects и
   данные контрактов совпадают. Биллинг и подписи не отключаются для скорости.
3. Разделить base-only, пустой native cache, прогретый native cache и новый
   процесс с сохранённым disk cache. Очистка OS page cache не подразумевается;
   cache helper/compiler и kernel file cache не называются одним «cold».
4. Чередовать парные запуски Spring/Spine на одном native Linux runner, сохраняя
   порядок и все сырые samples; минимум пять пар после отдельного warmup.
   Не запускать одновременно сборку или другой benchmark. На macOS ARM64
   сравнивать base/tier-up Spine нативно; Linux под эмуляцией не заменяет
   native Spring comparison.
5. Отдельно измерить конкуренцию фоновой компиляции с block load, полную очередь,
   restart и отказ helper. Сохранять offered/completed/failed work, а не только
   TPS успешного подмножества. Проверять, что cold auto/all не ждёт compiler
   под DB writer и что warm hit не создаёт новый процесс.
6. Показать распределения и парные отношения, а не только лучший результат.
   Если разброс/нагрузка shared runner не позволяет сделать вывод, результат
   INCONCLUSIVE; это не performance PASS. Не выбирать допустимый процент
   регрессии после просмотра результатов.

Один разрешённый целевой Linux CI-прогон должен собирать и проверять точные
кандидаты Forge/Spine и pinned Spring в рамках одного native runner для
сравнений. Он запускается только после local correctness/review; подготовка
этого плана не означает запуск CI или разрешение дополнительных workflows.

Read-only build preflight pinned Spring `e6a99f68b67abc4d89fe716755b2e1394a4991f7`:
его root `CMakeLists.txt` разрешает OC только на Linux x86-64 и требует LLVM
7–11; штатный `.cicd/platforms/ubuntu22.Dockerfile` устанавливает `llvm-11-dev`.
Поэтому Forge LLVM 22 и donor LLVM 11 фиксируются как реальные различные
toolchains, а не скрываются под заявлением об одинаковом compiler. Не менять
донорскую проверку версии ради сборки benchmark. При отдельных build containers
сравнение всё равно выполняется на одном native runner с одинаковыми CPU/memory
ограничениями; userland и dynamic-library версии входят в evidence. Это
сравнение продуктов, не изолированное доказательство выигрыша одного LLVM pass.
Найденный Spring `tests/PerformanceHarness/performance_test_basic.py` служит
донором разделения offered/completed/failed work и сохранения raw artifacts;
его nodeos-specific orchestration не копируется вторым framework в Spine.

### 5.2. Общий корпус и измеряющий тестовый слой

Read-only preflight выявил несовместимость готовой нагрузки, не ошибку VM:
Spring `tests/PerformanceHarness/cpuTrxData.json` на указанном pin вызывает
`eosmechanics::cpu()` с пустыми `action.data`, а существующий Spine
`action_factory` всегда добавляет восьмибайтовый sequence. Этот CLI нельзя
выдать за запуск точно того же корпуса. WASM/ABI донора не меняются ради теста.

Для такого корпуса тест реализует существующий `transaction_factory` и
использует прежние `chain_port` и `generator`. Основная action сохраняет пустые
данные; уникальность задаётся отдельной context-free action `eosio.null::nonce`,
как в pinned Spring `tests/trx_generator/trx_generator.cpp`. Одинаковые заранее
заданные nonce strings пакуются существующим Forge Raw; golden bytes сверяются
с донорским FC encoding. На обеих сетях `eosio.null` должен существовать и не
иметь WASM. Source preflight подтверждает этот путь в обеих реализациях, но
реальная совместимость пока не проверена. Chain IDs, TAPOS и подписи различаются:
равными заявляются WASM/ABI, action/CFA bytes и полезная работа, не все packed
transactions.

`latency_metrics` текущего генератора хранит count/total/min/max/average,
не распределение; p95/p99 из этих значений получить нельзя. Для приёмки
достаточен тестовый decorator существующего `transaction_port`: он без новых
запросов и повторов делегирует `prepare/submit/await`, сохраняя timeout,
результат, исключение и отмену. Заранее ограниченный журнал учитывает каждую
попытку по sequence и затем ID, включая ошибки подготовки без ID. Переполнение,
дубли или неполная сверка с отчётом генератора — ошибка измерения, не разрешение
отбросить медленные результаты. Наблюдатель живёт до завершения всех операций;
запись ошибки не должна заменить исходное исключение.

Эти timestamps показывают клиентское наблюдение ответа. Если первый ответ уже
`finalized`, из него нельзя восстановить отдельные времена фактического
`accepted/included`. Публиковать распределения наблюдаемых успешных исходов
вместе со всеми отказами, отменами и незавершёнными попытками. Время VM, первого
BASE-вызова, compiler queue и cache restart измеряется отдельно. Negative
controls: prepare failure, rejection, неверный/повторный ID, timeout/cancel,
немедленный finalized response и исчерпание ёмкости журнала. Этот раздел —
согласованный дизайн тестов; измерения и реализация ещё не выполнены. Новый
production API метрик или второй scheduler генератора не требуется.

## 6. Lifecycle, хранение и диагностика

- initialize: validate configuration/package capabilities и открыть bounded
  cache; start helper/session до допуска execution, которое требует OC.
- request_stop: запретить новые compiler requests; остановить admission
  новых execution jobs обычным controller mechanism.
- shutdown: отменить/drain compiler jobs и execution references в проверенном
  порядке, дождаться child exit, затем закрыть mappings/cache. Shared runtime
  остаётся доступен до окончания awaitable cleanup.
- Не терять отмену при ожидании IPC/queue; протестировать shutdown во время
  compile, cache write и исполнения. Отдельный OS process не отменяет
  требования bounded application shutdown.
- Cache хранится отдельно от `state`, `block` и `signing`. Не включать его в
  chain snapshot/Revision; при restore он проверяется или пересоздаётся.
- Кэшируется скомпилированный код, а не результат action/изменения БД. Warm
  invocation не компилирует тот же compatible artifact повторно; compatible
  disk cache переживает restart. Eviction, новый WASM, несовместимый compiler,
  OS/CPU/ABI или повреждение требуют безопасного rebuild. Mutable memory и
  globals всегда получают исходное состояние invocation.
- Существующие Admin DB, wallets, pairing/session и signer profiles не
  мигрируют из-за добавления OC.

Логи — `chain.execution` и Forge VM logger: startup capabilities на info,
ограниченные compiler failures на warning, runtime integrity fault на error,
cache/compile transitions на debug. Существующее покрытие не дублировать.
Диагностика обязана различать selected backend, фактически использованный
backend, pending compilation и причину fallback/unavailable.

## 7. Сборка и deployment

Forge устанавливается обычным package workflow. Spine запрашивает optional
`vm_wasm_optimizing` и связанную helper installation; LLVM headers не
становятся требованием всех четырёх программ.

- Новый внутренний `forge-wasm-compiler` входит в Linux x86-64 spineos image
  при включённом OC; он стартует под владельцем ноды через Forge session.
- Native macOS ARM64 package содержит тот же helper role с Darwin binary,
  проверенной code-signing/executable-memory конфигурацией и отдельным cache.
  Docker на macOS остаётся Linux, он не проверяет Darwin execution path.
- `spinead`, `spinekd`, `spinectl` не получают compiler process/lifecycle.
- Compose использует те же spineos service и image role, добавляет только
  конфигурацию/отдельный cache mount. Пятый product daemon/image не появляется.
- Проверить runtime dynamic libraries, executable relocation, права cache,
  uid и работу в стандартном контейнере без privileged mode.
- Native code/cache не включается в genesis, bootstrap bundles или contracts.
  S/4, public system-contract bundle и minimal bios bundle сохраняются.
- Helper/cache format version mismatch после upgrade вызывает bounded rebuild
  или явный startup error в строгом режиме, не исполнение старого artifact.

Ветка Forge основана на dev новее текущего pin Spine. Перед поиском OC-багов
отдельно проверить Spine на выбранном новом Forge baseline с OC выключенным.
Независимый сетевой regression записать и передать владельцу; не маскировать
изменением OC tests и не смешивать незапрошенный P2P repair с этим портом.

## 8. Обязательная приёмка Spine

| Группа | Проверка |
|---|---|
| Existing behaviour | interpreter-only build, current native suite, public/bios bootstrap и already_completed |
| Execution parity | тот же WASM corpus и system contracts на interpreter/JIT/OC; state rows, return data, action receipts, RAM/NET и typed errors |
| Policy | rejected setcode/imports/features, permissions, context-free/read-only; compiler не расширяет допустимость |
| Billing/deadlines | cold compile исключён из billable CPU по прежним правилам; block deadline действует; interrupt/trap приводит к rollback |
| Donor tier-up | auto/all/none по роли/context; cold baseline без ожидания compiler; ready OC; completion interrupt и ровно один full-transaction retry; исключения implicit/deferred/no-undo |
| Lifecycle | cold/warm restart, worker crash/hang, full queue/disk, cancel/shutdown, cache corruption/version mismatch |
| Chain state | setcode во время compilation, fork/reorg, snapshot restore, активный artifact во время eviction |
| Nested calls | interpreter/OC combinations, memory/register restoration, return data и exceptions |
| Node integration | actual spineos config, production, replay, simulation; engine selected через обычный controller |
| Mixed network | минимум три узла с base-only/tier-up/forced-test OC; одинаковые finalized block IDs и state roots, последовательные рестарты; общий block replay на native Linux x86-64 и macOS ARM64 |
| Packaging | installed package без исходников, relocation helper, свежий OCI image, no privileged container, missing-helper diagnostics |

Сравнение chains не строить на независимом producing одинаковых transaction
с требованием совпадения измеренных CPU receipts. Использовать общий block
stream/replay и отдельно проверять production/billing semantics.

Использовать и расширить текущие targets:
`spine_chain_unit_tests`, `spine_plugins_chain_controller_unit_tests`,
`spine_spring_execution_compatibility_tests`,
`spine_system_contracts_host_tests`, `spine_system_contracts_production_tests`,
существующие validator/deployment harnesses. После регистрации собрать targets
с `-j 4`, проверить CTest discovery и выполнить конкретную backend matrix.

Финальный прогон: свежее итоговое дерево, native Linux x86-64/macOS ARM64 OC, полный
актуальный native Spine registry и Docker runtime scenarios. Сохранить SHA,
donor/compiler/CPU, image digests, конфигурации без секретов, исходные logs,
state/receipt comparisons и timings. PASS одной платформы не закрывает другую.

## 9. Последовательность и готовность

1. В Forge phase 0 собрать минимальный installed consumer под требования
   Spine `apply` и `__forge_call`, imports, deadlines и cancellation.
2. Реализовать Forge compiler/executor/session/cache с независимым review.
3. Создать Spine integration branch от свежего dev и зафиксировать точный
   проверяемый Forge SHA. Выполнить interpreter baseline на новом pin.
4. Добавить controller-owned configuration/lifecycle и все execution paths.
5. Пройти unit/donor/parity, затем Linux Docker/mixed-node и benchmarks.
6. Review итогового diff обоих репозиториев, remediation и повторные
   затронутые проверки. Обновить README/architecture/donor/deployment docs.
7. Сначала опубликовать проверенный Forge в dev, затем exact pin и проверенную
   Spine интеграцию. Проверить ancestry, triggers и итоговый diff; без force,
   PR-triggered workflows, tags и release. Единственное разрешённое исключение
   CI — один целевой ручной OC acceptance run на native Linux x86-64 для обоих
   точных SHA после локальных проверок и review (согласовано 2026-10-12).

Foundation при согласовании архитектуры получает краткое cross-product
решение со ссылками на engineering documents и verified commits. Не
переписывать статус existing Foundation документов как accepted автоматически.

Готовность всего этапа означает работающий OC в spineos, а не только сборку
библиотеки: локальные gates итоговых деревьев PASS, no reportable findings у
независимого reviewer, артефакты сверены и все обязательные сценарии выполнены.
Открытый LLVM/process/host-ABI вопрос, NOT_RUN или работа только interpreter
означают незавершённый этап.
