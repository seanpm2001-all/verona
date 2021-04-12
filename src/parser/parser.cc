// Copyright Microsoft and Project Verona Contributors.
// SPDX-License-Identifier: MIT
#include "parser.h"

#include "dnf.h"
#include "escaping.h"
#include "ident.h"
#include "path.h"

#include <cassert>
#include <set>

namespace verona::parser
{
  constexpr auto ext = "verona";

  enum Result
  {
    Skip,
    Success,
    Error,
  };

  struct Parse
  {
    Source source;
    size_t pos = 0;
    size_t la = 0;
    Token previous;
    std::vector<Token> lookahead;

    Ast symbols;

    Ident ident;
    Location name_apply = ident("apply");
    Location name_create = ident("create");

    Result final_result = Success;
    std::vector<std::string> imports;
    std::string stdlib;
    std::ostream& out;

    struct SymbolPush
    {
      Parse& parser;
      bool is_done = false;

      SymbolPush(Parse& parser) : parser(parser) {}

      ~SymbolPush()
      {
        if (!is_done)
          parser.pop();
      }

      void done()
      {
        parser.pop();
        is_done = true;
      }
    };

    Parse(const std::string& stdlib, std::ostream& out)
    : stdlib(stdlib), out(out)
    {}

    void start(Source& src)
    {
      source = src;
      pos = 0;
      la = 0;
      previous = {};
      lookahead.clear();
    }

    std::ostream& error()
    {
      final_result = Error;
      return out << "--------" << std::endl;
    }

    SymbolPush push(Ast node)
    {
      assert(node->symbol_table() != nullptr);
      node->symbol_table()->parent = symbols;
      symbols = node;
      return SymbolPush(*this);
    }

    void pop()
    {
      symbols = symbols->symbol_table()->parent.lock();
    }

    void set_sym(const Location& id, Ast node)
    {
      auto st = symbols->symbol_table();
      auto prev = st->get(id);

      if (!prev)
      {
        st->set(id, node);
      }
      else
      {
        error() << node->location << "There is a previous definition of \""
                << id.view() << "\"" << text(node->location) << prev->location
                << "The previous definition is here" << text(prev->location);
      }
    }

    Node<Ref> ref(const Location& loc)
    {
      auto ref = std::make_shared<Ref>();
      ref->location = loc;
      return ref;
    }

    Location loc()
    {
      if (lookahead.size() > 0)
        return lookahead[0].location;
      else
        return previous.location;
    }

    text line()
    {
      return text(loc());
    }

    bool peek(const TokenKind& kind, const char* text = nullptr)
    {
      if (la >= lookahead.size())
        lookahead.push_back(lex(source, pos));

      assert(la < lookahead.size());

      if (lookahead[la].kind == kind)
      {
        if (!text || (lookahead[la].location == text))
        {
          next();
          return true;
        }
      }

      return false;
    }

    void next()
    {
      la++;
    }

    void rewind()
    {
      la = 0;
    }

    Token take()
    {
      assert(la == 0);

      if (lookahead.size() == 0)
        return lex(source, pos);

      previous = lookahead.front();
      lookahead.erase(lookahead.begin());
      return previous;
    }

    bool has(TokenKind kind, const char* text = nullptr)
    {
      assert(la == 0);

      if (peek(kind, text))
      {
        rewind();
        take();
        return true;
      }

      return false;
    }

    bool peek_delimited(TokenKind kind, TokenKind terminator)
    {
      // Look for `kind`, skipping over balanced () [] {}.
      while (!peek(TokenKind::End))
      {
        if (peek(kind))
          return true;

        if (peek(terminator))
          return false;

        if (peek(TokenKind::LParen))
        {
          peek_delimited(TokenKind::RParen, TokenKind::End);
        }
        else if (peek(TokenKind::LSquare))
        {
          peek_delimited(TokenKind::RSquare, TokenKind::End);
        }
        else if (peek(TokenKind::LBrace))
        {
          peek_delimited(TokenKind::RBrace, TokenKind::End);
        }
        else
        {
          next();
        }
      }

      return false;
    }

    void restart_before(const std::initializer_list<TokenKind>& kinds)
    {
      // Skip over balanced () [] {}
      while (!has(TokenKind::End))
      {
        for (auto& kind : kinds)
        {
          if (peek(kind))
          {
            rewind();
            return;
          }
        }

        if (has(TokenKind::LParen))
        {
          restart_before(TokenKind::RParen);
        }
        else if (has(TokenKind::LSquare))
        {
          restart_before(TokenKind::RSquare);
        }
        else if (has(TokenKind::LBrace))
        {
          restart_before(TokenKind::RBrace);
        }
        else
        {
          take();
        }
      }
    }

    void restart_after(const std::initializer_list<TokenKind>& kinds)
    {
      restart_before(kinds);
      take();
    }

    void restart_before(TokenKind kind)
    {
      restart_before({kind});
    }

    void restart_after(TokenKind kind)
    {
      restart_after({kind});
    }

    Result optwhen(Node<Expr>& expr)
    {
      // when <- 'when' postfix lambda
      if (!has(TokenKind::When))
        return Skip;

      Result r = Success;
      auto when = std::make_shared<When>();
      when->location = previous.location;
      expr = when;

      if (optpostfix(when->waitfor) != Success)
      {
        error() << loc() << "Expected a when condition" << line();
        r = Error;
      }

      if (optlambda(when->behaviour) != Success)
      {
        error() << loc() << "Expected a when body" << line();
        r = Error;
      }

      return r;
    }

