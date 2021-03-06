// Copyright (c) 2014-2016 Dropbox, Inc.
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

#include <cstdio>
#include <deque>
#include <queue>
#include <sstream>

#include "llvm/ADT/DenseSet.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"

#include "codegen/codegen.h"
#include "codegen/compvars.h"
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

static int numSuccessors(llvm::BasicBlock* b) {
    return std::distance(llvm::succ_begin(b), llvm::succ_end(b));
}

static int numPredecessors(llvm::BasicBlock* b) {
    return std::distance(llvm::pred_begin(b), llvm::pred_end(b));
}

llvm::Value* RefcountTracker::setType(llvm::Value* v, RefType reftype) {
    assert(!llvm::isa<llvm::UndefValue>(v));

    // Force tracked cast expressions to be immediately after the thing they cast.
    // Otherwise there is the opportunity for things to happen between them, which
    // may cause the refcount state to be examined, and the setType() call will not
    // be seen yet.
    //
    // We could relax this restriction by looking through the cast, or by requiring
    // the caller to also call setType() on the uncasted value.  This is a simpler
    // fix for now though.
    if (llvm::CastInst* cast = llvm::dyn_cast<llvm::CastInst>(v)) {
        auto uncasted = cast->getOperand(0);
        auto uncasted_inst = llvm::cast<llvm::Instruction>(uncasted);
        auto uncasted_invoke = llvm::dyn_cast<llvm::InvokeInst>(uncasted_inst);
        if (uncasted_invoke)
            assert(uncasted_invoke->getNormalDest()->getFirstNonPHI() == cast
                   && "Refcount-tracked casts must be immediately after the value they cast");
        else
            assert(uncasted_inst->getNextNode() == cast
                   && "Refcount-tracked casts must be immediately after the value they cast");
    }

    auto& var = this->vars[v];

    assert(var.reftype == reftype || var.reftype == RefType::UNKNOWN);
    var.reftype = reftype;

    if (llvm::isa<ConstantPointerNull>(v))
        var.nullable = true;

    return v;
}

llvm::Value* RefcountTracker::setNullable(llvm::Value* v, bool nullable) {
    assert(!llvm::isa<llvm::UndefValue>(v));

    auto& var = this->vars[v];

    assert(var.nullable == nullable || var.nullable == false);
    var.nullable = nullable;
    return v;
}

bool RefcountTracker::isNullable(llvm::Value* v) {
    assert(vars.count(v));
    return vars.lookup(v).nullable;
}

void RefcountTracker::refConsumed(llvm::Value* v, llvm::Instruction* inst) {
    if (llvm::isa<UndefValue>(v) || llvm::isa<ConstantPointerNull>(v))
        return;

    assert(this->vars[v].reftype != RefType::UNKNOWN);

    this->refs_consumed[inst].push_back(v);
}

void RefcountTracker::refUsed(llvm::Value* v, llvm::Instruction* inst) {
    if (llvm::isa<UndefValue>(v) || llvm::isa<ConstantPointerNull>(v))
        return;

    assert(this->vars[v].reftype != RefType::UNKNOWN);

    this->refs_used[inst].push_back(v);
}

void RefcountTracker::setMayThrow(llvm::Instruction* inst) {
    assert(!may_throw.count(inst));
    this->may_throw.insert(inst);
}


void remapPhis(llvm::BasicBlock* in_block, llvm::BasicBlock* from_block, llvm::BasicBlock* new_from_block) {
    for (llvm::Instruction& i : *in_block) {
        llvm::Instruction* I = &i;
        llvm::PHINode* phi = llvm::dyn_cast<llvm::PHINode>(I);
        if (!phi)
            break;

        int idx = phi->getBasicBlockIndex(from_block);
        if (idx == -1)
            continue;
        phi->setIncomingBlock(idx, new_from_block);
    }
}

typedef llvm::DenseMap<std::pair<llvm::BasicBlock*, llvm::BasicBlock*>, llvm::Instruction*> InsertionCache;
llvm::Instruction* findInsertionPoint(llvm::BasicBlock* BB, llvm::BasicBlock* from_bb, InsertionCache& cache) {
    assert(BB);
    assert(BB != from_bb);

    auto key = std::make_pair(BB, from_bb);

    auto it = cache.find(key);
    if (it != cache.end())
        return it->second;

    // Break critical edges if we need to:
    if (numPredecessors(BB) > 1) {
        ASSERT(from_bb, "Don't know how to break the critical edge to(%s)", BB->getName().data());

        llvm::BasicBlock* breaker_block = llvm::BasicBlock::Create(g.context, "breaker", from_bb->getParent(), BB);
        llvm::BranchInst::Create(BB, breaker_block);
        // llvm::outs() << "Breaking edge from " << from_bb->getName() << " to " << BB->getName() << "; name is "
        //<< breaker_block->getName() << '\n';

        auto terminator = from_bb->getTerminator();

        if (llvm::BranchInst* br = llvm::dyn_cast<llvm::BranchInst>(terminator)) {
            if (br->getSuccessor(0) == BB)
                br->setSuccessor(0, breaker_block);
            if (br->isConditional() && br->getSuccessor(1) == BB)
                br->setSuccessor(1, breaker_block);
        } else if (llvm::InvokeInst* ii = llvm::dyn_cast<llvm::InvokeInst>(terminator)) {
            if (ii->getNormalDest() == BB)
                ii->setNormalDest(breaker_block);
            ASSERT(ii->getUnwindDest() != BB, "don't know how break critical unwind edges");
        } else {
            llvm::outs() << *terminator << '\n';
            RELEASE_ASSERT(0, "unhandled terminator type");
        }

        remapPhis(BB, from_bb, breaker_block);

        cache[key] = breaker_block->getFirstInsertionPt();
        return cache[key];
    }

    if (llvm::isa<llvm::LandingPadInst>(*BB->begin())) {
        // Don't split up the landingpad+extract+cxa_begin_catch
        auto it = BB->begin();
        ++it;
        ++it;
        ++it;
        cache[key] = it;
        return &*it;
    } else {
        for (llvm::Instruction& I : *BB) {
            if (!llvm::isa<llvm::PHINode>(I) && !llvm::isa<llvm::AllocaInst>(I)) {
                cache[key] = &I;
                return &I;
            }
        }
        abort();
    }
}

