/*
** $Id: llex.h,v 1.79.1.1 2017/04/19 17:20:42 roberto Exp $
** Lexical Analyzer
** See Copyright Notice in lua.h
*/

#ifndef llex_h
#define llex_h

#include "lobject.h"
#include "lzio.h"


#define FIRST_RESERVED	257


#if !defined(LUA_ENV)
#define LUA_ENV		"_ENV"
#endif


/*
* WARNING: if you change the order of this enumeration,
* grep "ORDER RESERVED"
*/
enum RESERVED {
  /* terminal symbols denoted by reserved words */
  TK_AND = FIRST_RESERVED, TK_BREAK,
  TK_DO, TK_ELSE, TK_ELSEIF, TK_END, TK_FALSE, TK_FOR, TK_FUNCTION,
  TK_GOTO, TK_IF, TK_IN, TK_LOCAL, TK_NIL, TK_NOT, TK_OR, TK_REPEAT,
  TK_RETURN, TK_THEN, TK_TRUE, TK_UNTIL, TK_WHILE,
  /* other terminal symbols */
  TK_IDIV, TK_CONCAT, TK_DOTS, TK_EQ, TK_GE, TK_LE, TK_NE,
  TK_SHL, TK_SHR,
  TK_DBCOLON, TK_EOS,
  TK_FLT, TK_INT, TK_NAME, TK_STRING
};

/* number of reserved words */
#define NUM_RESERVED	(cast(int, TK_WHILE-FIRST_RESERVED+1))

// 语义辅助信息
typedef union {
  lua_Number r;			// 当token是数字时，内容存放在r中
  lua_Integer i;
  TString *ts;			// 其他情况存放在ts指向的TString中
} SemInfo;  /* semantics information */

// 语义切割的最小单位
typedef struct Token {
  int token;		// 代表了一个词法单元
  SemInfo seminfo;	// 存放了一些语义相关的一些内容信息
} Token;


/* state of the lexer plus state of the parser when shared by all
   functions 
其责任就是记录当前的行号、符号、期望的下一个符号、读取字符串或者数字。
LexState不仅用于保存当前的词法分析状态信息，而且也保存了整个编译系统的全局状态。

*/
typedef struct LexState {
  int current;  	/* 指示当前的字符（相对于文件开头的偏移位置）。 current character (charint) */
  int linenumber;  	/* 指示当前解析器的 current 指针的行位置。input line counter */
  int lastline;  	/* 指示当前文件里面的最后一个有作用的记号所在的行。 line of last token 'consumed' */
  Token t;  		/* 存放了当前的token。current token */
  Token lookahead;  /* 存放了向前看的token。look ahead token */
  struct FuncState *fs;  /* 指向了parser当前解析的函数的一些相关的信息。current function (parser) */
  struct lua_State *L;   /* Lua 状态机 */
  ZIO *z;  			/* 指向输入流。input stream */
  Mbuffer *buff;  	/* 用于存储所有 token 的一个缓存。buffer for tokens */
  Table *h;  		/* to avoid collection/reuse strings */
  struct Dyndata *dyd;  /* dynamic structures used by the parser */
  TString *source;  /* 当前源码的名字。current source name */
  TString *envn;  	/* environment variable name */
} LexState;


LUAI_FUNC void luaX_init (lua_State *L);
LUAI_FUNC void luaX_setinput (lua_State *L, LexState *ls, ZIO *z,
                              TString *source, int firstchar);
LUAI_FUNC TString *luaX_newstring (LexState *ls, const char *str, size_t l);
LUAI_FUNC void luaX_next (LexState *ls);
LUAI_FUNC int luaX_lookahead (LexState *ls);
LUAI_FUNC l_noret luaX_syntaxerror (LexState *ls, const char *s);
LUAI_FUNC const char *luaX_token2str (LexState *ls, int token);


#endif
