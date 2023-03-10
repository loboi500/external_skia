/*
 * Copyright 2015 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "include/mtk/SkJpegCodec_MTK.h"

#include "include/codec/SkCodec.h"
#include "include/core/SkStream.h"
#include "include/core/SkTypes.h"
#include "include/private/SkColorData.h"
#include "include/private/SkTemplates.h"
#include "include/private/SkTo.h"
#include "src/codec/SkCodecPriv.h"
#include "include/mtk/SkJpegDecoderMgr_MTK.h"
#include "src/codec/SkParseEncodedOrigin.h"
#include "src/pdf/SkJpegInfo.h"
#include "native_handle.h"
#include "oalinterface.h"
#include "include/core/SkTime.h"

// stdio is needed for libjpeg-turbo
#include <stdio.h>
#include "src/codec/SkJpegUtility.h"

#ifdef MTK_JPEG_HW_REGION_RESIZER
#include "client_utils/android/FrontBufferedStream.h"
#include <cutils/properties.h>
#include <cutils/log.h>
#include "DpColorFormat.h"
#include <ion/ion.h>
#include <linux/ion.h>
#include <linux/ion_drv.h>

#include <vendor/mediatek/hardware/mms/1.2/IMms.h>
#include <vendor/mediatek/hardware/mms/1.3/IMms.h>
using ::vendor::mediatek::hardware::mms::V1_3::IMms;
using ::vendor::mediatek::hardware::mms::V1_3::MDPParamFD;
using ::vendor::mediatek::hardware::mms::V1_2::MMS_PROFILE_ENUM;
using ::vendor::mediatek::hardware::mms::V1_2::MMS_MEDIA_TYPE_ENUM;
using namespace android;

#undef LOG_TAG
#define LOG_TAG "skia"
#define MAX_APP1_HEADER_SIZE 8 * 1024
#define TO_CEIL(x,a) ( ( (unsigned long)(x) + ((a)-1)) & ~((a)-1) )
#define ION_HEAP_MULTIMEDIA_MASK (1 << 10)
#define MAX_HEAP_COUNT  ION_HEAP_TYPE_CUSTOM

void freeBuffer(int ionClientHnd, ion_user_handle_t ionAllocHnd, void* bufferAddr, int bufferFD, size_t size)
{
    if (ionClientHnd >= 0)
    {
        if(bufferAddr != NULL)
        {
            int ret = munmap(bufferAddr, size);
            if (ret < 0)
                SkCodecPrintf("%s munmap failed (%d, %p, %zu)\n",  __func__, ionClientHnd, bufferAddr, size);
        }
        if (bufferFD != -1)
        {
            if (close(bufferFD))
            {
                SkCodecPrintf("%s close failed (%d, %d)\n",  __func__, ionClientHnd, bufferFD);
            }
        }
        if (ion_free(ionClientHnd, ionAllocHnd))
        {
            SkCodecPrintf("%s ion_free failed (%d, %d)\n",  __func__, ionClientHnd, bufferFD);
        }
    }
    else
    {
        if(bufferAddr != NULL)
        {
            int ret = munmap(bufferAddr, size);
            if (ret < 0)
                SkCodecPrintf("%s munmap failed (%p, %zu)\n",  __func__, bufferAddr, size);
        }
        if (bufferFD != -1)
        {
            if (close(bufferFD))
            {
                SkCodecPrintf("%s close failed (%d)\n",  __func__, bufferFD);
            }
        }
    }
}

void* allocateBuffer(int ionClientHnd, BufferAllocator *bufferAllocator, ion_user_handle_t *ionAllocHnd, int *bufferFD, size_t size)
{
    int ret;
    unsigned char *bufAddr = 0;
    if (ionClientHnd >= 0)
    {
        ret = ion_alloc(ionClientHnd, size, 0, ION_HEAP_MULTIMEDIA_MASK, ION_FLAG_CACHED | ION_FLAG_CACHED_NEEDS_SYNC, ionAllocHnd);
        if (ret)
        {
             SkCodecPrintf("%s ion_alloc failed (%d, %zu, %d)\n", __func__, ionClientHnd, size, *ionAllocHnd);
            return 0;
        }
        ret = ion_map(ionClientHnd, *ionAllocHnd, size, PROT_READ | PROT_WRITE, MAP_SHARED, 0, &bufAddr, bufferFD);
        if (ret)
        {
            SkCodecPrintf("%s ion_mmap failed (%d, %zu, %d)\n", __func__, ionClientHnd, size, *bufferFD);
            freeBuffer(ionClientHnd, *ionAllocHnd, (void*) bufAddr, *bufferFD, size);
            return 0;
        }
    }
    else //dma buf
    {
        *bufferFD = DmabufHeapAlloc(bufferAllocator, "mtk_mm", size, 0, 0);
        if (*bufferFD < 0)
        {
            SkCodecPrintf("%s DmabufHeapAlloc fail, bufferallocator %p fd: %d size: %zu\n", __func__, bufferAllocator, *bufferFD, size);
            *bufferFD = -1;
            return 0;
        }

        /* Create memory mapped buffer for the buffer fd */
        bufAddr = (unsigned char *)mmap(NULL, size, PROT_READ|PROT_WRITE, MAP_SHARED, *bufferFD, 0);
        if (bufAddr == MAP_FAILED) {
            SkCodecPrintf("%s  mmap failed (%zu, %d)\n",  __func__, size, *bufferFD);
            close(*bufferFD);
            return 0;
        }
    }
    return bufAddr;
}

int Buf_flush(int ionClientHnd, BufferAllocator *bufferAllocator, unsigned int size, void* bufAddr, ion_user_handle_t handle, int bufferFD)
{
    struct ion_sys_data sys_data;
    int ret = 0;
    if (ionClientHnd > 0)
    {
        sys_data.sys_cmd = ION_SYS_CACHE_SYNC;
        sys_data.cache_sync_param.va = bufAddr;
        sys_data.cache_sync_param.handle = handle;
        sys_data.cache_sync_param.size = size;
        sys_data.cache_sync_param.sync_type = ION_CACHE_FLUSH_BY_RANGE;
        ret = ion_custom_ioctl(ionClientHnd, ION_CMD_SYSTEM, &sys_data);
        if (ret)
        {
            SkCodecPrintf("ion_custom_ioctl ION_DMA_FLUSH_BY_RANGE_USE_VA failed. (%s)\n", strerror(errno));
        }
    }
    else
    {
        ret = DmabufHeapCpuSyncStart(bufferAllocator, bufferFD, kSyncReadWrite, nullptr, nullptr);
        if (ret)
        {
            SkCodecPrintf("DmabufHeapCpuSyncStart failed: %d\n", ret);
            return 0;
        }

        ret = DmabufHeapCpuSyncEnd(bufferAllocator, bufferFD, kSyncReadWrite, nullptr, nullptr);
        if (ret)
        {
            SkCodecPrintf("DmabufHeapCpuSyncEnd failed: %d\n", ret);
            return 0;
        }
    }

    return ret;
}

unsigned int getISOSpeedRatings(void *buffer, unsigned int size)
{
    unsigned char *bufPtr = (unsigned char *)buffer;
    unsigned char *TIFFPtr;
    int bytesLeft = size;
    bool findAPP1Marker = false;
    bool findTIFFHeader = false;
    bool isIntelAlign = true;
    bool findEXIFOffset = false;
    bool findISOSpeedRatings = false;
    unsigned int APP1Length;
    unsigned int IFDOffset;
    unsigned int EXIFOffset;
    unsigned int nextIFDOffset;

    /* find APP1 marker*/
    while(bytesLeft > 1)
    {
        if ((0xFF == *bufPtr) && (0xE1 == *(bufPtr + 1)))
        {
            findAPP1Marker = true;
            break;
        }
        bufPtr++;
        bytesLeft--;
    }
    if (findAPP1Marker == true && bytesLeft >= 4)
    {
        // get APP1 marker length as search limit size
        APP1Length = (*(bufPtr + 2) << 8) + *(bufPtr + 3);
        if (bytesLeft > (int)APP1Length)
            bytesLeft = (int)APP1Length;
    }
    else
        return 0;

    /* find TIFF header */
    while(bytesLeft >= 4)
    {
        // Intel type byte alignment
        if (((0x49 == *bufPtr) && (0x49 == *(bufPtr + 1)) &&
            (0x2a == *(bufPtr + 2)) && (0x00 == *(bufPtr + 3))))
        {
            findTIFFHeader = true;
            break;
        }
        // Motorola type byte alignment
        else if (((0x4d == *bufPtr) && (0x4d == *(bufPtr + 1)) &&
            (0x00 == *(bufPtr + 2)) && (0x2a == *(bufPtr + 3))))
        {
            findTIFFHeader = true;
            isIntelAlign = false;
            break;
        }
        bufPtr++;
        bytesLeft--;
    }
    if (findTIFFHeader == true && bytesLeft >= 8)
    {
        // get IFD offset to reach IFD position
        TIFFPtr = bufPtr;
        if (isIntelAlign == true)
            IFDOffset = (*(bufPtr + 7) << 24) + (*(bufPtr + 6) << 16)
                               + (*(bufPtr + 5) << 8) + *(bufPtr + 4);
        else
            IFDOffset = (*(bufPtr + 4) << 24) + (*(bufPtr + 5) << 16)
                        + (*(bufPtr + 6) << 8) + *(bufPtr + 7);
        if (bytesLeft >= (int)IFDOffset && (int)IFDOffset >= 0)
        {
            bufPtr += IFDOffset;
            bytesLeft -= IFDOffset;
        }
        else
            return 0;
    }
    else
        return 0;

    /* find IFD tag ExifOffset */
    while(findEXIFOffset == false && bytesLeft >= 2)
    {
        unsigned int dirEntries;
        if (isIntelAlign == true)
            dirEntries = (*(bufPtr + 1) << 8) + *bufPtr;
        else
            dirEntries = (*bufPtr << 8) + *(bufPtr + 1);
        bufPtr += 2;
        bytesLeft -= 2;
        while(dirEntries > 0 && bytesLeft >= 12)
        {
            if ((isIntelAlign == true && (0x69 == *bufPtr) && (0x87 == *(bufPtr + 1))) ||
                 (isIntelAlign == false && (0x87 == *bufPtr) && (0x69 == *(bufPtr + 1))))
            {
                if (isIntelAlign == true)
                    EXIFOffset = (*(bufPtr + 11) << 24) + (*(bufPtr + 10) << 16)
                                        + (*(bufPtr + 9) << 8) + *(bufPtr + 8);
                else
                    EXIFOffset = (*(bufPtr + 8) << 24) + (*(bufPtr + 9) << 16)
                                 + (*(bufPtr + 10) << 8) + *(bufPtr + 11);
                if (EXIFOffset - ((unsigned long)bufPtr - (unsigned long)TIFFPtr) > (unsigned long)bytesLeft) // EXIFOffset is invalid, ignore the next step
                    return 0;
                unsigned char *EXIFPtr = TIFFPtr + EXIFOffset;
                bytesLeft -= (unsigned long)EXIFPtr - (unsigned long)bufPtr;
                if (bytesLeft > 0)
                {
                    bufPtr = EXIFPtr;
                    findEXIFOffset = true;
                    break;
                }
                else
                    return 0;
            }
            dirEntries--;
            bufPtr += 12;
            bytesLeft -= 12;
        }

        /* move to next IFD */
        if (dirEntries == 0 && findEXIFOffset == false && bytesLeft >= 4)
        {
            if (isIntelAlign == true)
                nextIFDOffset = (*(bufPtr + 3) << 24) + (*(bufPtr + 2) << 16)
                                        + (*(bufPtr + 1) << 8) + *(bufPtr);
            else
                nextIFDOffset = (*(bufPtr) << 24) + (*(bufPtr + 1) << 16)
                                + (*(bufPtr + 2) << 8) + *(bufPtr + 3);

            // There is no next IFD existed, so abort the searching
            if (nextIFDOffset == 0 || nextIFDOffset > (bufPtr - TIFFPtr + (unsigned int)bytesLeft))
                return 0;
            unsigned char *nextIFDPtr = TIFFPtr + nextIFDOffset;
            bytesLeft -= (unsigned long)nextIFDPtr - (unsigned long)bufPtr;
            if (bytesLeft > 0)
                bufPtr = nextIFDPtr;
            else
                return 0;
        }
    }
    if (findEXIFOffset == true && bytesLeft >= 12)
    {
        unsigned int ISOSpeedRatings = 0;
        unsigned int dirEntries;
        if (isIntelAlign == true)
            dirEntries = (*(bufPtr + 1) << 8) + *bufPtr;
        else
            dirEntries = (*bufPtr << 8) + *(bufPtr + 1);
        bufPtr += 2;
        bytesLeft -= 2;
        while(dirEntries > 0 && bytesLeft >= 2)
        {
            /* find IFD tag ISOSpeedRatings */
            if ((isIntelAlign == true && (0x27 == *bufPtr) && (0x88 == *(bufPtr + 1))) ||
                (isIntelAlign == false && (0x88 == *bufPtr) && (0x27 == *(bufPtr + 1))))
            {
                if (isIntelAlign == true)
                    ISOSpeedRatings = (*(bufPtr + 9) << 8) + *(bufPtr + 8);
                else
                    ISOSpeedRatings = (*(bufPtr + 8) << 8) + *(bufPtr + 9);
                findISOSpeedRatings = true;
                break;
            }
            dirEntries--;
            bufPtr += 12;
            bytesLeft -= 12;
        }
        if (findISOSpeedRatings == true)
            return ISOSpeedRatings;
        else
            return 0;
    }
    else
        return 0;
}

