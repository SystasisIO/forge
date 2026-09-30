---
name: create-plugin
description: Use when creating, renaming, moving, refactoring, or reviewing an official plugin package, including plugin family, namespace, target, component, module prefix, contract id, and structure rules.
---

# Skill: Create Plugin

**Extends:** `create-library`. Плагин — это **C++ библиотека** с дополнительными
структурными правилами. Сначала применяй ВСЕ правила `create-library` (структура файлов,
расширения `.cppm/.hpp/.hxx/.cpp`, `details/`, парный инвариант, `_impl`, R1–R7). Этот
скилл **добавляет** поверх: организацию семейств, namespace/target/contract-маппинг,
публичные module-слайсы и методы проверки.

**Когда применять:** создание/переименование/перемещение плагина; ревью того, в том ли
семействе плагин и верен ли его namespace/target.

## 1. Расположение и namespace
- `<family>` — доменный путь, а не обязательно один сегмент. Разрешённые вложенные
  семейства задаёт repo-root `AGENTS.md`: сейчас `net/http` и `net/p2p`.
  Например, leaf `plugins/net/http/server` владеет namespace
  `forge::plugins::net::http::server`; дополнительные уровни ради реализации
  или отдельной программы не добавляются.
- Плагин живёт под `plugins/<family>/<name>/` (а не `libraries/<lib>/`).
- Владеет **листовым** namespace `<ns>::plugins::<family>::<name>`.
- Структура файлов внутри — по `create-library` (`include/<ns>/plugins/<family>/<name>/*.cppm`,
  `details/*.hxx`, корневые `*.cpp`).

## 2. Детерминированный маппинг (5 форм из одного листа)
Всё выводится из `<ns>::plugins::<family>::<name>` по `:: ↔ _ ↔ .`:

| Форма | Значение |
|---|---|
| namespace | `<ns>::plugins::<family>::<name>` |
| CMake target | `<ns>_plugins_<family>_<name>` |
| package component | `plugins_<family>_<name>` |
| module-префикс | `<ns>.plugins.<family>.<name>` |
| contract id | `"<ns>.plugins.<family>.<name>"` |
| доп. contract id | суффикс: `"<ns>.plugins.<family>.<name>.<slice>"` |
| каталог / include | `plugins/<family>/<name>/` · `include/<ns>/plugins/<family>/<name>/` |

Все 5 форм обязаны соответствовать листу — рассинхрон любой из них = баг.
Для вложенного семейства каждый сегмент участвует в маппинге:
`net/http/server` → `net::http::server` → `net_http_server` →
`net.http.server`. Пример target: `forge_plugins_net_http_server`;
config section: `plugins.net.http.server`. Все промежуточные уровни пусты,
конфигурация и lifecycle принадлежат конечному плагину.

## 3. Правила семейства

**P1. Член = функциональная роль.** `<name>` называет, **что плагин делает/чем владеет**
(роль-существительное): `server`, `node`, `signer`, `keyvalue`. **Не** сторона контракта
(`provider`/`consumer`), **не** активность, **не** бэкенд-как-семейство.

**P2. Семейство = домен, зеркалит домен-либу.** `<family>` — домен, который плагин
обслуживает, зеркаля либу `<ns>::<family>`. **Бэкенд-как-член разрешён** (`store::sqlite`,
`db::rocksdb` — домен + бэкенд).

**P3. Группируем по тому, ЧЕМ артефакт является.** Плагин группируется по своему домену
(`plugins::<domain>::<role>`). Приложение группируется по app-hood, и т.д. — head noun
задаёт дерево.

**P4. API-family naming задаёт `AGENTS.md`.** Этот skill не переопределяет дерево
`forge::api` и не выводит порядок `api`/channel самостоятельно. При выборе namespace
API binding сначала применяй актуальное правило API family из repo-root `AGENTS.md`.

**P5. Промежуточные уровни пусты.** Каждый промежуточный namespace в
`<ns>::plugins::<family>` — пустая группировка; типы только в конечном листе
`<ns>::plugins::<family>::<name>`. Вложенность не создаёт aggregate plugin,
module или target и не объединяет независимые lifecycle.

**P6. Размещение по слою (produces/consumes).** Плагин живёт в домене, который **обслуживает**:
производит/держит состояние домена X → `X`; потребляет нижний слой, чтобы обслужить домен Y
→ `Y` (вход, не выход). Зависимости односторонние, без циклов/инверсий.

## 4. Публичные module-слайсы
Под `include/<ns>/plugins/<family>/<name>/` (по R1 из create-library):
- `plugin.cppm` — класс плагина + канонический `descriptor(...)`
- опц. `descriptor.cppm` — лёгкий application-composition слайс только с
  `default_descriptor()`; `plugin.cppm` re-export-ит этот слайс
