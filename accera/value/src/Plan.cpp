////////////////////////////////////////////////////////////////////////////////////////////////////
//  Copyright (c) Microsoft Corporation. All rights reserved.
//  Licensed under the MIT License. See LICENSE in the project root for license information.
//  Authors: Mason Remy
////////////////////////////////////////////////////////////////////////////////////////////////////

#include "Plan.h"
#include "Cache.h"
#include "MLIREmitterContext.h"
#include "Schedule.h"

#include <ir/include/exec/ExecutionPlanAttributes.h>
#include <ir/include/exec/ExecutionPlanOps.h>
#include <ir/include/exec/ParallelizationInfo.h>
#include <ir/include/exec/VectorizationInfo.h>
#include <ir/include/nest/LoopNestOps.h>
#include <ir/include/value/ValueDialect.h>

#include <utilities/include/Exception.h>

#include <mlir/IR/Attributes.h>
#include <mlir/IR/Identifier.h>

#include <cassert>
#include <functional>
#include <tuple>
#include <utility>
#include <vector>

using namespace accera::ir::value;
using namespace accera::ir::loopnest;
using namespace accera::ir::executionPlan;

namespace accera
{
using namespace utilities;

namespace value
{
    // Implementation class
    class PlanImpl
    {
    public:
        PlanImpl(value::ExecutionOptions execOptions, ScheduleOp scheduleOp) :
            _scheduleOp(scheduleOp),
            _execOptions(execOptions)
        {
            std::visit(
                [this](auto options) {
                    // TODO: formalize setting exec target by using an interface
                    auto nestOp = mlir::dyn_cast<NestOp>(_scheduleOp->getParentOp());
                    _execPlanOp = _scheduleOp.getOrCreateExecPlan();

                    mlir::OpBuilder b(nestOp);
                    if constexpr (std::is_same_v<decltype(options), targets::CPU>)
                    {
                        auto execTargetAttr = ir::value::ExecutionTargetAttr::get(b.getContext(), ir::value::ExecutionTarget::CPU);
                        nestOp.exec_targetAttr(execTargetAttr);
                        _execPlanOp.exec_targetAttr(execTargetAttr);
                    }
                    else if constexpr (std::is_same_v<decltype(options), targets::GPU>)
                    {
                        auto execTargetAttr = ir::value::ExecutionTargetAttr::get(b.getContext(), ir::value::ExecutionTarget::GPU);
                        nestOp.exec_targetAttr(execTargetAttr);
                        _execPlanOp.exec_targetAttr(execTargetAttr);

                        _execPlanOp->setAttr(
                            _execPlanOp.getGPULaunchAttrName(),
                            b.getIndexArrayAttr({
                                options.grid.x,
                                options.grid.y,
                                options.grid.z,
                                options.block.x,
                                options.block.y,
                                options.block.z,
                            }));
                    }
                    else
                        llvm_unreachable("Unexpected");
                },
                execOptions);
        }

        Cache AddAutomaticCache(ViewAdapter target, const std::optional<ScalarIndex>& keySliceIndex, const std::optional<int64_t>& maxElements, CacheIndexing mapping, CacheAllocation allocation, MemorySpace memorySpace)
        {
            return { _scheduleOp, target, keySliceIndex, maxElements, mapping, allocation, memorySpace, _execOptions };
        }

        Cache AddManualCache(std::variant<ViewAdapter, Cache*> target, const std::optional<ScalarIndex>& keySliceIndex, const std::optional<ScalarIndex>& triggerIndex, const std::optional<int64_t>& maxElements, CacheIndexing mapping, CacheAllocation allocation, MemorySpace memorySpace, const MemoryAffineCoefficients& memoryMap)
        {
            return { _scheduleOp, target, keySliceIndex, triggerIndex, maxElements, memoryMap, mapping, allocation, memorySpace, _execOptions };
        }

        Cache AddManualCache(std::variant<ViewAdapter, Cache*> target, const std::optional<ScalarIndex>& keySliceIndex, const std::optional<ScalarIndex>& triggerIndex, const std::optional<int64_t>& maxElements, CacheIndexing mapping, CacheAllocation allocation, MemorySpace memorySpace, const DimensionOrder& dimOrder)
        {
            return { _scheduleOp, target, keySliceIndex, triggerIndex, maxElements, dimOrder, mapping, allocation, memorySpace, _execOptions };
        }

        Cache AddRuntimeInitCache(ViewAdapter target, const std::string& packingFnName, const std::string& packedBufferSizeFnName, CacheIndexing indexing)
        {
            return { _scheduleOp, target, packingFnName, packedBufferSizeFnName, indexing };
        }

        Cache PackAndEmbedBuffer(ViewAdapter target, ViewAdapter constantData, const std::string& wrapperFnName, const std::string& packedBufferName, CacheIndexing indexing)
        {
            return { _scheduleOp, target, constantData, wrapperFnName, packedBufferName, indexing };
        }