#ifdef Py_TRACE_REFS
#define REFCOUNT_IDX 2
#else
#define REFCOUNT_IDX 0
#endif

void addIncrefs(llvm::Value* v, bool nullable, int num_refs, llvm::Instruction* incref_pt) {
    if (num_refs > 1) {
        // Not bad but I don't think this should happen:
        // printf("Whoa more than one incref??\n");
        // raise(SIGTRAP);
    }

    if (isa<ConstantPointerNull>(v)) {
        assert(nullable);
        return;
    }

    assert(num_refs > 0);

    llvm::BasicBlock* cur_block;
    llvm::BasicBlock* continue_block = NULL;
    llvm::BasicBlock* incref_block;

    llvm::IRBuilder<true> builder(incref_pt);

    // Deal with subtypes of Box:
    while (v->getType() != g.llvm_value_type_ptr) {
        v = builder.CreateConstInBoundsGEP2_32(v, 0, 0);
    }

    if (nullable) {
        cur_block = incref_pt->getParent();
        continue_block = cur_block->splitBasicBlock(incref_pt);
        incref_block
            = llvm::BasicBlock::Create(g.context, "incref", incref_pt->getParent()->getParent(), continue_block);

        assert(llvm::isa<llvm::BranchInst>(cur_block->getTerminator()));
        cur_block->getTerminator()->eraseFromParent();

        builder.SetInsertPoint(cur_block);
        auto isnull = builder.CreateICmpEQ(v, getNullPtr(g.llvm_value_type_ptr));
        builder.CreateCondBr(isnull, continue_block, incref_block);

        builder.SetInsertPoint(incref_block);
    }

#ifdef Py_REF_DEBUG
    auto reftotal_gv = g.cur_module->getOrInsertGlobal("_Py_RefTotal", g.i64);
    auto reftotal = builder.CreateLoad(reftotal_gv);
    auto new_reftotal = builder.CreateAdd(reftotal, getConstantInt(num_refs, g.i64));
    builder.CreateStore(new_reftotal, reftotal_gv);
#endif

    auto refcount_ptr = builder.CreateConstInBoundsGEP2_32(v, 0, REFCOUNT_IDX);
    auto refcount = builder.CreateLoad(refcount_ptr);
    auto new_refcount = builder.CreateAdd(refcount, getConstantInt(num_refs, g.i64));
    builder.CreateStore(new_refcount, refcount_ptr);

    if (nullable)
        builder.CreateBr(continue_block);
}

void addDecrefs(llvm::Value* v, bool nullable, int num_refs, llvm::Instruction* decref_pt) {
    if (num_refs > 1) {
        // Not bad but I don't think this should happen:
        printf("Whoa more than one decref??\n");
        raise(SIGTRAP);
    }

    // TODO -- assert that v isn't a constant None?  Implies wasted extra increfs/decrefs

    if (isa<ConstantPointerNull>(v)) {
        assert(nullable);
        return;
    }

    assert(num_refs > 0);
    llvm::IRBuilder<true> builder(decref_pt);

    if (nullable) {
        llvm::BasicBlock* cur_block = decref_pt->getParent();
        llvm::BasicBlock* continue_block = cur_block->splitBasicBlock(decref_pt);
        llvm::BasicBlock* decref_block
            = llvm::BasicBlock::Create(g.context, "decref", decref_pt->getParent()->getParent(), continue_block);

        assert(llvm::isa<llvm::BranchInst>(cur_block->getTerminator()));
        cur_block->getTerminator()->eraseFromParent();

        builder.SetInsertPoint(cur_block);
        auto isnull = builder.CreateICmpEQ(v, getNullPtr(v->getType()));
        builder.CreateCondBr(isnull, continue_block, decref_block);

        builder.SetInsertPoint(decref_block);
        auto jmp = builder.CreateBr(continue_block);
        addDecrefs(v, false, num_refs, jmp);
        return;
    }

    RELEASE_ASSERT(num_refs == 1, "decref patchpoints don't support >1 refs");

    llvm::Function* patchpoint
        = llvm::Intrinsic::getDeclaration(g.cur_module, llvm::Intrinsic::experimental_patchpoint_void);
    int pp_id = nullable ? XDECREF_PP_ID : DECREF_PP_ID;
    int pp_size = nullable ? XDECREF_PP_SIZE : DECREF_PP_SIZE;
    builder.CreateCall(patchpoint, { getConstantInt(pp_id, g.i64), getConstantInt(pp_size, g.i32), getNullPtr(g.i8_ptr),
                                     getConstantInt(1, g.i32), v });

#if 0
    // Deal with subtypes of Box:
    while (v->getType() != g.llvm_value_type_ptr) {
        v = builder.CreateConstInBoundsGEP2_32(v, 0, 0);
    }

#ifdef Py_REF_DEBUG
    auto reftotal_gv = g.cur_module->getOrInsertGlobal("_Py_RefTotal", g.i64);
    auto reftotal = new llvm::LoadInst(reftotal_gv, "", decref_pt);
    auto new_reftotal = llvm::BinaryOperator::Create(llvm::BinaryOperator::BinaryOps::Sub, reftotal,
                                                     getConstantInt(num_refs, g.i64), "", decref_pt);
    new llvm::StoreInst(new_reftotal, reftotal_gv, decref_pt);
#endif

    llvm::ArrayRef<llvm::Value*> idxs({ getConstantInt(0, g.i32), getConstantInt(REFCOUNT_IDX, g.i32) });
    auto refcount_ptr = llvm::GetElementPtrInst::CreateInBounds(v, idxs, "", decref_pt);
    auto refcount = new llvm::LoadInst(refcount_ptr, "", decref_pt);
    auto new_refcount = llvm::BinaryOperator::Create(llvm::BinaryOperator::BinaryOps::Sub, refcount,
                                                     getConstantInt(num_refs, g.i64), "", decref_pt);
    new llvm::StoreInst(new_refcount, refcount_ptr, decref_pt);

#ifdef Py_REF_DEBUG
    llvm::CallInst::Create(g.funcs.checkRefs, { v }, "", decref_pt);
#endif

    llvm::BasicBlock* cur_block = decref_pt->getParent();
    llvm::BasicBlock* continue_block = cur_block->splitBasicBlock(decref_pt);
    llvm::BasicBlock* dealloc_block
        = llvm::BasicBlock::Create(g.context, "dealloc", decref_pt->getParent()->getParent(), continue_block);

    assert(llvm::isa<llvm::BranchInst>(cur_block->getTerminator()));
    cur_block->getTerminator()->eraseFromParent();

    builder.SetInsertPoint(cur_block);
    auto iszero = builder.CreateICmpEQ(new_refcount, getConstantInt(0, g.i64));
    builder.CreateCondBr(iszero, dealloc_block, continue_block);

    builder.SetInsertPoint(dealloc_block);

#ifdef COUNT_ALLOCS
#error "Don't support COUNT_ALLOCS here yet"
#endif

#ifdef Py_TRACE_REFS
    builder.CreateCall(g.funcs._Py_Dealloc, v);
#else
    auto cls_ptr = builder.CreateConstInBoundsGEP2_32(v, 0, 1 + REFCOUNT_IDX);
    auto cls = builder.CreateLoad(cls_ptr);
    auto dtor_ptr = builder.CreateConstInBoundsGEP2_32(cls, 0, 4);

#ifndef NDEBUG
    llvm::APInt offset(64, 0, true);
    assert(llvm::cast<llvm::GetElementPtrInst>(dtor_ptr)->accumulateConstantOffset(*g.tm->getDataLayout(), offset));
    assert(offset.getZExtValue() == offsetof(BoxedClass, tp_dealloc));
#endif
    auto dtor = builder.CreateLoad(dtor_ptr);
    builder.CreateCall(dtor, v);
#endif

    builder.CreateBr(continue_block);

    builder.SetInsertPoint(continue_block);
#endif
}

