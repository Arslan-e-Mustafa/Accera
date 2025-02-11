////////////////////////////////////////////////////////////////////////////////////////////////////
//  Copyright (c) Microsoft Corporation. All rights reserved.
//  Licensed under the MIT License. See LICENSE in the project root for license information.
////////////////////////////////////////////////////////////////////////////////////////////////////

#ifndef EXECUTIONPLAN_OPS
#define EXECUTIONPLAN_OPS

include "ir/include/Common.td"
include "ir/include/nest/LoopNestExportedInterfaces.td"
include "ir/include/nest/LoopNestAttrs.td"
include "ir/include/value/ValueAttrs.td"
include "ir/include/exec/ExecutionPlanAttrs.td"
include "ir/include/exec/ExecutionPlanInterfaces.td"

// Provide a definition of the dialect in the ODS framework so that we can define our operations.
def ExecutionPlan_Dialect : Dialect {
  let name = "accxp";
  let description = [{The ExecutionPlan (`rxp`) dialect
  }];
  let cppNamespace = "accera::ir::executionPlan";
}

//
// Attributes
//

def accxp_VectorizationInfoAttr : DialectAttr<
    ExecutionPlan_Dialect,
    CPred<"$_self.isa<VectorizationInfoAttr>()">,
    "Vectorization info attribute"> {
  let storageType = "VectorizationInfoAttr";
  let returnType = "VectorizationInfoAttr";
  let convertFromStorage = "$_self";
  let constBuilderCall = "$0";
}

def accxp_InPlaceUnrollInfoAttr : DialectAttr<
    ExecutionPlan_Dialect,
    CPred<"$_self.isa<InPlaceUnrollInfoAttr>()">,
    "In-place unroll info attribute"> {
  let storageType = "InPlaceUnrollInfoAttr";
  let returnType = "InPlaceUnrollInfoAttr";
  let convertFromStorage = "$_self";
  let constBuilderCall = "$0";
}

def accxp_ParallelizationInfoAttr : DialectAttr<
    ExecutionPlan_Dialect,
    CPred<"$_self.isa<ParallelizationInfoAttr>()">,
    "Parallelization info attribute"> {
  let storageType = "ParallelizationInfoAttr";
  let returnType = "ParallelizationInfoAttr";
  let convertFromStorage = "$_self";
  let constBuilderCall = "$0";
}

def accxp_TensorizationInfoAttr : DialectAttr<
    ExecutionPlan_Dialect,
    CPred<"$_self.isa<TensorizationInfoAttr>()">,
    "Parallelization info attribute"> {
  let storageType = "TensorizationInfoAttr";
  let returnType = "TensorizationInfoAttr";
  let convertFromStorage = "$_self";
  let constBuilderCall = "$0";
}

// Base class for executionPlan dialect operations. This operation inherits from the base
// `Op` class in OpBase.td, and provides:
//   * The parent dialect of the operation.
//   * The mnemonic for the operation, or the name without the dialect prefix.
//   * A list of traits for the operation.
class accxp_Op<string mnemonic, list<OpTrait> traits = []> :
    Op<ExecutionPlan_Dialect, mnemonic, traits>;

//
// ExecutionPlan Operations
//


//
// TerminatorOp
//
def accxp_TerminatorOp : accxp_Op<"terminator", [Terminator]> {
  let summary = "cf terminator operation";
  let description = [{
    "accxp.terminator" is a terminator operation for blocks used in ExecutionPlan op regions.
  }];
}

