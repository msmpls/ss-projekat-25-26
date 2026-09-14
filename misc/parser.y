/*
 * parser.y - jednoprolazni asembler za SS projekat 2025/26
 *
 * Instrukcioni format (postavka strana 13):
 *   bits 31-28: OC | bits 27-24: MOD
 *   bits 23-20: RegA | bits 19-16: RegB | bits 15-12: RegC
 *   bits 11-0 : Disp (oznaceni 12-bitni pomeraj)
 *
 * Sve instrukcije su fiksno 4 bajta.
 *
 * Mapa opkodova (strane 14-16):
 *   halt=0x0  int=0x1  call=0x2  jmp=0x3  xchg=0x4
 *   arit (add/sub/mul/div)=0x5  log (not/and/or/xor)=0x6
 *   shift (shl/shr)=0x7  st=0x8  ld=0x9
 *
 * Mapa MOD vrednosti za ld/st (strana 16):
 *   ld:
 *     0x0 csrrd: gpr[A] <= csr[B]
 *     0x1 imm:   gpr[A] <= gpr[B] + D           (za $literal, $simbol)
 *     0x2 mem:   gpr[A] <= mem32[gpr[B]+gpr[C]+D] (za [reg+lit], [reg+sim], [reg])
 *     0x3 mem+post: gpr[A] <= mem32[gpr[B]]; gpr[B] <= gpr[B] + D
 *     0x4 csrwr: csr[A] <= gpr[B]               (ovo je asemblerski csrwr)
 *     0x5        csr[A] <= csr[B] | D
 *     0x6        csr[A] <= mem32[gpr[B]+gpr[C]+D]
 *     0x7        csr[A] <= mem32[gpr[B]]; gpr[B] <= gpr[B] + D
 *   st:
 *     0x0 mem32[gpr[A]+gpr[B]+D] <= gpr[C]
 *     0x1 gpr[A] <= gpr[A] + D; mem32[gpr[A]] <= gpr[C]  (pre-dekrement? zapravo post-inc)
 *     0x2 mem32[mem32[gpr[A]+gpr[B]+D]] <= gpr[C]        (indirektno)
 *
 * Skokovi (strana 14):
 *   jmp=OC 0x3:
 *     MOD 0x0 pc <= gpr[A] + D
 *     MOD 0x1 if (gpr[B] == gpr[C]) pc <= gpr[A] + D   (beq)
 *     MOD 0x2 if (gpr[B] != gpr[C]) pc <= gpr[A] + D   (bne)
 *     MOD 0x3 if (gpr[B] >  gpr[C]) pc <= gpr[A] + D   (bgt)
 *
 * Sintaksne notacije za skok (strana 8):
 *   <literal> - vrednost literala
 *   <simbol>  - vrednost simbola
 *
 * Sintaksne notacije za podatke (strana 8):
 *   $<literal>     - vrednost literala
 *   $<simbol>      - vrednost simbola
 *   <literal>      - vrednost iz memorije na adresi literala
 *   <simbol>       - vrednost iz memorije na adresi simbola
 *   %<reg>         - vrednost u registru
 *   [%<reg>]       - vrednost iz memorije na adresi reg
 *   [%<reg> + <literal>] - vrednost iz memorije na adresi reg + literal
 *   [%<reg> + <simbol>]  - vrednost iz memorije na adresi reg + simbol
 *
 * Napomena: Po postavci, jmp/beq/bne/bgt/call imaju samo <literal> ili <simbol>
 * kao operand (ne i [%reg] itd.). Ali instrukcioni format podrzava i mem varijante.
 * Za nivo A zadrzavamo strogo ono sto je u sintaksi.
 *
 * call (strana 14):
 *   OC 0x2, MOD 0x0: push pc; pc <= gpr[A]+gpr[B]+D
 *   OC 0x2, MOD 0x1: push pc; pc <= mem32[gpr[A]+gpr[B]+D]
 *   Sintaksa: call <operand> gde je operand <literal> ili <simbol>
 *   => MOD 0x0, A=15(pc), B=0(r0), D=vrednost
 */

%{
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

// Globalni brojač semantičkih grešaka (pozivi yyerror iz akcija)
static int semantic_errors = 0;

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

%}

%union {
    int  num;
    char *str;
}

