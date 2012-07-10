// Copyright (c) 2012 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "shill/wimax.h"

#include <string>

#include <base/stringprintf.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "shill/event_dispatcher.h"
#include "shill/mock_manager.h"
#include "shill/mock_metrics.h"
#include "shill/mock_wimax_device_proxy.h"
#include "shill/mock_wimax_provider.h"
#include "shill/mock_wimax_service.h"
#include "shill/nice_mock_control.h"
#include "shill/proxy_factory.h"

using base::Bind;
using base::Unretained;
using std::string;
using testing::_;
using testing::NiceMock;
using testing::Return;

namespace shill {

namespace {

const char kTestLinkName[] = "wm0";
const char kTestAddress[] = "01:23:45:67:89:ab";
const int kTestInterfaceIndex = 5;
const char kTestPath[] = "/org/chromium/WiMaxManager/Device/6";

}  // namespace

class WiMaxTest : public testing::Test {
 public:
  WiMaxTest()
      : proxy_(new MockWiMaxDeviceProxy()),
        proxy_factory_(this),
        manager_(&control_, &dispatcher_, &metrics_, NULL),
        device_(new WiMax(&control_, &dispatcher_, &metrics_, &manager_,
                          kTestLinkName, kTestAddress, kTestInterfaceIndex,
                          kTestPath)) {}

  virtual ~WiMaxTest() {}

 protected:
  class TestProxyFactory : public ProxyFactory {
   public:
    explicit TestProxyFactory(WiMaxTest *test) : test_(test) {}

    virtual WiMaxDeviceProxyInterface *CreateWiMaxDeviceProxy(
        const string &/*path*/) {
      return test_->proxy_.release();
    }

   private:
    WiMaxTest *test_;

    DISALLOW_COPY_AND_ASSIGN(TestProxyFactory);
  };

  class Target {
   public:
    virtual ~Target() {}

    MOCK_METHOD1(EnabledStateChanged, void(const Error &error));
  };

  virtual void SetUp() {
    device_->proxy_factory_ = &proxy_factory_;
  }

  virtual void TearDown() {
    device_->SelectService(NULL);
    device_->pending_service_ = NULL;
    device_->proxy_factory_ = NULL;
  }

