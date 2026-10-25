# JOB-4 source review R2 — исправление caps и source binding

Дата: 2026-10-25. Только чтение исходников; сборки/тесты не запускались.
Базы: Forge `07267d4d13f56167ebba1482009978f0f649be9d`, Turbine
`baaa45a36df53f2366ac77c529d80cf1700fd80d`; те же изолированные деревья, что R1.

## Решение

**R1 P2 исправлен в исходниках. Новых блокирующих ошибок реализации не найдено.**
Новый `bind_exchange` корректно связывает bounded ciphertext list с доверенным
результатом verifier при явно внешней аутентификации этого результата и его
контекста. Одна документационная оговорка ниже требует уточнения; проверка
источников не заменяет runtime/WASM qualification.

## Исправление R1

`jobs.cpp:177–183` задаёт до `vector<block>::resize` все три container caps=16,
byte cap=2048 и exact decoding. Count 17/1,048,576/UINT32_MAX теперь отвергается
до allocation элементов. `jobs.cpp:840–846` также ограничивает byte vector
`fault.ciphertext` до 2080 до allocation. Файл зафиксирован SHA256
`a8a64df24c41fb2e8a3baac4fb27add393995870953cb83ac6b11538acd892ae`.

## Замечание к trust contract документации

**[P3] Явно потребовать аутентификацию открытых данных и связи source с операцией.**

В Forge `libraries/net/tlsnotary/README.md:243–244` новая инструкция говорит
аутентифицировать retained records и проверить смысл visible request/headers.
Но retained `evidence` содержит только origin/pin/lengths/commitments
(`types.cppm:63–69`), а `result.sent`/`received` находятся вне него
(`types.cppm:116–125`). Подпись одного retained record поэтому не аутентифицирует
отдельно импортированные открытые request/header bytes.

`bind_exchange` не может определить, был ли обычный C++ `result` получен из
`engine.verify`, сконструирован вызывающим кодом или восстановлен из чужого файла.
Он также сравнивает expected context только с offered context: freshness и
принадлежность source-сессии этой операции должны быть проверены снаружи.
Доверенная старая сессия с тем же body может иначе перепривязываться к новой
операции; изменённые, но такой же длины HTTP headers проходят структурную часть
helper. Это допустимая граница helper, но инструкции caller должны быть точными.

Нужно явно указать два допустимых источника: живой trusted `engine.verify`
result, уже связанный приложением с конкретной операцией, либо trusted persisted
полный verifier result/свидетельство, аутентифицирующее **также visible bytes,
полный retained record, доверенный operation context и replay/freshness policy**.
Одной подписи `retained` и последующего сравнения смыслов недоверенных headers
недостаточно. API comment `exchange.cppm:13` уже говорит `authenticate verified`;
README следует сформулировать с той же точностью. Новый notary/signing протокол
для этого helper не требуется.

## Подтверждённые изменения

- `net/tlsnotary/exchange.cpp:28–42`: manifest validated; independent expected
  context/origin; bounded expected name; sent length 1…16384; retained present,
  prover openings absent, visible sent present. Pin/format/origin/lengths и число
  retained commitments совпадают. Body begin 1…16384, body end равен полному
  received length, body size <=32768. Ровно один раскрытый received range `[0,h)`;
  bytes body verifier не получает.
- `exchange.cpp:43–56`: каждый commitment received-only, ровно один interval,
  contiguous ordered coverage, exact size и source digest; все ciphertext
  полностью присутствуют и имеют ожидаемые длины/SHA256. Индуктивно `next<=end`,
  поэтому `end-next` и `next+size` не переполняются. Последний next равен body end.
- `exchange.cpp:57–58`: Raw pack полного evidence выполняется **после** caps
  каждого сериализуемого поля: <=16 commitments, ровно один range у каждого,
  фиксированные digests, bounded origin, точный pin и bounded scalar lengths.
  Metadata, по которой helper не проходит, не сериализуется. Возврата большого
  nested-count allocation пути R1 здесь не обнаружено.
- `net/tlsnotary/CMakeLists.txt` добавляет реальный парный component source и
  public `crypto_exchange`/Raw dependencies; installed package closure в
  `cmake/ForgeConfig.cmake.in:595–605` добавляет те же компоненты. Новый framework,
  protocol, intrinsic, runtime или сторонняя crypto implementation не введены.
