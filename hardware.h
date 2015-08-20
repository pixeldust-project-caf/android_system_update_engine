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

#ifndef UPDATE_ENGINE_HARDWARE_H_
#define UPDATE_ENGINE_HARDWARE_H_

#include <string>
#include <vector>

#include <base/macros.h>

#include "update_engine/hardware_interface.h"

namespace chromeos_update_engine {

// Implements the real interface with the hardware.
class Hardware : public HardwareInterface {
 public:
  Hardware();
  ~Hardware() override;

  // HardwareInterface methods.
  std::string BootKernelDevice() const override;
  std::string BootDevice() const override;
  bool IsBootDeviceRemovable() const override;
  std::vector<std::string> GetKernelDevices() const override;
  bool IsKernelBootable(const std::string& kernel_device,
                        bool* bootable) const override;
  bool MarkKernelUnbootable(const std::string& kernel_device) override;
  bool IsOfficialBuild() const override;
  bool IsNormalBootMode() const override;
  bool IsOOBEComplete(base::Time* out_time_of_oobe) const override;
  std::string GetHardwareClass() const override;
  std::string GetFirmwareVersion() const override;
  std::string GetECVersion() const override;
  int GetPowerwashCount() const override;

 private:
  DISALLOW_COPY_AND_ASSIGN(Hardware);
};

}  // namespace chromeos_update_engine

#endif  // UPDATE_ENGINE_HARDWARE_H_
