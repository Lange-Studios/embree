// ======================================================================== //
// Copyright 2009-2014 Intel Corporation                                    //
//                                                                          //
// Licensed under the Apache License, Version 2.0 (the "License");          //
// you may not use this file except in compliance with the License.         //
// You may obtain a copy of the License at                                  //
//                                                                          //
//     http://www.apache.org/licenses/LICENSE-2.0                           //
//                                                                          //
// Unless required by applicable law or agreed to in writing, software      //
// distributed under the License is distributed on an "AS IS" BASIS,        //
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. //
// See the License for the specific language governing permissions and      //
// limitations under the License.                                           //
// ======================================================================== //

#include "bvh4i_builder_morton.h"
#include "builders/builder_util.h"
#include "bvh4i_rotate.h"

#define MORTON_BVH4I_NODE_PREALLOC_FACTOR   0.8f
#define NUM_MORTON_IDS_PER_BLOCK            4
#define SINGLE_THREADED_BUILD_THRESHOLD     (MAX_MIC_THREADS*64)

//#define PROFILE
#define PROFILE_ITERATIONS 200

#define TIMER(x) x
#define DBG(x) 

#define L1_PREFETCH_ITEMS 8
#define L2_PREFETCH_ITEMS 44

namespace embree 
{
#if defined(DEBUG)
  extern AtomicMutex mtx;
#endif

template<class T>
  __forceinline T bitInterleave64(const T& xin, const T& yin, const T& zin){
    T x = xin & 0x1fffff; 
    T y = yin & 0x1fffff; 
    T z = zin & 0x1fffff; 

    x = (x | x << 32) & 0x1f00000000ffff;  
    x = (x | x << 16) & 0x1f0000ff0000ff;  
    x = (x | x << 8) & 0x100f00f00f00f00f; 
    x = (x | x << 4) & 0x10c30c30c30c30c3; 
    x = (x | x << 2) & 0x1249249249249249;

    y = (y | y << 32) & 0x1f00000000ffff;  
    y = (y | y << 16) & 0x1f0000ff0000ff;  
    y = (y | y << 8) & 0x100f00f00f00f00f; 
    y = (y | y << 4) & 0x10c30c30c30c30c3; 
    y = (y | y << 2) & 0x1249249249249249;

    z = (z | z << 32) & 0x1f00000000ffff;  
    z = (z | z << 16) & 0x1f0000ff0000ff;  
    z = (z | z << 8) & 0x100f00f00f00f00f; 
    z = (z | z << 4) & 0x10c30c30c30c30c3; 
    z = (z | z << 2) & 0x1249249249249249;

    return x | (y << 1) | (z << 2);
}

  __aligned(64) static const unsigned int mortonLUT[256] =
{
    0x00000000, 0x00000001, 0x00000008, 0x00000009, 0x00000040, 0x00000041, 0x00000048, 0x00000049, 0x00000200,
    0x00000201, 0x00000208, 0x00000209, 0x00000240, 0x00000241, 0x00000248, 0x00000249, 0x00001000,
    0x00001001, 0x00001008, 0x00001009, 0x00001040, 0x00001041, 0x00001048, 0x00001049, 0x00001200,
    0x00001201, 0x00001208, 0x00001209, 0x00001240, 0x00001241, 0x00001248, 0x00001249, 0x00008000,
    0x00008001, 0x00008008, 0x00008009, 0x00008040, 0x00008041, 0x00008048, 0x00008049, 0x00008200,
    0x00008201, 0x00008208, 0x00008209, 0x00008240, 0x00008241, 0x00008248, 0x00008249, 0x00009000,
    0x00009001, 0x00009008, 0x00009009, 0x00009040, 0x00009041, 0x00009048, 0x00009049, 0x00009200,
    0x00009201, 0x00009208, 0x00009209, 0x00009240, 0x00009241, 0x00009248, 0x00009249, 0x00040000,
    0x00040001, 0x00040008, 0x00040009, 0x00040040, 0x00040041, 0x00040048, 0x00040049, 0x00040200,
    0x00040201, 0x00040208, 0x00040209, 0x00040240, 0x00040241, 0x00040248, 0x00040249, 0x00041000,
    0x00041001, 0x00041008, 0x00041009, 0x00041040, 0x00041041, 0x00041048, 0x00041049, 0x00041200,
    0x00041201, 0x00041208, 0x00041209, 0x00041240, 0x00041241, 0x00041248, 0x00041249, 0x00048000,
    0x00048001, 0x00048008, 0x00048009, 0x00048040, 0x00048041, 0x00048048, 0x00048049, 0x00048200,
    0x00048201, 0x00048208, 0x00048209, 0x00048240, 0x00048241, 0x00048248, 0x00048249, 0x00049000,
    0x00049001, 0x00049008, 0x00049009, 0x00049040, 0x00049041, 0x00049048, 0x00049049, 0x00049200,
    0x00049201, 0x00049208, 0x00049209, 0x00049240, 0x00049241, 0x00049248, 0x00049249, 0x00200000,
    0x00200001, 0x00200008, 0x00200009, 0x00200040, 0x00200041, 0x00200048, 0x00200049, 0x00200200,
    0x00200201, 0x00200208, 0x00200209, 0x00200240, 0x00200241, 0x00200248, 0x00200249, 0x00201000,
    0x00201001, 0x00201008, 0x00201009, 0x00201040, 0x00201041, 0x00201048, 0x00201049, 0x00201200,
    0x00201201, 0x00201208, 0x00201209, 0x00201240, 0x00201241, 0x00201248, 0x00201249, 0x00208000,
    0x00208001, 0x00208008, 0x00208009, 0x00208040, 0x00208041, 0x00208048, 0x00208049, 0x00208200,
    0x00208201, 0x00208208, 0x00208209, 0x00208240, 0x00208241, 0x00208248, 0x00208249, 0x00209000,
    0x00209001, 0x00209008, 0x00209009, 0x00209040, 0x00209041, 0x00209048, 0x00209049, 0x00209200,
    0x00209201, 0x00209208, 0x00209209, 0x00209240, 0x00209241, 0x00209248, 0x00209249, 0x00240000,
    0x00240001, 0x00240008, 0x00240009, 0x00240040, 0x00240041, 0x00240048, 0x00240049, 0x00240200,
    0x00240201, 0x00240208, 0x00240209, 0x00240240, 0x00240241, 0x00240248, 0x00240249, 0x00241000,
    0x00241001, 0x00241008, 0x00241009, 0x00241040, 0x00241041, 0x00241048, 0x00241049, 0x00241200,
    0x00241201, 0x00241208, 0x00241209, 0x00241240, 0x00241241, 0x00241248, 0x00241249, 0x00248000,
    0x00248001, 0x00248008, 0x00248009, 0x00248040, 0x00248041, 0x00248048, 0x00248049, 0x00248200,
    0x00248201, 0x00248208, 0x00248209, 0x00248240, 0x00248241, 0x00248248, 0x00248249, 0x00249000,
    0x00249001, 0x00249008, 0x00249009, 0x00249040, 0x00249041, 0x00249048, 0x00249049, 0x00249200,
    0x00249201, 0x00249208, 0x00249209, 0x00249240, 0x00249241, 0x00249248, 0x00249249
};


