// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fuchsia/device/llcpp/fidl.h>
#include <fuchsia/io/llcpp/fidl.h>
#include <lib/fdio/directory.h>
#include <lib/fdio/fd.h>
#include <lib/fdio/fdio.h>
#include <lib/fdio/io.h>
#include <lib/fdio/namespace.h>
#include <string.h>
#include <vector>
#include <zircon/device/vfs.h>
#include <zircon/syscalls.h>

#include <mutex>

#include "private-socket.h"

namespace fio = ::llcpp::fuchsia::io;
namespace fsocket = ::llcpp::fuchsia::posix::socket;
namespace fdevice = ::llcpp::fuchsia::device;

#define ZXDEBUG 0

// POLL_MASK and POLL_SHIFT intend to convert the lower five POLL events into
// ZX_USER_SIGNALs and vice-versa. Other events need to be manually converted to
// a zx_signals_t, if they are desired.
#define POLL_SHIFT 24
#define POLL_MASK 0x1F

static_assert(FDIO_CHUNK_SIZE >= PATH_MAX, "FDIO_CHUNK_SIZE must be large enough to contain paths");

static_assert(fio::VMO_FLAG_READ == ZX_VM_PERM_READ, "Vmar / Vmo flags should be aligned");
static_assert(fio::VMO_FLAG_WRITE == ZX_VM_PERM_WRITE, "Vmar / Vmo flags should be aligned");
static_assert(fio::VMO_FLAG_EXEC == ZX_VM_PERM_EXECUTE, "Vmar / Vmo flags should be aligned");

static_assert(ZX_USER_SIGNAL_0 == (1 << POLL_SHIFT), "");
static_assert((POLLIN << POLL_SHIFT) == fdevice::DEVICE_SIGNAL_READABLE, "");
static_assert((POLLPRI << POLL_SHIFT) == fdevice::DEVICE_SIGNAL_OOB, "");
static_assert((POLLOUT << POLL_SHIFT) == fdevice::DEVICE_SIGNAL_WRITABLE, "");
static_assert((POLLERR << POLL_SHIFT) == fdevice::DEVICE_SIGNAL_ERROR, "");
static_assert((POLLHUP << POLL_SHIFT) == fdevice::DEVICE_SIGNAL_HANGUP, "");

// The |mode| argument used for |fuchsia.io.Directory/Open| calls.
#define FDIO_CONNECT_MODE ((uint32_t)0755)

// Validates a |path| argument.
//
// Returns ZX_OK if |path| is non-null and less than |PATH_MAX| in length
// (excluding the null terminator). Upon success, the length of the path is
// returned via |out_length|.
//
// Otherwise, returns |ZX_ERR_INVALID_ARGS|.
static zx_status_t fdio_validate_path(const char* path, size_t* out_length) {
  if (path == nullptr) {
    return ZX_ERR_INVALID_ARGS;
  }
  size_t length = strnlen(path, PATH_MAX);
  if (length >= PATH_MAX) {
    return ZX_ERR_INVALID_ARGS;
  }
  *out_length = length;
  return ZX_OK;
}

__EXPORT
zx_status_t fdio_service_connect(const char* path, zx_handle_t h) {
  return fdio_open(path, ZX_FS_RIGHT_READABLE | ZX_FS_RIGHT_WRITABLE, h);
}

__EXPORT
zx_status_t fdio_service_connect_at(zx_handle_t dir, const char* path, zx_handle_t request_raw) {
  zx::channel request(request_raw);
  size_t length = 0u;
  zx_status_t status = fdio_validate_path(path, &length);
  if (status != ZX_OK) {
    return status;
  }

  if (dir == ZX_HANDLE_INVALID) {
    return ZX_ERR_UNAVAILABLE;
  }
  uint32_t flags = ZX_FS_RIGHT_READABLE | ZX_FS_RIGHT_WRITABLE;
  return fio::Directory::Call::Open(zx::unowned_channel(dir), flags, FDIO_CONNECT_MODE,
                                    fidl::StringView(length, path), std::move(request))
      .status();
}

zx_status_t fdio_service_connect_by_name(const char name[], zx::channel* out) {
  static zx_handle_t service_root;

  {
    static std::once_flag once;
    static zx_status_t status;
    std::call_once(once, [&]() {
      once_flags.push_back(&once);
      zx::channel c0, c1;
      status = zx::channel::create(0, &c0, &c1);
      if (status != ZX_OK) {
        return;
      }
      // TODO(abarth): Use "/svc/" once that actually works.
      status = fdio_service_connect("/svc/.", c0.release());
      if (status != ZX_OK) {
        return;
      }
      service_root = c1.release();
    });
    if (status != ZX_OK) {
      return status;
    }
  }

  zx::channel c0, c1;
  zx_status_t status = zx::channel::create(0, &c0, &c1);
  if (status != ZX_OK) {
    return status;
  }

  status = fdio_service_connect_at(service_root, name, c0.release());
  if (status != ZX_OK) {
    return status;
  }
  *out = std::move(c1);
  return ZX_OK;
}

