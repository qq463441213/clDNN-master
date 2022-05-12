﻿/*
// Copyright (c) 2016-2019 Intel Corporation
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

///////////////////////////////////////////////////////////////////////////////////////////////////

#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include "api/CPP/memory.hpp"
#include <api/CPP/input_layout.hpp>
#include "api/CPP/convolution.hpp"
#include <api/CPP/topology.hpp>
#include <api/CPP/network.hpp>
#include <api/CPP/engine.hpp>
#include "test_utils/test_utils.h"
#include "test_utils/float16.h"
#include <api/CPP/data.hpp>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <iomanip>
#include <thread>
#include <fstream>
#include <api/CPP/reorder.hpp>

using namespace cldnn;
using namespace tests;


namespace cldnn
{
    template<> struct type_to_data_type<FLOAT16> { static const data_types value = data_types::f16; };
}



template<typename T>
T kahan_summation(std::vector<T> &input) {
    T sum = 0;
    T c = 0;
    for (T x : input) {
        T y = x - c;
        T t = sum + y;
        c = (t - sum) - y;
        sum = t;
    }
    return sum;
}

template<typename T>
VVF<T> reference_convolve(VVVF<T> &input, VVVF<T> &filter, int stride_y, int stride_x, float bias, int dilation_y = 1, int dilation_x = 1,
        int input_padding_y = 0, int input_padding_x = 0, int output_padding_y = 0, 
        int output_padding_x = 0, size_t f_begin = 0)
{
    size_t kernel_extent_y = dilation_y * (filter[0].size() - 1) + 1;
    size_t kernel_extent_x = dilation_x * (filter[0][0].size() - 1) + 1;
    size_t output_y = 1 + (input[0].size() - kernel_extent_y + 2 * input_padding_y) / stride_y + 2 * output_padding_y;
    size_t output_x = 1 + (input[0][0].size() - kernel_extent_x + 2 * input_padding_x) / stride_x + 2 * output_padding_x;
    VVF<T> output(output_y, VF<T>(output_x, 0));
    for (size_t f = 0; f < filter.size(); ++f) {
        for (size_t y = 0; y < (output_y - 2 * output_padding_y); ++y) {
            for (size_t x = 0; x < (output_x - 2 * output_padding_x); ++x) {
                VF<T> values;
                values.reserve(filter[0].size() * filter[0][0].size());
                for (size_t yf = 0; yf < filter[0].size(); ++yf) {
                    int yi = -input_padding_y + (int)yf * dilation_y + stride_y * (int)y;
                    if (yi < 0 || (int)input[0].size() <= yi) continue;
                    for (size_t xf = 0; xf < filter[0][0].size(); ++xf) {
                        int xi = -input_padding_x + (int)xf * dilation_x + stride_x * (int)x;
                        if (xi < 0 || (int)input[0][0].size() <= xi) continue;
                        values.push_back(input[f_begin + f][yi][xi] * filter[f][yf][xf]);
                    }
                }
                output[y + output_padding_y][x + output_padding_x] += kahan_summation<T>(values);
            }
        }
    }

    for (size_t y = 0; y < (output_y - 2 * output_padding_y); ++y) {
        for (size_t x = 0; x < (output_x - 2 * output_padding_x); ++x) {
            output[y + output_padding_y][x + output_padding_x] += static_cast<T>(bias);
        }
    }
    return output;
}

void dump_buffer(memory const& mem, std::string const& name)
{
    std::ofstream out(name);
    auto size = mem.get_layout().get_buffer_size();
    auto ptr = mem.pointer<const float>();
    auto pitches = mem.get_layout().get_pitches();
    out << "Data size: " << mem.get_layout().size << "\n";
    out << "Lower padding: " << mem.get_layout().data_padding.lower_size() << "\n";
    out << "Upper padding: " << mem.get_layout().data_padding.upper_size() << "\n";
    out << "\n";

    for (int b = 0; b < size.batch[0]; ++b)
    {
        out << " ================ BATCH " << b << " =================\n\n";
        for (int f = 0; f < size.feature[0]; ++f)
        {
            out << "feature " << f << ":\n";
            for (int y = 0; y < size.spatial[1]; ++y)
            {
                for (int x = 0; x < size.spatial[0]; ++x)
                {
                    size_t idx = b * pitches.batch[0] + f * pitches.feature[0] + y * pitches.spatial[1] + x * pitches.spatial[0];
                    out << ptr[idx] << " ";
                }
                out << "\n";
            }

            out << "\n";
        }

        out << "\n";
    }
}


TEST(convolution_f32_fw_gpu, basic_convolution_no_bias) {
    //  Filter : 2x3
    //  Stride : 2x1
    //  Input  : 4x5
    //  Output : 2x3
    //
    //  Input:
    //  1  2  3  4  5
    //  2  2  3  4  6
    //  3  3  3  5  1
    //  1  1  1  1  1
    //
    //  Filter:
    //  1  2  1
    //  2  1  2
    //
    //  Output:
    // 21  28  39
    // 18  20  20

    const auto& engine = get_test_engine();

    auto input = memory::allocate(engine, { data_types::f32,format::yxfb,{ 1, 1, 5, 4 } });
    auto weights = memory::allocate(engine, { data_types::f32,format::bfyx,{ 1, 1, 3, 2 } });

    set_values(input, { 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 2.0f, 2.0f, 3.0f, 4.0f, 6.0f, 3.0f, 3.0f, 3.0f, 5.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f });
    set_values(weights, { 1.0f, 2.0f, 1.0f, 2.0f, 1.0f, 2.0f });
    VVF<float> output_vec = {
        { 20.0f, 27.0f, 38.0f },
        { 17.0f, 19.0f, 19.0f } };

    topology topology(
        input_layout("input", input.get_layout()),
        data("weights", weights),
        convolution("conv", "input", { "weights" }, { 1,1,1,2 }));

    network network(engine, topology);
    network.set_input_data("input", input);

    auto outputs = network.execute();
    EXPECT_EQ(outputs.size(), size_t(1));
    EXPECT_EQ(outputs.begin()->first, "conv");

    auto output_memory = outputs.at("conv").get_memory();
    auto output_layout = output_memory.get_layout();
    auto output_ptr = output_memory.pointer<float>();

    int y_size = output_layout.size.spatial[1];
    int x_size = output_layout.size.spatial[0];
    int f_size = output_layout.size.feature[0];
    int b_size = output_layout.size.batch[0];
    EXPECT_EQ(output_layout.format, format::yxfb);
    EXPECT_EQ(y_size, 2);
    EXPECT_EQ(x_size, 3);
    EXPECT_EQ(f_size, 1);
    EXPECT_EQ(b_size, 1);
    for (int y = 0; y < y_size; ++y) {
        for (int x = 0; x < x_size; ++x) {
            EXPECT_EQ(output_vec[y][x], output_ptr[y * x_size + x]);
        }
    }

    //VVF temp_vec(y_size, VF(x_size, 0.0f));
    //for (int y = 0; y < y_size; ++y) {
    //    for (int x = 0; x < x_size; ++x) {
    //        temp_vec[y][x] = output_ptr[y * x_size + x];
    //    }
    //}
    //print_2d(temp_vec);
}

TEST(convolution_f32_fw_gpu, basic_convolution_int8_no_bias) {
    //  Filter : 2x3
    //  Stride : 2x1
    //  Input  : 4x5
    //  Output : 2x3
    //
    //  Input:
    //  1  2  3  4  5
    //  2  2  3  4  6
    //  3  3  3  5  1
    //  1  1  1  1  1
    //
    //  Filter:
    //  1  2  1
    //  2  1  2
    //
    //  Output:
    // 21  28  39
    // 18  20  20

    const auto& engine = get_test_engine();

    auto input = memory::allocate(engine, { data_types::f32,format::bfyx,{ 1, 1, 5, 4 } });
    auto weights = memory::allocate(engine, { data_types::i8,format::bfyx,{ 1, 1, 3, 2 } });

    set_values(input, { 1.1f, 2.4f, 3.5f, 4.5f, 5.8f, 2.9f, 2.3f, 3.5f, 4.4f, 6.6f, 3.8f, 3.9f, 3.4f, 5.1f, 1.4f, 1.8f, 1.1f, 1.2f, 1.2f, 1.9f });
    set_values<char>(weights, { 1, 2, 1, 2, 1, 2 });
    VVF<float> output_vec = {
        { 20.0f, 27.0f, 38.0f },
        { 17.0f, 19.0f, 19.0f } };

    topology topology(
        input_layout("input", input.get_layout()),
        reorder("to_int","input", { data_types::i8,format::bfyx,{ 1, 1, 5, 4 } }),
        data("weights", weights),
        convolution("conv", "to_int", { "weights" }, { 1,1,1,2 }),
        reorder("output", "conv", { data_types::f32,format::bfyx,{ 1, 1, 3, 2 } }));

    network network(engine, topology);
    network.set_input_data("input", input);

    auto outputs = network.execute();
    EXPECT_EQ(outputs.size(), size_t(1));
    EXPECT_EQ(outputs.begin()->first, "output");

    auto output_memory = outputs.at("output").get_memory();
    auto output_layout = output_memory.get_layout();
    auto output_ptr = output_memory.pointer<float>();

    int y_size = output_layout.size.spatial[1];
    int x_size = output_layout.size.spatial[0];
    int f_size = output_layout.size.feature[0];
    int b_size = output_layout.size.batch[0];
    EXPECT_EQ(output_layout.format, format::bfyx);
    EXPECT_EQ(y_size, 2);
    EXPECT_EQ(x_size, 3);
    EXPECT_EQ(f_size, 1);
    EXPECT_EQ(b_size, 1);
    for (int y = 0; y < y_size; ++y) {
        for (int x = 0; x < x_size; ++x) {
            EXPECT_EQ(output_vec[y][x], output_ptr[y * x_size + x]);
        }
    }
}

TEST(convolution_f32_fw_gpu, basic_convolution3D_no_bias) {
    //  data is similar as in basic_convolution_no_bias

    //  Filter : 2x3x1
    //  Stride : 2x1x1
    //  Input  : 4x5x1
    //  Output : 2x3x1

    const auto& engine = get_test_engine();

    auto input = memory::allocate(engine, { data_types::f32, format::bfyx, { 1, 1, 5, 4 } });
    auto weights = memory::allocate(engine, { data_types::f32, format::bfyx, { 1, 1, 3, 2 } });

    set_values(input, { 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 2.0f, 2.0f, 3.0f, 4.0f, 6.0f, 3.0f, 3.0f, 3.0f, 5.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f });
    set_values(weights, { 1.0f, 2.0f, 1.0f, 2.0f, 1.0f, 2.0f });

    VVF<float> output_vec = {
        { 20.0f, 27.0f, 38.0f },
        { 17.0f, 19.0f, 19.0f } };

    topology topology(
        input_layout("input", input.get_layout()),
        data("weights", weights),
        convolution("conv", "input", { "weights" }, { 1,1,1,2 }));

    network network(engine, topology);
    network.set_input_data("input", input);

    auto outputs = network.execute();
    EXPECT_EQ(outputs.size(), size_t(1));
    EXPECT_EQ(outputs.begin()->first, "conv");

    auto output_memory = outputs.at("conv").get_memory();
    auto output_layout = output_memory.get_layout();
    auto output_ptr = output_memory.pointer<float>();

    int z_size = output_layout.size.spatial[2];
    int y_size = output_layout.size.spatial[1];
    int x_size = output_layout.size.spatial[0];
    int f_size = output_layout.size.feature[0];
    int b_size = output_layout.size.batch[0];
    EXPECT_EQ(output_layout.format, format::bfyx);
    EXPECT_EQ(z_size, 1);
    EXPECT_EQ(y_size, 2);
    EXPECT_EQ(x_size, 3);
    EXPECT_EQ(f_size, 1);
    EXPECT_EQ(b_size, 1);
    for (int y = 0; y < y_size; ++y) {
        for (int x = 0; x < x_size; ++x) {
            EXPECT_EQ(output_vec[y][x], output_ptr[y * x_size + x]);
        }
    }
}

TEST(convolution_f32_fw_gpu, basic_convolution3D) {
    //  Input  : 4x4x4
    //  Filter : 2x2x2
    //  Output : 3x3x3
    //
    //  Input:
    //  1  0  1  0
    //  1  1  3  1
    //  1  1  0  2
    //  0  2  1  1
    //
    //  1  0  0  1
    //  2  0  1  2
    //  3  1  1  1
    //  0  0  3  1
    //
    //  2  0  1  1
    //  3  3  1  0
    //  2  1  1  0
    //  3  2  1  2
    //
    //  1  0  2  0
    //  1  0  3  3
    //  3  1  0  0
    //  1  1  0  2
    //
    //  Filter:
    //  0  1
    //  0  0
    //
    //  2  1
    //  0  0

    //  Output:
    //  2  1  1
    //  5  4  5
    //  8  3  5
    //
    //  4  1  4
    //  9  8  4
    //  6  4  3
    //
    //  2  3  5
    //  5  4  9
    //  8  3  0
    //
    //  Bias:
    //  1

    const auto& engine = get_test_engine();

    auto input = memory::allocate(engine, { data_types::f32, format::bfzyx,{ 1, 1, 4, 4, 4 } });
    auto weights = memory::allocate(engine, { data_types::f32, format::bfzyx,{ 1, 1, 2, 2, 2 } });
    auto biases = memory::allocate(engine, { data_types::f32, format::bfyx,{ 1, 1, 1, 1, 1 } });

    set_values(input, {
        1.0f,  0.0f,  1.0f,  0.0f,
        1.0f,  1.0f,  3.0f,  1.0f,
        1.0f,  1.0f,  0.0f,  2.0f,
        0.0f,  2.0f,  1.0f,  1.0f,
        1.0f,  0.0f,  0.0f,  1.0f,
        2.0f,  0.0f,  1.0f,  2.0f,
        3.0f,  1.0f,  1.0f,  1.0f,
        0.0f,  0.0f,  3.0f,  1.0f,
        2.0f,  0.0f,  1.0f,  1.0f,
        3.0f,  3.0f,  1.0f,  0.0f,
        2.0f,  1.0f,  1.0f,  0.0f,
        3.0f,  2.0f,  1.0f,  2.0f,
        1.0f,  0.0f,  2.0f,  0.0f,
        1.0f,  0.0f,  3.0f,  3.0f,
        3.0f,  1.0f,  0.0f,  0.0f,
        1.0f,  1.0f,  0.0f,  2.0f,
    });

    set_values(weights, {
        0.0f,  1.0f,
        0.0f,  0.0f,
        2.0f,  1.0f,
        0.0f,  0.0f,
    });

    set_values(biases, { 1.0f });

    VVVF<float> output_vec = {
        {
            { 3.0f,   2.0f,   2.0f },
            { 6.0f,   5.0f,   6.0f },
            { 9.0f,   4.0f,   6.0f }
        },
        {
            { 5.0f,   2.0f,   5.0f },
            { 10.0f,   9.0f,   5.0f },
            { 7.0f,   5.0f,   4.0f }
        },
        {
            { 3.0f,   4.0f,   6.0f },
            { 6.0f,   5.0f,   10.0f },
            { 9.0f,   4.0f,   1.0f }
        }
    };

    topology topology(
        input_layout("input", input.get_layout()),
        data("weights", weights),
        data("biases", biases),
        convolution("conv", "input", { "weights" }, { "biases" }));

    network network(engine, topology);
    network.set_input_data("input", input);

    auto outputs = network.execute();
    EXPECT_EQ(outputs.size(), size_t(1));
    EXPECT_EQ(outputs.begin()->first, "conv");

    auto output_memory = outputs.at("conv").get_memory();
    auto output_layout = output_memory.get_layout();
    auto output_ptr = output_memory.pointer<float>();

    int z_size = output_layout.size.spatial[2];
    int y_size = output_layout.size.spatial[1];
    int x_size = output_layout.size.spatial[0];
    int f_size = output_layout.size.feature[0];
    int b_size = output_layout.size.batch[0];
    EXPECT_EQ(output_layout.format, format::bfzyx);
    EXPECT_EQ(z_size, 3);
    EXPECT_EQ(y_size, 3);
    EXPECT_EQ(x_size, 3);

    EXPECT_EQ(f_size, 1);
    EXPECT_EQ(b_size, 1);
    for (int z = 0; z < z_size; ++z) {
        for (int y = 0; y < y_size; ++y) {
            for (int x = 0; x < x_size; ++x) {
                EXPECT_EQ(output_vec[z][y][x], output_ptr[z * y_size * x_size + y * x_size + x]);
            }
        }
    }
}

TEST(convolution_f32_fw_gpu, basic_convolution3D_split2) {
    //  data is similar as in basic_convolution3D
    const auto& engine = get_test_engine();
    auto input = memory::allocate(engine, { data_types::f32, format::bfzyx,{ 1, 2, 4, 4, 4 } });
    auto weights_1 = memory::allocate(engine, { data_types::f32, format::bfzyx,{ 1, 1, 2, 2, 2 } });
    auto weights_2 = memory::allocate(engine, { data_types::f32, format::bfzyx,{ 1, 1, 2, 2, 2 } });
    auto biases_1 = memory::allocate(engine, { data_types::f32, format::bfyx,{ 1, 1, 1, 1, 1 } });
    auto biases_2 = memory::allocate(engine, { data_types::f32, format::bfyx,{ 1, 1, 1, 1, 1 } });

    set_values(input, {
        1.0f,  0.0f,  1.0f,  0.0f,
        1.0f,  1.0f,  3.0f,  1.0f,
        1.0f,  1.0f,  0.0f,  2.0f,
        0.0f,  2.0f,  1.0f,  1.0f,
        1.0f,  0.0f,  0.0f,  1.0f,
        2.0f,  0.0f,  1.0f,  2.0f,
        3.0f,  1.0f,  1.0f,  1.0f,
        0.0f,  0.0f,  3.0f,  1.0f,
        2.0f,  0.0f,  1.0f,  1.0f,
        3.0f,  3.0f,  1.0f,  0.0f,
        2.0f,  1.0f,  1.0f,  0.0f,
        3.0f,  2.0f,  1.0f,  2.0f,
        1.0f,  0.0f,  2.0f,  0.0f,
        1.0f,  0.0f,  3.0f,  3.0f,
        3.0f,  1.0f,  0.0f,  0.0f,
        1.0f,  1.0f,  0.0f,  2.0f,
        1.0f,  0.0f,  1.0f,  0.0f,
        1.0f,  1.0f,  3.0f,  1.0f,
        1.0f,  1.0f,  0.0f,  2.0f,
        0.0f,  2.0f,  1.0f,  1.0f,
        1.0f,  0.0f,  0.0f,  1.0f,
        2.0f,  0.0f,  1.0f,  2.0f,
        3.0f,  1.0f,  1.0f,  1.0f,
        0.0f,  0.0f,  3.0f,  1.0f,
        2.0f,  0.0f,  1.0f,  1.0f,
        3.0f,  3.0f,  1.0f,  0.0f,
        2.0f,  1.0f,  1.0f,  0.0f,
        3.0f,  2.0f,  1.0f,  2.0f,
        1.0f,  0.0f,  2.0f,  0.0f,
        1.0f,  0.0f,  3.0f,  3.0f,
        3.0f,  1.0f,  0.0f,  0.0f,
        1.0f,  1.0f,  0.0f,  2.0f,
    });

    set_values(weights_1, {
        0.0f,  1.0f,
        0.0f,  0.0f,
        2.0f,  1.0f,
        0.0f,  0.0f,
    });

    set_values(weights_2, {
        0.0f,  1.0f,
        0.0f,  0.0f,
        2.0f,  1.0f,
        0.0f,  0.0f,
    });

    set_values(biases_1, { 1.0f });
    set_values(biases_2, { 2.0f });

    VVVVF<float> output_vec = {
        {
            {
                { 3.0f,   2.0f,   2.0f },
                { 6.0f,   5.0f,   6.0f },
                { 9.0f,   4.0f,   6.0f }
            },
            {
                { 5.0f,   2.0f,   5.0f },
                { 10.0f,   9.0f,   5.0f },
                { 7.0f,   5.0f,   4.0f }
            },
            {
                { 3.0f,   4.0f,   6.0f },
                { 6.0f,   5.0f,   10.0f},
                { 9.0f,   4.0f,   1.0f }
            },
        },
        {
            {
                { 4.0f,   3.0f,   3.0f },
                { 7.0f,   6.0f,   7.0f },
                { 10.0f,  5.0f,   7.0f }
            },
            {
                { 6.0f,   3.0f,   6.0f },
                { 11.0f,  10.0f,  6.0f },
                { 8.0f,   6.0f,   5.0f }
            },
            {
                { 4.0f,   5.0f,   7.0f },
                { 7.0f,   6.0f,  11.0f },
                { 10.0f,  5.0f,   2.0f }
            },
        }
    };

    topology topology(
        input_layout("input", input.get_layout()),
        data("weights_1", weights_1),
        data("biases_1", biases_1),
        data("weights_2", weights_2),
        data("biases_2", biases_2),
        convolution("conv", "input", { "weights_1",  "weights_2" }, { "biases_1",  "biases_2" }));

    network network(engine, topology);
    network.set_input_data("input", input);

    auto outputs = network.execute();
    EXPECT_EQ(outputs.size(), size_t(1));
    EXPECT_EQ(outputs.begin()->first, "conv");

    auto output_memory = outputs.at("conv").get_memory();
    auto output_layout = output_memory.get_layout();
    auto output_ptr = output_memory.pointer<float>();

    int z_size = output_layout.size.spatial[2];
    int y_size = output_layout.size.spatial[1];
    int x_size = output_layout.size.spatial[0];
    int f_size = output_layout.size.feature[0];
    int b_size = output_layout.size.batch[0];
    EXPECT_EQ(output_layout.format, format::bfzyx);
    EXPECT_EQ(z_size, 3);
    EXPECT_EQ(y_size, 3);
    EXPECT_EQ(x_size, 3);
    EXPECT_EQ(b_size, 1);
    EXPECT_EQ(f_size, 2);
    for (int f = 0; f < f_size; ++f) {
        for (int z = 0; z < z_size; ++z) {
            for (int y = 0; y < y_size; ++y) {
                for (int x = 0; x < x_size; ++x) {
                    int i = f * z_size * y_size * x_size + z * y_size * x_size + y * x_size + x;
                    EXPECT_EQ(output_vec[f][z][y][x],
                        output_ptr[f * z_size * y_size * x_size + z * y_size * x_size + y * x_size + x]);
                }
            }
        }
    }
}

TEST(convolution_f32_fw_gpu, basic_convolution3D_group2) {
    //  data is similar as in basic_convolution3D_split2
    const auto& engine = get_test_engine();
    auto input = memory::allocate(engine, { data_types::f32, format::bfzyx,{ 1, 2, 4, 4, 4 } });
    auto weights = memory::allocate(engine, { data_types::f32, format::bfzyx,{ 2, 1, 2, 2, 2 } });
    auto biases = memory::allocate(engine, { data_types::f32, format::bfyx,{ 1, 1, 2, 1, 1 } });

    set_values(input, {
        1.0f,  0.0f,  1.0f,  0.0f,
        1.0f,  1.0f,  3.0f,  1.0f,
        1.0f,  1.0f,  0.0f,  2.0f,
        0.0f,  2.0f,  1.0f,  1.0f,
        1.0f,  0.0f,  0.0f,  1.0f,
        2.0f,  0.0f,  1.0f,  2.0f,
        3.0f,  1.0f,  1.0f,  1.0f,
        0.0f,  0.0f,  3.0f,  1.0f,
        2.0f,  0.0f,  1.0f,  1.0f,
        3.0f,  3.0f,  1.0f,  0.0f,
        2.0f,  1.0f,  1.0f,  0.0f,
        3.0f,  2.0f,  1.0f,  2.0f,
        1.0f,  0.0f,  2.0f,  0.0f,
        1.0f,  0.0f,  3.0f,  3.0f,
        3.0f,  1.0f,  0.0f,  0.0f,
        1.0f,  1.0f,  0.0f,  2.0f,
        1.0f,  0.0f,  1.0f,  0.0f,
        1.0f,  1.0f,  3.0f,  1.0f,
        1.0f,  1.0f,  0.0f,  2.0f,
        0.0f,  2.0f,  1.0f,  1.0f,
        1.0f,  0.0f,  0.0f,  1.0f,
        2.0f,  0.0f,  1.0f,  2.0f,
        3.0f,  1.0f,  1.0f,  1.0f,
        0.0f,  0.0f,  3.0f,  1.0f,
        2.0f,  0.0f,  1.0f,  1.0f,
        3.0f,  3.0f,  1.0f,  0.0f,
        2.0f,  1.0f,  1.0f,  0.0f,
        3.0f,  2.0f,  1.0f,  2.0f,
        1.0f,  0.0f,  2.0f,  0.0f,
        1.0f,  0.0f,  3.0f,  3.0f,
        3.0f,  1.0f,  0.0f,  0.0f,
        1.0f,  1.0f,  0.0f,  2.0f,
    });

    set_values(weights, {
        0.0f,  1.0f,
        0.0f,  0.0f,
        2.0f,  1.0f,
        0.0f,  0.0f,
        0.0f,  1.0f,
        0.0f,  0.0f,
        2.0f,  1.0f,
        0.0f,  0.0f,
    });

    set_values(biases, { 1.0f, 2.0f });

    VVVVF<float> output_vec = {
        {
            {
                { 3.0f,   2.0f,   2.0f },
                { 6.0f,   5.0f,   6.0f },
                { 9.0f,   4.0f,   6.0f }
            },
            {
                { 5.0f,   2.0f,   5.0f },
                { 10.0f,   9.0f,   5.0f },
                { 7.0f,   5.0f,   4.0f }
            },
            {
                { 3.0f,   4.0f,   6.0f },
                { 6.0f,   5.0f,   10.0f },
                { 9.0f,   4.0f,   1.0f }
            },
        },
        {
            {
                { 4.0f,   3.0f,   3.0f },
                { 7.0f,   6.0f,   7.0f },
                { 10.0f,  5.0f,   7.0f }
            },
            {
                { 6.0f,   3.0f,   6.0f },
                { 11.0f,  10.0f,  6.0f },
                { 8.0f,   6.0f,   5.0f }
            },
            {
                { 4.0f,   5.0f,   7.0f },
                { 7.0f,   6.0f,  11.0f },
                { 10.0f,  5.0f,   2.0f }
            },
        }
    };

    topology topology(
        input_layout("input", input.get_layout()),
        data("weights", weights),
        data("biases", biases),
        convolution("conv", "input", { "weights" }, { "biases" }));

    network network(engine, topology);
    network.set_input_data("input", input);

    auto outputs = network.execute();
    EXPECT_EQ(outputs.size(), size_t(1));
    EXPECT_EQ(outputs.begin()->first, "conv");

    auto output_memory = outputs.at("conv").get_memory();
    auto output_layout = output_memory.get_layout();
    auto output_ptr = output_memory.pointer<float>();

    int z_size = output_layout.size.spatial[2];
    int y_size = output_layout.size.spatial[1];
    int x_size = output_layout.size.spatial[0];
    int f_size = output_layout.size.feature[0];
    int b_size = output_layout.size.batch[0];
    EXPECT_EQ(output_layout.format, format::bfzyx);
    EXPECT_EQ(b_size, 1);
    EXPECT_EQ(f_size, 2);
    EXPECT_EQ(z_size, 3);
    EXPECT_EQ(y_size, 3);
    EXPECT_EQ(x_size, 3);
    for (int f = 0; f < f_size; ++f) {
        for (int z = 0; z < z_size; ++z) {
            for (int y = 0; y < y_size; ++y) {
                for (int x = 0; x < x_size; ++x) {
                    int i = f * z_size * y_size * x_size + z * y_size * x_size + y * x_size + x;
                    EXPECT_EQ(output_vec[f][z][y][x],
                        output_ptr[f * z_size * y_size * x_size + z * y_size * x_size + y * x_size + x]);
                }
            }
        }
    }
}

TEST(convolution_f32_fw_gpu, with_output_size_same_input) {

    const auto& engine = get_test_engine();

    auto input = memory::allocate(engine, { data_types::f32, format::bfyx, { 1, 4, 320, 320 } });
    auto weights = memory::allocate(engine, { data_types::f32, format::bfyx, { 64, 4, 7, 7 } });
    auto weights2 = memory::allocate(engine, { data_types::f32, format::bfyx, { 64, 4, 7, 7 } });

    topology topology(
        input_layout("input", input.get_layout()),
        data("weights", weights),
        data("weights2", weights2),
        convolution::create_with_output_size("conv1", "input", { "weights" }, {1, 64, 160, 160}, {1, 1, 2, 2}, {0, 0, -3, -3}),
        convolution::create_with_output_size("conv2", "input", { "weights2" }, {1, 64, 320, 320}, {1, 1, 1, 1}, {0, 0, -3, -3})
        );

    network network(engine, topology);
    network.set_input_data("input", input);

    auto outputs = network.execute();
    EXPECT_EQ(outputs.size(), size_t(2));
    EXPECT_EQ(outputs.begin()->first, "conv1");
    EXPECT_EQ(outputs.rbegin()->first, "conv2");
}

TEST(convolution_f32_fw_gpu, three_convolutions_same_weights) {
    //  Filter : 1x1
    //  Input  : 2x2
    //  Output : 2x2
    //
    //  Input:
    //  1  1   1  1
    //  1  1   1  1
    //  
    //  Filter:
    //  1 
    //  
    //  Output:
    //  8  8   8  8
    //  8  8   8  8


    const auto& engine = get_test_engine();

    auto input = memory::allocate(engine, { data_types::f32, format::bfyx, {1,2,2,2} });
    auto weights = memory::allocate(engine, { data_types::f32, format::bfyx, { 2,2,1,1 } });

    set_values(input, { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f });
    set_values(weights, { 1.0f, 1.0f, 1.0f, 1.0f });


    topology topology(
        input_layout("input", input.get_layout()),
        data("weights", weights),
        convolution("conv1", "input", { "weights" }),
        convolution("conv2", "conv1", { "weights" }),
        convolution("conv3", "conv2", { "weights" })
    );

    cldnn::build_options options;
    options.set_option(cldnn::build_option::optimize_data(true));
    network network(engine, topology, options);
    network.set_input_data("input", input);

    auto outputs = network.execute();

    auto output_memory = outputs.at("conv3").get_memory();
    auto output_layout = output_memory.get_layout();
    auto output_ptr = output_memory.pointer<float>();

    int y_size = output_layout.size.spatial[1];
    int x_size = output_layout.size.spatial[0];
    int f_size = output_layout.size.feature[0];
    int b_size = output_layout.size.batch[0];

    EXPECT_EQ(output_layout.format, format::bfyx);
    EXPECT_EQ(y_size, 2);
    EXPECT_EQ(x_size, 2);
    EXPECT_EQ(f_size, 2);
    EXPECT_EQ(b_size, 1);

    for (int y = 0; y < y_size; ++y) {
        for (int x = 0; x < x_size; ++x) {
            EXPECT_FLOAT_EQ(8.0f, output_ptr[y * x_size + x]);
        }
    }
}


TEST(convolution_f32_fw_gpu, basic_convolution) {
    //  Filter : 2x3
    //  Stride : 2x1
    //  Input  : 4x5
    //  Output : 2x3
    //
    //  Input:
    //  1  2  3  4  5
    //  2  2  3  4  6
    //  3  3  3  5  1
    //  1  1  1  1  1
    //
    //  Filter:
    //  1  2  1
    //  2  1  2
    //
    //  Output:
    // 21  28  39
    // 18  20  20
    //
    //  Bias:
    //  1

    const auto& engine = get_test_engine();

    auto input = memory::allocate(engine, { data_types::f32, format::yxfb, { 1, 1, 5, 4 } });
    auto weights = memory::allocate(engine, { data_types::f32, format::bfyx, { 1, 1, 3, 2 } });
    auto biases = memory::allocate(engine, { data_types::f32, format::bfyx, { 1, 1, 1, 1 } });

    set_values(input, { 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 2.0f, 2.0f, 3.0f, 4.0f, 6.0f, 3.0f, 3.0f, 3.0f, 5.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f });
    set_values(weights, { 1.0f, 2.0f, 1.0f, 2.0f, 1.0f, 2.0f });
    set_values(biases, { 1.0f });
    VVF<float> output_vec = {
        { 21.0f, 28.0f, 39.0f },
        { 18.0f, 20.0f, 20.0f } };

    topology topology(
        input_layout("input", input.get_layout()),
        data("weights", weights),
        data("biases", biases),
        convolution( "conv", "input", { "weights" }, { "biases" }, { 0,0,1,2 }));

    network network(engine, topology);
    network.set_input_data("input", input);

    auto outputs = network.execute();
    EXPECT_EQ(outputs.size(), size_t(1));
    EXPECT_EQ(outputs.begin()->first, "conv");

    auto output_memory = outputs.at("conv").get_memory();
    auto output_layout = output_memory.get_layout();
    auto output_ptr = output_memory.pointer<float>();

    int y_size = output_layout.size.spatial[1];
    int x_size = output_layout.size.spatial[0];
    int f_size = output_layout.size.feature[0];
    int b_size = output_layout.size.batch[0];
    EXPECT_EQ(output_layout.format, format::yxfb);
    EXPECT_EQ(y_size, 2);
    EXPECT_EQ(x_size, 3);
    EXPECT_EQ(f_size, 1);
    EXPECT_EQ(b_size, 1);
    for (int y = 0; y < y_size; ++y) {
        for (int x = 0; x < x_size; ++x) {
            EXPECT_EQ(output_vec[y][x], output_ptr[y * x_size + x]);
        }
    }
}

TEST(convolution_f32_fw_gpu, basic_convolution_bfyx_weights_as_input_layout) {
    //Same params as convolution_f32_fw_gpu, basic_convolution but with bfyx optimized data and weights set as input_layout
    const auto& engine = get_test_engine();
    auto input = memory::allocate(engine, { data_types::f32, format::bfyx,
    { 1, 1, 5, 4 }
    });
    auto weights = memory::allocate(engine, { data_types::f32, format::bfyx,
    { 1, 1, 3, 2 }
    });
    auto biases = memory::allocate(engine, { data_types::f32, format::bfyx,
    { 1, 1, 1, 1 }
    });
    set_values(input,
    { 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 2.0f, 2.0f, 3.0f, 4.0f, 6.0f, 3.0f, 3.0f, 3.0f, 5.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f }
    );
    set_values(weights,
    { 1.0f, 2.0f, 1.0f, 2.0f, 1.0f, 2.0f }
    );
    set_values(biases,
    { 1.0f }
    );
    VVF<float> output_vec = {
        { 21.0f, 28.0f, 39.0f }
        ,
        { 18.0f, 20.0f, 20.0f }
    };
    topology topology(
        input_layout("input", input.get_layout()),
        input_layout("weights", weights.get_layout()),
        input_layout("biases", biases.get_layout()),
        convolution("conv", "input",
        { "weights" }
            ,
            { "biases" }
            ,
            { 0,0,1,2 }
    ));
    cldnn::build_options options;
    options.set_option(cldnn::build_option::optimize_data(true));
    network network(engine, topology, options);
    network.set_input_data("input", input);
    network.set_input_data("weights", weights);
    network.set_input_data("biases", biases);
    auto outputs = network.execute();
    EXPECT_EQ(outputs.size(), size_t(1));
    EXPECT_EQ(outputs.begin()->first, "conv");

    auto output_memory = outputs.at("conv").get_memory();
    auto output_layout = output_memory.get_layout();
    auto output_ptr = output_memory.pointer<float>();

    int y_size = output_layout.size.spatial[1];
    int x_size = output_layout.size.spatial[0];
    int f_size = output_layout.size.feature[0];
    int b_size = output_layout.size.batch[0];
    EXPECT_EQ(output_layout.format, format::bfyx);
    EXPECT_EQ(y_size, 2);
    EXPECT_EQ(x_size, 3);
    EXPECT_EQ(f_size, 1);
    EXPECT_EQ(b_size, 1);
    for (int y = 0; y < y_size; ++y) {
        for (int x = 0; x < x_size; ++x) {
            EXPECT_EQ(output_vec[y][x], output_ptr[y * x_size + x]);
        }
    }
}

TEST(convolution_f32_fw_gpu, basic_convolution_input_padding) {
    //  Filter : 2x2
    //  Stride : 1x1
    //  Input  : 3x4
    //  Input padding : 2x1
    //  Output : 6x5
    //  Padding: Zero
    //
    //  Input:
    //  z  z  z  z  z  z
    //  z  z  z  z  z  z
    //  z  1  2  3  4  z
    //  z  2  2  3  4  z
    //  z  3  3  3  5  z
    //  z  z  z  z  z  z
    //  z  z  z  z  z  z
    //
    //  Filter:
    //  1  1
    //  1  1
    //
    //  Output:
    //  1  1  1  1  1
    //  2  4  6  8  5
    //  4  8 11 15  9
    //  6 11 12 16 10
    //  4  7  7  9  6
    //  1  1  1  1  1
    //
    //  Bias:
    //  1

    const auto& engine = get_test_engine();

    auto input = memory::allocate(engine, { data_types::f32, format::yxfb, { 1, 1, 4, 3 } });
    auto weights = memory::allocate(engine, { data_types::f32, format::bfyx, { 1, 1, 2, 2 } });
    auto biases = memory::allocate(engine, { data_types::f32, format::bfyx, { 1, 1, 1, 1 } });

    set_values(input, { 1.0f, 2.0f, 3.0f, 4.0f, 2.0f, 2.0f, 3.0f, 4.0f, 3.0f, 3.0f, 3.0f, 5.0f });
    set_values(weights, { 1.0f, 1.0f, 1.0f, 1.0f });
    set_values(biases, { 1.0f });
    VVF<float> output_vec = {
        { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f },
        { 2.0f, 4.0f, 6.0f, 8.0f, 5.0f },
        { 4.0f, 8.0f, 11.0f, 15.0f, 9.0f },
        { 6.0f, 11.0f, 12.0f, 16.0f, 10.0f },
        { 4.0f, 7.0f, 7.0f, 9.0f, 6.0f },
        { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f } };

    topology topology(
        input_layout("input", input.get_layout()),
        data("weights", weights),
        data("biases", biases),
        convolution(
            "conv",
            "input",
            { "weights" },
            { "biases" },
            { 1,1,1,1 },
            { 0,0,-1,-2 },
            { 1, 1, 1, 1 },
            false,
            0,
            padding{ { 0,0,0,0 }, 0 })
    );

    network network(engine, topology);
    network.set_input_data("input", input);

    auto outputs = network.execute();
    EXPECT_EQ(outputs.size(), size_t(1));
    EXPECT_EQ(outputs.begin()->first, "conv");

    auto output_memory = outputs.at("conv").get_memory();
    auto output_layout = output_memory.get_layout();
    auto output_ptr = output_memory.pointer<float>();

    int y_size = output_layout.size.spatial[1];
    int x_size = output_layout.size.spatial[0];
    int f_size = output_layout.size.feature[0];
    int b_size = output_layout.size.batch[0];
    EXPECT_EQ(output_layout.format, format::yxfb);
    EXPECT_EQ(y_size, 6);
    EXPECT_EQ(x_size, 5);
    EXPECT_EQ(f_size, 1);
    EXPECT_EQ(b_size, 1);

    for (int y = 0; y < y_size; ++y) {
        for (int x = 0; x < x_size; ++x) {
            EXPECT_EQ(output_vec[y][x], output_ptr[y * x_size + x]);
        }
    }

    //VVF temp_vec(y_size, VF(x_size, 0.0f));
    //for (int y = 0; y < y_size; ++y) {
    //    for (int x = 0; x < x_size; ++x) {
    //        temp_vec[y][x] = output_ptr[y * x_size + x];
    //    }
    //}
    //print_2d(temp_vec);
}

TEST(convolution_f32_fw_gpu, basic_convolution_sym_input_padding) {
    //  Filter : 2x2
    //  Stride : 1x1
    //  Input  : 3x4
    //  Input padding : above 2x1, below 2x1
    //  Output : 6x5
    //  Padding: Zero
    //
    //  Input:
    //  z  z  z  z  z  z
    //  z  z  z  z  z  z
    //  z  1  2  3  4  z
    //  z  2  2  3  4  z
    //  z  3  3  3  5  z
    //  z  z  z  z  z  z
    //  z  z  z  z  z  z
    //
    //  Filter:
    //  1  1
    //  1  1
    //
    //  Output:
    //  1  1  1  1  1
    //  2  4  6  8  5
    //  4  8 11 15  9
    //  6 11 12 16 10
    //  4  7  7  9  6
    //  1  1  1  1  1
    //
    //  Bias:
    //  1

    const auto& engine = get_test_engine();

    auto input = memory::allocate(engine, { data_types::f32, format::yxfb,{ 1, 1, 4, 3 } });
    auto weights = memory::allocate(engine, { data_types::f32, format::bfyx,{ 1, 1, 2, 2 } });
    auto biases = memory::allocate(engine, { data_types::f32, format::bfyx,{ 1, 1, 1, 1 } });

    set_values(input, { 1.0f, 2.0f, 3.0f, 4.0f, 2.0f, 2.0f, 3.0f, 4.0f, 3.0f, 3.0f, 3.0f, 5.0f });
    set_values(weights, { 1.0f, 1.0f, 1.0f, 1.0f });
    set_values(biases, { 1.0f });
    VVF<float> output_vec = {
        { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f },
        { 2.0f, 4.0f, 6.0f, 8.0f, 5.0f },
        { 4.0f, 8.0f, 11.0f, 15.0f, 9.0f },
        { 6.0f, 11.0f, 12.0f, 16.0f, 10.0f },
        { 4.0f, 7.0f, 7.0f, 9.0f, 6.0f },
        { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f } };

    topology topology(
        input_layout("input", input.get_layout()),
        data("weights", weights),
        data("biases", biases),
        convolution(
            "conv",
            "input",
            { "weights" },
            { "biases" },
            { 1,1,1,1 },
            { 0,0,0,0 },
            { 1, 1, 1, 1 },
            { 0,0,1,2 },
            { 0,0,1,2 },
            false,
            0,
            padding{ { 0,0,0,0 }, 0 })
    );

    network network(engine, topology);
    network.set_input_data("input", input);

    auto outputs = network.execute();
    EXPECT_EQ(outputs.size(), size_t(1));
    EXPECT_EQ(outputs.begin()->first, "conv");

    auto output_memory = outputs.at("conv").get_memory();
    auto output_layout = output_memory.get_layout();
    auto output_ptr = output_memory.pointer<float>();

    int y_size = output_layout.size.spatial[1];
    int x_size = output_layout.size.spatial[0];
    int f_size = output_layout.size.feature[0];
    int b_size = output_layout.size.batch[0];
    EXPECT_EQ(output_layout.format, format::yxfb);
    EXPECT_EQ(y_size, 6);
    EXPECT_EQ(x_size, 5);
    EXPECT_EQ(f_size, 1);
    EXPECT_EQ(b_size, 1);

    for (int y = 0; y < y_size; ++y) {
        for (int x = 0; x < x_size; ++x) {
            EXPECT_EQ(output_vec[y][x], output_ptr[y * x_size + x]);
        }
    }
}

TEST(convolution_f32_fw_gpu, basic_convolution_asym_input_padding) {
    //  Filter : 2x2
    //  Stride : 1x1
    //  Input  : 3x4
    //  Input padding : above 2x1, below 3x2
    //  Output : 7x6
    //  Padding: Zero
    //
    //  Input:
    //  z  z  z  z  z  z  z
    //  z  z  z  z  z  z  z
    //  z  1  2  3  4  z  z
    //  z  2  2  3  4  z  z
    //  z  3  3  3  5  z  z
    //  z  z  z  z  z  z  z
    //  z  z  z  z  z  z  z
    //  z  z  z  z  z  z  z
    //
    //  Filter:
    //  1  1
    //  1  1
    //
    //  Output:
    //  1  1  1  1  1  1
    //  2  4  6  8  5  1
    //  4  8 11 15  9  1
    //  6 11 12 16 10  1
    //  4  7  7  9  6  1
    //  1  1  1  1  1  1
    //  1  1  1  1  1  1
    //
    //  Bias:
    //  1

    const auto& engine = get_test_engine();

    auto input = memory::allocate(engine, { data_types::f32, format::yxfb,{ 1, 1, 4, 3 } });
    auto weights = memory::allocate(engine, { data_types::f32, format::bfyx,{ 1, 1, 2, 2 } });
    auto biases = memory::allocate(engine, { data_types::f32, format::bfyx,{ 1, 1, 1, 1 } });

    set_values(input, { 1.0f, 2.0f, 3.0f, 4.0f, 2.0f, 2.0f, 3.0f, 4.0f, 3.0f, 3.0f, 3.0f, 5.0f });
    set_values(weights, { 1.0f, 1.0f, 1.0f, 1.0f });
    set_values(biases, { 1.0f });
    VVF<float> output_vec = {
        { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f },
        { 2.0f, 4.0f, 6.0f, 8.0f, 5.0f, 1.0f },
        { 4.0f, 8.0f, 11.0f, 15.0f, 9.0f, 1.0f },
        { 6.0f, 11.0f, 12.0f, 16.0f, 10.0f, 1.0f },
        { 4.0f, 7.0f, 7.0f, 9.0f, 6.0f, 1.0f },
        { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f },
        { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f } };

    topology topology(
        input_layout("input", input.get_layout()),
        data("weights", weights),
        data("biases", biases),
        convolution(
            "conv",
            "input",
            { "weights" },
            { "biases" },
            { 1,1,1,1 },
            { 0,0,0,0 },
            { 1, 1, 1, 1 },
            { 0,0,1,2 },
            { 0,0,2,3 },
            false,
            0,
            padding{ { 0,0,0,0 }, 0 })
    );

    network network(engine, topology);
    network.set_input_data("input", input);

    auto outputs = network.execute();
    EXPECT_EQ(outputs.size(), size_t(1));
    EXPECT_EQ(outputs.begin()->first, "conv");

    auto output_memory = outputs.at("conv").get_memory();
    auto output_layout = output_memory.get_layout();
    auto output_ptr = output_memory.pointer<float>();

    int y_size = output_layout.size.spatial[1];
    int x_size = output_layout.size.spatial[0];
    int f_size = output_layout.size.feature[0];
    int b_size = output_layout.size.batch[0];
    EXPECT_EQ(output_layout.format, format::yxfb);
    EXPECT_EQ(y_size, 7);
    EXPECT_EQ(x_size, 6);
    EXPECT_EQ(f_size, 1);
    EXPECT_EQ(b_size, 1);

    for (int y = 0; y < y_size; ++y) {
        for (int x = 0; x < x_size; ++x) {
            EXPECT_EQ(output_vec[y][x], output_ptr[y * x_size + x]);
        }
    }
}

TEST(convolution_f32_fw_gpu, basic_convolution_sym_input_padding_with_input_offset) {
    //  Filter : 2x2
    //  Stride : 1x1
    //  Input  : 3x4
    //  Input padding : above 2x1, below 2x1
    //  Input offset: 2x1
    //  Output : 10x7
    //  Padding: Zero
    //
    //  Input:
    //  z  z  z  z  z  z  z  z
    //  z  z  z  z  z  z  z  z
    //  z  z  z  z  z  z  z  z
    //  z  z  z  z  z  z  z  z
    //  z  z  1  2  3  4  z  z
    //  z  z  2  2  3  4  z  z
    //  z  z  3  3  3  5  z  z
    //  z  z  z  z  z  z  z  z
    //  z  z  z  z  z  z  z  z
    //  z  z  z  z  z  z  z  z
    //  z  z  z  z  z  z  z  z
    //
    //  Filter:
    //  1  1
    //  1  1
    //
    //  Output:
    //  1  1  1  1  1  1  1
    //  1  1  1  1  1  1  1
    //  1  1  1  1  1  1  1
    //  1  2  4  6  8  5  1
    //  1  4  8 11 15  9  1
    //  1  6 11 12 16 10  1
    //  1  4  7  7  9  6  1
    //  1  1  1  1  1  1  1
    //  1  1  1  1  1  1  1
    //  1  1  1  1  1  1  1
    //
    //  Bias:
    //  1

    const auto& engine = get_test_engine();

    auto input = memory::allocate(engine, { data_types::f32, format::yxfb,{ 1, 1, 4, 3 } });
    auto weights = memory::allocate(engine, { data_types::f32, format::bfyx,{ 1, 1, 2, 2 } });
    auto biases = memory::allocate(engine, { data_types::f32, format::bfyx,{ 1, 1, 1, 1 } });

    set_values(input, { 1.0f, 2.0f, 3.0f, 4.0f, 2.0f, 2.0f, 3.0f, 4.0f, 3.0f, 3.0f, 3.0f, 5.0f });
    set_values(weights, { 1.0f, 1.0f, 1.0f, 1.0f });
    set_values(biases, { 1.0f });
    VVF<float> output_vec = {
        { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f },
        { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f },
        { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f },
        { 1.0f, 2.0f, 4.0f, 6.0f, 8.0f, 5.0f, 1.0f },
        { 1.0f, 4.0f, 8.0f, 11.0f, 15.0f, 9.0f, 1.0f },
        { 1.0f, 6.0f, 11.0f, 12.0f, 16.0f, 10.0f, 1.0f },
        { 1.0f, 4.0f, 7.0f, 7.0f, 9.0f, 6.0f, 1.0f },
        { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f },
        { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f },
        { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f } };

    topology topology(
        input_layout("input", input.get_layout()),
        data("weights", weights),
        data("biases", biases),
        convolution(
            "conv",
            "input",
            { "weights" },
            { "biases" },
            { 1,1,1,1 },
            { 0,0,-1,-2 },
            { 1, 1, 1, 1 },
            { 0,0,1,2 },
            { 0,0,1,2 },
            false,
            0,
            padding{ { 0,0,0,0 }, 0 })
    );

    network network(engine, topology);
    network.set_input_data("input", input);

    auto outputs = network.execute();
    EXPECT_EQ(outputs.size(), size_t(1));
    EXPECT_EQ(outputs.begin()->first, "conv");

    auto output_memory = outputs.at("conv").get_memory();
    auto output_layout = output_memory.get_layout();
    auto output_ptr = output_memory.pointer<float>();

    int y_size = output_layout.size.spatial[1];
    int x_size = output_layout.size.spatial[0];
    int f_size = output_layout.size.feature[0];
    int b_size = output_layout.size.batch[0];
    EXPECT_EQ(output_layout.format, format::yxfb);
    EXPECT_EQ(y_size, 10);
    EXPECT_EQ(x_size, 7);
    EXPECT_EQ(f_size, 1);
    EXPECT_EQ(b_size, 1);

    for (int y = 0; y < y_size; ++y) {
        for (int x = 0; x < x_size; ++x) {
            EXPECT_EQ(output_vec[y][x], output_ptr[y * x_size + x]);
        }
    }
}

TEST(convolution_f32_fw_gpu, basic_convolution_asym_input_padding_with_input_offset) {
    //  Filter : 2x2
    //  Stride : 1x1
    //  Input  : 3x4
    //  Input padding : above 2x1, below 3x2
    //  Input offset: 2x1
    //  Output : 11x8
    //  Padding: Zero
    //
    //  Input:
    //  z  z  z  z  z  z  z  z  z
    //  z  z  z  z  z  z  z  z  z
    //  z  z  z  z  z  z  z  z  z
    //  z  z  z  z  z  z  z  z  z
    //  z  z  1  2  3  4  z  z  z
    //  z  z  2  2  3  4  z  z  z
    //  z  z  3  3  3  5  z  z  z
    //  z  z  z  z  z  z  z  z  z
    //  z  z  z  z  z  z  z  z  z
    //  z  z  z  z  z  z  z  z  z
    //  z  z  z  z  z  z  z  z  z
    //  z  z  z  z  z  z  z  z  z
    //
    //  Filter:
    //  1  1
    //  1  1
    //
    //  Output:
    //  1  1  1  1  1  1  1  1
    //  1  1  1  1  1  1  1  1
    //  1  1  1  1  1  1  1  1
    //  1  2  4  6  8  5  1  1
    //  1  4  8 11 15  9  1  1
    //  1  6 11 12 16 10  1  1
    //  1  4  7  7  9  6  1  1
    //  1  1  1  1  1  1  1  1
    //  1  1  1  1  1  1  1  1
    //  1  1  1  1  1  1  1  1
    //  1  1  1  1  1  1  1  1
    //
    //  Bias:
    //  1

    const auto& engine = get_test_engine();

    auto input = memory::allocate(engine, { data_types::f32, format::yxfb,{ 1, 1, 4, 3 } });
    auto weights = memory::allocate(engine, { data_types::f32, format::bfyx,{ 1, 1, 2, 2 } });
    auto biases = memory::allocate(engine, { data_types::f32, format::bfyx,{ 1, 1, 1, 1 } });

    set_values(input, { 1.0f, 2.0f, 3.0f, 4.0f, 2.0f, 2.0f, 3.0f, 4.0f, 3.0f, 3.0f, 3.0f, 5.0f });
    set_values(weights, { 1.0f, 1.0f, 1.0f, 1.0f });
    set_values(biases, { 1.0f });
    VVF<float> output_vec = {
        { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f },
        { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f },
        { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f },
        { 1.0f, 2.0f, 4.0f, 6.0f, 8.0f, 5.0f, 1.0f, 1.0f },
        { 1.0f, 4.0f, 8.0f, 11.0f, 15.0f, 9.0f, 1.0f, 1.0f },
        { 1.0f, 6.0f, 11.0f, 12.0f, 16.0f, 10.0f, 1.0f, 1.0f },
        { 1.0f, 4.0f, 7.0f, 7.0f, 9.0f, 6.0f, 1.0f, 1.0f },
        { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f },
        { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f },
        { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f },
        { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f } };

    topology topology(
        input_layout("input", input.get_layout()),
        data("weights", weights),
        data("biases", biases),
        convolution(
            "conv",
            "input",
            { "weights" },
            { "biases" },
            { 1,1,1,1 },
            { 0,0,-1,-2 },
            { 1, 1, 1, 1 },
            { 0,0,1,2 },
            { 0,0,2,3 },
            false,
            0,
            padding{ { 0,0,0,0 }, 0 })
    );

    network network(engine, topology);
    network.set_input_data("input", input);

    auto outputs = network.execute();
    EXPECT_EQ(outputs.size(), size_t(1));
    EXPECT_EQ(outputs.begin()->first, "conv");

    auto output_memory = outputs.at("conv").get_memory();
    auto output_layout = output_memory.get_layout();
    auto output_ptr = output_memory.pointer<float>();

    int y_size = output_layout.size.spatial[1];
    int x_size = output_layout.size.spatial[0];
    int f_size = output_layout.size.feature[0];
    int b_size = output_layout.size.batch[0];
    EXPECT_EQ(output_layout.format, format::yxfb);
    EXPECT_EQ(y_size, 11);
    EXPECT_EQ(x_size, 8);
    EXPECT_EQ(f_size, 1);
    EXPECT_EQ(b_size, 1);

    for (int y = 0; y < y_size; ++y) {
        for (int x = 0; x < x_size; ++x) {
            EXPECT_EQ(output_vec[y][x], output_ptr[y * x_size + x]);
        }
    }
}

TEST(convolution_f32_fw_gpu, basic_convolution_input_and_output_padding) {
    //  Filter : 2x2
    //  Stride : 1x1
    //  Input  : 3x4
    //  Input padding : 2x1
    //  Output : 8x9
    //  Padding: Zero
    //
    //  Input:
    //  z  z  z  z  z  z
    //  z  z  z  z  z  z
    //  z  1  2  3  4  z
    //  z  2  2  3  4  z
    //  z  3  3  3  5  z
    //  z  z  z  z  z  z
    //  z  z  z  z  z  z
    //
    //  Filter:
    //  1  1
    //  1  1
    //
    //  Output:
    //  1  1  1  1  1  1  1  1  1
    //  1  1  1  1  1  1  1  1  1
    //  1  1  2  4  6  8  5  1  1
    //  1  1  4  8 11 15  9  1  1
    //  1  1  6 11 12 16 10  1  1
    //  1  1  4  7  7  9  6  1  1
    //  1  1  1  1  1  1  1  1  1
    //  1  1  1  1  1  1  1  1  1
    //
    //  Bias:
    //  1

    const auto& engine = get_test_engine();

    auto input = memory::allocate(engine, { data_types::f32, format::yxfb, { 1, 1, 4, 3 } });
    auto weights = memory::allocate(engine, { data_types::f32, format::bfyx, { 1, 1, 2, 2 } });
    auto biases = memory::allocate(engine, { data_types::f32, format::bfyx, { 1, 1, 1, 1 } });

    set_values(input, { 1.0f, 2.0f, 3.0f, 4.0f, 2.0f, 2.0f, 3.0f, 4.0f, 3.0f, 3.0f, 3.0f, 5.0f });
    set_values(weights, { 1.0f, 1.0f, 1.0f, 1.0f });
    set_values(biases, { 1.0f });
    VVF<float> output_vec = {
        { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f },
        { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f },
        { 1.0f, 1.0f, 2.0f, 4.0f, 6.0f, 8.0f, 5.0f, 1.0f, 1.0f },
        { 1.0f, 1.0f, 4.0f, 8.0f, 11.0f, 15.0f, 9.0f, 1.0f, 1.0f },
        { 1.0f, 1.0f, 6.0f, 11.0f, 12.0f, 16.0f, 10.0f, 1.0f, 1.0f },
        { 1.0f, 1.0f, 4.0f, 7.0f, 7.0f, 9.0f, 6.0f, 1.0f, 1.0f },
        { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f },
        { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f } };

    const int x_pad = 2;
    const int y_pad = 1;
    topology topology(
        input_layout("input", input.get_layout()),
        data("weights", weights),
        data("biases", biases),
        convolution(
            "conv",
            "input",
            { "weights" },
            { "biases" },
            { 1,1,1,1 },
            { 0,0,-1,-2 },
            { 1, 1, 1, 1 },
            false,
            0,
            padding{ { 0,0,-x_pad,-y_pad }, 0 })
    );

    network network(engine, topology);
    network.set_input_data("input", input);

    auto outputs = network.execute();
    EXPECT_EQ(outputs.size(), size_t(1));
    EXPECT_EQ(outputs.begin()->first, "conv");

    auto output_memory = outputs.at("conv").get_memory();
    auto output_layout = output_memory.get_layout();
    auto output_size = output_layout.get_buffer_size();
    auto output_ptr = output_memory.pointer<float>();

    int y_size = output_size.spatial[1];
    int x_size = output_size.spatial[0];
    int f_size = output_size.feature[0];
    int b_size = output_size.batch[0];
    EXPECT_EQ(output_layout.format, format::yxfb);
    EXPECT_EQ(y_size, 8);
    EXPECT_EQ(x_size, 9);
    EXPECT_EQ(f_size, 1);
    EXPECT_EQ(b_size, 1);

    for (int y = y_pad; y < y_size - y_pad; ++y) 
    {
        for (int x = x_pad; x < x_size - x_pad; ++x) 
        {
            EXPECT_EQ(output_vec[y][x], output_ptr[y * x_size + x]);
        }
    }

    //VVF temp_vec(y_size, VF(x_size, 0.0f));
    //for (int y = 0; y < y_size; ++y) {
    //    for (int x = 0; x < x_size; ++x) {
    //        temp_vec[y][x] = output_ptr[y * x_size + x];
    //    }
    //}
    //print_2d(temp_vec);
}

TEST(convolution_f32_fw_gpu, basic_wsiz2x2_wstr2x2_in4x4x1x1_nopad_random) {
    //  Filter : 2x2
    //  Stride : 2x2
    //  Input  : 4x4
    //  Output : 2x2
    //
    //  Input:
    //  rnd  rnd  rnd  rnd
    //  rnd  rnd  rnd  rnd
    //  rnd  rnd  rnd  rnd
    //  rnd  rnd  rnd  rnd
    //
    //  Filter
    //  rnd  rnd
    //  rnd  rnd
    //
    //  Bias
    //  rnd
    //
    //  Output:
    //  rnd  rnd
    //  rnd  rnd

    size_t batch = 1, input_f = 1, input_y = 4, input_x = 4;

    VVVVF<float> input_rnd = generate_random_4d<float>(batch, input_f, input_y, input_x, -10, 10);
    VF<float> input_rnd_vec = flatten_4d<float>(format::yxfb, input_rnd);
    VVVVF<float> filter_rnd = generate_random_4d<float>(1, 1, 2, 2, -10, 10);
    VF<float> filter_rnd_vec = flatten_4d<float>(format::bfyx, filter_rnd);
    VF<float> bias_rnd = generate_random_1d<float>(1, -10, 10);
    VVVVF<float> output_rnd(batch, VVVF<float>(filter_rnd.size()));
    for (size_t b = 0; b < output_rnd.size(); ++b) {
        for (size_t of = 0; of < filter_rnd.size(); ++of) {
            output_rnd[b][of] = reference_convolve<float>(input_rnd[b], filter_rnd[of], 2, 2, bias_rnd[of]);
        }
    }
    VF<float> output_rnd_vec = flatten_4d<float>(format::yxfb, output_rnd);

    const auto& engine = get_test_engine();

    auto input = memory::allocate(engine, { data_types::f32,  format::yxfb, { 1, 1, 4, 4 } });
    //auto output = memory::allocate({ memory::format::yxfb_f32,{ 1,{ 2, 2 }, 1 } });
    auto weights = memory::allocate(engine, { data_types::f32,  format::bfyx, { 1, 1, 2, 2 } });
    auto biases = memory::allocate(engine, { data_types::f32,  format::bfyx, { 1, 1, 1, 1 } });

    set_values(input, input_rnd_vec);
    set_values(weights, filter_rnd_vec);
    set_values(biases, bias_rnd);

    topology topology(
        input_layout("input", input.get_layout()),
        data("weights", weights),
        data("biases", biases),
        convolution("conv", "input", {"weights"}, {"biases"}, {1,1,2,2})
    );

    network network(engine, topology);
    network.set_input_data("input", input);

    auto outputs = network.execute();
    EXPECT_EQ(outputs.size(), size_t(1));
    EXPECT_EQ(outputs.begin()->first, "conv");

    auto output_prim = outputs.begin()->second.get_memory();

    auto output_ptr = output_prim.pointer<float>();

    for (size_t i = 0; i < output_rnd.size(); ++i) {
        float x = float_round(output_rnd_vec[i]), y = float_round(output_ptr[i]);
        EXPECT_FLOAT_EQ(x, y) << "random seed = " << random_seed << std::endl;
    }
}

TEST(convolution_f32_fw_gpu, basic_wsiz2x2_wstr2x2_in2x2x1x2_nopad_random) {
    //  Filter : 2x2
    //  Stride : 2x2
    //  Input  : 2x2x1x2
    //  Output : 1x1x1x2
    //
    //  Input:
    //  rnd  rnd    rnd  rnd
    //  rnd  rnd    rnd  rnd
    //
    //  Filter:
    //  rnd  rnd
    //  rnd  rnd
    //
    //  Bias:
    //  rnd
    //
    //  Output:
    //  rnd  rnd

    size_t batch = 2, input_f = 1, input_y = 2, input_x = 2;

    VVVVF<float> input_rnd = generate_random_4d<float>(batch, input_f, input_y, input_x, -10, 10);
    VF<float> input_rnd_vec = flatten_4d<float>(format::yxfb, input_rnd);
    VVVVF<float> filter_rnd = generate_random_4d<float>(1, 1, 2, 2, -10, 10);
    VF<float> filter_rnd_vec = flatten_4d<float>(format::bfyx, filter_rnd);
    VF<float> bias_rnd = generate_random_1d<float>(1, -10, 10);
    VVVVF<float> output_rnd(batch, VVVF<float>(filter_rnd.size()));
    for (size_t b = 0; b < output_rnd.size(); ++b) {
        for (size_t of = 0; of < filter_rnd.size(); ++of) {
            output_rnd[b][of] = reference_convolve<float>(input_rnd[b], filter_rnd[of], 2, 2, bias_rnd[of]);
        }
    }
    VF<float> output_rnd_vec = flatten_4d<float>(format::yxfb, output_rnd);

    const auto& engine = get_test_engine();

    auto input = memory::allocate(engine, { data_types::f32, format::yxfb, { 2, 1, 2, 2 } });
    //auto output = memory::allocate({ memory::format::yxfb_f32,{ 2,{ 1, 1 }, 1 } });
    auto weights = memory::allocate(engine, { data_types::f32, format::bfyx, { 1, 1, 2, 2 } });
    auto biases = memory::allocate(engine, { data_types::f32, format::bfyx, { 1, 1, 1, 1 } });

    set_values(input, input_rnd_vec);
    set_values(weights, filter_rnd_vec);
    set_values(biases, bias_rnd);

    topology topology(
        input_layout("input", input.get_layout()),
        data("weights", weights),
        data("biases", biases),
        convolution("conv", "input", { "weights" }, { "biases" }, { 1,1,2,2 })
    );

    network network(engine, topology);
    network.set_input_data("input", input);

    auto outputs = network.execute();
    EXPECT_EQ(outputs.size(), size_t(1));
    EXPECT_EQ(outputs.begin()->first, "conv");

    auto output_prim = outputs.begin()->second.get_memory();

    auto output_ptr = output_prim.pointer<float>();

    for (size_t i = 0; i < output_rnd.size(); ++i) {
        float x = float_round(output_rnd_vec[i]), y = float_round(output_ptr[i]);
        EXPECT_FLOAT_EQ(x, y) << "random seed = " << random_seed << std::endl;
    }
}

TEST(convolution_f32_fw_gpu, basic_wsiz2x2_wstr2x2_in4x4x1x1_nopad) {
    //  Filter : 2x2
    //  Stride : 2x2
    //  Input  : 4x4
    //  Output : 2x2
    //
    //  Input:
    //  -0.5   1     0.5  2
    //   1.5  -0.5   0   -1
    //   0.5   0.5  -1    1
    //   0.5   2     1.5 -0.5
    //
    //  Filter
    //  -2   0.5
    //   3.5 1.5
    //
    //  Bias
    //  2
    //
    //  Output:
    //  8  0.5
    //  6  9

    const auto& engine = get_test_engine();

    auto input = memory::allocate(engine, { data_types::f32, format::yxfb, { 1, 1, 4, 4 } });
    //auto output = memory::allocate({ memory::format::yxfb_f32,{ 1,{ 2, 2 }, 1 } });
    auto weights = memory::allocate(engine, { data_types::f32, format::bfyx, { 1, 1, 2, 2 } });
    auto biases = memory::allocate(engine, { data_types::f32, format::bfyx, { 1, 1, 1, 1 } });

    set_values(input, { -0.5f, 1.0f, 0.5f, 2.0f, 1.5f, -0.5f, 0.0f, -1.0f, 0.5f, 0.5f, -1.0f, 1.0f, 0.5f, 2.0f, 1.5f, -0.5f });
    set_values(weights, { -2.0f, 0.5f, 3.5f, 1.5f });
    set_values(biases, { 2.0f });

    topology topology(
        input_layout("input", input.get_layout()),
        data("weights", weights),
        data("biases", biases),
        convolution("conv", "input", { "weights" }, { "biases" }, { 1,1,2,2 })
    );

    network network(engine, topology);
    network.set_input_data("input", input);

    auto outputs = network.execute();
    EXPECT_EQ(outputs.size(), size_t(1));
    EXPECT_EQ(outputs.begin()->first, "conv");

    auto output_prim = outputs.begin()->second.get_memory();

    auto output_ptr = output_prim.pointer<float>();

    EXPECT_FLOAT_EQ(8.0f, output_ptr[0]);
    EXPECT_FLOAT_EQ(0.5f, output_ptr[1]);
    EXPECT_FLOAT_EQ(6.0f, output_ptr[2]);
    EXPECT_FLOAT_EQ(9.0f, output_ptr[3]);
}

TEST(convolution_f32_fw_gpu, basic_wsiz2x2_wstr2x2_in2x2x1x2_nopad) {
    //  Filter : 2x2
    //  Stride : 2x2
    //  Input  : 2x2x1x2
    //  Output : 1x1x1x2
    //
    //  Input:
    //  0.5   1.5    2.3 -0.4
    //  2.0  -4.0    1.0  3.0
    //
    //  Filter:
    //  -1.2  1.5
    //   0.5 -0.5
    //
    //  Bias:
    //  -1
    //
    //  Output:
    //  3.65 -5.36
    const auto& engine = get_test_engine();

    auto input = memory::allocate(engine, { data_types::f32, format::yxfb, { 2, 1, 2, 2 } });
    //auto output = memory::allocate({ memory::format::yxfb_f32,{ 2,{ 1, 1 }, 1 } });
    auto weights = memory::allocate(engine, { data_types::f32, format::bfyx, { 1, 1, 2, 2 } });
    auto biases = memory::allocate(engine, { data_types::f32, format::bfyx, { 1, 1, 1, 1 } });

    set_values(input, { 0.5f, 2.3f, 1.5f, -0.4f, 2.0f, 1.0f, -4.0f, 3.0f });
    set_values(weights, { -1.2f, 1.5f, 0.5f, -0.5f });
    set_values(biases, { -1.0f });

    topology topology(
        input_layout("input", input.get_layout()),
        data("weights", weights),
        data("biases", biases),
        convolution("conv", "input", { "weights" }, { "biases" }, { 1,1,2,2 } )
    );

    network network(engine, topology);
    network.set_input_data("input", input);

    auto outputs = network.execute();
    EXPECT_EQ(outputs.size(), size_t(1));
    EXPECT_EQ(outputs.begin()->first, "conv");

    auto output_prim = outputs.begin()->second.get_memory();

    auto output_ptr = output_prim.pointer<float>();

    EXPECT_FLOAT_EQ(3.65f, output_ptr[0]);
    EXPECT_FLOAT_EQ(-5.36f, output_ptr[1]);
}

TEST(convolution_f32_fw_gpu, basic_ofm_wsiz2x1x2x1_in1x2x1_nopad) {
    //  Filter : 1x2x1x2x1
    //  Input  : 1x1x2x1
    //  Output : 1x2x1x1
    //
    //  Input:
    //  1.0    2.0
    //
    // Filter:
    //   1.0    2.0  ofm=0
    //  -1.0   -2.0  ofm=1
    //
    //  Bias:
    //  0.1 -0.2
    //
    //  Output:
    //   5.1  f=0
    //  -5.2  f=1

    const auto& engine = get_test_engine();

    auto input = memory::allocate(engine, { data_types::f32, format::yxfb, { 1, 1, 1, 2 } });
    //auto output = memory::allocate({ memory::format::yxfb_f32,{ 1 ,{ 1, 1 }, 2 } });
    auto weights = memory::allocate(engine, { data_types::f32, format::bfyx, { 2, 1, 1, 2 } });
    auto biases = memory::allocate(engine, { data_types::f32, format::bfyx, { 1, 1, 2, 1 } });

    set_values(input, { 1.0f, 2.0f });
    set_values(weights, { 1.0f, 2.0f, -1.0f, -2.0f });
    set_values(biases, { 0.1f, -0.2f });

    topology topology(
        input_layout("input", input.get_layout()),
        data("weights", weights),
        data("biases", biases),
        convolution("conv", "input", { "weights" }, { "biases" }, { 1,1,5,5 })
    );

    network network(engine, topology);
    network.set_input_data("input", input);

    auto outputs = network.execute();
    EXPECT_EQ(outputs.size(), size_t(1));
    EXPECT_EQ(outputs.begin()->first, "conv");

    auto output_prim = outputs.begin()->second.get_memory();

    auto output_ptr = output_prim.pointer<float>();

    EXPECT_FLOAT_EQ(5.1f, output_ptr[0]);
    EXPECT_FLOAT_EQ(-5.2f, output_ptr[1]);
}

TEST(convolution_f32_fw_gpu, basic_ofm_wsiz3x2x2x1_in2x2x1_nopad) {
    //  Filter : 1x3x2x2x1
    //  Input  : 1x2x2x1
    //  Output : 1x3x1x1
    //
    //  Input:
    //  1.0    2.0  f=0
    //  3.0    4.0  f=1
    //
    // Filter:
    //   1.0    2.0  ifm=0  ofm=0
    //   3.0    4.0  ifm=1
    //
    //   5.0    6.0  ifm=0  ofm=1
    //   7.0    8.0  ifm=1
    //
    //   9.0   10.0  ifm=0  ofm=2
    //  11.0   12.0  ifm=1
    //  Bias:
    //   -5     -6     -7
    //
    //  Output:
    //   25.0  f=0
    //   64,0  f=1
    //  103.0  f=2

    const auto& engine = get_test_engine();

    auto input = memory::allocate(engine, { data_types::f32, format::yxfb, { 1, 2, 1, 2 } });
    //auto output = memory::allocate({ memory::format::yxfb_f32,{ 1 ,{ 1, 1 }, 3 } });
    auto weights = memory::allocate(engine, { data_types::f32, format::bfyx, { 3, 2, 1, 2 } });
    auto biases = memory::allocate(engine, { data_types::f32, format::bfyx, { 1, 1, 3, 1 } });

    set_values(input, { 1.0f, 3.0f, 2.0f, 4.0f });
    set_values(weights, { 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f, 9.0f, 10.0f, 11.0f, 12.0f });
    set_values(biases, { -5.0f, -6.0f, -7.0f });

    topology topology(
        input_layout("input", input.get_layout()),
        data("weights", weights),
        data("biases", biases),
        convolution("conv", "input", { "weights" }, { "biases" }, { 1,1,5,5 })
    );

    network network(engine, topology);
    network.set_input_data("input", input);

    auto outputs = network.execute();
    EXPECT_EQ(outputs.size(), size_t(1));
    EXPECT_EQ(outputs.begin()->first, "conv");

    auto output_prim = outputs.begin()->second.get_memory();

    auto output_ptr = output_prim.pointer<float>();

    EXPECT_FLOAT_EQ(25.0f, output_ptr[0]);
    EXPECT_FLOAT_EQ(64.0f, output_ptr[1]);
    EXPECT_FLOAT_EQ(103.0f, output_ptr[2]);
}

TEST(convolution_f32_fw_gpu, basic_wsiz2x2x1x3_wstr2x2_in2x2x1x1_nopad) {
    //  Filter : 2x2x1x3
    //  Stride : 2x2
    //  Input  : 2x2x1x1
    //  Output : 1x1x3x1
    //
    //  Input:
    //  -2.3 -0.1
    //   3.1  1.9
    //
    //  Filter:
    //  -1.1  1.5       0.1  0.2        2.0  -1.0
    //   0.5 -0.5       0.4  0.7        2.5  -1.5
    //
    //  Bias:
    //  0.1 -0.2 0.3
    //
    //  Output:
    //   0.7
    //   2.12
    //   3.08

    const auto& engine = get_test_engine();

    auto input = memory::allocate(engine, { data_types::f32, format::yxfb, { 1, 1, 2, 2 } });
    //auto output = memory::allocate({ memory::format::yxfb_f32,{ 1 ,{ 1, 1 }, 3 } });
    auto weights = memory::allocate(engine, { data_types::f32, format::bfyx, { 3, 1, 2, 2 } });
    auto biases = memory::allocate(engine, { data_types::f32, format::bfyx, { 1, 1, 3, 1 } });

    set_values(input, { -2.3f, -0.1f, 3.1f, 1.9f });
    set_values(weights, { -1.1f, 1.5f, 0.5f, -0.5f, 0.1f, 0.2f, 0.4f, 0.7f, 2.0f, -1.0f, 2.5f, -1.5f });
    set_values(biases, { 0.1f, -0.2f, 0.3f });

    topology topology(
        input_layout("input", input.get_layout()),
        data("weights", weights),
        data("biases", biases),
        convolution("conv", "input", { "weights" }, { "biases" }, { 1,1,2,2 })
    );

    network network(engine, topology);
    network.set_input_data("input", input);

    auto outputs = network.execute();
    EXPECT_EQ(outputs.size(), size_t(1));
    EXPECT_EQ(outputs.begin()->first, "conv");

    auto output_prim = outputs.begin()->second.get_memory();

    auto output_ptr = output_prim.pointer<float>();

    EXPECT_TRUE(are_equal(3.08f, output_ptr[0]));
    EXPECT_TRUE(are_equal(2.12f, output_ptr[1]));
    EXPECT_TRUE(are_equal(0.7f,  output_ptr[2]));
}

TEST(convolution_f32_fw_gpu, wsiz3x3_wstr2x2_in2x2x1x1_zeropad) {
    //  Filter  : 3x3
    //  Stride  : 2x2
    //  Input   : 2x2
    //  Output  : 1x1
    //  Padding : zero
    //
    //  Input:
    //  -0.5   1.0   padd
    //   0.5   2.0   padd
    //  padd  padd   padd
    //
    //  Filter
    //  -2    0.5  3.5
    //   1.5  4   -5
    //   0.5  1.5 -1.5
    //
    //  Bias
    //  2
    //
    //  Output:
    //  12.25
    const auto& engine = get_test_engine();

    auto input = memory::allocate(engine, { data_types::f32, format::yxfb, { 1, 1, 2, 2 } });
    //auto output = memory::allocate({ memory::format::yxfb_f32,{ 1,{ 1, 1 }, 1 } });
    auto weights = memory::allocate(engine, { data_types::f32, format::bfyx, { 1, 1, 3, 3 } });
    auto biases = memory::allocate(engine, { data_types::f32, format::bfyx, { 1, 1, 1, 1 } });

    set_values(input, { -0.5f, 1.0f, 0.5f, 2.0f });
    set_values(weights, { -2.0f, 0.5f, 3.5f, 1.5f, 4.0f, -5.0f, 0.5f, 1.5f, -1.5f });
    set_values(biases, { 2.0f });

    topology topology(
        input_layout("input", input.get_layout()),
        data("weights", weights),
        data("biases", biases),
        convolution("conv", "input", { "weights" }, { "biases" }, { 1,1,2,2 })
    );

    network network(engine, topology);
    network.set_input_data("input", input);

    auto outputs = network.execute();
    EXPECT_EQ(outputs.size(), size_t(1));
    EXPECT_EQ(outputs.begin()->first, "conv");

    auto output_prim = outputs.begin()->second.get_memory();

    auto output_ptr = output_prim.pointer<float>();

    EXPECT_FLOAT_EQ(12.25f, output_ptr[0]);
}

TEST(convolution_f32_fw_gpu, offsets_wsiz3x3_wstr2x2_in2x2x1x1_zeropad) {
    //   Filter       : 3x3
    //   Stride       : 2x2
    //   Input        : 2x2
    //   Input offset : -1x-1
    //   Output       : 2x2
    //   Output offset: 1x1
    //   Padding      : zero
    //
    //   Input:
    //   padd padd  padd
    //   padd -0.5   1
    //   padd  0.5   2.0
    //
    //   Filter
    //   -2    0.5  3.5
    //    1.5  4   -5
    //    0.5  1.5 -1.5
    //
    //   Bias
    //   2
    //
    //   Output:
    //   rnd   rnd
    //   rnd   2.0
    const auto& engine = get_test_engine();

    auto input = memory::allocate(engine, { data_types::f32, format::yxfb, { 1, 1, 2, 2 } });
    //auto output = memory::allocate({ memory::format::yxfb_f32,{ 1 ,{ 2, 2 }, 1 } });
    auto weights = memory::allocate(engine, { data_types::f32, format::bfyx, { 1, 1, 3, 3 } });
    auto biases = memory::allocate(engine, { data_types::f32, format::bfyx, { 1, 1, 1, 1 } });

    set_values(input, { -0.5f, 1.0f, 0.5f, 2.0f });
    set_values(weights, { -2.0f, 0.5f, 3.5f, 1.5f, 4.0f, -5.0f, 0.5f, 1.5f, -1.5f });
    set_values(biases, { 2.0f });

    topology topology(
        input_layout("input", input.get_layout()),
        data("weights", weights),
        data("biases", biases),
        convolution(
            "conv",
            "input",
            { "weights" },
            { "biases" },
            { 1,1,2,2 },
            { 0,0,-1,-1 },
            { 1, 1, 1, 1 },
            false,
            0,
            padding{ { 0,0,1,1 }, 0 })
    );

    network network(engine, topology);
    network.set_input_data("input", input);

    auto outputs = network.execute();
    EXPECT_EQ(outputs.size(), size_t(1));
    EXPECT_EQ(outputs.begin()->first, "conv");

    auto output_prim = outputs.begin()->second.get_memory();

    auto output_ptr = output_prim.pointer<float>();

    EXPECT_FLOAT_EQ(-7.25f, output_ptr[4]);
}

TEST(convolution_f32_fw_gpu, basic_wsiz2x2_wstr2x2_in4x4x2x1_nopad_split2) {
    //  Filter : 2x2
    //  Stride : 2x2
    //  Input  : 4x4x2
    //  Output : 2x2x2
    //
    //  Input:
    //  f0: -0.5   1     0.5  2
    //       1.5  -0.5   0   -1
    //       0.5   0.5  -1    1
    //       0.5   2     1.5 -0.5
    //
    //  f1:  0.5   1.5   2.3 -0.4
    //       2.0  -4.0   1.0  3.0
    //       0.5   1.5   2.3 -0.4
    //       2.0  -4.0   1.0  3.0
    //
    //  Filter1:
    //  -2   0.5
    //   3.5 1.5
    //
    //  Bias1:
    //  2
    //
    //  Filter2:
    //  -1.2  1.5
    //   0.5 -0.5
    //
    //  Bias2:
    //  -1

    //  Output:
    //   8  3.65 0.5 -5.36
    //   6  3.65 9   -5.36

    const auto& engine = get_test_engine();

    auto input = memory::allocate(engine, { data_types::f32, format::yxfb, { 1, 2, 4, 4 } });
    //auto output = memory::allocate({ memory::format::yxfb_f32,{ 1,{ 2, 2 }, 2 } });
    auto weights1 = memory::allocate(engine, { data_types::f32, format::bfyx, { 1, 1, 2, 2 } });
    auto biases1 = memory::allocate(engine, { data_types::f32, format::bfyx, { 1, 1, 1, 1 } });
    auto weights2 = memory::allocate(engine, { data_types::f32, format::bfyx, { 1, 1, 2, 2 } });
    auto biases2 = memory::allocate(engine, { data_types::f32, format::bfyx, { 1, 1, 1, 1 } });

    set_values(input, {
        -0.5f,  0.5f,  1.0f,  1.5f,  0.5f,  2.3f,  2.0f, -0.4f,
        1.5f,  2.0f, -0.5f, -4.0f,  0.0f,  1.0f, -1.0f,  3.0f,
        0.5f,  0.5f,  0.5f,  1.5f, -1.0f,  2.3f,  1.0f, -0.4f,
        0.5f,  2.0f,  2.0f, -4.0f,  1.5f,  1.0f, -0.5f,  3.0f
    });
    set_values(weights1, { -2.0f, 0.5f, 3.5f, 1.5f });
    set_values(biases1, { 2.0f });
    set_values(weights2, { -1.2f, 1.5f, 0.5f, -0.5f });
    set_values(biases2, { -1.0f });

    topology topology(
        input_layout("input", input.get_layout()),
        data("weights1", weights1),
        data("biases1", biases1),
        data("weights2", weights2),
        data("biases2", biases2),
        convolution(
            "conv",
            "input",
            { "weights1", "weights2" },
            { "biases1", "biases2" },
            { 0,0,2,2 },
            { 0,0,0,0 },
            { 1,1,1,1 })
    );

    network network(engine, topology);
    network.set_input_data("input", input);

    auto outputs = network.execute();
    EXPECT_EQ(outputs.size(), size_t(1));
    EXPECT_EQ(outputs.begin()->first, "conv");

    auto output_prim = outputs.begin()->second.get_memory();

    auto output_ptr = output_prim.pointer<float>();

    EXPECT_FLOAT_EQ(8.0f,   get_value<float>(output_ptr, 0));
    EXPECT_FLOAT_EQ(3.65f,  get_value<float>(output_ptr, 1));
    EXPECT_FLOAT_EQ(0.5f,   get_value<float>(output_ptr, 2));
    EXPECT_FLOAT_EQ(-5.36f, get_value<float>(output_ptr, 3));
    EXPECT_FLOAT_EQ(6.0f,   get_value<float>(output_ptr, 4));
    EXPECT_FLOAT_EQ(3.65f,  get_value<float>(output_ptr, 5));
    EXPECT_FLOAT_EQ(9.0f,   get_value<float>(output_ptr, 6));
    EXPECT_FLOAT_EQ(-5.36f, get_value<float>(output_ptr, 7));
}

TEST(convolution_f32_fw_gpu, basic_wsiz2x2_wstr2x2_in4x4x2x2_nopad_split2) {
    //  2x Filter : 2x2
    //  Stride : 2x2
    //  Input  : 2x4x4x2
    //  Output : 2x2x2x2
    //
    //  Input:
    //  f0b0: -0.5   1     0.5  2
    //         1.5  -0.5   0   -1
    //         0.5   0.5  -1    1
    //         0.5   2     1.5 -0.5
    //
    //  f0b1: -0.5   1     0.5  2
    //         1.5  -0.5   0   -1
    //         0.5   0.5  -1    1
    //         0.5   2     1.5 -0.5
    //
    //  f1b0:  0.5   1.5   2.3 -0.4
    //         2.0  -4.0   1.0  3.0
    //         0.5   1.5   2.3 -0.4
    //         2.0  -4.0   1.0  3.0
    //
    //  f1b1:  0.5   1.5   2.3 -0.4
    //         2.0  -4.0   1.0  3.0
    //         0.5   1.5   2.3 -0.4
    //         2.0  -4.0   1.0  3.0
    //
    //
    //  Filter1:
    //  -2   0.5
    //   3.5 1.5
    //
    //  Bias1:
    //  2
    //
    //  Filter2:
    //  -1.2  1.5
    //   0.5 -0.5
    //
    //  Bias2:
    //  -1

    //  Output:
    //   8  8 3.65 3.65 0.5  0.5 -5.36 -5.36
    //   6  6 3.65 3.65 9    9   -5.36 -5.36

    const auto& engine = get_test_engine();

    auto input = memory::allocate(engine, { data_types::f32, format::yxfb, { 2, 2, 4, 4 } });
    //auto output = memory::allocate({ memory::format::yxfb_f32,{ 2,{ 2, 2 }, 2 } });
    auto weights1 = memory::allocate(engine, { data_types::f32, format::bfyx, { 1, 1, 2, 2 } });
    auto biases1 = memory::allocate(engine, { data_types::f32, format::bfyx, { 1, 1, 1, 1 } });
    auto weights2 = memory::allocate(engine, { data_types::f32, format::bfyx, { 1, 1, 2, 2 } });
    auto biases2 = memory::allocate(engine, { data_types::f32, format::bfyx, { 1, 1, 1, 1 } });

    set_values(input, {
       -0.5f, -0.5f,  0.5f,  0.5f,  1.0f,  1.0f,  1.5f,  1.5f,  0.5f,  0.5f,  2.3f,  2.3f,  2.0f,  2.0f, -0.4f, -0.4f,
        1.5f,  1.5f,  2.0f,  2.0f, -0.5f, -0.5f, -4.0f, -4.0f,  0.0f,  0.0f,  1.0f,  1.0f, -1.0f, -1.0f,  3.0f,  3.0f,
        0.5f,  0.5f,  0.5f,  0.5f,  0.5f,  0.5f,  1.5f,  1.5f, -1.0f, -1.0f,  2.3f,  2.3f,  1.0f,  1.0f, -0.4f, -0.4f,
        0.5f,  0.5f,  2.0f,  2.0f,  2.0f,  2.0f, -4.0f, -4.0f,  1.5f,  1.5f,  1.0f,  1.0f, -0.5f, -0.5f,  3.0f,  3.0f,
    });
    set_values(weights1, { -2.0f, 0.5f, 3.5f, 1.5f });
    set_values(biases1, { 2.0f });
    set_values(weights2, { -1.2f, 1.5f, 0.5f, -0.5f });
    set_values(biases2, { -1.0f });

    topology topology(
        input_layout("input", input.get_layout()),
        data("weights1", weights1),
        data("biases1", biases1),
        data("weights2", weights2),
        data("biases2", biases2),
        convolution(
            "conv",
            "input",
            { "weights1", "weights2" },
            { "biases1", "biases2" },
            { 1,1,2,2 },
            { 0,0,0,0 },
            { 1,1,1,1 })
    );

    network network(engine, topology);
    network.set_input_data("input", input);

    auto outputs = network.execute();
    EXPECT_EQ(outputs.size(), size_t(1));
    EXPECT_EQ(outputs.begin()->first, "conv");

    auto output_prim = outputs.begin()->second.get_memory();

    auto output_ptr = output_prim.pointer<float>();

    EXPECT_FLOAT_EQ(8.0f,   get_value<float>(output_ptr, 0));
    EXPECT_FLOAT_EQ(8.0f,   get_value<float>(output_ptr, 1));
    EXPECT_FLOAT_EQ(3.65f,  get_value<float>(output_ptr, 2));
    EXPECT_FLOAT_EQ(3.65f,  get_value<float>(output_ptr, 3));
    EXPECT_FLOAT_EQ(0.5f,   get_value<float>(output_ptr, 4));
    EXPECT_FLOAT_EQ(0.5f,   get_value<float>(output_ptr, 5));
    EXPECT_FLOAT_EQ(-5.36f, get_value<float>(output_ptr, 6));
    EXPECT_FLOAT_EQ(-5.36f, get_value<float>(output_ptr, 7));
    EXPECT_FLOAT_EQ(6.0f,   get_value<float>(output_ptr, 8));
    EXPECT_FLOAT_EQ(6.0f,   get_value<float>(output_ptr, 9));
    EXPECT_FLOAT_EQ(3.65f,  get_value<float>(output_ptr, 10));
    EXPECT_FLOAT_EQ(3.65f,  get_value<float>(output_ptr, 11));
    EXPECT_FLOAT_EQ(9.0f,   get_value<float>(output_ptr, 12));
    EXPECT_FLOAT_EQ(9.0f,   get_value<float>(output_ptr, 13));
    EXPECT_FLOAT_EQ(-5.36f, get_value<float>(output_ptr, 14));
    EXPECT_FLOAT_EQ(-5.36f, get_value<float>(output_ptr, 15));
}

TEST(convolution_f32_fw_gpu, basic_wsiz2x2_wstr2x2_in4x4x2x1_nopad_group2) {
    //  data is similar as in basic_wsiz2x2_wstr2x2_in4x4x2x1_nopad_split2
    engine engine;

    auto input = memory::allocate(engine, { data_types::f32, format::yxfb,{ 1, 2, 4, 4 } });
    auto weights = memory::allocate(engine, { data_types::f32, format::bfyx,{ 2, 1, 2, 2 } });
    auto biases = memory::allocate(engine, { data_types::f32, format::bfyx,{ 1, 1, 2, 1 } });

    set_values(input, {
        -0.5f,  0.5f,  1.0f,  1.5f,  0.5f,  2.3f,  2.0f, -0.4f,
        1.5f,  2.0f, -0.5f, -4.0f,  0.0f,  1.0f, -1.0f,  3.0f,
        0.5f,  0.5f,  0.5f,  1.5f, -1.0f,  2.3f,  1.0f, -0.4f,
        0.5f,  2.0f,  2.0f, -4.0f,  1.5f,  1.0f, -0.5f,  3.0f
    });
    set_values(weights, {
        -2.0f, 0.5f, 3.5f, 1.5f,
        -1.2f, 1.5f, 0.5f, -0.5f
    });
    set_values(biases, { 2.0f, -1.0f });

    topology topology(
        input_layout("input", input.get_layout()),
        data("weights", weights),
        data("biases", biases),
        convolution(
            "conv",
            "input",
            { "weights" },
            { "biases" },
            2, // number of groups
            { 0,0,2,2 },
            { 0,0,0,0 },
            { 1,1,1,1 })
    );

    network network(engine, topology);
    network.set_input_data("input", input);

    auto outputs = network.execute();
    EXPECT_EQ(outputs.size(), size_t(1));
    EXPECT_EQ(outputs.begin()->first, "conv");

    auto output_prim = outputs.begin()->second.get_memory();

    auto output_ptr = output_prim.pointer<float>();

    EXPECT_FLOAT_EQ(8.0f, get_value<float>(output_ptr, 0));
    EXPECT_FLOAT_EQ(3.65f, get_value<float>(output_ptr, 1));
    EXPECT_FLOAT_EQ(0.5f, get_value<float>(output_ptr, 2));
    EXPECT_FLOAT_EQ(-5.36f, get_value<float>(output_ptr, 3));
    EXPECT_FLOAT_EQ(6.0f, get_value<float>(output_ptr, 4));
    EXPECT_FLOAT_EQ(3.65f, get_value<float>(output_ptr, 5));
    EXPECT_FLOAT_EQ(9.0f, get_value<float>(output_ptr, 6));
    EXPECT_FLOAT_EQ(-5.36f, get_value<float>(output_ptr, 7));
}

TEST(convolution_f32_fw_gpu, basic_wsiz2x2_wstr2x2_in4x4x2x1_nopad_group2_bfyx) {
    //  data is similar as in basic_wsiz2x2_wstr2x2_in4x4x2x1_nopad_split2

    engine engine;

    auto input = memory::allocate(engine, { data_types::f32, format::yxfb,{ 1, 2, 4, 4 } });
    auto weights = memory::allocate(engine, { data_types::f32, format::bfyx,{ 2, 1, 2, 2 } });
    auto biases = memory::allocate(engine, { data_types::f32, format::bfyx,{ 1, 1, 2, 1 } });

    set_values(input, {
        -0.5f,  0.5f,  1.0f,  1.5f,  0.5f,  2.3f,  2.0f, -0.4f,
        1.5f,  2.0f, -0.5f, -4.0f,  0.0f,  1.0f, -1.0f,  3.0f,
        0.5f,  0.5f,  0.5f,  1.5f, -1.0f,  2.3f,  1.0f, -0.4f,
        0.5f,  2.0f,  2.0f, -4.0f,  1.5f,  1.0f, -0.5f,  3.0f
    });
    set_values(weights, {
        -2.0f, 0.5f, 3.5f, 1.5f,
        -1.2f, 1.5f, 0.5f, -0.5f
    });
    set_values(biases, { 2.0f, -1.0f });

    topology topology(
        input_layout("input", input.get_layout()),
        reorder("input_1", "input", { data_types::f32,format::bfyx,{ 1, 2, 4, 4 } }),
        data("weights", weights),
        data("biases", biases),
        convolution(
            "conv",
            "input_1",
            { "weights" },
            { "biases" },
            2, // number of groups
            { 0,0,2,2 },
            { 0,0,0,0 },
            { 1,1,1,1 })
    );

    network network(engine, topology);
    network.set_input_data("input", input);

    auto outputs = network.execute();
    EXPECT_EQ(outputs.size(), size_t(1));
    EXPECT_EQ(outputs.begin()->first, "conv");

    auto output_prim = outputs.begin()->second.get_memory();

    auto output_ptr = output_prim.pointer<float>();

    EXPECT_FLOAT_EQ(8.0f, get_value<float>(output_ptr, 0));
    EXPECT_FLOAT_EQ(0.5f, get_value<float>(output_ptr, 1));
    EXPECT_FLOAT_EQ(6.0f, get_value<float>(output_ptr, 2));
    EXPECT_FLOAT_EQ(9.0f, get_value<float>(output_ptr, 3));
    EXPECT_FLOAT_EQ(3.65f, get_value<float>(output_ptr, 4));
    EXPECT_FLOAT_EQ(-5.36f, get_value<float>(output_ptr, 5));
    EXPECT_FLOAT_EQ(3.65f, get_value<float>(output_ptr, 6));
    EXPECT_FLOAT_EQ(-5.36f, get_value<float>(output_ptr, 7));
}

TEST(convolution_f32_fw_gpu, basic_wsiz2x2_wstr2x2_in4x4x2x2_nopad_group2) {
    //  data is similar as in basic_wsiz2x2_wstr2x2_in4x4x2x2_nopad_split2

    engine engine;

    auto input = memory::allocate(engine, { data_types::f32, format::yxfb,{ 2, 2, 4, 4 } });
    auto weights = memory::allocate(engine, { data_types::f32, format::bfyx,{ 2, 1, 2, 2 } });
    auto biases = memory::allocate(engine, { data_types::f32, format::bfyx,{ 1, 1, 2, 1 } });

    set_values(input, {
        -0.5f, -0.5f,  0.5f,  0.5f,  1.0f,  1.0f,  1.5f,  1.5f,  0.5f,  0.5f,  2.3f,  2.3f,  2.0f,  2.0f, -0.4f, -0.4f,
        1.5f,  1.5f,  2.0f,  2.0f, -0.5f, -0.5f, -4.0f, -4.0f,  0.0f,  0.0f,  1.0f,  1.0f, -1.0f, -1.0f,  3.0f,  3.0f,
        0.5f,  0.5f,  0.5f,  0.5f,  0.5f,  0.5f,  1.5f,  1.5f, -1.0f, -1.0f,  2.3f,  2.3f,  1.0f,  1.0f, -0.4f, -0.4f,
        0.5f,  0.5f,  2.0f,  2.0f,  2.0f,  2.0f, -4.0f, -4.0f,  1.5f,  1.5f,  1.0f,  1.0f, -0.5f, -0.5f,  3.0f,  3.0f,
    });
    set_values(weights, {
        -2.0f, 0.5f, 3.5f, 1.5f,
        -1.2f, 1.5f, 0.5f, -0.5f
    });
    set_values(biases, { 2.0f, -1.0f });

    topology topology(
        input_layout("input", input.get_layout()),
        data("weights", weights),
        data("biases", biases),
        convolution(
            "conv",
            "input",
            { "weights" },
            { "biases" },
            2, // number of groups
            { 1,1,2,2 },
            { 0,0,0,0 },
            { 1,1,1,1 })
    );

    network network(engine, topology);
    network.set_input_data("input", input);

    auto outputs = network.execute();
    EXPECT_EQ(outputs.size(), size_t(1));
    EXPECT_EQ(outputs.begin()->first, "conv");

    auto output_prim = outputs.begin()->second.get_memory();

    auto output_ptr = output_prim.pointer<float>();

    EXPECT_FLOAT_EQ(8.0f, get_value<float>(output_ptr, 0));
    EXPECT_FLOAT_EQ(8.0f, get_value<float>(output_ptr, 1));
    EXPECT_FLOAT_EQ(3.65f, get_value<float>(output_ptr, 2));
    EXPECT_FLOAT_EQ(3.65f, get_value<float>(output_ptr, 3));
    EXPECT_FLOAT_EQ(0.5f, get_value<float>(output_ptr, 4));
    EXPECT_FLOAT_EQ(0.5f, get_value<float>(output_ptr, 5));
    EXPECT_FLOAT_EQ(-5.36f, get_value<float>(output_ptr, 6));
    EXPECT_FLOAT_EQ(-5.36f, get_value<float>(output_ptr, 7));
    EXPECT_FLOAT_EQ(6.0f, get_value<float>(output_ptr, 8));
    EXPECT_FLOAT_EQ(6.0f, get_value<float>(output_ptr, 9));
    EXPECT_FLOAT_EQ(3.65f, get_value<float>(output_ptr, 10));
    EXPECT_FLOAT_EQ(3.65f, get_value<float>(output_ptr, 11));
    EXPECT_FLOAT_EQ(9.0f, get_value<float>(output_ptr, 12));
    EXPECT_FLOAT_EQ(9.0f, get_value<float>(output_ptr, 13));
    EXPECT_FLOAT_EQ(-5.36f, get_value<float>(output_ptr, 14));
    EXPECT_FLOAT_EQ(-5.36f, get_value<float>(output_ptr, 15));
}

TEST(convolution_f32_fw_gpu, basic_wsiz2x2_wstr2x2_in4x4x2x2_nopad_split2_depthwise_sep_opt) {
    //  Test for depthwise separable optimization, there are 16 weights and biases (split 16)
    //  data is similar as in basic_wsiz2x2_wstr2x2_in4x4x2x2_nopad_split2 but with batch 1

    const auto& engine = get_test_engine();

    auto input = memory::allocate(engine, { data_types::f32, format::yxfb,{ 2, 16, 4, 4 } });

    set_values(input, {
        -0.5f, -0.5f,  0.5f,  0.5f, -0.5f, -0.5f,  0.5f,  0.5f, -0.5f, -0.5f,  0.5f,  0.5f, -0.5f, -0.5f,  0.5f,  0.5f, -0.5f, -0.5f,  0.5f,  0.5f, -0.5f, -0.5f,  0.5f,  0.5f, -0.5f, -0.5f,  0.5f,  0.5f, -0.5f, -0.5f,  0.5f,  0.5f,
        1.0f,  1.0f,  1.5f,  1.5f, 1.0f,  1.0f,  1.5f,  1.5f, 1.0f,  1.0f,  1.5f,  1.5f, 1.0f,  1.0f,  1.5f,  1.5f, 1.0f,  1.0f,  1.5f,  1.5f, 1.0f,  1.0f,  1.5f,  1.5f, 1.0f,  1.0f,  1.5f,  1.5f, 1.0f,  1.0f,  1.5f,  1.5f,
        0.5f,  0.5f,  2.3f,  2.3f, 0.5f,  0.5f,  2.3f,  2.3f, 0.5f,  0.5f,  2.3f,  2.3f, 0.5f,  0.5f,  2.3f,  2.3f, 0.5f,  0.5f,  2.3f,  2.3f, 0.5f,  0.5f,  2.3f,  2.3f, 0.5f,  0.5f,  2.3f,  2.3f, 0.5f,  0.5f,  2.3f,  2.3f,
        2.0f,  2.0f, -0.4f, -0.4f, 2.0f,  2.0f, -0.4f, -0.4f, 2.0f,  2.0f, -0.4f, -0.4f, 2.0f,  2.0f, -0.4f, -0.4f, 2.0f,  2.0f, -0.4f, -0.4f, 2.0f,  2.0f, -0.4f, -0.4f, 2.0f,  2.0f, -0.4f, -0.4f, 2.0f,  2.0f, -0.4f, -0.4f,
        1.5f,  1.5f,  2.0f,  2.0f, 1.5f,  1.5f,  2.0f,  2.0f, 1.5f,  1.5f,  2.0f,  2.0f, 1.5f,  1.5f,  2.0f,  2.0f, 1.5f,  1.5f,  2.0f,  2.0f, 1.5f,  1.5f,  2.0f,  2.0f, 1.5f,  1.5f,  2.0f,  2.0f, 1.5f,  1.5f,  2.0f,  2.0f,
        -0.5f, -0.5f, -4.0f, -4.0f, -0.5f, -0.5f, -4.0f, -4.0f, -0.5f, -0.5f, -4.0f, -4.0f, -0.5f, -0.5f, -4.0f, -4.0f, -0.5f, -0.5f, -4.0f, -4.0f, -0.5f, -0.5f, -4.0f, -4.0f, -0.5f, -0.5f, -4.0f, -4.0f, -0.5f, -0.5f, -4.0f, -4.0f,
        0.0f,  0.0f,  1.0f,  1.0f, 0.0f,  0.0f,  1.0f,  1.0f, 0.0f,  0.0f,  1.0f,  1.0f, 0.0f,  0.0f,  1.0f,  1.0f, 0.0f,  0.0f,  1.0f,  1.0f, 0.0f,  0.0f,  1.0f,  1.0f, 0.0f,  0.0f,  1.0f,  1.0f, 0.0f,  0.0f,  1.0f,  1.0f,
        -1.0f, -1.0f,  3.0f,  3.0f, -1.0f, -1.0f,  3.0f,  3.0f, -1.0f, -1.0f,  3.0f,  3.0f, -1.0f, -1.0f,  3.0f,  3.0f, -1.0f, -1.0f,  3.0f,  3.0f, -1.0f, -1.0f,  3.0f,  3.0f, -1.0f, -1.0f,  3.0f,  3.0f, -1.0f, -1.0f,  3.0f,  3.0f,
        0.5f,  0.5f,  0.5f,  0.5f, 0.5f,  0.5f,  0.5f,  0.5f, 0.5f,  0.5f,  0.5f,  0.5f, 0.5f,  0.5f,  0.5f,  0.5f, 0.5f,  0.5f,  0.5f,  0.5f, 0.5f,  0.5f,  0.5f,  0.5f, 0.5f,  0.5f,  0.5f,  0.5f, 0.5f,  0.5f,  0.5f,  0.5f,
        0.5f,  0.5f,  1.5f,  1.5f, 0.5f,  0.5f,  1.5f,  1.5f, 0.5f,  0.5f,  1.5f,  1.5f, 0.5f,  0.5f,  1.5f,  1.5f, 0.5f,  0.5f,  1.5f,  1.5f, 0.5f,  0.5f,  1.5f,  1.5f, 0.5f,  0.5f,  1.5f,  1.5f, 0.5f,  0.5f,  1.5f,  1.5f,
        -1.0f, -1.0f,  2.3f,  2.3f, -1.0f, -1.0f,  2.3f,  2.3f, -1.0f, -1.0f,  2.3f,  2.3f, -1.0f, -1.0f,  2.3f,  2.3f, -1.0f, -1.0f,  2.3f,  2.3f, -1.0f, -1.0f,  2.3f,  2.3f, -1.0f, -1.0f,  2.3f,  2.3f, -1.0f, -1.0f,  2.3f,  2.3f,
        1.0f,  1.0f, -0.4f, -0.4f, 1.0f,  1.0f, -0.4f, -0.4f, 1.0f,  1.0f, -0.4f, -0.4f, 1.0f,  1.0f, -0.4f, -0.4f, 1.0f,  1.0f, -0.4f, -0.4f, 1.0f,  1.0f, -0.4f, -0.4f, 1.0f,  1.0f, -0.4f, -0.4f, 1.0f,  1.0f, -0.4f, -0.4f,
        0.5f,  0.5f,  2.0f,  2.0f, 0.5f,  0.5f,  2.0f,  2.0f, 0.5f,  0.5f,  2.0f,  2.0f, 0.5f,  0.5f,  2.0f,  2.0f, 0.5f,  0.5f,  2.0f,  2.0f, 0.5f,  0.5f,  2.0f,  2.0f, 0.5f,  0.5f,  2.0f,  2.0f, 0.5f,  0.5f,  2.0f,  2.0f,
        2.0f,  2.0f, -4.0f, -4.0f, 2.0f,  2.0f, -4.0f, -4.0f, 2.0f,  2.0f, -4.0f, -4.0f, 2.0f,  2.0f, -4.0f, -4.0f, 2.0f,  2.0f, -4.0f, -4.0f, 2.0f,  2.0f, -4.0f, -4.0f, 2.0f,  2.0f, -4.0f, -4.0f, 2.0f,  2.0f, -4.0f, -4.0f,
        1.5f,  1.5f,  1.0f,  1.0f, 1.5f,  1.5f,  1.0f,  1.0f, 1.5f,  1.5f,  1.0f,  1.0f, 1.5f,  1.5f,  1.0f,  1.0f, 1.5f,  1.5f,  1.0f,  1.0f, 1.5f,  1.5f,  1.0f,  1.0f, 1.5f,  1.5f,  1.0f,  1.0f, 1.5f,  1.5f,  1.0f,  1.0f,
        -0.5f, -0.5f,  3.0f,  3.0f, -0.5f, -0.5f,  3.0f,  3.0f, -0.5f, -0.5f,  3.0f,  3.0f, -0.5f, -0.5f,  3.0f,  3.0f, -0.5f, -0.5f,  3.0f,  3.0f, -0.5f, -0.5f,  3.0f,  3.0f, -0.5f, -0.5f,  3.0f,  3.0f, -0.5f, -0.5f,  3.0f,  3.0f,
    });

    topology topology(input_layout("input", input.get_layout()));

    std::vector<primitive_id> weights_vec;
    std::vector<primitive_id> bias_vec;

    for (uint32_t i = 0; i < 8; i++)
    {
        auto weights1 = memory::allocate(engine, { data_types::f32, format::bfyx,{ 1, 1, 2, 2 } });
        auto biases1 = memory::allocate(engine, { data_types::f32, format::bfyx,{ 1, 1, 1, 1 } });
        auto weights2 = memory::allocate(engine, { data_types::f32, format::bfyx,{ 1, 1, 2, 2 } });
        auto biases2 = memory::allocate(engine, { data_types::f32, format::bfyx,{ 1, 1, 1, 1 } });

        set_values(weights1, { -2.0f, 0.5f, 3.5f, 1.5f });
        set_values(biases1, { 2.0f });
        set_values(weights2, { -1.2f, 1.5f, 0.5f, -0.5f });
        set_values(biases2, { -1.0f });

        primitive_id weights_id = "weights_" + std::to_string(i);
        primitive_id weights2_id = "weights2_" + std::to_string(i);
        primitive_id bias_id = "biases_" + std::to_string(i);
        primitive_id bias2_id = "biases2_" + std::to_string(i);

        weights_vec.push_back(weights_id);
        weights_vec.push_back(weights2_id);
        bias_vec.push_back(bias_id);
        bias_vec.push_back(bias2_id);

        topology.add(
            data(weights_id, weights1),
            data(bias_id, biases1),
            data(weights2_id, weights2),
            data(bias2_id, biases2)
        );

    }

    topology.add(
        convolution(
            "conv",
            "input",
            weights_vec,
            bias_vec,
            { 1,1,2,2 },
            { 0,0,0,0 },
            { 1,1,1,1 })
    );

    network network(engine, topology);
    network.set_input_data("input", input);

    auto outputs = network.execute();
    EXPECT_EQ(outputs.size(), size_t(1));
    EXPECT_EQ(outputs.begin()->first, "conv");

    auto output_prim = outputs.begin()->second.get_memory();

    auto output_ptr = output_prim.pointer<float>();

    std::vector<float> expected_output_vec = {
        8.0f, 8.0f, 3.65f, 3.65f, 8.0f, 8.0f, 3.65f, 3.65f, 8.0f, 8.0f, 3.65f, 3.65f, 8.0f, 8.0f, 3.65f, 3.65f, 8.0f, 8.0f, 3.65f, 3.65f, 8.0f, 8.0f, 3.65f, 3.65f, 8.0f, 8.0f, 3.65f, 3.65f, 8.0f, 8.0f, 3.65f, 3.65f,
        0.5f, 0.5f, -5.36f, -5.36f, 0.5f, 0.5f, -5.36f, -5.36f, 0.5f, 0.5f, -5.36f, -5.36f, 0.5f, 0.5f, -5.36f, -5.36f, 0.5f, 0.5f, -5.36f, -5.36f, 0.5f, 0.5f, -5.36f, -5.36f, 0.5f, 0.5f, -5.36f, -5.36f, 0.5f, 0.5f, -5.36f, -5.36f,
        6.0f, 6.0f, 3.65f, 3.65f, 6.0f, 6.0f, 3.65f, 3.65f, 6.0f, 6.0f, 3.65f, 3.65f, 6.0f, 6.0f, 3.65f, 3.65f, 6.0f, 6.0f, 3.65f, 3.65f, 6.0f, 6.0f, 3.65f, 3.65f, 6.0f, 6.0f, 3.65f, 3.65f, 6.0f, 6.0f, 3.65f, 3.65f,
        9.0f, 9.0f, -5.36f, -5.36f, 9.0f, 9.0f, -5.36f, -5.36f, 9.0f, 9.0f, -5.36f, -5.36f, 9.0f, 9.0f, -5.36f, -5.36f, 9.0f, 9.0f, -5.36f, -5.36f, 9.0f, 9.0f, -5.36f, -5.36f, 9.0f, 9.0f, -5.36f, -5.36f, 9.0f, 9.0f, -5.36f, -5.36f,
    };

    for (unsigned int i = 0; i < expected_output_vec.size(); i++)
    {
        EXPECT_FLOAT_EQ(expected_output_vec[i], output_ptr[i]);
    }
}

TEST(convolution_f32_fw_gpu, basic_wsiz2x2_wstr2x2_in4x4x2x2_nopad_split2_depthwise_sep_opt_bfyx) {
    //  Test for depthwise separable optimization, there are 16 weights and biases (split 16)
    //  data is similar as in basic_wsiz2x2_wstr2x2_in4x4x2x2_nopad_split2 but with batch 1
    const auto& engine = get_test_engine();

    auto input = memory::allocate(engine, { data_types::f32, format::bfyx,{ 2, 16, 4, 4 } });

    set_values(input, {
        -0.5f, 1.0f, 0.5f, 2.0f, 1.5f, -0.5f, 0.0f, -1.0f, 0.5f, 0.5f, -1.0f, 1.0f, 0.5f, 2.0f, 1.5f, -0.5f,
        0.5f, 1.5f, 2.3f, -0.4f, 2.0f, -4.0f, 1.0f, 3.0f, 0.5f, 1.5f, 2.3f, -0.4f, 2.0f, -4.0f, 1.0f, 3.0f,
        -0.5f, 1.0f, 0.5f, 2.0f, 1.5f, -0.5f, 0.0f, -1.0f, 0.5f, 0.5f, -1.0f, 1.0f, 0.5f, 2.0f, 1.5f, -0.5f,
        0.5f, 1.5f, 2.3f, -0.4f, 2.0f, -4.0f, 1.0f, 3.0f, 0.5f, 1.5f, 2.3f, -0.4f, 2.0f, -4.0f, 1.0f, 3.0f,
        -0.5f, 1.0f, 0.5f, 2.0f, 1.5f, -0.5f, 0.0f, -1.0f, 0.5f, 0.5f, -1.0f, 1.0f, 0.5f, 2.0f, 1.5f, -0.5f,
        0.5f, 1.5f, 2.3f, -0.4f, 2.0f, -4.0f, 1.0f, 3.0f, 0.5f, 1.5f, 2.3f, -0.4f, 2.0f, -4.0f, 1.0f, 3.0f,
        -0.5f, 1.0f, 0.5f, 2.0f, 1.5f, -0.5f, 0.0f, -1.0f, 0.5f, 0.5f, -1.0f, 1.0f, 0.5f, 2.0f, 1.5f, -0.5f,
        0.5f, 1.5f, 2.3f, -0.4f, 2.0f, -4.0f, 1.0f, 3.0f, 0.5f, 1.5f, 2.3f, -0.4f, 2.0f, -4.0f, 1.0f, 3.0f,
        -0.5f, 1.0f, 0.5f, 2.0f, 1.5f, -0.5f, 0.0f, -1.0f, 0.5f, 0.5f, -1.0f, 1.0f, 0.5f, 2.0f, 1.5f, -0.5f,
        0.5f, 1.5f, 2.3f, -0.4f, 2.0f, -4.0f, 1.0f, 3.0f, 0.5f, 1.5f, 2.3f, -0.4f, 2.0f, -4.0f, 1.0f, 3.0f,
        -0.5f, 1.0f, 0.5f, 2.0f, 1.5f, -0.5f, 0.0f, -1.0f, 0.5f, 0.5f, -1.0f, 1.0f, 0.5f, 2.0f, 1.5f, -0.5f,
        0.5f, 1.5f, 2.3f, -0.4f, 2.0f, -4.0f, 1.0f, 3.0f, 0.5f, 1.5f, 2.3f, -0.4f, 2.0f, -4.0f, 1.0f, 3.0f,
        -0.5f, 1.0f, 0.5f, 2.0f, 1.5f, -0.5f, 0.0f, -1.0f, 0.5f, 0.5f, -1.0f, 1.0f, 0.5f, 2.0f, 1.5f, -0.5f,
        0.5f, 1.5f, 2.3f, -0.4f, 2.0f, -4.0f, 1.0f, 3.0f, 0.5f, 1.5f, 2.3f, -0.4f, 2.0f, -4.0f, 1.0f, 3.0f,
        -0.5f, 1.0f, 0.5f, 2.0f, 1.5f, -0.5f, 0.0f, -1.0f, 0.5f, 0.5f, -1.0f, 1.0f, 0.5f, 2.0f, 1.5f, -0.5f,
        0.5f, 1.5f, 2.3f, -0.4f, 2.0f, -4.0f, 1.0f, 3.0f, 0.5f, 1.5f, 2.3f, -0.4f, 2.0f, -4.0f, 1.0f, 3.0f,
    });

    topology topology(input_layout("input", input.get_layout()));

    std::vector<primitive_id> weights_vec;
    std::vector<primitive_id> bias_vec;

    for (uint32_t i = 0; i < 8; i++)
    {
        auto weights1 = memory::allocate(engine, { data_types::f32, format::bfyx,{ 1, 1, 2, 2 } });
        auto biases1 = memory::allocate(engine, { data_types::f32, format::bfyx,{ 1, 1, 1, 1 } });
        auto weights2 = memory::allocate(engine, { data_types::f32, format::bfyx,{ 1, 1, 2, 2 } });
        auto biases2 = memory::allocate(engine, { data_types::f32, format::bfyx,{ 1, 1, 1, 1 } });

        set_values(weights1, { -2.0f, 0.5f, 3.5f, 1.5f });
        set_values(biases1, { 2.0f });
        set_values(weights2, { -1.2f, 1.5f, 0.5f, -0.5f });
        set_values(biases2, { -1.0f });

        primitive_id weights_id = "weights_" + std::to_string(i);
        primitive_id weights2_id = "weights2_" + std::to_string(i);
        primitive_id bias_id = "biases_" + std::to_string(i);
        primitive_id bias2_id = "biases2_" + std::to_string(i);

        weights_vec.push_back(weights_id);
        weights_vec.push_back(weights2_id);
        bias_vec.push_back(bias_id);
        bias_vec.push_back(bias2_id);

        topology.add(
            data(weights_id, weights1),
            data(bias_id, biases1),
            data(weights2_id, weights2),
            data(bias2_id, biases2)
        );

    }

    topology.add(
        convolution(
            "conv",
            "input",
            weights_vec,
            bias_vec,
            { 1,1,2,2 },
            { 0,0,0,0 },
            { 1,1,1,1 })
    );

    network network(engine, topology);
    network.set_input_data("input", input);

    auto outputs = network.execute();
    EXPECT_EQ(outputs.size(), size_t(1));
    EXPECT_EQ(outputs.begin()->first, "conv");

    auto output_prim = outputs.begin()->second.get_memory();

    auto output_ptr = output_prim.pointer<float>();

    std::vector<float> expected_output_vec = {
        8.0f, 0.5f,  6.0f,  9.0f, 3.65f,-5.36f, 3.65f, -5.36f,
        8.0f, 0.5f,  6.0f,  9.0f, 3.65f,-5.36f, 3.65f, -5.36f,
        8.0f, 0.5f,  6.0f,  9.0f, 3.65f,-5.36f, 3.65f, -5.36f,
        8.0f, 0.5f,  6.0f,  9.0f, 3.65f,-5.36f, 3.65f, -5.36f,
        8.0f, 0.5f,  6.0f,  9.0f, 3.65f,-5.36f, 3.65f, -5.36f,
        8.0f, 0.5f,  6.0f,  9.0f, 3.65f,-5.36f, 3.65f, -5.36f,
        8.0f, 0.5f,  6.0f,  9.0f, 3.65f,-5.36f, 3.65f, -5.36f,
        8.0f, 0.5f,  6.0f,  9.0f, 3.65f,-5.36f, 3.65f, -5.36f,
    };

    for (unsigned int i = 0; i < expected_output_vec.size(); i++)
    {
        EXPECT_FLOAT_EQ(expected_output_vec[i], output_ptr[i]);
    }
}

TEST(convolution_f32_fw_gpu, basic_wsiz2x2_wstr2x2_in4x4x2x2_nopad_group16) {
    //  Test for grouped convolution, there are 16 joined weights and biases (group 16)
    //  data is similar as in basic_wsiz2x2_wstr2x2_in4x4x2x2_nopad_split2_depthwise_sep_opt

    engine engine;

    auto input = memory::allocate(engine, { data_types::f32, format::yxfb,{ 2, 16, 4, 4 } });

    set_values(input, {
        -0.5f, -0.5f,  0.5f,  0.5f, -0.5f, -0.5f,  0.5f,  0.5f, -0.5f, -0.5f,  0.5f,  0.5f, -0.5f, -0.5f,  0.5f,  0.5f, -0.5f, -0.5f,  0.5f,  0.5f, -0.5f, -0.5f,  0.5f,  0.5f, -0.5f, -0.5f,  0.5f,  0.5f, -0.5f, -0.5f,  0.5f,  0.5f,
        1.0f,  1.0f,  1.5f,  1.5f, 1.0f,  1.0f,  1.5f,  1.5f, 1.0f,  1.0f,  1.5f,  1.5f, 1.0f,  1.0f,  1.5f,  1.5f, 1.0f,  1.0f,  1.5f,  1.5f, 1.0f,  1.0f,  1.5f,  1.5f, 1.0f,  1.0f,  1.5f,  1.5f, 1.0f,  1.0f,  1.5f,  1.5f,
        0.5f,  0.5f,  2.3f,  2.3f, 0.5f,  0.5f,  2.3f,  2.3f, 0.5f,  0.5f,  2.3f,  2.3f, 0.5f,  0.5f,  2.3f,  2.3f, 0.5f,  0.5f,  2.3f,  2.3f, 0.5f,  0.5f,  2.3f,  2.3f, 0.5f,  0.5f,  2.3f,  2.3f, 0.5f,  0.5f,  2.3f,  2.3f,
        2.0f,  2.0f, -0.4f, -0.4f, 2.0f,  2.0f, -0.4f, -0.4f, 2.0f,  2.0f, -0.4f, -0.4f, 2.0f,  2.0f, -0.4f, -0.4f, 2.0f,  2.0f, -0.4f, -0.4f, 2.0f,  2.0f, -0.4f, -0.4f, 2.0f,  2.0f, -0.4f, -0.4f, 2.0f,  2.0f, -0.4f, -0.4f,
        1.5f,  1.5f,  2.0f,  2.0f, 1.5f,  1.5f,  2.0f,  2.0f, 1.5f,  1.5f,  2.0f,  2.0f, 1.5f,  1.5f,  2.0f,  2.0f, 1.5f,  1.5f,  2.0f,  2.0f, 1.5f,  1.5f,  2.0f,  2.0f, 1.5f,  1.5f,  2.0f,  2.0f, 1.5f,  1.5f,  2.0f,  2.0f,
        -0.5f, -0.5f, -4.0f, -4.0f, -0.5f, -0.5f, -4.0f, -4.0f, -0.5f, -0.5f, -4.0f, -4.0f, -0.5f, -0.5f, -4.0f, -4.0f, -0.5f, -0.5f, -4.0f, -4.0f, -0.5f, -0.5f, -4.0f, -4.0f, -0.5f, -0.5f, -4.0f, -4.0f, -0.5f, -0.5f, -4.0f, -4.0f,
        0.0f,  0.0f,  1.0f,  1.0f, 0.0f,  0.0f,  1.0f,  1.0f, 0.0f,  0.0f,  1.0f,  1.0f, 0.0f,  0.0f,  1.0f,  1.0f, 0.0f,  0.0f,  1.0f,  1.0f, 0.0f,  0.0f,  1.0f,  1.0f, 0.0f,  0.0f,  1.0f,  1.0f, 0.0f,  0.0f,  1.0f,  1.0f,
        -1.0f, -1.0f,  3.0f,  3.0f, -1.0f, -1.0f,  3.0f,  3.0f, -1.0f, -1.0f,  3.0f,  3.0f, -1.0f, -1.0f,  3.0f,  3.0f, -1.0f, -1.0f,  3.0f,  3.0f, -1.0f, -1.0f,  3.0f,  3.0f, -1.0f, -1.0f,  3.0f,  3.0f, -1.0f, -1.0f,  3.0f,  3.0f,
        0.5f,  0.5f,  0.5f,  0.5f, 0.5f,  0.5f,  0.5f,  0.5f, 0.5f,  0.5f,  0.5f,  0.5f, 0.5f,  0.5f,  0.5f,  0.5f, 0.5f,  0.5f,  0.5f,  0.5f, 0.5f,  0.5f,  0.5f,  0.5f, 0.5f,  0.5f,  0.5f,  0.5f, 0.5f,  0.5f,  0.5f,  0.5f,
        0.5f,  0.5f,  1.5f,  1.5f, 0.5f,  0.5f,  1.5f,  1.5f, 0.5f,  0.5f,  1.5f,  1.5f, 0.5f,  0.5f,  1.5f,  1.5f, 0.5f,  0.5f,  1.5f,  1.5f, 0.5f,  0.5f,  1.5f,  1.5f, 0.5f,  0.5f,  1.5f,  1.5f, 0.5f,  0.5f,  1.5f,  1.5f,
        -1.0f, -1.0f,  2.3f,  2.3f, -1.0f, -1.0f,  2.3f,  2.3f, -1.0f, -1.0f,  2.3f,  2.3f, -1.0f, -1.0f,  2.3f,  2.3f, -1.0f, -1.0f,  2.3f,  2.3f, -1.0f, -1.0f,  2.3f,  2.3f, -1.0f, -1.0f,  2.3f,  2.3f, -1.0f, -1.0f,  2.3f,  2.3f,
        1.0f,  1.0f, -0.4f, -0.4f, 1.0f,  1.0f, -0.4f, -0.4f, 1.0f,  1.0f, -0.4f, -0.4f, 1.0f,  1.0f, -0.4f, -0.4f, 1.0f,  1.0f, -0.4f, -0.4f, 1.0f,  1.0f, -0.4f, -0.4f, 1.0f,  1.0f, -0.4f, -0.4f, 1.0f,  1.0f, -0.4f, -0.4f,
        0.5f,  0.5f,  2.0f,  2.0f, 0.5f,  0.5f,  2.0f,  2.0f, 0.5f,  0.5f,  2.0f,  2.0f, 0.5f,  0.5f,  2.0f,  2.0f, 0.5f,  0.5f,  2.0f,  2.0f, 0.5f,  0.5f,  2.0f,  2.0f, 0.5f,  0.5f,  2.0f,  2.0f, 0.5f,  0.5f,  2.0f,  2.0f,
        2.0f,  2.0f, -4.0f, -4.0f, 2.0f,  2.0f, -4.0f, -4.0f, 2.0f,  2.0f, -4.0f, -4.0f, 2.0f,  2.0f, -4.0f, -4.0f, 2.0f,  2.0f, -4.0f, -4.0f, 2.0f,  2.0f, -4.0f, -4.0f, 2.0f,  2.0f, -4.0f, -4.0f, 2.0f,  2.0f, -4.0f, -4.0f,
        1.5f,  1.5f,  1.0f,  1.0f, 1.5f,  1.5f,  1.0f,  1.0f, 1.5f,  1.5f,  1.0f,  1.0f, 1.5f,  1.5f,  1.0f,  1.0f, 1.5f,  1.5f,  1.0f,  1.0f, 1.5f,  1.5f,  1.0f,  1.0f, 1.5f,  1.5f,  1.0f,  1.0f, 1.5f,  1.5f,  1.0f,  1.0f,
        -0.5f, -0.5f,  3.0f,  3.0f, -0.5f, -0.5f,  3.0f,  3.0f, -0.5f, -0.5f,  3.0f,  3.0f, -0.5f, -0.5f,  3.0f,  3.0f, -0.5f, -0.5f,  3.0f,  3.0f, -0.5f, -0.5f,  3.0f,  3.0f, -0.5f, -0.5f,  3.0f,  3.0f, -0.5f, -0.5f,  3.0f,  3.0f,
    });

    topology topology(input_layout("input", input.get_layout()));

    auto weights = memory::allocate(engine, { data_types::f32, format::bfyx,{ 16, 1, 2, 2 } });
    auto biases = memory::allocate(engine, { data_types::f32, format::bfyx,{ 1, 1, 16, 1 } });

    set_values(weights,
        {
            -2.0f, 0.5f, 3.5f, 1.5f,
            -1.2f, 1.5f, 0.5f, -0.5f,
            -2.0f, 0.5f, 3.5f, 1.5f,
            -1.2f, 1.5f, 0.5f, -0.5f,
            -2.0f, 0.5f, 3.5f, 1.5f,
            -1.2f, 1.5f, 0.5f, -0.5f,
            -2.0f, 0.5f, 3.5f, 1.5f,
            -1.2f, 1.5f, 0.5f, -0.5f,
            -2.0f, 0.5f, 3.5f, 1.5f,
            -1.2f, 1.5f, 0.5f, -0.5f,
            -2.0f, 0.5f, 3.5f, 1.5f,
            -1.2f, 1.5f, 0.5f, -0.5f,
            -2.0f, 0.5f, 3.5f, 1.5f,
            -1.2f, 1.5f, 0.5f, -0.5f,
            -2.0f, 0.5f, 3.5f, 1.5f,
            -1.2f, 1.5f, 0.5f, -0.5f
        }
    );
    set_values(biases, { 2.0f, -1.0f, 2.0f, -1.0f, 2.0f, -1.0f, 2.0f, -1.0f, 2.0f, -1.0f, 2.0f, -1.0f, 2.0f, -1.0f, 2.0f, -1.0f});

    topology.add(
        data("weights", weights),
        data("bias", biases)
    );

    topology.add(
        convolution(
            "conv",
            "input",
            { "weights" },
            { "bias" },
            16,
            { 1,1,2,2 },
            { 0,0,0,0 },
            { 1,1,1,1 })
    );

    network network(engine, topology);
    network.set_input_data("input", input);

    auto outputs = network.execute();
    EXPECT_EQ(outputs.size(), size_t(1));
    EXPECT_EQ(outputs.begin()->first, "conv");

    auto output_prim = outputs.begin()->second.get_memory();

    auto output_ptr = output_prim.pointer<float>();

    std::vector<float> expected_output_vec = {
        8.0f, 8.0f, 3.65f, 3.65f, 8.0f, 8.0f, 3.65f, 3.65f, 8.0f, 8.0f, 3.65f, 3.65f, 8.0f, 8.0f, 3.65f, 3.65f, 8.0f, 8.0f, 3.65f, 3.65f, 8.0f, 8.0f, 3.65f, 3.65f, 8.0f, 8.0f, 3.65f, 3.65f, 8.0f, 8.0f, 3.65f, 3.65f,
        0.5f, 0.5f, -5.36f, -5.36f, 0.5f, 0.5f, -5.36f, -5.36f, 0.5f, 0.5f, -5.36f, -5.36f, 0.5f, 0.5f, -5.36f, -5.36f, 0.5f, 0.5f, -5.36f, -5.36f, 0.5f, 0.5f, -5.36f, -5.36f, 0.5f, 0.5f, -5.36f, -5.36f, 0.5f, 0.5f, -5.36f, -5.36f,
        6.0f, 6.0f, 3.65f, 3.65f, 6.0f, 6.0f, 3.65f, 3.65f, 6.0f, 6.0f, 3.65f, 3.65f, 6.0f, 6.0f, 3.65f, 3.65f, 6.0f, 6.0f, 3.65f, 3.65f, 6.0f, 6.0f, 3.65f, 3.65f, 6.0f, 6.0f, 3.65f, 3.65f, 6.0f, 6.0f, 3.65f, 3.65f,
        9.0f, 9.0f, -5.36f, -5.36f, 9.0f, 9.0f, -5.36f, -5.36f, 9.0f, 9.0f, -5.36f, -5.36f, 9.0f, 9.0f, -5.36f, -5.36f, 9.0f, 9.0f, -5.36f, -5.36f, 9.0f, 9.0f, -5.36f, -5.36f, 9.0f, 9.0f, -5.36f, -5.36f, 9.0f, 9.0f, -5.36f, -5.36f,
    };

    for (unsigned int i = 0; i < expected_output_vec.size(); i++)
    {
        EXPECT_FLOAT_EQ(expected_output_vec[i], output_ptr[i]);
    }
}

TEST(convolution_f32_fw_gpu, basic_wsiz2x2_wstr2x2_in4x4x2x2_nopad_group16_bfyx) {
    //  Test for grouped convolution, there are 16 joined weights and biases (group 16)
    //  data is similar as in basic_wsiz2x2_wstr2x2_in4x4x2x2_nopad_split2_depthwise_sep_opt_bfyx
    engine engine;

    auto input = memory::allocate(engine, { data_types::f32, format::bfyx,{ 2, 16, 4, 4 } });

    set_values(input, {
        -0.5f, 1.0f, 0.5f, 2.0f, 1.5f, -0.5f, 0.0f, -1.0f, 0.5f, 0.5f, -1.0f, 1.0f, 0.5f, 2.0f, 1.5f, -0.5f,
        0.5f, 1.5f, 2.3f, -0.4f, 2.0f, -4.0f, 1.0f, 3.0f, 0.5f, 1.5f, 2.3f, -0.4f, 2.0f, -4.0f, 1.0f, 3.0f,
        -0.5f, 1.0f, 0.5f, 2.0f, 1.5f, -0.5f, 0.0f, -1.0f, 0.5f, 0.5f, -1.0f, 1.0f, 0.5f, 2.0f, 1.5f, -0.5f,
        0.5f, 1.5f, 2.3f, -0.4f, 2.0f, -4.0f, 1.0f, 3.0f, 0.5f, 1.5f, 2.3f, -0.4f, 2.0f, -4.0f, 1.0f, 3.0f,
        -0.5f, 1.0f, 0.5f, 2.0f, 1.5f, -0.5f, 0.0f, -1.0f, 0.5f, 0.5f, -1.0f, 1.0f, 0.5f, 2.0f, 1.5f, -0.5f,
        0.5f, 1.5f, 2.3f, -0.4f, 2.0f, -4.0f, 1.0f, 3.0f, 0.5f, 1.5f, 2.3f, -0.4f, 2.0f, -4.0f, 1.0f, 3.0f,
        -0.5f, 1.0f, 0.5f, 2.0f, 1.5f, -0.5f, 0.0f, -1.0f, 0.5f, 0.5f, -1.0f, 1.0f, 0.5f, 2.0f, 1.5f, -0.5f,
        0.5f, 1.5f, 2.3f, -0.4f, 2.0f, -4.0f, 1.0f, 3.0f, 0.5f, 1.5f, 2.3f, -0.4f, 2.0f, -4.0f, 1.0f, 3.0f,
        -0.5f, 1.0f, 0.5f, 2.0f, 1.5f, -0.5f, 0.0f, -1.0f, 0.5f, 0.5f, -1.0f, 1.0f, 0.5f, 2.0f, 1.5f, -0.5f,
        0.5f, 1.5f, 2.3f, -0.4f, 2.0f, -4.0f, 1.0f, 3.0f, 0.5f, 1.5f, 2.3f, -0.4f, 2.0f, -4.0f, 1.0f, 3.0f,
        -0.5f, 1.0f, 0.5f, 2.0f, 1.5f, -0.5f, 0.0f, -1.0f, 0.5f, 0.5f, -1.0f, 1.0f, 0.5f, 2.0f, 1.5f, -0.5f,
        0.5f, 1.5f, 2.3f, -0.4f, 2.0f, -4.0f, 1.0f, 3.0f, 0.5f, 1.5f, 2.3f, -0.4f, 2.0f, -4.0f, 1.0f, 3.0f,
        -0.5f, 1.0f, 0.5f, 2.0f, 1.5f, -0.5f, 0.0f, -1.0f, 0.5f, 0.5f, -1.0f, 1.0f, 0.5f, 2.0f, 1.5f, -0.5f,
        0.5f, 1.5f, 2.3f, -0.4f, 2.0f, -4.0f, 1.0f, 3.0f, 0.5f, 1.5f, 2.3f, -0.4f, 2.0f, -4.0f, 1.0f, 3.0f,
        -0.5f, 1.0f, 0.5f, 2.0f, 1.5f, -0.5f, 0.0f, -1.0f, 0.5f, 0.5f, -1.0f, 1.0f, 0.5f, 2.0f, 1.5f, -0.5f,
        0.5f, 1.5f, 2.3f, -0.4f, 2.0f, -4.0f, 1.0f, 3.0f, 0.5f, 1.5f, 2.3f, -0.4f, 2.0f, -4.0f, 1.0f, 3.0f,
    });

    topology topology(input_layout("input", input.get_layout()));

    auto weights = memory::allocate(engine, { data_types::f32, format::bfyx,{ 16, 1, 2, 2 } });
    auto biases = memory::allocate(engine, { data_types::f32, format::bfyx,{ 1, 1, 16, 1 } });

    set_values(weights,
        {
            -2.0f, 0.5f, 3.5f, 1.5f,
            -1.2f, 1.5f, 0.5f, -0.5f,
            -2.0f, 0.5f, 3.5f, 1.5f,
            -1.2f, 1.5f, 0.5f, -0.5f,
            -2.0f, 0.5f, 3.5f, 1.5f,
            -1.2f, 1.5f, 0.5f, -0.5f,
            -2.0f, 0.5f, 3.5f, 1.5f,
            -1.2f, 1.5f, 0.5f, -0.5f,
            -2.0f, 0.5f, 3.5f, 1.5f,
            -1.2f, 1.5f, 0.5f, -0.5f,
            -2.0f, 0.5f, 3.5f, 1.5f,
            -1.2f, 1.5f, 0.5f, -0.5f,
            -2.0f, 0.5f, 3.5f, 1.5f,
            -1.2f, 1.5f, 0.5f, -0.5f,
            -2.0f, 0.5f, 3.5f, 1.5f,
            -1.2f, 1.5f, 0.5f, -0.5f
        }
    );

    set_values(biases, { 2.0f, -1.0f, 2.0f, -1.0f, 2.0f, -1.0f, 2.0f, -1.0f, 2.0f, -1.0f, 2.0f, -1.0f, 2.0f, -1.0f, 2.0f, -1.0f});

    topology.add(
            data("weights", weights),
            data("bias", biases)
    );

    topology.add(
        convolution(
            "conv",
            "input",
            { "weights" },
            { "bias" },
            16,
            { 1,1,2,2 },
            { 0,0,0,0 },
            { 1,1,1,1 })
    );

    network network(engine, topology);
    network.set_input_data("input", input);

    auto outputs = network.execute();
    EXPECT_EQ(outputs.size(), size_t(1));
    EXPECT_EQ(outputs.begin()->first, "conv");

    auto output_prim = outputs.begin()->second.get_memory();

    auto output_ptr = output_prim.pointer<float>();

    std::vector<float> expected_output_vec = {
        8.0f, 0.5f,  6.0f,  9.0f, 3.65f,-5.36f, 3.65f, -5.36f,
        8.0f, 0.5f,  6.0f,  9.0f, 3.65f,-5.36f, 3.65f, -5.36f,
        8.0f, 0.5f,  6.0f,  9.0f, 3.65f,-5.36f, 3.65f, -5.36f,
        8.0f, 0.5f,  6.0f,  9.0f, 3.65f,-5.36f, 3.65f, -5.36f,
        8.0f, 0.5f,  6.0f,  9.0f, 3.65f,-5.36f, 3.65f, -5.36f,
        8.0f, 0.5f,  6.0f,  9.0f, 3.65f,-5.36f, 3.65f, -5.36f,
        8.0f, 0.5f,  6.0f,  9.0f, 3.65f,-5.36f, 3.65f, -5.36f,
        8.0f, 0.5f,  6.0f,  9.0f, 3.65f,-5.36f, 3.65f, -5.36f,
    };

    for (unsigned int i = 0; i < expected_output_vec.size(); i++)
    {
        EXPECT_FLOAT_EQ(expected_output_vec[i], output_ptr[i]);
    }
}

TEST(convolution_f32_fw_gpu, basic_wsiz1x1_wstr2x2_in1x1x4x1_nopad_split2) {
    //  Filter : 1x1
    //  Stride : 2x2
    //  Input  : 1x1x4
    //  Output : 1x1x4
    //
    //  Input:
    //  f0:  1.5
    //  f1:  0.5
    //
    //  f2:  0.0
    //  f3: -0.5
    //
    //
    //  Filter1:
    //  -2 -0.5  ofm=0
    //   1  2    ofm=1 
    //  Bias1:
    //   1  5
    //
    //  Filter2:
    //   4  1.5  ofm=0
    //   2  0.5  ofm=1
    //
    //  Bias2:
    //  -1  2.5
    //
    //  Output:
    //  -2.25  
    //   7.5
    //
    //  -1.75
    //   2.25

    const auto& engine = get_test_engine();

    auto input = memory::allocate(engine, { data_types::f32, format::yxfb, { 1, 4, 1, 1 } });
    //auto output = memory::allocate({ memory::format::yxfb_f32,{ 1,{ 1, 1 }, 4 } });
    auto weights1 = memory::allocate(engine, { data_types::f32, format::bfyx, { 2, 2, 1, 1 } });
    auto biases1 = memory::allocate(engine, { data_types::f32, format::bfyx, { 1, 1, 2, 1 } });
    auto weights2 = memory::allocate(engine, { data_types::f32, format::bfyx, { 2, 2, 1, 1 } });
    auto biases2 = memory::allocate(engine, { data_types::f32, format::bfyx, { 1, 1, 2, 1 } });

    set_values(input, {
       1.5f, 0.5f, 0.0f, -0.5f
    });
    set_values(weights1, { -2.0f, -0.5f, 1.0f, 2.0f });
    set_values(biases1, { 1.0f, 5.0f });
    set_values(weights2, { 4.0f, 1.5f, 2.0f, 0.5f });
    set_values(biases2, { -1.0f, 2.5f });

    topology topology(
        input_layout("input", input.get_layout()),
        data("weights1", weights1),
        data("biases1", biases1),
        data("weights2", weights2),
        data("biases2", biases2),
        convolution(
            "conv",
            "input",
            { "weights1", "weights2" },
            { "biases1", "biases2" },
            { 1,1,2,2 },
            { 0,0,0,0 },
            { 1,1,1,1 })
    );

    network network(engine, topology);
    network.set_input_data("input", input);

    auto outputs = network.execute();
    EXPECT_EQ(outputs.size(), size_t(1));
    EXPECT_EQ(outputs.begin()->first, "conv");

    auto output_prim = outputs.begin()->second.get_memory();

    auto output_ptr = output_prim.pointer<float>();

    EXPECT_FLOAT_EQ(-2.25f, get_value<float>(output_ptr, 0));
    EXPECT_FLOAT_EQ(7.5f, get_value<float>(output_ptr, 1));
    EXPECT_FLOAT_EQ(-1.75f, get_value<float>(output_ptr, 2));
    EXPECT_FLOAT_EQ(2.25f, get_value<float>(output_ptr, 3));
}

TEST(convolution_f32_fw_gpu, basic_wsiz1x1_wstr2x2_in1x1x2x1_nopad_split2) {
    //  Filter : 1x1
    //  Stride : 2x2
    //  Input  : 1x1x2
    //  Output : 1x1x4
    //
    //  Input:
    //  f0:  1.5
    //
    //  f1:  0.5
    //
    //  Filter1:
    //  -2  ofm=0
    //   1  ofm=1 
    //  Bias1:
    //   1  5
    //
    //  Filter2:
    //   4  ofm=0
    //   2  ofm=1
    //
    //  Bias2:
    //  -1  2.5
    //
    //  Output:
    //  -2  
    //   6.5
    //
    //   1
    //   3.5


    const auto& engine = get_test_engine();

    auto input = memory::allocate(engine, { data_types::f32, format::yxfb, { 1, 2, 1, 1 } });
    //auto output = memory::allocate({ memory::format::yxfb_f32,{ 1,{ 1, 1 }, 4 } });
    auto weights1 = memory::allocate(engine, { data_types::f32, format::bfyx, { 2, 1, 1, 1 } });
    auto biases1 = memory::allocate(engine, { data_types::f32, format::bfyx, { 1, 1, 2, 1 } });
    auto weights2 = memory::allocate(engine, { data_types::f32, format::bfyx, { 2, 1, 1, 1 } });
    auto biases2 = memory::allocate(engine, { data_types::f32, format::bfyx, { 1, 1, 2, 1 } });

    set_values(input, {
        1.5f, 0.5f
    });
    set_values(weights1, { -2.0f, 1.0f });
    set_values(biases1, { 1.0f, 5.0f });
    set_values(weights2, { 4.0f, 2.0f });
    set_values(biases2, { -1.0f, 2.5f });

    topology topology(
        input_layout("input", input.get_layout()),
        data("weights1", weights1),
        data("biases1", biases1),
        data("weights2", weights2),
        data("biases2", biases2),
        convolution(
            "conv",
            "input",
            { "weights1", "weights2" },
            { "biases1", "biases2" },
            { 1,1,2,2 },
            { 0,0,0,0 },
            { 1,1,1,1 })
    );

    network network(engine, topology);
    network.set_input_data("input", input);

    auto outputs = network.execute();
    EXPECT_EQ(outputs.size(), size_t(1));
    EXPECT_EQ(outputs.begin()->first, "conv");

    auto output_prim = outputs.begin()->second.get_memory();

    auto output_ptr = output_prim.pointer<float>();

    EXPECT_FLOAT_EQ(-2.0f, get_value<float>(output_ptr, 0));
    EXPECT_FLOAT_EQ(6.5f, get_value<float>(output_ptr, 1));
    EXPECT_FLOAT_EQ(1.0f, get_value<float>(output_ptr, 2));
    EXPECT_FLOAT_EQ(3.5f, get_value<float>(output_ptr, 3));
}

TEST(convolution_f32_fw_gpu, basic_wsiz1x1_wstr2x2_in1x1x4x1_filter_1x3x2x1x1_nopad_split2) {
    //  Filter : 1x1
    //  Stride : 2x2
    //  Input  : 1x1x4
    //  Output : 1x1x6
    //
    //  Input:
    //  f0:  1.5
    //  f1:  0.5
    //
    //  f2:  2
    //  f3: -1.0
    //
    //  Filter1:
    //  -2   1   ofm=0
    //   1   3   ofm=1
    //   0.5 8   ofm=2
    //  Bias1:
    //   1   5   3
    //
    //  Filter2:
    //   4  -4   ofm=0
    //   2   0.5 ofm=1
    //  -0.5 3   ofm=2
    //
    //  Bias2:
    //  -1   2.5 2
    //
    //  Output:
    //  -1.5  
    //   8
    //   7.75
    //
    //   11
    //   6
    //  -2


    const auto& engine = get_test_engine();

    auto input = memory::allocate(engine, { data_types::f32, format::yxfb, { 1, 4, 1, 1 } });
    //auto output = memory::allocate({ memory::format::yxfb_f32,{ 1,{ 1, 1 }, 6 } });
    auto weights1 = memory::allocate(engine, { data_types::f32, format::bfyx, { 3, 2, 1, 1 } });
    auto biases1 = memory::allocate(engine, { data_types::f32, format::bfyx, { 1, 1, 3, 1 } });
    auto weights2 = memory::allocate(engine, { data_types::f32, format::bfyx, { 3, 2, 1, 1 } });
    auto biases2 = memory::allocate(engine, { data_types::f32, format::bfyx, { 1, 1, 3, 1 } });

    set_values(input, {
        1.5f, 0.5f, 2.0f, -1.0f
    });
    set_values(weights1, { -2.0f, 1.0f, 1.0f, 3.0f, 0.5f, 8.0f });
    set_values(biases1, { 1.0f, 5.0f, 3.0f });
    set_values(weights2, { 4.0f, -4.0f, 2.0f, 0.5f, -0.5f, 3.0f });
    set_values(biases2, { -1.0f, 2.5f, 2.0f });

    topology topology(
        input_layout("input", input.get_layout()),
        data("weights1", weights1),
        data("biases1", biases1),
        data("weights2", weights2),
        data("biases2", biases2),
        convolution(
            "conv",
            "input",
            { "weights1", "weights2" },
            { "biases1", "biases2" },
            { 1,1,2,2 },
            { 0,0,0,0 },
            { 1,1,1,1 })
    );

    network network(engine, topology);
    network.set_input_data("input", input);

    auto outputs = network.execute();
    EXPECT_EQ(outputs.size(), size_t(1));
    EXPECT_EQ(outputs.begin()->first, "conv");

    auto output_prim = outputs.begin()->second.get_memory();

    auto output_ptr = output_prim.pointer<float>();

    EXPECT_FLOAT_EQ(-1.5f, get_value<float>(output_ptr, 0));
    EXPECT_FLOAT_EQ(8.0f, get_value<float>(output_ptr, 1));
    EXPECT_FLOAT_EQ(7.75f, get_value<float>(output_ptr, 2));
    EXPECT_FLOAT_EQ(11.0f, get_value<float>(output_ptr, 3));
    EXPECT_FLOAT_EQ(6.0f, get_value<float>(output_ptr, 4));
    EXPECT_FLOAT_EQ(-2.0f, get_value<float>(output_ptr, 5));

}

TEST(convolution_gpu, trivial_convolution_relu) {

    //  Filter : 2x2
    //  Stride : 2x2
    //  Input  : 4x4
    //  Output : 2x2

    //  Input:
    //  -0.5   1     0.5  2
    //   1.5  -0.5   0   -1
    //   0.5   0.5  -1    1
    //   0.5   2     1.5 -0.5
    //
    //  Filter
    //  -2   0.5
    //   3.5 1.5
    //
    //  Bias
    //  -2
    //
    //  Output:
    //  4  0.0
    //  2  5

    const auto& engine = get_test_engine();

    auto input = memory::allocate(engine, { data_types::f32, format::yxfb, { 1, 1, 4, 4 } });
    //auto output = memory::allocate({ memory::format::yxfb_f32,{ 1 ,{ 2, 2 }, 1 } });
    auto weights = memory::allocate(engine, { data_types::f32, format::bfyx, { 1, 1, 2, 2 } });
    auto biases = memory::allocate(engine, { data_types::f32, format::bfyx, { 1, 1, 1, 1 } });

    set_values(input, {
        -0.5f,  1.0f,  0.5f,  2.0f,
        1.5f, -0.5f,  0.0f, -1.0f,
        0.5f,  0.5f, -1.0f,  1.0f,
        0.5f,  2.0f,  1.5f, -0.5f
    });
    set_values(weights, { -2.0f, 0.5f, 3.5f, 1.5f });
    set_values(biases, { -2.0f });

    topology topology(
        input_layout("input", input.get_layout()),
        data("weights", weights),
        data("biases", biases),
        convolution(
            "conv",
            "input",
            { "weights" },
            { "biases" },
            { 1,1,2,2 },
            { 0,0,0,0 },
            { 1, 1, 1, 1 },
            true,
            0)
    );

    network network(engine, topology);
    network.set_input_data("input", input);

    auto outputs = network.execute();
    EXPECT_EQ(outputs.size(), size_t(1));
    EXPECT_EQ(outputs.begin()->first, "conv");

    auto output_prim = outputs.begin()->second.get_memory();

    auto output_ptr = output_prim.pointer<float>();

    EXPECT_FLOAT_EQ(4.0f, get_value<float>(output_ptr, 0));
    EXPECT_FLOAT_EQ(0.0f, get_value<float>(output_ptr, 1));
    EXPECT_FLOAT_EQ(2.0f, get_value<float>(output_ptr, 2));
    EXPECT_FLOAT_EQ(5.0f, get_value<float>(output_ptr, 3));
}

TEST(convolution_gpu, relu_with_negative_slope) {

    //  Filter : 2x2
    //  Stride : 2x2
    //  Input  : 4x4
    //  Output : 2x2
    //  Negative Slope : 0.1

    //  Input:
    //  -0.5   1     0.5  2
    //   1.5  -0.5   0   -1
    //   0.5   0.5  -1    1
    //   0.5   2     1.5 -0.5
    //
    //  Filter
    //  -2   0.5
    //   3.5 1.5
    //
    //  Bias
    //  -2
    //
    //  Output:
    //  4  -0.35
    //  2  5

    const auto& engine = get_test_engine();

    auto input = memory::allocate(engine, { data_types::f32, format::yxfb, { 1, 1, 4, 4 } });
    //auto output = memory::allocate({ memory::format::yxfb_f32,{ 1 ,{ 2, 2 }, 1 } });
    auto weights = memory::allocate(engine, { data_types::f32, format::bfyx, { 1, 1, 2, 2 } });
    auto biases = memory::allocate(engine, { data_types::f32, format::bfyx, { 1, 1, 1, 1 } });

    set_values(input, {
        -0.5f,  1.0f,  0.5f,  2.0f,
        1.5f, -0.5f,  0.0f, -1.0f,
        0.5f,  0.5f, -1.0f,  1.0f,
        0.5f,  2.0f,  1.5f, -0.5f
    });
    set_values(weights, { -2.0f, 0.5f, 3.5f, 1.5f });
    set_values(biases, { -2.0f });

    topology topology(
        input_layout("input", input.get_layout()),
        data("weights", weights),
        data("biases", biases),
        convolution(
            "conv",
            "input",
            { "weights" },
            { "biases" },
            { 1,1,2,2 },
            { 0,0,0,0 },
            { 1, 1, 1, 1 },
            true,
            0.1f)
    );

    network network(engine, topology);
    network.set_input_data("input", input);

    auto outputs = network.execute();
    EXPECT_EQ(outputs.size(), size_t(1));
    EXPECT_EQ(outputs.begin()->first, "conv");

    auto output_prim = outputs.begin()->second.get_memory();

    auto output_ptr = output_prim.pointer<float>();

    EXPECT_FLOAT_EQ(4.0f, get_value<float>(output_ptr, 0));
    EXPECT_FLOAT_EQ(-0.35f, get_value<float>(output_ptr, 1));
    EXPECT_FLOAT_EQ(2.0f, get_value<float>(output_ptr, 2));
    EXPECT_FLOAT_EQ(5.0f, get_value<float>(output_ptr, 3));
}

TEST(convolution_gpu, DISABLED_two_1x1_kernels_after_each_other) {

    const auto& engine = get_test_engine();

    extern const std::vector<float> conv_1x1_output;

    auto input = memory::allocate(engine, { data_types::f32, format::bfyx,{ 16, 8, 16, 16 } });
    auto weights_conv_1 = memory::allocate(engine, { data_types::f32, format::bfyx,{ 8, 8, 1, 1 } });
    auto weights_conv_2 = memory::allocate(engine, { data_types::f32, format::bfyx,{ 1, 8, 1, 1 } });

    set_random_values<float>(input);
    set_random_values<float>(weights_conv_1);
    set_random_values<float>(weights_conv_2);

    auto inp_lay = input_layout("input", input.get_layout());
    auto conv_1 = convolution(
        "conv_1",
        "input",
        { "weights_conv_1" });
    auto conv_2 = convolution(
        "conv_2",
        "conv_1",
        { "weights_conv_2" });

    topology topology(
        inp_lay,
        data("weights_conv_1", weights_conv_1),
        conv_1,
        data("weights_conv_2", weights_conv_2),
        conv_2
    );

    build_options bo;
    bo.set_option(build_option::optimize_data(true));
    network network(engine, topology, bo);
    network.set_input_data("input", input);

    auto outputs = network.execute();
    EXPECT_EQ(outputs.size(), size_t(1));

    auto output_prim = outputs.at("conv_2").get_memory();

    auto output_ptr = output_prim.pointer<float>();
    auto output_layout = output_prim.get_layout();
    
    int y_size = output_layout.size.spatial[1];
    int x_size = output_layout.size.spatial[0];
    int f_size = output_layout.size.feature[0];
    int b_size = output_layout.size.batch[0];
    int f_offset = y_size * x_size;
    int b_offset = f_size * f_offset;
    for (int b = 0; b < b_size; ++b)
    {
        for (int f = 0; f < f_size; ++f)
        {
            for (int y = 0; y < y_size; ++y)
            {
                for (int x = 0; x < x_size; ++x)
                {
                    int idx = b * b_offset + f * f_offset + y * x_size + x;
                    EXPECT_TRUE(are_equal(conv_1x1_output[idx], get_value<float>(output_ptr, idx)));
                }
            }
        }
    }
}

TEST(convolution_gpu, basic_yxfb_4_4_yxfb_2_2_b16_if2_of16_st2_2_p0_sp1_fp32)
{
#define USE_OLD_WEIGHTS_FORMAT 0

    const auto input_format   = format::yxfb;
#if USE_OLD_WEIGHTS_FORMAT
    const auto weights_format = format::bfyx;
#else
    const auto weights_format = format::yxfb;
#endif
    const auto biases_format = format::bfyx;

    const int32_t batch_size = 16;
    const int32_t input_feature_count = 2;
    const int32_t output_feature_count = 16;

    const int32_t stride_x = 2;
    const int32_t stride_y = 2;

    const int32_t input_x = 4;
    const int32_t input_y = 4;
    const int32_t weights_x = 2;
    const int32_t weights_y = 2;
    const int32_t output_x = (input_x - weights_x) / stride_x + 1;
    const int32_t output_y = (input_y - weights_y) / stride_y + 1;

    const auto& engine = get_test_engine();

    auto input_size = tensor( batch_size, input_feature_count, input_x, input_y );
    auto input = memory::allocate(engine, { data_types::f32, input_format, input_size });
    auto weights_size = tensor( output_feature_count, input_feature_count, weights_x, weights_y );
    auto weights = memory::allocate(engine, { data_types::f32, weights_format, weights_size });
    auto biases = memory::allocate(engine, { data_types::f32, biases_format, {1,1,output_feature_count,1}});

    //auto output = memory::allocate({output_format, {batch_size, {output_x, output_y}, output_feature_count}});


    // input:
    std::vector<float> input_vals_template {
        0.25f, 0.50f, 0.75f, 1.00f,
        1.25f, 1.50f, 1.75f, 2.00f,
        2.25f, 2.50f, 2.75f, 3.00f,
        3.25f, 3.50f, 3.75f, 4.00f,
    };
    input_vals_template.resize(input_y * input_x);

    std::vector<float> input_vals;
    input_vals.reserve(input_y * input_x * input_feature_count * batch_size);
    for (uint32_t yxi = 0; yxi < input_y * input_x; ++yxi)
    {
        for (uint32_t ifi = 0; ifi < input_feature_count; ++ifi)
        {
            for (uint32_t bi = 0; bi < batch_size; ++bi)
            {
                input_vals.push_back((bi * input_feature_count + ifi + 1) * input_vals_template[yxi]);
            }
        }
    }
    set_values(input, input_vals);


    // weights:
    std::vector<float> weights_vals_template {
        -4.0f, -2.0f,
         4.0f,  4.0f,
    };
    weights_vals_template.resize(weights_y * weights_x);

    std::vector<float> weights_vals;
    weights_vals.reserve(weights_y * weights_x * input_feature_count * output_feature_count);
#if USE_OLD_WEIGHTS_FORMAT
    for (uint32_t ofi = 0; ofi < output_feature_count; ++ofi)
    {
        for (uint32_t ifi = 0; ifi < input_feature_count; ++ifi)
        {
            for (uint32_t yxi = 0; yxi < weights_y * weights_x; ++yxi)
            {
                weights_vals.push_back((ofi * input_feature_count + ifi + 1) * weights_vals_template[yxi]);
            }
        }
    }
#else
    for (uint32_t yxi = 0; yxi < weights_y * weights_x; ++yxi)
    {
        for (uint32_t ifi = 0; ifi < input_feature_count; ++ifi)
        {
            for (uint32_t ofi = 0; ofi < output_feature_count; ++ofi)
            {
                weights_vals.push_back((ofi * input_feature_count + ifi + 1) * weights_vals_template[yxi]);
            }
        }
    }
#endif
    set_values(weights, weights_vals);


    // biases:
    std::vector<float> biases_vals;
    biases_vals.reserve(output_feature_count);
    for (uint32_t ofi = 0; ofi < output_feature_count; ++ofi)
    {
        biases_vals.push_back(ofi * 1.0f);
    }
    set_values(biases, biases_vals);


    // output:
    std::vector<float> output_vals_template {
         9.0f, 10.0f,
        13.0f, 14.0f,
    };
    output_vals_template.resize(output_y * output_x);

    std::vector<float> output_vals;
    output_vals.reserve(output_y * output_x * output_feature_count * batch_size);
    for (uint32_t yxi = 0; yxi < output_y * output_x; ++yxi)
    {
        for (uint32_t ofi = 0; ofi < output_feature_count; ++ofi)
        {
            for (uint32_t bi = 0; bi < batch_size; ++bi)
            {
                uint32_t template_factor = input_feature_count * input_feature_count * input_feature_count * bi * ofi +
                    input_feature_count * input_feature_count * (input_feature_count + 1) / 2 * (bi + ofi) +
                    input_feature_count * (input_feature_count + 1) * (2 * input_feature_count + 1) / 6;
                float bias_factor = ofi * 1.0f;

                output_vals.push_back(template_factor * output_vals_template[yxi] + bias_factor);
            }
        }
    }

    // Computing convolution.
    topology topology(
        input_layout("input", input.get_layout()),
        data("weights", weights),
        data("biases", biases),
        convolution(
            "conv",
            "input",
            { "weights" },
            { "biases" },
            { 1,1,stride_x,stride_y },
            { 0,0,0,0 },
            { 1, 1, 1, 1 },
            true,
            0.1f)
    );

    network network(engine, topology);
    network.set_input_data("input", input);

    auto outputs = network.execute();
    EXPECT_EQ(outputs.size(), size_t(1));
    EXPECT_EQ(outputs.begin()->first, "conv");

    auto output_prim = outputs.begin()->second.get_memory();

    auto output_ptr = output_prim.pointer<float>();

    // Checking result.
    uint32_t i = 0;
    for (uint32_t yxi = 0; yxi < output_y * output_x; ++yxi)
    {
        for (uint32_t ofi = 0; ofi < output_feature_count; ++ofi)
        {
            for (uint32_t bi = 0; bi < batch_size; ++bi, ++i)
            {
                auto equal = are_equal(output_vals[i], get_value<float>(output_ptr, i));
                EXPECT_TRUE(equal);
                if (!equal)
                {
                    std::cout << "Failed at position (" << yxi << ", output feature = " << ofi << ", batch = " << bi << "): "
                        << output_vals[i] << " != " << get_value<float>(output_ptr, i) << std::endl;
                    return;
                }
            }
        }
    }

#undef USE_OLD_WEIGHTS_FORMAT
}

template<typename T>
void quantize_weights(cldnn::memory& weights, cldnn::memory& w_qf)
{
    using std::abs;

    auto batch_pitch = weights.get_layout().get_pitches().batch[0];
    auto ptr = weights.pointer<T>();
    auto wqf_ptr = w_qf.pointer<float>();
    T max = (T) 0.0f;
    for (int ofm = 0; ofm < weights.get_layout().size.batch[0]; ofm++)
    {
        max = (T) 0.0f;
        for (int w = 0; w < batch_pitch; w++)
            if (max < abs(ptr[ofm* batch_pitch + w]))
                max = abs(ptr[ofm* batch_pitch + w]);
       
        if (max == (T)0)
            max = (T)1; // do not quantize

        for (int w = 0; w < batch_pitch; w++)
            ptr[ofm* batch_pitch + w] = (T)round((float)ptr[ofm* batch_pitch + w] * 127.0f / (float)max);
        wqf_ptr[ofm] = max/127.0f;
    }
}
template<typename T>
void calibrate(const cldnn::memory& output, cldnn::memory& calibrations)
{
    using std::abs;

    auto feature_pitch = output.get_layout().get_pitches().feature[0];
    auto ptr = output.pointer<T>();
    auto calibrations_ptr = calibrations.pointer<float>();
    T max = (T) 0.0f;
    for (int ofm = 0; ofm < output.get_layout().size.feature[0]; ofm++)
    {
        max = (T) 0.0f;
        for (int w = 0; w < feature_pitch; w++)
            if (max < abs(ptr[ofm* feature_pitch + w]))
                max = abs(ptr[ofm* feature_pitch + w]);
        calibrations_ptr[ofm] =  127.0f / max;
    }
}

template<typename T>
T max_abs(const cldnn::memory& mem)
{
    using std::abs;

    T max = (T)0;
    auto ptr = mem.pointer<T>();
    for (auto& a : ptr)
        if (max < abs(a))
            max = abs(a);
    return max;
}

template<typename T>
void apply_calibration_on_weights(cldnn::memory& weights, cldnn::memory& qf)
{
    auto batch_pitch = weights.get_layout().get_pitches().batch[0];
    auto ptr = weights.pointer<T>();
    auto wqf_ptr = qf.pointer<float>();
    tensor w_size = weights.get_layout().size;
    int index = 0;
    for (int ofm = 0; ofm < w_size.batch[0]; ofm++)
        for (int ifm = 0; ifm < w_size.feature[0]; ifm++)
            for (int xy = 0; xy < w_size.spatial[0] * w_size.spatial[1]; xy++)
            {
                ptr[index] = ptr[index] / wqf_ptr[ifm];
                index++;
            }
}

cldnn::memory create_int8_weights(engine engine, cldnn::memory& in_weights)
{
    auto layout = in_weights.get_layout();
    auto out_weights = memory::allocate(engine, { data_types::i8, layout.format, layout.size });
    auto in = in_weights.pointer<float>();
    auto out = out_weights.pointer<char>();
    int indx = 0;
    for (auto& a : in)
        out[indx++] = (char) a;
    return out_weights;
}

void add_primitives(const engine& engine, topology& topology)
{
    auto weights = memory::allocate(engine, { data_types::i8, format::bfyx,{ 2, 1, 3, 2 } });

    std::vector<char> weights_values = { 1, 2, 1, 2, 1, 2, 19, 17, -1, -10, 32, 23 };
    set_values<char>(weights, weights_values);
    cldnn::memory biases = memory::allocate(engine, { data_types::f32, format::bfyx,{ 1, 1, 2, 1 } });
    auto weigths_qfs = memory::allocate(engine, { data_types::f32, format::bfyx,{ 1, 1, 2, 1 } });
    set_values(biases, { 1.0f, -8.0f });

    topology.add(
        data("weights", weights),
        data("biases", biases),
        data("w_qfs", weigths_qfs),
        convolution("conv", "input", { "weights" }, { "biases" }, { 0, 0, 1, 2 }, { 0, 0, 0, 0 }, { 1, 1, 1, 1 }, true));
}

TEST(convolution_f32_fw_gpu, byte_activation) {
    //  Filter : 2x3
    //  Stride : 2x1
    //  Input  : 4x5
    //  Output : 2x3
    //
    //  Input:
    //  1  2  3  4  5
    //  2  2  3  4  6
    //  3  3  3  5  1
    //  1  1  1  1  1
    //
    //  Filter:
    //  1  2  1
    //  2  1  2
    //
    //  19 17 -1
    // -10 32 23
    //
    //  Output:
    // 21  28  39
    // 18  20  20
    //
    // -101 -11 92
    // -114 -116 -78
    //
    //  Bias:
    //  1 -8
    engine_configuration eng_conf(false, false, false, "", "", true, "", "kernels");
    engine engine{ eng_conf };
    auto input = memory::allocate(engine, { data_types::i8, format::bfyx,{ 1, 1, 5, 4 } });
       
    VVVF<char> output_vec = {
        {
            { 11, 0, 15 },
            { 0,  0, 2 }
        },
        {
            { 33, 0, 0 },
            { 0, 0, 0 }
        } };

    build_options opts;
    opts.set_option(build_option::optimize_data(true));
    opts.set_option(build_option::graph_dumps_dir("graph"));

    set_values<char>(input, { 1, 2, -3, 4, -5, 2, -2, 3, -4, 6, -3, 3, -3, 5, -1, -1, -1, -1, -1, -1 });

    topology topology(
        input_layout("input", input.get_layout()));
    add_primitives(engine, topology);
    network network(engine, topology, opts);
    network.set_input_data("input", input);

    auto outputs = network.execute();
    EXPECT_EQ(outputs.begin()->first, "conv");

    auto output_memory = outputs.at("conv").get_memory();
    auto output_layout = output_memory.get_layout();
    auto output_ptr = output_memory.pointer<char>();

    int y_size = output_layout.size.spatial[1];
    int x_size = output_layout.size.spatial[0];
    int f_size = output_layout.size.feature[0];
    int b_size = output_layout.size.batch[0];
    EXPECT_EQ(output_layout.format, format::bfyx);
    EXPECT_EQ(y_size, 2);
    EXPECT_EQ(x_size, 3);
    EXPECT_EQ(f_size, 2);
    EXPECT_EQ(b_size, 1);
    for (int f = 0; f < f_size; f++)
        for (int y = 0; y < y_size; ++y) {
            for (int x = 0; x < x_size; ++x) {
                EXPECT_NEAR(output_vec[f][y][x], ((float)output_ptr[f*y_size*x_size + y * x_size + x]), 3.0f);
            }
        }
}

TEST(convolution_f32_fw_gpu, quantized_convolution_low_prec_single_ofq) {
    //  Filter : 2x3
    //  Stride : 2x1
    //  Input  : 4x5
    //  Output : 2x3
    //
    //  Input:
    //  1  2  3  4  5
    //  2  2  3  4  6
    //  3  3  3  5  1
    //  1  1  1  1  1
    //
    //  Filter:
    //  1  2  1
    //  2  1  2
    //
    //  19 17 -1
    // -10 32 23
    //
    //  Output:
    // 21  28  39
    // 18  20  20
    //
    // -101 -11 92
    // -114 -116 -78
    //
    //  Bias:
    //  1 -8

    const auto& engine = get_test_engine();

    auto input_f = memory::allocate(engine, { data_types::f32, format::bfyx,{ 1, 1, 5, 4 } });
    auto weights_f = memory::allocate(engine, { data_types::f32, format::bfyx,{ 2, 1, 3, 2 } });
    cldnn::memory biases = memory::allocate(engine, { data_types::f32, format::bfyx,{ 1, 1, 2, 1 } });
    auto weigths_qfs = memory::allocate(engine, { data_types::f32, format::bfyx,{ 1, 1, 2, 1 } });

    std::vector<float> weights_values_f = { 1.0, 2.0, 1.0, 2.0, 1.0, 2.0, 19.0, 17.0, -1.0, -10.0, 32.0, 23.0 };
    set_values<float>(input_f, { 1.0, 2.0, 3.0, 4.0, 5.0, 2.0, 2.0, 3.0, 4.0, 6.0, 3.0, 3.0, 3.0, 5.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0 });
    set_values<float>(weights_f, weights_values_f);

    set_values(biases, { 1.0f, -8.0f });
    VVVF<float> output_vec = {
        { 
            { 21.0f, 28.0f, 39.0f },
            { 18.0f, 20.0f, 20.0f }
        },
        { 
            { 155.0f, 245.0f, 348.0f },
            { 142.0f, 140.0f, 178.0f }
        } };

    topology topology_f(
        input_layout("input_f", input_f.get_layout()),
        data("weights_f", weights_f),
        data("biases", biases),
        convolution("conv_f", "input_f", { "weights_f" }, { "biases" }, { 0, 0, 1, 2 }));

    build_options opts;
    opts.set_option(build_option::optimize_data(true));
    network network_f(engine, topology_f, opts);
    network_f.set_input_data("input_f", input_f);

    auto outputs_f = network_f.execute();
    EXPECT_EQ(outputs_f.begin()->first, "conv_f");

    auto output_memory_f = outputs_f.at("conv_f").get_memory();
    auto output_ptr_f = output_memory_f.pointer<float>();

    auto input = memory::allocate(engine, { data_types::i8, format::bfyx,{ 1, 1, 5, 4 } });
    auto weights = memory::allocate(engine, { data_types::i8, format::bfyx,{ 2, 1, 3, 2 } });

    float i_qf = 1.0f;
    auto o_max_abs = max_abs<float>(output_memory_f);
    float o_qf = o_max_abs != 0.0f ? 127.0f / o_max_abs : 127.0f;

    std::vector<char> weights_values = { 1, 2, 1, 2, 1, 2, 19, 17, -1, -10, 32, 23 };
    set_values<char>(input, { 1, 2, 3, 4, 5, 2, 2, 3, 4, 6, 3, 3, 3, 5, 1, 1, 1, 1, 1, 1 });
    set_values<char>(weights, weights_values);
    set_values<float>(weigths_qfs, { 1.0f, 1.0f });
    quantize_weights<char>(weights, weigths_qfs);

    topology topology(
        input_layout("input", input.get_layout()),
        data("weights", weights),
        data("biases", biases),
        data("w_qfs",weigths_qfs),
        convolution("conv", "input", { "weights" }, { "biases" }, { "w_qfs" },i_qf, o_qf, { 0, 0, 1, 2 }));

    network network(engine, topology, opts);
    network.set_input_data("input", input);

    auto outputs = network.execute();
    EXPECT_EQ(outputs.begin()->first, "conv");

    auto output_memory = outputs.at("conv").get_memory();
    auto output_layout = output_memory.get_layout();
    auto output_ptr = output_memory.pointer<char>();

    int y_size = output_layout.size.spatial[1];
    int x_size = output_layout.size.spatial[0];
    int f_size = output_layout.size.feature[0];
    int b_size = output_layout.size.batch[0];
    EXPECT_EQ(output_layout.format, format::bfyx);
    EXPECT_EQ(y_size, 2);
    EXPECT_EQ(x_size, 3);
    EXPECT_EQ(f_size, 2);
    EXPECT_EQ(b_size, 1);
    for (int f = 0; f < f_size; f++)
        for (int y = 0; y < y_size; ++y) {
            for (int x = 0; x < x_size; ++x) {
                EXPECT_NEAR(output_vec[f][y][x], ((float)output_ptr[f*y_size*x_size + y * x_size + x]) / o_qf, 3.0f);
            }
        }
}


TEST(convolution_f32_fw_gpu, quantized_convolution_high_prec_calib_per_ofm) {
    //  Filter : 2x3
    //  Stride : 2x1
    //  Input  : 4x5
    //  Output : 2x3
    //
    //  Input:
    //  1  2  3  4  5
    //  2  2  3  4  6
    //  3  3  3  5  1
    //  1  1  1  1  1
    //
    //  Filter:
    //  1  2  1
    //  2  1  2
    //
    //  19 17 -1
    // -10 32 23
    //
    //  Output:
    // 21  28  39
    // 18  20  20
    //
    // -101 -11 92
    // -114 -116 -78
    //
    //  Bias:
    //  1 -8
    const auto& engine = get_test_engine();

    auto input_f = memory::allocate(engine, { data_types::f32, format::bfyx,{ 1, 1, 5, 4 } });
    auto weights_f = memory::allocate(engine, { data_types::f32, format::bfyx,{ 2, 1, 3, 2 } });
    cldnn::memory biases = memory::allocate(engine, { data_types::f32, format::bfyx,{ 1, 1, 2, 1 } });
    auto weigths_qfs = memory::allocate(engine, { data_types::f32, format::bfyx,{ 1, 1, 2, 1 } });
    auto output_calibrations = memory::allocate(engine, { data_types::f32, format::bfyx,{ 1, 1, 2, 1 } });

    std::vector<float> weights_values_f = { 1.0, 2.0, 1.0, 2.0, 1.0, 2.0, 19.0, 17.0, -1.0, -10.0, 32.0, 23.0 };
    set_values<float>(input_f, { 1.0, 2.0, 3.0, 4.0, 5.0, 2.0, 2.0, 3.0, 4.0, 6.0, 3.0, 3.0, 3.0, 5.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0 });
    set_values<float>(weights_f, weights_values_f);
    set_values(biases, { 1.0f, -8.0f });
    VVVF<float> output_vec = {
        {
            { 21.0f, 28.0f, 39.0f },
            { 18.0f, 20.0f, 20.0f }
        },
        {
            { 155.0f, 245.0f, 348.0f },
            { 142.0f, 140.0f, 178.0f }
        } };

    topology topology_f(
        input_layout("input_f", input_f.get_layout()),
        data("weights_f", weights_f),
        data("biases", biases),
        convolution("conv_f", "input_f", { "weights_f" }, { "biases" }, { 0, 0, 1, 2 }));

    build_options opts;
    opts.set_option(build_option::optimize_data(true));
    network network_f(engine, topology_f, opts);
    network_f.set_input_data("input_f", input_f);

    auto outputs_f = network_f.execute();
    EXPECT_EQ(outputs_f.begin()->first, "conv_f");

    auto output_memory_f = outputs_f.at("conv_f").get_memory();
    auto output_ptr_f = output_memory_f.pointer<float>();
    
    auto input = memory::allocate(engine, { data_types::i8, format::bfyx,{ 1, 1, 5, 4 } });
    auto weights = memory::allocate(engine, { data_types::i8, format::bfyx,{ 2, 1, 3, 2 } });
    float i_qf = 1.0f; 

    std::vector<char> weights_values = { 1, 2, 1, 2, 1, 2, 19, 17, -1, -10, 32, 23 };
    set_values<char>(input, { 1, 2, 3, 4, 5, 2, 2, 3, 4, 6, 3, 3, 3, 5, 1, 1, 1, 1, 1, 1 });
    set_values<char>(weights, weights_values);
    set_values<float>(weigths_qfs, { 1.0f, 1.0f });
    quantize_weights<char>(weights, weigths_qfs);
    calibrate<float>(output_memory_f, output_calibrations);

    topology topology(
        input_layout("input", input.get_layout()),
        data("weights", weights),
        data("biases", biases),
        data("w_qfs", weigths_qfs),
        data("out_calibrations", output_calibrations),
        convolution( "conv", "input", { "weights" }, { "biases" },
                   { "w_qfs" }, { "out_calibrations" }, i_qf, { 0, 0, 1, 2 }));

    network network(engine, topology, opts);
    network.set_input_data("input", input);

    auto outputs = network.execute();
    EXPECT_EQ(outputs.begin()->first, "conv");

    auto output_memory = outputs.at("conv").get_memory();
    auto output_layout = output_memory.get_layout();
    auto output_ptr = output_memory.pointer<char>();
    auto o_qf = output_calibrations.pointer<float>();
    int y_size = output_layout.size.spatial[1];
    int x_size = output_layout.size.spatial[0];
    int f_size = output_layout.size.feature[0];
    int b_size = output_layout.size.batch[0];
    EXPECT_EQ(output_layout.format, format::bfyx);
    EXPECT_EQ(y_size, 2);
    EXPECT_EQ(x_size, 3);
    EXPECT_EQ(f_size, 2);
    EXPECT_EQ(b_size, 1);
    for (int f = 0; f < f_size; f++)
        for (int y = 0; y < y_size; ++y) {
            for (int x = 0; x < x_size; ++x) {
                EXPECT_NEAR(output_vec[f][y][x], ((float)output_ptr[f*y_size*x_size + y * x_size + x]) / o_qf[f], 3.0f);
            }
        }
}

template <typename InputTy, typename OutputTy, typename PreActivationTy = int32_t>
class ConvQuantizedTest : public testing::Test {
protected:
    std::vector<InputTy> input_values;
    // As for the depthwise convolution, will be processed to a normal
    // convolution later on.
    std::vector<int8_t> weights_values;
    std::vector<int32_t> biases_values;
    std::vector<float> quantization_values;
    std::vector<PreActivationTy> output_pre_relu; // ...but after quantization.

    void add_feature(std::vector<InputTy> input,
                     std::vector<int8_t> weights,
                     int32_t bias,
                     float quantization,
                     std::vector<PreActivationTy> output)
    {
        input_values.insert(input_values.end(), input.begin(), input.end());
        weights_values.insert(
            weights_values.end(), weights.begin(), weights.end());
        biases_values.push_back(bias);
        quantization_values.push_back(quantization);
        output_pre_relu.insert(
            output_pre_relu.end(), output.begin(), output.end());
    }

    template <typename T>
    static T pre_relu_to_output(T pre_relu) {
      // No std::clamp before C++17 :(
      return std::min(
          static_cast<T>(std::numeric_limits<OutputTy>::max()),
          std::max(static_cast<T>(std::numeric_limits<OutputTy>::lowest()),
                   std::max(static_cast<T>(0), pre_relu)));
    }

    void do_test()
    {
        const auto& engine = get_test_engine();
        int n_features = static_cast<int>(biases_values.size());

        auto input_shape = tensor(1, n_features, 4, 1);
        auto weights_shape = tensor(n_features, n_features, 3, 1);
        auto biases_shape = tensor(1, 1, n_features, 1);

        auto input = memory::allocate(
            engine,
            {type_to_data_type<InputTy>::value, format::bfyx, input_shape});
        auto weights = memory::allocate(
            engine, {data_types::i8, format::bfyx, weights_shape});

        auto biases = memory::allocate(
            engine, {data_types::i32, format::bfyx, biases_shape});
        auto quantization = memory::allocate(
            engine, {data_types::f32, format::bfyx, biases_shape});

        set_values(input, input_values);
        std::vector<int8_t> post_processed_weights_values(n_features
                                                          * n_features * 3);
        for (int output_feature = 0; output_feature < n_features; ++output_feature)
            for (int input_feature = 0; input_feature < n_features;
                 ++input_feature)
                for (int x = 0; x < 3; ++x)
                {
                    int idx =
                        output_feature * n_features * 3 + input_feature * 3 + x;
                    if (input_feature == output_feature)
                        post_processed_weights_values[idx] =
                            weights_values[input_feature * 3 + x];
                    else
                        post_processed_weights_values[idx] = 0;
                }
        set_values(weights, post_processed_weights_values);
        set_values(biases, biases_values);
        set_values(quantization, quantization_values);

        build_options opts;
        opts.set_option(build_option::optimize_data(false));
        opts.set_option(build_option::graph_dumps_dir("/tmp/cldnn_dumps/"));

        topology topology(input_layout("input", input.get_layout()),
                          data("weights", weights),
                          data("biases", biases),
                          data("quantization", quantization),
                          convolution("conv",
                                      "input",
                                      {"weights"},
                                      {"biases"},
                                      {"quantization"},
                                      type_to_data_type<OutputTy>::value,
                                      {1, 1, 1, 1},
                                      {0, 0, 0, 0},
                                      {1, 1, 1, 1},
                                      true));

        network network(engine, topology, opts);
        network.set_input_data("input", input);

        auto outputs = network.execute();

        auto output_memory = outputs.at("conv").get_memory();
        auto output_layout = output_memory.get_layout();
        auto output_ptr = output_memory.pointer<OutputTy>();
        int y_size = output_layout.size.spatial[1];
        int x_size = output_layout.size.spatial[0];
        int f_size = output_layout.size.feature[0];
        int b_size = output_layout.size.batch[0];
        EXPECT_EQ(output_layout.format, format::bfyx);
        EXPECT_EQ(y_size, 1);
        EXPECT_EQ(x_size, 2);
        EXPECT_EQ(f_size, n_features);
        EXPECT_EQ(b_size, 1);

        for (int f = 0; f < f_size; f++)
            for (int x = 0; x < x_size; ++x)
            {
                // printf("f: %d, x: %d\n", f, x);
                PreActivationTy expected = pre_relu_to_output(output_pre_relu[f * x_size + x]);
                auto actual = static_cast<PreActivationTy>(output_ptr[f * x_size + x]);
                if (std::is_floating_point<PreActivationTy>::value)
                {
                    EXPECT_NEAR(expected, actual, 0.001f);
                }
                else if (std::is_integral<PreActivationTy>::value)
                {
                    EXPECT_EQ(expected, actual);
                }
            }
    }
};

class ConvQuantizedTest_i8_to_u8 : public ConvQuantizedTest<int8_t, uint8_t>
{};

TEST_F(ConvQuantizedTest_i8_to_u8, basic) {
    // Check that the output precision is `u8` indeed.
    add_feature({125, 125, 0, 1}, {2, 0, 1}, 1, 1.0f, {251, 252});

    // Check ReLU (negative result will become zero in the output).
    add_feature({0, 50, 0, -50}, {0, 4, 4}, 1, 1.0f, {201, -199});

    // Same but with non-unit calibration (just in case).
    add_feature({0, 50, 0, -50}, {0, 8, 8}, 2, 0.5f, {201, -199});

    // Something with intermediate accumulator outside i8/u8 range.
    add_feature({120, 120, 120, -120}, {1, 1, 1}, 0, 0.25f, {90, 30});

    // Check rounding (TODO: currently rounding to nearest, with half rounded
    // away from zero, might need to change that).
    add_feature({125, 125, 0, 126}, {1, 1, 1}, 1, 0.5f, {126, 126});
    add_feature({125, 125, 0, 126}, {1, 1, 1}, 2, 0.5f, {126, 127});
    // Same, but with output outside the i8 range.
    add_feature({125, 125, 0, 126}, {1, 1, 1}, 21, 0.5f, {136, 136});

    // Check saturation.
    add_feature({0, 50, 0, -50}, {0, 8, 8}, 2, 1.0f, {402, -398});

    do_test();
}

class ConvQuantizedTest_u8_to_u8 : public ConvQuantizedTest<uint8_t, uint8_t>
{};

TEST_F(ConvQuantizedTest_u8_to_u8, basic) {
    // Start with the tests from the "i8" input case (move negative sign to the
    // weights were needed):
    //
    // Check that the output precision is `u8` indeed.
    add_feature({125, 125, 0, 1}, {2, 0, 1}, 1, 1.0f, {251, 252});

    // Check ReLU (negative result will become zero in the output).
    add_feature({0, 50, 0, 50}, {0, 4, -4}, 1, 1.0f, {201, -199});

    // Same but with non-unit calibration (just in case).
    add_feature({0, 50, 0, 50}, {0, 8, -8}, 2, 0.5f, {201, -199});

    // Something with intermediate accumulator outside i8/u8 range.
    add_feature({240, 240, 240, 240}, {2, 1, -1}, 0, 0.125f, {60, 60});

    // Check rounding (TODO: currently rounding to nearest, with half rounded
    // away from zero, might need to change that).
    add_feature({125, 125, 0, 126}, {1, 1, 1}, 1, 0.5f, {126, 126});
    add_feature({125, 125, 0, 126}, {1, 1, 1}, 2, 0.5f, {126, 127});
    // Same, but with output outside the i8 range.
    add_feature({125, 125, 0, 126}, {1, 1, 1}, 21, 0.5f, {136, 136});

    // Check saturation.
    add_feature({0, 50, 0, 50}, {0, 8, -8}, 2, 1.0f, {402, -398});

    // Now, something "u8"-input-specific (basically subset of the tests above
    // but move the scaling from the weights to the input):
    add_feature({250, 250, 0, 1}, {1, 0, 1}, 1, 1.0f, {251, 252});
    add_feature({0, 200, 0, 200}, {0, 1, -1}, 1, 1.0f, {201, -199});
    add_feature({0, 200, 0, 200}, {0, 2, -2}, 2, 0.5f, {201, -199});

    do_test();
}

class ConvQuantizedTest_u8_to_i8 : public ConvQuantizedTest<uint8_t, int8_t>
{};

TEST_F(ConvQuantizedTest_u8_to_i8, basic) {
    // Basic test + rounding
    add_feature({125, 125, 0, 1}, {2, 0, 1}, 1, 0.5f, {126, 126});

    // Test proper clamping to the output i8 range.
    add_feature({125, 125, 0, 1}, {2, 0, 1}, 1, 1.0f, {251, 252});

    // Test ReLU by having negative number pre-ReLU.
    add_feature({0, 50, 0, 50}, {0, 1, -1}, 1, 1.0f, {51, -49});

    do_test();
}

class ConvQuantizedTest_i8_to_float : public ConvQuantizedTest<int8_t, float, float>
{};

TEST_F(ConvQuantizedTest_i8_to_float, basic) {
    // Some basic checks.
    add_feature({125, 125, 0, 1}, {2, 0, 1}, 1, 1.0f, {251.0f, 252.0f});
    add_feature({0, 50, 0, -50}, {0, 8, 8}, 2, 0.5f, {201.0f, -199.0f});
    add_feature({0, 50, 0, -50}, {0, 8, 8}, 2, 1.0f, {402.0f, -398.0f});

    // Check the FP accuracy - no rounding should be performed.
    add_feature({0, 5, 0, -5}, {0, 8, 8}, 0, 1.01f, {40.4f, -40.4f});

    do_test();
}

TEST(convolution_f32_fw_gpu, calibration_advance) {
    //  Filter : 2x3
    //  Stride : 2x1
    //  Input  : 4x5
    //  Output : 2x3
    //
    //  Input:
    //  1  2  3  4  5
    //  2  2  3  4  6
    //  3  3  3  5  1
    //  1  1  1  1  1
    //
    //  Filter1:
    //  1  2  1
    //  2  1  2
    //
    //  1.9 1.7 -1
    // -1.0 3.2 2.3
    //
    //  Filter2:
    // IFM0:
    //  1.5  2.3  -1.0
    //  3  5.6  -1.0
    //
    //  3  5.6  -1.0
    //  1   2   3
    // IFM1:
    //  1.9 1.7 -1
    //  1.9 1.7 -1
    //
    // -1.0 3.2 2.3
    // -1.0 3.2 2.3
    // IFM2:
    //  1 2 -1
    //  2 1 -1
    //
    // -1 2  1
    //  1 2 -1
    //
    //  Output:
    // 313.32  217.43  118.10
    //
    //
    //  Bias1:
    //  1 -8
    //  Bias2:
    //  2  4  0

    const auto& engine = get_test_engine();

    auto input_f = memory::allocate(engine, { data_types::f32, format::bfyx,{ 1, 1, 5, 4 } });
    auto weights_f = memory::allocate(engine, { data_types::f32, format::bfyx,{ 2, 1, 3, 2 } });
    auto biases = memory::allocate(engine, { data_types::f32, format::bfyx,{ 1, 1, 2, 1 } });
    auto w_qf = memory::allocate(engine, { data_types::f32, format::bfyx,{ 1, 1, 2, 1 } });
    auto weights_f_2 = memory::allocate(engine, { data_types::f32, format::bfyx,{ 3, 2, 3, 2 } });
    auto biases_2 = memory::allocate(engine, { data_types::f32, format::bfyx,{ 1, 1, 3, 1 } });
    auto w_qf_2 = memory::allocate(engine, { data_types::f32, format::bfyx,{ 1, 1, 3, 1 } });

    std::vector<float> weights_values_f = { 1.0f, 2.0f, 1.0f, 2.0f, 1.0f, 2.0f, 1.9f, 1.7f, -1.0f, -1.0f, 3.2f, 2.3f };
    std::vector<float> weights_values_f_2 = { 
        1.5f, 2.3f, -1.0f, 3.0f, 5.6f, -1.0f,
        3.0f, 5.6f, -1.0f, 1.0f, 2.0f, 3.0f,
        
        1.9f, 1.7f, -1.0f, 1.9f, 1.7f, -1.0f,
        -1.0f, 3.2f, 2.3f, -1.0f, 3.2f, 2.3f,

        1.0f, 2.0f, -1.0f, 2.0f, 1.0f, -1.0f,
       -1.0f, 2.0f,  1.0f, 1.0f, 2.0f, -1.0f,};

    set_values<float>(input_f, { 1.0, 2.0, 3.0, 4.0, 5.0, 2.0, 2.0, 3.0, 4.0, 6.0, 3.0, 3.0, 3.0, 5.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0 });
    set_values<float>(weights_f, weights_values_f);
    set_values<float>(weights_f_2, weights_values_f_2);
    set_values(biases, { 1.0f, -8.0f });
    set_values(biases_2, { 2.0f, 4.0f, 0.0f });

    topology topology_f(
        input_layout("input_f", input_f.get_layout()),
        data("weights_f", weights_f),
        data("biases", biases),
        data("weights_f_2", weights_f_2),
        data("biases_2", biases_2),
        convolution("conv_f", "input_f", { "weights_f" }, { "biases" }, { 0, 0, 1, 2 }),
        convolution("conv_f_2", "conv_f", { "weights_f_2" }, { "biases_2" }, { 0, 0, 1, 1 }));

    build_options opts;
    opts.set_option(build_option::optimize_data(true));
    opts.set_option(build_option::outputs({ "conv_f", "conv_f_2" }));
    network network_f(engine, topology_f, opts);
    network_f.set_input_data("input_f", input_f);

    auto outputs_f = network_f.execute();
    auto output_memory_f = outputs_f.at("conv_f").get_memory();
    auto output_memory_f_2 = outputs_f.at("conv_f_2").get_memory();
    auto output_calibrations = memory::allocate(engine, { data_types::f32, format::bfyx,{ 1, 1, 2, 1 } });
    auto output_calibrations_2 = memory::allocate(engine, { data_types::f32, format::bfyx,{ 1, 1, 3, 1 } });

    calibrate<float>(output_memory_f, output_calibrations);
    calibrate<float>(output_memory_f_2, output_calibrations_2);
    apply_calibration_on_weights<float>(weights_f_2, output_calibrations);
    quantize_weights<float>(weights_f, w_qf);
    quantize_weights<float>(weights_f_2, w_qf_2);

    auto weights   = create_int8_weights(engine, weights_f);
    auto weigths_2 = create_int8_weights(engine, weights_f_2);
    auto input = create_int8_weights(engine, input_f);

    topology topology(
        input_layout("input", input.get_layout()),
        data("weights", weights),
        data("biases", biases),
        data("weights_2", weigths_2),
        data("biases_2", biases_2),
        data("w_qf", w_qf),
        data("w_qf_2", w_qf_2),
        data("calib", output_calibrations),
        data("calib_2", output_calibrations_2),
        convolution("conv", "input", { "weights" }, { "biases" }, { "w_qf" }, { "calib" }, 1.0f, { 0, 0, 1, 2 }),
        convolution("conv_2", "conv", { "weights_2" }, { "biases_2" }, { "w_qf_2" }, { "calib_2" }, 1.0f, { 0, 0, 1, 1 }));

    build_options opts_2;
    opts_2.set_option(build_option::optimize_data(true));
    opts_2.set_option(build_option::outputs({ "conv", "conv_2" }));
    cldnn::network network(engine, topology, opts_2);
    network.set_input_data("input", input);
    auto outputs = network.execute();
    auto output_memory = outputs.at("conv_2").get_memory();
    auto ref_ptr = output_memory_f_2.pointer<float>();
    auto test_ptr = output_memory.pointer<char>();
    auto& out_size = output_memory.get_layout().size;
    auto o_qf = output_calibrations_2.pointer<float>();

    for (int f = 0; f < out_size.feature[0]; f++)
    {
        for (int y = 0; y < out_size.spatial[1]; ++y)
        {
            for (int x = 0; x < out_size.spatial[0]; ++x)
            {
                EXPECT_NEAR(ref_ptr[x + out_size.spatial[0]
                    * (y + out_size.spatial[1] * f)], ((float)test_ptr[x + out_size.spatial[0]
                        * (y + out_size.spatial[1] * f)]) / o_qf[f], 3.0f);
            }
        }
    }

}
TEST(convolution_f32_fw_gpu, local_basic) {
    //  Filter : 3x3x2x2 - local sizes
    //  Stride : 1x1
    //  Input  : 4x4
    //  Output : 3x3 
    //
    //  Input:
    //  1  1  1  1
    //  1  1  1  1
    //  2  2  2  2
    //  2  2  2  2
    //    
    //
    //  Filter:
    //  0 0  1 1  2 2
    //  0 0  1 1  2 2
    //
    //  3 3  4 4  5 5  
    //  3 3  4 4  5 5
    //
    //  6 6  7 7  8 8
    //  6 6  7 7  8 8
    //
    //  Output:
    //  0  4  8
    // 18 24 30
    // 48 56 64
    //

    const auto& engine = get_test_engine();
    tensor local_size = tensor(1,1,2,2,3,3);
    auto input_f = memory::allocate(engine, { data_types::f32, format::bfyx,{ 1, 1, 4, 4 } });
    auto weights_f = memory::allocate(engine, { data_types::f32, format::bf_lyx_yx, local_size });
    cldnn::memory biases = memory::allocate(engine, { data_types::f32, format::bfyx,{ 1, 1, 1, 1 } });

    std::vector<float> weights_values_f = {
        0.0, 0.0, 0.0, 0.0,
        1.0, 1.0, 1.0, 1.0,
        2.0, 2.0, 2.0, 2.0,

        3.0, 3.0, 3.0, 3.0,
        4.0, 4.0, 4.0, 4.0,
        5.0, 5.0, 5.0, 5.0,

        6.0, 6.0, 6.0, 6.0,
        7.0, 7.0, 7.0, 7.0,
        8.0, 8.0, 8.0, 8.0,
    };
    set_values<float>(input_f, { 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 2.0, 2.0, 2.0, 2.0, 2.0, 2.0, 2.0, 2.0 });
    set_values<float>(weights_f, weights_values_f);
    set_values(biases, { 0.0f });
    std::vector<float> output_vec =
    {
        0.0f, 4.0f, 8.0f,
        18.0f, 24.0f, 30.0f,
        48.0f, 56.0f, 64.0f
    };

    topology topology_f(
        input_layout("input_f", input_f.get_layout()),
        data("weights_f", weights_f),
        data("biases", biases),
        convolution("conv_f", "input_f", { "weights_f" }, { "biases" }, { 0, 0, 1, 1 }));

    build_options opts;
    opts.set_option(build_option::optimize_data(true));
    network network_f(engine, topology_f, opts);
    network_f.set_input_data("input_f", input_f);

    auto outputs_f = network_f.execute();
    EXPECT_EQ(outputs_f.begin()->first, "conv_f");

    auto output_memory_f = outputs_f.at("conv_f").get_memory();
    auto output_ptr_f = output_memory_f.pointer<float>();
    unsigned int cntr = 0;
    for (auto fl : output_ptr_f)
        EXPECT_FLOAT_EQ(fl, output_vec[cntr++]);
}


TEST(convolution_f32_fw_gpu, local_multi_out_features) {
    //  Filter : 3x1x3x3x2x2 - local sizes
    //  Stride : 1x1
    //  Input  : 4x4
    //  Output : 3x3x3 
    //
    //  Input:
    //  1  1  1  1
    //  1  1  1  1
    //  2  2  2  2
    //  2  2  2  2
    //    
    //
    //  Filter:
    //  0 0  1 1  2 2  --- 1 ofm
    //  0 0  1 1  2 2
    //
    //  3 3  4 4  5 5  
    //  3 3  4 4  5 5
    //
    //  6 6  7 7  8 8
    //  6 6  7 7  8 8
    //
    //  0 0  0 0  0 0  --- 2 ofm 
    //  0 0  0 0  0 0
    //
    //  0 0  0 0  0 0
    //  0 0  0 0  0 0
    //
    //  0 0  0 0  0 0
    //  0 0  0 0  0 0
    //
    //  0 0  2 2  4 4 --- 3 ofm
    //  0 0  2 2  4 4 
    //
    //  6 6  8 8  1 1 
    //  6 6  8 8  1 1 
    //       
    //  3 3  5 5  7 7 
    //  3 3  5 5  7 7 
    //

    //
    //  Output:
    //  0  4  8
    // 18 24 30
    // 48 56 64
    //
    //  0  0  0
    //  0  0  0
    //  0  0  0
    //
    //  0  8 16
    // 36 48  6
    // 24 40 56
    //

    const auto& engine = get_test_engine();
    tensor local_size = tensor(3,1,2,2,3,3);
    auto input_f = memory::allocate(engine, { data_types::f32, format::bfyx,{ 1, 1, 4, 4 } });
    auto weights_f = memory::allocate(engine, { data_types::f32, format::bf_lyx_yx, local_size });
    cldnn::memory biases = memory::allocate(engine, { data_types::f32, format::bfyx,{ 1, 1, 3, 1 } });

    std::vector<float> weights_values_f = {
        0.0, 0.0, 0.0, 0.0,
        1.0, 1.0, 1.0, 1.0,
        2.0, 2.0, 2.0, 2.0,

        3.0, 3.0, 3.0, 3.0,
        4.0, 4.0, 4.0, 4.0,
        5.0, 5.0, 5.0, 5.0,

        6.0, 6.0, 6.0, 6.0,
        7.0, 7.0, 7.0, 7.0,
        8.0, 8.0, 8.0, 8.0,

        0.0, 0.0, 0.0, 0.0,
        0.0, 0.0, 0.0, 0.0,
        0.0, 0.0, 0.0, 0.0,

        0.0, 0.0, 0.0, 0.0,
        0.0, 0.0, 0.0, 0.0,
        0.0, 0.0, 0.0, 0.0,

        0.0, 0.0, 0.0, 0.0,
        0.0, 0.0, 0.0, 0.0,
        0.0, 0.0, 0.0, 0.0,

        0.0, 0.0, 0.0, 0.0,
        2.0, 2.0, 2.0, 2.0,
        4.0, 4.0, 4.0, 4.0,

        6.0, 6.0, 6.0, 6.0,
        8.0, 8.0, 8.0, 8.0,
        1.0, 1.0, 1.0, 1.0,

        3.0, 3.0, 3.0, 3.0,
        5.0, 5.0, 5.0, 5.0,
        7.0, 7.0, 7.0, 7.0,
    };
    set_values<float>(input_f, { 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 2.0, 2.0, 2.0, 2.0, 2.0, 2.0, 2.0, 2.0 });
    set_values<float>(weights_f, weights_values_f);
    set_values(biases, { 0.0f, 0.0f, 0.0f });
    std::vector<float> output_vec =
    {
        0.0f,  4.0f,  8.0f,
        18.0f, 24.0f, 30.0f,
        48.0f, 56.0f, 64.0f,

        0.0f,  0.0f, 0.0f,
        0.0f,  0.0f, 0.0f,
        0.0f,  0.0f, 0.0f,

        0.0f,  8.0f, 16.0f,
        36.0f, 48.0f,  6.0f,
        24.0f, 40.0f, 56.0f,
    };

    topology topology_f(
        input_layout("input_f", input_f.get_layout()),
        data("weights_f", weights_f),
        data("biases", biases),
        convolution("conv_f", "input_f", { "weights_f" }, { "biases" }, { 0, 0, 1, 1 }));

    build_options opts;
    opts.set_option(build_option::optimize_data(true));
    network network_f(engine, topology_f, opts);
    network_f.set_input_data("input_f", input_f);

    auto outputs_f = network_f.execute();
    EXPECT_EQ(outputs_f.begin()->first, "conv_f");

    auto output_memory_f = outputs_f.at("conv_f").get_memory();
    auto output_ptr_f = output_memory_f.pointer<float>();
    unsigned int cntr = 0;
    for (auto fl : output_ptr_f)
    {
        EXPECT_FLOAT_EQ(fl, output_vec[cntr++]);
    }
}

TEST(convolution_f32_fw_gpu, local_multi_input_features) {
    //  Filter : 1x3x3x3x2x2 - local sizes
    //  Stride : 1x1
    //  Input  : 3x4x4
    //  Output : 3x3 
    //
    //  Input:
    //  0  0  0  0
    //  0  0  0  0
    //  0  0  0  0
    //  0  0  0  0
    //
    //  1  1  1  1
    //  1  1  1  1
    //  1  1  1  1
    //  1  1  1  1
    //
    //  2  2  2  2
    //  2  2  2  2
    //  2  2  2  2
    //  2  2  2  2
    //    
    //
    //  Filter:
    //  0 0  1 1  2 2
    //  0 0  1 1  2 2
    //
    //  3 3  4 4  5 5  
    //  3 3  4 4  5 5
    //
    //  6 6  7 7  8 8
    //  6 6  7 7  8 8
    //
    //  0 0  1 1  2 2
    //  0 0  1 1  2 2
    //
    //  3 3  4 4  5 5  
    //  3 3  4 4  5 5
    //
    //  6 6  7 7  8 8
    //  6 6  7 7  8 8
    //
    //  0 0  1 1  2 2
    //  0 0  1 1  2 2
    //
    //  3 3  4 4  5 5  
    //  3 3  4 4  5 5
    //
    //  6 6  7 7  8 8
    //  6 6  7 7  8 8
    //
    //  Output:
    //  0  4  8
    // 18 24 30
    // 48 56 64
    //

    const auto& engine = get_test_engine();
    tensor local_size = tensor(1,3,2,2,3,3);
    auto input_f = memory::allocate(engine, { data_types::f32, format::bfyx,{ 1, 3, 4, 4 } });
    auto weights_f = memory::allocate(engine, { data_types::f32, format::bf_lyx_yx, local_size });
    cldnn::memory biases = memory::allocate(engine, { data_types::f32, format::bfyx,{ 1, 1, 1, 1 } });

    std::vector<float> weights_values_f = {
        0.0, 0.0, 0.0, 0.0,
        1.0, 1.0, 1.0, 1.0,
        2.0, 2.0, 2.0, 2.0,

        3.0, 3.0, 3.0, 3.0,
        4.0, 4.0, 4.0, 4.0,
        5.0, 5.0, 5.0, 5.0,

        6.0, 6.0, 6.0, 6.0,
        7.0, 7.0, 7.0, 7.0,
        8.0, 8.0, 8.0, 8.0,

        0.0, 0.0, 0.0, 0.0,
        1.0, 1.0, 1.0, 1.0,
        2.0, 2.0, 2.0, 2.0,

        3.0, 3.0, 3.0, 3.0,
        4.0, 4.0, 4.0, 4.0,
        5.0, 5.0, 5.0, 5.0,

        6.0, 6.0, 6.0, 6.0,
        7.0, 7.0, 7.0, 7.0,
        8.0, 8.0, 8.0, 8.0,

        0.0, 0.0, 0.0, 0.0,
        1.0, 1.0, 1.0, 1.0,
        2.0, 2.0, 2.0, 2.0,

        3.0, 3.0, 3.0, 3.0,
        4.0, 4.0, 4.0, 4.0,
        5.0, 5.0, 5.0, 5.0,

        6.0, 6.0, 6.0, 6.0,
        7.0, 7.0, 7.0, 7.0,
        8.0, 8.0, 8.0, 8.0,
    };
    set_values<float>(input_f, {
        0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0,
        1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0,
        2.0, 2.0, 2.0, 2.0, 2.0, 2.0, 2.0, 2.0, 2.0, 2.0, 2.0, 2.0, 2.0, 2.0, 2.0, 2.0 });
    set_values<float>(weights_f, weights_values_f);
    set_values(biases, { 0.0f });
    std::vector<float> output_vec =
    {
        60.0f, 72.0f, 84.0f,
        24.0f, 36.0f, 48.0f,
        24.0f, 36.0f, 48.0f
    };

    topology topology_f(
        input_layout("input_f", input_f.get_layout()),
        data("weights_f", weights_f),
        data("biases", biases),
        convolution("conv_f", "input_f", { "weights_f" }, { "biases" }, { 0, 0, 1, 1 }));

    build_options opts;
    opts.set_option(build_option::optimize_data(true));
    network network_f(engine, topology_f, opts);
    network_f.set_input_data("input_f", input_f);

    auto outputs_f = network_f.execute();
    EXPECT_EQ(outputs_f.begin()->first, "conv_f");

    auto output_memory_f = outputs_f.at("conv_f").get_memory();
    auto output_ptr_f = output_memory_f.pointer<float>();
    unsigned int cntr = 0;
    for (auto fl : output_ptr_f)
        EXPECT_FLOAT_EQ(fl, output_vec[cntr++]);
}


TEST(convolution_gpu, basic_yxfb_4_4_yxfb_2_2_b16_if2_of16_st2_2_p0_sp1_fp16)
{
#define USE_OLD_WEIGHTS_FORMAT 0

    const auto& engine = get_test_engine();

    if (!engine.get_info().supports_fp16)
    {
        std::cout << "[ SKIPPED ] The test is skipped (cl_khr_fp16 is not supported)." << std::endl;
        EXPECT_EQ(1, 1);
        return;
    }


    const auto input_format   = format::yxfb;
#if USE_OLD_WEIGHTS_FORMAT
    const auto weights_format = format::bfyx;
#else
    const auto weights_format = format::yxfb;
#endif
    const auto biases_format  = format::bfyx;
    const auto output_format  = input_format;

    const int32_t batch_size = 16;
    const int32_t input_feature_count = 2;
    const int32_t output_feature_count = 16;

    const int32_t stride_x = 2;
    const int32_t stride_y = 2;

    const int32_t input_x = 4;
    const int32_t input_y = 4;
    const int32_t weights_x = 2;
    const int32_t weights_y = 2;
    const int32_t output_x = (input_x - weights_x) / stride_x + 1;
    const int32_t output_y = (input_y - weights_y) / stride_y + 1;


    auto input_size = tensor( batch_size, input_feature_count, input_x, input_y );
    auto input = memory::allocate(engine, { data_types::f32, input_format, input_size });
    auto weights_size = tensor( output_feature_count, input_feature_count, weights_x, weights_y );
    auto weights = memory::allocate(engine, { data_types::f32, weights_format, weights_size });
    auto biases_size = tensor( 1,1,output_feature_count,1 );
    auto biases = memory::allocate(engine, { data_types::f32, biases_format, biases_size });
    auto output_size = tensor( batch_size, output_feature_count, output_x, output_y );
    //auto output = memory::allocate({output_format, {batch_size, {output_x, output_y}, output_feature_count}});

    //auto input_cvtd = memory::allocate(engine, { data_types::f16, input_size });
    //auto weights_cvtd = memory::allocate(engine, { data_types::f16, weights_size });
    //auto biases_cvtd = memory::allocate(engine, { data_types::f16, biases_size });
    //auto output_cvtd  = memory::allocate({output_cvt_format, {batch_size, {output_x, output_y}, output_feature_count}});


    // input:
    std::vector<float> input_vals_template {
        0.25f, 0.50f, 0.75f, 1.00f,
        1.25f, 1.50f, 1.75f, 2.00f,
        2.25f, 2.50f, 2.75f, 3.00f,
        3.25f, 3.50f, 3.75f, 4.00f,
    };
    input_vals_template.resize(input_y * input_x);

    std::vector<float> input_vals;
    input_vals.reserve(input_y * input_x * input_feature_count * batch_size);
    for (uint32_t yxi = 0; yxi < input_y * input_x; ++yxi)
    {
        for (uint32_t ifi = 0; ifi < input_feature_count; ++ifi)
        {
            for (uint32_t bi = 0; bi < batch_size; ++bi)
            {
                input_vals.push_back((bi * input_feature_count + ifi + 1) * input_vals_template[yxi]);
            }
        }
    }
    set_values(input, input_vals);


    // weights:
    std::vector<float> weights_vals_template {
        -0.50f, -0.25f,
         0.50f,  0.50f,
    };
    weights_vals_template.resize(weights_y * weights_x);

    std::vector<float> weights_vals;
    weights_vals.reserve(weights_y * weights_x * input_feature_count * output_feature_count);
#if USE_OLD_WEIGHTS_FORMAT
    for (uint32_t ofi = 0; ofi < output_feature_count; ++ofi)
    {
        for (uint32_t ifi = 0; ifi < input_feature_count; ++ifi)
        {
            for (uint32_t yxi = 0; yxi < weights_y * weights_x; ++yxi)
            {
                weights_vals.push_back((ofi * input_feature_count + ifi + 1) * weights_vals_template[yxi]);
            }
        }
    }
#else
    for (uint32_t yxi = 0; yxi < weights_y * weights_x; ++yxi)
    {
        for (uint32_t ifi = 0; ifi < input_feature_count; ++ifi)
        {
            for (uint32_t ofi = 0; ofi < output_feature_count; ++ofi)
            {
                weights_vals.push_back((ofi * input_feature_count + ifi + 1) * weights_vals_template[yxi]);
            }
        }
    }
#endif
    set_values(weights, weights_vals);


    // biases:
    std::vector<float> biases_vals;
    biases_vals.reserve(output_feature_count);
    for (uint32_t ofi = 0; ofi < output_feature_count; ++ofi)
    {
        biases_vals.push_back(ofi * 1.0f);
    }
    set_values(biases, biases_vals);


    // output:
    std::vector<float> output_vals_template {
        1.125f,  1.250f,
        1.625f,  1.750f,
    };
    output_vals_template.resize(output_y * output_x);

    std::vector<float> output_vals;
    output_vals.reserve(output_y * output_x * output_feature_count * batch_size);
    for (uint32_t yxi = 0; yxi < output_y * output_x; ++yxi)
    {
        for (uint32_t ofi = 0; ofi < output_feature_count; ++ofi)
        {
            for (uint32_t bi = 0; bi < batch_size; ++bi)
            {
                uint32_t template_factor = input_feature_count * input_feature_count * input_feature_count * bi * ofi +
                    input_feature_count * input_feature_count * (input_feature_count + 1) / 2 * (bi + ofi) +
                    input_feature_count * (input_feature_count + 1) * (2 * input_feature_count + 1) / 6;
                float bias_factor = ofi * 1.0f;

                output_vals.push_back(template_factor * output_vals_template[yxi] + bias_factor);
            }
        }
    }

    //auto expected_float = memory::allocate(engine, { data_types::f32,{ format::x,{ static_cast<int32_t>(output_vals.size()) } } });
    //auto expected_half  = memory::allocate(engine, { data_types::f16,{ format::x,{ static_cast<int32_t>(output_vals.size()) } } });
    //auto expected       = memory::allocate(engine, { data_types::f32,{ format::x,{ static_cast<int32_t>(output_vals.size()) } } });

//    set_values(expected_float, output_vals);
//    auto cvt_expected_f32_f16 = reorder::create({expected_float, expected_half});
//    auto cvt_expected_f16_f32 = reorder::create({expected_half, expected});
//    execute({cvt_expected_f32_f16, cvt_expected_f16_f32}).wait();
//
//    auto expected_ptr = expected.as<const memory&>().pointer<float>();


    // Computing convolution.
    topology topology(
        input_layout("input", input.get_layout()),
        reorder("cvt_input", "input", {data_types::f16, input_format, input_size}),
        data("weights", weights),
        reorder("cvt_weights", "weights", {data_types::f16, weights_format, weights_size}),
        data("biases", biases),
        reorder("cvt_biases", "biases", {data_types::f16, biases_format, biases_size}),
        convolution(
            "conv",
            "cvt_input",
            { "cvt_weights" },
            { "cvt_biases" },
            { 1,1,stride_x,stride_y }),
        reorder("output", "conv", {data_types::f32, output_format, output_size})
    );

    network network(engine, topology);
    network.set_input_data("input", input);

    auto outputs = network.execute();
    EXPECT_EQ(outputs.size(), size_t(1));
    EXPECT_EQ(outputs.begin()->first, "output");

    auto output_prim = outputs.begin()->second.get_memory();

    auto output_ptr = output_prim.pointer<float>();

    // Checking result.
    uint32_t i = 0;
    for (uint32_t yxi = 0; yxi < output_y * output_x; ++yxi)
    {
        for (uint32_t ofi = 0; ofi < output_feature_count; ++ofi)
        {
            for (uint32_t bi = 0; bi < batch_size; ++bi, ++i)
            {
                auto equal = are_equal(output_vals[i] /*get_value(expected_ptr, i)*/, output_ptr[i], 0.002f);
                EXPECT_TRUE(equal);
                if (!equal)
                {
                    std::cout << "Failed at position (" << yxi << ", output feature = " << ofi << ", batch = " << bi << "): "
                        << output_vals[i] /*get_value(expected_ptr, i)*/ << " != " << output_ptr[i] << std::endl;
                    return;
                }
            }
        }
    }

