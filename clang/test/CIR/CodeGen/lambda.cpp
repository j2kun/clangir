// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -fclangir-enable -Wno-return-stack-address -emit-cir %s -o %t.cir
// RUN: FileCheck --input-file=%t.cir %s

void fn() {
  auto a = [](){};
  a();
}

//      CHECK: !ty_22class2Eanon22 = !cir.struct<"class.anon", i8>
//  CHECK-DAG: module

//      CHECK: cir.func lambda internal private @_ZZ2fnvENK3$_0clEv

//      CHECK:   cir.func @_Z2fnv()
// CHECK-NEXT:     %0 = cir.alloca !ty_22class2Eanon22, cir.ptr <!ty_22class2Eanon22>, ["a"]
//      CHECK:   cir.call @_ZZ2fnvENK3$_0clEv

void l0() {
  int i;
  auto a = [&](){ i = i + 1; };
  a();
}

// CHECK: cir.func lambda internal private @_ZZ2l0vENK3$_0clEv(

// CHECK: %0 = cir.alloca !cir.ptr<!ty_22class2Eanon221>, cir.ptr <!cir.ptr<!ty_22class2Eanon221>>, ["this", init]
// CHECK: %1 = cir.load %0 : cir.ptr <!cir.ptr<!ty_22class2Eanon221>>, !cir.ptr<!ty_22class2Eanon221>
// CHECK: %2 = "cir.struct_element_addr"(%1) {member_name = "i"} : (!cir.ptr<!ty_22class2Eanon221>) -> !cir.ptr<!cir.ptr<i32>>
// CHECK: %3 = cir.load %2 : cir.ptr <!cir.ptr<i32>>, !cir.ptr<i32>
// CHECK: %4 = cir.load %3 : cir.ptr <i32>, i32
// CHECK: %5 = cir.const(1 : i32) : i32
// CHECK: %6 = cir.binop(add, %4, %5) : i32
// CHECK: %7 = "cir.struct_element_addr"(%1) {member_name = "i"} : (!cir.ptr<!ty_22class2Eanon221>) -> !cir.ptr<!cir.ptr<i32>>
// CHECK: %8 = cir.load %7 : cir.ptr <!cir.ptr<i32>>, !cir.ptr<i32>
// CHECK: cir.store %6, %8 : i32, cir.ptr <i32>

// CHECK: cir.func @_Z2l0v() {

auto g() {
  int i = 12;
  return [&] {
    i += 100;
    return i;
  };
}

// CHECK: cir.func @_Z1gv() -> !ty_22class2Eanon222 {
// CHECK: %0 = cir.alloca !ty_22class2Eanon222, cir.ptr <!ty_22class2Eanon222>, ["__retval"] {alignment = 8 : i64}
// CHECK: %1 = cir.alloca i32, cir.ptr <i32>, ["i", init] {alignment = 4 : i64}
// CHECK: %2 = cir.const(12 : i32) : i32
// CHECK: cir.store %2, %1 : i32, cir.ptr <i32>
// CHECK: %3 = "cir.struct_element_addr"(%0) {member_name = "i"} : (!cir.ptr<!ty_22class2Eanon222>) -> !cir.ptr<!cir.ptr<i32>>
// CHECK: cir.store %1, %3 : !cir.ptr<i32>, cir.ptr <!cir.ptr<i32>>
// CHECK: %4 = cir.load %0 : cir.ptr <!ty_22class2Eanon222>, !ty_22class2Eanon222
// CHECK: cir.return %4 : !ty_22class2Eanon222

auto g2() {
  int i = 12;
  auto lam = [&] {
    i += 100;
    return i;
  };
  return lam;
}

// Should be same as above because of NRVO
// CHECK: cir.func @_Z2g2v() -> !ty_22class2Eanon223 {
// CHECK-NEXT: %0 = cir.alloca !ty_22class2Eanon223, cir.ptr <!ty_22class2Eanon223>, ["__retval", init] {alignment = 8 : i64}
// CHECK-NEXT: %1 = cir.alloca i32, cir.ptr <i32>, ["i", init] {alignment = 4 : i64}
// CHECK-NEXT: %2 = cir.const(12 : i32) : i32
// CHECK-NEXT: cir.store %2, %1 : i32, cir.ptr <i32>
// CHECK-NEXT: %3 = "cir.struct_element_addr"(%0) {member_name = "i"} : (!cir.ptr<!ty_22class2Eanon223>) -> !cir.ptr<!cir.ptr<i32>>
// CHECK-NEXT: cir.store %1, %3 : !cir.ptr<i32>, cir.ptr <!cir.ptr<i32>>
// CHECK-NEXT: %4 = cir.load %0 : cir.ptr <!ty_22class2Eanon223>, !ty_22class2Eanon223
// CHECK-NEXT: cir.return %4 : !ty_22class2Eanon223

