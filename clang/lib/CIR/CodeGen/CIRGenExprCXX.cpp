//===--- CIRGenExprCXX.cpp - Emit CIR Code for C++ expressions ------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This contains code dealing with code generation of C++ expressions
//
//===----------------------------------------------------------------------===//

#include <CIRGenCXXABI.h>
#include <CIRGenFunction.h>
#include <CIRGenModule.h>
#include <CIRGenValue.h>
#include <UnimplementedFeatureGuarding.h>

#include <clang/AST/DeclCXX.h>

using namespace cir;
using namespace clang;

namespace {
struct MemberCallInfo {
  RequiredArgs ReqArgs;
  // Number of prefix arguments for the call. Ignores the `this` pointer.
  unsigned PrefixSize;
};
} // namespace

static RValue buildNewDeleteCall(CIRGenFunction &CGF,
                                 const FunctionDecl *CalleeDecl,
                                 const FunctionProtoType *CalleeType,
                                 const CallArgList &Args);

static MemberCallInfo
commonBuildCXXMemberOrOperatorCall(CIRGenFunction &CGF, const CXXMethodDecl *MD,
                                   mlir::Value This, mlir::Value ImplicitParam,
                                   QualType ImplicitParamTy, const CallExpr *CE,
                                   CallArgList &Args, CallArgList *RtlArgs) {
  assert(CE == nullptr || isa<CXXMemberCallExpr>(CE) ||
         isa<CXXOperatorCallExpr>(CE));
  assert(MD->isInstance() &&
         "Trying to emit a member or operator call expr on a static method!");

  // Push the this ptr.
  const CXXRecordDecl *RD =
      CGF.CGM.getCXXABI().getThisArgumentTypeForMethod(MD);
  Args.add(RValue::get(This), CGF.getTypes().DeriveThisType(RD, MD));

  // If there is an implicit parameter (e.g. VTT), emit it.
  if (ImplicitParam) {
    llvm_unreachable("NYI");
  }

  const auto *FPT = MD->getType()->castAs<FunctionProtoType>();
  RequiredArgs required = RequiredArgs::forPrototypePlus(FPT, Args.size());
  unsigned PrefixSize = Args.size() - 1;

  // Add the rest of the call args
  if (RtlArgs) {
    // Special case: if the caller emitted the arguments right-to-left already
    // (prior to emitting the *this argument), we're done. This happens for
    // assignment operators.
    Args.addFrom(*RtlArgs);
  } else if (CE) {
    // Special case: skip first argument of CXXOperatorCall (it is "this").
    unsigned ArgsToSkip = isa<CXXOperatorCallExpr>(CE) ? 1 : 0;
    CGF.buildCallArgs(Args, FPT, drop_begin(CE->arguments(), ArgsToSkip),
                      CE->getDirectCallee());
  } else {
    assert(
        FPT->getNumParams() == 0 &&
        "No CallExpr specified for function with non-zero number of arguments");
  }

  return {required, PrefixSize};
}

RValue CIRGenFunction::buildCXXMemberOrOperatorCall(
    const CXXMethodDecl *MD, const CIRGenCallee &Callee,
    ReturnValueSlot ReturnValue, mlir::Value This, mlir::Value ImplicitParam,
    QualType ImplicitParamTy, const CallExpr *CE, CallArgList *RtlArgs) {

  const auto *FPT = MD->getType()->castAs<FunctionProtoType>();
  CallArgList Args;
  MemberCallInfo CallInfo = commonBuildCXXMemberOrOperatorCall(
      *this, MD, This, ImplicitParam, ImplicitParamTy, CE, Args, RtlArgs);
  auto &FnInfo = CGM.getTypes().arrangeCXXMethodCall(
      Args, FPT, CallInfo.ReqArgs, CallInfo.PrefixSize);
  assert((CE || currSrcLoc) && "expected source location");
  mlir::Location loc = CE ? getLoc(CE->getExprLoc()) : *currSrcLoc;
  return buildCall(FnInfo, Callee, ReturnValue, Args, nullptr,
                   CE && CE == MustTailCall, loc);
}