  scoped_ptr<MockWiMaxDeviceProxy> proxy_;
  TestProxyFactory proxy_factory_;
  NiceMockControl control_;
  EventDispatcher dispatcher_;
  NiceMock<MockMetrics> metrics_;
  MockManager manager_;
  WiMaxRefPtr device_;
};

TEST_F(WiMaxTest, Constructor) {
  EXPECT_EQ(kTestPath, device_->path());
  EXPECT_FALSE(device_->scanning());
}

TEST_F(WiMaxTest, StartStop) {
  EXPECT_FALSE(device_->proxy_.get());
  EXPECT_CALL(*proxy_, Enable(_, _, _));
  EXPECT_CALL(*proxy_, set_networks_changed_callback(_));
  EXPECT_CALL(*proxy_, set_status_changed_callback(_));
  EXPECT_CALL(*proxy_, Disable(_, _, _));
  device_->Start(NULL, EnabledStateChangedCallback());
  ASSERT_TRUE(device_->proxy_.get());

  scoped_refptr<MockWiMaxService> service(
      new MockWiMaxService(&control_, NULL, &metrics_, &manager_));
  device_->pending_service_ = service;
  EXPECT_CALL(*service, SetState(Service::kStateIdle));
  device_->networks_.insert("path");
  MockWiMaxProvider provider;
  EXPECT_CALL(manager_, wimax_provider()).WillOnce(Return(&provider));
  EXPECT_CALL(provider, OnNetworksChanged());
  device_->StartConnectTimeout();
  device_->Stop(NULL, EnabledStateChangedCallback());
  EXPECT_TRUE(device_->networks_.empty());
  EXPECT_FALSE(device_->IsConnectTimeoutStarted());
  EXPECT_FALSE(device_->pending_service_);
}

TEST_F(WiMaxTest, OnServiceStopped) {
  scoped_refptr<NiceMock<MockWiMaxService> > service0(
      new NiceMock<MockWiMaxService>(
          &control_,
          reinterpret_cast<EventDispatcher *>(NULL),
          &metrics_,
          &manager_));
  scoped_refptr<MockWiMaxService> service1(
      new MockWiMaxService(&control_, NULL, &metrics_, &manager_));
  device_->SelectService(service0);
  device_->pending_service_ = service1;

  device_->OnServiceStopped(NULL);
  EXPECT_TRUE(device_->selected_service());
  EXPECT_TRUE(device_->pending_service_);

  device_->OnServiceStopped(service0);
  EXPECT_FALSE(device_->selected_service());
  EXPECT_TRUE(device_->pending_service_);

  device_->OnServiceStopped(service1);
  EXPECT_FALSE(device_->selected_service());
  EXPECT_FALSE(device_->pending_service_);
}

TEST_F(WiMaxTest, OnNetworksChanged) {
  MockWiMaxProvider provider;
  EXPECT_CALL(manager_, wimax_provider()).WillOnce(Return(&provider));
  EXPECT_CALL(provider, OnNetworksChanged());
  device_->networks_.insert("foo");
  RpcIdentifiers networks;
  networks.push_back("bar");
  networks.push_back("zoo");
  networks.push_back("bar");
  device_->OnNetworksChanged(networks);
  EXPECT_EQ(2, device_->networks_.size());
  EXPECT_TRUE(ContainsKey(device_->networks_, "bar"));
  EXPECT_TRUE(ContainsKey(device_->networks_, "zoo"));
}

TEST_F(WiMaxTest, OnConnectComplete) {
  scoped_refptr<MockWiMaxService> service(
      new MockWiMaxService(&control_, NULL, &metrics_, &manager_));
  device_->pending_service_ = service;
  EXPECT_CALL(*service, SetState(_)).Times(0);
  EXPECT_TRUE(device_->pending_service_);
  EXPECT_CALL(*service, SetState(Service::kStateFailure));
  device_->OnConnectComplete(Error(Error::kOperationFailed));
  EXPECT_FALSE(device_->pending_service_);
}

TEST_F(WiMaxTest, OnStatusChanged) {
  scoped_refptr<MockWiMaxService> service(
      new MockWiMaxService(&control_, NULL, &metrics_, &manager_));

  EXPECT_EQ(wimax_manager::kDeviceStatusUninitialized, device_->status_);
  device_->pending_service_ = service;
  EXPECT_CALL(*service, SetState(_)).Times(0);
  EXPECT_CALL(*service, ClearPassphrase()).Times(0);
  device_->OnStatusChanged(wimax_manager::kDeviceStatusScanning);
  EXPECT_TRUE(device_->pending_service_);
  EXPECT_EQ(wimax_manager::kDeviceStatusScanning, device_->status_);

  device_->status_ = wimax_manager::kDeviceStatusConnecting;
  EXPECT_CALL(*service, SetState(Service::kStateFailure));
  EXPECT_CALL(*service, ClearPassphrase());
  device_->OnStatusChanged(wimax_manager::kDeviceStatusScanning);
  EXPECT_FALSE(device_->pending_service_);

  device_->status_ = wimax_manager::kDeviceStatusConnecting;
  device_->SelectService(service);
  EXPECT_CALL(*service, SetState(Service::kStateFailure));
  EXPECT_CALL(*service, SetState(Service::kStateIdle));
  EXPECT_CALL(*service, ClearPassphrase()).Times(0);
  device_->OnStatusChanged(wimax_manager::kDeviceStatusScanning);
  EXPECT_FALSE(device_->selected_service());

  device_->pending_service_ = service;
  device_->SelectService(service);
  EXPECT_CALL(*service, SetState(_)).Times(0);
  EXPECT_CALL(*service, ClearPassphrase()).Times(0);
  device_->OnStatusChanged(wimax_manager::kDeviceStatusConnecting);
  EXPECT_TRUE(device_->pending_service_);
  EXPECT_TRUE(device_->selected_service());
  EXPECT_EQ(wimax_manager::kDeviceStatusConnecting, device_->status_);

  EXPECT_CALL(*service, SetState(Service::kStateIdle));
  device_->SelectService(NULL);
}

TEST_F(WiMaxTest, DropService) {
  scoped_refptr<NiceMock<MockWiMaxService> > service0(
      new NiceMock<MockWiMaxService>(
          &control_,
          reinterpret_cast<EventDispatcher *>(NULL),
          &metrics_,
          &manager_));
  scoped_refptr<MockWiMaxService> service1(
      new MockWiMaxService(&control_, NULL, &metrics_, &manager_));
  device_->SelectService(service0);
  device_->pending_service_ = service1;
  device_->StartConnectTimeout();

  EXPECT_CALL(*service0, SetState(Service::kStateIdle)).Times(2);
  EXPECT_CALL(*service1, SetState(Service::kStateIdle));
  device_->DropService(Service::kStateIdle);
  EXPECT_FALSE(device_->selected_service());
  EXPECT_FALSE(device_->pending_service_);
  EXPECT_FALSE(device_->IsConnectTimeoutStarted());

  // Expect no crash.
  device_->DropService(Service::kStateFailure);
}

TEST_F(WiMaxTest, OnDeviceVanished) {
  device_->proxy_.reset(proxy_.release());
  scoped_refptr<MockWiMaxService> service(
      new MockWiMaxService(&control_, NULL, &metrics_, &manager_));
  device_->pending_service_ = service;
  EXPECT_CALL(*service, SetState(Service::kStateIdle));
  device_->OnDeviceVanished();
  EXPECT_FALSE(device_->proxy_.get());
  EXPECT_FALSE(device_->pending_service_);
}

TEST_F(WiMaxTest, OnEnableComplete) {
  MockWiMaxProvider provider;
  EXPECT_CALL(manager_, wimax_provider()).WillOnce(Return(&provider));
  RpcIdentifiers networks(1, "path");
  EXPECT_CALL(*proxy_, Networks(_)).WillOnce(Return(networks));
  device_->proxy_.reset(proxy_.release());
  EXPECT_CALL(provider, OnNetworksChanged());
  Target target;
  EXPECT_CALL(target, EnabledStateChanged(_));
  EnabledStateChangedCallback callback(
      Bind(&Target::EnabledStateChanged, Unretained(&target)));
  Error error;
  device_->OnEnableComplete(callback, error);
  EXPECT_EQ(1, device_->networks_.size());
  EXPECT_TRUE(ContainsKey(device_->networks_, "path"));

  EXPECT_TRUE(device_->proxy_.get());
  error.Populate(Error::kOperationFailed);
  EXPECT_CALL(target, EnabledStateChanged(_));
  device_->OnEnableComplete(callback, error);
  EXPECT_FALSE(device_->proxy_.get());
}

TEST_F(WiMaxTest, ConnectTimeout) {
  EXPECT_EQ(&dispatcher_, device_->dispatcher());
  EXPECT_TRUE(device_->connect_timeout_callback_.IsCancelled());
  EXPECT_FALSE(device_->IsConnectTimeoutStarted());
  EXPECT_EQ(WiMax::kDefaultConnectTimeoutSeconds,
            device_->connect_timeout_seconds_);
  device_->connect_timeout_seconds_ = 0;
  device_->StartConnectTimeout();
  EXPECT_FALSE(device_->connect_timeout_callback_.IsCancelled());
  EXPECT_TRUE(device_->IsConnectTimeoutStarted());
  device_->dispatcher_ = NULL;
  device_->StartConnectTimeout();  // Expect no crash.
  scoped_refptr<MockWiMaxService> service(
      new MockWiMaxService(&control_, NULL, &metrics_, &manager_));
  device_->pending_service_ = service;
  EXPECT_CALL(*service, SetState(Service::kStateFailure));
  dispatcher_.DispatchPendingEvents();
  EXPECT_TRUE(device_->connect_timeout_callback_.IsCancelled());
  EXPECT_FALSE(device_->IsConnectTimeoutStarted());
  EXPECT_FALSE(device_->pending_service_);
}

TEST_F(WiMaxTest, ConnectTo) {
  static const char kPath[] = "/network/path";
  scoped_refptr<MockWiMaxService> service(
      new MockWiMaxService(&control_, NULL, &metrics_, &manager_));
  EXPECT_CALL(*service, SetState(Service::kStateAssociating));
  device_->status_ = wimax_manager::kDeviceStatusScanning;
  EXPECT_CALL(*service, GetNetworkObjectPath()).WillOnce(Return(kPath));
  EXPECT_CALL(*proxy_, Connect(kPath, _, _, _, _));
  device_->proxy_.reset(proxy_.release());
  Error error;
  device_->ConnectTo(service, &error);
  EXPECT_TRUE(error.IsSuccess());
  EXPECT_EQ(service.get(), device_->pending_service_.get());
  EXPECT_EQ(wimax_manager::kDeviceStatusUninitialized, device_->status_);
  EXPECT_TRUE(device_->IsConnectTimeoutStarted());

  device_->ConnectTo(service, &error);
  EXPECT_EQ(Error::kInProgress, error.type());

  device_->pending_service_ = NULL;
}

TEST_F(WiMaxTest, IsIdle) {
  EXPECT_TRUE(device_->IsIdle());
  scoped_refptr<NiceMock<MockWiMaxService> > service(
      new NiceMock<MockWiMaxService>(
          &control_,
          reinterpret_cast<EventDispatcher *>(NULL),
          &metrics_,
          &manager_));
  device_->pending_service_ = service;
  EXPECT_FALSE(device_->IsIdle());
  device_->pending_service_ = NULL;
  device_->SelectService(service);
  EXPECT_FALSE(device_->IsIdle());
}

}  // namespace shill
