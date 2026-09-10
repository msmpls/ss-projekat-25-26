/*
 * linker.cpp - implementacija linkera
 *
 * Format ulaznog .o fajla (generisan od strane asemblera):
 *
 *   # School ELF v1.0 - SS Project 2025/26
 *   ...
 *
 *   [symtab]
 *   # idx value      type   bind  ndx  name
 *      0 0x00000000  NOTYP   LOC   UND
 *      1 0x00000000   SCTN   LOC     1  text
 *      2 0x00000000   SCTN   LOC     2  data
 *      3 0x00000068  NOTYP   LOC     1  done
 *      ...
 *
 *   [section=text]
 *   # base=0x00000000 size=0x0000006c
 *   0x0000: 2a 00 10 91 00 00 20 92 ...
 *   ...
 *
 *   [rela=text]
 *   # offset type         symbol  addend
 *   0x0004     R_ABS_12    4  (value)               0
 *   ...
 */

#include "../inc/linker.hpp"

#include <iostream>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cstring>

// =========================================================================
// Konstruktor / destruktor
// =========================================================================

Linker::Linker()
    : mode_hex_(false), mode_relocatable_(false) {}

Linker::~Linker() {}

// =========================================================================
// CLI parsiranje
// =========================================================================

bool Linker::parseArgs(int argc, char* argv[])
{
    // Mora biti navedeno bar nešto
    if (argc < 2) {
        addError("Upotreba: linker [opcije] <ulaz1.o> <ulaz2.o> ...");
        addError("Opcije: -o <izlaz>, -place=<sek>@<adresa>, -hex, -relocatable");
        return false;
    }

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];

        if (arg == "-o") {
            if (i + 1 >= argc) {
                addError("Opcija -o zahteva argument");
                return false;
            }
            output_file_ = argv[++i];
        }
        else if (arg == "-hex") {
            mode_hex_ = true;
        }
        else if (arg == "-relocatable") {
            mode_relocatable_ = true;
        }
        else if (arg.substr(0, 7) == "-place=") {
            if (!parsePlaceOption(arg)) {
                return false;
            }
        }
        else if (arg[0] == '-') {
            addError("Nepoznata opcija: " + arg);
            return false;
        }
        else {
            // Ulazni fajl
            input_files_.push_back(arg);
        }
    }

    // Validacija
    if (mode_hex_ && mode_relocatable_) {
        addError("Ne mogu biti navedene i -hex i -relocatable opcije");
        return false;
    }
    if (!mode_hex_ && !mode_relocatable_) {
        addError("Mora biti navedena jedna od -hex ili -relocatable opcija");
        return false;
    }
    if (output_file_.empty()) {
        addError("Nije naveden izlazni fajl (-o)");
        return false;
    }
    if (input_files_.empty()) {
        addError("Nije naveden nijedan ulazni fajl");
        return false;
    }

    return true;
}

bool Linker::parsePlaceOption(const std::string& arg)
{
    // Format: -place=<sek>@<adresa>
    // Primer: -place=text@0x40000000
    std::string rest = arg.substr(7);  // ukloni "-place="

    size_t at_pos = rest.find('@');
    if (at_pos == std::string::npos) {
        addError("Neispravan -place format (nedostaje @): " + arg);
        return false;
    }

    std::string section = rest.substr(0, at_pos);
    std::string addr_str = rest.substr(at_pos + 1);

    if (section.empty() || addr_str.empty()) {
        addError("Neispravan -place format: " + arg);
        return false;
    }

    // std::stoul sa bazom 0 automatski prepoznaje prefiks:
    //   "0x..." → heksadecimalno (baza 16)
    //   "0..."  → oktalno       (baza 8)
    //   "123"   → decimalno     (baza 10)
    // Ovo je najfleksibilnije i najkonsekventnije ponašanje.
    uint32_t address;
    try {
        address = (uint32_t)std::stoul(addr_str, nullptr, 0);
    } catch (const std::exception&) {
        addError("Neispravna adresa u -place opciji: " + addr_str);
        return false;
    }

    place_options_[section] = address;
    return true;
}

// =========================================================================
// Glavni tok rada
// =========================================================================

bool Linker::run()
{
    if (!loadInputFiles()) return false;
    mergeSections();
    if (!mergeSymbols()) return false;
    if (!placeSections()) return false;
    resolveSymbols();
    if (!applyRelocations()) return false;

    if (mode_hex_) {
        if (!checkOverlaps()) return false;
    }

    if (!writeOutput()) return false;

    return true;
}

// =========================================================================
// Korak 1: Učitavanje svih .o fajlova
// =========================================================================

bool Linker::loadInputFiles()
{
    for (size_t i = 0; i < input_files_.size(); i++) {
        InputFile f;
        f.filename = input_files_[i];
        f.id = (int)i;
        input_files_data_.push_back(f);
    }

    for (auto& f : input_files_data_) {
        if (!parseObjectFile(f.filename, f.id)) {
            return false;
        }
    }

    return true;
}

