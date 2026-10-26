/* SPDX-License-Identifier: GPL-2.0-only */
/* SM7325 register sequences and ARC/BCM formats follow the Qualcomm Yupik
 * GPUCC, GCC, RPMh, QMP and A6xx GPU drivers. Copyright (c) 2016-2021,
 * The Linux Foundation. All rights reserved. */
#include "NativeAdrenoA6xxPower.h"
#include <IOKit/IOLib.h>

namespace {
constexpr uint32_t Bit(unsigned bit)
{
    return uint32_t(1) << bit;
}

struct Resource {
    uint32_t address;
    uint32_t length;
    uint32_t recordOffset;
    uint32_t auxiliaryOffset;
    uint8_t auxiliary[512];
};

uint16_t little16(const uint8_t *p)
{
    return uint16_t(p[0]) | uint16_t(p[1]) << 8;
}

uint32_t little32(const uint8_t *p)
{
    return little16(p) | uint32_t(little16(p + 2)) << 16;
}

bool resourceName(uint32_t low, uint32_t high, const char *name)
{
    uint32_t expectedLow = 0;
    uint32_t expectedHigh = 0;
    for (unsigned index = 0; index < 8 && name[index]; ++index) {
        if (index < 4) {
            expectedLow |= uint32_t(uint8_t(name[index])) << (8 * index);
        } else {
            expectedHigh |= uint32_t(uint8_t(name[index])) << (8 * (index - 4));
        }
    }

    return low == expectedLow && high == expectedHigh;
}

bool arcVote(const Resource &primary, const Resource &secondary,
    uint32_t level, uint32_t *vote)
{
    if (!primary.length || primary.length & 1 || !secondary.length ||
        secondary.length & 1 || little16(primary.auxiliary) ||
        little16(secondary.auxiliary)) {
        return false;
    }

    unsigned primaryIndex = 0;
    for (; primaryIndex < primary.length / 2; ++primaryIndex) {
        if (little16(primary.auxiliary + 2 * primaryIndex) >= level) {
            break;
        }
    }
    if (primaryIndex >= primary.length / 2 || primaryIndex > 255) {
        return false;
    }

    const uint32_t voltage = little16(primary.auxiliary + 2 * primaryIndex);
    unsigned secondaryCount = 1;
    while (secondaryCount < secondary.length / 2) {
        if (little16(secondary.auxiliary + 2 * (secondaryCount - 1)) &&
            !little16(secondary.auxiliary + 2 * secondaryCount)) {
            break;
        }
        ++secondaryCount;
    }

    unsigned secondaryIndex = 0;
    while (secondaryIndex + 1 < secondaryCount &&
        little16(secondary.auxiliary + 2 * secondaryIndex) < voltage) {
        ++secondaryIndex;
    }
    if (secondaryIndex > 255) {
        return false;
    }

    *vote = voltage << 16 | secondaryIndex << 8 | primaryIndex;
    return true;
}

uint32_t bcmVote(const Resource &resource, uint32_t busWidth,
    uint32_t bandwidth, bool commit, bool fixed)
{
    uint32_t vote = commit ? Bit(30) : 0;
    if (!bandwidth) {
        return vote;
    }

    vote |= Bit(29);
    if (fixed) {
        return vote | 8;
    }

    uint64_t peak = uint64_t(bandwidth) * little16(resource.auxiliary + 4);
    peak /= busWidth;
    peak = peak * 1000 / little32(resource.auxiliary);
    if (!peak) {
        peak = 1;
    }
    if (peak > 0x3fff) {
        peak = 0x3fff;
    }

    return vote | uint32_t(peak);
}
}

IOReturn NativeAdrenoA6xxPower::read(uint32_t domain, uint32_t offset, uint32_t *out)
{
    report_.domain = domain;
    report_.offset = offset;
    if (io_.read32(io_.cookie, domain, offset, out)) {
        report_.status = kIOReturnIOError;
        return report_.status;
    }

    report_.value = *out;
    return kIOReturnSuccess;
}

IOReturn NativeAdrenoA6xxPower::write(uint32_t domain, uint32_t offset, uint32_t value)
{
    report_.domain = domain;
    report_.offset = offset;
    report_.value = value;
    if (io_.write32(io_.cookie, domain, offset, value)) {
        report_.status = kIOReturnIOError;
        return report_.status;
    }

    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    return kIOReturnSuccess;
}

