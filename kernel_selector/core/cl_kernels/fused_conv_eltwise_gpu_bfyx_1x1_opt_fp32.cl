// Copyright (c) 2016-2017 Intel Corporation
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

#include "include/include_all.cl"

#define SIMD_SIZE 8
__attribute__((intel_reqd_sub_group_size(SIMD_SIZE)))
KERNEL(fused_conv_eltwise_gpu_bfyx_1x1_opt)(
    __global INPUT0_TYPE* input, 
    __global OUTPUT_TYPE* output, 
    __global FILTER_TYPE* weights, 
#if BIAS_TERM
    __global BIAS_TYPE* biases,
#endif
    uint split_idx,
    const __global float* src3)
{
   const uint group_x = get_group_id(0) * OUT_BLOCK_WIDTH;
    const uint group_y = get_group_id(1) * OUT_BLOCK_HEIGHT;
    const uint f = (get_group_id(2) * SIMD_SIZE * OUT_BLOCK_DEPTH) % OUTPUT_FEATURE_NUM;
    const uint b = (get_group_id(2) * SIMD_SIZE * OUT_BLOCK_DEPTH) / OUTPUT_FEATURE_NUM;;

    const uint ifm_part = get_sub_group_id();
    uint ifm_offset = ifm_part* OUT_BLOCK_DEPTH/2;

    UNIT_TYPE in[OUT_BLOCK_HEIGHT];
    UNIT_TYPE dotProd0[OUT_BLOCK_WIDTH * OUT_BLOCK_HEIGHT * OUT_BLOCK_DEPTH/2];
    UNIT_TYPE dotProd1[OUT_BLOCK_WIDTH * OUT_BLOCK_HEIGHT * OUT_BLOCK_DEPTH/2];

    for(uint i = 0; i < OUT_BLOCK_WIDTH * OUT_BLOCK_HEIGHT * OUT_BLOCK_DEPTH/2; i++)
    {
        dotProd0[i] = 0;
        dotProd1[i] = 0;
    }

#if OUT_BLOCK_DEPTH == 8
    const uint filter_offset = f * FILTER_IFM_NUM + ifm_part*(64 * FILTER_IFM_NUM/2);
#elif OUT_BLOCK_DEPTH == 4
    const uint filter_offset = f * FILTER_IFM_NUM + ifm_part*(32 * FILTER_IFM_NUM/2);
#elif OUT_BLOCK_DEPTH == 2
    const uint filter_offset = f * FILTER_IFM_NUM + ifm_part*(16 * FILTER_IFM_NUM/2);
#else
    const uint filter_offset = f*FILTER_OFM_PITCH + ifm_part*(FILTER_IFM_NUM/2) * FILTER_IFM_PITCH;
#endif
    const uint input_offset = b*INPUT0_BATCH_PITCH + INPUT0_OFFSET + group_x * INPUT0_X_PITCH + group_y * INPUT0_Y_PITCH + ifm_part*(FILTER_IFM_NUM/2) * INPUT0_FEATURE_PITCH;

    //--------------------------------------------------------------------
    // main computation phase
    //--------------------------------------------------------------------

    for (uint k = 0; k < FILTER_IFM_NUM/2; ++k)
    {
        for(uint i = 0; i < OUT_BLOCK_HEIGHT; i++)
        {
            const uint in_offset = input_offset + get_local_id(get_group_id(0)) + i * INPUT0_Y_PITCH + k * INPUT0_FEATURE_PITCH;
            in[i] = input[in_offset];
        }

#if OUT_BLOCK_DEPTH == 8
        float8 w = as_float8(intel_sub_group_block_read8((__global uint*)weights + filter_offset + k * 64));
#elif OUT_BLOCK_DEPTH == 4
        float4 w = as_float4(intel_sub_group_block_read4((__global uint*)weights + filter_offset + k * 32));
#elif OUT_BLOCK_DEPTH == 2
        float2 w = as_float2(intel_sub_group_block_read2((__global uint*)weights + filter_offset + k * 16));
#endif

        for(uint br = 0; br < OUT_BLOCK_HEIGHT; br++)
        {
            for(uint bc = 0; bc < OUT_BLOCK_WIDTH; bc++)
            {
                float _in = intel_sub_group_shuffle(in[br], bc);
                for(uint bd = 0; bd < OUT_BLOCK_DEPTH/2; bd++)
                {
                    dotProd0[bc + OUT_BLOCK_WIDTH * (br + OUT_BLOCK_HEIGHT * bd)] += _in * w[bd];
                    dotProd1[bc + OUT_BLOCK_WIDTH * (br + OUT_BLOCK_HEIGHT * bd)] += _in * w[bd + OUT_BLOCK_DEPTH/2];
                }
            }
        }
    }

    __local float slm_vals[OUT_BLOCK_WIDTH * OUT_BLOCK_HEIGHT * OUT_BLOCK_DEPTH * SIMD_SIZE];

    //--------------------------------------------------------------------
    // second sub_group in workgroup task
    //--------------------------------------------------------------------
    
    if(ifm_part == 1)
    {
        for(uint bd = 0; bd < OUT_BLOCK_DEPTH/2; bd++)
        {
            for(uint br = 0; br < OUT_BLOCK_HEIGHT; br++)
            {
                for(uint bc = 0; bc < OUT_BLOCK_WIDTH; bc++)
                {
                    slm_vals[SIMD_SIZE * (bc + OUT_BLOCK_WIDTH * (br + OUT_BLOCK_HEIGHT * bd)) + get_local_id(get_group_id(0))] = dotProd0[bc + OUT_BLOCK_WIDTH * (br + OUT_BLOCK_HEIGHT * bd)];
                    dotProd0[bc + OUT_BLOCK_WIDTH * (br + OUT_BLOCK_HEIGHT * bd)] = dotProd1[bc + OUT_BLOCK_WIDTH * (br + OUT_BLOCK_HEIGHT * bd)];
                }
            }
        }

    }

    //--------------------------------------------------------------------
    // first sub_group in workgroup task
    //--------------------------------------------------------------------
    
    if(ifm_part == 0)
    {
        for(uint bd = 0; bd < OUT_BLOCK_DEPTH/2; bd++)
        {
            for(uint br = 0; br < OUT_BLOCK_HEIGHT; br++)
            {
                for(uint bc = 0; bc < OUT_BLOCK_WIDTH; bc++)
                {
                    slm_vals[SIMD_SIZE * (bc + OUT_BLOCK_WIDTH * (br + OUT_BLOCK_HEIGHT * (bd+OUT_BLOCK_DEPTH/2) )) + get_local_id(get_group_id(0))] = dotProd1[bc + OUT_BLOCK_WIDTH * (br + OUT_BLOCK_HEIGHT * bd)];
                }
            }
        }

    }

    //--------------------------------------------------------------------
    // add bias phase
    //--------------------------------------------------------------------
    
    #if BIAS_TERM
    for(uint bd = 0; bd < OUT_BLOCK_DEPTH/2; bd++)
    {
        float _bias = biases[f + (bd + ifm_offset) * SIMD_SIZE + get_local_id(get_group_id(0))];
        for(uint br = 0; br < OUT_BLOCK_HEIGHT; br++)
        {
            for(uint bc = 0; bc < OUT_BLOCK_WIDTH; bc++)
            {
                dotProd0[bc + OUT_BLOCK_WIDTH * (br + OUT_BLOCK_HEIGHT * bd)] += _bias;
            }
        }
    }
    #endif

    barrier(CLK_LOCAL_MEM_FENCE); // we want to add barrier after biases addition so that the long slm write part latency is shadowed by it

    //--------------------------------------------------------------------
    // sum sub-group results + activation phase
    //--------------------------------------------------------------------
    
    for(uint bd = 0; bd < OUT_BLOCK_DEPTH/2; bd++)
    {
        for(uint br = 0; br < OUT_BLOCK_HEIGHT; br++)
        {
            for(uint bc = 0; bc < OUT_BLOCK_WIDTH; bc++)
            {
                dotProd0[bc + OUT_BLOCK_WIDTH * (br + OUT_BLOCK_HEIGHT * bd)] += slm_vals[SIMD_SIZE * (bc + OUT_BLOCK_WIDTH * (br + OUT_BLOCK_HEIGHT * (bd + ifm_offset) )) + get_local_id(get_group_id(0))];
                dotProd0[bc + OUT_BLOCK_WIDTH * (br + OUT_BLOCK_HEIGHT * bd)] = ACTIVATION(dotProd0[bc + OUT_BLOCK_WIDTH * (br + OUT_BLOCK_HEIGHT * bd)], NL_M, NL_N);;
            }
        }
    }

    //--------------------------------------------------------------------
    // eltwise with eltwise activation phase
    //--------------------------------------------------------------------
    #if IN_OUT_OPT != 1
    for(uint bd = 0; bd < OUT_BLOCK_DEPTH/2; bd++)
    {
        for(uint br = 0; br < OUT_BLOCK_HEIGHT; br++)
        {
            for(uint bc = 0; bc < OUT_BLOCK_WIDTH; bc++)
            {
                uint src3_offset = GET_DATA_INDEX(INPUT1, b, f + (bd + ifm_offset) * SIMD_SIZE + get_local_id(get_group_id(0)), (group_y + br) * ELTW_STRIDE_Y, (group_x + bc) * ELTW_STRIDE_X);
                dotProd0[bc + OUT_BLOCK_WIDTH * (br + OUT_BLOCK_HEIGHT * bd)] += src3[src3_offset];
                dotProd0[bc + OUT_BLOCK_WIDTH * (br + OUT_BLOCK_HEIGHT * bd)] = ACTIVATION_ELTW(dotProd0[bc + OUT_BLOCK_WIDTH * (br + OUT_BLOCK_HEIGHT * bd)], NL_M_ELTW, NL_N_ELTW);
            }
        }
    }
    #endif

    //--------------------------------------------------------------------
    // output phase
    //--------------------------------------------------------------------

    for(uint bd = 0; bd < OUT_BLOCK_DEPTH/2; bd++)
    {
        for(uint br = 0; br < OUT_BLOCK_HEIGHT; br++)
        {
            uint dst_index = GET_DATA_INDEX(OUTPUT, b, f + (bd + ifm_offset) * SIMD_SIZE + get_local_id(get_group_id(0)), group_y + br, group_x);
            uint out_vstore_offset = 0;
            #if (OUT_BLOCK_WIDTH >= 8)
            {
                float8 tmp = (float8)(dotProd0[out_vstore_offset + 0 + OUT_BLOCK_WIDTH * (br + OUT_BLOCK_HEIGHT * bd)],
                                      dotProd0[out_vstore_offset + 1 + OUT_BLOCK_WIDTH * (br + OUT_BLOCK_HEIGHT * bd)],
                                      dotProd0[out_vstore_offset + 2 + OUT_BLOCK_WIDTH * (br + OUT_BLOCK_HEIGHT * bd)],
                                      dotProd0[out_vstore_offset + 3 + OUT_BLOCK_WIDTH * (br + OUT_BLOCK_HEIGHT * bd)],
                                      dotProd0[out_vstore_offset + 4 + OUT_BLOCK_WIDTH * (br + OUT_BLOCK_HEIGHT * bd)],
                                      dotProd0[out_vstore_offset + 5 + OUT_BLOCK_WIDTH * (br + OUT_BLOCK_HEIGHT * bd)],
                                      dotProd0[out_vstore_offset + 6 + OUT_BLOCK_WIDTH * (br + OUT_BLOCK_HEIGHT * bd)],
                                      dotProd0[out_vstore_offset + 7 + OUT_BLOCK_WIDTH * (br + OUT_BLOCK_HEIGHT * bd)]);
#if IN_OUT_OPT == 1
                float8 tmp2 = vload8(0, output + dst_index + out_vstore_offset * OUTPUT_X_PITCH);
                tmp += tmp2;
                tmp = ACTIVATION_ELTW(tmp, NL_M_ELTW, NL_N_ELTW);
#endif
                vstore8(tmp, 0, output + dst_index + out_vstore_offset * OUTPUT_X_PITCH);
                out_vstore_offset += 8;
            }
            #endif
            #if (OUT_BLOCK_WIDTH % 8) > 3
            {
                float4 tmp = (float4)(dotProd0[out_vstore_offset + 0 + OUT_BLOCK_WIDTH * (br + OUT_BLOCK_HEIGHT * bd)],
                                      dotProd0[out_vstore_offset + 1 + OUT_BLOCK_WIDTH * (br + OUT_BLOCK_HEIGHT * bd)],
                                      dotProd0[out_vstore_offset + 2 + OUT_BLOCK_WIDTH * (br + OUT_BLOCK_HEIGHT * bd)],
                                      dotProd0[out_vstore_offset + 3 + OUT_BLOCK_WIDTH * (br + OUT_BLOCK_HEIGHT * bd)]);
#if IN_OUT_OPT == 1
                float4 tmp2 = vload4(0, output + dst_index + out_vstore_offset * OUTPUT_X_PITCH);
                tmp += tmp2;
                tmp = ACTIVATION_ELTW(tmp, NL_M_ELTW, NL_N_ELTW);
#endif
                vstore4(tmp, 0, output + dst_index + out_vstore_offset * OUTPUT_X_PITCH);
                out_vstore_offset += 4;
            }
            #endif
            #if (OUT_BLOCK_WIDTH % 4) > 1
            {
                float2 tmp = (float2)(dotProd0[out_vstore_offset + OUT_BLOCK_WIDTH * (br + OUT_BLOCK_HEIGHT * bd)],
                                       dotProd0[out_vstore_offset+1 + OUT_BLOCK_WIDTH * (br + OUT_BLOCK_HEIGHT * bd)]);
#if IN_OUT_OPT == 1
                float2 tmp2 = vload2(0, output + dst_index + out_vstore_offset * OUTPUT_X_PITCH);
                tmp += tmp2;
                tmp = ACTIVATION_ELTW(tmp, NL_M_ELTW, NL_N_ELTW);
#endif
                vstore2(tmp, 0, output + dst_index + out_vstore_offset * OUTPUT_X_PITCH);
                out_vstore_offset += 2;
            }
            #endif
            for(uint bc = out_vstore_offset; bc < OUT_BLOCK_WIDTH; bc++)
            {
#if IN_OUT_OPT == 1
                dotProd0[bc + OUT_BLOCK_WIDTH * (br + OUT_BLOCK_HEIGHT * bd)] += output[dst_index + bc * OUTPUT_X_PITCH];
                dotProd0[bc + OUT_BLOCK_WIDTH * (br + OUT_BLOCK_HEIGHT * bd)] = ACTIVATION_ELTW(dotProd0[bc + OUT_BLOCK_WIDTH * (br + OUT_BLOCK_HEIGHT * bd)], NL_M_ELTW, NL_N_ELTW);
#endif                
                output[dst_index + bc * OUTPUT_X_PITCH] = dotProd0[bc + OUT_BLOCK_WIDTH * (br + OUT_BLOCK_HEIGHT * bd)];
            }
        }
    }
}