  __forceinline size_t getMortonCode_LUT(const unsigned int &v)
  {
    const unsigned int byte0 = (v >> 0)  & 0xff;
    const unsigned int byte1 = (v >> 8)  & 0xff;
    const unsigned int byte2 = (v >> 16) & 0xff;

    return (size_t)mortonLUT[byte0] | ((size_t)mortonLUT[byte1] << 24) | ((size_t)mortonLUT[byte2] << 48);

  }
  __forceinline size_t bitInterleave64_LUT(const unsigned int& xin, const unsigned int& yin, const unsigned int& zin){
    const unsigned int x = xin & 0x1fffff; 
    const unsigned int y = yin & 0x1fffff; 
    const unsigned int z = zin & 0x1fffff; 
    return getMortonCode_LUT( x ) | (getMortonCode_LUT( y ) << 1) | (getMortonCode_LUT( z ) << 2);
}

  // =======================================================================================================
  // =======================================================================================================
  // =======================================================================================================

  __aligned(64) static double dt = 0.0f;

  BVH4iBuilderMorton64Bit::BVH4iBuilderMorton64Bit(BVH4i* bvh, void* geometry)
    : bvh(bvh), scene((Scene*)geometry), morton(NULL), node(NULL), accel(NULL), numPrimitives(0), numGroups(0),numNodes(0), numAllocatedNodes(0), size_morton(0), size_node(0), size_accel(0), numPrimitivesOld(-1), topLevelItemThreshold(0), numBuildRecords(0)
  {
  }

  BVH4iBuilderMorton64Bit::~BVH4iBuilderMorton64Bit()
  {
    if (morton) {
      assert(size_morton > 0);
      os_free(morton,size_morton);
    }
  }


  void BVH4iBuilderMorton64Bit::allocateData(size_t threadCount)
  {
    /* preallocate arrays */
    const size_t additional_size = 16 * CACHELINE_SIZE;
    if (numPrimitivesOld != numPrimitives)
    {
      DBG(
	  DBG_PRINT( numPrimitivesOld );
	  DBG_PRINT( numPrimitives );
	  );

      numPrimitivesOld = numPrimitives;
      /* free previously allocated memory */
      if (morton) {
	assert(size_morton > 0);
	os_free(morton,size_morton);
      }
      if (node) { 
	assert(size_node > 0);
	os_free(node  ,size_node);
      }
      if (accel ) {
	assert(size_accel > 0);
	os_free(accel ,size_accel);
      }
      
      /* allocated memory for primrefs,nodes, and accel */
      const size_t minAllocNodes = (threadCount+1) * 2* ALLOCATOR_NODE_BLOCK_SIZE;


      const size_t numPrims      = numPrimitives+4;
      const size_t numNodes      = (size_t)((numPrimitives+3)/4);


      const size_t sizeNodeInBytes = sizeof(BVH4i::Node);
      const size_t sizeAccelInBytes = sizeof(Triangle1);


      const size_t size_morton_tmp = numPrims * sizeof(MortonID64Bit) + additional_size;

      size_node         = (numNodes * MORTON_BVH4I_NODE_PREALLOC_FACTOR + minAllocNodes) * sizeNodeInBytes + additional_size;
      size_accel        = numPrims * sizeAccelInBytes + additional_size;
      numAllocatedNodes = size_node / sizeof(BVH4i::Node);

      morton = (MortonID64Bit* ) os_malloc(size_morton_tmp); 
      node   = (BVH4i::Node*)    os_malloc(size_node  );     
      accel  = (Triangle1*)      os_malloc(size_accel );     

      assert(morton != 0);
      assert(node   != 0);
      assert(accel  != 0);

      size_morton = size_morton_tmp;

#if DEBUG
      DBG_PRINT( minAllocNodes );
      DBG_PRINT( numNodes );
#endif

    }

    bvh->accel = accel;
    bvh->qbvh  = (BVH4i::Node*)node;
    bvh->size_node  = size_node;
    bvh->size_accel = size_accel;

#if DEBUG
    DBG_PRINT(bvh->size_node);
    DBG_PRINT(bvh->size_accel);
    DBG_PRINT(numAllocatedNodes);
#endif

  }

  void BVH4iBuilderMorton64Bit::split_fallback(SmallBuildRecord& current, SmallBuildRecord& leftChild, SmallBuildRecord& rightChild) 
  {
    unsigned int blocks4 = (current.items()+3)/4;
    unsigned int center = current.begin + (blocks4/2)*4; 

    assert(center != current.begin);
    assert(center != current.end);

    leftChild.init(current.begin,center);
    rightChild.init(center,current.end);
  }
		

