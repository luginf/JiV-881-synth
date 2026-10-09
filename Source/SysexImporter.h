#pragma once

// Reading JV-880 patch dumps out of .syx / .mid files (GitHub issue #4: "import SysEx or .MID banks, full
// backups or individual patches"). Only the file side lives here: finding the SysEx messages and putting the
// patches they carry back together. The conversion into the firmware's own 362-byte patch record is done by the
// firmware itself (VirtualJVProcessor::importSysexFile()): each patch is sent to a scratch Internal slot over MIDI
// and read back.

#include <JuceHeader.h>

#include <cstdint>
#include <map>
#include <set>
#include <vector>

namespace sysexio {

using Message = std::vector<uint8_t>; // F0 ... F7

// Every complete SysEx message of a .syx file, or of the SysEx events of a Standard MIDI File.
inline std::vector<Message> readMessages(const juce::File &file, juce::String &error)
{
    std::vector<Message> out;
    juce::FileInputStream in(file);
    if (!in.openedOk())
    {
        error = "Cannot open " + file.getFileName();
        return out;
    }

    if (file.hasFileExtension("mid;midi;smf"))
    {
        juce::MidiFile midi;
        if (!midi.readFrom(in))
        {
            error = file.getFileName() + " is not a valid MIDI file";
            return out;
        }
        for (int t = 0; t < midi.getNumTracks(); ++t)
        {
            const auto *track = midi.getTrack(t);
            for (int i = 0; i < track->getNumEvents(); ++i)
            {
                const auto &m = track->getEventPointer(i)->message;
                if (!m.isSysEx())
                    continue;
                Message msg(m.getRawData(), m.getRawData() + m.getRawDataSize());
                if (msg.empty() || msg.front() != 0xF0)
                    msg.insert(msg.begin(), 0xF0);
                if (msg.back() != 0xF7)
                    msg.push_back(0xF7);
                out.push_back(std::move(msg));
            }
        }
        return out;
    }

    juce::MemoryBlock block;
    in.readIntoMemoryBlock(block);
    const auto *bytes = static_cast<const uint8_t *>(block.getData());
    const size_t size = block.getSize();
    for (size_t i = 0; i < size; ++i)
    {
        if (bytes[i] != 0xF0)
            continue;
        size_t j = i + 1;
        while (j < size && bytes[j] < 0x80)
            ++j;
        if (j < size && bytes[j] == 0xF7)
        {
            out.emplace_back(bytes + i, bytes + j + 1);
            i = j;
        }
        // else: truncated message (a data byte was followed by something that is not F7): skipped
    }
    return out;
}

// One patch: its common block and the four tone blocks (data bytes only, as they travel in a DT1 message).
struct Patch
{
    static constexpr int kCommonBytes = 34; // name + effects + level...
    static constexpr int kToneBytes = 116;

    juce::String name;
    juce::String origin;                  // "Internal 12", "Patch Temp"
    std::vector<uint8_t> blocks[5];       // [0] common, [1..4] tones
};

inline uint8_t checksum(const uint8_t *data, size_t length)
{
    uint32_t sum = 0;
    for (size_t i = 0; i < length; ++i)
        sum += data[i];
    return (uint8_t)((0x80 - (sum & 0x7f)) & 0x7f);
}

// Puts the DT1 messages back together into patches. Understands what the JV-880 sends/receives: Patch Temp
// (00 08 20/28..2B) and Internal Patch Memory (01 40..7F 20/28..2B), data possibly split over several messages.
// Anything else (Performance, Rhythm, System) is only counted in `notes`.
inline std::vector<Patch> extractPatches(const std::vector<Message> &messages, juce::StringArray &notes)
{
    std::map<uint32_t, uint8_t> memory; // 7-bit packed address -> byte
    int ignored = 0, badChecksum = 0;
    for (const auto &m : messages)
    {
        // F0 41 <device> 46 12 a a a a <data...> <checksum> F7
        if (m.size() < 12 || m[1] != 0x41 || m[3] != 0x46 || m[4] != 0x12)
        {
            ++ignored;
            continue;
        }
        if (checksum(m.data() + 5, m.size() - 7) != m[m.size() - 2])
        {
            ++badChecksum;
            continue;
        }
        const uint32_t address = ((uint32_t)m[5] << 21) | ((uint32_t)m[6] << 14) | ((uint32_t)m[7] << 7) | m[8];
        for (size_t i = 9; i + 2 < m.size(); ++i)
            memory[address + (uint32_t)(i - 9)] = m[i];
    }
    if (ignored > 0)
        notes.add(juce::String(ignored) + " message(s) are not JV-880 data and were ignored");
    if (badChecksum > 0)
        notes.add(juce::String(badChecksum) + " message(s) with a wrong checksum were ignored");

    std::set<uint32_t> bases; // address of each patch's common block
    for (const auto &kv : memory)
    {
        const uint32_t a = kv.first;
        const uint32_t aa = a >> 21, bb = (a >> 14) & 127, cc = (a >> 7) & 127;
        if (cc == 0x20 && ((aa == 1 && bb >= 0x40) || (aa == 0 && bb == 0x08)))
            bases.insert(a & ~127u);
    }

    std::vector<Patch> patches;
    int incomplete = 0;
    for (uint32_t base : bases)
    {
        Patch p;
        bool complete = true;
        for (int k = 0; k < 5 && complete; ++k)
        {
            const uint32_t blockAddress = base + (k == 0 ? 0u : ((uint32_t)(0x08 + k - 1) << 7));
            for (uint32_t i = 0; i < 128; ++i)
            {
                const auto it = memory.find(blockAddress + i);
                if (it == memory.end())
                    break;
                p.blocks[k].push_back(it->second);
            }
            const size_t wanted = k == 0 ? Patch::kCommonBytes : Patch::kToneBytes;
            if (p.blocks[k].size() < wanted)
                complete = false;
        }
        const uint32_t bb = (base >> 14) & 127;
        p.origin = ((base >> 21) == 1) ? "Internal " + juce::String((int)bb - 0x40 + 1) : juce::String("Patch Temp");
        if (!complete)
        {
            ++incomplete;
            continue;
        }
        juce::String name;
        for (int i = 0; i < 12; ++i)
            name += (char)((p.blocks[0][(size_t)i] >= 32 && p.blocks[0][(size_t)i] < 127) ? p.blocks[0][(size_t)i] : ' ');
        p.name = name.trim();
        if (p.name.isEmpty())
            p.name = "Imported Patch";
        patches.push_back(std::move(p));
    }
    if (incomplete > 0)
        notes.add(juce::String(incomplete) + " incomplete patch(es) (common or tone block missing) were skipped");
    return patches;
}

// The five DT1 messages that write the patch into Internal Patch Memory slot `slot` (0..63): 01 40+slot, 20/28..2B.
inline std::vector<Message> internalSlotMessages(const Patch &p, int slot)
{
    std::vector<Message> out;
    for (int k = 0; k < 5; ++k)
    {
        const uint8_t cc = (uint8_t)(k == 0 ? 0x20 : 0x28 + k - 1);
        Message m = { 0xF0, 0x41, 0x10, 0x46, 0x12, 0x01, (uint8_t)(0x40 + slot), cc, 0x00 };
        m.insert(m.end(), p.blocks[k].begin(), p.blocks[k].end());
        m.push_back(checksum(m.data() + 5, m.size() - 5));
        m.push_back(0xF7);
        out.push_back(std::move(m));
    }
    return out;
}

} // namespace sysexio
