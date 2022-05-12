// Copyright (c) 2019 Intel Corporation
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

KERNEL(convolution)(
    __global INPUT0_TYPE* input,
    __global OUTPUT_TYPE* output,
    __global FILTER_TYPE* weights,
#if BIAS_TERM
    __global BIAS_TYPE* biases,
#endif
#if QUANTIZATION_TERM
    __global float* quantizations,
#endif
#if CALIBRATION_TERM
    __global float* calibrations,
#endif
    uint split_idx)
{
    const uint x = get_global_id(0);
#if  OUTPUT_SIZE_Z == 1
    const uint y = get_global_id(1);
    const uint z = 0;
#else
    const uint y = get_global_id(1) % OUTPUT_SIZE_Y;
    const uint z = get_global_id(1) / OUTPUT_SIZE_Y;
#endif
#if OUTPUT_BATCH_NUM == 1
    const uint f = get_global_id(2);
    const uint b = 0;
#else
    const uint f = get_global_id(2) % OUTPUT_FEATURE_NUM;
    const uint b = get_global_id(2) / OUTPUT_FEATURE_NUM;
#endif
#if QUANTIZATION_TERM
    int dotProd = 0;
#else
    UNIT_TYPE dotProd = UNIT_VAL_ZERO;
#endif
    const int input_x = x * STRIDE_SIZE_X - PADDING_SIZE_X;
    const int input_y = y * STRIDE_SIZE_Y - PADDING_SIZE_Y;
    const int input_z = z * STRIDE_SIZE_Z - PADDING_SIZE_Z;

// TODO check DEPTHWISE_SEPARABLE_OPT
#if DEPTHWISE_SEPARABLE_OPT
    const uint in_split_offset = (f / FILTER_OFM_NUM) * INPUT0_FEATURE_PITCH * FILTER_IFM_NUM;
#else
    const uint in_split_offset = split_idx * INPUT0_FEATURE_PITCH * FILTER_IFM_NUM;
#endif
#if GROUPED && !DEPTHWISE_SEPARABLE_OPT
    const uint filter_offset = f*FILTER_OFM_PITCH + split_idx * FILTER_LENGTH;
#else
    const uint filter_offset = f*FILTER_OFM_PITCH;
#endif
    const uint input_offset = b*INPUT0_BATCH_PITCH + INPUT0_OFFSET + in_split_offset;

// TODO check LOCAL_CONVOLUTION
#ifdef LOCAL_CONVOLUTION
    const int local_offset = FILTER_SIZE_X * FILTER_SIZE_Y * (x + OUTPUT_SIZE_X * y);
#endif
    for (uint k = 0; k < FILTER_IFM_NUM; ++k)
    {
        for (uint l = 0; l < FILTER_SIZE_Z ; ++l)
        {
            const int input_offset_z = input_z + l * DILATION_SIZE_Z;
            const bool zero_z = input_offset_z >= INPUT0_SIZE_Z || input_offset_z < 0;

            if(!zero_z)
            {
                for (uint j = 0; j < FILTER_SIZE_Y ; ++j)
                {
                    const int input_offset_y = input_y + j * DILATION_SIZE_Y;
                    const bool zero_y = input_offset_y >= INPUT0_SIZE_Y || input_offset_y < 0;

                    if(!zero_y)
                    {
                        for (uint i = 0; i < FILTER_SIZE_X ; ++i)
                        {
                            const int input_offset_x = input_x + i * DILATION_SIZE_X;
                            const bool zero_x = input_offset_x >= INPUT0_SIZE_X || input_offset_x < 0;

                            if(!zero_x)
                            {
                                uint input_idx = input_offset + (uint)input_offset_x*INPUT0_X_PITCH + (uint)input_offset_y*INPUT0_Y_PITCH +
                                                 (uint)input_offset_z*INPUT0_Z_PITCH + k*INPUT0_FEATURE_PITCH;
#ifdef LOCAL_CONVOLUTION
                                uint filter_idx = filter_offset + k*FILTER_IFM_PITCH + l*FILTER_Z_PITCH + j*FILTER_Y_PITCH + i*FILTER_X_PITCH + local_offset;
#else
                                uint filter_idx = filter_offset + k*FILTER_IFM_PITCH + l*FILTER_Z_PITCH + j*FILTER_Y_PITCH + i*FILTER_X_PITCH;
#endif
#if QUANTIZATION_TERM
                                dotProd += (int)input[input_idx] * (int)weights[filter_idx];
#else
                                dotProd += input[input_idx] * weights[filter_idx];
#endif
                            }
                        }
                    }
                }
            }
        }
    }

#if BIAS_TERM
#if GROUPED && !DEPTHWISE_SEPARABLE_OPT
    const uint bias_offset = split_idx * BIAS_LENGTH;
#else
    const uint bias_offset = 0;
#endif
#if   BIAS_PER_OUTPUT
    const uint bias_index = bias_offset + GET_3D_DATA_INDEX(BIAS, b, f, z, y, x);
#elif BIAS_PER_OFM
    const uint bias_index = bias_offset + f;
#endif
#if QUANTIZATION_TERM
#if CALIBRATION_TERM

    dotProd = (UNIT_TYPE)round(((float)dotProd * quantizations[f] * I_QF + biases[bias_index]) * calibrations[f]);
#else  // CALIBRATION_TERM
    dotProd = (UNIT_TYPE)round(((float)dotProd * quantizations[f] * I_QF + biases[bias_index]) * O_QF);
#endif // CALIBRATION_TERM
#else  // QUANTIZATION_TERM
    dotProd += (UNIT_TYPE)biases[bias_index];
#endif // QUANTIZATION_TERM
#endif

    const uint out_split_offset = split_idx * OUTPUT_FEATURE_PITCH * OUTPUT_FEATURE_NUM;
    const uint dst_index = GET_3D_DATA_INDEX(OUTPUT, b, f, z, y, x) + out_split_offset;

#if QUANTIZATION_TERM
    output[dst_index] = ACTIVATION(convert_char(dotProd), NL_M, NL_N);
#else
    output[dst_index] = ACTIVATION(dotProd, NL_M, NL_N);
#endif
}