RValue CIRGenFunction::buildCXXMemberOrOperatorMemberCallExpr(
    const CallExpr *CE, const CXXMethodDecl *MD, ReturnValueSlot ReturnValue,
    bool HasQualifier, NestedNameSpecifier *Qualifier, bool IsArrow,
    const Expr *Base) {
  assert(isa<CXXMemberCallExpr>(CE) || isa<CXXOperatorCallExpr>(CE));

  // Compute the object pointer.
  bool CanUseVirtualCall = MD->isVirtual() && !HasQualifier;
  assert(!CanUseVirtualCall && "NYI");

  const CXXMethodDecl *DevirtualizedMethod = nullptr;
  if (CanUseVirtualCall &&
      MD->getDevirtualizedMethod(Base, getLangOpts().AppleKext)) {
    llvm_unreachable("NYI");
  }

  bool TrivialForCodegen =
      MD->isTrivial() || (MD->isDefaulted() && MD->getParent()->isUnion());
  bool TrivialAssignment =
      TrivialForCodegen &&
      (MD->isCopyAssignmentOperator() || MD->isMoveAssignmentOperator()) &&
      !MD->getParent()->mayInsertExtraPadding();
  (void)TrivialAssignment;

  // C++17 demands that we evaluate the RHS of a (possibly-compound) assignment
  // operator before the LHS.
  CallArgList RtlArgStorage;
  CallArgList *RtlArgs = nullptr;
  LValue TrivialAssignmentRHS;
  if (auto *OCE = dyn_cast<CXXOperatorCallExpr>(CE)) {
    if (OCE->isAssignmentOp()) {
      // See further note on TrivialAssignment, we don't handle this during
      // codegen, differently than LLVM, which early optimizes like this:
      //  if (TrivialAssignment) {
      //    TrivialAssignmentRHS = buildLValue(CE->getArg(1));
      //  } else {
      RtlArgs = &RtlArgStorage;
      buildCallArgs(*RtlArgs, MD->getType()->castAs<FunctionProtoType>(),
                    drop_begin(CE->arguments(), 1), CE->getDirectCallee(),
                    /*ParamsToSkip*/ 0, EvaluationOrder::ForceRightToLeft);
    }
  }

  LValue This;
  if (IsArrow) {
    llvm_unreachable("NYI");
  } else {
    This = buildLValue(Base);
  }

  if (const CXXConstructorDecl *Ctor = dyn_cast<CXXConstructorDecl>(MD)) {
    llvm_unreachable("NYI");
  }

  if (TrivialForCodegen) {
    if (isa<CXXDestructorDecl>(MD))
      return RValue::get(nullptr);

    if (TrivialAssignment) {
      // From LLVM codegen:
      // We don't like to generate the trivial copy/move assignment operator
      // when it isn't necessary; just produce the proper effect here.
      // It's important that we use the result of EmitLValue here rather than
      // emitting call arguments, in order to preserve TBAA information from
      // the RHS.
      //
      // We don't early optimize like LLVM does:
      // LValue RHS = isa<CXXOperatorCallExpr>(CE) ? TrivialAssignmentRHS
      //                                           :
      //                                           buildLValue(*CE->arg_begin());
      // buildAggregateAssign(This, RHS, CE->getType());
      // return RValue::get(This.getPointer());
    } else {
      assert(MD->getParent()->mayInsertExtraPadding() &&
             "unknown trivial member function");
    }
  }

  // Compute the function type we're calling
  const CXXMethodDecl *CalleeDecl =
      DevirtualizedMethod ? DevirtualizedMethod : MD;
  const CIRGenFunctionInfo *FInfo = nullptr;
  if (const auto *Dtor = dyn_cast<CXXDestructorDecl>(CalleeDecl))
    llvm_unreachable("NYI");
  else
    FInfo = &CGM.getTypes().arrangeCXXMethodDeclaration(CalleeDecl);

  mlir::FunctionType Ty = CGM.getTypes().GetFunctionType(*FInfo);

  // C++11 [class.mfct.non-static]p2:
  //   If a non-static member function of a class X is called for an object that
  //   is not of type X, or of a type derived from X, the behavior is undefined.
  SourceLocation CallLoc;
  ASTContext &C = getContext();
  (void)C;
  if (CE)
    CallLoc = CE->getExprLoc();

  SanitizerSet SkippedChecks;
  if (const auto *cmce = dyn_cast<CXXMemberCallExpr>(CE)) {
    auto *ioa = cmce->getImplicitObjectArgument();
    auto isImplicitObjectCXXThis = isWrappedCXXThis(ioa);
    if (isImplicitObjectCXXThis)
      SkippedChecks.set(SanitizerKind::Alignment, true);
    if (isImplicitObjectCXXThis || isa<DeclRefExpr>(ioa))
      SkippedChecks.set(SanitizerKind::Null, true);
  }

  if (UnimplementedFeature::buildTypeCheck())
    llvm_unreachable("NYI");

  // C++ [class.virtual]p12:
  //   Explicit qualification with the scope operator (5.1) suppresses the
  //   virtual call mechanism.
  //
  // We also don't emit a virtual call if the base expression has a record type
  // because then we know what the type is.
  bool useVirtualCall = CanUseVirtualCall && !DevirtualizedMethod;

  if (const auto *dtor = dyn_cast<CXXDestructorDecl>(CalleeDecl)) {
    llvm_unreachable("NYI");
  }

  // FIXME: Uses of 'MD' past this point need to be audited. We may need to use
  // 'CalleeDecl' instead.

  CIRGenCallee Callee;
  if (useVirtualCall) {
    llvm_unreachable("NYI");
  } else {
    if (SanOpts.has(SanitizerKind::CFINVCall)) {
      llvm_unreachable("NYI");
    }

    if (getLangOpts().AppleKext)
      llvm_unreachable("NYI");
    else if (!DevirtualizedMethod)
      Callee = CIRGenCallee::forDirect(CGM.GetAddrOfFunction(MD, Ty),
                                       GlobalDecl(MD));
    else {
      llvm_unreachable("NYI");
    }
  }

  if (MD->isVirtual()) {
    llvm_unreachable("NYI");
  }

  return buildCXXMemberOrOperatorCall(
      CalleeDecl, Callee, ReturnValue, This.getPointer(),
      /*ImplicitParam=*/nullptr, QualType(), CE, RtlArgs);
}

