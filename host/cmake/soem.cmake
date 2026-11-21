include(FetchContent)
FetchContent_Declare(librmcs_soem
    GIT_REPOSITORY https://github.com/OpenEtherCATsociety/SOEM.git
    GIT_TAG abbf0d42e38d6cfbaa4c1e9e8e07ace651c386fd
    SOURCE_SUBDIR librmcs-unused
)
FetchContent_MakeAvailable(librmcs_soem)
file(GLOB librmcs_soem_sources
    "${librmcs_soem_SOURCE_DIR}/soem/*.c"
    "${librmcs_soem_SOURCE_DIR}/oshw/linux/*.c"
)
add_library(librmcs_soem_backend STATIC ${librmcs_soem_sources}
    "${librmcs_soem_SOURCE_DIR}/osal/linux/osal.c")
set_target_properties(librmcs_soem_backend PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_definitions(librmcs_soem_backend PRIVATE _GNU_SOURCE)
target_include_directories(librmcs_soem_backend SYSTEM PUBLIC
    "${librmcs_soem_SOURCE_DIR}/soem"
    "${librmcs_soem_SOURCE_DIR}/osal"
    "${librmcs_soem_SOURCE_DIR}/osal/linux"
    "${librmcs_soem_SOURCE_DIR}/oshw/linux")
target_link_libraries(librmcs_soem_backend PUBLIC Threads::Threads rt)
