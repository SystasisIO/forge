# JOB-4 source review R1

Дата: 2026-10-25. Независимая проверка исходников до runtime; сборки/тесты не запускались.
Forge worktree `forge-tlsnotary-jobs3-v1`, branch `crypto-exchange-blocks-v1`,
base `07267d4d13f56167ebba1482009978f0f649be9d`; Turbine worktree
`turbine-jobs45-v1`, base `baaa45a36df53f2366ac77c529d80cf1700fd80d`.
Новые untracked-файлы exchange прочитаны напрямую; одного `git diff` недостаточно.
Применены Forge `create-library` и `create-contract-library`.

## Итог

**Есть одно обязательное исправление до source qualification:** nested Raw decode
manifest допускает выделение памяти для значительно более чем 16 блоков.
В проверенной bounded exchange-криптографии ложный refund — возврат резерва —
или обход source commitment не обнаружен. Runtime, настоящий WASM и installed
consumer по этой проверке — **NOT_RUN**.

## Finding

**[P2] Ограничить число `manifest.blocks` до выделения памяти.**

В [jobs.cpp:177](/Users/vladimirtarnakin/.openclaw/workspace/Projects/Storlane/worktrees/turbine-jobs45-v1/guest/contracts/turbine.jobs/jobs.cpp:177)
`unpack_bounded_exact<manifest>` получает только `.max_total_bytes=2048`.
Это не ограничивает allocation для `std::vector<block>`: Raw
[codec.cppm:134](/Users/vladimirtarnakin/.openclaw/workspace/Projects/Storlane/worktrees/forge-tlsnotary-jobs3-v1/libraries/raw/include/forge/raw/codec.cppm:134)
списывает лишь число container elements, чей default равен 1,048,576, а затем
[codec.cppm:633](/Users/vladimirtarnakin/.openclaw/workspace/Projects/Storlane/worktrees/forge-tlsnotary-jobs3-v1/libraries/raw/include/forge/raw/codec.cppm:633)
вызывает `value.resize(size)` **до чтения элементов**. `max_total_bytes` расходуется
только через `require_byte_allocation`, не через этот non-byte vector path.

Manifest prefix с обычными 100 bytes фиксированных полей и трёхбайтовым
varint count=1,048,576 укладывается в 2048-byte payload, но пытается выделить
десятки MiB памяти прежде, чем stream read/`exchange::validate` отвергнет вход.
Ограничение `blocks.size()<=16` в `exchange.cpp:91` выполняется слишком поздно.
На `arm` нужны обе подписи, однако участники собственных заказов могут их создать;
это нарушение заявленного ограниченного contract checker, даже в test deployment.

Задать для этого decode явные `.allocations.max_container_elements=16`,
`.max_total_container_elements=16`, `.first_container_elements=16` с сохранением
byte budget; то же правило применять в host decoder этого профиля. Нужен тест
с большим count при коротком входе, который отклоняется по allocation admission.
Для `fault.ciphertext` byte budget=4096 уже даёт реальную границу до allocation;
дополнительно можно сузить её до 2080, это не отдельный blocker.

## Подтверждённые свойства исходников

- `exchange.cpp:41–63`: pad input имеет ровно 64 bytes: BE256 counter, затем
  32-byte key. Counter=`65*index+offset/32`; `index<16`, не более 65 words.
  Счётчики уникальны, максимум 1039; верхние 24 counter bytes нулевые.
  Native вызывает существующий `sha3::hash(...,false)` (Keccak), guest —
  существующий `contract::keccak`. Новая реализация hash или intrinsic не добавлена.
- `exchange.cpp:37–39,102–107`: plaintext+16-byte blinder дополнены нулями
  до кратности 32; максимальный ciphertext=2080 bytes. Последний короткий блок
  использует тот же stride 65 и не пересекается с другими блоками.
- `exchange.cpp:66–81`: manifest/index/witness cap, key SHA256 и ciphertext
  SHA256 проверяются раньше, чем функция может вернуть доказанное нарушение.
  Неверный ключ/чужой ciphertext дают `invalid_input`, не `proves_fault=true`.
  Аутентифицированный ciphertext с неверной длиной, nonzero padding или неверным
  `SHA256(plaintext||blinder)` даёт доказанное нарушение.
- `exchange.cpp:86–100`: поддерживается только format=1, непустые bindings,
  1…16 blocks, все блоки кроме последнего строго 2048, последний 1…2048;
  source/ciphertext digests непустые. После исправления decoder это bounded path.
- `exchange.cpp:110–122`: `open` возвращает только body bytes после успешной
  проверки и удаляет blinder/padding из возвращаемого результата.
