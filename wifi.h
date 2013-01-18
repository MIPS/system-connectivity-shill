// Copyright (c) 2012 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SHILL_WIFI_
#define SHILL_WIFI_

// A WiFi device represents a wireless network interface implemented as an IEEE
// 802.11 station.  An Access Point (AP) (or, more correctly, a Basic Service
// Set(BSS)) is represented by a WiFiEndpoint.  An AP provides a WiFiService,
// which is the same concept as Extended Service Set (ESS) in 802.11,
// identified by an SSID.  A WiFiService includes zero or more WiFiEndpoints
// that provide that service.
//
// A WiFi device interacts with a real device through WPA Supplicant.
// Wifi::Start() creates a connection to WPA Supplicant, represented by
// |supplicant_interface_proxy_|.  [1]
//
// A WiFi device becomes aware of WiFiEndpoints through BSSAdded signals from
// WPA Supplicant, which identifies them by a "path".  The WiFi object maintains
// an EndpointMap in |endpoint_by_rpcid_|, in which the key is the "path" and
// the value is a pointer to a WiFiEndpoint object.  When a WiFiEndpoint is
// added, it is associated with a WiFiService.
//
// The WiFi device connects to a WiFiService, not a WiFiEndpoint, through WPA
// Supplicant. It is the job of WPA Supplicant to select a BSS (aka
// WiFiEndpoint) to connect to.  The protocol for establishing a connection is
// as follows:
//
//  1.  The WiFi device sends AddNetwork to WPA Supplicant, which returns a
//  "network path" when done.
//
//  2.  The WiFi device sends SelectNetwork, indicating the network path
//  received in 1, to WPA Supplicant, which begins the process of associating
//  with an AP in the ESS.  At this point the WiFiService which is being
//  connected is called the |pending_service_|.
//
//  3.  During association to an EAP-TLS network, WPA Supplicant can send
//  multiple "Certification" events, which provide information about the
//  identity of the remote entity.
//
//  4.  When association is complete, WPA Supplicant sends a PropertiesChanged
//  signal to the WiFi device, indicating a change in the CurrentBSS.  The
//  WiFiService indicated by the new value of CurrentBSS is set as the
//  |current_service_|, and |pending_service_| is (normally) cleared.
//
// Some key things to notice are 1) WPA Supplicant does the work of selecting
// the AP (aka WiFiEndpoint) and it tells the WiFi device which AP it selected.
// 2) The process of connecting is asynchronous.  There is a |current_service_|
// to which the WiFi device is presently using and a |pending_service_| to which
// the WiFi device has initiated a connection.
//
// A WiFi device is notified that an AP has gone away via the BSSRemoved signal.
// When the last WiFiEndpoint of a WiFiService is removed, the WiFiService
// itself is deleted.
//
// TODO(gmorain): Add explanation of hidden SSIDs.
//
// WPA Supplicant's PropertiesChanged signal communicates changes in the state
// of WPA Supplicant's current service.  This state is stored in
// |supplicant_state_| and reflects WPA Supplicant's view of the state of the
// connection to an AP.  Changes in this state sometimes cause state changes in
// the WiFiService to which a WiFi device is connected.  For example, when WPA
// Supplicant signals the new state to be "completed", then the WiFiService
// state gets changed to "configuring".  State change notifications are not
// reliable because WPA Supplicant may coalesce state changes in quick
// succession so that only the last of the changes is signaled.
//
// Notes:
//
// 1.  Shill's definition of the interface is described in
// shill/dbus_bindings/supplicant-interface.xml, and the WPA Supplicant's
// description of the same interface is in
// third_party/wpa_supplicant/doc/dbus.doxygen.

#include <time.h>

#include <map>
#include <string>
#include <vector>

#include <base/callback_forward.h>
#include <base/cancelable_callback.h>
#include <base/memory/weak_ptr.h>
#include <dbus-c++/dbus.h>
#include <gtest/gtest_prod.h>  // for FRIEND_TEST

#include "shill/dbus_manager.h"
#include "shill/device.h"
#include "shill/event_dispatcher.h"
#include "shill/power_manager.h"
#include "shill/refptr_types.h"
#include "shill/service.h"

namespace shill {

class Error;
class GeolocationInfo;
class KeyValueStore;
class ProxyFactory;
class SupplicantInterfaceProxyInterface;
class SupplicantProcessProxyInterface;
class WiFiProvider;
class WiFiService;

// WiFi class. Specialization of Device for WiFi.
class WiFi : public Device {
 public:
  WiFi(ControlInterface *control_interface,
       EventDispatcher *dispatcher,
       Metrics *metrics,
       Manager *manager,
       const std::string &link,
       const std::string &address,
       int interface_index);
  virtual ~WiFi();

