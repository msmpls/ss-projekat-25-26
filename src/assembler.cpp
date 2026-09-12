/*
 * assembler.cpp - implementacija jednoprolaznog asemblera
 *
 * Tok rada:
 *   1. Parser tokom parsiranja zove metode ove klase.
 *   2. Pri referenci na nedefinisan simbol, emituju se nule i registruje patch.
 *   3. Pri definiciji simbola (defineLabel), flink lista se prolazi i popunjava.
 *   4. Na kraju (finalize()):
 *        - preostali patch-evi postaju relokacije (extern / druga sekcija)
 *        - nedefinisani simboli (niti global, niti extern) -> greska
 *   5. writeOutput() upisuje skolski ELF tekstualni format.
 */

#include "../inc/assembler.hpp"

#include <fstream>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <algorithm>
#include <cstring>

//Konstruktor destrutor
Assembler::Assembler()
    : current_section_(""), loc_counter_(0), next_section_id_(1)
{
}

Assembler::~Assembler()
{
}

//Pomocne metode
Symbol& Assembler::getOrCreateSymbol(const std::string& name)
{
    auto it = symbols_.find(name);
    if (it == symbols_.end()) {
        Symbol s;
        s.name = name;
        symbols_.emplace(name, std::move(s));
        return symbols_.at(name);
    }
    return it->second;
}

Section* Assembler::getCurrentSection()
{
    if (current_section_.empty()) return nullptr;
    for (auto& s : sections_) {
        if (s.name == current_section_) return &s;
    }
    return nullptr;
}

Section& Assembler::getOrCreateSection(const std::string& name)
{
    for (auto& s : sections_) {
        if (s.name == name) return s;
    }
    Section s;
    s.name = name;
    s.id = next_section_id_++;
    s.base = 0;
    s.length = 0;
    sections_.push_back(std::move(s));
    return sections_.back();
}

//Sekcije

void Assembler::switchSection(const std::string& name)
{
    // Snimi velicinu prethodne sekcije (po stvarnom broju bajtova, ne loc_counter)
    // Ovo je bitno jer loc_counter moze biti manji ako smo tek prebacili na sekciju
    // a nismo emitovali nista. bytes.size() je uvek tacan.
    if (!current_section_.empty()) {
        for (auto& s : sections_) {
            if (s.name == current_section_) {
                s.length = s.bytes.size();
                break;
            }
        }
    }

    // Prebaci na novu sekciju (kreiraj ako ne postoji)
    Section& sec = getOrCreateSection(name);

    // Ako je nova sekcija, dodaj je u symbol table kao SCTN
    Symbol& sym = getOrCreateSymbol(name);
    if (!sym.is_defined) {
        sym.is_defined = true;
        sym.is_section  = true;
        sym.section     = name;
        sym.section_id  = sec.id;
        sym.value       = 0;  // sekcije imaju value 0 (bazna adresa)
    }

    current_section_ = name;
    // Nastavi od gde smo stali - ovo omogucava AGREGACIJU sekcija:
    // ako se vracamo na vec postojecu sekciju, loc_counter mora da nastavi
    // od trenutnog broja bajtova u njoj, ne od 0.
    loc_counter_ = sec.bytes.size();
}

//Simboli

void Assembler::defineLabel(const std::string& name)
{
    if (current_section_.empty()) {
        addError("Label '" + name + "' defined outside any section");
        return;
    }

    Symbol& sym = getOrCreateSymbol(name);

    if (sym.is_defined) {
        addError("Symbol '" + name + "' redefined");
        return;
    }

    sym.is_defined = true;
    sym.value      = loc_counter_;
    sym.section    = current_section_;
    sym.section_id = getCurrentSection()->id;

    // Backpatch: prodji kroz flink listu i resi sve pending patch-eve
    std::vector<Patch> remaining;
    for (auto& p : sym.flink) {
        if (!resolvePatch(p)) {
            // Patch se ne moze resiti unutar asemblera (simbol u drugoj sekciji?)
            // Zadrzavamo ga za finalize() da postane relokacija.
            remaining.push_back(p);
        }
    }
    sym.flink = std::move(remaining);
}

void Assembler::markGlobal(const std::string& name)
{
    Symbol& sym = getOrCreateSymbol(name);
    sym.is_global = true;
    // Ne postavljamo is_defined = false; simbol moze biti definisan kasnije.
}

