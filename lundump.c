/*
** $Id: lundump.c,v 2.44.1.1 2017/04/19 17:20:42 roberto Exp $
** load precompiled Lua chunks
** See Copyright Notice in lua.h
*/

#define lundump_c
#define LUA_CORE

#include "lprefix.h"


#include <string.h>

#include "lua.h"

#include "ldebug.h"
#include "ldo.h"
#include "lfunc.h"
#include "lmem.h"
#include "lobject.h"
#include "lstring.h"
#include "lundump.h"
#include "lzio.h"


#if !defined(luai_verifycode)
#define luai_verifycode(L,b,f)  /* empty */
#endif


typedef struct {
  lua_State *L;
  ZIO *Z;
  const char *name;
} LoadState;


static l_noret error(LoadState *S, const char *why) {
  luaO_pushfstring(S->L, "%s: %s precompiled chunk", S->name, why);
  luaD_throw(S->L, LUA_ERRSYNTAX);
}


/*
** All high-level loads go through LoadVector; you can change it to
** adapt to the endianness of the input
*/
#define LoadVector(S,b,n)	LoadBlock(S,b,(n)*sizeof((b)[0]))

static void LoadBlock (LoadState *S, void *b, size_t size) {
  if (luaZ_read(S->Z, b, size) != 0)
    error(S, "truncated");
}


#define LoadVar(S,x)		LoadVector(S,&x,1)


static lu_byte LoadByte (LoadState *S) {
  lu_byte x;
  LoadVar(S, x);
  return x;
}


static int LoadInt (LoadState *S) {
  int x;
  LoadVar(S, x);
  return x;
}


static lua_Number LoadNumber (LoadState *S) {
  lua_Number x;
  LoadVar(S, x);
  return x;
}


static lua_Integer LoadInteger (LoadState *S) {
  lua_Integer x;
  LoadVar(S, x);
  return x;
}


static TString *LoadString (LoadState *S) {
  /* 0A 40 64 65 6D 6F 2E 6C 75 61
	0A：表示后面文件名的长度为 10-1 = 9，即：@demo.lua */
  size_t size = LoadByte(S);
  if (size == 0xFF)
    LoadVar(S, size);
  if (size == 0)
    return NULL;
  else if (--size <= LUAI_MAXSHORTLEN) {  /* short string? */
    char buff[LUAI_MAXSHORTLEN];
    LoadVector(S, buff, size);    			// 读取文件名 size 个字节
    return luaS_newlstr(S->L, buff, size);	// 创建文件名字符串
  }
  else {  /* long string */
    TString *ts = luaS_createlngstrobj(S->L, size);
    LoadVector(S, getstr(ts), size);  /* load directly in final place */
    return ts;
  }
}

/*
从文件中读取指令
*/
static void LoadCode (LoadState *S, Proto *f) {
  int n = LoadInt(S);  								// 指令条数
  f->code = luaM_newvector(S->L, n, Instruction);	// 分配指令数组空间
  f->sizecode = n;
  LoadVector(S, f->code, n);						// 读取指令块：n * sizeof(Instruction) 个字节
}


static void LoadFunction(LoadState *S, Proto *f, TString *psource);

/*
读取文件中的常量
*/
static void LoadConstants (LoadState *S, Proto *f) {
  int i;
  int n = LoadInt(S); 		// 常量个数
  f->k = luaM_newvector(S->L, n, TValue);	// 为常量分配数组
  f->sizek = n;
  for (i = 0; i < n; i++)	// 常量数组清空
    setnilvalue(&f->k[i]);
  
  for (i = 0; i < n; i++) {
    TValue *o = &f->k[i];
    int t = LoadByte(S);	// 读文件：常量类型
    switch (t) {			// 根据常量类型，读取不同的常量值
    case LUA_TNIL:
      setnilvalue(o);		// 不需要读，直接赋值
      break;
    case LUA_TBOOLEAN:	
      setbvalue(o, LoadByte(S));
      break;
    case LUA_TNUMFLT:
      setfltvalue(o, LoadNumber(S));
      break;
    case LUA_TNUMINT:
      setivalue(o, LoadInteger(S));
      break;
    case LUA_TSHRSTR:
    case LUA_TLNGSTR:
      setsvalue2n(S->L, o, LoadString(S));  // 读取字符串常量
      break;
    default:
      lua_assert(0);
    }
  }
}

