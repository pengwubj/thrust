/*
 *  Copyright 2008-2012 NVIDIA Corporation
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


/*! \file stable_merge_sort_dev.inl
 *  \brief Inline file for stable_merge_sort_dev.h.
 *  \note This algorithm is based on the one described
 *        in "Designing Efficient Sorting Algorithms for
 *        Manycore GPUs", by Satish, Harris, and Garland.
 *        Nadatur Satish is the original author of this
 *        implementation.
 */

#include <thrust/detail/config.h>

#include <thrust/functional.h>
#include <thrust/detail/copy.h>
#include <thrust/swap.h>

#include <thrust/device_ptr.h>
#include <thrust/detail/function.h>

#include <thrust/detail/mpl/math.h> // for log2<N>
#include <thrust/iterator/iterator_traits.h>

#include <thrust/system/cuda/detail/block/merging_sort.h>
#include <thrust/system/cuda/detail/runtime_introspection.h>
#include <thrust/detail/temporary_array.h>
#include <thrust/system/cuda/detail/detail/launch_closure.h>
#include <thrust/system/cuda/detail/detail/uninitialized.h>
#include <thrust/system/cuda/detail/block/merge.h>
#include <thrust/system/cuda/detail/block/copy.h>
#include <thrust/pair.h>
#include <thrust/tuple.h>
#include <thrust/iterator/counting_iterator.h>
#include <thrust/iterator/transform_iterator.h>
#include <thrust/gather.h>

__THRUST_DISABLE_MSVC_POSSIBLE_LOSS_OF_DATA_WARNING_BEGIN


namespace thrust
{
namespace detail
{

template<typename T>
  void destroy(T &x)
{
  x.~T();
} // end destroy()

} // end detail

namespace system
{
namespace cuda
{
namespace detail
{
namespace detail
{

namespace merge_sort_dev_namespace
{

// define our own min() rather than #include <thrust/extrema.h>
template<typename T>
  __host__ __device__
  T min THRUST_PREVENT_MACRO_SUBSTITUTION (const T &lhs, const T &rhs)
{
  return lhs < rhs ? lhs : rhs;
} // end min()


template<typename Key, typename Value>
  class block_size
{
  public:
    static const unsigned int result = 256;
};

template<typename Key, typename Value>
  class log_block_size
{
  private:
    static const unsigned int bs = block_size<Key,Value>::result;

  public:
    static const unsigned int result = thrust::detail::mpl::math::log2<bs>::value;
};

static const unsigned int warp_size = 32;

template <typename Size>
inline unsigned int max_grid_size(Size block_size)
{
  const device_properties_t& properties = device_properties();

  const unsigned int max_threads = properties.maxThreadsPerMultiProcessor * properties.multiProcessorCount;
  const unsigned int max_blocks  = properties.maxGridSize[0];
  
  return std::min<unsigned int>(max_blocks, 3 * max_threads / block_size);
} // end max_grid_size()

// Base case for the merge algorithm: merges data where tile_size <= block_size. 
// Works by loading two or more tiles into shared memory and doing a binary search.
template<unsigned int block_size,
         typename RandomAccessIterator1,
         typename RandomAccessIterator2,
         typename RandomAccessIterator3,
         typename RandomAccessIterator4,
         typename StrictWeakOrdering,
         typename Context>
struct merge_small_tiles_closure
{
  typedef Context context_type;

  RandomAccessIterator1 keys_first;
  RandomAccessIterator2 values_first;
  const unsigned int n;
  const unsigned int log_tile_size;
  RandomAccessIterator3 keys_result;
  RandomAccessIterator4 values_result;
  StrictWeakOrdering comp;
  context_type context;

  // these members are derivable from block_size, n, and log_tile_size
  unsigned int index_of_last_block;
  unsigned int index_of_last_tile_in_last_block;
  unsigned int size_of_last_tile;

  merge_small_tiles_closure
    (RandomAccessIterator1 keys_first,
     RandomAccessIterator2 values_first,
     const unsigned int n,
     const unsigned int log_tile_size,
     RandomAccessIterator3 keys_result,
     RandomAccessIterator4 values_result,
     StrictWeakOrdering comp,
     Context context = Context())
    : keys_first(keys_first), values_first(values_first),
      n(n), 
      log_tile_size(log_tile_size),
      keys_result(keys_result), values_result(values_result),
      comp(comp),
      context(context)
  {
    // compute the number of tiles, including a possible partial tile
    unsigned int tile_size = 1 << log_tile_size;
    unsigned int num_tiles = n / tile_size;
    unsigned int partial_tile_size = n % tile_size;
    if(partial_tile_size) ++num_tiles;

    // compute the number of logical thread blocks, including a possible partial block
    unsigned int tiles_per_block = block_size / tile_size;
    unsigned int partial_block_size = num_tiles % tiles_per_block;
    unsigned int num_blocks = num_tiles / tiles_per_block;
    if(partial_block_size) ++num_blocks;

    // compute the number of tiles in the last block, which might be of partial size
    unsigned int number_of_tiles_in_last_block = partial_block_size ? partial_block_size : tiles_per_block;

    size_of_last_tile = partial_tile_size ? partial_tile_size : tile_size;
    index_of_last_tile_in_last_block = number_of_tiles_in_last_block - 1;
    index_of_last_block = num_blocks - 1;
  }