bool Linker::parseObjectFile(const std::string& filename, int file_id)
{
    std::ifstream file(filename);
    if (!file.is_open()) {
        addError("Ne mogu da otvorim fajl: " + filename);
        return false;
    }

    InputFile& input = input_files_data_[file_id];

    std::string line;
    while (std::getline(file, line)) {
        std::string trimmed = trim(line);

        // Preskoči prazne linije i komentare
        if (trimmed.empty() || trimmed[0] == '#') continue;

        if (trimmed == "[symtab]") {
            if (!parseSymtab(file, input)) {
                file.close();
                return false;
            }
        }
        else if (trimmed.substr(0, 9) == "[section=") {
            // [section=text]
            size_t end = trimmed.find(']');
            if (end == std::string::npos) {
                addError("Neispravan format sekcije u " + filename);
                file.close();
                return false;
            }
            std::string section_name = trimmed.substr(9, end - 9);
            if (!parseSection(file, section_name, input)) {
                file.close();
                return false;
            }
        }
        else if (trimmed.substr(0, 6) == "[rela=") {
            // [rela=text]
            size_t end = trimmed.find(']');
            if (end == std::string::npos) {
                addError("Neispravan format rela sekcije u " + filename);
                file.close();
                return false;
            }
            std::string section_name = trimmed.substr(6, end - 6);
            if (!parseRela(file, section_name, input)) {
                file.close();
                return false;
            }
        }
    }

    file.close();

    // Nakon parsiranja, ažuriraj section polje za sve simbole
    // (pošto se symtab parsira PRE sekcija, section polje je prazno)
    for (auto& sym : input.symbols) {
        if (sym.is_section) {
            sym.section = sym.name;
        } else if (sym.is_defined && sym.section_id >= 1) {
            // Nađi sekciju po original_id
            for (auto& s : input.sections) {
                if (s.original_id == sym.section_id) {
                    sym.section = s.name;
                    break;
                }
            }
        }
    }

    return true;
}

bool Linker::parseSymtab(std::ifstream& file, InputFile& input)
{
    std::string line;

    while (std::getline(file, line)) {
        std::string trimmed = trim(line);

        if (trimmed.empty()) continue;
        if (trimmed[0] == '#') continue;
        if (trimmed[0] == '[') {
            // Sledeća sekcija - vratiti liniju u stream
            // Nažalost, ne možemo lako "vratiti" liniju sa ifstream-om
            // pa ćemo parsirati ovu liniju u sledećoj iteraciji glavne petlje
            // Zato vraćamo false u smislu "kraj symtab sekcije"
            // Bolje: probamo sve dok ne naiđemo na novu sekciju
            // Problem: getline je već pročitao ovu liniju.
            // Rešenje: čuvamo ovu liniju u statičkoj promenljivoj ili
            // pozivamo odgovarajući parser ovde.
            if (trimmed.substr(0, 9) == "[section=") {
                size_t end = trimmed.find(']');
                std::string section_name = trimmed.substr(9, end - 9);
                return parseSection(file, section_name, input);
            }
            else if (trimmed.substr(0, 6) == "[rela=") {
                size_t end = trimmed.find(']');
                std::string section_name = trimmed.substr(6, end - 6);
                return parseRela(file, section_name, input);
            }
            // Nepoznata sekcija - prekini
            return true;
        }

        // Parsiranje jednog reda symtab-a
        // Format: <idx> <value> <type> <bind> <ndx> <name>
        // Primer: "   1 0x00000000   SCTN   LOC     1  text"
        std::vector<std::string> tokens = split(trimmed);
        if (tokens.size() < 5) continue;

        InputSymbol sym;
        sym.symtab_index = parseDec(tokens[0]);

        // Value: 0x00000000 ili 0x00000068
        std::string val_str = tokens[1];
        if (val_str.substr(0, 2) == "0x" || val_str.substr(0, 2) == "0X") {
            sym.value = parseHex(val_str.substr(2));
        } else {
            sym.value = parseDec(val_str);
        }

        // Type: NOTYP ili SCTN
        std::string type = tokens[2];
        if (type == "SCTN") sym.is_section = true;

        // Bind: LOC ili GLOB
        std::string bind = tokens[3];
        if (bind == "GLOB") sym.is_global = true;

        // Ndx: UND ili broj
        std::string ndx = tokens[4];
        if (ndx == "UND") {
            sym.section_id = -1;
            sym.section = "undefined";
            sym.is_extern = true;
            sym.is_defined = false;
        } else {
            sym.section_id = parseDec(ndx);
            sym.is_extern = false;
            sym.is_defined = true;  // svi simboli sa Ndx su definisani
        }

        // Name (može biti prazan za null entry)
        if (tokens.size() >= 6) {
            sym.name = tokens[5];
        } else {
            sym.name = "";
        }

        // Ako je simbol section, section je njegovo ime
        if (sym.is_section) {
            sym.section = sym.name;
        } else if (sym.is_defined) {
            // Sekcija simbola - naći po section_id
            for (auto& s : input.sections) {
                if (s.original_id == sym.section_id) {
                    sym.section = s.name;
                    break;
                }
            }
        }

        sym.file_id = input.id;
        input.symbols.push_back(sym);
    }

    return true;
}

