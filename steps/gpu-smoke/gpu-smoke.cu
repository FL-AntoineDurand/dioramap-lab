// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// CUDA identity kernel for the gpu-smoke canary. Built only when
// -DDIORAMAP_CUDA=ON. Copies `n` bytes from `in` to `out` via a device buffer.

#include <cuda_runtime.h>
#include <stddef.h>
#include <stdint.h>

__global__ static void identity_kernel(const uint8_t* in, uint8_t* out, size_t n) {
    size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x;
    if (i < n) {
        out[i] = in[i];
    }
}

extern "C" int gpu_smoke_cuda_identity(const void* in, size_t n, void* out) {
    if (n == 0) {
        return 0;
    }
    void* d_in = nullptr;
    void* d_out = nullptr;
    if (cudaMalloc(&d_in, n) != cudaSuccess) {
        return -1;
    }
    if (cudaMalloc(&d_out, n) != cudaSuccess) {
        cudaFree(d_in);
        return -1;
    }
    if (cudaMemcpy(d_in, in, n, cudaMemcpyHostToDevice) != cudaSuccess) {
        cudaFree(d_in);
        cudaFree(d_out);
        return -1;
    }
    dim3 block(256);
    dim3 grid((unsigned int)((n + 255) / 256));
    identity_kernel<<<grid, block>>>((const uint8_t*)d_in, (uint8_t*)d_out, n);
    if (cudaDeviceSynchronize() != cudaSuccess) {
        cudaFree(d_in);
        cudaFree(d_out);
        return -1;
    }
    if (cudaMemcpy(out, d_out, n, cudaMemcpyDeviceToHost) != cudaSuccess) {
        cudaFree(d_in);
        cudaFree(d_out);
        return -1;
    }
    cudaFree(d_in);
    cudaFree(d_out);
    return 0;
}
