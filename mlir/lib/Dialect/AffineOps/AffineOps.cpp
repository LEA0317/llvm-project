//===- AffineOps.cpp - MLIR Affine Operations -----------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "mlir/Dialect/AffineOps/AffineOps.h"
#include "mlir/Analysis/AffineStructures.h"
#include "mlir/Dialect/AffineOps/AffineValueMap.h"
#include "mlir/Dialect/StandardOps/Ops.h"
#include "mlir/IR/Function.h"
#include "mlir/IR/IntegerSet.h"
#include "mlir/IR/Matchers.h"
#include "mlir/IR/OpImplementation.h"
#include "mlir/IR/PatternMatch.h"
#include "mlir/Transforms/InliningUtils.h"
#include "mlir/Transforms/SideEffectsInterface.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/SmallBitVector.h"
#include "llvm/Support/Debug.h"

using namespace mlir;
using llvm::dbgs;

#define DEBUG_TYPE "affine-analysis"

//===----------------------------------------------------------------------===//
// AffineOpsDialect Interfaces
//===----------------------------------------------------------------------===//

namespace {
/// This class defines the interface for handling inlining with affine
/// operations.
struct AffineInlinerInterface : public DialectInlinerInterface {
  using DialectInlinerInterface::DialectInlinerInterface;

  //===--------------------------------------------------------------------===//
  // Analysis Hooks
  //===--------------------------------------------------------------------===//

  /// Returns true if the given region 'src' can be inlined into the region
  /// 'dest' that is attached to an operation registered to the current dialect.
  bool isLegalToInline(Region *dest, Region *src,
                       BlockAndValueMapping &valueMapping) const final {
    // Conservatively don't allow inlining into affine structures.
    return false;
  }

  /// Returns true if the given operation 'op', that is registered to this
  /// dialect, can be inlined into the given region, false otherwise.
  bool isLegalToInline(Operation *op, Region *region,
                       BlockAndValueMapping &valueMapping) const final {
    // Always allow inlining affine operations into the top-level region of a
    // function. There are some edge cases when inlining *into* affine
    // structures, but that is handled in the other 'isLegalToInline' hook
    // above.
    // TODO: We should be able to inline into other regions than functions.
    return isa<FuncOp>(region->getParentOp());
  }

  /// Affine regions should be analyzed recursively.
  bool shouldAnalyzeRecursively(Operation *op) const final { return true; }
};

// TODO(mlir): Extend for other ops in this dialect.
struct AffineSideEffectsInterface : public SideEffectsDialectInterface {
  using SideEffectsDialectInterface::SideEffectsDialectInterface;

  SideEffecting isSideEffecting(Operation *op) const override {
    if (isa<AffineIfOp>(op)) {
      return Recursive;
    }
    return SideEffectsDialectInterface::isSideEffecting(op);
  };
};

} // end anonymous namespace

//===----------------------------------------------------------------------===//
// AffineOpsDialect
//===----------------------------------------------------------------------===//

AffineOpsDialect::AffineOpsDialect(MLIRContext *context)
    : Dialect(getDialectNamespace(), context) {
  addOperations<AffineApplyOp, AffineDmaStartOp, AffineDmaWaitOp, AffineLoadOp,
                AffineStoreOp,
#define GET_OP_LIST
#include "mlir/Dialect/AffineOps/AffineOps.cpp.inc"
                >();
  addInterfaces<AffineInlinerInterface, AffineSideEffectsInterface>();
}

/// Materialize a single constant operation from a given attribute value with
/// the desired resultant type.
Operation *AffineOpsDialect::materializeConstant(OpBuilder &builder,
                                                 Attribute value, Type type,
                                                 Location loc) {
  return builder.create<ConstantOp>(loc, type, value);
}

/// A utility function to check if a given region is attached to a function.
static bool isFunctionRegion(Region *region) {
  return llvm::isa<FuncOp>(region->getParentOp());
}

/// A utility function to check if a value is defined at the top level of a
/// function. A value of index type defined at the top level is always a valid
/// symbol.
bool mlir::isTopLevelValue(Value value) {
  if (auto arg = value.dyn_cast<BlockArgument>())
    return isFunctionRegion(arg.getOwner()->getParent());
  return isFunctionRegion(value.getDefiningOp()->getParentRegion());
}

// Value can be used as a dimension id if it is valid as a symbol, or
// it is an induction variable, or it is a result of affine apply operation
// with dimension id arguments.
bool mlir::isValidDim(Value value) {
  // The value must be an index type.
  if (!value.getType().isIndex())
    return false;

  if (auto *op = value.getDefiningOp()) {
    // Top level operation or constant operation is ok.
    if (isFunctionRegion(op->getParentRegion()) || isa<ConstantOp>(op))
      return true;
    // Affine apply operation is ok if all of its operands are ok.
    if (auto applyOp = dyn_cast<AffineApplyOp>(op))
      return applyOp.isValidDim();
    // The dim op is okay if its operand memref/tensor is defined at the top
    // level.
    if (auto dimOp = dyn_cast<DimOp>(op))
      return isTopLevelValue(dimOp.getOperand());
    return false;
  }
  // This value has to be a block argument for a FuncOp or an affine.for.
  auto *parentOp = value.cast<BlockArgument>().getOwner()->getParentOp();
  return isa<FuncOp>(parentOp) || isa<AffineForOp>(parentOp);
}

/// Returns true if the 'index' dimension of the `memref` defined by
/// `memrefDefOp` is a statically  shaped one or defined using a valid symbol.
template <typename AnyMemRefDefOp>
static bool isMemRefSizeValidSymbol(AnyMemRefDefOp memrefDefOp,
                                    unsigned index) {
  auto memRefType = memrefDefOp.getType();
  // Statically shaped.
  if (!ShapedType::isDynamic(memRefType.getDimSize(index)))
    return true;
  // Get the position of the dimension among dynamic dimensions;
  unsigned dynamicDimPos = memRefType.getDynamicDimIndex(index);
  return isValidSymbol(
      *(memrefDefOp.getDynamicSizes().begin() + dynamicDimPos));
}

/// Returns true if the result of the dim op is a valid symbol.
static bool isDimOpValidSymbol(DimOp dimOp) {
  // The dim op is okay if its operand memref/tensor is defined at the top
  // level.
  if (isTopLevelValue(dimOp.getOperand()))
    return true;

  // The dim op is also okay if its operand memref/tensor is a view/subview
  // whose corresponding size is a valid symbol.
  unsigned index = dimOp.getIndex();
  if (auto viewOp = dyn_cast<ViewOp>(dimOp.getOperand().getDefiningOp()))
    return isMemRefSizeValidSymbol<ViewOp>(viewOp, index);
  if (auto subViewOp = dyn_cast<SubViewOp>(dimOp.getOperand().getDefiningOp()))
    return isMemRefSizeValidSymbol<SubViewOp>(subViewOp, index);
  if (auto allocOp = dyn_cast<AllocOp>(dimOp.getOperand().getDefiningOp()))
    return isMemRefSizeValidSymbol<AllocOp>(allocOp, index);
  return false;
}

// Value can be used as a symbol if it is a constant, or it is defined at
// the top level, or it is a result of affine apply operation with symbol
// arguments, or a result of the dim op on a memref satisfying certain
// constraints.
bool mlir::isValidSymbol(Value value) {
  // The value must be an index type.
  if (!value.getType().isIndex())
    return false;

  if (auto *op = value.getDefiningOp()) {
    // Top level operation or constant operation is ok.
    if (isFunctionRegion(op->getParentRegion()) || isa<ConstantOp>(op))
      return true;
    // Affine apply operation is ok if all of its operands are ok.
    if (auto applyOp = dyn_cast<AffineApplyOp>(op))
      return applyOp.isValidSymbol();
    if (auto dimOp = dyn_cast<DimOp>(op)) {
      return isDimOpValidSymbol(dimOp);
    }
  }
  // Otherwise, check that the value is a top level value.
  return isTopLevelValue(value);
}

// Returns true if 'value' is a valid index to an affine operation (e.g.
// affine.load, affine.store, affine.dma_start, affine.dma_wait).
// Returns false otherwise.
static bool isValidAffineIndexOperand(Value value) {
  return isValidDim(value) || isValidSymbol(value);
}

/// Utility function to verify that a set of operands are valid dimension and
/// symbol identifiers. The operands should be laid out such that the dimension
/// operands are before the symbol operands. This function returns failure if
/// there was an invalid operand. An operation is provided to emit any necessary
/// errors.
template <typename OpTy>
static LogicalResult
verifyDimAndSymbolIdentifiers(OpTy &op, Operation::operand_range operands,
                              unsigned numDims) {
  unsigned opIt = 0;
  for (auto operand : operands) {
    if (opIt++ < numDims) {
      if (!isValidDim(operand))
        return op.emitOpError("operand cannot be used as a dimension id");
    } else if (!isValidSymbol(operand)) {
      return op.emitOpError("operand cannot be used as a symbol");
    }
  }
  return success();
}

//===----------------------------------------------------------------------===//
// AffineApplyOp
//===----------------------------------------------------------------------===//

void AffineApplyOp::build(Builder *builder, OperationState &result,
                          AffineMap map, ValueRange operands) {
  result.addOperands(operands);
  result.types.append(map.getNumResults(), builder->getIndexType());
  result.addAttribute("map", AffineMapAttr::get(map));
}

AffineValueMap AffineApplyOp::getAffineValueMap() {
  return AffineValueMap(getAffineMap(), getOperands(), getResult());
}

ParseResult AffineApplyOp::parse(OpAsmParser &parser, OperationState &result) {
  auto &builder = parser.getBuilder();
  auto indexTy = builder.getIndexType();

  AffineMapAttr mapAttr;
  unsigned numDims;
  if (parser.parseAttribute(mapAttr, "map", result.attributes) ||
      parseDimAndSymbolList(parser, result.operands, numDims) ||
      parser.parseOptionalAttrDict(result.attributes))
    return failure();
  auto map = mapAttr.getValue();

  if (map.getNumDims() != numDims ||
      numDims + map.getNumSymbols() != result.operands.size()) {
    return parser.emitError(parser.getNameLoc(),
                            "dimension or symbol index mismatch");
  }

  result.types.append(map.getNumResults(), indexTy);
  return success();
}

void AffineApplyOp::print(OpAsmPrinter &p) {
  p << "affine.apply " << getAttr("map");
  printDimAndSymbolList(operand_begin(), operand_end(),
                        getAffineMap().getNumDims(), p);
  p.printOptionalAttrDict(getAttrs(), /*elidedAttrs=*/{"map"});
}

LogicalResult AffineApplyOp::verify() {
  // Check that affine map attribute was specified.
  auto affineMapAttr = getAttrOfType<AffineMapAttr>("map");
  if (!affineMapAttr)
    return emitOpError("requires an affine map");

  // Check input and output dimensions match.
  auto map = affineMapAttr.getValue();

  // Verify that operand count matches affine map dimension and symbol count.
  if (getNumOperands() != map.getNumDims() + map.getNumSymbols())
    return emitOpError(
        "operand count and affine map dimension and symbol count must match");

  // Verify that all operands are of `index` type.
  for (Type t : getOperandTypes()) {
    if (!t.isIndex())
      return emitOpError("operands must be of type 'index'");
  }

  if (!getResult().getType().isIndex())
    return emitOpError("result must be of type 'index'");

  // Verify that the map only produces one result.
  if (map.getNumResults() != 1)
    return emitOpError("mapping must produce one value");

  return success();
}

