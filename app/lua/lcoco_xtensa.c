/*
** Copyright (C) 2004-2009 Mike Pall. All rights reserved.
**
** Permission is hereby granted, free of charge, to any person obtaining
** a copy of this software and associated documentation files (the
** "Software"), to deal in the Software without restriction, including
** without limitation the rights to use, copy, modify, merge, publish,
** distribute, sublicense, and/or sell copies of the Software, and to
** permit persons to whom the Software is furnished to do so, subject to
** the following conditions:
**
** The above copyright notice and this permission notice shall be
** included in all copies or substantial portions of the Software.
**
** THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
** EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
** MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
** IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
** CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
** TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
** SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
**
** [ MIT license: http://www.opensource.org/licenses/mit-license.php ]
*/

/* Coco -- True C coroutines for Lua. http://luajit.org/coco.html */
#ifndef COCO_DISABLE

#define lcoco_c
#define LUA_CORE

#include "lua.h"

#include "lobject.h"
#include "lstate.h"
#include "ldo.h"
#include "lvm.h"
#include "lgc.h"

#define STACK_REG(coco, p, sz)
#define STACK_DEREG(id)
#define STACK_VGID

/* Try _setjmp/_longjmp with a patched jump buffer. */
#include <setjmp.h>

// buf[0] is regsiter a0(return addr), buf[1] is regsiter a1(stack ptr)
// see newlib-2.0.0/newlib/libc/machine/xtensa/setjmp.S
#define COCO_PATCHCTX(coco, buf, func, stack, a0) \
  buf[0] = (int)(func); \
  buf[1] = (int)(stack); \
  stack[0] = (size_t)(a0);

#ifdef COCO_PATCHCTX
#define COCO_CTX		jmp_buf
#define COCO_MAKECTX(coco, buf, func, stack, a0) \
  setjmp(buf); COCO_PATCHCTX(coco, buf, func, stack, a0)
#define COCO_SWITCH(from, to)	if (!setjmp(from)) longjmp(to, 1);
#endif

#ifndef COCO_STACKADJUST
#define COCO_STACKADJUST	1
#endif

#define COCO_FILL(coco, NL, mainfunc) \
{ /* Include the return address to get proper stack alignment. */ \
  size_t *stackptr = &((size_t *)coco)[-COCO_STACKADJUST]; \
  COCO_MAKECTX(coco, coco->ctx, mainfunc, stackptr, NL) \
}

/* Common code for inline asm/setjmp/ucontext to allocate/free the stack. */

struct coco_State {
#ifdef COCO_STATE_HEAD
  COCO_STATE_HEAD
#endif
  COCO_CTX ctx;			/* Own context. */
  COCO_CTX back;		/* Context to switch back to. */
  void *allocptr;		/* Pointer to allocated memory. */
  int allocsize;		/* Size of allocated memory. */
  int nargs;			/* Number of arguments to pass. */
  STACK_VGID			/* Optional valgrind stack id. See above. */
};

typedef void (*coco_MainFunc)(void);

/* Put the Coco state at the end and align it downwards. */
#define ALIGNED_END(p, s, t) \
  ((t *)(((char *)0) + ((((char *)(p)-(char *)0)+(s)-sizeof(t)) & -16)))

#define COCO_NEW(OL, NL, cstacksize, mainfunc) \
{ \
  void *ptr = luaM_malloc(OL, cstacksize); \
  coco_State *coco = ALIGNED_END(ptr, cstacksize, coco_State); \
  STACK_REG(coco, ptr, cstacksize) \
  coco->allocptr = ptr; \
  coco->allocsize = cstacksize; \
  COCO_FILL(coco, NL, mainfunc) \
  L2COCO(NL) = coco; \
}

#define COCO_FREE(L) \
  STACK_DEREG(L2COCO(L)) \
  luaM_freemem(L, L2COCO(L)->allocptr, L2COCO(L)->allocsize); \
  L2COCO(L) = NULL;

