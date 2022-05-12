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


#include "one_hot_inst.h"

#include "error_handler.h"
#include "json_object.h"
#include "primitive_type_base.h"


namespace cldnn
{
    primitive_type_id one_hot_type_id()
    {
        static primitive_type_base<one_hot> instance;
        return &instance;
    }

    layout one_hot_inst::calc_output_layout(one_hot_node const& node)
    {
        assert((bool)node.get_primitive()->get_output_data_type() == false
               && "Output data type forcing is not supported for one_hot_node!");
        auto input_layout = node.input().get_output_layout();
        auto desc = node.get_primitive();

        if (desc->one_hot_axis > 3)
        {
            CLDNN_ERROR_MESSAGE(node.id(), "Incorrect parameters configuration: one_hot_axis should be less or equal to 3.");
        }

        return{ input_layout.data_type, input_layout.format, desc->shape };
    }

    std::string one_hot_inst::to_string(one_hot_node const& node)
    {
        auto desc = node.get_primitive();
        auto node_info = node.desc_to_json();
        const auto& shape = desc->shape;
        const auto& one_hot_axis = desc->one_hot_axis;
        auto& input = node.input();

        std::stringstream primitive_description;

        json_composite one_hot_info;
        one_hot_info.add("input id", input.id());
        one_hot_info.add("output shape", shape.to_string());
        one_hot_info.add("one-hot axis", one_hot_axis);

        node_info->add("one_hot info", one_hot_info);
        node_info->dump(primitive_description);

        return primitive_description.str();
    }

    one_hot_inst::typed_primitive_inst(network_impl& network, one_hot_node const& node)
        : parent(network, node)
    {
        auto input_layout = node.input().get_output_layout();

        const auto& input_sizes = input_layout.size;
        const auto& output_sizes = argument.shape;

        std::vector<tensor::value_type> input_dims = { input_sizes.batch[0], input_sizes.feature[0],
            input_sizes.spatial[1], input_sizes.spatial[0] };
        std::vector<tensor::value_type> output_dims = { output_sizes.batch[0], output_sizes.feature[0],
            output_sizes.spatial[1], output_sizes.spatial[0] };

        const auto& one_hot_axis = node.get_primitive()->one_hot_axis;
        if (input_dims[0] != 1)
        {
            CLDNN_ERROR_MESSAGE(node.id(), "Incorrect parameters configuration: input batch size should be equal to 1.");
        }

        //bfyx format
        for (int i = 3, j = 3; i > 0; --i, --j)
        {
            if (j == one_hot_axis)
                --j;
            if (input_dims[i] != output_dims[j])
            {
                CLDNN_ERROR_MESSAGE(node.id(), "Incorrect parameters configuration: shape does not fit input size.");
            }
        }
    }
}
