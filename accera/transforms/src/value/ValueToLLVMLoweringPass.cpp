////////////////////////////////////////////////////////////////////////////////////////////////////
//  Copyright (c) Microsoft Corporation. All rights reserved.
//  Licensed under the MIT License. See LICENSE in the project root for license information.
//  Authors: Kern Handa
////////////////////////////////////////////////////////////////////////////////////////////////////

#include "AcceraPasses.h"

#include <ir/include/IRUtil.h>
#include <ir/include/value/ValueDialect.h>
#include <mlir/Dialect/LLVMIR/LLVMTypes.h>
#include <mlir/IR/BuiltinTypes.h>
#include <mlir/IR/Types.h>
#include <transforms/include/util/SnapshotUtilities.h>
#include <value/include/Debugging.h>
#include <value/include/MLIREmitterContext.h>

#include <mlir/Conversion/AffineToStandard/AffineToStandard.h>
#include <mlir/Conversion/LLVMCommon/ConversionTarget.h>
#include <mlir/Conversion/LLVMCommon/MemRefBuilder.h>
#include <mlir/Conversion/LLVMCommon/Pattern.h>
#include <mlir/Conversion/LLVMCommon/TypeConverter.h>
#include <mlir/Conversion/LinalgToLLVM/LinalgToLLVM.h>
#include <mlir/Conversion/MemRefToLLVM/MemRefToLLVM.h>
#include <mlir/Conversion/SCFToStandard/SCFToStandard.h>
#include <mlir/Conversion/StandardToLLVM/ConvertStandardToLLVM.h>
#include <mlir/Conversion/StandardToLLVM/ConvertStandardToLLVMPass.h>
#include <mlir/Conversion/VectorToLLVM/ConvertVectorToLLVM.h>

#include <mlir/Dialect/Affine/IR/AffineOps.h>
#include <mlir/Dialect/LLVMIR/FunctionCallUtils.h>
#include <mlir/Dialect/LLVMIR/LLVMDialect.h>
#include <mlir/Dialect/Linalg/IR/LinalgOps.h>
#include <mlir/Dialect/SCF/SCF.h>
#include <mlir/Dialect/StandardOps/IR/Ops.h>
#include <mlir/Dialect/Vector/VectorOps.h>
#include <mlir/Dialect/Vector/VectorTransforms.h>

#include <mlir/Pass/Pass.h>
#include <mlir/Pass/PassManager.h>

#include <mlir/Transforms/GreedyPatternRewriteDriver.h>
#include <mlir/Transforms/Passes.h>

#include <llvm/IR/DataLayout.h>
#include <llvm/Support/raw_os_ostream.h>

#include <iostream>

#ifndef _MSC_VER
#include <time.h>
#endif

using namespace mlir;

using namespace accera::ir;
using namespace accera::ir::value;
using namespace accera::transforms;
using namespace accera::transforms::value;

namespace
{
static FlatSymbolRefAttr getOrInsertLibraryFunction(PatternRewriter& rewriter,
                                                    std::string libraryFunctionName,
                                                    mlir::Type llvmFnType,
                                                    ModuleOp module,
                                                    LLVM::LLVMDialect* /*llvmDialect*/)
{
    auto* context = module.getContext();
    if (module.lookupSymbol<LLVM::LLVMFuncOp>(libraryFunctionName))
        return SymbolRefAttr::get(context, libraryFunctionName);

    // Insert the function into the body of the parent module.
    PatternRewriter::InsertionGuard insertGuard(rewriter);
    rewriter.setInsertionPointToStart(module.getBody());
    rewriter.create<LLVM::LLVMFuncOp>(module.getLoc(), libraryFunctionName, llvmFnType);
    return SymbolRefAttr::get(context, libraryFunctionName);
}

template <typename ConcreteType>
class PrintOpLoweringBase : public OpConversionPattern<ConcreteType>
{
public:
    using OpConversionPattern<ConcreteType>::OpConversionPattern;

    // Return a symbol reference to the printf function, inserting it into the
    // module if necessary.
    static FlatSymbolRefAttr getOrInsertPrintFunction(PatternRewriter& rewriter,
                                                      ModuleOp module,
                                                      LLVM::LLVMDialect* llvmDialect)
    {
        auto* context = module.getContext();
        return getOrInsertLibraryFunction(rewriter, "printf", getPrintfType(context), module, llvmDialect);
    }

    static FlatSymbolRefAttr getOrInsertPrintErrorFunction(PatternRewriter& rewriter,
                                                           ModuleOp module,
                                                           LLVM::LLVMDialect* llvmDialect)
    {
        auto* context = module.getContext();
        return getOrInsertLibraryFunction(rewriter, accera::ir::GetPrintErrorFunctionName(), getPrintfType(context), module, llvmDialect);
    }

    static mlir::Type getPrintfType(mlir::MLIRContext* context)
    {
        // Create a function type for printf, the signature is:
        //   * `i32 (i8*, ...)`
        auto llvmI8PtrTy = LLVM::LLVMPointerType::get(IntegerType::get(context, 8));
        return LLVM::LLVMFunctionType::get(getPrintFnReturnType(context), llvmI8PtrTy, /*isVarArg=*/true);
    }

    static mlir::Type getPrintFnReturnType(mlir::MLIRContext* context)
    {
        return IntegerType::get(context, 32);
    }

    // Return a value representing an access into a global string with the given
    // name, creating the string if necessary.
    static mlir::Value getOrCreateGlobalString(Location loc, OpBuilder& builder, StringRef name, StringRef value, ModuleOp module)
    {
        // Create the global at the entry of the module.
        LLVM::GlobalOp global;
        auto context = builder.getContext();
        auto i8Ty = IntegerType::get(context, 8);
        if (!(global = module.lookupSymbol<LLVM::GlobalOp>(name)))
        {
            OpBuilder::InsertionGuard insertGuard(builder);
            builder.setInsertionPointToStart(module.getBody());
            auto type = LLVM::LLVMArrayType::get(i8Ty, value.size());
            global = builder.create<LLVM::GlobalOp>(loc, type, /*isConstant=*/true, LLVM::Linkage::Internal, name, builder.getStringAttr(value));
        }

        // Get the pointer to the first character in the global string.
        Value globalPtr = builder.create<LLVM::AddressOfOp>(loc, global);
        auto i64Ty = IntegerType::get(context, 64);
        Value cst0 = builder.create<LLVM::ConstantOp>(loc, i64Ty, builder.getIntegerAttr(builder.getIndexType(), 0));
        return builder.create<LLVM::GEPOp>(loc, LLVM::LLVMPointerType::get(i8Ty), globalPtr, ArrayRef<Value>({ cst0, cst0 }));
    }