- `exchange.cppm:30–39,70–72` и `policy.hpp`: native errors используют штатные
  Forge categories, guest — штатный assertion policy без native exceptions.
  Диагностика не включает key/plaintext/ciphertext. Exception tags guest не
  создают вторую runtime-иерархию.
- `jobs.cpp:112–150`: профиль 2 ограничен immutable `test_deployment`; version,
  jobs code hash и parameters hash проверяются, verification/exchange совпадают.
  `synthetic_enabled` остаётся admission gate новых заказов; checker принятых
  receipts не зависит от последующего выключения admission.
- `jobs.cpp:173–183,682–748`: manifest context связан с полным jobs context;
  подписи executor/verifier покрывают точный offer со всем manifest. Domain,
  chain/deployment/job/attempt/terms, profile, maximum, generation и сроки
  проверяются до записи receipt. Идентичный повтор сохраняет первоначальные сроки.
- `jobs.cpp:751–851`: reveal проверяет закреплённый SHA256(key), после этого
  challenge использует сохранённый key и точный accepted manifest. Неверная
  жалоба не создаёт receipt и не возвращает резерв. Поддерживается только kind=1.
- Public jobs header не изменён; внешняя wire/schema v1 не расширена. Новый payload
  находится в существующих bounded bytes профиля 2. Common exchange layout имеет
  отдельный format=1; Raw/native/WASM равенство пока требует runtime проверки.
- Forge guest registry: `forge.crypto.core` предоставляет только существующий
  value module `core.types`; native secret/random/OpenSSL реализации в guest
  не включены. `forge.crypto.exchange` получает отдельно скомпилированный guest
  archive из того же `exchange.cpp`, существующие crypto/raw/contract modules.
  Native package component добавлен отдельно; монолитного crypto target нет.

## Не подменять следующую приёмку

Manifest `source`/`evidence` не аутентифицируют TLS сами по себе. Проверенный здесь
контракт ожидаемо доверяет закреплённому verifier key. До подписи host verifier
обязан независимо сравнить **весь** evidence/context, ordered digests/ranges,
origin/pin/lengths, точные HTTP headers/request semantics и полное покрытие body;
проверить и долговременно сохранить все ciphertext blocks с ожидаемой длиной.
Эта host-политика данным source review не проверена; opaque seller hashes
по-прежнему недостаточны. Fresh one-time key и защищённая доставка ciphertext
также остаются обязательствами host owner.

Следующие обязательные проверки: donor/native/WASM pad vectors для
0/64/65/1039; SHA256/Keccak distinction; настоящие proofs для corruption/blinder/
padding/wrong length; подложные proofs; forged Raw counts; повтор/context/expiry;
максимальный manifest и witness; worst-case WASM ресурсы; fresh installed native
и standalone guest consumer. Linux, real Labs и commercial activation исключены.

## Проверенный source snapshot (SHA256)

| Файл | SHA256 |
| --- | --- |
| Forge `exchange.cpp` | `5810e2688771e4a32e3ac2c75eb5768e41084cf6d630113e390ae60c2e4193f8` |
| Forge `exchange.cppm` | `a9c4063b5d8a7bd9487ca6145d2da51bb269fea29ac9a2b5aa52e73d5059a02e` |
| Forge exchange CMake | `6b8d85d7a5a3c20b621b9d8301a90c3b2dcff4d584f7c8528d44b1f7b6a935d5` |
| Forge GuestComponents | `2ca831604e3ed7b514d17e5faa771488caabe39091e109c7b118a815e9242e4c` |
| Forge guest foundation CMake | `236ca26aa13c8922af11731d339688c72849595af2b954e94e9aef383f520079` |
| Forge guest CMake | `d464376a1a70deeea317c7bc1347f772b86a60f48fc7375932b8b3fa57f2fd45` |
| Forge crypto/core CMake | `9e998a053f67ca75e44c5817b2ccfaeafd15c44cbe321252ceac03fb10b36a7f` |
| Forge package config | `a9002b377b6fad903af23cc30f20a76fe2355083190576f5ad52e32a364775cb` |
| Forge root CMake | `974f0861d27846cd4b597408e8bdb078c7976c0be81a922b6d362ba76746771d` |
| Turbine `jobs.cpp` | `114f6a27988f8db2bd66d7f07f4dc7a34bbe17f66a722f16c99bdeb3796559cd` |
| Turbine jobs CMake | `cbe608664b7ea54ebde5d198a83578524fdcb1f3f4ae6e81b9426606d0d71607` |
| Turbine jobs header (unchanged) | `fb99f01d9651a7d432ab27fc937dd3fbe8d3f5ff3371430f5a2df35a30c92279` |

README/donor documents и новые тесты, над которыми ещё работала команда,
в snapshot review не включены. Этот агент изменил только данный артефакт.
