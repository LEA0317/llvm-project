//===- AffineMap.h - MLIR Affine Map Class ----------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Affine maps are mathematical functions which map a list of dimension
// identifiers and symbols, to multidimensional affine expressions.
//
//===----------------------------------------------------------------------===//

#ifndef MLIR_IR_AFFINE_MAP_H
#define MLIR_IR_AFFINE_MAP_H

#include "mlir/IR/AffineExpr.h"
#include "mlir/Support/LLVM.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMapInfo.h"

namespace mlir {

namespace detail {
struct AffineMapStorage;
} // end namespace detail

class Attribute;
struct LogicalResult;
class MLIRContext;

/// A multi-dimensional affine map
/// Affine map's are immutable like Type's, and they are uniqued.
/// Eg: (d0, d1) -> (d0/128, d0 mod 128, d1)
/// The names used (d0, d1) don't matter - it's the mathematical function that
/// is unique to this affine map.
class AffineMap {
public:
  using ImplType = detail::AffineMapStorage;

  constexpr AffineMap() : map(nullptr) {}
  explicit AffineMap(ImplType *map) : map(map) {}

  /// Returns a zero result affine map with no dimensions or symbols: () -> ().
  static AffineMap get(MLIRContext *context);

  static AffineMap get(unsigned dimCount, unsigned symbolCount,
                       ArrayRef<AffineExpr> results);

  /// Returns a single constant result affine map.
  static AffineMap getConstantMap(int64_t val, MLIRContext *context);

  /// Returns an AffineMap with 'numDims' identity result dim exprs.
  static AffineMap getMultiDimIdentityMap(unsigned numDims,
                                          MLIRContext *context);

  /// Returns an AffineMap representing a permutation.
  /// The permutation is expressed as a non-empty vector of integers.
  /// E.g. the permutation `(i,j,k) -> (j,k,i)` will be expressed with
  /// `permutation = [1,2,0]`. All values in `permutation` must be
  /// integers, in the range 0..`permutation.size()-1` without duplications
  /// (i.e. `[1,1,2]` is an invalid permutation).
  static AffineMap getPermutationMap(ArrayRef<unsigned> permutation,
                                     MLIRContext *context);

  MLIRContext *getContext() const;

  explicit operator bool() { return map != nullptr; }
  bool operator==(AffineMap other) const { return other.map == map; }
  bool operator!=(AffineMap other) const { return !(other.map == map); }

  /// Returns true if this affine map is an identity affine map.
  /// An identity affine map corresponds to an identity affine function on the
  /// dimensional identifiers.
  bool isIdentity() const;

  /// Returns true if this affine map is an empty map, i.e., () -> ().
  bool isEmpty() const;

  /// Returns true if this affine map is a single result constant function.
  bool isSingleConstant() const;

  /// Returns the constant result of this map. This methods asserts that the map
  /// has a single constant result.
  int64_t getSingleConstantResult() const;

  // Prints affine map to 'os'.
  void print(raw_ostream &os) const;
  void dump() const;

  unsigned getNumDims() const;
  unsigned getNumSymbols() const;
  unsigned getNumResults() const;
  unsigned getNumInputs() const;

  ArrayRef<AffineExpr> getResults() const;
  AffineExpr getResult(unsigned idx) const;

  /// Walk all of the AffineExpr's in this mapping. Each node in an expression
  /// tree is visited in postorder.
  void walkExprs(std::function<void(AffineExpr)> callback) const;

  /// This method substitutes any uses of dimensions and symbols (e.g.
  /// dim#0 with dimReplacements[0]) in subexpressions and returns the modified
  /// expression mapping.  Because this can be used to eliminate dims and
  /// symbols, the client needs to specify the number of dims and symbols in
  /// the result.  The returned map always has the same number of results.
  AffineMap replaceDimsAndSymbols(ArrayRef<AffineExpr> dimReplacements,
                                  ArrayRef<AffineExpr> symReplacements,
                                  unsigned numResultDims,
                                  unsigned numResultSyms);

  /// Folds the results of the application of an affine map on the provided
  /// operands to a constant if possible.
  LogicalResult constantFold(ArrayRef<Attribute> operandConstants,
                             SmallVectorImpl<Attribute> &results) const;

  /// Returns the AffineMap resulting from composing `this` with `map`.
  /// The resulting AffineMap has as many AffineDimExpr as `map` and as many
  /// AffineSymbolExpr as the concatenation of `this` and `map` (in which case
  /// the symbols of `this` map come first).
  ///
  /// Prerequisites:
  /// The maps are composable, i.e. that the number of AffineDimExpr of `this`
  /// matches the number of results of `map`.
  ///
  /// Example:
  ///   map1: `(d0, d1)[s0, s1] -> (d0 + 1 + s1, d1 - 1 - s0)`
  ///   map2: `(d0)[s0] -> (d0 + s0, d0 - s0)`
  ///   map1.compose(map2):
  ///     `(d0)[s0, s1, s2] -> (d0 + s1 + s2 + 1, d0 - s0 - s2 - 1)`
  AffineMap compose(AffineMap map);

  /// Returns true if the AffineMap represents a subset (i.e. a projection) of a
  /// symbol-less permutation map.
  bool isProjectedPermutation();