IOReturn NativeAdrenoA6xxPower::update(uint32_t domain, uint32_t offset,
    uint32_t clear, uint32_t set)
{
    uint32_t value;
    IOReturn result = read(domain, offset, &value);
    if (result) {
        return result;
    }

    return write(domain, offset, (value & ~clear) | set);
}

IOReturn NativeAdrenoA6xxPower::poll(uint32_t domain, uint32_t offset,
    uint32_t mask, uint32_t expected, uint32_t microseconds)
{
    report_.mask = mask;
    report_.expected = expected;
    for (uint32_t elapsed = 0; elapsed < microseconds; ++elapsed) {
        uint32_t value;
        IOReturn result = read(domain, offset, &value);
        if (result) {
            return result;
        }
        if ((value & mask) == expected) {
            return kIOReturnSuccess;
        }
        io_.delayUs(io_.cookie, 1);
    }

    report_.status = kIOReturnTimeout;
    return report_.status;
}

IOReturn NativeAdrenoA6xxPower::buildTables()
{
    constexpr unsigned ResourceCount = 8;
    static const char *const names[] = {
        "gfx.lvl", "mx.lvl", "cx.lvl", "SH0", "MC0", "ACV", "CN0", "xo.lvl"
    };
    Resource resources[ResourceCount]{};
    uint32_t headers[8][2]{};
    uint32_t version = 0;
    uint32_t magic = 0;
    unsigned headerCount = 0;
    unsigned seen = 0;
    auto reject = [&](const char *reason, IOReturn status, unsigned resource,
        uint32_t offset, uint32_t actual, uint32_t expected) -> IOReturn {
        report_.domain = NativeAdrenoA6xxCommandDb;
        report_.offset = offset;
        report_.value = actual;
        report_.expected = expected;
        report_.mask = 0xffffffff;
        report_.status = status;
        IOLog("NativeAdrenoA6xx: command-db rejected reason=%s resource=%u version=%x magic=%x seen=%x offset=%x actual=%x expected=%x\n",
            reason, resource, version, magic, seen, offset, actual, expected);
        for (unsigned index = 0; index < headerCount; ++index) {
            IOLog("NativeAdrenoA6xx: command-db header[%u] at=%x raw=%08x,%08x slave=%u records=%x auxiliary=%x count=%u\n",
                index, 8 + index * 16, headers[index][0], headers[index][1],
                headers[index][0] & 0xffff, 144 + (headers[index][0] >> 16),
                144 + (headers[index][1] & 0xffff), headers[index][1] >> 16);
        }
        for (unsigned index = 0; index < ResourceCount; ++index) {
            const Resource &entry = resources[index];
            IOLog("NativeAdrenoA6xx: command-db resource[%u]=%s found=%u record=%x address=%x auxiliary=%x bytes=%u first=%08x,%08x\n",
                index, names[index], (seen >> index) & 1, entry.recordOffset,
                entry.address, entry.auxiliaryOffset, entry.length,
                little32(entry.auxiliary), little32(entry.auxiliary + 4));
        }

        return status;
    };

    IOReturn result = read(NativeAdrenoA6xxCommandDb, 0, &version);
    if (result) {
        return reject("version-read", result, ResourceCount, 0, 0, 0);
    }
    result = read(NativeAdrenoA6xxCommandDb, 4, &magic);
    if (result) {
        return reject("magic-read", result, ResourceCount, 4, 0, 0x0c0330db);
    }
    if (magic != 0x0c0330db) {
        return reject("magic", kIOReturnBadMedia, ResourceCount, 4, magic, 0x0c0330db);
    }

    for (unsigned slave = 0; slave < 8; ++slave) {
        uint32_t *header = headers[slave];
        headerCount = slave + 1;
        for (unsigned word = 0; word < 2; ++word) {
            result = read(NativeAdrenoA6xxCommandDb, 8 + 16 * slave + 4 * word, &header[word]);
            if (result) {
                return reject("slave-header-read", result, ResourceCount,
                    8 + 16 * slave + 4 * word, 0, 0);
            }
        }
        if (!(header[0] & 0xffff)) {
            break;
        }

        const uint32_t records = 144 + (header[0] >> 16);
        const uint32_t auxiliary = 144 + (header[1] & 0xffff);
        const uint32_t count = header[1] >> 16;
        if (count > 4096 || records > 0x20000 || count * 24 > 0x20000 - records) {
            return reject("slave-record-extent", kIOReturnBadMedia, ResourceCount,
                8 + 16 * slave + 4, header[1], 0x20000);
        }

        for (uint32_t index = 0; index < count; ++index) {
            uint32_t record[6];
            for (unsigned word = 0; word < 6; ++word) {
                result = read(NativeAdrenoA6xxCommandDb, records + index * 24 + word * 4, &record[word]);
                if (result) {
                    return reject("resource-record-read", result, ResourceCount,
                        records + index * 24 + word * 4, 0, 0);
                }
            }

            for (unsigned name = 0; name < ResourceCount; ++name) {
                if (!resourceName(record[0], record[1], names[name])) {
                    continue;
                }
                const uint32_t length = record[5] & 0xffff;
                const uint32_t offset = auxiliary + (record[5] >> 16);
                const uint32_t slaveId = header[0] & 0xffff;
                const uint32_t recordOffset = records + index * 24;
                if (seen & Bit(name)) {
                    return reject("duplicate-resource", kIOReturnBadMedia,
                        name, recordOffset, seen, seen & ~Bit(name));
                }

                resources[name].address = record[4];
                resources[name].length = length;
                resources[name].recordOffset = recordOffset;
                resources[name].auxiliaryOffset = offset;
                if (!record[4] || (record[4] & 3)) {
                    return reject("resource-address", kIOReturnBadMedia,
                        name, recordOffset + 16, record[4], 4);
                }
                const uint32_t expectedSlave = name < 3 || name == 7 ? 3u : 5u;
                if (((record[4] >> 16) & 7) != slaveId || slaveId != expectedSlave) {
                    return reject("resource-slave", kIOReturnBadMedia,
                        name, recordOffset + 16, record[4], expectedSlave << 16);
                }
                if (length > sizeof(resources[name].auxiliary) ||
                    offset > 0x20000 || length > 0x20000 - offset) {
                    return reject("resource-auxiliary-extent", kIOReturnBadMedia,
                        name, recordOffset + 20, record[5], 512);
                }

                for (unsigned byte = 0; byte < length; ++byte) {
                    uint32_t value;
                    result = read(NativeAdrenoA6xxCommandDb, (offset + byte) & ~3u, &value);
                    if (result) {
                        return reject("resource-auxiliary-read", result,
                            name, (offset + byte) & ~3u, 0, 0);
                    }
                    resources[name].auxiliary[byte] = uint8_t(value >> (8 * ((offset + byte) & 3)));
                }
                seen |= Bit(name);
            }
        }
    }
    if (seen != 0xff) {
        return reject("required-resources", kIOReturnNotFound, ResourceCount, 0, seen, 0xff);
    }

    uint32_t gpuVote;
    uint32_t gmuVote;
    if (!arcVote(resources[0], resources[1], 64, &gpuVote)) {
        return reject("gpu-arc-level", kIOReturnUnsupported, 0,
            resources[0].auxiliaryOffset, little32(resources[0].auxiliary), 64);
    }
    if (!arcVote(resources[2], resources[1], 48, &gmuVote)) {
        return reject("gmu-arc-level", kIOReturnUnsupported, 2,
            resources[2].auxiliaryOffset, little32(resources[2].auxiliary), 48);
    }
    for (unsigned index = 3; index < 7; ++index) {
        const Resource &resource = resources[index];
        if (resource.length != 8) {
            return reject("bcm-auxiliary-size", kIOReturnBadMedia, index,
                resource.recordOffset + 20, resource.length, 8);
        }
        // ACV uses the fixed A660 y vote of 8 without unit/width arithmetic.
        if (index == 5) {
            continue;
        }
        if (!little32(resource.auxiliary)) {
            return reject("bcm-unit", kIOReturnBadMedia, index,
                resource.auxiliaryOffset, 0, 1);
        }
        if (!little16(resource.auxiliary + 4)) {
            return reject("bcm-width", kIOReturnBadMedia, index,
                resource.auxiliaryOffset + 4, little32(resource.auxiliary + 4), 1);
        }
    }

    tables_ = {};
    tables_.cxAddress = resources[2].address;
    tables_.mxAddress = resources[1].address;
    tables_.gfxAddress = resources[0].address;
    tables_.xoAddress = resources[7].address;
    tables_.cxIndex = gmuVote & 0xff;
    tables_.gfxIndex = gpuVote & 0xff;
    tables_.mxIndex = (gpuVote >> 8) & 0xff;
    if (((gmuVote >> 8) & 0xff) > tables_.mxIndex) {
        tables_.mxIndex = (gmuVote >> 8) & 0xff;
    }

    tables_.performance[0] = 2;
    tables_.performance[1] = 2;
    tables_.performance[3] = 0xffffffff;
    tables_.performance[5] = gpuVote;
    tables_.performance[6] = 0xffffffff;
    tables_.performance[7] = 315000;
    tables_.performance[52] = gmuVote;
    tables_.performance[53] = 200000;

    // The first four stock DDR levels are shared by the DDR7 and DDR8 tables.
    static const uint32_t bandwidths[] = {0, 0x0bebc2, 0x1ae1b6, 0x209a8e};
    tables_.bandwidth[0] = 4;
    tables_.bandwidth[1] = 1;
    tables_.bandwidth[2] = 3;
    tables_.bandwidth[3] = 1;
    tables_.bandwidth[5] = resources[6].address;
    tables_.bandwidth[11] = bcmVote(resources[6], 4, 0, true, false);
    tables_.bandwidth[17] = bcmVote(resources[6], 4, 100, true, false);
    for (unsigned index = 0; index < 3; ++index) {
        const Resource &resource = resources[index + 3];
        const bool commit = index == 2 ||
            resource.auxiliary[6] != resources[index + 4].auxiliary[6];
        tables_.bandwidth[23 + index] = resource.address;
        if (commit) {
            tables_.bandwidth[4] |= Bit(index);
        }
        for (unsigned level = 0; level < 4; ++level) {
            tables_.bandwidth[31 + level * 8 + index] = bcmVote(resource,
                index == 0 ? 16 : 4, bandwidths[level], commit, index == 2);
        }
    }

    return kIOReturnSuccess;
}

