; NOTE: Assertions have been autogenerated by utils/update_llc_test_checks.py
; RUN: llc -mtriple=riscv32 -mattr=+v -riscv-v-vector-bits-min=128 -verify-machineinstrs < %s \
; RUN:   | FileCheck %s --check-prefix RV32
; RUN: llc -mtriple=riscv64 -mattr=+v -riscv-v-vector-bits-min=128 -verify-machineinstrs < %s \
; RUN:   | FileCheck %s --check-prefix RV64

; This used to crash during type legalization, where lowering (v4i1 =
; BUILD_VECTOR) created a (v4i1 = SETCC v4i8) which during constant-folding
; created illegally-typed i8 nodes. Ultimately, constant-folding failed and so
; the new illegal nodes had no uses. However, during a second round of
; legalization, this same pattern was generated from another BUILD_VECTOR. This
; meant one of the illegally-typed (i8 = Constant<0>) nodes now had two dead
; uses. Because the Constant and one of the uses were from round 1, they were
; further up in the node order than the new second use, so the constant was
; visited while it wasn't "dead". At the point of visiting the constant, we
; crashed.

define void @constant_folding_crash(ptr %v54, <4 x ptr> %lanes.a, <4 x ptr> %lanes.b, <4 x i1> %sel) {
; RV32-LABEL: constant_folding_crash:
; RV32:       # %bb.0: # %entry
; RV32-NEXT:    lw a0, 8(a0)
; RV32-NEXT:    vmv1r.v v10, v0
; RV32-NEXT:    andi a0, a0, 1
; RV32-NEXT:    seqz a0, a0
; RV32-NEXT:    vsetivli zero, 4, e8, mf4, ta, ma
; RV32-NEXT:    vmv.v.x v11, a0
; RV32-NEXT:    vmsne.vi v0, v11, 0
; RV32-NEXT:    vsetvli zero, zero, e32, m1, ta, ma
; RV32-NEXT:    vmerge.vvm v8, v9, v8, v0
; RV32-NEXT:    vmv.x.s a0, v8
; RV32-NEXT:    vfirst.m a1, v10
; RV32-NEXT:    snez a1, a1
; RV32-NEXT:    vsetvli zero, zero, e8, mf4, ta, ma
; RV32-NEXT:    vmv.v.x v8, a1
; RV32-NEXT:    vmsne.vi v0, v8, 0
; RV32-NEXT:    vsetvli zero, zero, e32, m1, ta, ma
; RV32-NEXT:    vmv.v.i v8, 10
; RV32-NEXT:    vse32.v v8, (a0), v0.t
; RV32-NEXT:    ret
;
; RV64-LABEL: constant_folding_crash:
; RV64:       # %bb.0: # %entry
; RV64-NEXT:    ld a0, 8(a0)
; RV64-NEXT:    vmv1r.v v12, v0
; RV64-NEXT:    andi a0, a0, 1
; RV64-NEXT:    seqz a0, a0
; RV64-NEXT:    vsetivli zero, 4, e8, mf4, ta, ma
; RV64-NEXT:    vmv.v.x v13, a0
; RV64-NEXT:    vmsne.vi v0, v13, 0
; RV64-NEXT:    vsetvli zero, zero, e64, m2, ta, ma
; RV64-NEXT:    vmerge.vvm v8, v10, v8, v0
; RV64-NEXT:    vmv.x.s a0, v8
; RV64-NEXT:    vfirst.m a1, v12
; RV64-NEXT:    snez a1, a1
; RV64-NEXT:    vsetvli zero, zero, e8, mf4, ta, ma
; RV64-NEXT:    vmv.v.x v8, a1
; RV64-NEXT:    vmsne.vi v0, v8, 0
; RV64-NEXT:    vsetvli zero, zero, e32, m1, ta, ma
; RV64-NEXT:    vmv.v.i v8, 10
; RV64-NEXT:    vse32.v v8, (a0), v0.t
; RV64-NEXT:    ret
entry:
  %sunkaddr = getelementptr i8, ptr %v54, i64 8
  %v56 = load i64, ptr %sunkaddr, align 8
  %trunc = and i64 %v56, 1
  %cmp = icmp eq i64 %trunc, 0
  %ptrs = select i1 %cmp, <4 x ptr> %lanes.a, <4 x ptr> %lanes.b
  %v67 = extractelement <4 x ptr> %ptrs, i64 0
  %mask = shufflevector <4 x i1> %sel, <4 x i1> undef, <4 x i32> zeroinitializer
  call void @llvm.masked.store.v4i32.p0(<4 x i32> <i32 10, i32 10, i32 10, i32 10>, ptr %v67, i32 16, <4 x i1> %mask)
  ret void
}

declare void @llvm.masked.store.v4i32.p0(<4 x i32>, ptr, i32, <4 x i1>)
