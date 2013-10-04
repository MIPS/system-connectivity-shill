// Copyright (c) 2013 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SHILL_NO_OUT_OF_CREDITS_DETECTOR_H_
#define SHILL_NO_OUT_OF_CREDITS_DETECTOR_H_

#include <base/basictypes.h>

#include "shill/out_of_credits_detector.h"

namespace shill {

// This object performs no out-of-credits detection.
class NoOutOfCreditsDetector : public OutOfCreditsDetector {
 public:
  NoOutOfCreditsDetector(EventDispatcher *dispatcher,
                         Manager *manager,
                         Metrics *metrics,
                         CellularService *service)
      : OutOfCreditsDetector(dispatcher, manager, metrics, service) {}
  virtual ~NoOutOfCreditsDetector() {}

  // Resets the detector state.
  virtual void ResetDetector() override {}
  // Returns |true| if this object is busy detecting out-of-credits.
  virtual bool IsDetecting() const override { return false; }
  // Notifies this object of a service state change.
  virtual void NotifyServiceStateChanged(
      Service::ConnectState old_state,
      Service::ConnectState new_state) override {}
  // Notifies this object when the subscription state has changed.
  virtual void NotifySubscriptionStateChanged(
      uint32 subscription_state) override {}

 private:
  DISALLOW_COPY_AND_ASSIGN(NoOutOfCreditsDetector);
};

}  // namespace shill

#endif  // SHILL_NO_OUT_OF_CREDITS_DETECTOR_H_
