/*
** $Id: lparser.c,v 2.155.1.2 2017/04/29 18:11:40 roberto Exp $
** Lua Parser
** See Copyright Notice in lua.h
*/

#define lparser_c
#define LUA_CORE

#include "lprefix.h"


#include <string.h>

#include "lua.h"

#include "lcode.h"
#include "ldebug.h"
#include "ldo.h"
#include "lfunc.h"
#include "llex.h"
#include "lmem.h"
#include "lobject.h"
#include "lopcodes.h"
#include "lparser.h"
#include "lstate.h"
#include "lstring.h"
#include "ltable.h"



/* maximum number of local variables per function (must be smaller
   than 250, due to the bytecode format) */
#define MAXVARS		200


#define hasmultret(k)		((k) == VCALL || (k) == VVARARG)


/* because all strings are unified by the scanner, the parser
   can use pointer equality for string equality */
#define eqstr(a,b)	((a) == (b))


/*
** nodes for block list (list of active blocks)
一个 block 块，比如：if 语句；for循环语句。。。
*/
typedef struct BlockCnt {
  struct BlockCnt *previous;  /* 保存外围block的引用，形成一个栈结构。 chain */
  int firstlabel;  	/* index of first label in this block */
  int firstgoto;  	/* index of first pending goto in this block */
  lu_byte nactvar;  /* # active locals outside the block */
  lu_byte upval;  	/* true if some variable in the block is an upvalue */
  lu_byte isloop;  	/* true if 'block' is a loop */
} BlockCnt;



/*
** prototypes for recursive non-terminal functions
*/
static void statement (LexState *ls);
static void expr (LexState *ls, expdesc *v);


/* semantic error */
static l_noret semerror (LexState *ls, const char *msg) {
  ls->t.token = 0;  /* remove "near <token>" from final message */
  luaX_syntaxerror(ls, msg);
}


static l_noret error_expected (LexState *ls, int token) {
  luaX_syntaxerror(ls,
      luaO_pushfstring(ls->L, "%s expected", luaX_token2str(ls, token)));
}


static l_noret errorlimit (FuncState *fs, int limit, const char *what) {
  lua_State *L = fs->ls->L;
  const char *msg;
  int line = fs->f->linedefined;
  const char *where = (line == 0)
                      ? "main function"
                      : luaO_pushfstring(L, "function at line %d", line);
  msg = luaO_pushfstring(L, "too many %s (limit is %d) in %s",
                             what, limit, where);
  luaX_syntaxerror(fs->ls, msg);
}


static void checklimit (FuncState *fs, int v, int l, const char *what) {
  if (v > l) errorlimit(fs, l, what);
}


static int testnext (LexState *ls, int c) {
  if (ls->t.token == c) {
    luaX_next(ls);
    return 1;
  }
  else return 0;
}


static void check (LexState *ls, int c) {
  if (ls->t.token != c)
    error_expected(ls, c);
}


static void checknext (LexState *ls, int c) {
  check(ls, c);
  luaX_next(ls);
}


#define check_condition(ls,c,msg)	{ if (!(c)) luaX_syntaxerror(ls, msg); }



static void check_match (LexState *ls, int what, int who, int where) {
  if (!testnext(ls, what)) {
    if (where == ls->linenumber)
      error_expected(ls, what);
    else {
      luaX_syntaxerror(ls, luaO_pushfstring(ls->L,
             "%s expected (to close %s at line %d)",
              luaX_token2str(ls, what), luaX_token2str(ls, who), where));
    }
  }
}


static TString *str_checkname (LexState *ls) {
  TString *ts;
  check(ls, TK_NAME);
  ts = ls->t.seminfo.ts;
  luaX_next(ls);
  return ts;
}


static void init_exp (expdesc *e, expkind k, int i) {
  e->f = e->t = NO_JUMP;
  e->k = k;
  e->u.info = i;
}

// 把 s 放入函数原型 Proto 的常量数组中，索引放在 e.u.info 中
static void codestring (LexState *ls, expdesc *e, TString *s) {
  init_exp(e, VK, luaK_stringK(ls->fs, s));
}


static void checkname (LexState *ls, expdesc *e) {
  codestring(ls, e, str_checkname(ls));
}

/* 
把局部变量保存在 FuncState 的 locvars 数组中。
返回值：变量在数组中的索引
*/
static int registerlocalvar (LexState *ls, TString *varname) {
  FuncState *fs = ls->fs;
  Proto *f = fs->f;
  int oldsize = f->sizelocvars;
  luaM_growvector(ls->L, f->locvars, fs->nlocvars, f->sizelocvars, // 如果局部变量数组长度不够，扩充
                  LocVar, SHRT_MAX, "local variables");
  while (oldsize < f->sizelocvars)
    f->locvars[oldsize++].varname = NULL;  		// reset 扩充部分
  f->locvars[fs->nlocvars].varname = varname;	// 保存局部变量在函数的数组中
  luaC_objbarrier(ls->L, f, varname);
  return fs->nlocvars++;
}

/*
创建局部变量，保存在函数的局部变量数组中。
*/
static void new_localvar (LexState *ls, TString *name) {
  FuncState *fs = ls->fs;
  Dyndata *dyd = ls->dyd;
  int reg = registerlocalvar(ls, name);	// 把变量保存在 ls->fs->f(Proto)->locvars[] 数组中，返回值是数组的索引

  /*
  	检查一个函数的局部变量个数是否已经最大
  	在lua中，定义了一个宏LUAI_MAXVARS，是200，也就是说lua中一个函数所能容纳的局部变量个数是200个。
  	如果局部变量的个数已经达到了200个，此时再向里面注册局部变量，lua就会报错，
  	通过函数errorlimit()函数报错。
  */
  checklimit(fs, dyd->actvar.n + 1 - fs->firstlocal,
                  MAXVARS, "local variables");
  luaM_growvector(ls->L, dyd->actvar.arr, dyd->actvar.n + 1,
                  dyd->actvar.size, Vardesc, MAX_INT, "local variables");
  dyd->actvar.arr[dyd->actvar.n++].idx = cast(short, reg);  // 把局部变量在数组中的位置信息保存在 dyd->actvar 中.
}


static void new_localvarliteral_ (LexState *ls, const char *name, size_t sz) {
  new_localvar(ls, luaX_newstring(ls, name, sz));
}

#define new_localvarliteral(ls,v) \
	new_localvarliteral_(ls, "" v, (sizeof(v)/sizeof(char))-1)


static LocVar *getlocvar (FuncState *fs, int i) {
  int idx = fs->ls->dyd->actvar.arr[fs->firstlocal + i].idx;
  lua_assert(idx < fs->nlocvars);
  return &fs->f->locvars[idx];
}


static void adjustlocalvars (LexState *ls, int nvars) {
  FuncState *fs = ls->fs;
  fs->nactvar = cast_byte(fs->nactvar + nvars);
  for (; nvars; nvars--) {
    getlocvar(fs, fs->nactvar - nvars)->startpc = fs->pc;
  }
}


static void removevars (FuncState *fs, int tolevel) {
  fs->ls->dyd->actvar.n -= (fs->nactvar - tolevel);
  while (fs->nactvar > tolevel)
    getlocvar(fs, --fs->nactvar)->endpc = fs->pc;
}


static int searchupvalue (FuncState *fs, TString *name) {
  int i;
  Upvaldesc *up = fs->f->upvalues;
  for (i = 0; i < fs->nups; i++) {
    if (eqstr(up[i].name, name)) return i;
  }
  return -1;  /* not found */
}

/* 新建一个 upvalue，并存储到 Proto 结构中. */
static int newupvalue (FuncState *fs, TString *name, expdesc *v) {
  Proto *f = fs->f;
  int oldsize = f->sizeupvalues;
  checklimit(fs, fs->nups + 1, MAXUPVAL, "upvalues");
  luaM_growvector(fs->ls->L, f->upvalues, fs->nups, f->sizeupvalues,
                  Upvaldesc, MAXUPVAL, "upvalues");
  while (oldsize < f->sizeupvalues)
    f->upvalues[oldsize++].name = NULL;
  f->upvalues[fs->nups].instack = (v->k == VLOCAL);    // 设置 upvalue 的属性
  f->upvalues[fs->nups].idx = cast_byte(v->u.info);
  f->upvalues[fs->nups].name = name;
  luaC_objbarrier(fs->ls->L, f, name);
  return fs->nups++;  // upvalue计数递增
}