  unsigned int grid_size() const
  {
    const unsigned int max_num_blocks = max_grid_size(block_size);
    const unsigned int num_logical_blocks = index_of_last_block + 1;
    return min(num_logical_blocks, max_num_blocks);
  }

  __device__ __thrust_forceinline__
  bool task_not_idle(const unsigned int task_idx) const
  {
    return task_idx < n;
  }

  __device__ __thrust_forceinline__
  void operator()(void)
  {
    typedef typename iterator_value<RandomAccessIterator3>::type KeyType;
    typedef typename iterator_value<RandomAccessIterator4>::type ValueType;

    // load (2*block_size) elements into shared memory. These (2*block_size) elements belong to (2*block_size)/tile_size different tiles.
    __shared__ uninitialized_array<KeyType, 2 * block_size>   key;
    __shared__ uninitialized_array<KeyType, 2 * block_size>   outkey;
    __shared__ uninitialized_array<ValueType, 2 * block_size> outvalue;

    const unsigned int grid_size = context.grid_dimension() * context.block_dimension();

    unsigned int block_idx = context.block_index();
    
    // the global index of this task
    unsigned int i = context.thread_index() + context.block_index() * context.block_dimension();

    // advance iterators
    keys_first    += i;
    values_first  += i;
    keys_result   += i;
    values_result += i;

    for(;
        block_idx <= index_of_last_block;
        block_idx += context.grid_dimension(), i += grid_size, keys_first += grid_size, values_first += grid_size, keys_result += grid_size, values_result += grid_size)
    {
      KeyType my_key;
      
      // copy over inputs to shared memory
      if(i < n)
      {
        key[context.thread_index()] = my_key = *keys_first;
      } // end if
      
      // the tile to which the element belongs
      unsigned int tile_index = context.thread_index()>>log_tile_size;

      // figure out the index and size of the other tile
      unsigned int other_tile_index = tile_index^1;
      unsigned int other_tile_size = (1<<log_tile_size);

      // if the other tile is the final tile, it is potentially
      // smaller than the rest
      if(block_idx == index_of_last_block
         && other_tile_index == index_of_last_tile_in_last_block)
      {
        other_tile_size = size_of_last_tile;
      } // end if
      
      // figure out where the other tile begins in shared memory
      KeyType *other = key.data() + (other_tile_index<<log_tile_size);

      context.barrier();
      if(i < n)
      {
        // to compute the rank of my element in the merged sequence
        // add the rank of the element in the other tile
        // plus the rank of the element in this tile
        // the computation for the rank of the element in this tile 
        // differs depending on if we're in the odd or even tile
        unsigned int rank;
        if(tile_index & 1)
        {
          rank = thrust::system::detail::generic::scalar::upper_bound_n(other, other_tile_size, my_key, comp) - other;
          rank += context.thread_index() - (1<<log_tile_size);
        }
        else
        {
          rank = thrust::system::detail::generic::scalar::lower_bound_n(other, other_tile_size, my_key, comp) - other;
          rank += context.thread_index();
        }

        // store my key and value to the output arrays in smem
        outkey[rank] = my_key;
        outvalue[rank] = *values_first;
      } // end if
      context.barrier();
      
      if(i < n)
      {
        // coalesced writes to global memory
        *keys_result   = outkey[context.thread_index()];
        *values_result = outvalue[context.thread_index()];
      } // end if
      context.barrier();
    } // end for
  }
}; // merge_smalltiles_binarysearch_closure

template<unsigned int block_size,
         typename RandomAccessIterator1,
         typename RandomAccessIterator2,
         typename StrictWeakOrdering,
         typename Context>
struct stable_odd_even_block_sort_closure
{
  RandomAccessIterator1 keys_first;
  RandomAccessIterator2 values_first;
  StrictWeakOrdering comp;
  const unsigned int n;
  Context context;

  typedef Context context_type;

  stable_odd_even_block_sort_closure
    (RandomAccessIterator1 keys_first,
     RandomAccessIterator2 values_first,
     StrictWeakOrdering comp,
     const unsigned int n,
     Context context = Context())
    : keys_first(keys_first), values_first(values_first), comp(comp), n(n), context(context)
  {}

  __device__ __thrust_forceinline__
  void operator()(void)
  {
    typedef typename iterator_value<RandomAccessIterator1>::type KeyType;
    typedef typename iterator_value<RandomAccessIterator2>::type ValueType;
  
    __shared__ uninitialized_array<KeyType,block_size>   s_keys;
    __shared__ uninitialized_array<ValueType,block_size> s_data;
  
    const unsigned int grid_size = context.grid_dimension() * context.block_dimension();
  
    // block_offset records the global index of this block's 0th thread
    unsigned int block_offset = context.block_index() * block_size;
    unsigned int i = context.thread_index() + block_offset;
  
    // advance iterators
    keys_first   += i;
    values_first += i;
  
    for(;
        block_offset < n;
        block_offset += grid_size, i += grid_size, keys_first += grid_size, values_first += grid_size)
    {
      context.barrier();
      // copy input to shared
      if(i < n)
      {
        s_keys[context.thread_index()] = *keys_first;
        s_data[context.thread_index()] = *values_first;
      } // end if
      context.barrier();
  
      // this block could be partially full
      unsigned int length = block_size;
      if(block_offset + block_size > n)
      {
        length = n - block_offset;
      } // end if
  
      // run merge_sort over the block
      block::merging_sort(context, s_keys.begin(), s_data.begin(), length, comp);
  
      // write result
      if(i < n)
      {
        *keys_first   = s_keys[context.thread_index()];
        *values_first = s_data[context.thread_index()];
      } // end if
    } // end for i
  }
}; // stable_odd_even_block_sort_closure


template<unsigned int stride>
  class static_strided_integer_range
{
  // XXX cudafe doesn't like this private for some reason
  //private:
  public:
    typedef typename thrust::counting_iterator<unsigned int> counting_iterator;

