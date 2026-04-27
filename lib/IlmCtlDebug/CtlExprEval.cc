///////////////////////////////////////////////////////////////////////////
// Copyright Contributors to the CTL project.
// SPDX-License-Identifier: BSD-3-Clause
///////////////////////////////////////////////////////////////////////////

#include "CtlExprEval.h"

#include <CtlType.h>

#include <cctype>
#include <cstdlib>
#include <stdexcept>

namespace Ctl {

namespace {

// Tokenizer

enum class Tok
{
    End,
    Number,
    Ident,
    LParen, RParen, LBrack, RBrack,
    Plus, Minus, Star, Slash,
    Lt, Le, Gt, Ge, EqEq, NotEq,
    AndAnd, OrOr, Bang,
};

struct Token
{
    Tok         kind;
    std::string text;     // raw text (for Ident / Number)
    double      number;   // parsed number (for Tok::Number)
};

class Lexer
{
  public:
    explicit Lexer (const std::string &s) : _s(s), _i(0) { advance(); }
    const Token &peek () const { return _cur; }
    Token        eat  ()
    {
        Token t = _cur;
        advance();
        return t;
    }

  private:
    void advance ()
    {
        // Skip whitespace.
        while (_i < _s.size() && std::isspace (static_cast<unsigned char>(_s[_i])))
            ++_i;
        if (_i >= _s.size()) { _cur = {Tok::End, "", 0.0}; return; }
        char c = _s[_i];

        // Number (integer or decimal).
        if (std::isdigit (static_cast<unsigned char>(c)) ||
            (c == '.' && _i + 1 < _s.size() &&
             std::isdigit (static_cast<unsigned char>(_s[_i + 1]))))
        {
            std::size_t start = _i;
            while (_i < _s.size() &&
                   (std::isdigit (static_cast<unsigned char>(_s[_i])) ||
                    _s[_i] == '.'))
                ++_i;
            std::string text = _s.substr (start, _i - start);
            _cur = {Tok::Number, text, std::strtod (text.c_str(), nullptr)};
            return;
        }

        // Identifier — letters, digits, underscore (after a letter/_).
        if (std::isalpha (static_cast<unsigned char>(c)) || c == '_')
        {
            std::size_t start = _i;
            while (_i < _s.size() &&
                   (std::isalnum (static_cast<unsigned char>(_s[_i])) ||
                    _s[_i] == '_'))
                ++_i;
            _cur = {Tok::Ident, _s.substr (start, _i - start), 0.0};
            return;
        }

        // 2-char punctuation first, then 1-char.
        auto two = [&](const char *lit, Tok k) -> bool {
            if (_i + 1 < _s.size() && _s[_i] == lit[0] && _s[_i + 1] == lit[1])
            {
                _cur = {k, lit, 0.0}; _i += 2; return true;
            }
            return false;
        };
        if (two("==", Tok::EqEq))  return;
        if (two("!=", Tok::NotEq)) return;
        if (two("<=", Tok::Le))    return;
        if (two(">=", Tok::Ge))    return;
        if (two("&&", Tok::AndAnd))return;
        if (two("||", Tok::OrOr))  return;

        switch (c)
        {
          case '(': _cur = {Tok::LParen, "(", 0.0}; ++_i; return;
          case ')': _cur = {Tok::RParen, ")", 0.0}; ++_i; return;
          case '[': _cur = {Tok::LBrack, "[", 0.0}; ++_i; return;
          case ']': _cur = {Tok::RBrack, "]", 0.0}; ++_i; return;
          case '+': _cur = {Tok::Plus,   "+", 0.0}; ++_i; return;
          case '-': _cur = {Tok::Minus,  "-", 0.0}; ++_i; return;
          case '*': _cur = {Tok::Star,   "*", 0.0}; ++_i; return;
          case '/': _cur = {Tok::Slash,  "/", 0.0}; ++_i; return;
          case '<': _cur = {Tok::Lt,     "<", 0.0}; ++_i; return;
          case '>': _cur = {Tok::Gt,     ">", 0.0}; ++_i; return;
          case '!': _cur = {Tok::Bang,   "!", 0.0}; ++_i; return;
          default:
            throw std::runtime_error (std::string("unexpected character '")
                                      + c + "'");
        }
    }

    const std::string &_s;
    std::size_t        _i;
    Token              _cur;
};

// Parser / evaluator (single-pass recursive descent)

class Parser
{
  public:
    Parser (Lexer &lex, const std::vector<InspectableVar> &vars)
    : _lex(lex), _vars(vars) {}

    double parse ()
    {
        double v = orExpr();
        if (_lex.peek().kind != Tok::End)
            throw std::runtime_error ("trailing tokens after expression");
        return v;
    }

  private:
    double orExpr ()
    {
        double v = andExpr();
        while (_lex.peek().kind == Tok::OrOr)
        {
            _lex.eat();
            double r = andExpr();
            v = (v != 0.0 || r != 0.0) ? 1.0 : 0.0;
        }
        return v;
    }

    double andExpr ()
    {
        double v = compExpr();
        while (_lex.peek().kind == Tok::AndAnd)
        {
            _lex.eat();
            double r = compExpr();
            v = (v != 0.0 && r != 0.0) ? 1.0 : 0.0;
        }
        return v;
    }

