/*
** $Id: lstate.h,v 2.133.1.1 2017/04/19 17:39:34 roberto Exp $
** Global State
** See Copyright Notice in lua.h
*/

#ifndef lstate_h
#define lstate_h

#include "lua.h"

#include "lobject.h"
#include "ltm.h"
#include "lzio.h"


/*

** Some notes about garbage-collected objects: All objects in Lua must
** be kept somehow accessible until being freed, so all objects always
** belong to one (and only one) of these lists, using field 'next' of
** the 'CommonHeader' for the link:
**
** 'allgc': all objects not marked for finalization;
** 'finobj': all objects marked for finalization;
** 'tobefnz': all objects ready to be finalized;
** 'fixedgc': all objects that are not to be collected (currently
** only small strings, such as reserved words).
**
** Moreover, there is another set of lists that control gray objects.
** These lists are linked by fields 'gclist'. (All objects that
** can become gray have such a field. The field is not the same
** in all objects, but it always has this name.)  Any gray object
** must belong to one of these lists, and all objects in these lists
** must be gray:
**
** 'gray': regular gray objects, still waiting to be visited.
** 'grayagain': objects that must be revisited at the atomic phase.
**   That includes
**   - black objects got in a write barrier;
**   - all kinds of weak tables during propagation phase;
**   - all threads.
** 'weak': tables with weak values to be cleared;
** 'ephemeron': ephemeron tables with white->white entries;
** 'allweak': tables with weak keys and/or weak values to be cleared.
** The last three lists are used only during the atomic phase.

*/


struct lua_longjmp;  /* defined in ldo.c */


/*
** Atomic type (relative to signals) to better ensure that 'lua_sethook'
** is thread safe
*/
#if !defined(l_signalT)
#include <signal.h>
#define l_signalT	sig_atomic_t
#endif


/* extra stack space to handle TM calls and some other extras */
#define EXTRA_STACK   5


#define BASIC_STACK_SIZE        (2*LUA_MINSTACK)


/* kinds of Garbage Collection */
#define KGC_NORMAL	0
#define KGC_EMERGENCY	1	/* gc was forced by an allocation failure */

/*
在 Lua中短字符串是被内化的 ，什么是内化 ？ 
简单来说 ， 每个存放Lua字符串的变量 ，实际上存放的只是字符串数据的引用，
lua中有一个全局的地方存放着当前系统中所有字符串，每当创建一个新的字符串，
首先去查找是否已经存在同样的字符串。有的话，直接将引用指向已经存在的
字符串数据 ，否则在系统内创建一个新的字符串数据，复制引用 。

hash： 指向哈希桶的指针数组，数组中的每个元素都是一个 TString * 指针。
nuse： 当前哈希桶内的字符串数量 
size ：当前哈希桶的字符串容量
*/
typedef struct stringtable {
  TString **hash;
  int nuse;  /* number of elements */
  int size;
} stringtable;


/*
** Information about a call.
** When a thread yields, 'func' is adjusted to pretend that the
** top function has only the yielded values in its stack; in that
** case, the actual 'func' value is saved in field 'extra'.
** When a function calls another with a continuation, 'extra' keeps
** the function index so that, in case of errors, the continuation
** function can be called with the correct top.

Lua 把调用栈和数据栈分开保存。调用栈放在 CallInfo 结构中，
以双向链表的形式存储在 lua_State 对象里。

CallInfo 保存着正在调用的函数的运行状态。
CallInfo 是一个标准的双向链表结构，不直接被GC 管理。
这个链表表达的是一个逻辑上的栈，在运行过程中，并不是每次调用更深层次的函数，
就立刻构造出一个 CallInfo 节点 。整个 CallInfo 链表会在运行中反复复用。
直到 GC 的时候才清理那些比当前调用层次更深的无用节点。
*/
typedef struct CallInfo {
  StkId func;  	/* 指向正在执行的函数在数据栈上的位置,会初始化为 L1->top. function index in the stack */
  StkId	top; 	/* 初始化为：L1->top + LUA_MINSTACK。 top for this function */
  struct CallInfo *previous, *next;  /* dynamic call link */
  union {
    struct {  					/* only for Lua functions */
      StkId base;  				/* 调用此闭包时的第一个参数所在的数据栈位置，也就是 func 上一个位置。base for this function */
      const Instruction *savedpc; // 指令指针，指向 Proto 中的 code 指令数组中的某个位置
    } l;
    struct {  					/* only for C functions */
      lua_KFunction k;  		/* continuation in case of yields */
      ptrdiff_t old_errfunc;
      lua_KContext ctx;  		/* context info. in case of yields */
    } c;
  } u;
  ptrdiff_t extra;
  short nresults;  				/* expected number of results from this function */
  unsigned short callstatus;    /* 状态标识 */
} CallInfo;


