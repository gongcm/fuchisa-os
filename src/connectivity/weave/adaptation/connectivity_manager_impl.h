// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#ifndef SRC_CONNECTIVITY_WEAVE_ADAPTATION_CONNECTIVITY_MANAGER_IMPL_H_
#define SRC_CONNECTIVITY_WEAVE_ADAPTATION_CONNECTIVITY_MANAGER_IMPL_H_

#include <Weave/DeviceLayer/ConnectivityManager.h>
#include <Weave/DeviceLayer/internal/GenericConnectivityManagerImpl.h>
#if WEAVE_DEVICE_CONFIG_ENABLE_WOBLE
#include <Weave/DeviceLayer/internal/GenericConnectivityManagerImpl_BLE.h>
#else
#include <Weave/DeviceLayer/internal/GenericConnectivityManagerImpl_NoBLE.h>
#endif
#include <Weave/DeviceLayer/internal/GenericConnectivityManagerImpl_NoThread.h>
#include <Weave/Profiles/network-provisioning/NetworkProvisioning.h>
#include <Weave/Profiles/weave-tunneling/WeaveTunnelCommon.h>
#include <Weave/Profiles/weave-tunneling/WeaveTunnelConnectionMgr.h>

#include <Weave/Support/FlagUtils.hpp>

namespace nl {
namespace Inet {
// class IPAddress;
}  // namespace Inet
}  // namespace nl

