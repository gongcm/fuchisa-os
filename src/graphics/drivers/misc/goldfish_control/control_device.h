// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_GRAPHICS_DRIVERS_MISC_GOLDFISH_CONTROL_CONTROL_DEVICE_H_
#define SRC_GRAPHICS_DRIVERS_MISC_GOLDFISH_CONTROL_CONTROL_DEVICE_H_

#include <fuchsia/hardware/goldfish/llcpp/fidl.h>
#include <lib/fit/result.h>
#include <lib/zircon-internal/thread_annotations.h>
#include <zircon/types.h>

#include <map>

#include <ddk/device.h>
#include <ddk/io-buffer.h>
#include <ddktl/device.h>
#include <ddktl/protocol/goldfish/addressspace.h>
#include <ddktl/protocol/goldfish/control.h>
#include <ddktl/protocol/goldfish/pipe.h>
#include <fbl/condition_variable.h>
#include <fbl/intrusive_double_list.h>
#include <fbl/mutex.h>

#include "src/graphics/drivers/misc/goldfish_control/heap.h"

namespace goldfish {

class Control;
using ControlType = ddk::Device<Control, ddk::Unbindable, ddk::Messageable, ddk::GetProtocolable>;

class Control : public ControlType,
                public ddk::GoldfishControlProtocol<Control, ddk::base_protocol>,
                public llcpp::fuchsia::hardware::goldfish::ControlDevice::Interface {
 public:
  static zx_status_t Create(void* ctx, zx_device_t* parent);

  explicit Control(zx_device_t* parent);
  ~Control();

  zx_status_t Bind();

  uint64_t RegisterBufferHandle(const zx::vmo& vmo);
  void FreeBufferHandle(uint64_t id);

  using CreateColorBuffer2Result =
      fit::result<llcpp::fuchsia::hardware::goldfish::ControlDevice::CreateColorBuffer2Response,
                  zx_status_t>;

  CreateColorBuffer2Result CreateColorBuffer2(
      zx::vmo vmo, llcpp::fuchsia::hardware::goldfish::CreateColorBuffer2Params create_params);

  // |llcpp::fuchsia::hardware::goldfish::ControlDevice::Interface|
  void CreateColorBuffer2(
      zx::vmo vmo, llcpp::fuchsia::hardware::goldfish::CreateColorBuffer2Params create_params,
      CreateColorBuffer2Completer::Sync& completer) override;

  using CreateBuffer2Result =
      fit::result<llcpp::fuchsia::hardware::goldfish::ControlDevice_CreateBuffer2_Result,
                  zx_status_t>;

  CreateBuffer2Result CreateBuffer2(
      zx::vmo vmo, llcpp::fuchsia::hardware::goldfish::CreateBuffer2Params create_params);

  // |llcpp::fuchsia::hardware::goldfish::ControlDevice::Interface|
  void CreateBuffer2(zx::vmo vmo,
                     llcpp::fuchsia::hardware::goldfish::CreateBuffer2Params create_params,
                     CreateBuffer2Completer::Sync& completer) override;

  // |llcpp::fuchsia::hardware::goldfish::ControlDevice::Interface|
  void GetBufferHandle(zx::vmo vmo, GetBufferHandleCompleter::Sync& completer) override;

  // Device protocol implementation.
  void DdkUnbind(ddk::UnbindTxn txn);
  void DdkRelease();
  zx_status_t DdkMessage(fidl_incoming_msg_t* msg, fidl_txn_t* txn);
  zx_status_t DdkGetProtocol(uint32_t proto_id, void* out_protocol);
  zx_status_t GoldfishControlGetColorBuffer(zx::vmo vmo, uint32_t* out_id);

  // Used by heaps. Removes a specific heap from the linked list.
  void RemoveHeap(Heap* heap);

  llcpp::fuchsia::hardware::goldfish::AddressSpaceChildDriver::SyncClient* address_space_child()
      const {
    return address_space_child_.get();
  }

 private:
  zx_status_t Init();

  zx_status_t InitAddressSpaceDeviceLocked() TA_REQ(lock_);
  zx_status_t InitPipeDeviceLocked() TA_REQ(lock_);

  // Create a pair of channel and register a sysmem Heap of |heap_type| using
  // the channel pair. The client-side channel is sent to sysmem, and the
  // server-side channel is bound to |heap|.
  zx_status_t RegisterAndBindHeap(llcpp::fuchsia::sysmem2::HeapType heap_type, Heap* heap);

  int32_t WriteLocked(uint32_t cmd_size, int32_t* consumed_size) TA_REQ(lock_);
  void WriteLocked(uint32_t cmd_size) TA_REQ(lock_);
  zx_status_t ReadResultLocked(uint32_t* result) TA_REQ(lock_);
  zx_status_t ExecuteCommandLocked(uint32_t cmd_size, uint32_t* result) TA_REQ(lock_);
  zx_status_t CreateBuffer2Locked(uint64_t size, uint32_t memory_property, uint32_t* id)
      TA_REQ(lock_);
  zx_status_t CreateColorBufferLocked(uint32_t width, uint32_t height, uint32_t format,
                                      uint32_t* id) TA_REQ(lock_);
  void CloseBufferOrColorBufferLocked(uint32_t id) TA_REQ(lock_);
  void CloseBufferLocked(uint32_t id) TA_REQ(lock_);
  void CloseColorBufferLocked(uint32_t id) TA_REQ(lock_);
  zx_status_t SetColorBufferVulkanModeLocked(uint32_t id, uint32_t mode, uint32_t* result)
      TA_REQ(lock_);
  zx_status_t SetColorBufferVulkanMode2Locked(uint32_t id, uint32_t mode, uint32_t memory_property,
                                              uint32_t* result) TA_REQ(lock_);
  zx_status_t MapGpaToBufferHandleLocked(uint32_t id, uint64_t gpa, uint64_t size, uint32_t* result)
      TA_REQ(lock_);

  fbl::Mutex lock_;
  ddk::GoldfishPipeProtocolClient pipe_;
  ddk::GoldfishControlProtocolClient control_;
  ddk::GoldfishAddressSpaceProtocolClient address_space_;
  int32_t id_ = 0;
  zx::bti bti_ TA_GUARDED(lock_);
  ddk::IoBuffer cmd_buffer_ TA_GUARDED(lock_);
  ddk::IoBuffer io_buffer_ TA_GUARDED(lock_);

  fbl::DoublyLinkedList<std::unique_ptr<Heap>> heaps_ TA_GUARDED(lock_);

  zx::event pipe_event_;

  std::unique_ptr<llcpp::fuchsia::hardware::goldfish::AddressSpaceChildDriver::SyncClient>
      address_space_child_;

  // TODO(fxbug.dev/3213): This should be std::unordered_map.
  std::map<zx_koid_t, uint32_t> buffer_handles_ TA_GUARDED(lock_);
  std::map<uint32_t, llcpp::fuchsia::hardware::goldfish::BufferHandleType> buffer_handle_types_
      TA_GUARDED(lock_);

  DISALLOW_COPY_ASSIGN_AND_MOVE(Control);
};

}  // namespace goldfish

#endif  // SRC_GRAPHICS_DRIVERS_MISC_GOLDFISH_CONTROL_CONTROL_DEVICE_H_