void addCXXFixup(llvm::Instruction* inst, const llvm::SmallVector<llvm::TrackingVH<llvm::Value>, 4>& to_decref,
                 RefcountTracker* rt) {
    // inst->getParent()->getParent()->dump();
    // inst->dump();

    ASSERT(!llvm::isa<llvm::InvokeInst>(inst),
           "don't need a fixup here!"); // could either not ask for the fixup or maybe just skip it here
    assert(llvm::isa<llvm::CallInst>(inst));

    llvm::CallInst* call = llvm::cast<llvm::CallInst>(inst);

    llvm::BasicBlock* cur_block = inst->getParent();
    llvm::BasicBlock* continue_block = cur_block->splitBasicBlock(inst);
    llvm::BasicBlock* fixup_block
        = llvm::BasicBlock::Create(g.context, "cxx_fixup", inst->getParent()->getParent(), continue_block);

    assert(llvm::isa<llvm::BranchInst>(cur_block->getTerminator()));
    cur_block->getTerminator()->eraseFromParent();

    // llvm::SmallVector<llvm::Value*, 4> args(call->arg_begin(), call->arg_end());
    llvm::SmallVector<llvm::Value*, 4> args(call->arg_operands().begin(), call->arg_operands().end());
    llvm::InvokeInst* new_invoke
        = InvokeInst::Create(call->getCalledValue(), continue_block, fixup_block, args, call->getName(), cur_block);
    new_invoke->setAttributes(call->getAttributes());
    new_invoke->setDebugLoc(call->getDebugLoc());
    assert(!call->hasMetadataOtherThanDebugLoc());
    call->replaceAllUsesWith(new_invoke);
    call->eraseFromParent();

    llvm::IRBuilder<true> builder(fixup_block);

    static llvm::Function* _personality_func = g.stdlib_module->getFunction("__gxx_personality_v0");
    assert(_personality_func);
    llvm::Value* personality_func
        = g.cur_module->getOrInsertFunction(_personality_func->getName(), _personality_func->getFunctionType());
    assert(personality_func);
    static llvm::Type* lp_type = llvm::StructType::create(std::vector<llvm::Type*>{ g.i8_ptr, g.i64 });
    assert(lp_type);
    llvm::LandingPadInst* landing_pad = builder.CreateLandingPad(lp_type, personality_func, 1);
    landing_pad->addClause(getNullPtr(g.i8_ptr));

    llvm::SmallVector<llvm::Value*, 4> call_args;
    llvm::Value* cxaexc_pointer = builder.CreateExtractValue(landing_pad, { 0 });
    call_args.push_back(cxaexc_pointer);
    call_args.push_back(getConstantInt(to_decref.size(), g.i32));
    call_args.append(to_decref.begin(), to_decref.end());
    builder.CreateCall(g.funcs.xdecrefAndRethrow, call_args);
    builder.CreateUnreachable();
}