  /// Returns true if the AffineMap represents a symbol-less permutation map.
  bool isPermutation();

  /// Returns the map consisting of the `resultPos` subset.
  AffineMap getSubMap(ArrayRef<unsigned> resultPos);

  friend ::llvm::hash_code hash_value(AffineMap arg);

private:
  ImplType *map;

  static AffineMap getImpl(unsigned dimCount, unsigned symbolCount,
                           ArrayRef<AffineExpr> results, MLIRContext *context);
};

// Make AffineExpr hashable.
inline ::llvm::hash_code hash_value(AffineMap arg) {
  return ::llvm::hash_value(arg.map);
}

/// A mutable affine map. Its affine expressions are however unique.
struct MutableAffineMap {
public:
  MutableAffineMap() {}
  MutableAffineMap(AffineMap map);

  ArrayRef<AffineExpr> getResults() const { return results; }
  AffineExpr getResult(unsigned idx) const { return results[idx]; }
  void setResult(unsigned idx, AffineExpr result) { results[idx] = result; }
  unsigned getNumResults() const { return results.size(); }
  unsigned getNumDims() const { return numDims; }
  void setNumDims(unsigned d) { numDims = d; }
  unsigned getNumSymbols() const { return numSymbols; }
  void setNumSymbols(unsigned d) { numSymbols = d; }
  MLIRContext *getContext() const { return context; }

  /// Returns true if the idx'th result expression is a multiple of factor.
  bool isMultipleOf(unsigned idx, int64_t factor) const;

  /// Resets this MutableAffineMap with 'map'.
  void reset(AffineMap map);

  /// Simplify the (result) expressions in this map using analysis (used by
  //-simplify-affine-expr pass).
  void simplify();
  /// Get the AffineMap corresponding to this MutableAffineMap. Note that an
  /// AffineMap will be uniqued and stored in context, while a mutable one
  /// isn't.
  AffineMap getAffineMap() const;

private:
  // Same meaning as AffineMap's fields.
  SmallVector<AffineExpr, 8> results;
  unsigned numDims;
  unsigned numSymbols;
  /// A pointer to the IR's context to store all newly created
  /// AffineExprStorage's.
  MLIRContext *context;
};

/// Simplify an affine map by simplifying its underlying AffineExpr results.
AffineMap simplifyAffineMap(AffineMap map);

/// Returns a map of codomain to domain dimensions such that the first codomain
/// dimension for a particular domain dimension is selected.
/// Returns an empty map if the input map is empty or if `map` is not invertible
/// (i.e. `map` does not contain a subset that is a permutation of full domain
/// rank).
///
/// Prerequisites:
///   1. `map` has no symbols.
///
/// Example 1:
///
/// ```mlir
///    (d0, d1, d2) -> (d1, d1, d0, d2, d1, d2, d1, d0)
///                      0       2   3
/// ```
///
/// returns:
///
/// ```mlir
///    (d0, d1, d2, d3, d4, d5, d6, d7) -> (d2, d0, d3)
/// ```
///
/// Example 2:
///
/// ```mlir
///    (d0, d1, d2) -> (d1, d0 + d1, d0, d2, d1, d2, d1, d0)
///                      0            2   3
/// ```
///
/// returns:
///
/// ```mlir
///    (d0, d1, d2, d3, d4, d5, d6, d7) -> (d2, d0, d3)
/// ```
AffineMap inversePermutation(AffineMap map);

/// Concatenates a list of `maps` into a single AffineMap, stepping over
/// potentially empty maps. Assumes each of the underlying map has 0 symbols.
/// The resulting map has a number of dims equal to the max of `maps`' dims and
/// the concatenated results as its results.
/// Returns an empty map if all input `maps` are empty.
///
/// Example:
/// When applied to the following list of 3 affine maps,
///
/// ```mlir
///    {
///      (i, j, k) -> (i, k),
///      (i, j, k) -> (k, j),
///      (i, j, k) -> (i, j)
///    }
/// ```
///
/// Returns the map:
///
/// ```mlir
///     (i, j, k) -> (i, k, k, j, i, j)
/// ```
AffineMap concatAffineMaps(ArrayRef<AffineMap> maps);

inline raw_ostream &operator<<(raw_ostream &os, AffineMap map) {
  map.print(os);
  return os;
}
} // end namespace mlir

namespace llvm {

// AffineExpr hash just like pointers
template <>
struct DenseMapInfo<mlir::AffineMap> {
  static mlir::AffineMap getEmptyKey() {
    auto pointer = llvm::DenseMapInfo<void *>::getEmptyKey();
    return mlir::AffineMap(static_cast<mlir::AffineMap::ImplType *>(pointer));
  }
  static mlir::AffineMap getTombstoneKey() {
    auto pointer = llvm::DenseMapInfo<void *>::getTombstoneKey();
    return mlir::AffineMap(static_cast<mlir::AffineMap::ImplType *>(pointer));
  }
  static unsigned getHashValue(mlir::AffineMap val) {
    return mlir::hash_value(val);
  }
  static bool isEqual(mlir::AffineMap LHS, mlir::AffineMap RHS) {
    return LHS == RHS;
  }
};

} // namespace llvm

#endif // MLIR_IR_AFFINE_MAP_H
