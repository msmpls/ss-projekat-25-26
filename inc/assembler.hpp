#ifndef ASSEMBLER_HPP
#define ASSEMBLER_HPP

/*
 * assembler.hpp - interfejs jednoprolaznog asemblera
 *
 * Arhitektura (jednoprolazni asembler sa backpatching-om):
 *   1. Parser tokom parsiranja zove metode ove klase (emitByte, emitWord,
 *      emitInstruction, defineLabel, markGlobal, ...).
 *   2. Pri referenci na simbol koji jos nije definisan, parser emituje
 *      nule (placeholder) i zove addPatch() da registruje pending fixup.
 *   3. Kada se simbol kasnije definise (defineLabel), flink lista se
 *      prolazi i sve adrese se popunjavaju (backpatch).
 *   4. Na kraju (finalize()) se:
 *        - azuriraju velicine sekcija
 *        - preostali patch-evi (extern / druga sekcija) postaju relokacije
 *        - prijavljuju greske za nedefinisane simbole
 *   5. writeOutput() upisuje skolski ELF format (PPT Zadatak 9).
 *
 * Instrukcioni format (postavka strana 13):
 *   bits 31-28: OC | bits 27-24: MOD
 *   bits 23-20: RegA | bits 19-16: RegB | bits 15-12: RegC
 *   bits 11-0 : Disp (oznaceni 12-bitni pomeraj)
 *
 * Sve instrukcije su fiksno 4 bajta.
 */

#include <string>
#include <vector>
#include <unordered_map>
#include "types.hpp"

class Assembler
{
public:
    Assembler();
    ~Assembler();

    // Prebaci na novu sekciju (kreira je ako ne postoji).
    // Snima velicinu prethodne sekcije pre switch-a.
    void switchSection(const std::string &name);

    // Trenutna sekcija
    const std::string &currentSection() const { return current_section_; }

    // Trenutni location counter (offset u trenutnoj sekciji)
    int locationCounter() const { return loc_counter_; }

    // Definisi labelu u trenutnoj sekciji na trenutnom loc_counter.
    // Poziva se iz parser akcije za "IDENT COLON".
    // Radi backpatch: ako simbol vec ima flink listu, prolazi je i popunjava.
    void defineLabel(const std::string &name);

    // Oznaci simbol kao globalni (.global direktiva).
    // Ako simbol ne postoji u tabeli, dodaje ga kao nedefinisan.
    void markGlobal(const std::string &name);

    // Oznaci simbol kao extern (.extern direktiva).
    // Simbol se dodaje kao nedefinisan, is_extern=true.
    void markExtern(const std::string &name);

    // Vraca vrednost simbola (offset u sekciji). -1 ako nedefinisan.
    // Poziva se iz parsera pri referenci na simbol.
    int getSymbolValue(const std::string &name);

    // Da li je simbol definisan?
    bool isSymbolDefined(const std::string &name);

    // Ime sekcije u kojoj je simbol. "undefined" ako extern / nedefinisan.
    std::string getSymbolSection(const std::string &name);

    // Emituj jedan bajt u trenutnu sekciju, loc_counter += 1.
    void emitByte(uint8_t b);

    // Emituj 32-bitnu rec (little-endian) u trenutnu sekciju, loc_counter += 4.
    // Koristi se za .word direktivu.
    void emitWord(uint32_t w);

    // Emituj 4-bajtnu instrukciju u formatu OC|MOD|RegA|RegB|RegC|Disp[11:0].
    // loc_counter += 4.
    // Vraca offset gde je instrukcija smestena (za slucaj da treba patch).
    int emitInstruction(uint8_t oc, uint8_t mod,
                        uint8_t a, uint8_t b, uint8_t c,
                        int16_t disp);

    // Emituj n nula u trenutnu sekciju (.skip direktiva), loc_counter += n.
    void skipBytes(int n);

