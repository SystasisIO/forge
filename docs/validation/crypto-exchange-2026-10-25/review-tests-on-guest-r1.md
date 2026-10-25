# JOB-5: независимое review тестов и фикстуры, r1

Дата: 2026-10-25. Вердикт: **CLEAN_STATIC_AFTER_FIXES**.
Открытых блокирующих замечаний к исследованному frozen test-коду нет.

Это review тестов, написанных другим агентом. Собственный production guest
этим артефактом не одобряется. Сборки, CTest и новые runtime-процессы reviewer
не запускал; результаты выполняемых координатором проверок — отдельный gate.

## Точные источники

Checkout: `worktrees/turbine-jobs45-v1`, base
`baaa45a36df53f2366ac77c529d80cf1700fd80d`.

| Исходник | SHA-256 |
| --- | --- |
| tests/integration/jobs/contract/settlement_tests.hxx | a99d8f3c50c2593b3b6205cbd77c238f2ba764b4ef18f755d23155c196f1f325 |
| tests/integration/jobs/contract/contract_tests.cpp | 12e1d34e90adfed5ce6932505e365109499b274def96d0f840b2a8b047323e14 |
| tests/integration/jobs/contract/check_abi.py | f226cd91e8eb10c8f706779eced66b2afdaa73899ade71c95c234f2b0fd8ac03 |
| tests/integration/jobs/contract/fixture_values.hxx | 02bfb028e41b09e65aa1b005ff00ec6b1a76189de6aff3984a0a9a19af1eb995 |
| tests/integration/jobs/contract/guest/reader.cpp | d060d666c92da80d90a5d08ea12dd6d467c666e9ebbbf06f3c91a5f5d455a54f |
| tests/integration/jobs/contract/guest/token.cpp | b9f64a6145ad7790a9b21538cda965b2057853a527adbb76faf6807f0ec43fc6 |

Дополнительно прочитаны guest reader/token module interfaces, изменения CMake,
bill ABI checker и фактический уже сгенерированный
`build/contract-guest/bill/artifacts/spine.bill.abi` из каталога текущей приёмки.

## Найдено и исправлено до freeze

1. **ABI field mismatch.** Новый checker ожидал `request`, настоящий Abigen
   выдаёт `quotecharge.reference`. Исправлена только ожидаемая строка имени;
   сохранены точные типы, порядок аргументов и результат `charge_quote`.
2. **Неполная доставка эффектов.** Первый вариант main recipe проверял лишь
   наличие jobs/bill среди накопленных recipients. Теперь проверяется точный
   ordered recipient delta, число и Raw-содержимое известных inline actions,
   неизменность прежнего префикса и полная остаточная oracle state.
   Такие guards добавлены также в FIFO и claim, без незаметного удаления
   недоставленного нового effect при `clear_effects`.
3. **Неполный protocol rollback oracle.** Добавлены RAM payer таблиц и
   secondary rows, текущий/следующий payment sequence. Внутренние ObjectDB IDs
   намеренно не объявляются protocol state: allocation seals — служебные
   монотонные номера выделений — могут сохраняться после корректного rollback.
4. **Только fresh payout failures.** Добавлен второй settlement поверх уже
   принятого первого и управляемый отказ после настоящего успешного finish и
   начисления. Проверка восстанавливает первый receipt/payment/bucket и исходные
   остатки второго заказа, затем повторяет его штатным путём.

## Что проверено в frozen исходниках

Пути/строки ниже относительно checkout; `settlement_tests.hxx` сокращает
`tests/integration/jobs/contract/settlement_tests.hxx`.

- `contract_tests.cpp:167-233,385-392,472-483`: тела прежних `query`, `rejects`,
  `repeats`, `covered`, `no_payment` побайтно совпадают с base HEAD. Строгие raw
  snapshot assertions не заменены облегчённым oracle. Отдельный batch oracle
  не меняет прежние ожидания.
- `settlement_tests.hxx:71-161`: полное закрытие известных эффектов, primary
  bytes/payers, table existence/count/payer, u64/u128/key256 secondary keys/payers
  для затронутых сценарием scopes/keys и полный `oracle_state` до/после отказа.
  В нормализации успешного recipe разрешены только доставленные inline/notify
  и явно настраиваемые между вызовами sender/auth/time; context-free/deferred,
  параметры, features, console и прочие поля не игнорируются.