- `api.cppm` — типизированные локальные контракты
- `types.cppm` — config + DTO-типы
- `exceptions.cppm` — типизированные исключения
- опц. доп. публичный слайс (напр. `middleware.cppm`) — под тем же листовым namespace
- **Без** aggregate-only `.cppm`.

Приватный impl и `.cpp` — по `create-library` (`details/*.hxx`, корневые `*.cpp`, парные,
`_impl` только под крупный pimpl).

### Каноническая реализация plugin/API

- `plugin.cppm` + `plugin.cpp` объявляют и реализуют public plugin lifecycle и
  канонический, в том числе конфигурируемый, `descriptor(...)`.
- Опциональная пара `descriptor.cppm` + `descriptor.cpp` существует только для
  лёгкой application composition: интерфейс импортирует только
  `forge.app.plugin_registry` и объявляет `default_descriptor()`, реализация
  импортирует owning `plugin` module и возвращает `descriptor()`. Этот wrapper
  не дублирует plugin id, dependencies, `enabled_by_default`, factory behavior,
  config types или default arguments канонического descriptor.
- `details/plugin_impl.hxx` + `plugin_impl.cpp` содержат только `plugin::impl` и его
  методы. Concrete API implementation и lifecycle facade в этих файлах запрещены.
- Concrete implementation локального `api` называется `plugin::api_impl` и живёт в
  `details/api_impl.hxx` + `api_impl.cpp`.
- Pure-virtual `api.cppm` не получает декоративный `api.cpp`. `api.cpp` допустим только
  для out-of-line реализации concrete public entities, объявленных в `api.cppm`.
- `configure`, `provide`, `initialize`, `startup`, `request_stop` и `shutdown`
  реализуются прямо как методы `plugin` в `plugin.cpp`; одноразовые lifecycle helper
  facade и пустые helper headers для них запрещены.
- Private class нельзя объявлять внутри `.cpp`: declaration живёт в
  `details/<exact_entity>.hxx`, member implementation — в одноимённом `.cpp`.
- Private state record, даже если он header-only, получает собственный
  `details/<exact_entity>.hxx`. `plugin_impl.hxx` содержит только `plugin::impl`,
  а не соседние phase/config/snapshot/state types.

## 5. Методы проверки (перед добавлением/переименованием)

**V1. В том ли семействе?** is-a мастер-тест: «`<name>` — это **вид** `<family>` (kind/view)?»
Да → семейство верное. Нет → проверить роль, обслуживаемый домен и актуальные naming
rules из `AGENTS.md`, не изобретая локальное правило порядка namespace segments.

**V2. Семейство оправдано?** ≥2 is-a членов, ИЛИ uniform-nest политика проекта + правдоподобный
будущий сиблинг. Singleton-семейство по активности/бэкенду без сиблингов → пересмотреть.

**V3. Член по роли?** Не `provider`/`consumer`, не активность, не бэкенд-как-семейство (P1).

**V4. Маппинг консистентен?** Все 5 форм (§2) выводятся из листа по `:: ↔ _ ↔ .` — сверить
namespace/target/component/module/contract.

**V5. Группировка пуста?** В `<ns>::plugins::<family>` нет объявлений типов (P5).

**V6. Слой верный?** produces/consumes (P6); нет восходящих/циклических зависимостей.

## 6. Линт-гейты (grep)
```sh
P=plugins/<family>/<name>; NS=<ns>
# типы только в листе, группировочный уровень пуст (V5)
grep -rE '^(export )?(struct|class|enum)' "include/$NS/plugins/<family>/" \
  | grep -v "/plugins/<family>/<name>/"            # → пусто
# нет aggregate-only модулей
# 5 форм согласованы (V4): сверить target/component/module/contract против namespace листа
# слой (P6): нижний слой не импортит домен этого плагина (направление задаётся проектом)
```

## 7. Что этот скилл НЕ переписывает
Структура файлов, расширения, `details/`, парный инвариант, `_impl`, разбиение `.cpp` —
всё в `create-library`. Плагин подчиняется ему целиком; здесь только плагин-надстройка
(семейства, маппинг, слайсы, проверка).

---

## Интеграция в AGENTS.md (ссылка, без дублирования правил)
```md
## Plugins
Структура и нейминг плагинов заданы скиллом `create-plugin` (который расширяет
`create-library`). Перед созданием, переименованием ИЛИ правкой плагина — загрузи
и применяй `create-plugin`. Не авторь структуру/namespace/target плагина по памяти.
Правила здесь не дублируются.
```
