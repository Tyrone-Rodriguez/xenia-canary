/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef ALLOY_CORE_H_
#define ALLOY_CORE_H_

// TODO(benvanik): move the common stuff into here?
#include <xenia/common.h>

#include <alloy/arena.h>
#include <alloy/delegate.h>
#include <alloy/mutex.h>
#include <alloy/string_buffer.h>


namespace alloy {

typedef struct XECACHEALIGN vec128_s {
  union {
    struct {
      float     x;
      float     y;
      float     z;
      float     w;
    };
    struct {
      uint32_t  ix;
      uint32_t  iy;
      uint32_t  iz;
      uint32_t  iw;
    };
    float       f4[4];
    uint32_t    i4[4];
    uint16_t    s8[8];
    uint8_t     b16[16];
    struct {
      uint64_t  low;
      uint64_t  high;
    };
  };
} vec128_t;
XEFORCEINLINE vec128_t vec128i(uint32_t x, uint32_t y, uint32_t z, uint32_t w) {
  vec128_t v;
  v.i4[0] = x; v.i4[1] = y; v.i4[2] = z; v.i4[3] = w;
  return v;
}
XEFORCEINLINE vec128_t vec128f(float x, float y, float z, float w) {
  vec128_t v;
  v.f4[0] = x; v.f4[1] = y; v.f4[2] = z; v.f4[3] = w;
  return v;
}

}  // namespace alloy


#endif  // ALLOY_CORE_H_