#undef USE_OLD_WEIGHTS_FORMAT
}

using TestParamType_convolution_gpu = ::testing::tuple<int,   // 0 - Filter size
                                                       int,   // 1 - Input features
                                                       int,   // 2 - Stride
                                                       int,   // 3 - Output padding
                                                       bool>; // 4 - With bias

struct convolution_gpu : public ::testing::TestWithParam<TestParamType_convolution_gpu>
{
    static std::string
    PrintToStringParamName(testing::TestParamInfo<TestParamType_convolution_gpu> param_info)
    {
        // construct a readable name
        return std::to_string(testing::get<0>(param_info.param))
            + 'x' + std::to_string(testing::get<0>(param_info.param))
            + "_f" + std::to_string(testing::get<1>(param_info.param))
            + "_stride" + std::to_string(testing::get<2>(param_info.param))
            + "_pad" + std::to_string(testing::get<3>(param_info.param))
            + (testing::get<4>(param_info.param) ? "_bias" : "");
    }
};

TEST_P(convolution_gpu, b_fs_yx_fsv4)
{
    const int in_B = 2;
    const int in_X = 56;
    const int in_Y = 56;
    const int _OuD = 32;
    const int W_B = _OuD;

    // Kernel sizes
    int W_X = testing::get<0>(GetParam());
    int W_Y = W_X;

    // Convoluiton offset
    int offSet = -(W_X / 2);

    // Features
    int in_F = testing::get<1>(GetParam());
    int W_F = in_F;

    // Stride
    int stride = testing::get<2>(GetParam());

    // Output padding
    int output_padding = testing::get<3>(GetParam());

    // Biases
    bool with_bias = testing::get<4>(GetParam());

    engine engine;

    // Input data init
    std::vector<char> Data(in_B * in_F * in_X * in_Y);
    std::iota(Data.begin(), Data.end(), 0);
    auto input = memory::allocate(engine, {data_types::i8, format::bfyx, {in_B, in_F, in_X, in_Y}});
    set_values(input, std::move(Data));

    // Create a topology
    topology topology(input_layout("input", input.get_layout()));

    // Reorder
    topology.add(reorder("reorder_in",
                         "input",
                         layout(data_types::i8, format::b_fs_yx_fsv4, {in_B, in_F, in_X, in_Y})));

    // Weights init
    std::vector<char> Weights(W_B * W_F * W_X * W_Y);
    std::iota(Weights.begin(), Weights.end(), 0);
    auto weights_gold =
        memory::allocate(engine, {data_types::i8, format::bfyx, {W_B, W_F, W_X, W_Y}});
    auto weights_imad =
        memory::allocate(engine, {data_types::i8, format::bfyx, {W_B, W_F, W_X, W_Y}});
    set_values(weights_gold, Weights);
    set_values(weights_imad, std::move(Weights));
    topology.add(data("weights_gold", weights_gold), data("weights_imad", weights_imad));

    if (with_bias)
    {
        // Bias, Callibraiton, Quantization
        std::vector<float> vB(_OuD), vC(_OuD), vQ(_OuD);
        auto x = 0.1f;
        std::generate(vB.begin(), vB.end(), [&x]() {
            x += 0.01f;
            if (x >= 0.9f)
                x = 0.1f;
            return x;
        });
        x = 0.2f;
        std::generate(vC.begin(), vC.end(), [&x]() {
            x += 0.01f;
            if (x >= 0.9f)
                x = 0.2f;
            return x;
        });
        x = 0.3f;
        std::generate(vQ.begin(), vQ.end(), [&x]() {
            x += 0.01f;
            if (x >= 0.9f)
                x = 0.3f;
            return x;
        });
        auto bias_gold = memory::allocate(engine, {data_types::f32, format::bfyx, {1, 1, _OuD, 1}});
        auto bias_imad = memory::allocate(engine, {data_types::f32, format::bfyx, {1, 1, _OuD, 1}});
        auto callib_gold = memory::allocate(engine, {data_types::f32, format::bfyx, {1, 1, _OuD, 1}});
        auto callib_imad = memory::allocate(engine, {data_types::f32, format::bfyx, {1, 1, _OuD, 1}});
        auto quant_gold = memory::allocate(engine, {data_types::f32, format::bfyx, {1, 1, _OuD, 1}});
        auto quant_imad = memory::allocate(engine, {data_types::f32, format::bfyx, {1, 1, _OuD, 1}});
        set_values(bias_gold, vB);
        set_values(bias_imad, std::move(vB));
        set_values(callib_gold, vC);
        set_values(callib_imad, std::move(vC));
        set_values(quant_gold, vQ);
        set_values(quant_imad, std::move(vQ));
        topology.add(data("bias_gold", bias_gold),
                     data("callib_gold", callib_gold),
                     data("quant_gold", quant_gold));
        topology.add(data("bias_imad", bias_imad),
                     data("callib_imad", callib_imad),
                     data("quant_imad", quant_imad));

        // Convolutions
        convolution conv_gold("conv_gold",
                              "input",
                              {"weights_gold"},
                              {"bias_gold"},
                              {"quant_gold"},
                              {"callib_gold"},
                              1.0f,
                              {1, 1, stride, stride},
                              {0, 0, offSet, offSet});
        convolution conv_imad("conv_imad",
                              "reorder_in",
                              {"weights_imad"},
                              {"bias_imad"},
                              {"quant_imad"},
                              {"callib_imad"},
                              1.0f,
                              {1, 1, stride, stride},
                              {0, 0, offSet, offSet});
        conv_gold.set_output_padding(padding({0, 0, output_padding, output_padding}, 0.0f));
        conv_imad.set_output_padding(padding({0, 0, output_padding, output_padding}, 0.0f));
        topology.add(conv_gold, conv_imad);
    }
    else
    {
        // Convolutions
        convolution conv_gold(
            "conv_gold", "input", {"weights_gold"}, {1, 1, stride, stride}, {0, 0, offSet, offSet});
        convolution conv_imad(
            "conv_imad", "reorder_in", {"weights_imad"}, {1, 1, stride, stride}, {0, 0, offSet, offSet});
        conv_gold.set_output_padding(padding({0, 0, output_padding, output_padding}, 0.0f));
        conv_imad.set_output_padding(padding({0, 0, output_padding, output_padding}, 0.0f));
        topology.add(conv_gold, conv_imad);
    }

    // Reorder
    topology.add(reorder("reorder_out",
                         "conv_imad",
                         layout(data_types::i8,
                                format::bfyx,
                                {in_B, W_B, (in_X + stride - 1) / stride, (in_Y + stride - 1) / stride},
                                padding({0, 0, output_padding, output_padding}, 0.0f))));

    // Network build
    build_options build_opt;
    build_opt.set_option(build_option::optimize_data(true));
    network network(engine, topology, build_opt);

    // Network execuiton
    network.set_input_data("input", input);
    auto outputs = network.execute();

    auto out_gold = outputs.find("conv_gold");
    auto out_test = outputs.find("reorder_out");
    ASSERT_NE(out_gold, outputs.end());
    ASSERT_NE(out_test, outputs.end());

    auto gold_ptr = out_gold->second.get_memory().pointer<char>();
    auto test_ptr = out_test->second.get_memory().pointer<char>();

    ASSERT_EQ(gold_ptr.size(), test_ptr.size());
    for (size_t i = 0; i < gold_ptr.size(); i++)
    {
        ASSERT_EQ(gold_ptr[i], test_ptr[i]);
    }
}