  virtual void Start(Error *error, const EnabledStateChangedCallback &callback);
  virtual void Stop(Error *error, const EnabledStateChangedCallback &callback);
  virtual void Scan(Error *error);
  // Callback for system resume. If this WiFi device is idle, a scan
  // is initiated. Additionally, the base class implementation is
  // invoked unconditionally.
  virtual void OnAfterResume();
  // Callback for when a service is configured with an IP.
  virtual void OnConnected();

  // Called by SupplicantInterfaceProxy, in response to events from
  // wpa_supplicant.
  void BSSAdded(const ::DBus::Path &BSS,
                const std::map<std::string, ::DBus::Variant> &properties);
  void BSSRemoved(const ::DBus::Path &BSS);
  void Certification(const std::map<std::string, ::DBus::Variant> &properties);
  void EAPEvent(const std::string &status, const std::string &parameter);
  void PropertiesChanged(
      const std::map<std::string, ::DBus::Variant> &properties);
  void ScanDone();

  // Called by WiFiService.
  virtual void ConnectTo(
      WiFiService *service,
      std::map<std::string, ::DBus::Variant> service_params);
  // If |service| is connected, initiate the process of disconnecting it.
  // Otherwise, if it a pending or current service, discontinue the process
  // of connecting and return |service| to the idle state.
  virtual void DisconnectFrom(WiFiService *service);
  virtual bool IsIdle() const;
  // Clear any cached credentials wpa_supplicant may be holding for
  // |service|.  This has a side-effect of disconnecting the service
  // if it is connected.
  virtual void ClearCachedCredentials(const WiFiService *service);

  // Called by WiFiEndpoint.
  virtual void NotifyEndpointChanged(const WiFiEndpointConstRefPtr &endpoint);

  // Utility, used by WiFiService and WiFiEndpoint.
  // Replace non-ASCII characters with '?'. Return true if one or more
  // characters were changed.
  static bool SanitizeSSID(std::string *ssid);

  // Formats |ssid| for logging purposes, to ease scrubbing.
  static std::string LogSSID(const std::string &ssid);

  // Called by Linkmonitor (overriden from Device superclass).
  virtual void OnLinkMonitorFailure();

  bool IsCurrentService(const WiFiServiceRefPtr service) {
    return service.get() == current_service_.get();
  }

  // Overridden from Device superclass
  virtual std::vector<GeolocationInfo> GetGeolocationObjects() const;

  // Overridden from Device superclass
  virtual bool ShouldUseArpGateway() const;

  // Called by a WiFiService when it disassociates itself from this Device.
  virtual void DisassociateFromService(const WiFiServiceRefPtr &service);

 private:
  friend class WiFiObjectTest;  // access to supplicant_*_proxy_, link_up_
  friend class WiFiTimerTest;  // kNumFastScanAttempts, kFastScanIntervalSeconds
  FRIEND_TEST(WiFiMainTest, AppendBgscan);
  FRIEND_TEST(WiFiMainTest, DisconnectCurrentServiceWithErrors);
  FRIEND_TEST(WiFiMainTest, FlushBSSOnResume);  // kMaxBSSResumeAgeSeconds
  FRIEND_TEST(WiFiMainTest, InitialSupplicantState);  // kInterfaceStateUnknown
  FRIEND_TEST(WiFiMainTest, LinkMonitorFailure);  // set_link_monitor()
  FRIEND_TEST(WiFiMainTest, ScanResults);             // EndpointMap
  FRIEND_TEST(WiFiMainTest, ScanResultsWithUpdates);  // EndpointMap
  FRIEND_TEST(WiFiMainTest, Stop);  // weak_ptr_factory_
  FRIEND_TEST(WiFiMainTest, VerifyPaths);
  FRIEND_TEST(WiFiPropertyTest, BgscanMethodProperty);  // bgscan_method_
  FRIEND_TEST(WiFiTimerTest, FastRescan);  // kFastScanIntervalSeconds

  typedef std::map<const std::string, WiFiEndpointRefPtr> EndpointMap;
  typedef std::map<const WiFiService *, std::string> ReverseServiceMap;

