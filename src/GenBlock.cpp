#include <CodeGen.hpp>
#include <Utils.hpp>

#include "llvm/DerivedTypes.h"
#include "llvm/LLVMContext.h"
#include "llvm/Analysis/Verifier.h"
#include "llvm/Target/TargetData.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Support/IRReader.h"

extern "C" {
    #include <ocaml_runtime/mlvalues.h>
    #include <ocaml_runtime/major_gc.h>
    #include <ocaml_runtime/memory.h>
}

#include <stdexcept>
#include <cstdio>

/* GC interface 
 * Those macros are empty for the moment 
 * But need to be defined if we want to use Alloc_small
 */

#define Setup_for_gc \
  {}
#define Restore_after_gc \
  {}
#define Setup_for_c_call \
  {}
#define Restore_after_c_call \
  {}

using namespace std;
using namespace llvm;

// ============================ HELPERS ============================== //

Type* getValType() {
    return Type::getIntNTy(getGlobalContext(), sizeof(value) * 8);
}

// ================ GenBlock Implementation ================== //

GenBlock::GenBlock(int Id, GenFunction* Function) {
    // Basic init
    this->Id = Id;
    this->Function = Function;
    this->Builder = Function->Module->Builder;
    this->Accu = nullptr;

    addBlock();
}

pair<BasicBlock*, BasicBlock*> GenBlock::addBlock() {
    auto OldBlock = LlvmBlock;
    LlvmBlock = BasicBlock::Create(getGlobalContext(), name());
    LlvmBlocks.push_back(LlvmBlock);
    return make_pair(OldBlock, LlvmBlock);
}

void GenBlock::setNext(GenBlock* Block, bool IsBrBlock) {
    NextBlocks.push_back(Block);
    Block->PreviousBlocks.push_front(this);
    if (IsBrBlock) BrBlock = Block;
    else NoBrBlock = Block;
}

std::string GenBlock::name() {
    stringstream ss;
    ss << "Block_" << Id;
    return ss.str();
}

Value* GenBlock::getStackAt(size_t n, GenBlock* IgnorePrevBlock) {

    std::list<GenBlock*> PrBlocks = PreviousBlocks;
    if (IgnorePrevBlock != nullptr) PrBlocks.remove(IgnorePrevBlock);

    Value* Ret = nullptr;

    if (n >= Stack.size()) {
        auto NbPrevBlocks = PrBlocks.size();
        int PrevStackPos = n - Stack.size();

        auto It = PrevStackCache.find(PrevStackPos);
        if (It != PrevStackCache.end()) {
            Ret = It->second;
        } else {

            if (NbPrevBlocks == 0) {
                throw std::logic_error("Bad stack access !");
            } else if (NbPrevBlocks == 1) {
                Ret = PrBlocks.front()->getStackAt(PrevStackPos);
            } else {
                auto B = Builder->GetInsertBlock();
                Builder->SetInsertPoint(LlvmBlock);
                PHINode* PHI = Builder->CreatePHI(getValType(), NbPrevBlocks, "phi");
                PHINodes.push_back(make_pair(PHI, PrevStackPos));
                Builder->SetInsertPoint(B);
                Ret = PHI;
            }
            PrevStackCache[PrevStackPos] = Ret;
        }

    } else {
        Ret = Stack[n];
    }

    Ret = getMutatedValue(Ret);

    return Ret;
}

Value* GenBlock::getMutatedValue(Value* Val) {
    auto It = MutatedVals.find(Val);
    if (It != MutatedVals.end())
        return getMutatedValue(It->second);

    return Val;
}

void GenBlock::handlePHINodes() {
    for (auto Pair : PHINodes) 
        for (auto Block : PreviousBlocks) {
            if (Pair.second == -1)
                Pair.first->addIncoming(Block->Accu, Block->LlvmBlocks.front());
            else
                // We take the corresponding value in the stack of the previous block
                // And link it in the phi to the last llvm block of the previous block
                // Because we are necessarily coming from there if we come from this block
                Pair.first->addIncoming(Block->getStackAt(Pair.second, this), Block->LlvmBlocks.back());
        }

    auto& IList = LlvmBlock->getInstList();
    deque<decltype(IList.begin())> Phis;
    for (auto It=IList.begin(); It != IList.end(); ++It) {
        if (isa<PHINode>(*It)) {
            Phis.push_back(It);
        }
    }
    for (auto Phi : Phis) IList.remove(Phi);
    for (auto Phi : Phis) IList.push_front(Phi);
}

