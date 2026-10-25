/* SPDX-License-Identifier: GPL-2.0-only */
/* A660 firmware, RSCC and HFI procedures follow Qualcomm's A6xx GPU driver.
 * Copyright (c) 2017-2021, The Linux Foundation. All rights reserved. */
#include "NativeAdrenoA6xxGmu.h"

namespace {
constexpr uint32_t GmuWindowOffset = 0x6a000;
constexpr uint32_t QueueOffset = 0x350;
constexpr uint32_t QueueWords = 1024;
constexpr uint32_t QueueTableWords = 210;

uint32_t gmuOffset(uint32_t index)
{
    return index * 4 - GmuWindowOffset;
}

uint32_t little32(const uint8_t *bytes)
{
    return uint32_t(bytes[0]) | uint32_t(bytes[1]) << 8 |
        uint32_t(bytes[2]) << 16 | uint32_t(bytes[3]) << 24;
}

void publish()
{
    __asm__ volatile("dsb oshst" ::: "memory");
}

void acquire()
{
    __asm__ volatile("dmb oshld" ::: "memory");
}
}

IOReturn NativeAdrenoA6xxGmu::read(uint32_t domain, uint32_t offset, uint32_t *value)
{
    report_.domain = domain;
    report_.offset = offset;
    if (config_.io.read32(config_.io.cookie, domain, offset, value)) {
        report_.status = kIOReturnIOError;
        return report_.status;
    }

    report_.value = *value;
    return kIOReturnSuccess;
}

IOReturn NativeAdrenoA6xxGmu::write(uint32_t domain, uint32_t offset, uint32_t value)
{
    report_.domain = domain;
    report_.offset = offset;
    report_.value = value;
    if (config_.io.write32(config_.io.cookie, domain, offset, value)) {
        report_.status = kIOReturnIOError;
        return report_.status;
    }

    publish();
    return kIOReturnSuccess;
}

IOReturn NativeAdrenoA6xxGmu::gmuWrite(uint32_t index, uint32_t value)
{
    return write(NativeAdrenoA6xxGmuRegisters, gmuOffset(index), value);
}

IOReturn NativeAdrenoA6xxGmu::poll(uint32_t domain, uint32_t offset,
    uint32_t mask, uint32_t expected, uint32_t microseconds)
{
    report_.mask = mask;
    report_.expected = expected;
    for (uint32_t elapsed = 0; elapsed < microseconds; elapsed += 10) {
        uint32_t value;
        IOReturn result = read(domain, offset, &value);
        if (result) {
            return result;
        }
        if ((value & mask) == expected) {
            return kIOReturnSuccess;
        }
        config_.io.delayUs(config_.io.cookie, 10);
    }

    report_.status = kIOReturnTimeout;
    return report_.status;
}

