
// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_STORAGE_MINFS_CACHED_TRANSACTION_H_
#define SRC_STORAGE_MINFS_CACHED_TRANSACTION_H_

#include <memory>
#include <utility>
#include <vector>

#include <fbl/ref_ptr.h>

#include "lib/zx/status.h"

#ifdef __Fuchsia__
#include <lib/fzl/owned-vmo-mapper.h>
#include <lib/zx/vmo.h>

#include <fbl/auto_lock.h>
#include <fbl/mutex.h>
#include <fs/transaction/writeback.h>
#endif

#include <fbl/algorithm.h>
#include <fbl/intrusive_hash_table.h>
#include <fbl/intrusive_single_list.h>
#include <fbl/macros.h>
#include <fs/queue.h>
#include <fs/transaction/buffered_operations_builder.h>
#include <fs/vfs.h>

#include "src/storage/minfs/allocator_reservation.h"
#include "src/storage/minfs/bcache.h"
#include "src/storage/minfs/format.h"
#include "src/storage/minfs/pending_work.h"

namespace minfs {

class VnodeMinfs;

// CachedTransaction holds block reservations across multiple calls. Unlike Transaction(see below),
// CachedTransaction does not require a filesystem wide lock to be held throughout the duration of
// the object's life time.
// CachedTransation currently works only for block reservation.
class CachedTransaction {
 public:
  explicit CachedTransaction(std::unique_ptr<AllocatorReservation> block_reservation)
      : block_reservation_(std::move(block_reservation)) {}

  CachedTransaction() = delete;

  // Not copyable or movable
  CachedTransaction(const CachedTransaction&) = delete;
  CachedTransaction& operator=(const CachedTransaction&) = delete;
  CachedTransaction(CachedTransaction&&) = delete;
  CachedTransaction& operator=(CachedTransaction&&) = delete;

  std::unique_ptr<AllocatorReservation> TakeBlockReservations() {
    return (std::move(block_reservation_));
  }

 private:
  std::unique_ptr<AllocatorReservation> block_reservation_;
};

}  // namespace minfs

#endif  // SRC_STORAGE_MINFS_CACHED_TRANSACTION_H_
