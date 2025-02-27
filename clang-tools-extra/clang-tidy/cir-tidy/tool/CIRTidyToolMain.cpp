//===--- tools/extra/clang-tidy/cir/CIRTidyToolMain.cpp - cir tidy tool ---===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
///  \file This file contains cir-tidy tool entry point main function.
///
///  This tool uses the Clang Tooling infrastructure, see
///    http://clang.llvm.org/docs/HowToSetupToolingForLLVM.html
///  for details on setting it up with LLVM source tree.
///
//===----------------------------------------------------------------------===//

#include "CIRTidyMain.h"

int main(int argc, const char **argv) {
  return cir::tidy::CIRTidyMain(argc, argv);
}