    static mlir::Value getOrCreateGlobalArray(Location loc, OpBuilder& builder, StringRef name, Type elementType, size_t size, ModuleOp module, LLVM::LLVMDialect* llvmDialect)
    {
        LLVMTypeConverter llvmTypeConverter(builder.getContext());
        auto llvmElementType = llvmTypeConverter.convertType(elementType).dyn_cast<mlir::Type>();

        // Create the global at the entry of the module.
        LLVM::GlobalOp global;
        if (!(global = module.lookupSymbol<LLVM::GlobalOp>(name)))
        {
            OpBuilder::InsertionGuard insertGuard(builder);
            builder.setInsertionPointToStart(module.getBody());
            auto type = LLVM::LLVMArrayType::get(llvmElementType, size);
            mlir::Attribute valueAttr;
            global = builder.create<LLVM::GlobalOp>(loc, type, /*isConstant=*/false, LLVM::Linkage::Internal, name, valueAttr);
        }

        // Get the pointer to the first entry in the global array.
        Value globalPtr = builder.create<LLVM::AddressOfOp>(loc, global);
        Value cst0 = builder.create<LLVM::ConstantOp>(loc, IntegerType::get(builder.getContext(), 64), builder.getIntegerAttr(builder.getIndexType(), 0));
        return builder.create<LLVM::GEPOp>(loc, LLVM::LLVMPointerType::get(llvmElementType), globalPtr, ArrayRef<Value>({ cst0, cst0 }));
    }
};

class PrintFOpLowering : public PrintOpLoweringBase<PrintFOp>
{
public:
    using PrintOpLoweringBase<PrintFOp>::PrintOpLoweringBase;

    LogicalResult matchAndRewrite(PrintFOp op,
                                  ArrayRef<Value> operands,
                                  ConversionPatternRewriter& rewriter) const override;
};

template <typename OpTy>
struct ValueLLVMOpConversionPattern : public OpConversionPattern<OpTy>
{
public:
    explicit ValueLLVMOpConversionPattern(LLVMTypeConverter& typeConverter, mlir::MLIRContext* context, PatternBenefit benefit = 1) :
        OpConversionPattern<OpTy>(context, benefit), llvmTypeConverter(typeConverter) {}

protected:
    LLVMTypeConverter& llvmTypeConverter;
};

using ValueCallOp = accera::ir::value::CallOp;
struct CallOpLowering : public ValueLLVMOpConversionPattern<ValueCallOp>
{
    using ValueLLVMOpConversionPattern::ValueLLVMOpConversionPattern;

    LogicalResult matchAndRewrite(
        ValueCallOp op,
        ArrayRef<mlir::Value> operands,
        ConversionPatternRewriter& rewriter) const override;
};

struct BitcastOpLowering : public OpConversionPattern<BitcastOp>
{
    using OpConversionPattern<BitcastOp>::OpConversionPattern;

    LogicalResult matchAndRewrite(
        BitcastOp op,
        ArrayRef<mlir::Value> operands,
        ConversionPatternRewriter& rewriter) const override;
};

struct GlobalOpToLLVMLowering : public ValueLLVMOpConversionPattern<GlobalOp>
{
    using ValueLLVMOpConversionPattern::ValueLLVMOpConversionPattern;

    LogicalResult matchAndRewrite(
        GlobalOp op,
        ArrayRef<mlir::Value> operands,
        ConversionPatternRewriter& rewriter) const override;
};

struct ReferenceGlobalOpLowering : public ValueLLVMOpConversionPattern<ReferenceGlobalOp>
{
    using ValueLLVMOpConversionPattern::ValueLLVMOpConversionPattern;

    LogicalResult matchAndRewrite(
        ReferenceGlobalOp op,
        ArrayRef<mlir::Value> operands,
        ConversionPatternRewriter& rewriter) const override;
};

struct CPUEarlyReturnRewritePattern : ValueLLVMOpConversionPattern<EarlyReturnOp>
{
    using ValueLLVMOpConversionPattern::ValueLLVMOpConversionPattern;
    LogicalResult matchAndRewrite(EarlyReturnOp op, ArrayRef<Value> operands, ConversionPatternRewriter& rewriter) const final
    {
        if (auto target = util::ResolveExecutionTarget(op); !target || *target != ExecutionTarget::CPU)
        {
            return failure();
        }

        // Get the block the op belongs to
        Block* currentBlock = rewriter.getBlock();
        // Get an iterator pointing to one after the op
        auto position = ++Block::iterator(op);

        // Split the block at this point, so that the early_return op is the last op
        // in the original block and everything after is moved to a new block
        // we don't care about the new block, since we were asked to return early
        // TODO kerha: figure out cleanup semantics
        (void)rewriter.splitBlock(currentBlock, position);

        rewriter.replaceOpWithNewOp<mlir::ReturnOp>(op, operands);
        return success();
    }
};

struct GetTimeOpLowering : public ValueLLVMOpConversionPattern<GetTimeOp>
{
    using ValueLLVMOpConversionPattern::ValueLLVMOpConversionPattern;

    LogicalResult matchAndRewrite(
        GetTimeOp op,
        ArrayRef<mlir::Value> operands,
        ConversionPatternRewriter& rewriter) const override;

    mlir::Value GetTime(ConversionPatternRewriter& rewriter, mlir::Location loc, ModuleOp& parentModule) const;

    static FlatSymbolRefAttr getOrInsertQueryPerfFrequency(PatternRewriter& rewriter,
                                                           ModuleOp module,
                                                           LLVM::LLVMDialect* llvmDialect)
    {
        auto* context = module.getContext();
        auto llvmFnType = getQueryPerformanceFrequencyFunctionType(context);
        return getOrInsertLibraryFunction(rewriter, "QueryPerformanceFrequency", llvmFnType, module, llvmDialect);
    }

    static FlatSymbolRefAttr getOrInsertQueryPerfCounter(PatternRewriter& rewriter,
                                                         ModuleOp module,
                                                         LLVM::LLVMDialect* llvmDialect)
    {
        auto* context = module.getContext();
        auto llvmFnType = getQueryPerformanceCounterFunctionType(context);
        return getOrInsertLibraryFunction(rewriter, "QueryPerformanceCounter", llvmFnType, module, llvmDialect);
    }

    static FlatSymbolRefAttr getOrInsertClockGetTime(PatternRewriter& rewriter,
                                                     ModuleOp module,
                                                     LLVM::LLVMDialect* llvmDialect)
    {
        auto* context = module.getContext();
        auto llvmFnType = getGetTimeFunctionType(context);
        return getOrInsertLibraryFunction(rewriter, "clock_gettime", llvmFnType, module, llvmDialect);
    }

