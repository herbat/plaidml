// Copyright 2020 Intel Corporation

#pragma once

#include <algorithm>
#include <functional>
#include <limits>
#include <map>
#include <utility>
#include <vector>

#include "mlir/Dialect/Affine/IR/AffineOps.h"
#include "mlir/Pass/Pass.h"

#include "pmlc/dialect/pxa/analysis/strides.h"

namespace pmlc::dialect::pxa {

using BlockArgumentSet = llvm::SmallPtrSet<mlir::BlockArgument, 8>;

struct TensorAndIndexPermutation {
  // An order of the Tensors and Indexes used in an operation
  // Note: Tensors are tracked by their load/store/reduce ops, not values,
  // because we need to get stride information, which means we need the op,
  // and for stores/reduces getting to the op from the value is nontrivial
  llvm::SmallVector<mlir::Operation *, 3> ioOps;
  llvm::SmallVector<mlir::BlockArgument, 8> indexes;

  TensorAndIndexPermutation() = default;

  TensorAndIndexPermutation(llvm::SmallVector<mlir::Operation *, 3> ioOps,
                            llvm::SmallVector<mlir::BlockArgument, 8> indexes)
      : ioOps(ioOps), indexes(indexes) {}
};

struct LoadStoreOps {
  // The load and store ops of an AffineParallel
  llvm::SmallVector<mlir::Operation *, 2> loads;
  // TODO: Set up to enable either store or reduce
  // TODO: Or if this works, indicate that can be store or reduce or mix
  llvm::SmallVector<mlir::Operation *, 1> stores;
};

using TileSizeGenerator = std::function<std::vector<int64_t>(int64_t)>;

class StencilGeneric {
private:
  void BindIndexes(const llvm::SmallVector<mlir::Operation *, 3> &ioOps);
  void RecursiveBindIndex(llvm::SmallVector<mlir::BlockArgument, 8> *bound_idxs,
                          const llvm::SmallVector<mlir::Operation *, 3> &ioOps);
  void RecursiveTileIndex(const TensorAndIndexPermutation &perm,
                          llvm::SmallVector<int64_t, 8> *tileSize,
                          int64_t currIdx);

protected: // TODO: private backend for some of this?
  virtual llvm::Optional<LoadStoreOps> capture() = 0;
  virtual double getCost(TensorAndIndexPermutation perm,
                         ArrayRef<int64_t> tileSize) = 0;
  virtual void transform(TensorAndIndexPermutation perm,
                         ArrayRef<int64_t> tileSize) = 0;
  int64_t getIdxRange(mlir::BlockArgument idx);
  mlir::Optional<mlir::StrideInfo> getStrideInfo(mlir::Operation *ioOp);

  // Cache of StrideInfo results
  llvm::DenseMap<mlir::Operation *, mlir::StrideInfo> strideInfoCache;

  // The number of indexes whose semantics must be considered in the tiling
  unsigned semanticIdxCount; // TODO: how/where to initialize?

  // The ParallelOp that is being stenciled.
  mlir::AffineParallelOp op;

  // The BlockArguments of `op` (stored as a set for easy lookup)
  BlockArgumentSet blockArgs;

  // The load and store ops
  LoadStoreOps loadsAndStores;

  // The range of each index (cached result of op.getConstantRanges())
  llvm::SmallVector<int64_t, 8> ranges;

  // For each tensor/index semantic pair (given as a pair of size_ts), a
  // function to determine if a Value & BlockArg meet the requirements of that
  // pair
  llvm::DenseMap<std::pair<int64_t, int64_t>,
                 std::function<bool(mlir::Operation *, mlir::BlockArgument)>>
      requirements;

  // For each semantically relevant index, a generator for tile sizes. Ordered
  // to match the index permutation.
  llvm::SmallVector<TileSizeGenerator, 5> tilingGenerators;

  double bestCost;
  TensorAndIndexPermutation bestPermutation;
  llvm::SmallVector<int64_t, 8>
      bestTiling; // only makes sense paired with `bestPermutation`

public:
  explicit StencilGeneric(mlir::AffineParallelOp op)
      : op(op), bestCost(std::numeric_limits<double>::infinity()) {
    for (auto blockArg : op.getBody()->getArguments()) {
      blockArgs.insert(blockArg);
    }
  }

  // Main function
  void DoStenciling();
};

} // namespace pmlc::dialect::pxa
