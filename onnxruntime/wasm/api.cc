// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#ifdef ENABLE_TRAINING_APIS
#include "onnxruntime_training_cxx_api.h"
#endif

#include "core/session/onnxruntime_cxx_api.h"
#include "api.h"

#ifdef USE_WEBGPU
namespace onnxruntime {
namespace webgpu {
WGPUDevice GetDevice(int);
}
}  // namespace onnxruntime
#endif

#include <iostream>
#include <sstream>
#include <vector>

namespace {
OrtEnv* g_env;
OrtErrorCode g_last_error_code;
std::string g_last_error_message;
}  // namespace

enum DataLocation {
  DATA_LOCATION_NONE = 0,
  DATA_LOCATION_CPU = 1,
  DATA_LOCATION_CPU_PINNED = 2,
  DATA_LOCATION_TEXTURE = 3,
  DATA_LOCATION_GPU_BUFFER = 4,
  DATA_LOCATION_ML_TENSOR = 5
};

static_assert(sizeof(const char*) == sizeof(size_t), "size of a pointer and a size_t value should be the same.");
#ifndef ORT_WASM64
static_assert(sizeof(size_t) == 4, "size of size_t should be 4 in this build (wasm32).");
#endif

OrtErrorCode CheckStatus(OrtStatusPtr status) {
  if (status) {
    std::string error_message = Ort::GetApi().GetErrorMessage(status);
    g_last_error_code = Ort::GetApi().GetErrorCode(status);
    g_last_error_message = Ort::Exception(std::move(error_message), g_last_error_code).what();
    Ort::GetApi().ReleaseStatus(status);
  } else {
    g_last_error_code = ORT_OK;
    g_last_error_message.clear();
  }
  return g_last_error_code;
}

#define CHECK_STATUS(ORT_API_NAME, ...) \
  CheckStatus(Ort::GetApi().ORT_API_NAME(__VA_ARGS__))

#define RETURN_ERROR_CODE_IF_ERROR(ORT_API_NAME, ...)         \
  do {                                                        \
    int error_code = CHECK_STATUS(ORT_API_NAME, __VA_ARGS__); \
    if (error_code != ORT_OK) {                               \
      return error_code;                                      \
    }                                                         \
  } while (false)

#define RETURN_NULLPTR_IF_ERROR(ORT_API_NAME, ...)           \
  do {                                                       \
    if (CHECK_STATUS(ORT_API_NAME, __VA_ARGS__) != ORT_OK) { \
      return nullptr;                                        \
    }                                                        \
  } while (false)

// use auto release macros to make sure resources get released on function return.

// create a unique_ptr wrapper for auto release
#define REGISTER_AUTO_RELEASE(T, var, release_t, release_func) \
  std::unique_ptr<T, release_t> auto_release_##var { var, release_func }
