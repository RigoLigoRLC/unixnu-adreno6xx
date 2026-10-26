/* SPDX-License-Identifier: GPL-2.0-only */
/* A6xx initialization and A660 register tables follow adreno_a6xx.c and
 * adreno-gpulist.h from the Samsung SM7325 kernel.
 * Copyright (c) 2002,2007-2021, The Linux Foundation. All rights reserved. */
#include "NativeAdrenoA6xxHwInit.h"
#include <IOKit/IOLib.h>

namespace {
/* Register indices are dwords from SM7325 a6xx_reg.h. */
const uint32_t kTrustedMemoryRegisters[]={
    0xf803,0xf800,0xf801,0xf802
};
const uint32_t kAddressModes[]={
    0x842,0xc01,0x8601,0x8e05,0x9e01,0xbe05,0xa601,0x9601,
    0xe00,0xae01,0xb601,0xf810
};
const uint32_t kInitial[][2]={
    {0x3c03,0x00071620},{0x3c04,0x00071620},
    {0x3c05,0x00071620},{0x3c06,0x00071620},{0x11,3},
    {0x50b,0xffffffff},
    {0xe05,0xffffffc0},{0xe06,0x0001ffff},
    {0xe09,0xfffff000},{0xe0a,0x0001ffff},
    {0xe07,0xfffff000},{0xe08,0x0001ffff},
    {0xe18,0x804},{0xe17,4},
    {0x8c2,0x02000140},{0x8c1,0x8040362c},{0x8c3,128},{0xb34,0x20},
    {0x9e00,0x00200200},{0x98d,1},{0x500,1},
    {0x1f,0x4003ffff},{0xe19,1},
    {0xb608,0},{0xb609,0x3fe05ff4},{0xb60a,0x3fa0ebee},
    {0xb60b,0x3f5193ed},{0xb60c,0x3f0243f0},
    {0x841,1},{0x11,0},{0xe3c,0x66906},
    {0x844,0x6e}
};
const uint32_t kHwcg[][2]={
    {0xb0,0x02222222},{0xb4,0x02222220},{0xb8,0x80},{0xbc,0xf3cf},
    {0xc0,0x22222222},{0xc4,0x22222222},{0xc8,0x22222222},{0xcc,0x22222},
    {0xd0,0x11111111},{0xd4,0x11111111},{0xd8,0x11111111},{0xdc,0x11111},
    {0xe0,0x77777777},{0xe4,0x77777777},{0xe8,0x77777777},{0xec,0x77777},
    {0xf0,0x22222222},{0xf4,0x01002222},{0xf8,0x2220},{0x100,0x40f00},
    {0x104,0x25222022},{0x105,0x5555},{0x106,0x11},{0x107,0x445044},
    {0x108,0x04222222},{0x111,0x2222},{0x114,0x00222222},{0x117,2},
    {0x11b,0x2222},{0x109,0x4000},{0x112,0x2222},{0x115,0x200},
    {0x11c,0},{0x10a,0},{0x113,0},{0x116,0x04104004},{0x11d,0},
    {0x120,0x222},{0x121,0x111},{0x122,0},
    {0x10b,0x22222222},{0x110,4},{0x10f,2},{0x533,0x182},
    {0x44,0},{0x42,0},{0x118,0x222},{0x119,0x111},{0x11a,0x555}
};
/* Each entry is CP_PROTECT slot, first register, last register, no-access bit. */
const uint32_t kProtected[][4]={
    {0,0,0x4ff,0},{1,0x501,0x506,0},{2,0x50b,0x7ff,0},
    {3,0x50e,0x50e,1},{4,0x510,0x510,1},{5,0x534,0x534,1},
    {6,0x800,0x882,1},{7,0x8a0,0x8a8,1},{8,0x8ab,0x8cf,1},
    {9,0x8d0,0x98c,0},{10,0x900,0x94d,1},{11,0x98d,0xbff,1},
    {12,0xe00,0xe01,1},{13,0xe03,0xe0f,1},
    {14,0x3c00,0x3cc3,1},{15,0x3cc4,0x5cc3,0},
    {16,0x8630,0x87ff,1},{17,0x8e00,0x8e00,1},
    {18,0x8e08,0x8e08,1},{19,0x8e50,0x8e6f,1},
    {20,0x8e80,0x90ff,1},{21,0x9624,0x97ff,1},
    {22,0x9e60,0x9e71,1},{23,0x9e78,0x9fff,1},
    {24,0xa630,0xa7ff,1},{25,0xae02,0xae02,1},
    {26,0xae50,0xaf7f,1},{27,0xb604,0xb604,1},
    {28,0xb608,0xb60e,1},{29,0xbe02,0xbe03,1},
    {30,0xbe20,0xbf7f,1},{31,0xd000,0xd5ff,1},
    {32,0xf000,0xfbff,1},{33,0xfc00,0x11bff,0},
    {34,0x18400,0x1a3ff,1},{35,0x1a400,0x1c3ff,1},
    {36,0x1c400,0x1e3ff,1},{37,0x1f400,0x1f843,1},
    {38,0x1f844,0x1f8bf,0},{39,0x1f860,0x1f860,1},
    {40,0x1f887,0x1f8a2,1},{47,0x1f8c0,0x1f8c0,1}
};
}

