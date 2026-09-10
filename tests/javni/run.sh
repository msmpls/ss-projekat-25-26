#!/bin/bash
cd ~/Desktop/SS/resenje

./asembler -o tests/javni/main.o tests/javni/main.s
./asembler -o tests/javni/math.o tests/javni/math.s
./asembler -o tests/javni/handler.o tests/javni/handler.s
./asembler -o tests/javni/isr_timer.o tests/javni/isr_timer.s
./asembler -o tests/javni/isr_terminal.o tests/javni/isr_terminal.s
./asembler -o tests/javni/isr_software.o tests/javni/isr_software.s

./linker -hex \
  -place=my_code@0x40000000 -place=math@0xF0000000 \
  -o tests/javni/program.hex \
  tests/javni/handler.o tests/javni/math.o tests/javni/main.o \
  tests/javni/isr_terminal.o tests/javni/isr_timer.o tests/javni/isr_software.o

./emulator tests/javni/program.hex
