// Copyright 2016, Tobias Hermann.
// https://github.com/Dobiasd/frugally-deep
// Distributed under the MIT License.
// (See accompanying LICENSE file or at
//  https://opensource.org/licenses/MIT)

#pragma once

#include "fdeep/common.hpp"

#include "fdeep/filter.hpp"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <vector>

namespace fdeep { namespace internal
{

// todo: Is this still needed, or do the raw filters already have the right format?
struct im2col_filter_matrix
{
    tensor_shape filter_shape_;
    std::size_t filter_count_;
    float_vec biases_;
    bool use_bias_;
    std::vector<ColMajorMatrixXf> filter_mats_;
};

inline im2col_filter_matrix generate_im2col_filter_matrix(
    const std::vector<filter>& filters)
{
    assertion(fplus::all_the_same_on(
        fplus_c_mem_fn_t(filter, shape, tensor_shape), filters),
        "all filters must have the same shape");

    const auto biases = fplus::transform_convert<float_vec>(
        fplus_c_mem_fn_t(filter, get_bias, float_type),
        filters);

    const bool use_bias =
        fplus::sum(biases) != static_cast<float_type>(0) ||
        !fplus::all_the_same(biases);

    const auto shape = filters.front().shape();

    std::vector<ColMajorMatrixXf> filter_mats(
        shape.height_,
        ColMajorMatrixXf::Zero(
            static_cast<EigenIndex>(shape.width_ * shape.depth_),
            static_cast<EigenIndex>(filters.size())));

    for (std::size_t y = 0; y < shape.height_; ++y)
    {
        for (std::size_t n = 0; n < filters.size(); ++n)
        {
            for (std::size_t x = 0; x < shape.width_; ++x)
            {
                for (std::size_t z = 0; z < shape.depth_; ++z)
                {
                    filter_mats[y](
                        static_cast<EigenIndex>(x * shape.depth_ + z),
                        static_cast<EigenIndex>(n)
                    ) = filters[n].get(tensor_pos(y, x, z));
                }
            }
        }
    }

    return {shape, filters.size(), biases, use_bias, filter_mats};
}

inline im2col_filter_matrix generate_im2col_single_filter_matrix(
    const filter& filter)
{
    return generate_im2col_filter_matrix(filter_vec(1, filter));
}

inline tensor convolve_accumulative(
    std::size_t out_height,
    std::size_t out_width,
    std::size_t strides_y,
    std::size_t strides_x,
    const im2col_filter_matrix& filter_mat,
    const tensor& in)
{
    const std::vector<ColMajorMatrixXf>& filter_mats = filter_mat.filter_mats_;
    const auto f_height = filter_mat.filter_shape_.height_;
    const auto f_width = filter_mat.filter_shape_.width_;
    const auto f_depth = filter_mat.filter_shape_.depth_;
    const auto out_depth = filter_mat.filter_count_;

    assertion(f_depth == in.shape().depth_, "filter depth does not match input");
    assertion(filter_mats.size() == f_height, "incorrect number of filter levels in y direction");

    tensor output(tensor_shape(1, 1, out_height, out_width, out_depth), static_cast<float_type>(0));
    
    const EigenIndex times = static_cast<EigenIndex>((in.shape().width_ - f_width) / strides_x + 1);
    for (std::size_t y_filt = 0; y_filt < f_height; ++y_filt)
    {
        const ColMajorMatrixXf& filter = filter_mats[y_filt];
        // todo: can we get rid of this loop too?
        for (std::size_t y = 0, y_out = 0; y < in.shape().height_ + 1 - f_height; y += strides_y, ++y_out)
        {
            const float_type* input_ptr = &in.get_ref_ignore_rank(tensor_pos(0, 0, y + y_filt, 0, 0));
            Eigen::Map<Eigen::Matrix<float_type, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor>, Eigen::Unaligned, Eigen::OuterStride<>>
                input(const_cast<float_type*>(input_ptr),
                    times,
                    static_cast<EigenIndex>(f_width * f_depth),
                    Eigen::OuterStride<>(static_cast<EigenIndex>(f_depth * strides_x)));
            
            float_type* output_ptr = &output.get_ref_ignore_rank(tensor_pos(0, 0, y_out, 0, 0));
            Eigen::Map<Eigen::Matrix<float_type, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor>, Eigen::Unaligned>
                output_map(output_ptr,
                times,
                static_cast<EigenIndex>(out_depth));
            
            output_map.noalias() += input * filter;
        }
    }

    if (filter_mat.use_bias_) {
        for (std::size_t y_out = 0; y_out < out_height; ++y_out)
        {
            for (std::size_t x_out = 0; x_out < out_width; ++x_out)
            {
                for (std::size_t z_out = 0; z_out < out_depth; ++z_out)
                {
                    output.get_ref_ignore_rank(tensor_pos(0, 0, y_out, x_out, z_out)) += filter_mat.biases_[z_out];
                }
            }
        }
    }

    return tensor(
        tensor_shape_with_changed_rank(
            tensor_shape(out_height, out_width, out_depth),
            in.shape().rank()),
        output.as_vector());
}

enum class padding { valid, same, causal };

struct convolution_config
{
    std::size_t pad_top_;
    std::size_t pad_bottom_;
    std::size_t pad_left_;
    std::size_t pad_right_;
    std::size_t out_height_;
    std::size_t out_width_;
};

inline convolution_config preprocess_convolution(
    const shape2& filter_shape,
    const shape2& strides,
    padding pad_type,
    std::size_t input_shape_height,
    std::size_t input_shape_width)
{
    // https://www.tensorflow.org/api_guides/python/nn#Convolution
    const int filter_height = static_cast<int>(filter_shape.height_);
    const int filter_width = static_cast<int>(filter_shape.width_);
    const int in_height = static_cast<int>(input_shape_height);
    const int in_width = static_cast<int>(input_shape_width);
    const int strides_y = static_cast<int>(strides.height_);
    const int strides_x = static_cast<int>(strides.width_);

    int out_height = 0;
    int out_width = 0;

    if (pad_type == padding::same || pad_type == padding::causal)
    {
        out_height = fplus::ceil(static_cast<float>(in_height) / static_cast<float>(strides_y) - 0.001);
        out_width  = fplus::ceil(static_cast<float>(in_width) / static_cast<float>(strides_x) - 0.001);
    }
    else
    {
        out_height = fplus::ceil(static_cast<float>(in_height - filter_height + 1) / static_cast<float>(strides_y) - 0.001);
        out_width = fplus::ceil(static_cast<float>(in_width - filter_width + 1) / static_cast<float>(strides_x) - 0.001);
    }

    int pad_top = 0;
    int pad_bottom = 0;
    int pad_left = 0;
    int pad_right = 0;

    if (pad_type == padding::same)
    {
        int pad_along_height = 0;
        int pad_along_width = 0;

        if (in_height % strides_y == 0)
            pad_along_height = std::max(filter_height - strides_y, 0);
        else
            pad_along_height = std::max(filter_height - (in_height % strides_y), 0);
        if (in_width % strides_x == 0)
            pad_along_width = std::max(filter_width - strides_x, 0);
        else
            pad_along_width = std::max(filter_width - (in_width % strides_x), 0);

        pad_top = pad_along_height / 2;
        pad_bottom = pad_along_height - pad_top;
        pad_left = pad_along_width / 2;
        pad_right = pad_along_width - pad_left;
    }
    else if (pad_type == padding::causal)
    {
        pad_top = filter_height - 1;
        pad_left = filter_width - 1;
    }

    std::size_t out_height_size_t = fplus::integral_cast_throw<std::size_t>(out_height);
    std::size_t out_width_size_t = fplus::integral_cast_throw<std::size_t>(out_width);
    std::size_t pad_top_size_t = fplus::integral_cast_throw<std::size_t>(pad_top);
    std::size_t pad_bottom_size_t = fplus::integral_cast_throw<std::size_t>(pad_bottom);
    std::size_t pad_left_size_t = fplus::integral_cast_throw<std::size_t>(pad_left);
    std::size_t pad_right_size_t = fplus::integral_cast_throw<std::size_t>(pad_right);

    return {pad_top_size_t, pad_bottom_size_t,
        pad_left_size_t, pad_right_size_t,
        out_height_size_t, out_width_size_t};
}

inline tensor convolve(
    const shape2& strides,
    const padding& pad_type,
    const im2col_filter_matrix& filter_mat,
    const tensor& input)
{
    assertion(filter_mat.filter_shape_.depth_ == input.shape().depth_,
        "invalid filter depth");

    const auto conv_cfg = preprocess_convolution(
        filter_mat.filter_shape_.without_depth(),
        strides, pad_type, input.shape().height_, input.shape().width_);

    const std::size_t out_height = conv_cfg.out_height_;
    const std::size_t out_width = conv_cfg.out_width_;

    const auto in_padded = pad_tensor(0,
        conv_cfg.pad_top_, conv_cfg.pad_bottom_, conv_cfg.pad_left_, conv_cfg.pad_right_,
        input);

    return convolve_accumulative(
        out_height, out_width,
        strides.height_, strides.width_,
        filter_mat,
        in_padded);
}

} } // namespace fdeep, namespace internal
