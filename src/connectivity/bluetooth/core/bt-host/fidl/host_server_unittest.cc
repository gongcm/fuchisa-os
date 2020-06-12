// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/connectivity/bluetooth/core/bt-host/fidl/host_server.h"

#include <fuchsia/bluetooth/control/cpp/fidl.h>
#include <fuchsia/bluetooth/cpp/fidl.h>
#include <fuchsia/bluetooth/sys/cpp/fidl.h>
#include <fuchsia/bluetooth/sys/cpp/fidl_test_base.h>
#include <lib/inspect/testing/cpp/inspect.h>
#include <lib/zx/channel.h>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "adapter_test_fixture.h"
#include "helpers.h"
#include "src/connectivity/bluetooth/core/bt-host/common/byte_buffer.h"
#include "src/connectivity/bluetooth/core/bt-host/common/device_address.h"
#include "src/connectivity/bluetooth/core/bt-host/common/test_helpers.h"
#include "src/connectivity/bluetooth/core/bt-host/data/fake_domain.h"
#include "src/connectivity/bluetooth/core/bt-host/fidl/helpers.h"
#include "src/connectivity/bluetooth/core/bt-host/gap/gap.h"
#include "src/connectivity/bluetooth/core/bt-host/gatt/fake_layer.h"
#include "src/connectivity/bluetooth/core/bt-host/gatt_host.h"
#include "src/connectivity/bluetooth/core/bt-host/l2cap/fake_channel.h"
#include "src/connectivity/bluetooth/core/bt-host/testing/fake_controller_test.h"
#include "src/connectivity/bluetooth/core/bt-host/testing/fake_peer.h"
#include "src/connectivity/bluetooth/core/bt-host/testing/test_packets.h"

namespace bthost {
namespace {

using namespace inspect::testing;

// Limiting the de-scoped aliases here helps test cases be more specific about whether they're using
// FIDL names or bt-host internal names.
using bt::CreateStaticByteBuffer;
using bt::LowerBits;
using bt::UpperBits;
using bt::l2cap::testing::FakeChannel;
using bt::testing::FakePeer;

namespace fbt = fuchsia::bluetooth;
namespace fctrl = fuchsia::bluetooth::control;
namespace fsys = fuchsia::bluetooth::sys;

const bt::DeviceAddress kLeTestAddr(bt::DeviceAddress::Type::kLEPublic, {0x01, 0, 0, 0, 0, 0});
const bt::DeviceAddress kBredrTestAddr(bt::DeviceAddress::Type::kBREDR, {0x01, 0, 0, 0, 0, 0});

class MockPairingDelegate : public fsys::testing::PairingDelegate_TestBase {
 public:
  MockPairingDelegate(fidl::InterfaceRequest<PairingDelegate> request,
                      async_dispatcher_t* dispatcher)
      : binding_(this, std::move(request), dispatcher) {}

  ~MockPairingDelegate() override = default;

  MOCK_METHOD(void, OnPairingRequest,
              (fsys::Peer device, fsys::PairingMethod method, uint32_t displayed_passkey,
               OnPairingRequestCallback callback),
              (override));
  MOCK_METHOD(void, OnPairingComplete, (fbt::PeerId id, bool success), (override));
  MOCK_METHOD(void, OnRemoteKeypress, (fbt::PeerId id, fsys::PairingKeypress keypress), (override));

 private:
  fidl::Binding<PairingDelegate> binding_;

  void NotImplemented_(const std::string& name) override {
    FAIL() << name << " is not implemented";
  }
};

class FIDL_HostServerTest : public bthost::testing::AdapterTestFixture {
 public:
  FIDL_HostServerTest() = default;
  ~FIDL_HostServerTest() override = default;

  void SetUp() override {
    AdapterTestFixture::SetUp();

    gatt_host_ = GattHost::CreateForTesting(dispatcher(), gatt());
    ResetHostServer();
  }