  __forceinline BBox3fa BVH4iBuilderMorton64Bit::createSmallLeaf(SmallBuildRecord& current) 
  {    
    assert(current.size() > 0);
    mic_f bounds_min(pos_inf);
    mic_f bounds_max(neg_inf);

    Vec3fa lower(pos_inf);
    Vec3fa upper(neg_inf);
    unsigned int items = current.size();
    unsigned int start = current.begin;
    assert(items<=4);

    prefetch<PFHINT_L2>(&morton[start+8]);

    for (size_t i=0; i<items; i++) 
      {	
	const unsigned int primID = morton[start+i].primID;
	const unsigned int geomID = morton[start+i].groupID;

	const mic_i morton_primID = morton[start+i].primID;
	const mic_i morton_geomID = morton[start+i].groupID;

	const TriangleMesh* __restrict__ const mesh = scene->getTriangleMesh(geomID);
	const TriangleMesh::Triangle& tri = mesh->triangle(primID);

	const mic3f v = mesh->getTriangleVertices(tri);
	const mic_f v0 = v[0];
	const mic_f v1 = v[1];
	const mic_f v2 = v[2];

	const mic_f tri_accel = initTriangle1(v0,v1,v2,morton_geomID,morton_primID,mic_i(mesh->mask));

	bounds_min = min(bounds_min,min(v0,min(v1,v2)));
	bounds_max = max(bounds_max,max(v0,max(v1,v2)));
	store16f_ngo(&accel[start+i],tri_accel);
      }

    store4f(&node[current.parentNodeID].lower[current.parentLocalID],bounds_min);
    store4f(&node[current.parentNodeID].upper[current.parentLocalID],bounds_max);
    createLeaf(node[current.parentNodeID].lower[current.parentLocalID].child,start,items);
    __aligned(64) BBox3fa bounds;
    store4f(&bounds.lower,bounds_min);
    store4f(&bounds.upper,bounds_max);
    return bounds;
  }


  BBox3fa BVH4iBuilderMorton64Bit::createLeaf(SmallBuildRecord& current, NodeAllocator& alloc)
  {
#if defined(DEBUG)
    if (current.depth > BVH4i::maxBuildDepthLeaf) 
      throw std::runtime_error("ERROR: depth limit reached");
#endif
    
    /* create leaf for few primitives */
    if (current.size() <= MORTON_LEAF_THRESHOLD) {     
      return createSmallLeaf(current);
    }

    /* first split level */
    SmallBuildRecord record0, record1;
    split_fallback(current,record0,record1);

    /* second split level */
    SmallBuildRecord children[4];
    split_fallback(record0,children[0],children[1]);
    split_fallback(record1,children[2],children[3]);

    /* allocate next four nodes */
    size_t numChildren = 4;
    const size_t currentIndex = alloc.get(1);
   
    /* init used/unused nodes */
    // const mic_f init_node = load16f((float*)BVH4i::initQBVHNode);
    // store16f_ngo((float*)&node[currentIndex+0],init_node);
    // store16f_ngo((float*)&node[currentIndex+2],init_node);

    mic_f init_lower = broadcast4to16f(&BVH4i::initQBVHNode[0]);
    mic_f init_upper = broadcast4to16f(&BVH4i::initQBVHNode[1]);

    store16f_ngo((float*)&node[currentIndex].lower,init_lower);
    store16f_ngo((float*)&node[currentIndex].upper,init_upper);


    __aligned(64) BBox3fa bounds; 
    bounds = empty;
    /* recurse into each child */
    for (size_t i=0; i<numChildren; i++) {
      children[i].parentNodeID  = currentIndex;
      children[i].parentLocalID = i;
      children[i].depth = current.depth+1;
      bounds.extend( createLeaf(children[i],alloc) );
    }

    store4f(&node[current.parentNodeID].lower[current.parentLocalID],broadcast4to16f(&bounds.lower));
    store4f(&node[current.parentNodeID].upper[current.parentLocalID],broadcast4to16f(&bounds.upper));

    createNode(node[current.parentNodeID].lower[current.parentLocalID].child,currentIndex,0); // numChildren);

    return bounds;
  }  

  __forceinline bool BVH4iBuilderMorton64Bit::split(SmallBuildRecord& current,
						    SmallBuildRecord& left,
						    SmallBuildRecord& right) 
  {
    /* mark as leaf if leaf threshold reached */
    if (unlikely(current.size() <= BVH4iBuilderMorton64Bit::MORTON_LEAF_THRESHOLD)) {
      //current.createLeaf();
      return false;
    }

#if DEBUG
    for (size_t i=current.begin+1;  i<current.end; i++) 
      assert(morton[i-1].code <= morton[i].code);

#endif

    const size_t code_start = morton[current.begin].code;
    const size_t code_end   = morton[current.end-1].code;
    size_t bitpos = clz(code_start^code_end);

    /* if all items mapped to same morton code, then create new morton codes for the items */
    if (unlikely(bitpos == 64)) 
    {
      size_t blocks4 = (current.items()+3)/4;
      size_t center = current.begin + (blocks4/2)*4; 

      assert(current.begin < center);
      assert(center < current.end);
      
      left.init(current.begin,center);
      right.init(center,current.end);
      return true;
    }

    /* split the items at the topmost different morton code bit */
    const size_t bitpos_diff = 63-bitpos;
    const size_t bitmask = (size_t)1 << bitpos_diff;

    /* find location where bit differs using binary search */
    size_t begin = current.begin;
    size_t end   = current.end;
    while (begin + 1 != end) {
      const size_t mid = (begin+end)/2;
      const size_t bit = morton[mid].code & bitmask;
      if (bit == 0) begin = mid; else end = mid;
    }

    size_t center = end;

#if defined(DEBUG)      
    for (size_t i=current.begin;  i<center; i++) assert((morton[i].code & bitmask) == 0);
    for (size_t i=center; i<current.end;    i++) assert((morton[i].code & bitmask) == bitmask);
#endif

    assert(current.begin < center);
    assert(center < current.end);
    
    left.init(current.begin,center);
    right.init(center,current.end);
    return true;
  }