// The result of the affine apply operation can be used as a dimension id if all
// its operands are valid dimension ids.
bool AffineApplyOp::isValidDim() {
  return llvm::all_of(getOperands(),
                      [](Value op) { return mlir::isValidDim(op); });
}

// The result of the affine apply operation can be used as a symbol if all its
// operands are symbols.
bool AffineApplyOp::isValidSymbol() {
  return llvm::all_of(getOperands(),
                      [](Value op) { return mlir::isValidSymbol(op); });
}

OpFoldResult AffineApplyOp::fold(ArrayRef<Attribute> operands) {
  auto map = getAffineMap();

  // Fold dims and symbols to existing values.
  auto expr = map.getResult(0);
  if (auto dim = expr.dyn_cast<AffineDimExpr>())
    return getOperand(dim.getPosition());
  if (auto sym = expr.dyn_cast<AffineSymbolExpr>())
    return getOperand(map.getNumDims() + sym.getPosition());

  // Otherwise, default to folding the map.
  SmallVector<Attribute, 1> result;
  if (failed(map.constantFold(operands, result)))
    return {};
  return result[0];
}

AffineDimExpr AffineApplyNormalizer::renumberOneDim(Value v) {
  DenseMap<Value, unsigned>::iterator iterPos;
  bool inserted = false;
  std::tie(iterPos, inserted) =
      dimValueToPosition.insert(std::make_pair(v, dimValueToPosition.size()));
  if (inserted) {
    reorderedDims.push_back(v);
  }
  return getAffineDimExpr(iterPos->second, v.getContext())
      .cast<AffineDimExpr>();
}

AffineMap AffineApplyNormalizer::renumber(const AffineApplyNormalizer &other) {
  SmallVector<AffineExpr, 8> dimRemapping;
  for (auto v : other.reorderedDims) {
    auto kvp = other.dimValueToPosition.find(v);
    if (dimRemapping.size() <= kvp->second)
      dimRemapping.resize(kvp->second + 1);
    dimRemapping[kvp->second] = renumberOneDim(kvp->first);
  }
  unsigned numSymbols = concatenatedSymbols.size();
  unsigned numOtherSymbols = other.concatenatedSymbols.size();
  SmallVector<AffineExpr, 8> symRemapping(numOtherSymbols);
  for (unsigned idx = 0; idx < numOtherSymbols; ++idx) {
    symRemapping[idx] =
        getAffineSymbolExpr(idx + numSymbols, other.affineMap.getContext());
  }
  concatenatedSymbols.insert(concatenatedSymbols.end(),
                             other.concatenatedSymbols.begin(),
                             other.concatenatedSymbols.end());
  auto map = other.affineMap;
  return map.replaceDimsAndSymbols(dimRemapping, symRemapping,
                                   reorderedDims.size(),
                                   concatenatedSymbols.size());
}

// Gather the positions of the operands that are produced by an AffineApplyOp.
static llvm::SetVector<unsigned>
indicesFromAffineApplyOp(ArrayRef<Value> operands) {
  llvm::SetVector<unsigned> res;
  for (auto en : llvm::enumerate(operands))
    if (isa_and_nonnull<AffineApplyOp>(en.value().getDefiningOp()))
      res.insert(en.index());
  return res;
}

// Support the special case of a symbol coming from an AffineApplyOp that needs
// to be composed into the current AffineApplyOp.
// This case is handled by rewriting all such symbols into dims for the purpose
// of allowing mathematical AffineMap composition.
// Returns an AffineMap where symbols that come from an AffineApplyOp have been
// rewritten as dims and are ordered after the original dims.
// TODO(andydavis,ntv): This promotion makes AffineMap lose track of which
// symbols are represented as dims. This loss is static but can still be
// recovered dynamically (with `isValidSymbol`). Still this is annoying for the
// semi-affine map case. A dynamic canonicalization of all dims that are valid
// symbols (a.k.a `canonicalizePromotedSymbols`) into symbols helps and even
// results in better simplifications and foldings. But we should evaluate
// whether this behavior is what we really want after using more.
static AffineMap promoteComposedSymbolsAsDims(AffineMap map,
                                              ArrayRef<Value> symbols) {
  if (symbols.empty()) {
    return map;
  }

  // Sanity check on symbols.
  for (auto sym : symbols) {
    assert(isValidSymbol(sym) && "Expected only valid symbols");
    (void)sym;
  }

  // Extract the symbol positions that come from an AffineApplyOp and
  // needs to be rewritten as dims.
  auto symPositions = indicesFromAffineApplyOp(symbols);
  if (symPositions.empty()) {
    return map;
  }

  // Create the new map by replacing each symbol at pos by the next new dim.
  unsigned numDims = map.getNumDims();
  unsigned numSymbols = map.getNumSymbols();
  unsigned numNewDims = 0;
  unsigned numNewSymbols = 0;
  SmallVector<AffineExpr, 8> symReplacements(numSymbols);
  for (unsigned i = 0; i < numSymbols; ++i) {
    symReplacements[i] =
        symPositions.count(i) > 0
            ? getAffineDimExpr(numDims + numNewDims++, map.getContext())
            : getAffineSymbolExpr(numNewSymbols++, map.getContext());
  }
  assert(numSymbols >= numNewDims);
  AffineMap newMap = map.replaceDimsAndSymbols(
      {}, symReplacements, numDims + numNewDims, numNewSymbols);

  return newMap;
}

/// The AffineNormalizer composes AffineApplyOp recursively. Its purpose is to
/// keep a correspondence between the mathematical `map` and the `operands` of
/// a given AffineApplyOp. This correspondence is maintained by iterating over
/// the operands and forming an `auxiliaryMap` that can be composed
/// mathematically with `map`. To keep this correspondence in cases where
/// symbols are produced by affine.apply operations, we perform a local rewrite
/// of symbols as dims.
///
/// Rationale for locally rewriting symbols as dims:
/// ================================================
/// The mathematical composition of AffineMap must always concatenate symbols
/// because it does not have enough information to do otherwise. For example,
/// composing `(d0)[s0] -> (d0 + s0)` with itself must produce
/// `(d0)[s0, s1] -> (d0 + s0 + s1)`.
///
/// The result is only equivalent to `(d0)[s0] -> (d0 + 2 * s0)` when
/// applied to the same mlir::Value for both s0 and s1.
/// As a consequence mathematical composition of AffineMap always concatenates
/// symbols.
///
/// When AffineMaps are used in AffineApplyOp however, they may specify
/// composition via symbols, which is ambiguous mathematically. This corner case
/// is handled by locally rewriting such symbols that come from AffineApplyOp
/// into dims and composing through dims.
/// TODO(andydavis, ntv): Composition via symbols comes at a significant code
/// complexity. Alternatively we should investigate whether we want to
/// explicitly disallow symbols coming from affine.apply and instead force the
/// user to compose symbols beforehand. The annoyances may be small (i.e. 1 or 2
/// extra API calls for such uses, which haven't popped up until now) and the
/// benefit potentially big: simpler and more maintainable code for a
/// non-trivial, recursive, procedure.
AffineApplyNormalizer::AffineApplyNormalizer(AffineMap map,
                                             ArrayRef<Value> operands)
    : AffineApplyNormalizer() {
  static_assert(kMaxAffineApplyDepth > 0, "kMaxAffineApplyDepth must be > 0");
  assert(map.getNumInputs() == operands.size() &&
         "number of operands does not match the number of map inputs");

  LLVM_DEBUG(map.print(dbgs() << "\nInput map: "));

  // Promote symbols that come from an AffineApplyOp to dims by rewriting the
  // map to always refer to:
  //   (dims, symbols coming from AffineApplyOp, other symbols).
  // The order of operands can remain unchanged.
  // This is a simplification that relies on 2 ordering properties:
  //   1. rewritten symbols always appear after the original dims in the map;
  //   2. operands are traversed in order and either dispatched to:
  //      a. auxiliaryExprs (dims and symbols rewritten as dims);
  //      b. concatenatedSymbols (all other symbols)
  // This allows operand order to remain unchanged.
  unsigned numDimsBeforeRewrite = map.getNumDims();
  map = promoteComposedSymbolsAsDims(map,
                                     operands.take_back(map.getNumSymbols()));

  LLVM_DEBUG(map.print(dbgs() << "\nRewritten map: "));

  SmallVector<AffineExpr, 8> auxiliaryExprs;
  bool furtherCompose = (affineApplyDepth() <= kMaxAffineApplyDepth);
  // We fully spell out the 2 cases below. In this particular instance a little
  // code duplication greatly improves readability.
  // Note that the first branch would disappear if we only supported full
  // composition (i.e. infinite kMaxAffineApplyDepth).
  if (!furtherCompose) {
    // 1. Only dispatch dims or symbols.
    for (auto en : llvm::enumerate(operands)) {
      auto t = en.value();
      assert(t.getType().isIndex());
      bool isDim = (en.index() < map.getNumDims());
      if (isDim) {
        // a. The mathematical composition of AffineMap composes dims.
        auxiliaryExprs.push_back(renumberOneDim(t));
      } else {
        // b. The mathematical composition of AffineMap concatenates symbols.
        //    We do the same for symbol operands.
        concatenatedSymbols.push_back(t);
      }
    }
  } else {
    assert(numDimsBeforeRewrite <= operands.size());
    // 2. Compose AffineApplyOps and dispatch dims or symbols.
    for (unsigned i = 0, e = operands.size(); i < e; ++i) {
      auto t = operands[i];
      auto affineApply = dyn_cast_or_null<AffineApplyOp>(t.getDefiningOp());
      if (affineApply) {
        // a. Compose affine.apply operations.
        LLVM_DEBUG(affineApply.getOperation()->print(
            dbgs() << "\nCompose AffineApplyOp recursively: "));
        AffineMap affineApplyMap = affineApply.getAffineMap();
        SmallVector<Value, 8> affineApplyOperands(
            affineApply.getOperands().begin(), affineApply.getOperands().end());
        AffineApplyNormalizer normalizer(affineApplyMap, affineApplyOperands);

        LLVM_DEBUG(normalizer.affineMap.print(
            dbgs() << "\nRenumber into current normalizer: "));

        auto renumberedMap = renumber(normalizer);

        LLVM_DEBUG(
            renumberedMap.print(dbgs() << "\nRecursive composition yields: "));

        auxiliaryExprs.push_back(renumberedMap.getResult(0));
      } else {
        if (i < numDimsBeforeRewrite) {
          // b. The mathematical composition of AffineMap composes dims.
          auxiliaryExprs.push_back(renumberOneDim(t));
        } else {
          // c. The mathematical composition of AffineMap concatenates symbols.
          //    We do the same for symbol operands.
          concatenatedSymbols.push_back(t);
        }
      }
    }
  }

  // Early exit if `map` is already composed.
  if (auxiliaryExprs.empty()) {
    affineMap = map;
    return;
  }

  assert(concatenatedSymbols.size() >= map.getNumSymbols() &&
         "Unexpected number of concatenated symbols");
  auto numDims = dimValueToPosition.size();
  auto numSymbols = concatenatedSymbols.size() - map.getNumSymbols();
  auto auxiliaryMap = AffineMap::get(numDims, numSymbols, auxiliaryExprs);

  LLVM_DEBUG(map.print(dbgs() << "\nCompose map: "));
  LLVM_DEBUG(auxiliaryMap.print(dbgs() << "\nWith map: "));
  LLVM_DEBUG(map.compose(auxiliaryMap).print(dbgs() << "\nResult: "));

  // TODO(andydavis,ntv): Disabling simplification results in major speed gains.
  // Another option is to cache the results as it is expected a lot of redundant
  // work is performed in practice.
  affineMap = simplifyAffineMap(map.compose(auxiliaryMap));

  LLVM_DEBUG(affineMap.print(dbgs() << "\nSimplified result: "));
  LLVM_DEBUG(dbgs() << "\n");
}

