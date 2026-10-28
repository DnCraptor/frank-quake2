# NES/SNES pad driver shared with the other FRANK projects.
add_library(nespad STATIC nespad.c)
target_include_directories(nespad PUBLIC ${CMAKE_CURRENT_LIST_DIR})
target_link_libraries(nespad PUBLIC pico_stdlib hardware_pio hardware_gpio)
