// Copyright (c) 2014-2015 Dropbox, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "codegen/compvars.h"

#include <cstdio>
#include <deque>
#include <sstream>

#include "llvm/ADT/DenseSet.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"

#include "codegen/codegen.h"
#include "codegen/gcbuilder.h"
#include "codegen/irgen.h"
#include "codegen/irgen/irgenerator.h"
#include "codegen/irgen/util.h"
#include "codegen/patchpoints.h"
#include "core/options.h"
#include "core/types.h"
#include "runtime/float.h"
#include "runtime/int.h"
#include "runtime/objmodel.h"
#include "runtime/types.h"
#include "runtime/util.h"

namespace pyston {

llvm::Value* RefcountTracker::setType(llvm::Value* v, RefType reftype) {
    auto& var = this->vars[v];

    assert(var.reftype == reftype || var.reftype == RefType::UNKNOWN);
    var.reftype = reftype;
    return v;
}

void RefcountTracker::refConsumed(llvm::Value* v, llvm::Instruction* inst) {
    auto& var = this->vars[v];

    assert(var.reftype != RefType::UNKNOWN);
    var.ref_consumers.push_back(inst);

    // Make sure that this instruction actually references v:
    assert(std::find(inst->op_begin(), inst->op_end(), v) != inst->op_end());
}

llvm::Instruction* findIncrefPt(llvm::BasicBlock* BB) {
    llvm::Instruction* incref_pt;// = BB->getFirstInsertionPt();
    if (llvm::isa<llvm::LandingPadInst>(*BB->begin())) {
        // Don't split up the landingpad+extract+cxa_begin_catch
        auto it = BB->begin();
        ++it;
        ++it;
        ++it;
        incref_pt = &*it;
    } else {
        incref_pt = BB->getFirstInsertionPt();
    }
    return incref_pt;
}

void addIncrefs(llvm::Value* v, int num_refs, llvm::Instruction* incref_pt) {
    assert(num_refs > 0);
#ifdef Py_REF_DEBUG
    auto reftotal_gv = g.cur_module->getOrInsertGlobal("_Py_RefTotal", g.i64);
    auto reftotal = new llvm::LoadInst(reftotal_gv, "", incref_pt);
    auto new_reftotal = llvm::BinaryOperator::Create(
        llvm::BinaryOperator::BinaryOps::Add, reftotal,
        getConstantInt(num_refs, g.i64), "", incref_pt);
    new llvm::StoreInst(new_reftotal, reftotal_gv, incref_pt);
#endif

    llvm::ArrayRef<llvm::Value*> idxs({ getConstantInt(0, g.i32), getConstantInt(0, g.i32) });
    auto refcount_ptr = llvm::GetElementPtrInst::CreateInBounds(v, idxs, "", incref_pt);
    auto refcount = new llvm::LoadInst(refcount_ptr, "", incref_pt);
    auto new_refcount = llvm::BinaryOperator::Create(
        llvm::BinaryOperator::BinaryOps::Add, refcount,
        getConstantInt(num_refs, g.i64), "", incref_pt);
    new llvm::StoreInst(new_refcount, refcount_ptr, incref_pt);
}

void addDecrefs(llvm::Value* v, int num_refs, llvm::Instruction* decref_pt) {
    assert(num_refs > 0);
    llvm::IRBuilder<true> builder(decref_pt);
#ifdef Py_REF_DEBUG
    auto reftotal_gv = g.cur_module->getOrInsertGlobal("_Py_RefTotal", g.i64);
    auto reftotal = new llvm::LoadInst(reftotal_gv, "", decref_pt);
    auto new_reftotal = llvm::BinaryOperator::Create(
        llvm::BinaryOperator::BinaryOps::Sub, reftotal,
        getConstantInt(num_refs, g.i64), "", decref_pt);
    new llvm::StoreInst(new_reftotal, reftotal_gv, decref_pt);
#endif

    llvm::ArrayRef<llvm::Value*> idxs({ getConstantInt(0, g.i32), getConstantInt(0, g.i32) });
    auto refcount_ptr = llvm::GetElementPtrInst::CreateInBounds(v, idxs, "", decref_pt);
    auto refcount = new llvm::LoadInst(refcount_ptr, "", decref_pt);
    auto new_refcount = llvm::BinaryOperator::Create(
        llvm::BinaryOperator::BinaryOps::Sub, refcount,
        getConstantInt(num_refs, g.i64), "", decref_pt);
    new llvm::StoreInst(new_refcount, refcount_ptr, decref_pt);

    llvm::BasicBlock* cur_block = decref_pt->getParent();
    llvm::BasicBlock* dealloc_block = llvm::BasicBlock::Create(g.context, "", decref_pt->getParent()->getParent());
    llvm::BasicBlock* continue_block = cur_block->splitBasicBlock(decref_pt);

    assert(llvm::isa<llvm::BranchInst>(cur_block->getTerminator()));
    cur_block->getTerminator()->eraseFromParent();

    builder.SetInsertPoint(cur_block);
    auto iszero = builder.CreateICmpEQ(new_refcount, getConstantInt(0, g.i64));
    builder.CreateCondBr(iszero, dealloc_block, continue_block);

    builder.SetInsertPoint(dealloc_block);

    auto cls_ptr = builder.CreateConstInBoundsGEP2_32(v, 0, 1);
    auto cls = builder.CreateLoad(cls_ptr);
    auto dtor_ptr = builder.CreateConstInBoundsGEP2_32(cls, 0, 4);

#ifndef NDEBUG
    llvm::APInt offset(64, 0, true);
    assert(llvm::cast<llvm::GetElementPtrInst>(dtor_ptr)->accumulateConstantOffset(*g.tm->getDataLayout(), offset));
    assert(offset.getZExtValue() == offsetof(BoxedClass, tp_dealloc));
#endif
    auto dtor = builder.CreateLoad(dtor_ptr);
    builder.CreateCall(dtor, v);
    builder.CreateBr(continue_block);

    builder.SetInsertPoint(continue_block);
}

void RefcountTracker::addRefcounts(IRGenState* irstate) {
    llvm::Function* f = irstate->getLLVMFunction();
    RefcountTracker* rt = irstate->getRefcounts();

    fprintf(stderr, "Before refcounts:\n");
    fprintf(stderr, "\033[35m");
    dumpPrettyIR(f);
    fprintf(stderr, "\033[0m");

#ifndef NDEBUG
    int num_untracked = 0;
    auto check_val_missed = [&](llvm::Value* v) {
        if (rt->vars.count(v))
            return;

        auto t = v->getType();
        auto p = llvm::dyn_cast<llvm::PointerType>(t);
        if (!p) {
            //t->dump();
            //printf("Not a pointer\n");
            return;
        }
        auto s = llvm::dyn_cast<llvm::StructType>(p->getElementType());
        if (!s) {
            //t->dump();
            //printf("Not a pointer\n");
            return;
        }

        // Take care of inheritance.  It's represented as an instance of the base type at the beginning of the
        // derived type, not as the types concatenated.
        while (s->elements().size() > 0 && llvm::isa<llvm::StructType>(s->elements()[0]))
            s = llvm::cast<llvm::StructType>(s->elements()[0]);

        bool ok_type = false;
        if (s->elements().size() >= 2 && s->elements()[0] == g.i64 && s->elements()[1] == g.llvm_class_type_ptr) {
            //printf("This looks likes a class\n");
            ok_type = true;
        }

        if (!ok_type) {
#ifndef NDEBUG
            if (s->getName().startswith("struct.pyston::Box") || (s->getName().startswith("Py") || s->getName().endswith("Object")) || s->getName().startswith("class.pyston::Box")) {
                v->dump();
                if (s && s->elements().size() >= 2) {
                    s->elements()[0]->dump();
                    s->elements()[1]->dump();
                }
                fprintf(stderr, "This is named like a refcounted object though it doesn't look like one");
                assert(0);
            }
#endif
            return;
        }

        if (rt->vars.count(v) == 0) {
            num_untracked++;
            printf("missed a refcounted object: ");
            fflush(stdout);
            v->dump();
            //abort();
        }
    };

    for (auto&& g : f->getParent()->getGlobalList()) {
        //g.dump();
        check_val_missed(&g);
    }

    for (auto&& a : f->args()) {
        check_val_missed(&a);
    }

    for (auto&& BB : *f) {
        for (auto&& inst : BB) {
            check_val_missed(&inst);
            for (auto&& u : inst.uses()) {
                check_val_missed(u.get());
            }
            for (auto&& op : inst.operands()) {
                check_val_missed(op);
            }
        }
    }
    ASSERT(num_untracked == 0, "");
#endif

    std::deque<llvm::BasicBlock*> block_queue;
    struct RefState {
        llvm::DenseMap<llvm::Value*, int> refs;
        //llvm::Instruction* 
    };
    llvm::DenseMap<llvm::BasicBlock*, RefState> states;

    for (auto&& BB : *f) {
        if (llvm::succ_begin(&BB) == llvm::succ_end(&BB)) {
            printf("%s is a terminator block\n", BB.getName().data());

            block_queue.push_back(&BB);
        }
    }

    while (!block_queue.empty()) {
        llvm::BasicBlock& BB = *block_queue.front();
        block_queue.pop_front();

#if 0
        llvm::Instruction* term_inst = BB.getTerminator();
        llvm::Instruction* insert_before = term_inst;
        if (llvm::isa<llvm::UnreachableInst>(insert_before)) {
            insert_before = &*(++BB.rbegin());
            assert(llvm::isa<llvm::CallInst>(insert_before) || llvm::isa<llvm::IntrinsicInst>(insert_before));
        }
#endif

        llvm::outs() << "Processing " << BB.getName() << '\n';

        assert(!states.count(&BB));
        RefState& state = states[&BB];

        llvm::SmallVector<llvm::BasicBlock*, 4> successors;
        successors.insert(successors.end(), llvm::succ_begin(&BB), llvm::succ_end(&BB));
        if (successors.size()) {
            llvm::DenseSet<llvm::Value*> tracked_values;
            for (auto SBB : successors) {
                assert(states.count(SBB));
                for (auto&& p : states[SBB].refs) {
                    tracked_values.insert(p.first);
                }
            }

            for (auto v : tracked_values) {
                assert(rt->vars.count(v));
                auto refstate = rt->vars[v];

                if (refstate.reftype == RefType::BORROWED) {
                    int min_refs = 1000000000;
                    for (auto SBB : successors) {
                        auto it = states[SBB].refs.find(v);
                        if (it != states[SBB].refs.end()) {
                            min_refs = std::min(it->second, min_refs);
                        } else
                            min_refs = 0;
                    }

                    for (auto SBB : successors) {
                        auto it = states[SBB].refs.find(v);
                        int this_refs = 0;
                        if (it != states[SBB].refs.end()) {
                            this_refs = it->second;
                        }
                        if (this_refs > min_refs) {
                            addIncrefs(v, this_refs - min_refs, findIncrefPt(SBB));
                            //llvm::outs() << *incref_pt << '\n';
                            //llvm::outs() << "Need to incref " << *v << " at beginning of " << SBB->getName() << "\n";
                        }
                    }

                    if (min_refs)
                        state.refs[v] = min_refs;
                    else
                        assert(state.refs.count(v) == 0);
                } else {
                    assert(0 && "finish implementing");
                }
            }
        }

        for (auto &I : llvm::iterator_range<llvm::BasicBlock::reverse_iterator>(BB.rbegin(), BB.rend())) {
            llvm::DenseMap<llvm::Value*, int> num_consumed_by_inst;
            llvm::DenseMap<llvm::Value*, int> num_times_as_op;

            for (llvm::Value* op : I.operands()) {
                auto it = rt->vars.find(op);
                if (it == rt->vars.end())
                    continue;

                int& nops = num_times_as_op[op];
                nops++;

                if (nops > 1)
                    continue;

                auto&& var_state = it->second;
                for (auto consuming_inst : var_state.ref_consumers) {
                    if (consuming_inst == &I)
                        num_consumed_by_inst[op]++;
                }
            }

            for (auto&& p : num_times_as_op) {
                auto& op = p.first;

                auto&& it = num_consumed_by_inst.find(op);
                int num_consumed = 0;
                if (it != num_consumed_by_inst.end())
                    num_consumed = it->second;

                if (num_times_as_op[op] > num_consumed) {
                    if (rt->vars[op].reftype == RefType::BORROWED) {
                    } else {
                        assert(state.refs.count(op) && "handle this case (last reference of an owned var)");
                    }
                }

                if (num_consumed)
                    state.refs[op] += num_consumed;
            }
        }

        // Handle variables that were defined in this BB:
        for (auto&& p : rt->vars) {
            llvm::Instruction* inst = llvm::dyn_cast<llvm::Instruction>(p.first);
            if (inst && inst->getParent() == &BB) {
                int starting_refs = (p.second.reftype == RefType::OWNED ? 1 : 0);
                if (state.refs[inst] != starting_refs) {
                    llvm::Instruction* insertion_pt = inst->getNextNode();
                    assert(insertion_pt);
                    if (state.refs[inst] < starting_refs) {
                        assert(p.second.reftype == RefType::OWNED);
                        addDecrefs(inst, starting_refs - state.refs[inst], insertion_pt);
                    } else {
                        addIncrefs(inst, state.refs[inst] - starting_refs, insertion_pt);
                    }
                }
                state.refs.erase(inst);
            }
        }

        if (&BB == &BB.getParent()->front()) {
            for (auto&& p : state.refs) {
                llvm::outs() << *p.first << " " << p.second << '\n';
                assert(llvm::isa<llvm::GlobalVariable>(p.first));
                assert(rt->vars[p.first].reftype == RefType::BORROWED);
                addIncrefs(p.first, p.second, findIncrefPt(&BB));
            }
            state.refs.clear();
        }

        llvm::outs() << "End of " << BB.getName() << ":\n";
        for (auto&& p : state.refs) {
            llvm::outs() << "With " << p.second << " refs: " << *p.first << '\n';
        }
        llvm::outs() << '\n';

        for (auto&& PBB : llvm::iterator_range<llvm::pred_iterator>(llvm::pred_begin(&BB), llvm::pred_end(&BB))) {
            bool all_succ_done = true;
            for (auto&& SBB : llvm::iterator_range<llvm::succ_iterator>(llvm::succ_begin(PBB), llvm::succ_end(PBB))) {
                if (!states.count(SBB)) {
                    all_succ_done = false;
                    break;
                }
            }

            if (all_succ_done) {
                llvm::outs() << PBB->getName() << " is now ready to be processed\n";
                block_queue.push_back(PBB);
            }
        }
    }

    fprintf(stderr, "After refcounts:\n");
    fprintf(stderr, "\033[35m");
    f->dump();
    //dumpPrettyIR(f);
    fprintf(stderr, "\033[0m");
}

} // namespace pyston
