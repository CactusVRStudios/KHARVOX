#include "../src/common/DoomBuild.h"
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace kharvox::game;
static void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

// Optional real-file fixtures are mapped as data, never loaded as modules or
// executed. The ordinary CTest run does not require proprietary game files.
static void checkFixture(const char* path, Build expected) {
    std::ifstream file(path, std::ios::binary);
    require(bool(file), "fixture missing");
    std::vector<unsigned char> raw((std::istreambuf_iterator<char>(file)), {});
    require(raw.size() > sizeof(IMAGE_DOS_HEADER), "truncated fixture");
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(raw.data());
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(raw.data() + dos->e_lfanew);
    std::vector<unsigned char> image(nt->OptionalHeader.SizeOfImage);
    std::memcpy(image.data(), raw.data(), nt->OptionalHeader.SizeOfHeaders);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        const auto& section = IMAGE_FIRST_SECTION(nt)[i];
        require(size_t(section.PointerToRawData) + section.SizeOfRawData <= raw.size(), "section file bounds");
        require(size_t(section.VirtualAddress) + section.SizeOfRawData <= image.size(), "section image bounds");
        std::memcpy(image.data() + section.VirtualAddress, raw.data() + section.PointerToRawData, section.SizeOfRawData);
    }
    require(inspectImage(image.data()) == expected, "real image identity");
    for (const auto& h : guardHashes) {
        const auto address = mappedRva(expected, h.steam);
        require(address && address + h.length < image.size(), "code hash bounds");
        require(hashBytes(image.data() + address, h.length) ==
            (expected == Build::Gog20250225 ? h.gogHash : h.steamHash), "real hook body hash");
    }
    image[0x1000] ^= 1;
    require(inspectImage(image.data()) == Build::Unknown, "modified code rejected");
}

int main(int argc, char** argv) {
    try {
        require(identify(1711036533, 0x14161000, steamTextHash) == Build::Steam20240321, "Steam identity");
        require(identify(1740486338, 0x1415f000, gogTextHash) == Build::Gog20250225, "GOG identity");
        require(identify(1740486338, 0x1415f000, steamTextHash) == Build::Unknown, "wrong hash rejected");
        require(identify(0, 0x1415f000, gogTextHash) == Build::Unknown, "wrong timestamp rejected");
        require(mappedRva(Build::Gog20250225, 0x12345678) == 0, "unknown GOG address rejected");
        require(canonicalRva(Build::Steam20240321, 0x67288e0) == 0x67288e0, "Steam CVar identity unchanged");
        require(canonicalRva(Build::Gog20250225, mappedRva(Build::Gog20250225, 0x67288e0)) == 0x67288e0,
            "GOG film-grain CVar maps back to its canonical identity");
        require(canonicalRva(Build::Gog20250225, 0x67288e0) == 0,
            "unmapped GOG CVar must not impersonate Steam film grain at the same numeric address");
        uintptr_t previous = 0;
        for (const auto& a : addresses) {
            require(a.steam > previous && a.gog != 0, "sorted unique mappings");
            require(mappedRva(Build::Steam20240321, a.steam) == a.steam, "Steam unchanged");
            require(mappedRva(Build::Gog20250225, a.steam) == a.gog, "GOG mapping lookup");
            previous = a.steam;
        }
        for (const auto& p : guardBytes)
            require(translatedGuardByte(p.gog, p.steamByte) == p.gogByte, "exact guard-byte translation");
        require(translatedGuardByte(0, 0xaa) == 0xaa, "unmapped guard is not masked");
        if (argc == 3) {
            checkFixture(argv[1], Build::Steam20240321);
            checkFixture(argv[2], Build::Gog20250225);
        }
        std::cout << "DOOM build profile checks passed" << std::endl;
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << std::endl;
        return 1;
    }
}