//
// MakeCacheOp
//
def accxp_MakeCacheOp : accxp_Op<"make_cache"> {
  let summary = "Operation to infer and allocate a cache shape and viewing maps";
  let description = [{
    The "accxp.make_cache" operation lowers to an allocated cache in global memory.
    If given an offsetArrayToCacheAccessMap, multiCacheAccessIndices, and offsetAccessIndices,
    then applying the offsetArrayToCacheAccessMap to the
    list of { multiCacheAccessIndices, offset access indices IVs..., array access indices... } will access the
    corresponding position in the cache.
  }];
  let arguments = (ins MemorySpaceAttr:$memorySpace,
                   AffineMapAttr:$offsetArrayToCacheAccessMap,
                   ArrayAttr:$offsetAccessIndices,
                   ArrayAttr:$multiCacheAccessIndices);
  let results = (outs AnyMemRef:$cache);
  let builders = [
    OpBuilder<(ins
      "mlir::MemRefType":$cache,
      "MemorySpace":$memorySpace)>,
    OpBuilder<(ins
      "mlir::MemRefType":$cache,
      "MemorySpace":$memorySpace,
      "AffineMap":$offsetArrayToCacheAccessMap,
      "const std::vector<Index>&":$offsetAccessIndices,
      "const std::vector<Index>&":$multiCacheAccessIndices
    )>,
  ];

  let extraClassDeclaration = [{
    mlir::AffineValueMap insertCachePosition(const std::vector<mlir::Value>& multiCacheIndexIterationCounters, const std::vector<mlir::Value>& offsetAccessIVs, const std::vector<mlir::Value>& baseArrayIndices);
    mlir::AffineValueMap insertCachePosition(mlir::Operation* where, const std::vector<mlir::Value>& baseArrayIndices, const std::vector<std::pair<loopnest::Index, mlir::Value>>& unrealizedLoopNestIndices);
    mlir::AffineValueMap insertCachePosition(mlir::Block* where, const std::vector<mlir::Value>& baseArrayIndices, const std::vector<std::pair<loopnest::Index, mlir::Value>>& unrealizedLoopNestIndices);
    std::vector<mlir::Value> getBaseArrayPosition(mlir::AffineReadOpInterface loadOp);
    std::vector<mlir::Value> getBaseArrayPosition(mlir::AffineWriteOpInterface storeOp);
  }];
}

//
// ActiveElementCacheCopyOp
//
def accxp_ActiveElementCacheCopyOp : accxp_Op<"active_element_cache_copy", []> {
  let summary = "memory reshaping and cache data copying operation";
  let description = [{
    The "accxp.active_element_cache_copy" operation describes a memory shape for a cached piece of data and produces code to copy to or from the cache.
  }];

  let arguments = (ins  AnyMemRef:$src,
                        AnyMemRef:$dst,
                        Variadic<Index>:$externalRelevantIndices,
                        ArrayAttr:$cacheRegionRelevantIndexRanges,
                        ArrayAttr:$cacheRegionBaseIndices,
                        AffineMapAttr:$relevantIndicesToSrcMap,
                        AffineMapAttr:$relevantIndicesToDstMap);

  let builders = [
    OpBuilder<(ins
      "Value":$src,
      "Value":$dst,
      "ValueRange":$externalRelevantIndices,
      "const std::vector<IndexRange>&":$cacheRegionRelevantIndexRanges,
      "const std::vector<std::vector<Index>>&":$cacheRegionBaseIndices,
      "AffineMap":$relevantIndicesToSrcMap,
      "AffineMap":$relevantIndicesToDstMap
    )>,
    OpBuilder<(ins
      "Value":$src,
      "CacheAccessContext":$dstContext
    )>,
    OpBuilder<(ins
      "CacheAccessContext":$srcContext,
      "Value":$dst
    )>
  ];
}

//
// ActiveBlockCacheCopyOp
//
def accxp_ActiveBlockCacheCopyOp : accxp_Op<"active_block_cache_copy", [AttrSizedOperandSegments]> {
  let summary = "memory reshaping and cache data copying operation";
  let description = [{
    The "accxp.active_block_cache_copy" operation describes a memory shape for a cached piece of data and produces code to copy to or from the cache.
  }];

  let arguments = (ins  AnyMemRef:$array,
                        AnyMemRef:$cache,
                        Variadic<Index>:$lbOperands,
                        Variadic<Index>:$ubOperands,
                        Variadic<Index>:$multiCacheSliceOperands,
                        ArrayAttr:$lbMaps,
                        ArrayAttr:$ubMaps,
                        AffineMapAttr:$activeBlockToCacheMap,
                        UnitAttr:$toCache,
                        StrAttr:$activeBlockTag,
                        UnitAttr:$thrifty,
                        UnitAttr:$skipBarriers, // TODO : remove this once barrier analysis hoists barriers out of loops
                        OptionalAttr<accxp_VectorizationInfoAttr>:$vectorizationInfo);
}