RValue
CIRGenFunction::buildCXXOperatorMemberCallExpr(const CXXOperatorCallExpr *E,
                                               const CXXMethodDecl *MD,
                                               ReturnValueSlot ReturnValue) {
  assert(MD->isInstance() &&
         "Trying to emit a member call expr on a static method!");
  return buildCXXMemberOrOperatorMemberCallExpr(
      E, MD, ReturnValue, /*HasQualifier=*/false, /*Qualifier=*/nullptr,
      /*IsArrow=*/false, E->getArg(0));
}

void CIRGenFunction::buildCXXConstructExpr(const CXXConstructExpr *E,
                                           AggValueSlot Dest) {
  assert(!Dest.isIgnored() && "Must have a destination!");
  const auto *CD = E->getConstructor();

  // If we require zero initialization before (or instead of) calling the
  // constructor, as can be the case with a non-user-provided default
  // constructor, emit the zero initialization now, unless destination is
  // already zeroed.
  if (E->requiresZeroInitialization() && !Dest.isZeroed()) {
    switch (E->getConstructionKind()) {
    case CXXConstructExpr::CK_Delegating:
    case CXXConstructExpr::CK_Complete:
      buildNullInitialization(getLoc(E->getSourceRange()), Dest.getAddress(),
                              E->getType());
      break;
    case CXXConstructExpr::CK_VirtualBase:
    case CXXConstructExpr::CK_NonVirtualBase:
      llvm_unreachable("NYI");
      break;
    }
  }

  // If this is a call to a trivial default constructor:
  // In LLVM: do nothing.
  // In CIR: emit as a regular call, other later passes should lower the
  // ctor call into trivial initialization.
  // if (CD->isTrivial() && CD->isDefaultConstructor())
  //  return;

  // Elide the constructor if we're constructing from a temporary
  if (getLangOpts().ElideConstructors && E->isElidable()) {
    // FIXME: This only handles the simplest case, where the source object is
    //        passed directly as the first argument to the constructor. This
    //        should also handle stepping through implicit casts and conversion
    //        sequences which involve two steps, with a conversion operator
    //        follwed by a converting constructor.
    const auto *SrcObj = E->getArg(0);
    assert(SrcObj->isTemporaryObject(getContext(), CD->getParent()));
    assert(
        getContext().hasSameUnqualifiedType(E->getType(), SrcObj->getType()));
    buildAggExpr(SrcObj, Dest);
    return;
  }

  assert(!CGM.getASTContext().getAsArrayType(E->getType()) &&
         "array types NYI");

  clang::CXXCtorType Type = Ctor_Complete;
  bool ForVirtualBase = false;
  bool Delegating = false;

  switch (E->getConstructionKind()) {
  case CXXConstructExpr::CK_Complete:
    Type = Ctor_Complete;
    break;
  case CXXConstructExpr::CK_Delegating:
    llvm_unreachable("NYI");
    break;
  case CXXConstructExpr::CK_VirtualBase:
    llvm_unreachable("NYI");
    break;
  case CXXConstructExpr::CK_NonVirtualBase:
    Type = Ctor_Base;
    break;
  }

  buildCXXConstructorCall(CD, Type, ForVirtualBase, Delegating, Dest, E);
}