__EXPORT
zx_status_t fdio_open(const char* path, uint32_t flags, zx_handle_t request) {
  // TODO: fdio_validate_path?
  if (path == nullptr) {
    zx_handle_close(request);
    return ZX_ERR_INVALID_ARGS;
  }
  // Otherwise attempt to connect through the root namespace
  return fdio_ns_connect(fdio_root_ns, path, flags, request);
}

__EXPORT
zx_status_t fdio_open_at(zx_handle_t dir, const char* path, uint32_t flags,
                         zx_handle_t raw_request) {
  zx::channel request(raw_request);
  size_t length;
  zx_status_t status = fdio_validate_path(path, &length);
  if (status != ZX_OK) {
    return status;
  }

  if (flags & ZX_FS_FLAG_DESCRIBE) {
    return ZX_ERR_INVALID_ARGS;
  }

  return fio::Directory::Call::Open(zx::unowned_channel(dir), flags, FDIO_CONNECT_MODE,
                                    fidl::StringView(length, path), std::move(request))
      .status();
}

__EXPORT
zx_handle_t fdio_service_clone(zx_handle_t handle) {
  if (handle == ZX_HANDLE_INVALID) {
    return ZX_HANDLE_INVALID;
  }
  zx::channel clone, request;
  if (zx::channel::create(0, &clone, &request) != ZX_OK) {
    return ZX_HANDLE_INVALID;
  }
  uint32_t flags = ZX_FS_FLAG_CLONE_SAME_RIGHTS;
  auto result = fio::Node::Call::Clone(zx::unowned_channel(handle), flags, std::move(request));
  if (result.status() != ZX_OK) {
    return ZX_HANDLE_INVALID;
  }
  return clone.release();
}

__EXPORT
zx_status_t fdio_service_clone_to(zx_handle_t handle, zx_handle_t request_raw) {
  zx::channel request(request_raw);
  if (!request.is_valid()) {
    return ZX_ERR_INVALID_ARGS;
  }
  uint32_t flags = ZX_FS_FLAG_CLONE_SAME_RIGHTS;
  return fio::Node::Call::Clone(zx::unowned_channel(handle), flags, std::move(request)).status();
}

// Create an |fdio_t| from a |handle| and an |info|.
//
// Uses |info| to determine what kind of |fdio_t| to create.
//
// Upon success, |out_io| receives ownership of all handles.
//
// Upon failure, consumes all handles.
static zx_status_t fdio_from_node_info(zx::channel handle, fio::NodeInfo info, fdio_t** out_io) {
  if (!handle.is_valid()) {
    return ZX_ERR_INVALID_ARGS;
  }

  fdio_t* io = nullptr;
  switch (info.which()) {
    case fio::NodeInfo::Tag::kDirectory:
      io = fdio_dir_create(handle.release());
      break;
    case fio::NodeInfo::Tag::kService:
      io = fdio_remote_create(handle.release(), 0);
      break;
    case fio::NodeInfo::Tag::kFile:
      io = fdio_file_create(handle.release(), info.mutable_file().event.release());
      break;
    case fio::NodeInfo::Tag::kDevice:
      io = fdio_remote_create(handle.release(), info.mutable_device().event.release());
      break;
    case fio::NodeInfo::Tag::kTty:
      io = fdio_remote_create(handle.release(), info.mutable_tty().event.release());
      break;
    case fio::NodeInfo::Tag::kVmofile: {
      auto result =
          fio::File::Call::Seek(zx::unowned_channel(handle.get()), 0, fio::SeekOrigin::START);
      zx_status_t status = result.status();
      if (status != ZX_OK) {
        return status;
      }
      fio::File::SeekResponse* response = result.Unwrap();
      status = response->s;
      if (status != ZX_OK) {
        return status;
      }
      io = fdio_vmofile_create(handle.release(), info.mutable_vmofile().vmo.release(),
                               info.vmofile().offset, info.vmofile().length, response->offset);
      break;
    }
    case fio::NodeInfo::Tag::kPipe: {
      io = fdio_pipe_create(std::move(info.mutable_pipe().socket));
      break;
    }
    case fio::NodeInfo::Tag::kSocket: {
      // check the connection state.
      zx_signals_t observed;

      switch (zx_status_t status = info.socket().socket.wait_one(
                  ZXSIO_SIGNAL_CONNECTED, zx::time::infinite_past(), &observed)) {
        case ZX_OK:
          __FALLTHROUGH;
        case ZX_ERR_TIMED_OUT:
          break;
        default:
          return status;
      }

      zx_status_t status = fdio_socket_create(fsocket::Control::SyncClient(std::move(handle)),
                                              std::move(info.mutable_socket().socket), &io);
      if (status != ZX_OK) {
        return status;
      }
      if (observed & ZXSIO_SIGNAL_CONNECTED) {
        *fdio_get_ioflag(io) |= IOFLAG_SOCKET_CONNECTED;
      }
      break;
    }
    default:
      return ZX_ERR_NOT_SUPPORTED;
  }

  if (io == nullptr) {
    return ZX_ERR_NO_RESOURCES;
  }

  *out_io = io;
  return ZX_OK;
}

