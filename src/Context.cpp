#ifndef CONTEXT_HPP
#define CONTEXT_HPP

#include <string.h>
#include <Utils.hpp>
#include <iostream>
#include <sys/time.h>

extern "C" {
    #include <ocaml_runtime/config.h>
    #include <ocaml_runtime/exec.h>
    #include <ocaml_runtime/startup.h>
    #include <ocaml_runtime/dynlink.h>
    #include <ocaml_runtime/memory.h>
    #include <ocaml_runtime/fix_code.h>
    #include <ocaml_runtime/io.h>
    #include <ocaml_runtime/intext.h>
    #include <ocaml_runtime/gc_ctrl.h>
    #include <ocaml_runtime/stacks.h>
    #include <ocaml_runtime/custom.h>
    #include <ocaml_runtime/misc.h>
    #include <ocaml_runtime/fail.h>
}


#include <Context.hpp>
#include <Instructions.hpp>
#include <CodeGen.hpp>

using namespace std;

static uintnat percent_free_init = Percent_free_def;
static uintnat max_percent_free_init = Max_percent_free_def;
static uintnat minor_heap_init = Minor_heap_def;
static uintnat heap_chunk_init = Heap_chunk_def;
static uintnat heap_size_init = Init_heap_def;
static uintnat max_stack_init = Max_stack_def;

void Context::init(string _FileName, int EraseFrom, int EraseFirst, int EraseLast) {

    char * shared_lib_path, * shared_libs, * req_prims;
    struct exec_trailer Trail;
    struct channel * chan;
    int Fd;

    caml_init_custom_operations();
    caml_ext_table_init(&caml_shared_libs_path, 8);
    caml_external_raise = NULL;

    // Open file
    FileName = _FileName;
    char* CStrFileName = new char[FileName.length() + 1];
    strcpy(CStrFileName, FileName.c_str());
    Fd = caml_attempt_open(&CStrFileName, &Trail, 1);

    // Read section descriptors
    caml_read_section_descriptors(Fd, &Trail);

    /* Initialize the abstract machine */
    caml_init_gc (minor_heap_init, heap_size_init, heap_chunk_init,
                percent_free_init, max_percent_free_init);
    caml_init_stack (max_stack_init);
    init_atoms();

    /* Load the code */
    caml_code_size = caml_seek_section(Fd, &Trail, (char*)"CODE");
    caml_load_code(Fd, caml_code_size);

    /* Build the table of primitives */
    shared_lib_path = read_section(Fd, &Trail, (char*)"DLPT");
    shared_libs = read_section(Fd, &Trail, (char*)"DLLS");
    req_prims = read_section(Fd, &Trail, (char*)"PRIM");
    if (req_prims == NULL) caml_fatal_error((char*)"Fatal error: no PRIM section\n");
    caml_build_primitive_table(shared_lib_path, shared_libs, req_prims);
    caml_stat_free(shared_lib_path);
    caml_stat_free(shared_libs);
    caml_stat_free(req_prims);

    /* Load the globals */
    caml_seek_section(Fd, &Trail, (char*)"DATA");
    chan = caml_open_descriptor_in(Fd);
    caml_global_data = caml_input_val(chan);
    caml_close_channel(chan); /* this also closes Fd */
    caml_stat_free(Trail.section);

    readInstructions(Instructions, caml_start_code, caml_code_size);
    annotateNodes(Instructions);

    if (EraseFirst != EraseLast) {
        std::vector<ZInstruction*>::iterator Beginning, Ending;
        if (EraseFirst > 0)
            Beginning = Instructions.begin() + EraseFirst;
        else
            Beginning = Instructions.end() + EraseFirst;

        if (EraseLast > 0)
            Ending = Instructions.begin() + EraseLast;
        else
            Ending = Instructions.end() + EraseLast;
        
        Instructions.erase(Beginning, Ending);
    }

    Instructions.erase(Instructions.begin(), Instructions.begin() + EraseFrom);

    DEBUG(printInstructions(Instructions, true);)
}


void Context::generateMod() {
    GenModuleCreator GMC(&Instructions);
    Mod = GMC.generate(0);
    DEBUG(Mod->Print();)
}


void Context::compile() {
    auto MainFunc = Mod->MainFunction;
    MainFunc->CodeGen();

    Mod->PM->run(*Mod->TheModule);
    for (auto& F: Mod->TheModule->getFunctionList()) {
        Mod->FPM->run(F);
    }
    Mod->FPM->run(*MainFunc->LlvmFunc);

    DEBUG(
        for (auto FuncP : Mod->Functions)
            FuncP.second->LlvmFunc->dump();
        MainFunc->LlvmFunc->dump();
    )
}

void Context::exec(bool PrintTime) {
    struct timeval Begin, End;
    auto MainFunc = Mod->MainFunction;

    DEBUG(
        for (auto FuncP : Mod->Functions) {
            void *Ptr = Mod->ExecEngine->getPointerToFunction(FuncP.second->LlvmFunc);
            cout << "Function " << FuncP.second->name() << " : " << Ptr << endl;
        }
    )


    void *FPtr = Mod->ExecEngine->getPointerToFunction(MainFunc->LlvmFunc);
    void (*FP)() = (void (*)())(intptr_t)FPtr;

    if (PrintTime) {
        gettimeofday(&Begin, NULL);
    }

    FP();

    if (PrintTime) {
        gettimeofday(&End, NULL);
        double DiffSec = difftime(End.tv_sec, Begin.tv_sec);
        double DiffMicro = difftime(End.tv_usec, Begin.tv_usec)/1000000;
        cout << (DiffSec + DiffMicro) << "s\n"; // in sec
    }

    DEBUG(
        void *p = Mod->ExecEngine->getPointerToFunction(Mod->getFunction("printAccu"));
        void (*fp)() = (void (*)())p;
        fp();
    )
}

#endif
