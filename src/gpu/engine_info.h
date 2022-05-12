﻿/*
// Copyright (c) 2016 Intel Corporation
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
*/
#pragma once
#include <cstdint>
#include <memory>
#include "api/CPP/engine.hpp"
#include "document.h"


namespace cldnn {
    namespace gpu {

class gpu_toolkit;
struct engine_info_internal : cldnn::engine_info
{
    std::string dev_id;
    std::string driver_version;
    std::uint32_t compute_units_count;
    std::shared_ptr<rapidjson::Document> device_cache; 

private:
    friend class gpu_toolkit;
    explicit engine_info_internal(const gpu_toolkit& context);
};

}}