    struct stride_functor
      : public thrust::unary_function<unsigned int,unsigned int>
    {
      inline __host__ __device__
      unsigned int operator()(unsigned int i) const
      {
        return stride * i;
      }
    };

  public:
    typedef typename thrust::transform_iterator<stride_functor, counting_iterator> iterator;

    static_strided_integer_range(unsigned int num_strides)
      : m_begin(iterator(counting_iterator(0), stride_functor())),
        m_end(iterator(counting_iterator(num_strides), stride_functor()))
    {}

    iterator begin() const
    {
      return m_begin;
    }

    iterator end() const
    {
      return m_end;
    }

  private:
    iterator m_begin, m_end;
};


///////////////////// Find the rank of each extracted element in both arrays ////////////////////////////////////////
///////////////////// This breaks up the array into independent segments to merge ////////////////////////////////////////
// Inputs: d_splitters, d_splittes_pos: the merged array of splitters with corresponding positions.
//		   d_srcData: input data, datasize: number of entries in d_srcData
//		   N_SPLITTERS the number of splitters, log_blocksize: log of the size of each block of sorted data
//		   log_num_merged_splitters_per_block = log of the number of merged splitters. ( = log_blocksize - 7). 
// Output: d_rank1, d_rank2: ranks of each splitter in d_splitters in the block to which it belongs
//		   (say i) and its corresponding block (block i+1).
template<unsigned int block_size,
         typename RandomAccessIterator1,
         typename RandomAccessIterator2,
         typename RandomAccessIterator3,
         typename RandomAccessIterator4,
         typename StrictWeakOrdering,
         typename Context>
struct rank_splitters_closure
{
  typedef Context context_type;

  static const unsigned int log_block_size = thrust::detail::mpl::math::log2<block_size>::value;

  RandomAccessIterator1 splitters_first;
  RandomAccessIterator2 splitters_pos_first;
  RandomAccessIterator3 keys_first;
  RandomAccessIterator4 ranks_result1;
  RandomAccessIterator4 ranks_result2;
  unsigned int num_splitters;
  unsigned int num_keys;
  unsigned int log_tile_size;
  thrust::detail::device_function<
    StrictWeakOrdering,
    bool
  > comp;
  context_type context;

  // this member is derivable from those received in the constructor
  unsigned int log_num_merged_splitters_per_tile;

  rank_splitters_closure(RandomAccessIterator1 splitters_first,
                         RandomAccessIterator2 splitters_pos_first, 
                         RandomAccessIterator3 keys_first,
                         unsigned int num_splitters,
                         unsigned int num_keys, 
                         unsigned int log_tile_size, 
                         RandomAccessIterator4 ranks_result1,
                         RandomAccessIterator4 ranks_result2, 
                         StrictWeakOrdering comp,
                         context_type context = context_type())
    : splitters_first(splitters_first), splitters_pos_first(splitters_pos_first),
      keys_first(keys_first),
      ranks_result1(ranks_result1), ranks_result2(ranks_result2),
      num_splitters(num_splitters), num_keys(num_keys),
      log_tile_size(log_tile_size),
      comp(comp), context(context)
  {
    // the number of splitters in each tile before merging
    const unsigned int log_num_splitters_per_tile = log_tile_size - log_block_size;

    // the number of splitters in each merged tile
    log_num_merged_splitters_per_tile = log_num_splitters_per_tile + 1;
  }

  inline unsigned int grid_size() const
  {
    unsigned int num_blocks = num_splitters / block_size;
    if(num_splitters % block_size) ++num_blocks;

    // compute the maximum number of block_size we can launch on this arch
    const unsigned int max_num_blocks = max_grid_size(block_size);

    return min<unsigned int>(num_blocks, max_num_blocks);
  }

  /*! this member function returns the index of the (odd,even) block pair
   *  that the splitter of interest belongs to
   *  \param splitter_idx The index of the splitter in the splitters list
   *  \return The global index of the (odd,even) block pair
   */
  __device__ __thrust_forceinline__
  unsigned int block_pair_idx(unsigned int splitter_idx) const
  {
    return splitter_idx >> log_num_merged_splitters_per_tile;
  }

