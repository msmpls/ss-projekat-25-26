/* A Bison parser, made by GNU Bison 3.5.1.  */

/* Bison implementation for Yacc-like parsers in C

   Copyright (C) 1984, 1989-1990, 2000-2015, 2018-2020 Free Software Foundation,
   Inc.

   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.  */

/* As a special exception, you may create a larger work that contains
   part or all of the Bison parser skeleton and distribute that work
   under terms of your choice, so long as that work isn't itself a
   parser generator using the skeleton or a modified version thereof
   as a parser skeleton.  Alternatively, if you modify or redistribute
   the parser skeleton itself, you may (at your option) remove this
   special exception, which will cause the skeleton and the resulting
   Bison output files to be licensed under the GNU General Public
   License without this special exception.

   This special exception was added by the Free Software Foundation in
   version 2.2 of Bison.  */

/* C LALR(1) parser skeleton written by Richard Stallman, by
   simplifying the original so-called "semantic" parser.  */

/* All symbols defined below should begin with yy or YY, to avoid
   infringing on user name space.  This should be done even for local
   variables, as they might otherwise be expanded by user macros.
   There are some unavoidable exceptions within include files to
   define necessary library symbols; they are noted "INFRINGES ON
   USER NAME SPACE" below.  */

/* Undocumented macros, especially those whose name start with YY_,
   are private implementation details.  Do not rely on them.  */

/* Identify Bison output.  */
#define YYBISON 1

/* Bison version.  */
#define YYBISON_VERSION "3.5.1"

/* Skeleton name.  */
#define YYSKELETON_NAME "yacc.c"

/* Pure parsers.  */
#define YYPURE 0

/* Push parsers.  */
#define YYPUSH 0

/* Pull parsers.  */
#define YYPULL 1




/* First part of user prologue.  */
#line 63 "misc/parser.y"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <iostream>
#include <fstream>
#include <string>

#include "../inc/assembler.hpp"
#include "../inc/types.hpp"

using namespace std;

extern int yylex();
extern int yylineno;
extern FILE* yyin;

void yyerror(const char* s);

// Globalni asembler (jedna instanca po pokretanju)
Assembler assm;

// Lista simbola za .global / .extern (akumulira se u symbol_list produkcijama)
std::vector<std::string> sym_list;

// Trenutno stanje parsiranja operanada
// (parser akcije postavljaju ove vrednosti, instrukcija ih cita)
struct OperandState {
    int         value;            // numericka vrednost (ako je poznata)
    std::string pending_symbol;   // ime simbola ako nije definisan
    char        action;           // '+' ili '-' (kako se simbol odnosi)
    int         addend;           // konstantni deo
    bool        is_pc_relative;   // da li je PC-rel (za skokove i %sym notaciju)
    bool        is_memory;        // da li je memorijsko adresiranje [reg+...]
    bool        is_immediate;     // da li je $literal / $simbol
    bool        is_register;      // da li je sam REGISTER
    bool        is_bare_symbol;   // da li je sam <simbol> ili <literal> (bez $ i bez [])
    int         reg;              // indeks registra (ako je is_register ili memorijsko)
    int         addr_mode;        // modifikator instrukcije (postavlja se za ld/st)
} ops;

// Pomocna za reset stanja operanada pre svake instrukcije
static void reset_operand_state() {
    ops.value = 0;
    ops.pending_symbol = "";
    ops.action = '+';
    ops.addend = 0;
    ops.is_pc_relative = false;
    ops.is_memory = false;
    ops.is_immediate = false;
    ops.is_register = false;
    ops.is_bare_symbol = false;
    ops.reg = 0;
    ops.addr_mode = 0;
}

// Pomocna za emitovanje relokacije / patch-a za 12-bitni Disp
// Poziva se iz instrukcija koje koriste simbol u Disp polju.
// offset = adresa instrukcije u sekciji
// pc_rel = da li je PC-relativno (za skokove)
static void emit_disp_patch(int offset, bool pc_rel) {
    if (!ops.pending_symbol.empty()) {
        // Simbol nije poznat (ili je poznat ali u drugoj sekciji)
        // Proveri da li je poznat
        int val = assm.getSymbolValue(ops.pending_symbol);
        if (val >= 0 && assm.getSymbolSection(ops.pending_symbol) == assm.currentSection()) {
            // Simbol je u istoj sekciji - izracunaj disp
            int disp;
            if (pc_rel) {
                disp = val - (offset + 4);  // +4 jer pc pokazuje na sledecu inst
            } else {
                disp = val + ops.addend;
            }
            if (disp < -2048 || disp > 2047) {
                yyerror("Displacement out of 12-bit range");
            }
            // Upisi u instrukciju
            uint32_t instr = assm.getLastWord();
            instr = (instr & 0xFFFFF000) | (disp & 0xFFF);
            assm.updateLastWord(instr);
        } else {
            // Simbol nije poznat ili je u drugoj sekciji -> patch / relokacija
            RelocType type = pc_rel ? RelocType::R_PC_REL_12 : RelocType::R_ABS_12;
            assm.addPatch(ops.pending_symbol, offset, ops.action, ops.addend, type);
        }
    } else {
        // Imamo numericku vrednost - upisi direktno
        int disp = pc_rel ? (ops.value - (offset + 4)) : ops.value;
        if (disp < -2048 || disp > 2047) {
            yyerror("Displacement out of 12-bit range");
        }
        uint32_t instr = assm.getLastWord();
        instr = (instr & 0xFFFFF000) | (disp & 0xFFF);
        assm.updateLastWord(instr);
    }
}

// Pomocna za emitovanje 32-bitne apsolutne vrednosti (.word)
// Poziva se iz .word akcija.
static void emit_word_value(int value, const std::string& symbol, char action, int addend) {
    if (symbol.empty()) {
        // Imamo numericku vrednost - upisi direktno
        assm.emitWord((uint32_t)value);
    } else {
        // Simbol - uvek emituj placeholder + R_ABS_32 relokaciju.
        // Asembler ne zna finalnu adresu sekcije, pa ne sme da upisuje sirov offset.
        int offset = assm.locationCounter();
        assm.emitWord(0);  // placeholder
        assm.addPatch(symbol, offset, action, addend, RelocType::R_ABS_32);
    }
}

// Pomocna za emitovanje call/jmp/beq/bne/bgt instrukcija.
// Koristi PC-relativnu direktnu varijantu ako cilj staje u 12-bitni signed opseg.
// Inace koristi indirektnu varijantu (MOD 0x1 za call, MOD 0x8 za jmp) sa
// "literal pool" word-om odmah iza instrukcije koji sadrzi 32-bitnu ciljnu adresu.
//
// Parametri:
//   oc       - opkod (0x2 za call, 0x3 za jmp/beq/bne/bgt)
//   mod_direct - MOD za direktnu varijantu (0x0 za jmp/call, 0x1-0x3 za uslovne)
//   regB, regC - registri za uslovne skokove (B i C polja)
//   is_call  - true za call (indirektni MOD=0x1), false za jmp (indirektni MOD=0x8|mod_direct)
static void emit_jump_or_call(uint8_t oc, uint8_t mod_direct,
                              uint8_t regB, uint8_t regC, bool is_call) {
    // Proveri da li cilj staje u 12-bitni signed opseg
    bool use_indirect = false;

    if (ops.pending_symbol.empty()) {
        // Literal - proveri opseg
        int disp = ops.value - (assm.locationCounter() + 4);
        if (disp < -2048 || disp > 2047) {
            use_indirect = true;
        }
    } else {
        // Simbol - ako je poznat u istoj sekciji, proveri opseg
        int val = assm.getSymbolValue(ops.pending_symbol);
        if (val >= 0 && assm.getSymbolSection(ops.pending_symbol) == assm.currentSection()) {
            int disp = val - (assm.locationCounter() + 4);
            if (disp < -2048 || disp > 2047) {
                use_indirect = true;
            }
        } else {
            // Extern ili cross-section - uvek koristi indirektno jer ne znamo adresu
            use_indirect = true;
        }
    }

    if (!use_indirect) {
        // Direktna PC-rel varijanta
        int offset = assm.emitInstruction(oc, mod_direct, 15, regB, regC, 0);
        // Sad upiši Disp
        if (ops.pending_symbol.empty()) {
            int disp = ops.value - (offset + 4);
            uint32_t instr = assm.getLastWord();
            instr = (instr & 0xFFFFF000) | (disp & 0xFFF);
            assm.updateLastWord(instr);
        } else {
            // addPatch će se izvršiti kroz emit_disp_patch logiku ovde
            int val = assm.getSymbolValue(ops.pending_symbol);
            int disp = val - (offset + 4);
            if (val >= 0) {
                // Poznat u istoj sekciji - upiši direktno
                uint32_t instr = assm.getLastWord();
                instr = (instr & 0xFFFFF000) | (disp & 0xFFF);
                assm.updateLastWord(instr);
            } else {
                // Nije poznat - registruj patch (linker će računati S-P)
                assm.addPatch(ops.pending_symbol, offset, '+', 0, RelocType::R_PC_REL_12);
            }
        }
    } else {
        // Indirektna varijanta: instrukcija + jmp skip + pool word
        uint8_t mod_indirect = is_call ? 0x1 : (0x8 | mod_direct);

        if (is_call || mod_direct == 0x0) {
            // call ili jmp - uvek skače.
            // Za call: povratna adresa je cur_pc+4 (jmp instrukcija).
            //           call čita pool sa cur_pc+8 (D=4).
            //           posle ret, pc je cur_pc+4 (jmp +4), što preskacuje pool!
            // Za jmp: ne vraća se, ali jmp +4 je tu zbog call-a, ne škodi.
            //         jmp čita pool sa cur_pc+8 (D=4).
            //         posle jmp, ne izvršava se jmp +4.
            assm.emitInstruction(oc, mod_indirect, 15, regB, regC, 4);
            assm.emitInstruction(0x3, 0x0, 15, 0, 0, 4);  // jmp +4 (preskakanje pool-a)
            int pool_offset = assm.locationCounter();
            if (ops.pending_symbol.empty()) {
                assm.emitWord((uint32_t)ops.value);
            } else {
                assm.emitWord(0);
                assm.addPatch(ops.pending_symbol, pool_offset, '+', 0, RelocType::R_ABS_32);
            }
        } else {
            // Conditional branch (beq/bne/bgt) - može pasti kroz.
            // Sekvenca:
            //   1. beq/bne/bgt indirektno (pool je 8 bajtova ispred, D=4)
            //   2. jmp +4 (ako uslov nije ispunjen, skače preko pool-a)
            //   3. pool word (ciljna adresa)
            //   4. sledeća instrukcija
            assm.emitInstruction(oc, mod_indirect, 15, regB, regC, 4);
            assm.emitInstruction(0x3, 0x0, 15, 0, 0, 4);  // jmp +4 (preko pool-a)
            int pool_offset = assm.locationCounter();
            if (ops.pending_symbol.empty()) {
                assm.emitWord((uint32_t)ops.value);
            } else {
                assm.emitWord(0);
                assm.addPatch(ops.pending_symbol, pool_offset, '+', 0, RelocType::R_ABS_32);
            }
        }
    }
}

// Pomocna za emitovanje "pool word"-a (32-bitna konstanta/adresa) na trenutnoj lokaciji.
// Vraca offset pool-a (za slučaj da pozivalac treba da ga koristi).
// Ako je simbol poznat, upisuje se direktno; inače R_ABS_32 relokacija.
static int emit_pool_word() {
    int pool_offset = assm.locationCounter();
    if (ops.pending_symbol.empty()) {
        assm.emitWord((uint32_t)ops.value);
    } else {
        assm.emitWord(0);
        assm.addPatch(ops.pending_symbol, pool_offset, '+', 0, RelocType::R_ABS_32);
    }
    return pool_offset;
}

// Proverava da li ops.value (ili simbol) staje u 12-bitni signed opseg [-2048, 2047].
// Za simbole: ako je poznat u istoj sekciji, proverava vrednost; inače true (mora pool).
static bool needs_pool_for_value() {
    if (ops.pending_symbol.empty()) {
        // Za literale proveravamo opseg
        return (ops.value < -2048 || ops.value > 2047);
    }
    // Asembler ne zna finalnu baznu adresu sekcije (linker je određuje),
    // pa apsolutne reference na simbole uvek moraju kroz pool (R_ABS_32).
    return true;
}

