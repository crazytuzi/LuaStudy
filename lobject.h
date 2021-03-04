/*
** $Id: lobject.h,v 2.117.1.1 2017/04/19 17:39:34 roberto Exp $
** Type definitions for Lua objects
** See Copyright Notice in lua.h
*/


#ifndef lobject_h
#define lobject_h


#include <stdarg.h>


#include "llimits.h"
#include "lua.h"


/*
** Extra tags for non-values
*/
#define LUA_TPROTO	LUA_NUMTAGS		/* function prototypes */
#define LUA_TDEADKEY	(LUA_NUMTAGS+1)		/* removed keys in tables */

/*
** number of all possible tags (including LUA_TNONE but excluding DEADKEY)
*/
#define LUA_TOTALTAGS	(LUA_TPROTO + 2)


/*
** tags for Tagged Values have the following use of bits:
** bits 0-3: actual tag (a LUA_T* value)
** bits 4-5: variant bits
** bit 6: whether value is collectable
*/


/*
** LUA_TFUNCTION variants:
** 0 - Lua function
** 1 - light C function
** 2 - regular C function (closure)
*/

/* Variant tags for functions 闭包：Lua, 轻量C，C*/
// 函数类型的细化
#define LUA_TLCL	(LUA_TFUNCTION | (0 << 4))  /* Lua closure */
#define LUA_TLCF	(LUA_TFUNCTION | (1 << 4))  /* light C function */
#define LUA_TCCL	(LUA_TFUNCTION | (2 << 4))  /* C closure */


/* Variant tags for strings 字符串：短字符串，长字符串 */
#define LUA_TSHRSTR	(LUA_TSTRING | (0 << 4))  /* short strings */
#define LUA_TLNGSTR	(LUA_TSTRING | (1 << 4))  /* long strings */


/* Variant tags for numbers 数字：整型，浮点型 */
#define LUA_TNUMFLT	(LUA_TNUMBER | (0 << 4))  /* float numbers */
#define LUA_TNUMINT	(LUA_TNUMBER | (1 << 4))  /* integer numbers */


/* Bit mark for collectable types */
#define BIT_ISCOLLECTABLE	(1 << 6)

/* mark a tag as collectable */
#define ctb(t)			((t) | BIT_ISCOLLECTABLE)


/*
** Common type for all collectable objects
*/
typedef struct GCObject GCObject;


/*
** Common Header for all collectable objects (in macro form, to be
** included in other objects)
*/
#define CommonHeader	GCObject *next; lu_byte tt; lu_byte marked


/*
** Common type has only the common header

上述数据类型大体可以分为两类，需要GC的和 不需要GC的，
需要GC的有
LUA_TSTRING（字符串）、
LUA_TTABLE（表）’
LUA_TFUNCTION（函数）、
LUA_TUSERDATA（指针）
LUA_TTHREAD（线程），

对于需要GC的数据类型，
他们的公共信息是一个简单的宏定义CommonHeader，
并定义了一个只包含CommonHeader的结构体GCObject
*/
struct GCObject {
  CommonHeader;
};




/*
** Tagged Values. This is the basic representation of values in Lua,
** an actual value plus a tag with its type.
*/

/*
** Union of all Lua values
所有对象的基本数据结构.
需要注意的是：Value中存储的是一个GCObject的指针，实际指向的是一个GCUnion类型的值，
这里的Value已经可以表示lua中所有数据类型的值
*/
typedef union Value {
  GCObject *gc;    /* collectable objects */
  void *p;         /* light userdata */
  int b;           /* booleans */
  lua_CFunction f; /* light C functions */
  lua_Integer i;   /* integer numbers */
  lua_Number n;    /* float numbers */
} Value;


/*
tt_标识数据类型：
	0-3 bit 表示基本类型； 
	4-5 bit 表示子类型；
	第 6 bit 表示是否可GC。
*/
#define TValuefields	Value value_; int tt_


typedef struct lua_TValue {
  TValuefields;
} TValue;



/* macro defining a nil value */
#define NILCONSTANT	{NULL}, LUA_TNIL


