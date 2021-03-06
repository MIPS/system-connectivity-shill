//
// Copyright (C) 2013 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#ifndef SHILL_MOCK_SOCKET_INFO_READER_H_
#define SHILL_MOCK_SOCKET_INFO_READER_H_

#include <vector>

#include <base/macros.h>
#include <gmock/gmock.h>

#include "shill/socket_info_reader.h"

namespace shill {

class SocketInfo;

class MockSocketInfoReader : public SocketInfoReader {
 public:
  MockSocketInfoReader();
  ~MockSocketInfoReader() override;

  MOCK_METHOD1(LoadTcpSocketInfo, bool(std::vector<SocketInfo>* info_list));

 private:
  DISALLOW_COPY_AND_ASSIGN(MockSocketInfoReader);
};

}  // namespace shill

#endif  // SHILL_MOCK_SOCKET_INFO_READER_H_
