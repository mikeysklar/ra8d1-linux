cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(ra8d1_arcade)

# emu/ is portable C99 and knows nothing about Zephyr or this board. It is
# compiled unchanged by host/Makefile for the test harnesses.
target_sources(app PRIVATE
	emu/z80.c
	emu/pacman.c
	emu/romimage.c
)

# src/ is the platform shim plus glue. platform_zephyr.c is the only file
# that knows what board this is, and it takes its facts from devicetree.
target_sources(app PRIVATE
	src/main.c
	src/platform_zephyr.c
)

# The display stage owns video.c. It builds in only once a board actually has
# a panel configured, so the two stages can land independently.
#
# Gate on a chosen zephyr,display rather than on CONFIG_DISPLAY. They are not
# the same condition: CONFIG_DISPLAY only says the display subsystem is
# compiled in, and it can be enabled on a board with no panel wired - for
# ek_ra8d1 the panel arrives with SHIELD=rtkmipilcdb00000be, not with the
# board. Gating on the Kconfig symbol makes a shieldless build fail at
# video.c's BUILD_ASSERT instead of quietly building headless.
#
# ARCADE_HAS_DISPLAY carries the same condition into C. main.c uses that
# rather than testing devicetree itself, so it keeps its rule of including no
# Zephyr headers.
dt_chosen(arcade_display_node PROPERTY "zephyr,display")
if(DEFINED arcade_display_node)
	target_sources(app PRIVATE src/video.c)
	target_compile_definitions(app PRIVATE ARCADE_HAS_DISPLAY)
endif()

target_include_directories(app PRIVATE src emu)