static int searchvar (FuncState *fs, TString *n) {
  int i;
  for (i = cast_int(fs->nactvar) - 1; i >= 0; i--) {
    if (eqstr(n, getlocvar(fs, i)->varname))
      return i;
  }
  return -1;  /* not found */
}


/*
  Mark block where variable at given level was defined
  (to emit close instructions later).
*/
static void markupval (FuncState *fs, int level) {
  BlockCnt *bl = fs->bl;
  while (bl->nactvar > level)
    bl = bl->previous;
  bl->upval = 1;
}


/*
  Find variable with given name 'n'. If it is an upvalue, add this
  upvalue into all intermediate functions.
*/
static void singlevaraux (FuncState *fs, TString *n, expdesc *var, int base) {
  if (fs == NULL)  		/* 这个函数会被自己递归调用，在上一个层次中查找，所以最上面的一层中 fs=NULL。 no more levels? */
    init_exp(var, VVOID, 0);  	 /* 说明是全局的。default is global */
  else {
    int v = searchvar(fs, n);    /* 在当前的函数局部变量数组中查找。look up locals at current level */
    if (v >= 0) {  				 /* found? */
      init_exp(var, VLOCAL, v);  /* variable is local */
      if (!base)
        markupval(fs, v);  		 /* local will be used as an upval */
    }
    else {  	/* 不是局部变量，在 upvalues 中查找。 not found as local at current level; try upvalues */
      int idx = searchupvalue(fs, n);   /* 在当前函数的 upvalues 中查找。 try existing upvalues */
      if (idx < 0) {  					/* not found? */
        singlevaraux(fs->prev, n, var, 0);  /* 根据fs的prev成员取得其父函数的FuncState并传入singlevaraux中递归查找。try upper levels */
        if (var->k == VVOID)  				/* not found? */
          return;  /* it is a global */
		
        /* else was LOCAL or UPVAL */
        /* 在递归查找时，如果在上面的某个层找到了该变量，那么在递归返回的时候，
        	需要在这中间的每一个层中都记录这个upvalue。
        */
        idx  = newupvalue(fs, n, var);  	/* 创建一个 upvalue。will be a new upvalue */
      }
      init_exp(var, VUPVAL, idx);  			/* new or old upvalue */
    }
  }
}


/*
处理一个标识符
*/
static void singlevar (LexState *ls, expdesc *var) {
  TString *varname = str_checkname(ls);
  FuncState *fs = ls->fs;
  
  singlevaraux(fs, varname, var, 1); // 在局部变量或者upvalue数组中查找，得到的索引值放在v.u.info中

  if (var->k == VVOID) {  /* global name? */
  	// 全局变量
    expdesc key;
    singlevaraux(fs, ls->envn, var, 1); /* 查找全局变量表字符串。get environment variable */
    lua_assert(var->k != VVOID);  		/* 一定能找到(var->k=VLOCAL)。 this one must exist */
    codestring(ls, &key, varname);  	/* key代表了 varname。key is variable name */
    luaK_indexed(fs, var, &key);  		/* env[varname] */
  }
}

// 调整变量赋值
static void adjust_assign (LexState *ls, int nvars, int nexps, expdesc *e) {
  FuncState *fs = ls->fs;
  int extra = nvars - nexps;
  if (hasmultret(e->k)) {
    extra++;  /* includes call itself */
    if (extra < 0) extra = 0;
    luaK_setreturns(fs, e, extra);  /* last exp. provides the difference */
    if (extra > 1) luaK_reserveregs(fs, extra-1);
  }
  else {
    if (e->k != VVOID) luaK_exp2nextreg(fs, e);  /* close last expression */
    if (extra > 0) {         // '=' 右侧的表达式不够左侧的变量，填充NIL
      int reg = fs->freereg;
      luaK_reserveregs(fs, extra);
      luaK_nil(fs, reg, extra);
    }
  }
  if (nexps > nvars)
    ls->fs->freereg -= nexps - nvars;  /* remove extra values */
}


static void enterlevel (LexState *ls) {
  lua_State *L = ls->L;
  ++L->nCcalls;
  checklimit(ls->fs, L->nCcalls, LUAI_MAXCCALLS, "C levels");
}


#define leavelevel(ls)	((ls)->L->nCcalls--)


static void closegoto (LexState *ls, int g, Labeldesc *label) {
  int i;
  FuncState *fs = ls->fs;
  Labellist *gl = &ls->dyd->gt;
  Labeldesc *gt = &gl->arr[g];
  lua_assert(eqstr(gt->name, label->name));
  if (gt->nactvar < label->nactvar) {
    TString *vname = getlocvar(fs, gt->nactvar)->varname;
    const char *msg = luaO_pushfstring(ls->L,
      "<goto %s> at line %d jumps into the scope of local '%s'",
      getstr(gt->name), gt->line, getstr(vname));
    semerror(ls, msg);
  }
  luaK_patchlist(fs, gt->pc, label->pc);
  /* remove goto from pending list */
  for (i = g; i < gl->n - 1; i++)
    gl->arr[i] = gl->arr[i + 1];
  gl->n--;
}


/*
** try to close a goto with existing labels; this solves backward jumps
*/
static int findlabel (LexState *ls, int g) {
  int i;
  BlockCnt *bl = ls->fs->bl;
  Dyndata *dyd = ls->dyd;
  Labeldesc *gt = &dyd->gt.arr[g];
  /* check labels in current block for a match */
  for (i = bl->firstlabel; i < dyd->label.n; i++) {
    Labeldesc *lb = &dyd->label.arr[i];
    if (eqstr(lb->name, gt->name)) {  /* correct label? */
      if (gt->nactvar > lb->nactvar &&
          (bl->upval || dyd->label.n > bl->firstlabel))
        luaK_patchclose(ls->fs, gt->pc, lb->nactvar);
      closegoto(ls, g, lb);  /* close it */
      return 1;
    }
  }
  return 0;  /* label not found; cannot close goto */
}


static int newlabelentry (LexState *ls, Labellist *l, TString *name,
                          int line, int pc) {
  int n = l->n;
  luaM_growvector(ls->L, l->arr, n, l->size,
                  Labeldesc, SHRT_MAX, "labels/gotos");
  l->arr[n].name = name;
  l->arr[n].line = line;
  l->arr[n].nactvar = ls->fs->nactvar;
  l->arr[n].pc = pc;
  l->n = n + 1;
  return n;
}


/*
** check whether new label 'lb' matches any pending gotos in current
** block; solves forward jumps
*/
static void findgotos (LexState *ls, Labeldesc *lb) {
  Labellist *gl = &ls->dyd->gt;
  int i = ls->fs->bl->firstgoto;
  while (i < gl->n) {
    if (eqstr(gl->arr[i].name, lb->name))
      closegoto(ls, i, lb);
    else
      i++;
  }
}


/*
** export pending gotos to outer level, to check them against
** outer labels; if the block being exited has upvalues, and
** the goto exits the scope of any variable (which can be the
** upvalue), close those variables being exited.
*/
static void movegotosout (FuncState *fs, BlockCnt *bl) {
  int i = bl->firstgoto;
  Labellist *gl = &fs->ls->dyd->gt;
  /* correct pending gotos to current block and try to close it
     with visible labels */
  while (i < gl->n) {
    Labeldesc *gt = &gl->arr[i];
    if (gt->nactvar > bl->nactvar) {
      if (bl->upval)
        luaK_patchclose(fs, gt->pc, bl->nactvar);
      gt->nactvar = bl->nactvar;
    }
    if (!findlabel(fs->ls, i))
      i++;  /* move to next one */
  }
}


