add_library(
   forge_asio STATIC
   affine.cpp
   async_waiter.cpp
   cancellation_scope.cpp
   cancellation_scope_impl.cpp
   child_signal_registration.cpp
   child_signal_registration_impl.cpp
   child_signal_registration_test_access.cpp
   compute.cpp
   gate.cpp
   notification.cpp
   notification_impl.cpp
   notification_waiter.cpp
   runtime.cpp
   stop_state.cpp
   task.cpp
   thread_name.cpp
)
forge_target_modules(forge_asio asio)

target_include_directories(forge_asio PUBLIC ${FORGE_PUBLIC_INCLUDE_DIRS})

target_link_libraries(
   forge_asio PUBLIC
      Boost::asio
      forge_exceptions
   PRIVATE
      Threads::Threads
)