    static Type getQueryPerformanceFrequencyFunctionType(mlir::MLIRContext* context)
    {
        // BOOL QueryPerformanceFrequency(LARGE_INTEGER *lpFrequency); // LARGE_INTEGER is a signed 64-bit int
        auto boolTy = IntegerType::get(context, 8);
        auto argTy = LLVM::LLVMPointerType::get(getPerformanceCounterType(context));
        return LLVM::LLVMFunctionType::get(boolTy, { argTy }, /*isVarArg=*/false);
    }

    static Type getQueryPerformanceCounterFunctionType(mlir::MLIRContext* context)
    {
        // BOOL QueryPerformanceCounter(LARGE_INTEGER *lpPerformanceCount); // LARGE_INTEGER is a signed 64-bit int
        auto boolTy = IntegerType::get(context, 8);
        auto argTy = LLVM::LLVMPointerType::get(getPerformanceCounterType(context));
        return LLVM::LLVMFunctionType::get(boolTy, { argTy }, /*isVarArg=*/false);
    }

    static Type getGetTimeFunctionType(mlir::MLIRContext* context)
    {
        // Create a function type for clock_gettime, the signature is:
        //        int clock_gettime(clockid_t clockid, struct timespec *tp);
        auto returnTy = getIntType(context);
        auto llvmClockIdTy = getClockIdType(context);
        auto llvmTimespecTy = getTimeSpecType(context);
        auto llvmTimespecPtrTy = LLVM::LLVMPointerType::get(llvmTimespecTy);
        return LLVM::LLVMFunctionType::get(returnTy, { llvmClockIdTy, llvmTimespecPtrTy }, /*isVarArg=*/false);
    }

    static Type getPerformanceCounterType(mlir::MLIRContext* context)
    {
        return IntegerType::get(context, 64);
    }

    static Type getClockIdType(mlir::MLIRContext* context)
    {
        return getIntType(context);
    }

    static Type getTimeSpecType(mlir::MLIRContext* context)
    {
        //    struct timespec {
        //        time_t   tv_sec;        /* seconds */
        //        long     tv_nsec;       /* nanoseconds */
        //    };
        auto llvmIntTy = getIntType(context);
        auto llvmTimespecTy = LLVM::LLVMStructType::getLiteral(context, { llvmIntTy, llvmIntTy }, /* isPacked */ true);
        return llvmTimespecTy;
    }

    static Type getIntType(mlir::MLIRContext* context)
    {
        auto llvmI32Ty = IntegerType::get(context, 32);
        auto llvmI64Ty = IntegerType::get(context, 64);
        const int hostBitSize = 64; // TODO:: FIXME :: This assumes that the host is always 64bit
            // Should query the target hardware
        auto llvmIntTy = hostBitSize == 32 ? llvmI32Ty : llvmI64Ty;
        return llvmIntTy;
    }
};
struct ValueToLLVMLoweringPass : public ConvertValueToLLVMBase<ValueToLLVMLoweringPass>
{
    ValueToLLVMLoweringPass(bool useBarePtrCallConv, bool emitCWrappers, unsigned indexBitwidth, bool useAlignedAlloc, llvm::DataLayout dataLayout, const IntraPassSnapshotOptions& snapshotteroptions = {}) :
        _intrapassSnapshotter(snapshotteroptions)
    {
        this->useBarePtrCallConv = useBarePtrCallConv;
        this->emitCWrappers = emitCWrappers;
        this->indexBitwidth = indexBitwidth;
        // TODO: move to mlir::LowerToLLVMOptions::AllocLowering
        this->useAlignedAlloc = useAlignedAlloc;
        this->dataLayout = dataLayout.getStringRepresentation();
    }

    void runOnModule() final;

private:
    IRSnapshotter _intrapassSnapshotter;
};

// Creates a constant Op producing a value of `resultType` from an index-typed
// integer attribute.
Value createIndexAttrConstant(OpBuilder& builder, Location loc, Type resultType, int64_t value)
{
    return builder.create<LLVM::ConstantOp>(
        loc, resultType, builder.getIntegerAttr(builder.getIndexType(), value));
}

// Create an LLVM IR pseudo-operation defining the given index constant.
Value createIndexConstant(LLVMTypeConverter& converter, ConversionPatternRewriter& builder, Location loc, uint64_t value)
{
    return createIndexAttrConstant(builder, loc, converter.convertType(builder.getIndexType()), value);
}

struct LLVMCallFixupPattern : OpRewritePattern<LLVM::CallOp>
{
    using OpRewritePattern::OpRewritePattern;

    LogicalResult match(LLVM::CallOp op) const final
    {
        auto optionalCallee = op.callee();
        if (!optionalCallee) return failure();

        auto callee = *optionalCallee;
        auto funcOp = mlir::SymbolTable::lookupNearestSymbolFrom<LLVM::LLVMFuncOp>(op->getParentOfType<ModuleOp>(), callee);
        if (!funcOp) return failure();

        auto numCallArgs = op.getNumOperands();
        auto numFuncArgs = funcOp.getNumArguments();

        return success((numFuncArgs == 0 && numCallArgs == 0) ||
                       (numCallArgs / numFuncArgs == 5));
    }

    void rewrite(LLVM::CallOp callOp, mlir::PatternRewriter& rewriter) const final
    {
        rewriter.updateRootInPlace(callOp, [&] {
            mlir::Operation* op = callOp;
            llvm::SmallVector<mlir::Value, 4> newOperands;
            for (unsigned idx = 1, e = op->getNumOperands(); idx < e; idx += 5)
            {
                newOperands.push_back(op->getOperand(idx));
            }
            op->setOperands(newOperands);
        });
    }
};

struct RawPointerAPIFnConversion : public ConvertOpToLLVMPattern<FuncOp>
{
    using ConvertOpToLLVMPattern<FuncOp>::ConvertOpToLLVMPattern;

    // cf mlir\lib\Conversion\StandardToLLVM\StandardToLLVM.cpp
    /// Only retain those attributes that are not constructed by
    /// `LLVMFuncOp::build`. If `filterArgAttrs` is set, also filter out argument
    /// attributes.
    static void filterFuncAttributes(ArrayRef<NamedAttribute> attrs,
                                     bool filterArgAttrs,
                                     SmallVectorImpl<NamedAttribute>& result)
    {
        for (const auto& attr : attrs)
        {
            if (attr.first == SymbolTable::getSymbolAttrName() ||
                attr.first == function_like_impl::getTypeAttrName() || attr.first == "std.varargs" ||
                (filterArgAttrs && attr.first == function_like_impl::getArgDictAttrName()))
                continue;
            result.push_back(attr);
        }
    }

    // cf FuncOpConversionBase in mlir\lib\Conversion\StandardToLLVM\StandardToLLVM.cpp

