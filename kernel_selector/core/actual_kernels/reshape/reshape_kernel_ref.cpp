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

#include "reshape_kernel_ref.h"
#include "kernel_selector_utils.h" 
 
namespace kernel_selector 
{
    ParamsKey ReshapeKernelRef::GetSupportedKey() const
    {
        ParamsKey k;
        k.EnableInputDataType(Datatype::F16);
        k.EnableInputDataType(Datatype::F32);
        k.EnableInputDataType(Datatype::INT8);
        k.EnableInputDataType(Datatype::INT32);
        k.EnableInputDataType(Datatype::INT64);
        k.EnableOutputDataType(Datatype::F16);
        k.EnableOutputDataType(Datatype::F32);
        k.EnableOutputDataType(Datatype::INT8);
        k.EnableOutputDataType(Datatype::INT32);
        k.EnableOutputDataType(Datatype::INT64);
        k.EnableAllInputLayout();
        k.EnableAllOutputLayout();
        k.EnableTensorOffset();
        k.EnableTensorPitches();
        k.EnableBatching();
        return k;
    }

    KernelsData ReshapeKernelRef::GetKernelsData(const Params& params, const optional_params& options) const
    {
        assert(params.GetType() == KernelType::RESHAPE);

        KernelData kd = KernelData::Default<reshape_params>(params);
        reshape_params& newParams = *static_cast<reshape_params*>(kd.params.get());

        auto entry_point = GetEntryPoint(kernelName, newParams.layerID, options);
        auto cldnn_jit = MakeBaseParamsJitConstants(newParams);
        std::string jit = CreateJit(kernelName, cldnn_jit, entry_point);

        const auto& in = newParams.inputs[0];
        auto& kernel = kd.kernels[0];
        std::vector<size_t> gws;
        for (const auto& o : in.GetDims())
        {
            gws.push_back(o.v);
        }
        
        for (size_t i = gws.size(); i < 4; i++)
        {
            gws.push_back(1U);
        }

        if (gws.size() == 4)
            kernel.workGroups.global = { gws[0], gws[1], gws[2] * gws[3] };
        else
            kernel.workGroups.global = { gws[0], gws[1], gws[2] * gws[3] * gws[4] };
        kernel.workGroups.local = GetOptimalLocalWorkGroupSizes(kernel.workGroups.global);
        kernel.kernelString = GetKernelString(kernelName, jit, entry_point, params.engineInfo, DEFAULT);
        kernel.arguments = GetArgsDesc(1, false, false);
        
        kd.estimatedTime = DONT_USE_IF_HAVE_SOMETHING_ELSE;

        return{ kd };
    }
}