// Copyright (c) 2012 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "shill/wifi.h"

#include <netinet/ether.h>
#include <linux/if.h>
#include <sys/socket.h>
#include <linux/netlink.h>  // Needs typedefs from sys/socket.h.

#include <map>
#include <set>
#include <string>
#include <vector>

#include <base/memory/ref_counted.h>
#include <base/memory/scoped_ptr.h>
#include <base/stringprintf.h>
#include <base/string_number_conversions.h>
#include <base/string_util.h>
#include <chromeos/dbus/service_constants.h>
#include <dbus-c++/dbus.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "shill/dbus_adaptor.h"
#include "shill/event_dispatcher.h"
#include "shill/ieee80211.h"
#include "shill/key_value_store.h"
#include "shill/manager.h"
#include "shill/mock_device.h"
#include "shill/mock_dhcp_config.h"
#include "shill/mock_dhcp_provider.h"
#include "shill/mock_manager.h"
#include "shill/mock_metrics.h"
#include "shill/mock_rtnl_handler.h"
#include "shill/mock_store.h"
#include "shill/mock_supplicant_interface_proxy.h"
#include "shill/mock_supplicant_process_proxy.h"
#include "shill/mock_wifi_service.h"
#include "shill/nice_mock_control.h"
#include "shill/property_store_unittest.h"
#include "shill/proxy_factory.h"
#include "shill/wifi_endpoint.h"
#include "shill/wifi.h"
#include "shill/wifi_service.h"
#include "shill/wpa_supplicant.h"

using std::map;
using std::set;
using std::string;
using std::vector;
using ::testing::_;
using ::testing::AnyNumber;
using ::testing::DefaultValue;
using ::testing::DoAll;
using ::testing::InSequence;
using ::testing::Mock;
using ::testing::NiceMock;
using ::testing::Return;
using ::testing::SetArgumentPointee;
using ::testing::StrEq;
using ::testing::StrictMock;
using ::testing::Test;
using ::testing::Throw;
using ::testing::Values;

namespace shill {

class WiFiPropertyTest : public PropertyStoreTest {
 public:
  WiFiPropertyTest()
      : device_(new WiFi(control_interface(),
                         NULL, NULL, NULL, "wifi", "", 0)) {
  }
  virtual ~WiFiPropertyTest() {}

 protected:
  DeviceRefPtr device_;
};

TEST_F(WiFiPropertyTest, Contains) {
  EXPECT_TRUE(device_->store().Contains(flimflam::kNameProperty));
  EXPECT_FALSE(device_->store().Contains(""));
}

TEST_F(WiFiPropertyTest, Dispatch) {
  {
    ::DBus::Error error;
    EXPECT_TRUE(DBusAdaptor::DispatchOnType(
        device_->mutable_store(),
        flimflam::kBgscanSignalThresholdProperty,
        PropertyStoreTest::kInt32V,
        &error));
  }
  {
    ::DBus::Error error;
    EXPECT_TRUE(DBusAdaptor::DispatchOnType(device_->mutable_store(),
                                            flimflam::kScanIntervalProperty,
                                            PropertyStoreTest::kUint16V,
                                            &error));
  }
  // Ensure that an attempt to write a R/O property returns InvalidArgs error.
  {
    ::DBus::Error error;
    EXPECT_FALSE(DBusAdaptor::DispatchOnType(device_->mutable_store(),
                                             flimflam::kScanningProperty,
                                             PropertyStoreTest::kBoolV,
                                             &error));
    EXPECT_EQ(invalid_args(), error.name());
  }
}

TEST_F(WiFiPropertyTest, BgscanMethod) {
  {
    ::DBus::Error error;
    EXPECT_TRUE(DBusAdaptor::DispatchOnType(
        device_->mutable_store(),
        flimflam::kBgscanMethodProperty,
        DBusAdaptor::StringToVariant(
            wpa_supplicant::kNetworkBgscanMethodSimple),
        &error));
  }

  {
    ::DBus::Error error;
    EXPECT_FALSE(DBusAdaptor::DispatchOnType(
        device_->mutable_store(),
        flimflam::kBgscanMethodProperty,
        DBusAdaptor::StringToVariant("not a real scan method"),
        &error));
  }
}

class WiFiMainTest : public ::testing::TestWithParam<string> {
 public:
  WiFiMainTest()
      : manager_(&control_interface_, NULL, &metrics_, &glib_),
        wifi_(new WiFi(&control_interface_,
                       &dispatcher_,
                       &metrics_,
                       &manager_,
                       kDeviceName,
                       kDeviceAddress,
                       0)),
        supplicant_process_proxy_(new NiceMock<MockSupplicantProcessProxy>()),
        supplicant_interface_proxy_(
            new NiceMock<MockSupplicantInterfaceProxy>(wifi_)),
        dhcp_config_(new MockDHCPConfig(&control_interface_,
                                        &dispatcher_,
                                        &dhcp_provider_,
                                        kDeviceName,
                                        kHostName,
                                        &glib_)),
        proxy_factory_(this) {
    ::testing::DefaultValue< ::DBus::Path>::Set("/default/path");
    // Except for WiFiServices created via WiFi::GetService, we expect
    // that any WiFiService has been registered with the Manager. So
    // default Manager.HasService to true, to make the common case
    // easy.
    ON_CALL(manager_, HasService(_)).
        WillByDefault(Return(true));
  }

  virtual void SetUp() {
    wifi_->proxy_factory_ = &proxy_factory_;
    static_cast<Device *>(wifi_)->rtnl_handler_ = &rtnl_handler_;
    wifi_->set_dhcp_provider(&dhcp_provider_);
    EXPECT_CALL(manager_, DeregisterService(_)).Times(AnyNumber());
  }

  virtual void TearDown() {
    wifi_->proxy_factory_ = NULL;
    // must Stop WiFi instance, to clear its list of services.
    // otherwise, the WiFi instance will not be deleted. (because
    // services reference a WiFi instance, creating a cycle.)
    wifi_->Stop();
    wifi_->set_dhcp_provider(NULL);
  }

 protected:
  typedef scoped_refptr<MockWiFiService> MockWiFiServiceRefPtr;

  class TestProxyFactory : public ProxyFactory {
   public:
    explicit TestProxyFactory(WiFiMainTest *test) : test_(test) {}

    virtual SupplicantProcessProxyInterface *CreateSupplicantProcessProxy(
        const char */*dbus_path*/, const char */*dbus_addr*/) {
      return test_->supplicant_process_proxy_.release();
    }

    virtual SupplicantInterfaceProxyInterface *CreateSupplicantInterfaceProxy(
        const WiFiRefPtr &/*wifi*/,
        const DBus::Path &/*object_path*/,
        const char */*dbus_addr*/) {
      return test_->supplicant_interface_proxy_.release();
    }

   private:
    WiFiMainTest *test_;
  };

