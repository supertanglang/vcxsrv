/*
 * Mesa 3-D graphics library
 * Version:  7.1
 *
 * Copyright (C) 1999-2008  Brian Paul   All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * BRIAN PAUL BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN
 * AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */


/*
 * This file manages the OpenGL API dispatch layer.
 * The dispatch table (struct _glapi_table) is basically just a list
 * of function pointers.
 * There are functions to set/get the current dispatch table for the
 * current thread and to manage registration/dispatch of dynamically
 * added extension functions.
 *
 * It's intended that this file and the other glapi*.[ch] files are
 * flexible enough to be reused in several places:  XFree86, DRI-
 * based libGL.so, and perhaps the SGI SI.
 *
 * NOTE: There are no dependencies on Mesa in this code.
 *
 * Versions (API changes):
 *   2000/02/23  - original version for Mesa 3.3 and XFree86 4.0
 *   2001/01/16  - added dispatch override feature for Mesa 3.5
 *   2002/06/28  - added _glapi_set_warning_func(), Mesa 4.1.
 *   2002/10/01  - _glapi_get_proc_address() will now generate new entrypoints
 *                 itself (using offset ~0).  _glapi_add_entrypoint() can be
 *                 called afterward and it'll fill in the correct dispatch
 *                 offset.  This allows DRI libGL to avoid probing for DRI
 *                 drivers!  No changes to the public glapi interface.
 */



#ifdef HAVE_DIX_CONFIG_H

#include <dix-config.h>
#include <X11/Xfuncproto.h>
#ifdef _MSC_VER
#define PUBLIC _declspec(dllexport)
#else
#define PUBLIC _X_EXPORT
#endif

#else

#include "glheader.h"

#endif

#include <stdlib.h>
#include <string.h>
#ifdef DEBUG
#include <assert.h>
#endif
#include <unistd.h>
#define str_dup strdup

#include "glapi.h"
#include "glapioffsets.h"
#include "GL/gl.h"
#include "GL/glext.h"
#include "glapitable.h"

/***** BEGIN NO-OP DISPATCH *****/

static GLboolean WarnFlag = GL_FALSE;
static _glapi_proc warning_func;

#if defined(PTHREADS) || defined(GLX_USE_TLS)
static void init_glapi_relocs(void);
#endif

static _glapi_proc generate_entrypoint(GLuint functionOffset);
static void fill_in_entrypoint_offset(_glapi_proc entrypoint, GLuint offset);

/*
 * Enable/disable printing of warning messages.
 */
PUBLIC void
_glapi_noop_enable_warnings(GLboolean enable)
{
   WarnFlag = enable;
}

static GLboolean
warn(void)
{
   if ((WarnFlag || getenv("MESA_DEBUG") || getenv("LIBGL_DEBUG"))
       && warning_func) {
      return GL_TRUE;
   }
   else {
      return GL_FALSE;
   }
}

#if defined(__GNUC__) && (__GNUC__ > 2)
#define possibly_unused __attribute((unused))
#else
#define possibly_unused
#endif

#define KEYWORD1 static
#define KEYWORD1_ALT static
#define KEYWORD2 GLAPIENTRY possibly_unused
#define NAME(func)  NoOp##func

#define F NULL

#ifdef _DEBUG
#define DISPATCH(func, args, msg) ErrorF msg 

#define RETURN_DISPATCH(func, args, msg) ErrorF msg ; return 0
#else
#define DISPATCH(func, args, msg)

#define RETURN_DISPATCH(func, args, msg) return 0
#endif

#define DISPATCH_TABLE_NAME __glapi_noop_table
#define UNUSED_TABLE_NAME __unused_noop_functions

#define TABLE_ENTRY(name) (_glapi_proc) NoOp##name

static GLint NoOpUnused(void)
{
   #ifdef _DEBUG
   ErrorF("NoOpUnused\n");
   #endif
   return 0;
}

