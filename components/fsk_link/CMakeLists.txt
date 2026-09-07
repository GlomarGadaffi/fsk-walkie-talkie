idf_component_register(
    SRCS "fsk_link.cpp"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES RadioLib esp_driver_spi esp_driver_gpio esp_timer esp_hw_support
)

# RadioLib headers hide overloaded virtuals; IDF builds -Werror.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-overloaded-virtual)

# RadioLib itself (managed component, 7.1.2, declared in idf_component.yml):
# same warning, and only the SX126x driver is wanted. Only module excludes --
# the protocol ones cascade (BellModem needs AFSK, APRS needs AX25).
idf_component_get_property(radiolib_lib RadioLib COMPONENT_LIB)
target_compile_options(${radiolib_lib} PRIVATE -Wno-overloaded-virtual)
target_compile_definitions(${radiolib_lib} PUBLIC
    RADIOLIB_EXCLUDE_CC1101 RADIOLIB_EXCLUDE_NRF24 RADIOLIB_EXCLUDE_RF69
    RADIOLIB_EXCLUDE_SX1231 RADIOLIB_EXCLUDE_SI443X RADIOLIB_EXCLUDE_RFM2X
    RADIOLIB_EXCLUDE_SX127X RADIOLIB_EXCLUDE_SX128X)
