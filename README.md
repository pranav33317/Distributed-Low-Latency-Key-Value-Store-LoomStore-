// File structure
/loomstore/
├── CMakeLists.txt
├── include/
│   ├── core/
│   │   ├── io_uring_engine.hpp
│   │   ├── lockfree_map.hpp
│   │   ├── slab_allocator.hpp
│   │   └── simd_utils.hpp
│   ├── raft/
│   │   ├── raft_node.hpp
│   │   ├── log_entry.hpp
│   │   └── consensus.hpp
│   └── network/
│       ├── rpc_protocol.hpp
│       └── connection.hpp
├── src/
│   ├── main.cpp
│   ├── io_uring_engine.cpp
│   ├── lockfree_map.cpp
│   ├── raft_node.cpp
│   └── slab_allocator.cpp
└── tests/
    ├── benchmark.cpp
    └── unit_tests.cpp