IOReturn NativeAdrenoA6xxGmu::loadFirmware()
{
    // The first two persistent firmware reservations occupy the dummy buffer.
    for (unsigned index = 0; index < 5; ++index) {
        uint8_t *destination = static_cast<uint8_t *>(config_.buffers[index].cpu);
        for (size_t byte = 0; byte < config_.buffers[index].bytes; ++byte) {
            destination[byte] = 0;
        }
    }

    size_t cursor = 0;
    unsigned segments = 0;
    while (cursor < config_.firmwareBytes) {
        if (config_.firmwareBytes - cursor < 16) {
            report_.status = kIOReturnBadMedia;
            return report_.status;
        }
        const uint8_t *header = config_.firmware + cursor;
        const uint32_t address = little32(header);
        const uint32_t length = little32(header + 4);
        const uint32_t type = little32(header + 8);
        const uint32_t allocation = little32(header + 12);
        cursor += 16;
        report_.offset = address;
        report_.value = length;

        const uint32_t extent = length ? length : ((type == 1 || type == 7) ? allocation : 0);
        if (extent > 0xffffffffu - address || length > config_.firmwareBytes - cursor ||
            (address & 3) || (extent & 3)) {
            report_.status = kIOReturnBadMedia;
            return report_.status;
        }

        uint32_t tcmIndex = 0;
        if (address < 0x4000 && extent <= 0x4000 - address) {
            tcmIndex = 0x1b400 + address / 4;
        } else if (address >= 0x10004000 && address < 0x10008000 &&
            extent <= 0x10008000 - address) {
            tcmIndex = 0x1c400 + (address - 0x10004000) / 4;
        }

        bool found = tcmIndex != 0 || extent == 0;
        if (tcmIndex && length) {
            for (uint32_t byte = 0; byte < length; byte += 4) {
                IOReturn result = gmuWrite(tcmIndex + byte / 4,
                    little32(config_.firmware + cursor + byte));
                if (result) {
                    return result;
                }
            }
            ++segments;
        } else if (extent) {
            for (unsigned index = 0; index < 5; ++index) {
                const NativeAdrenoA6xxGmuBuffer &buffer = config_.buffers[index];
                if (address < buffer.iova || address - buffer.iova > buffer.bytes ||
                    extent > buffer.bytes - (address - buffer.iova)) {
                    continue;
                }

                uint8_t *destination = static_cast<uint8_t *>(buffer.cpu) + (address - buffer.iova);
                for (uint32_t byte = 0; byte < length; ++byte) {
                    destination[byte] = config_.firmware[cursor + byte];
                }
                found = true;
                if (length) {
                    ++segments;
                }
                break;
            }
        }
        if (!found) {
            report_.status = kIOReturnNoMemory;
            return report_.status;
        }

        cursor += length;
    }
    if (!segments) {
        report_.status = kIOReturnBadMedia;
        return report_.status;
    }

    publish();
    IOReturn result = read(NativeAdrenoA6xxGmuRegisters, gmuOffset(0x1c400 + 0xff8), &report_.firmwareCore);
    if (result) {
        return result;
    }
    result = read(NativeAdrenoA6xxGmuRegisters, gmuOffset(0x1c400 + 0xffc), &report_.hfiVersion);
    if (result) {
        return result;
    }
    if (!report_.firmwareCore || (report_.hfiVersion >> 28) != 2) {
        report_.status = kIOReturnUnsupported;
        return report_.status;
    }

    return kIOReturnSuccess;
}

IOReturn NativeAdrenoA6xxGmu::initializeQueues()
{
    const NativeAdrenoA6xxGmuBuffer &buffer = config_.buffers[4];
    volatile uint32_t *shared = static_cast<volatile uint32_t *>(buffer.cpu);
    shared[0] = 0;
    shared[1] = QueueTableWords;
    shared[2] = 6;
    shared[3] = 12;
    shared[4] = 3;
    shared[5] = 3;
    for (unsigned index = 0; index < 3; ++index) {
        volatile uint32_t *header = shared + 6 + index * 12;
        header[0] = 1;
        header[1] = uint32_t(buffer.iova) + QueueOffset + index * 4096;
        header[2] = index | (index == 2 ? 40u : 10u) << 8;
        header[3] = QueueWords;
    }

    publish();
    return kIOReturnSuccess;
}

IOReturn NativeAdrenoA6xxGmu::configureRegisters()
{
    struct RegisterValue { uint32_t index; uint32_t value; };
    const RegisterValue values[] = {
        {0x1f8f1, 1},
        {0x1f8f0, 1},
        {0x1f9cc, 1},
        {0x1f81c, 0},
        {0x1f801, 2},
        {0x1f985, uint32_t(config_.buffers[4].iova)},
        {0x1f984, 1},
        {0x23b11, 0x80a008a0},
        {0x1f82d, 0x4052},
        {0x1f901, 0},
        {0x1f982, 0x06035000},
        {0x1f900, uint32_t(config_.buffers[3].iova) | uint32_t(config_.buffers[3].bytes / 4096 - 1)},
        {0x1f40f, 1},
        {0x1f400, 1},
        {0x1f401, 1},
        {0x1f8c0, 0x09c40400},
        {0x1f8e8, 0x0f11},
        {0x1f991, 0xffffffff},
        {0x23b04, 0xffffffff},
        {0x1f993, 0xffffffff},
        {0x23b06, 0xffffffff}
    };
    for (const RegisterValue &entry : values) {
        IOReturn result = gmuWrite(entry.index, entry.value);
        if (result) {
            return result;
        }
    }

    return write(NativeAdrenoA6xxGpu, 0x3c45 * 4, 0);
}