// TODO: this should be cleaned up and moved to src/core/
template <typename K, typename V> class OrderedMap {
private:
    llvm::DenseMap<K, V> map;
    std::vector<K> order;

public:
    V& operator[](const K& k) {
        if (map.count(k) == 0)
            order.push_back(k);

        return map[k];
    }

    const V& get(const K& k) const { return map.find(k)->second; }

    size_t size() const {
        assert(order.size() == map.size());
        return order.size();
    }

    void clear() {
        map.clear();
        order.clear();
    }

    // TODO: this is slow
    void erase(const K& k) {
        assert(map.count(k));
        map.erase(k);
        order.erase(std::remove(order.begin(), order.end(), k), order.end());
    }

    class const_iterator {
    private:
        const OrderedMap* themap;
        int idx;

        const_iterator(const OrderedMap* themap, int idx) : themap(themap), idx(idx) {}

    public:
        std::pair<K, V> operator*() { return *themap->map.find(themap->order[idx]); }
        std::pair<K, V> operator->() { return *this; }

        bool operator==(const const_iterator& rhs) const {
            assert(themap == rhs.themap);
            return idx == rhs.idx;
        }
        bool operator!=(const const_iterator& rhs) const { return !(*this == rhs); }

        const_iterator& operator++() {
            idx++;
            return *this;
        }

        friend class OrderedMap;
    };

    size_t count(K& k) const { return map.count(k); }
    const_iterator begin() const { return const_iterator(this, 0); }
    const_iterator end() const { return const_iterator(this, size()); }
};

// TODO: this should be cleaned up and moved to src/core/
template <typename K, typename V> class SmallOrderedMap {
private:
    std::vector<std::pair<K, V>> v;

public:
    V& operator[](const K& k) {
        for (auto&& p : v)
            if (p.first == k)
                return p.second;

        v.emplace_back(k, V());
        return v.back().second;
    }

    size_t size() const { return v.size(); }

    V get(const K& k) const {
        for (auto&& p : v)
            if (p.first == k)
                return p.second;
        return V();
    }

    typename decltype(v)::iterator begin() { return v.begin(); }
    typename decltype(v)::iterator end() { return v.end(); }
};

// An optimized representation of the graph of llvm::BasicBlock's, since we will be dealing with them a lot.
struct BBGraph {
public:
    llvm::DenseMap<llvm::BasicBlock*, int> bb_idx;
    std::vector<llvm::BasicBlock*> bbs;

    std::vector<llvm::SmallVector<int, 4>> predecessors;
    std::vector<llvm::SmallVector<int, 4>> successors;

    explicit BBGraph(llvm::Function* f) : predecessors(f->size()), successors(f->size()) {
        int num_bb = f->size();

        bbs.reserve(num_bb);

        for (auto&& B : *f) {
            bb_idx[&B] = bbs.size();
            bbs.push_back(&B);
        }

        for (auto&& B : *f) {
            int idx = bb_idx[&B];
            for (auto&& PBB : llvm::iterator_range<llvm::pred_iterator>(llvm::pred_begin(&B), llvm::pred_end(&B)))
                predecessors[idx].push_back(bb_idx[PBB]);
            for (auto&& SBB : llvm::iterator_range<llvm::succ_iterator>(llvm::succ_begin(&B), llvm::succ_end(&B)))
                successors[idx].push_back(bb_idx[SBB]);
        }
    }

    int numBB() const { return bbs.size(); }
};

static std::vector<int> computeTraversalOrder(const BBGraph& bbg) {
    int num_bb = bbg.numBB();

    std::vector<int> ordering;
    ordering.reserve(num_bb);
    std::vector<bool> added(num_bb);
    std::vector<int> num_successors_added(num_bb);

    for (int i = 0; i < num_bb; i++) {
        if (bbg.successors[i].size() == 0) {
            // llvm::outs() << "Adding " << BB.getName() << " since it is an exit node.\n";
            ordering.push_back(i);
            added[i] = true;
        }
    }

    int check_predecessors_idx = 0;
    while (ordering.size() < num_bb) {
        if (check_predecessors_idx < ordering.size()) {
            // Case 1: look for any blocks whose successors have already been traversed.

            int idx = ordering[check_predecessors_idx];
            check_predecessors_idx++;

            for (int pidx : bbg.predecessors[idx]) {
                if (added[pidx])
                    continue;

                num_successors_added[pidx]++;
                int num_successors = bbg.successors[pidx].size();
                if (num_successors_added[pidx] == num_successors) {
                    ordering.push_back(pidx);
                    added[pidx] = true;
                    // llvm::outs() << "Adding " << PBB->getName() << " since it has all of its successors added.\n";
                }
            }
        } else {
            // Case 2: we hit a cycle.  Try to pick a good node to add.
            // The heuristic here is just to make sure to pick one in 0-successor component of the SCC

            std::vector<std::pair<int, int>> num_successors;
            for (int i = 0; i < num_bb; i++) {
                if (num_successors_added[i] == 0)
                    continue;
                if (added[i])
                    continue;
                num_successors.push_back(std::make_pair(i, num_successors_added[i]));
            }

            std::sort(
                num_successors.begin(), num_successors.end(),
                [](const std::pair<int, int>& p1, const std::pair<int, int>& p2) { return p1.second > p2.second; });

            std::deque<int> visit_queue;
            std::vector<bool> visited(num_bb);
            int best = -1;

            for (auto&& p : num_successors) {
                if (visited[p.first])
                    continue;

                best = p.first;
                visit_queue.push_back(p.first);
                visited[p.first] = true;

                while (visit_queue.size()) {
                    int idx = visit_queue.front();
                    visit_queue.pop_front();

                    for (int sidx : bbg.successors[idx]) {
                        if (!visited[sidx]) {
                            visited[sidx] = true;
                            visit_queue.push_back(sidx);
                        }
                    }
                }
            }

#ifndef NDEBUG
            // This can currently get tripped if we generate an LLVM IR that has an infinite loop in it.
            // This could definitely be supported, but I don't think we should be generating those cases anyway.
            if (best == -1) {
                for (auto& idx : ordering) {
                    llvm::outs() << "added to " << bbg.bbs[idx]->getName() << '\n';
                }
                for (auto& BB : bbg.bbs) {
                    if (!added[bbg.bb_idx.find(BB)->second])
                        llvm::outs() << "never got to " << BB->getName() << '\n';
                }
            }
#endif
            assert(best != -1);
            ordering.push_back(best);
            added[best] = true;
            // llvm::outs() << "Adding " << best->getName() << " since it is the best provisional node.\n";
        }
    }

    assert(ordering.size() == num_bb);
    assert(added.size() == num_bb);
    return ordering;
}