    Result opttry(Node<Expr>& expr)
    {
      // try <- 'try' lambda 'catch' '{' lambda* '}'
      if (!has(TokenKind::Try))
        return Skip;

      Result r = Success;
      auto tr = std::make_shared<Try>();
      tr->location = previous.location;
      expr = tr;

      if (optlambda(tr->body) != Success)
      {
        error() << loc() << "Expected a try block" << line();
        r = Error;
      }

      auto& body = tr->body->as<Lambda>();

      if (!body.typeparams.empty())
      {
        error() << body.typeparams.front()->location
                << "A try block can't have type parameters"
                << text(body.typeparams.front()->location);
        r = Error;
      }

      if (!body.params.empty())
      {
        error() << body.params.front()->location
                << "A try block can't have parameters"
                << text(body.params.front()->location);
        r = Error;
      }

      if (!has(TokenKind::Catch))
      {
        error() << loc() << "Expected a catch block" << line();
        return Error;
      }

      if (!has(TokenKind::LBrace))
      {
        error() << loc() << "Expected a {" << line();
        return Error;
      }

      while (true)
      {
        Node<Expr> clause;
        Result r2;

        if ((r2 = optlambda(clause)) == Skip)
          break;

        tr->catches.push_back(clause);

        if (r2 == Error)
          r = Error;
      }

      if (!has(TokenKind::RBrace))
      {
        error() << loc() << "Expected a }" << line();
        return Error;
      }

      return r;
    }

    Result optmatch(Node<Expr>& expr)
    {
      // match <- 'match' postfix '{' lambda* '}'
      if (!has(TokenKind::Match))
        return Skip;

      Result r = Success;
      auto match = std::make_shared<Match>();
      match->location = previous.location;
      expr = match;

      if (optpostfix(match->test) != Success)
      {
        error() << loc() << "Expected a match test-expression" << line();
        r = Error;
      }

      if (!has(TokenKind::LBrace))
      {
        error() << loc() << "Expected { to start match cases" << line();
        return Error;
      }

      while (!has(TokenKind::RBrace))
      {
        if (has(TokenKind::End))
        {
          error() << loc() << "Expected a case or } to end match cases"
                  << line();
          r = Error;
          break;
        }

        Node<Expr> clause;
        Result r2 = optlambda(clause);

        if (r2 == Skip)
          break;

        match->cases.push_back(clause);

        if (r2 == Error)
          r = Error;
      }

      return r;
    }

    Result opttuple(Node<Expr>& expr)
    {
      // tuple <- '(' (expr (',' expr)*)? ')'
      if (!has(TokenKind::LParen))
        return Skip;

      auto tup = std::make_shared<Tuple>();
      tup->location = previous.location;
      expr = tup;

      if (has(TokenKind::RParen))
        return Success;

      Result r = Success;

      do
      {
        Node<Expr> elem;
        Result r2;

        if ((r2 = optexpr(elem)) == Skip)
          break;

        if (r2 == Error)
        {
          error() << loc() << "Expected an expression" << line();
          restart_before({TokenKind::Comma, TokenKind::RParen});
          r = Error;
        }

        tup->seq.push_back(elem);
      } while (has(TokenKind::Comma));

      if (!has(TokenKind::RParen))
      {
        error() << loc() << "Expected , or )" << line();
        r = Error;
      }

      tup->location.extend(previous.location);
      return r;
    }

    Result optlambda(Node<Expr>& expr, bool is_func = false)
    {
      // lambda <-
      //  '{' (typeparams? (param (',' param)*)? '=>')? (expr ';'*)* '}'
      if (!has(TokenKind::LBrace))
        return Skip;

      Node<Lambda> lambda;

      if (is_func)
      {
        lambda = std::static_pointer_cast<Lambda>(expr);
      }
      else
      {
        lambda = std::make_shared<Lambda>();
        lambda->result = std::make_shared<InferType>();
      }

      lambda->location = previous.location;
      auto st = push(lambda);
      expr = lambda;

      Result r = opttypeparams(lambda->typeparams);

      if (is_func && (r != Skip))
      {
        error()
          << lambda->typeparams.back()->location
          << "Function type parameters can't be placed in lambda position."
          << text(lambda->typeparams.back()->location);
      }

      bool has_fatarrow = true;

      if (r == Skip)
      {
        has_fatarrow = peek_delimited(TokenKind::FatArrow, TokenKind::RBrace);
        r = Success;
        rewind();
      }

      if (has_fatarrow)
      {
        Result r2 = optparamlist(lambda->params, TokenKind::FatArrow);

        if (is_func && (r2 != Skip))
        {
          error() << lambda->params.back()->location
                  << "Function parameters can't be placed in lambda position."
                  << text(lambda->params.back()->location);
        }

        if (r2 == Error)
          r = Error;

        if (!has(TokenKind::FatArrow))
        {
          error() << loc() << "Expected =>" << line();
          r = Error;
        }
      }

      while (!has(TokenKind::RBrace))
      {
        if (has(TokenKind::End))
        {
          error() << lambda->location << "Unexpected EOF in lambda body"
                  << line();
          return Error;
        }

        Node<Expr> expr;
        Result r2 = optexpr(expr);

        if (r2 == Skip)
          break;

        // TODO: `using`
        lambda->body.push_back(expr);

        if (r2 == Error)
          r = Error;

        while (has(TokenKind::Semicolon))
          ;
      }

      return r;
    }

    Result optref(Node<Expr>& expr)
    {
      // ref <- [local] ident oftype?
      if (!peek(TokenKind::Ident))
        return Skip;

      auto def = symbols->symbol_table()->get_scope(lookahead[la - 1].location);
      bool local = def && is_kind(def, {Kind::Param, Kind::Let, Kind::Var});
      rewind();

      if (!local)
        return Skip;

      if (!has(TokenKind::Ident))
        return Skip;

      auto ref = std::make_shared<Ref>();
      ref->location = previous.location;
      expr = ref;
      return Success;
    }

