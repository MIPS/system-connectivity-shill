// Copyright (c) 2011 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "shill/wifi_endpoint.h"

#include <base/logging.h>
#include <base/stl_util-inl.h>
#include <base/stringprintf.h>
#include <base/string_number_conversions.h>
#include <base/string_util.h>
#include <chromeos/dbus/service_constants.h>

#include "shill/wpa_supplicant.h"

using std::map;
using std::set;
using std::string;
using std::vector;

namespace shill {

WiFiEndpoint::WiFiEndpoint(
    const map<string, ::DBus::Variant> &properties) {
  // XXX will segfault on missing properties
  ssid_ =
      properties.find(wpa_supplicant::kBSSPropertySSID)->second.
      operator std::vector<uint8_t>();
  bssid_ =
      properties.find(wpa_supplicant::kBSSPropertyBSSID)->second.
      operator std::vector<uint8_t>();
  signal_strength_ =
      properties.find(wpa_supplicant::kBSSPropertySignal)->second.
      reader().get_int16();
  network_mode_ = ParseMode(
      properties.find(wpa_supplicant::kBSSPropertyMode)->second);
  security_mode_ = ParseSecurity(properties);

  if (network_mode_.empty()) {
    // XXX log error?
  }

  ssid_string_ = string(ssid_.begin(), ssid_.end());
  ssid_hex_ = base::HexEncode(&(*ssid_.begin()), ssid_.size());
  bssid_string_ = StringPrintf("%02x:%02x:%02x:%02x:%02x:%02x",
                               bssid_[0], bssid_[1], bssid_[2],
                               bssid_[3], bssid_[4], bssid_[5]);
  bssid_hex_ = base::HexEncode(&(*bssid_.begin()), bssid_.size());
}

WiFiEndpoint::~WiFiEndpoint() {}

// static
uint32_t WiFiEndpoint::ModeStringToUint(const std::string &mode_string) {
  if (mode_string == flimflam::kModeManaged)
    return wpa_supplicant::kNetworkModeInfrastructureInt;
  else if (mode_string == flimflam::kModeAdhoc)
    return wpa_supplicant::kNetworkModeAdHocInt;
  else
    NOTIMPLEMENTED() << "Shill dos not support " << mode_string
                     << " mode at this time.";
  return 0;
}

const vector<uint8_t> &WiFiEndpoint::ssid() const {
  return ssid_;
}

const string &WiFiEndpoint::ssid_string() const {
  return ssid_string_;
}

const string &WiFiEndpoint::ssid_hex() const {
  return ssid_hex_;
}

const string &WiFiEndpoint::bssid_string() const {
  return bssid_string_;
}

const string &WiFiEndpoint::bssid_hex() const {
  return bssid_hex_;
}

int16_t WiFiEndpoint::signal_strength() const {
  return signal_strength_;
}

const string &WiFiEndpoint::network_mode() const {
  return network_mode_;
}

const string &WiFiEndpoint::security_mode() const {
  return security_mode_;
}

// static
const char *WiFiEndpoint::ParseMode(const string &mode_string) {
  if (mode_string == wpa_supplicant::kNetworkModeInfrastructure) {
    return flimflam::kModeManaged;
  } else if (mode_string == wpa_supplicant::kNetworkModeAdHoc) {
    return flimflam::kModeAdhoc;
  } else if (mode_string == wpa_supplicant::kNetworkModeAccessPoint) {
    NOTREACHED() << "Shill does not support AP mode at this time.";
    return NULL;
  } else {
    NOTREACHED() << "Unknown WiFi endpoint mode!";
    return NULL;
  }
}

// static
const char *WiFiEndpoint::ParseSecurity(
    const map<string, ::DBus::Variant> &properties) {
  set<KeyManagement> rsn_key_management_methods;
  if (ContainsKey(properties, wpa_supplicant::kPropertyRSN)) {
    // TODO(quiche): check type before casting
    const map<string, ::DBus::Variant> rsn_properties(
        properties.find(wpa_supplicant::kPropertyRSN)->second.
        operator map<string, ::DBus::Variant>());
    ParseKeyManagementMethods(rsn_properties, &rsn_key_management_methods);
  }

  set<KeyManagement> wpa_key_management_methods;
  if (ContainsKey(properties, wpa_supplicant::kPropertyWPA)) {
    // TODO(quiche): check type before casting
    const map<string, ::DBus::Variant> rsn_properties(
        properties.find(wpa_supplicant::kPropertyWPA)->second.
        operator map<string, ::DBus::Variant>());
    ParseKeyManagementMethods(rsn_properties, &wpa_key_management_methods);
  }

  bool wep_privacy = false;
  if (ContainsKey(properties, wpa_supplicant::kPropertyPrivacy)) {
    wep_privacy = properties.find(wpa_supplicant::kPropertyPrivacy)->second.
        reader().get_bool();
  }

  if (ContainsKey(rsn_key_management_methods, kKeyManagement802_1x) ||
      ContainsKey(wpa_key_management_methods, kKeyManagement802_1x)) {
    return flimflam::kSecurity8021x;
  } else if (ContainsKey(rsn_key_management_methods, kKeyManagementPSK)) {
    return flimflam::kSecurityRsn;
  } else if (ContainsKey(wpa_key_management_methods, kKeyManagementPSK)) {
    return flimflam::kSecurityWpa;
  } else if (wep_privacy) {
    return flimflam::kSecurityWep;
  } else {
    return flimflam::kSecurityNone;
  }
}

// static
void WiFiEndpoint::ParseKeyManagementMethods(
    const map<string, ::DBus::Variant> &security_method_properties,
    set<KeyManagement> *key_management_methods) {
  if (!ContainsKey(security_method_properties,
                   wpa_supplicant::kSecurityMethodPropertyKeyManagement)) {
    return;
  }

  // TODO(quiche): check type before cast
  const vector<string> key_management_vec =
      security_method_properties.
      find(wpa_supplicant::kSecurityMethodPropertyKeyManagement)->second.
      operator vector<string>();
  for (vector<string>::const_iterator it = key_management_vec.begin();
       it != key_management_vec.end();
       ++it) {
    if (EndsWith(*it, wpa_supplicant::kKeyManagementMethodSuffixEAP, true)) {
      key_management_methods->insert(kKeyManagement802_1x);
    } else if (
        EndsWith(*it, wpa_supplicant::kKeyManagementMethodSuffixPSK, true)) {
      key_management_methods->insert(kKeyManagementPSK);
    }
  }
}

}  // namespace shill