    // Convert input FuncOp to LLVMFuncOp by using the LLVMTypeConverter provided
    // to this legalization pattern.
    LLVM::LLVMFuncOp convertFuncOpToLLVMFuncOp(FuncOp funcOp, ConversionPatternRewriter& rewriter) const
    {
        // Convert the original function arguments. They are converted using the
        // LLVMTypeConverter provided to this legalization pattern.
        auto varargsAttr = funcOp->getAttrOfType<BoolAttr>("std.varargs");
        TypeConverter::SignatureConversion result(funcOp.getNumArguments());
        auto llvmType = getTypeConverter()->convertFunctionSignature(
            funcOp.getType(), varargsAttr && varargsAttr.getValue(), result);
        if (!llvmType)
            return nullptr;

        // Propagate argument attributes to all converted arguments obtained after
        // converting a given original argument.
        SmallVector<NamedAttribute, 4> attributes;
        filterFuncAttributes(funcOp->getAttrs(), /*filterArgAttrs=*/true, attributes);
        if (ArrayAttr argAttrDicts = funcOp.getAllArgAttrs())
        {
            SmallVector<Attribute, 4> newArgAttrs(
                llvmType.cast<LLVM::LLVMFunctionType>().getNumParams());
            for (unsigned i = 0, e = funcOp.getNumArguments(); i < e; ++i)
            {
                auto mapping = result.getInputMapping(i);
                assert(mapping.hasValue() &&
                       "unexpected deletion of function argument");
                for (size_t j = 0; j < mapping->size; ++j)
                    newArgAttrs[mapping->inputNo + j] = argAttrDicts[i];
            }
            attributes.push_back(
                rewriter.getNamedAttr(function_like_impl::getArgDictAttrName(),
                                      rewriter.getArrayAttr(newArgAttrs)));
        }

        // Create an LLVM function, use external linkage by default until MLIR
        // functions have linkage.
        auto newFuncOp = rewriter.create<LLVM::LLVMFuncOp>(
            funcOp.getLoc(), funcOp.getName(), llvmType, LLVM::Linkage::External, /*dsoLocal*/ false, attributes);

        rewriter.inlineRegionBefore(funcOp.getBody(), newFuncOp.getBody(), newFuncOp.end());
        auto beforeConversion = newFuncOp.getArguments().vec();

        if (failed(rewriter.convertRegionTypes(&newFuncOp.getBody(), *typeConverter, &result)))
            return nullptr;

        return newFuncOp;
    }

    // cf BarePtrFuncOpConversion in mlir\lib\Conversion\StandardToLLVM\StandardToLLVM.cpp
    LogicalResult matchAndRewrite(FuncOp funcOp, ArrayRef<Value> operands, ConversionPatternRewriter& rewriter) const override
    {
        if (!funcOp->getAttr(RawPointerAPIAttrName))
        {
            // Only match FuncOps with the raw pointer API attribute
            return failure();
        }

        // Store the type of memref-typed arguments before the conversion so that we
        // can promote them to MemRef descriptor at the beginning of the function.
        SmallVector<Type, 8> oldArgTypes =
            llvm::to_vector<8>(funcOp.getType().getInputs());

        auto newFuncOp = convertFuncOpToLLVMFuncOp(funcOp, rewriter);
        if (!newFuncOp)
            return failure();
        if (newFuncOp.getBody().empty())
        {
            rewriter.eraseOp(funcOp);
            return success();
        }

        // Promote bare pointers from memref arguments to memref descriptors at the
        // beginning of the function so that all the memrefs in the function have a
        // uniform representation.
        Block* entryBlock = &newFuncOp.getBody().front();
        auto blockArgs = entryBlock->getArguments();
        assert(blockArgs.size() == oldArgTypes.size() &&
               "The number of arguments and types doesn't match");

        OpBuilder::InsertionGuard guard(rewriter);
        rewriter.setInsertionPointToStart(entryBlock);
        for (auto it : llvm::zip(blockArgs, oldArgTypes))
        {
            BlockArgument arg = std::get<0>(it);
            Type argTy = std::get<1>(it);

            // Unranked memrefs are not supported in the bare pointer calling
            // convention. We should have bailed out before in the presence of
            // unranked memrefs.
            assert(!argTy.isa<UnrankedMemRefType>() &&
                   "Unranked memref is not supported");
            auto memrefTy = argTy.dyn_cast<MemRefType>();
            if (!memrefTy)
                continue;

            // Note: this diverges from the MLIR main branch implementation and avoids creating
            //       and UndefOp with a MemRef type as the unrealized cast conversion ops appear
            //       to have a bug where they do not get fully converted for those ops.
            //       Moreover, the MLIR main branch version of this claims that a placeholder undef
            //       op is required to avoid replaceUsesOfBlockArgument() causing the ops that fill out
            //       the MemRefDescriptor to themselves be replaced, however replaceUsesOfBlockArgument()
            //       already accounts for this type of scenario and doesn't perform the replacement on any
            //       ops that preceed the new op that is the old arg is being replaced with.
            Location loc = funcOp.getLoc();
            Value desc = MemRefDescriptor::fromStaticShape(
                rewriter, loc, *getTypeConverter(), memrefTy, arg);
            rewriter.replaceUsesOfBlockArgument(arg, desc);
        }

        rewriter.eraseOp(funcOp);
        return success();
    }
};

// Converts mlir::CallOps that are inside of a RawPointerAPI function and call into a non-raw-pointer-API function
struct RawPointerAPICallOpConversion : public mlir::ConvertOpToLLVMPattern<mlir::CallOp>
{
    using ConvertOpToLLVMPattern<mlir::CallOp>::ConvertOpToLLVMPattern;

