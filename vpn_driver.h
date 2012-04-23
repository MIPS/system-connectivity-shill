// Copyright (c) 2012 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SHILL_VPN_DRIVER_
#define SHILL_VPN_DRIVER_

#include <string>

#include <base/basictypes.h>

#include "shill/accessor_interface.h"
#include "shill/key_value_store.h"
#include "shill/refptr_types.h"

namespace shill {

class Error;
class PropertyStore;
class StoreInterface;

class VPNDriver {
 public:
  virtual ~VPNDriver();

  virtual bool ClaimInterface(const std::string &link_name,
                              int interface_index) = 0;
  virtual void Connect(const VPNServiceRefPtr &service, Error *error) = 0;
  virtual void Disconnect() = 0;
  virtual std::string GetProviderType() const = 0;

  virtual void InitPropertyStore(PropertyStore *store);

  virtual bool Load(StoreInterface *storage, const std::string &storage_id);
  virtual bool Save(StoreInterface *storage, const std::string &storage_id);

  KeyValueStore *args() { return &args_; }

 protected:
  struct Property {
    enum Flags {
      kEphemeral = 1 << 0,
      kCrypted = 1 << 1,
    };

    const char *property;
    int flags;
  };

  VPNDriver(const Property *properties, size_t property_count);

 private:
  Stringmap GetProvider(Error *error);
  void ClearMappedProperty(const size_t &index, Error *error);
  std::string GetMappedProperty(const size_t &index, Error *error);
  void SetMappedProperty(
      const size_t &index, const std::string &value, Error *error);

  const Property * const properties_;
  const size_t property_count_;
  KeyValueStore args_;

  DISALLOW_COPY_AND_ASSIGN(VPNDriver);
};

}  // namespace shill

#endif  // SHILL_VPN_DRIVER_
