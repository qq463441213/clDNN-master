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

///////////////////////////////////////////////////////////////////////////////////////////////////
#include "confiugration.h"
#include <CL/cl.h>
#include <iostream>
#include <string.h>
#include <math.h>
using namespace std;
static int getVendorID()
{
    cl_uint numPlatforms = 0;
    cl_platform_id * platforms = nullptr;
    // 第一次调用clGetPlatfromIDs，获取平台数量
    cl_int status = clGetPlatformIDs(0, nullptr, &numPlatforms);
    char *device_param = new char[50];
    if(status != CL_SUCCESS)
    {
        cout << "error : getting platforms failed";
        return 1;
    }
    if(numPlatforms == 0)
        return -1;
    platforms = new cl_platform_id[numPlatforms];
    status = clGetPlatformIDs(numPlatforms, platforms, nullptr);
    cl_uint vid = 0;
    for(int i = 0; i < 1; ++i)
    {
        // 获取设备
        cl_uint numDevices = 0;
        cl_device_id * devices;
        status = clGetDeviceIDs(platforms[i], CL_DEVICE_TYPE_ALL, 0, nullptr, &numDevices);
        devices = new cl_device_id[numDevices];
        clGetDeviceIDs(platforms[i], CL_DEVICE_TYPE_ALL, numDevices, devices, nullptr);
        // 打印设备信息
        for(int j = 0; j < 1; ++j)
        {
            clGetDeviceInfo(devices[j], CL_DEVICE_VENDOR_ID, sizeof(cl_uint), &vid, nullptr);
        }
        delete [] devices;
    }
    delete [] device_param;
    return vid;
}

namespace cldnn {
    namespace gpu {

        configuration::configuration()
            : enable_profiling(false)
            , meaningful_kernels_names(false)
            , device_type(gpu)
            , device_vendor(0x10ee)
            , compiler_options("")
            , single_kernel_name("")
            , host_out_of_order(false)
            , log("")
            , ocl_sources_dumps_dir("")
            , user_context(nullptr)            
            , tuning_cache_path("cache.json")        
        {
	    this->device_vendor = getVendorID();
	}
    }
}
