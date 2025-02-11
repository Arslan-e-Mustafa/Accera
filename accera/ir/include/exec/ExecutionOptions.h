////////////////////////////////////////////////////////////////////////////////////////////////////
//  Copyright (c) Microsoft Corporation. All rights reserved.
//  Licensed under the MIT License. See LICENSE in the project root for license information.
//  Authors: Abdul Dakkak, Mason Remy
////////////////////////////////////////////////////////////////////////////////////////////////////

#pragma once

#include <ir/include/IRUtil.h>

#include <mlir/IR/Attributes.h>

#include <cstdint>
#include <variant>
#include <vector>

namespace accera::ir
{
namespace targets
{
    //  <summary> A struct encapsulating x, y, z indices for a GPU processor </summary>
    struct Dim3
    {
        /// <summary> The x index </summary>
        int64_t x;
        /// <summary> The y index </summary>
        int64_t y;
        /// <summary> The z index </summary>
        int64_t z;

        Dim3(int64_t x_ = 1, int64_t y_ = 1, int64_t z_ = 1) :
            x(x_), y(y_), z(z_) {}
    };

    /// <summary> The CPU execution options </summary>
    struct CPU
    {};

    /// <summary> The GPU execution options </summary>
    struct GPU
    {
        /// <summary> Indicates the grid </summary>
        Dim3 grid;

        /// <summary> Indicates the block </summary>
        Dim3 block;

        GPU(Dim3 grid_ = Dim3(1, 1, 1), Dim3 block_ = Dim3(1, 1, 1)) :
            grid(grid_), block(block_){};

        static GPU FromArrayAttr(const mlir::ArrayAttr& arrayAttr)
        {
            auto launchParams = util::ConvertArrayAttrToIntVector(arrayAttr);
            Dim3 gridDimSizes(launchParams[0], launchParams[1], launchParams[2]);
            Dim3 blockDimSizes(launchParams[3], launchParams[4], launchParams[5]);
            return { gridDimSizes, blockDimSizes };
        }

        mlir::ArrayAttr ToArrayAttr(mlir::MLIRContext* context)
        {
            std::vector<int64_t> gridAndBlockDims{ grid.x, grid.y, grid.z, block.x, block.y, block.z };
            return util::VectorToArrayAttr<int64_t, mlir::IntegerAttr>(
                gridAndBlockDims, [&](const int64_t& intVal) {
                    return mlir::IntegerAttr::get(mlir::IntegerType::get(context, 64), intVal);
                },
                context);
        }
    };

    using Target = std::variant<CPU, GPU>;
    enum class Runtime : int
    {
        NONE,
        CUDA,
        ROCM,
        VULKAN,
        OPENMP,
        DEFAULT
    };

} // namespace targets
} // namespace accera::ir