#include "glapitemp.h"

/***** END NO-OP DISPATCH *****/



/**
 * \name Current dispatch and current context control variables
 *
 * Depending on whether or not multithreading is support, and the type of
 * support available, several variables are used to store the current context
 * pointer and the current dispatch table pointer.  In the non-threaded case,
 * the variables \c _glapi_Dispatch and \c _glapi_Context are used for this
 * purpose.
 *
 * In the "normal" threaded case, the variables \c _glapi_Dispatch and
 * \c _glapi_Context will be \c NULL if an application is detected as being
 * multithreaded.  Single-threaded applications will use \c _glapi_Dispatch
 * and \c _glapi_Context just like the case without any threading support.
 * When \c _glapi_Dispatch and \c _glapi_Context are \c NULL, the thread state
 * data \c _gl_DispatchTSD and \c ContextTSD are used.  Drivers and the
 * static dispatch functions access these variables via \c _glapi_get_dispatch
 * and \c _glapi_get_context.
 *
 * There is a race condition in setting \c _glapi_Dispatch to \c NULL.  It is
 * possible for the original thread to be setting it at the same instant a new
 * thread, perhaps running on a different processor, is clearing it.  Because
 * of that, \c ThreadSafe, which can only ever be changed to \c GL_TRUE, is
 * used to determine whether or not the application is multithreaded.
 * 
 * In the TLS case, the variables \c _glapi_Dispatch and \c _glapi_Context are
 * hardcoded to \c NULL.  Instead the TLS variables \c _glapi_tls_Dispatch and
 * \c _glapi_tls_Context are used.  Having \c _glapi_Dispatch and
 * \c _glapi_Context be hardcoded to \c NULL maintains binary compatability
 * between TLS enabled loaders and non-TLS DRI drivers.
 */
/*@{*/
#if defined(GLX_USE_TLS)

PUBLIC __thread struct _glapi_table * _glapi_tls_Dispatch
    __attribute__((tls_model("initial-exec")))
    = (struct _glapi_table *) __glapi_noop_table;

PUBLIC __thread void * _glapi_tls_Context
    __attribute__((tls_model("initial-exec")));

PUBLIC const struct _glapi_table *_glapi_Dispatch = NULL;

PUBLIC const void *_glapi_Context = NULL;

#else

#if defined(THREADS)

static GLboolean ThreadSafe = GL_FALSE;  /**< In thread-safe mode? */

_glthread_TSD _gl_DispatchTSD;           /**< Per-thread dispatch pointer */

static _glthread_TSD ContextTSD;         /**< Per-thread context pointer */

#endif /* defined(THREADS) */

PUBLIC struct _glapi_table *_glapi_Dispatch = (struct _glapi_table *) __glapi_noop_table;

PUBLIC void *_glapi_Context = NULL;

#endif /* defined(GLX_USE_TLS) */
/*@}*/



#if defined(THREADS) && !defined(GLX_USE_TLS)

void
_glapi_init_multithread(void)
{
   _glthread_InitTSD(&_gl_DispatchTSD);
   _glthread_InitTSD(&ContextTSD);
}

void
_glapi_destroy_multithread(void)
{
#ifdef WIN32_THREADS
   _glthread_DestroyTSD(&_gl_DispatchTSD);
   _glthread_DestroyTSD(&ContextTSD);
#endif
}

/**
 * Mutex for multithread check.
 */
#ifdef WIN32_THREADS
/* _glthread_DECLARE_STATIC_MUTEX is broken on windows.  There will be race! */
#define CHECK_MULTITHREAD_LOCK()
#define CHECK_MULTITHREAD_UNLOCK()
#else
_glthread_DECLARE_STATIC_MUTEX(ThreadCheckMutex);
#define CHECK_MULTITHREAD_LOCK() _glthread_LOCK_MUTEX(ThreadCheckMutex)
#define CHECK_MULTITHREAD_UNLOCK() _glthread_UNLOCK_MUTEX(ThreadCheckMutex)
#endif
/**
 * We should call this periodically from a function such as glXMakeCurrent
 * in order to test if multiple threads are being used.
 */
