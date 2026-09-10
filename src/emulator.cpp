/*
 * emulator.cpp - implementacija emulatora (Nivo A)
 *
 * Format instrukcije (postavka strana 13):
 *   bits 31-28: OC | bits 27-24: MOD
 *   bits 23-20: RegA | bits 19-16: RegB | bits 15-12: RegC
 *   bits 11-0 : Disp (oznaceni 12-bitni pomeraj)
 *
 * Mapa opkodova (strane 14-16):
 *   0x0 halt (MOD=0) / iret (MOD=1)
 *   0x1 int (softverski prekid)
 *   0x2 call
 *   0x3 jmp / beq / bne / bgt
 *   0x4 xchg
 *   0x5 add / sub / mul / div
 *   0x6 not / and / or / xor
 *   0x7 shl / shr
 *   0x8 st
 *   0x9 ld (i csrrd/csrwr preko MOD)
 *
 * VAŽNO: Tokom izvršavanja instrukcije, gpr[15] (pc) se prvo pomeri na pc+4
 * (sledeća instrukcija). Ovo omogućava da PC-relativne instrukcije (jmp, call,
 * beq, ...) koriste gpr[A] (gde je A=15) i dobiju tačan rezultat, jer asembler
 * računa Disp kao: cilj - (adresa_instrukcije + 4).
 */

#include "../inc/emulator.hpp"

#include <iostream>
#include <fstream>
#include <sstream>
#include <iomanip>

// =========================================================================
// Konstruktor / destruktor
// =========================================================================

Emulator::Emulator()
{
    // Inicijalizacija registara
    for (int i = 0; i < 16; i++) gpr_[i] = 0;
    for (int i = 0; i < 3; i++) csr_[i] = 0;
    // pc se postavlja u loadHex() na adresu prvog bajta u hex fajlu.
    // (Po postavci procesor kreće od 0x40000000, ali naš linker smešta
    // sekcije počevši od 0 ili sa -place, pa pratimo hex fajl.)
    gpr_[15] = 0;
}

Emulator::~Emulator() {}

// =========================================================================
// Pristup memoriji
// =========================================================================

uint8_t Emulator::readByte(uint32_t addr)
{
    auto it = memory_.find(addr);
    if (it == memory_.end()) return 0;  // neinicijalizovana memorija = 0
    return it->second;
}

void Emulator::writeByte(uint32_t addr, uint8_t val)
{
    memory_[addr] = val;
}

uint32_t Emulator::readWord(uint32_t addr)
{
    // Little-endian: LSB na nižoj adresi
    uint32_t w = 0;
    w |= (uint32_t)readByte(addr);
    w |= (uint32_t)readByte(addr + 1) << 8;
    w |= (uint32_t)readByte(addr + 2) << 16;
    w |= (uint32_t)readByte(addr + 3) << 24;
    return w;
}

void Emulator::writeWord(uint32_t addr, uint32_t val)
{
    writeByte(addr,     (uint8_t)(val & 0xFF));
    writeByte(addr + 1, (uint8_t)((val >> 8)  & 0xFF));
    writeByte(addr + 2, (uint8_t)((val >> 16) & 0xFF));
    writeByte(addr + 3, (uint8_t)((val >> 24) & 0xFF));
}

// =========================================================================
// Pristup registrima
// =========================================================================

uint32_t Emulator::getGPR(int idx) const
{
    if (idx == 0) return 0;  // r0 je uvek 0 (ožičen)
    if (idx < 0 || idx >= 16) return 0;
    return gpr_[idx];
}

void Emulator::setGPR(int idx, uint32_t val)
{
    if (idx == 0) return;  // pisanje u r0 se ignoriše
    if (idx < 0 || idx >= 16) return;
    gpr_[idx] = val;
}

uint32_t Emulator::getCSR(int idx) const
{
    if (idx < 0 || idx >= 3) return 0;
    return csr_[idx];
}

void Emulator::setCSR(int idx, uint32_t val)
{
    if (idx < 0 || idx >= 3) return;
    csr_[idx] = val;
}

// =========================================================================
// Učitavanje hex fajla
// =========================================================================

