---
name: create-library
description: Use when creating, extending, refactoring, or reviewing a library under libraries/, including file layout, module/header/source placement, file naming, and structure lint gates.
---

# Skill: Library Structure & File Layout

**Назначение:** правила физической структуры новой библиотеки в `libraries/` —
расположение файлов, именование, расширения, разбиение на `.cpp`. Про **структуру**,
не про содержимое.

**Когда применять:** создание новой библиотеки или добавление файла в существующую;
ревью структуры либы; нормализация «поехавшего» стиля.

Правила также обязательны для `guest/libraries/*`. Guest-код меняет target и
dependency boundary, но не физическую семантику `.cppm/.hpp/.hxx/.cpp` и не
парный инвариант.

## Каноническая структура
```
libraries/<lib>/                         # или <group>/<lib>/
  CMakeLists.txt
  include/<ns_root>/<lib_path>/
    <component>.cppm                      # публичный модуль-интерфейс (1 на компонент)
    <name>.hpp                           # публичный НЕ-модульный хедер — редко (см. R1)
  details/
    <component>.hxx                      # приватный хедер
  <component>.cpp                        # реализация (в корне либы)
```

Три и только три места для файлов: **`include/`** (публичный интерфейс),
**`details/`** (приватные хедеры), **корень либы** (реализация).

## Расширения = семантика (строго)
| Расширение | Где лежит | Значение |
|---|---|---|
| `.cppm` | `include/<ns_root>/<lib_path>/` | module interface unit или module interface partition |
| `.hpp` | `include/<ns_root>/<lib_path>/` | публичный **не**-модульный хедер (только макросы / экспорт-шаблоны) |
| `.hxx` | `details/` | приватный хедер |
| `.cpp` | корень либы | реализация |

Расширение однозначно говорит: публичное это или приватное, модуль или нет, интерфейс
или реализация. Структура самодокументируема.

## Правила

**R1. Три локации, ничего вне них.**
- Публичный интерфейс — только в `include/<ns_root>/<lib_path>/`.
- `.hpp` (публичный не-модульный хедер) — только когда модуль `.cppm` не может выразить
  (макросы, экспортируемые шаблоны). По умолчанию — `.cppm`. `.hpp` — исключение.
- Приватные хедеры — только в `details/` и только `.hxx`.
- Реализация (`.cpp`) — только в корне либы. Не в `include/`, не в `details/`, не в `src/`.
- Узкое исключение для EOSIO compatibility veneer: публичные `.hpp/.h` могут
  содержать module imports, targeted aliases и compatibility macros. Они не
  могут содержать самостоятельную реализацию, runtime state, serialization,
  allocator или dispatcher algorithms.
- Generated Contract SDK C ABI `.h` являются build/install artifacts, а не
  source headers библиотеки. Они могут объявлять только C-compatible ABI records
  и intrinsic functions, генерируются из `guest/cmake/` и не размещаются в
  `guest/libraries/*/include/forge`.

**R2. Парный инвариант.**
Каждый `X.cpp` в корне парен по базовому имени **ровно с одним** хедером:
- `include/<ns_root>/<lib_path>/X.cppm` — если реализует публичный компонент, **или**
- `details/X.hxx` — если это приватная реализация.

Не оба сразу, не ни одного. Декларации, которые реализует `.cpp`, живут в одном месте.
Одна пара владеет одним связным компонентом; нельзя использовать общий приватный
хедер как мешок деклараций для нескольких самостоятельных сущностей.

**R3. Header-only компонент — ограниченное исключение.**
`X.cppm` (или `details/X.hxx`) без парного `.cpp` допустим, если компонент состоит только
из деклараций, POD/value records, перечислений, исключений, опций, templates или
`constexpr`-кода. Нетривиальные non-template функции и методы реализуются out-of-line
в парном `X.cpp`.

Header-only module units и interface partitions всегда живут в `include/`. Partition
может быть не re-exported основным module interface и тем самым не входить в публичный
import-контракт, но её исходник всё равно нужен CMake для построения BMI и package
consumer. `details/` для таких `.cppm` не используется; он предназначен только для
настоящих текстовых `.hxx`.

**R4. Приватные текстовые внутренности → `details/X.hxx`.**
Внутренние хелперы/impl-хедеры идут в `details/` как `.hxx` (+ парный `X.cpp` в корне).
Каждый private header владеет одной связной сущностью или одним связным компонентом.
**Никогда** в корень либы, **никогда** `_X.hpp`, **никогда** `.hpp` для приватного.

Если template/constexpr реализация является частью module graph и должна быть доступна
при построении BMI, использовать interface partition `include/.../X.cppm`, а не
маскировать её под приватный `.hxx`, включаемый из публичного module interface.

**R5. Разбиение большого `.cpp`.**
Если реализация компонента велика — дробить как `X.cpp` + `X_<aspect>.cpp`
(напр. `parser.cpp` + `parser_simd.cpp`). Все части реализуют **одни и те же** декларации
(`X.cppm` или `details/X.hxx`). Aspect-файл допустим только при наличии основного
`X.cpp`. Единый суффикс `_<aspect>`; не `_`-префикс, не разнобой.

**R6. Путь `include/` зеркалит namespace.**
`include/<ns_root>/<lib_path>/` соответствует namespace `<ns_root>::<lib_path>`.

**R7. Имя файла = имя сущности дословно.**
Базовое имя файла повторяет имя основной сущности, которую файл объявляет/реализует —
**включая суффиксы** (`_impl` и т.п.) и с маппингом вложенности `:: → _`. Не сокращать,
не опускать суффикс.
- `<component>::impl` → `details/<component>_impl.hxx` (+ `<component>_impl.cpp`).
  **Не** `<component>.hxx`.
