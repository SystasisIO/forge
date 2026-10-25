# Forge P2P: Linux-сборка не видит std::condition_variable

Статус: **OPEN / ошибка компиляции**, не ошибка исполнения сетевого сценария.
P2P — взаимодействие равноправных сетевых узлов. Это отдельный отчёт, не
продолжение [58 диагностик donor matrix](p2p-donor-matrix-baseline-2026-10-25.md).
Исправление относится к сетевому владельцу Forge; принятие задачи и готовое
исправление этим документом не заявляются.

## Точный failed tuple и сохранённое evidence

- [Run 40194688967 / job 121172890543](https://github.com/SystasisIO/spine/actions/runs/40194688967/job/121172890543), attempt 1, workflow `.github/workflows/validator.yml`, mode `oc-native`, `helper_as_bytes=4294967296`.
- Исходный Spine commit: [1504e6deaa0a926bb2881687a53d40b9c24411f7](https://github.com/SystasisIO/spine/commit/1504e6deaa0a926bb2881687a53d40b9c24411f7).
- Исходный Forge commit: [4f9b571bc8c9d738c04ff7e52002ac2acca25e18](https://github.com/crossfed/forge/commit/4f9b571bc8c9d738c04ff7e52002ac2acca25e18); WAMR `33fbef8c7c233e9cf67d50fd1a351d71521e5a96`. Это **не** результат Spine415 / ForgeM2.
- Hosted Ubuntu 24.04 x86_64; actual `ImageVersion=20261023.334.3`. Host Clang 23.1.2 (`85ac560262434c9ccfc0c183ec22d4138ed647fb`), C++23, GCC15 libstdc++; private LLVM 22.1.8, CMake 3.31.6.
- Uploaded artifact `oc-native-40194688967-1`, ID `12603495564`, 425042 B. ZIP SHA256 `792dc4653252aed8dcc887c2f753b971275292064cd9f581621b337baf7bba2b` совпал с provider digest.
- Полный `forge-build.log` SHA256 `0601cbadf36584cfcb7e5303ceb7998333b2384e1862672e8b04331857057d94`. Configure/build commands и logs сохранены в artifact; полный transcript здесь не публикуется.

## Первичная ошибка и воспроизведение

Первый failed TU (единица трансляции) — [node.cpp](../../libraries/net/p2p/node.cpp),
include на строке 96: `13:45:08.7167900Z`, build log 3578–3585. Второй —
[node_impl_relay_transport.cpp](../../libraries/net/p2p/node_impl_relay_transport.cpp),
include на строке 74: `13:47:50.2711682Z`, build log 3604–3611. Оба получают:

```text
libraries/net/p2p/details/cancellation_latch.hxx:49:9: error: no type named 'condition_variable' in namespace 'std'
   std::condition_variable completion_;
```

Существенные actual compile flags:
`--gcc-install-dir=/usr/lib/gcc/x86_64-linux-gnu/15 -stdlib=libstdc++ -std=c++23 -O3 -DNDEBUG`,
с соответствующим `@...node.cpp.o.modmap` / `@...node_impl_relay_transport.cpp.o.modmap`.
Штатный reproducer — тот же focused `oc-native` workflow с указанными точными
source inputs. Его сохранённый build command использует canonical target
allowlist и `-j4 -- -k 0`; compile commands отдельных TU сохранены в build log.
В рамках расследования compiler, workflow и P2P-тесты не перезапускались.

## Include/module reachability: old, M2, dev

Весь `libraries/net/p2p` без delta между old4f9, кандидатом
[M2 2c983756d37d0fb9af885b4ee5021359d7f3eea0](https://github.com/crossfed/forge/commit/2c983756d37d0fb9af885b4ee5021359d7f3eea0)
и проверенным [dev 6a34cb8eac3b4545e3fa0ef745f0381e54889daa](https://github.com/crossfed/forge/commit/6a34cb8eac3b4545e3fa0ef745f0381e54889daa).
Во всех трёх refs одинаковы Git blobs:

| File под libraries/net/p2p | Blob |
| --- | --- |
| `details/cancellation_latch.hxx` | `42e5130d4f5146b5a8b12ba62474cd23c83e4a6c` |
| `details/node_impl.hxx` | `6e8c1c7d189cfae99504b29cd37176159b8f5afe` |
| `node.cpp` | `a1d24e778004c1a06a96e2f5ca7a5484619c5f3d` |
| `node_impl_relay_transport.cpp` | `4c0655b4d1df4375023c6045e72f5fb21358af62` |
| `cancellation_latch.cpp` | `224bce29ce53bbe101bd9e61bb466b125d59c212` |
| `CMakeLists.txt` | `6ac699e485c1d11712462e735494b2d2dd6ca71a` |

[Private latch header](../../libraries/net/p2p/details/cancellation_latch.hxx)
не содержит includes и использует `std::condition_variable` на строке 49.
Оба failed TU не включают `<condition_variable>` в global module fragment
(домодульную область включений). После `module forge.net.p2p.node;` (50/41)
они включают private header (96/74). Отдельный
[cancellation_latch.cpp](../../libraries/net/p2p/cancellation_latch.cpp) явно
включает `<condition_variable>` на строке 4 в своём fragment, но это не
обеспечивает доступность declaration в других TU.

Конкретный compiler output доказывает отсутствие нужной declaration в двух TU
данной конфигурации. Успешная другая платформа или случайный transitive include
не заменяют это требование. M2/dev не содержат source-исправления; нового
compile/runtime результата для них этим расследованием не получено.

## Watchdog и reached / NOT_RUN

Forge build начался `13:04:30.6734207Z`. После первичных compiler errors Ninja
с `-k 0` продолжал независимую работу. Watchdog (ограничитель времени) 110m
остановил процесс `14:54:32.8684976Z`: `build_exit=124`, `tee_exit=0`, последняя
progress строка `3422/4385`. Это progress, не число прошедших тестов.
`status.json` классифицирует конечное завершение как `watchdog_timeout`, но
timeout не является причиной раннего compile failure.

36 подготовительных stage receipts имеют exit 0, включая host toolchain,
dependencies и Forge configure. Product CTest cases executed = **0**.
Forge install, VM registry/runtime/sampling, package consumers/relocation,
SDK stages, Spine build/runtime и post-retirement closure — **NOT_RUN**.
Install custom target зависит от `forge`; install output отсутствует.
Остальные четыре workflow jobs skipped; evidence upload success.
Linux/platform PASS не получен.

## Минимальное действие владельца

Владелец — Forge `libraries/net/p2p`, private include/module dependency boundary.
Нужно восстановить явную доступность стандартной declaration в корректной
существующей module/include границе обоих TU, затем провести review и штатную
проверку прежними build/gates. Это не запрос менять P2P-алгоритмы, consensus,
VM, compiler flags, бюджеты или selectors.

Исходный failure остаётся evidence. Нельзя заменять source correction
увеличением watchdog или объявлять новый tuple проверенным старым run.
При подготовке этого отчёта code/checker/registry не менялись; commit, push
и CI rerun не выполнялись. Документ не является разрешением на новый запуск.
