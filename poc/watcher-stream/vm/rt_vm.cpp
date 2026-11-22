// The plan bytecode interpreter (rt_vm.h). Each case is plan2cpp's C++
// statement for the same opcode (plan2cpp/src/code.rs `statement`).
#include "rt_vm.h"

#include <vector>

namespace rt_vm {

using rt::V;

V run(const Ins* code, uint32_t count, rt::Ctx& c) {
  V st[kMaxStack];
  V lc[kMaxLocals];
  int sp = 0, lp = 0;
  for (uint32_t pc = 0; pc < count;) {
    const Ins& i = code[pc++];
    switch (i.op) {
      case kNumber: st[sp++] = V::num(i.n); break;
      case kBool: st[sp++] = V::boolean(i.a != 0); break;
      case kStr: st[sp++] = V::str(i.s ? i.s : ""); break;
      case kNone: st[sp++] = V(); break;
      case kLoadSlot: st[sp++] = c.slot(i.a); break;
      case kLoadDerive: st[sp++] = c.derive(i.a); break;
      case kLoadResource: st[sp++] = c.resource(i.a); break;
      case kLoadParam: st[sp++] = c.param(i.a); break;
      case kLoadLocal: st[sp++] = lc[i.a]; break;
      case kLoadItem: st[sp++] = c.item(i.a); break;
      case kField: st[sp - 1] = st[sp - 1].field(i.a); break;
      case kRecord:
      case kList: {
        const int n = static_cast<int>(i.op == kRecord ? i.b : i.a);
        sp -= n;
        std::vector<V> items(st + sp, st + sp + n);
        st[sp] = i.op == kRecord ? V::record(std::move(items)) : V::list(std::move(items));
        ++sp;
        break;
      }
      case kNot: st[sp - 1] = V::boolean(!st[sp - 1].b); break;
      case kNeg: st[sp - 1] = V::num(-st[sp - 1].n); break;
      case kSome: break;  // some(v): the runtime holds it as v itself
      case kAdd: case kSub: case kMul: case kDiv: case kRem: case kEq: case kNe: case kLt: case kLe:
      case kGt: case kGe: case kConcat: {
        V rhs = st[--sp];
        V& lhs = st[sp - 1];
        switch (i.op) {
          case kAdd: lhs = rt::add(lhs, rhs); break;
          case kSub: lhs = rt::sub(lhs, rhs); break;
          case kMul: lhs = rt::mul(lhs, rhs); break;
          case kDiv: lhs = rt::div(lhs, rhs); break;
          case kRem: lhs = rt::rem(lhs, rhs); break;
          case kEq: lhs = rt::eq(lhs, rhs); break;
          case kNe: lhs = rt::ne(lhs, rhs); break;
          case kLt: lhs = rt::lt(lhs, rhs); break;
          case kLe: lhs = rt::le(lhs, rhs); break;
          case kGt: lhs = rt::gt(lhs, rhs); break;
          case kGe: lhs = rt::ge(lhs, rhs); break;
          default: lhs = rt::concat(lhs, rhs); break;
        }
        break;
      }
      case kJump: pc = i.a; break;
      case kJumpIfFalse:
        if (!st[--sp].b) pc = i.a;
        break;
      case kStoreSlot: c.write(i.a, st[--sp]); break;
      case kPop: --sp; break;
      case kBindLocal: lc[lp++] = st[--sp]; break;
      case kDropLocal: --lp; break;
      case kCall: {
        if (i.a == kNow) {
          st[sp++] = c.now();
        } else if (i.a >= kRouteBase) {
          const int argc = static_cast<int>(i.b);
          sp -= argc;
          st[sp] = c.call(static_cast<uint8_t>(i.a - kRouteBase), &st[sp], argc);
          ++sp;
        } else if (i.b == 1) {
          V& x = st[sp - 1];
          switch (i.a) {
            case kFloor: x = rt::floor(x); break;
            case kCeil: x = rt::ceil(x); break;
            case kRound: x = rt::round(x); break;
            case kToString: x = rt::to_string(x); break;
            default: x = V(); break;
          }
        } else {
          V rhs = st[--sp];
          V& x = st[sp - 1];
          switch (i.a) {
            case kToFixed: x = rt::to_fixed(x, rhs); break;
            case kMax: x = rt::max(x, rhs); break;
            case kMin: x = rt::min(x, rhs); break;
            default: x = V(); break;
          }
        }
        break;
      }
      case kCommand: {
        const int argc = static_cast<int>(i.b);
        sp -= argc;
        c.command(i.s, st + sp, argc);
        break;
      }
      case kSend: {
        const int argc = static_cast<int>(i.c);
        sp -= argc;
        c.send(static_cast<uint16_t>(i.a), i.s, st + sp, argc);
        break;
      }
      case kNativeProps: {
        const int n = static_cast<int>(i.a);
        sp -= 2 * n;
        st[sp] = rt::native_props(&st[sp], n);
        ++sp;
        break;
      }
      case kReturn: return sp > 0 ? st[sp - 1] : V();
      default: return V();
    }
  }
  return sp > 0 ? st[sp - 1] : V();
}

}  // namespace rt_vm
