/*
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

#include "eltwise_kernel_base.h"

namespace kernel_selector
{
    class EltwiseKernelRef : public EltwiseKernelBase
    {
    public:
        EltwiseKernelRef() : EltwiseKernelBase("generic_eltwise_ref") {}
        virtual ~EltwiseKernelRef() {}

        virtual KernelsData GetKernelsData(const Params& params, const optional_params& options) const override;
        virtual ParamsKey GetSupportedKey() const override;
    protected:
        bool Validate(const Params& p, const optional_params& o) const override;

    };
}