IOReturn NativeAdrenoA6xxGmu::configureSequencers()
{
    // AOP owns the PDC command contents; the host selects and enables them.
    IOReturn result = write(NativeAdrenoA6xxPdc, 0x1148 * 4, 0);
    if (result) {
        return result;
    }
    result = write(NativeAdrenoA6xxPdc, 0x1140 * 4, 0x80000001);
    if (result) {
        return result;
    }

    struct RegisterValue { uint32_t index; uint32_t value; };
    const RegisterValue values[] = {
        {0x004, 0x01000000},
        {0x00b, 1},
        {0x00e, 0},
        {0x00d, 0},
        {0x010, 0},
        {0x00f, 0},
        {0x012, 0x80000000},
        {0x011, 0},
        {0x100, 0},
        {0x008, 0x4520},
        {0x009, 0x4510},
        {0x00a, 0x4514},
        {0x180, 0xeaaae5a0},
        {0x181, 0xe1a1ebab},
        {0x182, 0xa2e0a581},
        {0x183, 0xecac82e2},
        {0x184, 0x0020edad}
    };
    for (const RegisterValue &entry : values) {
        result = write(NativeAdrenoA6xxRscc, entry.index * 4, entry.value);
        if (result) {
            return result;
        }
    }

    return kIOReturnSuccess;
}

IOReturn NativeAdrenoA6xxGmu::health()
{
    const uint32_t offsets[] = {0x1f81c, 0x1f992, 0x23b05};
    const uint32_t masks[] = {0xe00, 0x8000, 0x29};
    for (unsigned index = 0; index < 3; ++index) {
        uint32_t value;
        IOReturn result = read(NativeAdrenoA6xxGmuRegisters, gmuOffset(offsets[index]), &value);
        if (result) {
            return result;
        }
        if (value & masks[index]) {
            report_.mask = masks[index];
            report_.expected = 0;
            report_.status = kIOReturnIOError;
            return report_.status;
        }
    }

    return kIOReturnSuccess;
}

