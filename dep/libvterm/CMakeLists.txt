add_library(vterm STATIC
  src/encoding.c
  src/keyboard.c
  src/mouse.c
  src/parser.c
  src/pen.c
  src/screen.c
  src/state.c
  src/unicode.c
  src/vterm.c
)

target_include_directories(vterm PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_definitions(vterm PRIVATE
  VTERM_VERSION_MAJOR=0
  VTERM_VERSION_MINOR=3
  VTERM_VERSION_PATCH=3
)

set_target_properties(vterm PROPERTIES
  C_STANDARD 99
  POSITION_INDEPENDENT_CODE ON
)