#define val_(o)		((o)->value_)


/* raw type tag of a TValue */
#define rttype(o)	((o)->tt_)

/* tag with no variants (bits 0-3) */
#define novariant(x)	((x) & 0x0F)

/* type tag of a TValue (bits 0-3 for tags + variant bits 4-5) */
#define ttype(o)	(rttype(o) & 0x3F)

/* type tag of a TValue with no variants (bits 0-3) */
#define ttnov(o)	(novariant(rttype(o)))


/* 检查一个对象是否为指定的类型 Macros to test type */
#define checktag(o,t)		(rttype(o) == (t))
#define checktype(o,t)		(ttnov(o) == (t))
#define ttisnumber(o)		checktype((o), LUA_TNUMBER)
#define ttisfloat(o)		checktag((o), LUA_TNUMFLT)
#define ttisinteger(o)		checktag((o), LUA_TNUMINT)
#define ttisnil(o)		checktag((o), LUA_TNIL)
#define ttisboolean(o)		checktag((o), LUA_TBOOLEAN)
#define ttislightuserdata(o)	checktag((o), LUA_TLIGHTUSERDATA)
#define ttisstring(o)		checktype((o), LUA_TSTRING)
#define ttisshrstring(o)	checktag((o), ctb(LUA_TSHRSTR))
#define ttislngstring(o)	checktag((o), ctb(LUA_TLNGSTR))
#define ttistable(o)		checktag((o), ctb(LUA_TTABLE))
#define ttisfunction(o)		checktype(o, LUA_TFUNCTION)
#define ttisclosure(o)		((rttype(o) & 0x1F) == LUA_TFUNCTION)
#define ttisCclosure(o)		checktag((o), ctb(LUA_TCCL))
#define ttisLclosure(o)		checktag((o), ctb(LUA_TLCL))
#define ttislcf(o)		checktag((o), LUA_TLCF)
#define ttisfulluserdata(o)	checktag((o), ctb(LUA_TUSERDATA))
#define ttisthread(o)		checktag((o), ctb(LUA_TTHREAD))
#define ttisdeadkey(o)		checktag((o), LUA_TDEADKEY)


/* Macros to access values */
// check_exp（）是一个逗号表达式，第一个表达式执行 assert 检查，第二个返回真正要得到的数据.
// 对于非GC数据，直接取Vlaue中对应类型的值;
// 对于GC数据需要先将Vlaue中的gc转为GCUnion*  再去取值 ，
// 例如TString类型，通过（GCUnion*）(t.value_.gc)->ts 取值 
#define ivalue(o)	check_exp(ttisinteger(o), val_(o).i)
#define fltvalue(o)	check_exp(ttisfloat(o), val_(o).n)
#define nvalue(o)	check_exp(ttisnumber(o), \
	(ttisinteger(o) ? cast_num(ivalue(o)) : fltvalue(o)))
#define gcvalue(o)	check_exp(iscollectable(o), val_(o).gc)
#define pvalue(o)	check_exp(ttislightuserdata(o), val_(o).p)
#define tsvalue(o)	check_exp(ttisstring(o), gco2ts(val_(o).gc))
#define uvalue(o)	check_exp(ttisfulluserdata(o), gco2u(val_(o).gc))
#define clvalue(o)	check_exp(ttisclosure(o), gco2cl(val_(o).gc))
#define clLvalue(o)	check_exp(ttisLclosure(o), gco2lcl(val_(o).gc))
#define clCvalue(o)	check_exp(ttisCclosure(o), gco2ccl(val_(o).gc))
#define fvalue(o)	check_exp(ttislcf(o), val_(o).f)
#define hvalue(o)	check_exp(ttistable(o), gco2t(val_(o).gc))
#define bvalue(o)	check_exp(ttisboolean(o), val_(o).b)
#define thvalue(o)	check_exp(ttisthread(o), gco2th(val_(o).gc))
/* a dead value may get the 'gc' field, but cannot access its contents */
#define deadvalue(o)	check_exp(ttisdeadkey(o), cast(void *, val_(o).gc))

