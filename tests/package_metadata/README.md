# Package metadata gate

`verify_layout.cmake` checks the genuine generated install export for private
`codec_xml` source/build paths and discovers the real configured `ForgeConfig`
at its configured `GNUInstallDirs` layout, before and after prefix relocation.
It also checks focused build-tree discovery without installed notices.

`Forge_NOTICE_FILES` is the basename-only required root-level list in the existing
`Forge_NOTICES_DIR`. It is derived from the same source list as the root install
rule, not a second downstream filename registry. The WAMR notices retain their
existing separate install rule and subdirectory; this gate does not restore
the removed compiler's root-level notice requirements.
The gate copies the owning root-level source list, compares package metadata with it,
and rejects a missing notice or a directory in place of a notice file.
This is not the complete third-party notice-file list: optional subdirectories
are installed by their existing owning rules. Product redistribution must copy
the whole `Forge_NOTICES_DIR`, including its subdirectories, rather than copying
only `Forge_NOTICE_FILES`. The metadata fixtures below validate the root-level
list; they do not claim to reproduce the entire installed notice directory.

The relocation fixtures use genuine build-tree targets and source legal notices.
They deliberately do not pretend to be fully installed libraries: no executables
are linked here. The existing installed component tests remain the runtime gate.
Configure Forge with `CMAKE_INSTALL_LIBDIR=lib/native-triplet` and
`CMAKE_INSTALL_DATADIR=legal-data` to exercise an alternate layout with this gate.
