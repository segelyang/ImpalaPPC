// Copyright 2012 Cloudera Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "codegen/llvm-codegen.h"
#include <fstream>
#include <iostream>
#include <sstream>
#include <boost/algorithm/string.hpp>
#include <boost/thread/mutex.hpp>
#include <gutil/strings/substitute.h>

#include <llvm/ADT/Triple.h>
#include <llvm/Analysis/TargetTransformInfo.h>
#include <llvm/Analysis/InstructionSimplify.h>
#include <llvm/Analysis/Passes.h>
#include <llvm/Bitcode/ReaderWriter.h>
#include <llvm/ExecutionEngine/ExecutionEngine.h>
#include <llvm/ExecutionEngine/MCJIT.h>
#include <llvm/IR/DataLayout.h>
#include "llvm/IR/Function.h"
#include "llvm/IR/InstIterator.h"
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/NoFolder.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Linker/Linker.h>
#include <llvm/Support/DynamicLibrary.h>
#include <llvm/Support/ErrorHandling.h>
#include <llvm/Support/Host.h>
#include <llvm/Support/TargetRegistry.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Transforms/IPO.h>
#include <llvm/Transforms/IPO/PassManagerBuilder.h>
#include <llvm/Transforms/Scalar.h>
#include <llvm/Transforms/Utils/BasicBlockUtils.h>
#include <llvm/Transforms/Utils/Cloning.h>

#include "common/logging.h"
#include "codegen/codegen-anyval.h"
#include "codegen/impala-ir-data.h"
#include "codegen/instruction-counter.h"
#include "codegen/mcjit-mem-mgr.h"
#include "impala-ir/impala-ir-names.h"
#include "runtime/hdfs-fs-cache.h"
#include "util/cpu-info.h"
#include "util/hdfs-util.h"
#include "util/path-builder.h"
#include "util/test-info.h"

#include "common/names.h"

using namespace llvm;
using namespace strings;
using std::unique_ptr;
using std::fstream;

DEFINE_bool(print_llvm_ir_instruction_count, false,
    "if true, prints the instruction counts of all JIT'd functions");

DEFINE_bool(disable_optimization_passes, false,
    "if true, disables llvm optimization passes (used for testing)");
DEFINE_bool(dump_ir, false, "if true, output IR after optimization passes");
DEFINE_string(unopt_module_dir, "",
    "if set, saves unoptimized generated IR modules to the specified directory.");
DEFINE_string(opt_module_dir, "",
    "if set, saves optimized generated IR modules to the specified directory.");
DECLARE_string(local_library_dir);