bool Linker::parseSection(std::ifstream& file, const std::string& section_name, InputFile& input)
{
    std::string line;

    // Prvo pročitaj header: # base=0x00000000 size=0x0000006c
    int section_size = 0;
    int section_id = -1;

    // Prvo, da nađemo section_id iz symtab-a (sekcija je simbol sa imenom == section_name)
    for (auto& sym : input.symbols) {
        if (sym.is_section && sym.name == section_name) {
            section_id = sym.section_id;
            break;
        }
    }

    while (std::getline(file, line)) {
        std::string trimmed = trim(line);

        if (trimmed.empty()) continue;
        if (trimmed[0] == '#') {
            // Header linija - parsiraj size
            size_t size_pos = trimmed.find("size=");
            if (size_pos != std::string::npos) {
                std::string size_str = trimmed.substr(size_pos + 5);
                size_t space = size_str.find(' ');
                if (space != std::string::npos) size_str = size_str.substr(0, space);
                if (size_str.substr(0, 2) == "0x") {
                    section_size = parseHex(size_str.substr(2));
                } else {
                    section_size = parseDec(size_str);
                }
            }
            continue;
        }
        if (trimmed[0] == '[') {
            // Sledeća sekcija - proveri da li je trenutna već sačuvana
            bool found = false;
            for (auto& s : input.sections) {
                if (s.name == section_name && s.file_id == input.id) {
                    found = true;
                    // Ažuriraj size ako je potrebno
                    if (s.size == 0) s.size = section_size;
                    break;
                }
            }
            if (!found) {
                // Sekcija nije kreirana (npr. prazna sekcija bez bajtova)
                InputSection isec;
                isec.name = section_name;
                isec.file_id = input.id;
                isec.original_id = section_id;
                isec.size = section_size;
                input.sections.push_back(isec);
            }

            if (trimmed.substr(0, 9) == "[section=") {
                size_t end = trimmed.find(']');
                std::string new_section = trimmed.substr(9, end - 9);
                return parseSection(file, new_section, input);
            }
            else if (trimmed.substr(0, 6) == "[rela=") {
                size_t end = trimmed.find(']');
                std::string new_section = trimmed.substr(6, end - 6);
                return parseRela(file, new_section, input);
            }
            return true;
        }

        // Parsiranje heksadecimalnih bajtova
        // Format: 0x0000: 2a 00 10 91 00 00 20 92 ...
        // Prvo nađi ":" - sve pre je offset, sve posle su bajtovi
        size_t colon = trimmed.find(':');
        if (colon == std::string::npos) continue;

        std::string bytes_part = trimmed.substr(colon + 1);
        std::istringstream iss(bytes_part);
        std::string byte_str;
        while (iss >> byte_str) {
            // byte_str je heksadecimalni bez 0x prefiksa (npr. "2a", "00")
            int b = parseHex(byte_str);
            // Dodaj u odgovarajuću sekciju (nađi po imenu)
            for (auto& s : input.sections) {
                if (s.name == section_name && s.file_id == input.id) {
                    s.bytes.push_back((uint8_t)b);
                    goto next_byte;
                }
            }
            // Ako sekcija još ne postoji, kreiraj je
            {
                InputSection isec;
                isec.name = section_name;
                isec.file_id = input.id;
                isec.original_id = section_id;
                isec.size = section_size;
                isec.bytes.push_back((uint8_t)b);
                input.sections.push_back(isec);
            }
            next_byte:;
        }
    }

    // Ako sekcija nije kreirana (npr. prazna), kreiraj je
    bool found = false;
    for (auto& s : input.sections) {
        if (s.name == section_name && s.file_id == input.id) {
            found = true;
            break;
        }
    }
    if (!found) {
        InputSection isec;
        isec.name = section_name;
        isec.file_id = input.id;
        isec.original_id = section_id;
        isec.size = section_size;
        input.sections.push_back(isec);
    }

    return true;
}

bool Linker::parseRela(std::ifstream& file, const std::string& section_name, InputFile& input)
{
    std::string line;

    while (std::getline(file, line)) {
        std::string trimmed = trim(line);

        if (trimmed.empty()) continue;
        if (trimmed[0] == '#') continue;
        if (trimmed[0] == '[') {
            // Sledeća sekcija
            if (trimmed.substr(0, 9) == "[section=") {
                size_t end = trimmed.find(']');
                std::string new_section = trimmed.substr(9, end - 9);
                return parseSection(file, new_section, input);
            }
            else if (trimmed.substr(0, 6) == "[rela=") {
                size_t end = trimmed.find(']');
                std::string new_section = trimmed.substr(6, end - 6);
                return parseRela(file, new_section, input);
            }
            return true;
        }

        // Parsiranje relokacije
        // Format: 0x0004  R_ABS_12  4  (value)  0
        // Tokeni: ["0x0004", "R_ABS_12", "4", "(value)", "0"]
        std::vector<std::string> tokens = split(trimmed);
        if (tokens.size() < 4) continue;

        Relocation r;
        // Offset (hex sa 0x)
        std::string off_str = tokens[0];
        if (off_str.substr(0, 2) == "0x" || off_str.substr(0, 2) == "0X") {
            r.offset = parseHex(off_str.substr(2));
        } else {
            r.offset = parseDec(off_str);
        }

        // Tip
        std::string type_str = tokens[1];
        if (type_str == "R_ABS_32") r.type = RelocType::R_ABS_32;
        else if (type_str == "R_PC_REL_12") r.type = RelocType::R_PC_REL_12;
        else if (type_str == "R_ABS_12") r.type = RelocType::R_ABS_12;
        else {
            addError("Nepoznat tip relokacije: " + type_str);
            continue;
        }

        // Symbol index
        r.symbol_index = parseDec(tokens[2]);

        // Symbol name (u zagradi)
        if (tokens.size() >= 4 && tokens[3][0] == '(') {
            std::string name = tokens[3];
            // Ukloni zagrade
            if (name.front() == '(') name = name.substr(1);
            if (name.back() == ')') name = name.substr(0, name.size() - 1);
            r.symbol_name = name;
        } else {
            r.symbol_name = "";
        }

        // Addend (zadnji token)
        r.addend = parseDec(tokens.back());

        // Dodaj u odgovarajuću sekciju
        for (auto& s : input.sections) {
            if (s.name == section_name && s.file_id == input.id) {
                s.relocations.push_back(r);
                break;
            }
        }
    }

    return true;
}

// =========================================================================
// Korak 2: Agregacija sekcija
// =========================================================================

