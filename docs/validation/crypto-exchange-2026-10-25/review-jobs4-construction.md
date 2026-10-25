# JOB-4: независимая проверка конструкции

Дата: 2026-10-25. Проверка исходников и конструкции; сборки/исполнение не запускались.
Turbine: `baaa45a36df53f2366ac77c529d80cf1700fd80d`, дерево
`worktrees/turbine-jobs45-v1`; Forge: `07267d4d13f56167ebba1482009978f0f649be9d`.

## Решение

**Условно одобрена для реализации ограниченная конструкция прямого ответа:**
не более 16 independently verified TLSNotary commitments, до 2048 bytes результата
на commitment, прямой список хешей ciphertext и спор об одном открытии.
Одобрение относится к описанной ниже композиции и её предпосылкам, не к ещё
не проверенному коду, производственному профилю или настоящему Labs.

**Не одобрено:** обычный файловый FairSwap с произвольным `fileRoot` исполнителя;
подпись buyer над opaque — непрозрачным — корнем не доказывает источник файла.
Общий профиль SHA256/HTTP/JSON/нескольких связанных сессий остаётся research gate.

## Проверенные первичные источники

- TLSN `5f67389de3f8962227fddffd71d78a852acc9b01`, чистый локальный donor.
  [`transcript/hash.rs:10–41`](https://github.com/tlsnotary/tlsn/blob/5f67389de3f8962227fddffd71d78a852acc9b01/crates/core/src/transcript/hash.rs#L10-L41)
  задаёт `H(plaintext || blinder)`; metadata расположена отдельно. Git blob
  `70b90e5fa2a3f5a650347c5bf399df059e11bf14`.
  [`commit/hash.rs:139–168`](https://github.com/tlsnotary/tlsn/blob/5f67389de3f8962227fddffd71d78a852acc9b01/crates/tlsn/src/transcript_internal/commit/hash.rs#L139-L168)
  использует 16-byte blinder, выбранные ranges и SHA256. Git blob
  `59b1b5a681570852f0bf1b69ddbd14e480f552b8`.
- FairSwap `07f5664de64e232a0e1a8c0a84eefc223b45ae8b`: GitHub tree получен
  read-only; raw-файлы совпали с Git blob IDs дерева.
  [`FairFileSale.sol:20–22,70–107,123–150`](https://github.com/lEthDev/FairSwap/blob/07f5664de64e232a0e1a8c0a84eefc223b45ae8b/FairFileSale.sol#L20-L150)
  фиксирует fileRoot/ciphertextRoot и проверяет файловые hash-узлы; pad для
  больших блоков — `keccak256(uint256 counter, bytes32 key)` XOR ciphertext.
  SHA256 файла: `bba1f17a9375cbd3680eb25a376ebf3e842e643b49cd5bfdf6ecd7289e5ff9bd`;
  Git blob `14081408ba3f327aff52ff7bc075cfa2181348b4`.
  Прототип не содержит TLSN/HTTP predicate; Merkle loop жёстко равен трём шагам.
  [`README:3–5`](https://github.com/lEthDev/FairSwap/blob/07f5664de64e232a0e1a8c0a84eefc223b45ae8b/README.md#L3-L5)
  прямо обозначает PoC без аудита реализации.
- [FairSwap, Dziembowski/Eckey/Faust, CCS 2018](https://eprint.iacr.org/2018/740):
  общий протокол продаёт witness, удовлетворяющий согласованному predicate.
  PDF с ePrint возвращал HTTP 403; полные алгоритмы проверены в первичном
  авторском изложении Lisa Eckey,
  [диссертация, §5.4.1, печатные стр.126–129, Algorithms 6–8](https://tuprints.ulb.tu-darmstadt.de/server/api/core/bitstreams/0d999e6b-864a-4470-9a6a-984db287b388/content).
  SHA256 PDF: `bca5b305bc392e87a642116cf66d4994718ab8c21e51a81c8fbc9c4033e90a18`.
  Там шифруются все входы и промежуточные wires; judge аутентифицирует gate,
  его wiring, входы/выход и окончательное значение. Произвольная C++-функция
  без такой схемы и trace не даёт короткого FairSwap proof.

## Что уже даёт JOB-3

В Forge `libraries/net/tlsnotary/include/.../types.cppm:43–72` commitment содержит
direction/ranges/digest, а evidence — pin/origin/длины/commitments.
`native/src/commitment.rs:125–159,177–228,233–295` требует точное совпадение
policy и проверяет открытие через upstream `hash_plaintext`.
`libraries/net/tlsnotary/README.md:93–110,130–136` явно требует доверенное
сохранение либо подпись **всей записи и контекста**; offline opening check сам
не подтверждает TLS-происхождение. Это точные файлы Forge pin выше.

Следовательно, buyer verifier — доверенная компанией сторона онлайн-проверки.
Контракт может доверять его закреплённой подписи о проверенных source facts,
но такая подпись не становится независимо проверяемым TLS proof. Ключ и context
для проверки импортированной записи берутся из доверенного order/config.

## Ограниченный профиль: достаточные условия

1. Единственная MPC-сессия, ожидаемый origin, аутентифицированный запрос с
   точными method/path и существенными параметрами. Credentials не раскрываются.
   Результат — непосредственно raw HTTP body; нет JSON extraction, redirect,
   content encoding, chunked framing, скрытого locator или отдельного download.
   Проверяется успешный статус, единственный Content-Length и ровно один ответ.
2. До arm verifier проверяет аутентифицированные headers, body offset `h` и
   длину `N`, `0 < N <= 32768`. Для управляемого fixture допустим заранее
   фиксированный header format и offset, например `h=2048`; проверяется
   фактическое совпадение, а не заявление prover. Reveal masks не захватывают body.
   Forge policy задаётся заранее (`types.cppm:79–94`): неизвестный header offset
   произвольного HTTP-сервера этим fixture не решается.
3. `n=ceil(N/2048) <= 16`. Commitment `i` относится ровно к received interval
   `[h+2048*i, h+min(N,2048*(i+1)))`, в этом порядке. Никаких holes, overlaps,
   duplicates, лишних ranges и перестановок. Verifier сверяет список с собственным
   успешным MPC output, origin/lengths/pin и context заказа.
4. Offer фиксирует ordered entries `{plaintext_size, source_sha256,
   ciphertext_sha256}`, число блоков, key commitment и evidence/context binding.
   16 таких записей занимают 1088 bytes до общего заголовка; полный Raw размер
   всё равно проверяется против 2048-byte cap. Buyer **сам сравнивает** все
   source digests с trusted evidence и проверяет длину/хеш каждого полностью
   полученного ciphertext. Затем долговременно сохраняет комплект и подписывает
   точный offer. Проверка подписи исполнителя не заменяет эти проверки buyer.
5. Открытие блока — исходные `block_i || blinder_i` из TLSN, с каноническим
   нулевым дополнением до 32 bytes. Для 2048-byte block это 2080 bytes = 65 words.
   Используется опубликованный pad `Keccak256(BE256(counter) || key32)` и XOR,
   **без собственной реализации Keccak**. Counter `65*i+j`, `0<=j<65`, уникален
   для всех words одного offer; последний короткий блок сохраняет тот же stride.
   Key — свежие непредсказуемые 32 bytes, один раз на offer. Это ограниченное
   применение донорского `cryptLarge`, не новый cipher и не AEAD-гарантия.
6. Native использует существующий Forge Keccak (`sha3::hash(...,false)`);
   guest — `forge::contract::keccak`. Их выбор подтверждён
   `libraries/crypto/digest/sha3.cpp:178,235–238` и
   `guest/libraries/contract/crypto_ext.cpp:64–66`. NIST SHA3 и Keccak различаются;
   native/WASM golden vectors обязаны подтвердить именно donor bytes.
7. После reveal challenge содержит только индекс и точный ciphertext этого блока.
   Checker сначала проверяет stage/context/index/размер/его SHA256 относительно
   закреплённого offer; затем decrypt, canonical padding и
   `SHA256(block_i || blinder_i) == expected_source_sha256[i]`.
   Доказанным нарушением являются неверное открытие либо padding после успешной
   аутентификации ciphertext. Неверный ciphertext hash — подложная жалоба,
   отклоняется без refund. Key сверяется с закреплённым commitment.

## Аргумент корректности композиции

Это собственный вывод из указанных условий. Прямой authenticated digest list
заменяет Merkle inclusion: при 16 элементах контракт хранит каждый expected digest,
поэтому branch не требуется. Это та же обязательная привязка leaf к accepted offer,
с линейным размером offer; не следует утверждать её криптографическое превосходство.

Если все decrypted blocks удовлетворяют своим independently authenticated TLSN
digests, collision resistance SHA256 связывает каждый block вместе с blinder с
соответствующим source opening. Точное покрытие связывает их concatenation с полным
raw body. Если хотя бы один отличается, у buyer есть ciphertext именно этого
блока; его hash подтверждает происхождение из accepted offer, а проверка открытия
показывает нарушение. Около 2080 bytes ciphertext и небольшой фиксированный
заголовок укладываются в 4096-byte challenge cap без общего SHA256 circuit.

Для ложного refund нечестный buyer должен либо подделать ciphertext commitment,
либо заставить детерминированный decrypt/hash проверить иной блок/ключ/длину.
Поэтому index, sizes, order и counter mapping являются частью security boundary.
Свежий key и injective counter нужны для скрытия результата до arm. Публичный
key после reveal означает, что ciphertext доставляется по защищённому каналу.
В споре challenged block и blinder становятся публичными; credentials должны
отсутствовать в openings. Защиты от раскрытия результата самим buyer нет.

## Атаки и обязательные проверки реализации

- Seller encrypts unrelated file с корректным собственным fileRoot: до arm
  rejected при source-list comparison либо после key доказан failing leaf.
- Перестановка, пропуск, дублирование, укороченный последний блок, лишний хвост,
  подмена direction/range/origin/implementation/context при прежнем digest.
- Чужой job/attempt/chain/deployment, другой offer, импортированная запись
  с ключом/контекстом из неё самой; все отклоняются до выдачи открытия.
- Подмена source digest в подписываемом offer: verifier не подписывает даже при
  корректной подписи продавца и корректном ciphertext hash.
- Wrong key, изменённые plaintext/blinder/padding; валидная жалоба только на
  действительно committed ciphertext. Подложный index/ciphertext/size/trailing
  bytes не возвращает резерв и не меняет сроки.
- Golden pad vectors для counters 0/64/65/1039, byte order, partial last word,
  всех 16 блоков; native и настоящий WASM должны совпасть с донорским Keccak.
  Counter не перезапускается на каждом блоке; key не переиспользуется.
- Min/max body, 16 leaves, max witness и rejected cap+1; arithmetic overflow
  checked до allocation/hash. Worst-case contract work измеряется отдельно.
- Headers/framing: другая длина/offset/status, duplicate Content-Length,
  Transfer-Encoding, encoding, redirect, truncated/multiple response; reject.
- До arm нет полного открытия или bearer URL. Verifier record/logs не содержат
  API-key, body или blinders. В challenge допускается только received body block.
- Storage/restart: частичная доставка не допускает arm; утраченный после arm
  ciphertext не превращается в доказательство вины seller. Recovery сохраняет
  весь accepted bundle и original deadlines; repeated reveal не продлевает окно.

## Ограничения и следующий gate

Изменены только этот review artifact. Ни crypto code, ни runtime не проверены.
Предложенные caps/формат ещё требуют явной фиксации в профиле и golden tests.
Схема доказывает происхождение конкретного прямого ответа и его доставку;
она не доказывает GPU execution, качество картинки или право на отдельную сетевую
награду. Real Labs, Linux, финальность/цензура цепи и commercial activation — NOT_RUN.
Общий TLSN→асинхронный Labs result→download путь этим профилем не закрыт.

Общая donor-based exchange механика принадлежит Forge; request/response policy,
body coverage, trusted evidence comparison и коммерческий смысл — Turbine.
До включения production остаются независимая проверка финального кода,
WASM/resource/privacy acceptance и нерешённые gates из JOB-плана.
