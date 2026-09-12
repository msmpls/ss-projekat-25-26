/*
 * linker.cpp - implementacija linkera (samo za -hex mod)
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
    : mode_hex_(false) {}

Linker::~Linker() {}

// =========================================================================
// CLI parsiranje
// =========================================================================

bool Linker::parseArgs(int argc, char* argv[])
{
    if (argc < 2) {
        addError("Upotreba: linker [opcije] <ulaz1.o> <ulaz2.o> ...");
        addError("Opcije: -o <izlaz>, -place=<sek>@<adresa>, -hex");
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
            input_files_.push_back(arg);
        }
    }

    // Validacija
    if (!mode_hex_) {
        addError("Mora biti navedena -hex opcija");
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
    if (!checkOverlaps()) return false;
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

        if (trimmed.empty() || trimmed[0] == '#') continue;

        if (trimmed == "[symtab]") {
            if (!parseSymtab(file, input)) {
                file.close();
                return false;
            }
        }
        else if (trimmed.substr(0, 9) == "[section=") {
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

    // Ažuriraj section polje za simbole
    for (auto& sym : input.symbols) {
        if (sym.is_section) {
            sym.section = sym.name;
        } else if (sym.is_defined && sym.section_id >= 1) {
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
            return true;
        }

        std::vector<std::string> tokens = split(trimmed);
        if (tokens.size() < 5) continue;

        InputSymbol sym;
        sym.symtab_index = parseDec(tokens[0]);

        std::string val_str = tokens[1];
        if (val_str.substr(0, 2) == "0x" || val_str.substr(0, 2) == "0X") {
            sym.value = parseHex(val_str.substr(2));
        } else {
            sym.value = parseDec(val_str);
        }

        std::string type = tokens[2];
        if (type == "SCTN") sym.is_section = true;

        std::string bind = tokens[3];
        if (bind == "GLOB") sym.is_global = true;

        std::string ndx = tokens[4];
        if (ndx == "UND") {
            sym.section_id = -1;
            sym.section = "undefined";
            sym.is_extern = true;
            sym.is_defined = false;
        } else {
            sym.section_id = parseDec(ndx);
            sym.is_extern = false;
            sym.is_defined = true;
        }

        if (tokens.size() >= 6) {
            sym.name = tokens[5];
        } else {
            sym.name = "";
        }

        if (sym.is_section) {
            sym.section = sym.name;
        }

        sym.file_id = input.id;
        input.symbols.push_back(sym);
    }

    return true;
}

bool Linker::parseSection(std::ifstream& file, const std::string& section_name, InputFile& input)
{
    std::string line;
    int section_size = 0;
    int section_id = -1;

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
            bool found = false;
            for (auto& s : input.sections) {
                if (s.name == section_name && s.file_id == input.id) {
                    found = true;
                    if (s.size == 0) s.size = section_size;
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

        size_t colon = trimmed.find(':');
        if (colon == std::string::npos) continue;

        std::string bytes_part = trimmed.substr(colon + 1);
        std::istringstream iss(bytes_part);
        std::string byte_str;
        while (iss >> byte_str) {
            int b = parseHex(byte_str);
            for (auto& s : input.sections) {
                if (s.name == section_name && s.file_id == input.id) {
                    s.bytes.push_back((uint8_t)b);
                    goto next_byte;
                }
            }
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

        std::vector<std::string> tokens = split(trimmed);
        if (tokens.size() < 4) continue;

        Relocation r;
        std::string off_str = tokens[0];
        if (off_str.substr(0, 2) == "0x" || off_str.substr(0, 2) == "0X") {
            r.offset = parseHex(off_str.substr(2));
        } else {
            r.offset = parseDec(off_str);
        }

        std::string type_str = tokens[1];
        if (type_str == "R_ABS_32") r.type = RelocType::R_ABS_32;
        else if (type_str == "R_PC_REL_12") r.type = RelocType::R_PC_REL_12;
        else if (type_str == "R_ABS_12") r.type = RelocType::R_ABS_12;
        else {
            addError("Nepoznat tip relokacije: " + type_str);
            continue;
        }

        r.symbol_index = parseDec(tokens[2]);

        if (tokens.size() >= 4 && tokens[3][0] == '(') {
            std::string name = tokens[3];
            if (name.front() == '(') name = name.substr(1);
            if (name.back() == ')') name = name.substr(0, name.size() - 1);
            r.symbol_name = name;
        } else {
            r.symbol_name = "";
        }

        r.addend = parseDec(tokens.back());

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
    for (auto& f : input_files_data_) {
        for (auto& s : f.sections) {
            MergedSection* ms = getMergedSection(s.name);
            ms->parts.push_back({f.id, s.size, ms->size});
            for (uint8_t b : s.bytes) {
                ms->bytes.push_back(b);
            }
            ms->size += s.size;
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
    for (auto& f : input_files_data_) {
        for (auto& sym : f.symbols) {
            if (sym.symtab_index == 0) continue;

            if (sym.is_section) {
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

            if (sym.is_extern || !sym.is_defined) {
                bool resolved = false;
                for (auto& ms : merged_symbols_) {
                    if (ms.name == sym.name && ms.is_defined && ms.is_global) {
                        resolved = true;
                        break;
                    }
                }
                if (!resolved) {
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
                        symbol_index_[sym.name] = (int)merged_symbols_.size() - 1;
                    }
                }
                continue;
            }

            if (sym.is_global) {
                for (auto& ms : merged_symbols_) {
                    if (ms.name == sym.name && ms.is_defined && !ms.is_extern && ms.is_global) {
                        addError("Višestruka definicija simbola: " + sym.name);
                        return false;
                    }
                }
            }

            bool updated_existing = false;
            if (sym.is_global) {
                for (auto& ms : merged_symbols_) {
                    if (ms.name == sym.name && ms.is_extern && !ms.is_defined && ms.is_global) {
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
                MergedSymbol new_sym;
                new_sym.name = sym.name;
                new_sym.is_global = sym.is_global;
                new_sym.is_defined = true;
                new_sym.is_extern = false;
                new_sym.section = sym.section;

                merged_symbols_.push_back(new_sym);

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
    // Hex mod: poštuje -place opcije
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

    uint32_t max_end = 0;
    for (auto& ms : merged_sections_) {
        if (place_options_.find(ms.name) != place_options_.end()) {
            uint32_t end = ms.address + ms.size;
            if (end > max_end) max_end = end;
        }
    }

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
    for (auto& f : input_files_data_) {
        for (auto& sym : f.symbols) {
            if (!sym.is_defined || sym.is_section) continue;

            MergedSection* ms = nullptr;
            for (auto& m : merged_sections_) {
                if (m.name == sym.section) {
                    ms = &m;
                    break;
                }
            }
            if (!ms) continue;

            int part_offset = getPartOffset(ms, f.id);

            uint32_t final_addr = ms->address + part_offset + sym.value;

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
    for (auto& ms : merged_sections_) {
        for (auto& f : input_files_data_) {
            for (auto& s : f.sections) {
                if (s.name != ms.name) continue;

                int part_offset = getPartOffset(&ms, f.id);

                for (auto& r : s.relocations) {
                    MergedSymbol* sym = nullptr;

                    auto it = symbol_index_.find(r.symbol_name);
                    if (it != symbol_index_.end()) {
                        sym = &merged_symbols_[it->second];
                    } else {
                        std::string key = std::to_string(f.id) + ":" + r.symbol_name;
                        it = symbol_index_.find(key);
                        if (it != symbol_index_.end()) {
                            sym = &merged_symbols_[it->second];
                        }
                    }

                    if (!sym || !sym->is_defined) {
                        addError("Nerazrešen simbol: " + r.symbol_name);
                        continue;
                    }

                    uint32_t S = sym->final_address;
                    int32_t A = r.addend;
                    uint32_t P = ms.address + part_offset + r.offset;

                    size_t byte_pos = (size_t)part_offset + (size_t)r.offset;

                    if (r.type == RelocType::R_ABS_32) {
                        uint32_t value = S + (uint32_t)A;
                        if (byte_pos + 4 > ms.bytes.size()) {
                            addError("Relokacija van opsega sekcije");
                            continue;
                        }
                        ms.bytes[byte_pos + 0] = (uint8_t)(value & 0xFF);
                        ms.bytes[byte_pos + 1] = (uint8_t)((value >> 8) & 0xFF);
                        ms.bytes[byte_pos + 2] = (uint8_t)((value >> 16) & 0xFF);
                        ms.bytes[byte_pos + 3] = (uint8_t)((value >> 24) & 0xFF);
                    }
                    else if (r.type == RelocType::R_ABS_12) {
                        int32_t value = (int32_t)(S + (uint32_t)A);
                        if (value < -2048 || value > 2047) {
                            addError("R_ABS_12 vrednost " + std::to_string(value) +
                                     " van 12-bitnog opsega za simbol " + r.symbol_name);
                            continue;
                        }
                        if (byte_pos + 4 > ms.bytes.size()) {
                            addError("Relokacija van opsega sekcije");
                            continue;
                        }
                        uint32_t instr = 0;
                        instr |= ms.bytes[byte_pos + 0];
                        instr |= (uint32_t)ms.bytes[byte_pos + 1] << 8;
                        instr |= (uint32_t)ms.bytes[byte_pos + 2] << 16;
                        instr |= (uint32_t)ms.bytes[byte_pos + 3] << 24;

                        instr = (instr & 0xFFFFF000) | ((uint32_t)value & 0xFFF);

                        ms.bytes[byte_pos + 0] = (uint8_t)(instr & 0xFF);
                        ms.bytes[byte_pos + 1] = (uint8_t)((instr >> 8) & 0xFF);
                        ms.bytes[byte_pos + 2] = (uint8_t)((instr >> 16) & 0xFF);
                        ms.bytes[byte_pos + 3] = (uint8_t)((instr >> 24) & 0xFF);
                    }
                    else if (r.type == RelocType::R_PC_REL_12) {
                        int32_t value = (int32_t)(S + (uint32_t)A - P);
                        if (value < -2048 || value > 2047) {
                            addError("R_PC_REL_12 vrednost " + std::to_string(value) +
                                     " van 12-bitnog opsega za simbol " + r.symbol_name);
                            continue;
                        }
                        if (byte_pos + 4 > ms.bytes.size()) {
                            addError("Relokacija van opsega sekcije");
                            continue;
                        }
                        uint32_t instr = 0;
                        instr |= ms.bytes[byte_pos + 0];
                        instr |= (uint32_t)ms.bytes[byte_pos + 1] << 8;
                        instr |= (uint32_t)ms.bytes[byte_pos + 2] << 16;
                        instr |= (uint32_t)ms.bytes[byte_pos + 3] << 24;

                        instr = (instr & 0xFFFFF000) | ((uint32_t)value & 0xFFF);

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
// Korak 7: Detekcija preklapanja sekcija
// =========================================================================

bool Linker::checkOverlaps()
{
    for (size_t i = 0; i < merged_sections_.size(); i++) {
        for (size_t j = i + 1; j < merged_sections_.size(); j++) {
            MergedSection& a = merged_sections_[i];
            MergedSection& b = merged_sections_[j];

            uint32_t a_start = a.address;
            uint32_t a_end = a.address + a.size;
            uint32_t b_start = b.address;
            uint32_t b_end = b.address + b.size;

            if (a_start < b_end && b_start < a_end) {
                addError("Preklapanje sekcija '" + a.name + "' i '" + b.name + "'");
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

    std::vector<std::pair<uint32_t, uint8_t>> mem;

    for (auto& ms : merged_sections_) {
        for (size_t i = 0; i < ms.bytes.size(); i++) {
            mem.push_back({ms.address + i, ms.bytes[i]});
        }
    }

    std::sort(mem.begin(), mem.end(),
              [](const std::pair<uint32_t, uint8_t>& a,
                 const std::pair<uint32_t, uint8_t>& b) {
                  return a.first < b.first;
              });

    size_t i = 0;
    while (i < mem.size()) {
        out << std::hex << std::setfill('0') << std::setw(4) << mem[i].first << ":";

        uint32_t expected_addr = mem[i].first;
        for (size_t j = 0; j < 8 && i < mem.size(); j++, i++) {
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