  size_t BVH4iBuilderMorton64Bit::createQBVHNode(SmallBuildRecord& current, SmallBuildRecord *__restrict__ const children)
  {

    /* create leaf node */
    if (unlikely(current.size() <= BVH4iBuilderMorton64Bit::MORTON_LEAF_THRESHOLD)) {
      children[0] = current;
      return 1;
    }

    /* fill all 4 children by always splitting the one with the largest number of primitives */
    __assume_aligned(children,sizeof(SmallBuildRecord));

    size_t numChildren = 1;
    children[0] = current;

    do {

      /* find best child with largest number of items*/
      int bestChild = -1;
      unsigned bestItems = 0;
      for (unsigned int i=0; i<numChildren; i++)
      {
        /* ignore leaves as they cannot get split */
        if (children[i].size() <= BVH4iBuilderMorton64Bit::MORTON_LEAF_THRESHOLD)
          continue;
        
        /* remember child with largest number of items */
        if (children[i].size() > bestItems) { 
          bestItems = children[i].size();
          bestChild = i;
        }
      }
      if (bestChild == -1) break;

      /*! split best child into left and right child */
      __aligned(64) SmallBuildRecord left, right;
      if (!split(children[bestChild],left,right))
        continue;
      
      /* add new children left and right */
      left.depth = right.depth = current.depth+1;
      children[bestChild] = children[numChildren-1];
      children[numChildren-1] = left;
      children[numChildren+0] = right;
      numChildren++;
      
    } while (numChildren < BVH4i::N);

    /* create leaf node if no split is possible */
    if (unlikely(numChildren == 1)) {
      children[0] = current;
      return 1;
    }

    /* allocate next four nodes and prefetch them */
    const size_t currentIndex = allocNode(BVH4i::N);    

    /* init used/unused nodes */
    //const mic_f init_node = load16f((float*)BVH4i::initQBVHNode);

    mic_f init_lower = broadcast4to16f(&BVH4i::initQBVHNode[0]);
    mic_f init_upper = broadcast4to16f(&BVH4i::initQBVHNode[1]);

    store16f_ngo((float*)&node[currentIndex].lower,init_lower);
    store16f_ngo((float*)&node[currentIndex].upper,init_upper);

    /* recurse into each child */
    for (size_t i=0; i<numChildren; i++) 
      {
	children[i].parentNodeID  = currentIndex;
	children[i].parentLocalID = i;
      }

    createNode(node[current.parentNodeID].lower[current.parentLocalID].child,currentIndex,0); //numChildren);
    return numChildren;
  }

  
  BBox3fa BVH4iBuilderMorton64Bit::recurse(SmallBuildRecord& current, 
					   NodeAllocator& alloc,
					   const size_t mode, 
					   const size_t numThreads) 
  {
    assert(current.size() > 0);
#if DEBUG
    //DBG_PRINT(current);
#endif
    /* stop toplevel recursion at some number of items */
    if (unlikely(mode == CREATE_TOP_LEVEL))
      {
	if (current.size()  <= topLevelItemThreshold &&
	    numBuildRecords >= numThreads) {
	  buildRecords[numBuildRecords++] = current;
	  return empty;
	}
      }

    __aligned(64) SmallBuildRecord children[BVH4i::N];

    /* create leaf node */
    if (unlikely(current.size() <= BVH4iBuilderMorton64Bit::MORTON_LEAF_THRESHOLD)) {
      return createSmallLeaf(current);
    }
    if (unlikely(current.depth >= BVH4i::maxBuildDepth)) {
      return createLeaf(current,alloc); 
    }

    /* fill all 4 children by always splitting the one with the largest number of primitives */
    size_t numChildren = 1;
    children[0] = current;

    do {

      /* find best child with largest number of items*/
      int bestChild = -1;
      unsigned bestItems = 0;
      for (unsigned int i=0; i<numChildren; i++)
      {
        /* ignore leaves as they cannot get split */
        if (children[i].size() <= BVH4iBuilderMorton64Bit::MORTON_LEAF_THRESHOLD)
          continue;
        
        /* remember child with largest number of items */
        if (children[i].size() > bestItems) { 
          bestItems = children[i].size();
          bestChild = i;
        }
      }
      if (bestChild == -1) break;

      /*! split best child into left and right child */
      __aligned(64) SmallBuildRecord left, right;
      if (!split(children[bestChild],left,right))
        continue;
      
      /* add new children left and right */
      left.depth = right.depth = current.depth+1;
      children[bestChild] = children[numChildren-1];
      children[numChildren-1] = left;
      children[numChildren+0] = right;
      numChildren++;
      
    } while (numChildren < BVH4i::N);

    /* create leaf node if no split is possible */
    if (unlikely(numChildren == 1)) {
      return createSmallLeaf(current);
    }

    /* allocate next four nodes and prefetch them */
    const size_t currentIndex = alloc.get(1);    

    /* init used/unused nodes */
    // const mic_f init_node = load16f((float*)BVH4i::initQBVHNode);
    // store16f_ngo((float*)&node[currentIndex+0],init_node);
    // store16f_ngo((float*)&node[currentIndex+2],init_node);

    mic_f init_lower = broadcast4to16f(&BVH4i::initQBVHNode[0]);
    mic_f init_upper = broadcast4to16f(&BVH4i::initQBVHNode[1]);

    store16f_ngo((float*)&node[currentIndex].lower,init_lower);
    store16f_ngo((float*)&node[currentIndex].upper,init_upper);

    /* recurse into each child */
    __aligned(64) BBox3fa bounds;
    bounds = empty;
    for (size_t i=0; i<numChildren; i++) 
    {
      children[i].parentNodeID = currentIndex;
      children[i].parentLocalID = i;

      if (children[i].size() <= BVH4iBuilderMorton64Bit::MORTON_LEAF_THRESHOLD)
	{
	  bounds.extend( createSmallLeaf(children[i]) );
	}
      else
	bounds.extend( recurse(children[i],alloc,mode,numThreads) );
    }

    store4f(&node[current.parentNodeID].lower[current.parentLocalID],broadcast4to16f(&bounds.lower));
    store4f(&node[current.parentNodeID].upper[current.parentLocalID],broadcast4to16f(&bounds.upper));
    createNode(node[current.parentNodeID].lower[current.parentLocalID].child,currentIndex,numChildren);

    return bounds;
  }