/* 
读文件：子函数原型。
*/
static void LoadProtos (LoadState *S, Proto *f) {
  int i;
  int n = LoadInt(S);   // 读文件：子函数原型的个数，4个字节
  f->p = luaM_newvector(S->L, n, Proto *);  // 分配空间
  f->sizep = n;
  for (i = 0; i < n; i++)
    f->p[i] = NULL;
  for (i = 0; i < n; i++) {
    f->p[i] = luaF_newproto(S->L);
    LoadFunction(S, f->p[i], f->source);
  }
}

/*  
读取文件中 upvalue 信息
*/
static void LoadUpvalues (LoadState *S, Proto *f) {
  int i, n;
  n = LoadInt(S);   // 读取 upvalue 个数，4个字节
  f->upvalues = luaM_newvector(S->L, n, Upvaldesc);// 分配 upvalue 描述空间： n *  sizeof(Upvaldesc) 
  f->sizeupvalues = n;
  for (i = 0; i < n; i++)
    f->upvalues[i].name = NULL;
  for (i = 0; i < n; i++) {
    f->upvalues[i].instack = LoadByte(S);
    f->upvalues[i].idx = LoadByte(S);
  }
}


static void LoadDebug (LoadState *S, Proto *f) {
  int i, n;
  n = LoadInt(S);   // 读文件：一共有多少个行号信息
  f->lineinfo = luaM_newvector(S->L, n, int); // 分配空间
  f->sizelineinfo = n;
  LoadVector(S, f->lineinfo, n); // 读文件：所有的行号。 每一个行号信息代表对应指令码所在源文件中的行号
  n = LoadInt(S);  	// 读文件：局部变量表
  f->locvars = luaM_newvector(S->L, n, LocVar);// 分配空间
  f->sizelocvars = n;
  for (i = 0; i < n; i++)
    f->locvars[i].varname = NULL;
  for (i = 0; i < n; i++) {    // 读取局部变量的信息：名称，作用域
    f->locvars[i].varname = LoadString(S);
    f->locvars[i].startpc = LoadInt(S);
    f->locvars[i].endpc = LoadInt(S);
  }
  n = LoadInt(S);   			// upvalue 个数
  for (i = 0; i < n; i++)		// 读取每一个 upvalue 的名称
    f->upvalues[i].name = LoadString(S);
}

/*
从文件中加载一个函数
*/
static void LoadFunction (LoadState *S, Proto *f, TString *psource) {
  f->source = LoadString(S);	/* 执行完之后，f->source  指向文件名字符串 */
  if (f->source == NULL)  		/* no source in dump? */
    f->source = psource;  		/* reuse parent's source */
  f->linedefined = LoadInt(S);	/* 读文件：函数定义起始行号 */
  f->lastlinedefined = LoadInt(S);/* 读文件：函数定义结束行号 */
  f->numparams = LoadByte(S);	/* 读文件：参数个数*/
  f->is_vararg = LoadByte(S);	/* 读文件：是否有可变参数 */
  f->maxstacksize = LoadByte(S);/* 读文件：使用寄存器数量 */
  LoadCode(S, f);		/* 读文件：指令 */
  LoadConstants(S, f);	/* 读文件：常量 */
  LoadUpvalues(S, f);	/* 读文件：upvalue表 */
  LoadProtos(S, f);		/* 读文件：子函数原型表 */
  LoadDebug(S, f);		/* 读文件：调试信息 */
}