void AffineApplyNormalizer::normalize(AffineMap *otherMap,
                                      SmallVectorImpl<Value> *otherOperands) {
  AffineApplyNormalizer other(*otherMap, *otherOperands);
  *otherMap = renumber(other);

  otherOperands->reserve(reorderedDims.size() + concatenatedSymbols.size());
  otherOperands->assign(reorderedDims.begin(), reorderedDims.end());
  otherOperands->append(concatenatedSymbols.begin(), concatenatedSymbols.end());
}

/// Implements `map` and `operands` composition and simplification to support
/// `makeComposedAffineApply`. This can be called to achieve the same effects
/// on `map` and `operands` without creating an AffineApplyOp that needs to be
/// immediately deleted.
static void composeAffineMapAndOperands(AffineMap *map,
                                        SmallVectorImpl<Value> *operands) {
  AffineApplyNormalizer normalizer(*map, *operands);
  auto normalizedMap = normalizer.getAffineMap();
  auto normalizedOperands = normalizer.getOperands();
  canonicalizeMapAndOperands(&normalizedMap, &normalizedOperands);
  *map = normalizedMap;
  *operands = normalizedOperands;
  assert(*map);
}

void mlir::fullyComposeAffineMapAndOperands(AffineMap *map,
                                            SmallVectorImpl<Value> *operands) {
  while (llvm::any_of(*operands, [](Value v) {
    return isa_and_nonnull<AffineApplyOp>(v.getDefiningOp());
  })) {
    composeAffineMapAndOperands(map, operands);
  }
}

AffineApplyOp mlir::makeComposedAffineApply(OpBuilder &b, Location loc,
                                            AffineMap map,
                                            ArrayRef<Value> operands) {
  AffineMap normalizedMap = map;
  SmallVector<Value, 8> normalizedOperands(operands.begin(), operands.end());
  composeAffineMapAndOperands(&normalizedMap, &normalizedOperands);
  assert(normalizedMap);
  return b.create<AffineApplyOp>(loc, normalizedMap, normalizedOperands);
}

// A symbol may appear as a dim in affine.apply operations. This function
// canonicalizes dims that are valid symbols into actual symbols.
template <class MapOrSet>
static void canonicalizePromotedSymbols(MapOrSet *mapOrSet,
                                        SmallVectorImpl<Value> *operands) {
  if (!mapOrSet || operands->empty())
    return;

  assert(mapOrSet->getNumInputs() == operands->size() &&
         "map/set inputs must match number of operands");

  auto *context = mapOrSet->getContext();
  SmallVector<Value, 8> resultOperands;
  resultOperands.reserve(operands->size());
  SmallVector<Value, 8> remappedSymbols;
  remappedSymbols.reserve(operands->size());
  unsigned nextDim = 0;
  unsigned nextSym = 0;
  unsigned oldNumSyms = mapOrSet->getNumSymbols();
  SmallVector<AffineExpr, 8> dimRemapping(mapOrSet->getNumDims());
  for (unsigned i = 0, e = mapOrSet->getNumInputs(); i != e; ++i) {
    if (i < mapOrSet->getNumDims()) {
      if (isValidSymbol((*operands)[i])) {
        // This is a valid symbol that appears as a dim, canonicalize it.
        dimRemapping[i] = getAffineSymbolExpr(oldNumSyms + nextSym++, context);
        remappedSymbols.push_back((*operands)[i]);
      } else {
        dimRemapping[i] = getAffineDimExpr(nextDim++, context);
        resultOperands.push_back((*operands)[i]);
      }
    } else {
      resultOperands.push_back((*operands)[i]);
    }
  }

  resultOperands.append(remappedSymbols.begin(), remappedSymbols.end());
  *operands = resultOperands;
  *mapOrSet = mapOrSet->replaceDimsAndSymbols(dimRemapping, {}, nextDim,
                                              oldNumSyms + nextSym);

  assert(mapOrSet->getNumInputs() == operands->size() &&
         "map/set inputs must match number of operands");
}

// Works for either an affine map or an integer set.
template <class MapOrSet>
static void canonicalizeMapOrSetAndOperands(MapOrSet *mapOrSet,
                                            SmallVectorImpl<Value> *operands) {
  static_assert(std::is_same<MapOrSet, AffineMap>::value ||
                    std::is_same<MapOrSet, IntegerSet>::value,
                "Argument must be either of AffineMap or IntegerSet type");

  if (!mapOrSet || operands->empty())
    return;

  assert(mapOrSet->getNumInputs() == operands->size() &&
         "map/set inputs must match number of operands");

  canonicalizePromotedSymbols<MapOrSet>(mapOrSet, operands);

  // Check to see what dims are used.
  llvm::SmallBitVector usedDims(mapOrSet->getNumDims());
  llvm::SmallBitVector usedSyms(mapOrSet->getNumSymbols());
  mapOrSet->walkExprs([&](AffineExpr expr) {
    if (auto dimExpr = expr.dyn_cast<AffineDimExpr>())
      usedDims[dimExpr.getPosition()] = true;
    else if (auto symExpr = expr.dyn_cast<AffineSymbolExpr>())
      usedSyms[symExpr.getPosition()] = true;
  });

  auto *context = mapOrSet->getContext();

  SmallVector<Value, 8> resultOperands;
  resultOperands.reserve(operands->size());

  llvm::SmallDenseMap<Value, AffineExpr, 8> seenDims;
  SmallVector<AffineExpr, 8> dimRemapping(mapOrSet->getNumDims());
  unsigned nextDim = 0;
  for (unsigned i = 0, e = mapOrSet->getNumDims(); i != e; ++i) {
    if (usedDims[i]) {
      // Remap dim positions for duplicate operands.
      auto it = seenDims.find((*operands)[i]);
      if (it == seenDims.end()) {
        dimRemapping[i] = getAffineDimExpr(nextDim++, context);
        resultOperands.push_back((*operands)[i]);
        seenDims.insert(std::make_pair((*operands)[i], dimRemapping[i]));
      } else {
        dimRemapping[i] = it->second;
      }
    }
  }
  llvm::SmallDenseMap<Value, AffineExpr, 8> seenSymbols;
  SmallVector<AffineExpr, 8> symRemapping(mapOrSet->getNumSymbols());
  unsigned nextSym = 0;
  for (unsigned i = 0, e = mapOrSet->getNumSymbols(); i != e; ++i) {
    if (!usedSyms[i])
      continue;
    // Handle constant operands (only needed for symbolic operands since
    // constant operands in dimensional positions would have already been
    // promoted to symbolic positions above).
    IntegerAttr operandCst;
    if (matchPattern((*operands)[i + mapOrSet->getNumDims()],
                     m_Constant(&operandCst))) {
      symRemapping[i] =
          getAffineConstantExpr(operandCst.getValue().getSExtValue(), context);
      continue;
    }
    // Remap symbol positions for duplicate operands.
    auto it = seenSymbols.find((*operands)[i + mapOrSet->getNumDims()]);
    if (it == seenSymbols.end()) {
      symRemapping[i] = getAffineSymbolExpr(nextSym++, context);
      resultOperands.push_back((*operands)[i + mapOrSet->getNumDims()]);
      seenSymbols.insert(std::make_pair((*operands)[i + mapOrSet->getNumDims()],
                                        symRemapping[i]));
    } else {
      symRemapping[i] = it->second;
    }
  }
  *mapOrSet = mapOrSet->replaceDimsAndSymbols(dimRemapping, symRemapping,
                                              nextDim, nextSym);
  *operands = resultOperands;
}

void mlir::canonicalizeMapAndOperands(AffineMap *map,
                                      SmallVectorImpl<Value> *operands) {
  canonicalizeMapOrSetAndOperands<AffineMap>(map, operands);
}

void mlir::canonicalizeSetAndOperands(IntegerSet *set,
                                      SmallVectorImpl<Value> *operands) {
  canonicalizeMapOrSetAndOperands<IntegerSet>(set, operands);
}

namespace {
/// Simplify AffineApply, AffineLoad, and AffineStore operations by composing
/// maps that supply results into them.
///
template <typename AffineOpTy>
struct SimplifyAffineOp : public OpRewritePattern<AffineOpTy> {
  using OpRewritePattern<AffineOpTy>::OpRewritePattern;

  /// Replace the affine op with another instance of it with the supplied
  /// map and mapOperands.
  void replaceAffineOp(PatternRewriter &rewriter, AffineOpTy affineOp,
                       AffineMap map, ArrayRef<Value> mapOperands) const;

  PatternMatchResult matchAndRewrite(AffineOpTy affineOp,
                                     PatternRewriter &rewriter) const override {
    static_assert(std::is_same<AffineOpTy, AffineLoadOp>::value ||
                      std::is_same<AffineOpTy, AffinePrefetchOp>::value ||
                      std::is_same<AffineOpTy, AffineStoreOp>::value ||
                      std::is_same<AffineOpTy, AffineApplyOp>::value,
                  "affine load/store/apply op expected");
    auto map = affineOp.getAffineMap();
    AffineMap oldMap = map;
    auto oldOperands = affineOp.getMapOperands();
    SmallVector<Value, 8> resultOperands(oldOperands);
    composeAffineMapAndOperands(&map, &resultOperands);
    if (map == oldMap && std::equal(oldOperands.begin(), oldOperands.end(),
                                    resultOperands.begin()))
      return this->matchFailure();

    replaceAffineOp(rewriter, affineOp, map, resultOperands);
    return this->matchSuccess();
  }
};

// Specialize the template to account for the different build signatures for
// affine load, store, and apply ops.
template <>
void SimplifyAffineOp<AffineLoadOp>::replaceAffineOp(
    PatternRewriter &rewriter, AffineLoadOp load, AffineMap map,
    ArrayRef<Value> mapOperands) const {
  rewriter.replaceOpWithNewOp<AffineLoadOp>(load, load.getMemRef(), map,
                                            mapOperands);
}
template <>
void SimplifyAffineOp<AffinePrefetchOp>::replaceAffineOp(
    PatternRewriter &rewriter, AffinePrefetchOp prefetch, AffineMap map,
    ArrayRef<Value> mapOperands) const {
  rewriter.replaceOpWithNewOp<AffinePrefetchOp>(
      prefetch, prefetch.memref(), map, mapOperands,
      prefetch.localityHint().getZExtValue(), prefetch.isWrite(),
      prefetch.isDataCache());
}
template <>
void SimplifyAffineOp<AffineStoreOp>::replaceAffineOp(
    PatternRewriter &rewriter, AffineStoreOp store, AffineMap map,
    ArrayRef<Value> mapOperands) const {
  rewriter.replaceOpWithNewOp<AffineStoreOp>(
      store, store.getValueToStore(), store.getMemRef(), map, mapOperands);
}
template <>
void SimplifyAffineOp<AffineApplyOp>::replaceAffineOp(
    PatternRewriter &rewriter, AffineApplyOp apply, AffineMap map,
    ArrayRef<Value> mapOperands) const {
  rewriter.replaceOpWithNewOp<AffineApplyOp>(apply, map, mapOperands);
}
} // end anonymous namespace.