  WiFiServiceRefPtr CreateServiceForEndpoint(const WiFiEndpoint &endpoint) {
    bool hidden_ssid = false;
    return wifi_->CreateServiceForEndpoint(endpoint, hidden_ssid);
  }
  const WiFiServiceRefPtr &GetCurrentService() {
    return wifi_->current_service_;
  }
  const WiFi::EndpointMap &GetEndpointMap() {
    return wifi_->endpoint_by_rpcid_;
  }
  const WiFiServiceRefPtr &GetPendingService() {
    return wifi_->pending_service_;
  }
  const vector<WiFiServiceRefPtr> &GetServices() {
    return wifi_->services_;
  }
  // note: the tests need the proxies referenced by WiFi (not the
  // proxies instantiated by WiFiMainTest), to ensure that WiFi
  // sets up its proxies correctly.
  SupplicantProcessProxyInterface *GetSupplicantProcessProxy() {
    return wifi_->supplicant_process_proxy_.get();
  }
  MockSupplicantInterfaceProxy *GetSupplicantInterfaceProxy() {
    return dynamic_cast<MockSupplicantInterfaceProxy *>(
        wifi_->supplicant_interface_proxy_.get());
  }
  const string &GetSupplicantState() {
    return wifi_->supplicant_state_;
  }
  void InitiateConnect(WiFiServiceRefPtr service) {
    map<string, ::DBus::Variant> params;
    wifi_->ConnectTo(service, params);
  }
  void InitiateDisconnect(WiFiServiceRefPtr service) {
    wifi_->DisconnectFrom(service);
  }
  bool IsLinkUp() {
    return wifi_->link_up_;
  }
  WiFiEndpointRefPtr MakeEndpoint(const string &ssid, const string &bssid) {
    return WiFiEndpoint::MakeOpenEndpoint(ssid, bssid);
  }
  MockWiFiServiceRefPtr MakeMockService() {
    vector<uint8_t> ssid(1, 'a');
    return new MockWiFiService(
        &control_interface_,
        &dispatcher_,
        &metrics_,
        &manager_,
        wifi_,
        ssid,
        flimflam::kModeManaged,
        flimflam::kSecurityNone,
        false);
  }
  void RemoveBSS(const ::DBus::Path &bss_path);
  void ReportBSS(const ::DBus::Path &bss_path,
                 const string &ssid,
                 const string &bssid,
                 int16_t signal_strength,
                 const char *mode);
  void ReportLinkUp() {
    wifi_->LinkEvent(IFF_LOWER_UP, IFF_LOWER_UP);
  }
  void ReportScanDone() {
    wifi_->ScanDoneTask();
  }
  void ReportCurrentBSSChanged(const string &new_bss) {
    wifi_->CurrentBSSChanged(new_bss);
  }
  void ReportStateChanged(const string &new_state) {
    wifi_->StateChanged(new_state);
  }
  void StartWiFi() {
    wifi_->Start();
  }
  void StopWiFi() {
    wifi_->Stop();
  }
 void GetOpenService(const char *service_type,
                      const char *ssid,
                      const char *mode,
                      Error *result) {
    GetServiceInner(service_type, ssid, mode, NULL, NULL, false, result);
  }
  void GetService(const char *service_type,
                  const char *ssid,
                  const char *mode,
                  const char *security,
                  const char *passphrase,
                  Error *result) {
    GetServiceInner(service_type, ssid, mode, security, passphrase, false,
                    result);
  }
  WiFiServiceRefPtr GetServiceInner(const char *service_type,
                                    const char *ssid,
                                    const char *mode,
                                    const char *security,
                                    const char *passphrase,
                                    bool allow_hidden,
                                    Error *result) {
    map<string, ::DBus::Variant> args;
    // in general, we want to avoid D-Bus specific code for any RPCs
    // that come in via adaptors. we make an exception here, because
    // calls to GetWifiService are rerouted from the Manager object to
    // the Wifi class.
    if (service_type != NULL)
      args[flimflam::kTypeProperty].writer().append_string(service_type);
    if (ssid != NULL)
      args[flimflam::kSSIDProperty].writer().append_string(ssid);
    if (mode != NULL)
      args[flimflam::kModeProperty].writer().append_string(mode);
    if (security != NULL)
      args[flimflam::kSecurityProperty].writer().append_string(security);
    if (passphrase != NULL)
      args[flimflam::kPassphraseProperty].writer().append_string(passphrase);
    if (!allow_hidden)
      args[flimflam::kWifiHiddenSsid].writer().append_bool(false);

    Error e;
    KeyValueStore args_kv;
    DBusAdaptor::ArgsToKeyValueStore(args, &args_kv, &e);
    return wifi_->GetService(args_kv, result);
  }
  WiFiServiceRefPtr FindService(const vector<uint8_t> &ssid,
                                const string &mode,
                                const string &security) {
    return wifi_->FindService(ssid, mode, security);
  }
  bool LoadHiddenServices(StoreInterface *storage) {
    return wifi_->LoadHiddenServices(storage);
  }
  void SetupHiddenStorage(MockStore *storage, const string &ssid, string *id) {
    const string hex_ssid = base::HexEncode(ssid.data(), ssid.size());
    *id = StringToLowerASCII(base::StringPrintf("%s_%s_%s_%s_%s",
                                                flimflam::kTypeWifi,
                                                kDeviceAddress,
                                                hex_ssid.c_str(),
                                                flimflam::kModeManaged,
                                                flimflam::kSecurityNone));
    const char *groups[] = { id->c_str() };
    EXPECT_CALL(*storage, GetGroupsWithKey(flimflam::kWifiHiddenSsid))
        .WillRepeatedly(Return(set<string>(groups, groups + 1)));
    EXPECT_CALL(*storage, GetBool(StrEq(*id), flimflam::kWifiHiddenSsid, _))
        .WillRepeatedly(DoAll(SetArgumentPointee<2>(true), Return(true)));
    EXPECT_CALL(*storage, GetString(StrEq(*id), flimflam::kSSIDProperty, _))
        .WillRepeatedly(DoAll(SetArgumentPointee<2>(hex_ssid), Return(true)));
  }
  MockManager *manager() {
    return &manager_;
  }
  const WiFiConstRefPtr wifi() const {
    return wifi_;
  }

  EventDispatcher dispatcher_;
  NiceMock<MockRTNLHandler> rtnl_handler_;

 private:
  NiceMockControl control_interface_;
  MockMetrics metrics_;
  MockGLib glib_;
  MockManager manager_;
  WiFiRefPtr wifi_;

  // protected fields interspersed between private fields, due to
  // initialization order
 protected:
  static const char kDeviceName[];
  static const char kDeviceAddress[];
  static const char kHostName[];
  static const char kNetworkModeAdHoc[];
  static const char kNetworkModeInfrastructure[];

  scoped_ptr<MockSupplicantProcessProxy> supplicant_process_proxy_;
  scoped_ptr<MockSupplicantInterfaceProxy> supplicant_interface_proxy_;
  MockDHCPProvider dhcp_provider_;
  scoped_refptr<MockDHCPConfig> dhcp_config_;