namespace {
/// The parameters to pass to a usual operator delete.
struct UsualDeleteParams {
  bool DestroyingDelete = false;
  bool Size = false;
  bool Alignment = false;
};
} // namespace

// FIXME(cir): this should be shared with LLVM codegen
static UsualDeleteParams getUsualDeleteParams(const FunctionDecl *FD) {
  UsualDeleteParams Params;

  const FunctionProtoType *FPT = FD->getType()->castAs<FunctionProtoType>();
  auto AI = FPT->param_type_begin(), AE = FPT->param_type_end();

  // The first argument is always a void*.
  ++AI;

  // The next parameter may be a std::destroying_delete_t.
  if (FD->isDestroyingOperatorDelete()) {
    Params.DestroyingDelete = true;
    assert(AI != AE);
    ++AI;
  }

  // Figure out what other parameters we should be implicitly passing.
  if (AI != AE && (*AI)->isIntegerType()) {
    Params.Size = true;
    ++AI;
  }

  if (AI != AE && (*AI)->isAlignValT()) {
    Params.Alignment = true;
    ++AI;
  }

  assert(AI == AE && "unexpected usual deallocation function parameter");
  return Params;
}

static mlir::Value buildCXXNewAllocSize(CIRGenFunction &CGF,
                                        const CXXNewExpr *e,
                                        unsigned minElements,
                                        mlir::Value &numElements,
                                        mlir::Value &sizeWithoutCookie) {
  QualType type = e->getAllocatedType();

  if (!e->isArray()) {
    CharUnits typeSize = CGF.getContext().getTypeSizeInChars(type);
    sizeWithoutCookie = CGF.getBuilder().create<mlir::cir::ConstantOp>(
        CGF.getLoc(e->getSourceRange()), CGF.SizeTy,
        mlir::IntegerAttr::get(CGF.SizeTy, typeSize.getQuantity()));
    return sizeWithoutCookie;
  }

  llvm_unreachable("NYI");
}

