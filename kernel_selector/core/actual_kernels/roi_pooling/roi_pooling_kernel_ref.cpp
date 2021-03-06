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

#include "roi_pooling_kernel_ref.h"

namespace kernel_selector {

    ParamsKey ROIPoolingKernelRef::GetSupportedKey() const
    {
        ParamsKey k;
        k.EnableInputDataType(Datatype::F16);
        k.EnableInputDataType(Datatype::F32);
        k.EnableOutputDataType(Datatype::F16);
        k.EnableOutputDataType(Datatype::F32);
        k.EnableInputLayout(DataLayout::bfyx);
        k.EnableOutputLayout(DataLayout::brfyx);
        k.EnablePoolType(PoolType::MAX);
        k.EnablePoolType(PoolType::AVG);
        k.EnablePoolType(PoolType::BILINEAR);
        k.EnableTensorOffset();
        k.EnableTensorPitches();
        k.EnableBatching();
        k.EnableDifferentTypes();
        return k;
    }

    static ROIPoolingKernelRef::DispatchData SetDefault(const roi_pooling_params& params)
    {
        ROIPoolingKernelRef::DispatchData kd;

        kd.fp16UnitUsed = (params.inputs[0].GetDType() == Datatype::F16);

        // Determine global work sizes.
        kd.gws0 = params.output.LogicalSize();
        kd.gws1 = 1;
        kd.gws2 = 1;

        // Find largest positive local work size that is divider for global work size.
        kd.lws0 = std::min(std::max(kd.gws0, static_cast<size_t>(1)), static_cast<size_t>(32));
        while (kd.gws0 % kd.lws0 != 0)
        {
            --kd.lws0;
        }
        kd.lws1 = 1;
        kd.lws2 = 1;

        return kd;
    }

    JitConstants ROIPoolingKernelRef::GetJitConstants(const roi_pooling_params& rp) const
    {
        JitConstants jit = MakeBaseParamsJitConstants(rp);

        jit.AddConstants({
            MakeJitConstant("POOLED_HEIGHT",     rp.pooledHeight),
            MakeJitConstant("POOLED_WIDTH",      rp.pooledWidth),
            MakeJitConstant("SPATIAL_SCALE",     rp.spatialScale),
            MakeJitConstant("GROUP_SIZE",        rp.groupSize),
            MakeJitConstant(toString(rp.mode) + "_POOLING", 1),
        });

        jit.AddConstants({
            MakeJitConstant("USE_OLD_SCALE_AND_ROUNDING",   rp.groupSize == 0)
        });

        return jit;
    }

    KernelsData ROIPoolingKernelRef::GetKernelsData(const Params& params, const optional_params& options) const
    {
        assert(params.GetType() == KernelType::ROI_POOLING);
        const roi_pooling_params& orgParams = static_cast<const roi_pooling_params&>(params);

        if (orgParams.activation.function != ActivationFunction::NONE)
        {
            return{};
        }

        DispatchData runInfo = SetDefault(orgParams);
        KernelData kd = KernelData::Default<roi_pooling_params>(params);

        auto cldnn_jit = GetJitConstants(orgParams);
        auto entry_point = GetEntryPoint(kernelName, orgParams.layerID, options);
        auto jit = CreateJit(kernelName, cldnn_jit, entry_point);

        auto& kernel = kd.kernels[0];
        FillCLKernelData(kernel, runInfo, params.engineInfo, kernelName, jit, entry_point);
        kernel.arguments.push_back({ ArgumentDescriptor::Types::INPUT, 1 });

        kd.estimatedTime = FORCE_PRIORITY_9;

        return{ kd };
    }
}