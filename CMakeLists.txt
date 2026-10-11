add_library(display INTERFACE)

target_sources(display INTERFACE
 ${CMAKE_CURRENT_LIST_DIR}/display_link.c
)

target_include_directories(display INTERFACE ${CMAKE_CURRENT_LIST_DIR})
