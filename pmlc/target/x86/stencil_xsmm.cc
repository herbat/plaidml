// Copyright 2020 Intel Corporation

#include "pmlc/dialect/pxa/transforms/stencil_generic.h"

#include "pmlc/dialect/pxa/transforms/autotile.h" // TODO: for PowerOfTwoGenerator

#include "mlir/Support/DebugStringHelper.h" // TODO: sort

// TODO: Just seeing if the stencil.cc includes work
#include "mlir/Dialect/Affine/IR/AffineOps.h"
#include "mlir/Dialect/StandardOps/IR/Ops.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Pass/PassOptions.h"

#include "pmlc/dialect/eltwise/ir/ops.h"
#include "pmlc/dialect/pxa/analysis/strides.h"
#include "pmlc/dialect/pxa/ir/ops.h"
#include "pmlc/dialect/pxa/transforms/passes.h"
#include "pmlc/dialect/xsmm/ir/ops.h"

#include "pmlc/util/logging.h"
#include "pmlc/util/util.h"

#include "pmlc/target/x86/heatmap.h" // TODO: for heatmap

// TODO: includes etc

namespace pmlc::dialect::pxa {

class StencilXSMM : public StencilGeneric {
private:
  unsigned numThreads;

  llvm::Optional<LoadStoreOps> capture() {
    // Looking for load..load..mul..reduce..terminator
    LoadStoreOps ret;
    const unsigned kNumValidInstrInGemmRegion = 5;
    auto *body = op.getBody();

    // Verify the number of ops
    if (body->getOperations().size() != kNumValidInstrInGemmRegion) {
      IVLOG(5, "The AffineParallelOp region didn't have the right number of "
               "instructions for a GEMM");
      return llvm::None;
    }

    // Find the Reduce Op
    auto it = std::prev(body->end(), 2);
    auto reduceOp = llvm::dyn_cast<AffineReduceOp>(*it);
    if (!reduceOp) {
      IVLOG(5, "The AffineParallelOp region didn't have a reduce as its last "
               "non-terminator");
      return llvm::None;
    }
    ret.stores.push_back(&*it);
    IVLOG(5, "Found ReduceOp");

    // Now check the reduceOp aggregation.
    if (reduceOp.agg() != AggregationKind::add) {
      IVLOG(5, "the reduce operation is not addition");
      return llvm::None;
    }

    // Get the operand for the reduce op and make sure it is the result of a
    // multiplication.
    auto defOp = reduceOp.val().getDefiningOp();
    if (!defOp) {
      IVLOG(5,
            "the source of the reduce operation is not defined in this block");
      return llvm::None;
    }

    mlir::Operation *lhs;
    mlir::Operation *rhs;
    if (auto mulfOp = llvm::dyn_cast_or_null<mlir::MulFOp>(defOp)) {
      lhs = mulfOp.lhs().getDefiningOp();
      if (!llvm::dyn_cast_or_null<mlir::AffineLoadOp>(lhs)) {
        IVLOG(3, "The LHS of the mul op is not affine.load.");
        return llvm::None;
      }
      rhs = mulfOp.rhs().getDefiningOp();
      if (!llvm::dyn_cast_or_null<mlir::AffineLoadOp>(rhs)) {
        IVLOG(3, "The RHS of the mul op is not affine.load.");
        return llvm::None;
      }
    } else if (auto muliOp = llvm::dyn_cast_or_null<mlir::MulIOp>(defOp)) {
      lhs = muliOp.lhs().getDefiningOp();
      if (!llvm::dyn_cast_or_null<mlir::AffineLoadOp>(lhs)) {
        IVLOG(3, "The LHS of the mul op is not affine.load.");
        return llvm::None;
      }
      rhs = muliOp.rhs().getDefiningOp();
      if (!llvm::dyn_cast_or_null<mlir::AffineLoadOp>(rhs)) {
        IVLOG(3, "The RHS of the mul op is not affine.load.");
        return llvm::None;
      }
    } else {
      IVLOG(5, "The source of the reduce is not a multiplication operation");
      return llvm::None;
    }
    ret.loads.push_back(lhs);
    ret.loads.push_back(rhs);

    return llvm::Optional<LoadStoreOps>(std::move(ret));
  }