namespace {
/// A cleanup to call the given 'operator delete' function upon abnormal
/// exit from a new expression. Templated on a traits type that deals with
/// ensuring that the arguments dominate the cleanup if necessary.
template <typename Traits>
class CallDeleteDuringNew final : public EHScopeStack::Cleanup {
  /// Type used to hold llvm::Value*s.
  typedef typename Traits::ValueTy ValueTy;
  /// Type used to hold RValues.
  typedef typename Traits::RValueTy RValueTy;
  struct PlacementArg {
    RValueTy ArgValue;
    QualType ArgType;
  };

  unsigned NumPlacementArgs : 31;
  unsigned PassAlignmentToPlacementDelete : 1;
  const FunctionDecl *OperatorDelete;
  ValueTy Ptr;
  ValueTy AllocSize;
  CharUnits AllocAlign;

  PlacementArg *getPlacementArgs() {
    return reinterpret_cast<PlacementArg *>(this + 1);
  }

public:
  static size_t getExtraSize(size_t NumPlacementArgs) {
    return NumPlacementArgs * sizeof(PlacementArg);
  }

  CallDeleteDuringNew(size_t NumPlacementArgs,
                      const FunctionDecl *OperatorDelete, ValueTy Ptr,
                      ValueTy AllocSize, bool PassAlignmentToPlacementDelete,
                      CharUnits AllocAlign)
      : NumPlacementArgs(NumPlacementArgs),
        PassAlignmentToPlacementDelete(PassAlignmentToPlacementDelete),
        OperatorDelete(OperatorDelete), Ptr(Ptr), AllocSize(AllocSize),
        AllocAlign(AllocAlign) {}

  void setPlacementArg(unsigned I, RValueTy Arg, QualType Type) {
    assert(I < NumPlacementArgs && "index out of range");
    getPlacementArgs()[I] = {Arg, Type};
  }

  void Emit(CIRGenFunction &CGF, Flags flags) override {
    const auto *FPT = OperatorDelete->getType()->castAs<FunctionProtoType>();
    CallArgList DeleteArgs;

    // The first argument is always a void* (or C* for a destroying operator
    // delete for class type C).
    DeleteArgs.add(Traits::get(CGF, Ptr), FPT->getParamType(0));

    // Figure out what other parameters we should be implicitly passing.
    UsualDeleteParams Params;
    if (NumPlacementArgs) {
      // A placement deallocation function is implicitly passed an alignment
      // if the placement allocation function was, but is never passed a size.
      Params.Alignment = PassAlignmentToPlacementDelete;
    } else {
      // For a non-placement new-expression, 'operator delete' can take a
      // size and/or an alignment if it has the right parameters.
      Params = getUsualDeleteParams(OperatorDelete);
    }

    assert(!Params.DestroyingDelete &&
           "should not call destroying delete in a new-expression");

    // The second argument can be a std::size_t (for non-placement delete).
    if (Params.Size)
      DeleteArgs.add(Traits::get(CGF, AllocSize),
                     CGF.getContext().getSizeType());

    // The next (second or third) argument can be a std::align_val_t, which
    // is an enum whose underlying type is std::size_t.
    // FIXME: Use the right type as the parameter type. Note that in a call
    // to operator delete(size_t, ...), we may not have it available.
    if (Params.Alignment) {
      llvm_unreachable("NYI");
    }

    // Pass the rest of the arguments, which must match exactly.
    for (unsigned I = 0; I != NumPlacementArgs; ++I) {
      auto Arg = getPlacementArgs()[I];
      DeleteArgs.add(Traits::get(CGF, Arg.ArgValue), Arg.ArgType);
    }

    // Call 'operator delete'.
    buildNewDeleteCall(CGF, OperatorDelete, FPT, DeleteArgs);
  }
};
} // namespace