  BBox3fa BVH4iBuilderMorton64Bit::refit(const BVH4i::NodeRef &ref)
  {    
    if (unlikely(ref.isLeaf()))
      {
	FATAL("HERE");
	return BBox3fa( empty );
      }

    BVH4i::Node *n = (BVH4i::Node*)ref.node(node);

    BBox3fa parentBounds = empty;

    for (size_t i=0;i<BVH4i::N;i++)
      {
	if (n->child(i) == BVH4i::invalidNode) break;
	
	if (n->child(i).isLeaf())
	  {
	    parentBounds.extend( n->bounds(i) );
	    //DBG_PRINT( n->bounds(i) );
	  }
	else
	  {
	    BBox3fa bounds = refit( n->child(i) );

	    //DBG_PRINT( bounds );

	    n->setBounds(i,bounds);
	    parentBounds.extend( bounds );
	  }
      }
    return parentBounds;
  }    

  BBox3fa BVH4iBuilderMorton64Bit::refit_toplevel(const BVH4i::NodeRef &ref)
  {    
    if (unlikely(ref.isLeaf()))
      {
	FATAL("HERE");
	return BBox3fa( empty );
      }

    BVH4i::Node *n = (BVH4i::Node*)ref.node(node);

    BBox3fa parentBounds = empty;

    for (size_t i=0;i<BVH4i::N;i++)
      {
	if (n->child(i) == BVH4i::invalidNode) break;
	
	if (n->child(i).isLeaf() || n->upper[i].child == (unsigned int)-1)
	  parentBounds.extend( n->bounds(i) );
	else
	  {
	    BBox3fa bounds = refit( n->child(i) );
	    n->setBounds(i,bounds);
	    parentBounds.extend( bounds );
	  }
      }
    return parentBounds;

  }


  void BVH4iBuilderMorton64Bit::build(size_t threadIndex, size_t threadCount) 
  {
    if (unlikely(g_verbose >= 2))
      {
	std::cout << "building BVH4i with 64Bit Morton builder (MIC)... " << std::flush;
      }
    
    /* do some global inits first */
    numPrimitives = 0;       
    for (size_t i=0;i<scene->size();i++)
      {
	if (unlikely(scene->get(i) == NULL)) continue;
	if (unlikely((scene->get(i)->type != TRIANGLE_MESH))) continue;
	if (unlikely(!scene->get(i)->isEnabled())) continue;
	const TriangleMesh* __restrict__ const mesh = scene->getTriangleMesh(i);
	if (unlikely(mesh->numTimeSteps != 1)) continue;
	numGroups++;
	numPrimitives += mesh->numTriangles;
      }

    if (likely(numPrimitives == 0))
      {
	DBG(std::cout << "EMPTY SCENE BUILD" << std::endl);
	bvh->root = BVH4i::invalidNode;
	bvh->bounds = empty;
	bvh->qbvh = NULL;
	bvh->accel = NULL;
	return;
      }

    /* allocate memory arrays */
    allocateData(TaskScheduler::getNumThreads());

#if defined(PROFILE)
    size_t numTotalPrimitives = numPrimitives;
    std::cout << "STARTING PROFILE MODE" << std::endl << std::flush;
    std::cout << "primitives = " << numTotalPrimitives << std::endl;

    double dt_min = pos_inf;
    double dt_avg = 0.0f;
    double dt_max = neg_inf;
    size_t iterations = PROFILE_ITERATIONS;
    for (size_t i=0; i<iterations; i++) 
    {
      TaskScheduler::executeTask(threadIndex,threadCount,_build_parallel_morton64,this,TaskScheduler::getNumThreads(),"build_parallel_morton");

      dt_min = min(dt_min,dt);
      dt_avg = dt_avg + dt;
      dt_max = max(dt_max,dt);
    }
    dt_avg /= double(iterations);

    std::cout << "[DONE]" << std::endl;
    std::cout << "  min = " << 1000.0f*dt_min << "ms (" << numTotalPrimitives/dt_min*1E-6 << " Mtris/s)" << std::endl;
    std::cout << "  avg = " << 1000.0f*dt_avg << "ms (" << numTotalPrimitives/dt_avg*1E-6 << " Mtris/s)" << std::endl;
    std::cout << "  max = " << 1000.0f*dt_max << "ms (" << numTotalPrimitives/dt_max*1E-6 << " Mtris/s)" << std::endl;
    std::cout << BVH4iStatistics<BVH4i::Node>(bvh).str();

#else
    DBG(DBG_PRINT(numPrimitives));


    if (likely(numPrimitives > SINGLE_THREADED_BUILD_THRESHOLD && TaskScheduler::getNumThreads() > 1))
      {
#if DEBUG
	DBG_PRINT( TaskScheduler::getNumThreads() );
	std::cout << "PARALLEL BUILD" << std::endl << std::flush;
#endif
	TaskScheduler::executeTask(threadIndex,threadCount,_build_parallel_morton64,this,TaskScheduler::getNumThreads(),"build_parallel");
      }
    else
      {
	/* number of primitives is small, just use single threaded mode */
#if DEBUG
	std::cout << "SERIAL BUILD" << std::endl << std::flush;
#endif
	build_parallel_morton64(0,1,0,0,NULL);
      }

    if (g_verbose >= 2) {
      double perf = numPrimitives/dt*1E-6;
      std::cout << "[DONE] " << 1000.0f*dt << "ms (" << perf << " Mtris/s), primitives " << numPrimitives << std::endl;
      std::cout << BVH4iStatistics<BVH4i::Node>(bvh).str();
    }
#endif
    
  }

    
  // =======================================================================================================
  // =======================================================================================================
  // =======================================================================================================

