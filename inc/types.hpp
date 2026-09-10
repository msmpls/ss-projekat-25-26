#ifndef TYPES_HPP
#define TYPES_HPP

/*
 * types.hpp - zajednicke strukture podataka za asembler
 *
 * Inspirisano:
 *   - PPT "Konstrukcija asemblera" Zadatak 2 (SectionDefinition, Symbol)
 *   - PPT Zadatak 3 (Patch / flink lista za backpatching)
 *   - PPT Zadatak 9 (Relocation record format za skolski ELF)
 *   - Postavka 2025/26 strana 13 (instrukcioni format OC|MOD|RegA|RegB|RegC|Disp[11:0])
 *
 * Nivo A + B: .global, .extern, .section, .word, .skip, .ascii, .end
 * Nivo C (.equ) nije implementiran.
 */

#include <string>
#include <vector>
#include <cstdint>

// =========================================================================
// Tipovi relokacija za nas apstraktni racunarski sistem
// =========================================================================
// Instrukcioni format (postavka strana 13):
//   bits 31-28: OC      bits 27-24: MOD
//   bits 23-20: RegA    bits 19-16: RegB    bits 15-12: RegC
//   bits 11-0 : Disp (oznaceni 12-bitni pomeraj)
//
// .word direktiva alocira 4 bajta (postavka strana 6).
// =========================================================================
enum class RelocType : uint8_t {
    R_ABS_32    = 0,   // 32-bitna apsolutna adresa (npr. .word simbol)
    R_PC_REL_12 = 1,   // 12-bitni PC-relativni pomeraj (Disp polje za jmp/call/beq/bne/bgt)
    R_ABS_12    = 2,   // 12-bitni apsolutni pomeraj (Disp polje za ld/st [%reg + simbol])
};

// =========================================================================
// Patch - unos u flink listi simbola (PPT Zadatak 3, strana 14-15)
// =========================================================================
// Predstavlja referencu unapred koja ceka da simbol bude definisan.
// Kada se simbol definise, prolazi se kroz njegovu flink listu i za svaki
// patch se upisuje vrednost simbola na odgovarajucu adresu (sa action +/-).
//
// Na kraju asembliranja:
//   - ako je simbol definisan u istoj sekciji -> backpatch (interni rezultat)
//   - ako je simbol u drugoj sekciji ili je extern -> postaje Relocation zapis
// =========================================================================
struct Patch {
    std::string symbol_name;   // ime simbola koji se ceka
    std::string section;       // u kojoj sekciji je patch (gde treba upisati)
    int         offset;        // bajt offset unutar sekcije (gde treba upisati)
    char        action;        // '+' (dodaj vrednost simbola) ili '-' (oduzmi)
    int         addend;        // konstantni deo izraza (npr. za "simbol + 5" je addend=5)
    RelocType   type;          // tip relokacije odredjuje velicinu i nacin upisa
};

// =========================================================================
// Symbol - unos u tabeli simbola (PPT Zadatak 2 + Zadatak 3)
// =========================================================================
// Polja prate PPT Zadatak 9 strana 53 (format .symtab):
//   Value  = offset u sekciji (ili 0 ako nedefinisan)
//   Type   = NOTYP (obicni simbol) ili SCTN (sekcija kao simbol)
//   Bind   = LOC (lokalni) ili GLOB (globalni)
//   Ndx    = section_id (indeks sekcije) ili UND (-1) za extern
// =========================================================================
struct Symbol {
    std::string name;
    int         value;         // offset u sekciji; -1 ako nedefinisan
    std::string section;       // ime sekcije; "undefined" ako extern
    int         section_id;    // -1 = undefined/extern, -2 = apsolutni, >=1 = u sekciji
    bool        is_global;     // pojavio se u .global
    bool        is_extern;     // pojavio se u .extern
    bool        is_defined;    // ima definiciju (labela ili .section)
    bool        is_section;    // da li je ovo simbol-sekcija (Type=SCTN)
    int         symtab_index;  // indeks u .symtab (dodeljuje se u finalize())
    std::vector<Patch> flink;  // lista referenci unapred (PPT Zadatak 3)

    Symbol()
        : value(-1), section_id(-1),
          is_global(false), is_extern(false),
          is_defined(false), is_section(false),
          symtab_index(-1) {}
};

// =========================================================================
// Relocation - zapis o relokaciji za linker (PPT Zadatak 9, strana 53)
// =========================================================================
// Format izlaza (.rela.<sek>):
//   Offset  = offset unutar sekcije
//   Type    = R_ABS_32 / R_PC_REL_12 / R_ABS_12
//   Symbol  = indeks simbola u .symtab
//   Addend  = konstantni deo
// =========================================================================
struct Relocation {
    int         offset;        // bajt offset unutar sekcije
    RelocType   type;          // tip relokacije
    std::string symbol_name;   // ime simbola (za povezivanje sa .symtab)
    int         symbol_index;  // indeks simbola u .symtab (dodeljuje se pri izlazu)
    int         addend;        // konstantni deo