void GenBlock::dumpStack() {
    cout << "============== Stack For Block " << name() << " ==================" << endl;
    for (auto Val : Stack) printf("%p\n", Val);
    for (auto Val : Stack) Val->dump();
    cout << "==========================================================" << endl;
}

Value* GenBlock::stackPop() {
    auto Val = getStackAt(0);
    // TODO: Consider previous blocks stacks
    Stack.pop_front();
    return Val;
}

void GenBlock::push(bool CreatePhi) { 
    if (Accu == nullptr && CreatePhi) {
        auto PHI = Builder->CreatePHI(getValType(), PreviousBlocks.size());
        PHINodes.push_back(make_pair(PHI, -1));
        Accu = PHI;
    }
    this->Stack.push_front(Accu); 
}
void GenBlock::acc(int n) { 
    this->Accu = this->getStackAt(n); 
}

void GenBlock::envAcc(int n) { 
    auto Env = Builder->CreateCall(getFunction("getEnv"), "Env");
    Accu = Builder->CreateCall2(getFunction("getField"), 
                               Env,
                               ConstInt(n), "Field");
}

void GenBlock::pushAcc(int n) { push(); acc(n); }

BasicBlock* GenBlock::CodeGen() {
    // Create the block and generate instruction's code
    Builder->SetInsertPoint(LlvmBlock);

    DEBUG(debug(ConstInt(this->Id));)

    for (auto Inst : this->Instructions)
        GenCodeForInst(Inst);

    return LlvmBlock;
}

void GenBlock::genTermInst() {

    Builder->SetInsertPoint(LlvmBlock);
    auto Inst = Instructions.back();

    if (!(Inst->isJumpInst() || Inst->isReturn()))
        Builder->CreateBr(this->NextBlocks.front()->LlvmBlock);
}

Value* GenBlock::castToInt(Value* Val) {
    if (Val->getType() != getValType())
        return Builder->CreatePtrToInt(Val, getValType());
    else
        return Val;
}

Value* GenBlock::castToPtr(Value* Val) {
    if (Val->getType() != getValType())
        return Builder->CreateIntToPtr(Val, getValType()->getPointerTo());
    else
        return Val;
}

Value* GenBlock::intVal(Value* From) {
    return Builder->CreateLShr(From, 1);
}

Value* GenBlock::valInt(Value* From) {
    return Builder->CreateAdd(Builder->CreateShl(From, 1), ConstInt(1));
}

Value* ConstInt(uint64_t val) {
    return ConstantInt::get(
        getGlobalContext(), 
        APInt(sizeof(value)*8, val, /*signed=*/true)
    );
}

void GenBlock::makeCheckedCall(Value* Callee, ArrayRef<Value*> Args) {
    /*
    if (UnwindBlocks.size() > 0) {
        auto Blocks = addBlock();
        Accu = Builder->CreateInvoke(Callee, Blocks.second, UnwindBlocks.front(), Args);
        Builder->SetInsertPoint(Blocks.second);
    } else {
    */
        Accu = Builder->CreateCall(Callee, Args);
    //}
}

