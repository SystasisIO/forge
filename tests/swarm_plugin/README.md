# Registration

The leaf `CMakeLists.txt` defines `test_forge_swarm_plugin` with:

- `main.cpp`
- `config_tests.cpp`
- `capability_tests.cpp` (implementation unit of the plugin module)
- `lifecycle_tests.cpp`
- existing `../quic_p2p/libp2p_identity_fixture.cpp`

The coordinator must add `add_subdirectory(swarm_plugin)` to the existing test
registration and register `plugins/swarm/node` / `forge_plugins_swarm_node` /
`plugins_swarm_node` in parent/package files. No parent registration is changed
by this slice. Link an installed consumer directly to all module targets it
imports; do not rely on incidental BMI discovery.

Run the focused test target, native plugin/storage/runtime suites, installed
consumer and structure gate in the coordinator-owned build. No build or test
execution was performed by the implementation agent. `capability_tests.cpp`
attaches to `forge.plugins.swarm.node.plugin` to exercise private joined-call
ownership without exporting a test-only production API.