static void enterblock (FuncState *fs, BlockCnt *bl, lu_byte isloop) {
  bl->isloop = isloop;
  bl->nactvar = fs->nactvar;
  bl->firstlabel = fs->ls->dyd->label.n;
  bl->firstgoto = fs->ls->dyd->gt.n;
  bl->upval = 0;
  bl->previous = fs->bl;
  fs->bl = bl;
  lua_assert(fs->freereg == fs->nactvar);
}


/*
** create a label named 'break' to resolve break statements
*/
static void breaklabel (LexState *ls) {
  TString *n = luaS_new(ls->L, "break");
  int l = newlabelentry(ls, &ls->dyd->label, n, 0, ls->fs->pc);
  findgotos(ls, &ls->dyd->label.arr[l]);
}

/*
** generates an error for an undefined 'goto'; choose appropriate
** message when label name is a reserved word (which can only be 'break')
*/
static l_noret undefgoto (LexState *ls, Labeldesc *gt) {
  const char *msg = isreserved(gt->name)
                    ? "<%s> at line %d not inside a loop"
                    : "no visible label '%s' for <goto> at line %d";
  msg = luaO_pushfstring(ls->L, msg, getstr(gt->name), gt->line);
  semerror(ls, msg);
}


static void leaveblock (FuncState *fs) {
  BlockCnt *bl = fs->bl;
  LexState *ls = fs->ls;
  if (bl->previous && bl->upval) {
    /* create a 'jump to here' to close upvalues */
    int j = luaK_jump(fs);
    luaK_patchclose(fs, j, bl->nactvar);
    luaK_patchtohere(fs, j);
  }
  if (bl->isloop)
    breaklabel(ls);  /* close pending breaks */
  fs->bl = bl->previous;
  removevars(fs, bl->nactvar);
  lua_assert(bl->nactvar == fs->nactvar);
  fs->freereg = fs->nactvar;  /* free registers */
  ls->dyd->label.n = bl->firstlabel;  /* remove local labels */
  if (bl->previous)  /* inner block? */
    movegotosout(fs, bl);  /* update pending gotos to outer block */
  else if (bl->firstgoto < ls->dyd->gt.n)  /* pending gotos in outer block? */
    undefgoto(ls, &ls->dyd->gt.arr[bl->firstgoto]);  /* error */
}


/*
** adds a new prototype into list of prototypes
*/
static Proto *addprototype (LexState *ls) {
  Proto *clp;
  lua_State *L = ls->L;
  FuncState *fs = ls->fs;
  Proto *f = fs->f;  /* prototype of current function */
  if (fs->np >= f->sizep) {
    int oldsize = f->sizep;
    luaM_growvector(L, f->p, fs->np, f->sizep, Proto *, MAXARG_Bx, "functions");
    while (oldsize < f->sizep)
      f->p[oldsize++] = NULL;
  }
  f->p[fs->np++] = clp = luaF_newproto(L);
  luaC_objbarrier(L, f, clp);
  return clp;
}


/*
** codes instruction to create new closure in parent function.
** The OP_CLOSURE instruction must use the last available register,
** so that, if it invokes the GC, the GC knows which registers
** are in use at that time.
*/
static void codeclosure (LexState *ls, expdesc *v) {
  FuncState *fs = ls->fs->prev;
  init_exp(v, VRELOCABLE, luaK_codeABx(fs, OP_CLOSURE, 0, fs->np - 1));
  luaK_exp2nextreg(fs, v);  /* fix it at the last register */
}


static void open_func (LexState *ls, FuncState *fs, BlockCnt *bl) {
  Proto *f;
  fs->prev = ls->fs;  /* linked list of funcstates */
  fs->ls = ls;
  ls->fs = fs;
  fs->pc = 0;
  fs->lasttarget = 0;
  fs->jpc = NO_JUMP;
  fs->freereg = 0;
  fs->nk = 0;
  fs->np = 0;
  fs->nups = 0;
  fs->nlocvars = 0;
  fs->nactvar = 0;
  fs->firstlocal = ls->dyd->actvar.n;
  fs->bl = NULL;
  f = fs->f;
  f->source = ls->source;
  f->maxstacksize = 2;  /* registers 0/1 are always valid */
  enterblock(fs, bl, 0);
}


static void close_func (LexState *ls) {
  lua_State *L = ls->L;
  FuncState *fs = ls->fs;
  Proto *f = fs->f;
  luaK_ret(fs, 0, 0);  /* final return */
  leaveblock(fs);
  luaM_reallocvector(L, f->code, f->sizecode, fs->pc, Instruction);
  f->sizecode = fs->pc;
  luaM_reallocvector(L, f->lineinfo, f->sizelineinfo, fs->pc, int);
  f->sizelineinfo = fs->pc;
  luaM_reallocvector(L, f->k, f->sizek, fs->nk, TValue);
  f->sizek = fs->nk;
  luaM_reallocvector(L, f->p, f->sizep, fs->np, Proto *);
  f->sizep = fs->np;
  luaM_reallocvector(L, f->locvars, f->sizelocvars, fs->nlocvars, LocVar);
  f->sizelocvars = fs->nlocvars;
  luaM_reallocvector(L, f->upvalues, f->sizeupvalues, fs->nups, Upvaldesc);
  f->sizeupvalues = fs->nups;
  lua_assert(fs->bl == NULL);
  ls->fs = fs->prev;
  luaC_checkGC(L);
}



/*============================================================*/
/* GRAMMAR RULES */
/*============================================================*/


/*
** check whether current token is in the follow set of a block.
** 'until' closes syntactical blocks, but do not close scope,
** so it is handled in separate.
*/
static int block_follow (LexState *ls, int withuntil) {
  switch (ls->t.token) {
    case TK_ELSE: 
	case TK_ELSEIF:
    case TK_END: 
	case TK_EOS:
    	return 1;
    case TK_UNTIL: 
		return withuntil;
    default: 
    	return 0;
  }
}

/*
循环分析语句
*/
static void statlist (LexState *ls) {
  /* statlist -> { stat [';'] } */
  while (!block_follow(ls, 1)) {   	// 判断 token 的类型
    if (ls->t.token == TK_RETURN) {
      statement(ls);				// 最后一个语法块
      return;  						/* 'return' must be last statement */
    }
    statement(ls);					// 处理一个语句
  }
}


static void fieldsel (LexState *ls, expdesc *v) {
  /* fieldsel -> ['.' | ':'] NAME */
  FuncState *fs = ls->fs;
  expdesc key;
  luaK_exp2anyregup(fs, v);
  luaX_next(ls);  /* skip the dot or colon */
  checkname(ls, &key);
  luaK_indexed(fs, v, &key);
}


static void yindex (LexState *ls, expdesc *v) {
  /* index -> '[' expr ']' */
  luaX_next(ls);  /* skip the '[' */
  expr(ls, v);
  luaK_exp2val(ls->fs, v);
  checknext(ls, ']');
}


/*
** {======================================================================
** Rules for Constructors
** =======================================================================
*/


struct ConsControl {
  expdesc v;  /* last list item read */
  expdesc *t;  /* table descriptor */
  int nh;  /* total number of 'record' elements */
  int na;  /* total number of array elements */
  int tostore;  /* number of array elements pending to be stored */
};


static void recfield (LexState *ls, struct ConsControl *cc) {
  /* recfield -> (NAME | '['exp1']') = exp1 */
  FuncState *fs = ls->fs;
  int reg = ls->fs->freereg;
  expdesc key, val;
  int rkkey;
  if (ls->t.token == TK_NAME) {
    checklimit(fs, cc->nh, MAX_INT, "items in a constructor");
    checkname(ls, &key);
  }
  else  /* ls->t.token == '[' */
    yindex(ls, &key);
  cc->nh++;
  checknext(ls, '=');
  rkkey = luaK_exp2RK(fs, &key);
  expr(ls, &val);
  luaK_codeABC(fs, OP_SETTABLE, cc->t->u.info, rkkey, luaK_exp2RK(fs, &val));
  fs->freereg = reg;  /* free registers */
}