void AffineApplyOp::getCanonicalizationPatterns(
    OwningRewritePatternList &results, MLIRContext *context) {
  results.insert<SimplifyAffineOp<AffineApplyOp>>(context);
}

//===----------------------------------------------------------------------===//
// Common canonicalization pattern support logic
//===----------------------------------------------------------------------===//

/// This is a common class used for patterns of the form
/// "someop(memrefcast) -> someop".  It folds the source of any memref_cast
/// into the root operation directly.
static LogicalResult foldMemRefCast(Operation *op) {
  bool folded = false;
  for (OpOperand &operand : op->getOpOperands()) {
    auto cast = dyn_cast_or_null<MemRefCastOp>(operand.get().getDefiningOp());
    if (cast && !cast.getOperand().getType().isa<UnrankedMemRefType>()) {
      operand.set(cast.getOperand());
      folded = true;
    }
  }
  return success(folded);
}

//===----------------------------------------------------------------------===//
// AffineDmaStartOp
//===----------------------------------------------------------------------===//

// TODO(b/133776335) Check that map operands are loop IVs or symbols.
void AffineDmaStartOp::build(Builder *builder, OperationState &result,
                             Value srcMemRef, AffineMap srcMap,
                             ValueRange srcIndices, Value destMemRef,
                             AffineMap dstMap, ValueRange destIndices,
                             Value tagMemRef, AffineMap tagMap,
                             ValueRange tagIndices, Value numElements,
                             Value stride, Value elementsPerStride) {
  result.addOperands(srcMemRef);
  result.addAttribute(getSrcMapAttrName(), AffineMapAttr::get(srcMap));
  result.addOperands(srcIndices);
  result.addOperands(destMemRef);
  result.addAttribute(getDstMapAttrName(), AffineMapAttr::get(dstMap));
  result.addOperands(destIndices);
  result.addOperands(tagMemRef);
  result.addAttribute(getTagMapAttrName(), AffineMapAttr::get(tagMap));
  result.addOperands(tagIndices);
  result.addOperands(numElements);
  if (stride) {
    result.addOperands({stride, elementsPerStride});
  }
}

void AffineDmaStartOp::print(OpAsmPrinter &p) {
  p << "affine.dma_start " << getSrcMemRef() << '[';
  p.printAffineMapOfSSAIds(getSrcMapAttr(), getSrcIndices());
  p << "], " << getDstMemRef() << '[';
  p.printAffineMapOfSSAIds(getDstMapAttr(), getDstIndices());
  p << "], " << getTagMemRef() << '[';
  p.printAffineMapOfSSAIds(getTagMapAttr(), getTagIndices());
  p << "], " << getNumElements();
  if (isStrided()) {
    p << ", " << getStride();
    p << ", " << getNumElementsPerStride();
  }
  p << " : " << getSrcMemRefType() << ", " << getDstMemRefType() << ", "
    << getTagMemRefType();
}

// Parse AffineDmaStartOp.
// Ex:
//   affine.dma_start %src[%i, %j], %dst[%k, %l], %tag[%index], %size,
//     %stride, %num_elt_per_stride
//       : memref<3076 x f32, 0>, memref<1024 x f32, 2>, memref<1 x i32>
//
ParseResult AffineDmaStartOp::parse(OpAsmParser &parser,
                                    OperationState &result) {
  OpAsmParser::OperandType srcMemRefInfo;
  AffineMapAttr srcMapAttr;
  SmallVector<OpAsmParser::OperandType, 4> srcMapOperands;
  OpAsmParser::OperandType dstMemRefInfo;
  AffineMapAttr dstMapAttr;
  SmallVector<OpAsmParser::OperandType, 4> dstMapOperands;
  OpAsmParser::OperandType tagMemRefInfo;
  AffineMapAttr tagMapAttr;
  SmallVector<OpAsmParser::OperandType, 4> tagMapOperands;
  OpAsmParser::OperandType numElementsInfo;
  SmallVector<OpAsmParser::OperandType, 2> strideInfo;

  SmallVector<Type, 3> types;
  auto indexType = parser.getBuilder().getIndexType();

  // Parse and resolve the following list of operands:
  // *) dst memref followed by its affine maps operands (in square brackets).
  // *) src memref followed by its affine map operands (in square brackets).
  // *) tag memref followed by its affine map operands (in square brackets).
  // *) number of elements transferred by DMA operation.
  if (parser.parseOperand(srcMemRefInfo) ||
      parser.parseAffineMapOfSSAIds(srcMapOperands, srcMapAttr,
                                    getSrcMapAttrName(), result.attributes) ||
      parser.parseComma() || parser.parseOperand(dstMemRefInfo) ||
      parser.parseAffineMapOfSSAIds(dstMapOperands, dstMapAttr,
                                    getDstMapAttrName(), result.attributes) ||
      parser.parseComma() || parser.parseOperand(tagMemRefInfo) ||
      parser.parseAffineMapOfSSAIds(tagMapOperands, tagMapAttr,
                                    getTagMapAttrName(), result.attributes) ||
      parser.parseComma() || parser.parseOperand(numElementsInfo))
    return failure();

  // Parse optional stride and elements per stride.
  if (parser.parseTrailingOperandList(strideInfo)) {
    return failure();
  }
  if (!strideInfo.empty() && strideInfo.size() != 2) {
    return parser.emitError(parser.getNameLoc(),
                            "expected two stride related operands");
  }
  bool isStrided = strideInfo.size() == 2;

  if (parser.parseColonTypeList(types))
    return failure();

  if (types.size() != 3)
    return parser.emitError(parser.getNameLoc(), "expected three types");

  if (parser.resolveOperand(srcMemRefInfo, types[0], result.operands) ||
      parser.resolveOperands(srcMapOperands, indexType, result.operands) ||
      parser.resolveOperand(dstMemRefInfo, types[1], result.operands) ||
      parser.resolveOperands(dstMapOperands, indexType, result.operands) ||
      parser.resolveOperand(tagMemRefInfo, types[2], result.operands) ||
      parser.resolveOperands(tagMapOperands, indexType, result.operands) ||
      parser.resolveOperand(numElementsInfo, indexType, result.operands))
    return failure();

  if (isStrided) {
    if (parser.resolveOperands(strideInfo, indexType, result.operands))
      return failure();
  }

  // Check that src/dst/tag operand counts match their map.numInputs.
  if (srcMapOperands.size() != srcMapAttr.getValue().getNumInputs() ||
      dstMapOperands.size() != dstMapAttr.getValue().getNumInputs() ||
      tagMapOperands.size() != tagMapAttr.getValue().getNumInputs())
    return parser.emitError(parser.getNameLoc(),
                            "memref operand count not equal to map.numInputs");
  return success();
}

LogicalResult AffineDmaStartOp::verify() {
  if (!getOperand(getSrcMemRefOperandIndex()).getType().isa<MemRefType>())
    return emitOpError("expected DMA source to be of memref type");
  if (!getOperand(getDstMemRefOperandIndex()).getType().isa<MemRefType>())
    return emitOpError("expected DMA destination to be of memref type");
  if (!getOperand(getTagMemRefOperandIndex()).getType().isa<MemRefType>())
    return emitOpError("expected DMA tag to be of memref type");

  // DMAs from different memory spaces supported.
  if (getSrcMemorySpace() == getDstMemorySpace()) {
    return emitOpError("DMA should be between different memory spaces");
  }
  unsigned numInputsAllMaps = getSrcMap().getNumInputs() +
                              getDstMap().getNumInputs() +
                              getTagMap().getNumInputs();
  if (getNumOperands() != numInputsAllMaps + 3 + 1 &&
      getNumOperands() != numInputsAllMaps + 3 + 1 + 2) {
    return emitOpError("incorrect number of operands");
  }

  for (auto idx : getSrcIndices()) {
    if (!idx.getType().isIndex())
      return emitOpError("src index to dma_start must have 'index' type");
    if (!isValidAffineIndexOperand(idx))
      return emitOpError("src index must be a dimension or symbol identifier");
  }
  for (auto idx : getDstIndices()) {
    if (!idx.getType().isIndex())
      return emitOpError("dst index to dma_start must have 'index' type");
    if (!isValidAffineIndexOperand(idx))
      return emitOpError("dst index must be a dimension or symbol identifier");
  }
  for (auto idx : getTagIndices()) {
    if (!idx.getType().isIndex())
      return emitOpError("tag index to dma_start must have 'index' type");
    if (!isValidAffineIndexOperand(idx))
      return emitOpError("tag index must be a dimension or symbol identifier");
  }
  return success();
}

LogicalResult AffineDmaStartOp::fold(ArrayRef<Attribute> cstOperands,
                                     SmallVectorImpl<OpFoldResult> &results) {
  /// dma_start(memrefcast) -> dma_start
  return foldMemRefCast(*this);
}

//===----------------------------------------------------------------------===//
// AffineDmaWaitOp
//===----------------------------------------------------------------------===//

// TODO(b/133776335) Check that map operands are loop IVs or symbols.
void AffineDmaWaitOp::build(Builder *builder, OperationState &result,
                            Value tagMemRef, AffineMap tagMap,
                            ValueRange tagIndices, Value numElements) {
  result.addOperands(tagMemRef);
  result.addAttribute(getTagMapAttrName(), AffineMapAttr::get(tagMap));
  result.addOperands(tagIndices);
  result.addOperands(numElements);
}

void AffineDmaWaitOp::print(OpAsmPrinter &p) {
  p << "affine.dma_wait " << getTagMemRef() << '[';
  SmallVector<Value, 2> operands(getTagIndices());
  p.printAffineMapOfSSAIds(getTagMapAttr(), operands);
  p << "], ";
  p.printOperand(getNumElements());
  p << " : " << getTagMemRef().getType();
}

