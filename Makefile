# Makefile za SS projekat - asembler, linker i emulator
CC      := g++
FLEX    := flex
BISON   := bison
CFLAGS  := -std=c++17 -Wall -Wextra -g
IFLAGS  := -I inc

.PHONY: all clean test test_asm test_link test_emu

all: asembler linker emulator


# Pravilo: bilo koji .o zavisi od odgovarajuceg .cpp i svih headera u inc/
%.o: %.cpp inc/assembler.hpp inc/linker.hpp inc/emulator.hpp inc/types.hpp
	$(CC) $(CFLAGS) $(IFLAGS) -c $< -o $@

%.o: %.c inc/assembler.hpp inc/linker.hpp inc/emulator.hpp inc/types.hpp
	$(CC) $(CFLAGS) $(IFLAGS) -c $< -o $@


ASSEMBLER_OBJS := src/assembler.o misc/parser.tab.o misc/lex.yy.o

asembler: $(ASSEMBLER_OBJS)
	$(CC) $(CFLAGS) -o $@ $^

# Bison generise parser.tab.c i parser.tab.h iz parser.y
misc/parser.tab.c misc/parser.tab.h: misc/parser.y
	$(BISON) -Wall -d -o misc/parser.tab.c misc/parser.y

# Flex generise lex.yy.c iz lexer.l (zavisi od parser.tab.h)
misc/lex.yy.c: misc/lexer.l misc/parser.tab.h
	$(FLEX) -o misc/lex.yy.c misc/lexer.l


linker: src/linker.o
	$(CC) $(CFLAGS) -o $@ $^


emulator: src/emulator.o
	$(CC) $(CFLAGS) -o $@ $^


clean:
	rm -f asembler linker emulator \
	    misc/parser.tab.c misc/parser.tab.h misc/parser.tab.o \
	    misc/lex.yy.c misc/lex.yy.o \
	    src/*.o \
	    tests/javni/*.o tests/javni/*.hex