/// Enter a cleanup to call 'operator delete' if the initializer in a
/// new-expression throws.
static void EnterNewDeleteCleanup(CIRGenFunction &CGF, const CXXNewExpr *E,
                                  Address NewPtr, mlir::Value AllocSize,
                                  CharUnits AllocAlign,
                                  const CallArgList &NewArgs) {
  unsigned NumNonPlacementArgs = E->passAlignment() ? 2 : 1;

  // If we're not inside a conditional branch, then the cleanup will
  // dominate and we can do the easier (and more efficient) thing.
  if (!CGF.isInConditionalBranch()) {
    struct DirectCleanupTraits {
      typedef mlir::Value ValueTy;
      typedef RValue RValueTy;
      static RValue get(CIRGenFunction &, ValueTy V) { return RValue::get(V); }
      static RValue get(CIRGenFunction &, RValueTy V) { return V; }
    };

    typedef CallDeleteDuringNew<DirectCleanupTraits> DirectCleanup;

    DirectCleanup *Cleanup = CGF.EHStack.pushCleanupWithExtra<DirectCleanup>(
        EHCleanup, E->getNumPlacementArgs(), E->getOperatorDelete(),
        NewPtr.getPointer(), AllocSize, E->passAlignment(), AllocAlign);
    for (unsigned I = 0, N = E->getNumPlacementArgs(); I != N; ++I) {
      auto &Arg = NewArgs[I + NumNonPlacementArgs];
      Cleanup->setPlacementArg(
          I, Arg.getRValue(CGF, CGF.getLoc(E->getSourceRange())), Arg.Ty);
    }

    return;
  }

  // Otherwise, we need to save all this stuff.
  DominatingValue<RValue>::saved_type SavedNewPtr =
      DominatingValue<RValue>::save(CGF, RValue::get(NewPtr.getPointer()));
  DominatingValue<RValue>::saved_type SavedAllocSize =
      DominatingValue<RValue>::save(CGF, RValue::get(AllocSize));

  struct ConditionalCleanupTraits {
    typedef DominatingValue<RValue>::saved_type ValueTy;
    typedef DominatingValue<RValue>::saved_type RValueTy;
    static RValue get(CIRGenFunction &CGF, ValueTy V) { return V.restore(CGF); }
  };
  typedef CallDeleteDuringNew<ConditionalCleanupTraits> ConditionalCleanup;

  ConditionalCleanup *Cleanup =
      CGF.EHStack.pushCleanupWithExtra<ConditionalCleanup>(
          EHCleanup, E->getNumPlacementArgs(), E->getOperatorDelete(),
          SavedNewPtr, SavedAllocSize, E->passAlignment(), AllocAlign);
  for (unsigned I = 0, N = E->getNumPlacementArgs(); I != N; ++I) {
    auto &Arg = NewArgs[I + NumNonPlacementArgs];
    Cleanup->setPlacementArg(
        I,
        DominatingValue<RValue>::save(
            CGF, Arg.getRValue(CGF, CGF.getLoc(E->getSourceRange()))),
        Arg.Ty);
  }

  CGF.initFullExprCleanup();
}