%token GLOBAL EXTERN SECTION WORD SKIP ASCII END
%token HALT INT IRET CALL RET JMP BEQ BNE BGT PUSH POP XCHG
%token ADD SUB MUL DIV NOT AND OR XOR SHL SHR LD ST CSRRD CSRWR
%token DOLLAR LBRACKET RBRACKET COLON COMMA PLUS MINUS STAR SLASH LPAREN RPAREN
%token EOL

%token <num> NUMBER REGISTER CSR
%token <str> IDENT STRING

%type <num> literal
%type <str> symbol

%start source

%%

source
    : line_list end_line
    ;

line_list
    : %empty
    | line_list line
    ;

line
    : EOL
    | label_def EOL
    | directive EOL
    | instruction EOL
    | label_def directive EOL
    | label_def instruction EOL
    ;

label_def
    : IDENT COLON
      {
          assm.defineLabel($1);
          free($1);
      }
    ;

end_line
    : END
    | END EOL
    ;

// =========================================================================
// Direktive
// =========================================================================

directive
    : GLOBAL symbol_list
      {
          // Svi simboli iz liste su vec dodati u sym_list
          // (vidi symbol_list produkciju ispod)
          for (auto& s : sym_list) {
              assm.markGlobal(s);
          }
          sym_list.clear();
      }
    | EXTERN symbol_list
      {
          for (auto& s : sym_list) {
              assm.markExtern(s);
          }
          sym_list.clear();
      }
    | SECTION IDENT
      {
          assm.switchSection($2);
          free($2);
      }
    | WORD word_list
    | SKIP literal
      {
          assm.skipBytes($2);
      }
    | ASCII STRING
      {
          emit_ascii_string($2);
          free($2);
      }
    ;

symbol_list
    : IDENT
      {
          sym_list.push_back($1);
          free($1);
      }
    | symbol_list COMMA IDENT
      {
          sym_list.push_back($3);
          free($3);
      }
    ;

word_list
    : word_init
    | word_list COMMA word_init
    ;

word_init
    : IDENT
      {
          // Simbol - proveri da li je poznat
          int val = assm.getSymbolValue($1);
          if (val >= 0 && assm.getSymbolSection($1) == assm.currentSection()) {
              assm.emitWord((uint32_t)val);
          } else {
              // Nije poznat ili u drugoj sekciji -> placeholder + patch
              int offset = assm.locationCounter();
              assm.emitWord(0);
              assm.addPatch($1, offset, '+', 0, RelocType::R_ABS_32);
          }
          free($1);
      }
    | literal
      {
          assm.emitWord((uint32_t)$1);
      }
    ;

// =========================================================================
// Instrukcije
// =========================================================================