void Linker::mergeSections()
{
    // Prolaz kroz sve ulazne fajlove, redom pojavljivanja
    for (auto& f : input_files_data_) {
        for (auto& s : f.sections) {
            MergedSection* ms = getMergedSection(s.name);

            // Zabilježi da ovaj fajl doprinosi deo počevši od trenutne veličine ms
            ms->parts.push_back({f.id, s.size, ms->size});

            // Nadoveži bajtove
            for (uint8_t b : s.bytes) {
                ms->bytes.push_back(b);
            }

            // Ažuriraj ukupnu veličinu
            ms->size += s.size;

            // Kopiraj relokacije (sa file_id informacijom koja nam treba kasnije)
            for (auto& r : s.relocations) {
                ms->relocations.push_back(r);
            }
        }
    }
}

MergedSection* Linker::getMergedSection(const std::string& name)
{
    for (auto& s : merged_sections_) {
        if (s.name == name) return &s;
    }
    MergedSection ms;
    ms.name = name;
    merged_sections_.push_back(std::move(ms));
    return &merged_sections_.back();
}

// =========================================================================
// Korak 3: Spajanje tabela simbola
// =========================================================================

bool Linker::mergeSymbols()
{
    // Prolaz kroz sve ulazne fajlove
    for (auto& f : input_files_data_) {
        for (auto& sym : f.symbols) {
            // Preskoči null entry (symtab_index == 0)
            if (sym.symtab_index == 0) continue;

            // Sekcije dodajemo kao simbole
            if (sym.is_section) {
                // Da li već postoji sekcija sa ovim imenom?
                bool exists = false;
                for (auto& ms : merged_symbols_) {
                    if (ms.name == sym.name && ms.is_section) {
                        exists = true;
                        break;
                    }
                }
                if (exists) continue;

                MergedSymbol new_sym;
                new_sym.name = sym.name;
                new_sym.is_section = true;
                new_sym.is_defined = true;
                new_sym.is_global = false;
                new_sym.section = sym.name;
                merged_symbols_.push_back(new_sym);
                continue;
            }

            // Extern simboli (nedefinisan, globalan)
            if (sym.is_extern || !sym.is_defined) {
                // Da li je već razrešen (postoji definicija)?
                bool resolved = false;
                for (auto& ms : merged_symbols_) {
                    if (ms.name == sym.name && ms.is_defined) {
                        resolved = true;
                        break;
                    }
                }
                if (!resolved) {
                    // Dodaj kao extern ako već ne postoji nikakav entry
                    bool exists = false;
                    for (auto& ms : merged_symbols_) {
                        if (ms.name == sym.name) {
                            exists = true;
                            break;
                        }
                    }
                    if (!exists) {
                        MergedSymbol new_sym;
                        new_sym.name = sym.name;
                        new_sym.is_extern = true;
                        new_sym.is_defined = false;
                        new_sym.is_global = true;
                        merged_symbols_.push_back(new_sym);
                        // Dodaj u symbol_index_ mapu (ključ = ime, jer je extern uvek globalan)
                        symbol_index_[sym.name] = (int)merged_symbols_.size() - 1;
                    }
                }
                // Ako je resolved, ne radimo ništa - referenca je razrešena
                continue;
            }

            // Definisani simbol (lokalni ili globalni)
            // Proveri multi-definiciju samo za globalne
            if (sym.is_global) {
                for (auto& ms : merged_symbols_) {
                    if (ms.name == sym.name && ms.is_defined && !ms.is_extern) {
                        addError("Višestruka definicija simbola: " + sym.name);
                        return false;
                    }
                }
            }

            // Ako već postoji extern entry sa istim imenom, modifikuj ga in-place.
            // (Ne sme se koristiti erase jer pomeri indekse svih kasnijih elemenata,
            //  a symbol_index_ mapa zadržava stare indekse.)
            bool updated_existing = false;
            if (sym.is_global) {
                for (auto& ms : merged_symbols_) {
                    if (ms.name == sym.name && ms.is_extern && !ms.is_defined) {
                        ms.is_extern = false;
                        ms.is_defined = true;
                        ms.is_global = true;
                        ms.section = sym.section;
                        updated_existing = true;
                        break;
                    }
                }
            }

            if (!updated_existing) {
                // Dodaj simbol u merged tabelu.
                // final_address se izračunava kasnije u resolveSymbols().
                // symtab_index se dodeljuje u writeRelocatableOutput().
                MergedSymbol new_sym;
                new_sym.name = sym.name;
                new_sym.is_global = sym.is_global;
                new_sym.is_defined = true;
                new_sym.is_extern = false;
                new_sym.section = sym.section;

                merged_symbols_.push_back(new_sym);

                // Za pretragu simbola u applyRelocations() i resolveSymbols():
                //   - globalni simboli su vidljivi u svim fajlovima → ključ je samo ime
                //   - lokalni simboli su vidljivi samo u svom fajlu → ključ je "file_id:name"
                if (sym.is_global) {
                    symbol_index_[sym.name] = (int)merged_symbols_.size() - 1;
                } else {
                    std::string key = std::to_string(sym.file_id) + ":" + sym.name;
                    symbol_index_[key] = (int)merged_symbols_.size() - 1;
                }
            }
        }
    }

    return true;
}

// =========================================================================
// Korak 4: Smeštanje sekcija na adrese
// =========================================================================