//
// MultiCacheCopyOp
//
def accxp_MultiCacheCopyOp : accxp_Op<"multi_cache_copy"> {
  let summary = "memory reshaping and cache data copying operation";
  let description = [{
    The "accxp.multi_cache_copy" operation describes a memory shape for a cached piece of data and produces code to copy to or from the multicache.
  }];

  // Needs the args for the internal CacheCopyOp as well as the multicache slicing
  let arguments = (ins  AnyMemRef:$array,
                        AnyMemRef:$cache,
                        Variadic<Index>:$multiCacheExternalSymbols,
                        ArrayAttr:$multiCacheLoopLowerBoundMaps,
                        ArrayAttr:$multiCacheLoopUpperBoundMaps,
                        ArrayAttr:$multiCacheLoopStepSizes,
                        ArrayAttr:$multiCacheLoopIndexIds,
                        ArrayAttr:$activeBlockLowerBoundMaps,
                        ArrayAttr:$activeBlockUpperBoundMaps,
                        AffineMapAttr:$externalSymbolsPermutationMap,
                        AffineMapAttr:$activeBlockToCacheMap,
                        StrAttr:$activeBlockTag,
                        UnitAttr:$thrifty,
                        UnitAttr:$toCache,
                        OptionalAttr<accxp_VectorizationInfoAttr>:$vectorizationInfo);
}

//
// CacheZeroOp
//
def accxp_CacheZeroOp : accxp_Op<"cache_zero"> {
  let summary = "cache zeroing operation";
  let description = [{
    The "accxp.cacheZeroOp" operation zeros out a cache
  }];

  let arguments = (ins AnyMemRef:$cache,
                       StrAttr:$activeBlockTag,
                       UnitAttr:$thrifty);
}

//
// ActiveElementCacheReduceOp
//
def accxp_ActiveElementCacheReduceOp : accxp_Op<"active_element_cache_reduce", [AttrSizedOperandSegments]> {
  let summary = "memory reshaping and cache reducing operation";
  let description = [{
    The "accxp.active_element_cache_reduce" operation describes a memory shape for a cached piece of data and produces code to copy from the cache back to the output.
  }];

  let arguments = (ins  AnyMemRef:$srcCache,
                        AnyMemRef:$dst,
                        Variadic<Index>:$externalRelevantIndices,
                        Variadic<AnyType>:$scaleValues,
                        ArrayAttr:$cacheRegionRelevantIndexRanges,
                        ArrayAttr:$cacheRegionBaseIndices,
                        AffineMapAttr:$relevantIndicesToSrcCacheMap,
                        AffineMapAttr:$relevantIndicesToDstMap);

  let builders = [
    OpBuilder<(ins
      "Value":$src,
      "Value":$dst,
      "ValueRange":$externalRelevantIndices,
      "const std::vector<IndexRange>&":$cacheRegionRelevantIndexRanges,
      "const std::vector<std::vector<Index>>&":$cacheRegionBaseIndices,
      "AffineMap":$relevantIndicesToSrcCacheMap,
      "AffineMap":$relevantIndicesToDstMap,
      "ValueRange":$scaleValues
    )>,
    OpBuilder<(ins
      "Value":$src,
      "CacheAccessContext":$dstContext
    )>,
    OpBuilder<(ins
      "CacheAccessContext":$srcContext,
      "Value":$dst
    )>,
  ];
}

//
// ActiveBlockCacheReduceOp
//
def accxp_ActiveBlockCacheReduceOp : accxp_Op<"active_block_cache_reduce", [AttrSizedOperandSegments]> {
  let summary = "memory reshaping and cache reducing operation";
  let description = [{
    The "accxp.active_block_cache_reduce" operation describes a memory shape for a cached piece of data and produces code to copy from the cache back to the output.
  }];

  let arguments = (ins  AnyMemRef:$array,
                        AnyMemRef:$cache,
                        Variadic<Index>:$lbOperands,
                        Variadic<Index>:$ubOperands,
                        ArrayAttr:$lbMaps,
                        ArrayAttr:$ubMaps,
                        AffineMapAttr:$activeBlockToCacheMap,
                        Variadic<AnyType>:$scaleValues,
                        StrAttr:$activeBlockTag,
                        UnitAttr:$thrifty,
                        OptionalAttr<accxp_VectorizationInfoAttr>:$vectorizationInfo);
}

