include(FetchContent)

# Fetch only the upstream sources; do not configure SOES's platform demos.
FetchContent_Declare(soes
    GIT_REPOSITORY https://github.com/OpenEtherCATsociety/SOES.git
    GIT_TAG 6ef7b9415e936c061ad30626ca00004bdde2ac62
    SOURCE_SUBDIR librmcs-unused
)
FetchContent_MakeAvailable(soes)

add_library(evk_soes STATIC
    "${soes_SOURCE_DIR}/soes/esc.c"
    "${soes_SOURCE_DIR}/soes/esc_coe.c"
    "${soes_SOURCE_DIR}/soes/esc_eep.c"
    "${soes_SOURCE_DIR}/soes/ecat_slv.c"
)
target_include_directories(evk_soes PUBLIC
    "${soes_SOURCE_DIR}/soes"
    "${soes_SOURCE_DIR}/soes/include/sys/gcc"
    "${CMAKE_CURRENT_LIST_DIR}/../app/src/ecat"
)
set_target_properties(evk_soes PROPERTIES C_STANDARD 11 C_EXTENSIONS OFF)