    LogicalResult matchAndRewrite(mlir::CallOp op, ArrayRef<Value> operands, ConversionPatternRewriter& rewriter) const override
    {
        // Only match if this mlir::CallOp is inside of an LLVM::FuncOp with the RawPointerAPI attribute and is calling
        // a function without the RawPointerAPI attribute
        auto callOp = cast<mlir::CallOp>(op);
        auto parentFuncOp = callOp->getParentOfType<LLVM::LLVMFuncOp>();
        if (!parentFuncOp || !parentFuncOp->getAttr(RawPointerAPIAttrName))
        {
            return failure();
        }
        auto parentModule = callOp->getParentOfType<mlir::ModuleOp>();
        auto callee = parentModule.lookupSymbol(callOp.getCallee());
        if (!callee || callee->getAttr(RawPointerAPIAttrName))
        {
            return failure();
        }

        // cf CallOpInterfaceLowering in mlir\lib\Conversion\StandardToLLVM\StandardToLLVM.cpp

        // Pack the result types into a struct.
        Type packedResult = nullptr;
        unsigned numResults = callOp.getNumResults();
        auto resultTypes = llvm::to_vector<4>(callOp.getResultTypes());

        if (numResults != 0)
        {
            packedResult = getTypeConverter()->packFunctionResults(op->getResultTypes());
            if (!packedResult)
                return failure();
        }

        auto promoted = getTypeConverter()->promoteOperands(
            op->getLoc(), /*opOperands=*/op->getOperands(), operands, rewriter);
        auto newOp = rewriter.create<LLVM::CallOp>(
            callOp.getLoc(), packedResult ? TypeRange(packedResult) : TypeRange(), promoted, callOp->getAttrs());

        SmallVector<Value, 4> results;
        if (numResults < 2)
        {
            // If < 2 results, packing did not do anything and we can just return.
            results.append(newOp.result_begin(), newOp.result_end());
        }
        else
        {
            // Otherwise, it had been converted to an operation producing a structure.
            // Extract individual results from the structure and return them as list.
            results.reserve(numResults);
            for (unsigned i = 0; i < numResults; ++i)
            {
                auto type = getTypeConverter()->convertType(op->getResult(i).getType());
                results.push_back(rewriter.create<LLVM::ExtractValueOp>(
                    op->getLoc(), type, newOp.getOperation()->getResult(0), rewriter.getI64ArrayAttr(i)));
            }
        }

        if (this->getTypeConverter()->getOptions().useBarePtrCallConv)
        {
            // For the bare-ptr calling convention, promote memref results to
            // descriptors.
            assert(results.size() == resultTypes.size() &&
                   "The number of arguments and types doesn't match");
            this->getTypeConverter()->promoteBarePtrsToDescriptors(
                rewriter, callOp.getLoc(), resultTypes, results);
        }
        else if (failed(copyUnrankedDescriptors(rewriter, callOp.getLoc(), resultTypes, results, /*toDynamic=*/false)))
        {
            return failure();
        }

        rewriter.replaceOp(op, results);
        return success();
    }
};

struct RawPointerAPIUnusedUndefRemoval : public OpRewritePattern<LLVM::UndefOp>
{
    RawPointerAPIUnusedUndefRemoval(MLIRContext* context) :
        OpRewritePattern(context, 100)
    {}

    LogicalResult match(LLVM::UndefOp op) const final
    {
        auto isMemref = op.res().getType().isa<MemRefType>();
        auto hasNoUses = op->use_empty();
        return success(isMemref && hasNoUses);
    }

    void rewrite(LLVM::UndefOp op, mlir::PatternRewriter& rewriter) const final
    {
        rewriter.eraseOp(op);
    }
};

} // namespace

using namespace accera::transforms::value;

static Type getPointerIndexType(LLVMTypeConverter& typeConverter)
{
    return IntegerType::get(&typeConverter.getContext(), typeConverter.getPointerBitwidth());
}

LogicalResult GlobalOpToLLVMLowering::matchAndRewrite(
    GlobalOp op,
    ArrayRef<mlir::Value> operands,
    ConversionPatternRewriter& rewriter) const
{
    auto type = op.getType();
    assert(type && type.hasStaticShape() && "unexpected type");

    uint64_t numElements = type.getNumElements();

    auto elementType = llvmTypeConverter.convertType(type.getElementType())
                           .cast<Type>();
    LLVM::LLVMArrayType arrayType;

    if (auto denseElementsAttr = op.valueAttr().dyn_cast_or_null<DenseElementsAttr>();
        op.constant() && denseElementsAttr)
    {
        // For tensor / vector constants, the llvm type needs to be nested arrays matching the rank of the constant buffer
        auto shape = denseElementsAttr.getType().getShape().vec();
        assert(!shape.empty());
        arrayType = LLVM::LLVMArrayType::get(elementType, shape.back());
        for (size_t idx = 1; idx < shape.size(); ++idx)
        {
            // Walk from the innermost part of the shape outwards
            size_t currentIdx = shape.size() - idx;
            arrayType = LLVM::LLVMArrayType::get(arrayType, shape[currentIdx]);
        }
    }
    else
    {
        arrayType = LLVM::LLVMArrayType::get(elementType, numElements);
    }

    {
        OpBuilder::InsertionGuard guard(rewriter);

        rewriter.create<LLVM::GlobalOp>(
            op.getLoc(),
            arrayType,
            op.constant(),
            op.external() ? LLVM::Linkage::External : LLVM::Linkage::Internal,
            op.sym_name(),
            op.valueAttr());
    }
    rewriter.eraseOp(op);

    return success();
}

LogicalResult ReferenceGlobalOpLowering::matchAndRewrite(
    ReferenceGlobalOp op,
    ArrayRef<mlir::Value> operands,
    ConversionPatternRewriter& rewriter) const
{
    auto parentValueFuncOp = op->getParentOfType<ValueFuncOp>();
    auto parentFuncOp = op->getParentOfType<mlir::FuncOp>();
    auto parentLLVMFuncOp = op->getParentOfType<LLVM::LLVMFuncOp>();
    if (!parentValueFuncOp && !parentFuncOp && !parentLLVMFuncOp)
    {
        // Global constant buffers are created with a module-level ReferenceGlobalOp as a handle that can be returned
        // However, a module-level ReferenceGlobalOp is not valid in LLVM so remove it here
        rewriter.eraseOp(op);
        return success();
    }

    auto loc = op.getLoc();

    auto i32Type = IntegerType::get(&llvmTypeConverter.getContext(), 32);
    auto zero = rewriter.create<LLVM::ConstantOp>(loc, i32Type, rewriter.getI32IntegerAttr(0));

    if (auto globalOp = op.getGlobal())
    {
        auto type = op.getType();
        assert(type && type.hasStaticShape() && "unexpected type");
        auto elementType = llvmTypeConverter.convertType(type.getElementType())
                               .cast<Type>();
        auto elementPtrType = LLVM::LLVMPointerType::get(elementType);
        Value address = rewriter.create<LLVM::AddressOfOp>(loc, elementPtrType, op.global_name());
        Value memory = rewriter.create<LLVM::GEPOp>(
            loc,
            elementPtrType,
            address,
            ArrayRef<Value>{ zero, zero });

        auto memrefType = op.getType();
        auto memref = MemRefDescriptor::fromStaticShape(rewriter, loc, llvmTypeConverter, memrefType, memory);
        rewriter.replaceOp(op, { memref });

        return success();
    }

    if (auto globalOp = llvm::dyn_cast_or_null<LLVM::GlobalOp>(
            mlir::SymbolTable::lookupSymbolIn(op->getParentOfType<ModuleOp>(), op.global_name())))
    {
        Value address = rewriter.create<LLVM::AddressOfOp>(loc, globalOp);
        auto elementType = globalOp.getType().cast<LLVM::LLVMArrayType>().getElementType();
        Value memory = rewriter.create<LLVM::GEPOp>(
            loc, LLVM::LLVMPointerType::get(elementType, globalOp.addr_space()), address, ArrayRef<Value>{ zero, zero });

        auto memrefType = op.getType();
        auto memref = MemRefDescriptor::fromStaticShape(rewriter, loc, llvmTypeConverter, memrefType, memory);
        rewriter.replaceOp(op, { memref });

        return success();
    }

    return failure();
}