/*
** Bits in CallInfo status
*/
#define CIST_OAH	(1<<0)	/* original value of 'allowhook' */
#define CIST_LUA	(1<<1)	/* call is running a Lua function */
#define CIST_HOOKED	(1<<2)	/* call is running a debug hook */
#define CIST_FRESH	(1<<3)	/* call is running on a fresh invocation
                                   of luaV_execute */
#define CIST_YPCALL	(1<<4)	/* call is a yieldable protected call */
#define CIST_TAIL	(1<<5)	/* call was tail called */
#define CIST_HOOKYIELD	(1<<6)	/* last hook called yielded */
#define CIST_LEQ	(1<<7)  /* using __lt for __le */
#define CIST_FIN	(1<<8)  /* call is running a finalizer */

#define isLua(ci)	((ci)->callstatus & CIST_LUA)

/* assume that CIST_OAH has offset 0 and that 'v' is strictly 0/1 */
#define setoah(st,v)	((st) = ((st) & ~CIST_OAH) | (v))
#define getoah(st)	((st) & CIST_OAH)


/*
** 'global state', shared by all threads of this state
全局状态机: 被所有的协程共享 
作用：管理全局数据，全局字符串表、内存管理函数、 GC 把所有对象串联起来的信息、内存等
*/
typedef struct global_State {
  lua_Alloc frealloc;  	/* 指向 Lua的全局内存分配器. function to reallocate memory */
  void *ud;         	/* 分配器的userdata. data auxiliary data to 'frealloc' */
  l_mem totalbytes;  	/* number of bytes currently allocated - GCdebt */
  l_mem GCdebt;  		/* bytes allocated not yet compensated by the collector */
  lu_mem GCmemtrav;  	/* memory traversed by the GC */
  lu_mem GCestimate;  	/* an estimate of the non-garbage memory in use */
  stringtable strt;  	/* 全局字符串表, 字符串池化. hash table for strings */
  TValue l_registry; 	/* 注册表（管理全局数据） */
  unsigned int seed;  	/* randomized seed for hashes */
  lu_byte currentwhite;
  lu_byte gcstate;  	/* state of garbage collector */
  lu_byte gckind;  		/* kind of GC running */
  lu_byte gcrunning;  	/* true if GC is running */
  GCObject *allgc;  	/* list of all collectable objects */
  GCObject **sweepgc;  	/* current position of sweep in list */
  GCObject *finobj;  	/* list of collectable objects with finalizers */
  GCObject *gray;  		/* list of gray objects */
  GCObject *grayagain;  /* list of objects to be traversed atomically */
  GCObject *weak;  		/* list of tables with weak values */
  GCObject *ephemeron;  /* list of ephemeron tables (weak keys) */
  GCObject *allweak;  	/* list of all-weak tables */
  GCObject *tobefnz;  	/* list of userdata to be GC */
  GCObject *fixedgc;  	/* list of objects not to be collected */
  struct lua_State *twups;  /* 闭包了当前线程（协程）变量的其他线程列表. list of threads with open upvalues */
  unsigned int gcfinnum;/* number of finalizers to call in each GC step */
  int gcpause;  		/* size of pause between successive GCs */
  int gcstepmul;  		/* GC 'granularity' */
  lua_CFunction panic;  /* 全局错误处理. to be called in unprotected errors */
  struct lua_State *mainthread; /* 主线程（协程） */
  const lua_Number *version;  	/* pointer to version number */
  TString *memerrmsg;  			/* memory-error message */
  TString *tmname[TM_N];  		/* 元方法名称。array with tag-method names */
  struct Table *mt[LUA_NUMTAGS];/* 基本类型共享的元表。metatables for basic types */
  TString *strcache[STRCACHE_N][STRCACHE_M];  /* cache for strings in API */
} global_State;


