// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/storage/minfs/minfs_private.h"
#include "zircon/types.h"

namespace minfs {

[[nodiscard]] bool Minfs::IsErrored() {
  return journal_ != nullptr && !journal_->IsWritebackEnabled();
}

std::vector<fbl::RefPtr<VnodeMinfs>> Minfs::GetDirtyVnodes() {
  fbl::RefPtr<VnodeMinfs> vn;
  std::vector<fbl::RefPtr<VnodeMinfs>> vnodes;
  {
    // Avoid releasing a reference to |vn| while holding |hash_lock_|.
    fbl::AutoLock lock(&hash_lock_);
    for (auto& raw_vnode : vnode_hash_) {
      vn = fbl::MakeRefPtrUpgradeFromRaw(&raw_vnode, hash_lock_);
      if (vn != nullptr && vn->IsDirty()) {
        vnodes.push_back(std::move(vn));
      }
    }
  }
  return vnodes;
}

zx_status_t Minfs::ContinueTransaction(size_t reserve_blocks,
                                       std::unique_ptr<CachedTransaction> cached_transaction,
                                       std::unique_ptr<Transaction>* out) {
  if (journal_ == nullptr) {
    return ZX_ERR_BAD_STATE;
  }

  if (!journal_->IsWritebackEnabled()) {
    return ZX_ERR_IO_REFUSED;
  }

  // TODO(planders): Once we are splitting up write
  // transactions, assert this on host as well.
  ZX_DEBUG_ASSERT(reserve_blocks <= limits_.GetMaximumDataBlocks());

  *out = Transaction::FromCachedTransaction(this, std::move(cached_transaction));
  // Reserve blocks from allocators before returning
  // WritebackWork to client.
  return (*out)->ExtendBlockReservation(reserve_blocks);
}

zx::status<> Minfs::AddDirtyBytes(uint64_t dirty_bytes, bool allocated) {
  if (!allocated) {
    fbl::AutoLock lock(&hash_lock_);
    // We need to allocate the block. Make sure that we have
    // enough space.
    uint32_t blocks_needed = BlocksReserved();
    uint32_t local_blocks_available = Info().block_count - Info().alloc_block_count;
    if (blocks_needed > local_blocks_available) {
      // Check if fvm has free slices.
      fuchsia_hardware_block_volume_VolumeInfo fvm_info;
      if (FVMQuery(&fvm_info) != ZX_OK) {
        return zx::error(ZX_ERR_NO_SPACE);
      }
      uint64_t free_slices = fvm_info.pslice_total_count - fvm_info.pslice_allocated_count;
      uint64_t blocks_available =
          local_blocks_available + (fvm_info.slice_size * free_slices / Info().BlockSize());
      if (blocks_needed > blocks_available) {
        return zx::error(ZX_ERR_NO_SPACE);
      }
    }
  }
  metrics_.dirty_bytes += dirty_bytes;

  return zx::ok();
}

void Minfs::RemoveDirtyBytes(uint64_t dirty_bytes, bool allocated) {
  ZX_ASSERT(dirty_bytes <= metrics_.dirty_bytes.load());
  metrics_.dirty_bytes -= dirty_bytes;
}

}  // namespace minfs
