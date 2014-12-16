// Copyright 2014 Cloudera inc.
// Confidential Cloudera Information: Covered by NDA.

// Requires that the location of the precompiled.ll file is defined
#ifndef KUDU_CODEGEN_MODULE_BUILDER_PRECOMPILED_LL
#error "KUDU_CODEGEN_MODULE_BUILDER_PRECOMPILED_LL should be defined to " \
  "the location of the LLVM IR file for kudu/codegen/precompiled.cc"
#endif

#include "kudu/codegen/module_builder.h"

#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

#include <boost/foreach.hpp>
#include <glog/logging.h>
#include <llvm/LinkAllPasses.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Analysis/Passes.h>
#include <llvm/ExecutionEngine/MCJIT.h>
#include <llvm/ExecutionEngine/ExecutionEngine.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/PassManager.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_os_ostream.h>
#include <llvm/Transforms/IPO.h>
#include <llvm/Transforms/IPO/PassManagerBuilder.h>

#include "kudu/gutil/macros.h"
#include "kudu/gutil/strings/substitute.h"
#include "kudu/util/status.h"

#ifndef CODEGEN_MODULE_BUILDER_DO_OPTIMIZATIONS
#if NDEBUG
#define CODEGEN_MODULE_BUILDER_DO_OPTIMIZATIONS 1
#else
#define CODEGEN_MODULE_BUILDER_DO_OPTIMIZATIONS 0
#endif
#endif

using llvm::CodeGenOpt::Level;
using llvm::ConstantExpr;
using llvm::ConstantInt;
using llvm::EngineBuilder;
using llvm::ExecutionEngine;
using llvm::Function;
using llvm::FunctionPassManager;
using llvm::FunctionType;
using llvm::IntegerType;
using llvm::LLVMContext;
using llvm::Module;
using llvm::PassManager;
using llvm::PassManagerBuilder;
using llvm::PointerType;
using llvm::raw_os_ostream;
using llvm::SMDiagnostic;
using llvm::TargetMachine;
using llvm::Type;
using llvm::Value;
using std::ostream;
using std::string;
using std::stringstream;
using std::vector;