void GenBlock::makeApply(size_t n) {

    ClosureInfo CI = Function->ClosuresFunctions[Accu];
    vector<Value*> ArgsV;

    if (CI.IsBare && CI.LlvmFunc->arg_size() == n) {

        for (size_t i = 1; i <= n; i++)
            ArgsV.push_back(getStackAt(n-i));
        makeCheckedCall(CI.LlvmFunc, ArgsV);

    } else {

        auto Array = Builder->CreateAlloca(ArrayType::get(getValType(), n));
        for (size_t i = 1; i <= n; i++) {
            vector<Value*> GEPlist; 
            GEPlist.push_back(ConstInt(0));
            GEPlist.push_back(ConstInt(i-1));
            auto Ptr = Builder->CreateGEP(Array, GEPlist);
            auto Val = getStackAt(n-i);
            Builder->CreateStore(Val, Ptr);
        }

        auto ArrayPtr = Builder->CreatePointerCast(Array, getValType()->getPointerTo());
        Accu->setName("ApplyClosure");
        ArgsV.push_back(Accu);
        ArgsV.push_back(ConstInt(n));
        ArgsV.push_back(ArrayPtr);
        makeCheckedCall(getFunction("apply"), ArgsV);
        Accu->setName("ApplyRes");

    }

    for (size_t i = 0; i < n; i++)
        stackPop();
}

void GenBlock::makePrimCall(size_t n, int32_t NumPrim) {
    vector<Value*> Args;
    stringstream ss;
    ss << "primCall";

    Args.push_back(ConstInt(NumPrim));
    if (n < 6) {
        Args.push_back(Accu);
        for (size_t i = 1; i < n; i++)
            Args.push_back(stackPop());
        ss << n;
    } else {
        Args.push_back(ConstInt(n));
        ss << "n";
    }
    makeCheckedCall(getFunction(ss.str()), Args);
    Accu->setName("PrimCallRes");
}

void GenBlock::makeClosure(int32_t NbFields, int32_t FnId) {
    auto MakeClos = getFunction("makeClosure");
    auto ClosSetVar = getFunction("closureSetVar");

    // Create pointer to dest function
    auto DestGenFunc = Function->Module->Functions[FnId];
    if (DestGenFunc->LlvmFunc == NULL) {
        DestGenFunc->CodeGen();
    }
    Builder->SetInsertPoint(LlvmBlock);
    auto ClosureFunc = DestGenFunc->ApplierFunction;
    auto CastPtr = Builder->CreatePtrToInt(ClosureFunc, getValType());


    int FuncNbArgs = DestGenFunc->LlvmFunc->arg_size();

    auto Closure = Builder->CreateCall3(MakeClos, 
                                        ConstInt(NbFields), 
                                        CastPtr, 
                                        ConstInt(FuncNbArgs));

    // If there are fields, push the Accu on the stack
    if (NbFields > 0) push();
    // Set Closure fields
    for (int i = 0; i < NbFields; i++) {
        auto FieldVal = stackPop();
        Builder->CreateCall3(ClosSetVar, Closure, ConstInt(i), FieldVal);
    }

    Accu = Closure;
    Accu->setName("Closure");

    ClosureInfo CI = {DestGenFunc->LlvmFunc, NbFields == 0 ? true : false};
    this->Function->ClosuresFunctions[Accu] = CI;
}

void GenBlock::makeSetField(size_t n) {
    Builder->CreateCall3(getFunction("setField"), Accu, ConstInt(n), stackPop());
}

void GenBlock::makeGetField(size_t n) {
    Accu = Builder->CreateCall2(getFunction("getField"), Accu, ConstInt(n));
}

void GenBlock::debug(Value* DbgVal) {
    if (DbgVal->getType() != getValType()) {
        DbgVal = Builder->CreateBitCast(DbgVal, getValType());
    }
    Builder->CreateCall(getFunction("debug"), DbgVal);
}