  void ResetHostServer() {
    fidl::InterfaceHandle<fuchsia::bluetooth::host::Host> host_handle;
    host_server_ = std::make_unique<HostServer>(host_handle.NewRequest().TakeChannel(),
                                                adapter()->AsWeakPtr(), gatt_host_);
    host_.Bind(std::move(host_handle));
  }

  void TearDown() override {
    RunLoopUntilIdle();

    host_ = nullptr;
    host_server_ = nullptr;
    gatt_host_->ShutDown();
    gatt_host_ = nullptr;

    RunLoopUntilIdle();
    AdapterTestFixture::TearDown();
  }

 protected:
  HostServer* host_server() const { return host_server_.get(); }

  fuchsia::bluetooth::host::Host* host_client() const { return host_.get(); }

  // Create and bind a MockPairingDelegate and attach it to the HostServer under test. It is
  // heap-allocated to permit its explicit destruction.
  [[nodiscard]] std::unique_ptr<MockPairingDelegate> SetMockPairingDelegate(
      fsys::InputCapability input_capability, fsys::OutputCapability output_capability) {
    using ::testing::StrictMock;
    fidl::InterfaceHandle<fsys::PairingDelegate> pairing_delegate_handle;
    auto pairing_delegate = std::make_unique<StrictMock<MockPairingDelegate>>(
        pairing_delegate_handle.NewRequest(), dispatcher());
    host_client()->SetPairingDelegate(input_capability, output_capability,
                                      std::move(pairing_delegate_handle));

    // Wait for the Control/SetPairingDelegate message to process.
    RunLoopUntilIdle();
    return pairing_delegate;
  }

  std::tuple<bt::gap::Peer*, FakeChannel*> ConnectFakePeer(bool connect_le = true) {
    auto device_addr = connect_le ? kLeTestAddr : kBredrTestAddr;
    auto* peer = adapter()->peer_cache()->NewPeer(device_addr, true);
    EXPECT_TRUE(peer->temporary());
    // This is to capture the channel created during the Connection process
    FakeChannel* fake_chan = nullptr;
    data_domain()->set_channel_callback(
        [&fake_chan](fbl::RefPtr<FakeChannel> new_fake_chan) { fake_chan = new_fake_chan.get(); });

    auto fake_peer = std::make_unique<FakePeer>(device_addr);
    test_device()->AddPeer(std::move(fake_peer));

    std::optional<fit::result<void, fsys::Error>> connect_result;
    host_client()->Connect(fbt::PeerId{peer->identifier().value()}, [&](auto result) {
      ASSERT_FALSE(result.is_err());
      connect_result = std::move(result);
    });
    RunLoopUntilIdle();

    if (!connect_result || connect_result->is_error()) {
      peer = nullptr;
      fake_chan = nullptr;
    }
    return std::make_tuple(peer, fake_chan);
  }

 private:
  std::unique_ptr<HostServer> host_server_;
  fbl::RefPtr<GattHost> gatt_host_;
  fuchsia::bluetooth::host::HostPtr host_;

  DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(FIDL_HostServerTest);
};

TEST_F(FIDL_HostServerTest, FidlIoCapabilitiesMapToHostIoCapability) {
  // Isolate HostServer's private bt::gap::PairingDelegate implementation.
  auto host_pairing_delegate = static_cast<bt::gap::PairingDelegate*>(host_server());

  // Getter should be safe to call when no PairingDelegate assigned.
  EXPECT_EQ(bt::sm::IOCapability::kNoInputNoOutput, host_pairing_delegate->io_capability());

  auto fidl_pairing_delegate =
      SetMockPairingDelegate(fsys::InputCapability::KEYBOARD, fsys::OutputCapability::DISPLAY);
  EXPECT_EQ(bt::sm::IOCapability::kKeyboardDisplay, host_pairing_delegate->io_capability());
}

TEST_F(FIDL_HostServerTest, HostCompletePairingCallsFidlOnPairingComplete) {
  using namespace ::testing;

  // Isolate HostServer's private bt::gap::PairingDelegate implementation.
  auto host_pairing_delegate = static_cast<bt::gap::PairingDelegate*>(host_server());
  auto fidl_pairing_delegate =
      SetMockPairingDelegate(fsys::InputCapability::KEYBOARD, fsys::OutputCapability::DISPLAY);

  // fuchsia::bluetooth::PeerId has no equality operator
  EXPECT_CALL(*fidl_pairing_delegate, OnPairingComplete(_, false))
      .WillOnce([](fbt::PeerId id, Unused) { EXPECT_EQ(0xc0decafe, id.value); });
  host_pairing_delegate->CompletePairing(bt::PeerId(0xc0decafe),
                                         bt::sm::Status(bt::sm::ErrorCode::kConfirmValueFailed));

  // Wait for the PairingDelegate/OnPairingComplete message to process.
  RunLoopUntilIdle();
}

TEST_F(FIDL_HostServerTest, HostConfirmPairingRequestsConsentPairingOverFidl) {
  using namespace ::testing;
  auto host_pairing_delegate = static_cast<bt::gap::PairingDelegate*>(host_server());
  auto fidl_pairing_delegate =
      SetMockPairingDelegate(fsys::InputCapability::KEYBOARD, fsys::OutputCapability::DISPLAY);

  auto* const peer = adapter()->peer_cache()->NewPeer(kLeTestAddr, /*connectable=*/true);
  ASSERT_TRUE(peer);

  EXPECT_CALL(*fidl_pairing_delegate, OnPairingRequest(_, fsys::PairingMethod::CONSENT, 0u, _))
      .WillOnce(
          [id = peer->identifier()](fsys::Peer peer, Unused, Unused,
                                    fsys::PairingDelegate::OnPairingRequestCallback callback) {
            ASSERT_TRUE(peer.has_id());
            EXPECT_EQ(id.value(), peer.id().value);
            callback(/*accept=*/true, /*entered_passkey=*/0u);
          });

  bool confirm_cb_value = false;
  bt::gap::PairingDelegate::ConfirmCallback confirm_cb = [&confirm_cb_value](bool confirmed) {
    confirm_cb_value = confirmed;
  };
  host_pairing_delegate->ConfirmPairing(peer->identifier(), std::move(confirm_cb));

  // Wait for the PairingDelegate/OnPairingRequest message to process.
  RunLoopUntilIdle();
  EXPECT_TRUE(confirm_cb_value);
}

TEST_F(FIDL_HostServerTest,
       HostDisplayPasskeyRequestsPasskeyDisplayOrNumericComparisonPairingOverFidl) {
  using namespace ::testing;
  auto host_pairing_delegate = static_cast<bt::gap::PairingDelegate*>(host_server());
  auto fidl_pairing_delegate =
      SetMockPairingDelegate(fsys::InputCapability::KEYBOARD, fsys::OutputCapability::DISPLAY);

  auto* const peer = adapter()->peer_cache()->NewPeer(kLeTestAddr, /*connectable=*/true);
  ASSERT_TRUE(peer);

  // This call should use PASSKEY_DISPLAY to request that the user perform peer passkey entry.
  using OnPairingRequestCallback = fsys::PairingDelegate::OnPairingRequestCallback;
  EXPECT_CALL(*fidl_pairing_delegate,
              OnPairingRequest(_, fsys::PairingMethod::PASSKEY_DISPLAY, 12345, _))
      .WillOnce([id = peer->identifier()](fsys::Peer peer, Unused, Unused,
                                          OnPairingRequestCallback callback) {
        ASSERT_TRUE(peer.has_id());
        EXPECT_EQ(id.value(), peer.id().value);
        callback(/*accept=*/false, /*entered_passkey=*/0u);
      });

  bool confirm_cb_called = false;
  auto confirm_cb = [&confirm_cb_called](bool confirmed) {
    EXPECT_FALSE(confirmed);
    confirm_cb_called = true;
  };
  using DisplayMethod = bt::gap::PairingDelegate::DisplayMethod;
  host_pairing_delegate->DisplayPasskey(peer->identifier(), 12345, DisplayMethod::kPeerEntry,
                                        confirm_cb);

  // Wait for the PairingDelegate/OnPairingRequest message to process.
  RunLoopUntilIdle();
  EXPECT_TRUE(confirm_cb_called);

  // This call should use PASSKEY_COMPARISON to request that the user compare the passkeys shown on
  // the local and peer devices.
  EXPECT_CALL(*fidl_pairing_delegate,
              OnPairingRequest(_, fsys::PairingMethod::PASSKEY_COMPARISON, 12345, _))
      .WillOnce([id = peer->identifier()](fsys::Peer peer, Unused, Unused,
                                          OnPairingRequestCallback callback) {
        ASSERT_TRUE(peer.has_id());
        EXPECT_EQ(id.value(), peer.id().value);
        callback(/*accept=*/false, /*entered_passkey=*/0u);
      });

  confirm_cb_called = false;
  host_pairing_delegate->DisplayPasskey(peer->identifier(), 12345, DisplayMethod::kComparison,
                                        confirm_cb);

  // Wait for the PairingDelegate/OnPairingRequest message to process.
  RunLoopUntilIdle();
  EXPECT_TRUE(confirm_cb_called);
}

TEST_F(FIDL_HostServerTest, HostRequestPasskeyRequestsPasskeyEntryPairingOverFidl) {
  using namespace ::testing;
  auto host_pairing_delegate = static_cast<bt::gap::PairingDelegate*>(host_server());
  auto fidl_pairing_delegate =
      SetMockPairingDelegate(fsys::InputCapability::KEYBOARD, fsys::OutputCapability::DISPLAY);

  auto* const peer = adapter()->peer_cache()->NewPeer(kLeTestAddr, /*connectable=*/true);
  ASSERT_TRUE(peer);

  using OnPairingRequestCallback = fsys::PairingDelegate::OnPairingRequestCallback;
  EXPECT_CALL(*fidl_pairing_delegate,
              OnPairingRequest(_, fsys::PairingMethod::PASSKEY_ENTRY, 0u, _))
      .WillOnce([id = peer->identifier()](fsys::Peer peer, Unused, Unused,
                                          OnPairingRequestCallback callback) {
        ASSERT_TRUE(peer.has_id());
        EXPECT_EQ(id.value(), peer.id().value);
        callback(/*accept=*/false, 12345);
      })
      .WillOnce([id = peer->identifier()](fsys::Peer peer, Unused, Unused,
                                          OnPairingRequestCallback callback) {
        ASSERT_TRUE(peer.has_id());
        EXPECT_EQ(id.value(), peer.id().value);
        callback(/*accept=*/true, 0u);
      })
      .WillOnce([id = peer->identifier()](fsys::Peer peer, Unused, Unused,
                                          OnPairingRequestCallback callback) {
        ASSERT_TRUE(peer.has_id());
        EXPECT_EQ(id.value(), peer.id().value);
        callback(/*accept=*/true, 12345);
      });

  std::optional<int64_t> passkey_response;
  auto response_cb = [&passkey_response](int64_t passkey) { passkey_response = passkey; };

  // The first request is rejected and should receive a negative passkey value, regardless what was
  // passed over FIDL (i.e. 12345).
  host_pairing_delegate->RequestPasskey(peer->identifier(), response_cb);
  RunLoopUntilIdle();
  ASSERT_TRUE(passkey_response.has_value());
  EXPECT_LT(passkey_response.value(), 0);

  // The second request should be accepted with the passkey set to "0".
  passkey_response.reset();
  host_pairing_delegate->RequestPasskey(peer->identifier(), response_cb);
  RunLoopUntilIdle();
  ASSERT_TRUE(passkey_response.has_value());
  EXPECT_EQ(0, passkey_response.value());

  // The third request should be accepted with the passkey set to "12345".
  passkey_response.reset();
  host_pairing_delegate->RequestPasskey(peer->identifier(), response_cb);
  RunLoopUntilIdle();
  ASSERT_TRUE(passkey_response.has_value());
  EXPECT_EQ(12345, passkey_response.value());
}

TEST_F(FIDL_HostServerTest, WatchState) {
  std::optional<fsys::HostInfo> info;
  host_server()->WatchState([&](auto value) { info = std::move(value); });
  ASSERT_TRUE(info.has_value());
  ASSERT_TRUE(info->has_id());
  ASSERT_TRUE(info->has_technology());
  ASSERT_TRUE(info->has_address());
  ASSERT_TRUE(info->has_local_name());
  ASSERT_TRUE(info->has_discoverable());
  ASSERT_TRUE(info->has_discovering());

  EXPECT_EQ(adapter()->identifier().value(), info->id().value);
  EXPECT_EQ(fsys::TechnologyType::DUAL_MODE, info->technology());
  EXPECT_EQ(fbt::AddressType::PUBLIC, info->address().type);
  EXPECT_TRUE(
      ContainersEqual(adapter()->state().controller_address().bytes(), info->address().bytes));
  EXPECT_EQ("fuchsia", info->local_name());
  EXPECT_FALSE(info->discoverable());
  EXPECT_FALSE(info->discovering());
}

TEST_F(FIDL_HostServerTest, WatchDiscoveryState) {
  std::optional<fsys::HostInfo> info;

  // Make initial watch call so that subsequent calls remain pending.
  host_server()->WatchState([&](auto value) { info = std::move(value); });
  ASSERT_TRUE(info.has_value());
  info.reset();

  // Watch for updates.
  host_server()->WatchState([&](auto value) { info = std::move(value); });
  EXPECT_FALSE(info.has_value());

  host_server()->StartDiscovery([](auto) {});
  RunLoopUntilIdle();
  ASSERT_TRUE(info.has_value());
  ASSERT_TRUE(info->has_discovering());
  EXPECT_TRUE(info->discovering());

  info.reset();
  host_server()->WatchState([&](auto value) { info = std::move(value); });
  EXPECT_FALSE(info.has_value());
  host_server()->StopDiscovery();
  RunLoopUntilIdle();
  ASSERT_TRUE(info.has_value());
  ASSERT_TRUE(info->has_discovering());
  EXPECT_FALSE(info->discovering());
}

TEST_F(FIDL_HostServerTest, WatchDiscoverableState) {
  std::optional<fsys::HostInfo> info;

  // Make initial watch call so that subsequent calls remain pending.
  host_server()->WatchState([&](auto value) { info = std::move(value); });
  ASSERT_TRUE(info.has_value());
  info.reset();

  // Watch for updates.
  host_server()->WatchState([&](auto value) { info = std::move(value); });
  EXPECT_FALSE(info.has_value());

  host_server()->SetDiscoverable(true, [](auto) {});
  RunLoopUntilIdle();
  ASSERT_TRUE(info.has_value());
  ASSERT_TRUE(info->has_discoverable());
  EXPECT_TRUE(info->discoverable());

  info.reset();
  host_server()->WatchState([&](auto value) { info = std::move(value); });
  EXPECT_FALSE(info.has_value());
  host_server()->SetDiscoverable(false, [](auto) {});
  RunLoopUntilIdle();
  ASSERT_TRUE(info.has_value());
  ASSERT_TRUE(info->has_discoverable());
  EXPECT_FALSE(info->discoverable());
}

TEST_F(FIDL_HostServerTest, InitiatePairingLeDefault) {
  // clang-format off
  const auto kExpected = CreateStaticByteBuffer(
      0x01,  // code: "Pairing Request"
      0x03,  // IO cap.: NoInputNoOutput
      0x00,  // OOB: not present
      0x0D,  // AuthReq: bonding, MITM (Authenticated), Secure Connections
      0x10,  // encr. key size: 16 (default max)
      0x00,  // initiator keys: none
      0x03   // responder keys: enc key and identity info
  );
  // clang-format on

  auto [peer, fake_chan] = ConnectFakePeer();
  ASSERT_TRUE(peer);
  ASSERT_TRUE(fake_chan);
  ASSERT_EQ(bt::gap::Peer::ConnectionState::kConnected, peer->le()->connection_state());

  bool pairing_request_sent = false;
  // This test only checks that PairingState kicks off an LE pairing feature exchange correctly, as
  // the call to Pair is only responsible for starting pairing, not for completing it.
  auto expect_default_bytebuffer = [&pairing_request_sent, kExpected](bt::ByteBufferPtr sent) {
    ASSERT_TRUE(sent);
    ASSERT_EQ(*sent, kExpected);
    pairing_request_sent = true;
  };
  fake_chan->SetSendCallback(expect_default_bytebuffer, dispatcher());

  std::optional<fit::result<void, fsys::Error>> pair_result;
  fctrl::PairingOptions opts;
  host_client()->Pair(fbt::PeerId{peer->identifier().value()}, std::move(opts),
                      [&](auto result) { pair_result = std::move(result); });
  RunLoopUntilIdle();

  // TODO(fxb/886): We don't have a good mechanism for driving pairing to completion without faking
  // the entire SMP exchange. We should add SMP mocks that allows us to propagate a result up to the
  // FIDL layer. For now we assert that pairing has started and remains pending.
  ASSERT_FALSE(pair_result);  // Pairing request is pending
  ASSERT_TRUE(pairing_request_sent);
}

TEST_F(FIDL_HostServerTest, InitiatePairingLeEncrypted) {
  // clang-format off
  const auto kExpected = CreateStaticByteBuffer(
      0x01,  // code: "Pairing Request"
      0x03,  // IO cap.: NoInputNoOutput
      0x00,  // OOB: not present
      0x09,  // AuthReq: bonding, no MITM (not authenticated), Secure Connections
      0x10,  // encr. key size: 16 (default max)
      0x00,  // initiator keys: none
      0x03   // responder keys: enc key and identity info
  );
  // clang-format on

  auto [peer, fake_chan] = ConnectFakePeer();
  ASSERT_TRUE(peer);
  ASSERT_TRUE(fake_chan);
  ASSERT_EQ(bt::gap::Peer::ConnectionState::kConnected, peer->le()->connection_state());

  bool pairing_request_sent = false;
  // This test only checks that PairingState kicks off an LE pairing feature exchange correctly, as
  // the call to Pair is only responsible for starting pairing, not for completing it.
  auto expect_default_bytebuffer = [&pairing_request_sent, kExpected](bt::ByteBufferPtr sent) {
    ASSERT_TRUE(sent);
    ASSERT_EQ(*sent, kExpected);
    pairing_request_sent = true;
  };
  fake_chan->SetSendCallback(expect_default_bytebuffer, dispatcher());

  std::optional<fit::result<void, fsys::Error>> pair_result;
  fctrl::PairingOptions opts;
  opts.set_le_security_level(fctrl::PairingSecurityLevel::ENCRYPTED);
  host_client()->Pair(fbt::PeerId{peer->identifier().value()}, std::move(opts),
                      [&](auto result) { pair_result = std::move(result); });
  RunLoopUntilIdle();

  // TODO(fxb/886): We don't have a good mechanism for driving pairing to completion without faking
  // the entire SMP exchange. We should add SMP mocks that allows us to propagate a result up to the
  // FIDL layer. For now we assert that pairing has started and remains pending.
  ASSERT_FALSE(pair_result);  // Pairing request is pending
  ASSERT_TRUE(pairing_request_sent);
}

TEST_F(FIDL_HostServerTest, InitiatePairingNonBondableLe) {
  // clang-format off
  const auto kExpected = CreateStaticByteBuffer(
      0x01,  // code: "Pairing Request"
      0x03,  // IO cap.: NoInputNoOutput
      0x00,  // OOB: not present
      0x0C,  // AuthReq: no bonding, MITM (authenticated), Secure Connections
      0x10,  // encr. key size: 16 (default max)
      0x00,  // initiator keys: none
      0x00   // responder keys: none
  );
  // clang-format on

  auto [peer, fake_chan] = ConnectFakePeer();
  ASSERT_TRUE(peer);
  ASSERT_TRUE(fake_chan);
  ASSERT_EQ(bt::gap::Peer::ConnectionState::kConnected, peer->le()->connection_state());

  bool pairing_request_sent = false;
  // This test only checks that PairingState kicks off an LE pairing feature exchange correctly, as
  // the call to Pair is only responsible for starting pairing, not for completing it.
  auto expect_default_bytebuffer = [&pairing_request_sent, kExpected](bt::ByteBufferPtr sent) {
    ASSERT_TRUE(sent);
    ASSERT_EQ(*sent, kExpected);
    pairing_request_sent = true;
  };
  fake_chan->SetSendCallback(expect_default_bytebuffer, dispatcher());

  std::optional<fit::result<void, fsys::Error>> pair_result;
  fctrl::PairingOptions opts;
  opts.set_non_bondable(true);
  host_client()->Pair(fbt::PeerId{peer->identifier().value()}, std::move(opts),
                      [&](auto result) { pair_result = std::move(result); });
  RunLoopUntilIdle();

  // TODO(fxb/886): We don't have a good mechanism for driving pairing to completion without faking
  // the entire SMP exchange. We should add SMP mocks that allows us to propagate a result up to the
  // FIDL layer. For now we assert that pairing has started and remains pending.
  ASSERT_FALSE(pair_result);  // Pairing request is pending
  ASSERT_TRUE(pairing_request_sent);
}

TEST_F(FIDL_HostServerTest, InitiateBrEdrPairingLePeerFails) {
  auto [peer, fake_chan] = ConnectFakePeer();
  ASSERT_TRUE(peer);
  ASSERT_TRUE(fake_chan);
  ASSERT_EQ(bt::gap::Peer::ConnectionState::kConnected, peer->le()->connection_state());

  std::optional<fit::result<void, fsys::Error>> pair_result;
  fctrl::PairingOptions opts;
  // Set pairing option with classic
  opts.set_transport(fctrl::TechnologyType::CLASSIC);
  auto pair_cb = [&](auto result) {
    ASSERT_TRUE(result.is_err());
    pair_result = std::move(result);
  };
  host_client()->Pair(fbt::PeerId{peer->identifier().value()}, std::move(opts), std::move(pair_cb));
  RunLoopUntilIdle();
  ASSERT_TRUE(pair_result);
  ASSERT_EQ(pair_result->error(), fsys::Error::PEER_NOT_FOUND);
}

TEST_F(FIDL_HostServerTest, WatchPeersHangsOnFirstCallWithNoExistingPeers) {
  // By default the peer cache contains no entries when HostServer is first constructed. The first
  // call to WatchPeers should hang.
  bool replied = false;
  host_server()->WatchPeers([&](auto, auto) { replied = true; });
  EXPECT_FALSE(replied);
}

TEST_F(FIDL_HostServerTest, WatchPeersRepliesOnFirstCallWithExistingPeers) {
  __UNUSED auto* peer = adapter()->peer_cache()->NewPeer(kLeTestAddr, /*connectable=*/true);
  ResetHostServer();

  // By default the peer cache contains no entries when HostServer is first constructed. The first
  // call to WatchPeers should hang.
  bool replied = false;
  host_server()->WatchPeers([&](auto updated, auto removed) {
    EXPECT_EQ(1u, updated.size());
    EXPECT_TRUE(removed.empty());
    replied = true;
  });
  EXPECT_TRUE(replied);
}

TEST_F(FIDL_HostServerTest, WatchPeersStateMachine) {
  std::optional<std::vector<fsys::Peer>> updated;
  std::optional<std::vector<fbt::PeerId>> removed;

  // Initial watch call hangs as the cache is empty.
  host_server()->WatchPeers([&](auto updated_arg, auto removed_arg) {
    updated = std::move(updated_arg);
    removed = std::move(removed_arg);
  });
  ASSERT_FALSE(updated.has_value());
  ASSERT_FALSE(removed.has_value());

  // Adding a new peer should resolve the hanging get.
  auto* peer = adapter()->peer_cache()->NewPeer(kLeTestAddr, /*connectable=*/true);
  ASSERT_TRUE(updated.has_value());
  ASSERT_TRUE(removed.has_value());
  EXPECT_EQ(1u, updated->size());
  EXPECT_TRUE(fidl::Equals(fidl_helpers::PeerToFidl(*peer), (*updated)[0]));
  EXPECT_TRUE(removed->empty());
  updated.reset();
  removed.reset();

  // The next call should hang.
  host_server()->WatchPeers([&](auto updated_arg, auto removed_arg) {
    updated = std::move(updated_arg);
    removed = std::move(removed_arg);
  });
  ASSERT_FALSE(updated.has_value());
  ASSERT_FALSE(removed.has_value());

  // Removing the peer should resolve the hanging get.
  auto peer_id = peer->identifier();
  __UNUSED auto result = adapter()->peer_cache()->RemoveDisconnectedPeer(peer_id);
  ASSERT_TRUE(updated.has_value());
  ASSERT_TRUE(removed.has_value());
  EXPECT_TRUE(updated->empty());
  EXPECT_EQ(1u, removed->size());
  EXPECT_TRUE(fidl::Equals(fbt::PeerId{peer_id.value()}, (*removed)[0]));
}

TEST_F(FIDL_HostServerTest, WatchPeersUpdatedThenRemoved) {
  // Add then remove a peer. The watcher should only report the removal.
  bt::PeerId id;
  {
    auto* peer = adapter()->peer_cache()->NewPeer(kLeTestAddr, /*connectable=*/true);
    id = peer->identifier();

    // |peer| becomes a dangling pointer after the call to RemoveDisconnectedPeer. We scoped the
    // binding of |peer| so that it doesn't exist beyond this point.
    __UNUSED auto result = adapter()->peer_cache()->RemoveDisconnectedPeer(id);
  }

  bool replied = false;
  host_server()->WatchPeers([&replied, id](auto updated, auto removed) {
    EXPECT_TRUE(updated.empty());
    EXPECT_EQ(1u, removed.size());
    EXPECT_TRUE(fidl::Equals(fbt::PeerId{id.value()}, removed[0]));
    replied = true;
  });
  EXPECT_TRUE(replied);
}

TEST_F(FIDL_HostServerTest, GetInspectVmoCallsCallbackWithAdapterInspectVmo) {
  std::optional<fuchsia::mem::Buffer> vmo_buf;
  auto vmo_cb = [&vmo_buf](fuchsia::mem::Buffer vmo) { vmo_buf = std::move(vmo); };
  host_server()->GetInspectVmo(std::move(vmo_cb));
  ASSERT_TRUE(vmo_buf.has_value());

  auto hierarchy = inspect::ReadFromVmo(vmo_buf->vmo).take_value();
  EXPECT_THAT(hierarchy, AllOf(NodeMatches(NameMatches("root"))));
}

}  // namespace
}  // namespace bthost