namespace nl {
namespace Weave {
namespace DeviceLayer {

class PlatformManagerImpl;

namespace Internal {

class NetworkProvisioningServerImpl;
template <class ImplClass>
class GenericNetworkProvisioningServerImpl;

}  // namespace Internal

/**
 * Concrete implementation of the ConnectivityManager singleton object for the Fuchsia platform.
 */
class ConnectivityManagerImpl final
    : public ConnectivityManager,
      public Internal::GenericConnectivityManagerImpl<ConnectivityManagerImpl>,
#if WEAVE_DEVICE_CONFIG_ENABLE_WOBLE
      public Internal::GenericConnectivityManagerImpl_BLE<ConnectivityManagerImpl>,
#else
      public Internal::GenericConnectivityManagerImpl_NoBLE<ConnectivityManagerImpl>,
#endif
      public Internal::GenericConnectivityManagerImpl_NoThread<ConnectivityManagerImpl> {
#if WEAVE_CONFIG_ENABLE_TUNNELING
  using TunnelConnNotifyReasons =
      ::nl::Weave::Profiles::WeaveTunnel::WeaveTunnelConnectionMgr::TunnelConnNotifyReasons;
#endif

  // Allow the ConnectivityManager interface class to delegate method calls to
  // the implementation methods provided by this class.
  friend class ConnectivityManager;

 public:
  /**
   * Delegate class to handle platform-specific implementation of the
   * ConnectivityManager API surface. This enables tests to swap out the
   * implementation of the static ConnectivityManager instance.
   */
  class Delegate {
   public:
    virtual ~Delegate() = default;

    // Provides a handle to ConnectivityManagerImpl object that this delegate
    // was attached to. This allows the delegate to invoke functions on
    // GenericConnectivityManagerImpl if required.
    void SetConnectivityManagerImpl(ConnectivityManagerImpl* impl) { impl_ = impl; }

    // ConnectivityManager APIs.

    // Initializes delegate state.
    virtual WEAVE_ERROR Init(void) = 0;
    // Returns whether the service tunnel is currently connected.
    virtual bool IsServiceTunnelConnected(void) = 0;
    // Returns whether the service tunnel is operating in restricted mode.
    virtual bool IsServiceTunnelRestricted(void) = 0;
    // Handles platform events generated by OpenWeave.
    virtual void OnPlatformEvent(const WeaveDeviceEvent* event) = 0;
    // Returns whether the device currently has IPv4 connectivity.
    bool HaveIPv4InternetConnectivity(void);
    // Returns whether the device currently has IPv6 connectivity.
    bool HaveIPv6InternetConnectivity(void);
    // Returns the current service tunnel mode.
    ServiceTunnelMode GetServiceTunnelMode(void);

    enum Flags {
      kFlag_HaveIPv4InternetConnectivity = 0x0001,
      kFlag_HaveIPv6InternetConnectivity = 0x0002,
      kFlag_ServiceTunnelStarted = 0x0004,
      kFlag_ServiceTunnelUp = 0x0008,
      kFlag_AwaitingConnectivity = 0x0010,
    };

   protected:
    ConnectivityManagerImpl* impl_;

    uint16_t flags_;
    ServiceTunnelMode service_tunnel_mode_;
  };

  // Sets the delegate containing the platform-specific implementation. It is
  // invalid to invoke the ConfigurationManager without setting a delegate
  // first. However, the OpenWeave surface requires a no-constructor
  // instantiation of this class, so it is up to the caller to enforce this.
  void SetDelegate(std::unique_ptr<Delegate> delegate);

  // Gets the delegate currently in use. This may return nullptr if no delegate
  // was set on this class.
  Delegate* GetDelegate();

 private:
  // ===== Members that implement the ConnectivityManager abstract interface.

  // WiFi Station Methods
  WiFiStationMode _GetWiFiStationMode(void);
  WEAVE_ERROR _SetWiFiStationMode(WiFiStationMode val);
  bool _IsWiFiStationEnabled(void);
  bool _IsWiFiStationApplicationControlled(void);
  bool _IsWiFiStationConnected(void);
  uint32_t _GetWiFiStationReconnectIntervalMS(void);
  WEAVE_ERROR _SetWiFiStationReconnectIntervalMS(uint32_t val);
  bool _IsWiFiStationProvisioned(void);
  void _ClearWiFiStationProvision(void);
  WEAVE_ERROR _GetAndLogWifiStatsCounters(void);

  // WiFi AP Methods
  WiFiAPMode _GetWiFiAPMode(void);
  WEAVE_ERROR _SetWiFiAPMode(WiFiAPMode val);
  bool _IsWiFiAPActive(void);
  bool _IsWiFiAPApplicationControlled(void);
  void _DemandStartWiFiAP(void);
  void _StopOnDemandWiFiAP(void);
  void _MaintainOnDemandWiFiAP(void);
  uint32_t _GetWiFiAPIdleTimeoutMS(void);
  void _SetWiFiAPIdleTimeoutMS(uint32_t val);

  // Internet Connectivity Methods
  bool _HaveIPv4InternetConnectivity(void);
  bool _HaveIPv6InternetConnectivity(void);

  // Service Tunnel Methods
  ServiceTunnelMode _GetServiceTunnelMode(void);
  WEAVE_ERROR _SetServiceTunnelMode(ServiceTunnelMode val);
  bool _IsServiceTunnelConnected(void);
  bool _IsServiceTunnelRestricted(void);
  bool _HaveServiceConnectivityViaTunnel(void);
  bool _HaveServiceConnectivity(void);

  WEAVE_ERROR _Init(void);
  void _OnPlatformEvent(const WeaveDeviceEvent* event);
  bool _CanStartWiFiScan();
  void _OnWiFiScanDone();
  void _OnWiFiStationProvisionChange();
  static const char* _WiFiStationModeToStr(WiFiStationMode mode);
  static const char* _WiFiAPModeToStr(WiFiAPMode mode);
  static const char* _ServiceTunnelModeToStr(ServiceTunnelMode mode);

  // ===== Members for internal use by the following friends.
  friend ConnectivityManager& ConnectivityMgr(void);
  friend ConnectivityManagerImpl& ConnectivityMgrImpl(void);

  static ConnectivityManagerImpl sInstance;

  // ===== Private members reserved for use by this class only.
  std::unique_ptr<Delegate> delegate_;
};

// TODO(fxbug.dev/59955): These functions temporarily report that the network is
// always enabled and always provisioned. These should be properly implemented
// by reaching out to the WLAN FIDLs.
inline ConnectivityManager::WiFiStationMode ConnectivityManagerImpl::_GetWiFiStationMode(void) {
  return kWiFiStationMode_Enabled;
}
inline bool ConnectivityManagerImpl::_IsWiFiStationEnabled(void) { return true; }
inline WEAVE_ERROR ConnectivityManagerImpl::_SetWiFiStationMode(WiFiStationMode val) {
  return WEAVE_ERROR_UNSUPPORTED_WEAVE_FEATURE;
}
inline bool ConnectivityManagerImpl::_IsWiFiStationProvisioned(void) { return true; }
inline void ConnectivityManagerImpl::_ClearWiFiStationProvision(void) {}
inline WEAVE_ERROR ConnectivityManagerImpl::_GetAndLogWifiStatsCounters(void) {
  return WEAVE_ERROR_UNSUPPORTED_WEAVE_FEATURE;
}
inline bool ConnectivityManagerImpl::_IsWiFiStationConnected(void) { return true; }
inline uint32_t ConnectivityManagerImpl::_GetWiFiStationReconnectIntervalMS(void) {
  return UINT32_MAX;
}
inline bool ConnectivityManagerImpl::_CanStartWiFiScan() { return true; }
inline void ConnectivityManagerImpl::_OnWiFiScanDone() {}
inline void ConnectivityManagerImpl::_OnWiFiStationProvisionChange() {}

// TODO(fxbug.dev/59956): These functions temporarily report that AP mode is
// disabled and unsupported. These should be properly implemented by reaching
// out the the WLAN FIDLs.
inline WEAVE_ERROR ConnectivityManagerImpl::_SetWiFiAPMode(WiFiAPMode val) {
  return WEAVE_ERROR_UNSUPPORTED_WEAVE_FEATURE;
}
inline void ConnectivityManagerImpl::_DemandStartWiFiAP(void) {}
inline void ConnectivityManagerImpl::_StopOnDemandWiFiAP(void) {}
inline void ConnectivityManagerImpl::_MaintainOnDemandWiFiAP(void) {}
inline void ConnectivityManagerImpl::_SetWiFiAPIdleTimeoutMS(uint32_t val) {}
inline bool ConnectivityManagerImpl::_IsWiFiStationApplicationControlled(void) {
  return kWiFiStationMode_NotSupported;
}
inline bool ConnectivityManagerImpl::_IsWiFiAPApplicationControlled(void) { return false; }
inline ConnectivityManager::WiFiAPMode ConnectivityManagerImpl::_GetWiFiAPMode(void) {
  return WiFiAPMode::kWiFiAPMode_NotSupported;
}
inline bool ConnectivityManagerImpl::_IsWiFiAPActive(void) { return false; }
inline uint32_t ConnectivityManagerImpl::_GetWiFiAPIdleTimeoutMS(void) { return UINT32_MAX; }

inline WEAVE_ERROR ConnectivityManagerImpl::_SetServiceTunnelMode(ServiceTunnelMode val) {
  return WEAVE_ERROR_UNSUPPORTED_WEAVE_FEATURE;
}

inline bool ConnectivityManagerImpl::_HaveServiceConnectivity(void) {
  return HaveServiceConnectivityViaTunnel() || HaveServiceConnectivityViaThread();
}

/**
 * Returns the public interface of the ConnectivityManager singleton object.
 *
 * Weave applications should use this to access features of the ConnectivityManager object
 * that are common to all platforms.
 */
inline ConnectivityManager& ConnectivityMgr(void) { return ConnectivityManagerImpl::sInstance; }

/**
 * Returns the platform-specific implementation of the ConnectivityManager singleton object.
 *
 * Weave applications can use this to gain access to features of the ConnectivityManager
 * that are specific to the Fuchsia platform.
 */
inline ConnectivityManagerImpl& ConnectivityMgrImpl(void) {
  return ConnectivityManagerImpl::sInstance;
}

}  // namespace DeviceLayer
}  // namespace Weave
}  // namespace nl

#endif  // SRC_CONNECTIVITY_WEAVE_ADAPTATION_CONNECTIVITY_MANAGER_IMPL_H_
