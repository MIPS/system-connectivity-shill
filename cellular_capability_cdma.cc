// Copyright (c) 2012 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "shill/cellular_capability_cdma.h"

#include <string>
#include <vector>

#include <base/bind.h>
#include <base/strings/stringprintf.h>
#include <chromeos/dbus/service_constants.h>
#include <mm/mm-modem.h>

#include "shill/cellular.h"
#include "shill/cellular_service.h"
#include "shill/logging.h"
#include "shill/proxy_factory.h"

using base::Bind;
using std::string;
using std::vector;

namespace shill {

// static
unsigned int CellularCapabilityCDMA::friendly_service_name_id_ = 0;

const char CellularCapabilityCDMA::kPhoneNumber[] = "#777";

CellularCapabilityCDMA::CellularCapabilityCDMA(Cellular *cellular,
                                               ProxyFactory *proxy_factory,
                                               ModemInfo *modem_info)
    : CellularCapabilityClassic(cellular, proxy_factory, modem_info),
      weak_ptr_factory_(this),
      activation_starting_(false),
      activation_state_(MM_MODEM_CDMA_ACTIVATION_STATE_NOT_ACTIVATED),
      registration_state_evdo_(MM_MODEM_CDMA_REGISTRATION_STATE_UNKNOWN),
      registration_state_1x_(MM_MODEM_CDMA_REGISTRATION_STATE_UNKNOWN) {
  SLOG(Cellular, 2) << "Cellular capability constructed: CDMA";
}

CellularCapabilityCDMA::~CellularCapabilityCDMA() {}

void CellularCapabilityCDMA::InitProxies() {
  CellularCapabilityClassic::InitProxies();
  proxy_.reset(proxy_factory()->CreateModemCDMAProxy(
      cellular()->dbus_path(), cellular()->dbus_owner()));
  proxy_->set_signal_quality_callback(
      Bind(&CellularCapabilityCDMA::OnSignalQualitySignal,
           weak_ptr_factory_.GetWeakPtr()));
  proxy_->set_activation_state_callback(
      Bind(&CellularCapabilityCDMA::OnActivationStateChangedSignal,
           weak_ptr_factory_.GetWeakPtr()));
  proxy_->set_registration_state_callback(
      Bind(&CellularCapabilityCDMA::OnRegistrationStateChangedSignal,
           weak_ptr_factory_.GetWeakPtr()));
}

string CellularCapabilityCDMA::GetTypeString() const {
  return kTechnologyFamilyCdma;
}

void CellularCapabilityCDMA::StartModem(Error *error,
                                        const ResultCallback &callback) {
  SLOG(Cellular, 2) << __func__;
  InitProxies();

  CellularTaskList *tasks = new CellularTaskList();
  ResultCallback cb =
      Bind(&CellularCapabilityCDMA::StepCompletedCallback,
           weak_ptr_factory_.GetWeakPtr(), callback, false, tasks);
  if (!cellular()->IsUnderlyingDeviceEnabled())
    tasks->push_back(Bind(&CellularCapabilityCDMA::EnableModem,
                          weak_ptr_factory_.GetWeakPtr(), cb));
  tasks->push_back(Bind(&CellularCapabilityCDMA::GetModemStatus,
                        weak_ptr_factory_.GetWeakPtr(), cb));
  tasks->push_back(Bind(&CellularCapabilityCDMA::GetMEID,
                        weak_ptr_factory_.GetWeakPtr(), cb));
  tasks->push_back(Bind(&CellularCapabilityCDMA::GetModemInfo,
                        weak_ptr_factory_.GetWeakPtr(), cb));
  tasks->push_back(Bind(&CellularCapabilityCDMA::FinishEnable,
                        weak_ptr_factory_.GetWeakPtr(), cb));

  RunNextStep(tasks);
}

void CellularCapabilityCDMA::ReleaseProxies() {
  CellularCapabilityClassic::ReleaseProxies();
  proxy_.reset();
}

bool CellularCapabilityCDMA::AllowRoaming() {
  return allow_roaming_property();
}


void CellularCapabilityCDMA::OnServiceCreated() {
  SLOG(Cellular, 2) << __func__;
  cellular()->service()->SetOLP(olp_);
  cellular()->service()->SetUsageURL(usage_url_);
  UpdateServingOperator();
  HandleNewActivationState(MM_MODEM_CDMA_ACTIVATION_ERROR_NO_ERROR);
}

void CellularCapabilityCDMA::UpdateStatus(const DBusPropertiesMap &properties) {
  string carrier;
  if (DBusProperties::GetString(properties, "carrier", &carrier)) {
    Cellular::Operator oper;
    oper.SetName(carrier);
    oper.SetCountry("us");
    cellular()->set_home_provider(oper);
  }

  uint16 prl_version;
  DBusProperties::GetUint32(
      properties, "activation_state", &activation_state_);
  UpdateOnlinePortal(properties);
  if (DBusProperties::GetUint16(properties, "prl_version", &prl_version))
    cellular()->set_prl_version(prl_version);
  // TODO(petkov): For now, get the payment and usage URLs from ModemManager to
  // match flimflam. In the future, get these from an alternative source (e.g.,
  // database, carrier-specific properties, etc.).
  string payment;
  if (DBusProperties::GetString(properties, "payment_url", &payment)) {
    olp_.SetURL(payment);
  }
  if (DBusProperties::GetString(properties, "payment_url_method", &payment)) {
    olp_.SetMethod(payment);
  }
  if (DBusProperties::GetString(properties, "payment_url_postdata", &payment)) {
    olp_.SetPostData(payment);
  }
  DBusProperties::GetString(properties, "usage_url", &usage_url_);
}

void CellularCapabilityCDMA::SetupConnectProperties(
    DBusPropertiesMap *properties) {
  (*properties)[kConnectPropertyPhoneNumber].writer().append_string(
      kPhoneNumber);
}

void CellularCapabilityCDMA::Activate(const string &carrier,
                                      Error *error,
                                      const ResultCallback &callback) {
  SLOG(Cellular, 2) << __func__ << "(" << carrier << ")";
  // We're going to trigger something which leads to an activation.
  activation_starting_ = true;
  if (cellular()->state() == Cellular::kStateEnabled ||
      cellular()->state() == Cellular::kStateRegistered) {
    ActivationResultCallback activation_callback =
        Bind(&CellularCapabilityCDMA::OnActivateReply,
             weak_ptr_factory_.GetWeakPtr(),
             callback);
    proxy_->Activate(carrier, error, activation_callback, kTimeoutActivate);
  } else if (cellular()->state() == Cellular::kStateConnected ||
             cellular()->state() == Cellular::kStateLinked) {
    pending_activation_callback_ = callback;
    pending_activation_carrier_ = carrier;
    cellular()->Disconnect(error);
  } else {
    Error::PopulateAndLog(error, Error::kInvalidArguments,
                          "Unable to activate in " +
                          Cellular::GetStateString(cellular()->state()));
    activation_starting_ = false;
  }
}

void CellularCapabilityCDMA::HandleNewActivationState(uint32 error) {
  SLOG(Cellular, 2) << __func__ << "(" << error << ")";
  if (!cellular()->service().get()) {
    LOG(ERROR) << "In " << __func__ << "(): service is null.";
    return;
  }
  cellular()->service()->SetActivationState(
      GetActivationStateString(activation_state_));
  cellular()->service()->set_error(GetActivationErrorString(error));
}

void CellularCapabilityCDMA::DisconnectCleanup() {
  CellularCapabilityClassic::DisconnectCleanup();
  if (pending_activation_callback_.is_null()) {
    return;
  }
  if (cellular()->state() == Cellular::kStateEnabled ||
      cellular()->state() == Cellular::kStateRegistered) {
    Error ignored_error;
    Activate(pending_activation_carrier_,
             &ignored_error,
             pending_activation_callback_);
  } else {
    Error error;
    Error::PopulateAndLog(
        &error,
        Error::kOperationFailed,
        "Tried to disconnect before activating cellular service and failed");
    HandleNewActivationState(MM_MODEM_CDMA_ACTIVATION_ERROR_UNKNOWN);
    activation_starting_ = false;
    pending_activation_callback_.Run(error);
  }
  pending_activation_callback_.Reset();
  pending_activation_carrier_.clear();
}

// static
string CellularCapabilityCDMA::GetActivationStateString(uint32 state) {
  switch (state) {
    case MM_MODEM_CDMA_ACTIVATION_STATE_ACTIVATED:
      return kActivationStateActivated;
    case MM_MODEM_CDMA_ACTIVATION_STATE_ACTIVATING:
      return kActivationStateActivating;
    case MM_MODEM_CDMA_ACTIVATION_STATE_NOT_ACTIVATED:
      return kActivationStateNotActivated;
    case MM_MODEM_CDMA_ACTIVATION_STATE_PARTIALLY_ACTIVATED:
      return kActivationStatePartiallyActivated;
    default:
      return kActivationStateUnknown;
  }
}

// static
string CellularCapabilityCDMA::GetActivationErrorString(uint32 error) {
  switch (error) {
    case MM_MODEM_CDMA_ACTIVATION_ERROR_WRONG_RADIO_INTERFACE:
      return kErrorNeedEvdo;
    case MM_MODEM_CDMA_ACTIVATION_ERROR_ROAMING:
      return kErrorNeedHomeNetwork;
    case MM_MODEM_CDMA_ACTIVATION_ERROR_COULD_NOT_CONNECT:
    case MM_MODEM_CDMA_ACTIVATION_ERROR_SECURITY_AUTHENTICATION_FAILED:
    case MM_MODEM_CDMA_ACTIVATION_ERROR_PROVISIONING_FAILED:
      return kErrorOtaspFailed;
    case MM_MODEM_CDMA_ACTIVATION_ERROR_NO_ERROR:
      return "";
    case MM_MODEM_CDMA_ACTIVATION_ERROR_NO_SIGNAL:
    default:
      return kErrorActivationFailed;
  }
}

void CellularCapabilityCDMA::GetMEID(const ResultCallback &callback) {
  SLOG(Cellular, 2) << __func__;
  if (cellular()->meid().empty()) {
    // TODO(petkov): Switch to asynchronous calls (crbug.com/200687).
    cellular()->set_meid(proxy_->MEID());
    SLOG(Cellular, 2) << "MEID: " << cellular()->meid();
  }
  callback.Run(Error());
}

void CellularCapabilityCDMA::GetProperties(const ResultCallback &callback) {
  SLOG(Cellular, 2) << __func__;
  // No properties.
  callback.Run(Error());
}

bool CellularCapabilityCDMA::IsActivating() const {
  return activation_starting_ ||
      activation_state_ == MM_MODEM_CDMA_ACTIVATION_STATE_ACTIVATING;
}

bool CellularCapabilityCDMA::IsRegistered() const {
  return registration_state_evdo_ != MM_MODEM_CDMA_REGISTRATION_STATE_UNKNOWN ||
      registration_state_1x_ != MM_MODEM_CDMA_REGISTRATION_STATE_UNKNOWN;
}

void CellularCapabilityCDMA::SetUnregistered(bool searching) {
  registration_state_evdo_ = MM_MODEM_CDMA_REGISTRATION_STATE_UNKNOWN;
  registration_state_1x_ = MM_MODEM_CDMA_REGISTRATION_STATE_UNKNOWN;
}

string CellularCapabilityCDMA::GetNetworkTechnologyString() const {
  if (registration_state_evdo_ != MM_MODEM_CDMA_REGISTRATION_STATE_UNKNOWN) {
    return kNetworkTechnologyEvdo;
  }
  if (registration_state_1x_ != MM_MODEM_CDMA_REGISTRATION_STATE_UNKNOWN) {
    return kNetworkTechnology1Xrtt;
  }
  return "";
}

string CellularCapabilityCDMA::GetRoamingStateString() const {
  uint32 state = registration_state_evdo_;
  if (state == MM_MODEM_CDMA_REGISTRATION_STATE_UNKNOWN) {
    state = registration_state_1x_;
  }
  switch (state) {
    case MM_MODEM_CDMA_REGISTRATION_STATE_UNKNOWN:
    case MM_MODEM_CDMA_REGISTRATION_STATE_REGISTERED:
      break;
    case MM_MODEM_CDMA_REGISTRATION_STATE_HOME:
      return kRoamingStateHome;
    case MM_MODEM_CDMA_REGISTRATION_STATE_ROAMING:
      return kRoamingStateRoaming;
    default:
      NOTREACHED();
  }
  return kRoamingStateUnknown;
}

void CellularCapabilityCDMA::GetSignalQuality() {
  SLOG(Cellular, 2) << __func__;
  SignalQualityCallback callback =
      Bind(&CellularCapabilityCDMA::OnGetSignalQualityReply,
           weak_ptr_factory_.GetWeakPtr());
  proxy_->GetSignalQuality(NULL, callback, kTimeoutDefault);
}

void CellularCapabilityCDMA::GetRegistrationState() {
  SLOG(Cellular, 2) << __func__;
  RegistrationStateCallback callback =
      Bind(&CellularCapabilityCDMA::OnGetRegistrationStateReply,
           weak_ptr_factory_.GetWeakPtr());
  proxy_->GetRegistrationState(NULL, callback, kTimeoutDefault);
}

string CellularCapabilityCDMA::CreateFriendlyServiceName() {
  SLOG(Cellular, 2) << __func__;
  if (!cellular()->carrier().empty()) {
    return cellular()->carrier();
  }
  return base::StringPrintf("CDMANetwork%u", friendly_service_name_id_++);
}

void CellularCapabilityCDMA::UpdateServingOperator() {
  SLOG(Cellular, 2) << __func__;
  if (cellular()->service().get()) {
    cellular()->service()->SetServingOperator(cellular()->home_provider());
  }
}

void CellularCapabilityCDMA::OnActivateReply(
    const ResultCallback &callback, uint32 status, const Error &error) {
  activation_starting_ = false;
  if (error.IsSuccess()) {
    if (status == MM_MODEM_CDMA_ACTIVATION_ERROR_NO_ERROR) {
      activation_state_ = MM_MODEM_CDMA_ACTIVATION_STATE_ACTIVATING;
    } else {
      LOG(WARNING) << "Modem activation failed with status: "
                   << GetActivationErrorString(status) << " (" << status << ")";
    }
    HandleNewActivationState(status);
  } else {
    LOG(ERROR) << "Activate() failed with error: " << error;
  }
  callback.Run(error);
}

void CellularCapabilityCDMA::OnGetRegistrationStateReply(
    uint32 state_1x, uint32 state_evdo, const Error &error) {
  SLOG(Cellular, 2) << __func__;
  if (error.IsSuccess())
    OnRegistrationStateChangedSignal(state_1x, state_evdo);
}

void CellularCapabilityCDMA::OnGetSignalQualityReply(uint32 quality,
                                                     const Error &error) {
  if (error.IsSuccess())
    OnSignalQualitySignal(quality);
}

void CellularCapabilityCDMA::OnActivationStateChangedSignal(
    uint32 activation_state,
    uint32 activation_error,
    const DBusPropertiesMap &status_changes) {
  SLOG(Cellular, 2) << __func__;
  string prop_value;

  if (DBusProperties::GetString(status_changes, "mdn", &prop_value))
    cellular()->set_mdn(prop_value);
  if (DBusProperties::GetString(status_changes, "min", &prop_value))
    cellular()->set_min(prop_value);

  string payment;
  if (DBusProperties::GetString(status_changes, "payment_url", &payment)) {
    olp_.SetURL(payment);
  }
  if (DBusProperties::GetString(
          status_changes, "payment_url_method", &payment)) {
    olp_.SetMethod(payment);
  }
  if (DBusProperties::GetString(
          status_changes, "payment_url_postdata", &payment)) {
    olp_.SetPostData(payment);
  }
  if (cellular()->service().get()) {
    cellular()->service()->SetOLP(olp_);
  }

  UpdateOnlinePortal(status_changes);
  activation_state_ = activation_state;
  HandleNewActivationState(activation_error);
}

void CellularCapabilityCDMA::OnRegistrationStateChangedSignal(
    uint32 state_1x, uint32 state_evdo) {
  SLOG(Cellular, 2) << __func__;
  registration_state_1x_ = state_1x;
  registration_state_evdo_ = state_evdo;
  cellular()->HandleNewRegistrationState();
}

void CellularCapabilityCDMA::OnSignalQualitySignal(uint32 strength) {
  cellular()->HandleNewSignalQuality(strength);
}

void CellularCapabilityCDMA::UpdateOnlinePortal(
    const DBusPropertiesMap &properties) {
  // Treat the three updates atomically: Only update the serving operator when
  // all three are known:
  string olp_url, olp_method, olp_post_data;
  if (DBusProperties::GetString(properties, "payment_url", &olp_url) &&
      DBusProperties::GetString(properties,
                                "payment_url_method", &olp_method) &&
      DBusProperties::GetString(properties,
                                "payment_url_postdata",
                                &olp_post_data)) {
    cellular()->serving_operator_info()->UpdateOnlinePortal(olp_url,
                                                            olp_method,
                                                            olp_post_data);
  }
}

}  // namespace shill