static void closelistfield (FuncState *fs, struct ConsControl *cc) {
  if (cc->v.k == VVOID) return;  /* there is no list item */
  luaK_exp2nextreg(fs, &cc->v);
  cc->v.k = VVOID;
  if (cc->tostore == LFIELDS_PER_FLUSH) {
    luaK_setlist(fs, cc->t->u.info, cc->na, cc->tostore);  /* flush */
    cc->tostore = 0;  /* no more items pending */
  }
}


static void lastlistfield (FuncState *fs, struct ConsControl *cc) {
  if (cc->tostore == 0) return;
  if (hasmultret(cc->v.k)) {
    luaK_setmultret(fs, &cc->v);
    luaK_setlist(fs, cc->t->u.info, cc->na, LUA_MULTRET);
    cc->na--;  /* do not count last expression (unknown number of elements) */
  }
  else {
    if (cc->v.k != VVOID)
      luaK_exp2nextreg(fs, &cc->v);
    luaK_setlist(fs, cc->t->u.info, cc->na, cc->tostore);
  }
}


static void listfield (LexState *ls, struct ConsControl *cc) {
  /* listfield -> exp */
  expr(ls, &cc->v);
  checklimit(ls->fs, cc->na, MAX_INT, "items in a constructor");
  cc->na++;
  cc->tostore++;
}


static void field (LexState *ls, struct ConsControl *cc) {
  /* field -> listfield | recfield */
  switch(ls->t.token) {
    case TK_NAME: {  /* may be 'listfield' or 'recfield' */
      if (luaX_lookahead(ls) != '=')  /* expression? */
        listfield(ls, cc);
      else
        recfield(ls, cc);
      break;
    }
    case '[': {
      recfield(ls, cc);
      break;
    }
    default: {
      listfield(ls, cc);
      break;
    }
  }
}


static void constructor (LexState *ls, expdesc *t) {
  /* constructor -> '{' [ field { sep field } [sep] ] '}'
     sep -> ',' | ';' */
  FuncState *fs = ls->fs;
  int line = ls->linenumber;
  int pc = luaK_codeABC(fs, OP_NEWTABLE, 0, 0, 0);
  struct ConsControl cc;
  cc.na = cc.nh = cc.tostore = 0;
  cc.t = t;
  init_exp(t, VRELOCABLE, pc);
  init_exp(&cc.v, VVOID, 0);  /* no value (yet) */
  luaK_exp2nextreg(ls->fs, t);  /* fix it at stack top */
  checknext(ls, '{');
  do {
    lua_assert(cc.v.k == VVOID || cc.tostore > 0);
    if (ls->t.token == '}') break;
    closelistfield(fs, &cc);
    field(ls, &cc);
  } while (testnext(ls, ',') || testnext(ls, ';'));
  check_match(ls, '}', '{', line);
  lastlistfield(fs, &cc);
  SETARG_B(fs->f->code[pc], luaO_int2fb(cc.na)); /* set initial array size */
  SETARG_C(fs->f->code[pc], luaO_int2fb(cc.nh));  /* set initial table size */
}

/* }====================================================================== */



static void parlist (LexState *ls) {
  /* parlist -> [ param { ',' param } ] */
  FuncState *fs = ls->fs;
  Proto *f = fs->f;
  int nparams = 0;
  f->is_vararg = 0;
  if (ls->t.token != ')') {  /* is 'parlist' not empty? */
    do {
      switch (ls->t.token) {
        case TK_NAME: {  /* param -> NAME */
          new_localvar(ls, str_checkname(ls));
          nparams++;
          break;
        }
        case TK_DOTS: {  /* param -> '...' */
          luaX_next(ls);
          f->is_vararg = 1;  /* declared vararg */
          break;
        }
        default: luaX_syntaxerror(ls, "<name> or '...' expected");
      }
    } while (!f->is_vararg && testnext(ls, ','));
  }
  adjustlocalvars(ls, nparams);
  f->numparams = cast_byte(fs->nactvar);
  luaK_reserveregs(fs, fs->nactvar);  /* reserve register for parameters */
}


static void body (LexState *ls, expdesc *e, int ismethod, int line) {
  /* body ->  '(' parlist ')' block END */
  FuncState new_fs;
  BlockCnt bl;
  new_fs.f = addprototype(ls);
  new_fs.f->linedefined = line;
  open_func(ls, &new_fs, &bl);
  checknext(ls, '(');
  if (ismethod) {
    new_localvarliteral(ls, "self");  /* create 'self' parameter */
    adjustlocalvars(ls, 1);
  }
  parlist(ls);
  checknext(ls, ')');
  statlist(ls);
  new_fs.f->lastlinedefined = ls->linenumber;
  check_match(ls, TK_END, TK_FUNCTION, line);
  codeclosure(ls, e);
  close_func(ls);
}


/*
解析表达式列表。
例如： locak a,b=1+2,3*4
就需要解析‘=’ 右侧部分
*/
static int explist (LexState *ls, expdesc *v) {
  /* explist -> expr { ',' expr } */
  int n = 1;  		/* 表达式个数。 at least one expression */
  expr(ls, v);
  while (testnext(ls, ',')) {
    luaK_exp2nextreg(ls->fs, v);
    expr(ls, v);
    n++;
  }
  return n;
}


static void funcargs (LexState *ls, expdesc *f, int line) {
  FuncState *fs = ls->fs;
  expdesc args;
  int base, nparams;
  switch (ls->t.token) {
    case '(': {  /* funcargs -> '(' [ explist ] ')' */
      luaX_next(ls);
      if (ls->t.token == ')')  /* arg list is empty? */
        args.k = VVOID;
      else {
        explist(ls, &args);
        luaK_setmultret(fs, &args);
      }
      check_match(ls, ')', '(', line);
      break;
    }
    case '{': {  /* funcargs -> constructor */
      constructor(ls, &args);
      break;
    }
    case TK_STRING: {  /* funcargs -> STRING */
      codestring(ls, &args, ls->t.seminfo.ts);
      luaX_next(ls);  /* must use 'seminfo' before 'next' */
      break;
    }
    default: {
      luaX_syntaxerror(ls, "function arguments expected");
    }
  }
  lua_assert(f->k == VNONRELOC);
  base = f->u.info;  /* base register for call */
  if (hasmultret(args.k))
    nparams = LUA_MULTRET;  /* open call */
  else {
    if (args.k != VVOID)
      luaK_exp2nextreg(fs, &args);  /* close last argument */
    nparams = fs->freereg - (base+1);
  }
  init_exp(f, VCALL, luaK_codeABC(fs, OP_CALL, base, nparams+1, 2));
  luaK_fixline(fs, line);
  fs->freereg = base+1;  /* call remove function and arguments and leaves
                            (unless changed) one result */
}


/*
** {======================================================================
** Expression parsing
** =======================================================================
*/
static void primaryexp (LexState *ls, expdesc *v) {
  /* primaryexp -> NAME | '(' expr ')' */
  switch (ls->t.token) {
    case '(': {
      int line = ls->linenumber;
      luaX_next(ls);
      expr(ls, v);
      check_match(ls, ')', '(', line);
      luaK_dischargevars(ls->fs, v);
      return;
    }
    case TK_NAME: {
      singlevar(ls, v);   // 解析表达式：在局部变量, upvalue 或者全局变量中取查找当前的token
      return;
    }
    default: {
      luaX_syntaxerror(ls, "unexpected symbol");
    }
  }
}


static void suffixedexp (LexState *ls, expdesc *v) {
  /* suffixedexp ->
       primaryexp { '.' NAME | '[' exp ']' | ':' NAME funcargs | funcargs } */
  FuncState *fs = ls->fs;
  int line = ls->linenumber;
  
  primaryexp(ls, v);	// 分析主表达式
  
  for (;;) {
    switch (ls->t.token) {
      case '.': {  /* fieldsel */
        fieldsel(ls, v);
        break;
      }
      case '[': {  /* '[' exp1 ']' */
        expdesc key;
        luaK_exp2anyregup(fs, v);
        yindex(ls, &key);
        luaK_indexed(fs, v, &key);
        break;
      }
      case ':': {  /* ':' NAME funcargs */
        expdesc key;
        luaX_next(ls);
        checkname(ls, &key);
        luaK_self(fs, v, &key);
        funcargs(ls, v, line);
        break;
      }
      case '(': case TK_STRING: case '{': {  /* funcargs */
        luaK_exp2nextreg(fs, v);
        funcargs(ls, v, line);
        break;
      }
      default: return;
    }
  }
}