// Select particular test cases
INSTANTIATE_TEST_CASE_P(convolution_gpu_imad,
                        convolution_gpu,
                        ::testing::Values(
                            // Filter size, Input features, Stride, Output padding, With bias
                            TestParamType_convolution_gpu(1, 32, 1, 0, false),
                            TestParamType_convolution_gpu(3, 32, 1, 0, false),
                            TestParamType_convolution_gpu(7,  3, 1, 0, false),
                            TestParamType_convolution_gpu(1, 32, 1, 0, true),
                            TestParamType_convolution_gpu(3, 32, 1, 0, true),
                            TestParamType_convolution_gpu(7,  3, 1, 0, true),
                            TestParamType_convolution_gpu(1, 32, 1, 1, false),
                            TestParamType_convolution_gpu(3, 32, 1, 1, false),
                            TestParamType_convolution_gpu(7,  3, 1, 1, false),
                            TestParamType_convolution_gpu(1, 32, 2, 0, false),
                            TestParamType_convolution_gpu(3, 32, 2, 0, false),
                            TestParamType_convolution_gpu(7,  3, 2, 0, false),
                            TestParamType_convolution_gpu(3, 64, 2, 1, true)),
                        convolution_gpu::PrintToStringParamName);
//// or test all combinations
//INSTANTIATE_TEST_CASE_P(convolution_gpu_imad,
//                        convolution_gpu,
//                        ::testing::Combine(::testing::Values(1, 3, 7),    // Filter size
//                                           ::testing::Values(3, 32),      // Input features
//                                           ::testing::Values(1, 2),       // Stride
//                                           ::testing::Values(0, 1),       // Output padding
//                                           ::testing::Values(false, true) // With bias
//                                           ),
//                        convolution_gpu::PrintToStringParamName);

