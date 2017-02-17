%{
    #include <iostream>
    #include <vector>
    #include "../ast/expr.h"
    #include "../ast/decl.h"
    #include "../sema/typecheck.h"

    struct YYLTYPE;

    int yyerror(const char*);

    namespace delta {

    #define yylex lex
    int lex();

    /// The root of the abstract syntax tree, for passing from Bison to compiler.
    extern std::vector<Decl> globalAST;

    /// Shorthand to avoid overly long lines.
    template<typename T>
    std::unique_ptr<T> u(T* ptr) { return std::unique_ptr<T>(ptr); }

    SrcLoc loc(YYLTYPE);

    }

    #pragma GCC diagnostic ignored "-Wunused-function"
%}

%glr-parser
%error-verbose

// Keywords
%token CAST     "cast"
%token CLASS    "class"
%token CONST    "const"
%token ELSE     "else"
%token EXTERN   "extern"
%token FALSE    "false"
%token FUNC     "func"
%token IF       "if"
%token IMPORT   "import"
%token INIT     "init"
%token MUTABLE  "mutable"
%token NULL_LITERAL "null"
%token RETURN   "return"
%token STRUCT   "struct"
%token THIS     "this"
%token TRUE     "true"
%token UNINITIALIZED "uninitialized"
%token VAR      "var"
%token WHILE    "while"

// Operators
%token EQ       "=="
%token NE       "!="
%token LT       "<"
%token LE       "<="
%token GT       ">"
%token GE       ">="
%token PLUS     "+"
%token MINUS    "-"
%token STAR     "*"
%token SLASH    "/"
%token INCREMENT"++"
%token DECREMENT"--"
%token NOT      "!"
%token AND      "&"
%token AND_AND  "&&"
%token OR       "|"
%token OR_OR    "||"
%token XOR      "^"
%token COMPL    "~"
%token LSHIFT   "<<"
%token RSHIFT   ">>"

// Miscellaneous
%token ASSIGN   "="
%token LPAREN   "("
%token RPAREN   ")"
%token LBRACKET "["
%token RBRACKET "]"
%token LBRACE   "{"
%token RBRACE   "}"
%token DOT      "."
%token COMMA    ","
%token COLON    ":"
%token COLON_COLON "::"
%token SEMICOLON";"
%token RARROW   "->"

// Precedence and associativity
%nonassoc "=="
%left "+" "-"
%left "*" "/"
%left "&"
%left "[" "]"

// Types
%token <string> IDENTIFIER STRING_LITERAL
%token <number> NUMBER
%type <expr> expression prefix_expression binary_expression parenthesized_expression
             call_expression cast_expression member_access_expression subscript_expression
             assignment_lhs_expression
%type <declList> declaration_list
%type <decl> declaration function_definition initializer_definition function_prototype
             member_function_prototype extern_function_declaration variable_definition
             immutable_variable_definition mutable_variable_definition
             typed_variable_definition composite_type_declaration import_declaration
%type <stmtList> else_body statement_list
%type <stmt> statement return_statement increment_statement decrement_statement
             call_statement if_statement while_statement assignment_statement
%type <exprList> return_value_list nonempty_return_value_list
%type <argList> argument_list nonempty_argument_list
%type <arg> argument
%type <paramDeclList> parameter_list nonempty_parameter_list
%type <paramDecl> parameter
%type <fieldDeclList> member_list
%type <fieldDecl> field_declaration
%type <type> type return_type_specifier return_type_list

%union {
    char* string;
    long long number;
    std::vector<delta::Decl>* declList;
    std::vector<delta::ParamDecl>* paramDeclList;
    std::vector<delta::FieldDecl>* fieldDeclList;
    std::vector<delta::Stmt>* stmtList;
    std::vector<delta::Expr>* exprList;
    std::vector<delta::Arg>* argList;
    delta::Arg* arg;
    delta::Decl* decl;
    delta::ParamDecl* paramDecl;
    delta::FieldDecl* fieldDecl;
    delta::Stmt* stmt;
    delta::Expr* expr;
    delta::Type* type;
};

%initial-action {
    yylloc.first_line = yylloc.last_line = 1;
    yylloc.first_column = yylloc.last_column = 0;
}

%{
    #ifdef YYBISON // To prevent this from being put in the generated header.
    using namespace delta;
    #endif
%}

%%

source_file: declaration_list { std::move($1->begin(), $1->end(), std::back_inserter(globalAST)); };

declaration_list:
    /* empty */ { $$ = new std::vector<Decl>(); }
|   declaration_list declaration { $$ = $1; $1->push_back(std::move(*$2)); };

