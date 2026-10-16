# Ordinary optimizing executor package consumer

One ordinary C++ translation unit imports the public executor leaf and performs
real standard stop_source/stop_callback registration, request_stop, immediate
registration on an already-stopped source and destruction. It then decodes a
matching reviewed add request/artifact through the public owners, prepares once,
binds the existing standalone registry and invokes add(7,9)=16. A second invoke
with an already-stopped token must return cancelled without a value. Worker
preparation is released before executor/scheduler drain. The physical guest cap
is explicitly 4MiB reserve + 256KiB payload. It contains no owning-module
declaration, private test object or standard-library symbol shim.

This source is a pending external-consumer gate. Its module map must be generated
for this target by CMake and the selected installed/build-tree Forge package;
the compiler test's existing module map does not contain executor and must not
be copied as a substitute. Compile/link/run and relocated-package verification
remain NOT_RUN until their exact commands and package/toolchain hashes exist.

The SAME main is also compiled by the root diagnostic target
`test_forge_vm_wasm_optimizing_ordinary_consumer` in an existing root graph. Run it
with two explicit arguments: the reviewed native `add-<profile>.request` and
matching `.foc`. Missing input fails; it is not silently skipped. This root
diagnostic is not installed/relocated package acceptance. It specifically tests
ordinary imported-consumer callback linkage/lifetime and public guest execution,
which an owning-module test does not prove.

For a later package configure, set `FORGE_OC_CONSUMER_REQUEST` and
`FORGE_OC_CONSUMER_ARTIFACT` to the same absolute reviewed native fixture paths.
No LLVM link or compiler/test-object dependency is used. Package closure,
relocation, Linux execution and performance remain separate gates.
