cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(ra8d1_skeleton)

target_sources(app PRIVATE src/main.c)