// Pomocna za obradu .ascii stringa (uklanja navodnike, interpretira escape)
static void emit_ascii_string(const char* raw) {
    // raw ukljucuje navodnike
    std::string s(raw);
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"') {
        s = s.substr(1, s.size() - 2);
    }
    // Interpretiraj escape sekvence
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            char c = s[i+1];
            switch (c) {
                case 'n':  assm.emitByte('\n'); break;
                case 't':  assm.emitByte('\t'); break;
                case 'r':  assm.emitByte('\r'); break;
                case '\\': assm.emitByte('\\'); break;
                case '"':  assm.emitByte('"');  break;
                case '0':  assm.emitByte('\0'); break;
                default:   assm.emitByte(c);    break;
            }
            i++;  // preskoci sledeci karakter
        } else {
            assm.emitByte((uint8_t)s[i]);
        }
    }
}


#line 337 "misc/parser.tab.c"

# ifndef YY_CAST
#  ifdef __cplusplus
#   define YY_CAST(Type, Val) static_cast<Type> (Val)
#   define YY_REINTERPRET_CAST(Type, Val) reinterpret_cast<Type> (Val)
#  else
#   define YY_CAST(Type, Val) ((Type) (Val))
#   define YY_REINTERPRET_CAST(Type, Val) ((Type) (Val))
#  endif
# endif
# ifndef YY_NULLPTR
#  if defined __cplusplus
#   if 201103L <= __cplusplus
#    define YY_NULLPTR nullptr
#   else
#    define YY_NULLPTR 0
#   endif
#  else
#   define YY_NULLPTR ((void*)0)
#  endif
# endif

/* Enabling verbose error messages.  */
#ifdef YYERROR_VERBOSE
# undef YYERROR_VERBOSE
# define YYERROR_VERBOSE 1
#else
# define YYERROR_VERBOSE 0
#endif

/* Use api.header.include to #include this header
   instead of duplicating it here.  */
#ifndef YY_YY_MISC_PARSER_TAB_H_INCLUDED
# define YY_YY_MISC_PARSER_TAB_H_INCLUDED
/* Debug traces.  */
#ifndef YYDEBUG
# define YYDEBUG 0
#endif
#if YYDEBUG
extern int yydebug;
#endif

/* Token type.  */
#ifndef YYTOKENTYPE
# define YYTOKENTYPE
  enum yytokentype
  {
    GLOBAL = 258,
    EXTERN = 259,
    SECTION = 260,
    WORD = 261,
    SKIP = 262,
    ASCII = 263,
    END = 264,
    HALT = 265,
    INT = 266,
    IRET = 267,
    CALL = 268,
    RET = 269,
    JMP = 270,
    BEQ = 271,
    BNE = 272,
    BGT = 273,
    PUSH = 274,
    POP = 275,
    XCHG = 276,
    ADD = 277,
    SUB = 278,
    MUL = 279,
    DIV = 280,
    NOT = 281,
    AND = 282,
    OR = 283,
    XOR = 284,
    SHL = 285,
    SHR = 286,
    LD = 287,
    ST = 288,
    CSRRD = 289,
    CSRWR = 290,
    DOLLAR = 291,
    LBRACKET = 292,
    RBRACKET = 293,
    COLON = 294,
    COMMA = 295,
    PLUS = 296,
    MINUS = 297,
    STAR = 298,
    SLASH = 299,
    LPAREN = 300,
    RPAREN = 301,
    EOL = 302,
    NUMBER = 303,
    REGISTER = 304,
    CSR = 305,
    IDENT = 306,
    STRING = 307
  };
#endif

/* Value type.  */
#if ! defined YYSTYPE && ! defined YYSTYPE_IS_DECLARED
union YYSTYPE
{
#line 330 "misc/parser.y"

    int  num;
    char *str;

#line 447 "misc/parser.tab.c"

};
typedef union YYSTYPE YYSTYPE;
# define YYSTYPE_IS_TRIVIAL 1
# define YYSTYPE_IS_DECLARED 1
#endif


extern YYSTYPE yylval;

int yyparse (void);

#endif /* !YY_YY_MISC_PARSER_TAB_H_INCLUDED  */



#ifdef short
# undef short
#endif

/* On compilers that do not define __PTRDIFF_MAX__ etc., make sure
   <limits.h> and (if available) <stdint.h> are included
   so that the code can choose integer types of a good width.  */

#ifndef __PTRDIFF_MAX__
# include <limits.h> /* INFRINGES ON USER NAME SPACE */
# if defined __STDC_VERSION__ && 199901 <= __STDC_VERSION__
#  include <stdint.h> /* INFRINGES ON USER NAME SPACE */
#  define YY_STDINT_H
# endif
#endif

/* Narrow types that promote to a signed type and that can represent a
   signed or unsigned integer of at least N bits.  In tables they can
   save space and decrease cache pressure.  Promoting to a signed type
   helps avoid bugs in integer arithmetic.  */

#ifdef __INT_LEAST8_MAX__
typedef __INT_LEAST8_TYPE__ yytype_int8;
#elif defined YY_STDINT_H
typedef int_least8_t yytype_int8;
#else
typedef signed char yytype_int8;
#endif

#ifdef __INT_LEAST16_MAX__
typedef __INT_LEAST16_TYPE__ yytype_int16;
#elif defined YY_STDINT_H
typedef int_least16_t yytype_int16;
#else
typedef short yytype_int16;
#endif

#if defined __UINT_LEAST8_MAX__ && __UINT_LEAST8_MAX__ <= __INT_MAX__
typedef __UINT_LEAST8_TYPE__ yytype_uint8;
#elif (!defined __UINT_LEAST8_MAX__ && defined YY_STDINT_H \
       && UINT_LEAST8_MAX <= INT_MAX)
typedef uint_least8_t yytype_uint8;
#elif !defined __UINT_LEAST8_MAX__ && UCHAR_MAX <= INT_MAX
typedef unsigned char yytype_uint8;
#else
typedef short yytype_uint8;
#endif

#if defined __UINT_LEAST16_MAX__ && __UINT_LEAST16_MAX__ <= __INT_MAX__
typedef __UINT_LEAST16_TYPE__ yytype_uint16;
#elif (!defined __UINT_LEAST16_MAX__ && defined YY_STDINT_H \
       && UINT_LEAST16_MAX <= INT_MAX)
typedef uint_least16_t yytype_uint16;
#elif !defined __UINT_LEAST16_MAX__ && USHRT_MAX <= INT_MAX
typedef unsigned short yytype_uint16;
#else
typedef int yytype_uint16;
#endif

#ifndef YYPTRDIFF_T
# if defined __PTRDIFF_TYPE__ && defined __PTRDIFF_MAX__
#  define YYPTRDIFF_T __PTRDIFF_TYPE__
#  define YYPTRDIFF_MAXIMUM __PTRDIFF_MAX__
# elif defined PTRDIFF_MAX
#  ifndef ptrdiff_t
#   include <stddef.h> /* INFRINGES ON USER NAME SPACE */
#  endif
#  define YYPTRDIFF_T ptrdiff_t
#  define YYPTRDIFF_MAXIMUM PTRDIFF_MAX
# else
#  define YYPTRDIFF_T long
#  define YYPTRDIFF_MAXIMUM LONG_MAX
# endif
#endif

#ifndef YYSIZE_T
# ifdef __SIZE_TYPE__
#  define YYSIZE_T __SIZE_TYPE__
# elif defined size_t
#  define YYSIZE_T size_t
# elif defined __STDC_VERSION__ && 199901 <= __STDC_VERSION__
#  include <stddef.h> /* INFRINGES ON USER NAME SPACE */
#  define YYSIZE_T size_t
# else
#  define YYSIZE_T unsigned
# endif
#endif

#define YYSIZE_MAXIMUM                                  \
  YY_CAST (YYPTRDIFF_T,                                 \
           (YYPTRDIFF_MAXIMUM < YY_CAST (YYSIZE_T, -1)  \
            ? YYPTRDIFF_MAXIMUM                         \
            : YY_CAST (YYSIZE_T, -1)))

#define YYSIZEOF(X) YY_CAST (YYPTRDIFF_T, sizeof (X))

/* Stored state numbers (used for stacks). */
typedef yytype_uint8 yy_state_t;

/* State numbers in computations.  */
typedef int yy_state_fast_t;

#ifndef YY_
# if defined YYENABLE_NLS && YYENABLE_NLS
#  if ENABLE_NLS
#   include <libintl.h> /* INFRINGES ON USER NAME SPACE */
#   define YY_(Msgid) dgettext ("bison-runtime", Msgid)
#  endif
# endif
# ifndef YY_
#  define YY_(Msgid) Msgid
# endif
#endif

#ifndef YY_ATTRIBUTE_PURE
# if defined __GNUC__ && 2 < __GNUC__ + (96 <= __GNUC_MINOR__)
#  define YY_ATTRIBUTE_PURE __attribute__ ((__pure__))
# else
#  define YY_ATTRIBUTE_PURE
# endif
#endif

#ifndef YY_ATTRIBUTE_UNUSED
# if defined __GNUC__ && 2 < __GNUC__ + (7 <= __GNUC_MINOR__)
#  define YY_ATTRIBUTE_UNUSED __attribute__ ((__unused__))
# else
#  define YY_ATTRIBUTE_UNUSED
# endif
#endif

/* Suppress unused-variable warnings by "using" E.  */
#if ! defined lint || defined __GNUC__
# define YYUSE(E) ((void) (E))
#else
# define YYUSE(E) /* empty */
#endif

#if defined __GNUC__ && ! defined __ICC && 407 <= __GNUC__ * 100 + __GNUC_MINOR__
/* Suppress an incorrect diagnostic about yylval being uninitialized.  */
# define YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN                            \
    _Pragma ("GCC diagnostic push")                                     \
    _Pragma ("GCC diagnostic ignored \"-Wuninitialized\"")              \
    _Pragma ("GCC diagnostic ignored \"-Wmaybe-uninitialized\"")
# define YY_IGNORE_MAYBE_UNINITIALIZED_END      \
    _Pragma ("GCC diagnostic pop")
#else
# define YY_INITIAL_VALUE(Value) Value
#endif
#ifndef YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN
# define YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN
# define YY_IGNORE_MAYBE_UNINITIALIZED_END
#endif
#ifndef YY_INITIAL_VALUE
# define YY_INITIAL_VALUE(Value) /* Nothing. */
#endif

#if defined __cplusplus && defined __GNUC__ && ! defined __ICC && 6 <= __GNUC__
# define YY_IGNORE_USELESS_CAST_BEGIN                          \
    _Pragma ("GCC diagnostic push")                            \
    _Pragma ("GCC diagnostic ignored \"-Wuseless-cast\"")
# define YY_IGNORE_USELESS_CAST_END            \
    _Pragma ("GCC diagnostic pop")
#endif
#ifndef YY_IGNORE_USELESS_CAST_BEGIN
# define YY_IGNORE_USELESS_CAST_BEGIN
# define YY_IGNORE_USELESS_CAST_END
#endif


#define YY_ASSERT(E) ((void) (0 && (E)))

#if ! defined yyoverflow || YYERROR_VERBOSE

/* The parser invokes alloca or malloc; define the necessary symbols.  */

# ifdef YYSTACK_USE_ALLOCA
#  if YYSTACK_USE_ALLOCA
#   ifdef __GNUC__
#    define YYSTACK_ALLOC __builtin_alloca
#   elif defined __BUILTIN_VA_ARG_INCR
#    include <alloca.h> /* INFRINGES ON USER NAME SPACE */
#   elif defined _AIX
#    define YYSTACK_ALLOC __alloca
#   elif defined _MSC_VER
#    include <malloc.h> /* INFRINGES ON USER NAME SPACE */
#    define alloca _alloca
#   else
#    define YYSTACK_ALLOC alloca
#    if ! defined _ALLOCA_H && ! defined EXIT_SUCCESS
#     include <stdlib.h> /* INFRINGES ON USER NAME SPACE */
      /* Use EXIT_SUCCESS as a witness for stdlib.h.  */