declaration:
    function_definition { $$ = $1; }
|   initializer_definition { $$ = $1; }
|   extern_function_declaration { $$ = $1; }
|   composite_type_declaration { $$ = $1; }
|   import_declaration { $$ = $1; }
|   variable_definition { $$ = $1; };

// Declarations ////////////////////////////////////////////////////////////////

function_definition:
    function_prototype "{" statement_list "}"
        { $$ = $1; $$->getFuncDecl().body.reset($3); }
|   member_function_prototype "{" statement_list "}"
        { $$ = $1; $$->getFuncDecl().body.reset($3); };

function_prototype:
    "func" IDENTIFIER "(" parameter_list ")" return_type_specifier
        { $$ = new Decl(FuncDecl{$2, std::move(*$4), std::move(*$6), "", nullptr, loc(@2)});
          addToSymbolTable($$->getFuncDecl()); };

member_function_prototype:
    "func" IDENTIFIER "::" IDENTIFIER "(" parameter_list ")" return_type_specifier
        { $$ = new Decl(FuncDecl{$4, std::move(*$6), std::move(*$8), $2, nullptr, loc(@2)});
          addToSymbolTable($$->getFuncDecl()); };

extern_function_declaration:
    "extern" function_prototype ";" { $$ = $2; };

return_type_specifier:
    /* empty */ { $$ = new Type(BasicType{"void"}); }
|   "->" return_type_list { $$ = $2; };

return_type_list:
    type { $$ = $1; }
|   return_type_list "," type { $$ = $1; $$->appendType(std::move(*$3)); };

parameter_list:
    /* empty */ { $$ = new std::vector<ParamDecl>(); }
|   nonempty_parameter_list { $$ = $1; };

nonempty_parameter_list:
    parameter { $$ = new std::vector<ParamDecl>(); $$->push_back(std::move(*$1)); }
|   nonempty_parameter_list "," parameter { $$ = $1; $$->push_back(std::move(*$3)); };

parameter:
    type IDENTIFIER { $$ = new ParamDecl{"", std::move(*$1), $2, loc(@2)}; }
|   IDENTIFIER ":" type IDENTIFIER { $$ = new ParamDecl{$1, std::move(*$3), $4, loc(@4)}; };

statement_list:
    /* empty */ { $$ = new std::vector<Stmt>(); }
|   statement_list statement { $$ = $1; $1->push_back(std::move(*$2)); };

type:
    IDENTIFIER { $$ = new Type(BasicType{$1}); }
|   IDENTIFIER "[" NUMBER "]" { $$ = new Type(ArrayType{u(new Type(BasicType{$1})), $3}); }
|   "mutable" IDENTIFIER { $$ = new Type(BasicType{$2}); $$->setMutable(true); }
|   "mutable" IDENTIFIER "[" NUMBER "]" { auto e = new Type(BasicType{$2}); e->setMutable(true); $$ = new Type(ArrayType{u(e), $4}); $$->setMutable(true);  }
|   "mutable" "(" type "&" ")" { $$ = new Type(PtrType{u($3), true}); $$->setMutable(true); }
|   type "&" { $$ = new Type(PtrType{u($1), true}); }
|   "mutable" "(" type "*" ")" { $$ = new Type(PtrType{u($3), false}); $$->setMutable(true); }
|   type "*" { $$ = new Type(PtrType{u($1), false}); };

composite_type_declaration:
    "struct" IDENTIFIER "{" member_list "}" { $$ = new Decl(TypeDecl{TypeTag::Struct, $2, std::move(*$4), loc(@2)});
                                              addToSymbolTable($$->getTypeDecl()); }
|   "class"  IDENTIFIER "{" member_list "}" { $$ = new Decl(TypeDecl{TypeTag::Class, $2, std::move(*$4), loc(@2)});
                                              addToSymbolTable($$->getTypeDecl()); };

member_list:
    /* empty */ { $$ = new std::vector<FieldDecl>(); }
|   member_list field_declaration { $$ = $1; $$->push_back(std::move(*$2)); };

field_declaration:
    type IDENTIFIER ";" { $$ = new FieldDecl{std::move(*$1), $2, loc(@2)}; };

initializer_definition:
    IDENTIFIER "::" "init" "(" parameter_list ")" "{" statement_list "}"
        { $$ = new Decl(InitDecl{$1, std::move(*$5), nullptr, loc(@3)});
          addToSymbolTable($$->getInitDecl());
          $$->getInitDecl().body.reset($8); };

import_declaration:
    "import" STRING_LITERAL ";" { $$ = new Decl(ImportDecl{$2, loc(@2)}); };

