# JOB-5 guest/bill/mirror — независимый source review R1

2026-10-25. **Source PASS: блокирующих ошибок в проверенном freeze не найдено.**
Это допуск к тестированию, не завершённая финансовая приёмка JOB-5.
Изменения production code reviewer не выполнял; сборки/тесты не запускал.

Проверены все 10 файлов `jobs5-guest-source.json`, включая guest jobs/bill,
внешние bill calls/charge_quote и README; SHA256 каждого совпал с manifest.
Base `baaa45a36df53f2366ac77c529d80cf1700fd80d`, manifest SHA256:
`efa8249ce129cee4c51507f8a1e41bb38e3d9f38f37fc02ec46cc9c59596d752`.

## Проверенные инварианты

- `jobs.cpp:262–273`: open получает текущий `quotecharge` у настоящего bill;
  полные price/payout сверяются с canonical immutable terms. Jobs не повторяет
  CEL/payout вычисление собственной формулой.
- `bill.cpp:263–314,484–490,561`: readonly quotecharge и новый charge используют
  один admission helper: точные recipient/asset/mode, enabled token, текущий
  period, CEL, maximum; при P>0 — token account и payout capacity. Query не
  создаёт payout/payment; charge сам создаёт подготовленную payout row.
  Existing receipt replay обрабатывается до mutable admission, без повторного CEL.
- `jobs.cpp:864–903`: только revealed и `now>=challenge_until`; code snapshot
  проверен. Sequence выделяется при settle, не при open/job PK; UINT64_MAX
  проверяется до `last_sequence+1`, занятый secondary key отвергается.
- `904–913`: charge использует сохранённые reference/inputs/recipient/maximum,
  accumulated mode, текущий epoch из quotecharge и checked expiry=now+60.
  Используется существующий `bill::content_hash`, а не новая формула.
- `914–950`: reserve/available арифметика проверена до изменений. При D=20000,
  M=15000, P=12000 будущий общий успех даёт A=8000, R=0 и перевод ровно P.
  Settlement/sequence/terminal row сохраняются до inline charge, чтобы reentry
  видел уже занятый receipt. Это tentative state до commit всего дерева.
- `953–984`: fundbill требует `get_sender()==bill`, payer=self, единственный
  secondary sequence, точные content hash и полный positive extended_asset,
  правильный settled order и `!funding_consumed`. Флаг ставится до единственного
  token transfer. Direct вызов/подмена authority не заменяют sender. Memo и
  permission self@billpay берутся из штатного протокола.
- Нулевая цена: bill сразу завершает zero payment без fundbill/finish/token
  transfer; jobs освобождает весь M, сохраняет zero settlement и consumed=false.
  Положительный незапрошенный callback к zero receipt отвергается exact amount.
- Повтор settle возвращается по постоянному jobs receipt до новых guards,
  не выделяет sequence, не отправляет charge. После удаления старого bill receipt
  из FIFO jobs receipt продолжает подтверждать прежний расчёт. Повтор callback
  positive payment отвергается. Порядок job PK не определяет sequence.
- Canonical inputs общие с существующим bill payment helper; нет расходящегося
  порядка входов для jobs content hash и bill hash. Новый `charge_quote` mirror
  совпадает с guest по порядку и типам price/payout/epoch; guest settlement_status
  совпадает с ранее объявленным native mirror. Существующие rows/wire поля не менялись.
- Таблицы settlement/secondary index принадлежат guest jobs. В native mirror
  не перенесены guest tables, token balances, очереди или RAM/authority state.

## Ограничения, которые не следует выдавать за исправленные

Quotecharge — текущая проверка допуска, не обещание будущей оплаты. Она вызывается
при open и settle; после open и до/после reveal mutable token/account/code policy
может заблокировать settle. Сохранение revealed reserve и retry не дают buyer
безопасного обычного refund и не гарантируют executor liveness. Это уже объявленный
commercial gate; production profile остаётся выключен. Нельзя считать новый
readonly helper гарантией оплаты после публичного key.

`host::transact` атомарно группирует **вручную вызванные** действия. Он не знает,
что fixture обязан доставить fundbill/token/notify/finish. Поэтому продуктовый
driver должен сравнить каждое child действие с реально записанным effect,
исполнить весь ordered subtree и проверить пустой остаток очередей до commit.
Иначе один успешный settle handler с tentative settled row не доказывает оплату.

Особенно нужны: late token/notify/finish failure с откатом всего дерева;
чужой callback/full asset/hash/quantity/reentry; ловля child ошибки внутри driver
не разрешает commit; reverse job order, current epoch boundary, overflow,
zero payment, concurrent/reentrant попытки, повтор и retired receipt. Самостоятельное
`getsettle` после первого handler не является достаточным финальным доказательством.

## Отдельная проверка common host runtime

Сверены SHA256 runtime source с `review-jobs5-host-source-r1.md`: implementation,
private state, public host module и шесть новых e2e cases не изменились.
`job5-forge-e2e-tests.xml` разобран независимо: 1 CTest suite, 59 Boost cases,
failures/skipped/disabled=0, `No errors detected`. XML SHA256:
`b63854d8f311596258cfe812f0016d22712ba0d94ccc7225f81c2a1f922bc3db`.
Это runtime принятие общей test-host границы, **не** JOB-5 product subtree pass.
Полный product driver и его runtime freeze на момент review ещё не проверены.

Предстоящее обновление Forge base только документацией не меняет эти source
hashes, но окончательная квалификация должна назвать фактические base/tree и
артефакты. Live chain permissions/finality, Linux, real Labs и production economics
остаются NOT_RUN. Поведение ObjectDB внутренних allocation seals не выдаётся за
побайтовый откат allocator/heap.