PUBLIC void
_glapi_check_multithread(void)
{
   static unsigned long knownID;
   static GLboolean firstCall = GL_TRUE;

   if (ThreadSafe)
      return;

   CHECK_MULTITHREAD_LOCK();
   if (firstCall) {
      _glapi_init_multithread();

      knownID = _glthread_GetID();
      firstCall = GL_FALSE;
   }
   else if (knownID != _glthread_GetID()) {
      ThreadSafe = GL_TRUE;
      _glapi_set_dispatch(NULL);
      _glapi_set_context(NULL);
   }
   CHECK_MULTITHREAD_UNLOCK();
}
#else

void
_glapi_init_multithread(void) { }

void
_glapi_destroy_multithread(void) { }

PUBLIC void
_glapi_check_multithread(void) { }

#endif



/**
 * Set the current context pointer for this thread.
 * The context pointer is an opaque type which should be cast to
 * void from the real context pointer type.
 */
void
_glapi_set_context(void *context)
{
#if defined(GLX_USE_TLS)
   _glapi_tls_Context = context;
#elif defined(THREADS)
   _glthread_SetTSD(&ContextTSD, context);
   _glapi_Context = (ThreadSafe) ? NULL : context;
#else
   _glapi_Context = context;
#endif
}



/**
 * Get the current context pointer for this thread.
 * The context pointer is an opaque type which should be cast from
 * void to the real context pointer type.
 */
void *
_glapi_get_context(void)
{
#if defined(GLX_USE_TLS)
   return _glapi_tls_Context;
#elif defined(THREADS)
   return (ThreadSafe) ? _glthread_GetTSD(&ContextTSD) : _glapi_Context;
#else
   return _glapi_Context;
#endif
}



/**
 * Set the global or per-thread dispatch table pointer.
 * If the dispatch parameter is NULL we'll plug in the no-op dispatch
 * table (__glapi_noop_table).
 */
void
_glapi_set_dispatch(struct _glapi_table *dispatch)
{
#if defined(PTHREADS) || defined(GLX_USE_TLS)
   static pthread_once_t once_control = PTHREAD_ONCE_INIT;
   pthread_once( & once_control, init_glapi_relocs );
#endif

   if (dispatch == NULL) {
      /* use the no-op functions */
      dispatch = (struct _glapi_table *) __glapi_noop_table;
   }
#ifdef DEBUG
   else {
      _glapi_check_table_not_null(dispatch);
      _glapi_check_table(dispatch);
   }
#endif

#if defined(GLX_USE_TLS)
   _glapi_tls_Dispatch = dispatch;
#elif defined(THREADS)
   _glthread_SetTSD(&_gl_DispatchTSD, (void *) dispatch);
   _glapi_Dispatch = (ThreadSafe) ? NULL : dispatch;
#else
   _glapi_Dispatch = dispatch;
#endif
}



/**
 * Return pointer to current dispatch table for calling thread.
 */
struct _glapi_table *
_glapi_get_dispatch(void)
{
#if defined(GLX_USE_TLS)
   return _glapi_tls_Dispatch;
#elif defined(THREADS)
   return (ThreadSafe)
     ? (struct _glapi_table *) _glthread_GetTSD(&_gl_DispatchTSD)
     : _glapi_Dispatch;
#else
   return _glapi_Dispatch;
#endif
}



/***
 *** The rest of this file is pretty much concerned with GetProcAddress
 *** functionality.
 ***/

#if defined(USE_X64_64_ASM) && defined(GLX_USE_TLS)
# define DISPATCH_FUNCTION_SIZE  16
#elif defined(USE_X86_ASM)
# if defined(THREADS) && !defined(GLX_USE_TLS)
#  define DISPATCH_FUNCTION_SIZE  32
# else
#  define DISPATCH_FUNCTION_SIZE  16
# endif
#endif