namespace impala {

bool LlvmCodeGen::llvm_initialized_ = false;
string LlvmCodeGen::cpu_name_;
vector<string> LlvmCodeGen::cpu_attrs_;

static void LlvmCodegenHandleError(void* user_data, const std::string& reason,
    bool gen_crash_diag) {
  LOG(FATAL) << "LLVM hit fatal error: " << reason.c_str();
}

void LlvmCodeGen::InitializeLlvm(bool load_backend) {
  DCHECK(!llvm_initialized_);
  llvm::remove_fatal_error_handler();
  llvm::install_fatal_error_handler(LlvmCodegenHandleError);
  // These functions can *only* be called once per process and are used to set up
  // LLVM subsystems for code generation targeting the machine we're running on.
  llvm::InitializeNativeTarget();
  llvm::InitializeNativeTargetAsmPrinter();
  llvm::InitializeNativeTargetAsmParser();
  llvm_initialized_ = true;

  if (load_backend) {
    string path;
    // For test env, we have to load libfesupport.so to provide sym for LLVM.
    PathBuilder::GetFullBuildPath("service/libfesupport.so", &path);
    bool failed = llvm::sys::DynamicLibrary::LoadLibraryPermanently(path.c_str());
    DCHECK_EQ(failed, 0);
  }

  cpu_name_ = llvm::sys::getHostCPUName().str();
  LOG(INFO) << "CPU class for runtime code generation: " << cpu_name_;
  GetHostCPUAttrs(&cpu_attrs_);
  LOG(INFO) << "CPU flags for runtime code generation: "
            << boost::algorithm::join(cpu_attrs_, ",");
}


LlvmCodeGen::LlvmCodeGen(ObjectPool* pool, const string& id) :
  id_(id),
  profile_(pool, "CodeGen"),
  optimizations_enabled_(false),
  is_corrupt_(false),
  is_compiled_(false),
  context_(new llvm::LLVMContext()),
  module_(NULL) {

  DCHECK(llvm_initialized_) << "Must call LlvmCodeGen::InitializeLlvm first.";

  load_module_timer_ = ADD_TIMER(&profile_, "LoadTime");
  prepare_module_timer_ = ADD_TIMER(&profile_, "PrepareTime");
  module_bitcode_size_ = ADD_COUNTER(&profile_, "ModuleBitcodeSize", TUnit::BYTES);
  codegen_timer_ = ADD_TIMER(&profile_, "CodegenTime");
  optimization_timer_ = ADD_TIMER(&profile_, "OptimizationTime");
  compile_timer_ = ADD_TIMER(&profile_, "CompileTime");

  loaded_functions_.resize(IRFunction::FN_END);
}

Status LlvmCodeGen::CreateFromFile(ObjectPool* pool,
    const string& file, const string& id, scoped_ptr<LlvmCodeGen>* codegen) {
  codegen->reset(new LlvmCodeGen(pool, id));
  SCOPED_TIMER((*codegen)->profile_.total_time_counter());

  unique_ptr<Module> loaded_module;
  RETURN_IF_ERROR((*codegen)->LoadModuleFromFile(file, &loaded_module));

  return (*codegen)->Init(std::move(loaded_module));
}

Status LlvmCodeGen::CreateFromMemory(ObjectPool* pool, MemoryBufferRef module_ir,
    const string& module_name, const string& id, scoped_ptr<LlvmCodeGen>* codegen) {
  codegen->reset(new LlvmCodeGen(pool, id));
  SCOPED_TIMER((*codegen)->profile_.total_time_counter());

  unique_ptr<Module> loaded_module;
  RETURN_IF_ERROR(
      (*codegen)->LoadModuleFromMemory(module_ir, module_name, &loaded_module));

  return (*codegen)->Init(std::move(loaded_module));
}

Status LlvmCodeGen::LoadModuleFromFile(const string& file, unique_ptr<Module>* module) {
  unique_ptr<MemoryBuffer> file_buffer;
  {
    SCOPED_TIMER(load_module_timer_);

    ErrorOr<unique_ptr<MemoryBuffer>> tmp_file_buffer = MemoryBuffer::getFile(file);
    if (!tmp_file_buffer) {
      stringstream ss;
      ss << "Could not load module " << file << ": "
         << tmp_file_buffer.getError().message();
      return Status(ss.str());
    }
    file_buffer = std::move(tmp_file_buffer.get());
  }

  COUNTER_ADD(module_bitcode_size_, file_buffer->getBufferSize());
  return LoadModuleFromMemory(file_buffer->getMemBufferRef(), file, module);
}

Status LlvmCodeGen::LoadModuleFromMemory(MemoryBufferRef module_ir, string module_name,
    unique_ptr<Module>* module) {
  DCHECK(!module_name.empty());
  SCOPED_TIMER(prepare_module_timer_);
  ErrorOr<unique_ptr<Module>> tmp_module =
      parseBitcodeFile(module_ir, context());
  if (!tmp_module) {
    stringstream ss;
    ss << "Could not parse module " << module_name << ": " << tmp_module.getError();
    return Status(ss.str());
  }

  *module = std::move(tmp_module.get());
  (*module)->setModuleIdentifier(module_name);
  COUNTER_ADD(module_bitcode_size_, module_ir.getBufferSize());
  return Status::OK();
}

// TODO: Create separate counters/timers (file size, load time) for each module linked
Status LlvmCodeGen::LinkModule(const string& file) {
  if (linked_modules_.find(file) != linked_modules_.end()) return Status::OK();

  SCOPED_TIMER(profile_.total_time_counter());
  unique_ptr<Module> new_module;
  RETURN_IF_ERROR(LoadModuleFromFile(file, &new_module));

  // The module data layout must match the one selected by the execution engine.
  new_module->setDataLayout(execution_engine_->getDataLayout());

  bool error = Linker::linkModules(*module_, std::move(new_module));
  if (error) {
    stringstream ss;
    ss << "Problem linking " << file << " to main module.";
    return Status(ss.str());
  }
  linked_modules_.insert(file);
  return Status::OK();
}

Status LlvmCodeGen::CreateImpalaCodegen(
    ObjectPool* pool, const string& id, scoped_ptr<LlvmCodeGen>* codegen_ret) {
  // Select the appropriate IR version.  We cannot use LLVM IR with sse instructions on
  // a machine without sse support (loading the module will fail regardless of whether
  // those instructions are run or not).
  StringRef module_ir;
  string module_name;
  if (CpuInfo::IsSupported(CpuInfo::SSE4_2)) {
    module_ir = StringRef(reinterpret_cast<const char*>(impala_sse_llvm_ir),
        impala_sse_llvm_ir_len);
    module_name = "Impala IR with SSE support";
  } else {
    module_ir = StringRef(reinterpret_cast<const char*>(impala_no_sse_llvm_ir),
        impala_no_sse_llvm_ir_len);
    module_name = "Impala IR with no SSE support";
  }
  unique_ptr<MemoryBuffer> module_ir_buf(
      MemoryBuffer::getMemBuffer(module_ir, "", false));
  RETURN_IF_ERROR(CreateFromMemory(pool, module_ir_buf->getMemBufferRef(), module_name, id,
      codegen_ret));
  LlvmCodeGen* codegen = codegen_ret->get();

  // Parse module for cross compiled functions and types
  SCOPED_TIMER(codegen->profile_.total_time_counter());
  SCOPED_TIMER(codegen->prepare_module_timer_);

  // Get type for StringValue
  codegen->string_val_type_ = codegen->GetType(StringValue::LLVM_CLASS_NAME);

  // Get type for TimestampValue
  codegen->timestamp_val_type_ = codegen->GetType(TimestampValue::LLVM_CLASS_NAME);

  // Verify size is correct
  const DataLayout& data_layout = codegen->execution_engine()->getDataLayout();
  const StructLayout* layout =
      data_layout.getStructLayout(static_cast<StructType*>(codegen->string_val_type_));
  if (layout->getSizeInBytes() != sizeof(StringValue)) {
    DCHECK_EQ(layout->getSizeInBytes(), sizeof(StringValue));
    return Status("Could not create llvm struct type for StringVal");
  }

  // Parse functions from module
  vector<Function*> functions;
  codegen->GetFunctions(&functions);
  int parsed_functions = 0;
  for (int i = 0; i < functions.size(); ++i) {
    string fn_name = functions[i]->getName();
    for (int j = IRFunction::FN_START; j < IRFunction::FN_END; ++j) {
      // Substring match to match precompiled functions.  The compiled function names
      // will be mangled.
      // TODO: reconsider this.  Substring match is probably not strict enough but
      // undoing the mangling is no fun either.
      if (fn_name.find(FN_MAPPINGS[j].fn_name) != string::npos) {
        // TODO: make this a DCHECK when we resolve IMPALA-2439
        CHECK(codegen->loaded_functions_[FN_MAPPINGS[j].fn] == NULL)
            << "Duplicate definition found for function " << FN_MAPPINGS[j].fn_name
            << ": " << fn_name;
        functions[i]->addFnAttr(Attribute::AlwaysInline);
        codegen->loaded_functions_[FN_MAPPINGS[j].fn] = functions[i];
        ++parsed_functions;
      }
    }
  }

  if (parsed_functions != IRFunction::FN_END) {
    stringstream ss;
    ss << "Unable to find these precompiled functions: ";
    bool first = true;
    for (int i = IRFunction::FN_START; i != IRFunction::FN_END; ++i) {
      if (codegen->loaded_functions_[i] == NULL) {
        if (!first) ss << ", ";
        ss << FN_MAPPINGS[i].fn_name;
        first = false;
      }
    }
    return Status(ss.str());
  }

  return Status::OK();
}

Status LlvmCodeGen::Init(unique_ptr<Module> module) {
  DCHECK(module != NULL);

  llvm::CodeGenOpt::Level opt_level = CodeGenOpt::Aggressive;
#ifndef NDEBUG
  // For debug builds, don't generate JIT compiled optimized assembly.
  // This takes a non-neglible amount of time (~.5 ms per function) and
  // blows up the fe tests (which take ~10-20 ms each).
  opt_level = CodeGenOpt::None;
#endif
  module_ = module.get();
  EngineBuilder builder(std::move(module));
  builder.setEngineKind(EngineKind::JIT);
  builder.setOptLevel(opt_level);
  builder.setMCJITMemoryManager(
      unique_ptr<ImpalaMCJITMemoryManager>(new ImpalaMCJITMemoryManager()));
  builder.setMCPU(cpu_name_);
  builder.setMAttrs(cpu_attrs_);
  builder.setErrorStr(&error_string_);

  execution_engine_.reset(builder.create());
  if (execution_engine_ == NULL) {
    module_ = NULL; // module_ was owned by builder.
    stringstream ss;
    ss << "Could not create ExecutionEngine: " << error_string_;
    return Status(ss.str());
  }

  // The module data layout must match the one selected by the execution engine.
  module_->setDataLayout(execution_engine_->getDataLayout());

  void_type_ = Type::getVoidTy(context());
  ptr_type_ = PointerType::get(GetType(TYPE_TINYINT), 0);
  true_value_ = ConstantInt::get(context(), APInt(1, true, true));
  false_value_ = ConstantInt::get(context(), APInt(1, false, true));

  RETURN_IF_ERROR(LoadIntrinsics());

  return Status::OK();
}

LlvmCodeGen::~LlvmCodeGen() {
}

void LlvmCodeGen::EnableOptimizations(bool enable) {
  optimizations_enabled_ = enable;
}

void LlvmCodeGen::GetHostCPUAttrs(vector<string>* attrs) {
  // LLVM's ExecutionEngine expects features to be enabled or disabled with a list
  // of strings like ["+feature1", "-feature2"].
  StringMap<bool> cpu_features;
  llvm::sys::getHostCPUFeatures(cpu_features);
  for (const StringMapEntry<bool>& entry: cpu_features) {
    attrs->emplace_back(
        Substitute("$0$1", entry.second ? "+" : "-", entry.first().data()));
  }
}

string LlvmCodeGen::GetIR(bool full_module) const {
  string str;
  raw_string_ostream stream(str);
  if (full_module) {
    module_->print(stream, NULL);
  } else {
    for (int i = 0; i < codegend_functions_.size(); ++i) {
        codegend_functions_[i]->print(stream, true);
    }
  }
  return str;
}

Type* LlvmCodeGen::GetType(const ColumnType& type) {
  switch (type.type) {
    case TYPE_NULL:
      return Type::getInt1Ty(context());
    case TYPE_BOOLEAN:
      return Type::getInt1Ty(context());
    case TYPE_TINYINT:
      return Type::getInt8Ty(context());
    case TYPE_SMALLINT:
      return Type::getInt16Ty(context());
    case TYPE_INT:
      return Type::getInt32Ty(context());
    case TYPE_BIGINT:
      return Type::getInt64Ty(context());
    case TYPE_FLOAT:
      return Type::getFloatTy(context());
    case TYPE_DOUBLE:
      return Type::getDoubleTy(context());
    case TYPE_STRING:
    case TYPE_VARCHAR:
      return string_val_type_;
    case TYPE_CHAR:
      // IMPALA-3207: Codegen for CHAR is not yet implemented, this should not
      // be called for TYPE_CHAR.
      DCHECK(false) << "NYI";
      return NULL;
    case TYPE_TIMESTAMP:
      return timestamp_val_type_;
    case TYPE_DECIMAL:
      return Type::getIntNTy(context(), type.GetByteSize() * 8);
    default:
      DCHECK(false) << "Invalid type: " << type;
      return NULL;
  }
}

PointerType* LlvmCodeGen::GetPtrType(const ColumnType& type) {
  return PointerType::get(GetType(type), 0);
}

Type* LlvmCodeGen::GetType(const string& name) {
  Type* type = module_->getTypeByName(name);
  DCHECK(type != NULL) << name;
  return type;
}

PointerType* LlvmCodeGen::GetPtrType(const string& name) {
  Type* type = GetType(name);
  DCHECK(type != NULL) << name;
  return PointerType::get(type, 0);
}

PointerType* LlvmCodeGen::GetPtrType(Type* type) {
  return PointerType::get(type, 0);
}

// Llvm doesn't let you create a PointerValue from a c-side ptr.  Instead
// cast it to an int and then to 'type'.
Value* LlvmCodeGen::CastPtrToLlvmPtr(Type* type, const void* ptr) {
  Constant* const_int = ConstantInt::get(Type::getInt64Ty(context()), (int64_t)ptr);
  return ConstantExpr::getIntToPtr(const_int, type);
}

Value* LlvmCodeGen::GetIntConstant(PrimitiveType type, int64_t val) {
  switch (type) {
    case TYPE_TINYINT:
      return ConstantInt::get(context(), APInt(8, val));
    case TYPE_SMALLINT:
      return ConstantInt::get(context(), APInt(16, val));
    case TYPE_INT:
      return ConstantInt::get(context(), APInt(32, val));
    case TYPE_BIGINT:
      return ConstantInt::get(context(), APInt(64, val));
    default:
      DCHECK(false);
      return NULL;
  }
}

Value* LlvmCodeGen::GetIntConstant(int num_bytes, int64_t val) {
  return ConstantInt::get(context(), APInt(8 * num_bytes, val));
}

AllocaInst* LlvmCodeGen::CreateEntryBlockAlloca(Function* f, const NamedVariable& var) {
  IRBuilder<> tmp(&f->getEntryBlock(), f->getEntryBlock().begin());
  AllocaInst* alloca = tmp.CreateAlloca(var.type, 0, var.name.c_str());
  if (var.type == GetType(CodegenAnyVal::LLVM_DECIMALVAL_NAME)) {
    // Generated functions may manipulate DecimalVal arguments via SIMD instructions such
    // as 'movaps' that require 16-byte memory alignment. LLVM uses 8-byte alignment by
    // default, so explicitly set the alignment for DecimalVals.
    alloca->setAlignment(16);
  }
  return alloca;
}

AllocaInst* LlvmCodeGen::CreateEntryBlockAlloca(const LlvmBuilder& builder, Type* type,
                                                const char* name) {
  return CreateEntryBlockAlloca(builder.GetInsertBlock()->getParent(),
                                NamedVariable(name, type));
}

void LlvmCodeGen::CreateIfElseBlocks(Function* fn, const string& if_name,
    const string& else_name, BasicBlock** if_block, BasicBlock** else_block,
    BasicBlock* insert_before) {
  *if_block = BasicBlock::Create(context(), if_name, fn, insert_before);
  *else_block = BasicBlock::Create(context(), else_name, fn, insert_before);
}

Function* LlvmCodeGen::GetLibCFunction(FnPrototype* prototype) {
  if (external_functions_.find(prototype->name()) != external_functions_.end()) {
    return external_functions_[prototype->name()];
  }
  Function* func = prototype->GeneratePrototype();
  external_functions_[prototype->name()] = func;
  return func;
}

Function* LlvmCodeGen::GetFunction(IRFunction::Type function, bool clone) {
  DCHECK(loaded_functions_[function] != NULL);
  Function* fn = loaded_functions_[function];
  if (clone) return CloneFunction(fn);
  return fn;
}

// TODO: this should return a Status
bool LlvmCodeGen::VerifyFunction(Function* fn) {
  if (is_corrupt_) return false;

  // Check that there are no calls to Expr::GetConstant(). These should all have been
  // inlined via Expr::InlineConstants().
  for (inst_iterator iter = inst_begin(fn); iter != inst_end(fn); ++iter) {
    Instruction* instr = &*iter;
    if (!isa<CallInst>(instr)) continue;
    CallInst* call_instr = reinterpret_cast<CallInst*>(instr);
    Function* called_fn = call_instr->getCalledFunction();
    // look for call to Expr::GetConstant()
    if (called_fn != NULL &&
        called_fn->getName().find(Expr::GET_CONSTANT_INT_SYMBOL_PREFIX) != string::npos) {
      LOG(ERROR) << "Found call to Expr::GetConstant*(): " << Print(call_instr);
      is_corrupt_ = true;
      break;
    }
  }

  // There is an llvm bug (#10957) that causes the first step of the verifier to always
  // abort the process if it runs into an issue and ignores ReturnStatusAction.  This
  // would cause impalad to go down if one query has a problem.  To work around this, we
  // will copy that step here and not abort on error. Adapted from the pre-verifier
  // function pass.
  // TODO: doesn't seem there is much traction in getting this fixed but we'll see
  for (Function::iterator i = fn->begin(), e = fn->end(); i != e; ++i) {
    if (i->empty() || !i->back().isTerminator()) {
      LOG(ERROR) << "Basic block must end with terminator: \n" << Print(&(*i));
      is_corrupt_ = true;
      break;
    }
  }

//  if (!is_corrupt_) is_corrupt_ = llvm::verifyFunction(*fn, PrintMessageAction);
  if (!is_corrupt_) {
    string str;
    raw_string_ostream stream(str);
    is_corrupt_ = verifyFunction(*fn, &stream);
    if (is_corrupt_) LOG(ERROR) << str;
  }

  if (is_corrupt_) {
    string fn_name = fn->getName(); // llvm has some fancy operator overloading
    LOG(ERROR) << "Function corrupt: " << fn_name;
    fn->dump();
    return false;
  }
  return true;
}

LlvmCodeGen::FnPrototype::FnPrototype(
    LlvmCodeGen* gen, const string& name, Type* ret_type) :
  codegen_(gen), name_(name), ret_type_(ret_type) {
  DCHECK(!codegen_->is_compiled_) << "Not valid to add additional functions";
}

Function* LlvmCodeGen::FnPrototype::GeneratePrototype(
      LlvmBuilder* builder, Value** params) {
  vector<Type*> arguments;
  for (int i = 0; i < args_.size(); ++i) {
    arguments.push_back(args_[i].type);
  }
  FunctionType* prototype = FunctionType::get(ret_type_, arguments, false);

  Function* fn = Function::Create(
      prototype, Function::ExternalLinkage, name_, codegen_->module_);
  DCHECK(fn != NULL);

  // Name the arguments
  int idx = 0;
  for (Function::arg_iterator iter = fn->arg_begin();
      iter != fn->arg_end(); ++iter, ++idx) {
    iter->setName(args_[idx].name);
    if (params != NULL) params[idx] = &*iter;
  }

  if (builder != NULL) {
    BasicBlock* entry_block = BasicBlock::Create(codegen_->context(), "entry", fn);
    builder->SetInsertPoint(entry_block);
  }

  codegen_->codegend_functions_.push_back(fn);
  return fn;
}

int LlvmCodeGen::ReplaceCallSites(Function* caller, Function* new_fn,
    const string& replacee_name) {
  DCHECK(!is_compiled_);
  DCHECK(caller->getParent() == module_);
  DCHECK(caller != NULL);
  DCHECK(new_fn != NULL);
  int replaced = 0;
  for (inst_iterator iter = inst_begin(caller); iter != inst_end(caller); ++iter) {
    Instruction* instr = &*iter;
    // look for call instructions
    if (CallInst::classof(instr)) {
      CallInst* call_instr = reinterpret_cast<CallInst*>(instr);
      Function* old_fn = call_instr->getCalledFunction();
      // look for call instruction that matches the name
      if (old_fn != NULL && old_fn->getName().find(replacee_name) != string::npos) {
        // Replace the called function
        call_instr->setCalledFunction(new_fn);
        ++replaced;
      }
    }
  }
  return replaced;
}

Function* LlvmCodeGen::CloneFunction(Function* fn) {
  DCHECK(!is_compiled_);
  ValueToValueMapTy dummy_vmap;
  // CloneFunction() automatically gives the new function a unique name
  Function* fn_clone = llvm::CloneFunction(fn, dummy_vmap, false);
  fn_clone->copyAttributesFrom(fn);
  module_->getFunctionList().push_back(fn_clone);
  return fn_clone;
}

Function* LlvmCodeGen::FinalizeFunction(Function* function) {
  function->addFnAttr(llvm::Attribute::AlwaysInline);

  if (!VerifyFunction(function)) {
    function->eraseFromParent(); // deletes function
    return NULL;
  }
  if (FLAGS_dump_ir) function->dump();
  return function;
}

Status LlvmCodeGen::FinalizeModule() {
  DCHECK(!is_compiled_);
  is_compiled_ = true;

  if (FLAGS_unopt_module_dir.size() != 0) {
    string path = FLAGS_unopt_module_dir + "/" + id_ + "_unopt.ll";
    fstream f(path.c_str(), fstream::out | fstream::trunc);
    if (f.fail()) {
      LOG(ERROR) << "Could not save IR to: " << path;
    } else {
      f << GetIR(true);
      f.close();
    }
  }

  if (is_corrupt_) return Status("Module is corrupt.");
  SCOPED_TIMER(profile_.total_time_counter());

  // Don't waste time optimizing module if there are no functions to JIT. This can happen
  // if the codegen object is created but no functions are successfully codegen'd.
  if (fns_to_jit_compile_.empty()) return Status::OK();

  if (optimizations_enabled_ && !FLAGS_disable_optimization_passes) OptimizeModule();

  if (FLAGS_opt_module_dir.size() != 0) {
    string path = FLAGS_opt_module_dir + "/" + id_ + "_opt.ll";
    fstream f(path.c_str(), fstream::out | fstream::trunc);
    if (f.fail()) {
      LOG(ERROR) << "Could not save IR to: " << path;
    } else {
      f << GetIR(true);
      f.close();
    }
  }

  {
    SCOPED_TIMER(compile_timer_);
    // Finalize module, which compiles all functions.
    execution_engine_->finalizeObject();
  }

  // Get pointers to all codegen'd functions.
  for (int i = 0; i < fns_to_jit_compile_.size(); ++i) {
    Function* function = fns_to_jit_compile_[i].first;
    void* jitted_function = execution_engine_->getPointerToFunction(function);
    DCHECK(jitted_function != NULL) << "Failed to jit " << function->getName().data();
    *fns_to_jit_compile_[i].second = jitted_function;
  }
  return Status::OK();
}

void LlvmCodeGen::OptimizeModule() {
  SCOPED_TIMER(optimization_timer_);

  // This pass manager will construct optimizations passes that are "typical" for
  // c/c++ programs.  We're relying on llvm to pick the best passes for us.
  // TODO: we can likely muck with this to get better compile speeds or write
  // our own passes.  Our subexpression elimination optimization can be rolled into
  // a pass.
  PassManagerBuilder pass_builder;
  // 2 maps to -O2
  // TODO: should we switch to 3? (3 may not produce different IR than 2 while taking
  // longer, but we should check)
  pass_builder.OptLevel = 2;
  // Don't optimize for code size (this corresponds to -O2/-O3)
  pass_builder.SizeLevel = 0;
  pass_builder.Inliner = createFunctionInliningPass();

  // The TargetIRAnalysis pass is required to provide information about the target
  // machine to optimisation passes, e.g. the cost model.
  TargetIRAnalysis target_analysis =
      execution_engine_->getTargetMachine()->getTargetIRAnalysis();

  // Before running any other optimization passes, run the internalize pass, giving it
  // the names of all functions registered by AddFunctionToJit(), followed by the
  // global dead code elimination pass. This causes all functions not registered to be
  // JIT'd to be marked as internal, and any internal functions that are not used are
  // deleted by DCE pass. This greatly decreases compile time by removing unused code.
  vector<const char*> exported_fn_names;
  for (int i = 0; i < fns_to_jit_compile_.size(); ++i) {
    exported_fn_names.push_back(fns_to_jit_compile_[i].first->getName().data());
  }
  unique_ptr<legacy::PassManager> module_pass_manager(new legacy::PassManager());
  module_pass_manager->add(createTargetTransformInfoWrapperPass(target_analysis));
  module_pass_manager->add(createInternalizePass(exported_fn_names));
  module_pass_manager->add(createGlobalDCEPass());
  module_pass_manager->run(*module_);

  // Update counters before final optimization, but after removing unused functions. This
  // gives us a rough measure of how much work the optimization and compilation must do.
  InstructionCounter counter;
  counter.visit(*module_);
  COUNTER_SET(num_functions_, counter.GetCount(InstructionCounter::TOTAL_FUNCTIONS));
  COUNTER_SET(num_instructions_, counter.GetCount(InstructionCounter::TOTAL_INSTS));

  // Create and run function pass manager
  unique_ptr<legacy::FunctionPassManager> fn_pass_manager(
      new legacy::FunctionPassManager(module_));
  fn_pass_manager->add(createTargetTransformInfoWrapperPass(target_analysis));
  pass_builder.populateFunctionPassManager(*fn_pass_manager);
  fn_pass_manager->doInitialization();
  for (Module::iterator it = module_->begin(), end = module_->end(); it != end ; ++it) {
    if (!it->isDeclaration()) fn_pass_manager->run(*it);
  }
  fn_pass_manager->doFinalization();

  // Create and run module pass manager
  module_pass_manager.reset(new legacy::PassManager());
  module_pass_manager->add(createTargetTransformInfoWrapperPass(target_analysis));
  pass_builder.populateModulePassManager(*module_pass_manager);
  module_pass_manager->run(*module_);
  if (FLAGS_print_llvm_ir_instruction_count) {
    for (int i = 0; i < fns_to_jit_compile_.size(); ++i) {
      InstructionCounter counter;
      counter.visit(*fns_to_jit_compile_[i].first);
      VLOG(1) << fns_to_jit_compile_[i].first->getName().str();
      VLOG(1) << counter.PrintCounters();
    }
  }
}

void LlvmCodeGen::AddFunctionToJit(Function* fn, void** fn_ptr) {
  Type* decimal_val_type = GetType(CodegenAnyVal::LLVM_DECIMALVAL_NAME);
  if (fn->getReturnType() == decimal_val_type) {
    // Per the x86 calling convention ABI, DecimalVals should be returned via an extra
    // first DecimalVal* argument. We generate non-compliant functions that return the
    // DecimalVal directly, which we can call from generated code, but not from compiled
    // native code.  To avoid accidentally calling a non-compliant function from native
    // code, call 'function' from an ABI-compliant wrapper.
    stringstream name;
    name << fn->getName().str() << "ABIWrapper";
    LlvmCodeGen::FnPrototype prototype(this, name.str(), void_type_);
    // Add return argument
    prototype.AddArgument(NamedVariable("result", decimal_val_type->getPointerTo()));
    // Add regular arguments
    for (Function::arg_iterator arg = fn->arg_begin(); arg != fn->arg_end(); ++arg) {
      prototype.AddArgument(NamedVariable(arg->getName(), arg->getType()));
    }
    LlvmBuilder builder(context());
    Value* args[fn->arg_size() + 1];
    Function* fn_wrapper = prototype.GeneratePrototype(&builder, &args[0]);
    fn_wrapper->addFnAttr(llvm::Attribute::AlwaysInline);
    // Mark first argument as sret (not sure if this is necessary but it can't hurt)
    fn_wrapper->addAttribute(1, Attribute::StructRet);
    // Call 'fn' and store the result in the result argument
    Value* result =
        builder.CreateCall(fn, ArrayRef<Value*>(&args[1], fn->arg_size()), "result");
    builder.CreateStore(result, args[0]);
    builder.CreateRetVoid();
    fn = FinalizeFunction(fn_wrapper);
    DCHECK(fn != NULL);
  }
  AddFunctionToJitInternal(fn, fn_ptr);
}

void LlvmCodeGen::AddFunctionToJitInternal(Function* fn, void** fn_ptr) {
  DCHECK(!is_compiled_);
  fns_to_jit_compile_.push_back(make_pair(fn, fn_ptr));
}

void LlvmCodeGen::CodegenDebugTrace(LlvmBuilder* builder, const char* str,
    Value* v1) {
  LOG(ERROR) << "Remove IR codegen debug traces before checking in.";

  // Make a copy of str into memory owned by this object.  This is no guarantee that str is
  // still around when the debug printf is executed.
  debug_strings_.push_back(Substitute("LLVM Trace: $0", str));
  str = debug_strings_.back().c_str();

  Function* printf = module()->getFunction("printf");
  DCHECK(printf != NULL);

  // Call printf by turning 'str' into a constant ptr value
  Value* str_ptr = CastPtrToLlvmPtr(ptr_type_, const_cast<char*>(str));

  vector<Value*> calling_args;
  calling_args.push_back(str_ptr);
  if (v1 != NULL) calling_args.push_back(v1);
  builder->CreateCall(printf, calling_args);
}

void LlvmCodeGen::GetFunctions(vector<Function*>* functions) {
  for (Function& fn: module_->functions()) {
    if (!fn.empty()) functions->push_back(&fn);
  }
}

void LlvmCodeGen::GetSymbols(unordered_set<string>* symbols) {
  for (const Function& fn: module_->functions()) {
    if (!fn.empty()) symbols->insert(fn.getName());
  }
}

// TODO: cache this function (e.g. all min(int, int) are identical).
// we probably want some more global IR function cache, or, implement this
// in c and precompile it with clang.
// define i32 @Min(i32 %v1, i32 %v2) {
// entry:
//   %0 = icmp slt i32 %v1, %v2
//   br i1 %0, label %ret_v1, label %ret_v2
//
// ret_v1:                                           ; preds = %entry
//   ret i32 %v1
//
// ret_v2:                                           ; preds = %entry
//   ret i32 %v2
// }
Function* LlvmCodeGen::CodegenMinMax(const ColumnType& type, bool min) {
  LlvmCodeGen::FnPrototype prototype(this, min ? "Min" : "Max", GetType(type));
  prototype.AddArgument(LlvmCodeGen::NamedVariable("v1", GetType(type)));
  prototype.AddArgument(LlvmCodeGen::NamedVariable("v2", GetType(type)));

  Value* params[2];
  LlvmBuilder builder(context());
  Function* fn = prototype.GeneratePrototype(&builder, &params[0]);

  Value* compare = NULL;
  switch (type.type) {
    case TYPE_NULL:
      compare = false_value();
      break;
    case TYPE_BOOLEAN:
      if (min) {
        // For min, return x && y
        compare = builder.CreateAnd(params[0], params[1]);
      } else {
        // For max, return x || y
        compare = builder.CreateOr(params[0], params[1]);
      }
      break;
    case TYPE_TINYINT:
    case TYPE_SMALLINT:
    case TYPE_INT:
    case TYPE_BIGINT:
    case TYPE_DECIMAL:
      if (min) {
        compare = builder.CreateICmpSLT(params[0], params[1]);
      } else {
        compare = builder.CreateICmpSGT(params[0], params[1]);
      }
      break;
    case TYPE_FLOAT:
    case TYPE_DOUBLE:
      if (min) {
        compare = builder.CreateFCmpULT(params[0], params[1]);
      } else {
        compare = builder.CreateFCmpUGT(params[0], params[1]);
      }
      break;
    default:
      DCHECK(false);
  }

  if (type.type == TYPE_BOOLEAN) {
    builder.CreateRet(compare);
  } else {
    BasicBlock* ret_v1, *ret_v2;
    CreateIfElseBlocks(fn, "ret_v1", "ret_v2", &ret_v1, &ret_v2);

    builder.CreateCondBr(compare, ret_v1, ret_v2);
    builder.SetInsertPoint(ret_v1);
    builder.CreateRet(params[0]);
    builder.SetInsertPoint(ret_v2);
    builder.CreateRet(params[1]);
  }

  fn = FinalizeFunction(fn);
  return fn;
}

// Intrinsics are loaded one by one.  Some are overloaded (e.g. memcpy) and the types must
// be specified.
// TODO: is there a better way to do this?
Status LlvmCodeGen::LoadIntrinsics() {
  // Load memcpy
  {
    Type* types[] = { ptr_type(), ptr_type(), GetType(TYPE_INT) };
    Function* fn = Intrinsic::getDeclaration(module(), Intrinsic::memcpy, types);
    if (fn == NULL) {
      return Status("Could not find memcpy intrinsic.");
    }
    llvm_intrinsics_[Intrinsic::memcpy] = fn;
  }

  // TODO: where is the best place to put this?
  struct {
    Intrinsic::ID id;
    const char* error;
  } non_overloaded_intrinsics[] = {
    { Intrinsic::x86_sse42_crc32_32_8, "sse4.2 crc32_u8" },
    { Intrinsic::x86_sse42_crc32_32_16, "sse4.2 crc32_u16" },
    { Intrinsic::x86_sse42_crc32_32_32, "sse4.2 crc32_u32" },
    { Intrinsic::x86_sse42_crc32_64_64, "sse4.2 crc32_u64" },
  };
  const int num_intrinsics =
      sizeof(non_overloaded_intrinsics) / sizeof(non_overloaded_intrinsics[0]);

  for (int i = 0; i < num_intrinsics; ++i) {
    Intrinsic::ID id = non_overloaded_intrinsics[i].id;
    Function* fn = Intrinsic::getDeclaration(module(), id);
    if (fn == NULL) {
      stringstream ss;
      ss << "Could not find " << non_overloaded_intrinsics[i].error << " intrinsic";
      return Status(ss.str());
    }
    llvm_intrinsics_[id] = fn;
  }

  return Status::OK();
}

void LlvmCodeGen::CodegenMemcpy(LlvmBuilder* builder, Value* dst, Value* src, int size) {
  DCHECK_GE(size, 0);
  if (size == 0) return;

  // Cast src/dst to int8_t*.  If they already are, this will get optimized away
  DCHECK(PointerType::classof(dst->getType()));
  DCHECK(PointerType::classof(src->getType()));
  dst = builder->CreateBitCast(dst, ptr_type());
  src = builder->CreateBitCast(src, ptr_type());

  // Get intrinsic function.
  Function* memcpy_fn = llvm_intrinsics_[Intrinsic::memcpy];
  DCHECK(memcpy_fn != NULL);

  // The fourth argument is the alignment.  For non-zero values, the caller
  // must guarantee that the src and dst values are aligned to that byte boundary.
  // TODO: We should try to take advantage of this since our tuples are well aligned.
  Value* args[] = {
    dst, src, GetIntConstant(TYPE_INT, size),
    GetIntConstant(TYPE_INT, 0),
    false_value()                       // is_volatile.
  };
  builder->CreateCall(memcpy_fn, args);
}

Value* LlvmCodeGen::CodegenArrayAt(LlvmBuilder* builder, Value* array, int idx,
    const char* name) {
  DCHECK(array->getType()->isPointerTy() || array->getType()->isArrayTy())
      << Print(array->getType());
  Value* ptr = builder->CreateConstGEP1_32(array, idx);
  return builder->CreateLoad(ptr, name);
}

void LlvmCodeGen::ClearHashFns() {
  hash_fns_.clear();
}

// Codegen to compute hash for a particular byte size.  Loops are unrolled in this
// process.  For the case where num_bytes == 11, we'd do this by calling
//   1. crc64 (for first 8 bytes)
//   2. crc16 (for bytes 9, 10)
//   3. crc8 (for byte 11)
// The resulting IR looks like:
// define i32 @CrcHash11(i8* %data, i32 %len, i32 %seed) {
// entry:
//   %0 = zext i32 %seed to i64
//   %1 = bitcast i8* %data to i64*
//   %2 = getelementptr i64* %1, i32 0
//   %3 = load i64* %2
//   %4 = call i64 @llvm.x86.sse42.crc32.64.64(i64 %0, i64 %3)
//   %5 = trunc i64 %4 to i32
//   %6 = getelementptr i8* %data, i32 8
//   %7 = bitcast i8* %6 to i16*
//   %8 = load i16* %7
//   %9 = call i32 @llvm.x86.sse42.crc32.32.16(i32 %5, i16 %8)
//   %10 = getelementptr i8* %6, i32 2
//   %11 = load i8* %10
//   %12 = call i32 @llvm.x86.sse42.crc32.32.8(i32 %9, i8 %11)
//   ret i32 %12
// }
Function* LlvmCodeGen::GetHashFunction(int num_bytes) {
  if (CpuInfo::IsSupported(CpuInfo::SSE4_2)) {
    if (num_bytes == -1) {
      // -1 indicates variable length, just return the generic loop based
      // hash fn.
      return GetFunction(IRFunction::HASH_CRC, false);
    }

    map<int, Function*>::iterator cached_fn = hash_fns_.find(num_bytes);
    if (cached_fn != hash_fns_.end()) {
      return cached_fn->second;
    }

    // Generate a function to hash these bytes
    stringstream ss;
    ss << "CrcHash" << num_bytes;
    FnPrototype prototype(this, ss.str(), GetType(TYPE_INT));
    prototype.AddArgument(LlvmCodeGen::NamedVariable("data", ptr_type()));
    prototype.AddArgument(LlvmCodeGen::NamedVariable("len", GetType(TYPE_INT)));
    prototype.AddArgument(LlvmCodeGen::NamedVariable("seed", GetType(TYPE_INT)));

    Value* args[3];
    LlvmBuilder builder(context());
    Function* fn = prototype.GeneratePrototype(&builder, &args[0]);
    Value* data = args[0];
    Value* result = args[2];

    Function* crc8_fn = llvm_intrinsics_[Intrinsic::x86_sse42_crc32_32_8];
    Function* crc16_fn = llvm_intrinsics_[Intrinsic::x86_sse42_crc32_32_16];
    Function* crc32_fn = llvm_intrinsics_[Intrinsic::x86_sse42_crc32_32_32];
    Function* crc64_fn = llvm_intrinsics_[Intrinsic::x86_sse42_crc32_64_64];

    // Generate the crc instructions starting with the highest number of bytes
    if (num_bytes >= 8) {
      Value* result_64 = builder.CreateZExt(result, GetType(TYPE_BIGINT));
      Value* ptr = builder.CreateBitCast(data, GetPtrType(TYPE_BIGINT));
      int i = 0;
      while (num_bytes >= 8) {
        Value* index[] = { GetIntConstant(TYPE_INT, i++) };
        Value* d = builder.CreateLoad(builder.CreateGEP(ptr, index));
        result_64 = builder.CreateCall(crc64_fn, ArrayRef<Value*>({result_64, d}));
        num_bytes -= 8;
      }
      result = builder.CreateTrunc(result_64, GetType(TYPE_INT));
      Value* index[] = { GetIntConstant(TYPE_INT, i * 8) };
      // Update data to past the 8-byte chunks
      data = builder.CreateGEP(data, index);
    }

    if (num_bytes >= 4) {
      DCHECK_LT(num_bytes, 8);
      Value* ptr = builder.CreateBitCast(data, GetPtrType(TYPE_INT));
      Value* d = builder.CreateLoad(ptr);
      result = builder.CreateCall(crc32_fn, ArrayRef<Value*>({result, d}));
      Value* index[] = { GetIntConstant(TYPE_INT, 4) };
      data = builder.CreateGEP(data, index);
      num_bytes -= 4;
    }

    if (num_bytes >= 2) {
      DCHECK_LT(num_bytes, 4);
      Value* ptr = builder.CreateBitCast(data, GetPtrType(TYPE_SMALLINT));
      Value* d = builder.CreateLoad(ptr);
      result = builder.CreateCall(crc16_fn, ArrayRef<Value*>({result, d}));
      Value* index[] = { GetIntConstant(TYPE_INT, 2) };
      data = builder.CreateGEP(data, index);
      num_bytes -= 2;
    }

    if (num_bytes > 0) {
      DCHECK_EQ(num_bytes, 1);
      Value* d = builder.CreateLoad(data);
      result = builder.CreateCall(crc8_fn, ArrayRef<Value*>({result, d}));
      --num_bytes;
    }
    DCHECK_EQ(num_bytes, 0);

    Value* shift_16 = GetIntConstant(TYPE_INT, 16);
    Value* upper_bits = builder.CreateShl(result, shift_16);
    Value* lower_bits = builder.CreateLShr(result, shift_16);
    result = builder.CreateOr(upper_bits, lower_bits);
    builder.CreateRet(result);

    fn = FinalizeFunction(fn);
    if (fn != NULL) {
      hash_fns_[num_bytes] = fn;
    }
    return fn;
  } else {
    return GetMurmurHashFunction(num_bytes);
  }
}

static Function* GetLenOptimizedHashFn(
    LlvmCodeGen* codegen, IRFunction::Type f, int len) {
  Function* fn = codegen->GetFunction(f, false);
  DCHECK(fn != NULL);
  if (len != -1) {
    // Clone this function since we're going to modify it by replacing the
    // length with num_bytes.
    fn = codegen->CloneFunction(fn);
    Value* len_arg = codegen->GetArgument(fn, 1);
    len_arg->replaceAllUsesWith(codegen->GetIntConstant(TYPE_INT, len));
  }
  return codegen->FinalizeFunction(fn);
}

Function* LlvmCodeGen::GetFnvHashFunction(int len) {
  return GetLenOptimizedHashFn(this, IRFunction::HASH_FNV, len);
}

Function* LlvmCodeGen::GetMurmurHashFunction(int len) {
  return GetLenOptimizedHashFn(this, IRFunction::HASH_MURMUR, len);
}

void LlvmCodeGen::ReplaceInstWithValue(Instruction* from, Value* to) {
  BasicBlock::iterator iter(from);
  llvm::ReplaceInstWithValue(from->getParent()->getInstList(), iter, to);
}

Argument* LlvmCodeGen::GetArgument(Function* fn, int i) {
  DCHECK_LE(i, fn->arg_size());
  Function::arg_iterator iter = fn->arg_begin();
  for (int j = 0; j < i; ++j) ++iter;
  return &*iter;
}

Value* LlvmCodeGen::GetPtrTo(LlvmBuilder* builder, Value* v, const char* name) {
  Value* ptr = CreateEntryBlockAlloca(*builder, v->getType(), name);
  builder->CreateStore(v, ptr);
  return ptr;
}

}