bool Linker::placeSections()
{
    // Po postavci (strana 10):
    //   -hex         → sekcije idu od 0 ili sa -place, guraju se ka višim adresama
    //   -relocatable → sve sekcije idu od nulte adrese, -place se IGNORIŠE

    if (mode_relocatable_) {
        // Relocatable mod: sve sekcije od 0, redom pojavljivanja
        uint32_t addr = 0;
        for (auto& ms : merged_sections_) {
            ms.address = addr;
            addr += ms.size;
        }
        return true;
    }

    // Hex mod: poštuje -place opcije

    // Prvo dodeli adrese sekcijama koje imaju -place
    for (auto& kv : place_options_) {
        const std::string& section_name = kv.first;
        uint32_t address = kv.second;

        for (auto& ms : merged_sections_) {
            if (ms.name == section_name) {
                ms.address = address;
                break;
            }
        }
    }

    // Sada dodeli adrese ostalim sekcijama
    // Pravilo: smeštaju se odmah iza sekcije sa najvišom adresom

    // Nađi maksimalnu krajnju adresu među sekcijama sa -place
    uint32_t max_end = 0;
    for (auto& ms : merged_sections_) {
        if (place_options_.find(ms.name) != place_options_.end()) {
            uint32_t end = ms.address + ms.size;
            if (end > max_end) max_end = end;
        }
    }

    // Dodeli adrese sekcijama bez -place, redom pojavljivanja
    for (auto& ms : merged_sections_) {
        if (place_options_.find(ms.name) == place_options_.end()) {
            ms.address = max_end;
            max_end += ms.size;
        }
    }

    return true;
}

// =========================================================================
// Korak 5: Razrešavanje simbola (finalne adrese)
// =========================================================================

void Linker::resolveSymbols()
{
    // Za svaki definisan simbol, izračunaj finalnu adresu
    // final_address = section.address + part.merged_offset + symbol.offset

    for (auto& f : input_files_data_) {
        for (auto& sym : f.symbols) {
            if (!sym.is_defined || sym.is_section) continue;

            // Pronađi merged sekciju po imenu
            MergedSection* ms = nullptr;
            for (auto& m : merged_sections_) {
                if (m.name == sym.section) {
                    ms = &m;
                    break;
                }
            }
            if (!ms) continue;

            // Pronađi part_offset (gde u merged sekciji počinje deo iz ovog fajla)
            int part_offset = getPartOffset(ms, f.id);

            // final_address = section.address + part_offset + symbol.offset
            uint32_t final_addr = ms->address + part_offset + sym.value;

            // Ažuriraj odgovarajući MergedSymbol preko symbol_index_ mape
            // (globalni: ključ = ime; lokalni: ključ = "file_id:ime")
            std::string key = sym.is_global
                ? sym.name
                : std::to_string(sym.file_id) + ":" + sym.name;

            auto it = symbol_index_.find(key);
            if (it != symbol_index_.end()) {
                merged_symbols_[it->second].final_address = final_addr;
            }
        }
    }
}

int Linker::getPartOffset(MergedSection* ms, int file_id)
{
    if (!ms) return 0;
    for (auto& part : ms->parts) {
        if (part.file_id == file_id) {
            return part.merged_offset;
        }
    }
    return 0;
}

// =========================================================================
// Korak 6: Primena relokacija
// =========================================================================