#define l_isfalse(o)	(ttisnil(o) || (ttisboolean(o) && bvalue(o) == 0))

// 是否可回收
#define iscollectable(o)	(rttype(o) & BIT_ISCOLLECTABLE)


/* Macros for internal tests */
#define righttt(obj)		(ttype(obj) == gcvalue(obj)->tt)

#define checkliveness(L,obj) \
	lua_longassert(!iscollectable(obj) || \
		(righttt(obj) && (L == NULL || !isdead(G(L),gcvalue(obj)))))


/* Macros to set values */
/*
设置数据
对于非gc数据，直接将值赋给对应字段，设置tt_为对应类型.
*/
#define settt_(o,t)	((o)->tt_=(t))

#define setfltvalue(obj,x) \
  { TValue *io=(obj); val_(io).n=(x); settt_(io, LUA_TNUMFLT); }

#define chgfltvalue(obj,x) \
  { TValue *io=(obj); lua_assert(ttisfloat(io)); val_(io).n=(x); }

#define setivalue(obj,x) \
  { TValue *io=(obj); val_(io).i=(x); settt_(io, LUA_TNUMINT); }

#define chgivalue(obj,x) \
  { TValue *io=(obj); lua_assert(ttisinteger(io)); val_(io).i=(x); }

#define setnilvalue(obj) settt_(obj, LUA_TNIL)

#define setfvalue(obj,x) \
  { TValue *io=(obj); val_(io).f=(x); settt_(io, LUA_TLCF); }

#define setpvalue(obj,x) \
  { TValue *io=(obj); val_(io).p=(x); settt_(io, LUA_TLIGHTUSERDATA); }

#define setbvalue(obj,x) \
  { TValue *io=(obj); val_(io).b=(x); settt_(io, LUA_TBOOLEAN); }

#define setgcovalue(L,obj,x) \
  { TValue *io = (obj); GCObject *i_g=(x); \
    val_(io).gc = i_g; settt_(io, ctb(i_g->tt)); }

/* 
对于 GC 类型：
obj2gcoJ: 将数据转换为GCObject，地址赋值给TVlaue中的gc字段
settt_: 设置tt_为对应类型, ctb是为类型加上 BIT_ISCOLLECTABLE 标志
*/
#define setsvalue(L,obj,x) \
  { TValue *io = (obj); TString *x_ = (x); \
    val_(io).gc = obj2gco(x_); settt_(io, ctb(x_->tt)); \
    checkliveness(L,io); }

#define setuvalue(L,obj,x) \
  { TValue *io = (obj); Udata *x_ = (x); \
    val_(io).gc = obj2gco(x_); settt_(io, ctb(LUA_TUSERDATA)); \
    checkliveness(L,io); }

#define setthvalue(L,obj,x) \
  { TValue *io = (obj); lua_State *x_ = (x); \
    val_(io).gc = obj2gco(x_); settt_(io, ctb(LUA_TTHREAD)); \
    checkliveness(L,io); }

#define setclLvalue(L,obj,x) \
  { TValue *io = (obj); LClosure *x_ = (x); \
    val_(io).gc = obj2gco(x_); settt_(io, ctb(LUA_TLCL)); \
    checkliveness(L,io); }

#define setclCvalue(L,obj,x) \
  { TValue *io = (obj); CClosure *x_ = (x); \
    val_(io).gc = obj2gco(x_); settt_(io, ctb(LUA_TCCL)); \
    checkliveness(L,io); }

#define sethvalue(L,obj,x) \
  { TValue *io = (obj); Table *x_ = (x); \
    val_(io).gc = obj2gco(x_); settt_(io, ctb(LUA_TTABLE)); \
    checkliveness(L,io); }

#define setdeadvalue(obj)	settt_(obj, LUA_TDEADKEY)



#define setobj(L,obj1,obj2) \
	{ TValue *io1=(obj1); *io1 = *(obj2); \
	  (void)L; checkliveness(L,io1); }


/*
** different types of assignments, according to destination
*/

