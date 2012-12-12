// Copyright (c) 2012 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SHILL_MM1_MOCK_MODEM_TIME_PROXY_H_
#define SHILL_MM1_MOCK_MODEM_TIME_PROXY_H_

#include <base/basictypes.h>
#include <gmock/gmock.h>

#include "shill/mm1_modem_time_proxy_interface.h"

namespace shill {
namespace mm1 {

class MockModemTimeProxy : public ModemTimeProxyInterface {
 public:
  MockModemTimeProxy();
  virtual ~MockModemTimeProxy();

  // Inherited methods from ModemTimeProxyInterface.
  MOCK_METHOD3(GetNetworkTime, void(Error *error,
                                    const StringCallback &callback,
                                    int timeout));

  MOCK_METHOD1(set_network_time_changed_callback,
               void(const NetworkTimeChangedSignalCallback &callback));

  // Inherited properties from ModemTimeProxyInterface.
  MOCK_METHOD0(NetworkTimezone, const DBusPropertiesMap());

 private:
  DISALLOW_COPY_AND_ASSIGN(MockModemTimeProxy);
};

}  // namespace mm1
}  // namespace shill

#endif  // SHILL_MOCK_MODEM_TIME_PROXY_H_
