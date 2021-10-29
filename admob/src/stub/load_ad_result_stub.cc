/*
 * Copyright 2021 Google LLC
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <string>

#include "admob/src/include/firebase/admob.h"
#include "admob/src/include/firebase/admob/types.h"

namespace firebase {
namespace admob {

struct LoadAdResultInternal {
  char stub;
};

LoadAdResult::LoadAdResult() : AdResult(), response_info_() {}

LoadAdResult::LoadAdResult(const LoadAdResultInternal& load_ad_result_internal)
    : AdResult() {}

LoadAdResult::LoadAdResult(const LoadAdResult& load_ad_result) : AdResult() {}

}  // namespace admob
}  // namespace firebase