int f() {
  return g2()();
}

// CHECK: cir.func @_Z1fv() -> i32 {
// CHECK-NEXT:   %0 = cir.alloca i32, cir.ptr <i32>, ["__retval"] {alignment = 4 : i64}
// CHECK-NEXT:   cir.scope {
// CHECK-NEXT:     %2 = cir.alloca !ty_22class2Eanon223, cir.ptr <!ty_22class2Eanon223>, ["ref.tmp0"] {alignment = 8 : i64}
// CHECK-NEXT:     %3 = cir.call @_Z2g2v() : () -> !ty_22class2Eanon223
// CHECK-NEXT:     cir.store %3, %2 : !ty_22class2Eanon223, cir.ptr <!ty_22class2Eanon223>
// CHECK-NEXT:     %4 = cir.call @_ZZ2g2vENK3$_0clEv(%2) : (!cir.ptr<!ty_22class2Eanon223>) -> i32
// CHECK-NEXT:     cir.store %4, %0 : i32, cir.ptr <i32>
// CHECK-NEXT:   }
// CHECK-NEXT:   %1 = cir.load %0 : cir.ptr <i32>, i32
// CHECK-NEXT:   cir.return %1 : i32
// CHECK-NEXT: }

int g3() {
  auto* fn = +[](int const& i) -> int { return i; };
  auto task = fn(3);
  return task;
}

// lambda operator()
// CHECK: cir.func lambda internal private @_ZZ2g3vENK3$_0clERKi

// lambda __invoke()
// CHECK:   cir.func internal private @_ZZ2g3vEN3$_08__invokeERKi

// lambda operator int (*)(int const&)()
// CHECK:   cir.func internal private @_ZZ2g3vENK3$_0cvPFiRKiEEv

// CHECK: cir.func @_Z2g3v() -> i32 {
// CHECK:     %0 = cir.alloca i32, cir.ptr <i32>, ["__retval"] {alignment = 4 : i64}
// CHECK:     %1 = cir.alloca !cir.ptr<(!cir.ptr<i32>) -> i32>, cir.ptr <!cir.ptr<(!cir.ptr<i32>) -> i32>>, ["fn", init] {alignment = 8 : i64}
// CHECK:     %2 = cir.alloca i32, cir.ptr <i32>, ["task", init] {alignment = 4 : i64}

// 1. Use `operator int (*)(int const&)()` to retrieve the fnptr to `__invoke()`.
// CHECK:     %3 = cir.scope {
// CHECK:       %7 = cir.alloca !ty_22class2Eanon224, cir.ptr <!ty_22class2Eanon224>, ["ref.tmp0"] {alignment = 1 : i64}
// CHECK:       %8 = cir.call @_ZZ2g3vENK3$_0cvPFiRKiEEv(%7) : (!cir.ptr<!ty_22class2Eanon224>) -> !cir.ptr<(!cir.ptr<i32>) -> i32>
// CHECK:       %9 = cir.unary(plus, %8) : !cir.ptr<(!cir.ptr<i32>) -> i32>, !cir.ptr<(!cir.ptr<i32>) -> i32>
// CHECK:       cir.yield %9 : !cir.ptr<(!cir.ptr<i32>) -> i32>
// CHECK:     }

// 2. Load ptr to `__invoke()`.
// CHECK:     cir.store %3, %1 : !cir.ptr<(!cir.ptr<i32>) -> i32>, cir.ptr <!cir.ptr<(!cir.ptr<i32>) -> i32>>
// CHECK:     %4 = cir.scope {
// CHECK:       %7 = cir.alloca i32, cir.ptr <i32>, ["ref.tmp1", init] {alignment = 4 : i64}
// CHECK:       %8 = cir.load %1 : cir.ptr <!cir.ptr<(!cir.ptr<i32>) -> i32>>, !cir.ptr<(!cir.ptr<i32>) -> i32>
// CHECK:       %9 = cir.const(3 : i32) : i32
// CHECK:       cir.store %9, %7 : i32, cir.ptr <i32>

// 3. Call `__invoke()`, which effectively executes `operator()`.
// CHECK:       %10 = cir.call %8(%7) : (!cir.ptr<(!cir.ptr<i32>) -> i32>, !cir.ptr<i32>) -> i32
// CHECK:       cir.yield %10 : i32
// CHECK:     }

// CHECK:   }