#     ifndef EXIT_SUCCESS
#      define EXIT_SUCCESS 0
#     endif
#    endif
#   endif
#  endif
# endif

# ifdef YYSTACK_ALLOC
   /* Pacify GCC's 'empty if-body' warning.  */
#  define YYSTACK_FREE(Ptr) do { /* empty */; } while (0)
#  ifndef YYSTACK_ALLOC_MAXIMUM
    /* The OS might guarantee only one guard page at the bottom of the stack,
       and a page size can be as small as 4096 bytes.  So we cannot safely
       invoke alloca (N) if N exceeds 4096.  Use a slightly smaller number
       to allow for a few compiler-allocated temporary stack slots.  */
#   define YYSTACK_ALLOC_MAXIMUM 4032 /* reasonable circa 2006 */
#  endif
# else
#  define YYSTACK_ALLOC YYMALLOC
#  define YYSTACK_FREE YYFREE
#  ifndef YYSTACK_ALLOC_MAXIMUM
#   define YYSTACK_ALLOC_MAXIMUM YYSIZE_MAXIMUM
#  endif
#  if (defined __cplusplus && ! defined EXIT_SUCCESS \
       && ! ((defined YYMALLOC || defined malloc) \
             && (defined YYFREE || defined free)))
#   include <stdlib.h> /* INFRINGES ON USER NAME SPACE */
#   ifndef EXIT_SUCCESS
#    define EXIT_SUCCESS 0
#   endif
#  endif
#  ifndef YYMALLOC
#   define YYMALLOC malloc
#   if ! defined malloc && ! defined EXIT_SUCCESS
void *malloc (YYSIZE_T); /* INFRINGES ON USER NAME SPACE */
#   endif
#  endif
#  ifndef YYFREE
#   define YYFREE free
#   if ! defined free && ! defined EXIT_SUCCESS
void free (void *); /* INFRINGES ON USER NAME SPACE */
#   endif
#  endif
# endif
#endif /* ! defined yyoverflow || YYERROR_VERBOSE */


#if (! defined yyoverflow \
     && (! defined __cplusplus \
         || (defined YYSTYPE_IS_TRIVIAL && YYSTYPE_IS_TRIVIAL)))

/* A type that is properly aligned for any stack member.  */
union yyalloc
{
  yy_state_t yyss_alloc;
  YYSTYPE yyvs_alloc;
};

/* The size of the maximum gap between one aligned stack and the next.  */
# define YYSTACK_GAP_MAXIMUM (YYSIZEOF (union yyalloc) - 1)

/* The size of an array large to enough to hold all stacks, each with
   N elements.  */
# define YYSTACK_BYTES(N) \
     ((N) * (YYSIZEOF (yy_state_t) + YYSIZEOF (YYSTYPE)) \
      + YYSTACK_GAP_MAXIMUM)

# define YYCOPY_NEEDED 1

/* Relocate STACK from its old location to the new one.  The
   local variables YYSIZE and YYSTACKSIZE give the old and new number of
   elements in the stack, and YYPTR gives the new location of the
   stack.  Advance YYPTR to a properly aligned location for the next
   stack.  */
# define YYSTACK_RELOCATE(Stack_alloc, Stack)                           \
    do                                                                  \
      {                                                                 \
        YYPTRDIFF_T yynewbytes;                                         \
        YYCOPY (&yyptr->Stack_alloc, Stack, yysize);                    \
        Stack = &yyptr->Stack_alloc;                                    \
        yynewbytes = yystacksize * YYSIZEOF (*Stack) + YYSTACK_GAP_MAXIMUM; \
        yyptr += yynewbytes / YYSIZEOF (*yyptr);                        \
      }                                                                 \
    while (0)

#endif

#if defined YYCOPY_NEEDED && YYCOPY_NEEDED
/* Copy COUNT objects from SRC to DST.  The source and destination do
   not overlap.  */
# ifndef YYCOPY
#  if defined __GNUC__ && 1 < __GNUC__
#   define YYCOPY(Dst, Src, Count) \
      __builtin_memcpy (Dst, Src, YY_CAST (YYSIZE_T, (Count)) * sizeof (*(Src)))
#  else
#   define YYCOPY(Dst, Src, Count)              \
      do                                        \
        {                                       \
          YYPTRDIFF_T yyi;                      \
          for (yyi = 0; yyi < (Count); yyi++)   \
            (Dst)[yyi] = (Src)[yyi];            \
        }                                       \
      while (0)
#  endif
# endif
#endif /* !YYCOPY_NEEDED */

/* YYFINAL -- State number of the termination state.  */
#define YYFINAL  3
/* YYLAST -- Last index in YYTABLE.  */
#define YYLAST   226

/* YYNTOKENS -- Number of terminals.  */
#define YYNTOKENS  53
/* YYNNTS -- Number of nonterminals.  */
#define YYNNTS  15
/* YYNRULES -- Number of rules.  */
#define YYNRULES  65
/* YYNSTATES -- Number of states.  */
#define YYNSTATES  151

#define YYUNDEFTOK  2
#define YYMAXUTOK   307


/* YYTRANSLATE(TOKEN-NUM) -- Symbol number corresponding to TOKEN-NUM
   as returned by yylex, with out-of-bounds checking.  */
#define YYTRANSLATE(YYX)                                                \
  (0 <= (YYX) && (YYX) <= YYMAXUTOK ? yytranslate[YYX] : YYUNDEFTOK)

/* YYTRANSLATE[TOKEN-NUM] -- Symbol number corresponding to TOKEN-NUM
   as returned by yylex.  */
static const yytype_int8 yytranslate[] =
{
       0,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     1,     2,     3,     4,
       5,     6,     7,     8,     9,    10,    11,    12,    13,    14,
      15,    16,    17,    18,    19,    20,    21,    22,    23,    24,
      25,    26,    27,    28,    29,    30,    31,    32,    33,    34,
      35,    36,    37,    38,    39,    40,    41,    42,    43,    44,
      45,    46,    47,    48,    49,    50,    51,    52
};

#if YYDEBUG
  /* YYRLINE[YYN] -- Source line where rule number YYN was defined.  */
static const yytype_int16 yyrline[] =
{
       0,   352,   352,   356,   357,   361,   362,   363,   364,   365,
     366,   370,   378,   379,   387,   396,   403,   408,   409,   413,
     421,   426,   434,   435,   439,   453,   464,   468,   472,   479,
     487,   493,   499,   505,   511,   517,   527,   535,   541,   552,
     558,   562,   566,   573,   580,   584,   588,   594,   598,   605,
     612,   693,   766,   771,   782,   788,   795,   805,   816,   823,
     831,   839,   852,   854,   856,   862
};
#endif

#if YYDEBUG || YYERROR_VERBOSE || 0
/* YYTNAME[SYMBOL-NUM] -- String name of the symbol SYMBOL-NUM.
   First, the terminals, then, starting at YYNTOKENS, nonterminals.  */
static const char *const yytname[] =
{
  "$end", "error", "$undefined", "GLOBAL", "EXTERN", "SECTION", "WORD",
  "SKIP", "ASCII", "END", "HALT", "INT", "IRET", "CALL", "RET", "JMP",
  "BEQ", "BNE", "BGT", "PUSH", "POP", "XCHG", "ADD", "SUB", "MUL", "DIV",
  "NOT", "AND", "OR", "XOR", "SHL", "SHR", "LD", "ST", "CSRRD", "CSRWR",
  "DOLLAR", "LBRACKET", "RBRACKET", "COLON", "COMMA", "PLUS", "MINUS",
  "STAR", "SLASH", "LPAREN", "RPAREN", "EOL", "NUMBER", "REGISTER", "CSR",
  "IDENT", "STRING", "$accept", "source", "line_list", "line", "label_def",
  "end_line", "directive", "symbol_list", "word_list", "word_init",
  "instruction", "jump_operand", "data_operand", "literal", "symbol", YY_NULLPTR
};
#endif

# ifdef YYPRINT
/* YYTOKNUM[NUM] -- (External) token number corresponding to the
   (internal) symbol number NUM (which must be that of a token).  */
static const yytype_int16 yytoknum[] =
{
       0,   256,   257,   258,   259,   260,   261,   262,   263,   264,
     265,   266,   267,   268,   269,   270,   271,   272,   273,   274,
     275,   276,   277,   278,   279,   280,   281,   282,   283,   284,
     285,   286,   287,   288,   289,   290,   291,   292,   293,   294,
     295,   296,   297,   298,   299,   300,   301,   302,   303,   304,
     305,   306,   307
};
# endif

#define YYPACT_NINF (-45)

#define yypact_value_is_default(Yyn) \
  ((Yyn) == YYPACT_NINF)

#define YYTABLE_NINF (-1)

#define yytable_value_is_error(Yyn) \
  0

  /* YYPACT[STATE-NUM] -- Index in YYTABLE of the portion describing
     STATE-NUM.  */
static const yytype_int16 yypact[] =
{
     -45,     6,   130,   -45,   -44,   -44,   -41,   -29,   -17,   -35,
     -24,   -45,   -45,   -45,   -21,   -45,   -21,   -20,   -16,   -15,
     -14,   -13,   -12,   -10,    -9,    -8,    -6,    -5,    -4,    -3,
      -2,     0,     1,   -33,     2,   -22,     3,   -45,    -1,   -45,
     179,   -45,     7,     8,   -45,    13,    13,   -45,     9,    10,
     -45,   -45,    16,   -45,   -45,   -45,   -45,   -45,   -45,   -45,
     -45,   -45,   -45,    19,    20,    21,   -45,   -45,    22,    23,
      24,    25,    26,   -45,    27,    28,    29,    30,    31,   -21,
      32,   -45,    33,   -45,   -45,    34,    35,    36,   -45,   -45,
      38,    39,   -45,   -45,   -19,   -45,   -45,   -29,    40,    42,
      43,    44,    45,    46,    47,    48,    49,    50,    51,    52,
      53,   -45,   -45,   -27,    54,   -33,    55,    37,   -45,   -45,
     -45,   -45,    66,    67,    69,   -45,   -45,   -45,   -45,   -45,
     -45,   -45,   -45,   -45,   -45,   -45,   -21,   -45,   -45,   -45,
     -45,   -21,   -21,   -21,     4,    41,   -45,   -45,   -45,   -45,
     -45
};

  /* YYDEFACT[STATE-NUM] -- Default reduction number in state STATE-NUM.
     Performed when YYTABLE does not specify something else to do.  Zero
     means the default is an error.  */
static const yytype_int8 yydefact[] =
{
       3,     0,     0,     1,     0,     0,     0,     0,     0,     0,
      12,    26,    27,    28,     0,    29,     0,     0,     0,     0,
       0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
       0,     0,     0,     0,     0,     0,     0,     5,     0,     4,
       0,     2,     0,     0,    20,    14,    15,    16,     0,     0,
      62,    24,    17,    22,    25,    18,    19,    13,    65,    30,
      52,    53,    31,     0,     0,     0,    35,    36,     0,     0,
       0,     0,     0,    42,     0,     0,     0,     0,     0,     0,
       0,    58,     0,    56,    57,     0,     0,     0,    11,     6,
       0,     0,     7,     8,     0,    63,    64,     0,     0,     0,
       0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
       0,    54,    55,     0,     0,     0,     0,     0,     9,    10,
      21,    23,     0,     0,     0,    37,    38,    39,    40,    41,
      43,    44,    45,    46,    47,    59,     0,    50,    51,    48,
      49,     0,     0,     0,     0,     0,    32,    33,    34,    60,
      61
};

  /* YYPGOTO[NTERM-NUM].  */
static const yytype_int8 yypgoto[] =
{
     -45,   -45,   -45,   -45,   -45,   -45,    70,    72,   -45,    14,
      73,   -11,   -37,    -7,   -31
};

  /* YYDEFGOTO[NTERM-NUM].  */
