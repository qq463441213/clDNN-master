/*
// Copyright (c) 2018 Intel Corporation
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
    class EltwiseKernel_fs_bs_yx_bsv4_fsv32 : public EltwiseKernelBase
    {
    public:
        EltwiseKernel_fs_bs_yx_bsv4_fsv32() : EltwiseKernelBase("eltwise_fs_bs_yx_bsv4_fsv32") {}
        virtual ~EltwiseKernel_fs_bs_yx_bsv4_fsv32() {}

        virtual KernelsData GetKernelsData(const Params& params, const optional_params& options) const override;
        virtual ParamsKey GetSupportedKey() const override;
    protected:
        JitConstants GetJitConstants(const eltwise_params& params) const override;
        virtual DispatchData SetDefault(const eltwise_params& params) const override;
    };
}