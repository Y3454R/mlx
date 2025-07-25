// Copyright © 2024 Apple Inc.
#include "mlx/backend/common/binary.h"
#include "mlx/backend/metal/device.h"
#include "mlx/backend/metal/kernels.h"
#include "mlx/backend/metal/utils.h"
#include "mlx/primitives.h"

#define BINARY_GPU(func)                                              \
  void func::eval_gpu(const std::vector<array>& inputs, array& out) { \
    binary_op_gpu(inputs, out, name());                               \
  }

#define BINARY_GPU_MULTI(func)                                         \
  void func::eval_gpu(                                                 \
      const std::vector<array>& inputs, std::vector<array>& outputs) { \
    binary_op_gpu(inputs, outputs, name());                            \
  }

namespace mlx::core {

std::string get_kernel_name(
    BinaryOpType bopt,
    const char* op,
    const array& a,
    bool large,
    int ndim,
    int work_per_thread) {
  std::string kname;
  switch (bopt) {
    case BinaryOpType::ScalarScalar:
      kname = "ss";
      break;
    case BinaryOpType::ScalarVector:
      kname = "sv";
      break;
    case BinaryOpType::VectorScalar:
      kname = "vs";
      break;
    case BinaryOpType::VectorVector:
      kname = "vv";
      break;
    case BinaryOpType::General:
      kname = "g";
      if (ndim <= 3) {
        kname += std::to_string(ndim);
      } else {
        concatenate(kname, "n", std::to_string(work_per_thread));
      }
      if (large) {
        kname += "large";
      }
      break;
  }
  if (bopt != BinaryOpType::General && bopt != BinaryOpType::ScalarScalar) {
    if (large) {
      kname += "2";
    } else if (work_per_thread > 1) {
      kname += "n";
    }
  }
  concatenate(kname, "_", op, type_to_name(a));
  return kname;
}

void binary_op_gpu_inplace(
    const std::vector<array>& inputs,
    std::vector<array>& outputs,
    const char* op,
    const Stream& s) {
  auto& a = inputs[0];
  auto& b = inputs[1];
  auto bopt = get_binary_op_type(a, b);

  auto& out = outputs[0];
  if (out.size() == 0) {
    return;
  }

  // Try to collapse contiguous dims
  auto maybe_collapse = [bopt, &a, &b, &out]() {
    if (bopt == BinaryOpType::General) {
      auto [shape, strides] = collapse_contiguous_dims(a, b, out);
      return std::make_tuple(shape, strides[0], strides[1], strides[2]);
    } else {
      decltype(a.strides()) e{};
      return std::make_tuple(decltype(a.shape()){}, e, e, e);
    }
  };
  auto [shape, strides_a, strides_b, strides_out] = maybe_collapse();

  bool large;
  auto ndim = shape.size();
  int work_per_thread;
  if (bopt == BinaryOpType::General) {
    large = a.data_size() > INT32_MAX || b.data_size() > INT32_MAX ||
        out.size() > INT32_MAX;
    work_per_thread = large ? 4 : 2;
  } else {
    large = out.data_size() > UINT32_MAX;
    work_per_thread = get_work_per_thread(a.dtype(), out.data_size());
  }
  std::string kernel_name =
      get_kernel_name(bopt, op, a, large, shape.size(), work_per_thread);
  auto& d = metal::device(s.device);

  auto kernel = outputs.size() == 2
      ? get_binary_two_kernel(d, kernel_name, a.dtype(), out.dtype(), op)
      : get_binary_kernel(d, kernel_name, a.dtype(), out.dtype(), op);
  auto& compute_encoder = d.get_command_encoder(s.index);
  compute_encoder.set_compute_pipeline_state(kernel);

  int arg_idx = 0;
  compute_encoder.set_input_array(a, arg_idx++);
  compute_encoder.set_input_array(b, arg_idx++);
  compute_encoder.set_output_array(outputs[0], arg_idx++);
  if (outputs.size() == 2) {
    compute_encoder.set_output_array(outputs[1], arg_idx++);
  }

  auto thread_group_size = kernel->maxTotalThreadsPerThreadgroup();
  if (bopt == BinaryOpType::General) {
    // Launch up to 3D grid of threads
    size_t dim0 = ndim > 0 ? shape[ndim - 1] : 1;
    size_t dim1 = ndim > 1 ? shape[ndim - 2] : 1;
    size_t rest = out.size() / (dim0 * dim1);

    if (ndim > 3) {
      compute_encoder.set_vector_bytes(shape, arg_idx++);
      compute_encoder.set_vector_bytes(strides_a, arg_idx++);
      compute_encoder.set_vector_bytes(strides_b, arg_idx++);
      compute_encoder.set_bytes<int>(ndim, arg_idx++);
      dim0 = (dim0 + work_per_thread - 1) / work_per_thread;
    } else {
      // The shape is implicit in the grid for <= 3D
      compute_encoder.set_vector_bytes(strides_a, arg_idx++);
      compute_encoder.set_vector_bytes(strides_b, arg_idx++);
    }

    if (thread_group_size != 1024) {
      throw std::runtime_error("[Metal::binary] Must use 1024 sized block");
    }
    auto group_dims = get_block_dims(dim0, dim1, rest);
    MTL::Size grid_dims = MTL::Size(dim0, dim1, rest);
    compute_encoder.dispatch_threads(grid_dims, group_dims);
  } else {
    // Launch a 1D or 2D grid of threads
    size_t nthreads = ceildiv(out.data_size(), work_per_thread);
    if (thread_group_size > nthreads) {
      thread_group_size = nthreads;
    }

    MTL::Size group_dims = MTL::Size(thread_group_size, 1, 1);
    MTL::Size grid_dims;
    if (large) {
      compute_encoder.set_bytes<int64_t>(out.data_size(), arg_idx++);
      grid_dims = get_2d_grid_dims(out.shape(), out.strides(), work_per_thread);
    } else {
      compute_encoder.set_bytes<int>(out.data_size(), arg_idx++);
      grid_dims = MTL::Size(nthreads, 1, 1);
    }
    compute_encoder.dispatch_threads(grid_dims, group_dims);
  }
}

void binary_op_gpu(
    const std::vector<array>& inputs,
    std::vector<array>& outputs,
    const char* op,
    const Stream& s) {
  assert(inputs.size() == 2);
  auto& a = inputs[0];
  auto& b = inputs[1];
  auto bopt = get_binary_op_type(a, b);
  set_binary_op_output_data(a, b, outputs[0], bopt);
  set_binary_op_output_data(a, b, outputs[1], bopt);
  binary_op_gpu_inplace(inputs, outputs, op, s);
}

void binary_op_gpu(
    const std::vector<array>& inputs,
    std::vector<array>& outputs,
    const char* op) {
  auto& s = outputs[0].primitive().stream();
  binary_op_gpu(inputs, outputs, op, s);
}

void binary_op_gpu_inplace(
    const std::vector<array>& inputs,
    array& out,
    const char* op,
    const Stream& s) {
  std::vector<array> outputs = {out};
  binary_op_gpu_inplace(inputs, outputs, op, s);
}

void binary_op_gpu(
    const std::vector<array>& inputs,
    array& out,
    const char* op,
    const Stream& s) {
  assert(inputs.size() == 2);
  auto& a = inputs[0];
  auto& b = inputs[1];
  auto bopt = get_binary_op_type(a, b);
  set_binary_op_output_data(a, b, out, bopt);
  binary_op_gpu_inplace(inputs, out, op, s);
}

void binary_op_gpu(
    const std::vector<array>& inputs,
    array& out,
    const char* op) {
  auto& s = out.primitive().stream();
  binary_op_gpu(inputs, out, op, s);
}

BINARY_GPU(Add)
BINARY_GPU(ArcTan2)
BINARY_GPU(Divide)
BINARY_GPU_MULTI(DivMod)
BINARY_GPU(Remainder)
BINARY_GPU(Equal)
BINARY_GPU(Greater)
BINARY_GPU(GreaterEqual)
BINARY_GPU(Less)
BINARY_GPU(LessEqual)
BINARY_GPU(LogicalAnd)
BINARY_GPU(LogicalOr)
BINARY_GPU(LogAddExp)
BINARY_GPU(Maximum)
BINARY_GPU(Minimum)
BINARY_GPU(Multiply)
BINARY_GPU(NotEqual)
BINARY_GPU(Power)
BINARY_GPU(Subtract)

void BitwiseBinary::eval_gpu(const std::vector<array>& inputs, array& out) {
  switch (op_) {
    case BitwiseBinary::And:
      binary_op_gpu(inputs, out, name());
      break;
    case BitwiseBinary::Or:
      binary_op_gpu(inputs, out, name());
      break;
    case BitwiseBinary::Xor:
      binary_op_gpu(inputs, out, name());
      break;
    case BitwiseBinary::LeftShift:
      binary_op_gpu(inputs, out, name());
      break;
    case BitwiseBinary::RightShift:
      binary_op_gpu(inputs, out, name());
      break;
  }
}

} // namespace mlx::core