#if !defined(DISPATCH_FUNCTION_SIZE) && !defined(XFree86Server) && !defined(XGLServer)
# define NEED_FUNCTION_POINTER
#endif

/* The code in this file is auto-generated with Python */
#include "glprocs.h"


/**
 * Search the table of static entrypoint functions for the named function
 * and return the corresponding glprocs_table_t entry.
 */
static const glprocs_table_t *
find_entry( const char * n )
{
   GLuint i;
   for (i = 0; static_functions[i].Name_offset >= 0; i++) {
      const char *testName = gl_string_table + static_functions[i].Name_offset;
      if (strcmp(testName, n) == 0) {
	 return &static_functions[i];
      }
   }
   return NULL;
}


/**
 * Return dispatch table offset of the named static (built-in) function.
 * Return -1 if function not found.
 */
static GLint
get_static_proc_offset(const char *funcName)
{
   const glprocs_table_t * const f = find_entry( funcName );
   if (f) {
      return f->Offset;
   }
   return -1;
}


/**********************************************************************
 * Extension function management.
 */

/*
 * Number of extension functions which we can dynamically add at runtime.
 */
#define MAX_EXTENSION_FUNCS 300


/*
 * The dispatch table size (number of entries) is the size of the
 * _glapi_table struct plus the number of dynamic entries we can add.
 * The extra slots can be filled in by DRI drivers that register new extension
 * functions.
 */
#define DISPATCH_TABLE_SIZE (sizeof(struct _glapi_table) / sizeof(void *) + MAX_EXTENSION_FUNCS)


/**
 * Track information about a function added to the GL API.
 */
struct _glapi_function {
   /**
    * Name of the function.
    */
   const char * name;


   /**
    * Text string that describes the types of the parameters passed to the
    * named function.   Parameter types are converted to characters using the
    * following rules:
    *   - 'i' for \c GLint, \c GLuint, and \c GLenum
    *   - 'p' for any pointer type
    *   - 'f' for \c GLfloat and \c GLclampf
    *   - 'd' for \c GLdouble and \c GLclampd
    */
   const char * parameter_signature;


   /**
    * Offset in the dispatch table where the pointer to the real function is
    * located.  If the driver has not requested that the named function be
    * added to the dispatch table, this will have the value ~0.
    */
   unsigned dispatch_offset;


   /**
    * Pointer to the dispatch stub for the named function.
    * 
    * \todo
    * The semantic of this field should be changed slightly.  Currently, it
    * is always expected to be non-\c NULL.  However, it would be better to
    * only allocate the entry-point stub when the application requests the
    * function via \c glXGetProcAddress.  This would save memory for all the
    * functions that the driver exports but that the application never wants
    * to call.
    */
   _glapi_proc dispatch_stub;
};


static struct _glapi_function ExtEntryTable[MAX_EXTENSION_FUNCS];
static GLuint NumExtEntryPoints = 0;

#ifdef USE_SPARC_ASM
extern void __glapi_sparc_icache_flush(unsigned int *);
#endif

/**
 * Generate a dispatch function (entrypoint) which jumps through
 * the given slot number (offset) in the current dispatch table.
 * We need assembly language in order to accomplish this.
 */