  void BVH4iBuilderMorton64Bit::initThreadState(const size_t threadID, const size_t numThreads)
  {
    const size_t numBlocks = (numPrimitives+NUM_MORTON_IDS_PER_BLOCK-1) / NUM_MORTON_IDS_PER_BLOCK;
    const size_t startID   =      ((threadID+0)*numBlocks/numThreads) * NUM_MORTON_IDS_PER_BLOCK;
    const size_t endID     = min( ((threadID+1)*numBlocks/numThreads) * NUM_MORTON_IDS_PER_BLOCK ,numPrimitives) ;
    
    assert(startID % NUM_MORTON_IDS_PER_BLOCK == 0);

    /* find first group containing startID */
    size_t group = 0, skipped = 0;
    for (; group<numGroups; group++) 
    {       
      if (unlikely(scene->get(group) == NULL)) continue;
      if (scene->get(group)->type != TRIANGLE_MESH) continue;
      const TriangleMesh* __restrict__ const mesh = scene->getTriangleMesh(group);
      if (unlikely(!mesh->isEnabled())) continue;
      if (unlikely(mesh->numTimeSteps != 1)) continue;

      const size_t numTriangles = mesh->numTriangles;	
      if (skipped + numTriangles > startID) break;
      skipped += numTriangles;
    }

    /* store start group and offset */
    thread_startGroup[threadID] = group;
    thread_startGroupOffset[threadID] = startID - skipped;
  }


  void BVH4iBuilderMorton64Bit::computeBounds(const size_t threadID, const size_t numThreads) 
  {
    const size_t numBlocks = (numPrimitives+NUM_MORTON_IDS_PER_BLOCK-1) / NUM_MORTON_IDS_PER_BLOCK;
    const size_t startID   =      ((threadID+0)*numBlocks/numThreads) * NUM_MORTON_IDS_PER_BLOCK;
    const size_t endID     = min( ((threadID+1)*numBlocks/numThreads) * NUM_MORTON_IDS_PER_BLOCK ,numPrimitives) ;
    assert(startID % NUM_MORTON_IDS_PER_BLOCK == 0);

    __aligned(64) Centroid_Scene_AABB bounds;
    bounds.reset();

    size_t currentID = startID;

    size_t startGroup = thread_startGroup[threadID];
    size_t offset = thread_startGroupOffset[threadID];

    mic_f bounds_centroid_min((float)pos_inf);
    mic_f bounds_centroid_max((float)neg_inf);

    for (size_t group = startGroup; group<numGroups; group++) 
    {       
      if (unlikely(scene->get(group) == NULL)) continue;
      if (unlikely(scene->get(group)->type != TRIANGLE_MESH)) continue;
      const TriangleMesh* __restrict__ const mesh = scene->getTriangleMesh(group);
      if (unlikely(!mesh->isEnabled())) continue;
      if (unlikely(mesh->numTimeSteps != 1)) continue;


      const char* __restrict__ cptr_tri = (char*)&mesh->triangle(offset);
      const unsigned int stride = mesh->triangles.getBufferStride();
      
      for (size_t i=offset; i<mesh->numTriangles && currentID < endID; i++, currentID++,cptr_tri+=stride)	 
	{
	  const TriangleMesh::Triangle& tri = *(TriangleMesh::Triangle*)cptr_tri;
	  prefetch<PFHINT_L1>(&tri + L1_PREFETCH_ITEMS);
	  prefetch<PFHINT_L2>(&tri + L2_PREFETCH_ITEMS);

	  const mic3f v = mesh->getTriangleVertices<PFHINT_L2>(tri);
	  const mic_f bmin  = min(min(v[0],v[1]),v[2]);
	  const mic_f bmax  = max(max(v[0],v[1]),v[2]);

	  const mic_f centroid2 = bmin+bmax;
	  bounds_centroid_min = min(bounds_centroid_min,centroid2);
	  bounds_centroid_max = max(bounds_centroid_max,centroid2);
	}
      
      if (unlikely(currentID == endID)) break;
      offset = 0;
    }

    store4f(&bounds.centroid2.lower,bounds_centroid_min);
    store4f(&bounds.centroid2.upper,bounds_centroid_max);
    
    global_bounds.extend_centroid_bounds_atomic(bounds); 
  }