/* from stack to (same) stack */
#define setobjs2s	setobj
/* to stack (not from same stack) */
#define setobj2s	setobj
#define setsvalue2s	setsvalue
#define sethvalue2s	sethvalue
#define setptvalue2s	setptvalue
/* from table to same table */
#define setobjt2t	setobj
/* to new object */
#define setobj2n	setobj
#define setsvalue2n	setsvalue

/* to table (define it as an expression to be used in macros) */
#define setobj2t(L,o1,o2)  ((void)L, *(o1)=*(o2), checkliveness(L,(o1)))




/*
** {======================================================
** types and prototypes
** =======================================================
*/


typedef TValue *StkId;  /* index to stack elements */




/*
** Header for string value; string bytes follow the end of this structure
** (aligned according to 'UTString'; see next).

字符串是存放于全局hash表里, 存放内部化字符串即短字符串时也可能会需要将哈希链表扩大;
实际的字符串数据是跟随在这个结构体之后的字节

extra : 针对短字符串 ，标识当前字符串是不是lua的保留字;
		针对长字符串 ，标识字符串是否已经计算过hash值
shrlen: 短字符串长度
hash：   字符串的散列值， 用于字符串比较的优化
*/
typedef struct TString {
  CommonHeader;   	// 需要进行GC的数据类型的通用头部结构 
  lu_byte extra;  	/* reserved words for short strings; "has hash" for longs */
  lu_byte shrlen;  	/* length for short strings */
  unsigned int hash;
  union {
    size_t lnglen;  /* 长字符串长度 length for long strings  */
    struct TString *hnext;  /* 指向下一个短字符串 linked list for hash table  */
  } u;
} TString;


/*
** Ensures that address after this type is always fully aligned.

定义联合体UTString是单纯为了加快lua中字符串的存取效率

*/
typedef union UTString {
  L_Umaxalign dummy;  /* ensures maximum alignment for strings */
  TString tsv;
} UTString;


/*
** Get the actual string (array of bytes) from a 'TString'.
** (Access to 'extra' ensures that value is really a 'TString'.)

在没有断言的情况下，该宏定义可以简化为 
#define getstr(ts) ((cha*)ts + sizeof(UTString) ) 
*/
#define getstr(ts)  \
  check_exp(sizeof((ts)->extra), cast(char *, (ts)) + sizeof(UTString))


/* get the actual string (array of bytes) from a Lua value */
#define svalue(o)       getstr(tsvalue(o))

/* get string length from 'TString *s' */
#define tsslen(s)	((s)->tt == LUA_TSHRSTR ? (s)->shrlen : (s)->u.lnglen)

/* get string length from 'TValue *o' */
#define vslen(o)	tsslen(tsvalue(o))


/*
** Header for userdata; memory area follows the end of this structure
** (aligned according to 'UUdata'; see next).

实际数据放在结构体的后面
*/
typedef struct Udata {
  CommonHeader;
  lu_byte ttuv_;  /* 数据类型 user value's tag */
  struct Table *metatable;
  size_t len;  /* number of bytes */
  union Value user_;  /* user value */
} Udata;


/*
** Ensures that address after this type is always fully aligned.
*/
typedef union UUdata {
  L_Umaxalign dummy;  /* ensures maximum alignment for 'local' udata */
  Udata uv;
} UUdata;


/*
**  Get the address of memory block inside 'Udata'.
** (Access to 'ttuv_' ensures that value is really a 'Udata'.)
*/
#define getudatamem(u)  \
  check_exp(sizeof((u)->ttuv_), (cast(char*, (u)) + sizeof(UUdata)))

#define setuservalue(L,u,o) \
	{ const TValue *io=(o); Udata *iu = (u); \
	  iu->user_ = io->value_; iu->ttuv_ = rttype(io); \
	  checkliveness(L,io); }


#define getuservalue(L,u,o) \
	{ TValue *io=(o); const Udata *iu = (u); \
	  io->value_ = iu->user_; settt_(io, iu->ttuv_); \
	  checkliveness(L,io); }