 private:
  TestProxyFactory proxy_factory_;
};

const char WiFiMainTest::kDeviceName[] = "wlan0";
const char WiFiMainTest::kDeviceAddress[] = "000102030405";
const char WiFiMainTest::kHostName[] = "hostname";
const char WiFiMainTest::kNetworkModeAdHoc[] = "ad-hoc";
const char WiFiMainTest::kNetworkModeInfrastructure[] = "infrastructure";

void WiFiMainTest::RemoveBSS(const ::DBus::Path &bss_path) {
  wifi_->BSSRemovedTask(bss_path);
}

void WiFiMainTest::ReportBSS(const ::DBus::Path &bss_path,
                             const string &ssid,
                             const string &bssid,
                             int16_t signal_strength,
                             const char *mode) {
  map<string, ::DBus::Variant> bss_properties;

  {
    DBus::MessageIter writer(bss_properties["SSID"].writer());
    writer << vector<uint8_t>(ssid.begin(), ssid.end());
  }
  {
    string bssid_nosep;
    vector<uint8_t> bssid_bytes;
    RemoveChars(bssid, ":", &bssid_nosep);
    base::HexStringToBytes(bssid_nosep, &bssid_bytes);

    DBus::MessageIter writer(bss_properties["BSSID"].writer());
    writer << bssid_bytes;
  }
  bss_properties["Signal"].writer().append_int16(signal_strength);
  bss_properties["Mode"].writer().append_string(mode);
  wifi_->BSSAddedTask(bss_path, bss_properties);
}

TEST_F(WiFiMainTest, ProxiesSetUpDuringStart) {
  EXPECT_TRUE(GetSupplicantProcessProxy() == NULL);
  EXPECT_TRUE(GetSupplicantInterfaceProxy() == NULL);

  StartWiFi();
  EXPECT_FALSE(GetSupplicantProcessProxy() == NULL);
  EXPECT_FALSE(GetSupplicantInterfaceProxy() == NULL);
}

TEST_F(WiFiMainTest, CleanStart) {
  EXPECT_CALL(*supplicant_process_proxy_, CreateInterface(_));
  EXPECT_CALL(*supplicant_process_proxy_, GetInterface(_))
      .Times(AnyNumber())
      .WillRepeatedly(Throw(
          DBus::Error(
              "fi.w1.wpa_supplicant1.InterfaceUnknown",
              "test threw fi.w1.wpa_supplicant1.InterfaceUnknown")));
  EXPECT_CALL(*supplicant_interface_proxy_, Scan(_));
  StartWiFi();
  dispatcher_.DispatchPendingEvents();
}

TEST_F(WiFiMainTest, Restart) {
  EXPECT_CALL(*supplicant_process_proxy_, CreateInterface(_))
      .Times(AnyNumber())
      .WillRepeatedly(Throw(
          DBus::Error(
              "fi.w1.wpa_supplicant1.InterfaceExists",
              "test thew fi.w1.wpa_supplicant1.InterfaceExists")));
  EXPECT_CALL(*supplicant_process_proxy_, GetInterface(_));
  EXPECT_CALL(*supplicant_interface_proxy_, Scan(_));
  StartWiFi();
  dispatcher_.DispatchPendingEvents();
}

TEST_F(WiFiMainTest, StartClearsState) {
  EXPECT_CALL(*supplicant_interface_proxy_, RemoveAllNetworks());
  EXPECT_CALL(*supplicant_interface_proxy_, FlushBSS(_));
  StartWiFi();
}

TEST_F(WiFiMainTest, ScanResults) {
  StartWiFi();
  ReportBSS("bss0", "ssid0", "00:00:00:00:00:00", 0, kNetworkModeAdHoc);
  ReportBSS(
      "bss1", "ssid1", "00:00:00:00:00:01", 1, kNetworkModeInfrastructure);
  ReportBSS(
      "bss2", "ssid2", "00:00:00:00:00:02", 2, kNetworkModeInfrastructure);
  ReportBSS(
      "bss3", "ssid3", "00:00:00:00:00:03", 3, kNetworkModeInfrastructure);
  ReportBSS("bss4", "ssid4", "00:00:00:00:00:04", 4, kNetworkModeAdHoc);
  EXPECT_EQ(5, GetEndpointMap().size());
}

TEST_F(WiFiMainTest, ScanResultsWithUpdates) {
  StartWiFi();
  ReportBSS("bss0", "ssid0", "00:00:00:00:00:00", 0, kNetworkModeAdHoc);
  ReportBSS(
      "bss1", "ssid1", "00:00:00:00:00:01", 1, kNetworkModeInfrastructure);
  ReportBSS(
      "bss2", "ssid2", "00:00:00:00:00:02", 2, kNetworkModeInfrastructure);
  ReportBSS(
      "bss1", "ssid1", "00:00:00:00:00:01", 3, kNetworkModeInfrastructure);
  ReportBSS("bss0", "ssid0", "00:00:00:00:00:00", 4, kNetworkModeAdHoc);

  const WiFi::EndpointMap &endpoints_by_rpcid = GetEndpointMap();
  EXPECT_EQ(3, endpoints_by_rpcid.size());

  WiFi::EndpointMap::const_iterator i;
  WiFiEndpointRefPtr endpoint;
  for (i = endpoints_by_rpcid.begin();
       i != endpoints_by_rpcid.end();
       ++i) {
    if (i->second->bssid_string() == "00:00:00:00:00:00")
      break;
  }
  ASSERT_TRUE(i != endpoints_by_rpcid.end());
  EXPECT_EQ(4, i->second->signal_strength());
}

TEST_F(WiFiMainTest, ScanCompleted) {
  StartWiFi();
  EXPECT_CALL(*manager(), RegisterService(_))
      .Times(3);
  ReportBSS("bss0", "ssid0", "00:00:00:00:00:00", 0, kNetworkModeAdHoc);
  ReportBSS(
      "bss1", "ssid1", "00:00:00:00:00:01", 1, kNetworkModeInfrastructure);
  ReportBSS(
      "bss2", "ssid2", "00:00:00:00:00:02", 2, kNetworkModeInfrastructure);
  ReportScanDone();
  EXPECT_EQ(3, GetServices().size());
}

TEST_F(WiFiMainTest, EndpointGroupingTogether) {
  StartWiFi();

  InSequence s;
  EXPECT_CALL(*manager(), RegisterService(_));
  EXPECT_CALL(*manager(), UpdateService(_));
  ReportBSS("bss0", "ssid", "00:00:00:00:00:00", 0, kNetworkModeAdHoc);
  ReportBSS("bss1", "ssid", "00:00:00:00:00:01", 0, kNetworkModeAdHoc);
  ReportScanDone();
  EXPECT_EQ(1, GetServices().size());
}

TEST_F(WiFiMainTest, EndpointGroupingDifferentSSID) {
  StartWiFi();
  EXPECT_CALL(*manager(), RegisterService(_))
      .Times(2);
  ReportBSS("bss0", "ssid1", "00:00:00:00:00:00", 0, kNetworkModeAdHoc);
  ReportBSS("bss1", "ssid2", "00:00:00:00:00:01", 0, kNetworkModeAdHoc);
  ReportScanDone();
  EXPECT_EQ(2, GetServices().size());
}

TEST_F(WiFiMainTest, EndpointGroupingDifferentMode) {
  StartWiFi();
  EXPECT_CALL(*manager(), RegisterService(_))
      .Times(2);
  ReportBSS("bss0", "ssid", "00:00:00:00:00:00", 0, kNetworkModeAdHoc);
  ReportBSS("bss1", "ssid", "00:00:00:00:00:01", 0, kNetworkModeInfrastructure);
  ReportScanDone();
  EXPECT_EQ(2, GetServices().size());
}

TEST_F(WiFiMainTest, NonExistentBSSRemoved) {
  // Removal of non-existent BSS should not cause a crash.
  StartWiFi();
  RemoveBSS("bss0");
  EXPECT_EQ(0, GetServices().size());
}

TEST_F(WiFiMainTest, LoneBSSRemoved) {
  StartWiFi();
  ReportBSS("bss0", "ssid", "00:00:00:00:00:00", 0, kNetworkModeAdHoc);
  ReportScanDone();
  EXPECT_EQ(1, GetServices().size());
  EXPECT_TRUE(GetServices().front()->IsVisible());

  EXPECT_CALL(*manager(), DeregisterService(_));
  RemoveBSS("bss0");
  EXPECT_TRUE(GetServices().empty());
}

TEST_F(WiFiMainTest, LoneBSSRemovedWhileConnected) {
  StartWiFi();
  ReportBSS("bss0", "ssid", "00:00:00:00:00:00", 0, kNetworkModeAdHoc);
  ReportScanDone();
  ReportCurrentBSSChanged("bss0");

  EXPECT_CALL(*GetSupplicantInterfaceProxy(), Disconnect());
  EXPECT_CALL(*manager(), DeregisterService(_));
  RemoveBSS("bss0");
  EXPECT_TRUE(GetServices().empty());
}

TEST_F(WiFiMainTest, LoneBSSRemovedWhileConnectedToHidden) {
  StartWiFi();

  Error e;
  WiFiServiceRefPtr service =
      GetServiceInner(flimflam::kTypeWifi, "ssid", flimflam::kModeManaged,
                      NULL, NULL, true, &e);
  EXPECT_EQ(1, GetServices().size());

  ReportBSS("bss", "ssid", "00:00:00:00:00:01", 0, kNetworkModeInfrastructure);
  ReportScanDone();
  ReportCurrentBSSChanged("bss");
  EXPECT_EQ(1, GetServices().size());

  EXPECT_CALL(*GetSupplicantInterfaceProxy(), Disconnect());
  EXPECT_CALL(*manager(), UpdateService(_));
  RemoveBSS("bss");
  EXPECT_TRUE(manager()->HasService(service));
  EXPECT_EQ(1, GetServices().size());
  // Verify expectations now, because WiFi may call UpdateService when
  // WiFi is Stop()-ed (during TearDown()).
  Mock::VerifyAndClearExpectations(manager());
}

TEST_F(WiFiMainTest, NonSolitaryBSSRemoved) {
  StartWiFi();
  ReportBSS("bss0", "ssid", "00:00:00:00:00:00", 0, kNetworkModeAdHoc);
  ReportBSS("bss1", "ssid", "00:00:00:00:00:00", 0, kNetworkModeAdHoc);
  ReportScanDone();
  EXPECT_EQ(1, GetServices().size());
  EXPECT_TRUE(GetServices().front()->IsVisible());

  EXPECT_CALL(*manager(), UpdateService(_));
  RemoveBSS("bss0");
  EXPECT_TRUE(GetServices().front()->IsVisible());
  EXPECT_EQ(1, GetServices().size());
}

TEST_F(WiFiMainTest, Connect) {
  MockSupplicantInterfaceProxy &supplicant_interface_proxy =
      *supplicant_interface_proxy_;

  StartWiFi();
  ReportBSS("bss0", "ssid0", "00:00:00:00:00:00", 0, kNetworkModeAdHoc);
  ReportScanDone();

  {
    InSequence s;
    DBus::Path fake_path("/fake/path");
    WiFiService *service(GetServices().begin()->get());

    EXPECT_CALL(supplicant_interface_proxy, AddNetwork(_))
        .WillOnce(Return(fake_path));
    EXPECT_CALL(supplicant_interface_proxy, SelectNetwork(fake_path));
    InitiateConnect(service);
    EXPECT_EQ(static_cast<Service *>(service),
              wifi()->selected_service_.get());
  }
}

TEST_F(WiFiMainTest, DisconnectPendingService) {
  MockSupplicantInterfaceProxy &supplicant_interface_proxy =
      *supplicant_interface_proxy_;

  StartWiFi();
  ReportBSS("bss0", "ssid0", "00:00:00:00:00:00", 0, kNetworkModeAdHoc);
  WiFiService *service(GetServices().begin()->get());
  InitiateConnect(service);

  EXPECT_FALSE(GetPendingService() == NULL);
  EXPECT_CALL(supplicant_interface_proxy, Disconnect());
  InitiateDisconnect(service);

  EXPECT_TRUE(GetPendingService() == NULL);
}

TEST_F(WiFiMainTest, DisconnectPendingServiceWithCurrent) {
  MockSupplicantInterfaceProxy &supplicant_interface_proxy =
      *supplicant_interface_proxy_;

  StartWiFi();
  ReportBSS("bss0", "ssid0", "00:00:00:00:00:00", 0, kNetworkModeAdHoc);
  ReportBSS("bss1", "ssid1", "00:00:00:00:00:01", 0, kNetworkModeAdHoc);
  WiFiService *service0(GetServices()[0].get());
  WiFiService *service1(GetServices()[1].get());

  InitiateConnect(service0);
  ReportCurrentBSSChanged("bss0");
  ReportStateChanged(wpa_supplicant::kInterfaceStateCompleted);
  InitiateConnect(service1);

  EXPECT_EQ(service0, GetCurrentService());
  EXPECT_EQ(service1, GetPendingService());
  EXPECT_CALL(supplicant_interface_proxy, Disconnect());
  InitiateDisconnect(service1);

  // |current_service_| will be unchanged until supplicant signals
  // that CurrentBSS has changed.
  EXPECT_EQ(service0, GetCurrentService());
  // |pending_service_| is updated immediately.
  EXPECT_TRUE(GetPendingService() == NULL);
}

TEST_F(WiFiMainTest, DisconnectCurrentService) {
  MockSupplicantInterfaceProxy &supplicant_interface_proxy =
      *supplicant_interface_proxy_;

  StartWiFi();
  ReportBSS("bss0", "ssid0", "00:00:00:00:00:00", 0, kNetworkModeAdHoc);
  WiFiService *service(GetServices().begin()->get());
  InitiateConnect(service);
  ReportCurrentBSSChanged("bss0");
  ReportStateChanged(wpa_supplicant::kInterfaceStateCompleted);

  EXPECT_EQ(service, GetCurrentService());
  EXPECT_CALL(supplicant_interface_proxy, Disconnect());
  InitiateDisconnect(service);

  // |current_service_| should not change until supplicant reports
  // a BSS change.
  EXPECT_EQ(service, GetCurrentService());
}

TEST_F(WiFiMainTest, DisconnectCurrentServiceWithPending) {
  MockSupplicantInterfaceProxy &supplicant_interface_proxy =
      *supplicant_interface_proxy_;

  StartWiFi();
  ReportBSS("bss0", "ssid0", "00:00:00:00:00:00", 0, kNetworkModeAdHoc);
  ReportBSS("bss1", "ssid1", "00:00:00:00:00:01", 0, kNetworkModeAdHoc);
  WiFiService *service0(GetServices()[0].get());
  WiFiService *service1(GetServices()[1].get());

  InitiateConnect(service0);
  ReportCurrentBSSChanged("bss0");
  ReportStateChanged(wpa_supplicant::kInterfaceStateCompleted);
  InitiateConnect(service1);

  EXPECT_EQ(service0, GetCurrentService());
  EXPECT_EQ(service1, GetPendingService());
  EXPECT_CALL(supplicant_interface_proxy, Disconnect())
      .Times(0);
  InitiateDisconnect(service0);

  EXPECT_EQ(service0, GetCurrentService());
  EXPECT_EQ(service1, GetPendingService());
}

TEST_F(WiFiMainTest, DisconnectInvalidService) {
  MockSupplicantInterfaceProxy &supplicant_interface_proxy =
      *supplicant_interface_proxy_;

  StartWiFi();
  ReportBSS("bss0", "ssid0", "00:00:00:00:00:00", 0, kNetworkModeAdHoc);
  WiFiService *service(GetServices().begin()->get());
  EXPECT_CALL(supplicant_interface_proxy, Disconnect())
      .Times(0);
  InitiateDisconnect(service);
}

TEST_F(WiFiMainTest, DisconnectCurrentServiceFailure) {
  MockSupplicantInterfaceProxy &supplicant_interface_proxy =
      *supplicant_interface_proxy_;

  StartWiFi();
  ReportBSS("bss0", "ssid0", "00:00:00:00:00:00", 0, kNetworkModeAdHoc);

  WiFiService *service(GetServices().begin()->get());
  DBus::Path fake_path("/fake/path");
  EXPECT_CALL(supplicant_interface_proxy, AddNetwork(_))
      .WillOnce(Return(fake_path));
  InitiateConnect(service);
  ReportCurrentBSSChanged("bss0");
  ReportStateChanged(wpa_supplicant::kInterfaceStateCompleted);

  EXPECT_EQ(service, GetCurrentService());
  EXPECT_CALL(supplicant_interface_proxy, Disconnect())
      .WillRepeatedly(Throw(
          DBus::Error(
              "fi.w1.wpa_supplicant1.NotConnected",
              "test threw fi.w1.wpa_supplicant1.NotConnected")));
  EXPECT_CALL(supplicant_interface_proxy, RemoveNetwork(fake_path));
  InitiateDisconnect(service);

  EXPECT_TRUE(GetCurrentService() == NULL);
}

TEST_F(WiFiMainTest, LinkEvent) {
  EXPECT_FALSE(IsLinkUp());
  EXPECT_CALL(dhcp_provider_, CreateConfig(_, _)).
      WillOnce(Return(dhcp_config_));
  ReportLinkUp();
}

TEST_F(WiFiMainTest, Stop) {
  {
    InSequence s;

    StartWiFi();
    ReportBSS("bss0", "ssid0", "00:00:00:00:00:00", 0, kNetworkModeAdHoc);
    ReportScanDone();
    EXPECT_CALL(dhcp_provider_, CreateConfig(_, _)).
        WillOnce(Return(dhcp_config_));
    ReportLinkUp();
  }

  {
    EXPECT_CALL(*manager(), DeregisterService(_));
    StopWiFi();
  }
}

TEST_F(WiFiMainTest, GetWifiServiceOpen) {
  Error e;
  GetOpenService(flimflam::kTypeWifi, "an_ssid", flimflam::kModeManaged, &e);
  EXPECT_TRUE(e.IsSuccess());
}

TEST_F(WiFiMainTest, GetWifiServiceOpenNoType) {
  Error e;
  GetOpenService(NULL, "an_ssid", flimflam::kModeManaged, &e);
  EXPECT_EQ(Error::kInvalidArguments, e.type());
  EXPECT_EQ("must specify service type", e.message());
}

TEST_F(WiFiMainTest, GetWifiServiceOpenNoSSID) {
  Error e;
  GetOpenService(flimflam::kTypeWifi, NULL, flimflam::kModeManaged, &e);
  EXPECT_EQ(Error::kInvalidArguments, e.type());
  EXPECT_EQ("must specify SSID", e.message());
}

TEST_F(WiFiMainTest, GetWifiServiceOpenLongSSID) {
  Error e;
  GetOpenService(
      flimflam::kTypeWifi, "123456789012345678901234567890123",
      flimflam::kModeManaged, &e);
  EXPECT_EQ(Error::kInvalidNetworkName, e.type());
  EXPECT_EQ("SSID is too long", e.message());
}

TEST_F(WiFiMainTest, GetWifiServiceOpenShortSSID) {
  Error e;
  GetOpenService(flimflam::kTypeWifi, "", flimflam::kModeManaged, &e);
  EXPECT_EQ(Error::kInvalidNetworkName, e.type());
  EXPECT_EQ("SSID is too short", e.message());
}

TEST_F(WiFiMainTest, GetWifiServiceOpenBadMode) {
  Error e;
  GetOpenService(flimflam::kTypeWifi, "an_ssid", "ad-hoc", &e);
  EXPECT_EQ(Error::kNotSupported, e.type());
  EXPECT_EQ("service mode is unsupported", e.message());
}

TEST_F(WiFiMainTest, GetWifiServiceOpenNoMode) {
  Error e;
  GetOpenService(flimflam::kTypeWifi, "an_ssid", NULL, &e);
  EXPECT_TRUE(e.IsSuccess());
}

TEST_F(WiFiMainTest, GetWifiServiceRSN) {
  Error e;
  GetService(flimflam::kTypeWifi, "an_ssid", flimflam::kModeManaged,
             flimflam::kSecurityRsn, "secure password", &e);
  EXPECT_TRUE(e.IsSuccess());
}

TEST_F(WiFiMainTest, GetWifiServiceRSNNoPassword) {
  Error e;
  GetService(flimflam::kTypeWifi, "an_ssid", flimflam::kModeManaged,
             flimflam::kSecurityRsn, NULL, &e);
  EXPECT_EQ(Error::kInvalidArguments, e.type());
  EXPECT_EQ("must specify passphrase", e.message());
}

TEST_F(WiFiMainTest, GetWifiServiceBadSecurity) {
  Error e;
  GetService(flimflam::kTypeWifi, "an_ssid", flimflam::kModeManaged, "rot-13",
             NULL, &e);
  EXPECT_EQ(Error::kNotSupported, e.type());
  EXPECT_EQ("security mode is unsupported", e.message());
}

TEST_F(WiFiMainTest, GetWifiServiceWEPNoPassword) {
  Error e;
  GetService(flimflam::kTypeWifi, "an_ssid", flimflam::kModeManaged,
             flimflam::kSecurityWep, NULL, &e);
  EXPECT_EQ(Error::kInvalidArguments, e.type());
  EXPECT_EQ("must specify passphrase", e.message());
}

TEST_F(WiFiMainTest, GetWifiServiceWEPEmptyPassword) {
  Error e;
  GetService(flimflam::kTypeWifi, "an_ssid", flimflam::kModeManaged,
             flimflam::kSecurityWep, "", &e);
  EXPECT_EQ(Error::kInvalidPassphrase, e.type());
}

TEST_F(WiFiMainTest, GetWifiServiceWEP40ASCII) {
  Error e;
  GetService(flimflam::kTypeWifi, "an_ssid", flimflam::kModeManaged,
             flimflam::kSecurityWep, "abcde", &e);
  EXPECT_TRUE(e.IsSuccess());
}

TEST_F(WiFiMainTest, GetWifiServiceWEP104ASCII) {
  Error e;
  GetService(flimflam::kTypeWifi, "an_ssid", flimflam::kModeManaged,
             flimflam::kSecurityWep, "abcdefghijklm", &e);
  EXPECT_TRUE(e.IsSuccess());
}

TEST_F(WiFiMainTest, GetWifiServiceWEP40ASCIIWithKeyIndex) {
  Error e;
  GetService(flimflam::kTypeWifi, "an_ssid", flimflam::kModeManaged,
             flimflam::kSecurityWep, "0:abcdefghijklm", &e);
  EXPECT_TRUE(e.IsSuccess());
}

TEST_F(WiFiMainTest, GetWifiServiceWEP40Hex) {
  Error e;
  GetService(flimflam::kTypeWifi, "an_ssid", flimflam::kModeManaged,
             flimflam::kSecurityWep, "0102030405", &e);
  EXPECT_TRUE(e.IsSuccess());
}

TEST_F(WiFiMainTest, GetWifiServiceWEP40HexBadPassphrase) {
  Error e;
  GetService(flimflam::kTypeWifi, "an_ssid", flimflam::kModeManaged,
             flimflam::kSecurityWep, "O102030405", &e);
  EXPECT_EQ(Error::kInvalidPassphrase, e.type());
}

TEST_F(WiFiMainTest, GetWifiServiceWEP40HexWithKeyIndexBadPassphrase) {
  Error e;
  GetService(flimflam::kTypeWifi, "an_ssid", flimflam::kModeManaged,
             flimflam::kSecurityWep, "1:O102030405", &e);
  EXPECT_EQ(Error::kInvalidPassphrase, e.type());
}

TEST_F(WiFiMainTest, GetWifiServiceWEP40HexWithKeyIndexAndBaseBadPassphrase) {
  Error e;
  GetService(flimflam::kTypeWifi, "an_ssid", flimflam::kModeManaged,
             flimflam::kSecurityWep, "1:0xO102030405", &e);
  EXPECT_EQ(Error::kInvalidPassphrase, e.type());
}

TEST_F(WiFiMainTest, GetWifiServiceWEP40HexWithBaseBadPassphrase) {
  Error e;
  GetService(flimflam::kTypeWifi, "an_ssid", flimflam::kModeManaged,
             flimflam::kSecurityWep, "0xO102030405", &e);
  EXPECT_EQ(Error::kInvalidPassphrase, e.type());
}

TEST_F(WiFiMainTest, GetWifiServiceWEP104Hex) {
  Error e;
  GetService(flimflam::kTypeWifi, "an_ssid", flimflam::kModeManaged,
             flimflam::kSecurityWep, "0102030405060708090a0b0c0d", &e);
  EXPECT_TRUE(e.IsSuccess());
}

TEST_F(WiFiMainTest, GetWifiServiceWEP104HexUppercase) {
  Error e;
  GetService(flimflam::kTypeWifi, "an_ssid", flimflam::kModeManaged,
             flimflam::kSecurityWep, "0102030405060708090A0B0C0D", &e);
  EXPECT_TRUE(e.IsSuccess());
}

TEST_F(WiFiMainTest, GetWifiServiceWEP104HexWithKeyIndex) {
  Error e;
  GetService(flimflam::kTypeWifi, "an_ssid", flimflam::kModeManaged,
             flimflam::kSecurityWep, "0:0102030405060708090a0b0c0d", &e);
  EXPECT_TRUE(e.IsSuccess());
}

TEST_F(WiFiMainTest, GetWifiServiceWEP104HexWithKeyIndexAndBase) {
  Error e;
  GetService(flimflam::kTypeWifi, "an_ssid", flimflam::kModeManaged,
             flimflam::kSecurityWep, "0:0x0102030405060708090a0b0c0d", &e);
  EXPECT_TRUE(e.IsSuccess());
}

class WiFiGetServiceSuccessTest : public WiFiMainTest {};
class WiFiGetServiceFailureTest : public WiFiMainTest {};

TEST_P(WiFiGetServiceSuccessTest, Passphrase) {
  Error e;
  GetService(flimflam::kTypeWifi, "an_ssid", flimflam::kModeManaged,
             flimflam::kSecurityWpa, GetParam().c_str(), &e);
  EXPECT_TRUE(e.IsSuccess());
}

TEST_P(WiFiGetServiceFailureTest, Passphrase) {
  Error e;
  GetService(flimflam::kTypeWifi, "an_ssid", flimflam::kModeManaged,
             flimflam::kSecurityWpa, GetParam().c_str(), &e);
  EXPECT_EQ(Error::kInvalidPassphrase, e.type());
}

INSTANTIATE_TEST_CASE_P(
    WiFiGetServiceSuccessTestInstance,
    WiFiGetServiceSuccessTest,
    Values(
        string(IEEE_80211::kWPAAsciiMinLen, 'Z'),
        string(IEEE_80211::kWPAAsciiMaxLen, 'Z'),
        // subtle: invalid length for hex key, but valid as ascii passphrase
        string(IEEE_80211::kWPAHexLen-1, '1'),
        string(IEEE_80211::kWPAHexLen, '1')));

INSTANTIATE_TEST_CASE_P(
    WiFiGetServiceFailureTestInstance,
    WiFiGetServiceFailureTest,
    Values(
        string(IEEE_80211::kWPAAsciiMinLen-1, 'Z'),
        string(IEEE_80211::kWPAAsciiMaxLen+1, 'Z'),
        string(IEEE_80211::kWPAHexLen+1, '1')));

TEST_F(WiFiMainTest, FindServiceWEP) {
  const string ssid("an_ssid");
  {
    Error e;
    GetService(flimflam::kTypeWifi, ssid.c_str(), flimflam::kModeManaged,
               flimflam::kSecurityWep, "abcde", &e);
    EXPECT_TRUE(e.IsSuccess());
  }
  vector<uint8_t> ssid_bytes(ssid.begin(), ssid.end());

  EXPECT_TRUE(FindService(ssid_bytes, flimflam::kModeManaged,
                          flimflam::kSecurityWep).get());
  EXPECT_FALSE(FindService(ssid_bytes, flimflam::kModeManaged,
                           flimflam::kSecurityWpa).get());
}

TEST_F(WiFiMainTest, FindServiceWPA) {
  const string ssid("an_ssid");
  {
    Error e;
    GetService(flimflam::kTypeWifi, ssid.c_str(), flimflam::kModeManaged,
               flimflam::kSecurityRsn, "abcdefgh", &e);
    EXPECT_TRUE(e.IsSuccess());
  }
  vector<uint8_t> ssid_bytes(ssid.begin(), ssid.end());
  WiFiServiceRefPtr wpa_service(FindService(ssid_bytes, flimflam::kModeManaged,
                                            flimflam::kSecurityWpa));
  EXPECT_TRUE(wpa_service.get());
  WiFiServiceRefPtr rsn_service(FindService(ssid_bytes, flimflam::kModeManaged,
                                            flimflam::kSecurityRsn));
  EXPECT_TRUE(rsn_service.get());
  EXPECT_EQ(wpa_service.get(), rsn_service.get());
  WiFiServiceRefPtr psk_service(FindService(ssid_bytes, flimflam::kModeManaged,
                                            flimflam::kSecurityPsk));
  EXPECT_EQ(wpa_service.get(), psk_service.get());
  // Indirectly test FindService by doing a GetService on something that
  // already exists.
  {
    Error e;
    WiFiServiceRefPtr wpa_service2(
        GetServiceInner(flimflam::kTypeWifi, ssid.c_str(),
                        flimflam::kModeManaged, flimflam::kSecurityWpa,
                        "abcdefgh", false, &e));
    EXPECT_TRUE(e.IsSuccess());
    EXPECT_EQ(wpa_service.get(), wpa_service2.get());
  }
}

MATCHER_P(HasHiddenSSID, ssid, "") {
  map<string, DBus::Variant>::const_iterator it =
      arg.find(wpa_supplicant::kPropertyScanSSIDs);
  if (it == arg.end()) {
    return false;
  }

  const DBus::Variant &ssids_variant = it->second;
  EXPECT_TRUE(DBusAdaptor::IsByteArrays(ssids_variant.signature()));
  const ByteArrays &ssids = it->second.operator ByteArrays();
  // A valid Scan containing a single hidden SSID should contain
  // two SSID entries: one containing the SSID we are looking for,
  // and an empty entry, signifying that we also want to do a
  // broadcast probe request for all non-hidden APs as well.
  return ssids.size() == 2 &&
      string(ssids[0].begin(), ssids[0].end()) == ssid &&
      ssids[1].empty();
}

TEST_F(WiFiMainTest, ScanHidden) {
  EXPECT_CALL(*supplicant_process_proxy_, CreateInterface(_));
  EXPECT_CALL(*supplicant_process_proxy_, GetInterface(_))
      .Times(AnyNumber())
      .WillRepeatedly(Throw(
          DBus::Error(
              "fi.w1.wpa_supplicant1.InterfaceUnknown",
              "test threw fi.w1.wpa_supplicant1.InterfaceUnknown")));
  {
    // Create a hidden, favorite service.
    Error e;
    WiFiServiceRefPtr service =
        GetServiceInner(flimflam::kTypeWifi, "ssid0", flimflam::kModeManaged,
                        NULL, NULL, true, &e);
    EXPECT_TRUE(e.IsSuccess());
    EXPECT_TRUE(service->hidden_ssid());
    service->MakeFavorite();
  }
  {
    // Create a hidden, non-favorite service.
    Error e;
    WiFiServiceRefPtr service =
        GetServiceInner(flimflam::kTypeWifi, "ssid1", flimflam::kModeManaged,
                        NULL, NULL, true, &e);
    EXPECT_TRUE(e.IsSuccess());
    EXPECT_TRUE(service->hidden_ssid());
  }
  {
    // Create a non-hidden, favorite service.
    Error e;
    WiFiServiceRefPtr service =
        GetServiceInner(flimflam::kTypeWifi, "ssid2", flimflam::kModeManaged,
                        NULL, NULL, false, &e);
    EXPECT_TRUE(e.IsSuccess());
    EXPECT_FALSE(service->hidden_ssid());
    service->MakeFavorite();
  }
  EXPECT_CALL(*supplicant_interface_proxy_, Scan(HasHiddenSSID("ssid0")));
  StartWiFi();
  dispatcher_.DispatchPendingEvents();
}

TEST_F(WiFiMainTest, InitialSupplicantState) {
  EXPECT_EQ(WiFi::kInterfaceStateUnknown, GetSupplicantState());
}

TEST_F(WiFiMainTest, StateChangeNoService) {
  // State change should succeed even if there is no pending Service.
  ReportStateChanged(wpa_supplicant::kInterfaceStateScanning);
  EXPECT_EQ(wpa_supplicant::kInterfaceStateScanning, GetSupplicantState());
}

TEST_F(WiFiMainTest, StateChangeWithService) {
  // Forward transition should trigger a Service state change.
  StartWiFi();
  dispatcher_.DispatchPendingEvents();
  MockWiFiServiceRefPtr service = MakeMockService();
  InitiateConnect(service);
  EXPECT_CALL(*service.get(), SetState(Service::kStateAssociating));
  ReportStateChanged(wpa_supplicant::kInterfaceStateAssociated);
  // Verify expectations now, because WiFi may report other state changes
  // when WiFi is Stop()-ed (during TearDown()).
  Mock::VerifyAndClearExpectations(service.get());
}

TEST_F(WiFiMainTest, StateChangeBackwardsWithService) {
  // Some backwards transitions should not trigger a Service state change.
  // Supplicant state should still be updated, however.
  StartWiFi();
  dispatcher_.DispatchPendingEvents();
  MockWiFiServiceRefPtr service = MakeMockService();
  InitiateConnect(service);
  ReportStateChanged(wpa_supplicant::kInterfaceStateCompleted);
  EXPECT_CALL(*service.get(), SetState(_)).Times(0);
  ReportStateChanged(wpa_supplicant::kInterfaceStateAuthenticating);
  EXPECT_EQ(wpa_supplicant::kInterfaceStateAuthenticating,
            GetSupplicantState());
  // Verify expectations now, because WiFi may report other state changes
  // when WiFi is Stop()-ed (during TearDown()).
  Mock::VerifyAndClearExpectations(service.get());
}

TEST_F(WiFiMainTest, LoadHiddenServicesFailWithNoGroups) {
  StrictMock<MockStore> storage;
  EXPECT_CALL(storage, GetGroupsWithKey(flimflam::kWifiHiddenSsid))
      .WillOnce(Return(set<string>()));
  EXPECT_FALSE(LoadHiddenServices(&storage));
}

TEST_F(WiFiMainTest, LoadHiddenServicesFailWithMissingHidden) {
  string id;
  StrictMock<MockStore> storage;
  SetupHiddenStorage(&storage, "an_ssid", &id);
  // Missing "Hidden" property.
  EXPECT_CALL(storage, GetBool(StrEq(id), flimflam::kWifiHiddenSsid, _))
      .WillOnce(Return(false));
  EXPECT_FALSE(LoadHiddenServices(&storage));
}

TEST_F(WiFiMainTest, LoadHiddenServicesFailWithFalseHidden) {
  string id;
  StrictMock<MockStore> storage;
  SetupHiddenStorage(&storage, "an_ssid", &id);
  // "Hidden" property set to "false".
  EXPECT_CALL(storage, GetBool(StrEq(id), flimflam::kWifiHiddenSsid, _))
      .WillOnce(DoAll(SetArgumentPointee<2>(true), Return(false)));
  EXPECT_FALSE(LoadHiddenServices(&storage));
}

TEST_F(WiFiMainTest, LoadHiddenServicesFailWithMissingSSID) {
  string id;
  StrictMock<MockStore> storage;
  SetupHiddenStorage(&storage, "an_ssid", &id);
  // Missing "SSID" property.
  EXPECT_CALL(storage, GetString(StrEq(id), flimflam::kSSIDProperty, _))
      .WillOnce(Return(false));
  EXPECT_FALSE(LoadHiddenServices(&storage));
}


TEST_F(WiFiMainTest, LoadHiddenServicesFailWithFoundService) {
  StrictMock<MockStore> storage;
  string id;
  SetupHiddenStorage(&storage, "an_ssid", &id);
  Error e;
  GetOpenService(flimflam::kTypeWifi, "an_ssid", NULL, &e);
  ASSERT_TRUE(e.IsSuccess());
  EXPECT_FALSE(LoadHiddenServices(&storage));
}

TEST_F(WiFiMainTest, LoadHiddenServicesSuccess) {
  StrictMock<MockStore> storage;
  string ssid("an_ssid");
  string id;
  SetupHiddenStorage(&storage, ssid, &id);
  EXPECT_TRUE(LoadHiddenServices(&storage));
  vector<uint8_t> ssid_bytes(ssid.begin(), ssid.end());
  EXPECT_TRUE(FindService(ssid_bytes, flimflam::kModeManaged,
                          flimflam::kSecurityNone).get());
}

TEST_F(WiFiMainTest, CurrentBSSChangeConnectedToDisconnected) {
  WiFiEndpointRefPtr ap = MakeEndpoint("an_ssid", "00:01:02:03:04:05");
  WiFiServiceRefPtr service = CreateServiceForEndpoint(*ap);

  // Note that the BSS handle used in this test ("an_ap") is not
  // intended to reflect the format used by supplicant. It's just
  // convenient for testing.

  StartWiFi();
  ReportBSS("an_ap", ap->ssid_string(), ap->bssid_string(), 0,
            kNetworkModeInfrastructure);
  InitiateConnect(service);
  EXPECT_EQ(service, GetPendingService().get());

  ReportCurrentBSSChanged("an_ap");
  ReportStateChanged(wpa_supplicant::kInterfaceStateCompleted);
  EXPECT_EQ(Service::kStateConfiguring, service->state());
  EXPECT_EQ(service, GetCurrentService().get());
  EXPECT_EQ(NULL, GetPendingService().get());

  ReportCurrentBSSChanged(wpa_supplicant::kCurrentBSSNull);
  EXPECT_EQ(Service::kStateFailure, service->state());
  EXPECT_EQ(NULL, GetCurrentService().get());
  EXPECT_EQ(NULL, GetPendingService().get());
}

TEST_F(WiFiMainTest, CurrentBSSChangeConnectedToConnectedNewService) {
  WiFiEndpointRefPtr ap1 = MakeEndpoint("an_ssid", "00:01:02:03:04:05");
  WiFiEndpointRefPtr ap2 = MakeEndpoint("another_ssid", "01:02:03:04:05:06");
  WiFiServiceRefPtr service1 = CreateServiceForEndpoint(*ap1);
  WiFiServiceRefPtr service2 = CreateServiceForEndpoint(*ap2);

  // Note that the BSS handles used in this test ("ap1", "ap2") are
  // not intended to reflect the format used by supplicant. They're
  // just convenient for testing.

  StartWiFi();
  ReportBSS("ap1", ap1->ssid_string(), ap1->bssid_string(), 0,
            kNetworkModeInfrastructure);
  ReportBSS("ap2", ap2->ssid_string(), ap2->bssid_string(), 0,
            kNetworkModeInfrastructure);
  InitiateConnect(service1);
  ReportCurrentBSSChanged("ap1");
  ReportStateChanged(wpa_supplicant::kInterfaceStateCompleted);
  EXPECT_EQ(service1.get(), GetCurrentService().get());

  ReportCurrentBSSChanged("ap2");
  ReportStateChanged(wpa_supplicant::kInterfaceStateCompleted);
  EXPECT_EQ(service2.get(), GetCurrentService().get());
  EXPECT_EQ(Service::kStateIdle, service1->state());
  EXPECT_EQ(Service::kStateConfiguring, service2->state());
}

TEST_F(WiFiMainTest, CurrentBSSChangeDisconnectedToConnected) {
  WiFiEndpointRefPtr ap = MakeEndpoint("an_ssid", "00:01:02:03:04:05");
  WiFiServiceRefPtr service = CreateServiceForEndpoint(*ap);

  // Note that the BSS handle used in this test ("an_ap") is not
  // intended to reflect the format used by supplicant. It's just
  // convenient for testing.

  StartWiFi();
  ReportBSS("an_ap", ap->ssid_string(), ap->bssid_string(), 0,
            kNetworkModeInfrastructure);
  InitiateConnect(service);
  ReportCurrentBSSChanged("an_ap");
  ReportStateChanged(wpa_supplicant::kInterfaceStateCompleted);
  EXPECT_EQ(service.get(), GetCurrentService().get());
  EXPECT_EQ(Service::kStateConfiguring, service->state());
}

TEST_F(WiFiMainTest, ConfiguredServiceRegistration) {
  Error e;
  EXPECT_CALL(*manager(), RegisterService(_))
      .Times(0);
  EXPECT_CALL(*manager(), HasService(_))
      .WillOnce(Return(false));
  GetOpenService(flimflam::kTypeWifi, "an_ssid", flimflam::kModeManaged, &e);
  EXPECT_CALL(*manager(), RegisterService(_));
  ReportBSS("ap0", "an_ssid", "00:00:00:00:00:00", 0,
            kNetworkModeInfrastructure);
}

TEST_F(WiFiMainTest, NewConnectPreemptsPending) {
  WiFiEndpointRefPtr ap1 = MakeEndpoint("an_ssid", "00:01:02:03:04:05");
  WiFiEndpointRefPtr ap2 = MakeEndpoint("another_ssid", "01:02:03:04:05:06");
  WiFiServiceRefPtr service1 = CreateServiceForEndpoint(*ap1);
  WiFiServiceRefPtr service2 = CreateServiceForEndpoint(*ap2);

  StartWiFi();
  ReportBSS("ap1", ap1->ssid_string(), ap1->bssid_string(), 0,
            kNetworkModeInfrastructure);
  ReportBSS("ap2", ap2->ssid_string(), ap2->bssid_string(), 0,
            kNetworkModeInfrastructure);
  InitiateConnect(service1);
  EXPECT_EQ(service1.get(), GetPendingService().get());

  EXPECT_CALL(*GetSupplicantInterfaceProxy(), Disconnect());
  EXPECT_CALL(*GetSupplicantInterfaceProxy(), AddNetwork(_));
  InitiateConnect(service2);
  EXPECT_EQ(service2.get(), GetPendingService().get());
}

TEST_F(WiFiMainTest, IsIdle) {
  StartWiFi();
  EXPECT_TRUE(wifi()->IsIdle());

  WiFiEndpointRefPtr ap = MakeEndpoint("an_ssid", "00:01:02:03:04:05");
  WiFiServiceRefPtr service = CreateServiceForEndpoint(*ap);
  Error error;
  service->AddEndpoint(ap);
  service->AutoConnect();
  EXPECT_FALSE(wifi()->IsIdle());
}

MATCHER(WiFiAddedArgs, "") {
  return ContainsKey(arg, wpa_supplicant::kNetworkPropertyScanSSID) &&
      ContainsKey(arg, wpa_supplicant::kNetworkPropertyBgscan);
}

TEST_F(WiFiMainTest, AddNetworkArgs) {
  MockSupplicantInterfaceProxy &supplicant_interface_proxy =
      *supplicant_interface_proxy_;

  StartWiFi();
  ReportBSS("bss0", "ssid0", "00:00:00:00:00:00", 0, kNetworkModeAdHoc);
  WiFiService *service(GetServices().begin()->get());
  EXPECT_CALL(supplicant_interface_proxy, AddNetwork(WiFiAddedArgs()));
  InitiateConnect(service);
}

}  // namespace shill