static const yytype_int8 yydefgoto[] =
{
      -1,     1,     2,    39,    40,    41,    42,    45,    52,    53,
      43,    59,    82,    60,    61
};

  /* YYTABLE[YYPACT[STATE-NUM]] -- What to do in state STATE-NUM.  If
     positive, shift that token.  If negative, reduce the rule whose
     number is the opposite.  If YYTABLE_NINF, syntax error.  */
static const yytype_uint8 yytable[] =
{
      54,    55,    84,    79,    80,    62,     3,    44,    48,    49,
      47,   135,    48,    49,   136,    50,    81,    56,    58,    50,
      48,    49,    51,    57,    48,    49,    83,    50,    86,    63,
      58,    50,   120,    64,    65,    66,    67,    68,    88,    69,
      70,    71,   149,    72,    73,    74,    75,    76,   112,    77,
      78,    85,    87,    94,    92,    93,    97,    95,    96,    98,
      99,   100,   101,   102,   103,   104,   105,   106,   107,   108,
     109,   110,   111,   114,   115,   116,   117,    46,   138,   150,
       0,   113,     0,     0,    84,   118,   119,   140,     0,   122,
      54,   123,   124,   125,   126,   127,   128,   129,   130,   131,
     132,   133,   134,   137,   139,   145,   141,   142,    83,   143,
      90,   121,     0,    91,     0,     0,     0,     0,     0,     0,
       0,     0,     0,     0,     0,     0,     0,     0,     0,   144,
     146,   147,   148,     4,     5,     6,     7,     8,     9,    10,
      11,    12,    13,    14,    15,    16,    17,    18,    19,    20,
      21,    22,    23,    24,    25,    26,    27,    28,    29,    30,
      31,    32,    33,    34,    35,    36,     0,     0,     0,     0,
       0,     0,     0,     0,     0,     0,     0,    37,     0,     0,
       0,    38,     4,     5,     6,     7,     8,     9,     0,    11,
      12,    13,    14,    15,    16,    17,    18,    19,    20,    21,
      22,    23,    24,    25,    26,    27,    28,    29,    30,    31,
      32,    33,    34,    35,    36,     0,     0,     0,     0,     0,
       0,     0,     0,     0,     0,     0,    89
};

static const yytype_int16 yycheck[] =
{
       7,     8,    33,    36,    37,    16,     0,    51,    41,    42,
      51,    38,    41,    42,    41,    48,    49,    52,    51,    48,
      41,    42,    51,    47,    41,    42,    33,    48,    50,    49,
      51,    48,    51,    49,    49,    49,    49,    49,    39,    49,
      49,    49,    38,    49,    49,    49,    49,    49,    79,    49,
      49,    49,    49,    40,    47,    47,    40,    48,    48,    40,
      40,    40,    40,    40,    40,    40,    40,    40,    40,    40,
      40,    40,    79,    40,    40,    40,    40,     5,   115,    38,
      -1,    49,    -1,    -1,   115,    47,    47,    50,    -1,    49,
      97,    49,    49,    49,    49,    49,    49,    49,    49,    49,
      49,    49,    49,    49,    49,   136,    40,    40,   115,    40,
      40,    97,    -1,    40,    -1,    -1,    -1,    -1,    -1,    -1,
      -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,   136,
     141,   142,   143,     3,     4,     5,     6,     7,     8,     9,
      10,    11,    12,    13,    14,    15,    16,    17,    18,    19,
      20,    21,    22,    23,    24,    25,    26,    27,    28,    29,
      30,    31,    32,    33,    34,    35,    -1,    -1,    -1,    -1,
      -1,    -1,    -1,    -1,    -1,    -1,    -1,    47,    -1,    -1,
      -1,    51,     3,     4,     5,     6,     7,     8,    -1,    10,
      11,    12,    13,    14,    15,    16,    17,    18,    19,    20,
      21,    22,    23,    24,    25,    26,    27,    28,    29,    30,
      31,    32,    33,    34,    35,    -1,    -1,    -1,    -1,    -1,
      -1,    -1,    -1,    -1,    -1,    -1,    47
};

  /* YYSTOS[STATE-NUM] -- The (internal number of the) accessing
     symbol of state STATE-NUM.  */
static const yytype_int8 yystos[] =
{
       0,    54,    55,     0,     3,     4,     5,     6,     7,     8,
       9,    10,    11,    12,    13,    14,    15,    16,    17,    18,
      19,    20,    21,    22,    23,    24,    25,    26,    27,    28,
      29,    30,    31,    32,    33,    34,    35,    47,    51,    56,
      57,    58,    59,    63,    51,    60,    60,    51,    41,    42,
      48,    51,    61,    62,    66,    66,    52,    47,    51,    64,
      66,    67,    64,    49,    49,    49,    49,    49,    49,    49,
      49,    49,    49,    49,    49,    49,    49,    49,    49,    36,
      37,    49,    65,    66,    67,    49,    50,    49,    39,    47,
      59,    63,    47,    47,    40,    48,    48,    40,    40,    40,
      40,    40,    40,    40,    40,    40,    40,    40,    40,    40,
      40,    66,    67,    49,    40,    40,    40,    40,    47,    47,
      51,    62,    49,    49,    49,    49,    49,    49,    49,    49,
      49,    49,    49,    49,    49,    38,    41,    49,    65,    49,
      50,    40,    40,    40,    66,    67,    64,    64,    64,    38,
      38
};

  /* YYR1[YYN] -- Symbol number of symbol that rule YYN derives.  */
static const yytype_int8 yyr1[] =
{
       0,    53,    54,    55,    55,    56,    56,    56,    56,    56,
      56,    57,    58,    58,    59,    59,    59,    59,    59,    59,
      60,    60,    61,    61,    62,    62,    63,    63,    63,    63,
      63,    63,    63,    63,    63,    63,    63,    63,    63,    63,
      63,    63,    63,    63,    63,    63,    63,    63,    63,    63,
      63,    63,    64,    64,    65,    65,    65,    65,    65,    65,
      65,    65,    66,    66,    66,    67
};

  /* YYR2[YYN] -- Number of symbols on the right hand side of rule YYN.  */
static const yytype_int8 yyr2[] =
{
       0,     2,     2,     0,     2,     1,     2,     2,     2,     3,
       3,     2,     1,     2,     2,     2,     2,     2,     2,     2,
       1,     3,     1,     3,     1,     1,     1,     1,     1,     1,
       2,     2,     6,     6,     6,     2,     2,     4,     4,     4,
       4,     4,     2,     4,     4,     4,     4,     4,     4,     4,
       4,     4,     1,     1,     2,     2,     1,     1,     1,     3,
       5,     5,     1,     2,     2,     1
};


#define yyerrok         (yyerrstatus = 0)
#define yyclearin       (yychar = YYEMPTY)
#define YYEMPTY         (-2)
#define YYEOF           0

#define YYACCEPT        goto yyacceptlab
#define YYABORT         goto yyabortlab
#define YYERROR         goto yyerrorlab


#define YYRECOVERING()  (!!yyerrstatus)

#define YYBACKUP(Token, Value)                                    \
  do                                                              \
    if (yychar == YYEMPTY)                                        \
      {                                                           \
        yychar = (Token);                                         \
        yylval = (Value);                                         \
        YYPOPSTACK (yylen);                                       \
        yystate = *yyssp;                                         \
        goto yybackup;                                            \
      }                                                           \
    else                                                          \
      {                                                           \
        yyerror (YY_("syntax error: cannot back up")); \
        YYERROR;                                                  \
      }                                                           \
  while (0)

/* Error token number */
#define YYTERROR        1
#define YYERRCODE       256



/* Enable debugging if requested.  */
#if YYDEBUG

# ifndef YYFPRINTF
#  include <stdio.h> /* INFRINGES ON USER NAME SPACE */
#  define YYFPRINTF fprintf
# endif

# define YYDPRINTF(Args)                        \
do {                                            \
  if (yydebug)                                  \
    YYFPRINTF Args;                             \
} while (0)

/* This macro is provided for backward compatibility. */
#ifndef YY_LOCATION_PRINT
# define YY_LOCATION_PRINT(File, Loc) ((void) 0)
#endif


# define YY_SYMBOL_PRINT(Title, Type, Value, Location)                    \
do {                                                                      \
  if (yydebug)                                                            \
    {                                                                     \
      YYFPRINTF (stderr, "%s ", Title);                                   \
      yy_symbol_print (stderr,                                            \
                  Type, Value); \
      YYFPRINTF (stderr, "\n");                                           \
    }                                                                     \
} while (0)


/*-----------------------------------.
| Print this symbol's value on YYO.  |
`-----------------------------------*/

static void
yy_symbol_value_print (FILE *yyo, int yytype, YYSTYPE const * const yyvaluep)
{
  FILE *yyoutput = yyo;
  YYUSE (yyoutput);
  if (!yyvaluep)
    return;
# ifdef YYPRINT
  if (yytype < YYNTOKENS)
    YYPRINT (yyo, yytoknum[yytype], *yyvaluep);
# endif
  YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN
  YYUSE (yytype);
  YY_IGNORE_MAYBE_UNINITIALIZED_END
}


/*---------------------------.
| Print this symbol on YYO.  |
`---------------------------*/

static void
yy_symbol_print (FILE *yyo, int yytype, YYSTYPE const * const yyvaluep)
{
  YYFPRINTF (yyo, "%s %s (",
             yytype < YYNTOKENS ? "token" : "nterm", yytname[yytype]);

  yy_symbol_value_print (yyo, yytype, yyvaluep);
  YYFPRINTF (yyo, ")");
}

/*------------------------------------------------------------------.
| yy_stack_print -- Print the state stack from its BOTTOM up to its |
| TOP (included).                                                   |
`------------------------------------------------------------------*/

static void
yy_stack_print (yy_state_t *yybottom, yy_state_t *yytop)
{
  YYFPRINTF (stderr, "Stack now");
  for (; yybottom <= yytop; yybottom++)
    {
      int yybot = *yybottom;
      YYFPRINTF (stderr, " %d", yybot);
    }
  YYFPRINTF (stderr, "\n");
}

# define YY_STACK_PRINT(Bottom, Top)                            \
do {                                                            \
  if (yydebug)                                                  \
    yy_stack_print ((Bottom), (Top));                           \
} while (0)


/*------------------------------------------------.
| Report that the YYRULE is going to be reduced.  |
`------------------------------------------------*/

static void
yy_reduce_print (yy_state_t *yyssp, YYSTYPE *yyvsp, int yyrule)
{
  int yylno = yyrline[yyrule];
  int yynrhs = yyr2[yyrule];
  int yyi;
  YYFPRINTF (stderr, "Reducing stack by rule %d (line %d):\n",
             yyrule - 1, yylno);
  /* The symbols being reduced.  */
  for (yyi = 0; yyi < yynrhs; yyi++)
    {
      YYFPRINTF (stderr, "   $%d = ", yyi + 1);
      yy_symbol_print (stderr,
                       yystos[+yyssp[yyi + 1 - yynrhs]],
                       &yyvsp[(yyi + 1) - (yynrhs)]
                                              );
      YYFPRINTF (stderr, "\n");
    }
}

# define YY_REDUCE_PRINT(Rule)          \
do {                                    \
  if (yydebug)                          \
    yy_reduce_print (yyssp, yyvsp, Rule); \
} while (0)

/* Nonzero means print parse trace.  It is left uninitialized so that
   multiple parsers can coexist.  */
int yydebug;
#else /* !YYDEBUG */
# define YYDPRINTF(Args)
# define YY_SYMBOL_PRINT(Title, Type, Value, Location)
# define YY_STACK_PRINT(Bottom, Top)
# define YY_REDUCE_PRINT(Rule)
#endif /* !YYDEBUG */


/* YYINITDEPTH -- initial size of the parser's stacks.  */
#ifndef YYINITDEPTH
# define YYINITDEPTH 200
#endif

/* YYMAXDEPTH -- maximum size the stacks can grow to (effective only
   if the built-in stack extension method is used).

   Do not make this value too large; the results are undefined if
   YYSTACK_ALLOC_MAXIMUM < YYSTACK_BYTES (YYMAXDEPTH)
   evaluated with infinite-precision integer arithmetic.  */

#ifndef YYMAXDEPTH
# define YYMAXDEPTH 10000
#endif