  double getCost(TensorAndIndexPermutation perm, ArrayRef<int64_t> tileSize) {
    unsigned tot_inner_loop = tileSize[0] * tileSize[1] * tileSize[2];

    // TODO: Just put these in the order heatmap expects? Flip heatmap? Also
    // int64_t vs. unsigned?
    llvm::SmallVector<unsigned, 3> tileSizeTODO;
    tileSizeTODO.push_back(tileSize[1]);
    tileSizeTODO.push_back(tileSize[0]);
    tileSizeTODO.push_back(tileSize[2]);
    auto cost = pmlc::target::x86::heatmapCost(tileSizeTODO);
    if (cost.throughput == 0) {
      return std::numeric_limits<double>::infinity();
    }
    double inner_time = tot_inner_loop / cost.throughput;
    IVLOG(6,
          "Inner: loop = " << tot_inner_loop << " inner_time = " << inner_time);
    for (unsigned i = 0; i < semanticIdxCount; ++i) {
      IVLOG(6, perm.indexes[i] << ": " << tileSize[i]);
    }

    // The middle idxs are the accumulation indexes, i.e. those used on loads
    // but not stores
    // llvm::DenseMap<mlir::BlockArgument, unsigned> middle_idxs;
    std::map<unsigned, unsigned> middle_idxs; // TODO: Why does this matter?
    unsigned TODO_loop_count = 0;
    auto in0StrideInfo = getStrideInfo(perm.ioOps[0]);
    for (const auto &kvp : in0StrideInfo->strides) {
      // TODO: Old version verifies that this is in the parallel op's BlockArgs,
      // but that seems excessive for something that I'd expect to be an
      // assert...
      if (!blockArgs.count(kvp.first)) {
        IVLOG(5, "Index found from outside current loop on left input: "
                     << kvp.first);
      } else {
        IVLOG(5, "[loop " << TODO_loop_count
                          << "] Based on first tensor, inserting middle index "
                          << kvp.first << ":" << kvp.first.getArgNumber());
        middle_idxs.insert(
            std::make_pair(kvp.first.getArgNumber(), getIdxRange(kvp.first)));
      }
      TODO_loop_count++;
    }
    IVLOG(5, "Current size of middle_idxs = " << middle_idxs.size());

    auto in1StrideInfo = getStrideInfo(perm.ioOps[1]);
    for (const auto &kvp : in1StrideInfo->strides) {
      // TODO: Old version verifies that this is in the parallel op's BlockArgs,
      // but that seems excessive for something that I'd expect to be an
      // assert...
      if (!blockArgs.count(kvp.first)) {
        IVLOG(5, "Index found from outside current loop on right input: "
                     << kvp.first);
      } else {
        IVLOG(5,
              "Based on second tensor, inserting middle index " << kvp.first);
        middle_idxs.insert(
            std::make_pair(kvp.first.getArgNumber(), getIdxRange(kvp.first)));
      }
    }
    IVLOG(5, "Current size of middle_idxs = " << middle_idxs.size());
    auto outStrideInfo = getStrideInfo(perm.ioOps[2]);
    for (const auto &kvp : outStrideInfo->strides) {
      if (!blockArgs.count(kvp.first)) {
        IVLOG(5,
              "Index found from outside current loop on output: " << kvp.first);
      } else {
        auto it = middle_idxs.find(kvp.first.getArgNumber());
        if (it != middle_idxs.end()) {
          IVLOG(5,
                "Based on output tensor, erasing middle index " << it->first);
          middle_idxs.erase(it);
        }
      }
    }

    for (unsigned i = 0; i < semanticIdxCount; ++i) {
      assert(blockArgs.count(perm.indexes[i]) &&
             "All tiled indexes must be introduced in current loop");
      auto it = middle_idxs.find(perm.indexes[i].getArgNumber());
      if (it != middle_idxs.end()) {
        it->second = llvm::divideCeil(it->second, tileSize[i]);
      }
    }
    unsigned tot_middle_loop = 1;
    for (auto &kvp : middle_idxs) {
      tot_middle_loop *= kvp.second;
    }

    IVLOG(4, "Middle: loop = " << tot_middle_loop);

    for (auto &kvp : middle_idxs) {
      if (kvp.second > 1) {
        IVLOG(4, kvp.first << ": " << kvp.second);
      }
    }

    // llvm::DenseMap<mlir::BlockArgument, unsigned> outer_idxs;
    std::map<unsigned, unsigned> outer_idxs; // TODO why does this matter...
    for (const auto &kvp : outStrideInfo->strides) {
      if (!blockArgs.count(kvp.first)) {
        IVLOG(5, "Index found from outside current loop on output (2nd pass): "
                     << kvp.first);
      } else {
        IVLOG(4, "First: " << kvp.first);
        IVLOG(5, "Second: " << kvp.second);
        IVLOG(5, "IdxRange: " << getIdxRange(kvp.first));
        outer_idxs.try_emplace(kvp.first.getArgNumber(),
                               getIdxRange(kvp.first));
        IVLOG(4, "And now emplaced");
      }
    }
    IVLOG(4, "Left loop...");
    for (unsigned i = 0; i < semanticIdxCount; i++) {
      assert(blockArgs.count(perm.indexes[i]) &&
             "All tiled indexes must be introduced in current loop");
      auto it = outer_idxs.find(perm.indexes[i].getArgNumber());
      if (it != outer_idxs.end()) {
        it->second = llvm::divideCeil(it->second, tileSize[i]);
      }
    }
    unsigned tot_outer_loop = 1;
    for (auto &kvp : outer_idxs) {
      tot_outer_loop *= kvp.second;
    }

    IVLOG(4, "Outer: loop = " << tot_outer_loop);

    for (auto &kvp : outer_idxs) {
      if (kvp.second > 1) {
        IVLOG(4, kvp.first << ": " << kvp.second);
      }
    }

    unsigned outer_batches = (tot_outer_loop - 1) / numThreads + 1;
    double perf =
        outer_batches * tot_middle_loop * (cost.startupCost + inner_time);

    IVLOG(3, "Performance = " << perf << "(outer count: " << outer_batches
                              << ", middle count: " << tot_middle_loop
                              << ", startup cost: " << cost.startupCost
                              << ", inner time: " << inner_time << ")");
    return perf;
  }