// Parse AffineDmaWaitOp.
// Eg:
//   affine.dma_wait %tag[%index], %num_elements
//     : memref<1 x i32, (d0) -> (d0), 4>
//
ParseResult AffineDmaWaitOp::parse(OpAsmParser &parser,
                                   OperationState &result) {
  OpAsmParser::OperandType tagMemRefInfo;
  AffineMapAttr tagMapAttr;
  SmallVector<OpAsmParser::OperandType, 2> tagMapOperands;
  Type type;
  auto indexType = parser.getBuilder().getIndexType();
  OpAsmParser::OperandType numElementsInfo;

  // Parse tag memref, its map operands, and dma size.
  if (parser.parseOperand(tagMemRefInfo) ||
      parser.parseAffineMapOfSSAIds(tagMapOperands, tagMapAttr,
                                    getTagMapAttrName(), result.attributes) ||
      parser.parseComma() || parser.parseOperand(numElementsInfo) ||
      parser.parseColonType(type) ||
      parser.resolveOperand(tagMemRefInfo, type, result.operands) ||
      parser.resolveOperands(tagMapOperands, indexType, result.operands) ||
      parser.resolveOperand(numElementsInfo, indexType, result.operands))
    return failure();

  if (!type.isa<MemRefType>())
    return parser.emitError(parser.getNameLoc(),
                            "expected tag to be of memref type");

  if (tagMapOperands.size() != tagMapAttr.getValue().getNumInputs())
    return parser.emitError(parser.getNameLoc(),
                            "tag memref operand count != to map.numInputs");
  return success();
}

LogicalResult AffineDmaWaitOp::verify() {
  if (!getOperand(0).getType().isa<MemRefType>())
    return emitOpError("expected DMA tag to be of memref type");
  for (auto idx : getTagIndices()) {
    if (!idx.getType().isIndex())
      return emitOpError("index to dma_wait must have 'index' type");
    if (!isValidAffineIndexOperand(idx))
      return emitOpError("index must be a dimension or symbol identifier");
  }
  return success();
}

LogicalResult AffineDmaWaitOp::fold(ArrayRef<Attribute> cstOperands,
                                    SmallVectorImpl<OpFoldResult> &results) {
  /// dma_wait(memrefcast) -> dma_wait
  return foldMemRefCast(*this);
}

//===----------------------------------------------------------------------===//
// AffineForOp
//===----------------------------------------------------------------------===//

void AffineForOp::build(Builder *builder, OperationState &result,
                        ValueRange lbOperands, AffineMap lbMap,
                        ValueRange ubOperands, AffineMap ubMap, int64_t step) {
  assert(((!lbMap && lbOperands.empty()) ||
          lbOperands.size() == lbMap.getNumInputs()) &&
         "lower bound operand count does not match the affine map");
  assert(((!ubMap && ubOperands.empty()) ||
          ubOperands.size() == ubMap.getNumInputs()) &&
         "upper bound operand count does not match the affine map");
  assert(step > 0 && "step has to be a positive integer constant");

  // Add an attribute for the step.
  result.addAttribute(getStepAttrName(),
                      builder->getIntegerAttr(builder->getIndexType(), step));

  // Add the lower bound.
  result.addAttribute(getLowerBoundAttrName(), AffineMapAttr::get(lbMap));
  result.addOperands(lbOperands);

  // Add the upper bound.
  result.addAttribute(getUpperBoundAttrName(), AffineMapAttr::get(ubMap));
  result.addOperands(ubOperands);

  // Create a region and a block for the body.  The argument of the region is
  // the loop induction variable.
  Region *bodyRegion = result.addRegion();
  Block *body = new Block();
  body->addArgument(IndexType::get(builder->getContext()));
  bodyRegion->push_back(body);
  ensureTerminator(*bodyRegion, *builder, result.location);

  // Set the operands list as resizable so that we can freely modify the bounds.
  result.setOperandListToResizable();
}

void AffineForOp::build(Builder *builder, OperationState &result, int64_t lb,
                        int64_t ub, int64_t step) {
  auto lbMap = AffineMap::getConstantMap(lb, builder->getContext());
  auto ubMap = AffineMap::getConstantMap(ub, builder->getContext());
  return build(builder, result, {}, lbMap, {}, ubMap, step);
}

static LogicalResult verify(AffineForOp op) {
  // Check that the body defines as single block argument for the induction
  // variable.
  auto *body = op.getBody();
  if (body->getNumArguments() != 1 || !body->getArgument(0).getType().isIndex())
    return op.emitOpError(
        "expected body to have a single index argument for the "
        "induction variable");

  // Verify that there are enough operands for the bounds.
  AffineMap lowerBoundMap = op.getLowerBoundMap(),
            upperBoundMap = op.getUpperBoundMap();
  if (op.getNumOperands() !=
      (lowerBoundMap.getNumInputs() + upperBoundMap.getNumInputs()))
    return op.emitOpError(
        "operand count must match with affine map dimension and symbol count");

  // Verify that the bound operands are valid dimension/symbols.
  /// Lower bound.
  if (failed(verifyDimAndSymbolIdentifiers(op, op.getLowerBoundOperands(),
                                           op.getLowerBoundMap().getNumDims())))
    return failure();
  /// Upper bound.
  if (failed(verifyDimAndSymbolIdentifiers(op, op.getUpperBoundOperands(),
                                           op.getUpperBoundMap().getNumDims())))
    return failure();
  return success();
}

/// Parse a for operation loop bounds.
static ParseResult parseBound(bool isLower, OperationState &result,
                              OpAsmParser &p) {
  // 'min' / 'max' prefixes are generally syntactic sugar, but are required if
  // the map has multiple results.
  bool failedToParsedMinMax =
      failed(p.parseOptionalKeyword(isLower ? "max" : "min"));

  auto &builder = p.getBuilder();
  auto boundAttrName = isLower ? AffineForOp::getLowerBoundAttrName()
                               : AffineForOp::getUpperBoundAttrName();

  // Parse ssa-id as identity map.
  SmallVector<OpAsmParser::OperandType, 1> boundOpInfos;
  if (p.parseOperandList(boundOpInfos))
    return failure();

  if (!boundOpInfos.empty()) {
    // Check that only one operand was parsed.
    if (boundOpInfos.size() > 1)
      return p.emitError(p.getNameLoc(),
                         "expected only one loop bound operand");

    // TODO: improve error message when SSA value is not of index type.
    // Currently it is 'use of value ... expects different type than prior uses'
    if (p.resolveOperand(boundOpInfos.front(), builder.getIndexType(),
                         result.operands))
      return failure();

    // Create an identity map using symbol id. This representation is optimized
    // for storage. Analysis passes may expand it into a multi-dimensional map
    // if desired.
    AffineMap map = builder.getSymbolIdentityMap();
    result.addAttribute(boundAttrName, AffineMapAttr::get(map));
    return success();
  }

  // Get the attribute location.
  llvm::SMLoc attrLoc = p.getCurrentLocation();

  Attribute boundAttr;
  if (p.parseAttribute(boundAttr, builder.getIndexType(), boundAttrName,
                       result.attributes))
    return failure();

  // Parse full form - affine map followed by dim and symbol list.
  if (auto affineMapAttr = boundAttr.dyn_cast<AffineMapAttr>()) {
    unsigned currentNumOperands = result.operands.size();
    unsigned numDims;
    if (parseDimAndSymbolList(p, result.operands, numDims))
      return failure();

    auto map = affineMapAttr.getValue();
    if (map.getNumDims() != numDims)
      return p.emitError(
          p.getNameLoc(),
          "dim operand count and integer set dim count must match");

    unsigned numDimAndSymbolOperands =
        result.operands.size() - currentNumOperands;
    if (numDims + map.getNumSymbols() != numDimAndSymbolOperands)
      return p.emitError(
          p.getNameLoc(),
          "symbol operand count and integer set symbol count must match");

    // If the map has multiple results, make sure that we parsed the min/max
    // prefix.
    if (map.getNumResults() > 1 && failedToParsedMinMax) {
      if (isLower) {
        return p.emitError(attrLoc, "lower loop bound affine map with "
                                    "multiple results requires 'max' prefix");
      }
      return p.emitError(attrLoc, "upper loop bound affine map with multiple "
                                  "results requires 'min' prefix");
    }
    return success();
  }

  // Parse custom assembly form.
  if (auto integerAttr = boundAttr.dyn_cast<IntegerAttr>()) {
    result.attributes.pop_back();
    result.addAttribute(
        boundAttrName,
        AffineMapAttr::get(builder.getConstantAffineMap(integerAttr.getInt())));
    return success();
  }

  return p.emitError(
      p.getNameLoc(),
      "expected valid affine map representation for loop bounds");
}

static ParseResult parseAffineForOp(OpAsmParser &parser,
                                    OperationState &result) {
  auto &builder = parser.getBuilder();
  OpAsmParser::OperandType inductionVariable;
  // Parse the induction variable followed by '='.
  if (parser.parseRegionArgument(inductionVariable) || parser.parseEqual())
    return failure();

  // Parse loop bounds.
  if (parseBound(/*isLower=*/true, result, parser) ||
      parser.parseKeyword("to", " between bounds") ||
      parseBound(/*isLower=*/false, result, parser))
    return failure();

  // Parse the optional loop step, we default to 1 if one is not present.
  if (parser.parseOptionalKeyword("step")) {
    result.addAttribute(
        AffineForOp::getStepAttrName(),
        builder.getIntegerAttr(builder.getIndexType(), /*value=*/1));
  } else {
    llvm::SMLoc stepLoc = parser.getCurrentLocation();
    IntegerAttr stepAttr;
    if (parser.parseAttribute(stepAttr, builder.getIndexType(),
                              AffineForOp::getStepAttrName().data(),
                              result.attributes))
      return failure();

    if (stepAttr.getValue().getSExtValue() < 0)
      return parser.emitError(
          stepLoc,
          "expected step to be representable as a positive signed integer");
  }

  // Parse the body region.
  Region *body = result.addRegion();
  if (parser.parseRegion(*body, inductionVariable, builder.getIndexType()))
    return failure();

  AffineForOp::ensureTerminator(*body, builder, result.location);

  // Parse the optional attribute list.
  if (parser.parseOptionalAttrDict(result.attributes))
    return failure();

  // Set the operands list as resizable so that we can freely modify the bounds.
  result.setOperandListToResizable();
  return success();
}

static void printBound(AffineMapAttr boundMap,
                       Operation::operand_range boundOperands,
                       const char *prefix, OpAsmPrinter &p) {
  AffineMap map = boundMap.getValue();

  // Check if this bound should be printed using custom assembly form.
  // The decision to restrict printing custom assembly form to trivial cases
  // comes from the will to roundtrip MLIR binary -> text -> binary in a
  // lossless way.
  // Therefore, custom assembly form parsing and printing is only supported for
  // zero-operand constant maps and single symbol operand identity maps.
  if (map.getNumResults() == 1) {
    AffineExpr expr = map.getResult(0);

    // Print constant bound.
    if (map.getNumDims() == 0 && map.getNumSymbols() == 0) {
      if (auto constExpr = expr.dyn_cast<AffineConstantExpr>()) {
        p << constExpr.getValue();
        return;
      }
    }

    // Print bound that consists of a single SSA symbol if the map is over a
    // single symbol.
    if (map.getNumDims() == 0 && map.getNumSymbols() == 1) {
      if (auto symExpr = expr.dyn_cast<AffineSymbolExpr>()) {
        p.printOperand(*boundOperands.begin());
        return;
      }
    }
  } else {
    // Map has multiple results. Print 'min' or 'max' prefix.
    p << prefix << ' ';
  }

  // Print the map and its operands.
  p << boundMap;
  printDimAndSymbolList(boundOperands.begin(), boundOperands.end(),
                        map.getNumDims(), p);
}