static void simpleexp (LexState *ls, expdesc *v) {
  /* simpleexp -> FLT | INT | STRING | NIL | TRUE | FALSE | ... |
                  constructor | FUNCTION body | suffixedexp */
  switch (ls->t.token) {
    case TK_FLT: {
      init_exp(v, VKFLT, 0);
      v->u.nval = ls->t.seminfo.r;
      break;
    }
    case TK_INT: {
      init_exp(v, VKINT, 0);
      v->u.ival = ls->t.seminfo.i;
      break;
    }
    case TK_STRING: {
      codestring(ls, v, ls->t.seminfo.ts);// 字符串添加到常量表中，然后返回其下标。
      break;
    }
    case TK_NIL: {
      init_exp(v, VNIL, 0);
      break;
    }
    case TK_TRUE: {
      init_exp(v, VTRUE, 0);
      break;
    }
    case TK_FALSE: {
      init_exp(v, VFALSE, 0);
      break;
    }
    case TK_DOTS: {  /* 点操作符。 vararg */
      FuncState *fs = ls->fs;
      check_condition(ls, fs->f->is_vararg,
                      "cannot use '...' outside a vararg function");
      init_exp(v, VVARARG, luaK_codeABC(fs, OP_VARARG, 0, 1, 0));
      break;
    }
    case '{': {  /* constructor */
      constructor(ls, v);
      return;
    }
    case TK_FUNCTION: {
      luaX_next(ls);
      body(ls, v, 0, ls->linenumber);
      return;
    }
    default: {
      suffixedexp(ls, v);   // 当成后缀表达式进行分析
      return;
    }
  }
  luaX_next(ls);
}


static UnOpr getunopr (int op) {
  switch (op) {
    case TK_NOT: return OPR_NOT;
    case '-': return OPR_MINUS;
    case '~': return OPR_BNOT;
    case '#': return OPR_LEN;
    default: return OPR_NOUNOPR;
  }
}


static BinOpr getbinopr (int op) {
  switch (op) {
    case '+': return OPR_ADD;
    case '-': return OPR_SUB;
    case '*': return OPR_MUL;
    case '%': return OPR_MOD;
    case '^': return OPR_POW;
    case '/': return OPR_DIV;
    case TK_IDIV: return OPR_IDIV;
    case '&': return OPR_BAND;
    case '|': return OPR_BOR;
    case '~': return OPR_BXOR;
    case TK_SHL: return OPR_SHL;
    case TK_SHR: return OPR_SHR;
    case TK_CONCAT: return OPR_CONCAT;
    case TK_NE: return OPR_NE;
    case TK_EQ: return OPR_EQ;
    case '<': return OPR_LT;
    case TK_LE: return OPR_LE;
    case '>': return OPR_GT;
    case TK_GE: return OPR_GE;
    case TK_AND: return OPR_AND;
    case TK_OR: return OPR_OR;
    default: return OPR_NOBINOPR;
  }
}


static const struct {
  lu_byte left;  /* left priority for each binary operator */
  lu_byte right; /* right priority */
} priority[] = {  /* ORDER OPR */
   {10, 10}, {10, 10},           /* '+' '-' */
   {11, 11}, {11, 11},           /* '*' '%' */
   {14, 13},                  /* '^' (right associative) */
   {11, 11}, {11, 11},           /* '/' '//' */
   {6, 6}, {4, 4}, {5, 5},   /* '&' '|' '~' */
   {7, 7}, {7, 7},           /* '<<' '>>' */
   {9, 8},                   /* '..' (right associative) */
   {3, 3}, {3, 3}, {3, 3},   /* ==, <, <= */
   {3, 3}, {3, 3}, {3, 3},   /* ~=, >, >= */
   {2, 2}, {1, 1}            /* and, or */
};

#define UNARY_PRIORITY	12  /* priority for unary operators */


/*
** subexpr -> (simpleexp | unop subexpr) { binop subexpr }
** where 'binop' is any binary operator with a priority higher than 'limit'
*/
static BinOpr subexpr (LexState *ls, expdesc *v, int limit) {
  BinOpr op;
  UnOpr uop;
  enterlevel(ls);
  uop = getunopr(ls->t.token);
  if (uop != OPR_NOUNOPR) {
  	// 如果是一元运算符，进入这里
    int line = ls->linenumber;
    luaX_next(ls);
    subexpr(ls, v, UNARY_PRIORITY);
    luaK_prefix(ls->fs, uop, v, line);
  }
  else 
  {
  	simpleexp(ls, v);   // 简单表达式，结果存储在 v 中（从 seminfo 语义变量中得到）
  }

  /* expand while operators have priorities higher than 'limit' */
  op = getbinopr(ls->t.token);
  while (op != OPR_NOBINOPR && priority[op].left > limit) {
  	// 二元操作符，例如： +， -， * 。。。。
    expdesc v2;
    BinOpr nextop;
    int line = ls->linenumber;
    luaX_next(ls);   		// 取下一个 token
    luaK_infix(ls->fs, op, v);
    /* read sub-expression with higher priority */
    nextop = subexpr(ls, &v2, priority[op].right);
    luaK_posfix(ls->fs, op, v, &v2, line);   // 返回结果放在 v 里面
    op = nextop;
  }
  leavelevel(ls);
  return op;  /* return first untreated operator */
}

// 解析一个表达式
static void expr (LexState *ls, expdesc *v) {
  subexpr(ls, v, 0);
}

/* }==================================================================== */



/*
** {======================================================================
** Rules for Statements
** =======================================================================
*/


static void block (LexState *ls) {
  /* block -> statlist */
  FuncState *fs = ls->fs;
  BlockCnt bl;
  enterblock(fs, &bl, 0);
  statlist(ls);
  leaveblock(fs);
}


/*
** structure to chain all variables in the left-hand side of an
** assignment
处理多变量赋值的情况
*/
struct LHS_assign {
  struct LHS_assign *prev;
  expdesc v;  /* variable (global, local, upvalue, or indexed) */
};


/*
** check whether, in an assignment to an upvalue/local variable, the
** upvalue/local variable is begin used in a previous assignment to a
** table. If so, save original upvalue/local value in a safe place and
** use this safe copy in the previous assignment.
*/
static void check_conflict (LexState *ls, struct LHS_assign *lh, expdesc *v) {
  FuncState *fs = ls->fs;
  int extra = fs->freereg;  /* eventual position to save local variable */
  int conflict = 0;
  for (; lh; lh = lh->prev) {  /* check all previous assignments */
    if (lh->v.k == VINDEXED) {  /* assigning to a table? */
      /* table is the upvalue/local being assigned now? */
      if (lh->v.u.ind.vt == v->k && lh->v.u.ind.t == v->u.info) {
        conflict = 1;
        lh->v.u.ind.vt = VLOCAL;
        lh->v.u.ind.t = extra;  /* previous assignment will use safe copy */
      }
      /* index is the local being assigned? (index cannot be upvalue) */
      if (v->k == VLOCAL && lh->v.u.ind.idx == v->u.info) {
        conflict = 1;
        lh->v.u.ind.idx = extra;  /* previous assignment will use safe copy */
      }
    }
  }
  if (conflict) {
    /* copy upvalue/local value to a temporary (in position 'extra') */
    OpCode op = (v->k == VLOCAL) ? OP_MOVE : OP_GETUPVAL;
    luaK_codeABC(fs, op, extra, v->u.info, 0);
    luaK_reserveregs(fs, 1);
  }
}

