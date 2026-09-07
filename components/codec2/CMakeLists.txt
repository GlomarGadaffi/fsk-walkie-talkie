# Codec2 (David Rowe, LGPL-2.1) via sh123's ESP32 packaging, fetched by
# scripts/fetch_deps.sh into upstream/. Only mode 3200 is compiled in; the
# FreeDV/OFDM/LDPC modem sources are globbed too but gc-sections drops them.
file(GLOB CODEC2_SRCS "${CMAKE_CURRENT_LIST_DIR}/upstream/src/*.c")
if(NOT CODEC2_SRCS)
    message(FATAL_ERROR "components/codec2/upstream is empty -- run scripts/fetch_deps.sh")
endif()

idf_component_register(
    SRCS ${CODEC2_SRCS}
    INCLUDE_DIRS "upstream/src"
)

# Upstream C is not -Wall/-Werror clean; it is third-party, build it quietly.
target_compile_options(${COMPONENT_LIB} PRIVATE -w -Wno-error)
target_compile_definitions(${COMPONENT_LIB} PUBLIC
    CODEC2_MODE_EN_DEFAULT=0
    CODEC2_MODE_3200_EN=1)