mlir::Value CIRGenFunction::buildCXXNewExpr(const CXXNewExpr *E) {
  // The element type being allocated.
  QualType allocType = getContext().getBaseElementType(E->getAllocatedType());

  // 1. Build a call to the allocation function.
  FunctionDecl *allocator = E->getOperatorNew();

  // If there is a brace-initializer, cannot allocate fewer elements than inits.
  unsigned minElements = 0;
  if (E->isArray() && E->hasInitializer()) {
    const InitListExpr *ILE = dyn_cast<InitListExpr>(E->getInitializer());
    if (ILE && ILE->isStringLiteralInit())
      minElements =
          cast<ConstantArrayType>(ILE->getType()->getAsArrayTypeUnsafe())
              ->getSize()
              .getZExtValue();
    else if (ILE)
      minElements = ILE->getNumInits();
  }

  mlir::Value numElements = nullptr;
  mlir::Value allocSizeWithoutCookie = nullptr;
  mlir::Value allocSize = buildCXXNewAllocSize(
      *this, E, minElements, numElements, allocSizeWithoutCookie);
  CharUnits allocAlign = getContext().getTypeAlignInChars(allocType);

  // Emit the allocation call.
  Address allocation = Address::invalid();
  CallArgList allocatorArgs;
  if (allocator->isReservedGlobalPlacementOperator()) {
    // In LLVM codegen: If the allocator is a global placement operator, just
    // "inline" it directly.
    llvm_unreachable("NYI");
  } else {
    const FunctionProtoType *allocatorType =
        allocator->getType()->castAs<FunctionProtoType>();
    unsigned ParamsToSkip = 0;

    // The allocation size is the first argument.
    QualType sizeType = getContext().getSizeType();
    allocatorArgs.add(RValue::get(allocSize), sizeType);
    ++ParamsToSkip;

    if (allocSize != allocSizeWithoutCookie) {
      llvm_unreachable("NYI");
    }

    // The allocation alignment may be passed as the second argument.
    if (E->passAlignment()) {
      llvm_unreachable("NYI");
    }

    // FIXME: Why do we not pass a CalleeDecl here?
    buildCallArgs(allocatorArgs, allocatorType, E->placement_arguments(),
                  /*AC*/
                  AbstractCallee(),
                  /*ParamsToSkip*/
                  ParamsToSkip);
    RValue RV =
        buildNewDeleteCall(*this, allocator, allocatorType, allocatorArgs);

    // Set !heapallocsite metadata on the call to operator new.
    assert(!UnimplementedFeature::generateDebugInfo());

    // If this was a call to a global replaceable allocation function that does
    // not take an alignment argument, the allocator is known to produce storage
    // that's suitably aligned for any object that fits, up to a known
    // threshold. Otherwise assume it's suitably aligned for the allocated type.
    CharUnits allocationAlign = allocAlign;
    if (!E->passAlignment() &&
        allocator->isReplaceableGlobalAllocationFunction()) {
      auto &Target = CGM.getASTContext().getTargetInfo();
      unsigned AllocatorAlign = llvm::bit_floor(std::min<uint64_t>(
          Target.getNewAlign(), getContext().getTypeSize(allocType)));
      allocationAlign = std::max(
          allocationAlign, getContext().toCharUnitsFromBits(AllocatorAlign));
    }

    allocation = Address(RV.getScalarVal(), Int8Ty, allocationAlign);
  }

  // Emit a null check on the allocation result if the allocation
  // function is allowed to return null (because it has a non-throwing
  // exception spec or is the reserved placement new) and we have an
  // interesting initializer will be running sanitizers on the initialization.
  bool nullCheck = E->shouldNullCheckAllocation() &&
                   (!allocType.isPODType(getContext()) || E->hasInitializer() ||
                    sanitizePerformTypeCheck());

  // The null-check means that the initializer is conditionally
  // evaluated.
  ConditionalEvaluation conditional(*this);

  if (nullCheck) {
    llvm_unreachable("NYI");
  }

  // If there's an operator delete, enter a cleanup to call it if an
  // exception is thrown.
  EHScopeStack::stable_iterator operatorDeleteCleanup;
  // llvm::Instruction *cleanupDominator = nullptr;
  if (E->getOperatorDelete() &&
      !E->getOperatorDelete()->isReservedGlobalPlacementOperator()) {
    llvm_unreachable("NYI");
    EnterNewDeleteCleanup(*this, E, allocation, allocSize, allocAlign,
                          allocatorArgs);
    operatorDeleteCleanup = EHStack.stable_begin();
    llvm_unreachable("NYI");
    // cleanupDominator = Builder.CreateUnreachable();
  }
  llvm_unreachable("NYI");
}

