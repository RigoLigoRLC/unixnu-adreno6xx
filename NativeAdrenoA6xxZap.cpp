/* SPDX-License-Identifier: MIT */
#include "NativeAdrenoA6xxZap.h"
#include <IOKit/IOLib.h>
#include <string.h>

extern "C" const uint8_t native_adreno_zap_metadata_image[];
extern "C" const uint64_t native_adreno_zap_metadata_image_size;
extern "C" const uint8_t native_adreno_zap_shader_image[];
extern "C" const uint64_t native_adreno_zap_shader_image_size;

namespace {
uint32_t word(const uint8_t *p)
{
    return uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24);
}

/* SCM arguments follow qcom_scm-smc.c: x1 encodes argument count and type,
 * x2 onward hold PAS ID and physical addresses. An interrupted call resumes
 * with x0=1 and the secure world's x6 continuation value. */
int64_t scm(uint32_t initialFunction,uint32_t info,uint64_t arg0,uint64_t arg1,
    uint64_t arg2,int64_t *transport)
{
    uint64_t function=initialFunction;
    uint64_t continuation=0;
    for(unsigned attempt=0;attempt<16;++attempt) {
        IOLog("NativeAdrenoZap: SCM begin attempt=%u function=%llx info=%x arg=%llx/%llx/%llx\n",
            attempt,function,info,arg0,arg1,arg2);
        register uint64_t x0 __asm__("x0")=function;
        register uint64_t x1 __asm__("x1")=info;
        register uint64_t x2 __asm__("x2")=arg0;
        register uint64_t x3 __asm__("x3")=arg1;
        register uint64_t x4 __asm__("x4")=arg2;
        register uint64_t x5 __asm__("x5")=0;
        register uint64_t x6 __asm__("x6")=continuation;
        register uint64_t x7 __asm__("x7")=0;
        __asm__ volatile("dsb sy\n\tsmc #0\n\tisb" :
            "+r"(x0),"+r"(x1),"+r"(x2),"+r"(x3),"+r"(x4),
            "+r"(x5),"+r"(x6),"+r"(x7) : :
            "x8","x9","x10","x11","x12","x13","x14","x15","x16","x17","memory","cc");
        const uint64_t result0=x0,result1=x1,result6=x6;
        *transport=int32_t(result0);
        IOLog("NativeAdrenoZap: SCM returned function=%x transport=%lld result=%lld continuation=%llx\n",
            initialFunction,*transport,int64_t(result1),result6);
        if(result0==1) {
            function=1;
            continuation=result6;
            continue;
        }
        if(int32_t(result0)==-12) {
            function=initialFunction;
            continuation=0;
            IOSleep(1);
            continue;
        }
        return int32_t(result1);
    }
    return -1;
}
}

IOReturn NativeAdrenoA6xxZap::load(const NativeAdrenoA6xxWindow &window,void *reservedCpu)
{
    if(phase_ || !reservedCpu || !window.physical || window.bytes<0x1000 ||
        native_adreno_zap_metadata_image_size<148 ||
        native_adreno_zap_shader_image_size>window.bytes) {
        return kIOReturnBadArgument;
    }
    phase_=1;
    const uint8_t *image=native_adreno_zap_metadata_image;
    if(word(image)!=0x464c457f || image[4]!=1 || image[5]!=1 ||
        word(image+28)!=52 || image[44]!=3 || image[45]!=0) {
        return kIOReturnBadArgument;
    }
    const uint8_t *shader=image+52+2*32;
    const uint32_t fileBytes=word(shader+16),memoryBytes=word(shader+20);
    if(word(shader)!=1 || !(word(shader+24)&(1u<<27)) ||
        fileBytes!=native_adreno_zap_shader_image_size || memoryBytes<fileBytes ||
        memoryBytes>window.bytes) {
        return kIOReturnBadArgument;
    }
    result_=scm(0xc2000601,1,0x02000601,0,0,&transport_);
    uint32_t pasBase=0x42000200;
    if(!transport_ && result_==1) {
        convention_=64;
    } else {
        result_=scm(0x82000601,1,0x02000601,0,0,&transport_);
        if(transport_ || result_!=1) {
            IOLog("NativeAdrenoZap: ARM SCM convention unavailable transport=%lld result=%lld\n",
                transport_,result_);
            return kIOReturnUnsupported;
        }
        convention_=32;
        pasBase=0x02000200;
        if(window.physical>UINT32_MAX || window.bytes>UINT32_MAX-window.physical) {
            return kIOReturnUnsupported;
        }
    }
    IOLog("NativeAdrenoZap: SCM convention=%u\n",convention_);

    const size_t metadataBytes=size_t((native_adreno_zap_metadata_image_size+4095)&~UINT64_C(4095));
    IOReturn status=metadata_.allocate(metadataBytes,convention_==64?40:32,true,metadataBytes);
    if(status) {
        return status;
    }
    IOLog("NativeAdrenoZap: metadata noncacheable mapping begin bytes=%lx\n",metadataBytes);
    status=metadata_.makeWriteCombined();
    IOLog("NativeAdrenoZap: metadata noncacheable mapping completed status=%08x\n",status);
    if(status) {
        return status;
    }
    memcpy(metadata_.mapping(),image,size_t(native_adreno_zap_metadata_image_size));
    IOLog("NativeAdrenoZap: metadata publication begin physical=%llx bytes=%lx\n",metadata_.spans()[0].physical,metadataBytes);
    status=metadata_.publishForDevice(0,metadataBytes,false);
    IOLog("NativeAdrenoZap: metadata publication completed status=%08x\n",status);
    if(status) {
        return status;
    }

    phase_=2;
    result_=scm(pasBase|1,0x82,13,metadata_.spans()[0].physical,0,&transport_);
    if(transport_ || result_) {
        return kIOReturnNotPermitted;
    }
    phase_=3;
    result_=scm(pasBase|2,3,13,window.physical,
        (uint64_t(memoryBytes)+4095)&~UINT64_C(4095),&transport_);
    if(transport_ || result_) {
        return kIOReturnNotPermitted;
    }

    phase_=4;
    IOLog("NativeAdrenoZap: reserved shader copy begin physical=%llx bytes=%u\n",window.physical,fileBytes);
    memcpy(reservedCpu,native_adreno_zap_shader_image,fileBytes);
    if(memoryBytes>fileBytes) {
        memset(static_cast<uint8_t *>(reservedCpu)+fileBytes,0,memoryBytes-fileBytes);
    }
    __asm__ volatile("dsb sy" ::: "memory");
    IOLog("NativeAdrenoZap: reserved shader copy completed\n");

    phase_=5;
    result_=scm(pasBase|5,1,13,0,0,&transport_);
    if(transport_ || result_) {
        return kIOReturnNotPermitted;
    }
    phase_=6;
    return kIOReturnSuccess;
}