bool ImgPostProc(void* src, int ionClientHnd, BufferAllocator *bufferAllocator, int srcFD, void* dst, int width, int height, int rowBytes,
                     SkColorType colorType, int tdsp, void* pPPParam, unsigned int ISOSpeed)
{
    if(NULL == dst)
    {
        SkCodecPrintf("ImgPostProc : null pixels");
        return false;
    }
    if((colorType == kRGBA_8888_SkColorType) ||
       (colorType == kRGB_565_SkColorType))
    {
        sp<IMms> IMms_service = IMms::getService();
        if (IMms_service == nullptr)
        {
            SkCodecPrintf("cannot find IMms_service!");
            return false;
        }
        MDPParamFD mdpParam;
        native_handle_t *srcHdl;
        native_handle_t *dstHdl;
        memset(&mdpParam, 0, sizeof(MDPParamFD));
        mdpParam.src_planeNumber = 1;
        unsigned int src_size = 0;
        unsigned int src_pByte = 4;

        if(colorType == kRGBA_8888_SkColorType)
        {
            mdpParam.dst_format = eRGBX8888;
            src_pByte = 4;
        }
        else
        {
            mdpParam.dst_format = eRGB565;
            src_pByte = 2;
        }
        mdpParam.src_format = mdpParam.dst_format ;
        src_size = rowBytes * height;
        mdpParam.src_sizeList[0] = src_size;
        SkCodecPrintf("ImgPostProc: wh (%d %d)->(%d %d), fmt %d, size %d->%d, regionPQ %d!!\n", width, height, width, height
        , colorType, src_size, src_size, tdsp);

        mdpParam.pq_param.enable = (tdsp == 0)? false:true;
        mdpParam.pq_param.scenario = MMS_MEDIA_TYPE_ENUM::MMS_MEDIA_PICTURE;
        mdpParam.pq_param.iso = ISOSpeed;

        if (pPPParam)
        {
            SkCodecPrintf("ImgPostProc: enable imgDc pParam %p", pPPParam);
        }

        if (srcFD >= 0)
        {
            srcHdl = native_handle_create(1, 0);
            if (srcHdl != nullptr)
            {
                srcHdl->data[0] = srcFD;
                mdpParam.inputHandle = srcHdl;
            }
        }
        else
        {
            SkCodecPrintf("ImgPostProc: src fd %d < 0", srcFD);
            return false;
        }

        mdpParam.src_rect.x = 0;
        mdpParam.src_rect.y = 0;
        mdpParam.src_rect.w = width;
        mdpParam.src_rect.h = height;
        mdpParam.src_width = width;
        mdpParam.src_height = height;
        mdpParam.src_yPitch = rowBytes;
        mdpParam.src_profile = MMS_PROFILE_ENUM::MMS_PROFILE_JPEG;

        // set dst buffer
        ion_user_handle_t ionAllocHnd = 0;
        int dstFD = -1;
        void* dstBuffer = NULL;
        unsigned int dst_size = 0;
        mdpParam.dst_planeNumber = 1;

        dst_size = src_size;
        mdpParam.dst_sizeList[0] = dst_size;
        dstBuffer = allocateBuffer(ionClientHnd, bufferAllocator, &ionAllocHnd, &dstFD, dst_size);

        dstHdl = native_handle_create(1, 0);
        if (dstHdl != nullptr)
        {
            dstHdl->data[0] = dstFD;
            mdpParam.outputHandle = dstHdl;
        }
        SkCodecPrintf("ImgPostProc allocateBuffer src:(%d), dst:(%d, %d, %d, %d, 0x%x)",
                srcFD, ionClientHnd, ionAllocHnd, dstFD, dst_size, mdpParam.dst_MVAList[0]);

        mdpParam.dst_rect.x = 0;
        mdpParam.dst_rect.y = 0;
        mdpParam.dst_rect.w = width;
        mdpParam.dst_rect.h = height;
        mdpParam.dst_width = width;
        mdpParam.dst_height = height;
        mdpParam.dst_yPitch = rowBytes;
        mdpParam.dst_profile = MMS_PROFILE_ENUM::MMS_PROFILE_JPEG;

        auto ret = IMms_service->BlitStreamFD(mdpParam);
        if (!ret.isOk())
        {
            SkCodecPrintf("IMms_service->BlitStreamFD failed");
            if (dstBuffer != NULL)
            {
                freeBuffer(ionClientHnd, ionAllocHnd, dstBuffer, dstFD, src_size);
            }
            native_handle_delete(srcHdl);
            native_handle_delete(dstHdl);
            return false;
        }

        // if dstBuffer is not NULL, need to copy pixels to bitmap and free ION buffer
        if (dstBuffer != NULL)
        {
            memcpy(dst, dstBuffer, src_size);
            freeBuffer(ionClientHnd, ionAllocHnd, dstBuffer, dstFD, src_size);
        }

        native_handle_delete(srcHdl);
        native_handle_delete(dstHdl);
        return true;
    }
    return false;
}
#endif

#ifdef MTK_JPEG_SW_OPTIMIZATION

#define READ_BUFFER_SIZE 1024 * 1024
#define MIN_WIDTH_APPLY_OPT 256 // image width need >= 256 when applying opt.
#define MIN_SIZE_APPLY_OPT 1000000 // No need to apply opt. when image resolution smaller than 1M

bool LoadInputStreamToMem(int IonClientHnd, BufferAllocator *bufferAllocator, SkBufMalloc *allocMemory, SkStream* stream, SkStream **mstream)
{
    int handle;
    size_t curStreamPosition = stream->getPosition();
    if(stream->rewind() && allocMemory != nullptr)
    {
        size_t length = stream->getLength();
        allocMemory->setAlignment(128);
        size_t bytes_read = 0;
        void *ptr;
        if (length)
        {
            ptr = allocMemory->reset((((length+127)>>7)<<7)+128+2048);
            if (ptr == nullptr)
            {
                SkCodecPrintf("allocMemory->reset failed.");
                if (curStreamPosition != 0 && !stream->seek(curStreamPosition))
                    SkCodecPrintf("Cannot seek back to original position L:%d", __LINE__);
                return false;
            }
            bytes_read = stream->read(allocMemory->getAddr(), length);
            if (bytes_read == 0)
            {
                SkCodecPrintf("SkStream bytes_read = 0, rollback to no opt. flow");
                if (curStreamPosition != 0 && !stream->seek(curStreamPosition))
                    SkCodecPrintf("Cannot seek back to original position L:%d", __LINE__);
                return false;
            }
            SkCodecPrintf("stream getLength() supported, buffer addr %p length %zu\n", allocMemory->getAddr(), allocMemory->getSize());
        }
        // stream does not support length, need to use temp buffer for loading stream
        else
        {
            SkAutoTMalloc<uint8_t> tmpReadBuffer;
            SkAutoTMalloc<uint8_t> tmpBuffer;
            tmpReadBuffer.reset(READ_BUFFER_SIZE);
            if(tmpReadBuffer == nullptr)
                return false;
            bytes_read = stream->read(tmpReadBuffer.get(), READ_BUFFER_SIZE);
            while(0 != bytes_read)
            {
                if (length != 0)
                {
                    tmpBuffer.reset(length);
                    if(tmpBuffer == nullptr)
                        return false;
                    // copy current bitstream content to a temp buffer
                    memcpy(tmpBuffer.get(), allocMemory->getAddr(), length);
                }
                allocMemory->reset((((length + bytes_read + 127)>>7)<<7)+128+2048);
                if(allocMemory->getAddr() == nullptr)
                    return false;
                if (length != 0)
                    // copy content from temp buffer to target buffer
                    memcpy(allocMemory->getAddr(), tmpBuffer.get(), length);

                // append read data to the end of target stream buffer
                memcpy((void*)((unsigned char*)allocMemory->getAddr() + length), tmpReadBuffer.get(), bytes_read);
                length += bytes_read;
                bytes_read = stream->read(tmpReadBuffer.get(), READ_BUFFER_SIZE);
            }
            SkCodecPrintf("stream getLength() not supported, use temp buffer for loading stream, buffer addr %p length %zu\n", allocMemory->getAddr(), length);
        }
        *mstream = new SkMemoryStream(allocMemory->getAddr(), length, true);
        handle = allocMemory->getIonAllocHnd();
        if (Buf_flush(IonClientHnd, bufferAllocator, length, allocMemory->getAddr(), handle, allocMemory->getFD()))
            SkCodecPrintf("LoadInputStreamToMem Buf_flush failed");
        SkCodecPrintf("LoadInputStreamToMem va %p  size %zu", allocMemory->getAddr(), allocMemory->getSize());
        return true;
    }
    else
    {
        SkCodecPrintf("stream rewind not support");
        return false;
    }
}

class JpgStreamAutoClean {
public:
    JpgStreamAutoClean(): ptr(nullptr) {}
    ~JpgStreamAutoClean() {
        if (ptr) {
            delete ptr;
        }
    }
    void set(SkStream* s) {
        ptr = s;
    }
private:
    SkStream* ptr;
};
#endif

// This warning triggers false postives way too often in here.
#if defined(__GNUC__) && !defined(__clang__)
    #pragma GCC diagnostic ignored "-Wclobbered"
#endif

extern "C" {
    #include "jerror_alpha.h"
    #include "jpeglib_alpha.h"
}

bool SkJpegCodec::IsJpeg(const void* buffer, size_t bytesRead) {
    constexpr uint8_t jpegSig[] = { 0xFF, 0xD8, 0xFF };
    return bytesRead >= 3 && !memcmp(buffer, jpegSig, sizeof(jpegSig));
}

const uint32_t kExifHeaderSize = 14;
const uint32_t kExifMarker = JPEG_APP0 + 1;

