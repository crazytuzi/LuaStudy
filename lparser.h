/*
** $Id: lparser.h,v 1.76.1.1 2017/04/19 17:20:42 roberto Exp $
** Lua Parser
** See Copyright Notice in lua.h
*/

#ifndef lparser_h
#define lparser_h

#include "llimits.h"
#include "lobject.h"
#include "lzio.h"


/*
** Expression and variable descriptor.
** Code generation for variables and expressions can be delayed to allow
** optimizations; An 'expdesc' structure describes a potentially-delayed
** variable/expression. It has a description of its "main" value plus a
** list of conditional jumps that can also produce its value (generated
** by short-circuit operators 'and'/'or').
*/

/* kinds of variables/expressions */
typedef enum {
  VVOID,  /* when 'expdesc' describes the last expression a list,
             this kind means an empty list (so, no expression) */
  VNIL,  /* constant nil */
  VTRUE,  /* constant true */
  VFALSE,  /* constant false */
  VK,  /* constant in 'k'; info = index of constant in 'k' */
  VKFLT,  /* floating constant; nval = numerical float value */
  VKINT,  /* integer constant; nval = numerical integer value */
  VNONRELOC,  /* expression has its value in a fixed register;
                 info = result register */
  VLOCAL,  /* local variable; info = local register */
  VUPVAL,  /* upvalue variable; info = index of upvalue in 'upvalues' */
  VINDEXED,  /* indexed variable;
                ind.vt = whether 't' is register or upvalue;
                ind.t = table register or upvalue;
                ind.idx = key's R/K index */
  VJMP,  /* expression is a test/comparison;
            info = pc of corresponding jump instruction */
  VRELOCABLE,  /* expression can put result in any register;
                  info = instruction pc */
  VCALL,  /* expression is a function call; info = instruction pc */
  VVARARG  /* vararg expression; info = instruction pc */
} expkind;


#define vkisvar(k)	(VLOCAL <= (k) && (k) <= VINDEXED)
#define vkisinreg(k)	((k) == VNONRELOC || (k) == VLOCAL)

/*
存放了表达式的相关描述信息
*/
typedef struct expdesc {
  expkind k;			/* 表达式的种类 */
  union {
    lua_Integer ival;   /* for VKINT */
    lua_Number nval;  	/* for VKFLT */
    int info;  			/* for generic use */
    struct {  			/* for indexed variables (VINDEXED) */
      short idx;  		/* index (R/K) */
      lu_byte t;  		/* table (register or upvalue) */
      lu_byte vt;  		/* whether 't' is register (VLOCAL) or upvalue (VUPVAL) */
    } ind;
  } u;
  int t;  				/* patch list of 'exit when true' */
  int f;  				/* patch list of 'exit when false' */
} expdesc;


/* description of active local variable */
typedef struct Vardesc {
  short idx;  /* variable index in stack */
} Vardesc;


/* description of pending goto statements and label statements */
typedef struct Labeldesc {
  TString *name;  /* label identifier */
  int pc;  /* position in code */
  int line;  /* line where it appeared */
  lu_byte nactvar;  /* local level where it appears in current block */
} Labeldesc;


/* list of labels or gotos */
typedef struct Labellist {
  Labeldesc *arr;  /* array */
  int n;  /* number of entries in use */
  int size;  /* array size */
} Labellist;


/* dynamic structures used by the parser */
typedef struct Dyndata {
  struct {  /* list of active local variables */
    Vardesc *arr;    // 局部变量在数组中的索引
    int n;			 // 局部变量数组中的有效元素个数
    int size;		 // 局部变量数组的容量
  } actvar;
  Labellist gt;  /* list of pending gotos */
  Labellist label;   /* list of active labels */
} Dyndata;


/* control of blocks */
struct BlockCnt;  /* defined in lparser.c */


/* state needed to generate code for a given function */
/* 在编译过程中，使用FuncState结构体来保存一个函数编译的状态数据 */
typedef struct FuncState {
  Proto *f;  		/* 指向了本函数的协议描述结构体。 current function header */
  struct FuncState *prev;  /* 指向了其父函数的FuncState描述。 enclosing function */
  							/*
  								因为在lua中可以在一个函数中定义另一个函数，因此当parse到
  								一个函数的内部函数的定义时会new一个FuncState来描述内部函数，
  								同时开始parse这个内部函数，将这个FuncState的prev指向其
  								外部函数的FuncState，prev变量用来引用外围函数的FuncState，
  								使当前所有没有分析完成的FuncState形成一个栈结构。
  							*/
  struct LexState *ls;  	/* lexical state */
  struct BlockCnt *bl;  	/* 指向当前parse的block，在一个函数中会有很多block代码，
  								lua会将这些同属于同一个函数的block用链表串联起来。
  								chain of current blocks */
  int pc;  			/* next position to code (equivalent to 'ncode') */
  int lasttarget;   /* 'label' of last 'jump label' */
  int jpc;  		/* 是一个OP_JMP指令的链表，因为lua是一遍过的parse，在开始的时候有一些
  						跳转指令不能决定其跳转位置，因此jpc将这些pending jmp指令串联起来，
  						在以后能确定的时候回填.
  						list of pending jumps to 'pc' */
  int nk;  			/* number of elements in 'k' */
  int np;  			/* number of elements in 'p' */
  int firstlocal;  	/* index of first local var (in Dyndata array) */
  short nlocvars;  	/* number of elements in 'f->locvars' */
  lu_byte nactvar;  /* 当前作用域的局部变量数。number of active local variables */
  lu_byte nups;  	/* upvalue 的数量。number of upvalues */
  lu_byte freereg;  /* 第一个空闲寄存器的下标。 first free register */
} FuncState;


LUAI_FUNC LClosure *luaY_parser (lua_State *L, ZIO *z, Mbuffer *buff,
                                 Dyndata *dyd, const char *name, int firstchar);


#endif