/*
变量赋值操作。
lh: 以链表形式存储的变量名称。
nvars: 值的个数
*/
static void assignment (LexState *ls, struct LHS_assign *lh, int nvars) {
  expdesc e;
  check_condition(ls, vkisvar(lh->v.k), "syntax error");
  if (testnext(ls, ',')) {  /* assignment -> ',' suffixedexp assignment */
    struct LHS_assign nv;
    nv.prev = lh;
    suffixedexp(ls, &nv.v);
    if (nv.v.k != VINDEXED)
      check_conflict(ls, lh, &nv.v);
    checklimit(ls->fs, nvars + ls->L->nCcalls, LUAI_MAXCCALLS,
                    "C levels");
    assignment(ls, &nv, nvars+1);
  }
  else {  /* assignment -> '=' explist */
    int nexps;
    checknext(ls, '=');						// 跳转到下一个token
    nexps = explist(ls, &e);
    if (nexps != nvars)						// 比较 = 两侧的数量是否相等
      adjust_assign(ls, nvars, nexps, &e); 	// 调整
    else {
      luaK_setoneret(ls->fs, &e); 			 /* close last expression */
      luaK_storevar(ls->fs, &lh->v, &e);
      return;  /* avoid default */
    }
  }
  init_exp(&e, VNONRELOC, ls->fs->freereg-1);  /* default assignment */
  luaK_storevar(ls->fs, &lh->v, &e);
}


static int cond (LexState *ls) {
  /* cond -> exp */
  expdesc v;
  expr(ls, &v);  /* read condition */
  if (v.k == VNIL) v.k = VFALSE;  /* 'falses' are all equal here */
  luaK_goiftrue(ls->fs, &v);
  return v.f;
}


static void gotostat (LexState *ls, int pc) {
  int line = ls->linenumber;
  TString *label;
  int g;
  if (testnext(ls, TK_GOTO))
    label = str_checkname(ls);
  else {
    luaX_next(ls);  /* skip break */
    label = luaS_new(ls->L, "break");
  }
  g = newlabelentry(ls, &ls->dyd->gt, label, line, pc);
  findlabel(ls, g);  /* close it if label already defined */
}


/* check for repeated labels on the same block */
static void checkrepeated (FuncState *fs, Labellist *ll, TString *label) {
  int i;
  for (i = fs->bl->firstlabel; i < ll->n; i++) {
    if (eqstr(label, ll->arr[i].name)) {
      const char *msg = luaO_pushfstring(fs->ls->L,
                          "label '%s' already defined on line %d",
                          getstr(label), ll->arr[i].line);
      semerror(fs->ls, msg);
    }
  }
}


/* skip no-op statements */
static void skipnoopstat (LexState *ls) {
  while (ls->t.token == ';' || ls->t.token == TK_DBCOLON)
    statement(ls);
}


static void labelstat (LexState *ls, TString *label, int line) {
  /* label -> '::' NAME '::' */
  FuncState *fs = ls->fs;
  Labellist *ll = &ls->dyd->label;
  int l;  /* index of new label being created */
  checkrepeated(fs, ll, label);  /* check for repeated labels */
  checknext(ls, TK_DBCOLON);  /* skip double colon */
  /* create new entry for this label */
  l = newlabelentry(ls, ll, label, line, luaK_getlabel(fs));
  skipnoopstat(ls);  /* skip other no-op statements */
  if (block_follow(ls, 0)) {  /* label is last no-op statement in the block? */
    /* assume that locals are already out of scope */
    ll->arr[l].nactvar = fs->bl->nactvar;
  }
  findgotos(ls, &ll->arr[l]);
}


static void whilestat (LexState *ls, int line) {
  /* whilestat -> WHILE cond DO block END */
  FuncState *fs = ls->fs;
  int whileinit;
  int condexit;
  BlockCnt bl;
  luaX_next(ls);  /* skip WHILE */
  whileinit = luaK_getlabel(fs);
  condexit = cond(ls);
  enterblock(fs, &bl, 1);
  checknext(ls, TK_DO);
  block(ls);
  luaK_jumpto(fs, whileinit);
  check_match(ls, TK_END, TK_WHILE, line);
  leaveblock(fs);
  luaK_patchtohere(fs, condexit);  /* false conditions finish the loop */
}


static void repeatstat (LexState *ls, int line) {
  /* repeatstat -> REPEAT block UNTIL cond */
  int condexit;
  FuncState *fs = ls->fs;
  int repeat_init = luaK_getlabel(fs);
  BlockCnt bl1, bl2;
  enterblock(fs, &bl1, 1);  /* loop block */
  enterblock(fs, &bl2, 0);  /* scope block */
  luaX_next(ls);  /* skip REPEAT */
  statlist(ls);
  check_match(ls, TK_UNTIL, TK_REPEAT, line);
  condexit = cond(ls);  /* read condition (inside scope block) */
  if (bl2.upval)  /* upvalues? */
    luaK_patchclose(fs, condexit, bl2.nactvar);
  leaveblock(fs);  /* finish scope */
  luaK_patchlist(fs, condexit, repeat_init);  /* close the loop */
  leaveblock(fs);  /* finish loop */
}


static int exp1 (LexState *ls) {
  expdesc e;
  int reg;
  expr(ls, &e);
  luaK_exp2nextreg(ls->fs, &e);
  lua_assert(e.k == VNONRELOC);
  reg = e.u.info;
  return reg;
}


static void forbody (LexState *ls, int base, int line, int nvars, int isnum) {
  /* forbody -> DO block */
  BlockCnt bl;
  FuncState *fs = ls->fs;
  int prep, endfor;
  adjustlocalvars(ls, 3);  /* control variables */
  checknext(ls, TK_DO);
  prep = isnum ? luaK_codeAsBx(fs, OP_FORPREP, base, NO_JUMP) : luaK_jump(fs);
  enterblock(fs, &bl, 0);  /* scope for declared variables */
  adjustlocalvars(ls, nvars);
  luaK_reserveregs(fs, nvars);
  block(ls);
  leaveblock(fs);  /* end of scope for declared variables */
  luaK_patchtohere(fs, prep);
  if (isnum)  /* numeric for? */
    endfor = luaK_codeAsBx(fs, OP_FORLOOP, base, NO_JUMP);
  else {  /* generic for */
    luaK_codeABC(fs, OP_TFORCALL, base, 0, nvars);
    luaK_fixline(fs, line);
    endfor = luaK_codeAsBx(fs, OP_TFORLOOP, base + 2, NO_JUMP);
  }
  luaK_patchlist(fs, endfor, prep + 1);
  luaK_fixline(fs, line);
}


static void fornum (LexState *ls, TString *varname, int line) {
  /* fornum -> NAME = exp1,exp1[,exp1] forbody */
  FuncState *fs = ls->fs;
  int base = fs->freereg;
  new_localvarliteral(ls, "(for index)");
  new_localvarliteral(ls, "(for limit)");
  new_localvarliteral(ls, "(for step)");
  new_localvar(ls, varname);
  checknext(ls, '=');
  exp1(ls);  /* initial value */
  checknext(ls, ',');
  exp1(ls);  /* limit */
  if (testnext(ls, ','))
    exp1(ls);  /* optional step */
  else {  /* default step = 1 */
    luaK_codek(fs, fs->freereg, luaK_intK(fs, 1));
    luaK_reserveregs(fs, 1);
  }
  forbody(ls, base, line, 1, 1);
}


static void forlist (LexState *ls, TString *indexname) {
  /* forlist -> NAME {,NAME} IN explist forbody */
  FuncState *fs = ls->fs;
  expdesc e;
  int nvars = 4;  /* gen, state, control, plus at least one declared var */
  int line;
  int base = fs->freereg;
  /* create control variables */
  new_localvarliteral(ls, "(for generator)");
  new_localvarliteral(ls, "(for state)");
  new_localvarliteral(ls, "(for control)");
  /* create declared variables */
  new_localvar(ls, indexname);
  while (testnext(ls, ',')) {
    new_localvar(ls, str_checkname(ls));
    nvars++;
  }
  checknext(ls, TK_IN);
  line = ls->linenumber;
  adjust_assign(ls, 3, explist(ls, &e), &e);
  luaK_checkstack(fs, 3);  /* extra space to call generator */
  forbody(ls, base, line, nvars - 3, 0);
}