static bool is_orientation_marker(jpeg_marker_struct_ALPHA* marker, SkEncodedOrigin* orientation) {
    if (kExifMarker != marker->marker || marker->data_length < kExifHeaderSize) {
        return false;
    }

    constexpr uint8_t kExifSig[] { 'E', 'x', 'i', 'f', '\0' };
    if (memcmp(marker->data, kExifSig, sizeof(kExifSig))) {
        return false;
    }

    // Account for 'E', 'x', 'i', 'f', '\0', '<fill byte>'.
    constexpr size_t kOffset = 6;
    return SkParseEncodedOrigin(marker->data + kOffset, marker->data_length - kOffset,
            orientation);
}

static SkEncodedOrigin get_exif_orientation(jpeg_decompress_struct_ALPHA* dinfo) {
    SkEncodedOrigin orientation;
    for (jpeg_marker_struct_ALPHA* marker = dinfo->marker_list; marker; marker = marker->next) {
        if (is_orientation_marker(marker, &orientation)) {
            return orientation;
        }
    }

    return kDefault_SkEncodedOrigin;
}

static bool is_icc_marker(jpeg_marker_struct_ALPHA* marker) {
    if (kICCMarker != marker->marker || marker->data_length < kICCMarkerHeaderSize) {
        return false;
    }

    return !memcmp(marker->data, kICCSig, sizeof(kICCSig));
}

/*
 * ICC profiles may be stored using a sequence of multiple markers.  We obtain the ICC profile
 * in two steps:
 *     (1) Discover all ICC profile markers and verify that they are numbered properly.
 *     (2) Copy the data from each marker into a contiguous ICC profile.
 */
static std::unique_ptr<SkEncodedInfo::ICCProfile> read_color_profile(jpeg_decompress_struct_ALPHA* dinfo)
{
    // Note that 256 will be enough storage space since each markerIndex is stored in 8-bits.
    jpeg_marker_struct_ALPHA* markerSequence[256];
    memset(markerSequence, 0, sizeof(markerSequence));
    uint8_t numMarkers = 0;
    size_t totalBytes = 0;

    // Discover any ICC markers and verify that they are numbered properly.
    for (jpeg_marker_struct_ALPHA* marker = dinfo->marker_list; marker; marker = marker->next) {
        if (is_icc_marker(marker)) {
            // Verify that numMarkers is valid and consistent.
            if (0 == numMarkers) {
                numMarkers = marker->data[13];
                if (0 == numMarkers) {
                    SkCodecPrintf("ICC Profile Error: numMarkers must be greater than zero.\n");
                    return nullptr;
                }
            } else if (numMarkers != marker->data[13]) {
                SkCodecPrintf("ICC Profile Error: numMarkers must be consistent.\n");
                return nullptr;
            }

            // Verify that the markerIndex is valid and unique.  Note that zero is not
            // a valid index.
            uint8_t markerIndex = marker->data[12];
            if (markerIndex == 0 || markerIndex > numMarkers) {
                SkCodecPrintf("ICC Profile Error: markerIndex is invalid.\n");
                return nullptr;
            }
            if (markerSequence[markerIndex]) {
                SkCodecPrintf("ICC Profile Error: Duplicate value of markerIndex.\n");
                return nullptr;
            }
            markerSequence[markerIndex] = marker;
            SkASSERT(marker->data_length >= kICCMarkerHeaderSize);
            totalBytes += marker->data_length - kICCMarkerHeaderSize;
        }
    }

    if (0 == totalBytes) {
        // No non-empty ICC profile markers were found.
        return nullptr;
    }

    // Combine the ICC marker data into a contiguous profile.
    sk_sp<SkData> iccData = SkData::MakeUninitialized(totalBytes);
    void* dst = iccData->writable_data();
    for (uint32_t i = 1; i <= numMarkers; i++) {
        jpeg_marker_struct_ALPHA* marker = markerSequence[i];
        if (!marker) {
            SkCodecPrintf("ICC Profile Error: Missing marker %d of %d.\n", i, numMarkers);
            return nullptr;
        }

        void* src = SkTAddOffset<void>(marker->data, kICCMarkerHeaderSize);
        size_t bytes = marker->data_length - kICCMarkerHeaderSize;
        memcpy(dst, src, bytes);
        dst = SkTAddOffset<void>(dst, bytes);
    }

    return SkEncodedInfo::ICCProfile::Make(std::move(iccData));
}

SkCodec::Result SkJpegCodec::ReadHeader(SkStream* stream, SkCodec** codecOut,
        JpegDecoderMgr_MTK** decoderMgrOut,
        std::unique_ptr<SkEncodedInfo::ICCProfile> defaultColorProfile) {

#if defined(MTK_JPEG_HW_REGION_RESIZER)
    // assume maximum header size is 256KB
    #define JPEG_HEADER_SIZE 256 * 1024
    if (codecOut && (!stream->hasLength() || !stream->hasPosition()))
    {
        //SkCodecPrintf("SkJpegCodec::ReadHeader wrap stream with FrontBufferedStream");
        stream = FrontBufferedStream::Make(std::unique_ptr<SkStream>(stream), JPEG_HEADER_SIZE).release();
    }
#endif

    // Create a JpegDecoderMgr to own all of the decompress information
    std::unique_ptr<JpegDecoderMgr_MTK> decoderMgr(new JpegDecoderMgr_MTK(stream));

    // libjpeg errors will be caught and reported here
    skjpeg_error_mgr_MTK::AutoPushJmpBuf jmp(decoderMgr->errorMgr_MTK());
    if (setjmp(jmp)) {
        return decoderMgr->returnFailure_MTK("ReadHeader", kInvalidInput);
    }

    jpeg_std_oal_ALPHA((j_common_ptr_ALPHA)decoderMgr->dinfo_MTK(), decoderMgr->oal_MTK());
    decoderMgr->oal_MTK()->queryHAL = Codec_queryHAL_Empty;
    // Initialize the decompress info and the source manager
    decoderMgr->init_MTK();
    auto* dinfo = decoderMgr->dinfo_MTK();
    // Instruct jpeg library to save the markers that we care about.  Since
    // the orientation and color profile will not change, we can skip this
    // step on rewinds.
    if (codecOut) {
        jpeg_save_markers_ALPHA(decoderMgr->dinfo_MTK(), kExifMarker, 0xFFFF);
        jpeg_save_markers_ALPHA(decoderMgr->dinfo_MTK(), kICCMarker, 0xFFFF);
    }

    // Read the jpeg header
    switch (jpeg_read_header_ALPHA(decoderMgr->dinfo_MTK(), true)) {
        case JPEG_HEADER_OK_ALPHA:
            break;
        case JPEG_SUSPENDED_ALPHA:
            return decoderMgr->returnFailure_MTK("ReadHeader", kIncompleteInput);
        default:
            return decoderMgr->returnFailure_MTK("ReadHeader", kInvalidInput);
    }

    if (codecOut) {
        // Get the encoded color type
        SkEncodedInfo::Color color;
        if (!decoderMgr->getEncodedColor_MTK(&color)) {
            return kInvalidInput;
        }

        SkEncodedOrigin orientation = get_exif_orientation(dinfo);
        auto profile = read_color_profile(dinfo);
        if (profile) {
            auto type = profile->profile()->data_color_space;
            switch (decoderMgr->dinfo_MTK()->jpeg_color_space) {
                case JCS_CMYK_ALPHA:
                case JCS_YCCK_ALPHA:
                    if (type != skcms_Signature_CMYK) {
                        profile = nullptr;
                    }
                    break;
                case JCS_GRAYSCALE_ALPHA:
                    if (type != skcms_Signature_Gray &&
                        type != skcms_Signature_RGB)
                    {
                        profile = nullptr;
                    }
                    break;
                default:
                    if (type != skcms_Signature_RGB) {
                        profile = nullptr;
                    }
                    break;
            }
        }
        if (!profile) {
            profile = std::move(defaultColorProfile);
        }

        SkEncodedInfo info = SkEncodedInfo::Make(dinfo->image_width, dinfo->image_height,
                                                 color, SkEncodedInfo::kOpaque_Alpha, 8,
                                                 std::move(profile));

        SkJpegCodec* codec = new SkJpegCodec(std::move(info), std::unique_ptr<SkStream>(stream),
                                             decoderMgr.release(), orientation);
        *codecOut = codec;
    } else {
        SkASSERT(nullptr != decoderMgrOut);
        *decoderMgrOut = decoderMgr.release();
    }
    return kSuccess;
}

std::unique_ptr<SkCodec> SkJpegCodec::MakeFromStream(std::unique_ptr<SkStream> stream,
                                                     Result* result) {
    return SkJpegCodec::MakeFromStream(std::move(stream), result, nullptr);
}

std::unique_ptr<SkCodec> SkJpegCodec::MakeFromStream(std::unique_ptr<SkStream> stream,
        Result* result, std::unique_ptr<SkEncodedInfo::ICCProfile> defaultColorProfile) {
    SkCodec* codec = nullptr;
    *result = ReadHeader(stream.get(), &codec, nullptr, std::move(defaultColorProfile));
    if (kSuccess == *result) {
        // Codec has taken ownership of the stream, we do not need to delete it
        SkASSERT(codec);
        stream.release();
        return std::unique_ptr<SkCodec>(codec);
    }
    return nullptr;
}

SkJpegCodec::SkJpegCodec(SkEncodedInfo&& info, std::unique_ptr<SkStream> stream,
                         JpegDecoderMgr_MTK* decoderMgr, SkEncodedOrigin origin)
    : INHERITED(std::move(info), skcms_PixelFormat_RGBA_8888, std::move(stream), origin)
    , fDecoderMgr(decoderMgr)
    , fReadyState(decoderMgr->dinfo_MTK()->global_state)
    , fSwizzleSrcRow(nullptr)
    , fColorXformSrcRow(nullptr)
    , fSwizzlerSubset(SkIRect::MakeEmpty())
    , fInfoDump(false)
{
#ifdef MTK_JPEG_HW_REGION_RESIZER
    if(!BufferAllocator::CheckIonSupport()) {
        fBufferAllocator = CreateDmabufHeapBufferAllocator();
        fIonClientHnd = -1;
    }
    else
    {
        fBufferAllocator = nullptr;
        fIonClientHnd = ion_open();
        if (fIonClientHnd < 0)
        {
            SkCodecPrintf("ion_open failed\n");
            fIonClientHnd = -1;
        }
    }

    fISOSpeedRatings = -1;
    fBufferStorage = NULL;
    fIsSampleDecode = false;
    fSampleDecodeY = 0;

    fFirstTileDone = false;
    fUseHWResizer = false;
    fEnTdshp = false;
    fRegionHeight = 0x0;
#endif
    fBitstreamBuf = new SkBufMalloc(fIonClientHnd, fBufferAllocator);
    fMemStream = NULL;
    if(fInfoDump.enable())
        fInfoDump.add(CODECTYPE, "JPEG");
}

#ifdef MTK_JPEG_HW_REGION_RESIZER
SkJpegCodec::~SkJpegCodec()
{
    JpegDecoderMgr_MTK* decoderMgr = fDecoderMgr.release();
    if (decoderMgr)
        delete decoderMgr;
    if (fBufferStorage)
    {
        delete fBufferStorage;
        fBufferStorage = NULL;
    }
    if (fBitstreamBuf)
    {
        delete fBitstreamBuf;
        fBitstreamBuf = NULL;
    }
    if (fMemStream)
    {
        delete fMemStream;
        fMemStream = NULL;
    }
    if (fIonClientHnd != -1)
        ion_close(fIonClientHnd);
    else
        FreeDmabufHeapBufferAllocator(fBufferAllocator);
}
#endif

/*
 * Return the row bytes of a particular image type and width
 */