static _glapi_proc
generate_entrypoint(GLuint functionOffset)
{
#if defined(USE_X86_ASM)
   /* 32 is chosen as something of a magic offset.  For x86, the dispatch
    * at offset 32 is the first one where the offset in the
    * "jmp OFFSET*4(%eax)" can't be encoded in a single byte.
    */
   const GLubyte * const template_func = gl_dispatch_functions_start 
     + (DISPATCH_FUNCTION_SIZE * 32);
   GLubyte * const code = (GLubyte *) malloc(DISPATCH_FUNCTION_SIZE);


   if ( code != NULL ) {
      (void) memcpy(code, template_func, DISPATCH_FUNCTION_SIZE);
      fill_in_entrypoint_offset( (_glapi_proc) code, functionOffset );
   }

   return (_glapi_proc) code;
#elif defined(USE_SPARC_ASM)

#ifdef __arch64__
   static const unsigned int insn_template[] = {
	   0x05000000,	/* sethi	%uhi(_glapi_Dispatch), %g2	*/
	   0x03000000,	/* sethi	%hi(_glapi_Dispatch), %g1	*/
	   0x8410a000,	/* or		%g2, %ulo(_glapi_Dispatch), %g2	*/
	   0x82106000,	/* or		%g1, %lo(_glapi_Dispatch), %g1	*/
	   0x8528b020,	/* sllx		%g2, 32, %g2			*/
	   0xc2584002,	/* ldx		[%g1 + %g2], %g1		*/
	   0x05000000,	/* sethi	%hi(8 * glapioffset), %g2	*/
	   0x8410a000,	/* or		%g2, %lo(8 * glapioffset), %g2	*/
	   0xc6584002,	/* ldx		[%g1 + %g2], %g3		*/
	   0x81c0c000,	/* jmpl		%g3, %g0			*/
	   0x01000000	/*  nop						*/
   };
#else
   static const unsigned int insn_template[] = {
	   0x03000000,	/* sethi	%hi(_glapi_Dispatch), %g1	  */
	   0xc2006000,	/* ld		[%g1 + %lo(_glapi_Dispatch)], %g1 */
	   0xc6006000,	/* ld		[%g1 + %lo(4*glapioffset)], %g3	  */
	   0x81c0c000,	/* jmpl		%g3, %g0			  */
	   0x01000000	/*  nop						  */
   };
#endif /* __arch64__ */
   unsigned int *code = (unsigned int *) malloc(sizeof(insn_template));
   unsigned long glapi_addr = (unsigned long) &_glapi_Dispatch;
   if (code) {
      memcpy(code, insn_template, sizeof(insn_template));

#ifdef __arch64__
      code[0] |= (glapi_addr >> (32 + 10));
      code[1] |= ((glapi_addr & 0xffffffff) >> 10);
      __glapi_sparc_icache_flush(&code[0]);
      code[2] |= ((glapi_addr >> 32) & ((1 << 10) - 1));
      code[3] |= (glapi_addr & ((1 << 10) - 1));
      __glapi_sparc_icache_flush(&code[2]);
      code[6] |= ((functionOffset * 8) >> 10);
      code[7] |= ((functionOffset * 8) & ((1 << 10) - 1));
      __glapi_sparc_icache_flush(&code[6]);
#else
      code[0] |= (glapi_addr >> 10);
      code[1] |= (glapi_addr & ((1 << 10) - 1));
      __glapi_sparc_icache_flush(&code[0]);
      code[2] |= (functionOffset * 4);
      __glapi_sparc_icache_flush(&code[2]);
#endif /* __arch64__ */
   }
   return (_glapi_proc) code;
#else
   (void) functionOffset;
   return NULL;
#endif /* USE_*_ASM */
}


/**
 * This function inserts a new dispatch offset into the assembly language
 * stub that was generated with the preceeding function.
 */