TEST_P(convolution_gpu, fs_byx_fsv32)
{
    const auto& engine = get_test_engine();

    if (!engine.get_info().supports_fp16)
    {
        std::cout << "[ SKIPPED ] The test is skipped (cl_khr_fp16 is not supported)." << std::endl;
        EXPECT_EQ(1, 1);
        return;
    }

    const int batch_num = 2;
    const int input_xy = 14;
    const int input_f = testing::get<1>(GetParam());
    const int output_f = 64;
    const int filter_xy = testing::get<0>(GetParam());
    const int stride = testing::get<2>(GetParam());
    const int output_padding = testing::get<3>(GetParam());
    const bool with_bias = testing::get<4>(GetParam());
    const int input_offset = -(filter_xy / 2);

    // TODO: Reenable after adding full support for fs_byx_fsv32 format
    //       Currently layout_optimizer will select fs_byx_fsv32 only if b != 1 and f % 32 == 0.
    if (input_f % 32 != 0 || batch_num == 1)
    {
        std::cout << "[ SKIPPED ] The test is skipped (fs_byx_fsv32 convolution support disabled)." << std::endl;
        EXPECT_EQ(1, 1);
        return;
    }

    const int output_xy = 1 + (input_xy + 2 * (-input_offset) - filter_xy) / stride + 2 * output_padding;

    auto input_size = tensor(batch_num, input_f, input_xy, input_xy);
    auto input_data = generate_random_4d<FLOAT16>(batch_num, input_f, input_xy, input_xy, -1, 1);
    auto input_data_bfyx = flatten_4d(format::bfyx, input_data);
    auto input_mem = memory::allocate(engine, { data_types::f16, format::bfyx, input_size });
    set_values(input_mem, input_data_bfyx);

    auto weights_size = tensor(output_f, input_f, filter_xy, filter_xy);
    auto weights_data = generate_random_4d<FLOAT16>(output_f, input_f, filter_xy, filter_xy, -1, 1);
    auto weights_data_bfyx = flatten_4d(format::bfyx, weights_data);
    auto weights_mem = memory::allocate(engine, { data_types::f16, format::bfyx, weights_size });
    set_values(weights_mem, weights_data_bfyx);

    // Will be used to store reference values calculated in branches depending on bias
    auto reference_result = VVVVF<FLOAT16>(batch_num, VVVF<FLOAT16>(output_f));

    topology topology(
        input_layout("input", input_mem.get_layout()),
        data("weights_fsv", weights_mem));

    // Reorder input to fs_byx_fsv32
    topology.add(reorder("input_fsv", "input", { data_types::f16, format::fs_b_yx_fsv32, input_size }));

    if (with_bias)
    {
        // Generate bias data
        auto biases_size = tensor(1, 1, output_f, 1);
        auto biases_data = generate_random_1d<FLOAT16>(output_f, -1, 1);
        auto biases_mem = memory::allocate(engine, { data_types::f16, format::bfyx, biases_size });
        set_values(biases_mem, biases_data);

        // Calculate reference values with bias
        for (auto bi = 0; bi < batch_num; ++bi)
        {
            for (auto ofi = 0; ofi < output_f; ++ofi)
            {
                reference_result[bi][ofi] = reference_convolve(
                    input_data[bi], weights_data[ofi], 
                    stride, stride, biases_data[ofi],
                    1, 1,                               // dilation
                    -input_offset, -input_offset,       // input padding
                    output_padding, output_padding);
            }
        }

        topology.add(data("biases_fsv", biases_mem));

        auto conv_fsv = convolution("conv_fsv", "input_fsv", { "weights_fsv" }, { "biases_fsv" },
                                    { 1, 1, stride, stride }, { 0, 0, input_offset, input_offset });
        conv_fsv.set_output_padding(padding({ 0, 0, output_padding, output_padding }, 0.f));

        topology.add(conv_fsv);
    }
    else
    {
        // Calculate reference values without bias
        for (auto bi = 0; bi < batch_num; ++bi)
        {
            for (auto ofi = 0; ofi < output_f; ++ofi)
            {
                reference_result[bi][ofi] = reference_convolve(
                    input_data[bi], weights_data[ofi],
                    stride, stride,
                    0,                                  // bias
                    1, 1,                               // dilation
                    -input_offset, -input_offset,       // input padding
                    output_padding, output_padding);
            }
        }

        auto conv_fsv = convolution("conv_fsv", "input_fsv", { "weights_fsv" },
            { 1, 1, stride, stride }, { 0, 0, input_offset, input_offset });
        conv_fsv.set_output_padding(padding({ 0, 0, output_padding, output_padding }, 0.f));

        topology.add(conv_fsv);
    }

    build_options options;
    options.set_option(build_option::optimize_data(true));
    network network(engine, topology, options);

    network.set_input_data("input", input_mem);

    network.execute();

    auto out_mem = network.get_output("conv_fsv").get_memory();
    auto out_ptr = out_mem.pointer<FLOAT16>();

    ASSERT_EQ(out_mem.get_layout().format, format::fs_b_yx_fsv32);

    for (int bi = 0; bi < batch_num; ++bi)
        for (int fi = 0; fi < output_f; ++fi)
            for (int yi = 0; yi < output_xy; ++yi)
                for (int xi = 0; xi < output_xy; ++xi)
                {
                    auto val_ref = reference_result[bi][fi][yi][xi];
                    auto val = out_ptr[(fi / 32) * batch_num * output_xy * output_xy * 32 +
                                        bi * output_xy * output_xy * 32 +
                                        yi * output_xy * 32 +
                                        xi * 32 +
                                        fi % 32];
                    auto equal = are_equal(val_ref, val, 1e-2f);
                    EXPECT_TRUE(equal);
                    if (!equal)
                    {
                        std::cout << "At b = " << bi << ", fi = " << fi << ", xi = " << xi << ", yi = " << yi << std::endl;
                    }
                }
}