static void print(OpAsmPrinter &p, AffineForOp op) {
  p << op.getOperationName() << ' ';
  p.printOperand(op.getBody()->getArgument(0));
  p << " = ";
  printBound(op.getLowerBoundMapAttr(), op.getLowerBoundOperands(), "max", p);
  p << " to ";
  printBound(op.getUpperBoundMapAttr(), op.getUpperBoundOperands(), "min", p);

  if (op.getStep() != 1)
    p << " step " << op.getStep();
  p.printRegion(op.region(),
                /*printEntryBlockArgs=*/false,
                /*printBlockTerminators=*/false);
  p.printOptionalAttrDict(op.getAttrs(),
                          /*elidedAttrs=*/{op.getLowerBoundAttrName(),
                                           op.getUpperBoundAttrName(),
                                           op.getStepAttrName()});
}

/// Fold the constant bounds of a loop.
static LogicalResult foldLoopBounds(AffineForOp forOp) {
  auto foldLowerOrUpperBound = [&forOp](bool lower) {
    // Check to see if each of the operands is the result of a constant.  If
    // so, get the value.  If not, ignore it.
    SmallVector<Attribute, 8> operandConstants;
    auto boundOperands =
        lower ? forOp.getLowerBoundOperands() : forOp.getUpperBoundOperands();
    for (auto operand : boundOperands) {
      Attribute operandCst;
      matchPattern(operand, m_Constant(&operandCst));
      operandConstants.push_back(operandCst);
    }

    AffineMap boundMap =
        lower ? forOp.getLowerBoundMap() : forOp.getUpperBoundMap();
    assert(boundMap.getNumResults() >= 1 &&
           "bound maps should have at least one result");
    SmallVector<Attribute, 4> foldedResults;
    if (failed(boundMap.constantFold(operandConstants, foldedResults)))
      return failure();

    // Compute the max or min as applicable over the results.
    assert(!foldedResults.empty() && "bounds should have at least one result");
    auto maxOrMin = foldedResults[0].cast<IntegerAttr>().getValue();
    for (unsigned i = 1, e = foldedResults.size(); i < e; i++) {
      auto foldedResult = foldedResults[i].cast<IntegerAttr>().getValue();
      maxOrMin = lower ? llvm::APIntOps::smax(maxOrMin, foldedResult)
                       : llvm::APIntOps::smin(maxOrMin, foldedResult);
    }
    lower ? forOp.setConstantLowerBound(maxOrMin.getSExtValue())
          : forOp.setConstantUpperBound(maxOrMin.getSExtValue());
    return success();
  };

  // Try to fold the lower bound.
  bool folded = false;
  if (!forOp.hasConstantLowerBound())
    folded |= succeeded(foldLowerOrUpperBound(/*lower=*/true));

  // Try to fold the upper bound.
  if (!forOp.hasConstantUpperBound())
    folded |= succeeded(foldLowerOrUpperBound(/*lower=*/false));
  return success(folded);
}

/// Canonicalize the bounds of the given loop.
static LogicalResult canonicalizeLoopBounds(AffineForOp forOp) {
  SmallVector<Value, 4> lbOperands(forOp.getLowerBoundOperands());
  SmallVector<Value, 4> ubOperands(forOp.getUpperBoundOperands());

  auto lbMap = forOp.getLowerBoundMap();
  auto ubMap = forOp.getUpperBoundMap();
  auto prevLbMap = lbMap;
  auto prevUbMap = ubMap;

  canonicalizeMapAndOperands(&lbMap, &lbOperands);
  canonicalizeMapAndOperands(&ubMap, &ubOperands);

  // Any canonicalization change always leads to updated map(s).
  if (lbMap == prevLbMap && ubMap == prevUbMap)
    return failure();

  if (lbMap != prevLbMap)
    forOp.setLowerBound(lbOperands, lbMap);
  if (ubMap != prevUbMap)
    forOp.setUpperBound(ubOperands, ubMap);
  return success();
}

namespace {
/// This is a pattern to fold trivially empty loops.
struct AffineForEmptyLoopFolder : public OpRewritePattern<AffineForOp> {
  using OpRewritePattern<AffineForOp>::OpRewritePattern;

  PatternMatchResult matchAndRewrite(AffineForOp forOp,
                                     PatternRewriter &rewriter) const override {
    // Check that the body only contains a terminator.
    if (!has_single_element(*forOp.getBody()))
      return matchFailure();
    rewriter.eraseOp(forOp);
    return matchSuccess();
  }
};
} // end anonymous namespace

void AffineForOp::getCanonicalizationPatterns(OwningRewritePatternList &results,
                                              MLIRContext *context) {
  results.insert<AffineForEmptyLoopFolder>(context);
}

LogicalResult AffineForOp::fold(ArrayRef<Attribute> operands,
                                SmallVectorImpl<OpFoldResult> &results) {
  bool folded = succeeded(foldLoopBounds(*this));
  folded |= succeeded(canonicalizeLoopBounds(*this));
  return success(folded);
}

AffineBound AffineForOp::getLowerBound() {
  auto lbMap = getLowerBoundMap();
  return AffineBound(AffineForOp(*this), 0, lbMap.getNumInputs(), lbMap);
}

AffineBound AffineForOp::getUpperBound() {
  auto lbMap = getLowerBoundMap();
  auto ubMap = getUpperBoundMap();
  return AffineBound(AffineForOp(*this), lbMap.getNumInputs(), getNumOperands(),
                     ubMap);
}

void AffineForOp::setLowerBound(ValueRange lbOperands, AffineMap map) {
  assert(lbOperands.size() == map.getNumInputs());
  assert(map.getNumResults() >= 1 && "bound map has at least one result");

  SmallVector<Value, 4> newOperands(lbOperands.begin(), lbOperands.end());

  auto ubOperands = getUpperBoundOperands();
  newOperands.append(ubOperands.begin(), ubOperands.end());
  getOperation()->setOperands(newOperands);

  setAttr(getLowerBoundAttrName(), AffineMapAttr::get(map));
}

void AffineForOp::setUpperBound(ValueRange ubOperands, AffineMap map) {
  assert(ubOperands.size() == map.getNumInputs());
  assert(map.getNumResults() >= 1 && "bound map has at least one result");

  SmallVector<Value, 4> newOperands(getLowerBoundOperands());
  newOperands.append(ubOperands.begin(), ubOperands.end());
  getOperation()->setOperands(newOperands);

  setAttr(getUpperBoundAttrName(), AffineMapAttr::get(map));
}

void AffineForOp::setLowerBoundMap(AffineMap map) {
  auto lbMap = getLowerBoundMap();
  assert(lbMap.getNumDims() == map.getNumDims() &&
         lbMap.getNumSymbols() == map.getNumSymbols());
  assert(map.getNumResults() >= 1 && "bound map has at least one result");
  (void)lbMap;
  setAttr(getLowerBoundAttrName(), AffineMapAttr::get(map));
}

void AffineForOp::setUpperBoundMap(AffineMap map) {
  auto ubMap = getUpperBoundMap();
  assert(ubMap.getNumDims() == map.getNumDims() &&
         ubMap.getNumSymbols() == map.getNumSymbols());
  assert(map.getNumResults() >= 1 && "bound map has at least one result");
  (void)ubMap;
  setAttr(getUpperBoundAttrName(), AffineMapAttr::get(map));
}

bool AffineForOp::hasConstantLowerBound() {
  return getLowerBoundMap().isSingleConstant();
}

bool AffineForOp::hasConstantUpperBound() {
  return getUpperBoundMap().isSingleConstant();
}

int64_t AffineForOp::getConstantLowerBound() {
  return getLowerBoundMap().getSingleConstantResult();
}

int64_t AffineForOp::getConstantUpperBound() {
  return getUpperBoundMap().getSingleConstantResult();
}

void AffineForOp::setConstantLowerBound(int64_t value) {
  setLowerBound({}, AffineMap::getConstantMap(value, getContext()));
}

void AffineForOp::setConstantUpperBound(int64_t value) {
  setUpperBound({}, AffineMap::getConstantMap(value, getContext()));
}

AffineForOp::operand_range AffineForOp::getLowerBoundOperands() {
  return {operand_begin(), operand_begin() + getLowerBoundMap().getNumInputs()};
}

AffineForOp::operand_range AffineForOp::getUpperBoundOperands() {
  return {operand_begin() + getLowerBoundMap().getNumInputs(), operand_end()};
}

bool AffineForOp::matchingBoundOperandList() {
  auto lbMap = getLowerBoundMap();
  auto ubMap = getUpperBoundMap();
  if (lbMap.getNumDims() != ubMap.getNumDims() ||
      lbMap.getNumSymbols() != ubMap.getNumSymbols())
    return false;

  unsigned numOperands = lbMap.getNumInputs();
  for (unsigned i = 0, e = lbMap.getNumInputs(); i < e; i++) {
    // Compare Value 's.
    if (getOperand(i) != getOperand(numOperands + i))
      return false;
  }
  return true;
}

Region &AffineForOp::getLoopBody() { return region(); }

bool AffineForOp::isDefinedOutsideOfLoop(Value value) {
  return !region().isAncestor(value.getParentRegion());
}

LogicalResult AffineForOp::moveOutOfLoop(ArrayRef<Operation *> ops) {
  for (auto *op : ops)
    op->moveBefore(*this);
  return success();
}

/// Returns if the provided value is the induction variable of a AffineForOp.
bool mlir::isForInductionVar(Value val) {
  return getForInductionVarOwner(val) != AffineForOp();
}

/// Returns the loop parent of an induction variable. If the provided value is
/// not an induction variable, then return nullptr.
AffineForOp mlir::getForInductionVarOwner(Value val) {
  auto ivArg = val.dyn_cast<BlockArgument>();
  if (!ivArg || !ivArg.getOwner())
    return AffineForOp();
  auto *containingInst = ivArg.getOwner()->getParent()->getParentOp();
  return dyn_cast<AffineForOp>(containingInst);
}

/// Extracts the induction variables from a list of AffineForOps and returns
/// them.
void mlir::extractForInductionVars(ArrayRef<AffineForOp> forInsts,
                                   SmallVectorImpl<Value> *ivs) {
  ivs->reserve(forInsts.size());
  for (auto forInst : forInsts)
    ivs->push_back(forInst.getInductionVar());
}

//===----------------------------------------------------------------------===//
// AffineIfOp
//===----------------------------------------------------------------------===//