  /*! This member function returns the end of the search range in the other tile in
   *  which the splitter of interest needs to be ranked.
   *  \param splitter_idx The index of the splitter in the splitters array
   *  \param splitter_global_idx The index of the splitter in the global array of elements
   *  \param tile_idx The index of the tile to which the splitter belongs.
   *  \return The half-open interval in the other tile in which the splitter needs to be ranked.
   *          [first_index_to_search, size_of_interval)
   */
  __device__ __thrust_forceinline__
  thrust::pair<unsigned int,unsigned int> search_interval(unsigned int splitter_idx, unsigned int splitter_global_idx, unsigned int tile_idx) const
  {
    // We want to compute the ranks of the splitter in d_srcData1 and d_srcData2
    // for instance, if the splitter belongs to d_srcData1, then 
    // (1) the rank in d_srcData1 is simply given by its splitter_global_idx
    // (2) to find the rank in d_srcData2, we first find the block in d_srcData2 where inp appears.
    //     We do this by noting that we have already merged/sorted splitters, and thus the rank
    //     of inp in the elements of d_srcData2 that are present in splitters is given by 
    //        position of inp in d_splitters - rank of inp in elements of d_srcData1 in splitters
    //        = i - splitter_global_idx
    //     This also gives us the block of d_srcData2 that the splitter belongs in, since we have one
    //     element in splitters per block of d_srcData2.
    
    //     We now perform a binary search over this block of d_srcData2 to find the rank of inp in d_srcData2.
    //     start and end are the start and end indices of this block in d_srcData2, forming the bounds of the binary search.
    //     Note that this binary search is in global memory with uncoalesced loads. However, we only find the ranks 
    //     of a small set of elements, one per splitter: thus it is not the performance bottleneck.
    
    // the local index of the splitter within the (odd, even) block pair.
    const unsigned int splitter_block_pair_idx = splitter_idx - (block_pair_idx(splitter_idx)<<log_num_merged_splitters_per_tile);

    // the index of the splitter within its tile
    const unsigned int splitter_tile_idx = splitter_global_idx - (tile_idx<<log_tile_size);

    // the index of the splitter's block within its tile
    const unsigned int block_tile_idx = splitter_tile_idx >> log_block_size;
    
    // find the end of the search range in the other tile
    unsigned int end = (( splitter_block_pair_idx - block_tile_idx) << log_block_size);

    // begin by assuming the search range is the size of a full block
    unsigned int other_block_size = block_size;

    // the index of the other tile can be found with
    const unsigned int other_tile_idx = tile_idx ^ 1;
    
    // the size of the other tile can be less than tile_size if the it is the last tile.
    unsigned int other_tile_size = min<unsigned int>(1 << log_tile_size, num_keys - (other_tile_idx<<log_tile_size));

    if(end > other_tile_size)
    {
      // the other block has partial size
      end = other_tile_size;
      other_block_size = num_keys % block_size;
    }
    else if(end == 0)
    {
      // when the search range is empty
      // the other_block_size is 0
      other_block_size = 0;
    }

    // the search range begins other_block_size elements before the end
    unsigned int start = end - other_block_size;

    return thrust::make_pair(start,other_block_size);
  }

  __device__ __thrust_forceinline__
  void operator()(void)
  {
    typedef typename iterator_value<RandomAccessIterator1>::type KeyType;
    typedef typename iterator_value<RandomAccessIterator2>::type IndexType;
  
    const unsigned int grid_size = context.grid_dimension() * context.block_dimension();
  
    unsigned int splitter_idx = context.thread_index() + context.block_index() * context.block_dimension();
  
    // advance iterators
    splitters_first     += splitter_idx;
    splitters_pos_first += splitter_idx;
    ranks_result1       += splitter_idx;
    ranks_result2       += splitter_idx;
    
    for(;
        splitter_idx < num_splitters;
        splitter_idx += grid_size, splitters_first += grid_size, splitters_pos_first += grid_size, ranks_result1 += grid_size, ranks_result2 += grid_size)
    {
      // the index of the splitter within the global array of elements
      IndexType splitter_global_idx = *splitters_pos_first;

      // the tile to which the splitter belongs.
      unsigned int tile_idx = (splitter_global_idx >> log_tile_size);
      
      // the index of the "other" tile which which tile_idx must be merged.
      unsigned int other_tile_idx = tile_idx^1;

      // compute the interval in the other tile to search
      unsigned int start, n;
      thrust::tie(start,n) = search_interval(splitter_idx, splitter_global_idx, tile_idx);

      // point to the beginning of the other tile
      RandomAccessIterator3 other_tile_begin = keys_first + (other_tile_idx<<log_tile_size);

      // offset the pointer to the other tile by the search range's offset
      RandomAccessIterator3 search_range_begin = other_tile_begin + start;
      
      // find the rank of our splitter in the other tile
      KeyType splitter = *splitters_first;

      // the index of the splitter within its tile
      // this is one of the output ranks
      const unsigned int splitter_tile_idx = splitter_global_idx - (tile_idx<<log_tile_size);

      // branch depending on whether or not our splitter is in the odd tile
      if(tile_idx & 1)
      {
        unsigned int result = thrust::system::detail::generic::scalar::upper_bound_n(search_range_begin, n, splitter, comp) - search_range_begin;

        *ranks_result1 = start + result;

        *ranks_result2 = splitter_tile_idx;
      } // end if
      else
      {
        unsigned int result = thrust::system::detail::generic::scalar::lower_bound_n(search_range_begin, n, splitter, comp) - search_range_begin;

        *ranks_result1 = splitter_tile_idx;

        *ranks_result2 = start + result;
      } // end else
    } // end for
  }
}; // rank_splitters_closure


template<unsigned int block_size,
         typename RandomAccessIterator1,
         typename RandomAccessIterator2,
         typename RandomAccessIterator3,
         typename RandomAccessIterator4,
         typename StrictWeakOrdering>
  void rank_splitters(RandomAccessIterator1 splitters_first,
                      RandomAccessIterator2 splitter_positions_first,
                      RandomAccessIterator3 keys_first,
                      size_t num_splitters,
                      size_t num_keys,
                      size_t log_tile_size,
                      RandomAccessIterator4 ranks_result1,
                      RandomAccessIterator4 ranks_result2,
                      StrictWeakOrdering comp)
{
  typedef rank_splitters_closure<
    block_size,
    RandomAccessIterator1,
    RandomAccessIterator2,
    RandomAccessIterator3,
    RandomAccessIterator4,
    StrictWeakOrdering,
    detail::statically_blocked_thread_array<block_size>
  > Closure;

  Closure closure(splitters_first,
                  splitter_positions_first,
                  keys_first,
                  num_splitters,
                  num_keys,
                  log_tile_size,
                  ranks_result1,
                  ranks_result2,
                  comp);

  detail::launch_closure(closure, closure.grid_size(), block_size);
}


template<typename Context,
         typename RandomAccessIterator1,
         typename RandomAccessIterator2,
         typename Size,
         typename RandomAccessIterator3,
         typename RandomAccessIterator4>
__device__
  void copy_n(Context context,
              RandomAccessIterator1 first1,
              RandomAccessIterator2 first2,
              Size n,
              RandomAccessIterator3 result1,
              RandomAccessIterator4 result2)
{
  for(Size i = context.thread_index();
      i < n;
      i += context.block_dimension())
  {
    result1[i] = first1[i];
    result2[i] = first2[i];
  }
}


///////////////////// MERGE TWO INDEPENDENT SEGMENTS USING BINARY SEARCH IN SHARED MEMORY ////////////////////////////////////////
// NOTE: This is the most compute-intensive part of the algorithm. 
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Thread block i merges entries between rank[i] and rank[i+1]. These can be independently
// merged and concatenated, as noted above. 
// Each thread in the thread block i does a binary search of one element between rank[i] -> rank[i+1] in the 
// other array. 

// Inputs: srcdatakey, value: inputs
//         log_blocksize, log_num_merged_splitters_per_block: as in previous functions
// Outputs: resultdatakey, resultdatavalue: output merged arrays are written here.
template<unsigned int block_size,
         typename RandomAccessIterator1,
         typename RandomAccessIterator2,
         typename RandomAccessIterator3,
         typename RandomAccessIterator4,
         typename RandomAccessIterator5,
         typename RandomAccessIterator6,
         typename StrictWeakOrdering,
         typename Context>
struct merge_subtiles_closure
{	
  static const unsigned int log_block_size = thrust::detail::mpl::math::log2<block_size>::value;

