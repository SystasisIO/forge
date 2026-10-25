# JOB-4 — независимое заключение по зафиксированному результату

Дата: 2026-10-25. **GO для ограниченного инженерного профиля и перехода к JOB-5.**
Производственный/commercial профиль остаётся выключенным. Это не разрешение
на публикацию, merge, deploy, реальные платежи или принятие настоящего Labs.

## Что проверено

Проверены конструкция, реализация общей Forge exchange-механики и TLSN binder,
подключение test-only профиля 2 в Turbine, окончательные тестовые исходники и
полученные другой ролью runtime artifacts. Сам этот reviewer не запускал сборки
и тесты. Базы: Forge `07267d4d13f56167ebba1482009978f0f649be9d`, Turbine
`baaa45a36df53f2366ac77c529d80cf1700fd80d`; изменения pending/uncommitted.

Результат: до 16 блоков raw HTTPS body по 2048 bytes, исходный TLSN
`SHA256(block||16-byte blinder)`, ordered digest list, опубликованный FairSwap
Keccak pad и bounded спор об одном полностью аутентифицированном ciphertext.
Отдельно проверяется доверенный результат verifier, контекст, полнота покрытия,
открытые request/header bytes и свежесть операции до buyer authorization.
Общий FairSwap circuit и generic HTTP/Labs/JSON/locator proof не заявлены.

## Закрытие замечаний

- R1 P2: explicit Raw container limits=16 и witness byte limit=2080 до allocation.
  Native tests требуют точный `allocation_limit`; финальный guest regression
  требует assertion diagnostic раннего allocation admission. Поздний stream EOF
  больше не может выдать этот regression за успешное исправление.
- R2 P3: API comment и README требуют аутентифицировать **весь** verifier result,
  visible bytes вместе с metadata/context/freshness. Подпись retained evidence
  отдельно прямо признана недостаточной.
- Новый настоящий MPC test выбирает публичный operation nonce до TLS и проверяет
  его в аутентифицированном request. Отдельно показано, что binder сам допускает
  переназвание context старой сессии; caller nonce policy отвергает такой replay
  до arm. Граница helper/caller показана честно, без нового notary/TEE/API-key flow.
- Новых блокирующих ошибок в проверенном source freeze не найдено.

## Проверенные runtime записи

XML разобраны независимым parser; в каждой suite failures=0, skipped=0,
disabled=0, записи command JSON имеют exit_code=0.

| Артефакт | Результат | Смысл |
| --- | --- | --- |
| `job4-forge-tests.xml` | 2/2 PASS | Независимые Keccak vectors, caps/faults; настоящий local HTTPS MPC→source hashes→ciphertext→plaintext; nonce replay policy |
| `job4-wire-tests.xml` | 3/3 PASS | Native/настоящий WASM, 219 accepted/57 rejected, ABI и прежние Raw/hash/K1 эталоны |
| `job4-contract-tests-r2.xml` | 2/2 PASS | Настоящий guest contract и ABI; три новые JOB-4 группы, 16 blocks, ложные/валидные споры, поздняя policy и allocation diagnostics |
| `sdk-build-r2.json`, `sdk-install.json` | PASS | Новая SDK сборка/установка; guest потребители используют этот установленный SDK |
| `job4-direct-build.json`, `job4-direct-equality.json` | PASS | Direct и embedded сборки дали побайтно одинаковые WASM, ABI и contract manifest |

Три новые contract группы проверены также в полном
`build/contract-native/Testing/Temporary/LastTest.log`: XML stdout обрезан на
1024 bytes и сам не содержит весь список. Старый явно отмеченный
`JOB2-distinct-K1-bytes ... NOT_RUN` сохраняется как ограничение этого fixture;
его не следует превращать в новый PASS.

Прямое побайтовое сравнение reviewer подтвердило:

| Артефакт | SHA256 |
| --- | --- |
| `turbine.jobs.wasm` (270164 bytes) | `81ca547a032e00825292ec13bdb073074cf247c56956dfee257213d351e0d360` |
| `turbine.jobs.abi` | `075d7d913150a8386800363372a2929f969c6cfdd364e34debddc41074678805` |
| `turbine.jobs.contract.json` | `a75d3e5f3b36a573f7f737cf3efe41259d73b770c1d4b4ed95a0f5978f4c8218` |

## Фиксация источников

Все 24 Forge и 12 Turbine файлов сверены с source manifests, затем reviewer
повторно проверил все 36 сохранённых копий под `job4-source/{forge,turbine}`:
полное совпадение SHA256, включая исходно untracked новые файлы. Tracked patches
сами по себе не включают эти untracked files; архивные копии устраняют этот разрыв.

| Freeze artifact | SHA256 |
| --- | --- |
| `job4-forge-source.json` | `8ce6fe83b286342250d71d11c7c563271225a206f449fdc0246664b53612e55f` |
| `job4-turbine-source.json` | `058b045cb486fd687604d616d1e130a1264e4fca63e7a761330781ef661bbc80` |
| `job4-forge-tests.xml` | `968c71a1717f42934f5f17e6432642862e36da8863f4f335a99851913ae56031` |
| `job4-wire-tests.xml` | `de2706039bd1332a370e4c4e8566bc853b166899f4236d4a7e60c1037d520ea6` |
| `job4-contract-tests-r2.xml` | `40c2f836a55a10e99b6af434b0ec116098b247e51da9d7510ef354249286e070` |

Это заключение относится к сохранённому JOB-4 freeze, а не к последующим
изменениям общих рабочих деревьев в JOB-5.

## Предел GO

Разрешённый следующий engineering шаг — JOB-5 settlement/rollback в тестовой
границе после отдельной проверки новых изменений. Производственный restricted
профиль **не включать**. Внешняя аутентификация whole verifier result, operation
nonce/freshness, fresh one-time key, защищённая доставка и долговременное хранение
ciphertext остаются обязательными обязанностями caller, не выводами из одного
валидного hash или подписи buyer над непрозрачным offer.

Не проверены: Linux, реальные Labs/OpenAI и асинхронные locator/download связи,
качество/GPU execution, две независимые TurbineOS с recovery, live permissions,
общий chain rollback/finality/censorship, production privacy/resource caps,
реальная экономика и сеть. Public dispute раскрывает свой блок, а даже
неуспешный reveal раскрывает ключ. Коммерческий допуск требует соответствующих
последующих gates. Fresh installed **native Forge** consumer этим набором
artifacts не подтверждён: native тесты используют build-tree Forge; установленный
Contract SDK подтверждён отдельно.

Локальная проверка XML default Homebrew Python 3.14 упёрлась в несовместимый
`pyexpat`/system libexpat. Reviewer использовал `/usr/bin/python3` для чтения XML;
пакеты/окружение и продуктовые проверки не менялись. Это ошибка инструмента
review, не ошибка тестов JOB-4.
