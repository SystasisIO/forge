# Registration

The leaf `CMakeLists.txt` defines `test_forge_swarm_plugin` with:

- `main.cpp`
- `config_tests.cpp`
- `capability_tests.cpp` (implementation unit of the plugin module)
- `lifecycle_tests.cpp`
- `metadata_tests.cpp` (real named catalog restart on MDBX and RocksDB)
- existing `../quic_p2p/libp2p_identity_fixture.cpp`

The parent registers `add_subdirectory(swarm_plugin)`, the plugin leaf and
the focused installed component `plugins_swarm_node`. Link installed consumers
to their focused module owner; do not rely on incidental BMI discovery.

Run the focused test target, native plugin/storage/runtime suites, installed
consumer and structure gate in the coordinator-owned build. No build or test
execution is inferred from source inspection alone. `capability_tests.cpp`
attaches to `forge.plugins.swarm.node.plugin` to exercise private joined-call
ownership without exporting a test-only production API.