    Relocation() : offset(0), type(RelocType::R_ABS_32),
                   symbol_index(-1), addend(0) {}
};

// =========================================================================
// Section - sekcija objektnog programa (PPT Zadatak 2, strana 9)
// =========================================================================
// Prosjirena u odnosu na PPT sa bytes[] i relocations[] nizovima.
// Polja 'base' i 'length' prate PPT SectionDefinition.
// =========================================================================
struct Section {
    std::string  name;
    int          id;           // numeric id (1, 2, 3, ...) - koristi se u .symtab Ndx
    int          base;         // pocetna adresa (0 tokom asembliranja, linker je postavlja)
    int          length;       // velicina u bajtovima
    std::vector<uint8_t>    bytes;         // sadrzaj sekcije
    std::vector<Relocation> relocations;   // relokacije za ovu sekciju

    Section() : id(-1), base(0), length(0) {}
};

// =========================================================================
// DummySymbol - pomocna struktura za parsiranje listi u .global / .extern / .word
// =========================================================================
// Parser akumulira simbole u listu dok parsira "a, b, c", pa poziva
// odgovarajucu metodu Assembler-a jednom sa celom listom.
// (Inspirisano koleginim parser.y pristupom.)
// =========================================================================
struct DummySymbol {
    std::string name;
    int         loc_counter;   // vrednost loc_counter u trenutku parsiranja
};

//LINKER**********************************************************************
// InputSection - sekcija iz jednog ulaznog .o fajla
struct InputSection {
    std::string name;
    int         file_id;          // iz kog fajla (0, 1, 2, ...)
    int         original_id;      // id u originalnom fajlu (1, 2, ...)
    int         size;             // velicina u bajtovima
    std::vector<uint8_t> bytes;   // sadrzaj
    std::vector<Relocation> relocations;  // relokacije ove sekcije
};

// InputSymbol - simbol iz jednog ulaznog .o fajla
struct InputSymbol {
    std::string name;
    int         value;            // offset u sekciji (ili 0 ako nedefinisan)
    std::string section;          // ime sekcije (ili "undefined" za extern)
    int         section_id;       // -1 = UND, >=1 = u sekciji
    bool        is_global;
    bool        is_extern;
    bool        is_defined;
    bool        is_section;
    int         file_id;          // iz kog fajla
    int         symtab_index;     // indeks u originalnom .symtab

    InputSymbol()
        : value(0), section_id(-1),
          is_global(false), is_extern(false),
          is_defined(false), is_section(false),
          file_id(-1), symtab_index(-1) {}
};

// InputFile - jedan ulazni .o fajl
struct InputFile {
    std::string filename;
    int         id;                              // redni broj (0, 1, 2, ...)
    std::vector<InputSection> sections;          // sekcije u ovom fajlu
    std::vector<InputSymbol>  symbols;           // simboli u ovom fajlu
};

// MergedSection - spojena sekcija (agregirana iz svih ulaznih fajlova)
struct MergedSection {
    std::string  name;
    uint32_t     address;         // finalna adresa (posle smeštanja)
    int          size;            // ukupna velicina (zbir svih delova)
    std::vector<uint8_t> bytes;   // spojeni sadrzaj
    std::vector<Relocation> relocations;  // sve relokacije ove sekcije

    // Pending relokacije - nerazrešene u -relocatable modu, preneti u izlaz
    // Sadrže: originalni offset (unutar parta), file_id, type, symbol_name, addend
    struct PendingRelocation {
        int         part_offset;     // gde u merged bytes[] se nalazi part iz kog fajla dolazi
        int         original_offset; // offset unutar originalne sekcije
        int         file_id;         // iz kog fajla (za pronalaženje part_offset-a)
        RelocType   type;
        std::string symbol_name;
        int         addend;
    };
    std::vector<PendingRelocation> pending_relocations;

    // Lista delova - koji ulazni fajl doprinosi kom delu merged sekcije
    struct SectionPart {
        int file_id;              // iz kog ulaznog fajla
        int input_size;           // velicina tog dela
        int merged_offset;        // gde u merged bytes[] počinje ovaj deo
    };
    std::vector<SectionPart> parts;

    MergedSection() : address(0), size(0) {}
};

// MergedSymbol - simbol u spojenoj tabeli simbola
struct MergedSymbol {
    std::string  name;
    uint32_t     final_address;   // finalna adresa (posle linkovanja)
    std::string  section;         // u kojoj spojenoj sekciji je
    bool         is_global;
    bool         is_defined;
    bool         is_extern;
    bool         is_section;
    int          symtab_index;    // za -relocatable izlaz

    MergedSymbol()
        : final_address(0), is_global(false),
          is_defined(false), is_extern(false),
          is_section(false), symtab_index(-1) {}
};

#endif // TYPES_HPP