    Result optconstant(Node<Expr>& expr)
    {
      // constant <-
      //  escapedstring / unescapedstring / character /
      //  float / int / hex / binary / 'true' / 'false'
      if (has(TokenKind::EscapedString))
        expr = std::make_shared<EscapedString>();
      else if (has(TokenKind::UnescapedString))
        expr = std::make_shared<UnescapedString>();
      else if (has(TokenKind::Character))
        expr = std::make_shared<Character>();
      else if (has(TokenKind::Int))
        expr = std::make_shared<Int>();
      else if (has(TokenKind::Float))
        expr = std::make_shared<Float>();
      else if (has(TokenKind::Hex))
        expr = std::make_shared<Hex>();
      else if (has(TokenKind::Binary))
        expr = std::make_shared<Binary>();
      else if (has(TokenKind::Bool))
        expr = std::make_shared<Bool>();
      else
        return Skip;

      expr->location = previous.location;
      return Success;
    }

    Result objectliteral(Node<Expr>& expr)
    {
      // new <- 'new' (typebody / type typebody) ('@' ident)?
      Result r = Success;
      auto obj = std::make_shared<ObjectLiteral>();
      auto st = push(obj);
      obj->location = previous.location;
      expr = obj;

      if (has(TokenKind::Symbol, "@"))
      {
        if (has(TokenKind::Ident))
        {
          obj->in = previous.location;
        }
        else
        {
          error() << loc() << "Expected an identifier" << line();
          r = Error;
        }
      }

      bool inherits = !peek(TokenKind::LBrace);
      rewind();

      if (inherits)
      {
        if (typeexpr(obj->inherits) == Error)
          r = Error;

        if (checkinherit(obj->inherits) == Error)
          r = Error;
      }

      if (typebody(obj->members) != Success)
        r = Error;

      return r;
    }

    Result optnew(Node<Expr>& expr)
    {
      // new <- 'new' ('@' ident)? (tuple / typebody / type typebody)
      if (!has(TokenKind::New))
        return Skip;

      bool ctor = peek(TokenKind::LParen) ||
        (peek(TokenKind::Symbol, "@") && peek(TokenKind::Ident) &&
         peek(TokenKind::LParen));
      rewind();

      if (!ctor)
        return objectliteral(expr);

      // ctor <- 'new' tuple ('@' ident)?
      Result r = Success;
      auto n = std::make_shared<New>();
      n->location = previous.location;
      expr = n;

      if (has(TokenKind::Symbol, "@"))
      {
        if (has(TokenKind::Ident))
        {
          n->in = previous.location;
        }
        else
        {
          error() << loc() << "Expected an identifier" << line();
          r = Error;
        }
      }

      if (opttuple(n->args) != Success)
        r = Error;

      return r;
    }

    Result optatom(Node<Expr>& expr)
    {
      // atom <- tuple / constant / new / when / try / match / lambda
      Result r;

      if ((r = opttuple(expr)) != Skip)
        return r;

      if ((r = optconstant(expr)) != Skip)
        return r;

      if ((r = optnew(expr)) != Skip)
        return r;

      if ((r = optwhen(expr)) != Skip)
        return r;

      if ((r = opttry(expr)) != Skip)
        return r;

      if ((r = optmatch(expr)) != Skip)
        return r;

      if ((r = optlambda(expr)) != Skip)
        return r;

      return Skip;
    }

    Result opttypeargs(List<Type>& typeargs)
    {
      // typeargs <- '[' type (',' type)* ']'
      if (!has(TokenKind::LSquare))
        return Skip;

      Result r = Success;

      do
      {
        Node<Type> arg;

        if (typeexpr(arg) != Success)
        {
          restart_before({TokenKind::Comma, TokenKind::RSquare});
          r = Error;
        }

        typeargs.push_back(arg);
      } while (has(TokenKind::Comma));

      if (!has(TokenKind::RSquare))
      {
        error() << loc() << "Expected , or ]" << line();
        r = Error;
      }

      return r;
    }

    Result optselector(Node<Expr>& expr)
    {
      // selector <- name typeargs? ('::' name typeargs?)*
      bool ok = peek(TokenKind::Ident) || peek(TokenKind::Symbol);
      rewind();

      if (!ok)
        return Skip;

      Result r = Success;

      // This keeps expr as the lhs of the selector.
      auto sel = std::make_shared<Select>();
      sel->expr = expr;
      expr = sel;

      Node<Type> type;

      if (opttyperef(type) != Success)
        r = Error;

      sel->typeref = std::static_pointer_cast<TypeRef>(type);
      sel->location = sel->typeref->location;
      return r;
    }

    Result optselect(Node<Expr>& expr)
    {
      // select <- '.' selector tuple?
      if (!has(TokenKind::Dot))
        return Skip;

      Result r = Success;

      // This keeps expr as the lhs of the selector.
      if (optselector(expr) != Success)
      {
        error() << loc() << "Expected a selector" << line();
        r = Error;
      }

      if (opttuple(expr->as<Select>().args) == Error)
        r = Error;

      return r;
    }

    Result optapplysugar(Node<Expr>& expr)
    {
      // applysugar <- ref typeargs? tuple?
      Result r;

      if ((r = optref(expr)) == Skip)
        return r;

      bool ok = peek(TokenKind::LSquare) || peek(TokenKind::LParen);
      rewind();

      if (!ok)
        return r;

      auto apply = std::make_shared<TypeName>();
      apply->location = name_apply;

      if (opttypeargs(apply->typeargs) == Error)
        r = Error;

      auto tr_apply = std::make_shared<TypeRef>();
      tr_apply->location = apply->location;
      tr_apply->typenames.push_back(apply);

      auto sel = std::make_shared<Select>();
      sel->location = apply->location;
      sel->expr = expr;
      sel->typeref = tr_apply;

      if (opttuple(sel->args) == Error)
        r = Error;

      expr = sel;
      return r;
    }