static size_t get_row_bytes(const j_decompress_ptr_ALPHA dinfo) {
    const size_t colorBytes = (dinfo->out_color_space == JCS_RGB565) ? 2 :
            dinfo->out_color_components;
    return dinfo->output_width * colorBytes;

}

/*
 *  Calculate output dimensions based on the provided factors.
 *
 *  Not to be used on the actual jpeg_decompress_struct used for decoding, since it will
 *  incorrectly modify num_components.
 */
void calc_output_dimensions(jpeg_decompress_struct_ALPHA* dinfo, unsigned int num, unsigned int denom) {
    dinfo->num_components = 0;
    dinfo->scale_num = num;
    dinfo->scale_denom = denom;
    jpeg_calc_output_dimensions_ALPHA(dinfo);
}

/*
 * Return a valid set of output dimensions for this decoder, given an input scale
 */
SkISize SkJpegCodec::onGetScaledDimensions(float desiredScale) const {
    // libjpeg-turbo supports scaling by 1/8, 1/4, 3/8, 1/2, 5/8, 3/4, 7/8, and 1/1, so we will
    // support these as well
    unsigned int num;
    unsigned int denom = 8;
    if (desiredScale >= 0.9375) {
        num = 8;
    } else if (desiredScale >= 0.8125) {
        num = 7;
    } else if (desiredScale >= 0.6875f) {
        num = 6;
    } else if (desiredScale >= 0.5625f) {
        num = 5;
    } else if (desiredScale >= 0.4375f) {
        num = 4;
    } else if (desiredScale >= 0.3125f) {
        num = 3;
    } else if (desiredScale >= 0.1875f) {
        num = 2;
    } else {
        num = 1;
    }

    // Set up a fake decompress struct in order to use libjpeg to calculate output dimensions
    jpeg_decompress_struct_ALPHA dinfo;
    sk_bzero(&dinfo, sizeof(dinfo));
    dinfo.image_width = this->dimensions().width();
    dinfo.image_height = this->dimensions().height();
    dinfo.global_state = fReadyState;
    calc_output_dimensions(&dinfo, num, denom);

    // Return the calculated output dimensions for the given scale
    return SkISize::Make(dinfo.output_width, dinfo.output_height);
}

bool SkJpegCodec::onRewind() {
    JpegDecoderMgr_MTK* decoderMgr = nullptr;
    if (kSuccess != ReadHeader(this->stream(), nullptr, &decoderMgr, nullptr)) {
        return fDecoderMgr->returnFalse_MTK("onRewind");
    }
    SkASSERT(nullptr != decoderMgr);
    fDecoderMgr.reset(decoderMgr);

    fSwizzler.reset(nullptr);
    fSwizzleSrcRow = nullptr;
    fColorXformSrcRow = nullptr;
    fStorage.reset();

    return true;
}

bool SkJpegCodec::conversionSupported(const SkImageInfo& dstInfo, bool srcIsOpaque,
                                      bool needsColorXform) {
    SkASSERT(srcIsOpaque);

    if (kUnknown_SkAlphaType == dstInfo.alphaType()) {
        return false;
    }

    if (kOpaque_SkAlphaType != dstInfo.alphaType()) {
        SkCodecPrintf("Warning: an opaque image should be decoded as opaque "
                      "- it is being decoded as non-opaque, which will draw slower\n");
    }

    J_COLOR_SPACE_ALPHA encodedColorType = fDecoderMgr->dinfo_MTK()->jpeg_color_space;

    // Check for valid color types and set the output color space
    switch (dstInfo.colorType()) {
        case kRGBA_8888_SkColorType:
            fDecoderMgr->dinfo_MTK()->out_color_space = JCS_EXT_RGBA_ALPHA;
            break;
        case kBGRA_8888_SkColorType:
            if (needsColorXform) {
                // Always using RGBA as the input format for color xforms makes the
                // implementation a little simpler.
                fDecoderMgr->dinfo_MTK()->out_color_space = JCS_EXT_RGBA_ALPHA;
            } else {
                fDecoderMgr->dinfo_MTK()->out_color_space = JCS_EXT_BGRA_ALPHA;
            }
            break;
        case kRGB_565_SkColorType:
            if (needsColorXform) {
                fDecoderMgr->dinfo_MTK()->out_color_space = JCS_EXT_RGBA_ALPHA;
            } else {
                fDecoderMgr->dinfo_MTK()->dither_mode = JDITHER_NONE_ALPHA;
                fDecoderMgr->dinfo_MTK()->out_color_space = JCS_RGB565_ALPHA;
            }
            break;
        case kGray_8_SkColorType:
            if (JCS_GRAYSCALE_ALPHA != encodedColorType) {
                return false;
            }

            if (needsColorXform) {
                fDecoderMgr->dinfo_MTK()->out_color_space = JCS_EXT_RGBA_ALPHA;
            } else {
                fDecoderMgr->dinfo_MTK()->out_color_space = JCS_GRAYSCALE_ALPHA;
            }
            break;
        case kRGBA_F16_SkColorType:
            SkASSERT(needsColorXform);
            fDecoderMgr->dinfo_MTK()->out_color_space = JCS_EXT_RGBA_ALPHA;
            break;
        default:
            return false;
    }

    // Check if we will decode to CMYK.  libjpeg-turbo does not convert CMYK to RGBA, so
    // we must do it ourselves.
    if (JCS_CMYK_ALPHA == encodedColorType || JCS_YCCK_ALPHA == encodedColorType) {
        fDecoderMgr->dinfo_MTK()->out_color_space = JCS_CMYK_ALPHA;
    }

    return true;
}

/*
 * Checks if we can natively scale to the requested dimensions and natively scales the
 * dimensions if possible
 */
bool SkJpegCodec::onDimensionsSupported(const SkISize& size) {
    skjpeg_error_mgr_MTK::AutoPushJmpBuf jmp(fDecoderMgr->errorMgr_MTK());
    if (setjmp(jmp)) {
        return fDecoderMgr->returnFalse_MTK("onDimensionsSupported");
    }

    const unsigned int dstWidth = size.width();
    const unsigned int dstHeight = size.height();

    // Set up a fake decompress struct in order to use libjpeg to calculate output dimensions
    // FIXME: Why is this necessary?
    jpeg_decompress_struct_ALPHA dinfo;
    sk_bzero(&dinfo, sizeof(dinfo));
    dinfo.image_width = this->dimensions().width();
    dinfo.image_height = this->dimensions().height();
    dinfo.global_state = fReadyState;

    // libjpeg-turbo can scale to 1/8, 1/4, 3/8, 1/2, 5/8, 3/4, 7/8, and 1/1
    unsigned int num = 8;
    const unsigned int denom = 8;
    calc_output_dimensions(&dinfo, num, denom);
    while (dinfo.output_width != dstWidth || dinfo.output_height != dstHeight) {

        // Return a failure if we have tried all of the possible scales
        if (1 == num || dstWidth > dinfo.output_width || dstHeight > dinfo.output_height) {
            return false;
        }

        // Try the next scale
        num -= 1;
        calc_output_dimensions(&dinfo, num, denom);
    }

    fDecoderMgr->dinfo_MTK()->scale_num = num;
    fDecoderMgr->dinfo_MTK()->scale_denom = denom;
    return true;
}

int SkJpegCodec::readRows(const SkImageInfo& dstInfo, void* dst, size_t rowBytes, int count,
                          const Options& opts) {
    // Set the jump location for libjpeg-turbo errors
    skjpeg_error_mgr_MTK::AutoPushJmpBuf jmp(fDecoderMgr->errorMgr_MTK());
    if (setjmp(jmp)) {
        return 0;
    }

    // When fSwizzleSrcRow is non-null, it means that we need to swizzle.  In this case,
    // we will always decode into fSwizzlerSrcRow before swizzling into the next buffer.
    // We can never swizzle "in place" because the swizzler may perform sampling and/or
    // subsetting.
    // When fColorXformSrcRow is non-null, it means that we need to color xform and that
    // we cannot color xform "in place" (many times we can, but not when the src and dst
    // are different sizes).
    // In this case, we will color xform from fColorXformSrcRow into the dst.
    JSAMPLE* decodeDst = (JSAMPLE*) dst;
    uint32_t* swizzleDst = (uint32_t*) dst;
    size_t decodeDstRowBytes = rowBytes;
    size_t swizzleDstRowBytes = rowBytes;
    int dstWidth = opts.fSubset ? opts.fSubset->width() : dstInfo.width();
    if (fSwizzleSrcRow && fColorXformSrcRow) {
        decodeDst = (JSAMPLE*) fSwizzleSrcRow;
        swizzleDst = fColorXformSrcRow;
        decodeDstRowBytes = 0;
        swizzleDstRowBytes = 0;
        dstWidth = fSwizzler->swizzleWidth();
    } else if (fColorXformSrcRow) {
        decodeDst = (JSAMPLE*) fColorXformSrcRow;
        swizzleDst = fColorXformSrcRow;
        decodeDstRowBytes = 0;
        swizzleDstRowBytes = 0;
    } else if (fSwizzleSrcRow) {
        decodeDst = (JSAMPLE*) fSwizzleSrcRow;
        decodeDstRowBytes = 0;
        dstWidth = fSwizzler->swizzleWidth();
    }

    for (int y = 0; y < count; y++) {
        uint32_t lines = jpeg_read_scanlines_ALPHA(fDecoderMgr->dinfo_MTK(), &decodeDst, 1);
        if (0 == lines) {
            return y;
        }

        if (fSwizzler) {
            fSwizzler->swizzle(swizzleDst, decodeDst);
        }

        if (this->colorXform()) {
            this->applyColorXform(dst, swizzleDst, dstWidth);
            dst = SkTAddOffset<void>(dst, rowBytes);
        }

        decodeDst = SkTAddOffset<JSAMPLE>(decodeDst, decodeDstRowBytes);
        swizzleDst = SkTAddOffset<uint32_t>(swizzleDst, swizzleDstRowBytes);
    }

    return count;
}