  RandomAccessIterator1 keys_first;
  RandomAccessIterator2 values_first;
  unsigned int datasize;
  RandomAccessIterator3 ranks_first1;
  RandomAccessIterator4 ranks_first2; 
  const unsigned int tile_size;
  const unsigned int log_num_merged_splitters_per_block;
  const unsigned int num_splitters;
  RandomAccessIterator5 keys_result;
  RandomAccessIterator6 values_result;
  StrictWeakOrdering comp;
  Context context;

  typedef Context context_type;

  merge_subtiles_closure
    (RandomAccessIterator1 keys_first,
     RandomAccessIterator2 values_first,
     unsigned int datasize, 
     RandomAccessIterator3 ranks_first1,
     RandomAccessIterator4 ranks_first2, 
     const unsigned int tile_size, 
     const unsigned int log_num_merged_splitters_per_block, 
     const unsigned int num_splitters,
     RandomAccessIterator5 keys_result,
     RandomAccessIterator6 values_result,
     StrictWeakOrdering comp,
     Context context = Context())
    : keys_first(keys_first), values_first(values_first), datasize(datasize),
      ranks_first1(ranks_first1), ranks_first2(ranks_first2),
      tile_size(tile_size),
      log_num_merged_splitters_per_block(log_num_merged_splitters_per_block),
      num_splitters(num_splitters),
      keys_result(keys_result), values_result(values_result),
      comp(comp), context(context)
  {}

  __device__ __thrust_forceinline__
  unsigned int even_offset(unsigned int oddeven_blockid) const
  {
    return oddeven_blockid << (log_num_merged_splitters_per_block + log_block_size);
  }

  __device__ __thrust_forceinline__
  void get_partition(unsigned int partition_idx, unsigned int oddeven_blockid,
                     unsigned int &rank1, unsigned int &size1,
                     unsigned int &rank2, unsigned int &size2) const
  {
    // XXX this logic would be much improved if we were guaranteed that there was 
    //     an element at ranks_first[1]
    // XXX we could eliminate the need for local_blockIdx, log_num_merged_splitters_per_block, tile_size, and datasize
    
    // the index of the merged splitter within the splitters for the odd-even block pair.
    unsigned int local_blockIdx = partition_idx - (oddeven_blockid<<log_num_merged_splitters_per_block);

    rank1 = *ranks_first1;
    rank2 = *ranks_first2;
  
    // get the rank of the next splitter if we aren't processing the very last splitter of a partially full tile
    // or if we aren't processing the last splitter in our tile
    if((partition_idx == num_splitters - 1) || (local_blockIdx == ((1<<log_num_merged_splitters_per_block)-1)))
    {
      // we're at the end
      size1 = size2 = tile_size;
    } // end if
    else
    {
      // dereference the rank of the *next* splitter
      size1 = ranks_first1[1];
      size2 = ranks_first2[1];
    } // end else
    
    // Adjust size2 to account for the last block possibly not being full.
    // check if size2 would fall off the end of the array
    if((even_offset(oddeven_blockid) + tile_size + size2) > datasize)
    {
      size2 = datasize - tile_size - even_offset(oddeven_blockid);
    } // end if
  
    // measure each array relative to its beginning
    size1 -= rank1;
    size2 -= rank2;
  }

