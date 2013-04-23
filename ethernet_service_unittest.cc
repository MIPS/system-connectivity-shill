// Copyright (c) 2012 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "shill/ethernet_service.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "shill/mock_adaptors.h"
#include "shill/mock_ethernet.h"
#include "shill/mock_manager.h"
#include "shill/property_store_unittest.h"
#include "shill/refptr_types.h"
#include "shill/service_property_change_test.h"

using ::testing::NiceMock;

namespace shill {

class EthernetServiceTest : public PropertyStoreTest {
 public:
  EthernetServiceTest()
      : mock_manager_(control_interface(), dispatcher(), metrics(), glib()),
        ethernet_(
            new NiceMock<MockEthernet>(control_interface(),
                                       dispatcher(),
                                       metrics(),
                                       &mock_manager_,
                                       "ethernet",
                                       fake_mac,
                                       0)),
        service_(
            new EthernetService(control_interface(),
                                dispatcher(),
                                metrics(),
                                &mock_manager_,
                                ethernet_)) {}
  virtual ~EthernetServiceTest() {}

 protected:
  static const char fake_mac[];

  bool GetAutoConnect() {
    return service_->GetAutoConnect(NULL);
  }

  bool SetAutoConnect(const bool connect, Error *error) {
    return service_->SetAutoConnectFull(connect, error);
  }

  ServiceMockAdaptor *GetAdaptor() {
    return dynamic_cast<ServiceMockAdaptor *>(service_->adaptor());
  }

  MockManager mock_manager_;
  scoped_refptr<MockEthernet> ethernet_;
  EthernetServiceRefPtr service_;
};

// static
const char EthernetServiceTest::fake_mac[] = "AaBBcCDDeeFF";

TEST_F(EthernetServiceTest, AutoConnect) {
  EXPECT_TRUE(service_->IsAutoConnectByDefault());
  EXPECT_TRUE(GetAutoConnect());
  {
    Error error;
    SetAutoConnect(false, &error);
    EXPECT_FALSE(error.IsSuccess());
  }
  EXPECT_TRUE(GetAutoConnect());
  {
    Error error;
    SetAutoConnect(true, &error);
    EXPECT_TRUE(error.IsSuccess());
  }
  EXPECT_TRUE(GetAutoConnect());
}

TEST_F(EthernetServiceTest, ConnectDisconnectDelegation) {
  EXPECT_CALL(*ethernet_, ConnectTo(service_.get()));
  service_->AutoConnect();
  EXPECT_CALL(*ethernet_, DisconnectFrom(service_.get()));
  Error error;
  service_->Disconnect(&error);
}

TEST_F(EthernetServiceTest, PropertyChanges) {
  TestCommonPropertyChanges(service_, GetAdaptor());
}

// Custom property setters should return false, and make no changes, if
// the new value is the same as the old value.
TEST_F(EthernetServiceTest, CustomSetterNoopChange) {
  TestCustomSetterNoopChange(service_, &mock_manager_);
}

}  // namespace shill