  void BVH4iBuilderMorton64Bit::computeMortonCodes(const size_t threadID, const size_t numThreads)
  {
    const size_t numBlocks = (numPrimitives+NUM_MORTON_IDS_PER_BLOCK-1) / NUM_MORTON_IDS_PER_BLOCK;
    const size_t startID   =      ((threadID+0)*numBlocks/numThreads) * NUM_MORTON_IDS_PER_BLOCK;
    const size_t endID     = min( ((threadID+1)*numBlocks/numThreads) * NUM_MORTON_IDS_PER_BLOCK ,numPrimitives) ;
    assert(startID % NUM_MORTON_IDS_PER_BLOCK == 0);

    /* store the morton codes in 'morton' memory */
    MortonID64Bit* __restrict__ dest = morton + startID; 

    /* compute mapping from world space into 3D grid */
    const mic_f base     = broadcast4to16f((float*)&global_bounds.centroid2.lower);
    const mic_f diagonal = 
      broadcast4to16f((float*)&global_bounds.centroid2.upper) - 
      broadcast4to16f((float*)&global_bounds.centroid2.lower);
    const mic_f scale    = select(diagonal != 0, rcp(diagonal) * mic_f(LATTICE_SIZE_PER_DIM * 0.99f),mic_f(0.0f));

    size_t currentID = startID;
    size_t offset = thread_startGroupOffset[threadID];

    size_t slot = 0;

    __aligned(64) MortonID64Bit local[4];

    for (size_t group = thread_startGroup[threadID]; group<numGroups; group++) 
    {       
      if (unlikely(scene->get(group) == NULL)) continue;
      if (unlikely(scene->get(group)->type != TRIANGLE_MESH)) continue;
      const TriangleMesh* const mesh = scene->getTriangleMesh(group);
      if (unlikely(!mesh->isEnabled())) continue;
      if (unlikely(mesh->numTimeSteps != 1)) continue;

      const size_t numTriangles = min(mesh->numTriangles-offset,endID-currentID);
       
      const char* __restrict__ cptr_tri = (char*)&mesh->triangle(offset);
      const unsigned int stride = mesh->triangles.getBufferStride();
      
      for (size_t i=0; i<numTriangles; i++,cptr_tri+=stride)	  
      {
	//prefetch<PFHINT_NTEX>(dest);

	const TriangleMesh::Triangle& tri = *(TriangleMesh::Triangle*)cptr_tri;

	prefetch<PFHINT_NT>(&tri + 16);

	const mic3f v = mesh->getTriangleVertices<PFHINT_L2>(tri);
	const mic_f bmin  = min(min(v[0],v[1]),v[2]);
	const mic_f bmax  = max(max(v[0],v[1]),v[2]);

	const mic_f cent  = bmin+bmax;
	const mic_i binID = convert_uint32((cent-base)*scale);

	// dest->primID  = offset+i;
	// dest->groupID = group;

	local[slot].primID  = offset+i;
	local[slot].groupID = group;

	const unsigned int binIDx = binID[0];
	const unsigned int binIDy = binID[1];
	const unsigned int binIDz = binID[2];

	const size_t code  = bitInterleave64_LUT(binIDx,binIDy,binIDz); 
	local[slot].code   = code;
	slot++;

	if (unlikely(slot == NUM_MORTON_IDS_PER_BLOCK))
	  {
	    mic_i m64 = load16i((int*)local);
	    assert((size_t)dest % 64 == 0);
	    store16i_ngo(dest,m64);	    
	    slot = 0;
	    dest += NUM_MORTON_IDS_PER_BLOCK;
	  }

	// dest->code = code;
	// dest++;
	// prefetch<PFHINT_L2EX>(dest + 4*4);
        currentID++;
      }

      offset = 0;
      if (currentID == endID) break;
    }

    if (unlikely(slot != 0))
      {
	mic_i m64 = load16i((int*)local);
	assert((size_t)dest % 64 == 0);
	store16i_ngo(dest,m64);	    
      }

    //DBG_PRINT(__bsr(global_code));
  }
  
  void BVH4iBuilderMorton64Bit::radixsort(const size_t threadID, const size_t numThreads)
  {
    const size_t numBlocks = (numPrimitives+NUM_MORTON_IDS_PER_BLOCK-1) / NUM_MORTON_IDS_PER_BLOCK;
    const size_t startID   = ((threadID+0)*numBlocks/numThreads) * NUM_MORTON_IDS_PER_BLOCK;
    const size_t endID     = ((threadID+1)*numBlocks/numThreads) * NUM_MORTON_IDS_PER_BLOCK;
    assert(startID % NUM_MORTON_IDS_PER_BLOCK == 0);
    assert(endID % NUM_MORTON_IDS_PER_BLOCK == 0);

    assert(((numThreads)*numBlocks/numThreads) * NUM_MORTON_IDS_PER_BLOCK == ((numPrimitives+3)&(-4)));

    MortonID64Bit* __restrict__ mortonID[2];
    mortonID[0] = (MortonID64Bit*) morton; 
    mortonID[1] = (MortonID64Bit*) node;


    /* we need 4 iterations to process all 32 bits */
    for (size_t b=0; b<8; b++)
    {
      const MortonID64Bit* __restrict__ const src = (MortonID64Bit*)mortonID[((b+0)%2)];
      MortonID64Bit*       __restrict__ const dst = (MortonID64Bit*)mortonID[((b+1)%2)];

      __assume_aligned(&radixCount[threadID][0],64);
      
      /* count how many items go into the buckets */

#pragma unroll(16)
      for (size_t i=0; i<16; i++)
	store16i(&radixCount[threadID][i*16],mic_i::zero());


      for (size_t i=startID; i<endID; i+=NUM_MORTON_IDS_PER_BLOCK) {
	prefetch<PFHINT_NT>(&src[i+L1_PREFETCH_ITEMS]);
	prefetch<PFHINT_L2>(&src[i+L2_PREFETCH_ITEMS]);
	
#pragma unroll(NUM_MORTON_IDS_PER_BLOCK)
	for (unsigned long j=0;j<NUM_MORTON_IDS_PER_BLOCK;j++)
	  {
	    const unsigned int index = src[i+j].getByte(b);
	    radixCount[threadID][index]++;
	  }
      }

      LockStepTaskScheduler::syncThreads(threadID,numThreads);


      /* calculate total number of items for each bucket */


      mic_i count[16];
#pragma unroll(16)
      for (size_t i=0; i<16; i++)
	count[i] = mic_i::zero();


      for (size_t i=0; i<threadID; i++)
#pragma unroll(16)
	for (size_t j=0; j<16; j++)
	  count[j] += load16i((int*)&radixCount[i][j*16]);
      
      __aligned(64) unsigned int inner_offset[RADIX_BUCKETS];

#pragma unroll(16)
      for (size_t i=0; i<16; i++)
	store16i(&inner_offset[i*16],count[i]);

#pragma unroll(16)
      for (size_t i=0; i<16; i++)
	count[i] = load16i((int*)&inner_offset[i*16]);

      for (size_t i=threadID; i<numThreads; i++)
#pragma unroll(16)
	for (size_t j=0; j<16; j++)
	  count[j] += load16i((int*)&radixCount[i][j*16]);	  

     __aligned(64) unsigned int total[RADIX_BUCKETS];

#pragma unroll(16)
      for (size_t i=0; i<16; i++)
	store16i(&total[i*16],count[i]);

      __aligned(64) unsigned int offset[RADIX_BUCKETS];

      /* calculate start offset of each bucket */
      offset[0] = 0;
      for (size_t i=1; i<RADIX_BUCKETS; i++)    
        offset[i] = offset[i-1] + total[i-1];
      
      /* calculate start offset of each bucket for this thread */

#pragma unroll(RADIX_BUCKETS)
	for (size_t j=0; j<RADIX_BUCKETS; j++)
          offset[j] += inner_offset[j];

      /* copy items into their buckets */
      for (size_t i=startID; i<endID; i+=NUM_MORTON_IDS_PER_BLOCK) {
	prefetch<PFHINT_NT>(&src[i+L1_PREFETCH_ITEMS]);
	prefetch<PFHINT_L2>(&src[i+L2_PREFETCH_ITEMS]);

#pragma nounroll
	for (unsigned long j=0;j<NUM_MORTON_IDS_PER_BLOCK;j++)
	  {
	    const unsigned int index = src[i+j].getByte(b);
	    assert(index < RADIX_BUCKETS);
	    dst[offset[index]] = src[i+j];
	    prefetch<PFHINT_L2EX>(&dst[offset[index]+L1_PREFETCH_ITEMS]);
	    offset[index]++;
	  }
	evictL2(&src[i]);
      }

      if (b<7) LockStepTaskScheduler::syncThreads(threadID,numThreads);

    }
  }