#ifdef MTK_JPEG_HW_REGION_RESIZER
int SkJpegCodec::readRows_MTK(const SkImageInfo& dstInfo, void* dst, size_t rowBytes, int count,
                          const Options& opts) {
    // Set the jump location for libjpeg-turbo errors
    skjpeg_error_mgr_MTK::AutoPushJmpBuf jmp(fDecoderMgr->errorMgr_MTK());
    if (setjmp(jmp)) {
        return 0;
    }

    JSAMPLE* tmpBuffer;
    int outputWidth = (fSwizzlerSubset.isEmpty()) ? fDecoderMgr->dinfo_MTK()->output_width : fSwizzlerSubset.width();
    int outputHeight = (fSwizzlerSubset.isEmpty()) ? fDecoderMgr->dinfo_MTK()->output_height :
        (fRegionHeight == 0) ? fSwizzlerSubset.height() : fRegionHeight;

    if ((!fFirstTileDone || fUseHWResizer) && fBufferStorage)
    {
        // do sampleDecode(full frame decode) => sampleSize does not support by decoder(1,2,4,8)
        // sampleDecode: 1. allocate buffer with size dstRowBytes * fDecoderMgr->dinfo_MTK()->output_height / fSwizzler->sampleX()
        if ((fSwizzler && fSwizzler->sampleX() != 1) && fIsSampleDecode == false)
        {
            fBufferStorage->reset(rowBytes * outputHeight / fSwizzler->sampleX());
            tmpBuffer = (JSAMPLE*) fBufferStorage->getAddr();
            fIsSampleDecode = true;
            fFirstTileDone = true;
            fUseHWResizer = false;
            SkCodecPrintf("SkJpegCodec::onGetScanlines SampleDecode region(%d, %d), size %zu, tmpBuffer %p, dstAddr %p\n",
                outputWidth / fSwizzler->sampleX(),
                outputHeight / fSwizzler->sampleX(),
                rowBytes * outputHeight / fSwizzler->sampleX(),
                fBufferStorage->getAddr(), dst);
        }
        // non-sample decode case
        else
        {
            fBufferStorage->reset(rowBytes * count);
            tmpBuffer = (JSAMPLE*) fBufferStorage->getAddr();
            fIsSampleDecode = false;
        }
    }
    // sampleDecode: 2.1 calculate tmpBuffer with fSampleDecodeY and dstRowBytes
    else if (fBufferStorage && fIsSampleDecode == true)
    {
        tmpBuffer = ((JSAMPLE*) fBufferStorage->getAddr()) + (fSampleDecodeY * rowBytes);
    }
    else
    {
        // use SW decoder only
        tmpBuffer = (JSAMPLE*) dst;
        fFirstTileDone = true;
        fUseHWResizer = false;
        //SkCodecPrintf("onGetScanlines use SW fFirstTileDone %d, fUseHWResizer %d, bytesToAlloc %d\n",
        //    fFirstTileDone, fUseHWResizer, dstRowBytes * count);
    }

    if(fBufferStorage && tmpBuffer == nullptr)
    {
        SkCodecPrintf("Ion allocate tmpBuffer failed, fall back to pure sw decoder");
        tmpBuffer = (JSAMPLE*) dst;
        fFirstTileDone = true;
        fUseHWResizer = false;
    }

    // When fSwizzleSrcRow is non-null, it means that we need to swizzle.  In this case,
    // we will always decode into fSwizzlerSrcRow before swizzling into the next buffer.
    // We can never swizzle "in place" because the swizzler may perform sampling and/or
    // subsetting.
    // When fColorXformSrcRow is non-null, it means that we need to color xform and that
    // we cannot color xform "in place" (many times we can, but not when the dst is F16).
    // In this case, we will color xform from fColorXformSrcRow into the dst.
    JSAMPLE* decodeDst = (JSAMPLE*) tmpBuffer;
    uint32_t* swizzleDst = (uint32_t*) tmpBuffer;
    JSAMPLE* colorXDst = (JSAMPLE*) tmpBuffer;
    size_t decodeDstRowBytes = rowBytes;
    size_t swizzleDstRowBytes = rowBytes;
    int dstWidth = opts.fSubset ? opts.fSubset->width() : dstInfo.width();
    if (fSwizzleSrcRow && fColorXformSrcRow) {
        decodeDst = (JSAMPLE*) fSwizzleSrcRow;
        swizzleDst = fColorXformSrcRow;
        decodeDstRowBytes = 0;
        swizzleDstRowBytes = 0;
        dstWidth = fSwizzler->swizzleWidth();
    } else if (fColorXformSrcRow) {
        decodeDst = (JSAMPLE*) fColorXformSrcRow;
        swizzleDst = fColorXformSrcRow;
        decodeDstRowBytes = 0;
        swizzleDstRowBytes = 0;
    } else if (fSwizzleSrcRow) {
        decodeDst = (JSAMPLE*) fSwizzleSrcRow;
        decodeDstRowBytes = 0;
        dstWidth = fSwizzler->swizzleWidth();
    }

#ifdef MTK_JPEG_DEBUG_DUMP_INOUT
    char value[PROPERTY_VALUE_MAX] = "0";
    int mDumpOut;
    FILE *fp = nullptr;
    unsigned char* cptr;
    char filepath[128];

    property_get("debug.jpegFull.output.dump", value, "0");
    mDumpOut = atoi(value);

    if(mDumpOut == 1)
    {
        int ret = sprintf(filepath, "/sdcard/jpegout_w%d_h%d_%d_%d.rgb", dstInfo.width(), dstInfo.height(), dstInfo.colorType(), (int)(SkTime::GetNSecs() * 1e-6));
        if (ret < 0)
            SkCodecPrintf("sprintf filepath fail");
        fp = fopen(filepath, "wb");
        if(fp == nullptr)
        {
            SkCodecPrintf("open out dump file error");
            mDumpOut = 0;
        }
    }
#endif

    for (int y = 0; y < count; y++) {
        uint32_t lines = jpeg_read_scanlines_ALPHA(fDecoderMgr->dinfo_MTK(), &decodeDst, 1);
#ifdef MTK_JPEG_DEBUG_DUMP_INOUT
        if(mDumpOut == 1)
        {
            cptr = (unsigned char*)decodeDst;
            for (unsigned int i = 0; i < rowBytes; i++)
            {
                fprintf(fp, "%c", *cptr);
                cptr++;
            }
        }
#endif
        if (0 == lines) {
            if(fBufferStorage && ((!fFirstTileDone || fUseHWResizer) || fIsSampleDecode == true))
            {
                bool result = false;
                unsigned long addrOffset =0;
                if (fIsSampleDecode == false)
                    result = ImgPostProc(tmpBuffer, fIonClientHnd, fBufferAllocator, fBufferStorage->getFD(), dst,
                        outputWidth, y, rowBytes, fBufferStorage->getColor(),
                        fEnTdshp, NULL, fISOSpeedRatings);
                else
                {
                    // sampledecode: 3. do ImgPostProc to apply PQ effect
                    addrOffset = fSampleDecodeY * rowBytes;
                    SkCodecPrintf("SkJpegCodec::onGetScanlines ImgPostProc src %p, dst %p, fSampleDecodeY %u\n",
                        tmpBuffer, dst, fSampleDecodeY);
                    result = ImgPostProc(tmpBuffer - addrOffset, fIonClientHnd, fBufferAllocator,
                        fBufferStorage->getFD(), (void*)((unsigned char*)dst - addrOffset),
                        outputWidth / fSwizzler->sampleX(), (fSampleDecodeY + y),
                        rowBytes, fBufferStorage->getColor(), fEnTdshp, NULL, fISOSpeedRatings);
                    if (result)
                    {
                        fIsSampleDecode = false;
                        fSampleDecodeY = 0;
                    }
                }
                if(!result)
                {
                    fFirstTileDone = true;
                    SkCodecPrintf("ImgPostProc fail, use default solution, L:%d!!\n", __LINE__);
                    if (fIsSampleDecode == false)
                        memcpy(dst, tmpBuffer, rowBytes * y);
                    else
                    {
                        memcpy((void*)((unsigned char*)dst - addrOffset), (void*)(tmpBuffer - addrOffset),
                               rowBytes * (fSampleDecodeY + y));
                        fIsSampleDecode = false;
                        fSampleDecodeY = 0;
                     }
                }
                else
                {
                    fFirstTileDone = true;
                    fUseHWResizer = true;
                    //SkCodecPrintf("ImgPostProc successfully, L:%d!!\n", __LINE__);
                }
            }
            #ifdef MTK_JPEG_DEBUG_DUMP_INOUT
            if(mDumpOut == 1)
            {
                fclose(fp);
            }
            #endif
            return y;
        }

        if (fSwizzler) {
            fSwizzler->swizzle(swizzleDst, decodeDst);
        }

        if (this->colorXform()) {
            this->applyColorXform(colorXDst, swizzleDst, dstWidth);
            if (fIsSampleDecode == false)
                colorXDst = SkTAddOffset<JSAMPLE>(colorXDst, rowBytes);
        }

        decodeDst = SkTAddOffset<JSAMPLE>(decodeDst, decodeDstRowBytes);
        swizzleDst = SkTAddOffset<uint32_t>(swizzleDst, swizzleDstRowBytes);
    }
#ifdef MTK_JPEG_DEBUG_DUMP_INOUT
    if(mDumpOut == 1)
    {
        fclose(fp);
    }
#endif

    if(fBufferStorage && ((!fFirstTileDone || fUseHWResizer) ||
        (fIsSampleDecode == true && (fSampleDecodeY + count) == (unsigned int)(outputHeight / fSwizzler->sampleX()))))
    {
        bool result = false;
        unsigned long addrOffset = 0;
        if (fIsSampleDecode == false)
            result = ImgPostProc(tmpBuffer, fIonClientHnd, fBufferAllocator, fBufferStorage->getFD(), dst,
                outputWidth, count, rowBytes, fBufferStorage->getColor(),
                fEnTdshp, NULL, fISOSpeedRatings);
        // sampledecode: 3. do ImgPostProc to apply PQ effect
        else
        {
            addrOffset = fSampleDecodeY * rowBytes;
            SkCodecPrintf("SkJpegCodec::onGetScanlines ImgPostProc src %p, dst %p, fSampleDecodeY %u\n",
                tmpBuffer, dst, fSampleDecodeY);
            result = ImgPostProc(tmpBuffer - addrOffset, fIonClientHnd, fBufferAllocator,
                fBufferStorage->getFD(), (void*)((unsigned char*)dst - addrOffset),
                outputWidth / fSwizzler->sampleX(), (fSampleDecodeY + count),
                rowBytes, fBufferStorage->getColor(), fEnTdshp, NULL, fISOSpeedRatings);
            if (result)
            {
                fIsSampleDecode = false;
                fSampleDecodeY = 0;
            }
        }
        if(!result)
        {
            fFirstTileDone = true;
            SkCodecPrintf("ImgPostProc fail, use default solution, L:%d!!\n", __LINE__);
            if (fIsSampleDecode == false)
                memcpy(dst, tmpBuffer, rowBytes * count);
            else
            {
                memcpy((void*)((unsigned char*)dst - addrOffset), (void*)(tmpBuffer - addrOffset),
                       rowBytes * (fSampleDecodeY + count));
                fIsSampleDecode = false;
                fSampleDecodeY = 0;
            }
        }
        else
        {
            fFirstTileDone = true;
            fUseHWResizer = true;
            //SkCodecPrintf("ImgPostProc successfully, L:%d!!\n", __LINE__);
        }
    }
    // sampleDecode: 2.2 record each count size by fSampleDecodeY until reach the target height
    else if (fIsSampleDecode == true)
    {
        fSampleDecodeY = fSampleDecodeY + count;
    }

    return count;
}
#endif

/*
 * This is a bit tricky.  We only need the swizzler to do format conversion if the jpeg is
 * encoded as CMYK.
 * And even then we still may not need it.  If the jpeg has a CMYK color profile and a color
 * xform, the color xform will handle the CMYK->RGB conversion.
 */
static inline bool needs_swizzler_to_convert_from_cmyk(J_COLOR_SPACE_ALPHA jpegColorType,
                                                       const skcms_ICCProfile* srcProfile,
                                                       bool hasColorSpaceXform) {
    if (JCS_CMYK_ALPHA != jpegColorType) {
        return false;
    }

    bool hasCMYKColorSpace = srcProfile && srcProfile->data_color_space == skcms_Signature_CMYK;
    return !hasCMYKColorSpace || !hasColorSpaceXform;
}

/*
 * Performs the jpeg decode
 */
