# JOB-5: review общей транзакционной границы test host

2026-10-25; только дизайн и существующие исходники Forge на базе
`07267d4d13f56167ebba1482009978f0f649be9d`. Реализация расширения не проверена;
сборки/тесты этим агентом не запускались.

## Решение

Предложенный `forge::tooling::testing::host::transact(std::function<void()>)`
**соответствует владельцу и является допустимым минимальным расширением**.
Существующий host уже владеет ObjectDB transaction, настоящим исполнением WASM
и rollback fixture effects. Внешний trusted fixture может вручную доставлять
записанные inline/notify действия как последовательность настоящих WASM-вызовов
в одной внешней транзакции. Это проверяет общий откат, сохраняя действующие
contract/token rows; второй chain emulator и копия финансовой модели не нужны.

Это не подтверждение реализации или chain acceptance: permissions и их переход
по дереву, dispatch/notify ordering, fork/finality, production resources и RAM
billing остаются JOB-7. Ручная последовательность обязана соответствовать
фактически записанным действиям, а не произвольно выбранным вызовам.

## Основание в существующем коде

- `host_impl.cpp:365–423` делает одну копию `oracle_state` до начала ObjectDB
  transaction, затем commit/rollback DB и restore effects вместе.
- `host_impl.cpp:438–463` выполняет настоящий WASM и направляет assertions,
  traps, cancellation/timeout через rollback. `eosio_exit` считается успехом.
- `memory_driver.cpp:88–92,171–186` уже владеет transactional records; новый
  алгоритм snapshot-deserialize restore поверх него не требуется.
- `host_impl.cpp:549–564` запрещает configure/register_contract при открытой
  invocation. При batch это правило требует отдельного состояния invocation.
- `README.md` явно отделяет fixture oracle от production blockchain runtime;
  это разделение должно сохраниться.

## Обязательные условия реализации

1. Отдельно различать `batch_active`, `child_active`, `poisoned` и DB transaction.
   Наличие `transaction_` больше не означает активный WASM-вызов. Nested transact,
   reentrant/overlapping invoke и register_contract в batch отклоняются типизированно.
   Concurrent использование host не становится поддержанным от появления batch.
2. Внешний batch сохраняет oracle state **до** открытия DB transaction; allocating
   state copy не должна оставлять полуоткрытую транзакцию. Child success завершает
   только invocation-local state, не коммитит DB и не удаляет outer snapshot.
   Только outer success коммитит один раз.
3. Любая ошибка child помечает весь batch failed, даже если callback её поймал.
   Сюда входят preflight cancellation/deadline и ошибки подготовки WASM;
   сейчас `begin_invocation` находится вне `try` в `invoke:441`.
   После poisoning нельзя успешно выполнить ещё child, reconfigure и commit.
   Outer exit обязан откатить всё и сообщить отказ; сохранение первого exception
   предпочтительно для точной диагностики. Обычное исключение самого callback
   также откатывает batch.
4. Rollback восстанавливает все seven ObjectDB models/indexes и весь oracle_state:
   auth/context, effects, deferred/recipients/console/return data и т.д.
   Invocation-local iterator caches, call state, deadlines/cancel/receiver
   очищаются на границе каждого child. Старые iterators не переносимы между ними.
   После завершения batch host пригоден для новой обычной invocation.
5. **Read helpers требуют явного решения.** Сейчас `find_table/find_primary/
   find_secondary` в `host_impl.cpp:567–587` читают `store_->index`, поэтому
   между children увидят старое committed состояние, а не outer transaction.
   Либо читать через `transaction_->index`, либо явно запретить такие helpers
   внутри batch и проверять intermediate state настоящими guest calls.
   `snapshot()` сейчас читает driver и также не показывает uncommitted данные;
   нельзя использовать его как свидетельство intermediate state без изменения
   контракта/явного запрета. Для final before/after rollback он подходит.
6. Configure разрешать лишь между children и до poisoning. Trusted fixture
   использует его для auth/sender/context; он должен сохранять накопленные
   recorded effects. Не выдавать замену всего oracle_state за production authority
   propagation. Contract registry/code bytes остаются неизменными на протяжении batch.
7. Nested guest `call` продолжает использовать existing invocation/read-only
   semantics; внешний batch не делает его новым child и не разрешает запись
   через read-only capability. Caller-owned expression/runtime lifetimes сохраняются.
8. Commit/rollback failure не превращать в успех и не терять исходную ошибку.
   Повтор outer cleanup не должен повторять effects/commit. Не вводить восстановление
   DB сериализацией, собственный allocator или второй token ledger.

## Минимальная проверка расширения

- Успех двух children публикует все rows/secondary indexes/effects одним commit.
- Child 1 меняет DB и эффекты, child 2 ошибается: final snapshot и oracle_state
  побайтно/структурно равны исходным; все primary/secondary queries подтверждают откат.
- Пойманная callback ошибка child всё равно откатывает batch и завершает transact
  ошибкой; следующая обычная invocation успешно работает.
- Ошибка callback между успешными children, preflight canceled/deadline и VM trap
  дают тот же полный откат. `eosio_exit` сохраняет принятую семантику успеха.
- Nested/reentrant transact/invoke/configure запрещены; configure между детьми
  меняет fixture auth, сохраняя effects. Intermediate reads имеют объявленную семантику.
- Ранее работавшие single-invocation commit/rollback и synchronous read-only calls
  проходят без batch. ID allocation и secondary uniqueness не обходят ObjectDB.
- Для JOB-5: actual jobs→bill→token/notify→finish, поздний отказ после предыдущих
  записей, пойманный отказ, повтор, sequence/recovery. Manual driver сравнивает
  каждый dispatch с реально записанным effect и не выполняет внешних действий.

Это approval дизайна для реализации после JOB-4 GO; готовность JOB-5 не заявлена.