TEST_P(convolution_gpu, bfyx_f16)
{
    const auto& engine = get_test_engine();

    if (!engine.get_info().supports_fp16)
    {
        std::cout << "[ SKIPPED ] The test is skipped (cl_khr_fp16 is not supported)." << std::endl;
        EXPECT_EQ(1, 1);
        return;
    }

    const int batch_num = 1;
    const int input_xy = 14;
    const int input_f = testing::get<1>(GetParam());
    const int output_f = 64;
    const int filter_xy = testing::get<0>(GetParam());
    const int stride = testing::get<2>(GetParam());
    const int output_padding = testing::get<3>(GetParam());
    const bool with_bias = testing::get<4>(GetParam());
    const int input_offset = -(filter_xy / 2);

    // TODO: Reenable after adding full support for bfyx_f16 format
    //       Currently layout_optimizer will select bfyx_f16 only if b == 1 and f % 16 == 0.
    if (input_f % 16 != 0 || batch_num != 1)
    {
        std::cout << "[ SKIPPED ] The test is skipped (bfyx_f16 convolution support disabled)." << std::endl;
        EXPECT_EQ(1, 1);
        return;
    }

    const int output_xy = 1 + (input_xy + 2 * (-input_offset) - filter_xy) / stride + 2 * output_padding;

    auto input_size = tensor(batch_num, input_f, input_xy, input_xy);
    auto input_data = generate_random_4d<FLOAT16>(batch_num, input_f, input_xy, input_xy, -1, 1);
    auto input_data_bfyx = flatten_4d(format::bfyx, input_data);
    auto input_mem = memory::allocate(engine, { data_types::f16, format::bfyx, input_size });
    set_values(input_mem, input_data_bfyx);

    auto weights_size = tensor(output_f, input_f, filter_xy, filter_xy);
    auto weights_data = generate_random_4d<FLOAT16>(output_f, input_f, filter_xy, filter_xy, -1, 1);
    auto weights_data_bfyx = flatten_4d(format::bfyx, weights_data);
    auto weights_mem = memory::allocate(engine, { data_types::f16, format::bfyx, weights_size });
    set_values(weights_mem, weights_data_bfyx);

    // Will be used to store reference values calculated in branches depending on bias
    auto reference_result = VVVVF<FLOAT16>(batch_num, VVVF<FLOAT16>(output_f));

    topology topology(
        input_layout("input", input_mem.get_layout()),
        data("weights", weights_mem));

    // Reorder input to bfyx_f16
    topology.add(reorder("input_f16", "input", { data_types::f16, format::bfyx_f16, input_size }));

    if (with_bias)
    {
        // Generate bias data
        auto biases_size = tensor(1, 1, output_f, 1);
        auto biases_data = generate_random_1d<FLOAT16>(output_f, -1, 1);
        auto biases_mem = memory::allocate(engine, { data_types::f16, format::bfyx, biases_size });
        set_values(biases_mem, biases_data);

        // Calculate reference values with bias
        for (auto bi = 0; bi < batch_num; ++bi)
        {
            for (auto ofi = 0; ofi < output_f; ++ofi)
            {
                reference_result[bi][ofi] = reference_convolve(
                    input_data[bi], weights_data[ofi],
                    stride, stride, biases_data[ofi],
                    1, 1,                               // dilation
                    -input_offset, -input_offset,       // input padding
                    output_padding, output_padding);
            }
        }

        topology.add(data("biases", biases_mem));

        auto conv_fsv = convolution("conv", "input_f16", { "weights" }, { "biases" },
            { 1, 1, stride, stride }, { 0, 0, input_offset, input_offset });
        conv_fsv.set_output_padding(padding({ 0, 0, output_padding, output_padding }, 0.f));

        topology.add(conv_fsv);
    }
    else
    {
        // Calculate reference values without bias
        for (auto bi = 0; bi < batch_num; ++bi)
        {
            for (auto ofi = 0; ofi < output_f; ++ofi)
            {
                reference_result[bi][ofi] = reference_convolve(
                    input_data[bi], weights_data[ofi],
                    stride, stride,
                    0,                                  // bias
                    1, 1,                               // dilation
                    -input_offset, -input_offset,       // input padding
                    output_padding, output_padding);
            }
        }

        auto conv_fsv = convolution("conv", "input_f16", { "weights" },
            { 1, 1, stride, stride }, { 0, 0, input_offset, input_offset });
        conv_fsv.set_output_padding(padding({ 0, 0, output_padding, output_padding }, 0.f));

        topology.add(conv_fsv);
    }

    build_options options;
    options.set_option(build_option::optimize_data(true));
    network network(engine, topology, options);

    network.set_input_data("input", input_mem);

    network.execute();

    auto out_mem = network.get_output("conv").get_memory();
    auto out_ptr = out_mem.pointer<FLOAT16>();

    ASSERT_EQ(out_mem.get_layout().format, format::bfyx_f16);

    for (int bi = 0; bi < batch_num; ++bi)
        for (int fi = 0; fi < output_f; ++fi)
            for (int yi = 0; yi < output_xy; ++yi)
                for (int xi = 0; xi < output_xy; ++xi)
                {
                    auto val_ref = reference_result[bi][fi][yi][xi];
                    auto val = out_ptr[bi * output_xy * output_xy * output_f +
                                       (fi / 16) * output_xy * output_xy * 16 +
                                       yi * output_xy * 16 +
                                       xi * 16 +
                                       fi % 16];
                    auto equal = are_equal(val_ref, val, 1e-2f);
                    EXPECT_TRUE(equal);
                    if (!equal)
                    {
                        std::cout << "At b = " << bi << ", fi = " << fi << ", xi = " << xi << ", yi = " << yi << std::endl;
                    }
                }
}