bool Linker::applyRelocations()
{
    // Za svaku merged sekciju, primeni relokacije
    for (auto& ms : merged_sections_) {
        // ms.relocations sadrži sve relokacije iz ulaznih fajlova
        // Svaka relokacija ima: offset (unutar originalne sekcije), type, symbol_name, addend
        // Ali ne znamo iz kog fajla dolazi - MORAMO da pratimo file_id!

        // Problem: u mergeSections() smo kopirali relokacije bez file_id informacije
        // Rešenje: idemo kroz ulazne fajlove i njihove sekcije

        for (auto& f : input_files_data_) {
            for (auto& s : f.sections) {
                if (s.name != ms.name) continue;

                // Pronađi part_offset za ovaj fajl u ovoj sekciji
                int part_offset = getPartOffset(&ms, f.id);

                for (auto& r : s.relocations) {
                    // Pronađi simbol po imenu - prvo u globalnim, pa u lokalnim
                    MergedSymbol* sym = nullptr;

                    // Pokušaj globalno
                    auto it = symbol_index_.find(r.symbol_name);
                    if (it != symbol_index_.end()) {
                        sym = &merged_symbols_[it->second];
                    } else {
                        // Pokušaj kao lokalni u ovom fajlu
                        std::string key = std::to_string(f.id) + ":" + r.symbol_name;
                        it = symbol_index_.find(key);
                        if (it != symbol_index_.end()) {
                            sym = &merged_symbols_[it->second];
                        }
                    }

                    if (!sym || !sym->is_defined) {
                        // Nerazrešen simbol:
                        // - U -hex modu: greška (finalna memorija mora biti kompletna)
                        // - U -relocatable modu: legitimno - preneti kao pending relokacija
                        if (mode_hex_) {
                            addError("Nerazrešen simbol: " + r.symbol_name);
                        } else {
                            // Sačuvaj kao pending za prenos u izlaz
                            MergedSection::PendingRelocation pr;
                            pr.part_offset = part_offset;
                            pr.original_offset = r.offset;
                            pr.file_id = f.id;
                            pr.type = r.type;
                            pr.symbol_name = r.symbol_name;
                            pr.addend = r.addend;
                            ms.pending_relocations.push_back(pr);
                        }
                        continue;
                    }

                    // S = adresa simbola (uint32_t - može biti i do 0xFFFFFFFF)
                    uint32_t S = sym->final_address;
                    // A = addend (int32_t - može biti negativan)
                    int32_t A = r.addend;
                    // P = adresa gde se primenjuje relokacija (u merged sekciji)
                    // P = ms.address + part_offset + r.offset
                    uint32_t P = ms.address + part_offset + r.offset;

                    // Pozicija u ms.bytes[] gde treba upisati
                    size_t byte_pos = (size_t)part_offset + (size_t)r.offset;

                    if (r.type == RelocType::R_ABS_32) {
                        // 32-bitna apsolutna adresa: value = S + A
                        // Koristimo uint32_t aritmetiku (wrap-around je definisan ponašanje)
                        uint32_t value = S + (uint32_t)A;

                        if (byte_pos + 4 > ms.bytes.size()) {
                            addError("Relokacija van opsega sekcije");
                            continue;
                        }

                        // Little-endian
                        ms.bytes[byte_pos + 0] = (uint8_t)(value & 0xFF);
                        ms.bytes[byte_pos + 1] = (uint8_t)((value >> 8) & 0xFF);
                        ms.bytes[byte_pos + 2] = (uint8_t)((value >> 16) & 0xFF);
                        ms.bytes[byte_pos + 3] = (uint8_t)((value >> 24) & 0xFF);
                    }
                    else if (r.type == RelocType::R_ABS_12) {
                        // 12-bitni apsolutni pomeraj u Disp polju instrukcije
                        // value = S + A (kao signed 32-bitni rezultat)
                        int32_t value = (int32_t)(S + (uint32_t)A);

                        // Proveri 12-bitni opseg (signed: -2048 do 2047)
                        if (value < -2048 || value > 2047) {
                            addError("R_ABS_12 vrednost " + std::to_string(value) +
                                     " van 12-bitnog opsega za simbol " + r.symbol_name);
                            continue;
                        }

                        if (byte_pos + 4 > ms.bytes.size()) {
                            addError("Relokacija van opsega sekcije");
                            continue;
                        }

                        // Pročitaj instrukciju (little-endian)
                        uint32_t instr = 0;
                        instr |= ms.bytes[byte_pos + 0];
                        instr |= (uint32_t)ms.bytes[byte_pos + 1] << 8;
                        instr |= (uint32_t)ms.bytes[byte_pos + 2] << 16;
                        instr |= (uint32_t)ms.bytes[byte_pos + 3] << 24;

                        // Modifikuj Disp polje (bits 11-0) - value je int32, ali nas zanima samo donjih 12 bita
                        instr = (instr & 0xFFFFF000) | ((uint32_t)value & 0xFFF);

                        // Upisati nazad (little-endian)
                        ms.bytes[byte_pos + 0] = (uint8_t)(instr & 0xFF);
                        ms.bytes[byte_pos + 1] = (uint8_t)((instr >> 8) & 0xFF);
                        ms.bytes[byte_pos + 2] = (uint8_t)((instr >> 16) & 0xFF);
                        ms.bytes[byte_pos + 3] = (uint8_t)((instr >> 24) & 0xFF);
                    }
                    else if (r.type == RelocType::R_PC_REL_12) {
                        // 12-bitni PC-relativni pomeraj
                        // value = S + A - P (kao signed 32-bitni rezultat)
                        int32_t value = (int32_t)(S + (uint32_t)A - P);

                        // Proveri 12-bitni opseg
                        if (value < -2048 || value > 2047) {
                            addError("R_PC_REL_12 vrednost " + std::to_string(value) +
                                     " van 12-bitnog opsega za simbol " + r.symbol_name);
                            continue;
                        }

                        if (byte_pos + 4 > ms.bytes.size()) {
                            addError("Relokacija van opsega sekcije");
                            continue;
                        }

                        // Pročitaj instrukciju
                        uint32_t instr = 0;
                        instr |= ms.bytes[byte_pos + 0];
                        instr |= (uint32_t)ms.bytes[byte_pos + 1] << 8;
                        instr |= (uint32_t)ms.bytes[byte_pos + 2] << 16;
                        instr |= (uint32_t)ms.bytes[byte_pos + 3] << 24;

                        // Modifikuj Disp polje
                        instr = (instr & 0xFFFFF000) | ((uint32_t)value & 0xFFF);

                        // Upisati nazad
                        ms.bytes[byte_pos + 0] = (uint8_t)(instr & 0xFF);
                        ms.bytes[byte_pos + 1] = (uint8_t)((instr >> 8) & 0xFF);
                        ms.bytes[byte_pos + 2] = (uint8_t)((instr >> 16) & 0xFF);
                        ms.bytes[byte_pos + 3] = (uint8_t)((instr >> 24) & 0xFF);
                    }
                }
            }
        }
    }

    return !hasErrors();
}

// =========================================================================
// Korak 7: Detekcija preklapanja sekcija (samo za -hex)
// =========================================================================

bool Linker::checkOverlaps()
{
    // Za -hex mod, sekcije moraju da se ne preklapaju
    // (relokacije su već primenjene i adrese su konačne)

    // Specijalno: ako dve sekcije imaju istu adresu i oba su alocirane,
    // to je greška. Takođe, ako se sekcije seku.

    for (size_t i = 0; i < merged_sections_.size(); i++) {
        for (size_t j = i + 1; j < merged_sections_.size(); j++) {
            MergedSection& a = merged_sections_[i];
            MergedSection& b = merged_sections_[j];

            uint32_t a_start = a.address;
            uint32_t a_end = a.address + a.size;
            uint32_t b_start = b.address;
            uint32_t b_end = b.address + b.size;

            // Preklapanje ako: a_start < b_end && b_start < a_end
            if (a_start < b_end && b_start < a_end) {
                addError("Preklapanje sekcija '" + a.name + "' [" +
                         std::to_string(a_start) + "-" + std::to_string(a_end) +
                         ") i '" + b.name + "' [" +
                         std::to_string(b_start) + "-" + std::to_string(b_end) + ")");
            }
        }
    }

    return !hasErrors();
}