static LogicalResult verify(AffineIfOp op) {
  // Verify that we have a condition attribute.
  auto conditionAttr =
      op.getAttrOfType<IntegerSetAttr>(op.getConditionAttrName());
  if (!conditionAttr)
    return op.emitOpError(
        "requires an integer set attribute named 'condition'");

  // Verify that there are enough operands for the condition.
  IntegerSet condition = conditionAttr.getValue();
  if (op.getNumOperands() != condition.getNumInputs())
    return op.emitOpError(
        "operand count and condition integer set dimension and "
        "symbol count must match");

  // Verify that the operands are valid dimension/symbols.
  if (failed(verifyDimAndSymbolIdentifiers(
          op, op.getOperation()->getNonSuccessorOperands(),
          condition.getNumDims())))
    return failure();

  // Verify that the entry of each child region does not have arguments.
  for (auto &region : op.getOperation()->getRegions()) {
    for (auto &b : region)
      if (b.getNumArguments() != 0)
        return op.emitOpError(
            "requires that child entry blocks have no arguments");
  }
  return success();
}

static ParseResult parseAffineIfOp(OpAsmParser &parser,
                                   OperationState &result) {
  // Parse the condition attribute set.
  IntegerSetAttr conditionAttr;
  unsigned numDims;
  if (parser.parseAttribute(conditionAttr, AffineIfOp::getConditionAttrName(),
                            result.attributes) ||
      parseDimAndSymbolList(parser, result.operands, numDims))
    return failure();

  // Verify the condition operands.
  auto set = conditionAttr.getValue();
  if (set.getNumDims() != numDims)
    return parser.emitError(
        parser.getNameLoc(),
        "dim operand count and integer set dim count must match");
  if (numDims + set.getNumSymbols() != result.operands.size())
    return parser.emitError(
        parser.getNameLoc(),
        "symbol operand count and integer set symbol count must match");

  // Create the regions for 'then' and 'else'.  The latter must be created even
  // if it remains empty for the validity of the operation.
  result.regions.reserve(2);
  Region *thenRegion = result.addRegion();
  Region *elseRegion = result.addRegion();

  // Parse the 'then' region.
  if (parser.parseRegion(*thenRegion, {}, {}))
    return failure();
  AffineIfOp::ensureTerminator(*thenRegion, parser.getBuilder(),
                               result.location);

  // If we find an 'else' keyword then parse the 'else' region.
  if (!parser.parseOptionalKeyword("else")) {
    if (parser.parseRegion(*elseRegion, {}, {}))
      return failure();
    AffineIfOp::ensureTerminator(*elseRegion, parser.getBuilder(),
                                 result.location);
  }

  // Parse the optional attribute list.
  if (parser.parseOptionalAttrDict(result.attributes))
    return failure();

  return success();
}

static void print(OpAsmPrinter &p, AffineIfOp op) {
  auto conditionAttr =
      op.getAttrOfType<IntegerSetAttr>(op.getConditionAttrName());
  p << "affine.if " << conditionAttr;
  printDimAndSymbolList(op.operand_begin(), op.operand_end(),
                        conditionAttr.getValue().getNumDims(), p);
  p.printRegion(op.thenRegion(),
                /*printEntryBlockArgs=*/false,
                /*printBlockTerminators=*/false);

  // Print the 'else' regions if it has any blocks.
  auto &elseRegion = op.elseRegion();
  if (!elseRegion.empty()) {
    p << " else";
    p.printRegion(elseRegion,
                  /*printEntryBlockArgs=*/false,
                  /*printBlockTerminators=*/false);
  }

  // Print the attribute list.
  p.printOptionalAttrDict(op.getAttrs(),
                          /*elidedAttrs=*/op.getConditionAttrName());
}

IntegerSet AffineIfOp::getIntegerSet() {
  return getAttrOfType<IntegerSetAttr>(getConditionAttrName()).getValue();
}
void AffineIfOp::setIntegerSet(IntegerSet newSet) {
  setAttr(getConditionAttrName(), IntegerSetAttr::get(newSet));
}

void AffineIfOp::setConditional(IntegerSet set, ValueRange operands) {
  setIntegerSet(set);
  getOperation()->setOperands(operands);
}

void AffineIfOp::build(Builder *builder, OperationState &result, IntegerSet set,
                       ValueRange args, bool withElseRegion) {
  result.addOperands(args);
  result.addAttribute(getConditionAttrName(), IntegerSetAttr::get(set));
  Region *thenRegion = result.addRegion();
  Region *elseRegion = result.addRegion();
  AffineIfOp::ensureTerminator(*thenRegion, *builder, result.location);
  if (withElseRegion)
    AffineIfOp::ensureTerminator(*elseRegion, *builder, result.location);
}

/// Canonicalize an affine if op's conditional (integer set + operands).
LogicalResult AffineIfOp::fold(ArrayRef<Attribute>,
                               SmallVectorImpl<OpFoldResult> &) {
  auto set = getIntegerSet();
  SmallVector<Value, 4> operands(getOperands());
  canonicalizeSetAndOperands(&set, &operands);

  // Any canonicalization change always leads to either a reduction in the
  // number of operands or a change in the number of symbolic operands
  // (promotion of dims to symbols).
  if (operands.size() < getIntegerSet().getNumInputs() ||
      set.getNumSymbols() > getIntegerSet().getNumSymbols()) {
    setConditional(set, operands);
    return success();
  }

  return failure();
}

//===----------------------------------------------------------------------===//
// AffineLoadOp
//===----------------------------------------------------------------------===//

void AffineLoadOp::build(Builder *builder, OperationState &result,
                         AffineMap map, ValueRange operands) {
  assert(operands.size() == 1 + map.getNumInputs() && "inconsistent operands");
  result.addOperands(operands);
  if (map)
    result.addAttribute(getMapAttrName(), AffineMapAttr::get(map));
  auto memrefType = operands[0].getType().cast<MemRefType>();
  result.types.push_back(memrefType.getElementType());
}

void AffineLoadOp::build(Builder *builder, OperationState &result, Value memref,
                         AffineMap map, ValueRange mapOperands) {
  assert(map.getNumInputs() == mapOperands.size() && "inconsistent index info");
  result.addOperands(memref);
  result.addOperands(mapOperands);
  auto memrefType = memref.getType().cast<MemRefType>();
  result.addAttribute(getMapAttrName(), AffineMapAttr::get(map));
  result.types.push_back(memrefType.getElementType());
}

void AffineLoadOp::build(Builder *builder, OperationState &result, Value memref,
                         ValueRange indices) {
  auto memrefType = memref.getType().cast<MemRefType>();
  auto rank = memrefType.getRank();
  // Create identity map for memrefs with at least one dimension or () -> ()
  // for zero-dimensional memrefs.
  auto map = rank ? builder->getMultiDimIdentityMap(rank)
                  : builder->getEmptyAffineMap();
  build(builder, result, memref, map, indices);
}

ParseResult AffineLoadOp::parse(OpAsmParser &parser, OperationState &result) {
  auto &builder = parser.getBuilder();
  auto indexTy = builder.getIndexType();

  MemRefType type;
  OpAsmParser::OperandType memrefInfo;
  AffineMapAttr mapAttr;
  SmallVector<OpAsmParser::OperandType, 1> mapOperands;
  return failure(
      parser.parseOperand(memrefInfo) ||
      parser.parseAffineMapOfSSAIds(mapOperands, mapAttr, getMapAttrName(),
                                    result.attributes) ||
      parser.parseOptionalAttrDict(result.attributes) ||
      parser.parseColonType(type) ||
      parser.resolveOperand(memrefInfo, type, result.operands) ||
      parser.resolveOperands(mapOperands, indexTy, result.operands) ||
      parser.addTypeToList(type.getElementType(), result.types));
}

void AffineLoadOp::print(OpAsmPrinter &p) {
  p << "affine.load " << getMemRef() << '[';
  if (AffineMapAttr mapAttr = getAttrOfType<AffineMapAttr>(getMapAttrName()))
    p.printAffineMapOfSSAIds(mapAttr, getMapOperands());
  p << ']';
  p.printOptionalAttrDict(getAttrs(), /*elidedAttrs=*/{getMapAttrName()});
  p << " : " << getMemRefType();
}

LogicalResult AffineLoadOp::verify() {
  if (getType() != getMemRefType().getElementType())
    return emitOpError("result type must match element type of memref");

  auto mapAttr = getAttrOfType<AffineMapAttr>(getMapAttrName());
  if (mapAttr) {
    AffineMap map = getAttrOfType<AffineMapAttr>(getMapAttrName()).getValue();
    if (map.getNumResults() != getMemRefType().getRank())
      return emitOpError("affine.load affine map num results must equal"
                         " memref rank");
    if (map.getNumInputs() != getNumOperands() - 1)
      return emitOpError("expects as many subscripts as affine map inputs");
  } else {
    if (getMemRefType().getRank() != getNumOperands() - 1)
      return emitOpError(
          "expects the number of subscripts to be equal to memref rank");
  }

  for (auto idx : getMapOperands()) {
    if (!idx.getType().isIndex())
      return emitOpError("index to load must have 'index' type");
    if (!isValidAffineIndexOperand(idx))
      return emitOpError("index must be a dimension or symbol identifier");
  }
  return success();
}

void AffineLoadOp::getCanonicalizationPatterns(
    OwningRewritePatternList &results, MLIRContext *context) {
  results.insert<SimplifyAffineOp<AffineLoadOp>>(context);
}

OpFoldResult AffineLoadOp::fold(ArrayRef<Attribute> cstOperands) {
  /// load(memrefcast) -> load
  if (succeeded(foldMemRefCast(*this)))
    return getResult();
  return OpFoldResult();
}

//===----------------------------------------------------------------------===//
// AffineStoreOp
//===----------------------------------------------------------------------===//

void AffineStoreOp::build(Builder *builder, OperationState &result,
                          Value valueToStore, Value memref, AffineMap map,
                          ValueRange mapOperands) {
  assert(map.getNumInputs() == mapOperands.size() && "inconsistent index info");
  result.addOperands(valueToStore);
  result.addOperands(memref);
  result.addOperands(mapOperands);
  result.addAttribute(getMapAttrName(), AffineMapAttr::get(map));
}

// Use identity map.
void AffineStoreOp::build(Builder *builder, OperationState &result,
                          Value valueToStore, Value memref,
                          ValueRange indices) {
  auto memrefType = memref.getType().cast<MemRefType>();
  auto rank = memrefType.getRank();
  // Create identity map for memrefs with at least one dimension or () -> ()
  // for zero-dimensional memrefs.
  auto map = rank ? builder->getMultiDimIdentityMap(rank)
                  : builder->getEmptyAffineMap();
  build(builder, result, valueToStore, memref, map, indices);
}

ParseResult AffineStoreOp::parse(OpAsmParser &parser, OperationState &result) {
  auto indexTy = parser.getBuilder().getIndexType();

  MemRefType type;
  OpAsmParser::OperandType storeValueInfo;
  OpAsmParser::OperandType memrefInfo;
  AffineMapAttr mapAttr;
  SmallVector<OpAsmParser::OperandType, 1> mapOperands;
  return failure(parser.parseOperand(storeValueInfo) || parser.parseComma() ||
                 parser.parseOperand(memrefInfo) ||
                 parser.parseAffineMapOfSSAIds(mapOperands, mapAttr,
                                               getMapAttrName(),
                                               result.attributes) ||
                 parser.parseOptionalAttrDict(result.attributes) ||
                 parser.parseColonType(type) ||
                 parser.resolveOperand(storeValueInfo, type.getElementType(),
                                       result.operands) ||
                 parser.resolveOperand(memrefInfo, type, result.operands) ||
                 parser.resolveOperands(mapOperands, indexTy, result.operands));
}