- плоский тип `<name>_impl` → `<name>_impl.hxx` / `<name>_impl.cpp`. **Не** `<name>.hxx`.
- Следствие: публичный `<component>` (`<component>.cppm`) и приватный `<component>::impl`
  (`details/<component>_impl.hxx`) — **разные файлы**, имена не схлопываются.

**R8. Aggregate без кода = `INTERFACE`.**
Convenience target, который только транзитивно собирает usage requirements других
target-ов, объявляется `INTERFACE`. Добавлять фиктивный `.cpp`, пустой namespace,
`aggregate_anchor`, `dummy_anchor` или другой placeholder symbol запрещено.

Публичный `.cppm`, который состоит только из `import` / `export import`, запрещён.
Основной модуль библиотеки владеет собственными declarations; удобное объединение
нескольких leaf-модулей остаётся на уровне CMake/package component.

**R9. Объявления раньше метаданных.** Сначала объявлять обычные классы, структуры
и enum. Внизу файла размещать единый блок namespace-scope metadata:
`BOOST_DESCRIBE_STRUCT`, `BOOST_DESCRIBE_ENUM`, serialization/API generation macros
и schema specializations. Не чередовать такие блоки с объявлениями типов.
Сохранять требуемые C++ namespace и export placement: reflection-макросы могут
оставаться в owning namespace, а schema specializations идти после его закрытия.
Нетривиальные schema definitions по-прежнему живут в парном `.cpp`.
Узкое техническое исключение: макрос, который по контракту требует class scope
(например, `BOOST_DESCRIBE_CLASS` для private members), остаётся в завершающем
metadata-блоке своего класса, после его членов; это не разрешает перемежать
namespace-scope metadata с обычными объявлениями.

## Куда положить новый файл (решение)
1. Публичный интерфейс? → `include/<ns_root>/<lib_path>/<entity>.cppm`
   (макросы/шаблоны, не выразимые модулем → `<entity>.hpp` там же).
2. Приватный хедер (видим только внутри либы)? → `details/<entity>.hxx`.
3. Реализация? → корень: `<entity>.cpp`, парный по имени с (1) или (2).
4. Реализация большая? → дробить `<entity>_<aspect>.cpp` (R5).

Во всех случаях `<entity>` — **дословное** имя сущности с `:: → _` (R7).

## Anti-pattern (признаки «поехавшего» стиля)
- Приватный хедер в **корне** либы (вместо `details/`).
- `.hpp` использован для **приватного** хедера (должен быть `.hxx` в `details/`).
- Приватные хедеры названы вразнобой: `_name.hpp` (префикс) **и** `name_backend.hpp`
  (суффикс) — должен быть единый `details/<name>.hxx`.
- **Опущен суффикс/квалификатор в имени файла:** `plugin.hxx` для класса `plugin::impl`
  (должно быть `plugin_impl.hxx`); `<name>.hxx` для типа `<name>_impl`.
- `.cppm` лежит вне `include/`; `.cpp` лежит в `include/` или `details/`.
- `.cpp` без парного хедера (декларации нигде или только инлайн в `.cpp`).
- `X_<aspect>.cpp` без основного `X.cpp`.
- Нетривиальная coroutine/функция реализована в `.hxx` без template/constexpr причины.
- Несколько самостоятельных private classes собраны в одном `details/X.hxx`.
- Reflection/API macros или schema specializations перемежаются с обычными
  объявлениями классов, структур или enum вместо завершающего metadata-блока.
- Code-less aggregate скомпилирован через dummy source или anchor symbol вместо `INTERFACE`.
- `details/` есть в одних либах и нет в других при наличии приватных хедеров.

## Линт-гейты (grep, для скилла/CI)
```sh
LIB=libraries/<lib>
# приватные хедеры только в details/
find "$LIB" -name '*.hxx' -not -path '*/details/*'      # → пусто
# в details/ только .hxx (не .hpp)
find "$LIB" -path '*/details/*' -name '*.hpp'           # → пусто
# публичные модули только в include/
find "$LIB" -name '*.cppm' -not -path '*/include/*'     # → пусто
# реализация только в корне (не в include/ или details/)
find "$LIB" \( -path '*/include/*' -o -path '*/details/*' \) -name '*.cpp'   # → пусто
# приватных хедеров в корне нет
find "$LIB" -maxdepth 1 \( -name '*.hpp' -o -name '*.hxx' \)                 # → пусто
# каждый X.cpp имеет пару: include/.../X.cppm или details/X.hxx (по базовому имени)
```

## Пример
```
libraries/widget/
  CMakeLists.txt
  include/acme/widget/
    widget.cppm        # публичный модуль — интерфейс         ↔ widget.cpp (+ widget_simd.cpp)
    types.cppm         # публичный, header-only               ↔ (нет .cpp)
    macros.hpp         # публичный не-модульный (макросы)      ↔ (нет .cpp)
  details/
    widget_impl.hxx    # приватный класс widget::impl          ↔ widget_impl.cpp   (R7: не widget.hxx)
    cache.hxx          # приватный хелпер                      ↔ cache.cpp
  widget.cpp           # реализует widget.cppm
  widget_simd.cpp      # разбиение widget.cppm (R5), суффикс _simd
  widget_impl.cpp      # реализует details/widget_impl.hxx (widget::impl)
  cache.cpp            # реализует details/cache.hxx
```