//
// BeginCacheMappingOp
//
def accxp_BeginCacheMappingOp : accxp_Op<"begin_cache_mapping",
  [AttrSizedOperandSegments,
  DeclareOpInterfaceMethods<accxp_BeginCacheRegionOpInterface>]> {
  let summary = "Maps input replacement value to cache value and replaces registered operations as appropriate";
  let description = [{
    The "accxp.begin_cache_mapping" operation marks the start of the subgraph graph to
    replace input usage and operations on the input with cache usage.
    It is an error for there to not be a symmetric accxp.end_cache_mapping op in the same block as accxp.begin_cache_mapping
  }];

  let arguments = (ins  AnyMemRef:$fromValue,
                        AnyMemRef:$baseCacheValue,
                        AnyMemRef:$baseInput,
                        AnyMemRef:$toValue,
                        Variadic<Index>:$fullRelevantIndices,
                        Variadic<Index>:$externalRelevantIndices,
                        ArrayAttr:$cacheRegionRelevantIndexRanges,
                        ArrayAttr:$cacheRegionBaseIndices,
                        DictionaryAttr:$toValueAccessMaps,
                        I64Attr:$id,
                        UnitAttr:$activeBlockCache);

  let results = (outs Index:$resultId);

  let skipDefaultBuilders = 1;
  let builders = [
    OpBuilder<(ins
        "Value":$fromValue,
        "Value":$baseCacheValue,
        "Value":$baseInput,
        "CacheAccessContext":$toValueContext,
        "int64_t":$id,
        "bool":$activeBlockCache)>
  ];

  let extraClassDeclaration = [{
    CacheAccessContext getToValueAccessContext();
  }];
}

//
// EndCacheMappingOp
//
def accxp_EndCacheMappingOp : accxp_Op<"end_cache_mapping",
  [DeclareOpInterfaceMethods<accxp_EndCacheRegionOpInterface>]> {
  let summary = "Denotes the end of the graph section to replace mappings for";
  let description = [{
    The "accxp.end_cache_mapping" operation marks the end the of the subgraph to
    replace input usage and operations on the input with cache usage.
    It is an error for there to not be a symmetric accxp.begin_cache_mapping op in the same block as accxp.end_cache_mapping
  }];

  let arguments = (ins  Index:$mappingId);
}

//
// BeginCacheRegionOp
//
def accxp_BeginCacheRegionOp : accxp_Op<"begin_cache_region",
  [AttrSizedOperandSegments,
   DeclareOpInterfaceMethods<accln_InjectableMappingOpInterface>,
   DeclareOpInterfaceMethods<accxp_BeginCacheRegionOpInterface>]> {
  let summary = "Denotes the beginning of the subgraph where a cache is active. Lowers to the appropriate cache data moving ops and cache mapping ops";
  let description = [{
    The "accxp.begin_cache_region" operation marks the beginning of the subraph where the cache is active.
    It is an error for there not to be a symmetric accxp.end_cache_region op in the same block as accxp.begin_cache_region
  }];

  // Requires most of arguments for CacheCopyOp, CacheReduceOp, CacheZeroOp, and CacheMappingOp since it will lower to
  // different subsets of those ops
  let arguments = (ins  AnyMemRef:$input,
                        AnyMemRef:$cache,
                        AnyMemRef:$baseInput,
                        Variadic<Index>:$fullRelevantIndices,
                        Variadic<Index>:$externalRelevantIndices,
                        ArrayAttr:$cacheRegionRelevantIndexRanges,
                        ArrayAttr:$cacheRegionBaseIndices,
                        DictionaryAttr:$cacheAccessMaps,
                        accln_IndexAttr:$triggerIndex,
                        accln_IndexAttr:$cacheIndex,
                        I64Attr:$id,
                        I64Attr:$cacheHierarchyLevel,
                        UnitAttr:$activeBlockCache,
                        UnitAttr:$dimReorderCache,
                        UnitAttr:$thrifty,
                        UnitAttr:$doubleBufferCache,
                        OptionalAttr<MemorySpaceAttr>:$doubleBufferMemorySpace,
                        OptionalAttr<accxp_VectorizationInfoAttr>:$vectorizationInfo);

  let results = (outs Index:$resultId);

  let skipDefaultBuilders = 1;
  let builders = [
    OpBuilder<(ins
      "Value":$inputValue,
      "CacheAccessContext":$cacheAccessContext,
      "Value":$baseInput,
      "loopnest::Index":$triggerIndex,
      "loopnest::Index":$cacheIndex,
      "int64_t":$id,
      "int64_t":$cacheHierarchyLevel,
      "bool":$activeBlockCache,
      "bool":$dimReorderCache,
      "bool":$thrifty,
      "bool":$doubleBufferCache,
      "MemorySpace":$doubleBufferMemorySpace,
      "const VectorizationInfo&":$vectorizationInfo)>
  ];

  let extraClassDeclaration = [{
    CacheAccessContext getInputAccessContext();
    CacheAccessContext getCacheAccessContext();
  }];
}