class BlockOrderer {
private:
    std::vector<int> priority; // lower goes first

    struct BlockComparer {
        bool operator()(std::pair<int, int> lhs, std::pair<int, int> rhs) {
            assert(lhs.second != rhs.second);
            return lhs.second > rhs.second;
        }
    };

    std::vector<bool> in_queue;
    std::priority_queue<std::pair<int, int>, std::vector<std::pair<int, int>>, BlockComparer> queue;

public:
    BlockOrderer(std::vector<int> order) : priority(order.size()), in_queue(order.size()) {
        for (int i = 0; i < order.size(); i++) {
            // errs() << "DETERMINISM: " << order[i]->getName() << " has priority " << i << '\n';
            priority[order[i]] = i;
        }
    }

    void add(int idx) {
        // errs() << "DETERMINISM: adding " << b->getName() << '\n';
        if (in_queue[idx])
            return;
        in_queue[idx] = true;
        queue.push(std::make_pair(idx, priority[idx]));
    }

    int pop() {
        if (!queue.size()) {
            return -1;
        }

        int idx = queue.top().first;
        // errs() << "DETERMINISM: popping " << b->getName() << " (priority " << priority[b] << " \n";
        queue.pop();
        assert(in_queue[idx]);
        in_queue[idx] = false;
        return idx;
    }
};

typedef OrderedMap<llvm::Value*, int> BlockMap;
bool endingRefsDifferent(const BlockMap& lhs, const BlockMap& rhs) {
    if (lhs.size() != rhs.size())
        return true;
    for (auto&& p : lhs) {
        if (rhs.count(p.first) == 0)
            return true;
        if (p.second != rhs.get(p.first))
            return true;
    }
    return false;
}