  void BVH4iBuilderMorton64Bit::build_main (const size_t threadIndex, const size_t threadCount)
  { 
    DBG(PING);
    TIMER(std::cout << std::endl);
    TIMER(double msec = 0.0);

    /* compute scene bounds */
    TIMER(msec = getSeconds());
    global_bounds.reset();
    LockStepTaskScheduler::dispatchTask( task_computeBounds, this, threadIndex, threadCount );
    TIMER(msec = getSeconds()-msec);    
    TIMER(std::cout << "task_computeBounds " << 1000. * msec << " ms" << std::endl << std::flush);
    TIMER(DBG_PRINT(global_bounds));

    /* compute morton codes */
    TIMER(msec = getSeconds());
    LockStepTaskScheduler::dispatchTask( task_computeMortonCodes, this, threadIndex, threadCount );   

    /* padding */
    MortonID64Bit* __restrict__ const dest = (MortonID64Bit*)morton;
    
    for (size_t i=numPrimitives; i<((numPrimitives+3)&(-4)); i++) {
      dest[i].code    = (size_t)-1; 
      dest[i].groupID = 0;
      dest[i].primID  = 0;
    }

    TIMER(msec = getSeconds()-msec);    
    TIMER(std::cout << "task_computeMortonCodes " << 1000. * msec << " ms" << std::endl << std::flush);

 
    /* sort morton codes */
    TIMER(msec = getSeconds());
    LockStepTaskScheduler::dispatchTask( task_radixsort, this, threadIndex, threadCount );

#if defined(DEBUG)
    for (size_t i=1; i<((numPrimitives+3)&(-4)); i++)
      assert(morton[i-1].code <= morton[i].code);

    for (size_t i=numPrimitives; i<((numPrimitives+3)&(-4)); i++) {
      assert(dest[i].code  == (size_t)-1); 
      assert(dest[i].groupID == 0);
      assert(dest[i].primID == 0);
    }

#endif	    

    TIMER(msec = getSeconds()-msec);    
    TIMER(std::cout << "task_radixsort " << 1000. * msec << " ms" << std::endl << std::flush);

    TIMER(msec = getSeconds());

    /* build and extract top-level tree */
    numBuildRecords = 0;
    atomicID.reset(1);
    topLevelItemThreshold = max((numPrimitives + threadCount-1)/((threadCount)),(size_t)64);

    SmallBuildRecord br;
    br.init(0,numPrimitives);
    br.parentNodeID = 0;
    br.parentLocalID = 0;
    br.depth = 1;

    NodeAllocator alloc(atomicID,numAllocatedNodes);
    recurse(br,alloc,RECURSE,0);

    numNodes = atomicID >> 2;

    DBG_PRINT(numNodes);

    TIMER(msec = getSeconds());

    /* refit toplevel part of tree */
    BBox3fa rootBounds = refit(node->child(0));

    DBG_PRINT(rootBounds);

    TIMER(msec = getSeconds()-msec);    
    TIMER(std::cout << "refit top level " << 1000. * msec << " ms" << std::endl << std::flush);


    bvh->root   = node->child(0); 
    bvh->bounds = rootBounds;

    DBG_PRINT( bvh->root );
    DBG_PRINT( bvh->bounds );

    std::cout << "BUILD DONE"  << std::endl;

    for (size_t r=0;r<4;r++)
      BVH4iRotate::rotate(bvh,bvh->root);

    std::cout << "TREE ROTATION DONE"  << std::endl;

  }

  void BVH4iBuilderMorton64Bit::build_parallel_morton64(size_t threadIndex, size_t threadCount, size_t taskIndex, size_t taskCount, TaskScheduler::Event* event) 
  {
    TIMER(double msec = 0.0);

    /* initialize thread state */
    initThreadState(threadIndex,threadCount);
    
    /* let all thread except for control thread wait for work */
    if (threadIndex != 0) {
      LockStepTaskScheduler::dispatchTaskMainLoop(threadIndex,threadCount);
      return;
    }

    /* start measurement */
    double t0 = 0.0f;

#if !defined(PROFILE)
    if (g_verbose >= 2) 
#endif
      t0 = getSeconds();

    /* performs build of tree */
    build_main(threadIndex,threadCount);

    /* end task */
    LockStepTaskScheduler::releaseThreads(threadCount);
    
    /* stop measurement */
#if !defined(PROFILE)
    if (g_verbose >= 2) 
#endif
      dt = getSeconds()-t0;

  }
}