// Statements //////////////////////////////////////////////////////////////////

statement:
    variable_definition { $$ = new Stmt(VariableStmt{&$1->getVarDecl()}); }
|   assignment_statement{ $$ = $1; }
|   return_statement    { $$ = $1; }
|   increment_statement { $$ = $1; }
|   decrement_statement { $$ = $1; }
|   call_statement      { $$ = $1; }
|   if_statement        { $$ = $1; }
|   while_statement     { $$ = $1; };

variable_definition:
    immutable_variable_definition { $$ = $1; }
|   mutable_variable_definition   { $$ = $1; }
|   typed_variable_definition     { $$ = $1; };

immutable_variable_definition:
    "const" IDENTIFIER "=" expression ";"
        { $$ = new Decl(VarDecl{false, $2, std::shared_ptr<Expr>($4), loc(@2)}); };

mutable_variable_definition:
    "var"   IDENTIFIER "=" expression ";"
        { $$ = new Decl(VarDecl{true, $2, std::shared_ptr<Expr>($4), loc(@2)}); };

typed_variable_definition:
    type    IDENTIFIER "=" expression ";"
        { $$ = new Decl(VarDecl{std::move(*$1), $2, std::shared_ptr<Expr>($4), loc(@2)}); }
|   type    IDENTIFIER "=" "uninitialized" ";"
        { $$ = new Decl(VarDecl{std::move(*$1), $2, nullptr, loc(@2)}); };

assignment_statement:
    assignment_lhs_expression "=" expression ";"
        { $$ = new Stmt(AssignStmt{std::move(*$1), std::move(*$3), loc(@2)}); };

return_statement:
    "return" return_value_list ";" { $$ = new Stmt(ReturnStmt{std::move(*$2), loc(@1)}); };

return_value_list:
    /* empty */ { $$ = new std::vector<Expr>(); }
|   nonempty_return_value_list { $$ = $1; };

nonempty_return_value_list:
    expression { $$ = new std::vector<Expr>(); $$->push_back(std::move(*$1)); }
|   nonempty_return_value_list "," expression { $$ = $1; $$->push_back(std::move(*$3)); };

increment_statement: expression "++" ";" { $$ = new Stmt(IncrementStmt{std::move(*$1), loc(@2)}); };

decrement_statement: expression "--" ";" { $$ = new Stmt(DecrementStmt{std::move(*$1), loc(@2)}); };

call_statement:
    call_expression ";" { $$ = new Stmt(CallStmt{std::move($1->getCallExpr())}); };

if_statement:
    "if" "(" expression ")" "{" statement_list "}"
        { $$ = new Stmt(IfStmt{std::move(*$3), std::move(*$6), {}}); }
|   "if" "(" expression ")" "{" statement_list "}" "else" else_body
        { $$ = new Stmt(IfStmt{std::move(*$3), std::move(*$6), std::move(*$9)}); };

else_body:
    if_statement { $$ = new std::vector<Stmt>(); $$->push_back(std::move(*$1)); }
|   "{" statement_list "}" { $$ = $2; };

while_statement:
    "while" "(" expression ")" "{" statement_list "}"
        { $$ = new Stmt(WhileStmt{std::move(*$3), std::move(*$6)}); };

// Expressions /////////////////////////////////////////////////////////////////

expression:
    STRING_LITERAL { $$ = new Expr(StrLiteralExpr{$1, loc(@1)}); }
|   NUMBER { $$ = new Expr(IntLiteralExpr{$1, loc(@1)}); }
|   TRUE { $$ = new Expr(BoolLiteralExpr{true, loc(@1)}); }
|   FALSE { $$ = new Expr(BoolLiteralExpr{false, loc(@1)}); }
|   "null" { $$ = new Expr(NullLiteralExpr{loc(@1)}); }
|   binary_expression { $$ = $1; }
|   assignment_lhs_expression { $$ = $1; };

assignment_lhs_expression:
    IDENTIFIER { $$ = new Expr(VariableExpr{$1, loc(@1)}); }
|   prefix_expression { $$ = $1; }
|   parenthesized_expression { $$ = $1; }
|   call_expression { $$ = $1; }
|   cast_expression { $$ = $1; }
|   member_access_expression { $$ = $1; }
|   subscript_expression { $$ = $1; };

