// SPDX-License-Identifier: AGPL-3.0-or-later
// Internal builtins (not installed): implemented in fileio.cpp, wired in
// Context's constructor.
#pragma once
#include "rkvc/registry.hpp"

namespace rkvc {

void register_builtin_fileio(Registry& registry);

}  // namespace rkvc