// Creates an |fdio_t| from a Zircon channel object.
//
// The |channel| must implement the |fuchsia.io.Node| protocol. Uses the
// |Describe| method from the |fuchsia.io.Node| protocol to determine the type
// of |fdio_t| object to create.
//
// Always consumes |channel|.
static zx_status_t fdio_from_channel(zx::channel channel, fdio_t** out_io) {
  auto response = fio::Node::Call::Describe(zx::unowned_channel(channel));
  zx_status_t status = response.status();
  if (status != ZX_OK) {
    return status;
  }
  return fdio_from_node_info(std::move(channel), std::move(response.Unwrap()->info), out_io);
}

__EXPORT
zx_status_t fdio_create(zx_handle_t handle, fdio_t** out_io) {
  zx_info_handle_basic_t info;
  zx_status_t status =
      zx_object_get_info(handle, ZX_INFO_HANDLE_BASIC, &info, sizeof(info), nullptr, nullptr);
  if (status != ZX_OK) {
    return status;
  }
  fdio_t* io = nullptr;
  switch (info.type) {
    case ZX_OBJ_TYPE_CHANNEL:
      return fdio_from_channel(zx::channel(handle), out_io);
    case ZX_OBJ_TYPE_SOCKET:
      io = fdio_pipe_create(zx::socket(handle));
      break;
    case ZX_OBJ_TYPE_VMO:
      io = fdio_vmo_create(handle, 0u);
      break;
    case ZX_OBJ_TYPE_LOG:
      io = fdio_logger_create(handle);
      break;
    default: {
      zx_handle_close(handle);
      return ZX_ERR_INVALID_ARGS;
    }
  }
  if (io == nullptr) {
    return ZX_ERR_NO_MEMORY;
  }
  *out_io = io;
  return ZX_OK;
}

zx_status_t fdio_remote_open_at(zx_handle_t dir, const char* path, uint32_t flags, uint32_t mode,
                                fdio_t** out_io) {
  size_t length;
  zx_status_t status = fdio_validate_path(path, &length);
  if (status != ZX_OK) {
    return status;
  }

  zx::channel handle, request;
  status = zx::channel::create(0, &handle, &request);
  if (status != ZX_OK) {
    return status;
  }

  status = fio::Directory::Call::Open(zx::unowned_channel(dir), flags, mode,
                                      fidl::StringView(length, path), std::move(request))
               .status();
  if (status != ZX_OK) {
    return status;
  }

  if (flags & ZX_FS_FLAG_DESCRIBE) {
    fio::NodeInfo node_info;
    zx_status_t on_open_status = ZX_OK;
    status = fio::Directory::Call::HandleEvents(
        zx::unowned_channel(handle),
        fio::Directory::EventHandlers{
            .on_open =
                [&node_info, &on_open_status](zx_status_t status, fio::NodeInfo* info) {
                  on_open_status = status;
                  if (info) {
                    node_info = std::move(*info);
                  }
                  return ZX_OK;
                },
            .unknown = [] { return ZX_ERR_IO; }});
    if (status != ZX_OK) {
      return status;
    }
    if (on_open_status != ZX_OK) {
      return on_open_status;
    }
    return fdio_from_node_info(std::move(handle), std::move(node_info), out_io);
  }

  fdio_t* io = fdio_remote_create(handle.release(), 0);
  if (io == nullptr) {
    return ZX_ERR_NO_RESOURCES;
  }
  *out_io = io;
  return ZX_OK;
}