SkCodec::Result SkJpegCodec::onGetPixels(const SkImageInfo& dstInfo,
                                         void* dst, size_t dstRowBytes,
                                         const Options& options,
                                         int* rowsDecoded) {
    if (options.fSubset) {
        // Subsets are not supported.
        return kUnimplemented;
    }

    if (dstInfo.width() >= 400 && dstInfo.height() >= 400)
        SkCodecPrintf("SkJpegCodec::onGetPixels +");

    fSwizzlerSubset.setEmpty(); // reset fSwizzlerSubset

#ifdef MTK_JPEG_SW_OPTIMIZATION
    char value[PROPERTY_VALUE_MAX] = "0";
    int mOptOn, mOptSwitch;
    property_get("ro.vendor.jpeg_decode_sw_opt", value, "0");
    mOptOn = atoi(value);
    property_get("debug.jpegfull.opt.switch", value, "1");
    mOptSwitch = atoi(value);
    int inwidth = this->getInfo().width();
    int inheight = this->getInfo().height();

    if (mOptOn > 0 && mOptSwitch == 1 && inwidth >= MIN_WIDTH_APPLY_OPT && inwidth * inheight >= MIN_SIZE_APPLY_OPT &&
        LoadInputStreamToMem(fIonClientHnd, fBufferAllocator, fBitstreamBuf, this->stream(), &fMemStream)) {
        std::unique_ptr<JpegDecoderMgr_MTK> decoderMgr(new JpegDecoderMgr_MTK(fMemStream));
        skjpeg_error_mgr_MTK::AutoPushJmpBuf jmp(decoderMgr->errorMgr_MTK());
        if (setjmp(jmp)) {
            return decoderMgr->returnFailure_MTK("ReadHeader", kInvalidInput);
        }

        jpeg_std_oal_ALPHA((j_common_ptr_ALPHA)decoderMgr->dinfo_MTK(), decoderMgr->oal_MTK());
        decoderMgr->oal_MTK()->queryHAL = Codec_queryHAL;
        decoderMgr->init_MTK();
        decoderMgr->dinfo_MTK()->multithread_decode = true; // Enable SW full image opt.
        decoderMgr->dinfo_MTK()->src->next_input_byte_pa = (const JOCTET_ALPHA*) (unsigned long) fBitstreamBuf->getFD();
        decoderMgr->dinfo_MTK()->src->bytes_in_buffer_pa = fMemStream->getLength();
        decoderMgr->dinfo_MTK()->src->buffer_size = fBitstreamBuf->getSize() - 2048;

        switch (jpeg_read_header_ALPHA(decoderMgr->dinfo_MTK(), true)) {
            case JPEG_HEADER_OK_ALPHA:
                break;
            case JPEG_SUSPENDED_ALPHA:
                return decoderMgr->returnFailure_MTK("ReadHeader", kIncompleteInput);
            default:
                return decoderMgr->returnFailure_MTK("ReadHeader", kInvalidInput);
        }
        decoderMgr->dinfo_MTK()->scale_num = fDecoderMgr->dinfo_MTK()->scale_num;
        decoderMgr->dinfo_MTK()->scale_denom = fDecoderMgr->dinfo_MTK()->scale_denom;
        decoderMgr->dinfo_MTK()->out_color_space = fDecoderMgr->dinfo_MTK()->out_color_space;
        decoderMgr->dinfo_MTK()->dither_mode = fDecoderMgr->dinfo_MTK()->dither_mode;
        SkASSERT(nullptr != decoderMgr);
        fDecoderMgr.reset(decoderMgr.release());
    }
#endif

#ifdef MTK_JPEG_DEBUG_DUMP_INOUT
    SkStream *instream = nullptr;
    int mDumpIn;
    FILE *fp = nullptr;
    unsigned char* cptr;
    char filepath[128];

    property_get("debug.jpegFull.input.dump", value, "0");
    mDumpIn = atoi(value);

    if(mDumpIn == 1)
    {
        if (fMemStream)
        {
            instream = fMemStream;
        }
        else
        {
            instream = this->stream();
        }

        int ret = sprintf(filepath, "/sdcard/jpegin_w%d_h%d_%d.jpg", fDecoderMgr->dinfo_MTK()->image_width, fDecoderMgr->dinfo_MTK()->image_height, (int)(SkTime::GetNSecs() * 1e-6));
        if (ret < 0)
            SkCodecPrintf("sprintf filepath fail");
        fp = fopen(filepath, "wb");
        if(fp == nullptr)
        {
            SkCodecPrintf("open in dump file error");
        }
        cptr = (unsigned char*)instream->getMemoryBase();
        if (fp != nullptr)
        {
            if(cptr != nullptr)
            {
                for (unsigned int i = 0; i < instream->getLength(); i++)
                {
                    fprintf(fp, "%c", *cptr);
                    cptr++;
                }
            }
            fclose(fp);
        }
    }
#endif

    // Get a pointer to the decompress info since we will use it quite frequently
    jpeg_decompress_struct_ALPHA* dinfo = fDecoderMgr->dinfo_MTK();

    if(fInfoDump.enable())
    {
        fInfoDump.add(WIDTH, inwidth);
        fInfoDump.add(HEIGHT, inheight);
        fInfoDump.add(PROGRESSIVE, dinfo->progressive_mode);
        fInfoDump.add(COLOR_TYPE, dstInfo.colorType());
        fInfoDump.add(COLOR_SPACE, dinfo->out_color_space);
        fInfoDump.add(SAMPLE_SIZE, dinfo->scale_denom);
        if(read_color_profile(dinfo))
            fInfoDump.add(ICC_PROFILE, 1);
        fInfoDump.commit();
    }

#ifdef MTK_JPEG_HW_REGION_RESIZER

    dinfo->do_fancy_upsampling = 0;
    dinfo->do_block_smoothing = 0;

    if (fFirstTileDone == false)
    {
        long u4PQOpt;
        char value[PROPERTY_VALUE_MAX] = "0";

        // property control for PQ flag
        property_get("jpegDecode.forceEnable.PQ", value, "-1");
        u4PQOpt = atol(value);
        if (-1 == u4PQOpt)
            fEnTdshp = (this->getPostProcFlag()) & 0x1;
        else if (0 == u4PQOpt)
            fEnTdshp = 0x0;
        else
            fEnTdshp = 0x1;

        if (!fEnTdshp)
        {
            fFirstTileDone = true;
            fUseHWResizer = false;
        }

    }
    if (fEnTdshp && fISOSpeedRatings == -1)
    {
        SkStream* stream = this->stream();
        size_t curStreamPosition = stream->getPosition();
        SkAutoTMalloc<uint8_t> tmpStorage;
        // record the stream position in order to restore when we need to use SW decoder
        if (stream->hasPosition() && stream->rewind())
        {
            tmpStorage.reset(MAX_APP1_HEADER_SIZE);
            size_t bytes_read = stream->read(tmpStorage.get(), MAX_APP1_HEADER_SIZE);
            fISOSpeedRatings = getISOSpeedRatings(tmpStorage.get(), bytes_read);

            // restore stream position and use original SW decoder
            stream->rewind();
            if (curStreamPosition != 0 && !stream->seek(curStreamPosition))
            {
                SkCodecPrintf("onGetPixels stream seek fail!");
                return kCouldNotRewind;
            }
        }
    }

    if(!fFirstTileDone || fUseHWResizer)
    {
        if (!fBufferStorage)
            fBufferStorage = new SkBufMalloc(fIonClientHnd, fBufferAllocator);
        if (fBufferStorage)
            fBufferStorage->setColor(dstInfo.colorType());
    }
#endif

    // Set the jump location for libjpeg errors
    skjpeg_error_mgr_MTK::AutoPushJmpBuf jmp(fDecoderMgr->errorMgr_MTK());
    if (setjmp(jmp)) {
        return fDecoderMgr->returnFailure_MTK("setjmp", kInvalidInput);
    }

    if (!jpeg_start_decompress_ALPHA(dinfo)) {
        return fDecoderMgr->returnFailure_MTK("startDecompress", kInvalidInput);
    }

    // The recommended output buffer height should always be 1 in high quality modes.
    // If it's not, we want to know because it means our strategy is not optimal.
    SkASSERT(1 == dinfo->rec_outbuf_height);

    if (needs_swizzler_to_convert_from_cmyk(dinfo->out_color_space,
                                            this->getEncodedInfo().profile(), this->colorXform())) {
        this->initializeSwizzler(dstInfo, options, true);
    }

    if (!this->allocateStorage(dstInfo)) {
        return kInternalError;
    }

    int rows = this->readRows_MTK(dstInfo, dst, dstRowBytes, dstInfo.height(), options);
    if (rows < dstInfo.height()) {
        *rowsDecoded = rows;
        return fDecoderMgr->returnFailure_MTK("Incomplete image data", kIncompleteInput);
    }

    jpeg_finish_decompress_ALPHA(dinfo);

    if (dstInfo.width() >= 400 && dstInfo.height() >= 400)
        SkCodecPrintf("SkJpegCodec::onGetPixels -");

    return kSuccess;
}

bool SkJpegCodec::allocateStorage(const SkImageInfo& dstInfo) {
    int dstWidth = dstInfo.width();

    size_t swizzleBytes = 0;
    if (fSwizzler) {
        swizzleBytes = get_row_bytes(fDecoderMgr->dinfo_MTK());
        dstWidth = fSwizzler->swizzleWidth();
        SkASSERT(!this->colorXform() || SkIsAlign4(swizzleBytes));
    }

    size_t xformBytes = 0;

    if (this->colorXform() && sizeof(uint32_t) != dstInfo.bytesPerPixel()) {
        xformBytes = dstWidth * sizeof(uint32_t);
    }

    size_t totalBytes = swizzleBytes + xformBytes;
    if (totalBytes > 0) {
        if (!fStorage.reset(totalBytes)) {
            return false;
        }
        fSwizzleSrcRow = (swizzleBytes > 0) ? fStorage.get() : nullptr;
        fColorXformSrcRow = (xformBytes > 0) ?
                SkTAddOffset<uint32_t>(fStorage.get(), swizzleBytes) : nullptr;
    }
    return true;
}

void SkJpegCodec::initializeSwizzler(const SkImageInfo& dstInfo, const Options& options,
        bool needsCMYKToRGB) {
    Options swizzlerOptions = options;
    if (options.fSubset) {
        // Use fSwizzlerSubset if this is a subset decode.  This is necessary in the case
        // where libjpeg-turbo provides a subset and then we need to subset it further.
        // Also, verify that fSwizzlerSubset is initialized and valid.
        SkASSERT(!fSwizzlerSubset.isEmpty() && fSwizzlerSubset.x() <= options.fSubset->x() &&
                fSwizzlerSubset.width() == options.fSubset->width());
        swizzlerOptions.fSubset = &fSwizzlerSubset;
    }

    SkImageInfo swizzlerDstInfo = dstInfo;
    if (this->colorXform()) {
        // The color xform will be expecting RGBA 8888 input.
        swizzlerDstInfo = swizzlerDstInfo.makeColorType(kRGBA_8888_SkColorType);
    }

    if (needsCMYKToRGB) {
        // The swizzler is used to convert to from CMYK.
        // The swizzler does not use the width or height on SkEncodedInfo.
        auto swizzlerInfo = SkEncodedInfo::Make(0, 0, SkEncodedInfo::kInvertedCMYK_Color,
                                                SkEncodedInfo::kOpaque_Alpha, 8);
        fSwizzler = SkSwizzler::Make(swizzlerInfo, nullptr, swizzlerDstInfo, swizzlerOptions);
    } else {
        int srcBPP = 0;
        switch (fDecoderMgr->dinfo_MTK()->out_color_space) {
            case JCS_EXT_RGBA:
            case JCS_EXT_BGRA:
            case JCS_CMYK:
                srcBPP = 4;
                break;
            case JCS_RGB565:
                srcBPP = 2;
                break;
            case JCS_GRAYSCALE:
                srcBPP = 1;
                break;
            default:
                SkASSERT(false);
                break;
        }
        fSwizzler = SkSwizzler::MakeSimple(srcBPP, swizzlerDstInfo, swizzlerOptions);
    }
    SkASSERT(fSwizzler);
}