  void transform(TensorAndIndexPermutation perm, ArrayRef<int64_t> tileSize) {
    // TODO: Clean up this logging
    if (VLOG_IS_ON(2)) {
      std::stringstream bestReport;
      bestReport << "Stencil Selection Report:\n";
      bestReport << "    Best Perf: " << bestCost << "\n";
      std::stringstream tensorPermStr;
      tensorPermStr << "[\n";
      for (auto ioOp : perm.ioOps) {
        tensorPermStr << "        " << mlir::debugString(*ioOp) << "\n";
      }
      tensorPermStr << "    ]";
      bestReport << "    Best Tensor Permutation: " << tensorPermStr.str()
                 << "\n";
      std::stringstream indexPermStr;
      indexPermStr << "[ ";
      for (auto ind : perm.indexes) {
        assert(blockArgs.count(ind) &&
               "All tiled indexes must be introduced in current loop");
        indexPermStr << ind.getArgNumber() << " ";
      }
      indexPermStr << "]";
      bestReport << "    Best Index Permutation: " << indexPermStr.str()
                 << "\n";
      std::stringstream bestTilingStr;
      bestTilingStr << "[ ";
      for (const auto &sz : tileSize) {
        bestTilingStr << sz << " ";
      }
      bestTilingStr << "]";
      bestReport << "    Best Tiling: " << bestTilingStr.str();
      IVLOG(2, bestReport.str());
    }

    // First, modify step size of all tiled indexes
    llvm::SmallVector<int64_t, 8> steps;
    auto oldSteps = op.steps().cast<ArrayAttr>().getValue();
    for (auto step : oldSteps) {
      steps.push_back(step.cast<IntegerAttr>().getInt());
    }
    for (size_t i = 0; i < ranges.size(); i++) {
      for (size_t j = 0; j < semanticIdxCount; j++) {
        if (perm.indexes[j] == op.getBody()->getArgument(i)) {
          steps[i] *= tileSize[j];
        }
      }
    }
    op.setSteps(steps);

    // Generate the XSMM call; first select inputs based on permutation order
    auto opA = llvm::dyn_cast<mlir::AffineLoadOp>(*perm.ioOps[0]);
    auto opB = llvm::dyn_cast<mlir::AffineLoadOp>(*perm.ioOps[1]);
    auto opC = llvm::dyn_cast<AffineReduceOp>(*perm.ioOps[2]);
    // TODO: Assert casts worked right?

    // Get the current memrefs
    Value aVal = opA.getMemRef();
    Value bVal = opB.getMemRef();
    Value cVal = opC.out();

    // Initialize helpers
    llvm::SmallVector<Value, 8> mapOperands;
    auto bodyBuilder = op.getBodyBuilder();
    auto makeTileMap = [&](AffineMap map, ValueRange ops,
                           ArrayRef<mlir::BlockArgument> idxs) {
      llvm::SmallVector<AffineExpr, 8> perOp;
      for (auto op : ops) {
        bool found = false;
        for (size_t i = 0; i < idxs.size(); i++) {
          if (op == idxs[i]) {
            perOp.push_back(bodyBuilder.getAffineDimExpr(i));
            found = true;
          }
        }
        if (!found) {
          perOp.push_back(bodyBuilder.getAffineConstantExpr(0));
        }
      }
      auto toIdxs = AffineMap::get(idxs.size(), 0, perOp, op.getContext());
      return map.compose(toIdxs);
    };

    // Set the tile size. Note XSMM wants n, m, k order and we have m, n, k
    // TODO: Or maybe we should change everything to match XSMM order?
    llvm::SmallVector<int64_t, 3> xsmmTileSize;
    xsmmTileSize.push_back(tileSize[1]);
    xsmmTileSize.push_back(tileSize[0]);
    xsmmTileSize.push_back(tileSize[2]);
    auto tiles = bodyBuilder.getI64ArrayAttr(xsmmTileSize);

    // Set up the maps
    AffineMap cMap = opC.getAffineMap();
    AffineMap cTile = makeTileMap(opC.getAffineMap(), opC.getMapOperands(),
                                  {perm.indexes[0], perm.indexes[1]});
    mapOperands.append(opC.getMapOperands().begin(),
                       opC.getMapOperands().end());

    AffineMap aMap = opA.getAffineMap();
    AffineMap aTile = makeTileMap(opA.getAffineMap(), opA.getMapOperands(),
                                  {perm.indexes[0], perm.indexes[2]});
    mapOperands.append(opA.getMapOperands().begin(),
                       opA.getMapOperands().end());

    AffineMap bMap = opB.getAffineMap();
    AffineMap bTile = makeTileMap(opB.getAffineMap(), opB.getMapOperands(),
                                  {perm.indexes[2], perm.indexes[1]});
    mapOperands.append(opB.getMapOperands().begin(),
                       opB.getMapOperands().end());

    // Make the XSMM op
    bodyBuilder.create<xsmm::GemmOp>(op.getLoc(), cVal, cMap, cTile, aVal, aMap,
                                     aTile, bVal, bMap, bTile, tiles,
                                     mapOperands);

    // Remove all other ops from the op interior
    auto xsmm_it = std::prev(op.getBody()->end(), 2);
    while (op.getBody()->begin() != xsmm_it) {
      auto prev_it = std::prev(xsmm_it);
      op.getBody()->getOperations().erase(prev_it);
    }
  }

public:
  explicit StencilXSMM(mlir::AffineParallelOp op, unsigned numThreads)
      : StencilGeneric{op}, numThreads{numThreads} {
    // TODO: Probably want to move these to be params on StencilGeneric ctor...
    semanticIdxCount = 3; // TODO [i.e., must match generators & requirements]
    requirements =        // TODO: Make nicer
        std::map<std::pair<int64_t, int64_t>,
                 std::function<bool(mlir::Operation *, mlir::BlockArgument)>>{
            {{0, 0},
             [this](mlir::Operation *ioOp, mlir::BlockArgument a) {
               return getStrideInfo(ioOp)->strides[a] != 0;
             }},
            {{0, 1},
             [this](mlir::Operation *ioOp, mlir::BlockArgument a) {
               return getStrideInfo(ioOp)->strides[a] == 0;
             }},
            {{0, 2},
             [this](mlir::Operation *ioOp, mlir::BlockArgument a) {
               return getStrideInfo(ioOp)->strides[a] == 1;
             }},
            {{1, 0},
             [this](mlir::Operation *ioOp, mlir::BlockArgument a) {
               return getStrideInfo(ioOp)->strides[a] == 0;
             }},
            {{1, 1},
             [this](mlir::Operation *ioOp, mlir::BlockArgument a) {
               return getStrideInfo(ioOp)->strides[a] == 1;
             }},
            {{1, 2},
             [this](mlir::Operation *ioOp, mlir::BlockArgument a) {
               return getStrideInfo(ioOp)->strides[a] != 0;
             }},
            {{2, 0},
             [this](mlir::Operation *ioOp, mlir::BlockArgument a) {
               return getStrideInfo(ioOp)->strides[a] != 0;
             }},
            {{2, 1},
             [this](mlir::Operation *ioOp, mlir::BlockArgument a) {
               return getStrideInfo(ioOp)->strides[a] == 1;
             }},
            {{2, 2},
             [this](mlir::Operation *ioOp, mlir::BlockArgument a) {
               return getStrideInfo(ioOp)->strides[a] == 0;
             }},
        };
    tilingGenerators.push_back(EvenTilingGenerator());
    tilingGenerators.push_back(EvenTilingGenerator());
    tilingGenerators.push_back(EvenTilingGenerator());
  }
};

struct XSMMStencilPass
    : public mlir::PassWrapper<XSMMStencilPass, mlir::FunctionPass> {
  // TODO: (?) probably actually need config for requirements & tilingGenerators
  XSMMStencilPass() { assert(false && "XSMMStencilPass must be configured"); }

  XSMMStencilPass(const XSMMStencilPass &rhs) {
    numThreads = rhs.numThreads.getValue();
  }

  explicit XSMMStencilPass(unsigned numThreads_) { numThreads = numThreads_; }

  void runOnFunction() final {
    auto func = getFunction();
    func.walk([this](mlir::AffineParallelOp op) {
      StencilXSMM stencil(op, numThreads.getValue());
      stencil.DoStenciling();
    });
  }

  Option<unsigned> numThreads{
      *this, "threads",
      llvm::cl::desc("Specifies number of threads for the stencil pass")};
};

std::unique_ptr<mlir::Pass> createXSMMStencilPass(unsigned numThreads) {
  return std::make_unique<XSMMStencilPass>(numThreads);
}

} // namespace pmlc::dialect::pxa