prefix_expression: "+" expression { $$ = new Expr(PrefixExpr{PLUS, u($2), loc(@1)}); };
prefix_expression: "-" expression { $$ = new Expr(PrefixExpr{MINUS, u($2), loc(@1)}); };
prefix_expression: "*" expression { $$ = new Expr(PrefixExpr{STAR, u($2), loc(@1)}); };
prefix_expression: "&" expression { $$ = new Expr(PrefixExpr{AND, u($2), loc(@1)}); };
prefix_expression: "!" expression { $$ = new Expr(PrefixExpr{NOT, u($2), loc(@1)}); };
prefix_expression: "~" expression { $$ = new Expr(PrefixExpr{COMPL, u($2), loc(@1)}); };
binary_expression: expression "==" expression { $$ = new Expr(BinaryExpr{EQ, u($1), u($3), loc(@2)}); };
binary_expression: expression "!=" expression { $$ = new Expr(BinaryExpr{NE, u($1), u($3), loc(@2)}); };
binary_expression: expression "<"  expression { $$ = new Expr(BinaryExpr{LT, u($1), u($3), loc(@2)}); };
binary_expression: expression "<=" expression { $$ = new Expr(BinaryExpr{LE, u($1), u($3), loc(@2)}); };
binary_expression: expression ">"  expression { $$ = new Expr(BinaryExpr{GT, u($1), u($3), loc(@2)}); };
binary_expression: expression ">=" expression { $$ = new Expr(BinaryExpr{GE, u($1), u($3), loc(@2)}); };
binary_expression: expression "+"  expression { $$ = new Expr(BinaryExpr{PLUS, u($1), u($3), loc(@2)}); };
binary_expression: expression "-"  expression { $$ = new Expr(BinaryExpr{MINUS, u($1), u($3), loc(@2)}); };
binary_expression: expression "*"  expression { $$ = new Expr(BinaryExpr{STAR, u($1), u($3), loc(@2)}); };
binary_expression: expression "/"  expression { $$ = new Expr(BinaryExpr{SLASH, u($1), u($3), loc(@2)}); };
binary_expression: expression "&"  expression { $$ = new Expr(BinaryExpr{AND, u($1), u($3), loc(@2)}); };
binary_expression: expression "&&" expression { $$ = new Expr(BinaryExpr{AND_AND, u($1), u($3), loc(@2)}); };
binary_expression: expression "|"  expression { $$ = new Expr(BinaryExpr{OR, u($1), u($3), loc(@2)}); };
binary_expression: expression "||" expression { $$ = new Expr(BinaryExpr{OR_OR, u($1), u($3), loc(@2)}); };
binary_expression: expression "^"  expression { $$ = new Expr(BinaryExpr{XOR, u($1), u($3), loc(@2)}); };
binary_expression: expression "<<" expression { $$ = new Expr(BinaryExpr{LSHIFT, u($1), u($3), loc(@2)}); };
binary_expression: expression ">>" expression { $$ = new Expr(BinaryExpr{RSHIFT, u($1), u($3), loc(@2)}); };

parenthesized_expression: "(" expression ")" { $$ = $2; };

call_expression:
    IDENTIFIER "(" argument_list ")" { $$ = new Expr(CallExpr{$1, std::move(*$3), false, nullptr, loc(@1)}); }
|   expression "." IDENTIFIER "(" argument_list ")" { $$ = new Expr(CallExpr{$3, std::move(*$5), false, u($1), loc(@1)}); };

cast_expression:
    "cast" "<" type ">" "(" expression ")" { $$ = new Expr(CastExpr{std::move(*$3), u($6), loc(@1)}); };

argument_list:
    /* empty */ { $$ = new std::vector<Arg>(); }
|   nonempty_argument_list { $$ = $1; };

nonempty_argument_list:
    argument { $$ = new std::vector<Arg>(); $$->push_back(std::move(*$1)); }
|   nonempty_argument_list "," argument { $$ = $1; $$->push_back(std::move(*$3)); };

argument:
    expression { $$ = new Arg{"", u($1), loc(@1)};  }
|   IDENTIFIER ":" expression { $$ = new Arg{$1, u($3), loc(@1)}; };

member_access_expression:
    "this" "." IDENTIFIER { $$ = new Expr(MemberExpr{"this", $3, loc(@1), loc(@3)}); }
|   IDENTIFIER "." IDENTIFIER { $$ = new Expr(MemberExpr{$1, $3, loc(@1), loc(@3)}); };

subscript_expression:
    expression "[" expression "]" { $$ = new Expr(SubscriptExpr{u($1), u($3), loc(@2)}); };

%%

int yyerror(const char* message) {
    std::cout << "error: " << message << '\n';
    return 1;
}

std::vector<Decl> delta::globalAST;

#include "lexer.cpp"
#include "operators.cpp"

SrcLoc delta::loc(YYLTYPE loc) { return SrcLoc(currentFileName, loc.first_line, loc.first_column); }