static void
fill_in_entrypoint_offset(_glapi_proc entrypoint, GLuint offset)
{
#if defined(USE_X86_ASM)
   GLubyte * const code = (GLubyte *) entrypoint;

#if DISPATCH_FUNCTION_SIZE == 32
   *((unsigned int *)(code + 11)) = 4 * offset;
   *((unsigned int *)(code + 22)) = 4 * offset;
#elif DISPATCH_FUNCTION_SIZE == 16 && defined( GLX_USE_TLS )
   *((unsigned int *)(code +  8)) = 4 * offset;
#elif DISPATCH_FUNCTION_SIZE == 16
   *((unsigned int *)(code +  7)) = 4 * offset;
#else
# error Invalid DISPATCH_FUNCTION_SIZE!
#endif

#elif defined(USE_SPARC_ASM)

   /* XXX this hasn't been tested! */
   unsigned int *code = (unsigned int *) entrypoint;
#ifdef __arch64__
   code[6] = 0x05000000;  /* sethi	%hi(8 * glapioffset), %g2	*/
   code[7] = 0x8410a000;  /* or		%g2, %lo(8 * glapioffset), %g2	*/
   code[6] |= ((offset * 8) >> 10);
   code[7] |= ((offset * 8) & ((1 << 10) - 1));
   __glapi_sparc_icache_flush(&code[6]);
#else /* __arch64__ */
   code[2] = 0xc6006000;  /* ld		[%g1 + %lo(4*glapioffset)], %g3	  */
   code[2] |= (offset * 4);
   __glapi_sparc_icache_flush(&code[2]);
#endif /* __arch64__ */

#else

   /* an unimplemented architecture */
   (void) entrypoint;
   (void) offset;

#endif /* USE_*_ASM */
}


/**
 * Generate new entrypoint
 *
 * Use a temporary dispatch offset of ~0 (i.e. -1).  Later, when the driver
 * calls \c _glapi_add_dispatch we'll put in the proper offset.  If that
 * never happens, and the user calls this function, he'll segfault.  That's
 * what you get when you try calling a GL function that doesn't really exist.
 * 
 * \param funcName  Name of the function to create an entry-point for.
 * 
 * \sa _glapi_add_entrypoint
 */

static struct _glapi_function *
add_function_name( const char * funcName )
{
   struct _glapi_function * entry = NULL;
   
   if (NumExtEntryPoints < MAX_EXTENSION_FUNCS) {
      _glapi_proc entrypoint = generate_entrypoint(~0);
      if (entrypoint != NULL) {
	 entry = & ExtEntryTable[NumExtEntryPoints];

	 ExtEntryTable[NumExtEntryPoints].name = str_dup(funcName);
	 ExtEntryTable[NumExtEntryPoints].parameter_signature = NULL;
	 ExtEntryTable[NumExtEntryPoints].dispatch_offset = ~0;
	 ExtEntryTable[NumExtEntryPoints].dispatch_stub = entrypoint;
	 NumExtEntryPoints++;
      }
   }

   return entry;
}


/**
 * Fill-in the dispatch stub for the named function.
 * 
 * This function is intended to be called by a hardware driver.  When called,
 * a dispatch stub may be created created for the function.  A pointer to this
 * dispatch function will be returned by glXGetProcAddress.
 *
 * \param function_names       Array of pointers to function names that should
 *                             share a common dispatch offset.
 * \param parameter_signature  String representing the types of the parameters
 *                             passed to the named function.  Parameter types
 *                             are converted to characters using the following
 *                             rules:
 *                               - 'i' for \c GLint, \c GLuint, and \c GLenum
 *                               - 'p' for any pointer type
 *                               - 'f' for \c GLfloat and \c GLclampf
 *                               - 'd' for \c GLdouble and \c GLclampd
 *
 * \returns
 * The offset in the dispatch table of the named function.  A pointer to the
 * driver's implementation of the named function should be stored at
 * \c dispatch_table[\c offset].
 *
 * \sa glXGetProcAddress
 *
 * \warning
 * This function can only handle up to 8 names at a time.  As far as I know,
 * the maximum number of names ever associated with an existing GL function is
 * 4 (\c glPointParameterfSGIS, \c glPointParameterfEXT,
 * \c glPointParameterfARB, and \c glPointParameterf), so this should not be
 * too painful of a limitation.
 *
 * \todo
 * Determine whether or not \c parameter_signature should be allowed to be
 * \c NULL.  It doesn't seem like much of a hardship for drivers to have to
 * pass in an empty string.
 *
 * \todo
 * Determine if code should be added to reject function names that start with
 * 'glX'.
 * 
 * \bug
 * Add code to compare \c parameter_signature with the parameter signature of
 * a static function.  In order to do that, we need to find a way to \b get
 * the parameter signature of a static function.
 */