IOReturn NativeAdrenoA6xxPower::disableAcd()
{
    uint32_t magic;
    uint32_t version;
    IOReturn result = read(NativeAdrenoA6xxAop, 0, &magic);
    if (result) {
        return result;
    }
    result = read(NativeAdrenoA6xxAop, 4, &version);
    if (result) {
        return result;
    }
    if (magic != 0x4d41494c || version != 1) {
        report_.status = kIOReturnUnsupported;
        return report_.status;
    }

    uint32_t mailboxBytes;
    uint32_t mailboxOffset;
    result = read(NativeAdrenoA6xxAop, 52, &mailboxBytes);
    if (result) {
        return result;
    }
    result = read(NativeAdrenoA6xxAop, 56, &mailboxOffset);
    if (result) {
        return result;
    }
    if (mailboxOffset < 64 || mailboxOffset & 3 || mailboxBytes < 36 ||
        mailboxOffset > 0x400 || mailboxBytes > 0x400 - mailboxOffset) {
        report_.status = kIOReturnBadMedia;
        return report_.status;
    }

    uint32_t remoteLink;
    result = read(NativeAdrenoA6xxAop, 12, &remoteLink);
    if (result) {
        return result;
    }
    result = write(NativeAdrenoA6xxAop, 16, remoteLink);
    if (result) {
        return result;
    }
    result = write(NativeAdrenoA6xxAop, 36, 0xffff);
    if (result) {
        return result;
    }
    result = write(NativeAdrenoA6xxIpcc, 0x0c, 0);
    if (result) {
        return result;
    }
    result = poll(NativeAdrenoA6xxAop, 40, 0xffffffff, 0xffff, 1000000);
    if (result) {
        return result;
    }

    result = write(NativeAdrenoA6xxAop, 44, 0xffff);
    if (result) {
        return result;
    }
    result = write(NativeAdrenoA6xxIpcc, 0x0c, 0);
    if (result) {
        return result;
    }
    for (unsigned attempt = 0; attempt < 100000; ++attempt) {
        uint32_t remoteChannel;
        uint32_t localAck;
        result = read(NativeAdrenoA6xxAop, 20, &remoteChannel);
        if (result) {
            return result;
        }
        result = write(NativeAdrenoA6xxAop, 24, remoteChannel);
        if (result) {
            return result;
        }
        result = read(NativeAdrenoA6xxAop, 48, &localAck);
        if (result) {
            return result;
        }
        if (localAck == 0xffff && remoteChannel == 0xffff) {
            break;
        }
        if (attempt == 99999) {
            report_.status = kIOReturnTimeout;
            return report_.status;
        }
        result = write(NativeAdrenoA6xxIpcc, 0x0c, 0);
        if (result) {
            return result;
        }
        io_.delayUs(io_.cookie, 10);
    }

    result = poll(NativeAdrenoA6xxAop, mailboxOffset, 0xffffffff, 0, 100000);
    if (result) {
        return result;
    }
    static const char message[] = "{class: gpu, res: acd, val: 0}";
    constexpr uint32_t messageBytes = (sizeof(message) + 3) & ~3u;
    for (unsigned offset = 0; offset < messageBytes; offset += 4) {
        uint32_t value = 0;
        for (unsigned byte = 0; byte < 4 && offset + byte < sizeof(message); ++byte) {
            value |= uint32_t(uint8_t(message[offset + byte])) << (8 * byte);
        }
        result = write(NativeAdrenoA6xxAop, mailboxOffset + 4 + offset, value);
        if (result) {
            return result;
        }
    }
    result = write(NativeAdrenoA6xxAop, mailboxOffset, messageBytes);
    if (result) {
        return result;
    }
    result = write(NativeAdrenoA6xxIpcc, 0x0c, 0);
    if (result) {
        return result;
    }

    return poll(NativeAdrenoA6xxAop, mailboxOffset, 0xffffffff, 0, 1000000);
}