  template<typename KeyType, typename ValueType>
  __device__ __thrust_forceinline__
  void do_it(KeyType *s_keys, ValueType *s_values)
  {
    // advance iterators
    unsigned int i = context.block_index();
    ranks_first1 += i;
    ranks_first2 += i;
    
    // Thread Block i merges the sub-block associated with splitter i: rank[i] -> rank[i+1] in a particular odd-even block pair.
    for(;
        i < num_splitters;
        i += context.grid_dimension(), ranks_first1 += context.grid_dimension(), ranks_first2 += context.grid_dimension())
    {
      // the (odd, even) block pair that the splitter belongs to.
      unsigned int oddeven_blockid = i >> log_num_merged_splitters_per_block;
      
      // start1 & start2 store rank[i] and rank[i+1] indices in arrays 1 and 2.
      // size1 & size2 store the number of of elements between rank[i] & rank[i+1] in arrays 1 & 2.
      unsigned int rank1, rank2, size1, size2;
      get_partition(i, oddeven_blockid, rank1, size1, rank2, size2);
  
      // find where the odd,even arrays begin
      RandomAccessIterator1 even_keys_first = keys_first + even_offset(oddeven_blockid);
      RandomAccessIterator1 odd_keys_first  = even_keys_first + tile_size;
  
      RandomAccessIterator2 even_values_first = values_first + even_offset(oddeven_blockid);
      RandomAccessIterator2 odd_values_first  = even_values_first + tile_size;
      
      // load tiles into smem
      copy_n(context, even_keys_first + rank1, even_values_first + rank1, size1, s_keys, s_values);
      copy_n(context, odd_keys_first  + rank2, odd_values_first  + rank2, size2, s_keys + size1, s_values + size1);

      context.barrier();
  
      // merge the arrays in-place
      block::inplace_merge_by_key_n(context, s_keys, s_values, size1, size2, comp);

      context.barrier();
      
      // write tiles to gmem
      unsigned int dst_offset = even_offset(oddeven_blockid) + rank1 + rank2;
      copy_n(context, s_keys, s_values, size1 + size2, keys_result + dst_offset, values_result + dst_offset);
    } // end for i
  }