PUBLIC int
_glapi_add_dispatch( const char * const * function_names,
		     const char * parameter_signature )
{
   static int next_dynamic_offset = _gloffset_FIRST_DYNAMIC;
   const char * const real_sig = (parameter_signature != NULL)
     ? parameter_signature : "";
   struct _glapi_function * entry[8];
   GLboolean is_static[8];
   unsigned i;
   unsigned j;
   int offset = ~0;
   int new_offset;


   (void) memset( is_static, 0, sizeof( is_static ) );
   (void) memset( entry, 0, sizeof( entry ) );

   for ( i = 0 ; function_names[i] != NULL ; i++ ) {
      /* Do some trivial validation on the name of the function.
       */

      if (!function_names[i] || function_names[i][0] != 'g' || function_names[i][1] != 'l')
	return GL_FALSE;
   
      /* Determine if the named function already exists.  If the function does
       * exist, it must have the same parameter signature as the function
       * being added.
       */

      new_offset = get_static_proc_offset(function_names[i]);
      if (new_offset >= 0) {
	 /* FIXME: Make sure the parameter signatures match!  How do we get
	  * FIXME: the parameter signature for static functions?
	  */

	 if ( (offset != ~0) && (new_offset != offset) ) {
	    return -1;
	 }

	 is_static[i] = GL_TRUE;
	 offset = new_offset;
      }
   
   
      for ( j = 0 ; j < NumExtEntryPoints ; j++ ) {
	 if (strcmp(ExtEntryTable[j].name, function_names[i]) == 0) {
	    /* The offset may be ~0 if the function name was added by
	     * glXGetProcAddress but never filled in by the driver.
	     */

	    if (ExtEntryTable[j].dispatch_offset != ~0) {
	       if (strcmp(real_sig, ExtEntryTable[j].parameter_signature) 
		   != 0) {
		  return -1;
	       }

	       if ( (offset != ~0) && (ExtEntryTable[j].dispatch_offset != offset) ) {
		  return -1;
	       }

	       offset = ExtEntryTable[j].dispatch_offset;
	    }
	    
	    entry[i] = & ExtEntryTable[j];
	    break;
	 }
      }
   }

   if (offset == ~0) {
      offset = next_dynamic_offset;
      next_dynamic_offset++;
   }

   for ( i = 0 ; function_names[i] != NULL ; i++ ) {
      if (! is_static[i] ) {
	 if (entry[i] == NULL) {
	    entry[i] = add_function_name( function_names[i] );
	    if (entry[i] == NULL) {
	       /* FIXME: Possible memory leak here.
		*/
	       return -1;
	    }
	 }

	 entry[i]->parameter_signature = str_dup(real_sig);
	 fill_in_entrypoint_offset(entry[i]->dispatch_stub, offset);
	 entry[i]->dispatch_offset = offset;
      }
   }
   
   return offset;
}

/**
 * Return size of dispatch table struct as number of functions (or
 * slots).
 */
GLuint
_glapi_get_dispatch_table_size(void)
{
   return DISPATCH_TABLE_SIZE;
}


/**
 * Make sure there are no NULL pointers in the given dispatch table.
 * Intended for debugging purposes.
 */
void
_glapi_check_table_not_null(const struct _glapi_table *table)
{
#if 0 /* enable this for extra DEBUG */
   const GLuint entries = _glapi_get_dispatch_table_size();
   const void **tab = (const void **) table;
   GLuint i;
   for (i = 1; i < entries; i++) {
      assert(tab[i]);
   }
#else
   (void) table;
#endif
}
