/*
 * Copyright 2015 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifndef BitmapRegionDecoder_DEFINED
#define BitmapRegionDecoder_DEFINED

#include "client_utils/android/BRDAllocator.h"
#include "include/codec/SkAndroidCodec.h"
#include "include/core/SkBitmap.h"
#include "include/core/SkData.h"

namespace android {
namespace skia {

class BitmapRegionDecoder {
public:
    static std::unique_ptr<BitmapRegionDecoder> Make(sk_sp<SkData> data);

    BitmapRegionDecoder(std::unique_ptr<SkAndroidCodec> codec);

    bool decodeRegion(SkBitmap* bitmap, BRDAllocator* allocator,
                      const SkIRect& desiredSubset, int sampleSize,
                      SkColorType colorType, bool requireUnpremul,
                      sk_sp<SkColorSpace> prefColorSpace);

    SkEncodedImageFormat getEncodedFormat() { return fCodec->getEncodedFormat(); }

    SkColorType computeOutputColorType(SkColorType requestedColorType) {
        return fCodec->computeOutputColorType(requestedColorType);
    }

    sk_sp<SkColorSpace> computeOutputColorSpace(SkColorType outputColorType,
            sk_sp<SkColorSpace> prefColorSpace = nullptr) {
        return fCodec->computeOutputColorSpace(outputColorType, prefColorSpace);
    }

    int width() const;
    int height() const;

    bool isVendorExt() { return vendorExt; }
    void setVendorExt(bool vendor) { vendorExt = vendor; }

private:

    std::unique_ptr<SkAndroidCodec> fCodec;
    bool vendorExt;
};

} // namespace skia
} // namespace android
#endif  // BitmapRegionDecoder_DEFINED
