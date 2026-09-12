#ifndef LINKER_HPP
#define LINKER_HPP

/*
 * linker.hpp - interfejs linkera nezavisan od ciljne arhitekture
 *
 * Linker radi sledece:
 *   1. Ucitava jedan ili vise .o fajlova (skolski ELF tekstualni format)
 *   2. Agregira istoimene sekcije iz svih fajlova
 *   3. Smešta sekcije na adrese (default redom od 0, ili sa -place)
 *   4. Spaja tabele simbola, razrešava extern reference
 *   5. Primena relokacija - upisuje finalne adrese u kod
 *   6. Generiše izlaz:
 *        -hex         → format adresa: bajt bajt... za inicijalizaciju memorije
 *
 * CLI: linker [opcije] <ulaz1.o> <ulaz2.o> ...
 *   -o <izlaz>                 naziv izlazne datoteke
 *   -place=<sek>@<adresa>      eksplicitna adresa sekcije (može više puta)
 *   -hex                       generiši hex zapis
 *
 * Pažnja: -hex opcija je obavezna.
 */

#include "types.hpp"
#include <string>
#include <vector>
#include <unordered_map>

class Linker {
public:
    Linker();
    ~Linker();

    // ---- CLI parsiranje ----
    bool parseArgs(int argc, char* argv[]);

    // ---- Glavni tok rada ----
    bool run();   // poziva sve korake redom, vraća false ako ima greška

    // ---- Greske ----
    bool hasErrors() const { return !errors_.empty(); }
    const std::vector<std::string>& errors() const { return errors_; }

private:
    // ---- Konfiguracija (iz CLI) ----
    std::string output_file_;
    std::vector<std::string> input_files_;
    std::unordered_map<std::string, uint32_t> place_options_;  // sekcija → adresa
    bool mode_hex_;

    // ---- Unutrasnje strukture ----
    std::vector<InputFile>     input_files_data_;
    std::vector<MergedSection> merged_sections_;
    std::vector<MergedSymbol>  merged_symbols_;

    // Mapa: ime simbola → indeks u merged_symbols_
    std::unordered_map<std::string, int> symbol_index_;

    // Greske
    std::vector<std::string> errors_;

    // ---- Koraci ----

    // 1. Učitavanje svih .o fajlova
    bool loadInputFiles();

    // 2. Agregacija sekcija
    void mergeSections();

    // 3. Spajanje tabela simbola (detekcija multi-definicija)
    bool mergeSymbols();

    // 4. Smeštanje sekcija na adrese
    bool placeSections();

    // 5. Razrešavanje simbola (finalne adrese)
    void resolveSymbols();

    // 6. Primena relokacija
    bool applyRelocations();

    // 7. Detekcija preklapanja sekcija (samo za -hex)
    bool checkOverlaps();

    // 8. Generisanje izlaza
    bool writeOutput();

    // ---- Pomocne metode ----

    // Parsiranje jednog .o fajla
    bool parseObjectFile(const std::string& filename, int file_id);

    // Pronađi spojenu sekciju po imenu (ili kreiraj novu)
    MergedSection* getMergedSection(const std::string& name);

    // Pronađi part_offset (gde u merged sekciji počinje deo iz datog fajla)
    int getPartOffset(MergedSection* ms, int file_id);

    // Izlaz za -hex mod
    void writeHexOutput(const std::string& filename);

    // Parsiranje -place=<sek>@<adresa> argumenta
    bool parsePlaceOption(const std::string& arg);

    // Pomocno za parsiranje .o fajla
    bool parseSymtab(std::ifstream& file, InputFile& input);
    bool parseSection(std::ifstream& file, const std::string& section_name, InputFile& input);
    bool parseRela(std::ifstream& file, const std::string& section_name, InputFile& input);

    // Pomocno za skip blank linija i komentara
    static std::string trim(const std::string& s);
    static std::vector<std::string> split(const std::string& s);
    static bool isHexDigit(char c);
    static uint32_t parseHex(const std::string& s);
    static int32_t parseDec(const std::string& s);

    // Dodaj gresku
    void addError(const std::string& msg) { errors_.push_back(msg); }
};

#endif // LINKER_HPP