void Assembler::markExtern(const std::string& name)
{
    Symbol& sym = getOrCreateSymbol(name);
    sym.is_extern  = true;
    sym.is_global  = true;  // extern je uvek globalni
    sym.is_defined = false;
    sym.section    = "undefined";
    sym.section_id = -1;
}

int Assembler::getSymbolValue(const std::string& name)
{
    auto it = symbols_.find(name);
    if (it == symbols_.end()) return -1;
    if (!it->second.is_defined) return -1;
    return it->second.value;
}

bool Assembler::isSymbolDefined(const std::string& name)
{
    auto it = symbols_.find(name);
    return it != symbols_.end() && it->second.is_defined;
}

std::string Assembler::getSymbolSection(const std::string& name)
{
    auto it = symbols_.find(name);
    if (it == symbols_.end()) return "undefined";
    if (!it->second.is_defined) return "undefined";
    return it->second.section;
}

//Emitovanje koda
void Assembler::emitByte(uint8_t b)
{
    Section* sec = getCurrentSection();
    if (!sec) {
        addError("emitByte: no current section");
        return;
    }
    sec->bytes.push_back(b);
    loc_counter_++;
}

void Assembler::emitWord(uint32_t w)
{
    // Little-endian: LSB prvi
    emitByte(static_cast<uint8_t>(w & 0xFF));
    emitByte(static_cast<uint8_t>((w >> 8) & 0xFF));
    emitByte(static_cast<uint8_t>((w >> 16) & 0xFF));
    emitByte(static_cast<uint8_t>((w >> 24) & 0xFF));
}

int Assembler::emitInstruction(uint8_t oc, uint8_t mod,
                               uint8_t a, uint8_t b, uint8_t c,
                               int16_t disp)
{
    Section* sec = getCurrentSection();
    if (!sec) {
        addError("emitInstruction: no current section");
        return -1;
    }

    int offset = loc_counter_;

    uint32_t instr = 0;
    instr |= (static_cast<uint32_t>(oc)  & 0xF) << 28;
    instr |= (static_cast<uint32_t>(mod) & 0xF) << 24;
    instr |= (static_cast<uint32_t>(a)   & 0xF) << 20;
    instr |= (static_cast<uint32_t>(b)   & 0xF) << 16;
    instr |= (static_cast<uint32_t>(c)   & 0xF) << 12;
    instr |= (static_cast<uint32_t>(disp) & 0xFFF);

    emitWord(instr);
    return offset;
}

void Assembler::skipBytes(int n)
{
    if (n < 0) {
        addError(".skip with negative size");
        return;
    }
    for (int i = 0; i < n; i++) {
        emitByte(0x00);
    }
}

uint8_t Assembler::getLastByte()
{
    Section* sec = getCurrentSection();
    if (!sec || sec->bytes.empty()) return 0;
    return sec->bytes.back();
}

void Assembler::updateLastByte(uint8_t b)
{
    Section* sec = getCurrentSection();
    if (!sec || sec->bytes.empty()) return;
    sec->bytes.back() = b;
}

uint32_t Assembler::getLastWord()
{
    Section* sec = getCurrentSection();
    if (!sec || sec->bytes.size() < 4) return 0;
    uint32_t w = 0;
    size_t n = sec->bytes.size();
    w |= static_cast<uint32_t>(sec->bytes[n - 4]);
    w |= static_cast<uint32_t>(sec->bytes[n - 3]) << 8;
    w |= static_cast<uint32_t>(sec->bytes[n - 2]) << 16;
    w |= static_cast<uint32_t>(sec->bytes[n - 1]) << 24;
    return w;
}

void Assembler::updateLastWord(uint32_t w)
{
    Section* sec = getCurrentSection();
    if (!sec || sec->bytes.size() < 4) return;
    size_t n = sec->bytes.size();
    sec->bytes[n - 4] = static_cast<uint8_t>(w & 0xFF);
    sec->bytes[n - 3] = static_cast<uint8_t>((w >> 8) & 0xFF);
    sec->bytes[n - 2] = static_cast<uint8_t>((w >> 16) & 0xFF);
    sec->bytes[n - 1] = static_cast<uint8_t>((w >> 24) & 0xFF);
}