static void forstat (LexState *ls, int line) {
  /* forstat -> FOR (fornum | forlist) END */
  FuncState *fs = ls->fs;
  TString *varname;
  BlockCnt bl;
  enterblock(fs, &bl, 1);  /* scope for loop and control variables */
  luaX_next(ls);  /* skip 'for' */
  varname = str_checkname(ls);  /* first variable name */
  switch (ls->t.token) {
    case '=': fornum(ls, varname, line); break;
    case ',': case TK_IN: forlist(ls, varname); break;
    default: luaX_syntaxerror(ls, "'=' or 'in' expected");
  }
  check_match(ls, TK_END, TK_FOR, line);
  leaveblock(fs);  /* loop scope ('break' jumps to this point) */
}

/*
	每次处理完一个 test_then_block 之后，会生成类似下面的指令码：
	n	TEST ...  // 条件判断表达式
	n+1	JMP  n+5  // 当判断为false时，跳转到n+5的位置
	n+2	XXX       // 当判断为true 时，执行的指令
	n+3	XXX
	n+4	JMP  n+i  // true 分支执行结束之后，需要跳转到if-end之后的位置
	n+5 TEST ...
	..
	n+i ZZZ		   // if..end 之后的代码生成的指令

	在处理 test_then_block 生成上面代码的过程中，需要记住2点：
	1.每次生成一个指令码时，都会把 fs->jpc 跳转指令链表中的指令跳转值设置为 fs->pc，也就是当前生成的指令。
	2.n+1行：false分支的这个JMP指令，会被添加到 jf 中，然后被追加到 fs->jpc 链表中。
	3.当遇到 else/elseif 时，说明当前处理的then部分结束了，那么就要生成一个 JMP 指令，跳过下面的 else/elseif 
		部分，直接跳转到if-end后面的指令。但是当前还不知道end在哪里，因此先追加到 escapelist 链表中。
  */

static void test_then_block (LexState *ls, int *escapelist) {
  /* test_then_block -> [IF | ELSEIF] cond THEN block */
  BlockCnt bl;
  FuncState *fs = ls->fs;
  expdesc v;
  int jf;  /* instruction to skip 'then' code (if condition is false) */
  luaX_next(ls);  			/* 跳过 IF or ELSEIF */
  
  expr(ls, &v);  			/* 处理条件表达式，判断结果 true/false 放在 v 里返回。read condition */

  checknext(ls, TK_THEN);	// 跳过 THEN
  
  if (ls->t.token == TK_GOTO || ls->t.token == TK_BREAK) {
    luaK_goiffalse(ls->fs, &v); /* will jump to label if condition is true */
    enterblock(fs, &bl, 0);  	/* must enter block before 'goto' */
    gotostat(ls, v.t);  		/* handle goto/break */
    while (testnext(ls, ';')) {}  /* skip colons */
    if (block_follow(ls, 0)) {  /* 'goto' is the entire block? */
      leaveblock(fs);
      return;  /* and that is it */
    }
    else  /* must skip over 'then' part if condition is false */
      jf = luaK_jump(fs);
  }
  else {  /* regular case (not goto/break) */
    luaK_goiftrue(ls->fs, &v);  /* 处理true的情况。skip over block if condition is false */
    enterblock(fs, &bl, 0);		// 进入一个block
    jf = v.f;					// 缓存一下false情况下的跳转指令链表 
  }
  statlist(ls);  				/* 处理then部分。 'then' part */
  leaveblock(fs);				/* 离开一个 block。 */
  
  if (ls->t.token == TK_ELSE ||
      ls->t.token == TK_ELSEIF)  /* followed by 'else'/'elseif'? */
     /* 
     	如果有 else/elseif，就需要在处理完true情况之后添加一条 JMP 指令，跳转到if-end之后的位置。
     	因为跳转位置目前不能确定，所以先串接到 escapelist 指令链表上。
     */
    luaK_concat(fs, escapelist, luaK_jump(fs));  /*  must jump over it */

  luaK_patchtohere(fs, jf);  // if .. then 这部分处理结束时，把 jf 指令链表添加到 fs->jpc 上。
}


/*
处理 if 语句。

TEST指令：
	成功：永远是跳过下一行的JMP指令，
	失败：执行下一行的 JMP指令（跳入失败情况下的处理逻辑）

JMP跳转，永远都是分支不成立的情况。

解析过程举例：

1  TSET A, B, C // TEST为 true 时， pc++，也就是跳过下一行的JMP，进入第3行
2  JMP 6        // TEST为 false 时，跳转，进入
3  XXX          // 处理true情况
4  XXX
5  JMP 8        // true情况处理结束，跳转到整个if语句后面的语句
6  YYY          // 处理false情况
7  YYY
8  ZZZ          // if 后面的其他语句

解析过程：
1. 在生成第2行的JMP时，跳转地址是无法确定的。
	只有当true情况处理逻辑结束之后（2种情况：遇到  elseif/else，或者遇到end），才能确定。
2. true情况处理结束之后的 JMP 指令在生成时也是无法确定跳转地址的，直到遇到 end 才能确定。
*/
static void ifstat (LexState *ls, int line) {
  /* ifstat -> IF cond THEN block {ELSEIF cond THEN block} [ELSE block] END */
  FuncState *fs = ls->fs;

  /*
	escapelist 指整个if语句的结束位置。
	采用穿起来的方式：escapelist指向第一个没有解析的JMP，然后这个JMP执行的地址是下一个没有解析的JMP，
	这样就形成了链表结构。后面可以一起修改目标地址，通过luaK_patchtohere来实现。
  */
  int escapelist = NO_JUMP;  			/* exit list for finished parts */

  test_then_block(ls, &escapelist);  	/* 首先处理 elseif/else 之前的代码。IF cond THEN block */
  
  while (ls->t.token == TK_ELSEIF)
    test_then_block(ls, &escapelist);  	/* 然后循环处理 elseif 代码。 ELSEIF cond THEN block */
  
  if (testnext(ls, TK_ELSE))			/* 最后处理 else 代码 */
    block(ls);  						/* 'else' part */
  
  check_match(ls, TK_END, TK_IF, line);

  /* 
	当所有的 elseif/else 处理结束后，遇到 end 标记时：
	1. fs->jpc 中就只剩下最后一个elseif/else 分支中产生的那个false情况下的跳转指令了。
	2. escapelist 中保存了所有 if...elseif 中 true情况下处理指令最后的那个 JMP 指令了。
	以上这两种跳转指令都应该跳转到 end 之后的位置，所以，下面这句代码把 escapelist 追加到 fs->jpc 后面。

	当 if 语句后面的代码生成指令的时候，就会自动把 fs->jpc 指令链表中的指令跳转到正在生成的指令位置。
  */
  luaK_patchtohere(fs, escapelist);  /* patch escape list to 'if' end */
}


/*
解析local function 
*/
static void localfunc (LexState *ls) {
  expdesc b;
  FuncState *fs = ls->fs;
  new_localvar(ls, str_checkname(ls));  /* new local variable */
  adjustlocalvars(ls, 1);  /* enter its scope */
  body(ls, &b, 0, ls->linenumber);  /* function created in next register */
  /* debug information will only see the variable after this point! */
  getlocvar(fs, b.u.info)->startpc = fs->pc;
}


/* 解析局部变量 */
static void localstat (LexState *ls) {
  /* stat -> LOCAL NAME {',' NAME} ['=' explist] */
  int nvars = 0;
  int nexps;   // 记录定义了多少个局部变量
  expdesc e;
  do {
    new_localvar(ls, str_checkname(ls));// 检查这是一个 TK_NAME, 然后保存在词法分析器的 locvars[] 数组中
    nvars++;
  } while (testnext(ls, ','));			// 如果定义多个局部变量，使用 , 分割
  
  if (testnext(ls, '='))		
    nexps = explist(ls, &e);	// 变量定义结束，=后面是表达式
  else {					
    e.k = VVOID;				// 变量定义结束
    nexps = 0;
  }
  adjust_assign(ls, nvars, nexps, &e);  // 赋值个数调整
  adjustlocalvars(ls, nvars);
}