    Result optpostfixstart(Node<Expr>& expr)
    {
      // postfixstart <- atom / applysugar
      Result r;

      if ((r = optatom(expr)) != Skip)
        return r;

      if ((r = optapplysugar(expr)) != Skip)
        return r;

      return Skip;
    }

    Result optpostfix(Node<Expr>& expr)
    {
      // postfix <- postfixstart select*
      Result r;
      Result r2;

      if ((r = optpostfixstart(expr)) == Skip)
        return Skip;

      while ((r2 = optselect(expr)) != Skip)
      {
        if (r2 == Error)
          r = Error;
      }

      return r;
    }

    Result optinfix(Node<Expr>& expr)
    {
      // infix <- (postfix / selector)+
      Result r = Success;

      while (true)
      {
        Result r2;
        Node<Expr> next;

        if ((r2 = optpostfix(next)) != Skip)
        {
          if (!expr)
          {
            // This is the first element in an expression.
            expr = next;
          }
          else if ((expr->kind() == Kind::Select) && !expr->as<Select>().args)
          {
            // This is the right-hand side of an infix operator.
            expr->as<Select>().args = next;
          }
          else
          {
            // Adjacency means `expr.apply(next)`
            auto sel = std::make_shared<Select>();
            sel->expr = expr;
            sel->args = next;
            expr = sel;

            auto apply = std::make_shared<TypeName>();
            apply->location = name_apply;

            auto tr_apply = std::make_shared<TypeRef>();
            tr_apply->location = expr->location;
            tr_apply->typenames.push_back(apply);

            sel->typeref = tr_apply;
            sel->location = tr_apply->location;
          }
        }
        else if ((r2 = optselector(expr)) != Skip)
        {
          // This keeps expr as the lhs of the selector.
          if (r2 == Error)
            r = Error;
        }
        else
        {
          break;
        }
      }

      if (!expr)
        return Skip;

      return r;
    }

    template<typename T>
    Result decl(Node<Expr>& expr)
    {
      if (!has(TokenKind::Ident))
      {
        error() << loc() << "Expected an identifier" << line();
        return Error;
      }

      auto decl = std::make_shared<T>();
      decl->location = previous.location;
      decl->type = std::make_shared<InferType>();
      set_sym(decl->location, decl);
      expr = decl;
      return Success;
    }

    Result optlet(Node<Expr>& expr)
    {
      if (!has(TokenKind::Let))
        return Skip;

      return decl<Let>(expr);
    }

    Result optvar(Node<Expr>& expr)
    {
      if (!has(TokenKind::Var))
        return Skip;

      return decl<Var>(expr);
    }

    Result optthrow(Node<Expr>& expr)
    {
      if (!has(TokenKind::Throw))
        return Skip;

      Result r = Success;
      auto thr = std::make_shared<Throw>();
      thr->location = previous.location;
      expr = thr;

      if ((r = optexpr(thr->expr)) == Skip)
      {
        error() << loc() << "Expected a throw expression" << line();
        r = Error;
      }

      return r;
    }

    Result optexprstart(Node<Expr>& expr)
    {
      // exprstart <- decl / throw / infix
      Result r;

      if ((r = optlet(expr)) != Skip)
        return r;

      if ((r = optvar(expr)) != Skip)
        return r;

      if ((r = optthrow(expr)) != Skip)
        return r;

      if ((r = optinfix(expr)) != Skip)
        return r;

      return Skip;
    }

    Result optexpr(Node<Expr>& expr)
    {
      // expr <- exprstart oftype? ('=' expr)?
      Result r;

      if ((r = optexprstart(expr)) == Skip)
        return Skip;

      if (peek(TokenKind::Colon))
      {
        rewind();
        auto ot = std::make_shared<Oftype>();
        ot->expr = expr;
        expr = ot;

        if (oftype(ot->type) != Success)
          r = Error;
      }

      if (has(TokenKind::Equals))
      {
        auto asgn = std::make_shared<Assign>();
        asgn->location = previous.location;
        asgn->left = expr;
        expr = asgn;

        if (optexpr(asgn->right) != Success)
        {
          error() << loc() << "Expected an expression on the right-hand side"
                  << line();
          r = Error;
        }
      }

      return r;
    }

    Result initexpr(Node<Expr>& expr)
    {
      // initexpr <- '=' expr
      if (!has(TokenKind::Equals))
        return Skip;

      Result r;

      // Encode an initexpr as a zero-argument lambda
      auto lambda = std::make_shared<Lambda>();
      lambda->location = previous.location;
      lambda->result = std::make_shared<InferType>();
      expr = lambda;

      auto st = push(lambda);
      Node<Expr> init;

      if ((r = optexpr(init)) != Skip)
      {
        lambda->body.push_back(init);
      }
      else
      {
        error() << loc() << "Expected an initialiser expression" << line();
        r = Error;
      }

      return r;
    }

    Result opttupletype(Node<Type>& type)
    {
      // tupletype <- '(' (type (',' type)*)? ')'
      if (!has(TokenKind::LParen))
        return Skip;

      auto tup = std::make_shared<TupleType>();
      tup->location = previous.location;
      type = tup;

      if (has(TokenKind::RParen))
        return Success;

      Result r = Success;

      do
      {
        Node<Type> elem;

        if (typeexpr(elem) != Success)
        {
          r = Error;
          restart_before({TokenKind::Comma, TokenKind::RParen});
        }

        tup->types.push_back(elem);
      } while (has(TokenKind::Comma));

      if (!has(TokenKind::RParen))
      {
        error() << loc() << "Expected )" << line();
        r = Error;
      }

      tup->location.extend(previous.location);

      if (tup->types.size() == 1)
        type = tup->types.front();

      return r;
    }