LogicalResult PrintFOpLowering::matchAndRewrite(PrintFOp op,
                                                ArrayRef<Value> operands,
                                                ConversionPatternRewriter& rewriter) const
{
    auto loc = op.getLoc();

    auto* llvmDialect = op.getContext()->getOrLoadDialect<LLVM::LLVMDialect>();
    assert(llvmDialect && "expected llvm dialect to be registered");

    ModuleOp parentModule = op->getParentOfType<ModuleOp>();
    PrintFOp::Adaptor operandAdapter(operands);

    std::string fmt = op.fmt_spec().str();
    auto tag = "fmt_" + std::to_string(llvm::hash_value(fmt));
    Value fmtStr = getOrCreateGlobalString(loc, rewriter, tag, StringRef(fmt.c_str(), fmt.length() + 1), parentModule);

    // The value to print
    auto inputVals = operandAdapter.input();

    std::vector<Value> args{ fmtStr };
    args.insert(args.end(), inputVals.begin(), inputVals.end());

    auto printFnRef = op.to_stderr() ? getOrInsertPrintErrorFunction(rewriter, parentModule, llvmDialect) : getOrInsertPrintFunction(rewriter, parentModule, llvmDialect);
    rewriter.create<LLVM::CallOp>(loc, std::vector<Type>{ getPrintFnReturnType(rewriter.getContext()) }, printFnRef, args);

    // Notify the rewriter that this operation has been removed.
    rewriter.eraseOp(op);
    return success();
}

using UnsignedTypePair = std::pair<unsigned, Type>;
static void getMemRefArgIndicesAndTypes(
    FunctionType type,
    SmallVectorImpl<UnsignedTypePair>& argsInfo)
{
    argsInfo.reserve(type.getNumInputs());
    for (auto en : llvm::enumerate(type.getInputs()))
    {
        if (en.value().isa<MemRefType>() || en.value().isa<UnrankedMemRefType>())
            argsInfo.push_back({ en.index(), en.value() });
    }
}

// Extract an LLVM IR type from the LLVM IR dialect type.
static Type unwrap(Type type)
{
    if (!type)
        return nullptr;
    auto* mlirContext = type.getContext();
    auto wrappedLLVMType = type.dyn_cast<Type>();
    if (!wrappedLLVMType)
        emitError(UnknownLoc::get(mlirContext),
                  "conversion resulted in a non-LLVM type");
    return wrappedLLVMType;
}

LogicalResult CallOpLowering::matchAndRewrite(
    ValueCallOp op,
    ArrayRef<mlir::Value> operands,
    ConversionPatternRewriter& rewriter) const
{
    auto loc = op.getLoc();

    ValueCallOp::Adaptor adaptor(operands);

    SmallVector<UnsignedTypePair, 4> promotedArgsInfo;
    auto funcType = op.getCalleeType();
    getMemRefArgIndicesAndTypes(funcType, promotedArgsInfo);

    SmallVector<MemRefDescriptor, 4> memrefDescriptors;
    for (auto argInfo : promotedArgsInfo)
    {
        memrefDescriptors.push_back(MemRefDescriptor{ adaptor.operands()[argInfo.first] });
    }

    SmallVector<mlir::Value, 4> newCallOperands;
    for (unsigned idx = 0, promotedArgsIdx = 0; idx < funcType.getNumInputs(); ++idx)
    {
        if (promotedArgsIdx < promotedArgsInfo.size() && idx == promotedArgsInfo[promotedArgsIdx].first)
        {
            newCallOperands.push_back(memrefDescriptors[promotedArgsIdx].alignedPtr(rewriter, loc));
            ++promotedArgsIdx;
        }
        else
        {
            newCallOperands.push_back(adaptor.operands()[idx]);
        }
    }

    TypeConverter::SignatureConversion result(funcType.getNumInputs());
    [[maybe_unused]] auto llvmType = llvmTypeConverter.convertFunctionSignature(funcType, false, result);

    SmallVector<mlir::Type, 1> resultTypes;
    if (funcType.getNumResults() > 0)
    {
        resultTypes.push_back(unwrap(llvmTypeConverter.packFunctionResults(funcType.getResults())));
    }
    [[maybe_unused]] auto newCallOp = rewriter.create<LLVM::CallOp>(loc, resultTypes, op.calleeAttr(), newCallOperands);
    rewriter.eraseOp(op);

    return success();
}

LogicalResult BitcastOpLowering::matchAndRewrite(
    BitcastOp op,
    ArrayRef<mlir::Value> operands,
    ConversionPatternRewriter& rewriter) const
{
    LLVMTypeConverter llvmTypeConverter(rewriter.getContext());
    auto resultType = llvmTypeConverter.convertType(op.getResult().getType());

    BitcastOp::Adaptor operandAdapter(operands);
    auto arg = operandAdapter.input();
    rewriter.replaceOpWithNewOp<LLVM::BitcastOp>(op, resultType, arg);
    return success();
}