- `settlement_tests.hxx:185-369`: настоящий записанный Raw charge проверяется
  на account/name/полномочие jobs@billcharge и точные неизменяемые поля/hash.
  Настоящие callback/finish проверяются до исполнения, fundbill вызывается с
  sender=bill и без подмены его account auth. Consumed виден до token child.
  Настоящий token перевод исполняется, затем доставляются ровно его jobs/bill
  notifications; bill payment наблюдается funded. Настоящий записанный finish
  исполняется **до возврата callback общей транзакции**, затем проверяется
  отсутствие недоставленного эффекта. Это явный известный сценарий, не router.
- `settlement_tests.hxx:371-434`: независимый денежный oracle D=20000/P=12000,
  M>P и M=P; positive не делает немедленный recipient payout. Zero P с M>0
  и M=0 создаёт один завершённый bill receipt, без callback/finish/transfer,
  consumed=false. Independent epoch oracle при now=5000 требует epoch=1,
  хотя open был в epoch=0; reverse job-PK completion даёт sequence 1,2.
- `settlement_tests.hxx:436-501`: чужие sender/payer, авторизация bill без
  sender, sequence/hash/amount±1/contract/symbol/precision; точный settle reentry
  не создаёт charge. Повтор fundbill и перехваченный отказ child не разрешают
  commit poisoned batch — группы, помеченной ошибкой. Старый принятый receipt
  и обычный повтор по-прежнему проверяются.
- `settlement_tests.hxx:503-603`: отказ настоящего token credit, jobs/bill
  notifications, finish и capacity; first-row creation и второй платёж после
  committed первого. `settlement_abort` возникает после фактически наблюдаемого
  completed payment и увеличенного bucket, до outer commit. При откате
  остаются revealed/reserve, предыдущий платёж и исходные реальные fixture
  balances; повтор возможен без новой лишней sequence.
- `settlement_tests.hxx:558-711`: bill token disable и jobs/bill/token code
  drift сохраняют revealed reserve; cancel/release/expire не освобождают его.
  Возвращение исходной policy позволяет один расчёт. Отдельно проверены
  readonly quotecharge, full asset/mode/max, отсутствующий token account
  получателя для positive и допустимость его отсутствия для zero, полный
  bucket, sequence exhaustion, checked +60 и последняя представимая граница.
- `settlement_tests.hxx:713-763`: действительно исполняются 1025 zero payments
  без изменения окна 1024. Retired, retained и not_executed различаются;
  постоянный jobs receipt не создаёт новый charge. Каждый шаг проверяет actual
  zero charge target/name/auth/hash и закрытие всех новых эффектов.
- `settlement_tests.hxx:765` и далее: настоящий claim/token transfer и rollback
  при recipient overflow. Bill notify доставляется до commit; обычный recipient
  без зарегистрированного кода явно учтён как no-op. Точные recipient order,
  inline count и остаточные oracle effects проверяются до commit.
- `contract_tests.cpp:2867-3010`: прежние отрицательные direct-profile witnesses
  всё ещё проходят строгий rejects/snapshot oracle и оставляют полный резерв;
  неправильное свидетельство не считается основанием refund. Только заранее
  аутентифицированный доказанный defect допускает invalid_delivery release.
- `contract_tests.cpp:3013,3043` и далее: новый hxx реально включён в acceptance
  executable, группы явно вызываются из main. Сценарии не оставлены мёртвым кодом.
- `guest/token.cpp:51-55`: open создаёт настоящую нулевую accounts row через
  существующий SDK и проверяет owner auth. Это не фиктивный положительный
  balance. Новые reader calls используют штатный read-only call_wrapper;
  seeding payout остаётся только в test-owned guest fixture. Независимые
  persisted layouts сверяются checker с настоящим Abigen ABI.

## Граница вывода

**CLEAN_STATIC** относится к указанным тестовым исходникам после исправлений,
а не к результату будущего CTest. Runtime соответствие, полный итог JOB-5 и
fresh installed SDK должны подтвердить отдельные логи координатора.

Модель вручную доставляемого дерева с одной ObjectDB transaction не доказывает
автоматический chain scheduler, настоящий permission/linkauth graph,
consensus/finality/forks или live token funds. Подмена code hashes — вход
фикстуры, не настоящий upgrade. Forge host::transact проходит отдельный review;
его 59 runtime cases этим reviewer не переисполнялись.
Linux, настоящий Labs и коммерческая активация остаются вне этого вердикта.