SkSampler* SkJpegCodec::getSampler(bool createIfNecessary) {
    if (!createIfNecessary || fSwizzler) {
        SkASSERT(!fSwizzler || (fSwizzleSrcRow && fStorage.get() == fSwizzleSrcRow));
        return fSwizzler.get();
    }

    bool needsCMYKToRGB = needs_swizzler_to_convert_from_cmyk(
            fDecoderMgr->dinfo_MTK()->out_color_space, this->getEncodedInfo().profile(),
            this->colorXform());
    this->initializeSwizzler(this->dstInfo(), this->options(), needsCMYKToRGB);
    if (!this->allocateStorage(this->dstInfo())) {
        return nullptr;
    }
    return fSwizzler.get();
}

SkCodec::Result SkJpegCodec::onStartScanlineDecode(const SkImageInfo& dstInfo,
        const Options& options) {
#ifdef MTK_JPEG_SW_OPTIMIZATION
    char value[PROPERTY_VALUE_MAX] = "0";
    int mOptOn, mOptSwitch;
    property_get("ro.vendor.jpeg_decode_sw_opt", value, "0");
    mOptOn = atoi(value);
    property_get("debug.jpegfull.opt.switch", value, "1");
    mOptSwitch = atoi(value);
    int inwidth = this->getInfo().width();
    int inheight = this->getInfo().height();

    if (!options.fSubset && mOptOn > 0 && mOptSwitch == 1 && inwidth >= MIN_WIDTH_APPLY_OPT && inwidth * inheight >= MIN_SIZE_APPLY_OPT &&
        LoadInputStreamToMem(fIonClientHnd, fBufferAllocator, fBitstreamBuf, this->stream(), &fMemStream)) {
        std::unique_ptr<JpegDecoderMgr_MTK> decoderMgr(new JpegDecoderMgr_MTK(fMemStream));
        skjpeg_error_mgr_MTK::AutoPushJmpBuf jmp(decoderMgr->errorMgr_MTK());
        if (setjmp(jmp)) {
            return decoderMgr->returnFailure_MTK("ReadHeader", kInvalidInput);
        }

        jpeg_std_oal_ALPHA((j_common_ptr_ALPHA)decoderMgr->dinfo_MTK(), decoderMgr->oal_MTK());
        decoderMgr->oal_MTK()->queryHAL = Codec_queryHAL;
        decoderMgr->init_MTK();
        decoderMgr->dinfo_MTK()->multithread_decode = true; // Enable SW full image opt.
        decoderMgr->dinfo_MTK()->src->next_input_byte_pa = (const JOCTET_ALPHA*) (unsigned long) fBitstreamBuf->getFD();
        decoderMgr->dinfo_MTK()->src->bytes_in_buffer_pa = fMemStream->getLength();
        decoderMgr->dinfo_MTK()->src->buffer_size = fBitstreamBuf->getSize() - 2048;

        switch (jpeg_read_header_ALPHA(decoderMgr->dinfo_MTK(), true)) {
            case JPEG_HEADER_OK_ALPHA:
                break;
            case JPEG_SUSPENDED_ALPHA:
                return decoderMgr->returnFailure_MTK("ReadHeader", kIncompleteInput);
            default:
                return decoderMgr->returnFailure_MTK("ReadHeader", kInvalidInput);
        }
        decoderMgr->dinfo_MTK()->scale_num = fDecoderMgr->dinfo_MTK()->scale_num;
        decoderMgr->dinfo_MTK()->scale_denom = fDecoderMgr->dinfo_MTK()->scale_denom;
        decoderMgr->dinfo_MTK()->out_color_space = fDecoderMgr->dinfo_MTK()->out_color_space;
        decoderMgr->dinfo_MTK()->dither_mode = fDecoderMgr->dinfo_MTK()->dither_mode;
        SkASSERT(nullptr != decoderMgr);
        fDecoderMgr.reset(decoderMgr.release());
        fDecoderMgr->dinfo_MTK()->do_fancy_upsampling = 0;
        fDecoderMgr->dinfo_MTK()->do_block_smoothing = 0;
    }
#endif
    if(fInfoDump.enable())
    {
        fInfoDump.add(WIDTH, fDecoderMgr->dinfo_MTK()->image_width);
        fInfoDump.add(HEIGHT, fDecoderMgr->dinfo_MTK()->image_height);
        fInfoDump.add(PROGRESSIVE, fDecoderMgr->dinfo_MTK()->progressive_mode);
        fInfoDump.add(COLOR_TYPE, dstInfo.colorType());
        fInfoDump.add(COLOR_SPACE, fDecoderMgr->dinfo_MTK()->out_color_space);
        fInfoDump.add(SAMPLE_SIZE, fDecoderMgr->dinfo_MTK()->scale_denom);
        if(read_color_profile(fDecoderMgr->dinfo_MTK()))
            fInfoDump.add(ICC_PROFILE, 1);
        if(options.fSubset)
            fInfoDump.add(REGION_DECODE, 1);
        fInfoDump.add(CODECTYPE, "JPEG-scanline");
        fInfoDump.commit();
    }
#if defined(MTK_JPEG_HW_REGION_RESIZER)
    if (fFirstTileDone == false)
    {
        long u4PQOpt;
        char value[PROPERTY_VALUE_MAX] = "0";

        // property control for PQ flag
        property_get("jpegDecode.forceEnable.PQ", value, "-1");
        u4PQOpt = atol(value);
        if (-1 == u4PQOpt)
            fEnTdshp = (this->getPostProcFlag()) & 0x1;
        else if (0 == u4PQOpt)
            fEnTdshp = 0x0;
        else
            fEnTdshp = 0x1;

        if (!fEnTdshp)
        {
            fFirstTileDone = true;
            fUseHWResizer = false;
        }

        if (fEnTdshp && fISOSpeedRatings == -1)
        {
            SkStream* stream = this->stream();
            size_t curStreamPosition = stream->getPosition();
            SkAutoTMalloc<uint8_t> tmpStorage;
            // record the stream position in order to restore when we need to use SW decoder
            if (stream->hasPosition() && stream->rewind())
            {
                tmpStorage.reset(MAX_APP1_HEADER_SIZE);
                size_t bytes_read = stream->read(tmpStorage.get(), MAX_APP1_HEADER_SIZE);
                fISOSpeedRatings = getISOSpeedRatings(tmpStorage.get(), bytes_read);

                // restore stream position and use original SW decoder
                stream->rewind();
                if (curStreamPosition != 0 && !stream->seek(curStreamPosition))
                {
                    SkCodecPrintf("onStartScanlineDecode stream seek fail!");
                    return kCouldNotRewind;
                }
            }
        }
    }

    if(!fFirstTileDone || fUseHWResizer)
    {
        if (!fBufferStorage)
            fBufferStorage = new SkBufMalloc(fIonClientHnd, fBufferAllocator);
        if (fBufferStorage)
            fBufferStorage->setColor(dstInfo.colorType());
    }
#endif

    // Set the jump location for libjpeg errors
    skjpeg_error_mgr_MTK::AutoPushJmpBuf jmp(fDecoderMgr->errorMgr_MTK());
    if (setjmp(jmp)) {
        SkCodecPrintf("setjmp: Error from libjpeg\n");
        return kInvalidInput;
    }

    if (!jpeg_start_decompress_ALPHA(fDecoderMgr->dinfo_MTK())) {
        SkCodecPrintf("start decompress failed\n");
        return kInvalidInput;
    }

    bool needsCMYKToRGB = needs_swizzler_to_convert_from_cmyk(
            fDecoderMgr->dinfo_MTK()->out_color_space, this->getEncodedInfo().profile(),
            this->colorXform());
    if (options.fSubset) {
        uint32_t startX = options.fSubset->x();
        uint32_t width = options.fSubset->width();

        // libjpeg-turbo may need to align startX to a multiple of the IDCT
        // block size.  If this is the case, it will decrease the value of
        // startX to the appropriate alignment and also increase the value
        // of width so that the right edge of the requested subset remains
        // the same.
        jpeg_crop_scanline_ALPHA(fDecoderMgr->dinfo_MTK(), &startX, &width);

        SkASSERT(startX <= (uint32_t) options.fSubset->x());
        SkASSERT(width >= (uint32_t) options.fSubset->width());
        SkASSERT(startX + width >= (uint32_t) options.fSubset->right());

        // Instruct the swizzler (if it is necessary) to further subset the
        // output provided by libjpeg-turbo.
        //
        // We set this here (rather than in the if statement below), so that
        // if (1) we don't need a swizzler for the subset, and (2) we need a
        // swizzler for CMYK, the swizzler will still use the proper subset
        // dimensions.
        //
        // Note that the swizzler will ignore the y and height parameters of
        // the subset.  Since the scanline decoder (and the swizzler) handle
        // one row at a time, only the subsetting in the x-dimension matters.
        fSwizzlerSubset.setXYWH(options.fSubset->x() - startX, 0,
                options.fSubset->width(), options.fSubset->height());

        fRegionHeight = options.fRegionHeight;

        // We will need a swizzler if libjpeg-turbo cannot provide the exact
        // subset that we request.
        if (startX != (uint32_t) options.fSubset->x() ||
                width != (uint32_t) options.fSubset->width()) {
            this->initializeSwizzler(dstInfo, options, needsCMYKToRGB);
        }
    }

    // Make sure we have a swizzler if we are converting from CMYK.
    if (!fSwizzler && needsCMYKToRGB) {
        this->initializeSwizzler(dstInfo, options, true);
    }

    if (!this->allocateStorage(dstInfo)) {
        return kInternalError;
    }

    return kSuccess;
}

int SkJpegCodec::onGetScanlines(void* dst, int count, size_t dstRowBytes) {
#if defined(MTK_JPEG_HW_REGION_RESIZER)
    int rows = this->readRows_MTK(this->dstInfo(), dst, dstRowBytes, count, this->options());
#else
    int rows = this->readRows(this->dstInfo(), dst, dstRowBytes, count, this->options());
#endif
    if (rows < count) {
        // This allows us to skip calling jpeg_finish_decompress().
        fDecoderMgr->dinfo_MTK()->output_scanline = this->dstInfo().height();
    }

    return rows;
}

bool SkJpegCodec::onSkipScanlines(int count) {
    // Set the jump location for libjpeg errors
    skjpeg_error_mgr_MTK::AutoPushJmpBuf jmp(fDecoderMgr->errorMgr_MTK());
    if (setjmp(jmp)) {
        return fDecoderMgr->returnFalse_MTK("onSkipScanlines");
    }

    return (uint32_t) count == jpeg_skip_scanlines_ALPHA(fDecoderMgr->dinfo_MTK(), count);
}