IOReturn NativeAdrenoA6xxPower::voteRails()
{
    constexpr unsigned CommandCount = 8;
    uint32_t configuration;
    IOReturn result = read(NativeAdrenoA6xxRpmh, 0x0c, &configuration);
    if (result) {
        return result;
    }
    if (((configuration >> 12) & 0x3f) < 2 || (configuration >> 27) < CommandCount) {
        report_.status = kIOReturnUnsupported;
        return report_.status;
    }

    unsigned tcs = 0;
    for (; tcs < 2; ++tcs) {
        uint32_t enabled;
        result = read(NativeAdrenoA6xxRpmh, 0xd00 + tcs * 672 + 0x1c, &enabled);
        if (result) {
            return result;
        }
        if (!enabled) {
            break;
        }
    }
    if (tcs == 2) {
        report_.status = kIOReturnBusy;
        return report_.status;
    }

    const uint32_t base = 0xd00 + tcs * 672;
    const uint32_t addresses[] = {tables_.mxAddress, tables_.cxAddress, tables_.gfxAddress,
        tables_.bandwidth[5], tables_.bandwidth[23], tables_.bandwidth[24],
        tables_.bandwidth[25], tables_.xoAddress};
    const uint32_t values[] = {tables_.mxIndex, tables_.cxIndex, tables_.gfxIndex,
        tables_.bandwidth[17], tables_.bandwidth[55], tables_.bandwidth[56],
        tables_.bandwidth[57], 3};

    uint32_t control;
    result = read(NativeAdrenoA6xxRpmh, base + 0x14, &control);
    if (result) {
        return result;
    }
    // The RSC requires trigger removal to finish before AMC mode is cleared.
    const uint32_t controlValues[] = {
        control & ~Bit(24), control & ~(Bit(24) | Bit(16)), Bit(16)
    };
    for (uint32_t value : controlValues) {
        result = write(NativeAdrenoA6xxRpmh, base + 0x14, value);
        if (result) {
            return result;
        }
        result = poll(NativeAdrenoA6xxRpmh, base + 0x14, 0xffffffff, value, 1000);
        if (result) {
            return result;
        }
    }

    result = write(NativeAdrenoA6xxRpmh, 0xd08, Bit(tcs));
    if (result) {
        return result;
    }
    for (unsigned command = 0; command < CommandCount; ++command) {
        IOLog("NativeAdrenoA6xx: parent vote tcs=%u command=%u address=%08x value=%08x\n",
            tcs, command, addresses[command], values[command]);
        result = write(NativeAdrenoA6xxRpmh, base + 0x30 + command * 20, 0x10108);
        if (result) {
            return result;
        }
        result = write(NativeAdrenoA6xxRpmh, base + 0x34 + command * 20, addresses[command]);
        if (result) {
            return result;
        }
        result = write(NativeAdrenoA6xxRpmh, base + 0x38 + command * 20, values[command]);
        if (result) {
            return result;
        }
    }
    const uint32_t commandMask = (1u << CommandCount) - 1;
    result = write(NativeAdrenoA6xxRpmh, base + 0x10, commandMask);
    if (result) {
        return result;
    }
    result = write(NativeAdrenoA6xxRpmh, base + 0x1c, commandMask);
    if (result) {
        return result;
    }
    result = write(NativeAdrenoA6xxRpmh, base + 0x14, Bit(16) | Bit(24));
    if (result) {
        return result;
    }
    result = poll(NativeAdrenoA6xxRpmh, 0xd04, Bit(tcs), Bit(tcs), 100000);
    if (result) {
        return result;
    }
    for (unsigned command = 0; command < CommandCount; ++command) {
        result = poll(NativeAdrenoA6xxRpmh, base + 0x3c + command * 20,
            Bit(8) | Bit(16), Bit(8) | Bit(16), 1000);
        if (result) {
            return result;
        }
    }
    result = write(NativeAdrenoA6xxRpmh, base + 0x1c, 0);
    if (result) {
        return result;
    }

    return write(NativeAdrenoA6xxRpmh, 0xd08, Bit(tcs));
}