  static const char kSupplicantConfPath[];
  static const char *kDefaultBgscanMethod;
  static const uint16 kDefaultBgscanShortIntervalSeconds;
  static const int32 kDefaultBgscanSignalThresholdDbm;
  static const uint16 kDefaultScanIntervalSeconds;
  static const uint16 kBackgroundScanIntervalSeconds;
  static const time_t kMaxBSSResumeAgeSeconds;
  static const char kInterfaceStateUnknown[];
  // Delay between scans when supplicant finds "No suitable network".
  static const time_t kRescanIntervalSeconds;
  // Number of times to quickly attempt a scan after startup / disconnect.
  static const int kNumFastScanAttempts;
  static const int kFastScanIntervalSeconds;
  static const int kPendingTimeoutSeconds;
  static const int kReconnectTimeoutSeconds;

  void AppendBgscan(WiFiService *service,
                    std::map<std::string, DBus::Variant> *service_params) const;
  std::string GetBgscanMethod(const int &argument, Error *error);
  uint16 GetBgscanShortInterval(Error */* error */) {
    return bgscan_short_interval_seconds_;
  }
  int32 GetBgscanSignalThreshold(Error */* error */) {
    return bgscan_signal_threshold_dbm_;
  }
  uint16 GetScanInterval(Error */* error */) { return scan_interval_seconds_; }
  void SetBgscanMethod(
      const int &argument, const std::string &method, Error *error);
  void SetBgscanShortInterval(const uint16 &seconds, Error *error);
  void SetBgscanSignalThreshold(const int32 &dbm, Error *error);
  void SetScanInterval(const uint16 &seconds, Error *error);
  void ClearBgscanMethod(const int &argument, Error *error);

  void CurrentBSSChanged(const ::DBus::Path &new_bss);
  // Return the RPC identifier associated with the wpa_supplicant network
  // entry created for |service|.  If one does not exist, an empty string
  // is returned, and |error| is populated.
  std::string FindNetworkRpcidForService(const WiFiService *service,
                                         Error *error);
  void HandleDisconnect();
  void HandleRoam(const ::DBus::Path &new_bssid);
  void BSSAddedTask(const ::DBus::Path &BSS,
                    const std::map<std::string, ::DBus::Variant> &properties);
  void BSSRemovedTask(const ::DBus::Path &BSS);
  void CertificationTask(
      const std::map<std::string, ::DBus::Variant> &properties);
  void EAPEventTask(const std::string &status, const std::string &parameter);
  void PropertiesChangedTask(
      const std::map<std::string, ::DBus::Variant> &properties);
  void ScanDoneTask();
  void ScanTask();
  void SetScanPending(bool pending);
  void StateChanged(const std::string &new_state);
  // Heuristic check if a connection failure was due to bad credentials.
  // Returns true and puts type of failure in |failure| if a credential
  // problem is detected.
  bool SuspectCredentials(const WiFiService &service,
                          Service::ConnectFailure *failure) const;
  void HelpRegisterDerivedInt32(
      PropertyStore *store,
      const std::string &name,
      int32(WiFi::*get)(Error *error),
      void(WiFi::*set)(const int32 &value, Error *error));
  void HelpRegisterDerivedUint16(
      PropertyStore *store,
      const std::string &name,
      uint16(WiFi::*get)(Error *error),
      void(WiFi::*set)(const uint16 &value, Error *error));

  // Disable a network entry in wpa_supplicant, and catch any exception
  // that occurs.  Returns false if an exception occurred, true otherwise.
  bool DisableNetwork(const ::DBus::Path &network);
  // Disable the wpa_supplicant network entry associated with |service|.
  // Any cached credentials stored in wpa_supplicant related to this
  // network entry will be preserved.  This will have the side-effect of
  // disconnecting this service if it is currently connected.  Returns
  // true if successful, otherwise returns false and populates |error|
  // with the reason for failure.
  virtual bool DisableNetworkForService(
      const WiFiService *service, Error *error);
  // Remove a network entry from wpa_supplicant, and catch any exception
  // that occurs.  Returns false if an exception occurred, true otherwise.
  bool RemoveNetwork(const ::DBus::Path &network);
  // Remove the wpa_supplicant network entry associated with |service|.
  // Any cached credentials stored in wpa_supplicant related to this
  // network entry will be removed.  This will have the side-effect of
  // disconnecting this service if it is currently connected.  Returns
  // true if successful, otherwise returns false and populates |error|
  // with the reason for failure.
  virtual bool RemoveNetworkForService(
      const WiFiService *service, Error *error);
  // Restart fast scanning after disconnection.
  void RestartFastScanAttempts();
  // Schedules a scan attempt at time |scan_interval_seconds_| in the
  // future.  Cancels any currently pending scan timer.
  void StartScanTimer();
  // Cancels any currently pending scan timer.
  void StopScanTimer();
  // Initiates a scan, if idle. Reschedules the scan timer regardless.
  void ScanTimerHandler();
  // Starts a timer in order to limit the length of an attempt to
  // connect to a pending network.
  void StartPendingTimer();
  // Cancels any currently pending network timer.
  void StopPendingTimer();
  // Aborts a pending network that is taking too long to connect.
  void PendingTimeoutHandler();
  // Starts a timer in order to limit the length of an attempt to
  // reconnect to the current network.
  void StartReconnectTimer();
  // Stops any pending reconnect timer.
  void StopReconnectTimer();
  // Disconnects from the current service that is taking too long
  // to reconnect on its own.
  void ReconnectTimeoutHandler();
  // Sets the current pending service.  If the argument is non-NULL,
  // the Pending timer is started and the associated service is set
  // to "Associating", otherwise it is stopped.
  void SetPendingService(const WiFiServiceRefPtr &service);

