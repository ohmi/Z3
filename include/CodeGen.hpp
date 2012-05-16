#ifndef CODEGEN_HPP
#define CODEGEN_HPP

#include <list>
#include <vector>
#include <map>
#include <deque>
#include <sstream>

#include "llvm/Value.h"
#include "llvm/Module.h"
#include "llvm/Support/IRBuilder.h"
#include "llvm/PassManager.h"
#include "llvm/ExecutionEngine/ExecutionEngine.h"
#include "llvm/ExecutionEngine/JIT.h"

#include <Instructions.hpp>

#define MAIN_FUNCTION_ID 0

class GenModule;
class GenFunction;

class CodeGen {
public:
    virtual void Print() = 0;
};

inline void printTab(int nbTabs) {
    for (int i = 0; i < nbTabs; i++)
        std::cout << "  ";
}

// ================ GenBlock Declaration ================== //

struct ClosureInfo {
    llvm::Function* LlvmFunc;
    bool IsBare;
};

struct StackValue {
    llvm::Value* Val;
    StackValue(llvm::Value* Val) {this->Val = Val;}
};

class GenBlock : public CodeGen {
    friend class GenModuleCreator;
    friend class GenFunction;

private:
    int Id;
    GenFunction* Function;
    llvm::IRBuilder<> * Builder;

    // Siblings blocks handling
    std::list<GenBlock*> PreviousBlocks;
    std::list<GenBlock*> NextBlocks;
    GenBlock* BrBlock;
    GenBlock* NoBrBlock;

    void makeCheckedCall(llvm::Value* callee, llvm::ArrayRef<llvm::Value*> Args);


    // List of PhiNodes to fill at the end of function codegen
    std::list<std::pair<llvm::PHINode*, int>> PHINodes;
    void genTermInst();

    // Instructions to generate
    std::vector<ZInstruction*> Instructions;

    // Stack handling
    std::deque<StackValue*> Stack;
    std::map<int, StackValue*> PrevStackCache;
    llvm::Value* Accu;
    llvm::Value* ExtraArgs;
    llvm::Value* getAccu(bool CreatePhi=true);
    llvm::Value* Sp;

    // Llvm block handling
    std::pair<llvm::BasicBlock*, llvm::BasicBlock*> addBlock();
    llvm::BasicBlock* LlvmBlock;
    std::list<llvm::BasicBlock*> LlvmBlocks;

    // This pointer is set only if the block last instruction 
    // is a conditional branch. It is equal to the llvm Val 
    // containing the result of the test
    llvm::Value* CondVal;

    std::map<StackValue*, StackValue*> MutatedVals;

    llvm::Value* createArrayFromStack(size_t Size);

public:
    GenBlock(int Id, GenFunction* Function);
    void setNext(GenBlock* Block, bool IsBrBlock);
    void Print(); 
    void PrintAdjBlocks(); 
    void GenCodeForInst(ZInstruction* Inst);
    llvm::BasicBlock* CodeGen();
    std::string name();

    // Codegen helper funcs
    void pushAcc(int n);
    void acc(int n);
    void envAcc(int n);
    void push(bool CreatePhi=true);
    void makeOffsetClosure(int32_t n);
    void makeSetField(size_t n);
    void makeGetField(size_t n);
    llvm::Value* makeCall0(std::string FuncName);
    llvm::Value* makeCall1(std::string FuncName, llvm::Value* arg1);
    llvm::Value* makeCall2(std::string FuncName, llvm::Value* arg1, llvm::Value* arg2);
    llvm::Value* makeCall3(std::string FuncName, llvm::Value* arg1, llvm::Value* arg2, llvm::Value* arg3);
    llvm::Value* makeCall4(std::string FuncName, llvm::Value* arg1, llvm::Value* arg2, llvm::Value* arg3, llvm::Value* arg4);
    llvm::Value* makeCall5(std::string FuncName, llvm::Value* arg1, llvm::Value* arg2, llvm::Value* arg3, llvm::Value* arg4, llvm::Value* arg5);
    void debug(llvm::Value* DbgVal);
    void makeBoolToIntCast();
    void addCallInfo();

    size_t StackOffset;
    llvm::Value* getStackAt(size_t n);

    llvm::Value* intVal(llvm::Value* From);
    llvm::Value* valInt(llvm::Value* From);
    llvm::Value* castToInt(llvm::Value* Val);
    llvm::Value* castToVal(llvm::Value* Val);
    llvm::Value* castToBool(llvm::Value* Val);
    llvm::Value* castToPtr(llvm::Value* Val);

    // Function getters
    llvm::Function* getFunction(std::string FuncName);
    llvm::Value* getPtrToFunc(int32_t FnId);

};

llvm::ConstantInt* ConstInt(uint64_t val);


// ================ GenFunction Declaration ================== //

class GenFunction : public CodeGen {

    friend class GenModuleCreator;
    friend class GenModule;
    friend class GenBlock;

private:
    int Id;
    int Arity;

    std::map<int, GenBlock*> Blocks;
    GenBlock* FirstBlock;
    GenModule* Module;

    // Map closures to llvm Functions to keep track of signatures
    std::map<llvm::Value*, ClosureInfo> ClosuresFunctions;

    std::map<llvm::Value*, llvm::Value*> BoolsAsVals;

    void generateRestartFunction();

public:
    llvm::Function* RestartFunction;
    llvm::Function* LlvmFunc;
    GenFunction(int Id, GenModule* Module);
    std::string name();
    void Print(); 
    void PrintBlocks(); 
    llvm::Function* CodeGen();
    void removeUnusedBlocks();
};


// ================ GenModule Declaration ================== //

class GenModule : public CodeGen {

    friend class GenModuleCreator;
    friend class GenFunction;
    friend class GenBlock;

public:

    GenFunction* MainFunction;
    std::map<int, GenFunction*> Functions;

    llvm::FunctionPassManager* FPM;
    llvm::PassManager* PM;
    llvm::Module *TheModule;
    llvm::IRBuilder<> * Builder;
    llvm::ExecutionEngine* ExecEngine;

    GenModule();
    llvm::Function* getFunction(std::string FuncName);
    void Print(); 
};


// ================ GenModuleCreator Declaration ================== //

class GenModuleCreator {

private:
    std::vector<ZInstruction*>* OriginalInstructions;
    GenModule* Module;

public:

    GenModuleCreator(std::vector<ZInstruction*>* Instructions) { 
        this->OriginalInstructions = Instructions; 
        Module = new GenModule();
    }

    GenModule* generate(int FirstInst=0, int LastInst=0);
    std::deque<ZInstruction*>* initFunction(std::deque<ZInstruction*>* Instructions);
    void generateFunction(GenFunction* Function, std::deque<ZInstruction*>* Instructions);
};

inline llvm::Type* getValType();
llvm::Type* getBoolType(); 

#endif // CODEGEN_HPP