//Backpatching
void Assembler::addPatch(const std::string& symbol, int offset,
                         char action, int addend, RelocType type)
{
    Symbol& sym = getOrCreateSymbol(symbol);

    Patch p;
    p.symbol_name = symbol;
    p.section     = current_section_;
    p.offset      = offset;
    p.action      = action;
    p.addend      = addend;
    p.type        = type;

    sym.flink.push_back(p);
}

bool Assembler::resolvePatch(Patch& p)
{
    // Pronadji simbol na koji se patch odnosi
    auto it = symbols_.find(p.symbol_name);
    if (it == symbols_.end() || !it->second.is_defined) {
        // Simbol nije definisan -> patch ostaje u flink listi (postace relokacija)
        return false;
    }

    Symbol& sym = it->second;

    // Ako je simbol extern, ne mozemo da resimo ovde -> relokacija
    if (sym.is_extern) {
        return false;
    }

    // Ako simbol nije u istoj sekciji kao patch -> relokacija
    if (sym.section != p.section) {
        return false;
    }

    // Za R_ABS_32: uvek proslediti linkeru kao relokaciju.
    // Razlog: asembler ne zna finalnu adresu sekcije (base), pa ne može
    // da upiše ispravnu 32-bitnu adresu. Backpatch bi upisao lokalni offset,
    // što je pogrešno kad linker pomeri sekciju.
    // (R_PC_REL_12 i R_ABS_12 se mogu backpatch-ovati jer su relativni/pomeraji.)
    if (p.type == RelocType::R_ABS_32) {
        return false;
    }

    // Simbol je u istoj sekciji -> backpatch (upisi vrednost direktno)
    int sym_val = sym.value;

    // Pronadji sekciju u kojoj je patch
    Section* sec = nullptr;
    for (auto& s : sections_) {
        if (s.name == p.section) { sec = &s; break; }
    }
    if (!sec) return false;

    // Izracunaj vrednost koju treba upisati
    int value;
    if (p.action == '+') {
        value = sym_val + p.addend;
    } else {  // '-'
        value = p.addend - sym_val;
    }

    // Upisi na odgovarajuci nacin (zavisi od tipa relokacije)
    if (p.type == RelocType::R_ABS_32) {
        // 4 bajta, little-endian
        if (p.offset + 4 > (int)sec->bytes.size()) {
            addError("Patch offset out of bounds for R_ABS_32");
            return true;
        }
        uint32_t v = static_cast<uint32_t>(value);
        sec->bytes[p.offset + 0] = static_cast<uint8_t>(v & 0xFF);
        sec->bytes[p.offset + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);
        sec->bytes[p.offset + 2] = static_cast<uint8_t>((v >> 16) & 0xFF);
        sec->bytes[p.offset + 3] = static_cast<uint8_t>((v >> 24) & 0xFF);
    }
    else if (p.type == RelocType::R_PC_REL_12 || p.type == RelocType::R_ABS_12) {
        // 12-bitni Disp u okviru 32-bitne instrukcije
        if (p.offset + 4 > (int)sec->bytes.size()) {
            addError("Patch offset out of bounds for 12-bit reloc");
            return true;
        }

        // PC-relativno: value = sym_val - (instr_addr + 4)
        int16_t disp;
        if (p.type == RelocType::R_PC_REL_12) {
            disp = static_cast<int16_t>(sym_val - (p.offset + 4));
            if (p.action == '-') disp = -disp;  // redak slucaj
        } else {
            // R_ABS_12: vrednost simbola kao apsolutni pomeraj
            disp = static_cast<int16_t>(value);
        }

        // Proveri 12-bitno ogranicenje (oznaceni: -2048 do 2047)
        if (disp < -2048 || disp > 2047) {
            addError("Displacement " + std::to_string(disp) +
                     " out of 12-bit signed range for symbol '" + p.symbol_name + "'");
            return true;
        }

        // Upisi u Disp polje (nizih 12 bita instrukcije)
        uint32_t instr = 0;
        instr |= static_cast<uint32_t>(sec->bytes[p.offset + 0]);
        instr |= static_cast<uint32_t>(sec->bytes[p.offset + 1]) << 8;
        instr |= static_cast<uint32_t>(sec->bytes[p.offset + 2]) << 16;
        instr |= static_cast<uint32_t>(sec->bytes[p.offset + 3]) << 24;

        instr = (instr & 0xFFFFF000) | (disp & 0xFFF);

        sec->bytes[p.offset + 0] = static_cast<uint8_t>(instr & 0xFF);
        sec->bytes[p.offset + 1] = static_cast<uint8_t>((instr >> 8) & 0xFF);
        sec->bytes[p.offset + 2] = static_cast<uint8_t>((instr >> 16) & 0xFF);
        sec->bytes[p.offset + 3] = static_cast<uint8_t>((instr >> 24) & 0xFF);
    }

    return true;  // patch resen
}