IOReturn NativeAdrenoA6xxHwInit::program(const NativeAdrenoRingIo *io,uint32_t highestBankBit)
{
    if(!io || !io->write32 || !io->read32 || !io->barrier ||
        highestBankBit<13 || highestBankBit>16) {
        return kIOReturnBadArgument;
    }

    uint32_t trustedBefore[4]={0};
    uint32_t trustedAfter[4]={0};
    for(unsigned index=0;index<4;++index) {
        if(io->read32(io->cookie,kTrustedMemoryRegisters[index],&trustedBefore[index])) {
            return kIOReturnIOError;
        }
    }
    IOLog("NativeAdrenoHwInit: inherited TSB control=%08x base=%08x:%08x size=%08x\n",
        trustedBefore[0],trustedBefore[2],trustedBefore[1],trustedBefore[3]);

    /* This driver allocates ordinary GPU memory. The DRM A6xx setup clears
     * TSB control and the trusted range before enabling SQE in this mode. */
    for(uint32_t reg:kTrustedMemoryRegisters) {
        if(io->write32(io->cookie,reg,0)) {
            return kIOReturnIOError;
        }
    }
    if(io->barrier(io->cookie)) {
        return kIOReturnIOError;
    }
    for(unsigned index=0;index<4;++index) {
        if(io->read32(io->cookie,kTrustedMemoryRegisters[index],&trustedAfter[index])) {
            return kIOReturnIOError;
        }
    }
    IOLog("NativeAdrenoHwInit: configured TSB control=%08x base=%08x:%08x size=%08x\n",
        trustedAfter[0],trustedAfter[2],trustedAfter[1],trustedAfter[3]);
    if(trustedAfter[0] || trustedAfter[1] || trustedAfter[2] || trustedAfter[3]) {
        return kIOReturnIOError;
    }

    IOLog("NativeAdrenoHwInit: address modes begin\n");
    for(uint32_t reg:kAddressModes) {
        if(io->write32(io->cookie,reg,1)) {
            return kIOReturnIOError;
        }
    }
    IOLog("NativeAdrenoHwInit: core register table begin\n");
    for(const auto &entry:kInitial) {
        if(io->write32(io->cookie,entry[0],entry[1])) {
            return kIOReturnIOError;
        }
    }

    IOLog("NativeAdrenoHwInit: UBWC mode registers begin\n");
    const uint32_t hbb=(highestBankBit-13)&3;
    const uint32_t modes[][2]={
        {0x8e08,0x10|(hbb<<1)},{0xb604,hbb<<1},
        {0xae02,0x20|(hbb<<1)},{0xe01,hbb<<21}
    };
    for(const auto &entry:modes) {
        if(io->write32(io->cookie,entry[0],entry[1])) {
            return kIOReturnIOError;
        }
    }
    IOLog("NativeAdrenoHwInit: CP protected register ranges begin\n");
    if(io->write32(io->cookie,0x84f,0xb)) {
        return kIOReturnIOError;
    }
    for(const auto &entry:kProtected) {
        const uint32_t encoded=entry[1]|((entry[2]-entry[1])<<18)|(entry[3]<<31);
        if(io->write32(io->cookie,0x850+entry[0],encoded)) {
            return kIOReturnIOError;
        }
    }
    IOLog("NativeAdrenoHwInit: core register setup completed\n");
    if(io->barrier(io->cookie)) {
        return kIOReturnIOError;
    }
    return kIOReturnSuccess;
}

IOReturn NativeAdrenoA6xxHwInit::programClockGating(const NativeAdrenoRingIo *io)
{
    if(!io || !io->write32 || !io->barrier) {
        return kIOReturnBadArgument;
    }
    IOLog("NativeAdrenoHwInit: RBBM hardware clock table begin\n");
    for(const auto &entry:kHwcg) {
        if(io->write32(io->cookie,entry[0],entry[1])) {
            return kIOReturnIOError;
        }
    }
    if(io->barrier(io->cookie)) {
        return kIOReturnIOError;
    }
    IOLog("NativeAdrenoHwInit: RBBM hardware clock table completed\n");
    return kIOReturnSuccess;
}

IOReturn NativeAdrenoA6xxHwInit::finish(const NativeAdrenoRingIo *io)
{
    if(!io || !io->write32 || !io->barrier) {
        return kIOReturnBadArgument;
    }
    IOLog("NativeAdrenoHwInit: GBIF halt release begin\n");
    if(io->write32(io->cookie,0x3c45,0) ||
        io->write32(io->cookie,0x16,0) || io->barrier(io->cookie)) {
        return kIOReturnIOError;
    }
    IOLog("NativeAdrenoHwInit: GBIF halt release completed\n");
    return kIOReturnSuccess;
}

IOReturn NativeAdrenoA6xxHwInit::stop(const NativeAdrenoRingIo *io,uint32_t pollLimit)
{
    if(!io || !io->read32 || !io->write32 || !pollLimit) {
        return kIOReturnBadArgument;
    }
    if(io->write32(io->cookie,0x38,0) || io->write32(io->cookie,0x808,3)) {
        return kIOReturnIOError;
    }
    const uint32_t halts[][3]={{0x16,0x17,1},{0x3c45,0x3c46,1},{0x3c45,0x3c46,2}};
    for(const auto &halt:halts) {
        if(io->write32(io->cookie,halt[0],halt[2])) {
            return kIOReturnIOError;
        }
        bool observed=false;
        for(uint32_t attempt=0;attempt<pollLimit;++attempt) {
            uint32_t ack=0;
            if(io->read32(io->cookie,halt[1],&ack)) {
                return kIOReturnIOError;
            }
            if((ack&halt[2])==halt[2]) {
                observed=true;
                break;
            }
            IODelay(1);
        }
        if(!observed) {
            return kIOReturnTimeout;
        }
    }
    return kIOReturnSuccess;
}