    Result optmodulename(Node<TypeName>& name)
    {
      if (!has(TokenKind::EscapedString))
        return Skip;

      Result r = Success;

      name = std::make_shared<ModuleName>();
      name->location = previous.location;

      // Look for a module relative to the current source file first.
      auto base = path::to_directory(escapedstring(name->location.view()));
      auto relative = path::join(source->origin, base);
      auto std = path::join(stdlib, base);
      auto find = path::canonical(relative);

      // Otherwise, look for a module relative to the standard library.
      if (find.empty())
        find = path::canonical(std);

      if (!find.empty())
      {
        auto it = std::find(imports.begin(), imports.end(), find);
        size_t i = it - imports.begin();

        if (it == imports.end())
        {
          i = imports.size();
          imports.push_back(find);
        }

        name->location = ident("$module-" + std::to_string(i));
      }
      else
      {
        auto& out = error() << name->location << "Couldn't locate module \""
                            << base << "\"" << text(name->location);
        out << "Tried " << relative << std::endl;
        out << "Tried " << std << std::endl;
        r = Error;
      }

      if (opttypeargs(name->typeargs) == Error)
        r = Error;

      return r;
    }

    Result opttyperef(Node<Type>& type)
    {
      // typename <- name typeargs?
      // modulename <- string typeargs?
      // typeref <- (modulename / typename) ('::' typename)*
      if (
        !peek(TokenKind::Ident) && !peek(TokenKind::Symbol) &&
        !peek(TokenKind::EscapedString) && !peek(TokenKind::UnescapedString))
        return Skip;

      rewind();
      auto typeref = std::make_shared<TypeRef>();
      type = typeref;

      Result r = Success;

      // A typeref can start with a module name.
      Node<TypeName> name;

      if (optmodulename(name) != Skip)
      {
        typeref->location = name->location;
        typeref->typenames.push_back(name);

        if (!has(TokenKind::DoubleColon))
          return r;
      }

      do
      {
        if (!has(TokenKind::Ident) && !has(TokenKind::Symbol))
        {
          error() << loc() << "Expected a type identifier" << line();
          return Error;
        }

        auto name = std::make_shared<TypeName>();
        name->location = previous.location;
        typeref->typenames.push_back(name);

        if (opttypeargs(name->typeargs) == Error)
          r = Error;

        typeref->location.extend(previous.location);
      } while (has(TokenKind::DoubleColon));

      return r;
    }

    Result opttypelist(Node<Type>& type)
    {
      bool ok = peek(TokenKind::Ident) && peek(TokenKind::Ellipsis);
      rewind();

      if (!ok)
        return Skip;

      auto tl = std::make_shared<TypeList>();
      type = tl;

      has(TokenKind::Ident);
      tl->location = previous.location;
      has(TokenKind::Ellipsis);

      Result r = Success;
      auto def = symbols->symbol_table()->get_scope(tl->location);

      if (!def)
      {
        error() << tl->location
                << "Couldn't find a definition of this type list."
                << text(tl->location);
        r = Error;
      }
      else if (def->kind() != Kind::TypeParamList)
      {
        error() << tl->location << "Expected a type list, but got a "
                << kindname(def->kind()) << text(tl->location) << def->location
                << "Definition is here" << text(def->location);
        r = Error;
      }

      return r;
    }

    Result optcaptype(Node<Type>& type)
    {
      // captype <-
      //  'iso' / 'mut' / 'imm' / 'Self' / tupletype / typelist / typeref
      if (has(TokenKind::Iso))
      {
        auto cap = std::make_shared<Iso>();
        cap->location = previous.location;
        type = cap;
        return Success;
      }

      if (has(TokenKind::Mut))
      {
        auto cap = std::make_shared<Mut>();
        cap->location = previous.location;
        type = cap;
        return Success;
      }

      if (has(TokenKind::Imm))
      {
        auto cap = std::make_shared<Imm>();
        cap->location = previous.location;
        type = cap;
        return Success;
      }

      if (has(TokenKind::Self))
      {
        auto self = std::make_shared<Self>();
        self->location = previous.location;
        type = self;
        return Success;
      }

      Result r;

      if ((r = opttupletype(type)) != Skip)
        return r;

      if ((r = opttypelist(type)) != Skip)
        return r;

      if ((r = opttyperef(type)) != Skip)
        return r;

      return Skip;
    }

    Result optviewtype(Node<Type>& type)
    {
      // viewtype <- captype (('~>' / '<~') captype)*
      Result r;

      if ((r = optcaptype(type)) == Skip)
        return r;

      Node<TypePair> pair;

      while (peek(TokenKind::Symbol, "~>") || peek(TokenKind::Symbol, "<~"))
      {
        rewind();

        if (has(TokenKind::Symbol, "~>"))
          pair = std::make_shared<ViewType>();
        else if (has(TokenKind::Symbol, "<~"))
          pair = std::make_shared<ExtractType>();

        pair->location = type->location.range(previous.location);
        pair->left = type;
        type = pair;

        Result r2;

        if ((r2 = optcaptype(pair->right)) != Success)
        {
          if (r2 == Skip)
            error() << loc() << "Expected a type" << line();

          r = Error;
          break;
        }

        pair->location.extend(pair->right->location);
      }

      rewind();
      return r;
    }

    Result optfunctiontype(Node<Type>& type)
    {
      // functiontype <- viewtype ('->' functiontype)?
      // Right associative.
      Result r;

      if ((r = optviewtype(type)) != Success)
        return r;

      if (!has(TokenKind::Symbol, "->"))
        return Success;

      auto functype = std::make_shared<FunctionType>();
      functype->location = type->location.range(previous.location);
      functype->left = type;
      type = functype;

      if (optfunctiontype(functype->right) != Success)
        return Error;

      functype->location.extend(functype->right->location);
      return Success;
    }

