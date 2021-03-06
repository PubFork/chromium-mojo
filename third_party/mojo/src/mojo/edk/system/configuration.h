// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef THIRD_PARTY_MOJO_SRC_MOJO_EDK_SYSTEM_CONFIGURATION_H_
#define THIRD_PARTY_MOJO_SRC_MOJO_EDK_SYSTEM_CONFIGURATION_H_

#include "third_party/mojo/src/mojo/edk/embedder/configuration.h"
#include "third_party/mojo/src/mojo/edk/system/system_impl_export.h"

namespace mojo {
namespace system {

namespace internal {
MOJO_SYSTEM_IMPL_EXPORT extern embedder::Configuration g_configuration;
}  // namespace internal

MOJO_SYSTEM_IMPL_EXPORT inline const embedder::Configuration&
GetConfiguration() {
  return internal::g_configuration;
}

MOJO_SYSTEM_IMPL_EXPORT inline embedder::Configuration*
GetMutableConfiguration() {
  return &internal::g_configuration;
}

}  // namespace system
}  // namespace mojo

#endif  // THIRD_PARTY_MOJO_SRC_MOJO_EDK_SYSTEM_CONFIGURATION_H_
