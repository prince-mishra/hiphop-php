/*
   +----------------------------------------------------------------------+
   | HipHop for PHP                                                       |
   +----------------------------------------------------------------------+
   | Copyright (c) 2010 Facebook, Inc. (http://www.facebook.com)          |
   +----------------------------------------------------------------------+
   | This source file is subject to version 3.01 of the PHP license,      |
   | that is bundled with this package in the file LICENSE, and is        |
   | available through the world-wide-web at the following url:           |
   | http://www.php.net/license/3_01.txt                                  |
   | If you did not receive a copy of the PHP license and are unable to   |
   | obtain it through the world-wide-web, please send a note to          |
   | license@php.net so we can mail you a copy immediately.               |
   +----------------------------------------------------------------------+
*/
// @generated by HipHop Compiler

#ifndef __GENERATED_cls_LengthException_h78d45e3c__
#define __GENERATED_cls_LengthException_h78d45e3c__

#include <cls/LogicException.h>

namespace HPHP {
///////////////////////////////////////////////////////////////////////////////

/* SRC: classes/exception.php line 203 */
class c_LengthException : public c_LogicException {
  public:

  // Properties

  // Class Map
  virtual bool o_instanceof(CStrRef s) const;
  DECLARE_CLASS_COMMON(LengthException, LengthException)
  DECLARE_INVOKE_EX(LengthException, LengthException, LogicException)

  // DECLARE_STATIC_PROP_OPS
  public:
  #define OMIT_JUMP_TABLE_CLASS_STATIC_GETINIT_LengthException 1
  #define OMIT_JUMP_TABLE_CLASS_STATIC_GET_LengthException 1
  #define OMIT_JUMP_TABLE_CLASS_STATIC_LVAL_LengthException 1
  #define OMIT_JUMP_TABLE_CLASS_CONSTANT_LengthException 1

  // DECLARE_INSTANCE_PROP_OPS
  public:
  #define OMIT_JUMP_TABLE_CLASS_GETARRAY_LengthException 1
  #define OMIT_JUMP_TABLE_CLASS_SETARRAY_LengthException 1
  #define OMIT_JUMP_TABLE_CLASS_realProp_LengthException 1
  #define OMIT_JUMP_TABLE_CLASS_realProp_PRIVATE_LengthException 1

  // DECLARE_INSTANCE_PUBLIC_PROP_OPS
  public:
  #define OMIT_JUMP_TABLE_CLASS_realProp_PUBLIC_LengthException 1

  // DECLARE_COMMON_INVOKE
  static bool os_get_call_info(MethodCallPackage &mcp, int64 hash = -1);
  #define OMIT_JUMP_TABLE_CLASS_STATIC_INVOKE_LengthException 1
  virtual bool o_get_call_info(MethodCallPackage &mcp, int64 hash = -1);

  public:
  DECLARE_INVOKES_FROM_EVAL
  void init();
};
extern struct ObjectStaticCallbacks cw_LengthException;

///////////////////////////////////////////////////////////////////////////////
}

#endif // __GENERATED_cls_LengthException_h78d45e3c__