class convolution_test : public tests::generic_test
{

public:

    static void TearDownTestCase() 
    {
        for (auto generic_params : all_generic_params)
        {
            delete generic_params;
        }

        for (auto layer_params : all_layer_params)
        {
            delete layer_params;
        }
    }

    static std::vector<cldnn::primitive*> generate_specific_test_params()
    {
        // TODO: check split 

        // TODO: check convolution without bias 

        const std::vector<primitive_id>& weights = { "input1" };
        const std::vector<primitive_id>& bias = { "input2" };

        std::vector<tensor> stride_sizes = { tensor(1, 1, 1, 1), tensor(1, 1, 2, 3), tensor(1, 1, 4, 1), tensor(1, 1, 5, 5) };
        std::vector<tensor> dilation_sizes = { tensor(1, 1, 1, 1), tensor(1, 1, 5, 4), tensor(1, 1, 1, 3), tensor(1, 1, 7, 2) };
        std::vector<tensor> input_offset_sizes = { tensor(0, 0, 0, 0), tensor(0, 0, 2, 2), tensor(0, 0, -5, -2), tensor(0, 0, 3, -3) };

        std::vector<bool> activations = { false, true };
        std::vector<float> activation_slopes = { 0.f, -2.3f };

        // No padding
        all_layer_params.push_back(new convolution("convolution", "input0", weights, bias, stride_sizes[0], input_offset_sizes[0], dilation_sizes[0], activations[0], activation_slopes[0]));
        all_layer_params.push_back(new convolution("convolution", "input0", weights, bias, stride_sizes[1], input_offset_sizes[1], dilation_sizes[1], activations[0], activation_slopes[0]));
        all_layer_params.push_back(new convolution("convolution", "input0", weights, bias, stride_sizes[2], input_offset_sizes[2], dilation_sizes[2], activations[1], activation_slopes[0]));
        all_layer_params.push_back(new convolution("convolution", "input0", weights, bias, stride_sizes[3], input_offset_sizes[3], dilation_sizes[3], activations[1], activation_slopes[1]));

        // Input padding
        all_layer_params.push_back(new convolution("convolution", "reorder0", weights, bias, stride_sizes[1], input_offset_sizes[1], dilation_sizes[1], activations[0], activation_slopes[0]));
        all_layer_params.push_back(new convolution("convolution", "reorder0", weights, bias, stride_sizes[3], input_offset_sizes[3], dilation_sizes[3], activations[1], activation_slopes[1]));

        // Output padding
        all_layer_params.push_back(new convolution("convolution", "input0", weights, bias, stride_sizes[1], input_offset_sizes[1], dilation_sizes[1], activations[0], activation_slopes[0], { { 0, 0, 2, 4 },{ 0, 0, 0, 19 } }));
        all_layer_params.push_back(new convolution("convolution", "input0", weights, bias, stride_sizes[2], input_offset_sizes[2], dilation_sizes[2], activations[1], activation_slopes[0], { { 0, 0, 1, 0 },{ 0, 0, 13, 9 } }));

        // Input + Output padding
        all_layer_params.push_back(new convolution("convolution", "reorder0", weights, bias, stride_sizes[0], input_offset_sizes[0], dilation_sizes[0], activations[0], activation_slopes[0], { { 0, 0, 1, 5 },{ 0, 0, 19, 4 } }));
        all_layer_params.push_back(new convolution("convolution", "reorder0", weights, bias, stride_sizes[3], input_offset_sizes[3], dilation_sizes[3], activations[1], activation_slopes[1], { { 0, 0, 1, 2 },{ 0, 0, 3, 4 } }));

        return all_layer_params;
    }