    Result optisecttype(Node<Type>& type)
    {
      // isecttype <- functiontype ('&' functiontype)*
      Result r = Success;

      if ((r = optfunctiontype(type)) != Success)
        return r;

      while (has(TokenKind::Symbol, "&"))
      {
        Node<Type> next;
        Result r2;

        if ((r2 = optfunctiontype(next)) != Success)
        {
          if (r2 == Skip)
            error() << loc() << "Expected a type" << line();

          r = Error;
        }

        if (r2 != Skip)
          type = dnf::conjunction(type, next);
      }

      return r;
    }

    Result optthrowtype(Node<Type>& type)
    {
      // throwtype <- 'throw'? isecttype
      bool throwing = has(TokenKind::Throw);
      Result r;

      if ((r = optisecttype(type)) == Skip)
        return Skip;

      if (throwing)
        type = dnf::throwtype(type);

      return r;
    }

    Result optuniontype(Node<Type>& type)
    {
      // uniontype <- throwtype ('|' throwtype)*
      Result r = Success;

      if ((r = optthrowtype(type)) != Success)
        return r;

      while (has(TokenKind::Symbol, "|"))
      {
        Node<Type> next;
        Result r2;

        if ((r2 = optthrowtype(next)) != Success)
        {
          if (r2 == Skip)
            error() << loc() << "Expected a type" << line();

          r = Error;
        }

        if (r2 != Skip)
          type = dnf::disjunction(type, next);
      }

      return r;
    }

    Result typeexpr(Node<Type>& type)
    {
      // typeexpr <- uniontype
      Result r = optuniontype(type);

      if (r == Skip)
      {
        error() << loc() << "Expected a type" << line();
        r = Error;
      }

      return r;
    }

    Result inittype(Node<Type>& type)
    {
      // inittype <- '=' type
      if (!has(TokenKind::Equals))
        return Skip;

      if (typeexpr(type) != Success)
        return Error;

      return Success;
    }

    Result oftype(Node<Type>& type)
    {
      if (!has(TokenKind::Colon))
        return Skip;

      return typeexpr(type);
    }

    Result optparam(Node<Expr>& param)
    {
      if (peek(TokenKind::Ident))
      {
        bool isparam = peek(TokenKind::Colon) || peek(TokenKind::Equals) ||
          peek(TokenKind::Comma) || peek(TokenKind::FatArrow) ||
          peek(TokenKind::RParen);
        rewind();

        if (isparam)
        {
          Result r = Success;
          has(TokenKind::Ident);
          auto p = std::make_shared<Param>();
          p->location = previous.location;

          if (oftype(p->type) == Error)
            r = Error;

          if (initexpr(p->dflt) == Error)
            r = Error;

          if (!p->type)
            p->type = std::make_shared<InferType>();

          set_sym(p->location, p);
          param = p;
          return r;
        }
      }

      return optexpr(param);
    }

    Result optparamlist(List<Expr>& params, TokenKind terminator)
    {
      Result r = Success;
      Result r2;
      Node<Expr> param;

      do
      {
        if ((r2 = optparam(param)) == Skip)
          break;

        params.push_back(param);

        if (r2 == Error)
        {
          r = Error;
          restart_before({TokenKind::Comma, terminator});
        }
      } while (has(TokenKind::Comma));

      return r;
    }

    Result optparams(List<Expr>& params)
    {
      if (!has(TokenKind::LParen))
        return Skip;

      Result r = optparamlist(params, TokenKind::RParen);

      if (!has(TokenKind::RParen))
      {
        error() << loc() << "Expected )" << line();
        r = Error;
      }

      return r;
    }

    Result optfield(Node<Member>& member)
    {
      // field <- ident oftype initexpr ';'
      if (!has(TokenKind::Ident))
        return Skip;

      auto field = std::make_shared<Field>();
      field->location = previous.location;
      member = field;

      Result r = Success;

      if (oftype(field->type) == Error)
        r = Error;

      if (initexpr(field->init) == Error)
        r = Error;

      if (!has(TokenKind::Semicolon))
      {
        error() << loc() << "Expected ;" << line();
        r = Error;
      }

      set_sym(field->location, field);
      return r;
    }

    Result optfunction(Node<Member>& member)
    {
      // function <- (ident / symbol)? typeparams? params oftype? (block / ';')
      bool ok = peek(TokenKind::Symbol) ||
        (peek(TokenKind::Ident) &&
         (peek(TokenKind::LSquare) || peek(TokenKind::LParen))) ||
        (peek(TokenKind::LSquare) || peek(TokenKind::LParen));

      rewind();

      if (!ok)
        return Skip;

      auto func = std::make_shared<Function>();
      member = func;
      Result r = Success;

      if (has(TokenKind::Ident) || has(TokenKind::Symbol))
      {
        func->location = previous.location;
        func->name = previous.location;
      }
      else
      {
        // Replace an empy name with 'apply'.
        func->location = lookahead.front().location;
        func->name = name_apply;
      }

      set_sym(func->name, func);

      auto lambda = std::make_shared<Lambda>();
      auto st = push(lambda);
      func->lambda = lambda;

      if (opttypeparams(lambda->typeparams) == Error)
        r = Error;

      if (optparams(lambda->params) != Success)
        r = Error;

      for (auto& param : lambda->params)
      {
        if (param->kind() != Kind::Param)
        {
          error() << param->location << "Function parameters can't be patterns"
                  << text(param->location);
        }
        else if (param->as<Param>().type->kind() == Kind::InferType)
        {
          error() << param->location << "Function parameters must have types"
                  << text(param->location);
        }
      }

      if (oftype(lambda->result) == Error)
        r = Error;

      st.done();
      Result r2;

      if ((r2 = optlambda(func->lambda, true)) != Skip)
      {
        if (r2 == Error)
          r = Error;
      }
      else if (!has(TokenKind::Semicolon))
      {
        error() << loc() << "Expected a lambda or ;" << line();
        r = Error;
      }

      return r;
    }

