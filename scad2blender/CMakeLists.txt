cmake_minimum_required(VERSION 3.16)
project(scad2blender VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find Flex and Bison
find_package(FLEX REQUIRED)
find_package(BISON REQUIRED)

# Generate lexer and parser
FLEX_TARGET(ScadLexer
    ${CMAKE_CURRENT_SOURCE_DIR}/src/lexer.l
    ${CMAKE_CURRENT_BINARY_DIR}/lexer.cpp
)

BISON_TARGET(ScadParser
    ${CMAKE_CURRENT_SOURCE_DIR}/src/parser.y
    ${CMAKE_CURRENT_BINARY_DIR}/parser.cpp
    DEFINES_FILE ${CMAKE_CURRENT_BINARY_DIR}/parser.h
)

ADD_FLEX_BISON_DEPENDENCY(ScadLexer ScadParser)

# Include directories
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_BINARY_DIR}
)

# Source files
set(SOURCES
    src/main.cpp
    src/ast.cpp
    src/blender_generator.cpp
    src/value.cpp
    ${FLEX_ScadLexer_OUTPUTS}
    ${BISON_ScadParser_OUTPUTS}
)

# Create executable
add_executable(scad2blender ${SOURCES})

# Install
install(TARGETS scad2blender RUNTIME DESTINATION bin)