  void OnSupplicantAppear(const std::string &owner);
  void OnSupplicantVanish();
  // Called by ScopeLogger when WiFi debug scope is enabled/disabled.
  void OnWiFiDebugScopeChanged(bool enabled);
  // Enable or disable debugging for the current connection attempt.
  void SetConnectionDebugging(bool enabled);
  // Enable high bitrates for the current network.  High rates are disabled
  // on the initial association and every reassociation afterward.
  void EnableHighBitrates();

  void ConnectToSupplicant();

  void Restart();

  // Pointer to the provider object that maintains WiFiService objects.
  WiFiProvider *provider_;

  base::WeakPtrFactory<WiFi> weak_ptr_factory_;

  // Store cached copies of singletons for speed/ease of testing.
  ProxyFactory *proxy_factory_;
  Time *time_;

  DBusManager::CancelableAppearedCallback on_supplicant_appear_;
  DBusManager::CancelableVanishedCallback on_supplicant_vanish_;
  bool supplicant_present_;

  scoped_ptr<SupplicantProcessProxyInterface> supplicant_process_proxy_;
  scoped_ptr<SupplicantInterfaceProxyInterface> supplicant_interface_proxy_;
  // The rpcid used as the key is wpa_supplicant's D-Bus path for the
  // Endpoint (BSS, in supplicant parlance).
  EndpointMap endpoint_by_rpcid_;
  // Map from Services to the D-Bus path for the corresponding wpa_supplicant
  // Network.
  ReverseServiceMap rpcid_by_service_;
  // The Service we are presently connected to. May be NULL is we're not
  // not connected to any Service.
  WiFiServiceRefPtr current_service_;
  // The Service we're attempting to connect to. May be NULL if we're
  // not attempting to connect to a new Service. If non-NULL, should
  // be distinct from |current_service_|. (A service should not
  // simultaneously be both pending, and current.)
  WiFiServiceRefPtr pending_service_;
  std::string supplicant_state_;
  std::string supplicant_bss_;
  std::string supplicant_tls_error_;
  // Indicates that we should flush supplicant's BSS cache after the
  // next scan completes.
  bool need_bss_flush_;
  struct timeval resumed_at_;
  // Executes when the (foreground) scan timer expires. Calls ScanTimerHandler.
  base::CancelableClosure scan_timer_callback_;
  // Executes when a pending service connect timer expires. Calls
  // PendingTimeoutHandler.
  base::CancelableClosure pending_timeout_callback_;
  // Executes when a reconnecting service timer expires. Calls
  // ReconnectTimeoutHandler.
  base::CancelableClosure reconnect_timeout_callback_;
  // Number of remaining fast scans to be done during startup and disconnect.
  int fast_scans_remaining_;
  // Indicates that the current BSS has reached the completed state according
  // to supplicant.
  bool has_already_completed_;
  // Indicates that we are debugging a problematic connection.
  bool is_debugging_connection_;
  // Indicates that we are in the middle of EAP authentication.
  bool is_eap_in_progress_;

  // Properties
  std::string bgscan_method_;
  uint16 bgscan_short_interval_seconds_;
  int32 bgscan_signal_threshold_dbm_;
  bool scan_pending_;
  uint16 scan_interval_seconds_;

  DISALLOW_COPY_AND_ASSIGN(WiFi);
};

}  // namespace shill

#endif  // SHILL_WIFI_