mlir::Value GetTimeOpLowering::GetTime(ConversionPatternRewriter& rewriter, mlir::Location loc, ModuleOp& parentModule) const
{
    auto* llvmDialect = rewriter.getContext()->getOrLoadDialect<LLVM::LLVMDialect>();
    assert(llvmDialect && "expected llvm dialect to be registered");

    // call the platform-specific time function and convert to seconds
    // TODO: encode the target platform in the module or platform somehow, so we can query it instead
    // of having the runtime environment being based on the compile-time environment
    auto* context = rewriter.getContext();
    auto doubleTy = Float64Type::get(context);

    // TODO: get check `TargetDeviceInfo` for the OS instead
#ifdef WIN32
    auto queryPerfCounterFn = getOrInsertQueryPerfCounter(rewriter, parentModule, llvmDialect);
    auto queryPerfFrequencyFn = getOrInsertQueryPerfFrequency(rewriter, parentModule, llvmDialect);

    auto boolTy = IntegerType::get(context, 8);
    auto argTy = getPerformanceCounterType(context);
    LLVMTypeConverter llvmTypeConverter(context);
    Value one = rewriter.create<LLVM::ConstantOp>(loc, llvmTypeConverter.convertType(rewriter.getIndexType()), rewriter.getIntegerAttr(rewriter.getIndexType(), 1));
    Value perfCountPtr = rewriter.create<LLVM::AllocaOp>(loc, LLVM::LLVMPointerType::get(argTy), one);
    Value perfFreqPtr = rewriter.create<LLVM::AllocaOp>(loc, LLVM::LLVMPointerType::get(argTy), one);

    auto getCounterCall = rewriter.create<LLVM::CallOp>(loc, std::vector<Type>{ boolTy }, queryPerfCounterFn, ValueRange{ perfCountPtr });
    auto getFreqCall = rewriter.create<LLVM::CallOp>(loc, std::vector<Type>{ boolTy }, queryPerfFrequencyFn, ValueRange{ perfFreqPtr });
    [[maybe_unused]] auto getCountResult = getCounterCall.getResult(0);
    [[maybe_unused]] auto getFreqResult = getFreqCall.getResult(0);

    Value perfCount = rewriter.create<LLVM::LoadOp>(loc, perfCountPtr);
    Value perfFreq = rewriter.create<LLVM::LoadOp>(loc, perfFreqPtr);

    Value ticksDoubleVal = rewriter.create<LLVM::SIToFPOp>(loc, doubleTy, perfCount);
    Value freqDoubleVal = rewriter.create<LLVM::SIToFPOp>(loc, doubleTy, perfFreq);
    Value result = rewriter.create<LLVM::FDivOp>(loc, doubleTy, ticksDoubleVal, freqDoubleVal);
    return result;
#else
    auto clockGetTimeFn = getOrInsertClockGetTime(rewriter, parentModule, llvmDialect);

    auto llvmTimespecTy = getTimeSpecType(context);
    auto clockIdTy = getClockIdType(context);
    auto intTy = getIntType(context);

    // Get a symbol reference to the gettime function, inserting it if necessary.
    LLVMTypeConverter llvmTypeConverter(context);
    Value zero = rewriter.create<LLVM::ConstantOp>(loc, llvmTypeConverter.convertType(rewriter.getIndexType()), rewriter.getIntegerAttr(rewriter.getIndexType(), 0));
    Value zero32 = rewriter.create<LLVM::ConstantOp>(loc, llvmTypeConverter.convertType(rewriter.getI32Type()), rewriter.getIntegerAttr(rewriter.getI32Type(), 0));
    Value one = rewriter.create<LLVM::ConstantOp>(loc, llvmTypeConverter.convertType(rewriter.getIndexType()), rewriter.getIntegerAttr(rewriter.getIndexType(), 1));
    Value one32 = rewriter.create<LLVM::ConstantOp>(loc, llvmTypeConverter.convertType(rewriter.getI32Type()), rewriter.getIntegerAttr(rewriter.getI32Type(), 1));
    Value clockId = rewriter.create<LLVM::ConstantOp>(loc, clockIdTy, rewriter.getI64IntegerAttr(CLOCK_REALTIME));

    Value timespecPtr = rewriter.create<LLVM::AllocaOp>(loc, LLVM::LLVMPointerType::get(llvmTimespecTy), one);
    Value secondsPtr = rewriter.create<LLVM::GEPOp>(loc, LLVM::LLVMPointerType::get(intTy), timespecPtr, ValueRange{ zero, zero32 });
    Value nanosecondsPtr = rewriter.create<LLVM::GEPOp>(loc, LLVM::LLVMPointerType::get(intTy), timespecPtr, ValueRange{ zero, one32 });

    std::vector<Value> args{ clockId, timespecPtr };
    auto getTimeCall = rewriter.create<LLVM::CallOp>(loc, std::vector<Type>{ getIntType(context) }, clockGetTimeFn, args);
    [[maybe_unused]] auto getTimeResult = getTimeCall.getResult(0);

    Value secondsIntVal = rewriter.create<LLVM::LoadOp>(loc, secondsPtr);
    Value nanosecondsIntVal = rewriter.create<LLVM::LoadOp>(loc, nanosecondsPtr);
    Value secondsDoubleVal = rewriter.create<LLVM::SIToFPOp>(loc, doubleTy, secondsIntVal);
    Value nanosecondsDoubleVal = rewriter.create<LLVM::UIToFPOp>(loc, doubleTy, nanosecondsIntVal);
    Value divisor = rewriter.create<LLVM::ConstantOp>(loc, doubleTy, rewriter.getF64FloatAttr(1.0e9));
    Value nanoseconds = rewriter.create<LLVM::FDivOp>(loc, doubleTy, nanosecondsDoubleVal, divisor);
    Value totalSecondsDoubleVal = rewriter.create<LLVM::FAddOp>(loc, doubleTy, secondsDoubleVal, nanoseconds);
    return totalSecondsDoubleVal;
#endif
}

LogicalResult GetTimeOpLowering::matchAndRewrite(
    GetTimeOp op,
    ArrayRef<mlir::Value> operands,
    ConversionPatternRewriter& rewriter) const
{
    ModuleOp parentModule = op->getParentOfType<ModuleOp>();
    auto currentTime = GetTime(rewriter, op.getLoc(), parentModule);
    rewriter.replaceOp(op, { currentTime });
    return success();
}

