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

namespace kernel_selector 
{
    class KernelRunnerInterface
    {
    public:
        // Gets a list of kernels, executes them and returns the run time of each kernel (in nano-seconds).
        virtual std::vector<uint64_t> run_kernels(const kernel_selector::KernelsData& kernelsData) = 0;

        virtual ~KernelRunnerInterface() = default;
    };
}