IOReturn NativeAdrenoA6xxGmu::exchange(uint32_t id, const uint32_t *payload,
    size_t words, uint32_t *returned)
{
    if (words > 254 || (words && !payload)) {
        return kIOReturnBadArgument;
    }
    report_.hfiMessage = id;
    volatile uint32_t *shared = static_cast<volatile uint32_t *>(config_.buffers[4].cpu);
    volatile uint32_t *command = shared + 6;
    volatile uint32_t *response = shared + 18;
    volatile uint32_t *commandData = shared + QueueOffset / 4;
    volatile uint32_t *responseData = commandData + QueueWords;
    acquire();
    uint32_t readIndex = command[10];
    uint32_t writeIndex = command[11];
    const uint32_t count = uint32_t(words) + 1;
    const uint32_t padded = (count + 3) & ~3u;
    if (readIndex >= QueueWords || writeIndex >= QueueWords ||
        (readIndex & 3) || (writeIndex & 3) ||
        ((readIndex - writeIndex - 1) & (QueueWords - 1)) < padded) {
        report_.status = kIOReturnNoSpace;
        return report_.status;
    }

    sequence_ = sequence_ % 4095 + 1;
    const uint32_t sent = sequence_ << 20 | count << 8 | id;
    commandData[writeIndex] = sent;
    for (uint32_t index = 0; index < words; ++index) {
        commandData[(writeIndex + index + 1) & (QueueWords - 1)] = payload[index];
    }
    for (uint32_t index = count; index < padded; ++index) {
        commandData[(writeIndex + index) & (QueueWords - 1)] = 0xfafafafa;
    }
    publish();
    command[11] = (writeIndex + padded) & (QueueWords - 1);
    publish();
    IOReturn result = gmuWrite(0x1f994, 1);
    if (result) {
        return result;
    }

    for (unsigned attempt = 0; attempt < 10000; ++attempt) {
        result = health();
        if (result) {
            return result;
        }
        acquire();
        readIndex = response[10];
        writeIndex = response[11];
        if (readIndex >= QueueWords || writeIndex >= QueueWords ||
            (readIndex & 3) || (writeIndex & 3)) {
            report_.status = kIOReturnBadMedia;
            return report_.status;
        }

        for (unsigned packet = 0; readIndex != writeIndex && packet < QueueWords / 4; ++packet) {
            const uint32_t header = responseData[readIndex];
            const uint32_t packetWords = (header >> 8) & 0xff;
            const uint32_t packetExtent = (packetWords + 3) & ~3u;
            const uint32_t available = (writeIndex - readIndex) & (QueueWords - 1);
            if (packetWords < 2 || packetWords > 19 || packetExtent > available) {
                report_.value = header;
                report_.status = kIOReturnBadMedia;
                return report_.status;
            }

            const uint32_t returnHeader = responseData[(readIndex + 1) & (QueueWords - 1)];
            const uint32_t error = packetWords > 2 ? responseData[(readIndex + 2) & (QueueWords - 1)] : 0;
            if ((header & 0xff) == 100) {
                report_.hfiError = error ? error : returnHeader;
                report_.status = kIOReturnIOError;
                return report_.status;
            }
            const bool match = packetWords >= 3 &&
                ((header >> 16) & 0xf) == 1 && (returnHeader >> 20) == sequence_;
            if (match && error) {
                report_.hfiError = error;
                report_.status = kIOReturnIOError;
                return report_.status;
            }
            if (match && returned && packetWords > 3) {
                *returned = responseData[(readIndex + 3) & (QueueWords - 1)];
            }

            readIndex = (readIndex + packetExtent) & (QueueWords - 1);
            publish();
            response[10] = readIndex;
            publish();
            if (match) {
                ++report_.acknowledged;
                return gmuWrite(0x1f991, 1);
            }
        }
        config_.io.delayUs(config_.io.cookie, 10);
    }

    report_.status = kIOReturnTimeout;
    return report_.status;
}

