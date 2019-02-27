// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LIB_FDIO_DEBUG_H_
#define LIB_FDIO_DEBUG_H_

#include <stdio.h>
#include <zircon/compiler.h>

__BEGIN_CDECLS

// per-file chatty debug macro
#define xprintf(fmt, args...)                                                  \
    do {                                                                       \
        if (ZXDEBUG) {                                                         \
            printf("%s:%d: " fmt, __FILE__, __LINE__, ##args);                 \
        }                                                                      \
    } while (0)

__END_CDECLS

#endif  // LIB_FDIO_DEBUG_H_