RValue CIRGenFunction::buildCXXDestructorCall(GlobalDecl Dtor,
                                              const CIRGenCallee &Callee,
                                              mlir::Value This, QualType ThisTy,
                                              mlir::Value ImplicitParam,
                                              QualType ImplicitParamTy,
                                              const CallExpr *CE) {
  const CXXMethodDecl *DtorDecl = cast<CXXMethodDecl>(Dtor.getDecl());

  assert(!ThisTy.isNull());
  assert(ThisTy->getAsCXXRecordDecl() == DtorDecl->getParent() &&
         "Pointer/Object mixup");

  LangAS SrcAS = ThisTy.getAddressSpace();
  LangAS DstAS = DtorDecl->getMethodQualifiers().getAddressSpace();
  if (SrcAS != DstAS) {
    llvm_unreachable("NYI");
  }

  CallArgList Args;
  commonBuildCXXMemberOrOperatorCall(*this, DtorDecl, This, ImplicitParam,
                                     ImplicitParamTy, CE, Args, nullptr);
  assert((CE || Dtor.getDecl()) && "expected source location provider");
  return buildCall(CGM.getTypes().arrangeCXXStructorDeclaration(Dtor), Callee,
                   ReturnValueSlot(), Args, nullptr, CE && CE == MustTailCall,
                   CE ? getLoc(CE->getExprLoc())
                      : getLoc(Dtor.getDecl()->getSourceRange()));
}

/// Emit a call to an operator new or operator delete function, as implicitly
/// created by new-expressions and delete-expressions.
static RValue buildNewDeleteCall(CIRGenFunction &CGF,
                                 const FunctionDecl *CalleeDecl,
                                 const FunctionProtoType *CalleeType,
                                 const CallArgList &Args) {
  mlir::cir::CallOp CallOrInvoke{};
  auto CalleePtr = CGF.CGM.GetAddrOfFunction(CalleeDecl);
  CIRGenCallee Callee =
      CIRGenCallee::forDirect(CalleePtr, GlobalDecl(CalleeDecl));
  RValue RV = CGF.buildCall(CGF.CGM.getTypes().arrangeFreeFunctionCall(
                                Args, CalleeType, /*ChainCall=*/false),
                            Callee, ReturnValueSlot(), Args, &CallOrInvoke);

  /// C++1y [expr.new]p10:
  ///   [In a new-expression,] an implementation is allowed to omit a call
  ///   to a replaceable global allocation function.
  ///
  /// We model such elidable calls with the 'builtin' attribute.
  assert(!UnimplementedFeature::attributeBuiltin());
  return RV;
}

void CIRGenFunction::buildDeleteCall(const FunctionDecl *DeleteFD,
                                     mlir::Value Ptr, QualType DeleteTy,
                                     mlir::Value NumElements,
                                     CharUnits CookieSize) {
  assert((!NumElements && CookieSize.isZero()) ||
         DeleteFD->getOverloadedOperator() == OO_Array_Delete);

  const auto *DeleteFTy = DeleteFD->getType()->castAs<FunctionProtoType>();
  CallArgList DeleteArgs;

  auto Params = getUsualDeleteParams(DeleteFD);
  auto ParamTypeIt = DeleteFTy->param_type_begin();

  // Pass the pointer itself.
  QualType ArgTy = *ParamTypeIt++;
  mlir::Value DeletePtr =
      builder.createBitcast(Ptr.getLoc(), Ptr, ConvertType(ArgTy));
  DeleteArgs.add(RValue::get(DeletePtr), ArgTy);

  // Pass the std::destroying_delete tag if present.
  mlir::Value DestroyingDeleteTag{};
  if (Params.DestroyingDelete) {
    llvm_unreachable("NYI");
  }

  // Pass the size if the delete function has a size_t parameter.
  if (Params.Size) {
    llvm_unreachable("NYI");
  }

  // Pass the alignment if the delete function has an align_val_t parameter.
  if (Params.Alignment) {
    llvm_unreachable("NYI");
  }

  assert(ParamTypeIt == DeleteFTy->param_type_end() &&
         "unknown parameter to usual delete function");

  // Emit the call to delete.
  buildNewDeleteCall(*this, DeleteFD, DeleteFTy, DeleteArgs);

  // If call argument lowering didn't use the destroying_delete_t alloca,
  // remove it again.
  if (DestroyingDeleteTag && DestroyingDeleteTag.use_empty()) {
    llvm_unreachable("NYI"); // DestroyingDeleteTag->eraseFromParent();
  }
}
