//===- CIRTypes.cpp - MLIR CIR Types --------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file defines the types in the CIR dialect.
//
//===----------------------------------------------------------------------===//

#include "clang/CIR/Dialect/IR/CIRAttrs.h"
#include "clang/CIR/Dialect/IR/CIRDialect.h"

#include "mlir/IR/Attributes.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinAttributeInterfaces.h"
#include "mlir/IR/BuiltinTypes.h"
#include "mlir/IR/DialectImplementation.h"
#include "mlir/IR/OpImplementation.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/TypeSwitch.h"

// ClangIR holds back AST references when available.
#include "clang/AST/Decl.h"

static void printConstStructMembers(mlir::AsmPrinter &p, mlir::Type type,
                                    mlir::ArrayAttr members);
static mlir::ParseResult parseConstStructMembers(::mlir::AsmParser &parser,
                                                 mlir::Type &type,
                                                 mlir::ArrayAttr &members);

#define GET_ATTRDEF_CLASSES
#include "clang/CIR/Dialect/IR/CIROpsAttributes.cpp.inc"

using namespace mlir;
using namespace mlir::cir;

//===----------------------------------------------------------------------===//
// General CIR parsing / printing
//===----------------------------------------------------------------------===//

Attribute CIRDialect::parseAttribute(DialectAsmParser &parser,
                                     Type type) const {
  llvm::SMLoc typeLoc = parser.getCurrentLocation();
  StringRef mnemonic;
  Attribute genAttr;
  OptionalParseResult parseResult =
      generatedAttributeParser(parser, &mnemonic, type, genAttr);
  if (parseResult.has_value())
    return genAttr;
  parser.emitError(typeLoc, "unknown attribute in CIR dialect");
  return Attribute();
}

void CIRDialect::printAttribute(Attribute attr, DialectAsmPrinter &os) const {
  if (failed(generatedAttributePrinter(attr, os)))
    llvm_unreachable("unexpected CIR type kind");
}

static void printConstStructMembers(mlir::AsmPrinter &p, mlir::Type type,
                                    mlir::ArrayAttr members) {
  p << "{";
  unsigned i = 0, e = members.size();
  while (i < e) {
    p << members[i];
    if (e > 0 && i < e - 1)
      p << ",";
    i++;
  }
  p << "}";
}

static ParseResult parseConstStructMembers(::mlir::AsmParser &parser,
                                           mlir::Type &type,
                                           mlir::ArrayAttr &members) {
  SmallVector<mlir::Attribute, 4> elts;
  SmallVector<mlir::Type, 4> tys;
  if (parser
          .parseCommaSeparatedList(
              AsmParser::Delimiter::Braces,
              [&]() {
                Attribute attr;
                if (parser.parseAttribute(attr).succeeded()) {
                  elts.push_back(attr);
                  if (auto tyAttr = attr.dyn_cast<mlir::TypedAttr>()) {
                    tys.push_back(tyAttr.getType());
                    return success();
                  }
                  parser.emitError(parser.getCurrentLocation(),
                                   "expected a typed attribute");
                }
                return failure();
              })
          .failed())
    return failure();

  auto *ctx = parser.getContext();
  members = mlir::ArrayAttr::get(ctx, elts);
  type = mlir::cir::StructType::get(ctx, tys, "", /*body=*/true);
  return success();
}

LogicalResult ConstStructAttr::verify(
    ::llvm::function_ref<::mlir::InFlightDiagnostic()> emitError,
    mlir::Type type, ArrayAttr members) {
  auto sTy = type.dyn_cast_or_null<mlir::cir::StructType>();
  if (!sTy) {
    emitError() << "expected !cir.struct type";
    return failure();
  }

  if (sTy.getMembers().size() != members.size()) {
    emitError() << "number of elements must match";
    return failure();
  }

  unsigned attrIdx = 0;
  for (auto &member : sTy.getMembers()) {
    auto m = members[attrIdx].dyn_cast_or_null<TypedAttr>();
    if (!m) {
      emitError() << "expected mlir::TypedAttr attribute";
      return failure();
    }
    if (member != m.getType()) {
      emitError() << "element at index " << attrIdx << " has type "
                  << m.getType() << " but return type for this element is "
                  << member;
      return failure();
    }
    attrIdx++;
  }

  return success();
}

//===----------------------------------------------------------------------===//
// CIR Dialect
//===----------------------------------------------------------------------===//

void CIRDialect::registerAttributes() {
  addAttributes<
#define GET_ATTRDEF_LIST
#include "clang/CIR/Dialect/IR/CIROpsAttributes.cpp.inc"
      >();
}