void AffineStoreOp::print(OpAsmPrinter &p) {
  p << "affine.store " << getValueToStore();
  p << ", " << getMemRef() << '[';
  if (AffineMapAttr mapAttr = getAttrOfType<AffineMapAttr>(getMapAttrName()))
    p.printAffineMapOfSSAIds(mapAttr, getMapOperands());
  p << ']';
  p.printOptionalAttrDict(getAttrs(), /*elidedAttrs=*/{getMapAttrName()});
  p << " : " << getMemRefType();
}

LogicalResult AffineStoreOp::verify() {
  // First operand must have same type as memref element type.
  if (getValueToStore().getType() != getMemRefType().getElementType())
    return emitOpError("first operand must have same type memref element type");

  auto mapAttr = getAttrOfType<AffineMapAttr>(getMapAttrName());
  if (mapAttr) {
    AffineMap map = mapAttr.getValue();
    if (map.getNumResults() != getMemRefType().getRank())
      return emitOpError("affine.store affine map num results must equal"
                         " memref rank");
    if (map.getNumInputs() != getNumOperands() - 2)
      return emitOpError("expects as many subscripts as affine map inputs");
  } else {
    if (getMemRefType().getRank() != getNumOperands() - 2)
      return emitOpError(
          "expects the number of subscripts to be equal to memref rank");
  }

  for (auto idx : getMapOperands()) {
    if (!idx.getType().isIndex())
      return emitOpError("index to store must have 'index' type");
    if (!isValidAffineIndexOperand(idx))
      return emitOpError("index must be a dimension or symbol identifier");
  }
  return success();
}

void AffineStoreOp::getCanonicalizationPatterns(
    OwningRewritePatternList &results, MLIRContext *context) {
  results.insert<SimplifyAffineOp<AffineStoreOp>>(context);
}

LogicalResult AffineStoreOp::fold(ArrayRef<Attribute> cstOperands,
                                  SmallVectorImpl<OpFoldResult> &results) {
  /// store(memrefcast) -> store
  return foldMemRefCast(*this);
}

//===----------------------------------------------------------------------===//
// AffineMinMaxOpBase
//===----------------------------------------------------------------------===//

template <typename T>
static LogicalResult verifyAffineMinMaxOp(T op) {
  // Verify that operand count matches affine map dimension and symbol count.
  if (op.getNumOperands() != op.map().getNumDims() + op.map().getNumSymbols())
    return op.emitOpError(
        "operand count and affine map dimension and symbol count must match");
  return success();
}

template <typename T>
static void printAffineMinMaxOp(OpAsmPrinter &p, T op) {
  p << op.getOperationName() << ' ' << op.getAttr(T::getMapAttrName());
  auto operands = op.getOperands();
  unsigned numDims = op.map().getNumDims();
  p << '(' << operands.take_front(numDims) << ')';

  if (operands.size() != numDims)
    p << '[' << operands.drop_front(numDims) << ']';
  p.printOptionalAttrDict(op.getAttrs(),
                          /*elidedAttrs=*/{T::getMapAttrName()});
}

template <typename T>
static ParseResult parseAffineMinMaxOp(OpAsmParser &parser,
                                       OperationState &result) {
  auto &builder = parser.getBuilder();
  auto indexType = builder.getIndexType();
  SmallVector<OpAsmParser::OperandType, 8> dim_infos;
  SmallVector<OpAsmParser::OperandType, 8> sym_infos;
  AffineMapAttr mapAttr;
  return failure(
      parser.parseAttribute(mapAttr, T::getMapAttrName(), result.attributes) ||
      parser.parseOperandList(dim_infos, OpAsmParser::Delimiter::Paren) ||
      parser.parseOperandList(sym_infos,
                              OpAsmParser::Delimiter::OptionalSquare) ||
      parser.parseOptionalAttrDict(result.attributes) ||
      parser.resolveOperands(dim_infos, indexType, result.operands) ||
      parser.resolveOperands(sym_infos, indexType, result.operands) ||
      parser.addTypeToList(indexType, result.types));
}

//===----------------------------------------------------------------------===//
// AffineMinOp
//===----------------------------------------------------------------------===//
//
//   %0 = affine.min (d0) -> (1000, d0 + 512) (%i0)
//

OpFoldResult AffineMinOp::fold(ArrayRef<Attribute> operands) {
  // Fold the affine map.
  // TODO(andydavis, ntv) Fold more cases: partial static information,
  // min(some_affine, some_affine + constant, ...).
  SmallVector<Attribute, 2> results;
  if (failed(map().constantFold(operands, results)))
    return {};

  // Compute and return min of folded map results.
  int64_t min = std::numeric_limits<int64_t>::max();
  int minIndex = -1;
  for (unsigned i = 0, e = results.size(); i < e; ++i) {
    auto intAttr = results[i].cast<IntegerAttr>();
    if (intAttr.getInt() < min) {
      min = intAttr.getInt();
      minIndex = i;
    }
  }
  if (minIndex < 0)
    return {};
  return results[minIndex];
}

//===----------------------------------------------------------------------===//
// AffineMaxOp
//===----------------------------------------------------------------------===//
//
//   %0 = affine.max (d0) -> (1000, d0 + 512) (%i0)
//

OpFoldResult AffineMaxOp::fold(ArrayRef<Attribute> operands) {
  // Fold the affine map.
  // TODO(andydavis, ntv, ouhang) Fold more cases: partial static information,
  // max(some_affine, some_affine + constant, ...).
  SmallVector<Attribute, 2> results;
  if (failed(map().constantFold(operands, results)))
    return {};

  // Compute and return max of folded map results.
  int64_t max = std::numeric_limits<int64_t>::min();
  int maxIndex = -1;
  for (unsigned i = 0, e = results.size(); i < e; ++i) {
    auto intAttr = results[i].cast<IntegerAttr>();
    if (intAttr.getInt() > max) {
      max = intAttr.getInt();
      maxIndex = i;
    }
  }
  if (maxIndex < 0)
    return {};
  return results[maxIndex];
}

//===----------------------------------------------------------------------===//
// AffinePrefetchOp
//===----------------------------------------------------------------------===//

//
// affine.prefetch %0[%i, %j + 5], read, locality<3>, data : memref<400x400xi32>
//
static ParseResult parseAffinePrefetchOp(OpAsmParser &parser,
                                         OperationState &result) {
  auto &builder = parser.getBuilder();
  auto indexTy = builder.getIndexType();

  MemRefType type;
  OpAsmParser::OperandType memrefInfo;
  IntegerAttr hintInfo;
  auto i32Type = parser.getBuilder().getIntegerType(32);
  StringRef readOrWrite, cacheType;

  AffineMapAttr mapAttr;
  SmallVector<OpAsmParser::OperandType, 1> mapOperands;
  if (parser.parseOperand(memrefInfo) ||
      parser.parseAffineMapOfSSAIds(mapOperands, mapAttr,
                                    AffinePrefetchOp::getMapAttrName(),
                                    result.attributes) ||
      parser.parseComma() || parser.parseKeyword(&readOrWrite) ||
      parser.parseComma() || parser.parseKeyword("locality") ||
      parser.parseLess() ||
      parser.parseAttribute(hintInfo, i32Type,
                            AffinePrefetchOp::getLocalityHintAttrName(),
                            result.attributes) ||
      parser.parseGreater() || parser.parseComma() ||
      parser.parseKeyword(&cacheType) ||
      parser.parseOptionalAttrDict(result.attributes) ||
      parser.parseColonType(type) ||
      parser.resolveOperand(memrefInfo, type, result.operands) ||
      parser.resolveOperands(mapOperands, indexTy, result.operands))
    return failure();

  if (!readOrWrite.equals("read") && !readOrWrite.equals("write"))
    return parser.emitError(parser.getNameLoc(),
                            "rw specifier has to be 'read' or 'write'");
  result.addAttribute(
      AffinePrefetchOp::getIsWriteAttrName(),
      parser.getBuilder().getBoolAttr(readOrWrite.equals("write")));

  if (!cacheType.equals("data") && !cacheType.equals("instr"))
    return parser.emitError(parser.getNameLoc(),
                            "cache type has to be 'data' or 'instr'");

  result.addAttribute(
      AffinePrefetchOp::getIsDataCacheAttrName(),
      parser.getBuilder().getBoolAttr(cacheType.equals("data")));

  return success();
}

static void print(OpAsmPrinter &p, AffinePrefetchOp op) {
  p << AffinePrefetchOp::getOperationName() << " " << op.memref() << '[';
  AffineMapAttr mapAttr = op.getAttrOfType<AffineMapAttr>(op.getMapAttrName());
  if (mapAttr) {
    SmallVector<Value, 2> operands(op.getMapOperands());
    p.printAffineMapOfSSAIds(mapAttr, operands);
  }
  p << ']' << ", " << (op.isWrite() ? "write" : "read") << ", "
    << "locality<" << op.localityHint() << ">, "
    << (op.isDataCache() ? "data" : "instr");
  p.printOptionalAttrDict(
      op.getAttrs(),
      /*elidedAttrs=*/{op.getMapAttrName(), op.getLocalityHintAttrName(),
                       op.getIsDataCacheAttrName(), op.getIsWriteAttrName()});
  p << " : " << op.getMemRefType();
}

static LogicalResult verify(AffinePrefetchOp op) {
  auto mapAttr = op.getAttrOfType<AffineMapAttr>(op.getMapAttrName());
  if (mapAttr) {
    AffineMap map = mapAttr.getValue();
    if (map.getNumResults() != op.getMemRefType().getRank())
      return op.emitOpError("affine.prefetch affine map num results must equal"
                            " memref rank");
    if (map.getNumInputs() + 1 != op.getNumOperands())
      return op.emitOpError("too few operands");
  } else {
    if (op.getNumOperands() != 1)
      return op.emitOpError("too few operands");
  }

  for (auto idx : op.getMapOperands()) {
    if (!isValidAffineIndexOperand(idx))
      return op.emitOpError("index must be a dimension or symbol identifier");
  }
  return success();
}

void AffinePrefetchOp::getCanonicalizationPatterns(
    OwningRewritePatternList &results, MLIRContext *context) {
  // prefetch(memrefcast) -> prefetch
  results.insert<SimplifyAffineOp<AffinePrefetchOp>>(context);
}

LogicalResult AffinePrefetchOp::fold(ArrayRef<Attribute> cstOperands,
                                     SmallVectorImpl<OpFoldResult> &results) {
  /// prefetch(memrefcast) -> prefetch
  return foldMemRefCast(*this);
}

//===----------------------------------------------------------------------===//
// TableGen'd op method definitions
//===----------------------------------------------------------------------===//

#define GET_OP_CLASSES
#include "mlir/Dialect/AffineOps/AffineOps.cpp.inc"
