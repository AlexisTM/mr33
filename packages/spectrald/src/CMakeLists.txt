cmake_minimum_required(VERSION 3.13)
project(spectrald C)

add_compile_options(-Wall -Wextra -Werror -Wno-unused-parameter -std=gnu11)
add_compile_definitions(_GNU_SOURCE)

add_executable(spectrald main.c nl.c spectral.c fft.c chan.c util.c)
target_link_libraries(spectrald ubus ubox nl-tiny m)

install(TARGETS spectrald RUNTIME DESTINATION sbin)