IOReturn NativeAdrenoA6xxGmu::start(const NativeAdrenoA6xxGmuConfig &config)
{
    if (attempted_) {
        return kIOReturnBusy;
    }
    if (!config.io.read32 || !config.io.write32 || !config.io.delayUs ||
        !config.firmware || config.firmwareBytes < 16 ||
        config.firmwareBytes > 0x100000 || !config.tables) {
        return kIOReturnBadArgument;
    }
    for (unsigned index = 0; index < 5; ++index) {
        const NativeAdrenoA6xxGmuBuffer &buffer = config.buffers[index];
        if (!buffer.cpu || !buffer.bytes || (buffer.bytes & 4095) ||
            (buffer.iova & 4095) || buffer.iova > 0x7fffffff ||
            buffer.bytes > 0x80000000 - buffer.iova ||
            (reinterpret_cast<uintptr_t>(buffer.cpu) & 4095)) {
            return kIOReturnBadArgument;
        }
    }
    if (config.buffers[4].bytes < 0x4000 || config.buffers[3].bytes > 0x100000 ||
        config.tables->performance[0] != 2 || config.tables->performance[1] != 2 ||
        config.tables->performance[7] != 315000 || config.tables->performance[53] != 200000) {
        return kIOReturnBadArgument;
    }

    config_ = config;
    attempted_ = true;
    report_ = {};
    report_.phase = Reset;
    IOReturn result = gmuWrite(0x1f800, 1);
    if (result) {
        return result;
    }
    config_.io.delayUs(config_.io.cookie, 10);

    report_.phase = Firmware;
    result = loadFirmware();
    if (result) {
        return result;
    }
    report_.phase = Queues;
    result = initializeQueues();
    if (result) {
        return result;
    }
    report_.phase = Registers;
    result = configureRegisters();
    if (result) {
        return result;
    }

    report_.phase = FirmwareBoot;
    result = gmuWrite(0x1f800, 0);
    if (result) {
        return result;
    }
    const bool legacyBoot = report_.firmwareCore <= 0x20010004;
    result = poll(NativeAdrenoA6xxGmuRegisters, gmuOffset(0x1f81c),
        legacyBoot ? 0xffffffff : 0x1ff, legacyBoot ? 0xbabeface : 0x100, 100000);
    if (result) {
        return result;
    }

    report_.phase = Sequencers;
    result = configureSequencers();
    if (result) {
        return result;
    }
    report_.phase = HfiBoot;
    result = gmuWrite(0x1f986, 1);
    if (result) {
        return result;
    }
    result = poll(NativeAdrenoA6xxGmuRegisters, gmuOffset(0x1f980), 1, 1, 100000);
    if (result) {
        return result;
    }

    report_.phase = PerformanceTable;
    result = exchange(4, config_.tables->performance, 58);
    if (result) {
        return result;
    }
    report_.phase = BandwidthTable;
    result = exchange(3, config_.tables->bandwidth, 159);
    if (result) {
        return result;
    }

    report_.phase = Features;
    const uint32_t features[] = {12, 7, 11};
    for (uint32_t feature : features) {
        const uint32_t payload[] = {feature, 0, 0};
        result = exchange(11, payload, 3);
        if (result) {
            return result;
        }
    }
    report_.phase = CoreStart;
    const uint32_t corePayload = 0;
    result = exchange(14, &corePayload, 1);
    if (result) {
        return result;
    }

    report_.phase = PerformanceVote;
    const uint32_t votePayload[] = {1, 1, 3};
    result = exchange(30, votePayload, 3);
    if (result) {
        return result;
    }
    report_.phase = GpuWake;
    result = gmuWrite(0x1f994, uint32_t(1) << 30);
    if (result) {
        return result;
    }
    result = poll(NativeAdrenoA6xxGmuRegisters, gmuOffset(0x1f992),
        uint32_t(1) << 31, uint32_t(1) << 31, 100000);
    if (result) {
        return result;
    }
    result = gmuWrite(0x1f991, uint32_t(1) << 31);
    if (result) {
        return result;
    }
    result = exchange(10, nullptr, 0);
    if (result) {
        return result;
    }
    result = gmuWrite(0x1f8c3, 1);
    if (result) {
        return result;
    }
    result = poll(NativeAdrenoA6xxGmuRegisters, gmuOffset(0x1f8d0), 0xc0, 0, 100000);
    if (result) {
        return result;
    }
    result = poll(NativeAdrenoA6xxGmuRegisters, gmuOffset(0x23b10), 1, 0, 100000);
    if (result) {
        return result;
    }

    running_ = true;
    report_.phase = Ready;
    report_.status = kIOReturnSuccess;
    return kIOReturnSuccess;
}

IOReturn NativeAdrenoA6xxGmu::keepAlive(bool enabled)
{
    if (!running_) {
        return kIOReturnNotReady;
    }

    IOReturn result = gmuWrite(0x1f8c3, enabled ? 1 : 0);
    if (result || !enabled) {
        return result;
    }

    return poll(NativeAdrenoA6xxGmuRegisters, gmuOffset(0x23b10), 1, 0, 100000);
}

IOReturn NativeAdrenoA6xxGmu::stop()
{
    if (!attempted_) {
        return kIOReturnNotReady;
    }
    report_.phase = Stop;
    IOReturn result = gmuWrite(0x1f993, 0xffffffff);
    if (result) {
        return result;
    }
    result = gmuWrite(0x23b06, 0xffffffff);
    if (result) {
        return result;
    }
    result = gmuWrite(0x1f800, 1);
    if (result) {
        return result;
    }
    result = poll(NativeAdrenoA6xxGmuRegisters, gmuOffset(0x1f800), 1, 1, 1000);
    if (result) {
        return result;
    }

    running_ = false;
    report_.status = kIOReturnSuccess;
    return kIOReturnSuccess;
}
