// Copyright 2016 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/asmjs/asm-typer.h"

#include <limits>
#include <string>

#include "src/v8.h"

#include "src/asmjs/asm-types.h"
#include "src/ast/ast.h"
#include "src/ast/scopes.h"
#include "src/base/bits.h"
#include "src/codegen.h"
#include "src/globals.h"
#include "src/type-cache.h"
#include "src/utils.h"

#define FAIL(node, msg)                                        \
  do {                                                         \
    int line = node->position() == kNoSourcePosition           \
                   ? -1                                        \
                   : script_->GetLineNumber(node->position()); \
    base::OS::SNPrintF(error_message_, sizeof(error_message_), \
                       "asm: line %d: %s\n", line + 1, msg);   \
    return AsmType::None();                                    \
  } while (false)

#define RECURSE(call)                                             \
  do {                                                            \
    if (GetCurrentStackPosition() < stack_limit_) {               \
      stack_overflow_ = true;                                     \
      FAIL(root_, "Stack overflow while parsing asm.js module."); \
    }                                                             \
                                                                  \
    AsmType* result = (call);                                     \
    if (stack_overflow_) {                                        \
      return AsmType::None();                                     \
    }                                                             \
                                                                  \
    if (result == AsmType::None()) {                              \
      return AsmType::None();                                     \
    }                                                             \
  } while (false)

