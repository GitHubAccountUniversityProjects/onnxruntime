// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "core/providers/cpu/nn/pool.h"

#include "core/framework/data_types_internal.h"
#include "core/framework/op_kernel_type_control_utils.h"
#include "core/platform/threadpool.h"
#include "core/providers/cpu/nn/pool_functors.h"
#include "core/providers/op_kernel_type_control.h"

using namespace ::onnxruntime::common;

namespace onnxruntime {

namespace op_kernel_type_control {
ORT_SPECIFY_OP_KERNEL_ARG_DEFAULT_TYPES(
    kCpuExecutionProvider, kOnnxDomain, MaxPool, 8, Input, 0,
    float,
    double);
ORT_SPECIFY_OP_KERNEL_ARG_DEFAULT_TYPES(
    kCpuExecutionProvider, kOnnxDomain, MaxPool, 12, Input, 0,
    double,
    float,
    int8_t,
    uint8_t);
}  // namespace op_kernel_type_control

using EnabledMaxPool8DataTypes = ORT_OP_KERNEL_ARG_ENABLED_TYPE_LIST(
    kCpuExecutionProvider, kOnnxDomain, MaxPool, 8, Input, 0);
using EnabledMaxPool12DataTypes = ORT_OP_KERNEL_ARG_ENABLED_TYPE_LIST(
    kCpuExecutionProvider, kOnnxDomain, MaxPool, 12, Input, 0);

using AllEnabledMaxPoolDataTypes =
    utils::TypeSetUnion<
        EnabledMaxPool8DataTypes,
        EnabledMaxPool12DataTypes>;

template <typename T>
inline static void RunLoop(concurrency::ThreadPool* tp, std::ptrdiff_t total_channels, T&& task) {
  concurrency::ThreadPool::TryParallelFor(tp, total_channels, task.Cost(), task);
}

template <typename T, typename PoolType>
Status Pool<T, PoolType>::Compute(OpKernelContext* context) const {
  concurrency::ThreadPool* tp = context->GetOperatorThreadPool();
  const auto* X = context->Input<Tensor>(0);
  const TensorShape& x_shape = X->Shape();

  ORT_RETURN_IF_NOT(x_shape.NumDimensions() >= 3, "Input dimension cannot be less than 3.");

  auto pads = pool_attrs_.pads;
  auto kernel_shape = pool_attrs_.kernel_shape;

  if (pool_attrs_.global_pooling) {
    const auto& input_dims = x_shape.GetDims();
    kernel_shape.assign(input_dims.begin() + 2, input_dims.end());
    pads.assign(kernel_shape.size(), 0);
  }

  auto output_dims = pool_attrs_.SetOutputSize(x_shape, x_shape[1], &pads);
  Tensor* Y = context->Output(0, output_dims);

  const auto* X_data = X->Data<T>();
  auto* Y_data = Y->MutableData<T>();

  // The main loop
  const int64_t channels = x_shape[1];
  const int64_t height = x_shape[2];
  const int64_t width = kernel_shape.size() > 1 ? x_shape[3] : 1;
  const int64_t depth = kernel_shape.size() > 2 ? x_shape[4] : 1;
  const int64_t pooled_height = output_dims[2];
  const int64_t pooled_width = kernel_shape.size() > 1 ? output_dims[3] : 1;
  const int64_t pooled_depth = kernel_shape.size() > 2 ? output_dims[4] : 1;
  const int64_t total_channels = x_shape[0] * channels;
  const int64_t x_step = height * width * depth;
  const int64_t y_step = pooled_height * pooled_width * pooled_depth;

  switch (kernel_shape.size()) {
    case 1: {
      RunLoop<Pool1DTask<T, PoolType>>(tp, onnxruntime::narrow<size_t>(total_channels),
                                       {X_data, Y_data, x_step, y_step, pooled_height, stride_h(), height, kernel_shape,
                                        pads, pool_context_, pool_attrs_});

      break;
    }

    case 2: {
      RunLoop<Pool2DTask<T, PoolType>>(tp, onnxruntime::narrow<size_t>(total_channels),
                                       {X_data, Y_data, x_step, y_step, pooled_height, pooled_width, stride_h(),
                                        stride_w(), height, width, kernel_shape, pads, pool_context_, pool_attrs_});

      break;
    }
    case 3: {
      RunLoop<Pool3DTask<T, PoolType>>(
          tp, onnxruntime::narrow<size_t>(total_channels),
          {X_data, Y_data, x_step, y_step, pooled_height, pooled_width, pooled_depth, stride_h(), stride_w(),
           stride_d(), height, width, depth, kernel_shape, pads, pool_context_, pool_attrs_});

      break;
    }
    default:
      return Status(ONNXRUNTIME, INVALID_ARGUMENT, "Unsupported pooling size : ");
  }

  return Status::OK();
}

Status PoolBase::Compute(OpKernelContext* context, MLAS_POOLING_KIND kind) const {
  const auto* X = context->Input<Tensor>(0);
  const TensorShape& x_shape = X->Shape();

  size_t input_dims = x_shape.NumDimensions();
  ORT_RETURN_IF_NOT(input_dims >= 3, "Input dimension cannot be less than 3.");

  size_t pooling_dims = input_dims - 2;
  if (pooling_dims > 3) {
    return Status(ONNXRUNTIME, INVALID_ARGUMENT, "Unsupported pooling size.");
  }
  if (!pool_attrs_.global_pooling) {
    ORT_RETURN_IF_NOT(pooling_dims == pool_attrs_.kernel_shape.size(),
                      "kernel_shape num_dims is not compatible with X num_dims.");
  }

  auto pads = pool_attrs_.pads;
  auto output_dims = pool_attrs_.SetOutputSize(x_shape, x_shape[1], &pads);
  TensorShape output_shape(output_dims);
  Tensor* Y = context->Output(0, output_shape);

  // edge case: one or more dims with value of 0
  if (output_shape.Size() == 0)
    return Status::OK();

  // Get access to the internal threadpool
  // Temporarily derive concurrency parameters without access to session state
  concurrency::ThreadPool* thread_pool = context->GetOperatorThreadPool();

  MlasPool(kind, pooling_dims, X->Shape().GetDims().data(),
           pool_attrs_.global_pooling ? nullptr : pool_attrs_.kernel_shape.data(),
           pool_attrs_.global_pooling ? nullptr : pads.data(),
           pool_attrs_.global_pooling ? nullptr : pool_attrs_.strides.data(), output_dims.data(),
           X->Data<float>(), Y->MutableData<float>(), thread_pool);

  return Status::OK();
}

template <>
Status Pool<float, MaxPool<1 /*VERSION*/>>::Compute(OpKernelContext* context) const {
  return PoolBase::Compute(context, MlasMaximumPooling);
}

template <>
Status Pool<float, AveragePool>::Compute(OpKernelContext* context) const {
  return PoolBase::Compute(context,
                           pool_attrs_.count_include_pad ? MlasAveragePoolingIncludePad : MlasAveragePoolingExcludePad);
}

Status MaxPoolV8::Compute(OpKernelContext* context) const {
  utils::MLTypeCallDispatcherFromTypeList<AllEnabledMaxPoolDataTypes>
      t_disp(context->Input<Tensor>(0)->GetElementType());
  return t_disp.InvokeRet<Status, ComputeHelper>(this, context);
}

template <typename T>
Status MaxPoolV8::ComputeImpl(OpKernelContext* context) const {
  concurrency::ThreadPool* tp = context->GetOperatorThreadPool();
  // Use MLAS pooling if the index output tensor is not used
  // and also if dilation is not required

  bool need_dilation = false;
  for (auto n : pool_attrs_.dilations) {
    need_dilation |= n > 1;
  }

  // MLAS implementation currently supports only floats
  if (std::is_same<T, float>::value) {
    if (OpKernel::Node().OutputDefs().size() == 1 && pool_attrs_.storage_order == 0 && !need_dilation) {
      return PoolBase::Compute(context, MlasMaximumPooling);
    }
  }

  const auto* X = context->Input<Tensor>(0);
  const TensorShape& x_shape = X->Shape();

  ORT_RETURN_IF_NOT(x_shape.NumDimensions() >= 3, "Input dimension cannot be less than 3.");

  auto pads = pool_attrs_.pads;
  auto kernel_shape = pool_attrs_.kernel_shape;

  auto output_dims = pool_attrs_.SetOutputSize(x_shape, x_shape[1], &pads);
  Tensor* Y = context->Output(0, output_dims);
  Tensor* I = context->Output(1, output_dims);

  const auto* X_data = X->Data<T>();
  auto* Y_data = Y->MutableData<T>();
  int64_t* I_data = I != nullptr ? I->MutableData<int64_t>() : nullptr;

  // The main loop
  int64_t channels = x_shape[1];
  int64_t height = x_shape[2];
  int64_t width = kernel_shape.size() > 1 ? x_shape[3] : 1;
  int64_t depth = kernel_shape.size() > 2 ? x_shape[4] : 1;
  int64_t pooled_height = output_dims[2];
  int64_t pooled_width = kernel_shape.size() > 1 ? output_dims[3] : 1;
  int64_t pooled_depth = kernel_shape.size() > 2 ? output_dims[4] : 1;
  const int64_t total_channels = x_shape[0] * channels;

  switch (kernel_shape.size()) {
    case 1: {
      int64_t x_step = height;
      int64_t y_step = pooled_height;
      const int64_t dilation_h = pool_attrs_.dilations[0];

      RunLoop<MaxPool1DTask<T>>(tp, onnxruntime::narrow<size_t>(total_channels),
                                {X_data, Y_data, I_data, x_step, y_step, dilation_h, pooled_height, stride_h(),
                                 height, kernel_shape, pads});
      break;
    }

    case 2: {
      int64_t x_step = height * width;
      int64_t y_step = pooled_height * pooled_width;
      const int64_t dilation_h = pool_attrs_.dilations[0];
      const int64_t dilation_w = pool_attrs_.dilations[1];
      RunLoop<MaxPool2DTask<T>>(
          tp, onnxruntime::narrow<size_t>(total_channels),
          {X_data, Y_data, I_data, x_step, y_step, dilation_h, dilation_w, pooled_height, pooled_width, stride_h(),
           stride_w(), height, width, kernel_shape, pads, pool_attrs_.storage_order});
      break;
    }
    case 3: {
      int64_t x_step = height * width * depth;
      int64_t y_step = pooled_height * pooled_width * pooled_depth;
      const int64_t dilation_h = pool_attrs_.dilations[0];
      const int64_t dilation_w = pool_attrs_.dilations[1];
      const int64_t dilation_d = pool_attrs_.dilations[2];
      RunLoop<MaxPool3DTask<T>>(tp, onnxruntime::narrow<size_t>(total_channels),
                                {X_data, Y_data, I_data, x_step, y_step,
                                 dilation_h, dilation_w, dilation_d, pooled_height, pooled_width,
                                 pooled_depth, stride_h(), stride_w(), stride_d(), height,
                                 width, depth, kernel_shape, pads, pool_attrs_.storage_order});
      break;
    }
    default:
      return Status(ONNXRUNTIME, INVALID_ARGUMENT, "Unsupported pooling size : ");
  }

  return Status::OK();
}

template <typename T>
Status AveragePoolV19<T>::Compute(OpKernelContext* context) const {
  concurrency::ThreadPool* tp = context->GetOperatorThreadPool();
  bool need_dilation = false;
  for (auto n : pool_attrs_.dilations) {
    need_dilation |= n > 1;
  }

  const auto* X = context->Input<Tensor>(0);
  const TensorShape& x_shape = X->Shape();

  ORT_RETURN_IF_NOT(x_shape.NumDimensions() >= 3, "Input dimension cannot be less than 3.");

  auto pads = pool_attrs_.pads;
  auto kernel_shape = pool_attrs_.kernel_shape;

  auto output_dims = pool_attrs_.SetOutputSize(x_shape, x_shape[1], &pads);
  Tensor* Y = context->Output(0, output_dims);

  const auto* X_data = X->Data<T>();
  auto* Y_data = Y->MutableData<T>();

  // The main loop
  int64_t channels = x_shape[1];
  int64_t height = x_shape[2];
  int64_t width = kernel_shape.size() > 1 ? x_shape[3] : 1;
  int64_t depth = kernel_shape.size() > 2 ? x_shape[4] : 1;
  int64_t pooled_height = output_dims[2];
  int64_t pooled_width = kernel_shape.size() > 1 ? output_dims[3] : 1;
  int64_t pooled_depth = kernel_shape.size() > 2 ? output_dims[4] : 1;
  const int64_t total_channels = x_shape[0] * channels;

  switch (kernel_shape.size()) {
    case 1: {
      int64_t x_step = height;
      int64_t y_step = pooled_height;
      const int64_t dilation_h = pool_attrs_.dilations[0];

      RunLoop<AveragePool1DTask<T>>(tp, onnxruntime::narrow<size_t>(total_channels),
                                    {X_data, Y_data, x_step, y_step, dilation_h, pooled_height, stride_h(),
                                     height, kernel_shape, pads, pool_attrs_.count_include_pad, p_});
      break;
    }

    case 2: {
      int64_t x_step = height * width;
      int64_t y_step = pooled_height * pooled_width;
      const int64_t dilation_h = pool_attrs_.dilations[0];
      const int64_t dilation_w = pool_attrs_.dilations[1];
      RunLoop<AveragePool2DTask<T>>(
          tp, onnxruntime::narrow<size_t>(total_channels),
          {X_data, Y_data, x_step, y_step, dilation_h, dilation_w, pooled_height, pooled_width, stride_h(),
           stride_w(), height, width, kernel_shape, pads, pool_attrs_.count_include_pad, p_});
      break;
    }
    case 3: {
      int64_t x_step = height * width * depth;
      int64_t y_step = pooled_height * pooled_width * pooled_depth;
      const int64_t dilation_h = pool_attrs_.dilations[0];
      const int64_t dilation_w = pool_attrs_.dilations[1];
      const int64_t dilation_d = pool_attrs_.dilations[2];
      RunLoop<AveragePool3DTask<T>>(tp, onnxruntime::narrow<size_t>(total_channels),
                                    {X_data, Y_data, x_step, y_step,
                                     dilation_h, dilation_w, dilation_d, pooled_height, pooled_width,
                                     pooled_depth, stride_h(), stride_w(), stride_d(), height,
                                     width, depth, kernel_shape, pads, pool_attrs_.count_include_pad, p_});
      break;
    }
    default:
      return Status(ONNXRUNTIME, INVALID_ARGUMENT, "Unsupported kernel dimension : " + std::to_string(kernel_shape.size()));
  }

  return Status::OK();
}

template <typename T>
Status LpPoolV18<T>::Compute(OpKernelContext* context) const {
  concurrency::ThreadPool* tp = context->GetOperatorThreadPool();
  bool need_dilation = false;
  for (auto n : pool_attrs_.dilations) {
    need_dilation |= n > 1;
  }

  const auto* X = context->Input<Tensor>(0);
  const TensorShape& x_shape = X->Shape();

  ORT_RETURN_IF_NOT(x_shape.NumDimensions() >= 3, "Input dimension cannot be less than 3.");

  auto pads = pool_attrs_.pads;
  auto kernel_shape = pool_attrs_.kernel_shape;

  auto output_dims = pool_attrs_.SetOutputSize(x_shape, x_shape[1], &pads);
  Tensor* Y = context->Output(0, output_dims);

  const auto* X_data = X->Data<T>();
  auto* Y_data = Y->MutableData<T>();

  // The main loop
  int64_t channels = x_shape[1];
  int64_t height = x_shape[2];
  int64_t width = kernel_shape.size() > 1 ? x_shape[3] : 1;
  int64_t depth = kernel_shape.size() > 2 ? x_shape[4] : 1;
  int64_t pooled_height = output_dims[2];
  int64_t pooled_width = kernel_shape.size() > 1 ? output_dims[3] : 1;
  int64_t pooled_depth = kernel_shape.size() > 2 ? output_dims[4] : 1;
  const int64_t total_channels = x_shape[0] * channels;

  switch (kernel_shape.size()) {
    case 1: {
      int64_t x_step = height;
      int64_t y_step = pooled_height;
      const int64_t dilation_h = pool_attrs_.dilations[0];

      RunLoop<LpPool1DTask<T>>(tp, onnxruntime::narrow<size_t>(total_channels),
                               {X_data, Y_data, x_step, y_step, dilation_h, pooled_height, stride_h(),
                                height, kernel_shape, pads, p_});
      break;
    }

    case 2: {
      int64_t x_step = height * width;
      int64_t y_step = pooled_height * pooled_width;
      const int64_t dilation_h = pool_attrs_.dilations[0];
      const int64_t dilation_w = pool_attrs_.dilations[1];
      RunLoop<LpPool2DTask<T>>(
          tp, onnxruntime::narrow<size_t>(total_channels),
          {X_data, Y_data, x_step, y_step, dilation_h, dilation_w, pooled_height, pooled_width, stride_h(),
           stride_w(), height, width, kernel_shape, pads, p_});
      break;
    }
    case 3: {
      int64_t x_step = height * width * depth;
      int64_t y_step = pooled_height * pooled_width * pooled_depth;
      const int64_t dilation_h = pool_attrs_.dilations[0];
      const int64_t dilation_w = pool_attrs_.dilations[1];
      const int64_t dilation_d = pool_attrs_.dilations[2];
      RunLoop<LpPool3DTask<T>>(tp, onnxruntime::narrow<size_t>(total_channels),
                               {X_data, Y_data, x_step, y_step,
                                dilation_h, dilation_w, dilation_d, pooled_height, pooled_width,
                                pooled_depth, stride_h(), stride_w(), stride_d(), height,
                                width, depth, kernel_shape, pads, p_});
      break;
    }
    default:
      return Status(ONNXRUNTIME, INVALID_ARGUMENT, "Unsupported kernel dimension : " + std::to_string(kernel_shape.size()));
  }

  return Status::OK();
}

#define REGISTER_KERNEL_VERSIONED(OpName, START_VER, END_VER, ...) \
  ONNX_CPU_OPERATOR_VERSIONED_KERNEL(                              \
      OpName,                                                      \
      START_VER,                                                   \
      END_VER,                                                     \
      KernelDefBuilder().TypeConstraint("T", DataTypeImpl::GetTensorType<float>()), __VA_ARGS__);

#define REGISTER_KERNEL(OpName, VER, ...) \
  ONNX_CPU_OPERATOR_KERNEL(               \
      OpName,                             \
      VER,                                \
      KernelDefBuilder().TypeConstraint("T", DataTypeImpl::GetTensorType<float>()), __VA_ARGS__);

REGISTER_KERNEL_VERSIONED(AveragePool, 7, 9, Pool<float, AveragePool>);
REGISTER_KERNEL_VERSIONED(AveragePool, 10, 10, Pool<float, AveragePool>);
REGISTER_KERNEL_VERSIONED(AveragePool, 11, 18, Pool<float, AveragePool>);
REGISTER_KERNEL_VERSIONED(AveragePool, 19, 21, AveragePoolV19<float>);
REGISTER_KERNEL(AveragePool, 22, AveragePoolV19<float>);

REGISTER_KERNEL_VERSIONED(MaxPool, 1, 7, Pool<float, MaxPool<1 /*VERSION*/>>);
ONNX_CPU_OPERATOR_VERSIONED_KERNEL(MaxPool, 8, 11,
                                   KernelDefBuilder()
                                       .TypeConstraint(
                                           "T",
                                           BuildKernelDefConstraintsFromTypeList<EnabledMaxPool8DataTypes>())
                                       .TypeConstraint("I", DataTypeImpl::GetTensorType<int64_t>()),
                                   MaxPoolV8);

ONNX_CPU_OPERATOR_VERSIONED_KERNEL(MaxPool, 12, 21,
                                   KernelDefBuilder()
                                       .TypeConstraint(
                                           "T",
                                           BuildKernelDefConstraintsFromTypeList<EnabledMaxPool12DataTypes>())
                                       .TypeConstraint("I", DataTypeImpl::GetTensorType<int64_t>()),
                                   MaxPoolV8);
ONNX_CPU_OPERATOR_KERNEL(MaxPool, 22,
                         KernelDefBuilder()
                             .TypeConstraint(
                                 "T",
                                 BuildKernelDefConstraintsFromTypeList<EnabledMaxPool12DataTypes>())
                             .TypeConstraint("I", DataTypeImpl::GetTensorType<int64_t>()),
                         MaxPoolV8);

REGISTER_KERNEL_VERSIONED(LpPool, 2, 10, Pool<float, LpPool>);
REGISTER_KERNEL_VERSIONED(LpPool, 11, 17, Pool<float, LpPool>);
REGISTER_KERNEL_VERSIONED(LpPool, 18, 21, LpPoolV18<float>);
REGISTER_KERNEL(LpPool, 22, LpPoolV18<float>);

REGISTER_KERNEL(GlobalLpPool, 2, Pool<float, LpPool>);

REGISTER_KERNEL_VERSIONED(GlobalAveragePool, 1, 21, Pool<float, AveragePool>);
REGISTER_KERNEL(GlobalAveragePool, 22, Pool<float, AveragePool>);

REGISTER_KERNEL_VERSIONED(GlobalMaxPool, 1, 21, Pool<float, MaxPool<1 /*VERSION*/>>);
REGISTER_KERNEL(GlobalMaxPool, 22, Pool<float, MaxPool<1 /*VERSION*/>>);

}  // namespace onnxruntime