void Assembler::patchToRelocation(Patch& p)
{
    // Patch se ne moze resiti unutar asemblera -> postaje relokacija
    // (simbol je extern ili u drugoj sekciji)

    // Pronadji sekciju u kojoj je patch
    Section* sec = nullptr;
    for (auto& s : sections_) {
        if (s.name == p.section) { sec = &s; break; }
    }
    if (!sec) {
        addError("Cannot find section '" + p.section + "' for patch");
        return;
    }

    Relocation r;
    r.offset       = p.offset;
    r.type         = p.type;
    r.symbol_name  = p.symbol_name;
    r.symbol_index = -1;  // dodeljuje se u finalize() posle assignSymbolIndices()
    r.addend       = p.addend;

    sec->relocations.push_back(r);
}

//Finalizacija
void Assembler::finalize()
{
    // 1. Snimi velicinu poslednje sekcije
    if (!current_section_.empty()) {
        for (auto& s : sections_) {
            if (s.name == current_section_) {
                s.length = loc_counter_;
                break;
            }
        }
    }

    // 2. Prolazak kroz sve simbole i njihove preostale flink liste
    for (auto& kv : symbols_) {
        Symbol& sym = kv.second;

        for (auto& p : sym.flink) {
            // Ako simbol nije definisan i nije extern -> greska
            if (!sym.is_defined && !sym.is_extern) {
                addError("Undefined symbol: " + sym.name);
                continue;
            }

            // Ako simbol nije definisan ali je extern -> relokacija
            // Ako simbol jeste definisan ali u drugoj sekciji -> relokacija
            patchToRelocation(p);
        }

        // Ocisti flink listu (vise nije potrebna)
        sym.flink.clear();
    }

    // 3. Dodeli indekse simbolima za .symtab
    assignSymbolIndices();

    // 4. Popuni symbol_index za sve relokacije (sada kad su indeksi dodeljeni)
    for (auto& sec : sections_) {
        for (auto& r : sec.relocations) {
            auto it = symbols_.find(r.symbol_name);
            if (it != symbols_.end()) {
                r.symbol_index = it->second.symtab_index;
            } else {
                addError("Relocation references unknown symbol: " + r.symbol_name);
            }
        }
    }
}

void Assembler::assignSymbolIndices()
{
    // Redosled u .symtab:
    //   0: null entry (rezervisano)
    //   1..N: sekcije (u redosledu definisanja)
    //   N+1..: lokalni simboli (nije sekcija, nije global)
    //   zatim: globalni definisani simboli
    //   zatim: extern (global, nedefinisan) simboli

    int idx = 1;  // 0 je null

    // Sekcije
    for (auto& s : sections_) {
        auto it = symbols_.find(s.name);
        if (it != symbols_.end()) {
            it->second.symtab_index = idx++;
        }
    }

    // Lokalni (non-section, non-global) simboli
    for (auto& kv : symbols_) {
        Symbol& s = kv.second;
        if (s.is_section) continue;
        if (s.is_global) continue;
        s.symtab_index = idx++;
    }

    // Globalni definisani
    for (auto& kv : symbols_) {
        Symbol& s = kv.second;
        if (s.is_section) continue;
        if (!s.is_global) continue;
        if (s.is_extern) continue;  // extern su posebno
        if (!s.is_defined) continue;
        s.symtab_index = idx++;
    }

    // Extern (global, nedefinisan)
    for (auto& kv : symbols_) {
        Symbol& s = kv.second;
        if (s.is_section) continue;
        if (!s.is_extern) continue;
        s.symtab_index = idx++;
    }
}

//Izlaz - writeout Skolski elf format
static std::string relocTypeStr(RelocType t)
{
    switch (t) {
        case RelocType::R_ABS_32:    return "R_ABS_32";
        case RelocType::R_PC_REL_12: return "R_PC_REL_12";
        case RelocType::R_ABS_12:    return "R_ABS_12";
    }
    return "R_UNKNOWN";
}

static std::string bindStr(const Symbol& s)
{
    if (s.is_global) return "GLOB";
    return "LOC";
}