    Result opttypeparam(Node<TypeParam>& tp)
    {
      // typeparam <- ident oftype inittype
      if (!has(TokenKind::Ident))
        return Skip;

      Result r = Success;
      auto loc = previous.location;

      if (has(TokenKind::Ellipsis))
        tp = std::make_shared<TypeParamList>();
      else
        tp = std::make_shared<TypeParam>();

      tp->location = loc;

      if (oftype(tp->upper) == Error)
        r = Error;

      if (inittype(tp->dflt) == Error)
        r = Error;

      set_sym(tp->location, tp);
      return r;
    }

    Result opttypeparams(List<TypeParam>& typeparams)
    {
      // typeparams <- ('[' typeparam (',' typeparam)* ']')?
      if (!has(TokenKind::LSquare))
        return Skip;

      Result r = Success;

      do
      {
        Node<TypeParam> tp;
        Result r2;

        if ((r2 = opttypeparam(tp)) != Success)
        {
          error() << loc() << "Expected a type parameter" << line();
          r = Error;
          restart_before({TokenKind::Comma, TokenKind::RSquare});
        }

        if (r2 != Skip)
          typeparams.push_back(tp);
      } while (has(TokenKind::Comma));

      if (!has(TokenKind::RSquare))
      {
        error() << loc() << "Expected , or ]" << line();
        r = Error;
      }

      return r;
    }

    Result checkinherit(Node<Type>& inherit)
    {
      if (!inherit)
        return Skip;

      Result r = Success;

      if (inherit->kind() == Kind::IsectType)
      {
        auto& isect = inherit->as<IsectType>();

        for (auto& type : isect.types)
        {
          if (checkinherit(type) == Error)
            r = Error;
        }
      }
      else if (inherit->kind() != Kind::TypeRef)
      {
        error() << inherit->location << "A type can't inherit from a "
                << kindname(inherit->kind()) << text(inherit->location);
        r = Error;
      }

      return r;
    }

    Result optusing(Node<Member>& member)
    {
      // using <- 'using' typeref ';'
      if (!has(TokenKind::Using))
        return Skip;

      auto use = std::make_shared<Using>();
      use->location = previous.location;
      member = use;

      Result r;

      if ((r = opttyperef(use->type)) != Success)
      {
        if (r == Skip)
          error() << loc() << "Expected a type reference" << line();

        r = Error;
      }

      if (!has(TokenKind::Semicolon))
      {
        error() << loc() << "Expected ;" << line();
        r = Error;
      }

      return r;
    }

    Result opttypealias(Node<Member>& member)
    {
      // typealias <- 'type' ident typeparams? '=' type ';'
      if (!has(TokenKind::Type))
        return Skip;

      Result r = Success;
      auto alias = std::make_shared<TypeAlias>();

      if (!has(TokenKind::Ident))
      {
        error() << loc() << "Expected an identifier" << line();
        r = Error;
      }

      alias->location = previous.location;
      set_sym(alias->location, alias);
      member = alias;

      auto st = push(alias);

      if (opttypeparams(alias->typeparams) == Error)
        r = Error;

      if (!has(TokenKind::Equals))
      {
        error() << loc() << "Expected =" << line();
        r = Error;
      }

      if (typeexpr(alias->inherits) == Error)
        r = Error;

      if (!has(TokenKind::Semicolon))
      {
        error() << loc() << "Expected ;" << line();
        r = Error;
      }

      return r;
    }

    template<typename T>
    Result entity(Node<Member>& member)
    {
      auto ent = std::make_shared<T>();
      member = ent;

      Result r = Success;
      auto st = push(ent);

      if (has(TokenKind::Ident))
      {
        ent->location = previous.location;
      }
      else
      {
        error() << loc() << "Expected an identifier" << line();
        r = Error;
      }

      if (opttypeparams(ent->typeparams) == Error)
        r = Error;

      if (oftype(ent->inherits) == Error)
        r = Error;

      if (typebody(ent->members) == Error)
        r = Error;

      st.done();
      set_sym(ent->location, ent);

      if (checkinherit(ent->inherits) == Error)
        r = Error;

      return r;
    }

    Result optinterface(Node<Member>& member)
    {
      // interface <- 'interface' ident typeparams oftype typebody
      if (!has(TokenKind::Interface))
        return Skip;

      return entity<Interface>(member);
    }

    Result optclass(Node<Member>& member)
    {
      // class <- 'class' ident typeparams oftype typebody
      if (!has(TokenKind::Class))
        return Skip;

      Result r = entity<Class>(member);
      auto& cls = member->as<Class>();
      bool trivial_create = !cls.symbol_table()->get(name_create);

      if (trivial_create)
      {
        for (auto& m : cls.members)
        {
          if (m->kind() != Kind::Field)
            continue;

          auto& f = m->as<Field>();

          if (!f.init)
          {
            trivial_create = false;
            break;
          }
        }
      }

      if (trivial_create)
      {
        auto n = std::make_shared<New>();
        n->location = cls.location;

        auto tn = std::make_shared<TypeName>();
        tn->location = cls.location;

        for (auto& tp : cls.typeparams)
        {
          if (tp->kind() == Kind::TypeParamList)
          {
            auto tl = std::make_shared<TypeList>();
            tl->location = tp->location;
            tn->typeargs.push_back(tl);
          }
          else
          {
            auto ta = std::make_shared<TypeName>();
            ta->location = tp->location;

            auto tr = std::make_shared<TypeRef>();
            tr->location = cls.location;
            tr->typenames.push_back(ta);

            tn->typeargs.push_back(tr);
          }
        }

        auto tr = std::make_shared<TypeRef>();
        tr->location = cls.location;
        tr->typenames.push_back(tn);

        auto iso = std::make_shared<Iso>();
        iso->location = cls.location;

        auto isect = std::make_shared<IsectType>();
        isect->location = cls.location;
        isect->types.push_back(tr);
        isect->types.push_back(iso);

        auto lambda = std::make_shared<Lambda>();
        lambda->location = cls.location;
        lambda->symbol_table()->parent = member;
        lambda->result = isect;
        lambda->body.push_back(n);

        auto create = std::make_shared<Function>();
        create->location = cls.location;
        create->name = name_create;
        create->lambda = lambda;

        cls.members.push_back(create);
        cls.symbol_table()->set(create->name, create);
      }

      return r;
    }

