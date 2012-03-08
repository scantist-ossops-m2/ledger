/*
 * Copyright (c) 2003-2012, John Wiegley.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 * - Redistributions of source code must retain the above copyright
 *   notice, this list of conditions and the following disclaimer.
 *
 * - Redistributions in binary form must reproduce the above copyright
 *   notice, this list of conditions and the following disclaimer in the
 *   documentation and/or other materials provided with the distribution.
 *
 * - Neither the name of New Artisans LLC nor the names of its
 *   contributors may be used to endorse or promote products derived from
 *   this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <system.hh>

#include "op.h"
#include "scope.h"
#include "commodity.h"
#include "pool.h"

namespace ledger {

void intrusive_ptr_add_ref(const expr_t::op_t * op)
{
  op->acquire();
}

void intrusive_ptr_release(const expr_t::op_t * op)
{
  op->release();
}

namespace {
  value_t split_cons_expr(expr_t::ptr_op_t op)
  {
    if (op->kind == expr_t::op_t::O_CONS) {
      value_t seq;
      seq.push_back(expr_value(op->left()));

      expr_t::ptr_op_t next = op->right();
      while (next) {
        expr_t::ptr_op_t value_op;
        if (next->kind == expr_t::op_t::O_CONS) {
          value_op = next->left();
          next     = next->has_right() ? next->right() : NULL;
        } else {
          value_op = next;
          next     = NULL;
        }
        seq.push_back(expr_value(value_op));
      }
      return seq;
    } else {
      return expr_value(op);
    }
  }

  inline void check_type_context(scope_t& scope, value_t& result)
  {
    if (scope.type_required() &&
        scope.type_context() != value_t::VOID &&
        result.type() != scope.type_context()) {
      throw_(calc_error,
             _("Expected return of %1, but received %2")
             << result.label(scope.type_context())
             << result.label());
    }
  }
}

expr_t::ptr_op_t expr_t::op_t::compile(scope_t& scope, const int depth,
                                       scope_t * param_scope)
{
  scope_t *           scope_ptr = &scope;
  unique_ptr<scope_t> bound_scope;
  expr_t::ptr_op_t    result;

#if defined(DEBUG_ON)
  if (SHOW_DEBUG("expr.compile")) {
    for (int i = 0; i < depth; i++)
      ledger::_log_buffer << '.';
    DEBUG("expr.compile", "");
  }
#endif

  assert(kind < LAST);

  if (is_ident()) {
    DEBUG("expr.compile", "Lookup: " << as_ident() << " in " << scope_ptr);
    ptr_op_t def;
    if (param_scope)
      def = param_scope->lookup(symbol_t::FUNCTION, as_ident());
    if (! def)
      def = scope_ptr->lookup(symbol_t::FUNCTION, as_ident());
    if (def) {
      // Identifier references are first looked up at the point of
      // definition, and then at the point of every use if they could
      // not be found there.
#if defined(DEBUG_ON)
      if (SHOW_DEBUG("expr.compile")) {
        DEBUG("expr.compile", "Found definition:");
        def->dump(*_log_stream, 0);
      }
#endif // defined(DEBUG_ON)
      result = copy(def);
    }
    else if (left()) {
      result = copy();
    }
    else {
      result = this;
    }
  }
  else if (is_scope()) {
    shared_ptr<scope_t> subscope(new symbol_scope_t(*scope_t::empty_scope));
    set_scope(subscope);
    bound_scope.reset(new bind_scope_t(*scope_ptr, *subscope.get()));
    scope_ptr = bound_scope.get();
  }
  else if (kind < TERMINALS) {
    result = this;
  }
  else if (kind == O_DEFINE) {
    switch (left()->kind) {
    case IDENT: {
      ptr_op_t node(right()->compile(*scope_ptr, depth + 1, param_scope));

      DEBUG("expr.compile",
            "Defining " << left()->as_ident() << " in " << scope_ptr);
      scope_ptr->define(symbol_t::FUNCTION, left()->as_ident(), node);
      break;
    }

    case O_CALL:
      if (left()->left()->is_ident()) {
        ptr_op_t node(new op_t(op_t::O_LAMBDA));
        node->set_left(left()->right());
        node->set_right(right());

        node = node->compile(*scope_ptr, depth + 1, param_scope);

        DEBUG("expr.compile",
              "Defining " << left()->left()->as_ident() << " in " << scope_ptr);
        scope_ptr->define(symbol_t::FUNCTION, left()->left()->as_ident(), node);
        break;
      }
      // fall through...

    default:
      throw_(compile_error, _("Invalid function definition"));
    }
    result = wrap_value(NULL_VALUE);
  }
  else if (kind == O_LAMBDA) {
    symbol_scope_t params(param_scope ? *param_scope : *scope_t::empty_scope);

    for (ptr_op_t sym = left();
         sym;
         sym = sym->has_right() ? sym->right() : NULL) {
      ptr_op_t varname = sym->kind == O_CONS ? sym->left() : sym;

      if (! varname->is_ident()) {
        throw_(calc_error, _("Invalid function or lambda parameter"));
      } else {
        DEBUG("expr.compile",
              "Defining function parameter " << varname->as_ident());
        params.define(symbol_t::FUNCTION, varname->as_ident(),
                      new op_t(PLUG));
      }
    }

    ptr_op_t rhs(right()->compile(*scope_ptr, depth + 1, &params));
    if (rhs == right())
      result = this;
    else
      result = copy(left(), rhs);
  }

  if (! result) {
    ptr_op_t lhs(left()->compile(*scope_ptr, depth + 1, param_scope));
    ptr_op_t rhs(kind > UNARY_OPERATORS && has_right() ?
                 (kind == O_LOOKUP ? right() :
                  right()->compile(*scope_ptr, depth + 1, param_scope)) : NULL);

    if (lhs == left() && (! rhs || rhs == right())) {
      result = this;
    } else {
      ptr_op_t intermediate(copy(lhs, rhs));

      // Reduce constants immediately if possible
      if ((! lhs || lhs->is_value()) && (! rhs || rhs->is_value()))
        result = wrap_value(intermediate->calc(*scope_ptr, NULL, depth + 1));
      else
        result = intermediate;
    }
  }

#if defined(DEBUG_ON)
  if (SHOW_DEBUG("expr.compile")) {
    for (int i = 0; i < depth; i++)
      ledger::_log_buffer << '.';
    DEBUG("expr.compile", "");
  }
#endif

  return result;
}

value_t expr_t::op_t::calc(scope_t& scope, ptr_op_t * locus, const int depth)
{
  try {

  value_t result;

#if defined(DEBUG_ON)
  if (SHOW_DEBUG("expr.calc")) {
    for (int i = 0; i < depth; i++)
      ledger::_log_buffer << '.';
    ledger::_log_buffer << op_context(this) << " => ...";
    DEBUG("expr.calc", "");
  }
#endif

  switch (kind) {
  case VALUE:
    result = as_value();
    break;

  case O_DEFINE:
    result = NULL_VALUE;
    break;

  case IDENT: {
    ptr_op_t definition = left();
    // If no definition was pre-compiled for this identifier, look it up
    // in the current scope.
    if (! definition || definition->kind == PLUG) {
      DEBUG("scope.symbols", "Looking for IDENT '" << as_ident() << "'");
      definition = scope.lookup(symbol_t::FUNCTION, as_ident());
    }
    if (! definition)
      throw_(calc_error, _("Unknown identifier '%1'") << as_ident());

    // Evaluating an identifier is the same as calling its definition
    // directly
    result = definition->calc(scope, locus, depth + 1);
    check_type_context(scope, result);
    break;
  }

  case FUNCTION: {
    // Evaluating a FUNCTION is the same as calling it directly; this
    // happens when certain functions-that-look-like-variables (such as
    // "amount") are resolved.
    call_scope_t call_args(scope, locus, depth + 1);
    result = as_function()(call_args);
    check_type_context(scope, result);
    break;
  }

  case SCOPE:
    assert(! is_scope_unset());
    if (is_scope_unset()) {
      symbol_scope_t subscope(scope);
      result = left()->calc(subscope, locus, depth + 1);
    } else {
      bind_scope_t bound_scope(scope, *as_scope());
      result = left()->calc(bound_scope, locus, depth + 1);
    }
    break;

  case O_LOOKUP: {
    context_scope_t context_scope(scope, value_t::SCOPE);
    bool scope_error = true;
    if (value_t obj = left()->calc(context_scope, locus, depth + 1)) {
      if (obj.is_scope() && obj.as_scope() != NULL) {
        bind_scope_t bound_scope(scope, *obj.as_scope());
        result = right()->calc(bound_scope, locus, depth + 1);
        scope_error = false;
      }
    }
    if (scope_error)
      throw_(calc_error, _("Left operand does not evaluate to an object"));
    break;
  }

  case O_CALL: {
    ptr_op_t func = left();
    string   name;

    if (func->is_ident()) {
      name = func->as_ident();
      func = func->left();
      if (! func)
        func = scope.lookup(symbol_t::FUNCTION, name);
      if (! func)
        throw_(calc_error, _("Calling unknown function '%1'") << name);
    } else {
      name = "<lambda>";
    }

    call_scope_t call_args(scope, locus, depth + 1);
    if (has_right())
      call_args.set_args(split_cons_expr(right()));

    try {
      if (func->is_function())
        result = func->as_function()(call_args);
      else
        result = func->calc(call_args, locus, depth + 1);
    }
    catch (const std::exception&) {
      add_error_context(_("While calling function '%1':" << name));
      throw;
    }

    check_type_context(scope, result);
    break;
  }

  case O_LAMBDA: {
    call_scope_t&  call_args(find_scope<call_scope_t>(scope, true));
    std::size_t    args_count(call_args.size());
    std::size_t    args_index(0);
    symbol_scope_t args_scope(*scope_t::empty_scope);

    for (ptr_op_t sym = left();
         sym;
         sym = sym->has_right() ? sym->right() : NULL) {
      ptr_op_t varname = sym->kind == O_CONS ? sym->left() : sym;
      if (! varname->is_ident()) {
        throw_(calc_error, _("Invalid function definition"));
      }
      else if (args_index == args_count) {
        DEBUG("expr.compile", "Defining function argument as null: "
              << varname->as_ident());
        args_scope.define(symbol_t::FUNCTION, varname->as_ident(),
                          wrap_value(NULL_VALUE));
      }
      else {
        DEBUG("expr.compile", "Defining function argument from call_args: "
              << varname->as_ident());
        args_scope.define(symbol_t::FUNCTION, varname->as_ident(),
                          wrap_value(call_args[args_index++]));
      }
    }

    if (args_index < args_count)
      throw_(calc_error,
             _("Too few arguments in function call (saw %1, wanted %2)")
             << args_count << args_index);

    if (right()->is_scope()) {
      bind_scope_t outer_scope(scope, *right()->as_scope());
      bind_scope_t bound_scope(outer_scope, args_scope);
      result = right()->left()->calc(bound_scope, locus, depth + 1);
    } else {
      result = right()->calc(args_scope, locus, depth + 1);
    }
    break;
  }

  case O_MATCH:
    result = (right()->calc(scope, locus, depth + 1).as_mask()
              .match(left()->calc(scope, locus, depth + 1).to_string()));
    break;

  case O_EQ:
    result = (left()->calc(scope, locus, depth + 1) ==
              right()->calc(scope, locus, depth + 1));
    break;
  case O_LT:
    result = (left()->calc(scope, locus, depth + 1) <
              right()->calc(scope, locus, depth + 1));
    break;
  case O_LTE:
    result = (left()->calc(scope, locus, depth + 1) <=
              right()->calc(scope, locus, depth + 1));
    break;
  case O_GT:
    result = (left()->calc(scope, locus, depth + 1) >
              right()->calc(scope, locus, depth + 1));
    break;
  case O_GTE:
    result = (left()->calc(scope, locus, depth + 1) >=
              right()->calc(scope, locus, depth + 1));
    break;

  case O_ADD:
    result = (left()->calc(scope, locus, depth + 1) +
              right()->calc(scope, locus, depth + 1));
    break;
  case O_SUB:
    result = (left()->calc(scope, locus, depth + 1) -
              right()->calc(scope, locus, depth + 1));
    break;
  case O_MUL:
    result = (left()->calc(scope, locus, depth + 1) *
              right()->calc(scope, locus, depth + 1));
    break;
  case O_DIV:
    result = (left()->calc(scope, locus, depth + 1) /
              right()->calc(scope, locus, depth + 1));
    break;

  case O_NEG:
    result = left()->calc(scope, locus, depth + 1).negated();
    break;

  case O_NOT:
    result = ! left()->calc(scope, locus, depth + 1);
    break;

  case O_AND:
    if (left()->calc(scope, locus, depth + 1))
      result = right()->calc(scope, locus, depth + 1);
    else
      result = false;
    break;

  case O_OR:
    if (value_t temp = left()->calc(scope, locus, depth + 1))
      result = temp;
    else
      result = right()->calc(scope, locus, depth + 1);
    break;

  case O_QUERY:
    assert(right());
    assert(right()->kind == O_COLON);
    if (value_t temp = left()->calc(scope, locus, depth + 1))
      result = right()->left()->calc(scope, locus, depth + 1);
    else
      result = right()->right()->calc(scope, locus, depth + 1);
    break;

  case O_COLON:
    assert("We should never calculate an O_COLON operator" == NULL);
    break;

  case O_CONS:
    result = left()->calc(scope, locus, depth + 1);
    if (has_right()) {
      value_t temp;
      temp.push_back(result);

      ptr_op_t next = right();
      while (next) {
        ptr_op_t value_op;
        if (next->kind == O_CONS) {
          value_op = next->left();
          next     = next->has_right() ? next->right() : NULL;
        } else {
          value_op = next;
          next     = NULL;
        }
        temp.push_back(value_op->calc(scope, locus, depth + 1));
      }
      result = temp;
    }
    break;

  case O_SEQ: {
    // An O_SEQ is very similar to an O_CONS except that only the last
    // result value in the series is kept.  O_CONS builds up a list.
    //
    // Another feature of O_SEQ is that it pushes a new symbol scope
    // onto the stack.  We evaluate the left side here to catch any
    // side-effects, such as definitions in the case of 'x = 1; x'.
    result = left()->calc(scope, locus, depth + 1);
    if (has_right()) {
      ptr_op_t next = right();
      while (next) {
        ptr_op_t value_op;
        if (next->kind == O_SEQ) {
          value_op = next->left();
          next     = next->right();
        } else {
          value_op = next;
          next     = NULL;
        }
        result = value_op->calc(scope, locus, depth + 1);
      }
    }
    break;
  }

  default:
    throw_(calc_error, _("Unexpected expr node '%1'") << op_context(this));
  }

#if defined(DEBUG_ON)
  if (SHOW_DEBUG("expr.calc")) {
    for (int i = 0; i < depth; i++)
      ledger::_log_buffer << '.';
    ledger::_log_buffer << op_context(this) <<  " => ";
    result.dump(ledger::_log_buffer, true);
    DEBUG("expr.calc", "");
  }
#endif

  return result;

  }
  catch (const std::exception&) {
    if (locus && ! *locus)
      *locus = this;
    throw;
  }
}

namespace {
  bool print_cons(std::ostream& out, const expr_t::const_ptr_op_t op,
                  const expr_t::op_t::context_t& context)
  {
    bool found = false;

    assert(op->left());
    if (op->left()->print(out, context))
      found = true;

    if (op->has_right()) {
      out << ", ";
      if (op->right()->kind == expr_t::op_t::O_CONS)
        found = print_cons(out, op->right(), context);
      else if (op->right()->print(out, context))
        found = true;
    }
    return found;
  }

  bool print_seq(std::ostream& out, const expr_t::const_ptr_op_t op,
                 const expr_t::op_t::context_t& context)
  {
    bool found = false;

    assert(op->left());
    if (op->left()->print(out, context))
      found = true;

    if (op->has_right()) {
      out << "; ";
      if (op->right()->kind == expr_t::op_t::O_SEQ)
        found = print_seq(out, op->right(), context);
      else if (op->right()->print(out, context))
        found = true;
    }

    return found;
  }
}

bool expr_t::op_t::print(std::ostream& out, const context_t& context) const
{
  bool found = false;

  if (context.start_pos && this == context.op_to_find) {
    *context.start_pos = out.tellp();
    *context.start_pos -= 1;
    found = true;
  }

  string symbol;

  if (kind > TERMINALS && (kind != O_CALL && kind != O_DEFINE))
    out << '(';

  switch (kind) {
  case VALUE:
    as_value().dump(out, context.relaxed);
    break;

  case IDENT:
    out << as_ident();
    break;

  case FUNCTION:
    out << "<FUNCTION>";
    break;

  case SCOPE:
    if (left() && left()->print(out, context))
      found = true;
    break;

  case O_NOT:
    out << "! ";
    if (left() && left()->print(out, context))
      found = true;
    break;
  case O_NEG:
    out << "- ";
    if (left() && left()->print(out, context))
      found = true;
    break;

  case O_ADD:
    if (left() && left()->print(out, context))
      found = true;
    out << " + ";
    if (has_right() && right()->print(out, context))
      found = true;
    break;
  case O_SUB:
    if (left() && left()->print(out, context))
      found = true;
    out << " - ";
    if (has_right() && right()->print(out, context))
      found = true;
    break;
  case O_MUL:
    if (left() && left()->print(out, context))
      found = true;
    out << " * ";
    if (has_right() && right()->print(out, context))
      found = true;
    break;
  case O_DIV:
    if (left() && left()->print(out, context))
      found = true;
    out << " / ";
    if (has_right() && right()->print(out, context))
      found = true;
    break;

  case O_EQ:
    if (left() && left()->print(out, context))
      found = true;
    out << " == ";
    if (has_right() && right()->print(out, context))
      found = true;
    break;
  case O_LT:
    if (left() && left()->print(out, context))
      found = true;
    out << " < ";
    if (has_right() && right()->print(out, context))
      found = true;
    break;
  case O_LTE:
    if (left() && left()->print(out, context))
      found = true;
    out << " <= ";
    if (has_right() && right()->print(out, context))
      found = true;
    break;
  case O_GT:
    if (left() && left()->print(out, context))
      found = true;
    out << " > ";
    if (has_right() && right()->print(out, context))
      found = true;
    break;
  case O_GTE:
    if (left() && left()->print(out, context))
      found = true;
    out << " >= ";
    if (has_right() && right()->print(out, context))
      found = true;
    break;

  case O_AND:
    if (left() && left()->print(out, context))
      found = true;
    out << " & ";
    if (has_right() && right()->print(out, context))
      found = true;
    break;
  case O_OR:
    if (left() && left()->print(out, context))
      found = true;
    out << " | ";
    if (has_right() && right()->print(out, context))
      found = true;
    break;

  case O_QUERY:
    if (left() && left()->print(out, context))
      found = true;
    out << " ? ";
    if (has_right() && right()->print(out, context))
      found = true;
    break;

  case O_COLON:
    if (left() && left()->print(out, context))
      found = true;
    out << " : ";
    if (has_right() && right()->print(out, context))
      found = true;
    break;

  case O_CONS:
    found = print_cons(out, this, context);
    break;
  case O_SEQ:
    found = print_seq(out, this, context);
    break;

  case O_DEFINE:
    if (left() && left()->print(out, context))
      found = true;
    out << " = ";
    if (has_right() && right()->print(out, context))
      found = true;
    break;

  case O_LOOKUP:
    if (left() && left()->print(out, context))
      found = true;
    out << ".";
    if (has_right() && right()->print(out, context))
      found = true;
    break;

  case O_LAMBDA:
    if (left() && left()->print(out, context))
      found = true;
    out << " -> ";
    if (has_right() && right()->print(out, context))
      found = true;
    break;

  case O_CALL:
    if (left() && left()->print(out, context))
      found = true;
    if (has_right()) {
      if (right()->kind == O_CONS) {
        if (right()->print(out, context))
          found = true;
      } else {
        out << "(";
        if (has_right() && right()->print(out, context))
          found = true;
        out << ")";
      }
    } else {
      out << "()";
    }
    break;

  case O_MATCH:
    if (left() && left()->print(out, context))
      found = true;
    out << " =~ ";
    if (has_right() && right()->print(out, context))
      found = true;
    break;

  case LAST:
  default:
    assert(false);
    break;
  }

  if (kind > TERMINALS && (kind != O_CALL && kind != O_DEFINE))
    out << ')';

  if (! symbol.empty()) {
    if (commodity_pool_t::current_pool->find(symbol))
      out << '@';
    out << symbol;
  }

  if (context.end_pos && this == context.op_to_find) {
    *context.end_pos = out.tellp();
    *context.end_pos -= 1;
  }

  return found;
}

void expr_t::op_t::dump(std::ostream& out, const int depth) const
{
  out.setf(std::ios::left);
  out.width((sizeof(void *) * 2) + 2);
  out << this;

  for (int i = 0; i < depth; i++)
    out << " ";

  switch (kind) {
  case PLUG:
    out << "PLUG";
    break;

  case VALUE:
    out << "VALUE: ";
    as_value().dump(out);
    break;

  case IDENT:
    out << "IDENT: " << as_ident();
    break;

  case FUNCTION:
    out << "FUNCTION";
    break;

  case SCOPE:
    out << "SCOPE: ";
    if (is_scope_unset())
      out << "null";
    else
      out << as_scope().get();
    break;

  case O_DEFINE: out << "O_DEFINE"; break;
  case O_LOOKUP: out << "O_LOOKUP"; break;
  case O_LAMBDA: out << "O_LAMBDA"; break;
  case O_CALL:   out << "O_CALL"; break;
  case O_MATCH:  out << "O_MATCH"; break;

  case O_NOT:    out << "O_NOT"; break;
  case O_NEG:    out << "O_NEG"; break;

  case O_ADD:    out << "O_ADD"; break;
  case O_SUB:    out << "O_SUB"; break;
  case O_MUL:    out << "O_MUL"; break;
  case O_DIV:    out << "O_DIV"; break;

  case O_EQ:     out << "O_EQ"; break;
  case O_LT:     out << "O_LT"; break;
  case O_LTE:    out << "O_LTE"; break;
  case O_GT:     out << "O_GT"; break;
  case O_GTE:    out << "O_GTE"; break;

  case O_AND:    out << "O_AND"; break;
  case O_OR:     out << "O_OR"; break;

  case O_QUERY:  out << "O_QUERY"; break;
  case O_COLON:  out << "O_COLON"; break;

  case O_CONS:   out << "O_CONS"; break;
  case O_SEQ:    out << "O_SEQ"; break;

  case LAST:
  default:
    assert(false);
    break;
  }

  out << " (" << refc << ')' << std::endl;

  // An identifier is a special non-terminal, in that its left() can
  // hold the compiled definition of the identifier.
  if (kind > TERMINALS || is_scope() || is_ident()) {
    if (left()) {
      left()->dump(out, depth + 1);
      if (kind > UNARY_OPERATORS && has_right())
        right()->dump(out, depth + 1);
    }
    else if (kind > UNARY_OPERATORS) {
      assert(! has_right());
    }
  }
}

string op_context(const expr_t::ptr_op_t op,
                  const expr_t::ptr_op_t locus)
{
  ostream_pos_type start_pos, end_pos;
  expr_t::op_t::context_t context(op, locus, &start_pos, &end_pos);
  std::ostringstream buf;
  buf << "  ";
  if (op->print(buf, context)) {
    buf << "\n";
    for (int i = 0; i <= end_pos; i++) {
      if (i > start_pos)
        buf << "^";
      else
        buf << " ";
    }
  }
  return buf.str();
}

} // namespace ledger