#define COCO_JUMPIN(coco)	COCO_SWITCH(coco->back, coco->ctx)
#define COCO_JUMPOUT(coco)	COCO_SWITCH(coco->ctx, coco->back)

/* ------------------------------------------------------------------------ */

#ifndef COCO_MIN_CSTACKSIZE
#define COCO_MIN_CSTACKSIZE		(2048)
#endif

/* Don't use multiples of 64K to avoid D-cache aliasing conflicts. */
#ifndef COCO_DEFAULT_CSTACKSIZE
#define COCO_DEFAULT_CSTACKSIZE		(8192)
#endif

static int defaultcstacksize = COCO_DEFAULT_CSTACKSIZE;

/* Start the Lua or C function. */
static void coco_start(lua_State *L, void *ud)
{
  if (luaD_precall(L, (StkId)ud, LUA_MULTRET) == PCRLUA)
    luaV_execute(L, L->ci - L->base_ci);
}

// _a to _f for register file a2 to a7, thus L is where a1(stack ptr) point to
#define COCO_MAIN_PARAM		int _a, int _b, int _c, int _d, int _e, int _f, lua_State *L

#ifndef COCO_MAIN_DECL
#define COCO_MAIN_DECL
#endif

/* Toplevel function for the new coroutine stack. Never exits. */
static void COCO_MAIN_DECL coco_main(COCO_MAIN_PARAM)
{
#ifdef COCO_MAIN_GETL
  COCO_MAIN_GETL
#endif
  coco_State *coco = L2COCO(L);
  for (;;) {
    L->status = luaD_rawrunprotected(L, coco_start, L->top - (coco->nargs+1));
    if (L->status != 0) luaD_seterrorobj(L, L->status, L->top);
    COCO_JUMPOUT(coco)
  }
}

/* Add a C stack to a coroutine. */
lua_State *lua_newcthread(lua_State *OL, int cstacksize)
{
  lua_State *NL = lua_newthread(OL);

  if (cstacksize < 0)
    return NL;
  if (cstacksize == 0)
    cstacksize = defaultcstacksize;
  else if (cstacksize < COCO_MIN_CSTACKSIZE)
    cstacksize = COCO_MIN_CSTACKSIZE;
  cstacksize &= -16;

  COCO_NEW(OL, NL, cstacksize, ((coco_MainFunc)(coco_main)))

  return NL;
}

/* Free the C stack of a coroutine. Called from lstate.c. */
void luaCOCO_free(lua_State *L)
{
  COCO_FREE(L)
}

/* Resume a coroutine with a C stack. Called from ldo.c. */
int luaCOCO_resume(lua_State *L, int nargs)
{
  coco_State *coco = L2COCO(L);
  coco->nargs = nargs;
  COCO_JUMPIN(coco)
#ifndef COCO_DISABLE_EARLY_FREE
  if (L->status != LUA_YIELD) {
    COCO_FREE(L)
  }
#endif
  return L->status;
}

/* Yield from a coroutine with a C stack. Called from ldo.c. */
int luaCOCO_yield(lua_State *L)
{
  coco_State *coco = L2COCO(L);
  L->status = LUA_YIELD;
  COCO_JUMPOUT(coco)
  L->status = 0;
  {
    StkId base = L->top - coco->nargs;
    StkId rbase = L->base;
    if (rbase < base) {  /* Need to move args down? */
      while (base < L->top)
	setobjs2s(L, rbase++, base++);
      L->top = rbase;
    }
  }
  L->base = L->ci->base;  /* Restore invariant. */
  return coco->nargs;
}

/* Get/set the default C stack size. */
int luaCOCO_cstacksize(int cstacksize)
{
  int oldsz = defaultcstacksize;
  if (cstacksize >= 0) {
    if (cstacksize == 0)
      cstacksize = COCO_DEFAULT_CSTACKSIZE;
    else if (cstacksize < COCO_MIN_CSTACKSIZE)
      cstacksize = COCO_MIN_CSTACKSIZE;
    defaultcstacksize = cstacksize;
  }
  return oldsz;
}

#endif
