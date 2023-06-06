#include <stdio.h>
#include "types.h"
#include "cross.h"

#ifndef CROSS_CPU

  __shared__ real_t  sumtab[MAX_THREADS];
  __shared__ real_t* sumptr[MAX_THREADS];

  #if __CUDA_ARCH__ < 200
    #define CROSS_NEED_ATOMICADD_DOUBLE
  #endif

  #ifndef CROSS_HIP
    #define CROSS_NEED_ATOMICMAX_FLOAT
    #define CROSS_NEED_ATOMICMAX_DOUBLE
  #endif

  template <class R_t> struct R2I_caster {};
  template <> struct R2I_caster< float > {
    typedef int   I_t;
    typedef float R_t;
    __device__ static inline int toI(const float& x) { return __float_as_int(x); }
    __device__ static inline float toR(const int& x) { return __int_as_float(x); }
  };
  template <> struct R2I_caster< double > {
    typedef unsigned long long int I_t;
    typedef double                 R_t;
    __device__ static inline int toI(const double& x) { return __double_as_longlong(x); }
    __device__ static inline double toR(const int& x) { return __longlong_as_double(x); }
  };
  struct CrossOPAdd {
    template <class T> __device__ static inline T op(const T& x, const T& y) { return x + y; }
  };
  struct CrossOPMax {
    template <class T> __device__ static inline T op(const T& x, const T& y) { return x > y ? x : y; }
  };


  template <class R_t, class OP>
  __device__ inline void CudaAtomicOP(const R_t* address, const R_t& val)
  {
      typedef R2I_caster<R_t> R2I;
      typedef typename R2I::I_t I_t;
      I_t* address_as_ull = (I_t*) address;
      I_t  old = *address_as_ull;
      I_t  assumed, nw;
      do {
          assumed = old;
          nw = R2I::toI(OP::op(val, R2I::toR(assumed)));
          old = atomicCAS(address_as_ull, assumed, nw);
      } while (assumed != old);
  }


  #ifdef CROSS_NEED_ATOMICADD_DOUBLE
    __device__ inline void atomicAdd(const double* address, const double& val) {
      return CudaAtomicOP<double, CrossOPAdd>(address, val);
    }
  #endif
  #ifdef CROSS_NEED_ATOMICMAX_DOUBLE
    __device__ inline void atomicMax(const double* address, const double& val) {
      return CudaAtomicOP<double, CrossOPMax>(address, val);
    }
  #endif
  #ifdef CROSS_NEED_ATOMICMAX_FLOAT
    __device__ inline void atomicMax(const float* address, const float& val) {
      return CudaAtomicOP<float, CrossOPMax>(address, val);
    }
  #endif
      
  #define CudaAtomicAdd atomicAdd
  #define CudaAtomicMax atomicMax

   #ifndef MAX_THREADS
      #error FUCK!
    #else
      #if MAX_THREADS < 500
        #error Double Fuck!
      #endif
    #endif

      __device__ inline void CudaAtomicAddReduce(real_t * sum, real_t val)
      {
              __syncthreads();
              int i = blockDim.x*blockDim.y;
              int k = blockDim.x*blockDim.y;
              int j = blockDim.x*threadIdx.y + threadIdx.x;
              sumtab[j] = val;
              __syncthreads();
              while (i> 1) {
                      k = i >> 1;
                      i = i - k;
                      if (j<k) sumtab[j] += sumtab[j+i];
                      __syncthreads();
              }
              if (j==0) {
                real_t val = sumtab[0];
                if (val != 0.0) {
                  CudaAtomicAdd(sum, val);
                }
              }
      }

#if CUDART_VERSION >= 9000
__device__ inline void CudaAtomicAddReduceWarp(real_t * sum, real_t val)
{
	#define FULL_MASK 0xffffffff
	if (__any_sync(FULL_MASK, val != 0)) {
		for (int offset = 16; offset > 0; offset /= 2)
		    val += __shfl_down_sync(FULL_MASK, val, offset);
		if (threadIdx.x == 0) CudaAtomicAdd(sum,val);
	}
}

/*
__device__ inline void CudaAtomicAddReduceWarpArr(real_t * sum, real_t * val, unsigned char len)
{
	#define FULL_MASK 0xffffffff
	bool pred = false;
	for (unsigned char i=0; i<len; i++) pred = pred || (val[i] != 0.0);
	if (__any_sync(FULL_MASK, pred)) {
		for (int offset = 16; offset > 0; offset /= 2) {
			for (unsigned char i=0; i<len; i++) val[i] += __shfl_xor_sync(FULL_MASK, val[i], offset);
		}
		if (threadIdx.x < len) {
			CudaAtomicAdd(sum+threadIdx.x,val[threadIdx.x]);
		}
	}
}*/

#include <cooperative_groups.h>
#include <cooperative_groups/reduce.h>
namespace cg = cooperative_groups;

__device__ inline void CudaAtomicAddReduceWarpArr(real_t * sum, real_t * val, unsigned char len)
{
        
	cg::coalesced_group active = cg::coalesced_threads();
	cg::coalesced_group com = cg::labeled_partition(active, (unsigned long long)(void*)sum);
	for (unsigned char i=0; i<len; i++) val[i] = cg::reduce(com, val[i], cg::plus<real_t>());
	int j = com.thread_rank();
	if (j < len) CudaAtomicAdd(sum+j,val[j]);
}

#elif CUDART_VERSION >= 7000 || defined(CROSS_HIP)

__device__ inline void CudaAtomicAddReduceWarp(real_t * sum, real_t val)
{
	#define FULL_MASK 0xffffffff
	if (__any(val != 0)) {
		for (int offset = 16; offset > 0; offset /= 2)
		    val += __shfl_down(val, offset);
		if (threadIdx.x == 0) CudaAtomicAdd(sum,val);
	}
}

__device__ inline void CudaAtomicAddReduceWarpArr(real_t * sum, real_t * val, unsigned char len)
{
	#define FULL_MASK 0xffffffff
	bool pred = false;
	for (unsigned char i=0; i<len; i++) pred = pred || (val[i] != 0.0);
	if (__any(pred)) {
		for (int offset = 16; offset > 0; offset /= 2) {
			for (unsigned char i=0; i<len; i++) val[i] += __shfl_xor(val[i], offset);
		}
		if (threadIdx.x < len) {
			CudaAtomicAdd(sum+threadIdx.x,val[threadIdx.x]);
		}
	}
}
#else
  #warning "no CudaAtomicAddReduceWarp for this CUDA version"
#endif

      __device__ inline void CudaAtomicMaxReduce(real_t * sum, real_t val)
      {
              int i = blockDim.x*blockDim.y;
              int k = blockDim.x*blockDim.y;
              int j = blockDim.x*threadIdx.y + threadIdx.x;
              __syncthreads();
              sumtab[j] = val;
              __syncthreads();
              while (i> 1) {
                      k = i >> 1;
                      i = i - k;
                      if (j<k) sumtab[j] = max(sumtab[j],sumtab[j+i]);
                      __syncthreads();
              }
              if (j==0) CudaAtomicMax(sum,sumtab[0]);
      }

      __device__ inline void CudaAtomicAddReduceDiff(real_t * sum, real_t val, bool yes)
      {
                if (! yes) val = 0.0;
                CudaAtomicAddReduce(sum, val);
      }

#endif