    static std::vector<std::tuple<tests::test_params*, cldnn::primitive*>> generate_all_test_params()
    {
        generate_specific_test_params();

        std::vector<cldnn::format> input_formats = { cldnn::format::bfyx, cldnn::format::yxfb };
        std::vector<cldnn::format> weights_formats = { cldnn::format::bfyx, cldnn::format::yxfb };

        std::vector<int32_t> output_features_sizes = { 1, 3, 16 };
        std::vector<cldnn::tensor> kernel_sizes = { tensor(1, 1, 1, 1), tensor(1, 1, 4, 7), tensor(1, 1, 5, 3) };

        std::vector<tensor> input_tensor_size = { tensor(1, 5, 59, 72), tensor(8, 3, 63, 56), tensor(16, 2, 50, 50), tensor(32, 1, 44, 62) };

        auto data_types = test_data_types();

        for (cldnn::data_types data_type : data_types)
        {
            for (cldnn::format input_format : input_formats)
            {
                for (cldnn::format weights_format : weights_formats)
                {
                    cldnn::build_options network_build_options;
                    if (input_format == cldnn::format::bfyx)
                    {
                        network_build_options.set_option(cldnn::build_option::optimize_data(true));
                    }
                    for (cldnn::tensor input_size : input_tensor_size)
                    {
                        for (cldnn::tensor kernel_size : kernel_sizes)
                        {
                            for (auto output_features : output_features_sizes)
                            {
                                test_params* params = new test_params(data_type, input_format, input_size.batch[0], input_size.feature[0], tensor(1, 1, input_size.spatial[0], input_size.spatial[1]), network_build_options);
                                int input_features = params->input_layouts[0].size.feature[0];
                                params->input_layouts.push_back(cldnn::layout(params->data_type, weights_format, cldnn::tensor(output_features, input_features, kernel_size.spatial[0], kernel_size.spatial[1]))); // weights
                                params->input_layouts.push_back(cldnn::layout(params->data_type, params->fmt, cldnn::tensor(1, 1, output_features, 1))); // biases
                                all_generic_params.push_back(params);
                            }
                        }
                    }
                }
            }
        }

        // Create all the combinations for the test.
        for (cldnn::primitive* layer_param : all_layer_params)
        {
            for (tests::test_params* test_param : all_generic_params)
            {
                all_test_params.push_back(std::make_tuple(test_param, layer_param));
            }
        }

        return all_test_params;
    }

