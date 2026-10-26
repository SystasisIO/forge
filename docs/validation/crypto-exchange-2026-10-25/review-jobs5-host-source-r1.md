# JOB-5 common test host — source review R1

2026-10-25. Forge worktree `forge-tlsnotary-jobs3-v1`, base
`07267d4d13f56167ebba1482009978f0f649be9d`. Только чтение; runtime NOT_RUN этим
reviewer. Проверены `host::transact` и шесть новых test cases в существующем
`guest/tests/e2e/contract_tests.cpp`.

**Source disposition: PASS, блокирующих ошибок реализации не найдено.**
Это подтверждение общего механизма для дальнейшего runtime qualification,
не JOB-5 financial acceptance или live-chain acceptance.

## Проверенные свойства

- `host_impl.cpp:367–408`: отдельные group/invocation/poison flags; существующий
  transaction повторно используется детьми. Новая копия oracle и новый DB
  transaction создаются только вне group. Iterator caches и per-invocation
  deadline/cancel сбрасываются на старте каждого child.
- `411–445`: успешный child не коммитит DB, не удаляет внешний snapshot;
  неуспешный помечает group failed. Rollback всей group выполняется внешней
  границей один раз с восстановлением исходного oracle_state.
- `460–493`: preflight ошибки begin, cancellation/deadline и ошибки подготовки
  WASM также poison group. Пойманная callback ошибка child не разрешает commit
  или следующий child. Настоящий guest `eosio_exit` сохраняет семантику успеха.
- `578–605`: allocating snapshot copy идёт до DB transaction. Только успешный
  внешний callback со всеми успешными детьми коммитит. Callback exception и
  nested group failure откатывают DB+oracle. Исключение callback сохраняется
  через `exception_ptr`; если сам callback поймал ошибку child, outer exit
  сообщает typed database_error вместо ложного успеха.
- `608–658`: configure только между успешными children, contract registration
  запрещён при любом открытом transaction. Independent primary/secondary read
  helpers используют текущий transaction и видят uncommitted child writes.
- `1431–1435`: snapshot внутри uncommitted transaction явно запрещён;
  прежняя ловушка чтения committed driver snapshot устранена.
- Existing synchronous guest `call`, read-only capability, actual WASM backend,
  ObjectDB schema/rollback и memory driver переиспользованы. Нет автоматической
  доставки действий, второй модели token balances или snapshot restore protocol.
- README уточняет: caller начинает configure с текущего `state()`, сохраняя
  recorded effects; вручную исполняемые children сверяются с реальными effects.

## Важные границы

`configure`/`register_contract`/`snapshot` — trusted fixture APIs. Пойманный
ошибочный setup/read вызов сам по себе не обязан poison group: он не изменяет
состояние и не является failed guest invocation. Документированный строгий
poison относится к failed `invoke` и nested `transact`. Если callback не ловит
ошибку такого helper, внешний catch всё равно откатывает group. Не предоставлять
configure из активного guest execution; concurrent использование host этим
изменением не становится поддержанным.

Тесты корректно различают откат protocol rows/effects и монотонные внутренние
ObjectDB allocation seals. Это не byte-identical heap/allocator restoration.
Не заявлять буквальное равенство всего внутреннего DB snapshot после allocations,
если проверены только предметные rows/indexes и oracle_state.

Пост-commit observer/rollback failures не моделируются новыми тестами; базовая
memory-driver транзакционная механика сохранена. Нельзя переносить qualification
тестового driver на production DB/цепь. Permissions, inline/notify ordering,
финальность и полномочия остаются JOB-7.

## Тестовые исходники

Новые шесть cases покрывают real-WASM success с primary/secondary rows и effects,
`eosio_exit`, late child rollback, пойманную ошибку и запрещённое продолжение,
callback exception, snapshot/nested group rejection, preflight cancel/deadline,
настоящий watchdog timeout и пригодность host после rollback.
По этой проверке их наличие не выдаётся за успешный запуск: нужны отдельные
raw runtime records и точный tested source freeze.

Дальнейшая JOB-5 проверка должна связать механизм с настоящими
jobs→bill→fundbill→token→notify→finish invocations и с поздним отказом всего
дерева. Только возврат `jobs::settle` или `bill::charge` оплату не подтверждает.

## Проверенный SHA256 snapshot

| Файл | SHA256 |
| --- | --- |
| `host_impl.cpp` | `16b791352687dcf224332d6615f55a8e8089f2051b6a3058833ce39a9ea33d14` |
| `details/host_impl.hxx` | `b36f6ccd491b775ea2154ddba0173d32ce05162c9eca7fec059b7e57422efcfd` |
| `host.cpp` | `888b6f13ac12bde179d59e6ed39b3a7258d9f143979a3a3334aba6d1743e86e1` |
| `host.cppm` | `e3a56b57773d9a9ca81f9b0519267c344c0a64855e8415169c36d5e441450bcc` |
| Testing README | `5cb21c6ceabc313cae3d013ecd72527698f2e0d0d686b5f707cc467c0a76e4c1` |
| Contract e2e tests | `c60c0e1b47de5b6ec507810ce501eee16267bc4622874f0158f054ca2ca34c9d` |