namespace kudu {
namespace codegen {

namespace {

string ToString(const SMDiagnostic& err) {
  stringstream sstr;
  raw_os_ostream os(sstr);
  err.print("kudu/codegen", os);
  os.flush();
  return sstr.str();
}

string ToString(const Module& m) {
  stringstream sstr;
  raw_os_ostream os(sstr);
  os << m;
  return sstr.str();
}

// This method is needed for the implicit conversion from
// llvm::StringRef to std::string
string ToString(const Function* f) {
  return f->getName();
}

bool ModuleContains(const Module& m, const Function* fptr) {
  for (Module::const_iterator it = m.begin(); it != m.end(); ++it) {
    if (&*it == fptr) return true;
  }
  return false;
}

} // anonymous namespace

const char* const ModuleBuilder::kKuduIRFile =
  KUDU_CODEGEN_MODULE_BUILDER_PRECOMPILED_LL;

static std::string GetDefaultIRFileLocation() {
  char* kudu_home = getenv("KUDU_HOME");
  return kudu_home ? strings::Substitute("$0/precompiled.ll", kudu_home) : "";
}

ModuleBuilder::ModuleBuilder()
  : state_(kUninitialized),
    context_(new LLVMContext()),
    builder_(*context_) {}

ModuleBuilder::~ModuleBuilder() {}

Status ModuleBuilder::Init() {
  CHECK_EQ(state_, kUninitialized) << "Cannot Init() twice";
  // Parse IR file
  SMDiagnostic err;
  string ir_location = kKuduIRFile;
  module_.reset(llvm::ParseIRFile(kKuduIRFile, err, *context_));
  // We first try to find it next to the binaries. This is normally the case for dev environments.
  // If this fails, we look under KUDU_HOME.
  if (!module_) {
    ir_location = GetDefaultIRFileLocation();
    module_.reset(llvm::ParseIRFile(GetDefaultIRFileLocation(), err, *context_));
    if (!module_) {
      return Status::ConfigurationError("Could not parse IR file",
                                        ToString(err));
    }
  }
  VLOG(3) << "Successfully parsed IR file at " << ir_location << ":\n"
          << ToString(*module_);

  // TODO: consider loading this module once and then just copying it
  // from memory. If this strategy is used it may be worth trying to
  // reduce the .ll file size.

  state_ = kBuilding;
  return Status::OK();
}

Function* ModuleBuilder::Create(FunctionType* fty, const string& name) {
  CHECK_EQ(state_, kBuilding);
  return Function::Create(fty, Function::ExternalLinkage, name, module_.get());
}

Function* ModuleBuilder::GetFunction(const string& name) {
  CHECK_EQ(state_, kBuilding);
  // All extern "C" functions are guaranteed to have the same
  // exact name as declared in the source file.
  return CHECK_NOTNULL(module_->getFunction(name));
}

Type* ModuleBuilder::GetType(const string& name) {
  CHECK_EQ(state_, kBuilding);
  // Technically clang is not obligated to name every
  // class as "class.kudu::ClassName" but so long as there
  // are no naming conflicts in the LLVM context it appears
  // to do so (naming conflicts are avoided by having 1 context
  // per module)
  return CHECK_NOTNULL(module_->getTypeByName(name));
}

Value* ModuleBuilder::GetPointerValue(void* ptr) const {
  CHECK_EQ(state_, kBuilding);
  // No direct way of creating constant pointer values in LLVM, so
  // first a constant int has to be created and then casted to a pointer
  IntegerType* llvm_uintptr_t = Type::getIntNTy(*context_, 8 * sizeof(ptr));
  uintptr_t int_value = reinterpret_cast<uintptr_t>(ptr);
  ConstantInt* llvm_int_value = ConstantInt::get(llvm_uintptr_t,
                                                 int_value, false);
  Type* llvm_ptr_t = Type::getInt8PtrTy(*context_);
  return ConstantExpr::getIntToPtr(llvm_int_value, llvm_ptr_t);
}


void ModuleBuilder::AddJITPromise(llvm::Function* llvm_f,
                                  FunctionAddress* actual_f) {
  CHECK_EQ(state_, kBuilding);
  DCHECK(ModuleContains(*module_, llvm_f))
    << "Function " << ToString(llvm_f) << " does not belong to ModuleBuilder.";
  JITFuture fut;
  fut.llvm_f_ = llvm_f;
  fut.actual_f_ = actual_f;
  futures_.push_back(fut);
}

namespace {

#if CODEGEN_MODULE_BUILDER_DO_OPTIMIZATIONS

void DoOptimizations(ExecutionEngine* engine,
                     Module* module,
                     const vector<const char*>& external_functions) {
  PassManagerBuilder pass_builder;
  pass_builder.OptLevel = 2;
  // Don't optimize for code size (this corresponds to -O2/-O3)
  pass_builder.SizeLevel = 0;
  pass_builder.Inliner = llvm::createFunctionInliningPass();

  FunctionPassManager fpm(module);
  pass_builder.populateFunctionPassManager(fpm);
  fpm.doInitialization();

  // For each function in the module, optimize it
  BOOST_FOREACH(Function& f, *module) {
    // The bool return value here just indicates whether the passes did anything.
    // We can safely expect that many functions are too small to do any optimization.
    ignore_result(fpm.run(f));
  }
  fpm.doFinalization();

  PassManager module_passes;

  // Specifying the data layout is necessary for some optimizations (e.g. removing many of
  // the loads/stores produced by structs).
  // Transfers ownership of the data layout to module_passes.
  module_passes.add(new llvm::DataLayout(module->getDataLayout()));
  // Internalize all functions that aren't explicitly specified with external linkage.
  module_passes.add(llvm::createInternalizePass(external_functions));
  pass_builder.populateModulePassManager(module_passes);

  // Same as above, the result here just indicates whether optimization made any changes.
  // Don't need to check it.
  ignore_result(module_passes.run(*module));
}

#endif

} // anonymous namespace

Status ModuleBuilder::Compile(gscoped_ptr<ExecutionEngine>* out) {
  CHECK_EQ(state_, kBuilding);

  // Attempt to generate the engine
  string str;
#ifdef NDEBUG
  Level opt_level = llvm::CodeGenOpt::Aggressive;
#else
  Level opt_level = llvm::CodeGenOpt::None;
#endif
  EngineBuilder ebuilder(module_.get());
  ebuilder.setErrorStr(&str);
  ebuilder.setUseMCJIT(true);
  ebuilder.setOptLevel(opt_level);
  target_ = ebuilder.selectTarget();
  gscoped_ptr<ExecutionEngine> local_engine(ebuilder.create(target_));
  if (!local_engine) {
    return Status::ConfigurationError("Code generation for module failed. "
                                      "Could not start ExecutionEngine",
                                      str);
  }

#if CODEGEN_MODULE_BUILDER_DO_OPTIMIZATIONS
  DoOptimizations(local_engine.get(), module_.get(), GetFunctionNames());
#endif

  // Compile the module
  local_engine->finalizeObject();

  // Satisfy the promises
  BOOST_FOREACH(JITFuture& fut, futures_) {
    *fut.actual_f_ = local_engine->getPointerToFunction(fut.llvm_f_);
    if (*fut.actual_f_ == NULL) {
      return Status::NotFound(
        "Code generation for module failed. Could not find function \""
        + ToString(fut.llvm_f_) + "\".");
    }
  }

  // For LLVM 3.4, generated code lasts exactly as long as the execution engine
  // that created it does. Furthermore, if the module is removed from the
  // engine's ownership, neither the context nor the module have to stick
  // around for the jitted code to run. NOTE: this may change in LLVM 3.5
  CHECK(local_engine->removeModule(module_.get())); // releases ownership

  // Upon success write to the output parameter
  *out = local_engine.Pass();
  state_ = kCompiled;
  return Status::OK();
}

const TargetMachine& ModuleBuilder::GetTargetMachine() const {
  CHECK_EQ(state_, kCompiled);
  return *CHECK_NOTNULL(target_);
}

vector<const char*> ModuleBuilder::GetFunctionNames() const {
  vector<const char*> ret;
  BOOST_FOREACH(const JITFuture& fut, futures_) {
    const char* name = CHECK_NOTNULL(fut.llvm_f_)->getName().data();
    ret.push_back(name);
  }
  return ret;
}

} // namespace codegen
} // namespace kudu