/*
** Description of an upvalue for function prototypes
upvalue 叫闭包变量，也叫上值。
为了让 upvalue 可比较，使用Upvaldesc 结构 描述了UpValue 变量的信息。

1. name ：	 upvalue 变量名称。
2. instack ：描述了函数将引用这个upvalue 是否恰好处于定义这个函数的函数中，
			 如果是：upvalue 是这个外层函数的局部变量，它位于数据栈上。
3. idx ： 	 指的是upvalue 的序号。
			 对于关闭的upvalue ，已经无法从栈上获取到，idx 指外层函数的upvalue 表中的索引号；
			 对于在数据栈上的 upvalue ，序号即是变量对应的寄存器号。

这个结构体只是描述了 upvalue的信息，真正的upvalue 变量值存储在 LClosure 结构体重的 upvals 链表中。
*/
typedef struct Upvaldesc {
  TString *name;  	/* upvalue name (for debug information) */
  lu_byte instack;  /* whether it is in stack (register) */
  lu_byte idx;  	/* index of upvalue (in stack or in outer function's list) */
} Upvaldesc;


/*
** Description of a local variable for function prototypes
** (used for debug information)
*/
typedef struct LocVar {
  TString *varname; /* 变量名 */
  int startpc;  	/* 变量的作用域[开始]位置。 first point where variable is active */
  int endpc;    	/* 变量的作用域[结束]位置。 first point where variable is dead */
} LocVar;


/*
** Function Prototypes

Closure对象是lua运行期一个函数的实例对象 ,
我们在运行期调用的都是一个个Cloure对象，
而 Proto 就是 Lua VM 编译系统的中间产物，代表了一个Closure原型的对象，
大部分的函数信息都保持在 Proto 对象中，Proto对象是对用户不可见的。
*/
typedef struct Proto {
  CommonHeader;			/* Proto也是需要回收的对象，也会有与GCHeader对应的CommonHeader */
  lu_byte numparams;  	/* 固定参数个数。 number of fixed parameters */
  lu_byte is_vararg;  	/* 函数是否接收可变参数 */
  lu_byte maxstacksize; /* 使用寄存器个数。 number of registers needed by this function */
  int sizeupvalues;  	/* upvalues 名称的数组长度。 size of 'upvalues' */
  int sizek;  			/* 常量数组长度。 size of 'k' */
  int sizecode;			/* code 数组长度。 */
  int sizelineinfo;		/* lineinfo 数组长度。 */
  int sizep;  			/* p 数组长度。 size of 'p' */
  int sizelocvars;		/* locvars 数组长度 */
  int linedefined;  	/* 函数定义起始行号，即function语句行号 debug information  */
  int lastlinedefined;  /* 函数结束行号，即end语句行号。 debug information  */
  TValue *k;  			/* 函数使用的常量数组,存放则函数要用到的常量。 constants used by the function */
  Instruction *code;  	/* 虚拟机指令码数组。 opcodes */
  struct Proto **p;  	/* 函数里定义的函数的函数原型。 functi。ons defined inside the function */
  int *lineinfo;  		/* 主要用于调试，每个操作码所对应的行号 map from opcodes to source lines (debug information) */
  LocVar *locvars;  	/* 主要用于调试，记录每个本地变量的名称和作用范围。 information about local variables (debug information) */
  Upvaldesc *upvalues;  /* 指向本函数upvalue变量数组。 upvalue information */
  struct LClosure *cache;/* 缓存生成的闭包。last-created closure with this prototype */
  TString  *source; 	/* 用于调试，函数来源，如c:\t1.lua@。 main used for debug information */
  GCObject *gclist;		/* 用于回收 */
} Proto;



/*
** Lua Upvalues
*/
typedef struct UpVal UpVal;


/*
** Closures
nupvalues：upvalue 个数，这个值是在加载 chuck 文件时，从 header 部分读取得到
gclist：    用于GC销毁
*/
#define ClosureHeader \
	CommonHeader; lu_byte nupvalues; GCObject *gclist