bool Emulator::loadHex(const std::string& filename)
{
    std::ifstream file(filename);
    if (!file.is_open()) {
        addError("Ne mogu da otvorim fajl: " + filename);
        return false;
    }

    bool first_line = true;

    std::string line;
    while (std::getline(file, line)) {
        // Format: "adresa: bajt bajt bajt..."
        // Primer: "ffffff00: fc 1f e0 81 fc 2f e0 81"

        // Preskoči prazne linije
        size_t start = line.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) continue;

        // Nađi ":"
        size_t colon = line.find(':');
        if (colon == std::string::npos) continue;

        // Parsiraj adresu (hex)
        std::string addr_str = line.substr(0, colon);
        // Ukloni "0x" ako postoji
        if (addr_str.size() > 2 && addr_str[0] == '0' && (addr_str[1] == 'x' || addr_str[1] == 'X')) {
            addr_str = addr_str.substr(2);
        }
        uint32_t addr = (uint32_t)std::stoul(addr_str, nullptr, 16);

        // Prva adresa u fajlu postaje početna adresa programa (pc)
        // (pošto linker smešta sekcije po redosledu, prva je obično početna sekcija)
        if (first_line) {
            gpr_[15] = addr;  // pc = prva adresa
            first_line = false;
        }

        // Parsiraj bajtove
        std::istringstream iss(line.substr(colon + 1));
        std::string byte_str;
        uint32_t cur_addr = addr;
        while (iss >> byte_str) {
            uint8_t b = (uint8_t)std::stoul(byte_str, nullptr, 16);
            writeByte(cur_addr, b);
            cur_addr++;
        }
    }

    file.close();
    return true;
}

// =========================================================================
// Pomoćne funkcije
// =========================================================================

int32_t Emulator::signExtend12(uint32_t val)
{
    // 12-bitni signed -> 32-bitni signed
    if (val & 0x800) {
        // Bit znaka (bit 11) = 1 → negativan → popuni gornjih 20 bita sa 1
        return (int32_t)(val | 0xFFFFF000);
    }
    return (int32_t)val;
}

// =========================================================================
// Izvršavanje instrukcija
// =========================================================================