  __device__ __thrust_forceinline__
  void operator()(void)
  {
    typedef typename iterator_value<RandomAccessIterator5>::type KeyType;
    typedef typename iterator_value<RandomAccessIterator6>::type ValueType;
  
    __shared__ uninitialized_array<KeyType,   2 * block_size> s_keys;
    __shared__ uninitialized_array<ValueType, 2 * block_size> s_values;
  
    do_it(s_keys.data(), s_values.data());
  }
}; // merge_subtiles_closure

// merge_subtiles() merges each sub-tile independently. As explained in rank_splitters(), 
// the sub-tiles are defined by the ranks of the splitter elements d_rank1 and d_rank2 in the odd and even tiles resp.
// It can be easily shown that each sub-tile cannot contain more than block_size elements of either the odd or even tile.

// the function calls merge_subblocks_binarysearch_kernel() for the remaining N_splitterS sub-tiles
// We use 1 thread block per splitter: For instance, thread block 0 will merge rank1[0] -> rank1[1] of array i with
// rank2[0] -> rank2[1] of array i^1, with i being the thread block to which the splitter belongs.

// We implement each sub-tile merge using a binary search. We compute the rank of each element belonging to a sub-tile 
// of an odd numbered tile in the corresponding sub-tile of its even numbered pair. It then adds this rank to 
// the index of the element in its own sub-tile to find the output index of the element in the merged sub-tile.

template<typename RandomAccessIterator1,
         typename RandomAccessIterator2,
         typename RandomAccessIterator3,
         typename RandomAccessIterator4,
         typename RandomAccessIterator5,
         typename RandomAccessIterator6,
         typename RandomAccessIterator7,
         typename StrictWeakOrdering>
  void merge_subtiles(RandomAccessIterator1 keys_first,
                      RandomAccessIterator2 values_first,
                      unsigned int datasize, 
                      RandomAccessIterator3 splitters_pos_first, 
                      RandomAccessIterator4 ranks_first1,
                      RandomAccessIterator5 ranks_first2, 
                      RandomAccessIterator6 keys_result,
                      RandomAccessIterator7 values_result, 
                      unsigned int num_splitters, unsigned int tile_size, 
                      unsigned int log_num_merged_splitters_per_block,
                      unsigned int num_oddeven_tile_pairs,
                      StrictWeakOrdering comp)
{
  typedef typename iterator_value<RandomAccessIterator6>::type KeyType;
  typedef typename iterator_value<RandomAccessIterator7>::type ValueType;

  const unsigned int block_size = merge_sort_dev_namespace::block_size<KeyType,ValueType>::result;

  typedef merge_subtiles_closure<
    block_size,
    RandomAccessIterator1,
    RandomAccessIterator2,
    RandomAccessIterator4,
    RandomAccessIterator5,
    RandomAccessIterator6,
    RandomAccessIterator7,
    StrictWeakOrdering,
    detail::statically_blocked_thread_array<block_size>
  > Closure;

  Closure closure(keys_first,
                  values_first,
                  datasize, 
                  ranks_first1,
                  ranks_first2, 
                  tile_size,
                  log_num_merged_splitters_per_block, 
                  num_splitters,
  	          keys_result,
                  values_result,
                  comp);

  const unsigned int max_num_blocks = max_grid_size(block_size);
  const unsigned int grid_size = min(num_splitters, max_num_blocks);

  // XXX why do we allocate dynamic smem???
  detail::launch_closure(closure, grid_size, block_size, block_size*(2*sizeof(KeyType) + 2*sizeof(ValueType)));
}


template<unsigned int block_size,
         typename System,
         typename RandomAccessIterator1,
         typename RandomAccessIterator2,
         typename RandomAccessIterator3,
         typename RandomAccessIterator4,
         typename StrictWeakOrdering>
  void merge_small_tiles(dispatchable<System> &,
                         RandomAccessIterator1 keys_first,
                         RandomAccessIterator2 values_first,
                         size_t n,
                         size_t log_tile_size,
                         RandomAccessIterator3 keys_result,
                         RandomAccessIterator4 values_result,
                         StrictWeakOrdering comp)
{
  typedef merge_small_tiles_closure<
    block_size,
    RandomAccessIterator1,
    RandomAccessIterator2,
    RandomAccessIterator3,
    RandomAccessIterator4,
    StrictWeakOrdering,
    detail::statically_blocked_thread_array<block_size>
  > Closure;

  Closure closure(keys_first, values_first, n, log_tile_size, keys_result, values_result, comp);

  detail::launch_closure(closure, closure.grid_size(), block_size);
} // end merge_small_tiles()


template<typename System,
         typename RandomAccessIterator1,
         typename RandomAccessIterator2,
         typename RandomAccessIterator3,
         typename RandomAccessIterator4,
         typename StrictWeakOrdering>
  void merge_recursive(dispatchable<System> &system,
                       RandomAccessIterator1 keys_first,
                       RandomAccessIterator2 values_first,
                       size_t n,
                       RandomAccessIterator3 keys_result,
                       RandomAccessIterator4 values_result,
                       size_t log_tile_size,
                       StrictWeakOrdering comp)
{
  typedef typename iterator_value<RandomAccessIterator3>::type KeyType;
  typedef typename iterator_value<RandomAccessIterator4>::type ValueType;

  size_t tile_size = 1<<log_tile_size;

  // Compute the block_size based on the types to sort
  const unsigned int block_size = merge_sort_dev_namespace::block_size<KeyType,ValueType>::result;

  // Case (a): tile_size <= block_size
  if(tile_size <= block_size)
  {
    return merge_small_tiles<2*block_size>(system, keys_first, values_first, n, log_tile_size, keys_result, values_result, comp);
  } // end if

  // Case (b) tile_size >= block_size

  // Step 1 of the recursive case: gather one splitter per block_size entries in each odd-even block pair.
  size_t num_splitters = n / block_size;
  if(n % block_size) ++num_splitters;

  using thrust::detail::temporary_array;

  // gather splitters
  static_strided_integer_range<block_size>   splitters_pos(num_splitters);
  temporary_array<KeyType,      System>      splitters(system, num_splitters);
  thrust::gather(system, splitters_pos.begin(), splitters_pos.end(), keys_first, splitters.begin());
                            
  // Step 2 of the recursive case: merge these elements using merge
  // We need to merge num_splitters elements, each new "block" is the set of
  // splitters for each original block.
  temporary_array<KeyType,      System>      merged_splitters(system, num_splitters);
  temporary_array<unsigned int, System>      merged_splitters_pos(system, num_splitters);

  // compute the log base 2 of the block_size
  const unsigned int log_block_size = merge_sort_dev_namespace::log_block_size<KeyType,ValueType>::result;

  size_t log_num_splitters_per_tile = log_tile_size - log_block_size;
  merge_recursive(system,
                  splitters.begin(),
                  splitters_pos.begin(),
                  num_splitters,
                  merged_splitters.begin(),
                  merged_splitters_pos.begin(),
                  log_num_splitters_per_tile,
                  comp);

  // step 3 of the recursive case: Find the ranks of each splitter in the respective two tiles.
  temporary_array<unsigned int, System> rank1(system, num_splitters);
  temporary_array<unsigned int, System> rank2(system, num_splitters);

  rank_splitters<block_size>(merged_splitters.begin(),
                             merged_splitters_pos.begin(),
                             keys_first,
                             num_splitters,
                             n,
                             log_tile_size,
                             rank1.begin(),
                             rank2.begin(),
                             comp);

  // assumption: num_tiles is even; tile_size is a power of 2
  size_t num_tiles = n / tile_size;
  size_t partial_tile_size = n % tile_size;
  if(partial_tile_size) ++num_tiles;

  // step 4 of the recursive case: merge each sub-tile independently in parallel.
  size_t log_num_merged_splitters_per_tile = log_num_splitters_per_tile + 1;
  merge_subtiles(keys_first,
                 values_first,
                 n,
                 merged_splitters_pos.begin(),
                 rank1.begin(),
                 rank2.begin(),
                 keys_result,
                 values_result,
                 num_splitters,
                 1<<log_tile_size,
                 log_num_merged_splitters_per_tile,
                 num_tiles / 2,
                 comp);
}

template<typename System,
         typename RandomAccessIterator1,
         typename RandomAccessIterator2,
         typename RandomAccessIterator3,
         typename RandomAccessIterator4,
         typename StrictWeakOrdering>
  void merge(dispatchable<System> &system,
             RandomAccessIterator1 keys_first,
             RandomAccessIterator2 values_first,
             size_t n,
             RandomAccessIterator3 keys_result,
             RandomAccessIterator4 values_result,
             size_t block_size,
             StrictWeakOrdering comp)
{
#if THRUST_DEVICE_COMPILER == THRUST_DEVICE_COMPILER_NVCC
  unsigned int log_block_size = (unsigned int)logb((float)block_size);
#else
  unsigned int log_block_size = 0;
#endif // THRUST_DEVICE_COMPILER_NVCC
  unsigned int num_blocks = (n%block_size)?((n/block_size)+1):(n/block_size);

  merge_recursive(system,
                  keys_first,
                  values_first,
                  (num_blocks%2)?((num_blocks-1)*block_size):n,
                  keys_result,
                  values_result,
                  log_block_size,
                  comp);

  if(num_blocks%2)
  {
    thrust::copy(system,
                 keys_first + (num_blocks-1)*block_size,
                 keys_first + n,
                 keys_result + (num_blocks-1)*block_size);
    
    thrust::copy(system,
                 values_first + (num_blocks-1)*block_size,
                 values_first + n,
                 values_result + (num_blocks-1)*block_size);
  }
}


} // end merge_sort_dev_namespace



template<typename System,
         typename RandomAccessIterator,
         typename StrictWeakOrdering>
void stable_merge_sort(dispatchable<System> &system,
                       RandomAccessIterator first,
                       RandomAccessIterator last,
                       StrictWeakOrdering comp)
{
    // XXX it's potentially unsafe to pass the same array for keys & values
    //     implement a legit merge_sort_dev function later
    thrust::system::cuda::detail::detail::stable_merge_sort_by_key(system, first, last, first, comp);
}


template<typename System,
         typename RandomAccessIterator1,
         typename RandomAccessIterator2,
         typename StrictWeakOrdering>
  void stable_merge_sort_by_key(dispatchable<System> &system,
                                RandomAccessIterator1 keys_first,
                                RandomAccessIterator1 keys_last,
                                RandomAccessIterator2 values_first,
                                StrictWeakOrdering comp)
{
  typedef typename thrust::iterator_traits<RandomAccessIterator1>::value_type KeyType;
  typedef typename thrust::iterator_traits<RandomAccessIterator2>::value_type ValueType;

  const size_t n = keys_last - keys_first;

  // don't launch an empty kernel
  if(n == 0) return;

  // compute the block_size based on the types we're sorting
  const unsigned int block_size = merge_sort_dev_namespace::block_size<KeyType,ValueType>::result;

  // compute the maximum number of blocks we can launch on this arch
  const unsigned int max_num_blocks = merge_sort_dev_namespace::max_grid_size(block_size);

  // first, sort within each block
  size_t num_blocks = n / block_size;
  if(n % block_size) ++num_blocks;

  size_t grid_size = merge_sort_dev_namespace::min<size_t>(num_blocks, max_num_blocks);

  typedef merge_sort_dev_namespace::stable_odd_even_block_sort_closure<
    block_size,
    RandomAccessIterator1,
    RandomAccessIterator2,
    StrictWeakOrdering,
    detail::statically_blocked_thread_array<block_size>
  > StableOddEvenBlockSortClosure;
 
  // do an odd-even sort per block of data
  detail::launch_closure
    (StableOddEvenBlockSortClosure(keys_first, values_first, comp, n),
     grid_size, block_size);

  // allocate scratch space
  using namespace thrust::detail;
  temporary_array<KeyType,   System> temp_keys(system, n);
  temporary_array<ValueType, System> temp_vals(system, n);

  // give iterators simpler names
  RandomAccessIterator1 keys0 = keys_first;
  RandomAccessIterator2 vals0 = values_first;
  typename temporary_array<KeyType,   System>::iterator keys1 = temp_keys.begin();
  typename temporary_array<ValueType, System>::iterator vals1 = temp_vals.begin();

  // The log(n) iterations start here. Each call to 'merge' merges an odd-even pair of tiles
  // Currently uses additional arrays for sorted outputs.
  unsigned int iterations = 0;
  for(unsigned int tile_size = block_size;
      tile_size < n;
      tile_size *= 2)
  {
    if (iterations % 2)
      merge_sort_dev_namespace::merge(system, keys1, vals1, n, keys0, vals0, tile_size, comp);
    else
      merge_sort_dev_namespace::merge(system, keys0, vals0, n, keys1, vals1, tile_size, comp);
    ++iterations;
  }

  // this is to make sure that our data is finally in the data and keys arrays
  // and not in the temporary arrays
  if(iterations % 2)
  {
    thrust::copy(system, vals1, vals1 + n, vals0);
    thrust::copy(system, keys1, keys1 + n, keys0);
  }
} // end stable_merge_sort_by_key()

} // end namespace detail
} // end namespace detail
} // end namespace cuda
} // end namespace system
} // end namespace thrust

__THRUST_DISABLE_MSVC_POSSIBLE_LOSS_OF_DATA_WARNING_END