// =========================================================================
// Korak 8: Generisanje izlaza
// =========================================================================

bool Linker::writeOutput()
{
    if (mode_hex_) {
        writeHexOutput(output_file_);
    } else if (mode_relocatable_) {
        writeRelocatableOutput(output_file_);
    }
    return true;
}

void Linker::writeHexOutput(const std::string& filename)
{
    std::ofstream out(filename);
    if (!out.is_open()) {
        addError("Ne mogu da otvorim izlazni fajl: " + filename);
        return;
    }

    // Generiši hex format: adresa: bajt bajt bajt... (8 bajta po redu)
    // Samo adrese koje imaju sadržaj se ispisuju

    // Skupi sve (adresa, bajt) parove
    std::vector<std::pair<uint32_t, uint8_t>> mem;

    for (auto& ms : merged_sections_) {
        for (size_t i = 0; i < ms.bytes.size(); i++) {
            mem.push_back({ms.address + i, ms.bytes[i]});
        }
    }

    // Sortiraj po adresi
    std::sort(mem.begin(), mem.end(),
              [](const std::pair<uint32_t, uint8_t>& a,
                 const std::pair<uint32_t, uint8_t>& b) {
                  return a.first < b.first;
              });

    // Ispiši 8 bajta po redu, ali prekini liniju ako adrese nisu uzastopne
    // (npr. između sekcija koje su na različitim adresama)
    size_t i = 0;
    while (i < mem.size()) {
        // Početak reda
        out << std::hex << std::setfill('0') << std::setw(4) << mem[i].first << ":";

        // 8 bajta u redu (ili manje ako dođemo do kraja ili do prekida)
        uint32_t expected_addr = mem[i].first;
        for (size_t j = 0; j < 8 && i < mem.size(); j++, i++) {
            // Ako adresa nije uzastopna (ima gap), prekini liniju
            if (mem[i].first != expected_addr) {
                break;
            }
            out << " " << std::setw(2) << (int)mem[i].second;
            expected_addr++;
        }
        out << "\n";
    }

    out.close();
}