// 在绑定到Lua空间的C函数，函数原型就是lua_CFunction的一个函数指针，指向用户绑定的C函数
// 每一个闭包至少有一个 upval，即：_EVN.
typedef struct CClosure {
  ClosureHeader;
  lua_CFunction f;    /* 函数指针，指向自定义的C函数 */
  TValue upvalue[1];  /* C的闭包中，用户绑定的任意数量个upvalue. list of upvalues */
} CClosure;

// Lua 闭包
// 每一个闭包至少有一个 upval，即：_EVN.
typedef struct LClosure {
  ClosureHeader;
  struct Proto *p;	 /* Lua的函数原型 */
  UpVal *upvals[1];  /* list of upvalues */
} LClosure;


typedef union Closure {
  CClosure c;
  LClosure l;
} Closure;


#define isLfunction(o)	ttisLclosure(o)

#define getproto(o)	(clLvalue(o)->p)


/*
** Tables
*/

// hash 表中 node 的键数据结构
typedef union TKey {
  struct {
    TValuefields;
    int next;  /* for chaining (offset for next node) */
  } nk;
  TValue tvk;
} TKey;


/* copy a value into a key without messing up field 'next' */
#define setnodekey(L,key,obj) \
	{ TKey *k_=(key); const TValue *io_=(obj); \
	  k_->nk.value_ = io_->value_; k_->nk.tt_ = io_->tt_; \
	  (void)L; checkliveness(L,io_); }

// hash 链表节点
typedef struct Node {
  TValue i_val;
  TKey i_key;
} Node;


/*
表中包含有hash表node(长度lsizenode)和数组array(长度sizearray)两部分
table 中使用数组来存储 key 为整型的值，array 指向数组首地址
使用hash表来存储 key 为字符串的值，node 指向第一个节点，lastfree指向最后一个空闲节点。

在 node 节点组成的链表中，模拟了类似“哈希桶”的概念，
例如：如果有多个 node 的 key 哈希值相同，那么这些节点之间就会用 i_key.nk.next 串接起来。
*/

typedef struct Table {
  CommonHeader;   			/* 公共头部 */
  lu_byte flags; 			/* 每一个bit标志元方法是否存在。 1<<p means tagmethod(p) is not present */
  lu_byte lsizenode;  		/* 哈希表长度是2的多少次幂。log2 of size of 'node' array */
  unsigned int sizearray;  	/* 数组的长度。size of 'array' array */
  TValue *array;  			/* 数组部分. array part */
  Node *node; 				// 指向哈希表的起始位置
  Node *lastfree;  			/* 指向哈希表的最后一个空闲位置。 any free position is before this position */
  struct Table *metatable;	/* 元表指针 */
  GCObject *gclist;			/* GC相关的链表 */
} Table;



/*
** 'module' operation for hashing (size is always a power of 2)
*/
#define lmod(s,size) \
	(check_exp((size&(size-1))==0, (cast(int, (s) & ((size)-1)))))


#define twoto(x)	(1<<(x))
#define sizenode(t)	(twoto((t)->lsizenode))


/*
** (address of) a fixed nil value
*/
#define luaO_nilobject		(&luaO_nilobject_)


LUAI_DDEC const TValue luaO_nilobject_;

/* size of buffer for 'luaO_utf8esc' function */
#define UTF8BUFFSZ	8

LUAI_FUNC int luaO_int2fb (unsigned int x);
LUAI_FUNC int luaO_fb2int (int x);
LUAI_FUNC int luaO_utf8esc (char *buff, unsigned long x);
LUAI_FUNC int luaO_ceillog2 (unsigned int x);
LUAI_FUNC void luaO_arith (lua_State *L, int op, const TValue *p1,
                           const TValue *p2, TValue *res);
LUAI_FUNC size_t luaO_str2num (const char *s, TValue *o);
LUAI_FUNC int luaO_hexavalue (int c);
LUAI_FUNC void luaO_tostring (lua_State *L, StkId obj);
LUAI_FUNC const char *luaO_pushvfstring (lua_State *L, const char *fmt,
                                                       va_list argp);
LUAI_FUNC const char *luaO_pushfstring (lua_State *L, const char *fmt, ...);
LUAI_FUNC void luaO_chunkid (char *out, const char *source, size_t len);


#endif

