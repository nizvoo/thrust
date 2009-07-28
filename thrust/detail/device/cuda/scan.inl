/*
 *  Copyright 2008-2009 NVIDIA Corporation
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 */


/*! \file scan.inl
 *  \brief Inline file for scan.h.
 */

// do not attempt to compile this file with any other compiler
#ifdef __CUDACC__


#include <thrust/experimental/arch.h>
#include <thrust/functional.h>
#include <thrust/device_malloc.h>
#include <thrust/device_free.h>
#include <thrust/copy.h>

#include <thrust/scan.h>    // for second level scans
#include <stdlib.h>         // for malloc & free

#include <thrust/detail/util/blocking.h>

#include <thrust/detail/device/dereference.h>

namespace thrust
{

namespace detail
{

namespace device
{

namespace cuda
{

namespace interval_scan
{

/////////////    
// Kernels //
/////////////    

// XXX replace with thrust::detail::warp::scan() after perf testing
//template<typename T, 
//         typename AssociativeOperator>
//         __device__
//void scan_warp(const unsigned int& thread_lane, volatile T * sdata, const AssociativeOperator binary_op)
//{
//    // the use of 'volatile' is a workaround so that nvcc doesn't reorder the following lines
//    if (thread_lane >=  1)  sdata[threadIdx.x] = binary_op((T &) sdata[threadIdx.x -  1] , (T &) sdata[threadIdx.x]);
//    if (thread_lane >=  2)  sdata[threadIdx.x] = binary_op((T &) sdata[threadIdx.x -  2] , (T &) sdata[threadIdx.x]);
//    if (thread_lane >=  4)  sdata[threadIdx.x] = binary_op((T &) sdata[threadIdx.x -  4] , (T &) sdata[threadIdx.x]);
//    if (thread_lane >=  8)  sdata[threadIdx.x] = binary_op((T &) sdata[threadIdx.x -  8] , (T &) sdata[threadIdx.x]);
//    if (thread_lane >= 16)  sdata[threadIdx.x] = binary_op((T &) sdata[threadIdx.x - 16] , (T &) sdata[threadIdx.x]);
//}

template<typename InputType, 
         typename InputIterator, 
         typename AssociativeOperator>
         __device__
void scan_warp(const unsigned int& thread_lane, InputType& val, InputIterator sdata, const AssociativeOperator binary_op)
{
    sdata[threadIdx.x] = val;

    if (thread_lane >=  1)  sdata[threadIdx.x] = val = binary_op(sdata[threadIdx.x -  1], val);
    if (thread_lane >=  2)  sdata[threadIdx.x] = val = binary_op(sdata[threadIdx.x -  2], val);
    if (thread_lane >=  4)  sdata[threadIdx.x] = val = binary_op(sdata[threadIdx.x -  4], val);
    if (thread_lane >=  8)  sdata[threadIdx.x] = val = binary_op(sdata[threadIdx.x -  8], val);
    if (thread_lane >= 16)  sdata[threadIdx.x] = val = binary_op(sdata[threadIdx.x - 16], val);
}

template<unsigned int BLOCK_SIZE,
         typename OutputIterator,
         typename OutputType,
         typename AssociativeOperator>
__global__ void
inclusive_update_kernel(OutputIterator result,
                        const AssociativeOperator binary_op,
                        const unsigned int n,
                        const unsigned int interval_size,
                        OutputType * carry_in)
{
    const unsigned int thread_id   = BLOCK_SIZE * blockIdx.x + threadIdx.x;        // global thread index
    const unsigned int thread_lane = threadIdx.x & 31;                             // thread index within the warp
    const unsigned int warp_id     = thread_id   / 32;                             // global warp index
    
    const unsigned int interval_begin = warp_id * interval_size;                   // beginning of this warp's segment
    const unsigned int interval_end   = min(interval_begin + interval_size, n);    // end of this warp's segment
    
    if(interval_begin == 0 || interval_begin >= n) return;                         // nothing to do

    OutputType carry = carry_in[warp_id - 1];                                      // value to add to this segment

    for(unsigned int i = interval_begin + thread_lane; i < interval_end; i += 32){
        thrust::detail::device::dereference(result, i) = binary_op(carry, thrust::detail::device::dereference(result, i));
    }
}

template<unsigned int BLOCK_SIZE,
         typename OutputIterator,
         typename OutputType,
         typename AssociativeOperator>
__global__ void
exclusive_update_kernel(OutputIterator result,
                        OutputType init,
                        const AssociativeOperator binary_op,
                        const unsigned int n,
                        const unsigned int interval_size,
                        OutputType * carry_in)
{
    // XXX workaround types with constructors in __shared__ memory
    //__shared__ OutputType sdata[BLOCK_SIZE];
    __shared__ unsigned char sdata_workaround[BLOCK_SIZE * sizeof(OutputType)];
    OutputType *sdata = reinterpret_cast<OutputType*>(sdata_workaround);

    const unsigned int thread_id   = BLOCK_SIZE * blockIdx.x + threadIdx.x;        // global thread index
    const unsigned int thread_lane = threadIdx.x & 31;                             // thread index within the warp
    const unsigned int warp_id     = thread_id   / 32;                             // global warp index
    
    const unsigned int interval_begin = warp_id * interval_size;                   // beginning of this warp's segment
    const unsigned int interval_end   = min(interval_begin + interval_size, n);    // end of this warp's segment
    
    if(interval_begin >= n) return;                                                // nothing to do

    OutputType carry = (warp_id == 0) ? init : binary_op(init, carry_in[warp_id - 1]);  // value to add to this segment
    OutputType val   = carry;

    for(unsigned int i = interval_begin + thread_lane; i < interval_end; i += 32){
        sdata[threadIdx.x] = binary_op(carry, thrust::detail::device::dereference(result, i));

        thrust::detail::device::dereference(result, i) = (thread_lane == 0) ? val : sdata[threadIdx.x - 1]; 

        if(thread_lane == 0)
            val = sdata[threadIdx.x + 31];
    }
}

/* Perform an inclusive scan on separate intervals
 *
 * For intervals of length 2:
 *    [ a, b, c, d, ... ] -> [ a, a+b, c, c+d, ... ]
 *
 * Each warp is assigned an interval of [first, first + n)
 */
template<unsigned int BLOCK_SIZE,
         typename InputIterator,
         typename OutputIterator,
         typename AssociativeOperator,
         typename OutputType>
__global__ void
kernel(InputIterator first,
       const unsigned int n,
       OutputIterator result,
       const AssociativeOperator binary_op,
       const unsigned int interval_size,
       OutputType * final_carry)
{
  // XXX warpSize exists, but is not known at compile time,
  //     so define our own constant
  const unsigned int WARP_SIZE = 32;

  //__shared__ volatile OutputType sdata[BLOCK_SIZE];
  __shared__ unsigned char sdata_workaround[BLOCK_SIZE * sizeof(OutputType)];
  OutputType *sdata = reinterpret_cast<OutputType*>(sdata_workaround);
  
  const unsigned int thread_id   = BLOCK_SIZE * blockIdx.x + threadIdx.x;  // global thread index
  const unsigned int thread_lane = threadIdx.x & (WARP_SIZE - 1);          // thread index within the warp
  const unsigned int warp_id     = thread_id   / WARP_SIZE;                // global warp index

  const unsigned int interval_begin = warp_id * interval_size;                 // beginning of this warp's segment
  const unsigned int interval_end   = min(interval_begin + interval_size, n);  // end of this warp's segment

  unsigned int i = interval_begin + thread_lane;                               // initial thread starting position

  // nothing to do
  if(i >= interval_end)
      return;

//  /// XXX BEGIN TEST
//  if(thread_lane == 0){
//    end = min(base + interval_size, n);
//
//    OutputType sum = thrust::detail::device::dereference(first, i);
//    thrust::detail::device::dereference(result, i) = sum;
//
//    i++;
//    while( i < end ){
//        sum = binary_op(sum, thrust::detail::device::dereference(first, i));
//        thrust::detail::device::dereference(result, i) = sum;
//        i++;
//    }
//    final_carry[warp_id] = sum;
//  }
//  // XXX END TEST


  // First iteration has no carry in
  if(i < interval_end){
      OutputType val = thrust::detail::device::dereference(first, i);

      scan_warp(thread_lane, val, sdata, binary_op);

      thrust::detail::device::dereference(result, i) = val;

      i += WARP_SIZE;
  }

  // Remaining iterations have carry in
  while(i < interval_end){
      OutputType val = thrust::detail::device::dereference(first, i);

      if (thread_lane == 0)
          val = binary_op(sdata[threadIdx.x + (WARP_SIZE - 1)], val);

      scan_warp(thread_lane, val, sdata, binary_op);

      thrust::detail::device::dereference(result, i) = val;

      i += WARP_SIZE;
  }

  if (i == interval_end + (WARP_SIZE - 1))
      final_carry[warp_id] = sdata[threadIdx.x];

} // end kernel()


} // end namespace interval_scan




//////////////////
// Entry Points //
//////////////////

template<typename InputIterator,
         typename OutputIterator,
         typename AssociativeOperator>
  OutputIterator inclusive_scan(InputIterator first,
                                InputIterator last,
                                OutputIterator result,
                                AssociativeOperator binary_op)
{
    typedef typename thrust::iterator_traits<OutputIterator>::value_type OutputType;
  
    const size_t n = last - first;

    if( n == 0 ) 
        return result;

    // XXX todo query for warp size
    const unsigned int WARP_SIZE  = 32;
    const unsigned int BLOCK_SIZE = 256;
    const unsigned int MAX_BLOCKS = experimental::arch::max_active_threads()/BLOCK_SIZE;
    const unsigned int WARPS_PER_BLOCK = BLOCK_SIZE/WARP_SIZE;

    const unsigned int num_units  = thrust::detail::util::divide_into(n, WARP_SIZE);
    const unsigned int num_warps  = std::min(num_units, WARPS_PER_BLOCK * MAX_BLOCKS);
    const unsigned int num_blocks = thrust::detail::util::divide_into(num_warps,WARPS_PER_BLOCK);
    const unsigned int num_iters  = thrust::detail::util::divide_into(num_units, num_warps);          // number of times each warp iterates, interval length is 32*num_iters

    const unsigned int interval_size = WARP_SIZE * num_iters;

    // create a temp vector for per-warp results
    thrust::device_ptr<OutputType> d_carry_out = thrust::device_malloc<OutputType>(num_warps);

    //////////////////////
    // first level scan
    interval_scan::kernel<BLOCK_SIZE> <<<num_blocks, BLOCK_SIZE>>>
        (first, n, result, binary_op, interval_size, d_carry_out.get());

    bool second_scan_device = true;

    ///////////////////////
    // second level scan
    if (second_scan_device) {
        // scan carry_out on the device (use one warp of GPU method for second level scan)
        interval_scan::kernel<WARP_SIZE> <<<1, WARP_SIZE>>>
            (d_carry_out.get(), num_warps, d_carry_out.get(), binary_op, num_warps, (d_carry_out + num_warps - 1).get());
    } else {
        // scan carry_out on the host
        OutputType *h_carry_out = (OutputType*)(::malloc(num_warps * sizeof(OutputType)));
        thrust::copy(d_carry_out, d_carry_out + num_warps, h_carry_out);
        thrust::inclusive_scan(h_carry_out, h_carry_out + num_warps, h_carry_out, binary_op);

        // copy back to device
        thrust::copy(h_carry_out, h_carry_out + num_warps, d_carry_out);
        ::free(h_carry_out);
    }

    //////////////////////
    // update intervals
    interval_scan::inclusive_update_kernel<BLOCK_SIZE> <<<num_blocks, BLOCK_SIZE>>>
        (result, binary_op, n, interval_size, d_carry_out.get());

    // free device work array
    thrust::device_free(d_carry_out);

    return result + n;
} // end inclusive_interval_scan()



template<typename InputIterator,
         typename OutputIterator,
         typename T,
         typename AssociativeOperator>
  OutputIterator exclusive_scan(InputIterator first,
                                InputIterator last,
                                OutputIterator result,
                                T init,
                                AssociativeOperator binary_op)
{
    typedef typename thrust::iterator_traits<OutputIterator>::value_type OutputType;
  
    const size_t n = last - first;

    if( n == 0 )
        return result;

    const unsigned int WARP_SIZE  = 32;
    const unsigned int BLOCK_SIZE = 256;
    const unsigned int MAX_BLOCKS = experimental::arch::max_active_threads()/BLOCK_SIZE;
    const unsigned int WARPS_PER_BLOCK = BLOCK_SIZE/WARP_SIZE;

    const unsigned int num_units  = thrust::detail::util::divide_into(n, WARP_SIZE);
    const unsigned int num_warps  = std::min(num_units, WARPS_PER_BLOCK * MAX_BLOCKS);
    const unsigned int num_blocks = thrust::detail::util::divide_into(num_warps,WARPS_PER_BLOCK);
    const unsigned int num_iters  = thrust::detail::util::divide_into(num_units, num_warps);

    const unsigned int interval_size = WARP_SIZE * num_iters;

    // create a temp vector for per-warp results
    thrust::device_ptr<OutputType> d_carry_out = thrust::device_malloc<OutputType>(num_warps);

    //////////////////////
    // first level scan
    interval_scan::kernel<BLOCK_SIZE> <<<num_blocks, BLOCK_SIZE>>>
        (first, n, result, binary_op, interval_size, d_carry_out.get());

    bool second_scan_device = true;

    ///////////////////////
    // second level scan
    if (second_scan_device) {
        // scan carry_out on the device (use one warp of GPU method for second level scan)
        interval_scan::kernel<WARP_SIZE> <<<1, WARP_SIZE>>>
            (d_carry_out.get(), num_warps, d_carry_out.get(), binary_op, num_warps, (d_carry_out + num_warps - 1).get());
    } 
    else {
        // scan carry_out on the host
        OutputType *h_carry_out = (OutputType*)(::malloc(num_warps * sizeof(OutputType)));
        thrust::copy(d_carry_out, d_carry_out + num_warps, h_carry_out);
        thrust::inclusive_scan(h_carry_out, h_carry_out + num_warps, h_carry_out, binary_op);

        // copy back to device
        thrust::copy(h_carry_out, h_carry_out + num_warps, d_carry_out);
        ::free(h_carry_out);
    }

    //////////////////////
    // update intervals
    interval_scan::exclusive_update_kernel<BLOCK_SIZE> <<<num_blocks, BLOCK_SIZE>>>
        (result, OutputType(init), binary_op, n, interval_size, d_carry_out.get());

    // free device work array
    thrust::device_free(d_carry_out);

    return result + n;
} // end exclusive_interval_scan()


} // end namespace cuda

} // end namespace device

} // end namespace detail

} // end namespace thrust

#endif // __CUDACC__