namespace v8 {
namespace internal {
namespace wasm {

using v8::internal::AstNode;
using v8::internal::GetCurrentStackPosition;

// ----------------------------------------------------------------------------
// Implementation of AsmTyper::FlattenedStatements

AsmTyper::FlattenedStatements::FlattenedStatements(Zone* zone,
                                                   ZoneList<Statement*>* s)
    : context_stack_(zone) {
  context_stack_.emplace_back(Context(s));
}

Statement* AsmTyper::FlattenedStatements::Next() {
  for (;;) {
    if (context_stack_.empty()) {
      return nullptr;
    }

    Context* current = &context_stack_.back();

    if (current->statements_->length() <= current->next_index_) {
      context_stack_.pop_back();
      continue;
    }

    Statement* current_statement =
        current->statements_->at(current->next_index_++);
    if (current_statement->IsBlock()) {
      context_stack_.emplace_back(
          Context(current_statement->AsBlock()->statements()));
      continue;
    }

    return current_statement;
  }
}

// ----------------------------------------------------------------------------
// Implementation of AsmTyper::VariableInfo

AsmTyper::VariableInfo* AsmTyper::VariableInfo::Clone(Zone* zone) const {
  CHECK(standard_member_ != kNone);
  auto* new_var_info = new (zone) VariableInfo(type_);
  new_var_info->standard_member_ = standard_member_;
  new_var_info->mutability_ = mutability_;
  return new_var_info;
}

void AsmTyper::VariableInfo::FirstForwardUseIs(VariableProxy* var) {
  DCHECK(first_forward_use_ == nullptr);
  missing_definition_ = true;
  first_forward_use_ = var;
}

// ----------------------------------------------------------------------------
// Implementation of AsmTyper

AsmTyper::AsmTyper(Isolate* isolate, Zone* zone, Script* script,
                   FunctionLiteral* root)
    : isolate_(isolate),
      zone_(zone),
      script_(script),
      root_(root),
      forward_definitions_(zone),
      stdlib_types_(zone),
      stdlib_math_types_(zone),
      global_scope_(ZoneHashMap::PointersMatch,
                    ZoneHashMap::kDefaultHashMapCapacity,
                    ZoneAllocationPolicy(zone)),
      local_scope_(ZoneHashMap::PointersMatch,
                   ZoneHashMap::kDefaultHashMapCapacity,
                   ZoneAllocationPolicy(zone)),
      stack_limit_(isolate->stack_guard()->real_climit()),
      node_types_(zone_) {
  InitializeStdlib();
  module_info_.set_standard_member(kModule);
}

namespace {
bool ValidAsmIdentifier(Handle<String> name) {
  static const char* kInvalidAsmNames[] = {"eval", "arguments"};

  for (size_t ii = 0; ii < arraysize(kInvalidAsmNames); ++ii) {
    if (strcmp(name->ToCString().get(), kInvalidAsmNames[ii]) == 0) {
      return false;
    }
  }
  return true;
}
}  // namespace

void AsmTyper::InitializeStdlib() {
  auto* d = AsmType::Double();
  auto* dq = AsmType::DoubleQ();
  auto* dq2d = AsmType::Function(zone_, d);
  dq2d->AsFunctionType()->AddArgument(dq);

  auto* dqdq2d = AsmType::Function(zone_, d);
  dqdq2d->AsFunctionType()->AddArgument(dq);
  dqdq2d->AsFunctionType()->AddArgument(dq);

  auto* f = AsmType::Float();
  auto* fq = AsmType::FloatQ();
  auto* fq2f = AsmType::Function(zone_, f);
  fq2f->AsFunctionType()->AddArgument(fq);

  auto* s = AsmType::Signed();
  auto* s2s = AsmType::Function(zone_, s);
  s2s->AsFunctionType()->AddArgument(s);

  auto* i = AsmType::Int();
  auto* ii2s = AsmType::Function(zone_, s);
  ii2s->AsFunctionType()->AddArgument(i);
  ii2s->AsFunctionType()->AddArgument(i);

  auto* minmax_d = AsmType::MinMaxType(zone_, d, d);
  auto* minmax_i = AsmType::MinMaxType(zone_, s, i);
  auto* minmax = AsmType::OverloadedFunction(zone_);
  minmax->AsOverloadedFunctionType()->AddOverload(minmax_i);
  minmax->AsOverloadedFunctionType()->AddOverload(minmax_d);

  auto* fround = AsmType::FroundType(zone_);

  auto* abs = AsmType::OverloadedFunction(zone_);
  abs->AsOverloadedFunctionType()->AddOverload(s2s);
  abs->AsOverloadedFunctionType()->AddOverload(dq2d);
  abs->AsOverloadedFunctionType()->AddOverload(fq2f);

  auto* ceil = AsmType::OverloadedFunction(zone_);
  ceil->AsOverloadedFunctionType()->AddOverload(dq2d);
  ceil->AsOverloadedFunctionType()->AddOverload(fq2f);

  auto* floor = ceil;
  auto* sqrt = ceil;

  struct StandardMemberInitializer {
    const char* name;
    StandardMember standard_member;
    AsmType* type;
  };

  const StandardMemberInitializer stdlib[] = {{"Infinity", kInfinity, d},
                                              {"NaN", kNaN, d},
#define asm_TYPED_ARRAYS(V) \
  V(Uint8)                  \
  V(Int8)                   \
  V(Uint16)                 \
  V(Int16)                  \
  V(Uint32)                 \
  V(Int32)                  \
  V(Float32)                \
  V(Float64)

#define asm_TYPED_ARRAY(TypeName) \
  {#TypeName "Array", kNone, AsmType::TypeName##Array()},
                                              asm_TYPED_ARRAYS(asm_TYPED_ARRAY)
#undef asm_TYPED_ARRAY
  };
  for (size_t ii = 0; ii < arraysize(stdlib); ++ii) {
    stdlib_types_[stdlib[ii].name] = new (zone_) VariableInfo(stdlib[ii].type);
    stdlib_types_[stdlib[ii].name]->set_standard_member(
        stdlib[ii].standard_member);
    stdlib_types_[stdlib[ii].name]->set_mutability(
        VariableInfo::kImmutableGlobal);
  }

  const StandardMemberInitializer math[] = {
      {"PI", kMathPI, d},
      {"E", kMathE, d},
      {"LN2", kMathLN2, d},
      {"LN10", kMathLN10, d},
      {"LOG2E", kMathLOG2E, d},
      {"LOG10E", kMathLOG10E, d},
      {"SQRT2", kMathSQRT2, d},
      {"SQRT1_2", kMathSQRT1_2, d},
      {"imul", kMathImul, ii2s},
      {"abs", kMathAbs, abs},
      {"ceil", kMathCeil, ceil},
      {"floor", kMathFloor, floor},
      {"fround", kMathFround, fround},
      {"pow", kMathPow, dqdq2d},
      {"exp", kMathExp, dq2d},
      {"log", kMathLog, dq2d},
      {"min", kMathMin, minmax},
      {"max", kMathMax, minmax},
      {"sqrt", kMathSqrt, sqrt},
      {"cos", kMathCos, dq2d},
      {"sin", kMathSin, dq2d},
      {"tan", kMathTan, dq2d},
      {"acos", kMathAcos, dq2d},
      {"asin", kMathAsin, dq2d},
      {"atan", kMathAtan, dq2d},
      {"atan2", kMathAtan2, dqdq2d},
  };
  for (size_t ii = 0; ii < arraysize(math); ++ii) {
    stdlib_math_types_[math[ii].name] = new (zone_) VariableInfo(math[ii].type);
    stdlib_math_types_[math[ii].name]->set_standard_member(
        math[ii].standard_member);
    stdlib_math_types_[math[ii].name]->set_mutability(
        VariableInfo::kImmutableGlobal);
  }
}

// Used for 5.5 GlobalVariableTypeAnnotations
AsmTyper::VariableInfo* AsmTyper::ImportLookup(Property* import) {
  auto* obj = import->obj();
  auto* key = import->key()->AsLiteral();

  ObjectTypeMap* stdlib = &stdlib_types_;
  if (auto* obj_as_property = obj->AsProperty()) {
    // This can only be stdlib.Math
    auto* math_name = obj_as_property->key()->AsLiteral();
    if (math_name == nullptr || !math_name->IsPropertyName()) {
      return nullptr;
    }

    if (!math_name->AsPropertyName()->IsUtf8EqualTo(CStrVector("Math"))) {
      return nullptr;
    }

    auto* stdlib_var_proxy = obj_as_property->obj()->AsVariableProxy();
    if (stdlib_var_proxy == nullptr) {
      return nullptr;
    }
    obj = stdlib_var_proxy;
    stdlib = &stdlib_math_types_;
  }

  auto* obj_as_var_proxy = obj->AsVariableProxy();
  if (obj_as_var_proxy == nullptr) {
    return nullptr;
  }

  auto* obj_info = Lookup(obj_as_var_proxy->var());
  if (obj_info == nullptr) {
    return nullptr;
  }

  if (obj_info->IsFFI()) {
    // For FFI we can't validate import->key, so assume this is OK.
    return obj_info;
  }

  base::SmartArrayPointer<char> aname = key->AsPropertyName()->ToCString();
  ObjectTypeMap::iterator i = stdlib->find(std::string(aname.get()));
  if (i == stdlib->end()) {
    return nullptr;
  }
  return i->second;
}

AsmTyper::VariableInfo* AsmTyper::Lookup(Variable* variable) {
  ZoneHashMap* scope = in_function_ ? &local_scope_ : &global_scope_;
  ZoneHashMap::Entry* entry =
      scope->Lookup(variable, ComputePointerHash(variable));
  if (entry == nullptr && in_function_) {
    entry = global_scope_.Lookup(variable, ComputePointerHash(variable));
  }

  if (entry == nullptr && !module_name_.is_null() &&
      module_name_->Equals(*variable->name())) {
    return &module_info_;
  }

  return entry ? reinterpret_cast<VariableInfo*>(entry->value) : nullptr;
}

void AsmTyper::AddForwardReference(VariableProxy* proxy, VariableInfo* info) {
  info->FirstForwardUseIs(proxy);
  forward_definitions_.push_back(info);
}

bool AsmTyper::AddGlobal(Variable* variable, VariableInfo* info) {
  // We can't DCHECK(!in_function_) because function may actually install global
  // names (forward defined functions and function tables.)
  DCHECK(info->mutability() != VariableInfo::kInvalidMutability);
  DCHECK(info->IsGlobal());
  DCHECK(ValidAsmIdentifier(variable->name()));

  if (!module_name_.is_null() && module_name_->Equals(*variable->name())) {
    return false;
  }

  ZoneHashMap::Entry* entry = global_scope_.LookupOrInsert(
      variable, ComputePointerHash(variable), ZoneAllocationPolicy(zone_));

  if (entry->value != nullptr) {
    return false;
  }

  entry->value = info;
  return true;
}

bool AsmTyper::AddLocal(Variable* variable, VariableInfo* info) {
  DCHECK(in_function_);
  DCHECK(info->mutability() != VariableInfo::kInvalidMutability);
  DCHECK(!info->IsGlobal());
  DCHECK(ValidAsmIdentifier(variable->name()));

  ZoneHashMap::Entry* entry = local_scope_.LookupOrInsert(
      variable, ComputePointerHash(variable), ZoneAllocationPolicy(zone_));

  if (entry->value != nullptr) {
    return false;
  }

  entry->value = info;
  return true;
}

void AsmTyper::SetTypeOf(AstNode* node, AsmType* type) {
  DCHECK_NE(type, AsmType::None());
  auto** node_type = &node_types_[node];
  DCHECK(*node_type == nullptr);
  *node_type = type;
}

AsmType* AsmTyper::TypeOf(AstNode* node) const {
  auto node_type_iter = node_types_.find(node);
  if (node_type_iter == node_types_.end()) {
    return AsmType::None();
  }
  return node_type_iter->second;
}

AsmTyper::StandardMember AsmTyper::VariableAsStandardMember(Variable* var) {
  auto* var_info = Lookup(var);
  if (var_info == nullptr) {
    return kNone;
  }
  return var_info->standard_member();
}

bool AsmTyper::Validate() {
  if (!AsmType::None()->IsExactly(ValidateModule(root_))) {
    return true;
  }
  return false;
}

namespace {
bool IsUseAsmDirective(Statement* first_statement) {
  ExpressionStatement* use_asm = first_statement->AsExpressionStatement();
  if (use_asm == nullptr) {
    return false;
  }

  Literal* use_asm_literal = use_asm->expression()->AsLiteral();

  if (use_asm_literal == nullptr) {
    return false;
  }

  return use_asm_literal->raw_value()->AsString()->IsOneByteEqualTo("use asm");
}

Assignment* ExtractInitializerExpression(Statement* statement) {
  auto* expr_stmt = statement->AsExpressionStatement();
  if (expr_stmt == nullptr) {
    // Done with initializers.
    return nullptr;
  }
  auto* assign = expr_stmt->expression()->AsAssignment();
  if (assign == nullptr) {
    // Done with initializers.
    return nullptr;
  }
  if (assign->op() != Token::INIT) {
    // Done with initializers.
    return nullptr;
  }
  return assign;
}

}  // namespace

// 6.1 ValidateModule
AsmType* AsmTyper::ValidateModule(FunctionLiteral* fun) {
  Scope* scope = fun->scope();
  if (!scope->is_function_scope()) FAIL(fun, "Not at function scope.");
  if (!ValidAsmIdentifier(fun->name()))
    FAIL(fun, "Invalid asm.js identifier in module name.");
  module_name_ = fun->name();

  // Allowed parameters: Stdlib, FFI, Mem
  static const uint32_t MaxModuleParameters = 3;
  if (scope->num_parameters() > MaxModuleParameters) {
    FAIL(fun, "asm.js modules may not have more than three parameters.");
  }

  struct {
    StandardMember standard_member;
    AsmType* type;
  } kModuleParamInfo[3] = {
      {kStdlib, AsmType::None()},
      {kFFI, AsmType::FFIType(zone_)},
      {kHeap, AsmType::None()},
  };

  for (int ii = 0; ii < scope->num_parameters(); ++ii) {
    Variable* param = scope->parameter(ii);
    DCHECK(param);

    if (!ValidAsmIdentifier(param->name())) {
      FAIL(fun, "Invalid asm.js identifier in module parameter.");
    }

    auto* param_info = new (zone_) VariableInfo();
    param_info->set_mutability(VariableInfo::kImmutableGlobal);
    param_info->set_standard_member(kModuleParamInfo[ii].standard_member);
    param_info->set_type(kModuleParamInfo[ii].type);

    if (!AddGlobal(param, param_info)) {
      FAIL(fun, "Redeclared identifier in module parameter.");
    }
  }

  ZoneVector<Assignment*> function_pointer_tables(zone_);
  FlattenedStatements iter(zone_, fun->body());
  auto* use_asm_directive = iter.Next();
  if (use_asm_directive == nullptr || !IsUseAsmDirective(use_asm_directive)) {
    FAIL(fun, "Missing \"use asm\".");
  }
  ReturnStatement* module_return = nullptr;

  // *VIOLATION* The spec states that globals should be followed by function
  // declarations, which should be followed by function pointer tables, followed
  // by the module export (return) statement. Our AST might be rearraged by the
  // parser, so we can't rely on it being in source code order.
  while (Statement* current = iter.Next()) {
    if (auto* assign = ExtractInitializerExpression(current)) {
      if (assign->value()->IsArrayLiteral()) {
        // Save function tables for later validation.
        function_pointer_tables.push_back(assign);
      } else {
        RECURSE(ValidateGlobalDeclaration(assign));
      }
      continue;
    }

    if (auto* current_as_return = current->AsReturnStatement()) {
      if (module_return != nullptr) {
        FAIL(fun, "Multiple export statements.");
      }
      module_return = current_as_return;
      continue;
    }

    FAIL(current, "Invalid top-level statement in asm.js module.");
  }

  ZoneList<Declaration*>* decls = scope->declarations();

  for (int ii = 0; ii < decls->length(); ++ii) {
    Declaration* decl = decls->at(ii);

    if (FunctionDeclaration* fun_decl = decl->AsFunctionDeclaration()) {
      RECURSE(ValidateFunction(fun_decl));
      continue;
    }
  }

  for (auto* function_table : function_pointer_tables) {
    RECURSE(ValidateFunctionTable(function_table));
  }

  for (int ii = 0; ii < decls->length(); ++ii) {
    Declaration* decl = decls->at(ii);

    if (decl->IsFunctionDeclaration()) {
      continue;
    }

    VariableDeclaration* var_decl = decl->AsVariableDeclaration();
    if (var_decl == nullptr) {
      FAIL(decl, "Invalid asm.js declaration.");
    }

    auto* var_proxy = var_decl->proxy();
    if (var_proxy == nullptr) {
      FAIL(decl, "Invalid asm.js declaration.");
    }

    if (Lookup(var_proxy->var()) == nullptr) {
      FAIL(decl, "Global variable missing initializer in asm.js module.");
    }
  }

  // 6.2 ValidateExport
  if (module_return == nullptr) {
    FAIL(fun, "Missing asm.js module export.");
  }

  for (auto* forward_def : forward_definitions_) {
    if (forward_def->missing_definition()) {
      FAIL(forward_def->first_forward_use(),
           "Missing definition for forward declared identifier.");
    }
  }

  RECURSE(ValidateExport(module_return));

  return AsmType::Int();  // Any type that is not AsmType::None();
}

namespace {
bool IsDoubleAnnotation(BinaryOperation* binop) {
  // *VIOLATION* The parser replaces uses of +x with x*1.0.
  if (binop->op() != Token::MUL) {
    return false;
  }

  auto* right_as_literal = binop->right()->AsLiteral();
  if (right_as_literal == nullptr) {
    return false;
  }

  return right_as_literal->raw_value()->ContainsDot() &&
         right_as_literal->raw_value()->AsNumber() == 1.0;
}

bool IsIntAnnotation(BinaryOperation* binop) {
  if (binop->op() != Token::BIT_OR) {
    return false;
  }

  auto* right_as_literal = binop->right()->AsLiteral();
  if (right_as_literal == nullptr) {
    return false;
  }

  return !right_as_literal->raw_value()->ContainsDot() &&
         right_as_literal->raw_value()->AsNumber() == 0.0;
}
}  // namespace

AsmType* AsmTyper::ValidateGlobalDeclaration(Assignment* assign) {
  DCHECK(!assign->is_compound());
  if (assign->is_compound()) {
    FAIL(assign,
         "Compound assignment not supported when declaring global variables.");
  }

  auto* target = assign->target();
  if (!target->IsVariableProxy()) {
    FAIL(target, "Module assignments may only assign to globals.");
  }
  auto* target_variable = target->AsVariableProxy()->var();
  auto* target_info = Lookup(target_variable);

  if (target_info != nullptr) {
    FAIL(target, "Redefined global variable.");
  }

  auto* value = assign->value();
  // Not all types of assignment are allowed by asm.js. See
  // 5.5 Global Variable Type Annotations.
  if (value->IsLiteral() || value->IsCall()) {
    AsmType* type = nullptr;
    RECURSE(type = VariableTypeAnnotations(value));
    target_info = new (zone_) VariableInfo(type);
    target_info->set_mutability(VariableInfo::kMutableGlobal);
  } else if (value->IsProperty()) {
    target_info = ImportLookup(value->AsProperty());
    if (target_info == nullptr) {
      FAIL(assign, "Invalid import.");
    }
    CHECK(target_info->mutability() == VariableInfo::kImmutableGlobal);
    if (!target_info->IsFFI()) {
      target_info = target_info->Clone(zone_);
    } else {
      // create a new target info that represents a foreign variable.
      DCHECK(target_info->type()->AsFFIType() != nullptr);
      target_info = new (zone_) VariableInfo(target_info->type());
      target_info->set_mutability(VariableInfo::kImmutableGlobal);
    }
  } else if (value->IsBinaryOperation()) {
    // This should either be:
    //
    // var <> = ffi.<>|0
    //
    // or
    //
    // var <> = +ffi.<>
    auto* value_binop = value->AsBinaryOperation();
    auto* left = value_binop->left();
    AsmType* import_type = nullptr;

    if (IsDoubleAnnotation(value_binop)) {
      import_type = AsmType::Double();
    } else if (IsIntAnnotation(value_binop)) {
      import_type = AsmType::Int();
    } else {
      FAIL(value,
           "Invalid initializer for foreign import - unrecognized annotation.");
    }

    if (!left->IsProperty()) {
      FAIL(value,
           "Invalid initializer for foreign import - must import member.");
    }
    target_info = ImportLookup(left->AsProperty());
    if (target_info == nullptr) {
      // TODO(jpp): this error message is innacurate: this may fail if the
      // object lookup fails, or if the property lookup fails, or even if the
      // import is bogus like a().c.
      FAIL(value,
           "Invalid initializer for foreign import - object lookup failed.");
    }
    CHECK(target_info->mutability() == VariableInfo::kImmutableGlobal);
    if (!target_info->IsFFI()) {
      FAIL(value,
           "Invalid initializer for foreign import - object is not the ffi.");
    }

    // Create a new target info that represents the foreign import.
    DCHECK(target_info->type()->AsFFIType() != nullptr);
    target_info = new (zone_) VariableInfo(import_type);
    target_info->set_mutability(VariableInfo::kMutableGlobal);
  } else if (value->IsCallNew()) {
    AsmType* type = nullptr;
    RECURSE(type = NewHeapView(value->AsCallNew()));
    target_info = new (zone_) VariableInfo(type);
    target_info->set_mutability(VariableInfo::kImmutableGlobal);
  }

  if (target_info == nullptr) {
    FAIL(assign, "Invalid global variable initializer.");
  }

  if (!ValidAsmIdentifier(target_variable->name())) {
    FAIL(target, "Invalid asm.js identifier in global variable.");
  }

  if (!AddGlobal(target_variable, target_info)) {
    FAIL(assign, "Redeclared global identifier.");
  }

  DCHECK(target_info->type() != AsmType::None());
  return target_info->type();
}

// 6.2 ValidateExport
AsmType* AsmTyper::ExportType(VariableProxy* fun_export) {
  auto* fun_info = Lookup(fun_export->var());
  if (fun_info == nullptr) {
    FAIL(fun_export, "Undefined identifier in asm.js module export.");
  }

  if (fun_info->standard_member() != kNone) {
    FAIL(fun_export, "Module cannot export standard library functions.");
  }

  auto* type = fun_info->type();
  if (type->AsFFIType() != nullptr) {
    FAIL(fun_export, "Module cannot export foreign functions.");
  }

  if (type->AsFunctionTableType() != nullptr) {
    FAIL(fun_export, "Module cannot export function tables.");
  }

  if (fun_info->type()->AsFunctionType() == nullptr) {
    FAIL(fun_export, "Module export is not an asm.js function.");
  }

  return type;
}

AsmType* AsmTyper::ValidateExport(ReturnStatement* exports) {
  // asm.js modules can export single functions, or multiple functions in an
  // object literal.
  if (auto* fun_export = exports->expression()->AsVariableProxy()) {
    // Exporting single function.
    AsmType* export_type;
    RECURSE(export_type = ExportType(fun_export));
    return export_type;
  }

  if (auto* obj_export = exports->expression()->AsObjectLiteral()) {
    // Exporting object literal.
    for (auto* prop : *obj_export->properties()) {
      if (!prop->key()->IsLiteral()) {
        FAIL(prop->key(),
             "Only normal object properties may be used in the export object "
             "literal.");
      }

      auto* export_obj = prop->value()->AsVariableProxy();
      if (export_obj == nullptr) {
        FAIL(prop->value(), "Exported value must be an asm.js function name.");
      }

      RECURSE(ExportType(export_obj));
    }

    return AsmType::Int();
  }

  FAIL(exports, "Unrecognized expression in asm.js module export expression.");
}

// 6.3 ValidateFunctionTable
AsmType* AsmTyper::ValidateFunctionTable(Assignment* assign) {
  if (assign->is_compound()) {
    FAIL(assign,
         "Compound assignment not supported when declaring global variables.");
  }

  auto* target = assign->target();
  if (!target->IsVariableProxy()) {
    FAIL(target, "Module assignments may only assign to globals.");
  }
  auto* target_variable = target->AsVariableProxy()->var();

  auto* value = assign->value()->AsArrayLiteral();
  CHECK(value != nullptr);
  ZoneList<Expression*>* pointers = value->values();

  // The function table size must be n = 2 ** m, for m >= 0;
  // TODO(jpp): should this be capped?
  if (!base::bits::IsPowerOfTwo32(pointers->length())) {
    FAIL(assign, "Invalid length for function pointer table.");
  }

  AsmType* table_element_type = nullptr;
  AsmCallableType* callable_type = nullptr;
  for (auto* initializer : *pointers) {
    auto* var_proxy = initializer->AsVariableProxy();
    if (var_proxy == nullptr) {
      FAIL(initializer,
           "Function pointer table initializer must be a function name.");
    }

    auto* var_info = Lookup(var_proxy->var());
    if (var_info == nullptr) {
      FAIL(var_proxy,
           "Undefined identifier in function pointer table initializer.");
    }

    if (var_info->standard_member() != kNone) {
      FAIL(initializer,
           "Function pointer table must not be a member of the standard "
           "library.");
    }

    auto* initializer_callable = var_info->type()->AsFunctionType();
    if (initializer_callable == nullptr) {
      FAIL(initializer,
           "Function pointer table initializer must be an asm.js function.");
    }

    DCHECK(var_info->type()->AsFFIType() == nullptr);
    DCHECK(var_info->type()->AsFunctionTableType() == nullptr);

    if (callable_type == nullptr) {
      table_element_type = var_info->type();
      callable_type = initializer_callable;
    } else if (callable_type->ValidateCall(initializer_callable->ReturnType(),
                                           initializer_callable->Arguments()) ==
               AsmType::None()) {
      FAIL(initializer, "Type mismatch in function pointer table initializer.");
    }
  }

  auto* target_info = Lookup(target_variable);
  if (target_info == nullptr) {
    // Function pointer tables are the last entities to be validates, so this is
    // unlikely to happen: only unreferenced function tables will not already
    // have an entry in the global scope.
    target_info = new (zone_) VariableInfo(AsmType::FunctionTableType(
        zone_, pointers->length(), table_element_type));
    target_info->set_mutability(VariableInfo::kImmutableGlobal);
    if (!ValidAsmIdentifier(target_variable->name())) {
      FAIL(target, "Invalid asm.js identifier in function table name.");
    }
    if (!AddGlobal(target_variable, target_info)) {
      DCHECK(false);
      FAIL(assign, "Redeclared global identifier in function table name.");
    }
    return target_info->type();
  }

  auto* target_info_table = target_info->type()->AsFunctionTableType();
  if (target_info_table == nullptr) {
    FAIL(assign, "Identifier redefined as function pointer table.");
  }

  if (!target_info->missing_definition()) {
    FAIL(assign, "Identifier redefined (function table name).");
  }

  if (target_info_table->length() != pointers->length()) {
    FAIL(assign, "Function table size mismatch.");
  }

  auto* function_type = callable_type->AsFunctionType();
  if (target_info_table->ValidateCall(function_type->ReturnType(),
                                      function_type->Arguments()) ==
      AsmType::None()) {
    FAIL(assign, "Function table initializer does not match previous type.");
  }

  target_info->MarkDefined();
  DCHECK(target_info->type() == AsmType::None());
  SetTypeOf(value, target_info->type());

  return target_info->type();
}

// 6.4 ValidateFunction
AsmType* AsmTyper::ValidateFunction(FunctionDeclaration* fun_decl) {
  FunctionScope _(this);

  // Extract parameter types.
  auto* fun = fun_decl->fun();

  auto* fun_decl_proxy = fun_decl->proxy();
  if (fun_decl_proxy == nullptr) {
    FAIL(fun_decl, "Anonymous functions are not support in asm.js.");
  }

  Statement* current;
  FlattenedStatements iter(zone_, fun->body());

  size_t annotated_parameters = 0;

  // 5.3 Function type annotations
  //     * parameters
  ZoneVector<AsmType*> parameter_types(zone_);
  for (; (current = iter.Next()) != nullptr; ++annotated_parameters) {
    auto* stmt = current->AsExpressionStatement();
    if (stmt == nullptr) {
      // Done with parameters.
      break;
    }
    auto* expr = stmt->expression()->AsAssignment();
    if (expr == nullptr || expr->is_compound()) {
      // Done with parameters.
      break;
    }
    auto* proxy = expr->target()->AsVariableProxy();
    if (proxy == nullptr) {
      // Done with parameters.
      break;
    }
    auto* param = proxy->var();
    if (param->location() != VariableLocation::PARAMETER ||
        param->index() != annotated_parameters) {
      // Done with parameters.
      break;
    }

    AsmType* type;
    RECURSE(type = ParameterTypeAnnotations(param, expr->value()));
    DCHECK(type->IsParameterType());
    auto* param_info = new (zone_) VariableInfo(type);
    param_info->set_mutability(VariableInfo::kLocal);
    if (!ValidAsmIdentifier(proxy->name())) {
      FAIL(proxy, "Invalid asm.js identifier in parameter name.");
    }

    if (!AddLocal(param, param_info)) {
      FAIL(proxy, "Redeclared parameter.");
    }
    parameter_types.push_back(type);
  }

  if (annotated_parameters != fun->parameter_count()) {
    FAIL(fun_decl, "Incorrect parameter type annotations.");
  }

  // 5.3 Function type annotations
  //     * locals
  for (; current; current = iter.Next()) {
    auto* initializer = ExtractInitializerExpression(current);
    if (initializer == nullptr) {
      // Done with locals.
      break;
    }

    auto* local = initializer->target()->AsVariableProxy();
    if (local == nullptr) {
      // Done with locals. It should never happen. Even if it does, the asm.js
      // code should not declare any other locals after this point, so we assume
      // this is OK. If any other variable declaration is found we report a
      // validation error.
      DCHECK(false);
      break;
    }

    AsmType* type;
    RECURSE(type = VariableTypeAnnotations(initializer->value()));
    auto* local_info = new (zone_) VariableInfo(type);
    local_info->set_mutability(VariableInfo::kLocal);
    if (!ValidAsmIdentifier(local->name())) {
      FAIL(local, "Invalid asm.js identifier in local variable.");
    }

    if (!AddLocal(local->var(), local_info)) {
      FAIL(initializer, "Redeclared local.");
    }
  }

  // 5.2 Return Type Annotations
  // *VIOLATION* we peel blocks to find the last statement in the asm module
  // because the parser may introduce synthetic blocks.
  ZoneList<Statement*>* statements = fun->body();

  do {
    if (statements->length() == 0) {
      return_type_ = AsmType::Void();
    } else {
      auto* last_statement = statements->last();
      auto* as_block = last_statement->AsBlock();
      if (as_block != nullptr) {
        statements = as_block->statements();
      } else {
        // We don't check whether AsReturnStatement() below returns non-null --
        // we leave that to the ReturnTypeAnnotations method.
        RECURSE(return_type_ =
                    ReturnTypeAnnotations(last_statement->AsReturnStatement()));
      }
    }
  } while (return_type_ == AsmType::None());

  DCHECK(return_type_->IsReturnType());

  for (auto* decl : *fun->scope()->declarations()) {
    auto* var_decl = decl->AsVariableDeclaration();
    if (var_decl == nullptr) {
      FAIL(decl, "Functions may only define inner variables.");
    }

    auto* var_proxy = var_decl->proxy();
    if (var_proxy == nullptr) {
      FAIL(decl, "Invalid local declaration declaration.");
    }

    auto* var_info = Lookup(var_proxy->var());
    if (var_info == nullptr || var_info->IsGlobal()) {
      FAIL(decl, "Local variable missing initializer in asm.js module.");
    }
  }

  for (; current; current = iter.Next()) {
    AsmType* current_type;
    RECURSE(current_type = ValidateStatement(current));
  }

  auto* fun_type = AsmType::Function(zone_, return_type_);
  auto* fun_type_as_function = fun_type->AsFunctionType();
  for (auto* param_type : parameter_types) {
    fun_type_as_function->AddArgument(param_type);
  }

  auto* fun_var = fun_decl_proxy->var();
  auto* fun_info = new (zone_) VariableInfo(fun_type);
  fun_info->set_mutability(VariableInfo::kImmutableGlobal);
  auto* old_fun_type = Lookup(fun_var);
  if (old_fun_type == nullptr) {
    if (!ValidAsmIdentifier(fun_var->name())) {
      FAIL(fun_decl_proxy, "Invalid asm.js identifier in function name.");
    }
    if (!AddGlobal(fun_var, fun_info)) {
      DCHECK(false);
      FAIL(fun_decl, "Redeclared global identifier.");
    }
    return fun_type;
  }

  // Not necessarily an error -- fun_decl might have been used before being
  // defined. If that's the case, then the type in the global environment must
  // be the same as the type inferred by the parameter/return type annotations.
  auto* old_fun_callable = old_fun_type->type()->AsCallableType();
  if (old_fun_callable == nullptr) {
    FAIL(fun_decl, "Identifier redefined as function.");
  }

  if (!old_fun_type->missing_definition()) {
    FAIL(fun_decl, "Identifier redefined (function name).");
  }

  if (old_fun_callable->ValidateCall(fun_type_as_function->ReturnType(),
                                     fun_type_as_function->Arguments()) ==
      AsmType::None()) {
    FAIL(fun_decl, "Signature mismatch when defining function.");
  }

  old_fun_type->MarkDefined();
  SetTypeOf(fun, fun_type);

  return fun_type;
}

// 6.5 ValidateStatement
AsmType* AsmTyper::ValidateStatement(Statement* statement) {
  switch (statement->node_type()) {
    default:
      FAIL(statement, "Statement type invalid for asm.js.");
    case AstNode::kBlock:
      return ValidateBlockStatement(statement->AsBlock());
    case AstNode::kExpressionStatement:
      return ValidateExpressionStatement(statement->AsExpressionStatement());
    case AstNode::kEmptyStatement:
      return ValidateEmptyStatement(statement->AsEmptyStatement());
    case AstNode::kIfStatement:
      return ValidateIfStatement(statement->AsIfStatement());
    case AstNode::kReturnStatement:
      return ValidateReturnStatement(statement->AsReturnStatement());
    case AstNode::kWhileStatement:
      return ValidateWhileStatement(statement->AsWhileStatement());
    case AstNode::kDoWhileStatement:
      return ValidateDoWhileStatement(statement->AsDoWhileStatement());
    case AstNode::kForStatement:
      return ValidateForStatement(statement->AsForStatement());
    case AstNode::kBreakStatement:
      return ValidateBreakStatement(statement->AsBreakStatement());
    case AstNode::kContinueStatement:
      return ValidateContinueStatement(statement->AsContinueStatement());
    case AstNode::kSwitchStatement:
      return ValidateSwitchStatement(statement->AsSwitchStatement());
  }

  return AsmType::Void();
}

// 6.5.1 BlockStatement
AsmType* AsmTyper::ValidateBlockStatement(Block* block) {
  FlattenedStatements iter(zone_, block->statements());

  while (auto* current = iter.Next()) {
    RECURSE(ValidateStatement(current));
  }

  return AsmType::Void();
}

// 6.5.2 ExpressionStatement
AsmType* AsmTyper::ValidateExpressionStatement(ExpressionStatement* expr) {
  auto* expression = expr->expression();
  if (auto* call = expression->AsCall()) {
    RECURSE(ValidateCall(AsmType::Void(), call));
  } else {
    RECURSE(ValidateExpression(expression));
  }

  return AsmType::Void();
}

// 6.5.3 EmptyStatement
AsmType* AsmTyper::ValidateEmptyStatement(EmptyStatement* empty) {
  return AsmType::Void();
}

// 6.5.4 IfStatement
AsmType* AsmTyper::ValidateIfStatement(IfStatement* if_stmt) {
  AsmType* cond_type;
  RECURSE(cond_type = ValidateExpression(if_stmt->condition()));
  if (!cond_type->IsA(AsmType::Int())) {
    FAIL(if_stmt->condition(), "If condition must be type int.");
  }
  RECURSE(ValidateStatement(if_stmt->then_statement()));
  RECURSE(ValidateStatement(if_stmt->else_statement()));
  return AsmType::Void();
}

// 6.5.5 ReturnStatement
AsmType* AsmTyper::ValidateReturnStatement(ReturnStatement* ret_stmt) {
  AsmType* ret_expr_type = AsmType::Void();
  if (auto* ret_expr = ret_stmt->expression()) {
    RECURSE(ret_expr_type = ValidateExpression(ret_expr));
    if (ret_expr_type == AsmType::Void()) {
      // *VIOLATION* The parser modifies the source code so that expressionless
      // returns will return undefined, so we need to allow that.
      if (!ret_expr->IsUndefinedLiteral()) {
        FAIL(ret_stmt, "Return statement expression can't be void.");
      }
    }
  }

  if (!ret_expr_type->IsA(return_type_)) {
    FAIL(ret_stmt, "Type mismatch in return statement.");
  }

  return ret_expr_type;
}

// 6.5.6 IterationStatement
// 6.5.6.a WhileStatement
AsmType* AsmTyper::ValidateWhileStatement(WhileStatement* while_stmt) {
  AsmType* cond_type;
  RECURSE(cond_type = ValidateExpression(while_stmt->cond()));
  if (!cond_type->IsA(AsmType::Int())) {
    FAIL(while_stmt->cond(), "While condition must be type int.");
  }

  if (auto* body = while_stmt->body()) {
    RECURSE(ValidateStatement(body));
  }
  return AsmType::Void();
}

// 6.5.6.b DoWhileStatement
AsmType* AsmTyper::ValidateDoWhileStatement(DoWhileStatement* do_while) {
  AsmType* cond_type;
  RECURSE(cond_type = ValidateExpression(do_while->cond()));
  if (!cond_type->IsA(AsmType::Int())) {
    FAIL(do_while->cond(), "Do {} While condition must be type int.");
  }

  if (auto* body = do_while->body()) {
    RECURSE(ValidateStatement(body));
  }
  return AsmType::Void();
}

// 6.5.6.c ForStatement
AsmType* AsmTyper::ValidateForStatement(ForStatement* for_stmt) {
  if (auto* init = for_stmt->init()) {
    RECURSE(ValidateStatement(init));
  }

  if (auto* cond = for_stmt->cond()) {
    AsmType* cond_type;
    RECURSE(cond_type = ValidateExpression(cond));
    if (!cond_type->IsA(AsmType::Int())) {
      FAIL(cond, "For condition must be type int.");
    }
  }

  if (auto* next = for_stmt->next()) {
    RECURSE(ValidateStatement(next));
  }

  if (auto* body = for_stmt->body()) {
    RECURSE(ValidateStatement(body));
  }

  return AsmType::Void();
}

// 6.5.7 BreakStatement
AsmType* AsmTyper::ValidateBreakStatement(BreakStatement* brk_stmt) {
  return AsmType::Void();
}

// 6.5.8 ContinueStatement
AsmType* AsmTyper::ValidateContinueStatement(ContinueStatement* cont_stmt) {
  return AsmType::Void();
}

// 6.5.9 LabelledStatement
// No need to handle these here -- see the AsmTyper's definition.

// 6.5.10 SwitchStatement
AsmType* AsmTyper::ValidateSwitchStatement(SwitchStatement* stmt) {
  AsmType* cond_type;
  RECURSE(cond_type = ValidateExpression(stmt->tag()));
  if (!cond_type->IsA(AsmType::Signed())) {
    FAIL(stmt, "Switch tag must be signed.");
  }

  bool has_default = false;

  ZoneSet<int32_t> cases_seen(zone_);
  for (auto* a_case : *stmt->cases()) {
    if (a_case->is_default()) {
      CHECK(!has_default);
      RECURSE(ValidateDefault(a_case));
      has_default = true;
    }

    int32_t case_lbl;
    RECURSE(ValidateCase(a_case, &case_lbl));
    auto case_lbl_pos = cases_seen.find(case_lbl);
    if (case_lbl_pos != cases_seen.end() && *case_lbl_pos == case_lbl) {
      FAIL(a_case, "Duplicated case label.");
    }
    cases_seen.insert(case_lbl);
  }

  if (!cases_seen.empty()) {
    const int64_t max_lbl = *cases_seen.rbegin();
    const int64_t min_lbl = *cases_seen.begin();
    if (max_lbl - min_lbl > std::numeric_limits<int32_t>::max()) {
      FAIL(stmt, "Out-of-bounds case label range.");
    }
  }

  return AsmType::Void();
}

// 6.6 ValidateCase
namespace {
bool ExtractInt32CaseLabel(CaseClause* clause, int32_t* lbl) {
  auto* lbl_expr = clause->label()->AsLiteral();

  if (lbl_expr == nullptr) {
    return false;
  }

  if (lbl_expr->raw_value()->ContainsDot()) {
    return false;
  }

  return lbl_expr->value()->ToInt32(lbl);
}
}  // namespace

AsmType* AsmTyper::ValidateCase(CaseClause* label, int32_t* case_lbl) {
  if (!ExtractInt32CaseLabel(label, case_lbl)) {
    FAIL(label, "Case label must be a 32-bit signed integer.");
  }

  FlattenedStatements iter(zone_, label->statements());
  while (auto* current = iter.Next()) {
    RECURSE(ValidateStatement(current));
  }
  return AsmType::Void();
}

// 6.7 ValidateDefault
AsmType* AsmTyper::ValidateDefault(CaseClause* label) {
  FlattenedStatements iter(zone_, label->statements());
  while (auto* current = iter.Next()) {
    RECURSE(ValidateStatement(current));
  }
  return AsmType::Void();
}

// 6.8 ValidateExpression
AsmType* AsmTyper::ValidateExpression(Expression* expr) {
  AsmType* expr_ty = AsmType::None();

  switch (expr->node_type()) {
    default:
      FAIL(expr, "Invalid asm.js expression.");
    case AstNode::kLiteral:
      RECURSE(expr_ty = ValidateNumericLiteral(expr->AsLiteral()));
      break;
    case AstNode::kVariableProxy:
      RECURSE(expr_ty = ValidateIdentifier(expr->AsVariableProxy()));
      break;
    case AstNode::kCall:
      RECURSE(expr_ty = ValidateCallExpression(expr->AsCall()));
      break;
    case AstNode::kProperty:
      RECURSE(expr_ty = ValidateMemberExpression(expr->AsProperty()));
      break;
    case AstNode::kAssignment:
      RECURSE(expr_ty = ValidateAssignmentExpression(expr->AsAssignment()));
      break;
    case AstNode::kUnaryOperation:
      RECURSE(expr_ty = ValidateUnaryExpression(expr->AsUnaryOperation()));
      break;
    case AstNode::kConditional:
      RECURSE(expr_ty = ValidateConditionalExpression(expr->AsConditional()));
      break;
    case AstNode::kCompareOperation:
      RECURSE(expr_ty = ValidateCompareOperation(expr->AsCompareOperation()));
      break;
    case AstNode::kBinaryOperation:
      RECURSE(expr_ty = ValidateBinaryOperation(expr->AsBinaryOperation()));
      break;
  }

  SetTypeOf(expr, expr_ty);
  return expr_ty;
}

AsmType* AsmTyper::ValidateCompareOperation(CompareOperation* cmp) {
  switch (cmp->op()) {
    default:
      FAIL(cmp, "Invalid asm.js comparison operator.");
    case Token::LT:
    case Token::LTE:
    case Token::GT:
    case Token::GTE:
      return ValidateRelationalExpression(cmp);
    case Token::EQ:
    case Token::NE:
      return ValidateEqualityExpression(cmp);
  }

  UNREACHABLE();
}

namespace {
bool IsNegate(BinaryOperation* binop) {
  if (binop->op() != Token::BIT_XOR) {
    return false;
  }

  auto* right_as_literal = binop->right()->AsLiteral();
  if (right_as_literal == nullptr) {
    return false;
  }

  return !right_as_literal->raw_value()->ContainsDot() &&
         right_as_literal->raw_value()->AsNumber() == -1.0;
}

bool IsUnaryMinus(BinaryOperation* binop) {
  // *VIOLATION* The parser replaces uses of +x with x*1.0.
  if (binop->op() != Token::MUL) {
    return false;
  }

  auto* right_as_literal = binop->right()->AsLiteral();
  if (right_as_literal == nullptr) {
    return false;
  }

  return right_as_literal->raw_value()->ContainsDot() &&
         right_as_literal->raw_value()->AsNumber() == -1.0;
}
}  // namespace

AsmType* AsmTyper::ValidateBinaryOperation(BinaryOperation* expr) {
#define UNOP_OVERLOAD(Src, Dest)          \
  do {                                    \
    if (left_type->IsA(AsmType::Src())) { \
      return AsmType::Dest();             \
    }                                     \
  } while (0)

  switch (expr->op()) {
    default:
      FAIL(expr, "Invalid asm.js binary expression.");
    case Token::COMMA:
      return ValidateCommaExpression(expr);
    case Token::MUL:
      if (IsDoubleAnnotation(expr)) {
        // *VIOLATION* We can't be 100% sure this really IS a unary + in the asm
        // source so we have to be lenient, and treat this as a unary +.
        if (auto* Call = expr->left()->AsCall()) {
          return ValidateCall(AsmType::Double(), Call);
        }
        AsmType* left_type;
        RECURSE(left_type = ValidateExpression(expr->left()));
        UNOP_OVERLOAD(Signed, Double);
        UNOP_OVERLOAD(Unsigned, Double);
        UNOP_OVERLOAD(DoubleQ, Double);
        UNOP_OVERLOAD(FloatQ, Double);
        FAIL(expr, "Invalid type for conversion to double.");
      }

      if (IsUnaryMinus(expr)) {
        // *VIOLATION* the parser converts -x to x * -1.0.
        AsmType* left_type;
        RECURSE(left_type = ValidateExpression(expr->left()));
        UNOP_OVERLOAD(Int, Intish);
        UNOP_OVERLOAD(DoubleQ, Double);
        UNOP_OVERLOAD(FloatQ, Floatish);
        FAIL(expr, "Invalid type for unary -.");
      }
    // FALTHROUGH
    case Token::DIV:
    case Token::MOD:
      return ValidateMultiplicativeExpression(expr);
    case Token::ADD:
    case Token::SUB: {
      static const uint32_t kInitialIntishCount = 0;
      return ValidateAdditiveExpression(expr, kInitialIntishCount);
    }
    case Token::SAR:
    case Token::SHL:
    case Token::SHR:
      return ValidateShiftExpression(expr);
    case Token::BIT_AND:
      return ValidateBitwiseANDExpression(expr);
    case Token::BIT_XOR:
      if (IsNegate(expr)) {
        auto* left = expr->left();
        auto* left_as_binop = left->AsBinaryOperation();

        if (left_as_binop != nullptr && IsNegate(left_as_binop)) {
          // This is the special ~~ operator.
          AsmType* left_type;
          RECURSE(left_type = ValidateExpression(left_as_binop->left()));
          UNOP_OVERLOAD(Double, Signed);
          UNOP_OVERLOAD(FloatQ, Signed);
          FAIL(left_as_binop, "Invalid type for conversion to signed.");
        }

        AsmType* left_type;
        RECURSE(left_type = ValidateExpression(left));
        UNOP_OVERLOAD(Intish, Signed);
        FAIL(left, "Invalid type for ~.");
      }

      return ValidateBitwiseXORExpression(expr);
    case Token::BIT_OR:
      return ValidateBitwiseORExpression(expr);
  }
#undef UNOP_OVERLOAD
  UNREACHABLE();
}

// 6.8.1 Expression
AsmType* AsmTyper::ValidateCommaExpression(BinaryOperation* comma) {
  // The AST looks like:
  // (expr COMMA (expr COMMA (expr COMMA (... ))))

  auto* left = comma->left();
  auto* left_as_binop = left->AsBinaryOperation();
  if (left_as_binop && left_as_binop->op() == Token::COMMA) {
    ValidateCommaExpression(left_as_binop);
  } else if (auto* left_as_call = left->AsCall()) {
    ValidateCall(AsmType::Void(), left_as_call);
  } else {
    ValidateExpression(left);
  }

  auto* right = comma->right();
  auto* right_as_binop = right->AsBinaryOperation();
  if (right_as_binop && right_as_binop->op() == Token::COMMA) {
    return ValidateCommaExpression(right_as_binop);
  } else {
    return ValidateExpression(right);
  }

  UNREACHABLE();
}

// 6.8.2 NumericLiteral
AsmType* AsmTyper::ValidateNumericLiteral(Literal* literal) {
  // *VIOLATION* asm.js does not allow the use of undefined, but our parser
  // inserts them, so we have to handle them.
  if (literal->IsUndefinedLiteral()) {
    return AsmType::Void();
  }

  if (literal->raw_value()->ContainsDot()) {
    return AsmType::Double();
  }

  uint32_t value;
  if (!literal->value()->ToUint32(&value)) {
    int32_t value;
    if (!literal->value()->ToInt32(&value)) {
      FAIL(literal, "Integer literal is out of range.");
    }
    // *VIOLATION* Not really a violation, but rather a different in the
    // validation. The spec handles -NumericLiteral in ValidateUnaryExpression,
    // but V8's AST represents the negative literals as Literals.
    return AsmType::Signed();
  }

  static const uint32_t LargestFixNum = std::numeric_limits<int32_t>::max();
  if (value <= LargestFixNum) {
    return AsmType::FixNum();
  }

  return AsmType::Unsigned();
}

// 6.8.3 Identifier
AsmType* AsmTyper::ValidateIdentifier(VariableProxy* proxy) {
  auto* proxy_info = Lookup(proxy->var());
  if (proxy_info == nullptr) {
    FAIL(proxy, "Undeclared identifier.");
  }
  if (proxy_info->type()->AsCallableType() != nullptr) {
    FAIL(proxy, "Identifier may not be accessed by ordinary expressions.");
  }
  DCHECK(!proxy_info->type()->IsA(AsmType::None()));
  return proxy_info->type();
}

// 6.8.4 CallExpression
AsmType* AsmTyper::ValidateCallExpression(Call* call) {
  AsmType* return_type;
  RECURSE(return_type = ValidateFloatCoercion(call));
  if (return_type == nullptr) {
    FAIL(call, "Unanotated call to a function must be a call to fround.");
  }
  return return_type;
}

// 6.8.5 MemberExpression
AsmType* AsmTyper::ValidateMemberExpression(Property* prop) {
  AsmType* return_type;
  RECURSE(return_type = ValidateHeapAccess(prop, LoadFromHeap));
  return return_type;
}

// 6.8.6 AssignmentExpression
AsmType* AsmTyper::ValidateAssignmentExpression(Assignment* assignment) {
  AsmType* value_type;
  RECURSE(value_type = ValidateExpression(assignment->value()));

  if (assignment->op() == Token::INIT) {
    FAIL(assignment,
         "Local variable declaration must be at the top of the function.");
  }

  if (auto* target_as_proxy = assignment->target()->AsVariableProxy()) {
    auto* var = target_as_proxy->var();
    auto* target_info = Lookup(var);

    if (target_info == nullptr) {
      if (var->mode() != TEMPORARY) {
        FAIL(target_as_proxy, "Undeclared identifier.");
      }
      // Temporary variables are special: we add them to the local symbol table
      // as we see them, with the exact type of the variable's initializer. This
      // means that temporary variables might have nonsensical types (i.e.,
      // intish, float?, fixnum, and not just the "canonical" types.)
      auto* var_info = new (zone_) VariableInfo(value_type);
      var_info->set_mutability(VariableInfo::kLocal);
      if (!ValidAsmIdentifier(target_as_proxy->name())) {
        FAIL(target_as_proxy,
             "Invalid asm.js identifier in temporary variable.");
      }

      if (!AddLocal(var, var_info)) {
        FAIL(assignment, "Failed to add temporary variable to symbol table.");
      }
      return value_type;
    }

    DCHECK(target_info->type() != AsmType::None());
    if (!value_type->IsA(target_info->type())) {
      FAIL(assignment, "Type mismatch in assignment.");
    }

    return value_type;
  }

  if (auto* target_as_property = assignment->target()->AsProperty()) {
    AsmType* allowed_store_types;
    RECURSE(allowed_store_types =
                ValidateHeapAccess(target_as_property, StoreToHeap));

    // TODO(jpp): Change FloatishDoubleQ and FloatQDoubleQ so that they are base
    // classes for Floatish, DoubleQ, and FloatQ, and then invert this if so
    // that it reads more naturally as
    //
    // if (!value_type->IsA(allowed_store_types))
    if (allowed_store_types == AsmType::FloatishDoubleQ() ||
        allowed_store_types == AsmType::FloatQDoubleQ()) {
      if (!allowed_store_types->IsA(value_type)) {
        FAIL(assignment, "Type mismatch in heap assignment.");
      }
    } else {
      if (!value_type->IsA(allowed_store_types)) {
        FAIL(assignment, "Type mismatch in heap assignment.");
      }
    }

    return value_type;
  }

  FAIL(assignment, "Invalid asm.js assignment.");
}

// 6.8.7 UnaryExpression
AsmType* AsmTyper::ValidateUnaryExpression(UnaryOperation* unop) {
  // *VIOLATION* -NumericLiteral is validated in ValidateLiteral.
  // *VIOLATION* +UnaryExpression is validated in ValidateBinaryOperation.
  // *VIOLATION* ~UnaryOperation is validated in ValidateBinaryOperation.
  // *VIOLATION* ~~UnaryOperation is validated in ValidateBinaryOperation.
  DCHECK(unop->op() != Token::BIT_NOT);
  DCHECK(unop->op() != Token::ADD);
  AsmType* exp_type;
  RECURSE(exp_type = ValidateExpression(unop->expression()));
#define UNOP_OVERLOAD(Src, Dest)         \
  do {                                   \
    if (exp_type->IsA(AsmType::Src())) { \
      return AsmType::Dest();            \
    }                                    \
  } while (0)

  // 8.1 Unary Operators
  switch (unop->op()) {
    default:
      FAIL(unop, "Invalid unary operator.");
    case Token::ADD:
      // We can't test this because of the +x -> x * 1.0 transformation.
      DCHECK(false);
      UNOP_OVERLOAD(Signed, Double);
      UNOP_OVERLOAD(Unsigned, Double);
      UNOP_OVERLOAD(DoubleQ, Double);
      UNOP_OVERLOAD(FloatQ, Double);
      FAIL(unop, "Invalid type for unary +.");
    case Token::SUB:
      // We can't test this because of the -x -> x * -1.0 transformation.
      DCHECK(false);
      UNOP_OVERLOAD(Int, Intish);
      UNOP_OVERLOAD(DoubleQ, Double);
      UNOP_OVERLOAD(FloatQ, Floatish);
      FAIL(unop, "Invalid type for unary -.");
    case Token::BIT_NOT:
      // We can't test this because of the ~x -> x ^ -1 transformation.
      DCHECK(false);
      UNOP_OVERLOAD(Intish, Signed);
      FAIL(unop, "Invalid type for ~.");
    case Token::NOT:
      UNOP_OVERLOAD(Int, Int);
      FAIL(unop, "Invalid type for !.");
  }

#undef UNOP_OVERLOAD

  UNREACHABLE();
}

// 6.8.8 MultiplicativeExpression
namespace {
bool IsIntishLiteralFactor(Expression* expr, int32_t* factor) {
  auto* literal = expr->AsLiteral();
  if (literal == nullptr) {
    return false;
  }

  if (literal->raw_value()->ContainsDot()) {
    return false;
  }

  if (!literal->value()->ToInt32(factor)) {
    return false;
  }
  static const int32_t kIntishBound = 1 << 20;
  return -kIntishBound < *factor && *factor < kIntishBound;
}
}  // namespace

AsmType* AsmTyper::ValidateMultiplicativeExpression(BinaryOperation* binop) {
  DCHECK(!IsDoubleAnnotation(binop));

  auto* left = binop->left();
  auto* right = binop->right();

  bool intish_mul_failed = false;
  if (binop->op() == Token::MUL) {
    int32_t factor;
    if (IsIntishLiteralFactor(left, &factor)) {
      AsmType* right_type;
      RECURSE(right_type = ValidateExpression(right));
      if (right_type->IsA(AsmType::Int())) {
        return AsmType::Intish();
      }
      // Can't fail here, because the rhs might contain a valid intish factor.
      //
      // The solution is to flag that there was an error, and later on -- when
      // both lhs and rhs are evaluated -- complain.
      intish_mul_failed = true;
    }

    if (IsIntishLiteralFactor(right, &factor)) {
      AsmType* left_type;
      RECURSE(left_type = ValidateExpression(left));
      if (left_type->IsA(AsmType::Int())) {
        // *VIOLATION* This will also (and correctly) handle -X, when X is an
        // integer. Therefore, we don't need to handle this case within the if
        // block below.
        return AsmType::Intish();
      }
      intish_mul_failed = true;

      if (factor == -1) {
        // *VIOLATION* The frontend transforms -x into x * -1 (not -1.0, because
        // consistency is overrated.)
        if (left_type->IsA(AsmType::DoubleQ())) {
          return AsmType::Double();
        } else if (left_type->IsA(AsmType::FloatQ())) {
          return AsmType::Floatish();
        }
      }
    }
  }

  if (intish_mul_failed) {
    FAIL(binop, "Invalid types for intish * (or unary -).");
  }

  AsmType* left_type;
  AsmType* right_type;
  RECURSE(left_type = ValidateExpression(left));
  RECURSE(right_type = ValidateExpression(right));

#define BINOP_OVERLOAD(Src0, Src1, Dest)                                       \
  do {                                                                         \
    if (left_type->IsA(AsmType::Src0()) && right_type->IsA(AsmType::Src1())) { \
      return AsmType::Dest();                                                  \
    }                                                                          \
  } while (0)
  switch (binop->op()) {
    default:
      FAIL(binop, "Invalid multiplicative expression.");
    case Token::MUL:
      BINOP_OVERLOAD(DoubleQ, DoubleQ, Double);
      BINOP_OVERLOAD(FloatQ, FloatQ, Floatish);
      FAIL(binop, "Invalid operands for *.");
    case Token::DIV:
      BINOP_OVERLOAD(Signed, Signed, Intish);
      BINOP_OVERLOAD(Unsigned, Unsigned, Intish);
      BINOP_OVERLOAD(DoubleQ, DoubleQ, Double);
      BINOP_OVERLOAD(FloatQ, FloatQ, Floatish);
      FAIL(binop, "Invalid operands for /.");
    case Token::MOD:
      BINOP_OVERLOAD(Signed, Signed, Intish);
      BINOP_OVERLOAD(Unsigned, Unsigned, Intish);
      BINOP_OVERLOAD(DoubleQ, DoubleQ, Double);
      FAIL(binop, "Invalid operands for %.");
  }
#undef BINOP_OVERLOAD

  UNREACHABLE();
}

// 6.8.9 AdditiveExpression
AsmType* AsmTyper::ValidateAdditiveExpression(BinaryOperation* binop,
                                              uint32_t intish_count) {
  static const uint32_t kMaxIntish = 1 << 20;

  auto* left = binop->left();
  auto* left_as_binop = left->AsBinaryOperation();
  AsmType* left_type;

  // TODO(jpp): maybe use an iterative approach instead of the recursion to
  // ValidateAdditiveExpression.
  if (left_as_binop != nullptr && (left_as_binop->op() == Token::ADD ||
                                   left_as_binop->op() == Token::SUB)) {
    RECURSE(left_type =
                ValidateAdditiveExpression(left_as_binop, intish_count + 1));
  } else {
    RECURSE(left_type = ValidateExpression(left));
  }

  auto* right = binop->right();
  auto* right_as_binop = right->AsBinaryOperation();
  AsmType* right_type;

  if (right_as_binop != nullptr && (right_as_binop->op() == Token::ADD ||
                                    right_as_binop->op() == Token::SUB)) {
    RECURSE(right_type =
                ValidateAdditiveExpression(right_as_binop, intish_count + 1));
  } else {
    RECURSE(right_type = ValidateExpression(right));
  }

  if (left_type->IsA(AsmType::FloatQ()) && right_type->IsA(AsmType::FloatQ())) {
    return AsmType::Floatish();
  }

  if (left_type->IsA(AsmType::Int()) && right_type->IsA(AsmType::Int())) {
    if (intish_count == 0) {
      return AsmType::Intish();
    }
    if (intish_count < kMaxIntish) {
      return AsmType::Int();
    }
    FAIL(binop, "Too many uncoerced integer additive expressions.");
  }

  if (left_type->IsA(AsmType::Double()) && right_type->IsA(AsmType::Double())) {
    return AsmType::Double();
  }

  if (binop->op() == Token::SUB) {
    if (left_type->IsA(AsmType::DoubleQ()) &&
        right_type->IsA(AsmType::DoubleQ())) {
      return AsmType::Double();
    }
  }

  FAIL(binop, "Invalid operands for additive expression.");
}

// 6.8.10 ShiftExpression
AsmType* AsmTyper::ValidateShiftExpression(BinaryOperation* binop) {
  auto* left = binop->left();
  auto* right = binop->right();

  AsmType* left_type;
  AsmType* right_type;
  RECURSE(left_type = ValidateExpression(left));
  RECURSE(right_type = ValidateExpression(right));

#define BINOP_OVERLOAD(Src0, Src1, Dest)                                       \
  do {                                                                         \
    if (left_type->IsA(AsmType::Src0()) && right_type->IsA(AsmType::Src1())) { \
      return AsmType::Dest();                                                  \
    }                                                                          \
  } while (0)
  switch (binop->op()) {
    default:
      FAIL(binop, "Invalid shift expression.");
    case Token::SHL:
      BINOP_OVERLOAD(Intish, Intish, Signed);
      FAIL(binop, "Invalid operands for <<.");
    case Token::SAR:
      BINOP_OVERLOAD(Intish, Intish, Signed);
      FAIL(binop, "Invalid operands for >>.");
    case Token::SHR:
      BINOP_OVERLOAD(Intish, Intish, Unsigned);
      FAIL(binop, "Invalid operands for >>>.");
  }
#undef BINOP_OVERLOAD

  UNREACHABLE();
}

// 6.8.11 RelationalExpression
AsmType* AsmTyper::ValidateRelationalExpression(CompareOperation* cmpop) {
  auto* left = cmpop->left();
  auto* right = cmpop->right();

  AsmType* left_type;
  AsmType* right_type;
  RECURSE(left_type = ValidateExpression(left));
  RECURSE(right_type = ValidateExpression(right));

#define CMPOP_OVERLOAD(Src0, Src1, Dest)                                       \
  do {                                                                         \
    if (left_type->IsA(AsmType::Src0()) && right_type->IsA(AsmType::Src1())) { \
      return AsmType::Dest();                                                  \
    }                                                                          \
  } while (0)
  switch (cmpop->op()) {
    default:
      FAIL(cmpop, "Invalid relational expression.");
    case Token::LT:
      CMPOP_OVERLOAD(Signed, Signed, Int);
      CMPOP_OVERLOAD(Unsigned, Unsigned, Int);
      CMPOP_OVERLOAD(Float, Float, Int);
      CMPOP_OVERLOAD(Double, Double, Int);
      FAIL(cmpop, "Invalid operands for <.");
    case Token::GT:
      CMPOP_OVERLOAD(Signed, Signed, Int);
      CMPOP_OVERLOAD(Unsigned, Unsigned, Int);
      CMPOP_OVERLOAD(Float, Float, Int);
      CMPOP_OVERLOAD(Double, Double, Int);
      FAIL(cmpop, "Invalid operands for >.");
    case Token::LTE:
      CMPOP_OVERLOAD(Signed, Signed, Int);
      CMPOP_OVERLOAD(Unsigned, Unsigned, Int);
      CMPOP_OVERLOAD(Float, Float, Int);
      CMPOP_OVERLOAD(Double, Double, Int);
      FAIL(cmpop, "Invalid operands for <=.");
    case Token::GTE:
      CMPOP_OVERLOAD(Signed, Signed, Int);
      CMPOP_OVERLOAD(Unsigned, Unsigned, Int);
      CMPOP_OVERLOAD(Float, Float, Int);
      CMPOP_OVERLOAD(Double, Double, Int);
      FAIL(cmpop, "Invalid operands for >=.");
  }
#undef CMPOP_OVERLOAD

  UNREACHABLE();
}

// 6.8.12 EqualityExpression
AsmType* AsmTyper::ValidateEqualityExpression(CompareOperation* cmpop) {
  auto* left = cmpop->left();
  auto* right = cmpop->right();

  AsmType* left_type;
  AsmType* right_type;
  RECURSE(left_type = ValidateExpression(left));
  RECURSE(right_type = ValidateExpression(right));

#define CMPOP_OVERLOAD(Src0, Src1, Dest)                                       \
  do {                                                                         \
    if (left_type->IsA(AsmType::Src0()) && right_type->IsA(AsmType::Src1())) { \
      return AsmType::Dest();                                                  \
    }                                                                          \
  } while (0)
  switch (cmpop->op()) {
    default:
      FAIL(cmpop, "Invalid equality expression.");
    case Token::EQ:
      CMPOP_OVERLOAD(Signed, Signed, Int);
      CMPOP_OVERLOAD(Unsigned, Unsigned, Int);
      CMPOP_OVERLOAD(Float, Float, Int);
      CMPOP_OVERLOAD(Double, Double, Int);
      FAIL(cmpop, "Invalid operands for ==.");
    case Token::NE:
      CMPOP_OVERLOAD(Signed, Signed, Int);
      CMPOP_OVERLOAD(Unsigned, Unsigned, Int);
      CMPOP_OVERLOAD(Float, Float, Int);
      CMPOP_OVERLOAD(Double, Double, Int);
      FAIL(cmpop, "Invalid operands for !=.");
  }
#undef CMPOP_OVERLOAD

  UNREACHABLE();
}

// 6.8.13 BitwiseANDExpression
AsmType* AsmTyper::ValidateBitwiseANDExpression(BinaryOperation* binop) {
  auto* left = binop->left();
  auto* right = binop->right();

  AsmType* left_type;
  AsmType* right_type;
  RECURSE(left_type = ValidateExpression(left));
  RECURSE(right_type = ValidateExpression(right));

  if (binop->op() != Token::BIT_AND) {
    FAIL(binop, "Invalid & expression.");
  }

#define BINOP_OVERLOAD(Src0, Src1, Dest)                                       \
  do {                                                                         \
    if (left_type->IsA(AsmType::Src0()) && right_type->IsA(AsmType::Src1())) { \
      return AsmType::Dest();                                                  \
    }                                                                          \
  } while (0)
  BINOP_OVERLOAD(Intish, Intish, Signed);
  FAIL(binop, "Invalid operands for &.");
#undef BINOP_OVERLOAD

  UNREACHABLE();
}

// 6.8.14 BitwiseXORExpression
AsmType* AsmTyper::ValidateBitwiseXORExpression(BinaryOperation* binop) {
  auto* left = binop->left();
  auto* right = binop->right();

  AsmType* left_type;
  AsmType* right_type;
  RECURSE(left_type = ValidateExpression(left));
  RECURSE(right_type = ValidateExpression(right));

  if (binop->op() != Token::BIT_XOR) {
    FAIL(binop, "Invalid ^ expression.");
  }

#define BINOP_OVERLOAD(Src0, Src1, Dest)                                       \
  do {                                                                         \
    if (left_type->IsA(AsmType::Src0()) && right_type->IsA(AsmType::Src1())) { \
      return AsmType::Dest();                                                  \
    }                                                                          \
  } while (0)
  BINOP_OVERLOAD(Intish, Intish, Signed);
  FAIL(binop, "Invalid operands for ^.");
#undef BINOP_OVERLOAD

  UNREACHABLE();
}

// 6.8.15 BitwiseORExpression
AsmType* AsmTyper::ValidateBitwiseORExpression(BinaryOperation* binop) {
  auto* left = binop->left();
  if (IsIntAnnotation(binop)) {
    if (auto* left_as_call = left->AsCall()) {
      AsmType* type;
      RECURSE(type = ValidateCall(AsmType::Signed(), left_as_call));
      return type;
    }

    // TODO(jpp): at this point we know that binop is expr|0. We could sinply
    //
    // RECURSE(t = ValidateExpression(left));
    // FAIL_IF(t->IsNotA(Intish));
    // return Signed;
  }

  auto* right = binop->right();
  AsmType* left_type;
  AsmType* right_type;
  RECURSE(left_type = ValidateExpression(left));
  RECURSE(right_type = ValidateExpression(right));

  if (binop->op() != Token::BIT_OR) {
    FAIL(binop, "Invalid | expression.");
  }

#define BINOP_OVERLOAD(Src0, Src1, Dest)                                       \
  do {                                                                         \
    if (left_type->IsA(AsmType::Src0()) && right_type->IsA(AsmType::Src1())) { \
      return AsmType::Dest();                                                  \
    }                                                                          \
  } while (0)
  BINOP_OVERLOAD(Intish, Intish, Signed);
  FAIL(binop, "Invalid operands for |.");
#undef BINOP_OVERLOAD

  UNREACHABLE();
}

// 6.8.16 ConditionalExpression
AsmType* AsmTyper::ValidateConditionalExpression(Conditional* cond) {
  AsmType* cond_type;
  RECURSE(cond_type = ValidateExpression(cond->condition()));
  if (!cond_type->IsA(AsmType::Int())) {
    FAIL(cond, "Ternary operation condition should be int.");
  }

  AsmType* then_type;
  RECURSE(then_type = ValidateExpression(cond->then_expression()));
  AsmType* else_type;
  RECURSE(else_type = ValidateExpression(cond->else_expression()));

#define SUCCEED_IF_BOTH_ARE(type)                                       \
  do {                                                                  \
    if (then_type->IsA(AsmType::type())) {                              \
      if (!else_type->IsA(AsmType::type())) {                           \
        FAIL(cond, "Type mismatch for ternary operation result type."); \
      }                                                                 \
      return AsmType::type();                                           \
    }                                                                   \
  } while (0)
  SUCCEED_IF_BOTH_ARE(Int);
  SUCCEED_IF_BOTH_ARE(Float);
  SUCCEED_IF_BOTH_ARE(Double);
#undef SUCCEED_IF_BOTH_ARE

  FAIL(cond, "Ternary operator must return int, float, or double.");
}

// 6.9 ValidateCall
namespace {
bool ExtractIndirectCallMask(Expression* expr, uint32_t* value) {
  auto* as_literal = expr->AsLiteral();
  if (as_literal == nullptr) {
    return false;
  }

  if (as_literal->raw_value()->ContainsDot()) {
    return false;
  }

  if (!as_literal->value()->ToUint32(value)) {
    return false;
  }

  return base::bits::IsPowerOfTwo32(1 + *value);
}

// TODO(jpp): Add a AsmType::ValidateCall is poorly designed. It can only handle
// function declarations, not invocations. CheckInvocationOf temporarily works
// around this limitation by converting each actual in actuals to a parameter
// type before invoking prototype->ValidateCall. This is the wrong behavior for
// FFIs (we need to pass Signed integers to FFIs, not Ints), so that case is
// handled separately.
bool CheckInvocationOf(AsmCallableType* prototype, AsmType* return_type,
                       ZoneVector<AsmType*>* actuals) {
  if (auto* ffi = prototype->AsFFIType()) {
    return ffi->ValidateCall(return_type, *actuals) != AsmType::None();
  }

  for (size_t ii = 0; ii < actuals->size(); ++ii) {
    (*actuals)[ii] = (*actuals)[ii]->ToParameterType();
  }
  return prototype->ValidateCall(return_type, *actuals) != AsmType::None();
}

}  // namespace

AsmType* AsmTyper::ValidateCall(AsmType* return_type, Call* call) {
  AsmType* float_coercion_type;
  RECURSE(float_coercion_type = ValidateFloatCoercion(call));
  if (float_coercion_type == AsmType::Float()) {
    return AsmType::Float();
  }

  // TODO(jpp): we should be able to reuse the args vector's storage space.
  ZoneVector<AsmType*> args(zone_);
  args.reserve(call->arguments()->length());

  for (auto* arg : *call->arguments()) {
    AsmType* arg_type;
    RECURSE(arg_type = ValidateExpression(arg));
    args.emplace_back(arg_type);
  }

  auto* call_expr = call->expression();

  // identifier(Expression...)
  if (auto* call_var_proxy = call_expr->AsVariableProxy()) {
    auto* call_var_info = Lookup(call_var_proxy->var());

    if (call_var_info == nullptr) {
      // We can't fail here: the validator performs a single pass over the AST,
      // so it is possible for some calls to be currently unresolved. We eagerly
      // add the function to the table of globals.
      auto* call_type = AsmType::Function(zone_, return_type)->AsFunctionType();
      for (auto* arg : args) {
        call_type->AddArgument(arg->ToParameterType());
      }
      auto* fun_info =
          new (zone_) VariableInfo(reinterpret_cast<AsmType*>(call_type));
      fun_info->set_mutability(VariableInfo::kImmutableGlobal);
      AddForwardReference(call_var_proxy, fun_info);
      if (!ValidAsmIdentifier(call_var_proxy->name())) {
        FAIL(call_var_proxy,
             "Invalid asm.js identifier in (forward) function name.");
      }
      if (!AddGlobal(call_var_proxy->var(), fun_info)) {
        DCHECK(false);
        FAIL(call, "Redeclared global identifier.");
      }
      return return_type;
    }

    auto* callee_type = call_var_info->type()->AsCallableType();
    if (callee_type == nullptr) {
      FAIL(call, "Calling something that's not a function.");
    }

    if (callee_type->AsFFIType() != nullptr &&
        return_type == AsmType::Float()) {
      FAIL(call, "Foreign functions can't return float.");
    }

    if (!CheckInvocationOf(callee_type, return_type, &args)) {
      FAIL(call, "Function invocation does not match function type.");
    }

    return return_type;
  }

  // identifier[expr & n](Expression...)
  if (auto* call_property = call_expr->AsProperty()) {
    auto* index = call_property->key()->AsBinaryOperation();
    if (index == nullptr || index->op() != Token::BIT_AND) {
      FAIL(call_property->key(),
           "Indirect call index must be in the expr & mask form.");
    }

    auto* left = index->left();
    auto* right = index->right();
    uint32_t mask;
    if (!ExtractIndirectCallMask(right, &mask)) {
      if (!ExtractIndirectCallMask(left, &mask)) {
        FAIL(right, "Invalid indirect call mask.");
      } else {
        left = right;
      }
    }
    const uint32_t table_length = mask + 1;

    AsmType* left_type;
    RECURSE(left_type = ValidateExpression(left));
    if (!left_type->IsA(AsmType::Intish())) {
      FAIL(left, "Indirect call index should be an intish.");
    }

    auto* name_var = call_property->obj()->AsVariableProxy();

    if (name_var == nullptr) {
      FAIL(call_property, "Invalid call.");
    }

    auto* name_info = Lookup(name_var->var());
    if (name_info == nullptr) {
      // We can't fail here -- just like above.
      auto* call_type = AsmType::Function(zone_, return_type)->AsFunctionType();
      for (auto* arg : args) {
        call_type->AddArgument(arg->ToParameterType());
      }
      auto* table_type = AsmType::FunctionTableType(
          zone_, table_length, reinterpret_cast<AsmType*>(call_type));
      auto* fun_info =
          new (zone_) VariableInfo(reinterpret_cast<AsmType*>(table_type));
      fun_info->set_mutability(VariableInfo::kImmutableGlobal);
      AddForwardReference(name_var, fun_info);
      if (!ValidAsmIdentifier(name_var->name())) {
        FAIL(name_var,
             "Invalid asm.js identifier in (forward) function table name.");
      }
      if (!AddGlobal(name_var->var(), fun_info)) {
        DCHECK(false);
        FAIL(call, "Redeclared global identifier.");
      }
      return return_type;
    }

    auto* previous_type = name_info->type()->AsFunctionTableType();
    if (previous_type == nullptr) {
      FAIL(call, "Identifier does not name a function table.");
    }

    if (table_length != previous_type->length()) {
      FAIL(call, "Function table size does not match expected size.");
    }

    auto* previous_type_signature =
        previous_type->signature()->AsFunctionType();
    DCHECK(previous_type_signature != nullptr);
    if (!CheckInvocationOf(previous_type_signature, return_type, &args)) {
      FAIL(call,
           "Function pointer table signature does not match previous "
           "signature.");
    }

    return return_type;
  }

  FAIL(call, "Invalid call.");
}

// 6.10 ValidateHeapAccess
namespace {
bool ExtractHeapAccessShift(Expression* expr, uint32_t* value) {
  auto* as_literal = expr->AsLiteral();
  if (as_literal == nullptr) {
    return false;
  }

  if (as_literal->raw_value()->ContainsDot()) {
    return false;
  }

  return as_literal->value()->ToUint32(value);
}
}  // namespace

AsmType* AsmTyper::ValidateHeapAccess(Property* heap,
                                      HeapAccessType access_type) {
  auto* obj = heap->obj()->AsVariableProxy();
  if (obj == nullptr) {
    FAIL(heap, "Invalid heap access.");
  }

  auto* obj_info = Lookup(obj->var());
  if (obj_info == nullptr) {
    FAIL(heap, "Undeclared identifier in heap access.");
  }

  auto* obj_type = obj_info->type();
  if (!obj_type->IsA(AsmType::Heap())) {
    FAIL(heap, "Identifier does not represent a heap view.");
  }

  if (auto* key_as_literal = heap->key()->AsLiteral()) {
    if (key_as_literal->raw_value()->ContainsDot()) {
      FAIL(key_as_literal, "Heap access index must be intish.");
    }

    uint32_t _;
    if (!key_as_literal->value()->ToUint32(&_)) {
      FAIL(key_as_literal,
           "Heap access index must be a 32-bit unsigned integer.");
    }

    if (access_type == LoadFromHeap) {
      return obj_type->LoadType();
    }
    return obj_type->StoreType();
  }

  if (auto* key_as_binop = heap->key()->AsBinaryOperation()) {
    uint32_t shift;
    if (key_as_binop->op() == Token::SAR &&
        ExtractHeapAccessShift(key_as_binop->right(), &shift) &&
        (1 << shift) == obj_type->ElementSizeInBytes()) {
      AsmType* type;
      RECURSE(type = ValidateExpression(key_as_binop->left()));
      if (type->IsA(AsmType::Intish())) {
        if (access_type == LoadFromHeap) {
          return obj_type->LoadType();
        }
        return obj_type->StoreType();
      }
      // TODO(jpp): it may be the case that, if type is not an Intish, we could
      // fail here instead of letting the validator try using the "leniency"
      // rule (i.e., allow unshifted indexes for heap views of 8-bit integers.
    }
  }

  if (obj_type->ElementSizeInBytes() == 1) {
    // Leniency: if this is a byte array, we don't require the shift operation
    // to be present.
    AsmType* index_type;
    RECURSE(index_type = ValidateExpression(heap->key()));
    if (!index_type->IsA(AsmType::Int())) {
      FAIL(heap, "Invalid heap access index for byte array.");
    }
    if (access_type == LoadFromHeap) {
      return obj_type->LoadType();
    }
    return obj_type->StoreType();
  }

  FAIL(heap, "Invalid heap access index.");
}

// 6.11 ValidateFloatCoercion
bool AsmTyper::IsCallToFround(Call* call) {
  if (call->arguments()->length() != 1) {
    return false;
  }

  auto* call_var_proxy = call->expression()->AsVariableProxy();
  if (call_var_proxy == nullptr) {
    return false;
  }

  auto* call_var_info = Lookup(call_var_proxy->var());
  if (call_var_info == nullptr) {
    return false;
  }

  return call_var_info->standard_member() == kMathFround;
}

AsmType* AsmTyper::ValidateFloatCoercion(Call* call) {
  if (!IsCallToFround(call)) {
    return nullptr;
  }

  auto* arg = call->arguments()->at(0);
  // call is a fround() node. From now, there can be two possible outcomes:
  // 1. fround is used as a return type annotation.
  if (auto* arg_as_call = arg->AsCall()) {
    RECURSE(ValidateCall(AsmType::Float(), arg_as_call));
    return AsmType::Float();
  }

  // 2. fround is used for converting to float.
  AsmType* arg_type;
  RECURSE(arg_type = ValidateExpression(arg));
  if (arg_type->IsA(AsmType::Floatish()) || arg_type->IsA(AsmType::DoubleQ()) ||
      arg_type->IsA(AsmType::Signed()) || arg_type->IsA(AsmType::Unsigned())) {
    return AsmType::Float();
  }

  FAIL(call, "Invalid argument type to fround.");
}

// 5.1 ParameterTypeAnnotations
AsmType* AsmTyper::ParameterTypeAnnotations(Variable* parameter,
                                            Expression* annotation) {
  if (auto* binop = annotation->AsBinaryOperation()) {
    // Must be:
    //   * x|0
    //   * x*1 (*VIOLATION* i.e.,, +x)
    auto* left = binop->left()->AsVariableProxy();
    if (left == nullptr) {
      FAIL(
          binop->left(),
          "Invalid parameter type annotation - should annotate an identifier.");
    }
    if (left->var() != parameter) {
      FAIL(binop->left(),
           "Invalid parameter type annotation - should annotate a parameter.");
    }
    if (IsDoubleAnnotation(binop)) {
      return AsmType::Double();
    }
    if (IsIntAnnotation(binop)) {
      return AsmType::Int();
    }
    FAIL(binop, "Invalid parameter type annotation.");
  }

  auto* call = annotation->AsCall();
  if (call == nullptr) {
    FAIL(
        annotation,
        "Invalid float parameter type annotation - must be fround(parameter).");
  }

  if (!IsCallToFround(call)) {
    FAIL(annotation,
         "Invalid float parameter type annotation - must be call to fround.");
  }

  auto* src_expr = call->arguments()->at(0)->AsVariableProxy();
  if (src_expr == nullptr) {
    FAIL(annotation,
         "Invalid float parameter type annotation - argument to fround is not "
         "an identifier.");
  }

  if (src_expr->var() != parameter) {
    FAIL(annotation,
         "Invalid float parameter type annotation - argument to fround is not "
         "a parameter.");
  }

  return AsmType::Float();
}

// 5.2 ReturnTypeAnnotations
AsmType* AsmTyper::ReturnTypeAnnotations(ReturnStatement* statement) {
  if (statement == nullptr) {
    return AsmType::Void();
  }

  auto* ret_expr = statement->expression();
  if (ret_expr == nullptr) {
    return AsmType::Void();
  }

  if (auto* binop = ret_expr->AsBinaryOperation()) {
    if (IsDoubleAnnotation(binop)) {
      return AsmType::Double();
    } else if (IsIntAnnotation(binop)) {
      return AsmType::Signed();
    }
    FAIL(statement, "Invalid return type annotation.");
  }

  if (auto* call = ret_expr->AsCall()) {
    if (IsCallToFround(call)) {
      return AsmType::Float();
    }
    FAIL(statement, "Invalid function call in return statement.");
  }

  if (auto* literal = ret_expr->AsLiteral()) {
    int32_t _;
    if (literal->raw_value()->ContainsDot()) {
      return AsmType::Double();
    } else if (literal->value()->ToInt32(&_)) {
      return AsmType::Signed();
    } else if (literal->IsUndefinedLiteral()) {
      // *VIOLATION* The parser changes
      //
      // return;
      //
      // into
      //
      // return undefined
      return AsmType::Void();
    }
    FAIL(statement, "Invalid literal in return statement.");
  }

  FAIL(statement, "Invalid return type expression.");
}

// 5.4 VariableTypeAnnotations
// Also used for 5.5 GlobalVariableTypeAnnotations
AsmType* AsmTyper::VariableTypeAnnotations(Expression* initializer) {
  if (auto* literal = initializer->AsLiteral()) {
    if (literal->raw_value()->ContainsDot()) {
      return AsmType::Double();
    }
    int32_t i32;
    uint32_t u32;
    if (literal->value()->ToInt32(&i32) || literal->value()->ToUint32(&u32)) {
      return AsmType::Int();
    }
    FAIL(initializer, "Invalid type annotation - forbidden literal.");
  }

  auto* call = initializer->AsCall();
  DCHECK(call != nullptr);
  if (call == nullptr) {
    FAIL(initializer,
         "Invalid variable initialization - it should be a literal, or "
         "fround(literal).");
  }

  if (!IsCallToFround(call)) {
    FAIL(initializer,
         "Invalid float coercion - expected call fround(literal).");
  }

  auto* src_expr = call->arguments()->at(0)->AsLiteral();
  if (src_expr == nullptr) {
    FAIL(initializer,
         "Invalid float type annotation - expected literal argument for call "
         "to fround.");
  }

  if (!src_expr->raw_value()->ContainsDot()) {
    FAIL(initializer,
         "Invalid float type annotation - expected literal argument to be a "
         "floating point literal.");
  }

  return AsmType::Float();
}

// 5.5 GlobalVariableTypeAnnotations
AsmType* AsmTyper::NewHeapView(CallNew* new_heap_view) {
  auto* heap_type = new_heap_view->expression()->AsProperty();
  if (heap_type == nullptr) {
    FAIL(new_heap_view, "Invalid type after new.");
  }
  auto* heap_view_info = ImportLookup(heap_type);

  if (heap_view_info == nullptr) {
    FAIL(new_heap_view, "Unknown stdlib member in heap view declaration.");
  }

  if (!heap_view_info->type()->IsA(AsmType::Heap())) {
    FAIL(new_heap_view, "Type is not a heap view type.");
  }

  if (new_heap_view->arguments()->length() != 1) {
    FAIL(new_heap_view, "Invalid number of arguments when creating heap view.");
  }

  auto* heap = new_heap_view->arguments()->at(0);
  auto* heap_var_proxy = heap->AsVariableProxy();

  if (heap_var_proxy == nullptr) {
    FAIL(heap,
         "Heap view creation parameter should be the module's heap parameter.");
  }

  auto* heap_var_info = Lookup(heap_var_proxy->var());

  if (heap_var_info == nullptr) {
    FAIL(heap, "Undeclared identifier instead of heap parameter.");
  }

  if (!heap_var_info->IsHeap()) {
    FAIL(heap,
         "Heap view creation parameter should be the module's heap parameter.");
  }

  DCHECK(heap_view_info->type()->IsA(AsmType::Heap()));
  return heap_view_info->type();
}

bool IsValidAsm(Isolate* isolate, Zone* zone, Script* script,
                FunctionLiteral* root, std::string* error_message) {
  error_message->clear();

  AsmTyper typer(isolate, zone, script, root);
  if (typer.Validate()) {
    return true;
  }

  *error_message = typer.error_message();
  return false;
}

}  // namespace wasm
}  // namespace internal
}  // namespace v8