bool Emulator::executeInstruction(uint32_t instr)
{
    uint8_t oc  = (instr >> 28) & 0xF;
    uint8_t mod = (instr >> 24) & 0xF;
    uint8_t a   = (instr >> 20) & 0xF;
    uint8_t b   = (instr >> 16) & 0xF;
    uint8_t c   = (instr >> 12) & 0xF;
    int32_t disp = signExtend12(instr & 0xFFF);

    // Adresa ove instrukcije (za poruke o greškama)
    uint32_t cur_pc = getGPR(15);

    // Posebno obradi halt - ne pomerati pc (završavamo izvršavanje)
    if (oc == 0x0 && mod == 0x0) {
        return false;  // halt
    }

    // VAŽNO: Pomeri pc na sledeću instrukciju (pc + 4).
    // Ovo se radi PRE izvršavanja kako bi:
    //   1. PC-relativne instrukcije (jmp/call/beq/...) koristile gpr[15] = pc+4
    //      što odgovara tome kako asembler računa Disp.
    //   2. Instrukcije koje ne menjaju pc ne moraju eksplicitno da pomeraju pc.
    // Ako instrukcija menja pc (skok, call, ret), prepisaće ovu vrednost.
    setGPR(15, cur_pc + 4);

    switch (oc) {
        // ---------------------------------------------------------
        // 0x0: iret (MOD=1) — halt (MOD=0) je već obrađen iznad
        // ---------------------------------------------------------
        case 0x0:
            // iret = pop pc; pop status
            // Stack layout (posle int-a): [sp]→pc, [sp+4]→status
            {
                uint32_t sp = getGPR(14);
                setGPR(15, readWord(sp));         // pop pc
                setCSR(0, readWord(sp + 4));      // pop status
                setGPR(14, sp + 8);
            }
            return true;

        // ---------------------------------------------------------
        // 0x1: int (softverski prekid)
        // push status; push pc; cause<=4; status<=status&(~0x1); pc<=handler
        // ---------------------------------------------------------
        case 0x1: {
            uint32_t sp = getGPR(14);
            // Push status (na višu adresu)
            sp -= 4;
            writeWord(sp, getCSR(0));
            // Push pc (povratna adresa = cur_pc + 4, već u gpr[15])
            sp -= 4;
            writeWord(sp, cur_pc + 4);
            setGPR(14, sp);
            // cause = 4 (softverski prekid)
            setCSR(2, 4);
            // Globalno maskiraj prekide (I bit = bit 0)
            setCSR(0, getCSR(0) & ~0x1u);
            // pc = handler
            setGPR(15, getCSR(1));
            return true;
        }

        // ---------------------------------------------------------
        // 0x2: call (push pc; pc <= ...)
        // ---------------------------------------------------------
        case 0x2: {
            uint32_t sp = getGPR(14);
            sp -= 4;
            // Push povratnu adresu (cur_pc + 4, već u gpr[15])
            writeWord(sp, getGPR(15));
            setGPR(14, sp);

            if (mod == 0x0) {
                // pc <= gpr[A] + gpr[B] + D
                // Za A=15 (pc), gpr[15] = cur_pc + 4
                setGPR(15, getGPR(a) + getGPR(b) + disp);
            } else if (mod == 0x1) {
                // pc <= mem32[gpr[A] + gpr[B] + D]
                setGPR(15, readWord(getGPR(a) + getGPR(b) + disp));
            }
            return true;
        }

        // ---------------------------------------------------------
        // 0x3: jmp / beq / bne / bgt
        // MOD 0x0-0x3: direktno (target = gpr[A] + D)
        // MOD 0x8-0xB: indirektno (target = mem32[gpr[A] + D])
        // ---------------------------------------------------------
        case 0x3: {
            // Indirektni bit (bit 3 od MOD) - ako je setovan, cilj se čita iz memorije
            bool indirect = (mod & 0x8) != 0;
            // Bazna adresa = gpr[A] + D (za A=15, gpr[15] je već pc+4)
            uint32_t base = getGPR(a) + disp;
            uint32_t target = indirect ? readWord(base) : base;

            bool take = false;
            switch (mod & 0x7) {  // donja 3 bita MOD određuju uslov
                case 0x0: take = true; break;                                       // jmp
                case 0x1: take = (getGPR(b) == getGPR(c)); break;                   // beq
                case 0x2: take = (getGPR(b) != getGPR(c)); break;                   // bne
                case 0x3: take = ((int32_t)getGPR(b) > (int32_t)getGPR(c)); break;  // bgt (signed)
            }

            if (take) {
                setGPR(15, target);
            }
            // Ako ne skoči, pc je već na cur_pc + 4
            return true;
        }

        // ---------------------------------------------------------
        // 0x4: xchg (atomična zamena)
        // temp <= gpr[B]; gpr[B] <= gpr[C]; gpr[C] <= temp
        // ---------------------------------------------------------
        case 0x4: {
            uint32_t temp = getGPR(b);
            setGPR(b, getGPR(c));
            setGPR(c, temp);
            return true;
        }

        // ---------------------------------------------------------
        // 0x5: aritmetičke (add/sub/mul/div)
        // gpr[A] <= gpr[B] op gpr[C]
        // ---------------------------------------------------------
        case 0x5: {
            uint32_t result = 0;
            switch (mod) {
                case 0x0: result = getGPR(b) + getGPR(c); break;  // add
                case 0x1: result = getGPR(b) - getGPR(c); break;  // sub
                case 0x2: result = getGPR(b) * getGPR(c); break;  // mul
                case 0x3: {                                       // div (signed)
                    int32_t divisor = (int32_t)getGPR(c);
                    if (divisor == 0) {
                        std::ostringstream oss;
                        oss << "Deljenje nulom na adresi 0x" << std::hex << cur_pc;
                        addError(oss.str());
                        result = 0;
                    } else {
                        result = (uint32_t)((int32_t)getGPR(b) / divisor);
                    }
                    break;
                }
            }
            setGPR(a, result);
            return true;
        }

        // ---------------------------------------------------------
        // 0x6: logičke (not/and/or/xor)
        // ---------------------------------------------------------
        case 0x6: {
            uint32_t result = 0;
            switch (mod) {
                case 0x0: result = ~getGPR(b); break;             // not
                case 0x1: result = getGPR(b) & getGPR(c); break;  // and
                case 0x2: result = getGPR(b) | getGPR(c); break;  // or
                case 0x3: result = getGPR(b) ^ getGPR(c); break;  // xor
            }
            setGPR(a, result);
            return true;
        }

        // ---------------------------------------------------------
        // 0x7: pomerčke (shl/shr)
        // gpr[A] <= gpr[B] op gpr[C]
        // ---------------------------------------------------------
        case 0x7: {
            uint32_t result = 0;
            // Shift po modulu 32 (C++ ima UB za shift >= 32)
            uint32_t shift = getGPR(c) & 0x1F;
            switch (mod) {
                case 0x0: result = getGPR(b) << shift; break;  // shl (logički levo)
                case 0x1: result = getGPR(b) >> shift; break;  // shr (logički desno)
            }
            setGPR(a, result);
            return true;
        }

        // ---------------------------------------------------------
        // 0x8: st (smeštanje u memoriju)
        // ---------------------------------------------------------
        case 0x8: {
            switch (mod) {
                case 0x0:
                    // mem32[gpr[A] + gpr[B] + D] <= gpr[C]
                    writeWord(getGPR(a) + getGPR(b) + disp, getGPR(c));
                    break;
                case 0x1:
                    // gpr[A] <= gpr[A] + D; mem32[gpr[A]] <= gpr[C]
                    // (koristi se za push: A=sp, D=-4 → sp-=4; mem[sp]=gpr[C])
                    {
                        uint32_t new_a = getGPR(a) + disp;
                        setGPR(a, new_a);
                        writeWord(new_a, getGPR(c));
                    }
                    break;
                case 0x2:
                    // mem32[mem32[gpr[A]+gpr[B]+D]] <= gpr[C] (indirektno)
                    {
                        uint32_t indirect = readWord(getGPR(a) + getGPR(b) + disp);
                        writeWord(indirect, getGPR(c));
                    }
                    break;
            }
            return true;
        }

        // ---------------------------------------------------------
        // 0x9: ld (učitavanje, uključujući csrrd/csrwr preko MOD)
        // ---------------------------------------------------------
        case 0x9: {
            switch (mod) {
                case 0x0:
                    // csrrd: gpr[A] <= csr[B]
                    setGPR(a, getCSR(b));
                    break;
                case 0x1:
                    // immediate: gpr[A] <= gpr[B] + D
                    // (koristi se za ld $literal: B=r0(0), D=literal)
                    setGPR(a, getGPR(b) + disp);
                    break;
                case 0x2:
                    // mem: gpr[A] <= mem32[gpr[B] + gpr[C] + D]
                    // (koristi se za ld [reg], ld [reg+lit], ld simbol, ld [reg+sim])
                    setGPR(a, readWord(getGPR(b) + getGPR(c) + disp));
                    break;
                case 0x3:
                    // post-increment: gpr[A] <= mem32[gpr[B]]; gpr[B] <= gpr[B] + D
                    // (koristi se za pop: A=gpr, B=sp, D=+4)
                    setGPR(a, readWord(getGPR(b)));
                    setGPR(b, getGPR(b) + disp);
                    break;
                case 0x4:
                    // csrwr: csr[A] <= gpr[B]
                    setCSR(a, getGPR(b));
                    break;
                case 0x5:
                    // csr[A] <= csr[B] | D
                    setCSR(a, getCSR(b) | (uint32_t)disp);
                    break;
                case 0x6:
                    // csr[A] <= mem32[gpr[B] + gpr[C] + D]
                    setCSR(a, readWord(getGPR(b) + getGPR(c) + disp));
                    break;
                case 0x7:
                    // csr[A] <= mem32[gpr[B]]; gpr[B] <= gpr[B] + D
                    setCSR(a, readWord(getGPR(b)));
                    setGPR(b, getGPR(b) + disp);
                    break;
            }
            return true;
        }

        default: {
            std::ostringstream oss;
            oss << "Nepoznat opkod 0x" << std::hex << (int)oc
                << " na adresi 0x" << cur_pc;
            addError(oss.str());
            return false;
        }
    }
}