#if YYERROR_VERBOSE

# ifndef yystrlen
#  if defined __GLIBC__ && defined _STRING_H
#   define yystrlen(S) (YY_CAST (YYPTRDIFF_T, strlen (S)))
#  else
/* Return the length of YYSTR.  */
static YYPTRDIFF_T
yystrlen (const char *yystr)
{
  YYPTRDIFF_T yylen;
  for (yylen = 0; yystr[yylen]; yylen++)
    continue;
  return yylen;
}
#  endif
# endif

# ifndef yystpcpy
#  if defined __GLIBC__ && defined _STRING_H && defined _GNU_SOURCE
#   define yystpcpy stpcpy
#  else
/* Copy YYSRC to YYDEST, returning the address of the terminating '\0' in
   YYDEST.  */
static char *
yystpcpy (char *yydest, const char *yysrc)
{
  char *yyd = yydest;
  const char *yys = yysrc;

  while ((*yyd++ = *yys++) != '\0')
    continue;

  return yyd - 1;
}
#  endif
# endif

# ifndef yytnamerr
/* Copy to YYRES the contents of YYSTR after stripping away unnecessary
   quotes and backslashes, so that it's suitable for yyerror.  The
   heuristic is that double-quoting is unnecessary unless the string
   contains an apostrophe, a comma, or backslash (other than
   backslash-backslash).  YYSTR is taken from yytname.  If YYRES is
   null, do not copy; instead, return the length of what the result
   would have been.  */
static YYPTRDIFF_T
yytnamerr (char *yyres, const char *yystr)
{
  if (*yystr == '"')
    {
      YYPTRDIFF_T yyn = 0;
      char const *yyp = yystr;

      for (;;)
        switch (*++yyp)
          {
          case '\'':
          case ',':
            goto do_not_strip_quotes;

          case '\\':
            if (*++yyp != '\\')
              goto do_not_strip_quotes;
            else
              goto append;

          append:
          default:
            if (yyres)
              yyres[yyn] = *yyp;
            yyn++;
            break;

          case '"':
            if (yyres)
              yyres[yyn] = '\0';
            return yyn;
          }
    do_not_strip_quotes: ;
    }

  if (yyres)
    return yystpcpy (yyres, yystr) - yyres;
  else
    return yystrlen (yystr);
}
# endif

/* Copy into *YYMSG, which is of size *YYMSG_ALLOC, an error message
   about the unexpected token YYTOKEN for the state stack whose top is
   YYSSP.

   Return 0 if *YYMSG was successfully written.  Return 1 if *YYMSG is
   not large enough to hold the message.  In that case, also set
   *YYMSG_ALLOC to the required number of bytes.  Return 2 if the
   required number of bytes is too large to store.  */
static int
yysyntax_error (YYPTRDIFF_T *yymsg_alloc, char **yymsg,
                yy_state_t *yyssp, int yytoken)
{
  enum { YYERROR_VERBOSE_ARGS_MAXIMUM = 5 };
  /* Internationalized format string. */
  const char *yyformat = YY_NULLPTR;
  /* Arguments of yyformat: reported tokens (one for the "unexpected",
     one per "expected"). */
  char const *yyarg[YYERROR_VERBOSE_ARGS_MAXIMUM];
  /* Actual size of YYARG. */
  int yycount = 0;
  /* Cumulated lengths of YYARG.  */
  YYPTRDIFF_T yysize = 0;

  /* There are many possibilities here to consider:
     - If this state is a consistent state with a default action, then
       the only way this function was invoked is if the default action
       is an error action.  In that case, don't check for expected
       tokens because there are none.
     - The only way there can be no lookahead present (in yychar) is if
       this state is a consistent state with a default action.  Thus,
       detecting the absence of a lookahead is sufficient to determine
       that there is no unexpected or expected token to report.  In that
       case, just report a simple "syntax error".
     - Don't assume there isn't a lookahead just because this state is a
       consistent state with a default action.  There might have been a
       previous inconsistent state, consistent state with a non-default
       action, or user semantic action that manipulated yychar.
     - Of course, the expected token list depends on states to have
       correct lookahead information, and it depends on the parser not
       to perform extra reductions after fetching a lookahead from the
       scanner and before detecting a syntax error.  Thus, state merging
       (from LALR or IELR) and default reductions corrupt the expected
       token list.  However, the list is correct for canonical LR with
       one exception: it will still contain any token that will not be
       accepted due to an error action in a later state.
  */
  if (yytoken != YYEMPTY)
    {
      int yyn = yypact[+*yyssp];
      YYPTRDIFF_T yysize0 = yytnamerr (YY_NULLPTR, yytname[yytoken]);
      yysize = yysize0;
      yyarg[yycount++] = yytname[yytoken];
      if (!yypact_value_is_default (yyn))
        {
          /* Start YYX at -YYN if negative to avoid negative indexes in
             YYCHECK.  In other words, skip the first -YYN actions for
             this state because they are default actions.  */
          int yyxbegin = yyn < 0 ? -yyn : 0;
          /* Stay within bounds of both yycheck and yytname.  */
          int yychecklim = YYLAST - yyn + 1;
          int yyxend = yychecklim < YYNTOKENS ? yychecklim : YYNTOKENS;
          int yyx;

          for (yyx = yyxbegin; yyx < yyxend; ++yyx)
            if (yycheck[yyx + yyn] == yyx && yyx != YYTERROR
                && !yytable_value_is_error (yytable[yyx + yyn]))
              {
                if (yycount == YYERROR_VERBOSE_ARGS_MAXIMUM)
                  {
                    yycount = 1;
                    yysize = yysize0;
                    break;
                  }
                yyarg[yycount++] = yytname[yyx];
                {
                  YYPTRDIFF_T yysize1
                    = yysize + yytnamerr (YY_NULLPTR, yytname[yyx]);
                  if (yysize <= yysize1 && yysize1 <= YYSTACK_ALLOC_MAXIMUM)
                    yysize = yysize1;
                  else
                    return 2;
                }
              }
        }
    }

  switch (yycount)
    {
# define YYCASE_(N, S)                      \
      case N:                               \
        yyformat = S;                       \
      break
    default: /* Avoid compiler warnings. */
      YYCASE_(0, YY_("syntax error"));
      YYCASE_(1, YY_("syntax error, unexpected %s"));
      YYCASE_(2, YY_("syntax error, unexpected %s, expecting %s"));
      YYCASE_(3, YY_("syntax error, unexpected %s, expecting %s or %s"));
      YYCASE_(4, YY_("syntax error, unexpected %s, expecting %s or %s or %s"));
      YYCASE_(5, YY_("syntax error, unexpected %s, expecting %s or %s or %s or %s"));
# undef YYCASE_
    }

  {
    /* Don't count the "%s"s in the final size, but reserve room for
       the terminator.  */
    YYPTRDIFF_T yysize1 = yysize + (yystrlen (yyformat) - 2 * yycount) + 1;
    if (yysize <= yysize1 && yysize1 <= YYSTACK_ALLOC_MAXIMUM)
      yysize = yysize1;
    else
      return 2;
  }

  if (*yymsg_alloc < yysize)
    {
      *yymsg_alloc = 2 * yysize;
      if (! (yysize <= *yymsg_alloc
             && *yymsg_alloc <= YYSTACK_ALLOC_MAXIMUM))
        *yymsg_alloc = YYSTACK_ALLOC_MAXIMUM;
      return 1;
    }

  /* Avoid sprintf, as that infringes on the user's name space.
     Don't have undefined behavior even if the translation
     produced a string with the wrong number of "%s"s.  */
  {
    char *yyp = *yymsg;
    int yyi = 0;
    while ((*yyp = *yyformat) != '\0')
      if (*yyp == '%' && yyformat[1] == 's' && yyi < yycount)
        {
          yyp += yytnamerr (yyp, yyarg[yyi++]);
          yyformat += 2;
        }
      else
        {
          ++yyp;
          ++yyformat;
        }
  }
  return 0;
}
#endif /* YYERROR_VERBOSE */

/*-----------------------------------------------.
| Release the memory associated to this symbol.  |
`-----------------------------------------------*/

static void
yydestruct (const char *yymsg, int yytype, YYSTYPE *yyvaluep)
{
  YYUSE (yyvaluep);
  if (!yymsg)
    yymsg = "Deleting";
  YY_SYMBOL_PRINT (yymsg, yytype, yyvaluep, yylocationp);

  YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN
  YYUSE (yytype);
  YY_IGNORE_MAYBE_UNINITIALIZED_END
}




/* The lookahead symbol.  */
int yychar;

/* The semantic value of the lookahead symbol.  */
YYSTYPE yylval;
/* Number of syntax errors so far.  */
int yynerrs;


/*----------.
| yyparse.  |
`----------*/