    virtual bool is_format_supported(cldnn::format format)
    {
        return ((format == cldnn_format_type::cldnn_format_bfyx) || (format == cldnn_format_type::cldnn_format_yxfb));
    }

    virtual cldnn::tensor get_expected_output_tensor()
    {
        const cldnn::convolution* convolution = (cldnn::convolution*)layer_params;
        tensor input_size = generic_params->input_layouts[0].size;
        tensor dilation = convolution->dilation;
        tensor stride = convolution->stride;
        tensor input_offset = convolution->input_offset;
        tensor weights_size = generic_params->input_layouts[1].size;

        int kernel_extent_y = dilation.spatial[1] * (weights_size.spatial[1] - 1) + 1;
        int kernel_extent_x = dilation.spatial[0] * (weights_size.spatial[0] - 1) + 1;

        // Calculate output size
        int output_size_y = 1 + (input_size.spatial[1] - kernel_extent_y - 2 * input_offset.spatial[1]) / stride.spatial[1];
        int output_size_x = 1 + (input_size.spatial[0] - kernel_extent_x - 2 * input_offset.spatial[0]) / stride.spatial[0];
        int output_features = weights_size.batch[0];

        return cldnn::tensor(input_size.batch[0], output_features, output_size_x, output_size_y);
    }

    virtual void prepare_input_for_test(std::vector<cldnn::memory>& inputs)
    {
        if (generic_params->data_type == data_types::f32)
        {
            prepare_input_for_test_typed<float>(inputs);
        }
        else
        {
            prepare_input_for_test_typed<FLOAT16>(inputs);
        }
    }

    template<typename Type>
    void prepare_input_for_test_typed(std::vector<cldnn::memory>& inputs)
    {
        int k = (generic_params->data_type == data_types::f32) ? 8 : 4;

        // Update inputs.
        auto input = inputs[0];
        auto input_size = inputs[0].get_layout().size;
        VVVVF<Type> input_rnd = generate_random_4d<Type>(input_size.batch[0], input_size.feature[0], input_size.spatial[1], input_size.spatial[0], -2, 2, k);
        VF<Type> input_rnd_vec = flatten_4d<Type>(input.get_layout().format, input_rnd);
        set_values(input, input_rnd_vec);

        // Update weights.
        auto weight_input = inputs[1];
        auto weight_size = inputs[1].get_layout().size;
        VVVVF<Type> weight_rnd = generate_random_4d<Type>(weight_size.batch[0], weight_size.feature[0], weight_size.spatial[1], weight_size.spatial[0], -2, 2, k);
        VF<Type> weight_rnd_vec = flatten_4d<Type>(weight_input.get_layout().format, weight_rnd);
        set_values(weight_input, weight_rnd_vec);

        // Update biases.
        auto bias_input = inputs[2];
        auto bias_size = inputs[2].get_layout().size;
        VF<Type> bias_rnd = generate_random_1d<Type>(bias_size.spatial[0], -2, 2, k);
        set_values(bias_input, bias_rnd);
    }

    template<typename Type>
    memory generate_reference_typed(const std::vector<cldnn::memory>& inputs)
    {
        // Output reference is always bfyx.

        const cldnn::convolution* convolution = (cldnn::convolution*)layer_params;

        data_types dt = inputs[0].get_layout().data_type;
        
        tensor input_size = inputs[0].get_layout().size;
        tensor dilation = convolution->dilation;
        tensor stride = convolution->stride;
        bool is_relu_fused = convolution->with_activation;
        float activation_slope = convolution->activation_negative_slope;
        tensor input_offset = convolution->input_offset;
        tensor weights_size = inputs[1].get_layout().size;
        padding output_padding = convolution->get_output_padding();

        tensor output_size = get_expected_output_tensor();

        // Calculate output size
        int output_size_y = output_size.spatial[1];
        int output_size_x = output_size.spatial[0];
        int output_features = weights_size.batch[0];
        int input_features = weights_size.feature[0];

        auto output = memory::allocate( engine, cldnn::layout(dt, cldnn::format::bfyx, output_size, output_padding) );

        auto input_mem = inputs[0].pointer<Type>();
        auto weights_mem = inputs[1].pointer<Type>();
        auto bias_mem = inputs[2].pointer<Type>();
        auto output_mem = output.pointer<Type>();

        tensor output_buffer_size = output.get_layout().get_buffer_size();

        // Initialized output with zeros.
        std::fill(output_mem.begin(), output_mem.end(), static_cast<Type>(0));
    
        // Add the bias
        for (int b = 0; b < input_size.batch[0]; b++)
        {
            for (int out_f = 0; out_f < output_features; out_f++)
            {
                for (int y = 0; y < output_size_y; y++)
                {
                    for (int x = 0; x < output_size_x; x++)
                    {
                        int output_index = (b * output_buffer_size.feature[0] + out_f) * output_buffer_size.spatial[1] * output_buffer_size.spatial[0];
                        tensor lower_output_padding = convolution->get_output_padding().lower_size();
                        output_index += (lower_output_padding.spatial[1] + y) * output_buffer_size.spatial[0] + lower_output_padding.spatial[0] + x;

                        output_mem[output_index] += bias_mem[out_f];
                    }
                }
            }
        }

        const auto input0_desc = get_linear_memory_desc(inputs[0].get_layout());
        const auto input1_desc = get_linear_memory_desc(inputs[1].get_layout());

        // Convolve with weights
        for (int b = 0; b < input_size.batch[0]; b++)
        {
            int input_bi = b;
            for (int out_f = 0; out_f < output_features; out_f++)
            {
                for (int in_f = 0; in_f < input_features; in_f++)
                {
                    int input_fi = in_f;
                    for (int y = 0; y < output_size_y; y++)
                    {
                        for (int x = 0; x < output_size_x; x++)
                        {
                            int output_bi = b;
                            int output_fi = out_f;
                            int output_yi = y;
                            int output_xi = x;
                            int output_index = (output_bi * output_buffer_size.feature[0] + output_fi) * output_buffer_size.spatial[1] * output_buffer_size.spatial[0];
                            tensor lower_output_padding = convolution->get_output_padding().lower_size();
                            output_index += (lower_output_padding.spatial[1] + output_yi) * output_buffer_size.spatial[0] + lower_output_padding.spatial[0] + output_xi;

                            for (int kernel_y = 0; kernel_y < weights_size.spatial[1]; kernel_y++)
                            {
                                int input_yi = y * stride.spatial[1] + input_offset.spatial[1] + kernel_y * dilation.spatial[1];
                                if ((input_yi < 0) || (input_yi >= input_size.spatial[1]))
                                {
                                    continue;
                                }

                                for (int kernel_x = 0; kernel_x < weights_size.spatial[0]; kernel_x++)
                                {
                                    int input_xi = x * stride.spatial[0] + input_offset.spatial[0] + kernel_x * dilation.spatial[0];
                                    if ((input_xi < 0) || (input_xi >= input_size.spatial[0]))
                                    {
                                        continue;
                                    }

                                    size_t input_index = get_linear_index(inputs[0].get_layout(), input_bi, input_fi, input_yi, input_xi, input0_desc);

                                    int weight_bi = out_f;
                                    int weight_fi = in_f;
                                    int weight_yi = kernel_y;
                                    int weight_xi = kernel_x;
                                    size_t weight_index = get_linear_index(inputs[1].get_layout(), weight_bi, weight_fi, weight_yi, weight_xi, input1_desc);
                                    output_mem[output_index] += input_mem[input_index] * weights_mem[weight_index];
                                }
                            }
                        }
                    }
                }
            }
        }

        // Relu activation
        if (is_relu_fused)
        {
            for (int i = 0; i < (int)output_buffer_size.count(); i++)
            {
                output_mem[i] = (output_mem[i] > 0.f) ? output_mem[i] : (output_mem[i] * (Type)activation_slope);
            }
        }

        return output;
    }

    virtual memory generate_reference(const std::vector<cldnn::memory>& inputs)
    {
        if (generic_params->data_type == data_types::f32)
        {
            return generate_reference_typed<float>(inputs);
        }
        else
        {
            return generate_reference_typed<FLOAT16>(inputs);
        }
    }

private:

    static std::vector<tests::test_params*> all_generic_params;
    static std::vector<cldnn::primitive*> all_layer_params;
    static std::vector<std::tuple<tests::test_params*, cldnn::primitive*>> all_test_params;
};

std::vector<tests::test_params*> convolution_test::all_generic_params = {};
std::vector<cldnn::primitive*> convolution_test::all_layer_params = {};
std::vector<std::tuple<tests::test_params*, cldnn::primitive*>> convolution_test::all_test_params = {};

TEST_P(convolution_test, CONVOLUTION)
{
    run_single_test();
}

INSTANTIATE_TEST_CASE_P(DISABLED_CONVOLUTION, 
                        convolution_test, 
                        ::testing::ValuesIn(convolution_test::generate_all_test_params()),
                        tests::generic_test::custom_param_name_functor());