    // Pristup poslednjem emitovanom bajtu (za patch register polja nakon emit).
    uint8_t getLastByte();
    void updateLastByte(uint8_t b);

    // Pristup poslednjoj 32-bitnoj reci (za patch Disp polja instrukcije).
    uint32_t getLastWord();
    void updateLastWord(uint32_t w);
    // BACKPATCHING

    // Registruj pending patch za simbol koji nije definisan.
    //   symbol  - ime simbola koji se ceka
    //   offset  - bajt offset u trenutnoj sekciji gde treba upisati
    //   action  - '+' (dodaj vrednost) ili '-' (oduzmi)
    //   addend  - konstantni deo koji je vec na mestu (npr. za "sim + 5")
    //   type    - R_ABS_32 / R_PC_REL_12 / R_ABS_12 (odredjuje velicinu)
    //
    // Patch se dodaje u flink listu simbola. Kad se simbol definise
    // (defineLabel), flink lista se prolazi i patch-evi se resavaju.
    void addPatch(const std::string &symbol, int offset,
                  char action, int addend, RelocType type);

    // Poziva se nakon sto parser zavrsi (.end ili EOF).
    // Radi:
    //   1. Snima velicinu poslednje sekcije
    //   2. Prolazi kroz sve simbole i njihove flink liste
    //   3. Za definisane simbole u istoj sekciji -> backpatch (vec uradjeno)
    //   4. Za simbole u drugoj sekciji -> kreira Relocation zapis
    //   5. Za extern simbole -> kreira Relocation zapis
    //   6. Za nedefinisane (niti global, niti extern) -> greska
    void finalize();

    // Upisi izlaznu datoteku u skolskom ELF formatu (PPT Zadatak 9).
    // Sekcije u izlazu:
    //   .symtab - tabela simbola (null + sekcije + lokalni + globalni + extern)
    //   .<ime>  - sadrzaj sekcije za svaku definisanu sekciju
    //   .rela.<ime> - relokacije za sekcije koje ih imaju
    void writeOutput(const std::string &filename);
    //Greske

    bool hasErrors() const { return !errors_.empty(); }
    const std::vector<std::string> &errors() const { return errors_; }

    // Dodaj gresku (parser zove pri sintaksnim/semantickim greskama)
    void addError(const std::string &msg) { errors_.push_back(msg); }

private:
    // Tabela simbola (ime -> Symbol)
    std::unordered_map<std::string, Symbol> symbols_;

    // Sekcije u redosledu definisanja
    std::vector<Section> sections_;

    // Trenutno stanje
    std::string current_section_;
    int loc_counter_;
    int next_section_id_;

    // Greske
    std::vector<std::string> errors_;

    // --- Pomocne metode ---

    // Vrati Symbol po imenu, ili kreiraj novi (nedefinisan) ako ne postoji.
    Symbol &getOrCreateSymbol(const std::string &name);

    // Vrati trenutnu sekciju (nullptr ako nijedna nije aktivna).
    Section *getCurrentSection();

    // Vrati sekciju po imenu, ili kreiraj novu ako ne postoji.
    Section &getOrCreateSection(const std::string &name);

    // Resi jedan patch (backpatch ako simbol definisan u istoj sekciji).
    // Vraca true ako je resen, false ako mora postati relokacija.
    bool resolvePatch(Patch &p);

    // Pretvori patch u relokacioni zapis (simbol se resava u linkeru).
    void patchToRelocation(Patch &p);

    // Dodeli numericke indekse simbolima za .symtab (redosled:
    // 0=null, 1..N sekcije, pa lokalni, pa globalni, pa extern).
    void assignSymbolIndices();

    // Pomoc za ispis .symtab (jedan red)
    void writeSymtabEntry(std::ostream &os, int num, const Symbol &s);
    void writeRelaEntry(std::ostream &os, const Relocation &r);
};

#endif // ASSEMBLER_HPP