IOReturn NativeAdrenoA6xxPower::branch(uint32_t domain, uint32_t offset,
    uint32_t enable, BranchRule rule, uint32_t hwcgMask)
{
    uint32_t value;
    IOReturn result = update(domain, offset, 0, enable);
    if (result) {
        return result;
    }
    result = read(domain, offset, &value);
    if (result) {
        return result;
    }
    if (rule == BranchDelay) {
        io_.delayUs(io_.cookie, 10);
        return kIOReturnSuccess;
    }
    if (rule == BranchSkip || (hwcgMask && (value & hwcgMask))) {
        return kIOReturnSuccess;
    }

    return poll(domain, offset, Bit(31), 0, 200);
}

IOReturn NativeAdrenoA6xxPower::setSource(uint32_t command, uint32_t configuration)
{
    IOReturn result = update(NativeAdrenoA6xxGpuCc, command + 4, 0x71f, configuration);
    if (result) {
        return result;
    }
    result = update(NativeAdrenoA6xxGpuCc, command, 0, 1);
    if (result) {
        return result;
    }

    return poll(NativeAdrenoA6xxGpuCc, command, 1, 0, 1000);
}

IOReturn NativeAdrenoA6xxPower::enableCx(const NativeAdrenoA6xxIo &io)
{
    if (cxReady_ || !io.read32 || !io.write32 || !io.delayUs) {
        return kIOReturnBadArgument;
    }
    io_ = io;
    report_ = {};
    report_.phase = CommandDatabase;
    IOReturn result = buildTables();
    if (result) {
        return result;
    }
    report_.phase = AopAcd;
    result = disableAcd();
    if (result) {
        return result;
    }
    report_.phase = ParentVotes;
    result = voteRails();
    if (result) {
        return result;
    }

    report_.phase = GccClocks;
    result = update(NativeAdrenoA6xxGcc, 0x52010, 0, 1);
    if (result) {
        return result;
    }
    result = poll(NativeAdrenoA6xxGcc, 0, Bit(31), Bit(31), 1000);
    if (result) {
        return result;
    }
    result = branch(NativeAdrenoA6xxGcc, 0x52000, Bit(15) | Bit(16), BranchDelay);
    if (result) {
        return result;
    }
    result = branch(NativeAdrenoA6xxGcc, 0x71004, 1, BranchDelay);
    if (result) {
        return result;
    }

    report_.phase = CxDomain;
    result = update(NativeAdrenoA6xxGpuCc, 0x106c, 7, Bit(11));
    if (result) {
        return result;
    }
    io_.delayUs(io_.cookie, 1);
    result = poll(NativeAdrenoA6xxGpuCc, 0x1540, Bit(31), Bit(31), 5000);
    if (result) {
        return result;
    }

    report_.phase = GmuClock;
    result = setSource(0x1120, 0x602); // GPLL0/2 divided by 1.5 gives 200 MHz.
    if (result) {
        return result;
    }
    report_.phase = HubClock;
    result = setSource(0x117c, 0x603); // GPLL0/2 divided by 2 gives 150 MHz.
    if (result) {
        return result;
    }

    report_.phase = GpuCcClocks;
    struct Clock {
        uint32_t domain;
        uint32_t offset;
        BranchRule rule;
        uint32_t hwcgMask;
    };
    const Clock clocks[] = {
        {NativeAdrenoA6xxGpuCc, 0x1170, BranchDelay, 0},
        {NativeAdrenoA6xxGpuCc, 0x1004, BranchVoted, 0},
        {NativeAdrenoA6xxGpuCc, 0x109c, BranchHalt, 0},
        {NativeAdrenoA6xxGpuCc, 0x1178, BranchHalt, 0},
        {NativeAdrenoA6xxGpuCc, 0x1204, BranchHalt, 0},
        {NativeAdrenoA6xxGpuCc, 0x1078, BranchDelay, 0},
        {NativeAdrenoA6xxGpuCc, 0x107c, BranchVoted, 0},
        {NativeAdrenoA6xxGpuCc, 0x1098, BranchHalt, 0},
        {NativeAdrenoA6xxGpuCc, 0x108c, BranchVoted, 0},
        {NativeAdrenoA6xxGpuCc, 0x1090, BranchVoted, 0},
        {NativeAdrenoA6xxGpuCc, 0x5000, BranchVoted, 0},
        {NativeAdrenoA6xxGcc, 0x71154, BranchSkip, Bit(1)},
        {NativeAdrenoA6xxGcc, 0x7100c, BranchVoted, Bit(1)},
        {NativeAdrenoA6xxGcc, 0x71018, BranchHalt, 0}
    };
    for (const Clock &clock : clocks) {
        result = branch(clock.domain, clock.offset, 1, clock.rule, clock.hwcgMask);
        if (result) {
            return result;
        }
    }

    cxReady_ = true;
    report_.phase = CxReady;
    report_.status = kIOReturnSuccess;
    return kIOReturnSuccess;
}