int
yyparse (void)
{
    yy_state_fast_t yystate;
    /* Number of tokens to shift before error messages enabled.  */
    int yyerrstatus;

    /* The stacks and their tools:
       'yyss': related to states.
       'yyvs': related to semantic values.

       Refer to the stacks through separate pointers, to allow yyoverflow
       to reallocate them elsewhere.  */

    /* The state stack.  */
    yy_state_t yyssa[YYINITDEPTH];
    yy_state_t *yyss;
    yy_state_t *yyssp;

    /* The semantic value stack.  */
    YYSTYPE yyvsa[YYINITDEPTH];
    YYSTYPE *yyvs;
    YYSTYPE *yyvsp;

    YYPTRDIFF_T yystacksize;

  int yyn;
  int yyresult;
  /* Lookahead token as an internal (translated) token number.  */
  int yytoken = 0;
  /* The variables used to return semantic value and location from the
     action routines.  */
  YYSTYPE yyval;

#if YYERROR_VERBOSE
  /* Buffer for error messages, and its allocated size.  */
  char yymsgbuf[128];
  char *yymsg = yymsgbuf;
  YYPTRDIFF_T yymsg_alloc = sizeof yymsgbuf;
#endif

#define YYPOPSTACK(N)   (yyvsp -= (N), yyssp -= (N))

  /* The number of symbols on the RHS of the reduced rule.
     Keep to zero when no symbol should be popped.  */
  int yylen = 0;

  yyssp = yyss = yyssa;
  yyvsp = yyvs = yyvsa;
  yystacksize = YYINITDEPTH;

  YYDPRINTF ((stderr, "Starting parse\n"));

  yystate = 0;
  yyerrstatus = 0;
  yynerrs = 0;
  yychar = YYEMPTY; /* Cause a token to be read.  */
  goto yysetstate;


/*------------------------------------------------------------.
| yynewstate -- push a new state, which is found in yystate.  |
`------------------------------------------------------------*/
yynewstate:
  /* In all cases, when you get here, the value and location stacks
     have just been pushed.  So pushing a state here evens the stacks.  */
  yyssp++;


/*--------------------------------------------------------------------.
| yysetstate -- set current state (the top of the stack) to yystate.  |
`--------------------------------------------------------------------*/
yysetstate:
  YYDPRINTF ((stderr, "Entering state %d\n", yystate));
  YY_ASSERT (0 <= yystate && yystate < YYNSTATES);
  YY_IGNORE_USELESS_CAST_BEGIN
  *yyssp = YY_CAST (yy_state_t, yystate);
  YY_IGNORE_USELESS_CAST_END

  if (yyss + yystacksize - 1 <= yyssp)
#if !defined yyoverflow && !defined YYSTACK_RELOCATE
    goto yyexhaustedlab;
#else
    {
      /* Get the current used size of the three stacks, in elements.  */
      YYPTRDIFF_T yysize = yyssp - yyss + 1;

# if defined yyoverflow
      {
        /* Give user a chance to reallocate the stack.  Use copies of
           these so that the &'s don't force the real ones into
           memory.  */
        yy_state_t *yyss1 = yyss;
        YYSTYPE *yyvs1 = yyvs;

        /* Each stack pointer address is followed by the size of the
           data in use in that stack, in bytes.  This used to be a
           conditional around just the two extra args, but that might
           be undefined if yyoverflow is a macro.  */
        yyoverflow (YY_("memory exhausted"),
                    &yyss1, yysize * YYSIZEOF (*yyssp),
                    &yyvs1, yysize * YYSIZEOF (*yyvsp),
                    &yystacksize);
        yyss = yyss1;
        yyvs = yyvs1;
      }
# else /* defined YYSTACK_RELOCATE */
      /* Extend the stack our own way.  */
      if (YYMAXDEPTH <= yystacksize)
        goto yyexhaustedlab;
      yystacksize *= 2;
      if (YYMAXDEPTH < yystacksize)
        yystacksize = YYMAXDEPTH;

      {
        yy_state_t *yyss1 = yyss;
        union yyalloc *yyptr =
          YY_CAST (union yyalloc *,
                   YYSTACK_ALLOC (YY_CAST (YYSIZE_T, YYSTACK_BYTES (yystacksize))));
        if (! yyptr)
          goto yyexhaustedlab;
        YYSTACK_RELOCATE (yyss_alloc, yyss);
        YYSTACK_RELOCATE (yyvs_alloc, yyvs);
# undef YYSTACK_RELOCATE
        if (yyss1 != yyssa)
          YYSTACK_FREE (yyss1);
      }
# endif

      yyssp = yyss + yysize - 1;
      yyvsp = yyvs + yysize - 1;

      YY_IGNORE_USELESS_CAST_BEGIN
      YYDPRINTF ((stderr, "Stack size increased to %ld\n",
                  YY_CAST (long, yystacksize)));
      YY_IGNORE_USELESS_CAST_END

      if (yyss + yystacksize - 1 <= yyssp)
        YYABORT;
    }
#endif /* !defined yyoverflow && !defined YYSTACK_RELOCATE */

  if (yystate == YYFINAL)
    YYACCEPT;

  goto yybackup;


/*-----------.
| yybackup.  |
`-----------*/
yybackup:
  /* Do appropriate processing given the current state.  Read a
     lookahead token if we need one and don't already have one.  */

  /* First try to decide what to do without reference to lookahead token.  */
  yyn = yypact[yystate];
  if (yypact_value_is_default (yyn))
    goto yydefault;

  /* Not known => get a lookahead token if don't already have one.  */

  /* YYCHAR is either YYEMPTY or YYEOF or a valid lookahead symbol.  */
  if (yychar == YYEMPTY)
    {
      YYDPRINTF ((stderr, "Reading a token: "));
      yychar = yylex ();
    }

  if (yychar <= YYEOF)
    {
      yychar = yytoken = YYEOF;
      YYDPRINTF ((stderr, "Now at end of input.\n"));
    }
  else
    {
      yytoken = YYTRANSLATE (yychar);
      YY_SYMBOL_PRINT ("Next token is", yytoken, &yylval, &yylloc);
    }

  /* If the proper action on seeing token YYTOKEN is to reduce or to
     detect an error, take that action.  */
  yyn += yytoken;
  if (yyn < 0 || YYLAST < yyn || yycheck[yyn] != yytoken)
    goto yydefault;
  yyn = yytable[yyn];
  if (yyn <= 0)
    {
      if (yytable_value_is_error (yyn))
        goto yyerrlab;
      yyn = -yyn;
      goto yyreduce;
    }

  /* Count tokens shifted since error; after three, turn off error
     status.  */
  if (yyerrstatus)
    yyerrstatus--;

  /* Shift the lookahead token.  */
  YY_SYMBOL_PRINT ("Shifting", yytoken, &yylval, &yylloc);
  yystate = yyn;
  YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN
  *++yyvsp = yylval;
  YY_IGNORE_MAYBE_UNINITIALIZED_END

  /* Discard the shifted token.  */
  yychar = YYEMPTY;
  goto yynewstate;


/*-----------------------------------------------------------.
| yydefault -- do the default action for the current state.  |
`-----------------------------------------------------------*/
yydefault:
  yyn = yydefact[yystate];
  if (yyn == 0)
    goto yyerrlab;
  goto yyreduce;


/*-----------------------------.
| yyreduce -- do a reduction.  |
`-----------------------------*/
yyreduce:
  /* yyn is the number of a rule to reduce with.  */
  yylen = yyr2[yyn];

  /* If YYLEN is nonzero, implement the default value of the action:
     '$$ = $1'.

     Otherwise, the following line sets YYVAL to garbage.
     This behavior is undocumented and Bison
     users should not rely upon it.  Assigning to YYVAL
     unconditionally makes the parser a bit smaller, and it avoids a
     GCC warning that YYVAL may be used uninitialized.  */
  yyval = yyvsp[1-yylen];


  YY_REDUCE_PRINT (yyn);
  switch (yyn)
    {
  case 11:
#line 371 "misc/parser.y"
      {
          assm.defineLabel((yyvsp[-1].str));
          free((yyvsp[-1].str));
      }
#line 1738 "misc/parser.tab.c"
    break;

  case 14:
#line 388 "misc/parser.y"
      {
          // Svi simboli iz liste su vec dodati u sym_list
          // (vidi symbol_list produkciju ispod)
          for (auto& s : sym_list) {
              assm.markGlobal(s);
          }
          sym_list.clear();
      }
#line 1751 "misc/parser.tab.c"
    break;

  case 15:
#line 397 "misc/parser.y"
      {
          for (auto& s : sym_list) {
              assm.markExtern(s);
          }
          sym_list.clear();
      }
#line 1762 "misc/parser.tab.c"
    break;

  case 16:
#line 404 "misc/parser.y"
      {
          assm.switchSection((yyvsp[0].str));
          free((yyvsp[0].str));
      }
#line 1771 "misc/parser.tab.c"
    break;

  case 18:
#line 410 "misc/parser.y"
      {
          assm.skipBytes((yyvsp[0].num));
      }
#line 1779 "misc/parser.tab.c"
    break;

  case 19:
#line 414 "misc/parser.y"
      {
          emit_ascii_string((yyvsp[0].str));
          free((yyvsp[0].str));
      }
#line 1788 "misc/parser.tab.c"
    break;

  case 20:
#line 422 "misc/parser.y"
      {
          sym_list.push_back((yyvsp[0].str));
          free((yyvsp[0].str));
      }
#line 1797 "misc/parser.tab.c"
    break;

  case 21:
#line 427 "misc/parser.y"
      {
          sym_list.push_back((yyvsp[0].str));
          free((yyvsp[0].str));
      }
#line 1806 "misc/parser.tab.c"
    break;

  case 24:
#line 440 "misc/parser.y"
      {
          // Simbol - proveri da li je poznat
          int val = assm.getSymbolValue((yyvsp[0].str));
          if (val >= 0 && assm.getSymbolSection((yyvsp[0].str)) == assm.currentSection()) {
              assm.emitWord((uint32_t)val);
          } else {
              // Nije poznat ili u drugoj sekciji -> placeholder + patch
              int offset = assm.locationCounter();
              assm.emitWord(0);
              assm.addPatch((yyvsp[0].str), offset, '+', 0, RelocType::R_ABS_32);
          }
          free((yyvsp[0].str));
      }
#line 1824 "misc/parser.tab.c"
    break;

  case 25:
#line 454 "misc/parser.y"
      {
          assm.emitWord((uint32_t)(yyvsp[0].num));
      }
#line 1832 "misc/parser.tab.c"
    break;

  case 26:
#line 465 "misc/parser.y"
      {
          assm.emitInstruction(0x0, 0x0, 0, 0, 0, 0);
      }
#line 1840 "misc/parser.tab.c"
    break;

  case 27:
#line 469 "misc/parser.y"
      {
          assm.emitInstruction(0x1, 0x0, 0, 0, 0, 0);
      }
#line 1848 "misc/parser.tab.c"
    break;

  case 28:
#line 473 "misc/parser.y"
      {
          // iret = pop pc; pop status
          // Postavka ne daje poseban opkod - rezervisemo OC=0, MOD=1
          // (emulator prepoznaje (OC=0, MOD=1) kao iret)
          assm.emitInstruction(0x0, 0x1, 0, 0, 0, 0);
      }
#line 1859 "misc/parser.tab.c"
    break;

  case 29:
#line 480 "misc/parser.y"
      {
          // ret = pop pc
          // Implementirano kao ld sa post-increment (MOD=3):
          //   gpr[A] <= mem32[gpr[B]]; gpr[B] <= gpr[B] + D
          // A = pc (15), B = sp (14), D = +4
          assm.emitInstruction(0x9, 0x3, 15, 14, 0, 4);
      }
#line 1871 "misc/parser.tab.c"
    break;

  case 30:
#line 488 "misc/parser.y"
      {
          // call - koristi emit_jump_or_call za automatski odabir direktne/indirektne varijante
          emit_jump_or_call(0x2, 0x0, 0, 0, true);
          reset_operand_state();
      }
#line 1881 "misc/parser.tab.c"
    break;

  case 31:
#line 494 "misc/parser.y"
      {
          // jmp - koristi emit_jump_or_call
          emit_jump_or_call(0x3, 0x0, 0, 0, false);
          reset_operand_state();
      }
#line 1891 "misc/parser.tab.c"
    break;

  case 32:
#line 500 "misc/parser.y"
      {
          // beq - OC 0x3, MOD 0x1 (if gpr[B]==gpr[C] pc <= ...)
          emit_jump_or_call(0x3, 0x1, (yyvsp[-4].num), (yyvsp[-2].num), false);
          reset_operand_state();
      }
#line 1901 "misc/parser.tab.c"
    break;

  case 33:
#line 506 "misc/parser.y"
      {
          // bne - OC 0x3, MOD 0x2 (if gpr[B]!=gpr[C] pc <= ...)
          emit_jump_or_call(0x3, 0x2, (yyvsp[-4].num), (yyvsp[-2].num), false);
          reset_operand_state();
      }
#line 1911 "misc/parser.tab.c"
    break;

  case 34:
#line 512 "misc/parser.y"
      {
          // bgt - OC 0x3, MOD 0x3 (if gpr[B] > gpr[C] pc <= ...)
          emit_jump_or_call(0x3, 0x3, (yyvsp[-4].num), (yyvsp[-2].num), false);
          reset_operand_state();
      }
#line 1921 "misc/parser.tab.c"
    break;

  case 35:
#line 518 "misc/parser.y"
      {
          // push = sp -= 4; mem32[sp] <= gpr
          // Implementirano kroz st instrukciju:
          //   st = OC 0x8, MOD 0x1 (gpr[A] <= gpr[A] + D; mem32[gpr[A]] <= gpr[C])
          //   A = sp (14), D = -4, C = $2
          // Ali MOD 0x1 za st je "gpr[A] <= gpr[A] + D; mem32[gpr[A]] <= gpr[C]"
          // sto je post-dekrement (ako je D negativno)
          assm.emitInstruction(0x8, 0x1, 14, 0, (yyvsp[0].num), -4);
      }
#line 1935 "misc/parser.tab.c"
    break;

  case 36:
#line 528 "misc/parser.y"
      {
          // pop = gpr <= mem32[sp]; sp += 4
          // Implementirano kroz ld instrukciju:
          //   ld = OC 0x9, MOD 0x3 (gpr[A] <= mem32[gpr[B]]; gpr[B] <= gpr[B] + D)
          //   A = $2, B = sp (14), D = 4
          assm.emitInstruction(0x9, 0x3, (yyvsp[0].num), 14, 0, 4);
      }
#line 1947 "misc/parser.tab.c"
    break;

  case 37:
#line 536 "misc/parser.y"
      {
          // xchg = OC 0x4, MOD 0x0
          // A=0, B=$2, C=$4
          assm.emitInstruction(0x4, 0x0, 0, (yyvsp[-2].num), (yyvsp[0].num), 0);
      }
#line 1957 "misc/parser.tab.c"
    break;

  case 38:
#line 542 "misc/parser.y"
      {
          // add = OC 0x5, MOD 0x0: gpr[A] <= gpr[B] + gpr[C]
          // A=$2, B=$4, C=0 (postavka: add %gprS, %gprD => gprD <= gprD + gprS)
          // Zapravo: gprD = A, gprS = B, pa je: gpr[A] <= gpr[A] + gpr[B]?
          // Postavka kaze: add %gprS, %gprD => gprD <= gprD + gprS
          // => A = gprD, B = gprS, ali instr format je gpr[A] <= gpr[B] + gpr[C]
          // Znaci A = gprD, B = gprD, C = gprS
          // (kombinacija koja daje A <= B + C = gprD + gprS)
            assm.emitInstruction(0x5, 0x0, (yyvsp[0].num), (yyvsp[0].num), (yyvsp[-2].num), 0);
      }
#line 1972 "misc/parser.tab.c"
    break;

  case 39:
#line 553 "misc/parser.y"
      {
          // sub %gprS, %gprD => gprD <= gprD - gprS
          // A = gprD, B = gprD, C = gprS
         assm.emitInstruction(0x5, 0x1, (yyvsp[0].num), (yyvsp[0].num), (yyvsp[-2].num), 0);
      }
#line 1982 "misc/parser.tab.c"
    break;

  case 40:
#line 559 "misc/parser.y"
      {
          assm.emitInstruction(0x5, 0x2, (yyvsp[0].num), (yyvsp[0].num), (yyvsp[-2].num), 0);
      }
#line 1990 "misc/parser.tab.c"
    break;

  case 41:
#line 563 "misc/parser.y"
      {
          assm.emitInstruction(0x5, 0x3, (yyvsp[0].num), (yyvsp[0].num), (yyvsp[-2].num), 0);
      }
#line 1998 "misc/parser.tab.c"
    break;

  case 42:
#line 567 "misc/parser.y"
      {
          // not %gpr => gpr <= ~gpr
          // OC 0x6, MOD 0x0: gpr[A] <= ~gpr[B]
          // A = $2, B = $2
          assm.emitInstruction(0x6, 0x0, (yyvsp[0].num), (yyvsp[0].num), 0, 0);
      }
#line 2009 "misc/parser.tab.c"
    break;

  case 43:
#line 574 "misc/parser.y"
      {
          // and %gprS, %gprD => gprD <= gprD & gprS
          // OC 0x6, MOD 0x1: gpr[A] <= gpr[B] & gpr[C]
          // A = gprD, B = gprD, C = gprS
           assm.emitInstruction(0x6, 0x1, (yyvsp[0].num), (yyvsp[0].num), (yyvsp[-2].num), 0);
      }
#line 2020 "misc/parser.tab.c"
    break;

  case 44:
#line 581 "misc/parser.y"
      {
          assm.emitInstruction(0x6, 0x2, (yyvsp[0].num), (yyvsp[0].num), (yyvsp[-2].num), 0);
      }
#line 2028 "misc/parser.tab.c"
    break;

  case 45:
#line 585 "misc/parser.y"
      {
          assm.emitInstruction(0x6, 0x3, (yyvsp[0].num), (yyvsp[0].num), (yyvsp[-2].num), 0);
      }
#line 2036 "misc/parser.tab.c"
    break;

  case 46:
#line 589 "misc/parser.y"
      {
          // shl %gprS, %gprD => gprD <= gprD << gprS
          // OC 0x7, MOD 0x0: gpr[A] <= gpr[B] << gpr[C]
          assm.emitInstruction(0x7, 0x0, (yyvsp[0].num), (yyvsp[0].num), (yyvsp[-2].num), 0);
      }
#line 2046 "misc/parser.tab.c"
    break;

  case 47:
#line 595 "misc/parser.y"
      {
          assm.emitInstruction(0x7, 0x1, (yyvsp[0].num), (yyvsp[0].num), (yyvsp[-2].num), 0);
      }
#line 2054 "misc/parser.tab.c"
    break;

  case 48:
#line 599 "misc/parser.y"
      {
          // csrrd %csr, %gpr => gpr <= csr
          // ld = OC 0x9, MOD 0x0: gpr[A] <= csr[B]
          // A = $4, B = $2 (csr)
          assm.emitInstruction(0x9, 0x0, (yyvsp[0].num), (yyvsp[-2].num), 0, 0);
      }
#line 2065 "misc/parser.tab.c"
    break;

  case 49:
#line 606 "misc/parser.y"
      {
          // csrwr %gpr, %csr => csr <= gpr
          // ld = OC 0x9, MOD 0x4: csr[A] <= gpr[B]
          // A = $4 (csr), B = $2 (gpr)
          assm.emitInstruction(0x9, 0x4, (yyvsp[0].num), (yyvsp[-2].num), 0, 0);
      }
#line 2076 "misc/parser.tab.c"
    break;

  case 50:
#line 613 "misc/parser.y"
      {
          // ld <operand>, %gprD => gprD <= operand
          // Trenutno stanje operanada je u ops
          int offset;
          uint8_t mod;

          if (ops.is_immediate) {
              // $literal ili $simbol => gpr[A] <= gpr[B] + D
              // Ako vrednost staje u 12-bit: MOD=0x1, B=r0, D=vrednost
              // Inace: ld [pc+4], %gprD + jmp +4 + pool word
              // (jmp preskace pool da ne bi bio izvršen kao instrukcija)
              if (needs_pool_for_value()) {
                  // ld MOD=0x2, A=$4, B=pc(15), C=0, D=4
                  // Posle fetch, pc = cur_pc+4. Pool je na cur_pc+8, tako D=4.
                  assm.emitInstruction(0x9, 0x2, (yyvsp[0].num), 15, 0, 4);
                  // jmp +4 (preskoči pool word)
                  assm.emitInstruction(0x3, 0x0, 15, 0, 0, 4);
                  // Pool word
                  emit_pool_word();
              } else {
                  mod = 0x1;
                  offset = assm.emitInstruction(0x9, mod, (yyvsp[0].num), 0, 0, 0);
                  emit_disp_patch(offset, false);  // apsolutno (ne PC-rel)
              }
          } else if (ops.is_register) {
              // %reg - ovo je "vrednost u registru"
              // Mod 0x1 sa D=0: gpr[A] <= gpr[B] + 0 = gpr[B]
              mod = 0x1;
              offset = assm.emitInstruction(0x9, mod, (yyvsp[0].num), ops.reg, 0, 0);
          } else if (ops.is_memory) {
              if (ops.is_bare_symbol) {
                  // Sam <simbol> ili <literal> (bez $ i bez [])
                  if (needs_pool_for_value()) {
                      // Dve instrukcije + pool:
                      // 1. ld [pc+8], %r_dest  — učitaj adresu iz pool-a
                      // 2. ld [%r_dest], %r_dest — dereferenciraj
                      // 3. jmp +4 — preskoči pool
                      // 4. pool word — adresa simbola
                      assm.emitInstruction(0x9, 0x2, (yyvsp[0].num), 15, 0, 8);   // ld [pc+8], %r_dest
                      assm.emitInstruction(0x9, 0x2, (yyvsp[0].num), (yyvsp[0].num), 0, 0);    // ld [%r_dest], %r_dest
                      assm.emitInstruction(0x3, 0x0, 15, 0, 0, 4);     // jmp +4
                      emit_pool_word();
                  } else {
                      mod = 0x2;
                      offset = assm.emitInstruction(0x9, mod, (yyvsp[0].num), 0, 0, 0);
                      emit_disp_patch(offset, false);
                  }
              } else if (ops.pending_symbol.empty() && !ops.is_pc_relative) {
                  // [reg + lit] ili [reg]
                  // ld MOD 0x2: gpr[A] <= mem32[gpr[B] + gpr[C] + D]
                  if (ops.value < -2048 || ops.value > 2047) {
                      yyerror("Displacement out of 12-bit range");
                  }
                  mod = 0x2;
                  offset = assm.emitInstruction(0x9, mod, (yyvsp[0].num), ops.reg, 0, ops.value);
              } else if (ops.is_pc_relative) {
                  // simbol (memorijsko na adresi simbola) - APSOLUTNO preko r0
                  // Ako adresa staje u 12-bit: MOD=0x2, B=r0, C=r0, D=adresa
                  if (needs_pool_for_value()) {
                      yyerror("ld <simbol> sa dalekim simbolom nije podržano (treba tmp registar)");
                  } else {
                      mod = 0x2;
                      offset = assm.emitInstruction(0x9, mod, (yyvsp[0].num), 0, 0, 0);
                      emit_disp_patch(offset, false);  // apsolutno (ne PC-rel)
                  }
              } else {
                  // [reg + simbol]
                  // ld MOD 0x2: gpr[A] <= mem32[gpr[B] + gpr[C] + D]
                  // A = $4, B = ops.reg, C = 0, D = adresa(simbol)
                  if (needs_pool_for_value()) {
                      yyerror("ld [reg + simbol] sa dalekim simbolom nije podržano");
                  } else {
                      mod = 0x2;
                      offset = assm.emitInstruction(0x9, mod, (yyvsp[0].num), ops.reg, 0, 0);
                      emit_disp_patch(offset, false);
                  }
              }
          }
          reset_operand_state();
      }
#line 2161 "misc/parser.tab.c"
    break;

  case 51:
#line 694 "misc/parser.y"
      {
          // st %gprS, <operand> => operand <= gprS
          // gprS = $2
          int offset;
          uint8_t mod;

          if (ops.is_immediate) {
              // $literal / $simbol - nije validno za st (ne mozes smestiti u literal)
              yyerror("st ne moze imati $ kao odredisni operand");
          } else if (ops.is_register) {
              // %reg - nije validno (postavka to ne dozvoljava za st)
              yyerror("st ne moze imati registar kao odredisni operand");
          } else if (ops.is_memory) {
              if (ops.is_bare_symbol) {
                  // Sam <simbol> ili <literal> (bez $ i bez []) - memorijsko na toj adresi
                  // APSOLUTNO adresiranje preko r0
                  if (needs_pool_for_value()) {
                      // Indirektni st: mem32[mem32[pc+4]] <= gpr[C]
                      assm.emitInstruction(0x8, 0x2, 15, 0, (yyvsp[-2].num), 4);
                      // jmp +4 (preskoči pool)
                      assm.emitInstruction(0x3, 0x0, 15, 0, 0, 4);
                      emit_pool_word();
                  } else {
                      mod = 0x0;
                      offset = assm.emitInstruction(0x8, mod, 0, 0, (yyvsp[-2].num), 0);
                      emit_disp_patch(offset, false);
                  }
              } else if (ops.pending_symbol.empty() && !ops.is_pc_relative) {
                  // [reg + lit] ili [reg]
                  // st MOD 0x0: mem32[gpr[A]+gpr[B]+D] <= gpr[C]
                  if (ops.value < -2048 || ops.value > 2047) {
                      yyerror("Displacement out of 12-bit range");
                  }
                  mod = 0x0;
                  offset = assm.emitInstruction(0x8, mod, ops.reg, 0, (yyvsp[-2].num), ops.value);
              } else if (ops.is_pc_relative) {
                  // simbol (memorijsko) - APSOLUTNO preko r0
                  // Ako adresa staje u 12-bit: MOD=0x0, A=r0, B=r0, C=$2, D=adresa
                  // Inace: st indirektno (MOD=0x2) + jmp +4 + pool word
                  if (needs_pool_for_value()) {
                      // st MOD=0x2: mem32[mem32[gpr[A]+gpr[B]+D]] <= gpr[C]
                      // A=pc(15), B=0, C=$2, D=4 (pool je na pc+4 = cur_pc+8)
                      assm.emitInstruction(0x8, 0x2, 15, 0, (yyvsp[-2].num), 4);
                      // jmp +4 (preskoči pool)
                      assm.emitInstruction(0x3, 0x0, 15, 0, 0, 4);
                      emit_pool_word();
                  } else {
                      mod = 0x0;
                      offset = assm.emitInstruction(0x8, mod, 0, 0, (yyvsp[-2].num), 0);
                      emit_disp_patch(offset, false);
                  }
              } else {
                  // [reg + simbol]
                  if (needs_pool_for_value()) {
                      yyerror("st [reg + simbol] sa dalekim simbolom nije podržano");
                  } else {
                      mod = 0x0;
                      offset = assm.emitInstruction(0x8, mod, ops.reg, 0, (yyvsp[-2].num), 0);
                      emit_disp_patch(offset, false);
                  }
              }
          }
          reset_operand_state();
      }
#line 2230 "misc/parser.tab.c"
    break;

  case 52:
#line 767 "misc/parser.y"
      {
          reset_operand_state();
          ops.value = (yyvsp[0].num);
      }
#line 2239 "misc/parser.tab.c"
    break;

  case 53:
#line 772 "misc/parser.y"
      {
          reset_operand_state();
          ops.pending_symbol = (yyvsp[0].str);
          ops.is_pc_relative = true;  // skok je PC-rel
          free((yyvsp[0].str));
      }
#line 2250 "misc/parser.tab.c"
    break;

  case 54:
#line 783 "misc/parser.y"
      {
          reset_operand_state();
          ops.value = (yyvsp[0].num);
          ops.is_immediate = true;
      }
#line 2260 "misc/parser.tab.c"
    break;

  case 55:
#line 789 "misc/parser.y"
      {
          reset_operand_state();
          ops.pending_symbol = (yyvsp[0].str);
          ops.is_immediate = true;
          free((yyvsp[0].str));
      }
#line 2271 "misc/parser.tab.c"
    break;

  case 56:
#line 796 "misc/parser.y"
      {
          // <literal> - memorijsko na adresi literala (APSOLUTNO, ne PC-rel!)
          // Koristi r0 (uvek 0) kao bazni registar: mem32[r0 + 0 + literal]
          reset_operand_state();
          ops.value = (yyvsp[0].num);
          ops.is_memory = true;
          ops.is_bare_symbol = true;
          ops.is_pc_relative = false;  // apsolutno preko r0
      }
#line 2285 "misc/parser.tab.c"
    break;

  case 57:
#line 806 "misc/parser.y"
      {
          // <simbol> - memorijsko na adresi simbola (APSOLUTNO, ne PC-rel!)
          // Koristi r0 kao bazni registar: mem32[r0 + 0 + simbol_adresa]
          reset_operand_state();
          ops.pending_symbol = (yyvsp[0].str);
          ops.is_memory = true;
          ops.is_bare_symbol = true;
          ops.is_pc_relative = false;  // apsolutno preko r0
          free((yyvsp[0].str));
      }
#line 2300 "misc/parser.tab.c"
    break;

  case 58:
#line 817 "misc/parser.y"
      {
          // %reg
          reset_operand_state();
          ops.is_register = true;
          ops.reg = (yyvsp[0].num);
      }
#line 2311 "misc/parser.tab.c"
    break;

  case 59:
#line 824 "misc/parser.y"
      {
          // [reg]
          reset_operand_state();
          ops.is_memory = true;
          ops.reg = (yyvsp[-1].num);
          ops.value = 0;
      }
#line 2323 "misc/parser.tab.c"
    break;

  case 60:
#line 832 "misc/parser.y"
      {
          // [reg + literal]
          reset_operand_state();
          ops.is_memory = true;
          ops.reg = (yyvsp[-3].num);
          ops.value = (yyvsp[-1].num);
      }
#line 2335 "misc/parser.tab.c"
    break;

  case 61:
#line 840 "misc/parser.y"
      {
          // [reg + simbol]
          reset_operand_state();
          ops.is_memory = true;
          ops.reg = (yyvsp[-3].num);
          ops.pending_symbol = (yyvsp[-1].str);
          free((yyvsp[-1].str));
      }
#line 2348 "misc/parser.tab.c"
    break;

  case 62:
#line 853 "misc/parser.y"
      { (yyval.num) = (yyvsp[0].num); }
#line 2354 "misc/parser.tab.c"
    break;

  case 63:
#line 855 "misc/parser.y"
      { (yyval.num) = (yyvsp[0].num); }
#line 2360 "misc/parser.tab.c"
    break;

  case 64:
#line 857 "misc/parser.y"
      { (yyval.num) = -(yyvsp[0].num); }
#line 2366 "misc/parser.tab.c"
    break;

  case 65:
#line 863 "misc/parser.y"
      { (yyval.str) = (yyvsp[0].str); }
#line 2372 "misc/parser.tab.c"
    break;


#line 2376 "misc/parser.tab.c"

      default: break;
    }
  /* User semantic actions sometimes alter yychar, and that requires
     that yytoken be updated with the new translation.  We take the
     approach of translating immediately before every use of yytoken.
     One alternative is translating here after every semantic action,
     but that translation would be missed if the semantic action invokes
     YYABORT, YYACCEPT, or YYERROR immediately after altering yychar or
     if it invokes YYBACKUP.  In the case of YYABORT or YYACCEPT, an
     incorrect destructor might then be invoked immediately.  In the
     case of YYERROR or YYBACKUP, subsequent parser actions might lead
     to an incorrect destructor call or verbose syntax error message
     before the lookahead is translated.  */
  YY_SYMBOL_PRINT ("-> $$ =", yyr1[yyn], &yyval, &yyloc);

  YYPOPSTACK (yylen);
  yylen = 0;
  YY_STACK_PRINT (yyss, yyssp);

  *++yyvsp = yyval;

  /* Now 'shift' the result of the reduction.  Determine what state
     that goes to, based on the state we popped back to and the rule
     number reduced by.  */
  {
    const int yylhs = yyr1[yyn] - YYNTOKENS;
    const int yyi = yypgoto[yylhs] + *yyssp;
    yystate = (0 <= yyi && yyi <= YYLAST && yycheck[yyi] == *yyssp
               ? yytable[yyi]
               : yydefgoto[yylhs]);
  }

  goto yynewstate;


/*--------------------------------------.
| yyerrlab -- here on detecting error.  |
`--------------------------------------*/
yyerrlab:
  /* Make sure we have latest lookahead translation.  See comments at
     user semantic actions for why this is necessary.  */
  yytoken = yychar == YYEMPTY ? YYEMPTY : YYTRANSLATE (yychar);

  /* If not already recovering from an error, report this error.  */
  if (!yyerrstatus)
    {
      ++yynerrs;
#if ! YYERROR_VERBOSE
      yyerror (YY_("syntax error"));
#else
# define YYSYNTAX_ERROR yysyntax_error (&yymsg_alloc, &yymsg, \
                                        yyssp, yytoken)
      {
        char const *yymsgp = YY_("syntax error");
        int yysyntax_error_status;
        yysyntax_error_status = YYSYNTAX_ERROR;
        if (yysyntax_error_status == 0)
          yymsgp = yymsg;
        else if (yysyntax_error_status == 1)
          {
            if (yymsg != yymsgbuf)
              YYSTACK_FREE (yymsg);
            yymsg = YY_CAST (char *, YYSTACK_ALLOC (YY_CAST (YYSIZE_T, yymsg_alloc)));
            if (!yymsg)
              {
                yymsg = yymsgbuf;
                yymsg_alloc = sizeof yymsgbuf;
                yysyntax_error_status = 2;
              }
            else
              {
                yysyntax_error_status = YYSYNTAX_ERROR;
                yymsgp = yymsg;
              }
          }
        yyerror (yymsgp);
        if (yysyntax_error_status == 2)
          goto yyexhaustedlab;
      }
# undef YYSYNTAX_ERROR
#endif
    }



  if (yyerrstatus == 3)
    {
      /* If just tried and failed to reuse lookahead token after an
         error, discard it.  */

      if (yychar <= YYEOF)
        {
          /* Return failure if at end of input.  */
          if (yychar == YYEOF)
            YYABORT;
        }
      else
        {
          yydestruct ("Error: discarding",
                      yytoken, &yylval);
          yychar = YYEMPTY;
        }
    }

  /* Else will try to reuse lookahead token after shifting the error
     token.  */
  goto yyerrlab1;


