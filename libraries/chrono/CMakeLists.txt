add_library(
   forge_chrono STATIC
   iso8601.cpp
   timestamp.cpp
   relative.cpp
)
target_sources(
   forge_chrono PUBLIC
      FILE_SET CXX_MODULES
      BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
      FILES
         include/forge/chrono/iso8601.cppm
         include/forge/chrono/relative.cppm
         include/forge/chrono/timestamp.cppm
)
target_compile_features(forge_chrono PUBLIC cxx_std_23)

target_include_directories(forge_chrono PUBLIC ${FORGE_PUBLIC_INCLUDE_DIRS})
target_link_libraries(forge_chrono PRIVATE Boost::date_time)