IOReturn NativeAdrenoA6xxPower::enableGx()
{
    if (!cxReady_) {
        return kIOReturnNotReady;
    }
    report_.phase = GxDomain;
    IOReturn result = poll(NativeAdrenoA6xxGpuCc, 0x100c, Bit(31), Bit(31), 5000);
    if (result) {
        return result;
    }
    const uint32_t gxDomain = report_.value;
    result = poll(NativeAdrenoA6xxGmuRegisters, 0x1f8d0 * 4 - 0x6a000, 0xc0, 0, 5000);
    if (result) {
        return result;
    }
    const uint32_t gxStatus = report_.value;
    result = poll(NativeAdrenoA6xxGmuRegisters, 0x23b10 * 4 - 0x6a000, 0xffffffff, 0, 5000);
    if (result) {
        return result;
    }
    const uint32_t fence = report_.value;
    uint32_t gxClock;
    result = read(NativeAdrenoA6xxGpuCc, 0x1064, &gxClock);
    if (result) {
        return result;
    }
    IOLog("NativeAdrenoA6xx: GMU owns GX domain=%08x status=%08x fence=%08x clock=%08x\n",
        gxDomain, gxStatus, fence, gxClock);

    gxReady_ = true;
    report_.phase = GxReady;
    report_.status = kIOReturnSuccess;
    return kIOReturnSuccess;
}

IOReturn NativeAdrenoA6xxPower::stop(bool dmaStopped)
{
    if (!dmaStopped) {
        return kIOReturnBusy;
    }
    if (!cxReady_) {
        return kIOReturnNotReady;
    }

    // Host votes and SMMU clocks remain asserted while the service owns memory.
    if (!report_.status) {
        report_.phase = Stopped;
    }
    return kIOReturnSuccess;
}