/*---------------------------------------------------.
| yyerrorlab -- error raised explicitly by YYERROR.  |
`---------------------------------------------------*/
yyerrorlab:
  /* Pacify compilers when the user code never invokes YYERROR and the
     label yyerrorlab therefore never appears in user code.  */
  if (0)
    YYERROR;

  /* Do not reclaim the symbols of the rule whose action triggered
     this YYERROR.  */
  YYPOPSTACK (yylen);
  yylen = 0;
  YY_STACK_PRINT (yyss, yyssp);
  yystate = *yyssp;
  goto yyerrlab1;


/*-------------------------------------------------------------.
| yyerrlab1 -- common code for both syntax error and YYERROR.  |
`-------------------------------------------------------------*/
yyerrlab1:
  yyerrstatus = 3;      /* Each real token shifted decrements this.  */

  for (;;)
    {
      yyn = yypact[yystate];
      if (!yypact_value_is_default (yyn))
        {
          yyn += YYTERROR;
          if (0 <= yyn && yyn <= YYLAST && yycheck[yyn] == YYTERROR)
            {
              yyn = yytable[yyn];
              if (0 < yyn)
                break;
            }
        }

      /* Pop the current state because it cannot handle the error token.  */
      if (yyssp == yyss)
        YYABORT;


      yydestruct ("Error: popping",
                  yystos[yystate], yyvsp);
      YYPOPSTACK (1);
      yystate = *yyssp;
      YY_STACK_PRINT (yyss, yyssp);
    }

  YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN
  *++yyvsp = yylval;
  YY_IGNORE_MAYBE_UNINITIALIZED_END


  /* Shift the error token.  */
  YY_SYMBOL_PRINT ("Shifting", yystos[yyn], yyvsp, yylsp);

  yystate = yyn;
  goto yynewstate;


