
// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "core/providers/webgpu/shader_helper.h"
#include "core/providers/webgpu/webgpu_supported_types.h"
#include "core/providers/webgpu/webgpu_utils.h"
#include "core/providers/webgpu/nn/layer_norm.h"

namespace onnxruntime {
namespace webgpu {

static size_t NormalizeAxis(int64_t axis, size_t tensor_rank) {
  int64_t rank = static_cast<int64_t>(tensor_rank);
  if (axis < -rank && axis >= rank) {
    ORT_THROW("invalid axis: ", axis);
  }
  return onnxruntime::narrow<size_t>(axis < 0 ? axis + rank : axis);
}

Status LayerNormProgram::GenerateShaderCode(ShaderHelper& shader) const {
  const auto& x = shader.AddInput("x", ShaderUsage::UseUniform | ShaderUsage::UseValueTypeAlias);
  shader.AddInput("scale", ShaderUsage::UseUniform);
  if (has_bias_) {
    shader.AddInput("bias", ShaderUsage::UseUniform);
  }
  shader.AddOutput("y", ShaderUsage::UseUniform);
  if (has_mean_output_) {
    shader.AddOutput("mean_output", ShaderUsage::None);
  }
  if (has_inv_std_dev_output_) {
    shader.AddOutput("inv_std_dev_output", ShaderUsage::None);
  }

  int components = x.NumComponents();
  std::string bias = (has_bias_) ? " + bias[j]" : "";
  std::string simpl1 = (simplified_) ? "" : " - mean * mean";
  std::string simpl2 = (simplified_) ? "" : " - mean";

  shader.AdditionalImplementation() << "alias element_t = " << (is_fp16_ ? "f16;\n" : "f32;\n")
                                    << "alias f32_val_t = " << (components == 4 ? "vec4<f32>" : (components == 2 ? "vec2<f32>" : "f32")) << ";\n";

  shader.MainFunctionBody() << shader.GuardAgainstOutOfBoundsWorkgroupSizes("uniforms.norm_count")
                            << "let offset = global_idx * uniforms.norm_size_vectorized;\n"
                            << "var mean_vector = f32_val_t(0);\n"
                            << "var mean_square_vector = f32_val_t(0);\n"
                            << "for (var h: u32 = 0u; h < uniforms.norm_size_vectorized; h++) {\n"
                            << "   let value = f32_val_t(x[h + offset]);\n"
                            << "   mean_vector += value;\n"
                            << "   mean_square_vector += value * value;\n"
                            << "}\n"
                            << "let mean = " << SumVector("mean_vector", components) << " / f32(uniforms.norm_size);\n"
                            << "let inv_std_dev = inverseSqrt(" << SumVector("mean_square_vector", components) << " / f32(uniforms.norm_size)" << simpl1 << " + uniforms.epsilon);\n"
                            << "for (var j: u32 = 0; j < uniforms.norm_size_vectorized; j++) {\n"
                            << "   let f32input = f32_val_t(x[j + offset]);\n"
                            << "   let f32scale = f32_val_t(scale[j]);\n"
                            << "   y[j + offset] =  x_value_t((f32input" << simpl2 << ") * inv_std_dev * f32scale)" << bias << ";\n"
                            << "}\n";
  if (has_mean_output_) {
    shader.MainFunctionBody() << "mean_output[global_idx] = mean;\n";
  }
  if (has_inv_std_dev_output_) {
    shader.MainFunctionBody() << "inv_std_dev_output[global_idx] = inv_std_dev;\n";
  }

  return Status::OK();
}

template <bool simplified>
Status LayerNorm<simplified>::ComputeInternal(onnxruntime::webgpu::ComputeContext& context) const {
  const auto* x = context.Input(0);
  const auto* scale = context.Input(1);
  const auto* bias = context.Input(2);

  const auto x_shape = x->Shape();

  const bool is_fp16 = x->GetElementType() == ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16;

  const size_t axis = NormalizeAxis(axis_, x_shape.NumDimensions());
  const uint32_t norm_count = onnxruntime::narrow<uint32_t>(x_shape.SizeToDimension(axis));
  const int64_t norm_size = x_shape.SizeFromDimension(axis);
  const int components = GetMaxComponents(norm_size);
  const uint32_t norm_size_vectorized = onnxruntime::narrow<uint32_t>((norm_size + components - 1) / components);

  const auto scale_size = scale->Shape().Size();
  const auto bias_size = (bias) ? bias->Shape().Size() : 0;
  if (scale_size != norm_size || (bias && bias_size != norm_size)) {
    return ORT_MAKE_STATUS(ONNXRUNTIME, INVALID_ARGUMENT,
                           "Size of X.shape()[axis:] == ", norm_size,
                           ". Size of scale and bias (if provided) must match this. Got scale size of ",
                           scale_size, " and bias size of ", bias_size);
  }

  TensorShapeVector mean_dim;
  for (size_t i = 0; i < x_shape.NumDimensions(); ++i) {
    if (i < axis) {
      mean_dim.push_back(x_shape[i]);
    } else {
      mean_dim.push_back(1);
    }
  }
  TensorShape mean_shape(mean_dim);

  auto* y = context.Output(0, x_shape);
  auto* mean = context.Output(1, mean_shape);
  auto* inv_std_dev = context.Output(2, mean_shape);

  if (x_shape.Size() == 0) {
    return Status::OK();
  }

  LayerNormProgram program{bias != nullptr, is_fp16, simplified, mean != nullptr, inv_std_dev != nullptr};

  program
      .CacheHint(simplified)
      .AddInputs({{x, ProgramTensorMetadataDependency::Type, components}})
      .AddInputs({{scale, ProgramTensorMetadataDependency::Type, components}})
      .AddOutputs({{y, ProgramTensorMetadataDependency::None, components}})
      .SetDispatchGroupSize((norm_count + WORKGROUP_SIZE - 1) / WORKGROUP_SIZE)
      .AddUniformVariables({
          {static_cast<uint32_t>(norm_count)},
      })
      .AddUniformVariables({
          {static_cast<uint32_t>(norm_size)},
      })
      .AddUniformVariables({
          {static_cast<uint32_t>(norm_size_vectorized)},
      })
      .AddUniformVariables({
          {static_cast<float>(epsilon_)},
      });

  if (bias != nullptr) {
    program.AddInput({bias, ProgramTensorMetadataDependency::Type, components});
  }

  if (mean != nullptr) {
    program.AddOutputs({{mean, ProgramTensorMetadataDependency::None}});
  }
  if (inv_std_dev != nullptr) {
    program.AddOutputs({{inv_std_dev, ProgramTensorMetadataDependency::None}});
  }

  return context.RunProgram(program);
}

ONNX_OPERATOR_KERNEL_EX(LayerNormalization, kOnnxDomain, 17, kWebGpuExecutionProvider,
                        (*KernelDefBuilder::Create()).TypeConstraint("T", WebGpuSupportedFloatTypes()),
                        LayerNorm<false>);

ONNX_OPERATOR_KERNEL_EX(SimplifiedLayerNormalization, kOnnxDomain, 1, kWebGpuExecutionProvider,
                        (*KernelDefBuilder::Create())
                            .TypeConstraint("T", WebGpuSupportedFloatTypes())
                            .TypeConstraint("U", WebGpuSupportedFloatTypes()),
                        LayerNorm<true>);

}  // namespace webgpu
}  // namespace onnxruntime