        void Vectorize(ScalarIndex i, const VectorizationInformation& dslVectorizationInfo)
        {
            auto& builder = GetBuilder();
            auto symbolicIndexOp = GetIndexOp(i);
            auto index = symbolicIndexOp.getValue();

            VectorizationInfo vectorizationInfo{ dslVectorizationInfo.vectorBytes, dslVectorizationInfo.vectorUnitCount, dslVectorizationInfo.unrollOnly };
            auto vectorizationInfoIdentifier = builder.getIdentifier(VectorizationInfoAttr::getKeyName());
            auto vectorizationInfoAttr = VectorizationInfoAttr::get(vectorizationInfo, builder.getContext());
            _scheduleOp.addLoopAttribute(index, vectorizationInfoIdentifier, vectorizationInfoAttr);

            // tag the ExecPlanOp with this vectorization info so that other cache ops
            // can extract this info from the loopnest graph later
            _execPlanOp->setAttr(vectorizationInfoIdentifier, vectorizationInfoAttr);
        }

        void Parallelize(std::vector<ScalarIndex> indices, int64_t numThreads, ParallelizationPolicy policy)
        {
            auto& builder = GetBuilder();

            ParallelizationInfo parallelizationInfo{ numThreads, policy == ParallelizationPolicy::Dynamic };
            auto parallelizationInfoIdentifier = builder.getIdentifier(ParallelizationInfoAttr::getKeyName());
            auto parallelizationInfoAttr = ParallelizationInfoAttr::get(parallelizationInfo, builder.getContext());

            // mark each index as parallelized
            // during lowering, indices are continguous in the schedule ordering will be collapsed
            for (auto& i : indices)
            {
                auto symbolicIndexOp = GetIndexOp(i);
                auto index = symbolicIndexOp.getValue();
                _scheduleOp.addLoopAttribute(index, parallelizationInfoIdentifier, parallelizationInfoAttr);
            }
        }

        void MapIndexToProcessor(ScalarIndex i, Processor proc)
        {
            auto& builder = GetBuilder();
            auto symbolicIndexOp = GetIndexOp(i);
            auto index = symbolicIndexOp.getValue();

            auto planOp = _scheduleOp.getOrCreateExecPlan();
            auto procStr = ir::value::stringifyEnum(proc);
            auto procMapAttrName = planOp.getGPUProcessorMapAttrName();
            auto procMap = planOp->getAttrOfType<mlir::DictionaryAttr>(procMapAttrName);
            if (!procMap)
            {
                procMap = builder.getDictionaryAttr(
                    { { builder.getIdentifier(procStr), IndexAttr::get(index, builder.getContext()) } });
                planOp->setAttr(procMapAttrName, procMap);
            }
            else
            {
                auto mapArray = procMap.getValue().vec();
                mapArray.emplace_back(
                    builder.getIdentifier(procStr), IndexAttr::get(index, builder.getContext()));
                planOp->setAttr(procMapAttrName, builder.getDictionaryAttr(mapArray));
            }
        }

    private:
        mlir::OpBuilder& GetBuilder()
        {
            return ::accera::value::GetMLIRContext().GetOpBuilder();
        }

        // TODO : de-dupe with ScheduleImpl
        SymbolicIndexOp GetIndexOp(ScalarIndex val)
        {
            auto mlirValue = mlir::Value::getFromOpaquePointer(val.GetValue().Get<Emittable>().GetDataAs<MLIRContext::EmittableInfo*>()->data);
            auto op = mlirValue.getDefiningOp();
            assert(op);

            auto indexOp = llvm::dyn_cast_or_null<SymbolicIndexOp>(op);
            assert(indexOp);
            return indexOp;
        }

        ScheduleOp _scheduleOp;
        ExecPlanOp _execPlanOp;
        value::ExecutionOptions _execOptions;
    };

    //
    // Plan impl
    //

    Plan::Plan(Plan&& other) noexcept :
        _impl(std::move(other._impl))
    {}

    Plan& Plan::operator=(Plan&& other) noexcept
    {
        if (this != &other)
        {
            using std::swap;
            swap(_impl, other._impl);
        }
        return *this;
    }

    Plan::Plan(Schedule& schedule) :
        _impl(std::make_unique<PlanImpl>(value::targets::CPU{}, schedule.GetOp()))
    {}

    Plan::~Plan() = default;

    Cache Plan::AddCache(std::variant<ViewAdapter, Cache*> target, const ScalarIndex& outermostIncludedSplitIndex, const ScalarIndex& triggerIndex, const MemoryAffineCoefficients& memoryMap, CacheIndexing mapping, CacheAllocation allocation, MemorySpace memorySpace)
    {
        return _impl->AddManualCache(target, outermostIncludedSplitIndex, triggerIndex, std::nullopt, mapping, allocation, memorySpace, memoryMap);
    }

    Cache Plan::AddCache(std::variant<ViewAdapter, Cache*> target, const ScalarIndex& outermostIncludedSplitIndex, const ScalarIndex& triggerIndex, const DimensionOrder& dimOrder, CacheIndexing mapping, CacheAllocation allocation, MemorySpace memorySpace)
    {
        return _impl->AddManualCache(target, outermostIncludedSplitIndex, triggerIndex, std::nullopt, mapping, allocation, memorySpace, dimOrder);
    }