void GenBlock::GenCodeForInst(ZInstruction* Inst) {

    Value *TmpVal;

    DEBUG(
        cout << "Generating Instruction "; Inst->Print(true);
        printTab(2);
        printf("Accu pointer before: {%p}\n", Accu);
        if (Accu) Accu->dump();
    )

    switch (Inst->OpNum) {

        case CONST0: this->Accu = ConstInt(Val_int(0)); break;
        case CONST1: this->Accu = ConstInt(Val_int(1)); break;
        case CONST2: this->Accu = ConstInt(Val_int(2)); break;
        case CONST3: this->Accu = ConstInt(Val_int(3)); break;
        case CONSTINT:
            this->Accu = ConstInt(Val_int(Inst->Args[0]));
            break;

        case PUSHCONST0: push(); this->Accu = ConstInt(Val_int(0)); break;
        case PUSHCONST1: push(); this->Accu = ConstInt(Val_int(1)); break;
        case PUSHCONST2: push(); this->Accu = ConstInt(Val_int(2)); break;
        case PUSHCONST3: push(); this->Accu = ConstInt(Val_int(3)); break;
        case PUSHCONSTINT: 
            push(); 
            this->Accu = ConstInt(Val_int(Inst->Args[0])); 
            break;

        case POP: 
            for (int i = 0; i < Inst->Args[0]; i++) stackPop(); 
            break;
        case PUSH: push(); break;
        case PUSH_RETADDR:
            push(); push(); push();
            break;

        case PUSHTRAP: {
            auto Buf = Builder->CreateCall(getFunction("getNewBuffer"));
            auto SetJmpFunc = getFunction("_setjmp");
            auto JmpBufType = Function->Module->TheModule->getTypeByName("struct.__jmp_buf_tag")->getPointerTo();
            auto JmpBuf = Builder->CreateBitCast(Buf, JmpBufType);
            auto SetJmpRes = Builder->CreateCall(SetJmpFunc, JmpBuf);
            auto BoolVal = Builder->CreateIntCast(SetJmpRes, Type::getInt1Ty(getGlobalContext()), getValType());
            auto Blocks = addBlock();
            auto TrapBlock = Function->Blocks[Inst->Args[0]];
            Builder->CreateCondBr(BoolVal, TrapBlock->LlvmBlocks.front(), Blocks.second);

            Builder->SetInsertPoint(TrapBlock->LlvmBlock);
            TrapBlock->Accu = Builder->CreateCall(getFunction("getExceptionValue"));
            Builder->CreateCall(getFunction("removeExceptionContext"));

            Builder->SetInsertPoint(Blocks.second);
            for (int i=0;i<4;i++) push(false);
            break;

        }
        case POPTRAP: {
            //UnwindBlocks.pop_front();
            Builder->CreateCall(getFunction("removeExceptionContext"));
            stackPop();stackPop();stackPop();stackPop();
            break;
        }
        case RAISE: {
            Builder->CreateCall(getFunction("throwException"), Accu);
            Builder->CreateRet(Accu);
            break;
        }


        case ACC0: acc(0); break;
        case ACC1: acc(1); break;
        case ACC2: acc(2); break;
        case ACC3: acc(3); break;
        case ACC4: acc(4); break;
        case ACC5: acc(5); break;
        case ACC6: acc(6); break;
        case ACC7: acc(7); break;
        case ACC: acc(Inst->Args[0]); break;

        case PUSHACC0: pushAcc(0); break;
        case PUSHACC1: pushAcc(1); break;
        case PUSHACC2: pushAcc(2); break;
        case PUSHACC3: pushAcc(3); break;
        case PUSHACC4: pushAcc(4); break;
        case PUSHACC5: pushAcc(5); break;
        case PUSHACC6: pushAcc(6); break;
        case PUSHACC7: pushAcc(7); break;
        case PUSHACC:  pushAcc(Inst->Args[0]); break;


        case ENVACC1: envAcc(1); break;
        case ENVACC2: envAcc(2); break;
        case ENVACC3: envAcc(3); break;
        case ENVACC4: envAcc(4); break;
        case ENVACC:  envAcc(Inst->Args[0]); break;

        case PUSHENVACC1: push(); envAcc(1); break;
        case PUSHENVACC2: push(); envAcc(2); break;
        case PUSHENVACC3: push(); envAcc(3); break;
        case PUSHENVACC4: push(); envAcc(4); break;
        case PUSHENVACC:  push(); envAcc(Inst->Args[0]); break;

        case ADDINT:
            TmpVal = castToInt(stackPop());
            Accu = Builder->CreateAdd(castToInt(Accu), Builder->CreateSub(TmpVal, ConstInt(1)));
            break;
        case NEGINT:
            Accu = Builder->CreateSub(ConstInt(2), Accu);
            break;
        case SUBINT:
            TmpVal = stackPop();
            Accu = Builder->CreateSub(Accu, Builder->CreateAdd(TmpVal, ConstInt(1)));
            break;
        case MULINT:
            TmpVal = stackPop();
            TmpVal = Builder->CreateMul(intVal(Accu), intVal(TmpVal));
            Accu = valInt(TmpVal);
            break;
        case DIVINT:
            TmpVal = stackPop();
            TmpVal = Builder->CreateSDiv(intVal(Accu), intVal(TmpVal));
            Accu = valInt(TmpVal);
            break;
        case MODINT:
            TmpVal = stackPop();
            TmpVal = Builder->CreateSRem(intVal(Accu), intVal(TmpVal));
            Accu = valInt(TmpVal);
            break;
        case OFFSETINT:
            Accu = Builder->CreateAdd(Accu, ConstInt(Inst->Args[0] << 1));
            break;

        case GTINT:
            TmpVal = stackPop();
            Accu = Builder->CreateICmpSGT(Accu, TmpVal);
            break;
        case NEQ:
            TmpVal = stackPop();
            Accu = Builder->CreateICmpNE(Accu, TmpVal);
            break;

        case ASSIGN:
            MutatedVals[getStackAt(Inst->Args[0])] = Accu;
            break;

        case PUSHGETGLOBAL:
            push();
        case GETGLOBAL:
            Accu = Builder->CreateCall(getFunction("getGlobal"), ConstInt(Inst->Args[0]), "Global");
            break;

        case SETGLOBAL:
            Builder->CreateCall2(getFunction("setGlobal"), ConstInt(Inst->Args[0]), Accu);
            break;

        case PUSHATOM0:
            push();
        case ATOM0:
            Accu = Builder->CreateCall(getFunction("getAtom"), ConstInt(0));
            break;

        case PUSHATOM:
            push();
        case ATOM:
            Accu = Builder->CreateCall(getFunction("getAtom"),
                                       ConstInt(Inst->Args[0]));
            break;


        case MAKEBLOCK1:
            Accu = Builder->CreateCall2(getFunction("makeBlock1"), 
                                        ConstInt(Inst->Args[0]), 
                                        Accu, "Block");
            break;
        case MAKEBLOCK2:
            Accu = Builder->CreateCall3(getFunction("makeBlock2"), 
                                        ConstInt(Inst->Args[0]), 
                                        Accu, 
                                        getStackAt(0), "Block");
            stackPop();
            break;
        case MAKEBLOCK3:
            Accu = Builder->CreateCall4(getFunction("makeBlock3"), 
                                        ConstInt(Inst->Args[0]), 
                                        Accu, 
                                        getStackAt(0), 
                                        getStackAt(1), "Block");
            stackPop(); stackPop();
            break;

        case SETFIELD0: makeSetField(0); break;
        case SETFIELD1: makeSetField(1); break;
        case SETFIELD2: makeSetField(2); break;
        case SETFIELD3: makeSetField(3); break;
        case SETFIELD:  makeSetField(Inst->Args[0]); break;

        case GETFIELD0: makeGetField(0); break;
        case GETFIELD1: makeGetField(1); break;
        case GETFIELD2: makeGetField(2); break;
        case GETFIELD3: makeGetField(3); break;
        case GETFIELD:  makeGetField(Inst->Args[0]); break;

        case CLOSUREREC:
            // Simple recursive function with no trampoline and no closure fields
            if (Inst->Args[0] == 1 && Inst->Args[1] == 0) {
                makeClosure(0, Inst->ClosureRecFns[0]);
                push();
            } else {
                // TODO: Handle mutually recursive functions and rec fun with environnements
            }
            break;

        case CLOSURE:
            makeClosure(Inst->Args[0], Inst->Args[1]);
            break;

        case PUSHOFFSETCLOSURE0:
            push();
        case OFFSETCLOSURE0: {
            Accu = Builder->CreateCall(getFunction("getEnv"));
            ClosureInfo CI = {Function->LlvmFunc, true};
            this->Function->ClosuresFunctions[Accu] = CI;
            break;
        }

        case C_CALL1: makePrimCall(1, Inst->Args[0]); break;
        case C_CALL2: makePrimCall(2, Inst->Args[0]); break;
        case C_CALL3: makePrimCall(3, Inst->Args[0]); break;
        case C_CALL4: makePrimCall(4, Inst->Args[0]); break;
        case C_CALL5: makePrimCall(5, Inst->Args[0]); break;

        case APPLY1: makeApply(1); break;
        case APPLY2: makeApply(2); break;
        case APPLY3: makeApply(3); break;
        case APPLY:  makeApply(Inst->Args[0]); break;
        case APPTERM1: makeApply(1); Builder->CreateRet(Accu); break;
        case APPTERM2: makeApply(2); Builder->CreateRet(Accu); break;
        case APPTERM3: makeApply(3); Builder->CreateRet(Accu); break;
        case APPTERM: makeApply(Inst->Args[0]); Builder->CreateRet(Accu); break;

        // Fall through return
        case STOP:
        case RETURN: 
            Builder->CreateRet(Accu); 
            break;
        case BRANCH:{
            BasicBlock* LBrBlock = BrBlock->LlvmBlock;
            Builder->CreateBr(LBrBlock);
            break;
        }
        case BRANCHIF: {
            auto BoolVal = Builder->CreateIntCast(Accu, Type::getInt1Ty(getGlobalContext()), getValType());
            Builder->CreateCondBr(BoolVal, BrBlock->LlvmBlocks.front(), NoBrBlock->LlvmBlocks.front());
            break;
        }
        case BRANCHIFNOT: {
            auto BoolVal = Builder->CreateIntCast(Accu, Type::getInt1Ty(getGlobalContext()), getValType());
            Builder->CreateCondBr(BoolVal, NoBrBlock->LlvmBlocks.front(),BrBlock->LlvmBlocks.front());
            break;
        }

        case EQ:
            Accu = Builder->CreateICmpEQ(Accu, stackPop());
            break;

        case BEQ:
            TmpVal = Builder->CreateICmpEQ(ConstInt(Val_int(Inst->Args[0])), Accu);
            goto makebr;
        case BNEQ:
           TmpVal = Builder->CreateICmpNE(ConstInt(Val_int(Inst->Args[0])), Accu);
           goto makebr;
        case BLTINT:
           TmpVal = Builder->CreateICmpSLT(ConstInt(Val_int(Inst->Args[0])), Accu);
           goto makebr;
        case BLEINT:
           TmpVal = Builder->CreateICmpSLE(ConstInt(Val_int(Inst->Args[0])), Accu);
           goto makebr;
        case BGTINT:
           TmpVal = Builder->CreateICmpSGT(ConstInt(Val_int(Inst->Args[0])), Accu);
           goto makebr;
        case BGEINT:
           TmpVal = Builder->CreateICmpSGE(ConstInt(Val_int(Inst->Args[0])), Accu);
           goto makebr;
        case BULTINT:
           TmpVal = Builder->CreateICmpULT(ConstInt(Val_int(Inst->Args[0])), Accu);
           goto makebr;
        case BUGEINT:
           TmpVal = Builder->CreateICmpUGE(ConstInt(Val_int(Inst->Args[0])), Accu);
           goto makebr;


        makebr: {
            BasicBlock* LBrBlock = BrBlock->LlvmBlocks.front();
            BasicBlock* LNoBrBlock = NoBrBlock->LlvmBlocks.front();
            Builder->CreateCondBr(TmpVal, LBrBlock, LNoBrBlock);
            break;
        }

        case CHECK_SIGNALS:
            // TODO: Understand and implement ocaml's event system
            break;

        default:
            printTab(2);
            cout << "INSTRUCTION NOT HANDLED" << endl;
            exit(42);

    }

    DEBUG(cout << "Instruction generated ===  \n";)
}

void GenBlock::Print() {
    PrintAdjBlocks();
    for (ZInstruction* Inst : Instructions) {
        printTab(2);
        Inst->Print(true);
    }
}

void GenBlock::PrintAdjBlocks() {
    printTab(2);
    cout << "Predecessors : ";

    for (auto Block : PreviousBlocks)
        cout << Block->Id << " ";

    cout << " | Successors : ";

    for (auto Block : NextBlocks)
        cout << Block->Id << " ";

    cout << "\n";
}