instruction
    : HALT
      {
          assm.emitInstruction(0x0, 0x0, 0, 0, 0, 0);
      }
    | INT
      {
          assm.emitInstruction(0x1, 0x0, 0, 0, 0, 0);
      }
    | IRET
      {
          // iret = pop pc; pop status
          // Postavka ne daje poseban opkod - rezervisemo OC=0, MOD=1
          // (emulator prepoznaje (OC=0, MOD=1) kao iret)
          assm.emitInstruction(0x0, 0x1, 0, 0, 0, 0);
      }
    | RET
      {
          // ret = pop pc
          // Implementirano kao ld sa post-increment (MOD=3):
          //   gpr[A] <= mem32[gpr[B]]; gpr[B] <= gpr[B] + D
          // A = pc (15), B = sp (14), D = +4
          assm.emitInstruction(0x9, 0x3, 15, 14, 0, 4);
      }
    | CALL jump_operand
      {
          // call - koristi emit_jump_or_call za automatski odabir direktne/indirektne varijante
          emit_jump_or_call(0x2, 0x0, 0, 0, true);
          reset_operand_state();
      }
    | JMP jump_operand
      {
          // jmp - koristi emit_jump_or_call
          emit_jump_or_call(0x3, 0x0, 0, 0, false);
          reset_operand_state();
      }
    | BEQ REGISTER COMMA REGISTER COMMA jump_operand
      {
          // beq - OC 0x3, MOD 0x1 (if gpr[B]==gpr[C] pc <= ...)
          emit_jump_or_call(0x3, 0x1, $2, $4, false);
          reset_operand_state();
      }
    | BNE REGISTER COMMA REGISTER COMMA jump_operand
      {
          // bne - OC 0x3, MOD 0x2 (if gpr[B]!=gpr[C] pc <= ...)
          emit_jump_or_call(0x3, 0x2, $2, $4, false);
          reset_operand_state();
      }
    | BGT REGISTER COMMA REGISTER COMMA jump_operand
      {
          // bgt - OC 0x3, MOD 0x3 (if gpr[B] > gpr[C] pc <= ...)
          emit_jump_or_call(0x3, 0x3, $2, $4, false);
          reset_operand_state();
      }
    | PUSH REGISTER
      {
          // push = sp -= 4; mem32[sp] <= gpr
          // Implementirano kroz st instrukciju:
          //   st = OC 0x8, MOD 0x1 (gpr[A] <= gpr[A] + D; mem32[gpr[A]] <= gpr[C])
          //   A = sp (14), D = -4, C = $2
          // Ali MOD 0x1 za st je "gpr[A] <= gpr[A] + D; mem32[gpr[A]] <= gpr[C]"
          // sto je post-dekrement (ako je D negativno)
          assm.emitInstruction(0x8, 0x1, 14, 0, $2, -4);
      }
    | POP REGISTER
      {
          // pop = gpr <= mem32[sp]; sp += 4
          // Implementirano kroz ld instrukciju:
          //   ld = OC 0x9, MOD 0x3 (gpr[A] <= mem32[gpr[B]]; gpr[B] <= gpr[B] + D)
          //   A = $2, B = sp (14), D = 4
          assm.emitInstruction(0x9, 0x3, $2, 14, 0, 4);
      }
    | XCHG REGISTER COMMA REGISTER
      {
          // xchg = OC 0x4, MOD 0x0
          // A=0, B=$2, C=$4
          assm.emitInstruction(0x4, 0x0, 0, $2, $4, 0);
      }
    | ADD REGISTER COMMA REGISTER
      {
          // add = OC 0x5, MOD 0x0: gpr[A] <= gpr[B] + gpr[C]
          // A=$2, B=$4, C=0 (postavka: add %gprS, %gprD => gprD <= gprD + gprS)
          // Zapravo: gprD = A, gprS = B, pa je: gpr[A] <= gpr[A] + gpr[B]?
          // Postavka kaze: add %gprS, %gprD => gprD <= gprD + gprS
          // => A = gprD, B = gprS, ali instr format je gpr[A] <= gpr[B] + gpr[C]
          // Znaci A = gprD, B = gprD, C = gprS
          // (kombinacija koja daje A <= B + C = gprD + gprS)
            assm.emitInstruction(0x5, 0x0, $4, $4, $2, 0);
      }
    | SUB REGISTER COMMA REGISTER
      {
          // sub %gprS, %gprD => gprD <= gprD - gprS
          // A = gprD, B = gprD, C = gprS
         assm.emitInstruction(0x5, 0x1, $4, $4, $2, 0);
      }
    | MUL REGISTER COMMA REGISTER
      {
          assm.emitInstruction(0x5, 0x2, $4, $4, $2, 0);
      }
    | DIV REGISTER COMMA REGISTER
      {
          assm.emitInstruction(0x5, 0x3, $4, $4, $2, 0);
      }
    | NOT REGISTER
      {
          // not %gpr => gpr <= ~gpr
          // OC 0x6, MOD 0x0: gpr[A] <= ~gpr[B]
          // A = $2, B = $2
          assm.emitInstruction(0x6, 0x0, $2, $2, 0, 0);
      }
    | AND REGISTER COMMA REGISTER
      {
          // and %gprS, %gprD => gprD <= gprD & gprS
          // OC 0x6, MOD 0x1: gpr[A] <= gpr[B] & gpr[C]
          // A = gprD, B = gprD, C = gprS
           assm.emitInstruction(0x6, 0x1, $4, $4, $2, 0);
      }
    | OR REGISTER COMMA REGISTER
      {
          assm.emitInstruction(0x6, 0x2, $4, $4, $2, 0);
      }
    | XOR REGISTER COMMA REGISTER
      {
          assm.emitInstruction(0x6, 0x3, $4, $4, $2, 0);
      }
    | SHL REGISTER COMMA REGISTER
      {
          // shl %gprS, %gprD => gprD <= gprD << gprS
          // OC 0x7, MOD 0x0: gpr[A] <= gpr[B] << gpr[C]
          assm.emitInstruction(0x7, 0x0, $4, $4, $2, 0);
      }
    | SHR REGISTER COMMA REGISTER
      {
          assm.emitInstruction(0x7, 0x1, $4, $4, $2, 0);
      }
    | CSRRD CSR COMMA REGISTER
      {
          // csrrd %csr, %gpr => gpr <= csr
          // ld = OC 0x9, MOD 0x0: gpr[A] <= csr[B]
          // A = $4, B = $2 (csr)
          assm.emitInstruction(0x9, 0x0, $4, $2, 0, 0);
      }
    | CSRWR REGISTER COMMA CSR
      {
          // csrwr %gpr, %csr => csr <= gpr
          // ld = OC 0x9, MOD 0x4: csr[A] <= gpr[B]
          // A = $4 (csr), B = $2 (gpr)
          assm.emitInstruction(0x9, 0x4, $4, $2, 0, 0);
      }
    | LD data_operand COMMA REGISTER
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
                  assm.emitInstruction(0x9, 0x2, $4, 15, 0, 4);
                  // jmp +4 (preskoči pool word)
                  assm.emitInstruction(0x3, 0x0, 15, 0, 0, 4);
                  // Pool word
                  emit_pool_word();
              } else {
                  mod = 0x1;
                  offset = assm.emitInstruction(0x9, mod, $4, 0, 0, 0);
                  emit_disp_patch(offset, false);  // apsolutno (ne PC-rel)
              }
          } else if (ops.is_register) {
              // %reg - ovo je "vrednost u registru"
              // Mod 0x1 sa D=0: gpr[A] <= gpr[B] + 0 = gpr[B]
              mod = 0x1;
              offset = assm.emitInstruction(0x9, mod, $4, ops.reg, 0, 0);
          } else if (ops.is_memory) {
              if (ops.is_bare_symbol) {
                  // Sam <simbol> ili <literal> (bez $ i bez [])
                  if (needs_pool_for_value()) {
                      // Dve instrukcije + pool:
                      // 1. ld [pc+8], %r_dest  — učitaj adresu iz pool-a
                      // 2. ld [%r_dest], %r_dest — dereferenciraj
                      // 3. jmp +4 — preskoči pool
                      // 4. pool word — adresa simbola
                      assm.emitInstruction(0x9, 0x2, $4, 15, 0, 8);   // ld [pc+8], %r_dest
                      assm.emitInstruction(0x9, 0x2, $4, $4, 0, 0);    // ld [%r_dest], %r_dest
                      assm.emitInstruction(0x3, 0x0, 15, 0, 0, 4);     // jmp +4
                      emit_pool_word();
                  } else {
                      mod = 0x2;
                      offset = assm.emitInstruction(0x9, mod, $4, 0, 0, 0);
                      emit_disp_patch(offset, false);
                  }
              } else if (ops.pending_symbol.empty() && !ops.is_pc_relative) {
                  // [reg + lit] ili [reg]
                  // ld MOD 0x2: gpr[A] <= mem32[gpr[B] + gpr[C] + D]
                  if (ops.value < -2048 || ops.value > 2047) {
                      yyerror("Displacement out of 12-bit range");
                  }
                  mod = 0x2;
                  offset = assm.emitInstruction(0x9, mod, $4, ops.reg, 0, ops.value);
              } else if (ops.is_pc_relative) {
                  // simbol (memorijsko na adresi simbola) - APSOLUTNO preko r0
                  // Ako adresa staje u 12-bit: MOD=0x2, B=r0, C=r0, D=adresa
                  if (needs_pool_for_value()) {
                      yyerror("ld <simbol> sa dalekim simbolom nije podržano (treba tmp registar)");
                  } else {
                      mod = 0x2;
                      offset = assm.emitInstruction(0x9, mod, $4, 0, 0, 0);
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
                      offset = assm.emitInstruction(0x9, mod, $4, ops.reg, 0, 0);
                      emit_disp_patch(offset, false);
                  }
              }
          }
          reset_operand_state();
      }
    | ST REGISTER COMMA data_operand
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
                      assm.emitInstruction(0x8, 0x2, 15, 0, $2, 4);
                      // jmp +4 (preskoči pool)
                      assm.emitInstruction(0x3, 0x0, 15, 0, 0, 4);
                      emit_pool_word();
                  } else {
                      mod = 0x0;
                      offset = assm.emitInstruction(0x8, mod, 0, 0, $2, 0);
                      emit_disp_patch(offset, false);
                  }
              } else if (ops.pending_symbol.empty() && !ops.is_pc_relative) {
                  // [reg + lit] ili [reg]
                  // st MOD 0x0: mem32[gpr[A]+gpr[B]+D] <= gpr[C]
                  if (ops.value < -2048 || ops.value > 2047) {
                      yyerror("Displacement out of 12-bit range");
                  }
                  mod = 0x0;
                  offset = assm.emitInstruction(0x8, mod, ops.reg, 0, $2, ops.value);
              } else if (ops.is_pc_relative) {
                  // simbol (memorijsko) - APSOLUTNO preko r0
                  // Ako adresa staje u 12-bit: MOD=0x0, A=r0, B=r0, C=$2, D=adresa
                  // Inace: st indirektno (MOD=0x2) + jmp +4 + pool word
                  if (needs_pool_for_value()) {
                      // st MOD=0x2: mem32[mem32[gpr[A]+gpr[B]+D]] <= gpr[C]
                      // A=pc(15), B=0, C=$2, D=4 (pool je na pc+4 = cur_pc+8)
                      assm.emitInstruction(0x8, 0x2, 15, 0, $2, 4);
                      // jmp +4 (preskoči pool)
                      assm.emitInstruction(0x3, 0x0, 15, 0, 0, 4);
                      emit_pool_word();
                  } else {
                      mod = 0x0;
                      offset = assm.emitInstruction(0x8, mod, 0, 0, $2, 0);
                      emit_disp_patch(offset, false);
                  }
              } else {
                  // [reg + simbol]
                  if (needs_pool_for_value()) {
                      yyerror("st [reg + simbol] sa dalekim simbolom nije podržano");
                  } else {
                      mod = 0x0;
                      offset = assm.emitInstruction(0x8, mod, ops.reg, 0, $2, 0);
                      emit_disp_patch(offset, false);
                  }
              }
          }
          reset_operand_state();
      }
    ;