static bool is_yuv_supported(const jpeg_decompress_struct_ALPHA* dinfo,
                             const SkJpegCodec& codec,
                             const SkYUVAPixmapInfo::SupportedDataTypes* supportedDataTypes,
                             SkYUVAPixmapInfo* yuvaPixmapInfo) {
    // Scaling is not supported in raw data mode.
    SkASSERT(dinfo->scale_num == dinfo->scale_denom);

    // I can't imagine that this would ever change, but we do depend on it.
    static_assert(8 == DCTSIZE_ALPHA, "DCTSIZE_ALPHA (defined in jpeg library) should always be 8.");

    if (JCS_YCbCr_ALPHA != dinfo->jpeg_color_space) {
        return false;
    }

    SkASSERT(3 == dinfo->num_components);
    SkASSERT(dinfo->comp_info);

    // It is possible to perform a YUV decode for any combination of
    // horizontal and vertical sampling that is supported by
    // libjpeg/libjpeg-turbo.  However, we will start by supporting only the
    // common cases (where U and V have samp_factors of one).
    //
    // The definition of samp_factor is kind of the opposite of what SkCodec
    // thinks of as a sampling factor.  samp_factor is essentially a
    // multiplier, and the larger the samp_factor is, the more samples that
    // there will be.  Ex:
    //     U_plane_width = image_width * (U_h_samp_factor / max_h_samp_factor)
    //
    // Supporting cases where the samp_factors for U or V were larger than
    // that of Y would be an extremely difficult change, given that clients
    // allocate memory as if the size of the Y plane is always the size of the
    // image.  However, this case is very, very rare.
    if  ((1 != dinfo->comp_info[1].h_samp_factor) ||
         (1 != dinfo->comp_info[1].v_samp_factor) ||
         (1 != dinfo->comp_info[2].h_samp_factor) ||
         (1 != dinfo->comp_info[2].v_samp_factor))
    {
        return false;
    }

    // Support all common cases of Y samp_factors.
    // TODO (msarett): As mentioned above, it would be possible to support
    //                 more combinations of samp_factors.  The issues are:
    //                 (1) Are there actually any images that are not covered
    //                     by these cases?
    //                 (2) How much complexity would be added to the
    //                     implementation in order to support these rare
    //                     cases?
    int hSampY = dinfo->comp_info[0].h_samp_factor;
    int vSampY = dinfo->comp_info[0].v_samp_factor;
    SkASSERT(hSampY == dinfo->max_h_samp_factor);
    SkASSERT(vSampY == dinfo->max_v_samp_factor);

    SkYUVAInfo::Subsampling tempSubsampling;
    if        (1 == hSampY && 1 == vSampY) {
        tempSubsampling = SkYUVAInfo::Subsampling::k444;
    } else if (2 == hSampY && 1 == vSampY) {
        tempSubsampling = SkYUVAInfo::Subsampling::k422;
    } else if (2 == hSampY && 2 == vSampY) {
        tempSubsampling = SkYUVAInfo::Subsampling::k420;
    } else if (1 == hSampY && 2 == vSampY) {
        tempSubsampling = SkYUVAInfo::Subsampling::k440;
    } else if (4 == hSampY && 1 == vSampY) {
        tempSubsampling = SkYUVAInfo::Subsampling::k411;
    } else if (4 == hSampY && 2 == vSampY) {
        tempSubsampling = SkYUVAInfo::Subsampling::k410;
    } else {
        return false;
    }
    if (supportedDataTypes &&
        !supportedDataTypes->supported(SkYUVAInfo::PlaneConfig::kY_U_V,
                                       SkYUVAPixmapInfo::DataType::kUnorm8)) {
        return false;
    }
    if (yuvaPixmapInfo) {
        SkColorType colorTypes[SkYUVAPixmapInfo::kMaxPlanes];
        size_t rowBytes[SkYUVAPixmapInfo::kMaxPlanes];
        for (int i = 0; i < 3; ++i) {
            colorTypes[i] = kAlpha_8_SkColorType;
            rowBytes[i] = dinfo->comp_info[i].width_in_blocks * DCTSIZE;
        }
        SkYUVAInfo yuvaInfo(codec.dimensions(),
                            SkYUVAInfo::PlaneConfig::kY_U_V,
                            tempSubsampling,
                            kJPEG_Full_SkYUVColorSpace,
                            codec.getOrigin(),
                            SkYUVAInfo::Siting::kCentered,
                            SkYUVAInfo::Siting::kCentered);
        *yuvaPixmapInfo = SkYUVAPixmapInfo(yuvaInfo, colorTypes, rowBytes);
    }
    return true;
}

bool SkJpegCodec::onQueryYUVAInfo(const SkYUVAPixmapInfo::SupportedDataTypes& supportedDataTypes,
                                  SkYUVAPixmapInfo* yuvaPixmapInfo) const {
    jpeg_decompress_struct_ALPHA* dinfo = fDecoderMgr->dinfo_MTK();
    return is_yuv_supported(dinfo, *this, &supportedDataTypes, yuvaPixmapInfo);
}

SkCodec::Result SkJpegCodec::onGetYUVAPlanes(const SkYUVAPixmaps& yuvaPixmaps) {
    // Get a pointer to the decompress info since we will use it quite frequently
    jpeg_decompress_struct_ALPHA* dinfo = fDecoderMgr->dinfo_MTK();
    if (!is_yuv_supported(dinfo, *this, nullptr, nullptr)) {
        return fDecoderMgr->returnFailure_MTK("onGetYUVAPlanes", kInvalidInput);
    }
    // Set the jump location for libjpeg errors
    skjpeg_error_mgr_MTK::AutoPushJmpBuf jmp(fDecoderMgr->errorMgr_MTK());
    if (setjmp(jmp)) {
        return fDecoderMgr->returnFailure_MTK("setjmp", kInvalidInput);
    }

    dinfo->raw_data_out = TRUE;
    if (!jpeg_start_decompress_ALPHA(dinfo)) {
        return fDecoderMgr->returnFailure_MTK("startDecompress", kInvalidInput);
    }

    const std::array<SkPixmap, SkYUVAPixmaps::kMaxPlanes>& planes = yuvaPixmaps.planes();

#ifdef SK_DEBUG
    {
        // A previous implementation claims that the return value of is_yuv_supported()
        // may change after calling jpeg_start_decompress().  It looks to me like this
        // was caused by a bug in the old code, but we'll be safe and check here.
        // Also check that pixmap properties agree with expectations.
        SkYUVAPixmapInfo info;
        SkASSERT(is_yuv_supported(dinfo, *this, nullptr, &info));
        SkASSERT(info.yuvaInfo() == yuvaPixmaps.yuvaInfo());
        for (int i = 0; i < info.numPlanes(); ++i) {
            SkASSERT(planes[i].colorType() == kAlpha_8_SkColorType);
            SkASSERT(info.planeInfo(i) == planes[i].info());
        }
    }
#endif

    // Build a JSAMPIMAGE to handle output from libjpeg-turbo.  A JSAMPIMAGE has
    // a 2-D array of pixels for each of the components (Y, U, V) in the image.
    // Cheat Sheet:
    //     JSAMPIMAGE == JSAMPLEARRAY* == JSAMPROW** == JSAMPLE***
    JSAMPARRAY_ALPHA yuv[3];

    // Set aside enough space for pointers to rows of Y, U, and V.
    JSAMPROW_ALPHA rowptrs[2 * DCTSIZE_ALPHA + DCTSIZE_ALPHA + DCTSIZE_ALPHA];
    yuv[0] = &rowptrs[0];           // Y rows (DCTSIZE or 2 * DCTSIZE)
    yuv[1] = &rowptrs[2 * DCTSIZE_ALPHA]; // U rows (DCTSIZE)
    yuv[2] = &rowptrs[3 * DCTSIZE_ALPHA]; // V rows (DCTSIZE)

    // Initialize rowptrs.
    int numYRowsPerBlock = DCTSIZE_ALPHA * dinfo->comp_info[0].v_samp_factor;
    static_assert(sizeof(JSAMPLE_ALPHA) == 1);
    for (int i = 0; i < numYRowsPerBlock; i++) {
        rowptrs[i] = static_cast<JSAMPLE_ALPHA*>(planes[0].writable_addr()) + i* planes[0].rowBytes();
    }
    for (int i = 0; i < DCTSIZE_ALPHA; i++) {
        rowptrs[i + 2 * DCTSIZE_ALPHA] =
            static_cast<JSAMPLE_ALPHA*>(planes[1].writable_addr()) + i* planes[1].rowBytes();
        rowptrs[i + 3 * DCTSIZE_ALPHA] =
            static_cast<JSAMPLE_ALPHA*>(planes[2].writable_addr()) + i* planes[2].rowBytes();
    }

    // After each loop iteration, we will increment pointers to Y, U, and V.
    size_t blockIncrementY = numYRowsPerBlock * planes[0].rowBytes();
    size_t blockIncrementU = DCTSIZE_ALPHA * planes[1].rowBytes();
    size_t blockIncrementV = DCTSIZE_ALPHA * planes[2].rowBytes();

    uint32_t numRowsPerBlock = numYRowsPerBlock;

    // We intentionally round down here, as this first loop will only handle
    // full block rows.  As a special case at the end, we will handle any
    // remaining rows that do not make up a full block.
    const int numIters = dinfo->output_height / numRowsPerBlock;
    for (int i = 0; i < numIters; i++) {
        JDIMENSION linesRead = jpeg_read_raw_data_ALPHA(dinfo, yuv, numRowsPerBlock);
        if (linesRead < numRowsPerBlock) {
            // FIXME: Handle incomplete YUV decodes without signalling an error.
            return kInvalidInput;
        }

        // Update rowptrs.
        for (int i = 0; i < numYRowsPerBlock; i++) {
            rowptrs[i] += blockIncrementY;
        }
        for (int i = 0; i < DCTSIZE_ALPHA; i++) {
            rowptrs[i + 2 * DCTSIZE_ALPHA] += blockIncrementU;
            rowptrs[i + 3 * DCTSIZE_ALPHA] += blockIncrementV;
        }
    }

    uint32_t remainingRows = dinfo->output_height - dinfo->output_scanline;
    SkASSERT(remainingRows == dinfo->output_height % numRowsPerBlock);
    SkASSERT(dinfo->output_scanline == numIters * numRowsPerBlock);
    if (remainingRows > 0) {
        // libjpeg-turbo needs memory to be padded by the block sizes.  We will fulfill
        // this requirement using an extra row buffer.
        // FIXME: Should SkCodec have an extra memory buffer that can be shared among
        //        all of the implementations that use temporary/garbage memory?
        SkAutoTMalloc<JSAMPLE> extraRow(planes[0].rowBytes());
        for (int i = remainingRows; i < numYRowsPerBlock; i++) {
            rowptrs[i] = extraRow.get();
        }
        int remainingUVRows = dinfo->comp_info[1].downsampled_height - DCTSIZE_ALPHA * numIters;
        for (int i = remainingUVRows; i < DCTSIZE_ALPHA; i++) {
            rowptrs[i + 2 * DCTSIZE] = extraRow.get();
            rowptrs[i + 3 * DCTSIZE] = extraRow.get();
        }

        JDIMENSION linesRead = jpeg_read_raw_data_ALPHA(dinfo, yuv, numRowsPerBlock);
        if (linesRead < remainingRows) {
            // FIXME: Handle incomplete YUV decodes without signalling an error.
            return kInvalidInput;
        }
    }

    return kSuccess;
}

// This function is declared in SkJpegInfo.h, used by SkPDF.
bool SkGetJpegInfo(const void* data, size_t len,
                   SkISize* size,
                   SkEncodedInfo::Color* colorType,
                   SkEncodedOrigin* orientation) {
    if (!SkJpegCodec::IsJpeg(data, len)) {
        return false;
    }

    SkMemoryStream stream(data, len);
    JpegDecoderMgr_MTK decoderMgr(&stream);
    // libjpeg errors will be caught and reported here
    skjpeg_error_mgr_MTK::AutoPushJmpBuf jmp(decoderMgr.errorMgr_MTK());
    if (setjmp(jmp)) {
        return false;
    }
    decoderMgr.init_MTK();
    jpeg_decompress_struct_ALPHA* dinfo = decoderMgr.dinfo_MTK();
    jpeg_save_markers_ALPHA(dinfo, kExifMarker, 0xFFFF);
    jpeg_save_markers_ALPHA(dinfo, kICCMarker, 0xFFFF);
    if (JPEG_HEADER_OK_ALPHA != jpeg_read_header_ALPHA(dinfo, true)) {
        return false;
    }
    SkEncodedInfo::Color encodedColorType;
    if (!decoderMgr.getEncodedColor_MTK(&encodedColorType)) {
        return false;  // Unable to interpret the color channels as colors.
    }
    if (colorType) {
        *colorType = encodedColorType;
    }
    if (orientation) {
        *orientation = get_exif_orientation(dinfo);
    }
    if (size) {
        *size = {SkToS32(dinfo->image_width), SkToS32(dinfo->image_height)};
    }
    return true;
}