- Core exchange `.cpp/.cppm` побайтно совпадают с R1: counter/stride65,
  BE256+key, zero padding, hash-list authentication и различие foreign witness /
  доказанного fault не изменились.
- Contract profile/context/replay и checker после выключения admission по diff
  не изменились, кроме decoder budgets. Test-only admission сохраняется.
- README crypto exchange и donor traceability корректно отделяют этот restricted
  direct raw-response механизм от generic FairSwap circuit, Labs/JSON/locator,
  GPU proof, коммерческого разрешения, окончательности и settlement.

## Тестовые исходники и предел доказательств

В `tests/tlsnotary/exchange_tests.cpp:67–89,114–133` настоящий MPC result
получается из `engine.verify`; exact visible request/header policy проверяется
отдельно **до** helper и simulated authorization. Есть негативные source/range/
direction/context/origin/coverage/ciphertext cases и сравнение recovered body.
Это точная модель fixed-framing fixture; header parser общего назначения не заявлен.
Детерминированные ключи относятся только к fixture. Физическая изоляция двух
операторов, долговременное хранение ciphertext и реальная подпись arm этим
single-process fixture не подтверждаются.

Contract source содержит hostile manifest/fault counts, 16-block path, настоящие
source/padding/length faults, false witnesses, повторы и позднее выключение policy.
WASM wire fixture содержит независимые Keccak vectors 0/64/65/1039.

**Уточнение validation evidence:** `fixture::rejects` ловит typed
`tooling::testing::exceptions::assertion_failure`, но не различает ранний Raw
allocation refusal и поздний stream EOF после resize. Поэтому короткий forged
count test, который лишь должен быть rejected, сам не доказывает устранение R1.
Для runtime утверждения «до allocation» нужен expected allocation-admission
reason либо allocator/resource guard. По source order исправление подтверждено.

По этому review все runtime/native/WASM/install результаты — NOT_RUN.
Linux, real Labs и production activation исключены. Источники тестов прочитаны,
готовность/успешность тестов из наличия исходников не выводилась.

## Source freeze SHA256

| Файл | SHA256 |
| --- | --- |
| Forge crypto `exchange.cpp` | `5810e2688771e4a32e3ac2c75eb5768e41084cf6d630113e390ae60c2e4193f8` |
| Forge crypto `exchange.cppm` | `a9c4063b5d8a7bd9487ca6145d2da51bb269fea29ac9a2b5aa52e73d5059a02e` |
| Forge TLSN `exchange.cpp` | `2ce8172526064882280d81377e12af95cc2a70c59bf5e466584857f8171de4a5` |
| Forge TLSN `exchange.cppm` | `f3cb7e826a6dc9de82088e241c1e2e200b1808f63f7e9541401b63dcafac75a7` |
| Forge TLSN CMake | `ae0977779e8dcdc09cb60dbf491b9d248aaa67485e28387a7aa83406e99b25b5` |
| Forge package config | `f4c3b34635654842c23340f7b4d5655e8ec93dad205db1d611776319143a610b` |
| Forge TLSN README | `bfbe54ec22c395def355e7f4c792a8c4c02030c4dfcfe5052703b89d1847cddf` |
| Forge crypto README | `8421859991a8d07652b8520899e1291297139d1cf14a69782f32c9be6fbc7f1d` |
| Forge donor note | `4439d1cdf5f88f5db723f1b271c1b1413b5aad9f682ed610db7ef11088c4f551` |
| Forge actual MPC test | `af4cf130ab6ebc9945cb5b46ba72d7327aab013b717c573694692093d257c173` |
| Turbine `jobs.cpp` | `a8a64df24c41fb2e8a3baac4fb27add393995870953cb83ac6b11538acd892ae` |
| Turbine contract tests | `c6f6ec84ddf57aa8f210ae7a058a0ac80c49f99751dd25cd33a161dcfade1d57` |
| Turbine wire tests | `07d9452bd1bb9fe8dc032be380696b57f7db3a65cabeae7e6a0a50a182c9e520` |

Этот агент изменил только данный review artifact.