void ValueToLLVMLoweringPass::runOnModule()
{
    llvm::DebugFlag =
#if !defined(NDEBUG) && 0
        true
#else
        false
#endif
        ;

    LLVMConversionTarget target(getContext());

    auto moduleOp = getModule();
    auto snapshotter = _intrapassSnapshotter.MakeSnapshotPipe();
    snapshotter.Snapshot("Initial", moduleOp);

    target.addLegalOp<ModuleOp>();

    // Set pass parameter values with command line options inherited from ConvertValueToLLVMBase
    mlir::LowerToLLVMOptions options(&getContext());
    options.useBarePtrCallConv = useBarePtrCallConv;
    options.emitCWrappers = emitCWrappers;
    if (indexBitwidth != kDeriveIndexBitwidthFromDataLayout)
    {
        options.overrideIndexBitwidth(indexBitwidth);
    }
    options.allocLowering = mlir::LowerToLLVMOptions::AllocLowering::AlignedAlloc;
    options.dataLayout = llvm::DataLayout(dataLayout);

    LLVMTypeConverter llvmTypeConverter(&getContext(), options);

    // Create bare pointer llvm options for handling raw-pointer-API function to non-raw-pointer-API function conversion and calls
    mlir::LowerToLLVMOptions barePtrOptions = options;
    barePtrOptions.useBarePtrCallConv = true;
    barePtrOptions.emitCWrappers = false;

    LLVMTypeConverter barePtrTypeConverter(&getContext(), barePtrOptions);

    llvm::SmallVector<Operation*> rawPointerFuncs;

    for (auto it = moduleOp.getOps().begin(), e = moduleOp.getOps().end(); it != e; ++it)
    {
        if (it->hasAttr(RawPointerAPIAttrName))
        {
            rawPointerFuncs.push_back(&*it);
        }
    }

    // Apply targeted Raw / Bare pointer conversions manually
    {
        OwningRewritePatternList patterns(&getContext());

        patterns.insert<RawPointerAPIFnConversion>(barePtrTypeConverter);
        patterns.insert<RawPointerAPICallOpConversion>(llvmTypeConverter, 100);

        if (failed(applyPartialConversion(rawPointerFuncs, target, std::move(patterns))))
        {
            signalPassFailure();
        }
    }

    snapshotter.Snapshot("BarePtrConversion", moduleOp);

    {
        OwningRewritePatternList patterns(&getContext());
        populateValueToLLVMPatterns(llvmTypeConverter, patterns);

        populateLinalgToLLVMConversionPatterns(llvmTypeConverter, patterns);

        populateVectorToLLVMConversionPatterns(llvmTypeConverter, patterns, /*reassociateFPReductions*/ true);
        vector::populateVectorContractLoweringPatterns(patterns, vector::VectorTransformsOptions{}.setVectorTransferSplit(mlir::vector::VectorTransferSplit::VectorTransfer));
        vector::populateVectorMaskMaterializationPatterns(patterns, true);

        if (failed(applyPartialConversion(moduleOp, target, std::move(patterns))))
        {
            signalPassFailure();
        }
    }

    snapshotter.Snapshot("ToLLVM_NonMem", moduleOp);

    FrozenRewritePatternSet toLLVMPatterns;
    {
        OwningRewritePatternList patterns(&getContext());

        populateMathToLLVMConversionPatterns(llvmTypeConverter, patterns);
        populateMemRefToLLVMConversionPatterns(llvmTypeConverter, patterns);
        populateStdToLLVMConversionPatterns(llvmTypeConverter, patterns);

        populateVectorToLLVMConversionPatterns(llvmTypeConverter, patterns, /*reassociateFPReductions*/ true);
        vector::populateVectorContractLoweringPatterns(patterns, vector::VectorTransformsOptions{}.setVectorTransferSplit(mlir::vector::VectorTransferSplit::VectorTransfer));
        vector::populateVectorMaskMaterializationPatterns(patterns, true);

        // cf. mlir\lib\Conversion\OpenMPToLLVM\OpenMPToLLVM.cpp
        target.addDynamicallyLegalOp<mlir::omp::ParallelOp, mlir::omp::WsLoopOp>(
            [&](Operation* op) { return llvmTypeConverter.isLegal(&op->getRegion(0)); });
        target.addLegalOp<mlir::omp::TerminatorOp, mlir::omp::TaskyieldOp, mlir::omp::FlushOp, mlir::omp::BarrierOp, mlir::omp::TaskwaitOp>();

        populateOpenMPToLLVMConversionPatterns(llvmTypeConverter, patterns);

        toLLVMPatterns = std::move(patterns);
        if (failed(applyPartialConversion(moduleOp, target, toLLVMPatterns)))
        {
            signalPassFailure();
        }
    }

    snapshotter.Snapshot("ToLLVM_Mem", moduleOp);

    {
        OwningRewritePatternList patterns(&getContext());
        patterns.insert<LLVMCallFixupPattern>(&getContext());

        FrozenRewritePatternSet frozen{ std::move(patterns) };
        {
            if (failed(applyPatternsAndFoldGreedily(moduleOp, frozen)))
            {
                signalPassFailure();
            }
        }
    }

    snapshotter.Snapshot("Final", moduleOp);

    llvm::DebugFlag = false;
}

namespace accera::transforms::value
{

void populateGlobalValueToLLVMPatterns(mlir::LLVMTypeConverter& typeConverter, mlir::OwningRewritePatternList& patterns)
{
    mlir::MLIRContext* context = patterns.getContext();
    patterns.insert<GlobalOpToLLVMLowering>(typeConverter, context);
}

void populateLocalValueToLLVMPatterns(mlir::LLVMTypeConverter& typeConverter, mlir::OwningRewritePatternList& patterns)
{
    mlir::MLIRContext* context = patterns.getContext();

    patterns.insert<
        CPUEarlyReturnRewritePattern,
        ReferenceGlobalOpLowering,
        BitcastOpLowering,
        CallOpLowering,
        PrintFOpLowering,
        GetTimeOpLowering>(typeConverter, context);
}

void populateValueToLLVMPatterns(mlir::LLVMTypeConverter& typeConverter, mlir::OwningRewritePatternList& patterns)
{
    populateGlobalValueToLLVMPatterns(typeConverter, patterns);
    populateLocalValueToLLVMPatterns(typeConverter, patterns);
}

const mlir::LowerToLLVMOptions& GetDefaultAcceraLLVMOptions(mlir::MLIRContext* context)
{
    static LowerToLLVMOptions options(context); // statically allocated default we hand out copies to

    // set Accera alterations to the defaults
    options.allocLowering = mlir::LowerToLLVMOptions::AllocLowering::AlignedAlloc;

    return options;
}

std::unique_ptr<mlir::OperationPass<mlir::ModuleOp>> createValueToLLVMPass(mlir::LowerToLLVMOptions options)
{
    return std::make_unique<ValueToLLVMLoweringPass>(options.useBarePtrCallConv, options.emitCWrappers, options.getIndexBitwidth(), options.allocLowering == mlir::LowerToLLVMOptions::AllocLowering::AlignedAlloc, options.dataLayout);
}

std::unique_ptr<mlir::OperationPass<mlir::ModuleOp>> createValueToLLVMPass(mlir::MLIRContext* context)
{
    return createValueToLLVMPass();
}

std::unique_ptr<mlir::OperationPass<mlir::ModuleOp>> createValueToLLVMPass(bool useBasePtrCallConv,
                                                                           bool emitCWrappers,
                                                                           unsigned indexBitwidth,
                                                                           bool useAlignedAlloc,
                                                                           llvm::DataLayout dataLayout,
                                                                           const IntraPassSnapshotOptions& options /*  = {} */)
{
    return std::make_unique<ValueToLLVMLoweringPass>(useBasePtrCallConv, emitCWrappers, indexBitwidth, useAlignedAlloc, dataLayout, options);
}

std::unique_ptr<mlir::OperationPass<mlir::ModuleOp>> createValueToLLVMPass()
{
    // The values here should always match the ones specified by GetDefaultAcceraLLVMOptions
    llvm::DataLayout dataLayout = llvm::DataLayout("");
    return createValueToLLVMPass(/* useBasePtrCallConv = */ false,
                                 /* emitCWrappers = */ false,
                                 /* indexBitwidth = */ kDeriveIndexBitwidthFromDataLayout,
                                 /* useAlignedAlloc = */ true,
                                 /* dataLayout = */ dataLayout);
}

} // namespace accera::transforms::value