/*
** 'per thread' state
函数调用栈：调用栈是由CallInfo 结构串起来的链表，函数执行过程中，动态的调整CallInfo 结构。
调用栈是一个链表，而数据栈是一个数组，这是他们之间的区别。

ci 指针指向当前调用函数；
base_ci 记录调用栈的栈底（最外层的CallInfo）, base_ci 一定是从 C 函数发起的调用；

而调用栈的栈顶，一定是当前正在执行的函数的CallInfo。
另外 nCcalls 字段记录调用栈中调用 C 函数的个数，而 nny字段记录着 non-yieldable 的调用个数。


闭包变量信息：C Closure和Lua Closure都会有闭包变量。
C Closure的闭包直接就是一个TValue数组保存在CClosure里，
而Lua Closure的闭包变量，分为open和close两种状态，
如果是close状态，则拷贝到LClosure自己的UpVal数组里，
但如果是open状态，则直接指向了作用域上的变量地址。 调用栈展开过程中，
从调用栈的栈顶的所有open 状态的 UpVal 也构成 栈结构（链表串起来的），

一个lua-State 代表一个协程,一个协程能闭包其它协程的变量，所以 twups 就是代表 
其它的协程（闭包了当前的lua-state 的变量）。
*/
struct lua_State {
  CommonHeader;        		/* 公共头部 */
  unsigned short nci;  		/* 函数调用栈 c 的个数. number of items in 'ci' list */
  lu_byte status;    		/* 每一个协程实际上就是一个死循环解释执行指令的容器，本质上是一个状态机，该字段表示中间步骤的状态。 */
  StkId top;  		 		/* 数据栈栈顶：一个动态 TValue 数组. first free slot in the stack */
  global_State *l_G; 		/* Lua的全局对象，只存在一份，所有的 L 都共享这个 G。 */
  
  CallInfo *ci;  	 		/* 函数调用栈. call info for current function */
  const Instruction *oldpc; /* 指向最后一次执行的指令. last pc traced */
  StkId stack_last;  		/* 指向栈最后的使用位置，即：栈空间的上限，也就是栈指针不能再往上增长了。 last free slot in the stack */
  StkId stack;  	 		/* 数据栈栈底. stack base */
  UpVal *openupval;  		/* 保存在数据栈中的 upval。list of open upvalues in this stack */
  GCObject *gclist;
  struct lua_State *twups;  /* 闭包了当前的lua-state 的变量的其它协程. list of threads with open upvalues */
  struct lua_longjmp *errorJmp;  /* 错误恢复点。 current error recover point */
  CallInfo base_ci;  		/* 第一个调用栈。 CallInfo for first level (C calling Lua) */
  volatile lua_Hook hook;
  ptrdiff_t errfunc;  		/* current error handling function (stack index) */
  int stacksize;
  int basehookcount;
  int hookcount;
  unsigned short nny;  	  	/* 记录着 non-yieldable 的调用个数. number of non-yieldable calls in stack */
  unsigned short nCcalls; 	/* 记录调用栈中调用 C 函数的个数. number of nested C calls */
  l_signalT hookmask;
  lu_byte allowhook;
};


#define G(L)	(L->l_G)


/*
** Union of all collectable objects (only for conversions)

联合体GCUnion来表示所有需要GC的数据类型.
*/
union GCUnion {
  GCObject gc;  /* common header */
  struct TString ts;
  struct Udata u;
  union Closure cl;
  struct Table h;
  struct Proto p;
  struct lua_State th;  /* thread */
};


#define cast_u(o)	cast(union GCUnion *, (o))

/* 获取 union GCObject 结构中具体的指定数据类型.
macros to convert a GCObject into a specific value */
#define gco2ts(o)  \
	check_exp(novariant((o)->tt) == LUA_TSTRING, &((cast_u(o))->ts))
#define gco2u(o)  check_exp((o)->tt == LUA_TUSERDATA, &((cast_u(o))->u))
#define gco2lcl(o)  check_exp((o)->tt == LUA_TLCL, &((cast_u(o))->cl.l))
#define gco2ccl(o)  check_exp((o)->tt == LUA_TCCL, &((cast_u(o))->cl.c))
#define gco2cl(o)  \
	check_exp(novariant((o)->tt) == LUA_TFUNCTION, &((cast_u(o))->cl))
#define gco2t(o)  check_exp((o)->tt == LUA_TTABLE, &((cast_u(o))->h))
#define gco2p(o)  check_exp((o)->tt == LUA_TPROTO, &((cast_u(o))->p))
#define gco2th(o)  check_exp((o)->tt == LUA_TTHREAD, &((cast_u(o))->th))


/* macro to convert a Lua object into a GCObject */
#define obj2gco(v) \
	check_exp(novariant((v)->tt) < LUA_TDEADKEY, (&(cast_u(v)->gc)))


/* actual number of total bytes allocated */
#define gettotalbytes(g)	cast(lu_mem, (g)->totalbytes + (g)->GCdebt)

LUAI_FUNC void luaE_setdebt (global_State *g, l_mem debt);
LUAI_FUNC void luaE_freethread (lua_State *L, lua_State *L1);
LUAI_FUNC CallInfo *luaE_extendCI (lua_State *L);
LUAI_FUNC void luaE_freeCI (lua_State *L);
LUAI_FUNC void luaE_shrinkCI (lua_State *L);


#endif

