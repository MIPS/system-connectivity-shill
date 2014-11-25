// Copyright 2014 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "shill/device_claimer.h"

#include "shill/device_info.h"

using std::string;

namespace shill {

DeviceClaimer::DeviceClaimer(
    const std::string &dbus_service_name,
    DeviceInfo *device_info)
    : dbus_service_name_(dbus_service_name),
      device_info_(device_info) {}

DeviceClaimer::~DeviceClaimer() {
  // Release claimed devices if there is any.
  if (DevicesClaimed()) {
    for (const auto &device : claimed_device_names_) {
      device_info_->RemoveDeviceFromBlackList(device);
    }
    // Clear claimed device list.
    claimed_device_names_.clear();
  }
  // Reset DBus name watcher.
  dbus_name_watcher_.reset();
}

bool DeviceClaimer::StartDBusNameWatcher(
    DBusManager *dbus_manager,
    const DBusNameWatcher::NameAppearedCallback &name_appeared_callback,
    const DBusNameWatcher::NameVanishedCallback &name_vanished_callback) {
  if (dbus_name_watcher_) {
    LOG(ERROR) << "DBus name watcher already started";
    return false;
  }
  dbus_name_watcher_.reset(
      dbus_manager->CreateNameWatcher(dbus_service_name_,
                                      name_appeared_callback,
                                      name_vanished_callback));
  return true;
}

bool DeviceClaimer::Claim(const string &device_name, Error *error) {
  // Check if device is claimed already.
  if (claimed_device_names_.find(device_name) != claimed_device_names_.end()) {
    Error::PopulateAndLog(error, Error::kInvalidArguments,
                          "Device " + device_name +
                          " had already been claimed");
    return false;
  }

  // Add device to the black list.
  device_info_->AddDeviceToBlackList(device_name);

  claimed_device_names_.insert(device_name);
  return true;
}

bool DeviceClaimer::Release(const std::string &device_name,
                            Error *error) {
  // Make sure this is a device that have been claimed.
  if (claimed_device_names_.find(device_name) == claimed_device_names_.end()) {
    Error::PopulateAndLog(error, Error::kInvalidArguments,
                          "Device " + device_name +
                          " have not been claimed");
    return false;
  }

  // Remove the device from the black list.
  device_info_->RemoveDeviceFromBlackList(device_name);

  // Remove device from the claimed list.
  claimed_device_names_.erase(device_name);
  return true;
}

bool DeviceClaimer::DevicesClaimed() {
  return !claimed_device_names_.empty();
}

}  // namespace shill
