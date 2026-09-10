#ifndef EMULATOR_HPP
#define EMULATOR_HPP

/*
 * emulator.hpp - interfejs interpretativnog emulatora (Nivo A)
 *
 * Po postavci (strana 11-17):
 *   - 32-bitni dvoadresni procesor, Von Neuman arhitektura
 *   - Adresibilna jedinica: 1 bajt, little-endian
 *   - Memorijski prostor: 2^32 bajta
 *   - 16 GPR registara (r0-r15), r0=0 (ožičen), r14=sp, r15=pc
 *   - 3 CSR registra: status=0, handler=1, cause=2
 *   - Početna adresa: 0x40000000
 *   - Svaka instrukcija je 4 bajta
 *   - Izvršavanje se završava instrukcijom halt
 *   - Nakon halt-a ispisuje stanje svih registara
 *
 * CLI: emulator <naziv_ulazne_datoteke>
 *   Ulaz: hex fajl iz linkera (-hex opcija)
 *   Format: adresa: bajt bajt bajt... (8 bajta po redu)
 */

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>

class Emulator {
public:
    Emulator();
    ~Emulator();

    // Učitavanje hex fajla u memoriju.
    // Početna adresa programa (pc) se postavlja na adresu prvog bajta u fajlu.
    bool loadHex(const std::string& filename);

    // Glavna petlja izvršavanja
    void run();

    // Ispis stanja registara nakon halt
    void dumpRegisters() const;

    bool hasErrors() const { return !errors_.empty(); }
    const std::vector<std::string>& errors() const { return errors_; }

private:
    // ===== Memorija =====
    // 32-bitni adresni prostor (4GB) - koristimo hash mapu za sparse memoriju.
    // Neinicijalizovane adrese se tretiraju kao 0.
    std::unordered_map<uint32_t, uint8_t> memory_;

    // ===== Registar =====
    uint32_t gpr_[16];   // r0-r15 (r0 uvek 0, r14=sp, r15=pc)
    uint32_t csr_[3];    // status=0, handler=1, cause=2

    // ===== Greske =====
    std::vector<std::string> errors_;

    // ===== Pomocne metode =====

    // Pristup memoriji (1 bajt)
    uint8_t  readByte(uint32_t addr);
    void     writeByte(uint32_t addr, uint8_t val);

    // Pristup memoriji (32 bita, little-endian)
    uint32_t readWord(uint32_t addr);
    void     writeWord(uint32_t addr, uint32_t val);

    // Pristup registrima
    uint32_t getGPR(int idx) const;        // r0 uvek vraća 0
    void     setGPR(int idx, uint32_t val); // pisanje u r0 se ignoriše
    uint32_t getCSR(int idx) const;
    void     setCSR(int idx, uint32_t val);

    // Izvršavanje jedne instrukcije
    // Pre ulaska: gpr[15] je adresa instrukcije.
    // Unutar funkcije: gpr[15] se prvo pomera na pc+4 (sledeća instrukcija),
    //   pa onda jump/call instrukcije mogu prepisati gpr[15] ciljnom adresom.
    // Vraća true za nastavak, false za halt.
    bool executeInstruction(uint32_t instr);

    // Sign-extend 12-bitne vrednosti
    static int32_t signExtend12(uint32_t val);

    // Pomocno za error
    void addError(const std::string& msg) { errors_.push_back(msg); }
};

#endif // EMULATOR_HPP