static int funcname (LexState *ls, expdesc *v) {
  /* funcname -> NAME {fieldsel} [':' NAME] */
  int ismethod = 0;
  singlevar(ls, v);
  while (ls->t.token == '.')
    fieldsel(ls, v);
  if (ls->t.token == ':') {
    ismethod = 1;
    fieldsel(ls, v);
  }
  return ismethod;
}


static void funcstat (LexState *ls, int line) {
  /* funcstat -> FUNCTION funcname body */
  int ismethod;
  expdesc v, b;
  luaX_next(ls);  /* skip FUNCTION */
  ismethod = funcname(ls, &v);
  body(ls, &b, ismethod, line);
  luaK_storevar(ls->fs, &v, &b);
  luaK_fixline(ls->fs, line);  /* definition "happens" in the first line */
}

/*
普通表达式处理逻辑
*/
static void exprstat (LexState *ls) {
  /* stat -> func | assignment */
  FuncState *fs = ls->fs;
  struct LHS_assign v;		// 处理多个值
  suffixedexp(ls, &v.v);  	// 处理变量名

  // 变量赋值处理
  if (ls->t.token == '=' || ls->t.token == ',') { /* stat -> assignment ? */
    v.prev = NULL;
    assignment(ls, &v, 1);	// 赋值
  }
  else {  /* stat -> func */
    check_condition(ls, v.v.k == VCALL, "syntax error");
    SETARG_C(getinstruction(fs, &v.v), 1);  /* call statement uses no results */
  }
}


static void retstat (LexState *ls) {
  /* stat -> RETURN [explist] [';'] */
  FuncState *fs = ls->fs;
  expdesc e;
  int first, nret;  /* registers with returned values */
  if (block_follow(ls, 1) || ls->t.token == ';')
    first = nret = 0;  /* return no values */
  else {
    nret = explist(ls, &e);  /* optional return values */
    if (hasmultret(e.k)) {
      luaK_setmultret(fs, &e);
      if (e.k == VCALL && nret == 1) {  /* tail call? */
        SET_OPCODE(getinstruction(fs,&e), OP_TAILCALL);
        lua_assert(GETARG_A(getinstruction(fs,&e)) == fs->nactvar);
      }
      first = fs->nactvar;
      nret = LUA_MULTRET;  /* return all values */
    }
    else {
      if (nret == 1)  /* only one single value? */
        first = luaK_exp2anyreg(fs, &e);
      else {
        luaK_exp2nextreg(fs, &e);  /* values must go to the stack */
        first = fs->nactvar;  /* return all active values */
        lua_assert(nret == fs->freereg - first);
      }
    }
  }
  luaK_ret(fs, first, nret);
  testnext(ls, ';');  /* skip optional semicolon */
}

/* 
解析一个句子: 通过switch方法，针对不同的Token值，执行不同的语法块处理。
*/
static void statement (LexState *ls) {
  int line = ls->linenumber;  /* may be needed for error messages */
  enterlevel(ls);

  // 根据不同的 token 类型，进入不同的处理函数
  switch (ls->t.token) {
    case ';': {  /* stat -> ';' (empty statement) */
      luaX_next(ls);  /* 开始获取下一个 token。skip ';' */
      break;
    }
    case TK_IF: {  /* stat -> ifstat */
      ifstat(ls, line);
      break;
    }
    case TK_WHILE: {  /* stat -> whilestat */
      whilestat(ls, line);
      break;
    }
    case TK_DO: {  /* stat -> DO block END */
      luaX_next(ls);  /* skip DO */
      block(ls);
      check_match(ls, TK_END, TK_DO, line);
      break;
    }
    case TK_FOR: {  /* stat -> forstat */
      forstat(ls, line);
      break;
    }
    case TK_REPEAT: {  /* stat -> repeatstat */
      repeatstat(ls, line);
      break;
    }
    case TK_FUNCTION: {  /* stat -> funcstat */
      funcstat(ls, line);
      break;
    }
    case TK_LOCAL: {  /* 越过local关键字。stat -> localstat */
      luaX_next(ls);  /* skip LOCAL */
      if (testnext(ls, TK_FUNCTION))  /* local function? */
        localfunc(ls); // local函数
      else
        localstat(ls); // local变量
      break;
    }
    case TK_DBCOLON: {  /* stat -> label */
      luaX_next(ls);  /* skip double colon */
      labelstat(ls, str_checkname(ls), line);
      break;
    }
    case TK_RETURN: {  /* stat -> retstat */
      luaX_next(ls);  /* skip RETURN */
      retstat(ls);
      break;
    }
    case TK_BREAK:   /* stat -> breakstat */
    case TK_GOTO: {  /* stat -> 'goto' NAME */
      gotostat(ls, luaK_jump(ls->fs));
      break;
    }
    default: {  /* stat -> func | assignment */
      exprstat(ls);
      break;
    }
  }
  lua_assert(ls->fs->f->maxstacksize >= ls->fs->freereg &&
             ls->fs->freereg >= ls->fs->nactvar);
  ls->fs->freereg = ls->fs->nactvar;  /* free registers */
  leavelevel(ls);
}

/* }====================================================================== */


/*
** compiles the main function, which is a regular vararg function with an
** upvalue named LUA_ENV
编译主函数
*/
static void mainfunc (LexState *ls, FuncState *fs) {
  BlockCnt bl;
  expdesc v;
  open_func(ls, fs, &bl);
  fs->f->is_vararg = 1;  			/* main 函数肯定接收可变参数。 main function is always declared vararg */
  init_exp(&v, VLOCAL, 0);  		/* create and... */
  newupvalue(fs, ls->envn, &v);  	/* 环境变量作为每一个函数的 upvalue。 ...set environment upvalue */
  luaX_next(ls);  					/* 读取第一个 token。read first token */
  statlist(ls);  					/* parse main body */
  check(ls, TK_EOS);
  close_func(ls);
}

/*
对文本文件的源码进行编译的开始地址.
语言解析和编译过程的入口函数是luaY_parser，而真正实现状态机的是在mainfunc函数中的statlist。
*/
LClosure *luaY_parser (lua_State *L, ZIO *z, Mbuffer *buff,
                       Dyndata *dyd, const char *name, int firstchar) {
  LexState lexstate;	// 扫描状态机，单词读取、步进等操作
  FuncState funcstate;	// 函数状态机，每输入一个源代码文件或者是一段源代码字符串，都将有一个状态机对应
  LClosure *cl = luaF_newLclosure(L, 1);/* create main closure */
  setclLvalue(L, L->top, cl);  			/* anchor it (to avoid being collected) */
  luaD_inctop(L);
  lexstate.h = luaH_new(L);  			/* create table for scanner */
  sethvalue(L, L->top, lexstate.h);  	/* anchor it */
  luaD_inctop(L);
  funcstate.f = cl->p = luaF_newproto(L);	/* 创建函数原型。 */
  funcstate.f->source = luaS_new(L, name);  /* create and anchor TString */
  lua_assert(iswhite(funcstate.f));  		/* do not need barrier here */
  lexstate.buff = buff;						/* 最终指向的是分析器   SParser 中的变量 */
  lexstate.dyd = dyd;
  dyd->actvar.n = dyd->gt.n = dyd->label.n = 0;

  // 初始化词法分析器
  luaX_setinput(L, &lexstate, z, funcstate.f->source, firstchar);

  // 进入主函数分析
  mainfunc(&lexstate, &funcstate);
  
  lua_assert(!funcstate.prev && funcstate.nups == 1 && !lexstate.fs);
  /* all scopes should be correctly finished */
  lua_assert(dyd->actvar.n == 0 && dyd->gt.n == 0 && dyd->label.n == 0);
  L->top--;  /* remove scanner's table */
  return cl; /* 返回的闭包，将保存在lua_State当前的栈顶 closure is on the stack, too */
}