void RefcountTracker::addRefcounts(IRGenState* irstate) {
    Timer _t("refcounting");

    llvm::Function* f = irstate->getLLVMFunction();
    RefcountTracker* rt = irstate->getRefcounts();

    int num_bb = f->size();
    BBGraph bbg(f);

    if (VERBOSITY() >= 2) {
        fprintf(stderr, "Before refcounts:\n");
        fprintf(stderr, "\033[35m");
        dumpPrettyIR(f);
        fprintf(stderr, "\033[0m");
    }

#ifndef NDEBUG
    int num_untracked = 0;
    auto check_val_missed = [&](llvm::Value* v) {
        if (rt->vars.count(v))
            return;

        if (llvm::isa<UndefValue>(v))
            return;

        auto t = v->getType();
        auto p = llvm::dyn_cast<llvm::PointerType>(t);
        if (!p) {
            // t->dump();
            // printf("Not a pointer\n");
            return;
        }
        auto s = llvm::dyn_cast<llvm::StructType>(p->getElementType());
        if (!s) {
            // t->dump();
            // printf("Not a pointer\n");
            return;
        }

        // Take care of inheritance.  It's represented as an instance of the base type at the beginning of the
        // derived type, not as the types concatenated.
        while (s->elements().size() > 0 && llvm::isa<llvm::StructType>(s->elements()[0]))
            s = llvm::cast<llvm::StructType>(s->elements()[0]);

        if (isa<ConstantPointerNull>(v))
            return;

        bool ok_type = false;
        if (s->elements().size() >= 2 + REFCOUNT_IDX && s->elements()[REFCOUNT_IDX] == g.i64
            && s->elements()[REFCOUNT_IDX + 1] == g.llvm_class_type_ptr) {
            // printf("This looks likes a class\n");
            ok_type = true;
        }

        if (!ok_type) {
#ifndef NDEBUG
            if (s->getName().startswith("struct.pyston::Box")
                || (s->getName().startswith("Py") && s->getName().endswith("Object"))
                || s->getName().startswith("class.pyston::Box")) {
                v->dump();
                s->dump();
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
            abort();
        }
    };

    for (auto&& g : f->getParent()->getGlobalList()) {
        // g.dump();
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

    struct RefOp {
        llvm::TrackingVH<llvm::Value> operand;
        bool nullable;
        int num_refs;

        // Exactly one of these should be non-NULL:
        llvm::Instruction* insertion_inst;
        llvm::BasicBlock* insertion_bb;
        llvm::BasicBlock* insertion_from_bb;
    };

    struct RefState {
        bool been_run = false;

        // We do a backwards scan and starting/ending here refers to the scan, not the instruction sequence.
        // So "starting_refs" are the refs that are inherited, ie the refstate at the end of the basic block.
        // "ending_refs" are the refs we calculated, which corresponds to the refstate at the beginning of the block.
        BlockMap starting_refs;
        BlockMap ending_refs;

        llvm::SmallVector<RefOp, 4> increfs;
        llvm::SmallVector<RefOp, 4> decrefs;

        struct CXXFixup {
            llvm::Instruction* inst;
            llvm::SmallVector<llvm::TrackingVH<llvm::Value>, 4> to_decref;
        };
        llvm::SmallVector<CXXFixup, 4> cxx_fixups;
    };

    std::vector<RefState> states(num_bb);

    BlockOrderer orderer(computeTraversalOrder(bbg));
    for (int i = 0; i < num_bb; i++) {
        orderer.add(i);
    }

    std::vector<llvm::InvokeInst*> invokes;
    std::vector<llvm::CallInst*> yields;
    for (auto&& II : llvm::inst_range(f)) {
        llvm::Instruction* inst = &II;

        // is this a yield?
        if (llvm::isa<llvm::CallInst>(inst)) {
            llvm::CallInst* call = llvm::cast<llvm::CallInst>(inst);
            if (call->getCalledValue() == g.funcs.yield_capi)
                yields.push_back(call);
        }

        // invoke specific code
        if (!rt->vars.count(inst))
            continue;
        if (auto ii = dyn_cast<InvokeInst>(inst))
            invokes.push_back(ii);
    }

    while (true) {
        int idx = orderer.pop();
        if (idx == -1)
            break;

        auto bb = bbg.bbs[idx];

// errs() << "DETERMINISM: Processing " << bb->getName() << '\n';
#if 0
        llvm::Instruction* term_inst = BB.getTerminator();
        llvm::Instruction* insert_before = term_inst;
        if (llvm::isa<llvm::UnreachableInst>(insert_before)) {
            insert_before = &*(++BB.rbegin());
            assert(llvm::isa<llvm::CallInst>(insert_before) || llvm::isa<llvm::IntrinsicInst>(insert_before));
        }
#endif

        if (VERBOSITY() >= 2) {
            llvm::outs() << '\n';
            llvm::outs() << "Processing " << bbg.bbs[idx]->getName() << '\n';
        }

        RefState& state = states[idx];
        bool firsttime = !state.been_run;
        state.been_run = true;

        BlockMap orig_ending_refs = std::move(state.ending_refs);

        state.starting_refs.clear();
        state.ending_refs.clear();
        state.increfs.clear();
        state.decrefs.clear();
        state.cxx_fixups.clear();

        // Compute the incoming refstate based on the refstate of any successor nodes
        llvm::SmallVector<int, 4> successors;
        for (auto sidx : bbg.successors[idx]) {
            if (states[sidx].been_run)
                successors.push_back(sidx);
        }
        if (successors.size()) {
            std::vector<llvm::Value*> tracked_values;
            llvm::DenseSet<llvm::Value*> in_tracked_values;
            for (auto sidx : successors) {
                // errs() << "DETERMINISM: successor " << SBB->getName() << '\n';
                assert(states[sidx].been_run);
                for (auto&& p : states[sidx].ending_refs) {
                    // errs() << "DETERMINISM: looking at ref " << p.first->getName() << '\n';
                    assert(p.second > 0);
                    if (!in_tracked_values.count(p.first)) {
                        in_tracked_values.insert(p.first);
                        tracked_values.push_back(p.first);
                    }
                }
            }

            llvm::SmallVector<std::pair<BlockMap*, int>, 4> successor_info;
            for (auto sidx : successors) {
                successor_info.emplace_back(&states[sidx].ending_refs, sidx);
            }

            // size_t hash = 0;
            for (auto v : tracked_values) {
                // hash = hash * 31 + std::hash<llvm::StringRef>()(v->getName());
                assert(rt->vars.count(v));
                const auto refstate = rt->vars.lookup(v);

                int min_refs = 1000000000;
                for (auto&& s_info : successor_info) {
                    if (s_info.first->count(v)) {
                        // llvm::outs() << "Going from " << BB.getName() << " to " << SBB->getName() << ", have "
                        //<< it->second << " refs on " << *v << '\n';
                        min_refs = std::min((*s_info.first)[v], min_refs);
                    } else {
                        // llvm::outs() << "Going from " << BB.getName() << " to " << SBB->getName()
                        //<< ", have 0 (missing) refs on " << *v << '\n';
                        min_refs = 0;
                    }
                }

                if (refstate.reftype == RefType::OWNED)
                    min_refs = std::max(1, min_refs);

                for (auto&& s_info : successor_info) {
                    int this_refs = 0;
                    if (s_info.first->count(v))
                        this_refs = (*s_info.first)[v];

                    if (this_refs > min_refs) {
                        // llvm::outs() << "Going from " << BB.getName() << " to " << SBB->getName() << ", need to add "
                        //<< (this_refs - min_refs) << " refs to " << *v << '\n';
                        state.increfs.push_back(RefOp({ v, refstate.nullable, this_refs - min_refs, NULL,
                                                        bbg.bbs[s_info.second], bbg.bbs[idx] }));
                    } else if (this_refs < min_refs) {
                        assert(refstate.reftype == RefType::OWNED);
                        state.decrefs.push_back(RefOp({ v, refstate.nullable, min_refs - this_refs, NULL,
                                                        bbg.bbs[s_info.second], bbg.bbs[idx] }));
                    }
                }

                if (min_refs)
                    state.starting_refs[v] = min_refs;
                else
                    assert(state.starting_refs.count(v) == 0);
            }
            // errs() << "DETERMINISM: tracked value name hash: " << hash << '\n';
        }

        state.ending_refs = state.starting_refs;

        // Then, iterate backwards through the instructions in this BB, updating the ref states
        for (auto& I : llvm::iterator_range<llvm::BasicBlock::reverse_iterator>(bb->rbegin(), bb->rend())) {
            // Phis get special handling:
            // - we only use one of the operands to the phi node (based on the block we came from)
            // - the phi-node-generator is supposed to handle that by putting a refConsumed on the terminator of the
            // previous block
            // - that refConsumed will caus a use as well.
            auto inst = &I;

            if (!isa<InvokeInst>(inst) && rt->vars.count(&I)) {
                const auto&& rstate = rt->vars.lookup(inst);
                int starting_refs = (rstate.reftype == RefType::OWNED ? 1 : 0);
                if (state.ending_refs[inst] != starting_refs) {
                    llvm::Instruction* insertion_pt = NULL;
                    llvm::BasicBlock* insertion_block = NULL, * insertion_from_block = NULL;
                    assert(inst != inst->getParent()->getTerminator());
                    insertion_pt = inst->getNextNode();
                    while (llvm::isa<llvm::PHINode>(insertion_pt)) {
                        insertion_pt = insertion_pt->getNextNode();
                    }

                    if (state.ending_refs[inst] < starting_refs) {
                        assert(rstate.reftype == RefType::OWNED);
                        state.decrefs.push_back(RefOp({ inst, rstate.nullable, starting_refs - state.ending_refs[inst],
                                                        insertion_pt, insertion_block, insertion_from_block }));
                    } else {
                        state.increfs.push_back(RefOp({ inst, rstate.nullable, state.ending_refs[inst] - starting_refs,
                                                        insertion_pt, insertion_block, insertion_from_block }));
                    }
                }
                state.ending_refs.erase(inst);
            }

            if (llvm::isa<llvm::PHINode>(&I))
                continue;

            // If we are about to insert a CXX fixup, do the increfs after the call, rather than trying to push
            // them before the call and having to insert decrefs on the fixup path.
            if (rt->may_throw.count(&I)) {
                llvm::SmallVector<llvm::Value*, 4> to_erase;
                for (auto&& p : state.ending_refs) {
                    int needed_refs = (rt->vars.lookup(p.first).reftype == RefType::OWNED ? 1 : 0);
                    if (p.second > needed_refs) {
                        state.increfs.push_back(RefOp({ p.first, rt->vars.lookup(p.first).nullable,
                                                        p.second - needed_refs, I.getNextNode(), NULL, NULL }));
                    }

                    state.ending_refs[p.first] = needed_refs;
                    if (needed_refs == 0)
                        to_erase.push_back(p.first);
                }

                for (auto v : to_erase)
                    state.ending_refs.erase(v);
            }

            SmallOrderedMap<llvm::Value*, int> num_consumed_by_inst;
            SmallOrderedMap<llvm::Value*, int> num_times_as_op;

            {
                auto it = rt->refs_consumed.find(&I);
                if (it != rt->refs_consumed.end()) {
                    for (auto v : it->second) {
                        num_consumed_by_inst[v]++;
                        assert(rt->vars.count(v) && rt->vars.lookup(v).reftype != RefType::UNKNOWN);
                        num_times_as_op[v]; // just make sure it appears in there
                    }
                }
            }

            {
                auto it = rt->refs_used.find(&I);
                if (it != rt->refs_used.end()) {
                    for (auto v : it->second) {
                        assert(rt->vars.lookup(v).reftype != RefType::UNKNOWN);
                        num_times_as_op[v]++;
                    }
                }
            }

            for (llvm::Value* op : I.operands()) {
                auto it = rt->vars.find(op);
                if (it == rt->vars.end())
                    continue;

                num_times_as_op[op]++;
            }

            // First, calculate anything we need to keep alive through the end of the function call:
            for (auto&& p : num_times_as_op) {
                auto& op = p.first;

                int num_consumed = num_consumed_by_inst.get(op);

                if (p.second > num_consumed) {
                    if (rt->vars.lookup(op).reftype == RefType::OWNED) {
                        if (state.ending_refs[op] == 0) {
                            // llvm::outs() << "Last use of " << *op << " is at " << I << "; adding a decref after\n";

                            if (llvm::InvokeInst* invoke = llvm::dyn_cast<llvm::InvokeInst>(&I)) {
                                state.decrefs.push_back(
                                    RefOp({ op, rt->vars.lookup(op).nullable, 1, NULL, invoke->getNormalDest(), bb }));
                                state.decrefs.push_back(
                                    RefOp({ op, rt->vars.lookup(op).nullable, 1, NULL, invoke->getUnwindDest(), bb }));
                            } else {
                                assert(&I != I.getParent()->getTerminator());
                                auto next = I.getNextNode();
                                // while (llvm::isa<llvm::PHINode>(next))
                                // next = next->getNextNode();
                                if (llvm::isa<llvm::UnreachableInst>(next)) {
                                    // ASSERT(!llvm::isa<llvm::UnreachableInst>(next),
                                    //"Can't add decrefs after this function...");
                                    assert(rt->may_throw.count(&I));
                                } else {
                                    state.decrefs.push_back(
                                        RefOp({ op, rt->vars.lookup(op).nullable, 1, next, NULL, NULL }));
                                }
                            }
                            state.ending_refs[op] = 1;
                        }
                    }
                }
            }

            if (rt->may_throw.count(&I)) {
                // TODO: pump out any increfs rather than pushing them before this.

                state.cxx_fixups.emplace_back();
                auto&& fixup = state.cxx_fixups.back();
                fixup.inst = &I;
                for (auto&& p : state.ending_refs) {
                    for (int i = 0; i < p.second; i++) {
                        assert(rt->vars.count(p.first));
                        fixup.to_decref.push_back(p.first);
                    }
                }

                if (fixup.to_decref.empty())
                    state.cxx_fixups.pop_back();
            }

            // Lastly, take care of any stolen refs.  This happens regardless of whether an exception gets thrown,
            // so it goes after that handling (since we are processing in reverse).
            for (auto&& p : num_times_as_op) {
                auto& op = p.first;

                int num_consumed = num_consumed_by_inst.get(op);

                if (num_consumed)
                    state.ending_refs[op] += num_consumed;
            }
        }

        if (VERBOSITY() >= 2) {
            llvm::outs() << "End of " << bb->getName() << '\n';
            if (VERBOSITY() >= 3) {
                for (auto&& p : state.ending_refs) {
                    llvm::outs() << *p.first << ": " << p.second << '\n';
                }
            }
        }


        // Invokes are special.  Handle them here by treating them as if they happened in their normal-dest block.
        for (InvokeInst* ii : invokes) {
            const auto&& rstate = rt->vars.lookup(ii);

            if (ii->getNormalDest() == bb) {
                // TODO: duplicated with the non-invoke code
                int starting_refs = (rstate.reftype == RefType::OWNED ? 1 : 0);
                if (state.ending_refs[ii] != starting_refs) {
                    llvm::Instruction* insertion_pt = NULL;
                    llvm::BasicBlock* insertion_block = NULL, * insertion_from_block = NULL;

                    insertion_block = bb;
                    insertion_from_block = ii->getParent();

                    if (state.ending_refs[ii] < starting_refs) {
                        assert(rstate.reftype == RefType::OWNED);
                        state.decrefs.push_back(RefOp({ ii, rstate.nullable, starting_refs - state.ending_refs[ii],
                                                        insertion_pt, insertion_block, insertion_from_block }));
                    } else {
                        state.increfs.push_back(RefOp({ ii, rstate.nullable, state.ending_refs[ii] - starting_refs,
                                                        insertion_pt, insertion_block, insertion_from_block }));
                    }
                }
                state.ending_refs.erase(ii);
            }
        }

        // If this is the entry block, finish dealing with the ref state rather than handing off to a predecessor
        if (bb == &bb->getParent()->front()) {
            for (auto&& p : state.ending_refs) {
                assert(p.second);

// Anything left should either be an argument, constant or global variable
#ifndef NDEBUG
                if (!llvm::isa<llvm::GlobalVariable>(p.first) && !llvm::isa<llvm::Constant>(p.first)) {
                    bool found = false;
                    for (auto&& arg : f->args()) {
                        if (&arg == p.first) {
                            found = true;
                            break;
                        }
                    }
                    if (!found)
                        llvm::outs() << "Couldn't find " << *p.first << '\n';
                    assert(found);
                }
#endif
                assert(rt->vars.lookup(p.first).reftype == RefType::BORROWED);

                state.increfs.push_back(
                    RefOp({ p.first, rt->vars.lookup(p.first).nullable, p.second, NULL, bb, NULL }));
            }
            state.ending_refs.clear();
        }

        // It is possible that we ended with zero live variables, which due to our skipping of un-run blocks,
        // is not the same thing as an un-run block.  Hence the check of 'firsttime'
        if (firsttime || endingRefsDifferent(orig_ending_refs, state.ending_refs)) {
            for (auto sidx : bbg.predecessors[idx]) {
                // llvm::outs() << "reconsidering: " << SBB->getName() << '\n';
                orderer.add(sidx);
            }
        }

        // for (auto&& p : state.ending_refs) {
        // errs() << "DETERMINISM: ending ref: " << p.first->getName() << '\n';
        //}
    }

    ASSERT(states.size() == f->size(), "We didn't process all nodes...");

    // First, find all insertion points.  This may change the CFG by breaking critical edges.
    InsertionCache insertion_pts;
    for (int idx = 0; idx < num_bb; idx++) {
        auto&& state = states[idx];
        for (auto& op : state.increfs) {
            auto insertion_pt = op.insertion_inst;
            if (!insertion_pt)
                insertion_pt = findInsertionPoint(op.insertion_bb, op.insertion_from_bb, insertion_pts);
        }
        for (auto& op : state.decrefs) {
            auto insertion_pt = op.insertion_inst;
            if (!insertion_pt)
                insertion_pt = findInsertionPoint(op.insertion_bb, op.insertion_from_bb, insertion_pts);
        }
    }

    // Then use the insertion points (it's the same code but this time it will hit the cache).
    // This may change the CFG by adding decref's
    for (int idx = 0; idx < num_bb; idx++) {
        auto&& state = states[idx];
        for (auto& op : state.increfs) {
            assert(rt->vars.count(op.operand));
            auto insertion_pt = op.insertion_inst;
            if (!insertion_pt)
                insertion_pt = findInsertionPoint(op.insertion_bb, op.insertion_from_bb, insertion_pts);
            addIncrefs(op.operand, op.nullable, op.num_refs, insertion_pt);
        }
        for (auto& op : state.decrefs) {
            assert(rt->vars.count(op.operand));
            auto insertion_pt = op.insertion_inst;
            if (!insertion_pt)
                insertion_pt = findInsertionPoint(op.insertion_bb, op.insertion_from_bb, insertion_pts);
            addDecrefs(op.operand, op.nullable, op.num_refs, insertion_pt);
        }

        for (auto&& fixup : state.cxx_fixups) {
            addCXXFixup(fixup.inst, fixup.to_decref, rt);
        }
    }

    // yields need to get handled specially
    // we pass all object which we own at the point of the yield call to the yield so that we can traverse them in
    // tp_traverse.
    // we have to create a new call instruction because we can't add arguments to an existing call instruction
    for (auto&& old_yield : yields) {
        auto&& state = states[bbg.bb_idx[old_yield->getParent()]];
        assert(old_yield->getNumArgOperands() == 3);
        llvm::Value* yield_value = old_yield->getArgOperand(1);

        llvm::SmallVector<llvm::Value*, 8> args;
        args.push_back(old_yield->getArgOperand(0)); // generator
        args.push_back(yield_value);                 // value
        args.push_back(NULL); // num live values. we replace it with the actual number of varargs after inserting them
        // we can just traverse state.ending_refs because when generating the yield we make sure that it's at the start
        // of the BB.
        for (auto ref : state.ending_refs) {
            if (rt->vars.lookup(ref.first).reftype == RefType::OWNED) {
                if (yield_value != ref.first) // ignore this value because yield steals it!
                    args.push_back(ref.first);
            }
        }
        int num_live_values = args.size() - 3;
        if (num_live_values == 0)
            continue; // nothing to do

        args[2] = getConstantInt(num_live_values, g.i32); // replace the dummy value the actual amount

        llvm::CallInst* new_yield = llvm::CallInst::Create(g.funcs.yield_capi, args, llvm::Twine(), old_yield);
        old_yield->replaceAllUsesWith(new_yield);
        old_yield->eraseFromParent();
    }

    long us = _t.end();
    static StatCounter us_refcounting("us_compiling_irgen_refcounting");
    us_refcounting.log(us);
}

} // namespace pyston