static void checkliteral (LoadState *S, const char *s, const char *msg) {
  char buff[sizeof(LUA_SIGNATURE) + sizeof(LUAC_DATA)]; /* larger than both */
  size_t len = strlen(s);
  LoadVector(S, buff, len);
  if (memcmp(s, buff, len) != 0)
    error(S, msg);
}


static void fchecksize (LoadState *S, size_t size, const char *tname) {
  if (LoadByte(S) != size)
    error(S, luaO_pushfstring(S->L, "%s size mismatch in", tname));
}


#define checksize(S,t)	fchecksize(S,sizeof(t),#t)

/*
--demo.lua
print("Hello World")

生成的二进制
1B 4C 75 61 53 00 19 93 0D 0A 1A 0A 04 04 04 08 
08 78 56 00 00 00 00 00 00 00 00 00 00 00 28 77 
40 01 0A 40 64 65 6D 6F 2E 6C 75 61 00 00 00 00 
00 00 00 00 00 01 02 04 00 00 00 06 00 40 00 41 
40 00 00 24 40 00 01 26 00 80 00 02 00 00 00 04 
06 70 72 69 6E 74 04 0D 48 65 6C 6C 6F 20 57 6F 
72 6C 64 21 01 00 00 00 01 00 00 00 00 00 04 00 
00 00 01 00 00 00 01 00 00 00 01 00 00 00 01 00 
00 00 00 00 00 00 01 00 00 00 05 5F 45 4E 56

1B 4C 75 61
"\x1bLua" 用于识别文件格式

53
版本号

00
标准格式

19 93 0D 0A 1A 0A
用于校验的数据块

04 04 04 08 08
分别表示:
int 类型大小
size_t 类型大小
Lua 指令大小
Lua 整数大小
Lua 浮点数大小

78 56 00 00 00 00 00 00
Lua 整数 0x5678

00 00 00 00 00 28 77 40
Lua 浮点数 370.5
*/
static void checkHeader (LoadState *S) {
  checkliteral(S, LUA_SIGNATURE + 1, "not a");  /* 1st char already checked */
  if (LoadByte(S) != LUAC_VERSION)
    error(S, "version mismatch in");
  if (LoadByte(S) != LUAC_FORMAT)
    error(S, "format mismatch in");
  checkliteral(S, LUAC_DATA, "corrupted");  // 标准格式
  checksize(S, int);    				// 5中数据类型的大小
  checksize(S, size_t);
  checksize(S, Instruction);
  checksize(S, lua_Integer);
  checksize(S, lua_Number);
  if (LoadInteger(S) != LUAC_INT)		// 整数
    error(S, "endianness mismatch in");
  if (LoadNumber(S) != LUAC_NUM)		// 浮点数
    error(S, "float format mismatch in");
}


/*
** load precompiled chunk
加载已经编译好的二进制字节码，创建一个 Lua 闭包，并返回

先检查文件头部，再创建一个 Lua 闭包，然后读取文件构造 Lua 闭包中的 proto
*/
LClosure *luaU_undump(lua_State *L, ZIO *Z, const char *name) {
  LoadState S;
  LClosure *cl;
  if (*name == '@' || *name == '=')
    S.name = name + 1;
  else if (*name == LUA_SIGNATURE[0])
    S.name = "binary string";
  else
    S.name = name;
  S.L = L;
  S.Z = Z;
  checkHeader(&S);
  cl = luaF_newLclosure(L, LoadByte(&S));  // 创建 Lua 闭包，读取的一个字节是 upvalue 大小
  setclLvalue(L, L->top, cl);				// 把闭包压栈
  luaD_inctop(L);
  cl->p = luaF_newproto(L);					// 创建一个函数原型
  LoadFunction(&S, cl->p, NULL);			// 读取文件，构造 ci->proto
  lua_assert(cl->nupvalues == cl->p->sizeupvalues);
  luai_verifycode(L, buff, cl->p);
  return cl;
}