static std::string typeStr(const Symbol& s)
{
    if (s.is_section) return "SCTN";
    return "NOTYP";
}

static std::string ndxStr(const Symbol& s)
{
    if (s.is_extern || !s.is_defined) return "UND";
    if (s.is_section) return std::to_string(s.section_id);
    if (s.section_id >= 0) return std::to_string(s.section_id);
    return "ABS";
}

void Assembler::writeSymtabEntry(std::ostream& os, int num, const Symbol& s)
{
    os << std::dec << std::setw(4) << num << " "
       << "0x" << std::setfill('0') << std::setw(8) << std::hex
       << (s.is_defined ? static_cast<unsigned int>(s.value) : 0u)
       << std::dec << std::setfill(' ')
       << "  " << std::setw(5) << typeStr(s)
       << "  " << std::setw(4) << bindStr(s)
       << "  " << std::setw(4) << ndxStr(s)
       << "  " << s.name << "\n";
}

void Assembler::writeRelaEntry(std::ostream& os, const Relocation& r)
{
    os << "0x" << std::setfill('0') << std::setw(4) << std::hex
       << static_cast<unsigned int>(r.offset)
       << std::dec << std::setfill(' ')
       << "  " << std::setw(11) << relocTypeStr(r.type)
       << "  " << std::setw(3) << r.symbol_index
       << "  " << std::setw(20) << std::left << ("(" + r.symbol_name + ")")
       << std::right << "  " << r.addend << "\n";
}

void Assembler::writeOutput(const std::string& filename)
{
    std::ofstream out(filename);
    if (!out.is_open()) {
        addError("Cannot open output file: " + filename);
        return;
    }

    out << "# School ELF v1.0 - SS Project 2025/26\n";
    out << "# Assembler output (single-pass, with backpatching)\n\n";

    // --- symtab ---
    out << "[symtab]\n";
    out << "# idx value      type   bind  ndx  name\n";

    // Null entry
    Symbol null_sym;
    null_sym.name = "";
    writeSymtabEntry(out, 0, null_sym);

    // Sekcije (redosled definisanja)
    for (auto& s : sections_) {
        auto it = symbols_.find(s.name);
        if (it != symbols_.end()) {
            writeSymtabEntry(out, it->second.symtab_index, it->second);
        }
    }

    // Lokalni simboli (redosled ubacivanja u mapu je proizvoljan, ali konzistentan)
    for (auto& kv : symbols_) {
        Symbol& s = kv.second;
        if (s.is_section) continue;
        if (s.is_global) continue;
        writeSymtabEntry(out, s.symtab_index, s);
    }

    // Globalni definisani
    for (auto& kv : symbols_) {
        Symbol& s = kv.second;
        if (s.is_section) continue;
        if (!s.is_global) continue;
        if (s.is_extern) continue;
        if (!s.is_defined) continue;
        writeSymtabEntry(out, s.symtab_index, s);
    }

    // Extern
    for (auto& kv : symbols_) {
        Symbol& s = kv.second;
        if (s.is_section) continue;
        if (!s.is_extern) continue;
        writeSymtabEntry(out, s.symtab_index, s);
    }
    out << "\n";

    // --- Sadrzaj sekcija ---
    for (auto& s : sections_) {
        out << "[section=" << s.name << "]\n";
        out << "# base=0x" << std::setfill('0') << std::setw(8) << std::hex << s.base
            << " size=0x" << std::setw(8) << s.length << std::dec << "\n";

        // Heksadecimalni dump - 16 bajta po redu
        for (size_t i = 0; i < s.bytes.size(); i += 16) {
            out << "0x" << std::setfill('0') << std::setw(4) << std::hex << i << ": ";
            for (size_t j = 0; j < 16 && (i + j) < s.bytes.size(); j++) {
                out << std::setfill('0') << std::setw(2) << std::hex
                    << static_cast<int>(s.bytes[i + j]) << " ";
            }
            out << "\n";
        }
        out << std::dec;
        out << "\n";
    }

    // --- Relokacije ---
    for (auto& s : sections_) {
        if (s.relocations.empty()) continue;

        out << "[rela=" << s.name << "]\n";
        out << "# offset type         symbol  addend\n";
        for (auto& r : s.relocations) {
            writeRelaEntry(out, r);
        }
        out << "\n";
    }

    out.close();
}