    Result optmoduledef(Node<Module>& module)
    {
      // moduledef <- 'module' typeparams oftype ';'
      if (!has(TokenKind::Module))
        return Skip;

      if (module)
      {
        error() << previous.location << "The module has already been defined"
                << text(previous.location) << module->location
                << "The previous definition is here" << text(module->location);

        restart_after(TokenKind::Semicolon);
        return Error;
      }

      module = std::make_shared<Module>();
      module->location = previous.location;
      Result r = Success;

      if (opttypeparams(module->typeparams) == Error)
        r = Error;

      if (oftype(module->inherits) == Error)
        r = Error;

      if (checkinherit(module->inherits) == Error)
        r = Error;

      if (!has(TokenKind::Semicolon))
      {
        error() << loc() << "Expected ;" << line();
        r = Error;
      }

      return r;
    }

    Result optmember(Node<Member>& member)
    {
      // member <- classdef / interface / typealias / using / field / function
      Result r;

      if ((r = optclass(member)) != Skip)
        return r;

      if ((r = optinterface(member)) != Skip)
        return r;

      if ((r = opttypealias(member)) != Skip)
        return r;

      if ((r = optusing(member)) != Skip)
        return r;

      if ((r = optfunction(member)) != Skip)
        return r;

      if ((r = optfield(member)) != Skip)
        return r;

      return Skip;
    }

    Result typebody(List<Member>& members)
    {
      // typebody <- '{' member* '}'
      Result r = Success;

      if (!has(TokenKind::LBrace))
      {
        error() << loc() << "Expected {" << line();
        r = Error;
      }

      if (has(TokenKind::RBrace))
        return r;

      while (!has(TokenKind::RBrace))
      {
        if (has(TokenKind::End))
        {
          error() << loc() << "Expected }" << line();
          return Error;
        }

        Node<Member> member;
        Result r2;

        if ((r2 = optmember(member)) == Skip)
        {
          error() << loc()
                  << "Expected a class, interface, type alias, field, "
                     "or function"
                  << line();

          restart_before(
            {TokenKind::RBrace,
             TokenKind::Class,
             TokenKind::Interface,
             TokenKind::Type,
             TokenKind::Ident,
             TokenKind::Symbol,
             TokenKind::LSquare,
             TokenKind::LParen});
        }

        members.push_back(member);

        if (r2 == Error)
          r = Error;
      }

      return r;
    }

    Result sourcefile(
      const std::string& file, Node<Class>& module, Node<Module>& moduledef)
    {
      auto source = load_source(file);

      if (!source)
      {
        error() << "Couldn't read file " << file << std::endl;
        return Error;
      }

      start(source);

      // module <- (moduledef / member)*
      while (!has(TokenKind::End))
      {
        Node<Member> member;
        Result r;

        if ((r = optmoduledef(moduledef)) == Skip)
        {
          if ((r = optmember(member)) != Skip)
            module->members.push_back(member);
        }

        if (r == Skip)
        {
          error() << loc()
                  << "Expected a module, class, interface, type alias, field, "
                     "or function"
                  << line();

          restart_before(
            {TokenKind::Module,
             TokenKind::Class,
             TokenKind::Interface,
             TokenKind::Type,
             TokenKind::Ident,
             TokenKind::Symbol,
             TokenKind::LSquare,
             TokenKind::LParen});
        }
      }

      return final_result;
    }

    // `path` can't be a reference, because `imports` may be modified during
    // parsing.
    Result
    module(const std::string path, size_t module_index, Node<Class>& program)
    {
      auto modulename = ident("$module-" + std::to_string(module_index));

      // Check if this module has already been loaded.
      if (symbols->symbol_table()->get(modulename))
        return final_result;

      Node<Module> moduledef;
      auto r = Success;

      auto module = std::make_shared<Class>();
      module->location = modulename;
      set_sym(module->location, module);
      auto st = push(module);
      program->members.push_back(module);

      if (!path::is_directory(path))
      {
        // This is only for testing.
        r = sourcefile(path, module, moduledef);
      }
      else
      {
        auto files = path::files(path);
        size_t count = 0;

        for (auto& file : files)
        {
          if (ext != path::extension(file))
            continue;

          auto filename = path::join(path, file);
          count++;

          if (sourcefile(filename, module, moduledef) == Error)
            r = Error;
        }

        if (!count)
        {
          error() << "No " << ext << " files found in " << path << std::endl;
          r = Error;
        }
      }

      if (moduledef)
      {
        module->typeparams = std::move(moduledef->typeparams);
        module->inherits = moduledef->inherits;
      }

      return r;
    }
  };

  std::pair<bool, Ast>
  parse(const std::string& path, const std::string& stdlib, std::ostream& out)
  {
    Parse parse(stdlib, out);
    auto program = std::make_shared<Class>();
    auto st = parse.push(program);
    parse.imports.push_back(path::canonical(path));

    for (size_t i = 0; i < parse.imports.size(); i++)
      parse.module(parse.imports[i], i, program);

    return {parse.final_result == Success, program};
  }
}
