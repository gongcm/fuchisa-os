// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "signaling_channel.h"

#include <lib/async/default.h>
#include <lib/fit/function.h>
#include <zircon/assert.h>

#include "channel.h"
#include "src/connectivity/bluetooth/core/bt-host/common/log.h"
#include "src/connectivity/bluetooth/core/bt-host/common/slab_allocator.h"

namespace bt {
namespace l2cap {
namespace internal {

SignalingChannel::SignalingChannel(fbl::RefPtr<Channel> chan, hci::Connection::Role role)
    : is_open_(true),
      chan_(std::move(chan)),
      role_(role),
      next_cmd_id_(0x01),
      weak_ptr_factory_(this) {
  ZX_DEBUG_ASSERT(chan_);
  ZX_DEBUG_ASSERT(chan_->id() == kSignalingChannelId || chan_->id() == kLESignalingChannelId);

  // Note: No need to guard against out-of-thread access as these callbacks are
  // called on the L2CAP thread.
  auto self = weak_ptr_factory_.GetWeakPtr();
  chan_->ActivateOnDataDomain(
      [self](ByteBufferPtr sdu) {
        if (self)
          self->OnRxBFrame(std::move(sdu));
      },
      [self] {
        if (self)
          self->OnChannelClosed();
      });
}

SignalingChannel::~SignalingChannel() { ZX_DEBUG_ASSERT(IsCreationThreadCurrent()); }

bool SignalingChannel::SendRequest(CommandCode req_code, const ByteBuffer& payload,
                                   ResponseHandler cb) {
  ZX_ASSERT(cb);
  const CommandId id = EnqueueResponse(req_code + 1, std::move(cb));
  if (id == kInvalidCommandId) {
    return false;
  }

  return SendPacket(req_code, id, payload);
}

void SignalingChannel::ServeRequest(CommandCode req_code, RequestDelegate cb) {
  ZX_ASSERT(!IsSupportedResponse(req_code));
  ZX_ASSERT(cb);
  inbound_handlers_[req_code] = std::move(cb);
}

CommandId SignalingChannel::EnqueueResponse(CommandCode expected_code, ResponseHandler cb) {
  ZX_ASSERT(IsSupportedResponse(expected_code));

  // Command identifiers for pending requests are assumed to be unique across
  // all types of requests and reused by order of least recent use. See v5.0
  // Vol 3, Part A Section 4.
  //
  // Uniqueness across different command types: "Within each signaling channel a
  // different Identifier shall be used for each successive command"
  // Reuse order: "the Identifier may be recycled if all other Identifiers have
  // subsequently been used"
  const CommandId initial_id = GetNextCommandId();
  CommandId id;
  for (id = initial_id; IsCommandPending(id);) {
    id = GetNextCommandId();

    if (id == initial_id) {
      bt_log(WARN, "l2cap",
             "sig: all valid command IDs in use for pending requests; can't queue expected "
             "response command %#.2x",
             expected_code);
      return kInvalidCommandId;
    }
  }

  const auto [iter, inserted] = pending_commands_.try_emplace(id, expected_code, std::move(cb));
  ZX_ASSERT(inserted);

  // Start the RTX timer per Core Spec v5.0, Volume 3, Part A, Sec 6.2.1 which will call
  // OnResponseTimeout when it expires. This timer is canceled if the response is received before
  // expiry because OnRxResponse destroys its containing PendingCommand.
  auto& rtx_task = iter->second.response_timeout_task;
  rtx_task.set_handler(std::bind(&SignalingChannel::OnResponseTimeout, this, id));
  rtx_task.PostDelayed(async_get_default_dispatcher(), kSignalingChannelResponseTimeout);
  return id;
}

bool SignalingChannel::IsCommandPending(CommandId id) const {
  return pending_commands_.find(id) != pending_commands_.end();
}

SignalingChannel::ResponderImpl::ResponderImpl(SignalingChannel* sig, CommandCode code,
                                               CommandId id)
    : sig_(sig), code_(code), id_(id) {
  ZX_DEBUG_ASSERT(sig_);
}

void SignalingChannel::ResponderImpl::Send(const ByteBuffer& rsp_payload) {
  sig()->SendPacket(code_, id_, rsp_payload);
}

void SignalingChannel::ResponderImpl::RejectNotUnderstood() {
  sig()->SendCommandReject(id_, RejectReason::kNotUnderstood, BufferView());
}

void SignalingChannel::ResponderImpl::RejectInvalidChannelId(ChannelId local_cid,
                                                             ChannelId remote_cid) {
  uint16_t ids[2];
  ids[0] = htole16(local_cid);
  ids[1] = htole16(remote_cid);
  sig()->SendCommandReject(id_, RejectReason::kInvalidCID, BufferView(ids, sizeof(ids)));
}

bool SignalingChannel::SendPacket(CommandCode code, uint8_t identifier, const ByteBuffer& data) {
  ZX_DEBUG_ASSERT(IsCreationThreadCurrent());
  return Send(BuildPacket(code, identifier, data));
}

bool SignalingChannel::HandlePacket(const SignalingPacket& packet) {
  if (IsSupportedResponse(packet.header().code)) {
    OnRxResponse(packet);
    return true;
  }

  // Handle request commands from remote.
  const auto iter = inbound_handlers_.find(packet.header().code);
  if (iter != inbound_handlers_.end()) {
    ResponderImpl responder(this, packet.header().code + 1, packet.header().id);
    iter->second(packet.payload_data(), &responder);
    return true;
  }

  bt_log(DEBUG, "l2cap", "sig: ignoring unsupported code %#.2x", packet.header().code);

  return false;
}

void SignalingChannel::OnRxResponse(const SignalingPacket& packet) {
  auto iter = pending_commands_.find(packet.header().id);
  if (iter == pending_commands_.end()) {
    bt_log(TRACE, "l2cap", "sig: ignoring unexpected response, id %#.2x", packet.header().id);
    SendCommandReject(packet.header().id, RejectReason::kNotUnderstood, BufferView());
    return;
  }

  Status status;
  auto& [_, pending_command] = *iter;
  if (packet.header().code == pending_command.expected_code) {
    status = Status::kSuccess;
  } else if (packet.header().code == kCommandRejectCode) {
    status = Status::kReject;
  } else {
    bt_log(WARN, "l2cap", "sig: response (id %#.2x) has unexpected code %#.2x", packet.header().id,
           packet.header().code);
    SendCommandReject(packet.header().id, RejectReason::kNotUnderstood, BufferView());
    return;
  }

  if (pending_command.response_handler(status, packet.payload_data()) ==
      ResponseHandlerAction::kCompleteOutboundTransaction) {
    pending_commands_.erase(iter);
  } else {
    // Renew the timer as an ERTX timer per Core Spec v5.0, Volume 3, Part A, Sec 6.2.2.
    pending_command.response_timeout_task.Cancel();
    pending_command.response_timeout_task.PostDelayed(async_get_default_dispatcher(),
                                                      kSignalingChannelExtendedResponseTimeout);
  }
}

void SignalingChannel::OnResponseTimeout(CommandId id) {
  auto node = pending_commands_.extract(id);
  ZX_ASSERT(node);
  ResponseHandler& response_handler = node.mapped().response_handler;
  response_handler(Status::kTimeOut, BufferView());
}

bool SignalingChannel::Send(ByteBufferPtr packet) {
  ZX_DEBUG_ASSERT(IsCreationThreadCurrent());
  ZX_DEBUG_ASSERT(packet);
  ZX_DEBUG_ASSERT(packet->size() >= sizeof(CommandHeader));

  if (!is_open())
    return false;

  // While 0x00 is an illegal command identifier (see v5.0, Vol 3, Part A,
  // Section 4) we don't assert that here. When we receive a command that uses
  // 0 as the identifier, we reject the command and use that identifier in the
  // response rather than assert and crash.
  __UNUSED SignalingPacket reply(packet.get(), packet->size() - sizeof(CommandHeader));
  ZX_DEBUG_ASSERT(reply.header().code);
  ZX_DEBUG_ASSERT(reply.payload_size() == le16toh(reply.header().length));
  ZX_DEBUG_ASSERT(chan_);

  return chan_->Send(std::move(packet));
}

ByteBufferPtr SignalingChannel::BuildPacket(CommandCode code, uint8_t identifier,
                                            const ByteBuffer& data) {
  ZX_DEBUG_ASSERT(data.size() <= std::numeric_limits<uint16_t>::max());

  auto buffer = NewSlabBuffer(sizeof(CommandHeader) + data.size());
  ZX_ASSERT(buffer);

  MutableSignalingPacket packet(buffer.get(), data.size());
  packet.mutable_header()->code = code;
  packet.mutable_header()->id = identifier;
  packet.mutable_header()->length = htole16(static_cast<uint16_t>(data.size()));
  packet.mutable_payload_data().Write(data);

  return buffer;
}

bool SignalingChannel::SendCommandReject(uint8_t identifier, RejectReason reason,
                                         const ByteBuffer& data) {
  ZX_DEBUG_ASSERT(data.size() <= kCommandRejectMaxDataLength);

  constexpr size_t kMaxPayloadLength = sizeof(CommandRejectPayload) + kCommandRejectMaxDataLength;
  StaticByteBuffer<kMaxPayloadLength> rej_buf;

  MutablePacketView<CommandRejectPayload> reject(&rej_buf, data.size());
  reject.mutable_header()->reason = htole16(reason);
  reject.mutable_payload_data().Write(data);

  return SendPacket(kCommandRejectCode, identifier, reject.data());
}

CommandId SignalingChannel::GetNextCommandId() {
  // Recycling identifiers is permitted and only 0x00 is invalid (v5.0 Vol 3,
  // Part A, Section 4).
  const auto cmd = next_cmd_id_++;
  if (next_cmd_id_ == kInvalidCommandId) {
    next_cmd_id_ = 0x01;
  }

  return cmd;
}

void SignalingChannel::OnChannelClosed() {
  ZX_DEBUG_ASSERT(IsCreationThreadCurrent());
  ZX_DEBUG_ASSERT(is_open());

  is_open_ = false;
}

void SignalingChannel::OnRxBFrame(ByteBufferPtr sdu) {
  ZX_DEBUG_ASSERT(IsCreationThreadCurrent());

  if (!is_open())
    return;

  DecodeRxUnit(std::move(sdu), fit::bind_member(this, &SignalingChannel::CheckAndDispatchPacket));
}

void SignalingChannel::CheckAndDispatchPacket(const SignalingPacket& packet) {
  if (packet.size() > mtu()) {
    // Respond with our signaling MTU.
    uint16_t rsp_mtu = htole16(mtu());
    BufferView rej_data(&rsp_mtu, sizeof(rsp_mtu));
    SendCommandReject(packet.header().id, RejectReason::kSignalingMTUExceeded, rej_data);
  } else if (!packet.header().id) {
    // "Signaling identifier 0x00 is an illegal identifier and shall never be
    // used in any command" (v5.0, Vol 3, Part A, Section 4).
    bt_log(DEBUG, "l2cap", "illegal signaling cmd ID: 0x00; reject");
    SendCommandReject(packet.header().id, RejectReason::kNotUnderstood, BufferView());
  } else if (!HandlePacket(packet)) {
    SendCommandReject(packet.header().id, RejectReason::kNotUnderstood, BufferView());
  }
}

}  // namespace internal
}  // namespace l2cap
}  // namespace bt
