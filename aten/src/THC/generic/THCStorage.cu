#ifndef THC_GENERIC_FILE
#define THC_GENERIC_FILE "THC/generic/THCStorage.cu"
#else

void THCStorage_(fill)(THCState *state, THCStorage *self, scalar_t value)
{
  at::cuda::ThrustAllocator thrustAlloc;
  thrust::device_ptr<scalar_t> self_data(THCStorage_(data)(state, self));
  thrust::fill(
#if (defined(CUDA_VERSION) && CUDA_VERSION >= 7000) || defined(USE_ROCM)
      thrust::cuda::par(thrustAlloc).on(c10::cuda::getCurrentCUDAStream()),
#endif
      self_data,
      self_data + (self->nbytes() / sizeof(scalar_t)),
      value);
}

void THCStorage_(
    resizeBytes)(THCState* state, THCStorage* self, ptrdiff_t size_bytes) {
  THCStorage_resizeBytes(state, self, size_bytes);
}

int THCStorage_(getDevice)(THCState* state, const THCStorage* storage) {
  return THCStorage_getDevice(state, storage);
}

#endif