// register auto release for handle of Ort API resources
#define REGISTER_AUTO_RELEASE_HANDLE(T, var) \
  REGISTER_AUTO_RELEASE(Ort##T, var, void (*)(Ort##T*), [](Ort##T* p) { Ort::GetApi().Release##T(p); })
// register auto release for Ort allocated buffers
#define REGISTER_AUTO_RELEASE_BUFFER(T, var, allocator)                                     \
  auto auto_release_##var##_deleter = [allocator](T* p) { allocator->Free(allocator, p); }; \
  REGISTER_AUTO_RELEASE(T, var, decltype(auto_release_##var##_deleter), auto_release_##var##_deleter)
// unregister the auto release wrapper
#define UNREGISTER_AUTO_RELEASE(var) auto_release_##var.release()

int OrtInit(int num_threads, int logging_level) {
  // Assume that a logging level is check and properly set at JavaScript
#if defined(__EMSCRIPTEN_PTHREADS__)
  OrtThreadingOptions* tp_options = nullptr;
  RETURN_ERROR_CODE_IF_ERROR(CreateThreadingOptions, &tp_options);
  RETURN_ERROR_CODE_IF_ERROR(SetGlobalIntraOpNumThreads, tp_options, num_threads);
  RETURN_ERROR_CODE_IF_ERROR(SetGlobalInterOpNumThreads, tp_options, 1);

  return CHECK_STATUS(CreateEnvWithGlobalThreadPools,
                      static_cast<OrtLoggingLevel>(logging_level),
                      "Default",
                      tp_options,
                      &g_env);
#else
  return CHECK_STATUS(CreateEnv, static_cast<OrtLoggingLevel>(logging_level), "Default", &g_env);
#endif
}

int OrtGetLastError(int* error_code, const char** error_message) {
  *error_code = g_last_error_code;
  *error_message = g_last_error_message.empty() ? nullptr : g_last_error_message.c_str();
  return ORT_OK;
}

OrtSessionOptions* OrtCreateSessionOptions(size_t graph_optimization_level,
                                           bool enable_cpu_mem_arena,
                                           bool enable_mem_pattern,
                                           size_t execution_mode,
                                           bool enable_profiling,
                                           const char* /*profile_file_prefix*/,
                                           const char* log_id,
                                           size_t log_severity_level,
                                           size_t log_verbosity_level,
                                           const char* optimized_model_filepath) {
  OrtSessionOptions* session_options = nullptr;
  RETURN_NULLPTR_IF_ERROR(CreateSessionOptions, &session_options);
  REGISTER_AUTO_RELEASE_HANDLE(SessionOptions, session_options);

  if (optimized_model_filepath) {
    RETURN_NULLPTR_IF_ERROR(SetOptimizedModelFilePath, session_options, optimized_model_filepath);
  }

  // assume that a graph optimization level is checked and properly set at JavaScript
  RETURN_NULLPTR_IF_ERROR(SetSessionGraphOptimizationLevel,
                          session_options,
                          static_cast<GraphOptimizationLevel>(graph_optimization_level));

  if (enable_cpu_mem_arena) {
    RETURN_NULLPTR_IF_ERROR(EnableCpuMemArena, session_options);
  } else {
    RETURN_NULLPTR_IF_ERROR(DisableCpuMemArena, session_options);
  }

  if (enable_mem_pattern) {
    RETURN_NULLPTR_IF_ERROR(EnableMemPattern, session_options);
  } else {
    RETURN_NULLPTR_IF_ERROR(DisableMemPattern, session_options);
  }

  // assume that an execution mode is checked and properly set at JavaScript
  RETURN_NULLPTR_IF_ERROR(SetSessionExecutionMode, session_options, static_cast<ExecutionMode>(execution_mode));

  // TODO: support profling
  if (enable_profiling) {
    RETURN_NULLPTR_IF_ERROR(EnableProfiling, session_options, "");
  } else {
    RETURN_NULLPTR_IF_ERROR(DisableProfiling, session_options);
  }

  if (log_id != nullptr) {
    RETURN_NULLPTR_IF_ERROR(SetSessionLogId, session_options, log_id);
  }

  // assume that a log severity level is checked and properly set at JavaScript
  RETURN_NULLPTR_IF_ERROR(SetSessionLogSeverityLevel, session_options, log_severity_level);

  RETURN_NULLPTR_IF_ERROR(SetSessionLogVerbosityLevel, session_options, log_verbosity_level);

#ifdef ENABLE_EXTENSION_CUSTOM_OPS
  // Enable ORT CustomOps in onnxruntime-extensions
  RETURN_NULLPTR_IF_ERROR(EnableOrtCustomOps, session_options);
#endif

  return UNREGISTER_AUTO_RELEASE(session_options);
}

int OrtAppendExecutionProvider(ort_session_options_handle_t session_options,
                               const char* name,
                               const char* const* provider_options_keys,
                               const char* const* provider_options_values,
                               size_t num_keys) {
  return CHECK_STATUS(SessionOptionsAppendExecutionProvider, session_options, name, provider_options_keys, provider_options_values, num_keys);
}

int OrtAddFreeDimensionOverride(ort_session_options_handle_t session_options,
                                const char* dim_param_name,
                                int dim_value) {
  return CHECK_STATUS(AddFreeDimensionOverrideByName, session_options, dim_param_name, dim_value);
}

int OrtAddSessionConfigEntry(OrtSessionOptions* session_options,
                             const char* config_key,
                             const char* config_value) {
  return CHECK_STATUS(AddSessionConfigEntry, session_options, config_key, config_value);
}

int OrtReleaseSessionOptions(OrtSessionOptions* session_options) {
  Ort::GetApi().ReleaseSessionOptions(session_options);
  return ORT_OK;
}

OrtSession* OrtCreateSession(void* data, size_t data_length, OrtSessionOptions* session_options) {
#if defined(__EMSCRIPTEN_PTHREADS__)
  RETURN_NULLPTR_IF_ERROR(DisablePerSessionThreads, session_options);
#else
  // must disable thread pool when WebAssembly multi-threads support is disabled.
  RETURN_NULLPTR_IF_ERROR(SetIntraOpNumThreads, session_options, 1);
  RETURN_NULLPTR_IF_ERROR(SetSessionExecutionMode, session_options, ORT_SEQUENTIAL);
#endif

  OrtSession* session = nullptr;
  return (CHECK_STATUS(CreateSessionFromArray, g_env, data, data_length, session_options, &session) == ORT_OK)
             ? session
             : nullptr;
}

int OrtReleaseSession(OrtSession* session) {
  Ort::GetApi().ReleaseSession(session);
  return ORT_OK;
}

int OrtGetInputOutputCount(OrtSession* session, size_t* input_count, size_t* output_count) {
  RETURN_ERROR_CODE_IF_ERROR(SessionGetInputCount, session, input_count);
  RETURN_ERROR_CODE_IF_ERROR(SessionGetOutputCount, session, output_count);
  return ORT_OK;
}

int OrtGetInputOutputMetadata(ort_session_handle_t session, size_t index, char** name_cstr_ptr, void** type_info_ptr) {
  OrtAllocator* allocator = nullptr;
  RETURN_ERROR_CODE_IF_ERROR(GetAllocatorWithDefaultOptions, &allocator);

  size_t input_count, output_count;
  int error_code = OrtGetInputOutputCount(session, &input_count, &output_count);
  if (error_code != ORT_OK) {
    return error_code;
  }

  if (index >= input_count + output_count) {
    std::ostringstream ostr;
    ostr << "Invalid index: " << index << ", input count: " << input_count << ", output count: " << output_count;
    return CheckStatus(Ort::GetApi().CreateStatus(ORT_INVALID_ARGUMENT, ostr.str().c_str()));
  }

  char* name_cstr;
  if (index < input_count) {
    RETURN_ERROR_CODE_IF_ERROR(SessionGetInputName, session, index, allocator, &name_cstr);
  } else {
    RETURN_ERROR_CODE_IF_ERROR(SessionGetOutputName, session, index - input_count, allocator, &name_cstr);
  }
  REGISTER_AUTO_RELEASE_BUFFER(char, name_cstr, allocator);

  OrtTypeInfo* type_info;
  if (index < input_count) {
    RETURN_ERROR_CODE_IF_ERROR(SessionGetInputTypeInfo, session, index, &type_info);
  } else {
    RETURN_ERROR_CODE_IF_ERROR(SessionGetOutputTypeInfo, session, index - input_count, &type_info);
  }
  REGISTER_AUTO_RELEASE_HANDLE(TypeInfo, type_info);

  const OrtTensorTypeAndShapeInfo* tensor_info;
  RETURN_ERROR_CODE_IF_ERROR(CastTypeInfoToTensorInfo, type_info, &tensor_info);

  size_t type_info_size = 4;
  ONNXTensorElementDataType element_type = ONNX_TENSOR_ELEMENT_DATA_TYPE_UNDEFINED;
  size_t dim_count = 0;
  if (tensor_info != nullptr) {
    RETURN_ERROR_CODE_IF_ERROR(GetTensorElementType, tensor_info, &element_type);
    RETURN_ERROR_CODE_IF_ERROR(GetDimensionsCount, tensor_info, &dim_count);

    // byte [0, 4): [i32] element type
    // byte [4, 8): [u32] dimension count
    // byte [8, 8 + dim_count * ptr_size): [ptr] symbolic dimension names for dim[0], dim[1], ..., dim[dim_count - 1]
    // byte [8 + dim_count * ptr_size, 8 + dim_count * ptr_size * 2): [size_t] dimension values for dim[0], dim[1], ..., dim[dim_count - 1]
    // from byte 8 + dim_count * ptr_size * 2: optional string copies for symbolic dimension names
    type_info_size = 8 + dim_count * (sizeof(size_t) * 2);
  }

  std::vector<int64_t> dim_values(dim_count);
  std::vector<const char*> dim_params(dim_count);
  std::vector<size_t> dim_params_str_len(dim_count);
  if (dim_count > 0) {
    size_t str_len_total = 0;
    RETURN_ERROR_CODE_IF_ERROR(GetDimensions, tensor_info, dim_values.data(), dim_count);
    RETURN_ERROR_CODE_IF_ERROR(GetSymbolicDimensions, tensor_info, dim_params.data(), dim_count);
    for (size_t i = 0; i < dim_count; ++i) {
      size_t str_size = dim_params[i] ? strlen(dim_params[i]) : 0;
      if (str_size > 0) {
        str_len_total += str_size + 1;
        dim_params_str_len[i] = str_size + 1;
      } else {
        dim_params_str_len[i] = 0;
      }
    }
    type_info_size += str_len_total;
  }

  uint8_t* type_info_buffer = reinterpret_cast<uint8_t*>(allocator->Alloc(allocator, type_info_size));
  // write to buffer @ byte [0, 4)
  int32_t* p_type_info_element_type = reinterpret_cast<int32_t*>(type_info_buffer);
  *p_type_info_element_type = static_cast<int32_t>(element_type);

  if (tensor_info != nullptr) {
    // write to buffer @ byte [4, 8)
    uint32_t* p_type_info_dim_count = reinterpret_cast<uint32_t*>(type_info_buffer + 4);
    *p_type_info_dim_count = static_cast<uint32_t>(dim_count);

    if (dim_count > 0) {
      // write to buffer @ byte [8, 8 + dim_count * ptr_size)
      const char** p_dim_params = reinterpret_cast<const char**>(type_info_buffer + 8);
      char* p_str_copy_dest = reinterpret_cast<char*>(type_info_buffer + 8 + dim_count * sizeof(size_t) * 2);
      for (size_t i = 0; i < dim_count; ++i) {
        if (dim_params_str_len[i] > 0) {
          p_dim_params[i] = p_str_copy_dest;
          memcpy(p_str_copy_dest, dim_params[i], dim_params_str_len[i]);
          p_str_copy_dest += dim_params_str_len[i];
        } else {
          p_dim_params[i] = nullptr;
        }
      }

      // write to buffer @ byte [8 + dim_count * ptr_size, 8 + dim_count * ptr_size + dim_count * 4 + dim_count * 4)
      size_t* p_dim_values = reinterpret_cast<size_t*>(type_info_buffer + 8 + dim_count * sizeof(size_t));
      for (size_t i = 0; i < dim_count; ++i) {
        p_dim_values[i] = static_cast<size_t>(dim_values[i]);
      }
    }
  }

  UNREGISTER_AUTO_RELEASE(name_cstr);
  *name_cstr_ptr = name_cstr;
  *type_info_ptr = type_info_buffer;
  return ORT_OK;
}

char* OrtGetInputName(OrtSession* session, size_t index) {
  OrtAllocator* allocator = nullptr;
  RETURN_NULLPTR_IF_ERROR(GetAllocatorWithDefaultOptions, &allocator);

  char* input_name = nullptr;
  return (CHECK_STATUS(SessionGetInputName, session, index, allocator, &input_name) == ORT_OK)
             ? input_name
             : nullptr;
}

char* OrtGetOutputName(OrtSession* session, size_t index) {
  OrtAllocator* allocator = nullptr;
  RETURN_NULLPTR_IF_ERROR(GetAllocatorWithDefaultOptions, &allocator);

  char* output_name = nullptr;
  return (CHECK_STATUS(SessionGetOutputName, session, index, allocator, &output_name) == ORT_OK)
             ? output_name
             : nullptr;
}

int OrtFree(void* ptr) {
  OrtAllocator* allocator = nullptr;
  if (CHECK_STATUS(GetAllocatorWithDefaultOptions, &allocator) == ORT_OK) {
    allocator->Free(allocator, ptr);
  }
  return ORT_OK;
}

OrtValue* OrtCreateTensor(int data_type, void* data, size_t data_length, size_t* dims, size_t dims_length, int data_location) {
  if (data_location != DATA_LOCATION_CPU &&
      data_location != DATA_LOCATION_CPU_PINNED &&
      data_location != DATA_LOCATION_GPU_BUFFER &&
      data_location != DATA_LOCATION_ML_TENSOR) {
    std::ostringstream ostr;
    ostr << "Invalid data location: " << data_location;
    CheckStatus(Ort::GetApi().CreateStatus(ORT_INVALID_ARGUMENT, ostr.str().c_str()));
    return nullptr;
  }

  std::vector<int64_t> shapes(dims_length);
  for (size_t i = 0; i < dims_length; i++) {
    shapes[i] = dims[i];
  }

  if (data_type == ONNX_TENSOR_ELEMENT_DATA_TYPE_STRING) {
    // data_location is ignored for string tensor. It is always CPU.
    OrtAllocator* allocator = nullptr;
    RETURN_NULLPTR_IF_ERROR(GetAllocatorWithDefaultOptions, &allocator);

    OrtValue* value = nullptr;
    RETURN_NULLPTR_IF_ERROR(CreateTensorAsOrtValue, allocator,
                            dims_length > 0 ? shapes.data() : nullptr, dims_length,
                            ONNX_TENSOR_ELEMENT_DATA_TYPE_STRING, &value);
    REGISTER_AUTO_RELEASE_HANDLE(Value, value);

    const char* const* strings = reinterpret_cast<const char* const*>(data);
    RETURN_NULLPTR_IF_ERROR(FillStringTensor, value, strings, data_length / sizeof(const char*));

    return UNREGISTER_AUTO_RELEASE(value);
  } else {
    OrtMemoryInfo* memory_info = nullptr;
    switch (data_location) {
      case DATA_LOCATION_GPU_BUFFER:
        RETURN_NULLPTR_IF_ERROR(CreateMemoryInfo, "WebGPU_Buffer", OrtDeviceAllocator, 0, OrtMemTypeDefault, &memory_info);
        break;
      case DATA_LOCATION_ML_TENSOR:
        RETURN_NULLPTR_IF_ERROR(CreateMemoryInfo, "WebNN_Tensor", OrtDeviceAllocator, 0, OrtMemTypeDefault, &memory_info);
        break;
      default:
        RETURN_NULLPTR_IF_ERROR(CreateCpuMemoryInfo, OrtDeviceAllocator, OrtMemTypeDefault, &memory_info);
    }
    REGISTER_AUTO_RELEASE_HANDLE(MemoryInfo, memory_info);

    OrtValue* value = nullptr;
    int error_code = CHECK_STATUS(CreateTensorWithDataAsOrtValue, memory_info, data, data_length,
                                  dims_length > 0 ? shapes.data() : nullptr, dims_length,
                                  static_cast<ONNXTensorElementDataType>(data_type), &value);

    return (error_code == ORT_OK) ? value : nullptr;
  }
}

int OrtGetTensorData(OrtValue* tensor, size_t* data_type, void** data, size_t** dims, size_t* dims_length) {
  ONNXType tensor_type;
  RETURN_ERROR_CODE_IF_ERROR(GetValueType, tensor, &tensor_type);
  if (tensor_type != ONNX_TYPE_TENSOR) {
    return CheckStatus(
        Ort::GetApi().CreateStatus(ORT_NOT_IMPLEMENTED, "Reading data from non-tensor typed value is not supported."));
  }

  OrtTensorTypeAndShapeInfo* info = nullptr;
  RETURN_ERROR_CODE_IF_ERROR(GetTensorTypeAndShape, tensor, &info);
  REGISTER_AUTO_RELEASE_HANDLE(TensorTypeAndShapeInfo, info);

  size_t dims_len = 0;
  RETURN_ERROR_CODE_IF_ERROR(GetDimensionsCount, info, &dims_len);

  OrtAllocator* allocator = nullptr;
  RETURN_ERROR_CODE_IF_ERROR(GetAllocatorWithDefaultOptions, &allocator);

  size_t* p_dims = reinterpret_cast<size_t*>(allocator->Alloc(allocator, sizeof(size_t) * dims_len));
  REGISTER_AUTO_RELEASE_BUFFER(size_t, p_dims, allocator);

  ONNXTensorElementDataType type;
  RETURN_ERROR_CODE_IF_ERROR(GetTensorElementType, info, &type);

  std::vector<int64_t> shape(dims_len, 0);
  RETURN_ERROR_CODE_IF_ERROR(GetDimensions, info, shape.data(), shape.size());
  for (size_t i = 0; i < dims_len; i++) {
    p_dims[i] = static_cast<size_t>(shape[i]);
  }

  if (type == ONNX_TENSOR_ELEMENT_DATA_TYPE_STRING) {
    size_t num_elements;
    RETURN_ERROR_CODE_IF_ERROR(GetTensorShapeElementCount, info, &num_elements);

    // NOTE: ORT C-API does not expose an interface for users to get string raw data directly. There is always a copy.
    //       we can use the tensor raw data because it is type of "std::string *", which is very straightforward to
    //       implement and can also save memory usage. However, this approach depends on the Tensor's implementation
    //       details. So we have to copy the string content here.

    size_t string_data_length;
    RETURN_ERROR_CODE_IF_ERROR(GetStringTensorDataLength, tensor, &string_data_length);

    // The buffer contains following data:
    //  - a sequence of pointers to (const char*), size = num_elements * sizeof(const char*).
    //  - followed by a raw buffer to store string content, size = string_data_length + 1.
    size_t string_data_offset = num_elements * sizeof(const char*);
    size_t buf_size = string_data_offset + string_data_length;
    void* p_string_data = allocator->Alloc(allocator, buf_size + 1);
    void* p_string_content = reinterpret_cast<char*>(p_string_data) + string_data_offset;
    REGISTER_AUTO_RELEASE_BUFFER(void, p_string_data, allocator);

    size_t* p_offsets = reinterpret_cast<size_t*>(p_string_data);
    RETURN_ERROR_CODE_IF_ERROR(GetStringTensorContent, tensor, p_string_content, string_data_length, p_offsets, num_elements);

    // replace offsets by pointers
    const char** p_c_strs = reinterpret_cast<const char**>(p_offsets);
    for (size_t i = 0; i < num_elements; i++) {
      p_c_strs[i] = reinterpret_cast<const char*>(p_string_content) + p_offsets[i];
    }

    // put null at the last char
    reinterpret_cast<char*>(p_string_data)[buf_size] = '\0';

    *data = UNREGISTER_AUTO_RELEASE(p_string_data);
  } else {
    void* p_tensor_raw_data = nullptr;
    RETURN_ERROR_CODE_IF_ERROR(GetTensorMutableData, tensor, &p_tensor_raw_data);
    *data = p_tensor_raw_data;
  }

  *data_type = static_cast<size_t>(type);
  *dims_length = dims_len;
  *dims = UNREGISTER_AUTO_RELEASE(p_dims);
  return ORT_OK;
}

int OrtReleaseTensor(OrtValue* tensor) {
  Ort::GetApi().ReleaseValue(tensor);
  return ORT_OK;
}

OrtRunOptions* OrtCreateRunOptions(size_t log_severity_level,
                                   size_t log_verbosity_level,
                                   bool terminate,
                                   const char* tag) {
  OrtRunOptions* run_options = nullptr;
  RETURN_NULLPTR_IF_ERROR(CreateRunOptions, &run_options);
  REGISTER_AUTO_RELEASE_HANDLE(RunOptions, run_options);

  // Assume that a logging level is check and properly set at JavaScript
  RETURN_NULLPTR_IF_ERROR(RunOptionsSetRunLogSeverityLevel, run_options, log_severity_level);

  RETURN_NULLPTR_IF_ERROR(RunOptionsSetRunLogVerbosityLevel, run_options, log_verbosity_level);

  if (terminate) {
    RETURN_NULLPTR_IF_ERROR(RunOptionsSetTerminate, run_options);
  } else {
    RETURN_NULLPTR_IF_ERROR(RunOptionsUnsetTerminate, run_options);
  }

  if (tag != nullptr) {
    RETURN_NULLPTR_IF_ERROR(RunOptionsSetRunTag, run_options, tag);
  }

  return UNREGISTER_AUTO_RELEASE(run_options);
}

int OrtAddRunConfigEntry(OrtRunOptions* run_options,
                         const char* config_key,
                         const char* config_value) {
  return CHECK_STATUS(AddRunConfigEntry, run_options, config_key, config_value);
}

int OrtReleaseRunOptions(OrtRunOptions* run_options) {
  Ort::GetApi().ReleaseRunOptions(run_options);
  return ORT_OK;
}

OrtIoBinding* OrtCreateBinding(OrtSession* session) {
  OrtIoBinding* binding = nullptr;
  int error_code = CHECK_STATUS(CreateIoBinding, session, &binding);
  return (error_code == ORT_OK) ? binding : nullptr;
}

int EMSCRIPTEN_KEEPALIVE OrtBindInput(OrtIoBinding* io_binding,
                                      const char* name,
                                      OrtValue* input) {
  return CHECK_STATUS(BindInput, io_binding, name, input);
}

int EMSCRIPTEN_KEEPALIVE OrtBindOutput(OrtIoBinding* io_binding,
                                       const char* name,
                                       OrtValue* output,
                                       int output_location) {
  if (output) {
    return CHECK_STATUS(BindOutput, io_binding, name, output);
  } else {
    if (output_location != DATA_LOCATION_NONE &&
        output_location != DATA_LOCATION_CPU &&
        output_location != DATA_LOCATION_CPU_PINNED &&
        output_location != DATA_LOCATION_GPU_BUFFER &&
        output_location != DATA_LOCATION_ML_TENSOR) {
      std::ostringstream ostr;
      ostr << "Invalid data location (" << output_location << ") for output: \"" << name << "\".";
      return CheckStatus(Ort::GetApi().CreateStatus(ORT_INVALID_ARGUMENT, ostr.str().c_str()));
    }

    OrtMemoryInfo* memory_info = nullptr;
    if (output_location != DATA_LOCATION_GPU_BUFFER && output_location != DATA_LOCATION_ML_TENSOR) {
      RETURN_ERROR_CODE_IF_ERROR(CreateCpuMemoryInfo, OrtDeviceAllocator, OrtMemTypeDefault, &memory_info);
    } else if (output_location == DATA_LOCATION_ML_TENSOR) {
      RETURN_ERROR_CODE_IF_ERROR(CreateMemoryInfo, "WebNN_Tensor", OrtDeviceAllocator, 0, OrtMemTypeDefault, &memory_info);
    } else {
      RETURN_ERROR_CODE_IF_ERROR(CreateMemoryInfo, "WebGPU_Buffer", OrtDeviceAllocator, 0, OrtMemTypeDefault, &memory_info);
    }
    REGISTER_AUTO_RELEASE_HANDLE(MemoryInfo, memory_info);
    return CHECK_STATUS(BindOutputToDevice, io_binding, name, memory_info);
  }
}

int OrtClearBoundOutputs(OrtIoBinding* io_binding) {
  Ort::GetApi().ClearBoundOutputs(io_binding);
  return ORT_OK;
}

int OrtReleaseBinding(OrtIoBinding* io_binding) {
  Ort::GetApi().ReleaseIoBinding(io_binding);
  return ORT_OK;
}

int OrtRunWithBinding(OrtSession* session,
                      OrtIoBinding* io_binding,
                      size_t output_count,
                      OrtValue** outputs,
                      OrtRunOptions* run_options) {
  RETURN_ERROR_CODE_IF_ERROR(RunWithBinding, session, run_options, io_binding);

  OrtAllocator* allocator = nullptr;
  RETURN_ERROR_CODE_IF_ERROR(GetAllocatorWithDefaultOptions, &allocator);

  size_t binding_output_count = 0;
  OrtValue** binding_outputs = nullptr;
  RETURN_ERROR_CODE_IF_ERROR(GetBoundOutputValues, io_binding, allocator, &binding_outputs, &binding_output_count);
  REGISTER_AUTO_RELEASE_BUFFER(OrtValue*, binding_outputs, allocator);

  if (binding_output_count != output_count) {
    return CheckStatus(
        Ort::GetApi().CreateStatus(ORT_INVALID_ARGUMENT, "Output count is inconsistent with IO Binding output data."));
  }

  for (size_t i = 0; i < output_count; i++) {
    outputs[i] = binding_outputs[i];
  }

  return ORT_OK;
}

int OrtRun(OrtSession* session,
           const char** input_names, const ort_tensor_handle_t* inputs, size_t input_count,
           const char** output_names, size_t output_count, ort_tensor_handle_t* outputs,
           OrtRunOptions* run_options) {
  return CHECK_STATUS(Run, session, run_options, input_names, inputs, input_count, output_names, output_count, outputs);
}

char* OrtEndProfiling(ort_session_handle_t session) {
  OrtAllocator* allocator = nullptr;
  RETURN_NULLPTR_IF_ERROR(GetAllocatorWithDefaultOptions, &allocator);

  char* file_name = nullptr;
  return (CHECK_STATUS(SessionEndProfiling, session, allocator, &file_name) == ORT_OK)
             ? file_name
             : nullptr;
}

// WebGPU API Section

#ifdef USE_WEBGPU

WGPUDevice OrtGetWebGpuDevice(int device_id) {
  return onnxruntime::webgpu::GetDevice(device_id);
}

#endif

// Training API Section

#ifdef ENABLE_TRAINING_APIS
#define CHECK_TRAINING_STATUS(ORT_API_NAME, ...) \
  CheckStatus(Ort::GetTrainingApi().ORT_API_NAME(__VA_ARGS__))

#define RETURN_TRAINING_ERROR_CODE_IF_ERROR(ORT_API_NAME, ...)         \
  do {                                                                 \
    int error_code = CHECK_TRAINING_STATUS(ORT_API_NAME, __VA_ARGS__); \
    if (error_code != ORT_OK) {                                        \
      return error_code;                                               \
    }                                                                  \
  } while (false)

ort_training_checkpoint_handle_t EMSCRIPTEN_KEEPALIVE OrtTrainingLoadCheckpoint(void* checkpoint_data_buffer,
                                                                                size_t checkpoint_size) {
  OrtCheckpointState* checkpoint_state = nullptr;
  return (CHECK_TRAINING_STATUS(LoadCheckpointFromBuffer, checkpoint_data_buffer,
                                checkpoint_size, &checkpoint_state) == ORT_OK)
             ? checkpoint_state
             : nullptr;
}

int EMSCRIPTEN_KEEPALIVE OrtTrainingReleaseCheckpoint(ort_training_checkpoint_handle_t training_checkpoint_state_handle) {
  Ort::GetTrainingApi().ReleaseCheckpointState(training_checkpoint_state_handle);
  return ORT_OK;
}

ort_training_session_handle_t EMSCRIPTEN_KEEPALIVE OrtTrainingCreateSession(const ort_session_options_handle_t options,
                                                                            ort_training_checkpoint_handle_t training_checkpoint_state_handle,
                                                                            void* train_model,
                                                                            size_t train_size,
                                                                            void* eval_model,
                                                                            size_t eval_size,
                                                                            void* optimizer_model,
                                                                            size_t optimizer_size) {
  OrtTrainingSession* training_session = nullptr;
  return (CHECK_TRAINING_STATUS(CreateTrainingSessionFromBuffer, g_env, options,
                                training_checkpoint_state_handle, train_model, train_size,
                                eval_model, eval_size, optimizer_model, optimizer_size,
                                &training_session) == ORT_OK)
             ? training_session
             : nullptr;
}

int EMSCRIPTEN_KEEPALIVE OrtTrainingLazyResetGrad(ort_training_session_handle_t training_handle) {
  return CHECK_TRAINING_STATUS(LazyResetGrad, training_handle);
}

int EMSCRIPTEN_KEEPALIVE OrtTrainingRunTrainStep(ort_training_session_handle_t training_handle,
                                                 ort_tensor_handle_t* inputs,
                                                 size_t input_count,
                                                 ort_tensor_handle_t* outputs,
                                                 size_t output_count,
                                                 ort_run_options_handle_t options) {
  return CHECK_TRAINING_STATUS(TrainStep, training_handle, options, input_count, inputs, output_count, outputs);
}

int EMSCRIPTEN_KEEPALIVE OrtTrainingOptimizerStep(ort_training_session_handle_t training_handle,
                                                  const ort_run_options_handle_t run_options) {
  return CHECK_TRAINING_STATUS(OptimizerStep, training_handle, run_options);
}

int EMSCRIPTEN_KEEPALIVE OrtTrainingEvalStep(ort_training_session_handle_t training_handle,
                                             ort_tensor_handle_t* inputs,
                                             size_t input_count,
                                             ort_tensor_handle_t* outputs,
                                             size_t output_count,
                                             ort_run_options_handle_t options) {
  return CHECK_TRAINING_STATUS(EvalStep, training_handle,
                               options, input_count, inputs, output_count, outputs);
}

int EMSCRIPTEN_KEEPALIVE OrtTrainingGetParametersSize(ort_training_session_handle_t training_handle,
                                                      size_t* param_size,
                                                      bool trainable_only) {
  return CHECK_TRAINING_STATUS(GetParametersSize, training_handle, param_size, trainable_only);
}

int EMSCRIPTEN_KEEPALIVE OrtTrainingCopyParametersToBuffer(ort_training_session_handle_t training_handle,
                                                           ort_tensor_handle_t parameters_buffer,
                                                           size_t parameter_count,
                                                           bool trainable_only) {
  return CHECK_TRAINING_STATUS(CopyParametersToBuffer, training_handle, parameters_buffer, trainable_only);
}

int EMSCRIPTEN_KEEPALIVE OrtTrainingCopyParametersFromBuffer(ort_training_session_handle_t training_handle,
                                                             ort_tensor_handle_t parameters_buffer,
                                                             size_t parameter_count,
                                                             bool trainable_only) {
  return CHECK_TRAINING_STATUS(CopyBufferToParameters, training_handle, parameters_buffer, trainable_only);
}

int EMSCRIPTEN_KEEPALIVE OrtTrainingGetModelInputOutputCount(ort_training_session_handle_t training_handle,
                                                             size_t* input_count,
                                                             size_t* output_count,
                                                             bool isEvalModel) {
  if (isEvalModel) {
    RETURN_TRAINING_ERROR_CODE_IF_ERROR(TrainingSessionGetEvalModelInputCount, training_handle, input_count);
    RETURN_TRAINING_ERROR_CODE_IF_ERROR(TrainingSessionGetEvalModelOutputCount, training_handle, output_count);
    return ORT_OK;
  } else {
    RETURN_TRAINING_ERROR_CODE_IF_ERROR(TrainingSessionGetTrainingModelInputCount, training_handle, input_count);
    RETURN_TRAINING_ERROR_CODE_IF_ERROR(TrainingSessionGetTrainingModelOutputCount, training_handle, output_count);
    return ORT_OK;
  }
}

char* EMSCRIPTEN_KEEPALIVE OrtTrainingGetModelInputOutputName(ort_training_session_handle_t training_handle,
                                                              size_t index,
                                                              bool isInput,
                                                              bool isEvalModel) {
  OrtAllocator* allocator = nullptr;
  RETURN_NULLPTR_IF_ERROR(GetAllocatorWithDefaultOptions, &allocator);

  char* name = nullptr;

  if (isEvalModel) {
    if (isInput) {
      return (CHECK_TRAINING_STATUS(TrainingSessionGetEvalModelInputName, training_handle, index,
                                    allocator, &name) == ORT_OK)
                 ? name
                 : nullptr;
    } else {
      return (CHECK_TRAINING_STATUS(TrainingSessionGetEvalModelOutputName, training_handle, index,
                                    allocator, &name) == ORT_OK)
                 ? name
                 : nullptr;
    }
  } else {
    if (isInput) {
      return (CHECK_TRAINING_STATUS(TrainingSessionGetTrainingModelInputName, training_handle, index,
                                    allocator, &name) == ORT_OK)
                 ? name
                 : nullptr;
    } else {
      return (CHECK_TRAINING_STATUS(TrainingSessionGetTrainingModelOutputName, training_handle, index,
                                    allocator, &name) == ORT_OK)
                 ? name
                 : nullptr;
    }
  }
}

int EMSCRIPTEN_KEEPALIVE OrtTrainingReleaseSession(ort_training_session_handle_t training_handle) {
  Ort::GetTrainingApi().ReleaseTrainingSession(training_handle);
  return ORT_OK;
}

#endif