//
// EndCacheRegionOp
//
def accxp_EndCacheRegionOp : accxp_Op<"end_cache_region",
  [DeclareOpInterfaceMethods<accxp_EndCacheRegionOpInterface>]> {
  let summary = "Denotes the end of the graph section the cache is active for";
  let description = [{
    The "accxp.end_cache_region" operation marks the end the of the subgraph to
    replace input usage and operations on the input with cache usage.
    It is an error for there to not be a symmetric accxp.begin_cache_region op in the same block as accxp.end_cache_region
  }];

  let arguments = (ins  Index:$regionId);
}

//
// BeginMaxElementCacheRegionOp
//
def accxp_BeginMaxElementCacheRegionOp : accxp_Op<"begin_max_element_cache_region",
  [DeclareOpInterfaceMethods<accln_InjectableMappingOpInterface>,
   DeclareOpInterfaceMethods<accxp_BeginCacheRegionOpInterface>]> {
  let summary = "Denotes the beginning of the subgraph where a max element cache is active. Lowers to a begin_cache_region op at the appropriate level for the element budget";
  let description = [{
    The "accxp.begin_max_element_cache_region" operation marks the beginning of the subraph where the max element cache is active.
    It is an error for there not to be a symmetric accxp.end_cache_region op in the same block as accxp.begin_max_element_cache_region
  }];

  // Requires most of arguments for CacheCopyOp, CacheReduceOp, CacheZeroOp, and CacheMappingOp since it will lower to
  // different subsets of those ops
  let arguments = (ins  AnyMemRef:$input,
                        AnyMemRef:$cache,
                        AnyMemRef:$baseInput,
                        DictionaryAttr:$cacheAccessMaps,
                        accln_IndexAttr:$innermostLoopNestIndex, // The max element cache is initially positioned around the innermost loop index, but will be hoisted out as part of lowering
                        I64Attr:$id,
                        I64Attr:$cacheHierarchyLevel,
                        I64Attr:$maxElements,
                        UnitAttr:$dimReorderCache,
                        UnitAttr:$thrifty,
                        UnitAttr:$doubleBufferCache,
                        OptionalAttr<MemorySpaceAttr>:$doubleBufferMemorySpace,
                        OptionalAttr<accxp_VectorizationInfoAttr>:$vectorizationInfo);

  let results = (outs Index:$resultId);

  let skipDefaultBuilders = 1;
  let builders = [
    OpBuilder<(ins
      "Value":$input,
      "Value":$cache,
      "Value":$baseInput,
      "CacheAccessMaps":$accessMaps,
      "int64_t":$maxElements,
      "loopnest::Index":$innermostLoopNestIndex,
      "int64_t":$id,
      "int64_t":$cacheHierarchyLevel,
      "bool":$dimReorderCache,
      "bool":$thrifty,
      "bool":$doubleBufferCache,
      "MemorySpace":$doubleBufferMemorySpace,
      "const VectorizationInfo&":$vectorizationInfo)>
  ];
}

//
// DelayedMappingRegionOp
//
def accxp_DelayedMappingRegionOp : accxp_Op<"delayed_mapping_region_op", [SingleBlockImplicitTerminator<"TerminatorOp">]> {
  let summary = "Holds a mapping from one value to another to be applied to all the operations within its region";
  let description = [{
    The "accxp.delayed_mapping_region_op" operation will map one value to another for all the ops in its region.
    This op exists as a way to replace one value with another in ops that haven't been fully expanded to consume
    the "from" SSA value yet, as is sometimes the case with accesses into cache memrefs.
  }];

  let arguments = (ins  AnyType:$from,
                        AnyType:$to);

  let regions = (region AnyRegion:$region);

  let skipDefaultBuilders = 1;
  let builders = [
    OpBuilder<(ins
        "Value":$from,
        "Value":$to)>
  ];
  
  let extraClassDeclaration = [{
    mlir::OpBuilder getBodyBuilder() {
      return mlir::OpBuilder(&region().front(), std::prev(region().front().end()));
    }
  }];
}

#endif // EXECUTIONPLAN_OPS