// =========================================================================
// Operandi
// =========================================================================

// Skok-operand (postavka strana 8): samo <literal> ili <simbol>
jump_operand
    : literal
      {
          reset_operand_state();
          ops.value = $1;
      }
    | symbol
      {
          reset_operand_state();
          ops.pending_symbol = $1;
          ops.is_pc_relative = true;  // skok je PC-rel
          free($1);
      }
    ;

// Data-operand (postavka strana 8): sve notacije
data_operand
    : DOLLAR literal
      {
          reset_operand_state();
          ops.value = $2;
          ops.is_immediate = true;
      }
    | DOLLAR symbol
      {
          reset_operand_state();
          ops.pending_symbol = $2;
          ops.is_immediate = true;
          free($2);
      }
    | literal
      {
          // <literal> - memorijsko na adresi literala (APSOLUTNO, ne PC-rel!)
          // Koristi r0 (uvek 0) kao bazni registar: mem32[r0 + 0 + literal]
          reset_operand_state();
          ops.value = $1;
          ops.is_memory = true;
          ops.is_bare_symbol = true;
          ops.is_pc_relative = false;  // apsolutno preko r0
      }
    | symbol
      {
          // <simbol> - memorijsko na adresi simbola (APSOLUTNO, ne PC-rel!)
          // Koristi r0 kao bazni registar: mem32[r0 + 0 + simbol_adresa]
          reset_operand_state();
          ops.pending_symbol = $1;
          ops.is_memory = true;
          ops.is_bare_symbol = true;
          ops.is_pc_relative = false;  // apsolutno preko r0
          free($1);
      }
    | REGISTER
      {
          // %reg
          reset_operand_state();
          ops.is_register = true;
          ops.reg = $1;
      }
    | LBRACKET REGISTER RBRACKET
      {
          // [reg]
          reset_operand_state();
          ops.is_memory = true;
          ops.reg = $2;
          ops.value = 0;
      }
    | LBRACKET REGISTER PLUS literal RBRACKET
      {
          // [reg + literal]
          reset_operand_state();
          ops.is_memory = true;
          ops.reg = $2;
          ops.value = $4;
      }
    | LBRACKET REGISTER PLUS symbol RBRACKET
      {
          // [reg + simbol]
          reset_operand_state();
          ops.is_memory = true;
          ops.reg = $2;
          ops.pending_symbol = $4;
          free($4);
      }
    ;

// Literali: NUMBER, +NUMBER, -NUMBER
literal
    : NUMBER
      { $$ = $1; }
    | PLUS NUMBER
      { $$ = $2; }
    | MINUS NUMBER
      { $$ = -$2; }
    ;

// Simbol je samo IDENT (za sada)
symbol
    : IDENT
      { $$ = $1; }
    ;

%%

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

    if (assm.hasErrors() || semantic_errors > 0) {
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
    semantic_errors++;
}