// =========================================================================
// Glavna petlja izvršavanja
// =========================================================================

void Emulator::run()
{
    // Limit iteracija da izbegnemo beskonačnu petlju u slučaju bag-a u programu
    const uint64_t MAX_ITERATIONS = 10000000;
    uint64_t iter = 0;

    while (iter < MAX_ITERATIONS) {
        uint32_t pc = getGPR(15);
        uint32_t instr = readWord(pc);

        if (!executeInstruction(instr)) {
            break;  // halt
        }
        iter++;
    }

    if (iter >= MAX_ITERATIONS) {
        addError("Prekoračen limit iteracija (moguća beskonačna petlja)");
    }
}

// =========================================================================
// Ispis stanja registara
// =========================================================================

void Emulator::dumpRegisters() const
{
    std::cout << "-----------------------------------------------------------------\n";
    std::cout << "Emulated processor executed halt instruction\n";
    std::cout << "Emulated processor state:\n";

    // 4 registra po redu, format iz postavke (strana 11)
    for (int i = 0; i < 16; i += 4) {
        for (int j = 0; j < 4; j++) {
            int idx = i + j;
            std::cout << "r" << std::setw(2) << std::setfill(' ') << idx << "=0x"
                      << std::hex << std::setfill('0') << std::setw(8)
                      << gpr_[idx] << std::dec;
            if (j < 3) std::cout << "    ";
        }
        std::cout << "\n";
    }
}

// =========================================================================
// main - ulazna tačka emulatora
// =========================================================================

int main(int argc, char* argv[])
{
    if (argc != 2) {
        std::cerr << "Upotreba: " << argv[0] << " <ulazni_hex_fajl>\n";
        return 1;
    }

    Emulator emu;

    if (!emu.loadHex(argv[1])) {
        std::cerr << "Greske pri ucitavanju:\n";
        for (auto& e : emu.errors()) {
            std::cerr << "  " << e << "\n";
        }
        return 1;
    }

    emu.run();

    if (emu.hasErrors()) {
        std::cerr << "Greske pri izvrsavanju:\n";
        for (auto& e : emu.errors()) {
            std::cerr << "  " << e << "\n";
        }
        // Ispiši registre i pored grešaka (radi debug)
        emu.dumpRegisters();
        return 1;
    }

    emu.dumpRegisters();
    return 0;
}