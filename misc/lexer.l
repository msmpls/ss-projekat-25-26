%option noyywrap
%option yylineno

%x AFTER_END

%{
#include "parser.tab.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Pomocne funkcije za prepoznavanje / mapiranje registara i CSR-ova
static int is_register(const char *s);
static int is_csr(const char *s);
static int register_index(const char *s);
static int csr_index(const char *s);

// Pomocna za kopiranje yytext u yylval.str (identifikatori i stringovi)
static char *dup_str(const char *s);
%}

DIGIT   [0-9]
HEX     [0-9a-fA-F]
ID      [a-zA-Z_][a-zA-Z0-9_]*

%%

<AFTER_END>.|\n          ;

[ \t\r]+                 ;
"#"[^\n]*                ;
\n                       { return EOL; }

\.global                 { return GLOBAL; }
\.extern                 { return EXTERN; }
\.section                { return SECTION; }
\.word                   { return WORD; }
\.skip                   { return SKIP; }
\.ascii                  { return ASCII; }
\.end                    { BEGIN(AFTER_END); return END; }

halt                     { return HALT; }
int                      { return INT; }
iret                     { return IRET; }
call                     { return CALL; }
ret                      { return RET; }
jmp                      { return JMP; }
beq                      { return BEQ; }
bne                      { return BNE; }
bgt                      { return BGT; }
push                     { return PUSH; }
pop                      { return POP; }
xchg                     { return XCHG; }
add                      { return ADD; }
sub                      { return SUB; }
mul                      { return MUL; }
div                      { return DIV; }
not                      { return NOT; }
and                      { return AND; }
or                       { return OR; }
xor                      { return XOR; }
shl                      { return SHL; }
shr                      { return SHR; }
ld                       { return LD; }
st                       { return ST; }
csrrd                    { return CSRRD; }
csrwr                    { return CSRWR; }

0[xX]{HEX}+              {
                             yylval.num = (int)strtol(yytext, NULL, 16);
                             return NUMBER;
                         }
{DIGIT}+                 {
                             yylval.num = (int)strtol(yytext, NULL, 10);
                             return NUMBER;
                         }

%[a-zA-Z_][a-zA-Z0-9_]*  {
                             if (is_register(yytext)) {
                                 yylval.num = register_index(yytext);
                                 return REGISTER;
                             }
                             if (is_csr(yytext)) {
                                 yylval.num = csr_index(yytext);
                                 return CSR;
                             }
                             fprintf(stderr,
                                     "lexer error at line %d: unknown register/CSR '%s'\n",
                                     yylineno, yytext);
                             exit(EXIT_FAILURE);
                         }

{ID}                     {
                             yylval.str = dup_str(yytext);
                             return IDENT;
                         }

\"([^\"\\]|\\.)*\"       {
                             // String sa escape sekvencama - cuvamo sirov tekst
                             // (parser / assembler obradjuje escape sekvence)
                             yylval.str = dup_str(yytext);
                             return STRING;
                         }

\$                       { return DOLLAR; }
\[                       { return LBRACKET; }
\]                       { return RBRACKET; }
:                        { return COLON; }
,                        { return COMMA; }
\+                       { return PLUS; }
-                        { return MINUS; }
\*                       { return STAR; }
\/                       { return SLASH; }
\(                       { return LPAREN; }
\)                       { return RPAREN; }

.                        {
                             fprintf(stderr,
                                     "lexer error at line %d: unexpected character '%s'\n",
                                     yylineno, yytext);
                             exit(EXIT_FAILURE);
                         }

%%

// ---------------------------------------------------------------------
// Pomocne funkcije
// ---------------------------------------------------------------------

static int is_register(const char *s)
{
    if (s[0] != '%') return 0;

    // %sp i %pc su aliasi za r14 i r15
    if (strcmp(s, "%sp") == 0) return 1;
    if (strcmp(s, "%pc") == 0) return 1;

    // %r0..%r15
    if (s[1] != 'r') return 0;
    if (s[2] == '\0') return 0;

    char *end;
    long n = strtol(s + 2, &end, 10);
    if (*end != '\0') return 0;
    return n >= 0 && n <= 15;
}

static int is_csr(const char *s)
{
    return strcmp(s, "%status") == 0 ||
           strcmp(s, "%handler") == 0 ||
           strcmp(s, "%cause") == 0;
}

static int register_index(const char *s)
{
    if (strcmp(s, "%sp") == 0) return 14;
    if (strcmp(s, "%pc") == 0) return 15;

    // %rN
    return (int)strtol(s + 2, NULL, 10);
}

static int csr_index(const char *s)
{
    // Indeksi po postavci strana 13:
    //   status = 0, handler = 1, cause = 2
    if (strcmp(s, "%status")  == 0) return 0;
    if (strcmp(s, "%handler") == 0) return 1;
    if (strcmp(s, "%cause")   == 0) return 2;
    return -1;
}

static char *dup_str(const char *s)
{
    // strdup nije standardno u -std=c11, pa pravimo rucno
    size_t n = strlen(s) + 1;
    char *p = (char*)malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}