/*-------------------------------------.
| yyacceptlab -- YYACCEPT comes here.  |
`-------------------------------------*/
yyacceptlab:
  yyresult = 0;
  goto yyreturn;


/*-----------------------------------.
| yyabortlab -- YYABORT comes here.  |
`-----------------------------------*/
yyabortlab:
  yyresult = 1;
  goto yyreturn;


#if !defined yyoverflow || YYERROR_VERBOSE
/*-------------------------------------------------.
| yyexhaustedlab -- memory exhaustion comes here.  |
`-------------------------------------------------*/
yyexhaustedlab:
  yyerror (YY_("memory exhausted"));
  yyresult = 2;
  /* Fall through.  */
#endif


/*-----------------------------------------------------.
| yyreturn -- parsing is finished, return the result.  |
`-----------------------------------------------------*/
yyreturn:
  if (yychar != YYEMPTY)
    {
      /* Make sure we have latest lookahead translation.  See comments at
         user semantic actions for why this is necessary.  */
      yytoken = YYTRANSLATE (yychar);
      yydestruct ("Cleanup: discarding lookahead",
                  yytoken, &yylval);
    }
  /* Do not reclaim the symbols of the rule whose action triggered
     this YYABORT or YYACCEPT.  */
  YYPOPSTACK (yylen);
  YY_STACK_PRINT (yyss, yyssp);
  while (yyssp != yyss)
    {
      yydestruct ("Cleanup: popping",
                  yystos[+*yyssp], yyvsp);
      YYPOPSTACK (1);
    }
#ifndef yyoverflow
  if (yyss != yyssa)
    YYSTACK_FREE (yyss);
#endif
#if YYERROR_VERBOSE
  if (yymsg != yymsgbuf)
    YYSTACK_FREE (yymsg);
#endif
  return yyresult;
}
#line 866 "misc/parser.y"


// =========================================================================
// main - ulazna tacka asemblera
// =========================================================================

int main(int argc, char* argv[])
{
    if (argc < 4 || strcmp(argv[1], "-o") != 0) {
        fprintf(stderr, "Upotreba: %s -o <izlaz> <ulaz>\n", argv[0]);
        return 1;
    }

    const char* output_file = argv[2];
    const char* input_file  = argv[3];

    FILE* file = fopen(input_file, "r");
    if (!file) {
        fprintf(stderr, "Ne mogu da otvorim ulaznu datoteku: %s\n", input_file);
        return 1;
    }
    yyin = file;

    yyparse();

    fclose(file);

    // Finalizacija: snimi velicine sekcija, kreiraj relokacije
    assm.finalize();

    if (assm.hasErrors()) {
        fprintf(stderr, "Greske pri asembliranju:\n");
        for (auto& e : assm.errors()) {
            fprintf(stderr, "  %s\n", e.c_str());
        }
        return 1;
    }

    // Snimi izlaz
    assm.writeOutput(output_file);

    return 0;
}

void yyerror(const char* s)
{
    fprintf(stderr, "Greska (linija %d): %s\n", yylineno, s);
}
