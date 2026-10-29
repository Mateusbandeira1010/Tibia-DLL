#include "../src/mount_system.cpp"

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <vector>

// The exercised packet readers never call rendering, hooks or the timer.
// Abort if a test unexpectedly leaves that isolated path.
void (*volatile unexpectedNativeCall)() = &std::abort;
namespace EnhancedDat
{
    void BeginCreatureDraw(DWORD) { unexpectedNativeCall(); }
    void EndCreatureDraw() { unexpectedNativeCall(); }
}
bool HookJMP(DWORD, DWORD, std::size_t) { unexpectedNativeCall(); return false; }
bool HookCall(DWORD, DWORD) { unexpectedNativeCall(); return false; }
bool Nop(DWORD, int) { unexpectedNativeCall(); return false; }
DWORD getTimerTick() { unexpectedNativeCall(); return 0; }

namespace
{
    std::vector<BYTE> packet;
    size_t position = 0;
    unsigned int checks = 0;

    void Check(bool condition)
    {
        ++checks;
        if(!condition)
        {
            std::cerr << "Failed check " << checks << " at byte " << position << '\n';
            std::exit(1);
        }
    }

    BYTE __cdecl ReadByte()
    {
        Check(position < packet.size());
        return packet[position++];
    }

    WORD __cdecl ReadWord()
    {
        const WORD low = ReadByte();
        return static_cast<WORD>(low | (static_cast<WORD>(ReadByte()) << 8));
    }

    void __cdecl ReadString(char* output, DWORD maximumLength)
    {
        const WORD length = ReadWord();
        Check(position + length <= packet.size());
        const size_t copied = (std::min)(static_cast<size_t>(length),
            static_cast<size_t>(maximumLength));
        std::memcpy(output, packet.data() + position, copied);
        output[copied] = 0;
        position += length;
    }

    void AddWord(WORD value)
    {
        packet.push_back(static_cast<BYTE>(value));
        packet.push_back(static_cast<BYTE>(value >> 8));
    }

    void AddString(const char* text)
    {
        const size_t length = std::strlen(text);
        AddWord(static_cast<WORD>(length));
        packet.insert(packet.end(), text, text + length);
    }

    void TestCount(WORD count, BYTE mountCount)
    {
        packet.clear();
        position = 0;
        AddWord(400); // current mount
        AddWord(count); // TFS uses a uint16 outfit count
        for(unsigned int i = 0; i < count; ++i)
        {
            AddWord(static_cast<WORD>(100 + i));
            AddString("Outfit");
            packet.push_back(3);
        }
        packet.push_back(mountCount);
        for(unsigned int i = 0; i < mountCount; ++i)
        {
            AddWord(static_cast<WORD>(400 + i));
            AddString("Mount");
        }
        packet.push_back(0xA0); // next server opcode must remain unread

        const DWORD visible = ReadOutfitWindowHeader(0);
        Check(visible == (std::min)(static_cast<DWORD>(count), DWORD{255}));
        Check(g_skippedOutfitCount == count - visible);
        for(DWORD i = 0; i < visible; ++i)
        {
            Check(ClientReadU16() == 100 + i);
            char name[32] = {};
            ClientReadString(name, sizeof(name));
            Check(std::strcmp(name, "Outfit") == 0);
            Check(ClientReadU8() == 3);
        }

        SkipOutfitWindowMountList();
        Check(g_skippedOutfitCount == 0);
        Check(g_mountEntryCount == mountCount);
        for(unsigned int i = 0; i < mountCount; ++i)
        {
            Check(g_mountEntries[i].clientId == 400 + i);
            Check(std::strcmp(g_mountEntries[i].name, "Mount") == 0);
        }
        Check(position == packet.size() - 1);
        Check(ReadByte() == 0xA0);
    }
}

int main()
{
    g_readU8Addr = reinterpret_cast<DWORD>(&ReadByte);
    g_readU16Addr = reinterpret_cast<DWORD>(&ReadWord);
    g_readStringAddr = reinterpret_cast<DWORD>(&ReadString);
    for(WORD count : {WORD{0}, WORD{1}, WORD{254}, WORD{255},
        WORD{256}, WORD{300}, WORD{5000}, WORD{65535}})
    {
        TestCount(count, 0);
        TestCount(count, 3);
    }
    TestCount(256, 255);
    std::cout << checks << " outfit/mount protocol checks passed\n";
}
