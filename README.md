# SS Projekat 2025/2026

Projekat iz predmeta **Sistemski softver**, Elektrotehnički fakultet Beograd (ETF).

Implementacija lanca prevođenja i emulatora za apstraktni 32-bitni računarski sistem:

- **Asembler** — prevodi izvorni asemblerski kod (`.s`) u predmetni program (`.o`)
- **Linker** — povezuje jedan ili više `.o` fajlova u izlaz za inicijalizaciju memorije
- **Emulator** — izvršava povezan program na emuliranom procesoru

## Korišćenje

```bash
./asembler -o program.o program.s
./linker -hex -o program.hex program.o
./emulator program.hex
```

## Struktura

```
resenje/
├── inc/     # .hpp fajlovi
├── src/     # .cpp fajlovi
├── misc/    # flex/bison ulazni fajlovi
└── tests/   # test primeri
```

## Build

```bash
make
```

Zahteva Linux (amd64) i GNU toolchain (g++, flex, bison).
