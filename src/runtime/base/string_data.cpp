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

#include <runtime/base/string_data.h>
#include <runtime/base/shared/shared_variant.h>
#include <runtime/base/zend/zend_functions.h>
#include <runtime/base/util/exceptions.h>
#include <util/alloc.h>
#include <math.h>
#include <runtime/base/zend/zend_string.h>
#include <runtime/base/zend/zend_strtod.h>
#include <runtime/base/complex_types.h>
#include <runtime/base/runtime_option.h>
#include <runtime/base/runtime_error.h>
#include <runtime/base/type_conversions.h>
#include <runtime/base/builtin_functions.h>
#include <runtime/base/tainted_metadata.h>

namespace HPHP {

IMPLEMENT_SMART_ALLOCATION(StringData, SmartAllocatorImpl::NeedRestoreOnce);
///////////////////////////////////////////////////////////////////////////////
// constructor and destructor

StringData::StringData(const char *data,
                       StringDataMode mode /* = AttachLiteral */)
  : m_data(NULL), _count(0), m_len(0) {
  m_hash = 0;

  #ifdef TAINTED
  m_tainting = default_tainting;
  m_tainted_metadata = NULL;
  #endif
  assign(data, mode);
}

StringData::StringData(SharedVariant *shared)
  : m_data(NULL), _count(0), m_len(0) {
  m_hash = 0;
  #ifdef TAINTED
  m_tainting = default_tainting;
  m_tainted_metadata = NULL;
  #endif
  ASSERT(shared);
  shared->incRef();
  m_shared = shared;
  m_data = m_shared->stringData();
  m_len = m_shared->stringLength() | IsShared;
  ASSERT(m_data);
}

StringData::StringData(const char *data, int len, StringDataMode mode)
  : m_data(NULL), _count(0), m_len(0) {
  m_hash = 0;
  #ifdef TAINTED
  m_tainting = default_tainting;
  m_tainted_metadata = NULL;
  #endif
  assign(data, len, mode);
}

StringData::~StringData() {
  releaseData();
}

void StringData::releaseData() {
  if ((m_len & (IsLinear | IsLiteral)) == 0) {
    if (isShared()) {
      m_shared->decRef();
    } else if (m_data) {
      free((void*)m_data);
    }
  }
  m_hash = 0;
}

void StringData::assign(const char *data, StringDataMode mode) {
  ASSERT(data);
  assign(data, strlen(data), mode);
}

void StringData::assign(const char *data, int len, StringDataMode mode) {
  ASSERT(data);
  ASSERT(len >= 0);
  ASSERT(mode >= 0 && mode < StringDataModeCount);

  if (len < 0 || (len & IsMask)) {
    throw InvalidArgumentException("len: %d", len);
  }

  releaseData();
  m_hash = 0;
  m_len = len;
  if (m_len) {
    switch (mode) {
    case CopyString:
      {
        char *buf = (char*)malloc(len + 1);
        buf[len] = '\0';
        memcpy(buf, data, len);
        m_data = buf;
      }
      break;
    case AttachLiteral:
      m_len |= IsLiteral;
      m_data = data;
      ASSERT(m_data[len] == '\0');// all PHP strings need NULL termination
      break;
    case AttachString:
      m_data = data;
      ASSERT(m_data[len] == '\0');// all PHP strings need NULL termination
      break;
    default:
      ASSERT(false);
      break;
    }
  } else {
    if (mode == AttachString) {
      free((void*)data); // we don't really need a malloc-ed empty string
    }
    m_len |= IsLiteral;
    m_data = "";
  }
  ASSERT(m_data);
}

void StringData::append(const char *s, int len) {
  if (len == 0) return;

  if (len < 0 || (len & IsMask)) {
    throw InvalidArgumentException("len: %d", len);
  }

  ASSERT(!isStatic()); // never mess around with static strings!

  if (!isMalloced()) {
    int newlen;
    m_data = string_concat(data(), size(), s, len, newlen);
    if (isShared()) {
      m_shared->decRef();
    }
    m_len = newlen;
    m_hash = 0;
  } else if (m_data == s) {
    int newlen;
    char *newdata = string_concat(data(), size(), s, len, newlen);
    releaseData();
    m_data = newdata;
    m_len = newlen;
  } else {
    int dataLen = size();
    ASSERT((m_data > s && m_data - s > len) ||
           (m_data < s && s - m_data > dataLen)); // no overlapping
    m_len = len + dataLen;
    m_data = (const char*)realloc((void*)m_data, m_len + 1);
    memcpy((void*)(m_data + dataLen), s, len);
    ((char*)m_data)[m_len] = '\0';
    m_hash = 0;
  }

  if (m_len & IsMask) {
    int len = m_len;
    m_len &= ~IsMask;
    releaseData();
    m_data = NULL;
    throw FatalErrorException(0, "String length exceeded 2^29 - 1: %d", len);
  }
}

StringData *StringData::copy(bool sharedMemory /* = false */) const {
  if (isStatic()) {
    // Static strings cannot change, and are always available.
    return const_cast<StringData *>(this);
  }
  if (sharedMemory) {
    // Even if it's literal, it might come from hphpi's class info
    // which will be freed at the end of the request, and so must be
    // copied.
    return new StringData(m_data, size(), CopyString);
  } else {
    if (isLiteral()) {
      return NEW(StringData)(m_data, size(), AttachLiteral);
    }
    return NEW(StringData)(m_data, size(), CopyString);
  }
}

void StringData::escalate() {
  ASSERT(isImmutable() && !isStatic());

  int len = size();
  ASSERT(len);

  char *buf = (char*)malloc(len+1);
  memcpy(buf, data(), len);
  buf[len] = '\0';
  m_len = len;
  m_data = buf;
  // clear precomputed hashcode
  m_hash = 0;
}

StringData *StringData::escalate(StringData *in) {
  if (!in) return NEW(StringData)();
  if (in->_count != 1 || in->isImmutable()) {
    StringData *ret = NEW(StringData)(in->data(), in->size(), CopyString);
    ret->incRefCount();
    if (!in->decRefCount()) in->release();
    return ret;
  }
  in->m_hash = 0;
  return in;
}

void StringData::dump() const {
  const char *p = data();
  int len = size();

  printf("StringData(%d) (%s%s%s%s%d): [", _count,
         isLiteral() ? "literal " : "",
         isShared() ? "shared " : "",
         isLinear() ? "linear " : "",
         isStatic() ? "static " : "",
         len);
  for (int i = 0; i < len; i++) {
    char ch = p[i];
    if (isprint(ch)) {
      std::cout << ch;
    } else {
      printf("\\x%02x", ch);
    }
  }
  printf("]\n");
}

///////////////////////////////////////////////////////////////////////////////
// mutations

StringData *StringData::getChar(int offset) const {
  if (offset >= 0 && offset < size()) {
    char *buf = (char *)malloc(2);
    buf[0] = m_data[offset];
    buf[1] = 0;
    return NEW(StringData)(buf, 1, AttachString);
  }

  raise_notice("Uninitialized string offset: %d", offset);
  return NEW(StringData)("", 0, AttachLiteral);
}

void StringData::setChar(int offset, CStrRef substring) {
  ASSERT(!isStatic());
  if (offset >= 0) {
    int len = size();
    if (len == 0) {
      // PHP will treat data as an array and we don't want to follow that.
      throw OffsetOutOfRangeException();
    }

    if (offset < len) {
      if (!substring.empty()) {
        setChar(offset, substring.data()[0]);
      } else {
        removeChar(offset);
      }
    } else if (offset > RuntimeOption::StringOffsetLimit) {
      throw OffsetOutOfRangeException();
    } else {
      int newlen = offset + 1;
      char *buf = (char *)Util::safe_malloc(newlen + 1);
      memset(buf, ' ', newlen);
      buf[newlen] = 0;
      memcpy(buf, data(), len);
      if (!substring.empty()) buf[offset] = substring.data()[0];
      assign(buf, newlen, AttachString);
    }
  }
}

void StringData::setChar(int offset, char ch) {
  ASSERT(offset >= 0 && offset < size());
  ASSERT(!isStatic());
  if (isImmutable()) {
    escalate();
  }
  ((char*)m_data)[offset] = ch;
}

void StringData::removeChar(int offset) {
  ASSERT(offset >= 0 && offset < size());
  ASSERT(!isStatic());
  int len = size();
  if (isImmutable()) {
    char *data = (char*)malloc(len);
    if (offset) {
      memcpy(data, this->data(), offset);
    }
    if (offset < len - 1) {
      memcpy(data + offset, this->data() + offset + 1, len - offset - 1);
    }
    data[len] = 0;
    m_len = len;
    releaseData();
    m_data = data;
  } else {
    m_len = ((m_len & IsMask) | (len - 1));
    memmove((void*)(m_data + offset), m_data + offset + 1, len - offset);
    m_hash = 0;
  }
}

void StringData::inc() {
  ASSERT(!isStatic());
  if (empty()) {
    m_len = (IsLiteral | 1);
    m_data = "1";
    return;
  }
  if (isImmutable()) {
    escalate();
  }
  char *overflowed = increment_string((char *)m_data, size());
  if (overflowed) {
    assign(overflowed, AttachString);
  }
}

void StringData::negate() {
  if (empty()) return;
  ASSERT(!isImmutable());
  char *buf = (char*)m_data;
  int len = size();
  for (int i = 0; i < len; i++) {
    buf[i] = ~(buf[i]);
  }
}

///////////////////////////////////////////////////////////////////////////////
// type conversions

bool StringData::isNumeric() const {
  int len = size();
  if (len) {
    int64 lval; double dval;
    DataType ret = is_numeric_string(data(), len, &lval, &dval, 0);
    switch (ret) {
    case KindOfNull:   return false;
    case KindOfInt64:
    case KindOfDouble: return true;
    default:
      ASSERT(false);
      break;
    }
  }
  return false;
}

bool StringData::isInteger() const {
  int len = size();
  if (len) {
    int64 lval; double dval;
    DataType ret = is_numeric_string(data(), len, &lval, &dval, 0);
    switch (ret) {
    case KindOfNull:   return false;
    case KindOfInt64:  return true;
    case KindOfDouble: return false;
    default:
      ASSERT(false);
      break;
    }
  }
  return false;
}

bool StringData::isValidVariableName() const {
  return is_valid_var_name(data(), size());
}

#ifdef TAINTED
void StringData::setTaint(bitstring b){
  m_tainting = m_tainting | b;
  if(is_tainting_metadata(b)){
    // resetting the metadata
    if(m_tainted_metadata != NULL){
      delete m_tainted_metadata;
      m_tainted_metadata = NULL;
    }
    m_tainted_metadata = new TaintedMetadata();
  }
}
void StringData::unsetTaint(bitstring b){
  m_tainting = m_tainting & (~b);
  if(is_tainting_metadata(b)){
    // erasing the metadata
    if(m_tainted_metadata != NULL){
      delete m_tainted_metadata;
      m_tainted_metadata = NULL;
    }
  }
}
TaintedMetadata* StringData::getTaintedMetadata() const {
  return m_tainted_metadata;
}
#endif

bool StringData::toBoolean() const {
  return !empty() && !isZero();
}

int64 StringData::toInt64(int base /* = 10 */) const {
  return strtoll(data(), NULL, base);
}

double StringData::toDouble() const {
  int len = size();
  if (len) return zend_strtod(data(), NULL);
  return 0;
}

DataType StringData::toNumeric(int64 &ival, double &dval) const {
  int len = size();
  if (len) {
    DataType r = is_numeric_string(data(), len, &ival, &dval, 0);
    if (r == KindOfInt64 || r == KindOfDouble) return r;
  }
  return KindOfString;
}

///////////////////////////////////////////////////////////////////////////////
// comparisons

int StringData::numericCompare(const StringData *v2) const {
  ASSERT(v2);

  int64 lval1, lval2;
  double dval1, dval2;
  DataType ret1, ret2;
  if ((ret1 = is_numeric_string(data(), size(),
                                &lval1, &dval1, 0)) == KindOfNull ||
      (ret1 == KindOfDouble && !finite(dval1)) ||
      (ret2 = is_numeric_string(v2->data(), v2->size(),
                                &lval2, &dval2, 0)) == KindOfNull ||
      (ret2 == KindOfDouble && !finite(dval2))) {
    return -2;
  }
  if (ret1 == KindOfInt64 && ret2 == KindOfInt64) {
    if (lval1 > lval2) return 1;
    if (lval1 == lval2) return 0;
    return -1;
  }
  if (ret1 == KindOfDouble && ret2 == KindOfDouble) {
    if (dval1 > dval2) return 1;
    if (dval1 == dval2) return 0;
    return -1;
  }
  if (ret1 == KindOfDouble) {
    ASSERT(ret2 == KindOfInt64);
    dval2 = (double)lval2;
  } else {
    ASSERT(ret1 == KindOfInt64);
    ASSERT(ret2 == KindOfDouble);
    dval1 = (double)lval1;
  }

  if (dval1 > dval2) return 1;
  if (dval1 == dval2) return 0;
  return -1;
}

int StringData::compare(const StringData *v2) const {
  ASSERT(v2);

  if (v2 == this) return 0;

  int ret = numericCompare(v2);
  if (ret < -1) {
    int len1 = size();
    int len2 = v2->size();
    int len = len1 < len2 ? len1 : len2;
    ret = memcmp(data(), v2->data(), len);
    if (ret) return ret;
    if (len1 == len2) return 0;
    return len < len1 ? 1 : -1;
  }
  return ret;
}

int64 StringData::getSharedStringHash() const {
  ASSERT(isShared());
  return m_shared->stringHash();
}

///////////////////////////////////////////////////////////////////////////////

bool StringData::calculate(int &totalSize) {
  if (m_data && !isLiteral()) {
    totalSize += (size() + 1); // ending NULL
    return true;
  }
  return false;
}

void StringData::backup(LinearAllocator &allocator) {
  allocator.backup(m_data, size() + 1);
}

void StringData::restore(const char *&data) {
  ASSERT(!isLiteral());
  m_data = data;
  m_len &= LenMask;
  m_len |= IsLinear;
  m_hash = hash_string(m_data, size());
}

void StringData::sweep() {
  releaseData();
}

///////////////////////////////////////////////////////////////////////////////
// Debug

std::string StringData::toCPPString() const {
  return std::string(data(), size());
}

///////////////////////////////////////////////////////////////////////////////
}
