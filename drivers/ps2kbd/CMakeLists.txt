add_library(ps2kbd STATIC ps2kbd.c)
pico_generate_pio_header(ps2kbd ${CMAKE_CURRENT_LIST_DIR}/ps2kbd.pio)
target_include_directories(ps2kbd PUBLIC ${CMAKE_CURRENT_LIST_DIR})
target_link_libraries(ps2kbd PUBLIC pico_stdlib hardware_pio hardware_gpio hardware_irq hardware_clocks)