    Cache Plan::AddCache(std::variant<ViewAdapter, Cache*> target, int64_t maxElements, const MemoryAffineCoefficients& memoryMap, CacheIndexing mapping, CacheAllocation allocation, MemorySpace memorySpace)
    {
        return _impl->AddManualCache(target, std::nullopt, std::nullopt, maxElements, mapping, allocation, memorySpace, memoryMap);
    }

    Cache Plan::AddCache(std::variant<ViewAdapter, Cache*> target, int64_t maxElements, const DimensionOrder& dimOrder, CacheIndexing mapping, CacheAllocation allocation, MemorySpace memorySpace)
    {
        return _impl->AddManualCache(target, std::nullopt, std::nullopt, maxElements, mapping, allocation, memorySpace, dimOrder);
    }

    Cache Plan::AddCache(std::variant<ViewAdapter, Cache*> target, const ScalarIndex& outermostIncludedSplitIndex, CacheIndexing mapping, CacheAllocation allocation, MemorySpace memorySpace)
    {
        Value baseValue;
        if (std::holds_alternative<Cache*>(target))
        {
            auto cache = std::get<Cache*>(target);
            baseValue = cache->GetBaseValue();
        }
        else
        {
            auto viewAdapter = std::get<ViewAdapter>(target);
            baseValue = viewAdapter.GetValue();
        }
        int64_t rank = baseValue.GetLayout().NumDimensions();
        DimensionOrder dimOrder(rank);
        return _impl->AddManualCache(target, outermostIncludedSplitIndex, outermostIncludedSplitIndex, std::nullopt, mapping, allocation, memorySpace, dimOrder);
    }

    Cache Plan::AddCache(std::variant<ViewAdapter, Cache*> target, int64_t maxElements, CacheIndexing mapping, CacheAllocation allocation, MemorySpace memorySpace)
    {
        Value baseValue;
        if (std::holds_alternative<Cache*>(target))
        {
            auto cache = std::get<Cache*>(target);
            baseValue = cache->GetBaseValue();
        }
        else
        {
            auto viewAdapter = std::get<ViewAdapter>(target);
            baseValue = viewAdapter.GetValue();
        }
        int64_t rank = baseValue.GetLayout().NumDimensions();
        DimensionOrder dimOrder(rank);
        auto viewAdapter = std::get<ViewAdapter>(target);
        return _impl->AddManualCache(target, std::nullopt, std::nullopt, maxElements, mapping, allocation, memorySpace, dimOrder);
    }

    Cache Plan::EmitRuntimeInitPacking(ViewAdapter target, const std::string& packingFnName, const std::string& packedBufferSizeFnName, CacheIndexing indexing)
    {
        return _impl->AddRuntimeInitCache(target, packingFnName, packedBufferSizeFnName, indexing);
    }

    Cache Plan::PackAndEmbedBuffer(ViewAdapter target, ViewAdapter constantData, const std::string& wrapperFnName, const std::string& packedBufferName, CacheIndexing indexing)
    {
        return _impl->PackAndEmbedBuffer(target, constantData, wrapperFnName, packedBufferName, indexing);
    }

    void Plan::Vectorize(ScalarIndex i, const VectorizationInformation& vectorizationInfo)
    {
        _impl->Vectorize(i, vectorizationInfo);
    }

    void Plan::Parallelize(std::vector<ScalarIndex> indices, int64_t numThreads, ParallelizationPolicy policy)
    {
        _impl->Parallelize(indices, numThreads, policy);
    }

    //
    // GPUPlan impl
    //
    GPUPlan::GPUPlan(GPUPlan&& other) noexcept :
        _impl(std::move(other._impl))
    {}

    GPUPlan& GPUPlan::operator=(GPUPlan&& other) noexcept
    {
        if (this != &other)
        {
            using std::swap;
            swap(_impl, other._impl);
        }
        return *this;
    }

    GPUPlan::~GPUPlan() = default;

    GPUPlan::GPUPlan(value::targets::GPU gpuOptions, Schedule& sched) :
        _impl(std::make_unique<PlanImpl>(gpuOptions, sched.GetOp()))
    {
    }

    Cache GPUPlan::AddCache(ViewAdapter target, const ScalarIndex& outermostIncludedSplitIndex, MemorySpace memorySpace)
    {
        return _impl->AddAutomaticCache(target, outermostIncludedSplitIndex, std::nullopt, CacheIndexing::GlobalToPhysical, CacheAllocation::Automatic, memorySpace);
    }

    Cache GPUPlan::AddCache(ViewAdapter target, int64_t maxElements, MemorySpace memorySpace)
    {
        return _impl->AddAutomaticCache(target, std::nullopt, maxElements, CacheIndexing::GlobalToPhysical, CacheAllocation::Automatic, memorySpace);
    }

    void GPUPlan::MapIndexToProcessor(ScalarIndex index, Processor proc)
    {
        _impl->MapIndexToProcessor(index, proc);
    }
} // namespace value
} // namespace accera