void Linker::writeRelocatableOutput(const std::string& filename)
{
    std::ofstream out(filename);
    if (!out.is_open()) {
        addError("Ne mogu da otvorim izlazni fajl: " + filename);
        return;
    }

    out << "# School ELF v1.0 - SS Project 2025/26\n";
    out << "# Linker output (relocatable)\n\n";

    // Dodeli symtab indekse
    int idx = 1;  // 0 = null
    for (auto& ms : merged_symbols_) {
        if (ms.is_section) ms.symtab_index = idx++;
    }
    for (auto& ms : merged_symbols_) {
        if (ms.is_section) continue;
        if (!ms.is_global) ms.symtab_index = idx++;
    }
    for (auto& ms : merged_symbols_) {
        if (ms.is_section) continue;
        if (ms.is_global && ms.is_defined) ms.symtab_index = idx++;
    }
    for (auto& ms : merged_symbols_) {
        if (ms.is_section) continue;
        if (ms.is_extern) ms.symtab_index = idx++;
    }

    // Pomoćna funkcija za formatiranje simbol entry-ja
    auto writeEntry = [&](int sym_idx, uint32_t value, const std::string& type,
                          const std::string& bind, const std::string& ndx,
                          const std::string& name) {
        std::ostringstream ss;
        ss << std::dec << std::setw(4) << sym_idx << " ";
        ss << "0x" << std::hex << std::setfill('0') << std::setw(8) << value << std::dec << std::setfill(' ');
        ss << "  " << std::setw(5) << type;
        ss << "  " << std::setw(4) << bind;
        ss << "  " << std::setw(4) << ndx;
        ss << "  " << name << "\n";
        out << ss.str();
    };

    // --- symtab ---
    out << "[symtab]\n";
    out << "# idx value      type   bind  ndx  name\n";

    // Null entry
    writeEntry(0, 0, "NOTYP", "LOC", "UND", "");

    // Sekcije
    int sec_id = 1;
    for (auto& ms : merged_symbols_) {
        if (!ms.is_section) continue;
        writeEntry(ms.symtab_index, 0, "SCTN", "LOC",
                  std::to_string(sec_id), ms.name);
        sec_id++;
    }

    // Lokalni
    for (auto& ms : merged_symbols_) {
        if (ms.is_section || ms.is_global) continue;
        int ndx = -1;
        int sid = 1;
        for (auto& s : merged_symbols_) {
            if (s.is_section) {
                if (s.name == ms.section) { ndx = sid; break; }
                sid++;
            }
        }
        writeEntry(ms.symtab_index, ms.is_defined ? ms.final_address : 0,
                  "NOTYP", "LOC",
                  ndx >= 0 ? std::to_string(ndx) : "UND",
                  ms.name);
    }

    // Globalni definisani
    for (auto& ms : merged_symbols_) {
        if (ms.is_section || !ms.is_global || !ms.is_defined) continue;
        int ndx = -1;
        int sid = 1;
        for (auto& s : merged_symbols_) {
            if (s.is_section) {
                if (s.name == ms.section) { ndx = sid; break; }
                sid++;
            }
        }
        writeEntry(ms.symtab_index, ms.final_address, "NOTYP", "GLOB",
                  ndx >= 0 ? std::to_string(ndx) : "UND",
                  ms.name);
    }

    // Extern
    for (auto& ms : merged_symbols_) {
        if (ms.is_section || !ms.is_extern) continue;
        writeEntry(ms.symtab_index, 0, "NOTYP", "GLOB", "UND", ms.name);
    }
    out << "\n";

    // --- Sadrzaj sekcija ---
    // Redosled: pojavljivanje u merged_sections_
    for (auto& ms : merged_sections_) {
        out << "[section=" << ms.name << "]\n";
        std::ostringstream hdr;
        hdr << "# base=0x" << std::hex << std::setfill('0') << std::setw(8) << ms.address
            << " size=0x" << std::setw(8) << ms.size << std::dec << "\n";
        out << hdr.str();

        // Heksadecimalni dump - 16 bajta po redu
        for (size_t i = 0; i < ms.bytes.size(); i += 16) {
            std::ostringstream row;
            row << "0x" << std::hex << std::setfill('0') << std::setw(4) << i << ": ";
            for (size_t j = 0; j < 16 && (i + j) < ms.bytes.size(); j++) {
                row << std::setfill('0') << std::setw(2) << std::hex
                    << (int)ms.bytes[i + j] << " ";
            }
            row << "\n";
            out << row.str();
        }
        out << "\n";
    }

    // --- Relokacije ---
    // Za -relocatable, nerazrešene relokacije (extern simboli) se prenose dalje.
    // Razrešene relokacije su već primenjene u applyRelocations().
    // Pending relokacije imaju original_offset (u originalnoj sekciji) + part_offset
    // (gde taj part počinje u merged sekciji) = finalni offset u merged sekciji.

    for (auto& ms : merged_sections_) {
        if (ms.pending_relocations.empty()) continue;

        out << "[rela=" << ms.name << "]\n";
        out << "# offset type         symbol  addend\n";

        for (auto& pr : ms.pending_relocations) {
            // final_offset = part_offset + original_offset (u merged sekciji)
            int final_offset = pr.part_offset + pr.original_offset;

            // Nađi symtab_index simbola u novom .o koristeći symbol_index_ mapu.
            // Pošto su pending relokacije uvek ka nerazrešenim extern/global simbolima,
            // ključ je samo ime (bez "file_id:" prefiksa koji lokalni simboli imaju).
            // Ovo garantuje da nećemo pogrešno match-ovati lokalni simbol iz drugog fajla.
            int sym_idx = -1;
            auto it = symbol_index_.find(pr.symbol_name);
            if (it != symbol_index_.end()) {
                sym_idx = merged_symbols_[it->second].symtab_index;
            }

            // Tip relokacije kao string
            std::string type_str;
            switch (pr.type) {
                case RelocType::R_ABS_32:    type_str = "R_ABS_32";    break;
                case RelocType::R_PC_REL_12: type_str = "R_PC_REL_12"; break;
                case RelocType::R_ABS_12:    type_str = "R_ABS_12";    break;
            }

            std::ostringstream row;
            row << "0x" << std::hex << std::setfill('0') << std::setw(4)
                << static_cast<unsigned int>(final_offset) << std::dec << std::setfill(' ')
                << "  " << std::setw(11) << type_str
                << "  " << std::setw(3) << sym_idx
                << "  " << std::setw(20) << std::left << ("(" + pr.symbol_name + ")")
                << std::right << "  " << pr.addend << "\n";
            out << row.str();
        }
        out << "\n";
    }

    out.close();
}

// =========================================================================
// Pomocne metode
// =========================================================================

std::string Linker::trim(const std::string& s)
{
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

std::vector<std::string> Linker::split(const std::string& s)
{
    std::vector<std::string> tokens;
    std::istringstream iss(s);
    std::string tok;
    while (iss >> tok) {
        tokens.push_back(tok);
    }
    return tokens;
}

bool Linker::isHexDigit(char c)
{
    return (c >= '0' && c <= '9') ||
           (c >= 'a' && c <= 'f') ||
           (c >= 'A' && c <= 'F');
}

uint32_t Linker::parseHex(const std::string& s)
{
    uint32_t result = 0;
    for (char c : s) {
        result *= 16;
        if (c >= '0' && c <= '9') result += c - '0';
        else if (c >= 'a' && c <= 'f') result += c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') result += c - 'A' + 10;
    }
    return result;
}

int32_t Linker::parseDec(const std::string& s)
{
    int32_t result = 0;
    bool negative = false;
    size_t i = 0;
    if (i < s.size() && s[i] == '-') {
        negative = true;
        i++;
    }
    for (; i < s.size(); i++) {
        if (s[i] < '0' || s[i] > '9') break;
        result = result * 10 + (s[i] - '0');
    }
    return negative ? -result : result;
}

// =========================================================================
// main - ulazna tacka linkera
// =========================================================================

int main(int argc, char* argv[])
{
    Linker linker;

    if (!linker.parseArgs(argc, argv)) {
        std::cerr << "Greske pri parsiranju argumenata:\n";
        for (auto& e : linker.errors()) {
            std::cerr << "  " << e << "\n";
        }
        return 1;
    }

    if (!linker.run()) {
        std::cerr << "Greske pri linkovanju:\n";
        for (auto& e : linker.errors()) {
            std::cerr << "  " << e << "\n";
        }
        return 1;
    }

    if (linker.hasErrors()) {
        std::cerr << "Greske pri linkovanju:\n";
        for (auto& e : linker.errors()) {
            std::cerr << "  " << e << "\n";
        }
        return 1;
    }

    return 0;
}