    double compExpr ()
    {
        double v = addExpr();
        Tok k = _lex.peek().kind;
        if (k == Tok::EqEq || k == Tok::NotEq || k == Tok::Lt ||
            k == Tok::Le   || k == Tok::Gt    || k == Tok::Ge)
        {
            _lex.eat();
            double r = addExpr();
            switch (k)
            {
              case Tok::EqEq:  return v == r ? 1.0 : 0.0;
              case Tok::NotEq: return v != r ? 1.0 : 0.0;
              case Tok::Lt:    return v <  r ? 1.0 : 0.0;
              case Tok::Le:    return v <= r ? 1.0 : 0.0;
              case Tok::Gt:    return v >  r ? 1.0 : 0.0;
              case Tok::Ge:    return v >= r ? 1.0 : 0.0;
              default: break;
            }
        }
        return v;
    }

    double addExpr ()
    {
        double v = mulExpr();
        for (;;)
        {
            Tok k = _lex.peek().kind;
            if (k == Tok::Plus)       { _lex.eat(); v += mulExpr(); }
            else if (k == Tok::Minus) { _lex.eat(); v -= mulExpr(); }
            else                       break;
        }
        return v;
    }

    double mulExpr ()
    {
        double v = unary();
        for (;;)
        {
            Tok k = _lex.peek().kind;
            if (k == Tok::Star)       { _lex.eat(); v *= unary(); }
            else if (k == Tok::Slash) { _lex.eat(); v /= unary(); }
            else                       break;
        }
        return v;
    }

    double unary ()
    {
        if (_lex.peek().kind == Tok::Minus) { _lex.eat(); return -unary(); }
        if (_lex.peek().kind == Tok::Bang)
        {
            _lex.eat();
            return unary() == 0.0 ? 1.0 : 0.0;
        }
        return primary();
    }

    double primary ()
    {
        Token t = _lex.eat();
        switch (t.kind)
        {
          case Tok::Number: return t.number;
          case Tok::LParen: {
              double v = orExpr();
              if (_lex.peek().kind != Tok::RParen)
                  throw std::runtime_error ("expected ')'");
              _lex.eat();
              return v;
          }
          case Tok::Ident: {
              // Optional postfix [index].
              int idx = -1;
              if (_lex.peek().kind == Tok::LBrack)
              {
                  _lex.eat();
                  double i = orExpr();
                  if (_lex.peek().kind != Tok::RBrack)
                      throw std::runtime_error ("expected ']'");
                  _lex.eat();
                  idx = static_cast<int>(i);
              }
              return resolveIdent (t.text, idx);
          }
          default:
            throw std::runtime_error ("unexpected token in expression");
        }
    }

    // Look up `name` (and optionally name[idx]) in the scope snapshot.
    // Matches by full qualified name OR by the friendly leaf name
    // (last segment after `::` or `$`).
    double resolveIdent (const std::string &name, int idx)
    {
        for (const auto &v : _vars)
        {
            std::size_t pos = v.name.find_last_of (":$");
            std::string leaf = (pos == std::string::npos)
                                 ? v.name : v.name.substr (pos + 1);
            if (leaf != name && v.name != name) continue;
            if (!v.type || !v.data) continue;

            // Scalar lookup.
            if (idx < 0)
                return readNumeric (v.type, v.data, 0);

            // Array lookup — resolve the element type and stride from
            // the type description.  Only arrays of basic numeric
            // scalars are supported; multi-dimensional arrays fall
            // through to an error.
            return readArrayElement (v.type, v.data, idx);
        }
        throw std::runtime_error ("unknown name: " + name);
    }

    static double readNumeric (const DataTypePtr &t,
                               const void *p, int /*offset*/)
    {
        switch (t->cDataType())
        {
          case IntTypeEnum:
            return static_cast<double>(*reinterpret_cast<const int *>(p));
          case UIntTypeEnum:
            return static_cast<double>(
                       *reinterpret_cast<const unsigned int *>(p));
          case FloatTypeEnum:
            return static_cast<double>(*reinterpret_cast<const float *>(p));
          default:
            throw std::runtime_error ("cannot read non-numeric value");
        }
    }

    static double readArrayElement (const DataTypePtr &t,
                                    const void *base, int idx)
    {
        // The lane-0 base pointer steps through array elements at the
        // element type's alignedObjectSize.  Walk via ArrayType.
        ArrayTypePtr at = t.cast<ArrayType>();
        if (!at) throw std::runtime_error ("not an array");
        DataTypePtr et = at->elementType().cast<DataType>();
        if (!et) throw std::runtime_error ("non-data array element");
        std::size_t stride = et->alignedObjectSize();
        const char *p = reinterpret_cast<const char *>(base) + idx * stride;
        return readNumeric (et, p, 0);
    }

    Lexer                              &_lex;
    const std::vector<InspectableVar>  &_vars;
};

} // namespace

EvalResult
evalExpression (const std::string &expr,
                const std::vector<InspectableVar> &vars)
{
    EvalResult r;
    try
    {
        Lexer lex (expr);
        Parser parser (lex, vars);
        r.value = parser.parse();
        r.ok    = true;
    }
    catch (const std::exception &e)
    {
        r.ok    = false;
        r.error = e.what();
    }
    return r;
}

} // namespace Ctl
