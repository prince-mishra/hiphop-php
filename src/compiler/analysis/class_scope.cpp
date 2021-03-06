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

#include <boost/foreach.hpp>
#include <boost/tuple/tuple.hpp>
#include <compiler/analysis/analysis_result.h>
#include <compiler/analysis/class_scope.h>
#include <compiler/analysis/function_scope.h>
#include <compiler/analysis/code_error.h>
#include <compiler/construct.h>
#include <compiler/analysis/variable_table.h>
#include <compiler/statement/statement_list.h>
#include <compiler/option.h>
#include <compiler/statement/interface_statement.h>
#include <util/util.h>
#include <compiler/analysis/constant_table.h>
#include <compiler/analysis/file_scope.h>
#include <runtime/base/class_info.h>
#include <compiler/parser/parser.h>
#include <compiler/statement/method_statement.h>
#include <runtime/base/zend/zend_string.h>

using namespace HPHP;
using namespace std;
using namespace boost;

///////////////////////////////////////////////////////////////////////////////

ClassScope::ClassScope(KindOf kindOf, const std::string &name,
                       const std::string &parent,
                       const vector<string> &bases,
                       const std::string &docComment, StatementPtr stmt,
                       FileScopePtr file)
  : BlockScope(name, docComment, stmt, BlockScope::ClassScope), m_file(file),
    m_kindOf(kindOf), m_parent(parent), m_bases(bases), m_attribute(0),
    m_redeclaring(-1), m_volatile(false), m_needStaticInitializer(false),
    m_derivesFromRedeclaring(FromNormal), m_derivedByDynamic(false),
    m_sep(false) {

  m_dynamic = Option::IsDynamicClass(m_name);

  // dynamic class is also volatile
  m_volatile = Option::AllVolatile || m_dynamic;

  ASSERT(m_parent.empty() || (!m_bases.empty() && m_bases[0] == m_parent));
}


// System
ClassScope::ClassScope(AnalysisResultPtr ar,
                       const std::string &name, const std::string &parent,
                       const std::vector<std::string> &bases,
                       const FunctionScopePtrVec &methods)
  : BlockScope(name, "", StatementPtr(), BlockScope::ClassScope),
    m_kindOf(KindOfObjectClass), m_parent(parent), m_bases(bases),
    m_attribute(0), m_dynamic(false), m_redeclaring(-1), m_volatile(false),
    m_needStaticInitializer(false),
    m_derivesFromRedeclaring(FromNormal), m_derivedByDynamic(false),
    m_sep(false) {
  BOOST_FOREACH(FunctionScopePtr f, methods) {
    if (f->getName() == "__construct") setAttribute(HasConstructor);
    else if (f->getName() == "__destruct") setAttribute(HasDestructor);
    else if (f->getName() == "__call") setAttribute(HasUnknownMethodHandler);
    else if (f->getName() == "__get") setAttribute(HasUnknownPropGetter);
    else if (f->getName() == "__set") setAttribute(HasUnknownPropSetter);
    addFunction(ar, f);
  }
  setAttribute(Extension);
  setAttribute(System);

  ASSERT(m_parent.empty() || (!m_bases.empty() && m_bases[0] == m_parent));
}

const std::string &ClassScope::getOriginalName() const {
  if (m_stmt) {
    return dynamic_pointer_cast<InterfaceStatement>(m_stmt)->
      getOriginalName();
  }
  return m_originalName;
}

std::string ClassScope::getId(CodeGenerator &cg) const {
  string name = cg.formatLabel(getOriginalName());
  if (m_redeclaring < 0) {
    return name;
  }
  return name + Option::IdPrefix +
    boost::lexical_cast<std::string>(m_redeclaring);
}

///////////////////////////////////////////////////////////////////////////////

int ClassScope::implementsArrayAccess(AnalysisResultPtr ar) {
  hphp_const_char_imap<int>::iterator it = m_implemented.find("arrayaccess");
  if (it != m_implemented.end()) {
    return it->second;
  }

  int ret = 0;
  unsigned s = m_parent.empty() ? 0 : 1;
  for (unsigned i = s; i < m_bases.size(); i++) {
    if (strcasecmp(m_bases[i].c_str(), "arrayaccess")) {
      ret = 1;
      break;
    }
  }

  if (s && !ret) {
    int yes = 0, no = 0;
    ClassScopePtrVec classes = ar->findClasses(m_parent);
    BOOST_FOREACH(ClassScopePtr cls, classes) {
      int a = cls->implementsArrayAccess(ar);
      if (a < 0) {
        yes = no = 1;
        break;
      }
      if (a > 0) {
        yes++;
      } else {
        no++;
      }
    }
    if (yes) {
      ret = no ? -1 : 1;
    }
  }

  m_implemented["arrayaccess"] = ret;
  return ret;
}

int ClassScope::implementsAccessor(AnalysisResultPtr ar, const char *name) {
  hphp_const_char_imap<int>::iterator it = m_implemented.find(name);
  if (it != m_implemented.end()) {
    return it->second;
  }

  int ret = 0;
  for (StringToFunctionScopePtrVecMap::const_iterator iter =
         m_functions.begin(); iter != m_functions.end(); ++iter) {

    if (strcasecmp(iter->first.c_str(), name)) {
      ret = 1;
    }
  }

  if (!m_parent.empty() && !ret) {
    int yes = 0, no = 0;
    ClassScopePtrVec classes = ar->findClasses(m_parent);
    BOOST_FOREACH(ClassScopePtr cls, classes) {
      int a = cls->implementsAccessor(ar, name);
      if (a < 0) {
        yes = no = 1;
        break;
      }
      if (a > 0) {
        yes++;
      } else {
        no++;
      }
    }
    if (yes) {
      ret = no ? -1 : 1;
    }
  }

  m_implemented[name] = ret;
  return ret;
}

void ClassScope::checkDerivation(AnalysisResultPtr ar, hphp_string_set &seen) {
  seen.insert(m_name);

  hphp_string_set bases;
  for (int i = m_bases.size() - 1; i >= 0; i--) {
    const string &base = m_bases[i];

    if (seen.find(base) != seen.end() || bases.find(base) != bases.end()) {
      ar->getCodeError()->record(CodeError::InvalidDerivation, m_stmt,
                                 ConstructPtr(), base.c_str());
      if (i == 0 && !m_parent.empty()) {
        ASSERT(base == m_parent);
        m_parent.clear();
      }
      m_bases.erase(m_bases.begin() + i);
      continue;
    }
    bases.insert(base);

    ClassScopePtrVec parents = ar->findClasses(base);
    for (unsigned int j = 0; j < parents.size(); j++) {
      parents[j]->checkDerivation(ar, seen);
    }
  }

  seen.erase(m_name);
}

void ClassScope::collectMethods(AnalysisResultPtr ar,
                                StringToFunctionScopePtrMap &funcs,
                                bool collectPrivate /* = true */,
                                bool forInvoke /* = false */) {
  // add all functions this class has
  for (StringToFunctionScopePtrVecMap::const_iterator iter =
         m_functions.begin(); iter != m_functions.end(); ++iter) {
    if (!collectPrivate && iter->second.back()->isPrivate()) continue;

    StringToFunctionScopePtrMap::const_iterator iterFuncs =
      funcs.find(iter->first);
    if (iterFuncs == funcs.end()) {
      funcs[iter->first] = iter->second.back();
    } else {
      iterFuncs->second->setVirtual();
      iter->second.back()->setVirtual();
    }
  }

  BOOST_FOREACH(string miss, m_missingMethods) {
    StringToFunctionScopePtrMap::const_iterator iterFuncs =
      funcs.find(miss);
    if (iterFuncs != funcs.end()) {
      iterFuncs->second->setVirtual();
    }
  }

  // walk up
  for (int i = m_bases.size() - 1; i >= 0; i--) {
    const string &base = m_bases[i];
    if (forInvoke && base != m_parent) {
      continue;
    }
    ClassScopePtr super = ar->findClass(base);
    if (super) {
      if (derivedByDynamic()) {
        super->m_derivedByDynamic = true;
      }
      if (super->isRedeclaring()) {
        if (base == m_parent) {
          if (forInvoke) continue;
          const ClassScopePtrVec &classes = ar->findRedeclaredClasses(m_parent);
          StringToFunctionScopePtrMap pristine(funcs);
          BOOST_FOREACH(ClassScopePtr cls, classes) {
            cls->m_derivedByDynamic = true;
            StringToFunctionScopePtrMap cur(pristine);
            cls->collectMethods(ar, cur, false, forInvoke);
            funcs.insert(cur.begin(), cur.end());
          }
          m_derivesFromRedeclaring = DirectFromRedeclared;
          getVariables()->forceVariants(ar, VariableTable::AnyNonPrivateVars);
          getVariables()->setAttribute(VariableTable::NeedGlobalPointer);
          setVolatile();
        } else if (isInterface()) {
          m_derivesFromRedeclaring = DirectFromRedeclared;
        }
      } else {
        super->collectMethods(ar, funcs, false, forInvoke);
        if (super->derivesFromRedeclaring()) {
          if (base == m_parent) {
            m_derivesFromRedeclaring = IndirectFromRedeclared;
            getVariables()->forceVariants(ar, VariableTable::AnyNonPrivateVars);
            setVolatile();
          } else if (isInterface()) {
            m_derivesFromRedeclaring = IndirectFromRedeclared;
          }
        }
      }
    } else {
      ar->getCodeError()->record(CodeError::UnknownBaseClass, m_stmt,
                                 ConstructPtr(), base.c_str());
      if (base == m_parent) {
        ar->declareUnknownClass(m_parent);
        m_derivesFromRedeclaring = DirectFromRedeclared;
        getVariables()->setAttribute(VariableTable::NeedGlobalPointer);
        getVariables()->forceVariants(ar, VariableTable::AnyNonPrivateVars);
        setVolatile();
      } else {
        if (isInterface()) {
          m_derivesFromRedeclaring = DirectFromRedeclared;
        }
        m_bases.erase(m_bases.begin() + i);
      }
    }
  }
}

bool ClassScope::needsInvokeParent(AnalysisResultPtr ar,
                                   bool considerSelf /* = true */) {
  // check all functions this class has
  if (considerSelf) {
    for (StringToFunctionScopePtrVecMap::const_iterator iter =
           m_functions.begin(); iter != m_functions.end(); ++iter) {
      if (iter->second.back()->isPrivate()) return true;
    }
  }

  // walk up
  if (!m_parent.empty()) {
    ClassScopePtr super = ar->findClass(m_parent);
    return !super || super->isRedeclaring() || super->needsInvokeParent(ar);
  }
  return false;
}

bool ClassScope::derivesDirectlyFrom(AnalysisResultPtr ar,
                                     const std::string &base) const {
  BOOST_FOREACH(std::string base_i, m_bases) {
    if (base_i == base) return true;
  }
  return false;
}

bool ClassScope::derivesFrom(AnalysisResultPtr ar,
                             const std::string &base,
                             bool strict, bool def) const {

  if (derivesDirectlyFrom(ar, base)) return true;

  BOOST_FOREACH(std::string base_i, m_bases) {
    ClassScopePtr cl = ar->findClass(base_i);
    if (cl) {
      if (strict && cl->isRedeclaring()) {
        if (def) return true;
        continue;
      }
      if (cl->derivesFrom(ar, base, strict, def)) return true;
    }
  }
  return false;
}

std::string ClassScope::findCommonParent(AnalysisResultPtr ar,
                                         const std::string cn1,
                                         const std::string cn2) {

  ClassScopePtr cls1 = ar->findClass(cn1);
  ClassScopePtr cls2 = ar->findClass(cn2);

  if (!cls1 || cls1->derivesFrom(ar, cn2, true, false)) return cn2;
  if (!cls2 || cls2->derivesFrom(ar, cn1, true, false)) return cn1;

  // walk up the class hierarchy.
  BOOST_FOREACH(std::string base1, cls1->m_bases) {
    BOOST_FOREACH(std::string base2, cls2->m_bases) {
      std::string parent = findCommonParent(ar, base1, base2);
      if (!parent.empty()) return parent;
    }
  }

  return "";
}

FunctionScopePtr ClassScope::findFunction(AnalysisResultPtr ar,
                                          const std::string &name,
                                          bool recursive,
                                          bool exclIntfBase /*= false */) {
  ASSERT(Util::toLower(name) == name);
  StringToFunctionScopePtrVecMap::const_iterator iter;
  iter = m_functions.find(name);
  if (iter != m_functions.end()) {
    ASSERT(iter->second.back());
    return iter->second.back();
  }

  // walk up
  if (recursive) {
    int s = m_bases.size();
    for (int i = 0; i < s; i++) {
      const string &base = m_bases[i];
      ClassScopePtr super = ar->findClass(base);
      if (!super) continue;
      if (exclIntfBase && super->isInterface()) break;
      if (super->isRedeclaring()) {
        if (!super->isInterface()) {
          m_derivesFromRedeclaring = DirectFromRedeclared;
          break;
        }
        continue;
      }
      FunctionScopePtr func =
        super->findFunction(ar, name, true, exclIntfBase);
      if (func) return func;
    }
  }
  if (derivesFromRedeclaring() == DirectFromRedeclared) {
    setDynamic(ar, name);
  }

  return FunctionScopePtr();
}

FunctionScopePtr ClassScope::findConstructor(AnalysisResultPtr ar,
                                             bool recursive) {
  StringToFunctionScopePtrVecMap::const_iterator iter;
  string name;
  if (classNameCtor()) {
    name = getName();
  }
  else {
    name = "__construct";
  }
  iter = m_functions.find(name);
  if (iter != m_functions.end()) {
    ASSERT(iter->second.back());
    return iter->second.back();
  }

  // walk up
  if (recursive && derivesFromRedeclaring() != DirectFromRedeclared) {
    ClassScopePtr super = ar->findClass(m_parent);
    if (super) {
      FunctionScopePtr func =
        super->findConstructor(ar, true);
      if (func) return func;
    }
  }
  if (derivesFromRedeclaring() == DirectFromRedeclared) {
    setDynamic(ar, name);
  }

  return FunctionScopePtr();
}

void ClassScope::setStaticDynamic(AnalysisResultPtr ar) {
  for (StringToFunctionScopePtrVecMap::const_iterator iter =
         m_functions.begin(); iter != m_functions.end(); ++iter) {
    BOOST_FOREACH(FunctionScopePtr fs, iter->second) {
      if (fs->isStatic()) fs->setDynamic();
    }
  }
  if (!m_parent.empty()) {
    if (derivesFromRedeclaring() == DirectFromRedeclared) {
      const ClassScopePtrVec &parents = ar->findRedeclaredClasses(m_parent);
      BOOST_FOREACH(ClassScopePtr cl, parents) {
        cl->setStaticDynamic(ar);
      }
    } else {
      ClassScopePtr parent = ar->findClass(m_parent);
      if (parent) {
        parent->setStaticDynamic(ar);
      }
    }
  }
}

void ClassScope::setDynamic(AnalysisResultPtr ar, const std::string &name) {
  StringToFunctionScopePtrVecMap::const_iterator iter =
    m_functions.find(name);
  if (iter != m_functions.end()) {
    BOOST_FOREACH(FunctionScopePtr fs, iter->second) {
      fs->setDynamic();
    }
  } else if (!m_parent.empty()) {
    if (derivesFromRedeclaring() == DirectFromRedeclared) {
      const ClassScopePtrVec &parents = ar->findRedeclaredClasses(m_parent);
      BOOST_FOREACH(ClassScopePtr cl, parents) {
        cl->setDynamic(ar, name);
      }
    } else {
      ClassScopePtr parent = ar->findClass(m_parent);
      if (parent) {
        parent->setDynamic(ar, name);
      }
    }
  }
}

void ClassScope::setSystem() {
  setAttribute(ClassScope::System);
  m_volatile = m_dynamic = false;
  for (StringToFunctionScopePtrVecMap::const_iterator iter =
         m_functions.begin(); iter != m_functions.end(); ++iter) {
    iter->second[0]->setSystem();
  }
}

bool ClassScope::needLazyStaticInitializer() {
  return getVariables()->getAttribute(VariableTable::ContainsDynamicStatic) ||
    getConstants()->hasDynamic();
}

bool ClassScope::hasAttribute(ClassScope::Attribute attr,
                              AnalysisResultPtr ar) const {
  if (getAttribute(attr)) return true;

  if (!m_parent.empty()) {
    ClassScopePtr super = ar->findClass(m_parent);
    if (super) return super->hasAttribute(attr, ar);
  }

  return false;
}

void ClassScope::outputCPPClassMap(CodeGenerator &cg, AnalysisResultPtr ar) {
  // header
  int attribute = ClassInfo::IsNothing;
  if (!isUserClass()) attribute |= ClassInfo::IsSystem;
  if (isRedeclaring()) attribute |= ClassInfo::IsRedeclared;
  if (isVolatile()) attribute |= ClassInfo::IsVolatile;
  if (isInterface()) attribute |= ClassInfo::IsInterface|ClassInfo::IsAbstract;
  if (m_kindOf == KindOfAbstractClass) attribute |= ClassInfo::IsAbstract;
  if (m_kindOf == KindOfFinalClass) attribute |= ClassInfo::IsFinal;
  if (needLazyStaticInitializer()) attribute |= ClassInfo::IsLazyInit;

  attribute |= m_attributeClassInfo;
  if (!m_docComment.empty() && Option::GenerateDocComments) {
    attribute |= ClassInfo::HasDocComment;
  } else {
    attribute &= ~ClassInfo::HasDocComment;
  }

  string parent;
  if (!m_parent.empty()) {
    ClassScopePtr parCls = ar->findClass(m_parent);
    if (parCls) {
      parent = parCls->getOriginalName();
    } else {
      parent = m_parent;
    }
  }
  cg_printf("(const char *)0x%04X, \"%s\", \"%s\", \"%s\", (const char *)%d, "
            "(const char *)%d,\n", attribute,
            getOriginalName().c_str(), parent.c_str(),
            m_stmt ? m_stmt->getLocation()->file : "",
            m_stmt ? m_stmt->getLocation()->line0 : 0,
            m_stmt ? m_stmt->getLocation()->line1 : 0);

  if (!m_docComment.empty() && Option::GenerateDocComments) {
    char *dc = string_cplus_escape(m_docComment.c_str(), m_docComment.size());
    cg_printf("\"%s\",\n", dc);
    free(dc);
  }

  // parent interfaces
  for (unsigned int i = (m_parent.empty() ? 0 : 1); i < m_bases.size(); i++) {
    string base;
    ClassScopePtr baseCls = ar->findClass(m_bases[i]);
    if (baseCls) {
      base = baseCls->getOriginalName();
    } else {
      base = m_bases[i];
    }
    cg_printf("\"%s\", ", base.c_str());
  }
  cg_printf("NULL,\n");

  // methods
  for (unsigned int i = 0; i < m_functionsVec.size(); i++) {
    m_functionsVec[i]->outputCPPClassMap(cg, ar);
  }
  cg_printf("NULL,\n");

  // properties && constants
  ar->pushScope(shared_from_this());
  m_variables->outputCPPClassMap(cg, ar);
  m_constants->outputCPPClassMap(cg, ar);
  ar->popScope();
}

bool ClassScope::hasConst(const string &name) {
  return m_constants->isPresent(name);
}

Symbol *ClassScope::findProperty(ClassScopePtr &cls,
                                 const string &name,
                                 AnalysisResultPtr ar, ConstructPtr construct) {
  return getVariables()->findProperty(cls, name, ar, construct);
}

TypePtr ClassScope::checkProperty(Symbol *sym, TypePtr type,
                                  bool coerce, AnalysisResultPtr ar) {
  return getVariables()->checkProperty(sym, type, coerce, ar);
}

TypePtr ClassScope::checkStatic(const std::string &name, TypePtr type,
                                bool coerce, AnalysisResultPtr ar,
                                ConstructPtr construct, int &properties) {
  ar->pushScope(shared_from_this());
  TypePtr ret = getVariables()->checkVariable(name, type, coerce,
                                              ar, construct, properties);
  ar->popScope();
  return ret;
}

TypePtr ClassScope::checkConst(const std::string &name, TypePtr type,
                               bool coerce, AnalysisResultPtr ar,
                               ConstructPtr construct,
                               const std::vector<std::string> &bases,
                               BlockScope *&defScope) {
  defScope = NULL;
  TypePtr t = getConstants()->check(name, type, coerce,
                                    ar, construct, m_bases, defScope);
  if (defScope) return t;
  return t;
}

ClassScopePtr ClassScope::getParentScope(AnalysisResultPtr ar) {
  if (m_parent.empty()) return ClassScopePtr();
  return ar->findClass(m_parent);
}

void ClassScope::serialize(JSON::OutputStream &out) const {
  JSON::MapStream ms(out);
  map<string, int> propMap;
  set<string> names;
  m_variables->getNames(names);
  BOOST_FOREACH(string name, names) {
    int pm = 0;
    if (m_variables->isPublic(name)) pm |= ClassScope::Public;
    else if (m_variables->isPrivate(name)) pm |= ClassScope::Private;
    else if (m_variables->isProtected(name)) pm |= ClassScope::Protected;
    if (m_variables->isStatic(name)) pm |= ClassScope::Static;
    propMap[name] = pm;
  }
  names.clear();
  vector<string> cnames;
  m_constants->getSymbols(cnames);

  // What's a mod again?
  ms.add("attributes", m_attribute)
    .add("kind", m_kindOf)
    .add("parent", m_parent)
    .add("bases", m_bases)
    .add("properties", propMap)
    .add("functions", m_functions);

  ms.add("consts");

  JSON::MapStream cs(out);
  BOOST_FOREACH(string cname, cnames) {
    TypePtr type =  m_constants->getType(cname, true);
    if (!type) {
      cs.add(cname, -1);
    } else if (type->isSpecificObject()) {
      cs.add(cname, type->getName());
    } else {
      cs.add(cname, type->getKindOf());
    }
  }
  cs.done();
  ms.done();
}

void ClassScope::outputCPPDynamicClassDecl(CodeGenerator &cg) {
  string clsStr = getId(cg);
  const char *clsName = clsStr.c_str();
  cg_printf("Object %s%s(CArrRef params, bool init = true);\n",
            Option::CreateObjectPrefix, clsName);
  cg_printf("Object %s%s();\n",
            Option::CreateObjectOnlyPrefix, clsName);
}

void ClassScope::outputCPPDynamicClassCreateDecl(CodeGenerator &cg) {
  cg_printf("Object create_object_only(const char *s, ObjectData *root);\n");
}

void ClassScope::outputCPPDynamicClassImpl(CodeGenerator &cg,
                                           AnalysisResultPtr ar) {
  string clsStr = getId(cg);
  const char *clsName = clsStr.c_str();
  cg_indentBegin("Object %s%s(CArrRef params, bool init /* = true */) {\n",
                 Option::CreateObjectPrefix, clsName);
  cg_printf("return Object((NEW(%s%s)())->dynCreate(params, init));\n",
            Option::ClassPrefix, clsName);
  cg_indentEnd("}\n");
  cg_indentBegin("Object %s%s() {\n", Option::CreateObjectOnlyPrefix, clsName);
  cg_printf("Object r(NEW(%s%s)());\n", Option::ClassPrefix, clsName);
  cg_printf("r->init();\n");
  cg_printf("return r;\n");
  cg_indentEnd("}\n");
}

void ClassScope::outputCPPClassJumpTable
(CodeGenerator &cg,
 const StringToClassScopePtrVecMap &classScopes,
 const vector<const char*> &classes, const char* macro) {
  cg.printDeclareGlobals();
  for (JumpTable jt(cg, classes, true, false, false); jt.ready(); jt.next()) {
    const char *clsName = jt.key();
    StringToClassScopePtrVecMap::const_iterator iterClasses =
      classScopes.find(Util::toLower(clsName));
    bool redeclaring = iterClasses->second[0]->isRedeclaring();
    if (iterClasses != classScopes.end()) {
      const char *suffix = "";
      if (redeclaring) suffix = "_REDECLARED";
      else if (iterClasses->second[0]->isVolatile()) suffix = "_VOLATILE";
      cg_printf("%s%s", macro, suffix);
      cg_printf("(0x%016llXLL, %s);\n",
                hash_string_i(clsName),
                redeclaring ? iterClasses->second[0]->getName().c_str() :
                cg.formatLabel(clsName).c_str());
    }
  }
}

void ClassScope::outputCPPClassVarInitImpl
(CodeGenerator &cg, const StringToClassScopePtrVecMap &classScopes,
 const vector<const char*> &classes) {
  bool system = cg.getOutput() == CodeGenerator::SystemCPP;
  cg_indentBegin("Variant get%s_class_var_init(const char *s, "
                 "const char *var) {\n",
                 system ? "_builtin" : "");
  bool withEval = !system && Option::EnableEval == Option::FullEval;
  if (withEval) {
    // See if there's an eval'd version
    cg_indentBegin("{\n");
    cg_printf("Variant r;\n");
    cg_printf("if (eval_get_class_var_init_hook(r, s, var)) "
              "return r;\n");
    cg_indentEnd("}\n");
  }
  outputCPPClassJumpTable(cg, classScopes, classes, "HASH_GET_CLASS_VAR_INIT");
  if (!system) {
    cg_printf("return get_builtin_class_var_init(s, var);\n");
  } else {
    cg_printf("return throw_missing_class(s);\n");
  }
  cg_indentEnd("}\n");
}

void ClassScope::outputCPPDynamicClassCreateImpl
(CodeGenerator &cg, const StringToClassScopePtrVecMap &classScopes,
 const vector<const char*> &classes) {
  bool system = cg.getOutput() == CodeGenerator::SystemCPP;
  bool withEval = !system && Option::EnableEval == Option::FullEval;
  // output create_object_only()
  cg_indentBegin("Object create%s_object_only(const char *s, "
      "ObjectData* root /* = NULL*/) {\n", system ?  "_builtin" : "");
  if (withEval) {
    // See if there's an eval'd version
    cg_indentBegin("{\n");
    cg_printf("Variant r;\n");
    cg_printf("if (eval_create_object_only_hook(r, s, root)) "
              "return r;\n");
    cg_indentEnd("}\n");
  }
  outputCPPClassJumpTable(cg, classScopes, classes, "HASH_CREATE_OBJECT_ONLY");
  if (!system) {
    cg_printf("return create_builtin_object_only(s, root);\n");
  } else {
    cg_printf("return throw_missing_class(s);\n");
  }
  cg_indentEnd("}\n");
}

void ClassScope::outputCPPInvokeStaticMethodImpl
(CodeGenerator &cg, const StringToClassScopePtrVecMap &classScopes,
 const vector<const char*> &classes) {
  return;
  bool system = cg.getOutput() == CodeGenerator::SystemCPP;
  cg_indentBegin("Variant invoke%s_static_method(const char *s, "
                 "const char *method, CArrRef params, bool fatal) {\n",
                 system ? "_builtin" : "");
  if (!system && Option::EnableEval == Option::FullEval) {
    // See if there's an eval'd version
    cg_printf("bool foundClass = false;\n");
    cg_indentBegin("{\n");
    cg_printf("Variant r;\n");

    cg_printf("if (eval_invoke_static_method_hook(r, s, method, params, "
        "foundClass)) return r;\n");
    cg_indentBegin("else if (foundClass) {\n");
    cg_printf("return o_invoke_failed(s, method, fatal);\n");
    cg_indentEnd("}\n");
    cg_indentEnd("}\n");
  }
  outputCPPClassJumpTable(cg, classScopes, classes,
                          "HASH_INVOKE_STATIC_METHOD");

  // There should be invoke_failed for static methods...
  if (!system) {
    cg_printf("return invoke_builtin_static_method(s, method, params, fatal);"
              "\n");
  } else {
    cg_indentBegin("if (fatal) {\n");
    cg_printf("return throw_missing_class(s);\n");
    cg_indentEnd("");
    cg_indentBegin("} else {\n");
    cg_printf("raise_warning(\"call_user_func to non-existent class's method"
              " %%s::%%s\", s, method);\n");
    cg_printf("return false;\n");
    cg_indentEnd("}\n");
  }
  cg_indentEnd("}\n");
}

void ClassScope::outputCPPGetCallInfoStaticMethodImpl
(CodeGenerator &cg, const StringToClassScopePtrVecMap &classScopes,
 const vector<const char*> &classes) {
  bool system = cg.getOutput() == CodeGenerator::SystemCPP;
  cg_indentBegin("bool get_call_info_static_method%s(MethodCallPackage &mcp)"
      " {\n", system ? "_builtin" : "");
  if (Option::UseMethodIndex) {
    cg_printf("return get_call_info_static_method_no_index%s(mcp);\n",
        system ? "_builtin" : "");
  } else {
    cg_printf("const char *s __attribute__((__unused__)) "
        "(mcp.rootObj.getCStr());\n");

    if (!system && Option::EnableEval == Option::FullEval) {
      cg_printf("bool foundClass = false;\n");
      cg_printf("if (eval_get_call_info_static_method_hook(mcp, foundClass)) "
          "return true;\n");
      cg_indentBegin("else if (foundClass) {\n");
      cg_printf("return false;\n");
      cg_indentEnd("}\n");
    }
    outputCPPClassJumpTable(cg, classScopes, classes,
        "HASH_CALL_INFO_STATIC_METHOD");

    if (!system) {
      cg_printf("return get_call_info_static_method_builtin(mcp);\n");
    } else {
      cg_printf("mcp.fail();\n");
      cg_printf("return false;\n");
    }
  }
  cg_indentEnd("}\n");

  cg_indentBegin("bool get_call_info_static_method_with_index%s"
      "(MethodCallPackage &mcp, MethodIndex mi) {\n",
      system ? "_builtin" : "");
  if (Option::UseMethodIndex) {
    cg_printf("const char *s __attribute__((__unused__)) "
        "(mcp.rootObj.getCStr());\n");

    if (!system && Option::EnableEval == Option::FullEval) {
      cg_printf("bool foundClass = false;\n");
      cg_printf("if (eval_get_call_info_static_method_hook(mcp, foundClass)) "
          "return true;\n");
      cg_indentBegin("else if (foundClass) {\n");
      cg_printf("return false;\n");
      cg_indentEnd("}\n");
    }
    outputCPPClassJumpTable(cg, classScopes, classes,
        "HASH_CALL_INFO_STATIC_METHOD_WITH_INDEX");

    if (!system) {
      cg_printf("return get_call_info_static_method_with_index_builtin(mcp, "
          "mi);\n");
    } else {
      cg_printf("mcp.fail();\n");
      cg_printf("return false;\n");
    }
  } else {
    cg_printf("return get_call_info_static_method%s(mcp);\n",
        system ? "_builtin" : "");
  }
  cg_indentEnd("}\n");
}


void ClassScope::outputCPPGetStaticPropertyImpl
(CodeGenerator &cg, const StringToClassScopePtrVecMap &classScopes,
 const vector<const char*> &classes) {
  bool system = cg.getOutput() == CodeGenerator::SystemCPP;

  cg_indentBegin("const ObjectStaticCallbacks * "
                 "get%s_object_static_callbacks(const char *s) {\n",
                 system ? "_builtin" : "");
  outputCPPClassJumpTable(cg, classScopes, classes,
                          "HASH_GET_OBJECT_STATIC_CALLBACKS");
  if (!system) {
    cg_printf("return get_builtin_object_static_callbacks(s);\n");
  } else {
    cg_printf("return NULL;\n");
  }
  cg_indentEnd("}\n");

  cg_indentBegin("Variant get%s_static_property(const char *s, "
                 "const char *prop) {\n",
                 system ? "_builtin" : "");
  if (!system && Option::EnableEval == Option::FullEval) {
    // See if there's an eval'd version
    cg_indentBegin("{\n");
    cg_printf("Variant r;\n");
    cg_printf("if (eval_get_static_property_hook(r, s, prop)) "
              "return r;\n");
    cg_indentEnd("}\n");
  }

  cg.indentBegin("{\n");
  cg.printf("const ObjectStaticCallbacks * cwo = "
            "get%s_object_static_callbacks(s);\n",
            system ? "_builtin" : "");
  cg.printf("if (cwo) return cwo->os_get(prop);\n");
  cg.indentEnd("}\n");

  if (!system) {
    cg_printf("return get_builtin_static_property(s, prop);\n");
  } else {
    cg_printf("return null;\n");
  }
  cg_indentEnd("}\n");

  cg_indentBegin("Variant *get%s_static_property_lv(const char *s, "
                 "const char *prop) {\n",
                 system ? "_builtin" : "");
  if (!system && Option::EnableEval == Option::FullEval) {
    // See if there's an eval'd version
    cg_indentBegin("{\n");
    cg_printf("Variant *r;\n");
    cg_printf("if (eval_get_static_property_lv_hook(r, s, prop)) "
              "return r;\n");
    cg_indentEnd("}\n");
  }

  cg.indentBegin("{\n");
  cg.printf("const ObjectStaticCallbacks * cwo = "
            "get%s_object_static_callbacks(s);\n",
            system ? "_builtin" : "");
  cg.printf("if (cwo) return &cwo->os_lval(prop);\n");
  cg.indentEnd("}\n");

  if (!system) {
    cg_printf("return get_builtin_static_property_lv(s, prop);\n");
  } else {
    cg_printf("return NULL;\n");
  }
  cg_indentEnd("}\n");
}

void ClassScope::outputCPPGetClassConstantImpl
(CodeGenerator &cg, const StringToClassScopePtrVecMap &classScopes,
 const vector<const char*> &classes) {
  bool system = cg.getOutput() == CodeGenerator::SystemCPP;
  cg_indentBegin("Variant get%s_class_constant(const char *s, "
                 "const char *constant, bool fatal /* = true */) {\n",
                 system ? "_builtin" : "");
  if (!system && Option::EnableEval == Option::FullEval) {
    // See if there's an eval'd version
    cg_indentBegin("{\n");
    cg_printf("Variant r;\n");
    cg_printf("if (eval_get_class_constant_hook(r, s, constant)) "
              "return r;\n");
    cg_indentEnd("}\n");
  }

  cg.indentBegin("{\n");
  cg.printf("const ObjectStaticCallbacks * cwo = "
            "get%s_object_static_callbacks(s);\n",
            system ? "_builtin" : "");
  cg.printf("if (cwo) return cwo->os_constant(constant);\n");
  cg.indentEnd("}\n");

  if (!system) {
    cg_printf("return get_builtin_class_constant(s, constant, fatal);\n");
  } else {
    cg_indentBegin("if (fatal) {\n");
    cg_printf("raise_error(\"Couldn't find constant %%s::%%s\", s, "
              "constant);\n");
    cg_indentEnd("");
    cg_indentBegin("} else {\n");
    cg_printf("raise_warning(\"Couldn't find constant %%s::%%s\", s, "
              "constant);\n");
    cg_indentEnd("}\n");
    cg_printf("return null;\n");
  }
  cg_indentEnd("}\n");
}

bool ClassScope::hasProperty(const string &name) {
  return m_variables->isPresent(name);
}

void ClassScope::setRedeclaring(AnalysisResultPtr ar, int redecId) {
  m_redeclaring = redecId;
  setVolatile(); // redeclared class is also volatile
  for (StringToFunctionScopePtrVecMap::const_iterator iter =
         m_functions.begin(); iter != m_functions.end(); ++iter) {
    BOOST_FOREACH(FunctionScopePtr fs, iter->second) {
      fs->setDynamic();
    }
  }
  m_variables->forceVariants(ar, VariableTable::AnyNonPrivateVars);
}

ClassScopePtr ClassScope::getRootParent(AnalysisResultPtr ar,
                                        const std::string &methodName) {
  ClassScopePtr root = dynamic_pointer_cast<ClassScope>(shared_from_this());
  for (ClassScopePtr cls = getParentScope(ar); cls;
       cls = cls->getParentScope(ar)) {
    if (methodName.empty() ||
        cls->m_functions.find(methodName) != cls->m_functions.end()) {
      root = cls;
    }
  }
  return root;
}

void ClassScope::getRootParents(AnalysisResultPtr ar,
                                const std::string &methodName,
                                ClassScopePtrVec &roots,
                                ClassScopePtr curClass) {
  ClassScopePtr root = dynamic_pointer_cast<ClassScope>(shared_from_this());
  if (m_parent.empty()) {
    roots.push_back(curClass);
  } else {
    ClassScopePtrVec parents = ar->findRedeclaredClasses(m_parent);
    for (unsigned int i = 0; i < parents.size(); i++) {
      ClassScopePtr cls = parents[i];
      if (methodName.empty() ||
          cls->m_functions.find(methodName) != cls->m_functions.end()) {
        curClass = cls;
      }
      cls->getRootParents(ar, methodName, roots, curClass);
    }
  }
}

string ClassScope::getHeaderFilename(CodeGenerator &cg) {
  FileScopePtr file = getFileScope();
  ASSERT(file);
  string fileBase = file->outputFilebase();
  string headerFile = Option::ClassHeaderPrefix;
  headerFile += getId(cg) + ".h";
  return headerFile;
}

void ClassScope::outputCPPHeader(CodeGenerator &old_cg, AnalysisResultPtr ar,
                                 CodeGenerator::Output output) {
  string filename = getHeaderFilename(old_cg);
  string root = ar->getOutputPath() + "/";
  Util::mkdir(root + filename);
  ofstream f((root + filename).c_str());
  CodeGenerator cg(&f, output);

  cg.headerBegin(filename);

  // 1. includes
  BOOST_FOREACH(string base, m_bases) {
    ClassScopePtr cls = ar->findClass(base);
    if (cls && cls->isUserClass()) {
      cg_printInclude(cls->getHeaderFilename(cg));
    }
  }

  // 2. Declarations
  cg.namespaceBegin();
  ar->pushScope(shared_from_this());
  cg.setContext(CodeGenerator::CppDeclaration);
  getStmt()->outputCPP(cg, ar);
  ar->popScope();
  cg.namespaceEnd();

  cg.headerEnd(filename);
}

void ClassScope::outputCPPSupportMethodsImpl(CodeGenerator &cg,
                                             AnalysisResultPtr ar) {
  string clsNameStr = getId(cg);
  const char *clsName = clsNameStr.c_str();
  bool dynamicObject = derivesFromRedeclaring() == DirectFromRedeclared;
  string parent = "ObjectData";
  string parentName = "ObjectData";
  if (!getParent().empty()) {
    parentName = getParent();
    ClassScopePtr cls = ar->findClass(parentName);
    if (cls) {
      parent = cls->getId(cg);
    } else {
      parent = parentName;
    }
  }

  if (Option::GenerateCPPMacros) {
    // Constant Lookup Table
    getVariables()->outputCPPPropertyTable(cg, ar, parent.c_str(),
                                           parentName.c_str(),
                                           derivesFromRedeclaring());

    // If parent is redeclared, you have to go to their class statics object.
    if (dynamicObject) {
      cg_indentBegin("Variant %s%s::%sconstant(const char *s) {\n",
                     Option::ClassPrefix, clsName, Option::ObjectStaticPrefix);
      cg.printDeclareGlobals();
      getConstants()->outputCPPJumpTable(cg, ar, !dynamicObject, false);
      cg_printf("return %s->%s%s->%sconstant(s);\n", cg.getGlobals(ar),
                Option::ClassStaticsObjectPrefix, parentName.c_str(),
                Option::ObjectStaticPrefix);
      cg_indentEnd("}\n");
    } else {
      cg.ifdefBegin(false, "OMIT_JUMP_TABLE_CLASS_CONSTANT_%s", clsName);
      cg_indentBegin("Variant %s%s::%sconstant(const char *s) {\n",
                     Option::ClassPrefix, clsName, Option::ObjectStaticPrefix);
      getConstants()->outputCPPJumpTable(cg, ar, !dynamicObject, false);
      cg_printf("return %s%s::%sconstant(s);\n", Option::ClassPrefix,
                parent.c_str(), Option::ObjectStaticPrefix);
      cg_indentEnd("}\n");
      cg.ifdefEnd("OMIT_JUMP_TABLE_CLASS_CONSTANT_%s", clsName);
    }

    cg_printf("IMPLEMENT_CLASS(%s)\n", clsName);
  }

  if (Option::GenerateCPPMacros) {
  }

  // Destruct method
  if (getAttribute(ClassScope::HasDestructor)) {
    cg_indentBegin("void %s%s::destruct() {\n", Option::ClassPrefix, clsName);
    cg_indentBegin("if (!inCtorDtor()) {\n");
    cg_printf("incRefCount();\n");
    cg_indentBegin("try {\n");
    cg_printf("%s__destruct();\n", Option::MethodPrefix);
    cg_indentEnd("} catch (...) { handle_destructor_exception();}\n");
    cg_indentEnd("}\n");
    cg_indentEnd("}\n");
  }

  // instanceof
  if (!isExtensionClass()) { // Extension class uses macros
    cg_indentBegin("bool %s%s::o_instanceof(CStrRef s) const {\n",
                   Option::ClassPrefix, clsName);
    vector<string> bases;
    getAllParents(ar, bases);
    // Eliminate duplicates
    sort(bases.begin(), bases.end());
    bases.erase(unique(bases.begin(), bases.end()), bases.end());
    vector<const char *> ancestors;
    // Convert to char * and add self
    ancestors.push_back(m_name.c_str());
    for (unsigned int i = 0; i < bases.size(); i++) {
      ancestors.push_back(bases[i].c_str());
    }
    for (JumpTable jt(cg, ancestors, true, false, true);
         jt.ready(); jt.next()) {
      const char *name = jt.key();
      string nameStr(name);
      cg_printf("HASH_INSTANCEOF(0x%016llXLL, ", hash_string_i(name));
      cg_printString(nameStr, ar);
      cg_printf(");\n");
    }
    if (derivesFromRedeclaring()) {
      cg_printf("if (parent->o_instanceof(s)) return true;\n");
    }
    cg_printf("return false;\n");
    cg_indentEnd("}\n");
  }

  // Cloning
  cg_indentBegin("ObjectData *%s%s::cloneImpl() {\n",
                 Option::ClassPrefix, clsName);
  cg_printf("%s%s *obj = NEW(%s%s)();\n", Option::ClassPrefix, clsName,
            Option::ClassPrefix, clsName);
  cg_printf("cloneSet(obj);\n");
  cg_printf("return obj;\n");
  cg_indentEnd("}\n");
  cg_indentBegin("void %s%s::cloneSet(%s%s *clone) {\n",
                 Option::ClassPrefix, clsName, Option::ClassPrefix, clsName);
  getVariables()->outputCPPPropertyClone(cg, ar, derivesFromRedeclaring());
  if (derivesFromRedeclaring()) {
    cg_printf("clone->setParent(parent->clone());\n");
  } else if(!getParent().empty()) {
    cg_printf("%s%s::cloneSet(clone);\n", Option::ClassPrefix, parent.c_str());
  } else {
    cg_printf("ObjectData::cloneSet(clone);\n");
  }
  cg_indentEnd("}\n");

  // doCall
  if (getAttribute(ClassScope::HasUnknownMethodHandler)) {
    cg_indentBegin("Variant %s%s::doCall(Variant v_name, Variant "
                   "v_arguments, bool fatal) {\n",
                   Option::ClassPrefix, clsName);
    cg_printf("return t___call(v_name, !v_arguments.isNull() ? "
              "v_arguments : Variant(Array::Create()));\n");
    cg_indentEnd("}\n");
  }

  if (isRedeclaring() && !derivesFromRedeclaring() && derivedByDynamic()) {
    cg_indentBegin("Variant %s%s::doRootCall(Variant v_name, Variant "
                   "v_arguments, bool fatal) {\n",
                   Option::ClassPrefix, clsName);
    cg_printf("return root->doCall(v_name, v_arguments, fatal);\n");
    cg_indentEnd("}\n");
  }

  // Invoke tables
  if (Option::GenerateCPPMacros) {
    outputCPPJumpTable(cg, ar, false, dynamicObject);
    outputCPPJumpTable(cg, ar, true, dynamicObject);
    if (cg.getOutput() == CodeGenerator::SystemCPP ||
        Option::EnableEval >= Option::LimitedEval) {
      outputCPPJumpTable(cg, ar, false, dynamicObject, Eval);
      outputCPPJumpTable(cg, ar, true, dynamicObject, Eval);
    }
    bool hasRedec;
    outputCPPCallInfoTableSupport(cg, ar, hasRedec);
    vector<const char *> funcs;
    findJumpTableMethods(cg, ar, false, funcs);
    outputCPPMethodInvokeTableSupport(cg, ar, funcs, m_functions, false);
    outputCPPMethodInvokeTableSupport(cg, ar, funcs, m_functions, true);
    outputCPPJumpTable(cg, ar, true, dynamicObject, CallInfo);
    outputCPPJumpTable(cg, ar, false, dynamicObject, CallInfo);
  }

  // Create method
  if (getAttribute(ClassScope::HasConstructor)
   || getAttribute(ClassScope::classNameConstructor)) {
    FunctionScopePtr func = findConstructor(ar, false);
    if (func && !func->isAbstract() && !isInterface()) {
      // abstract methods are not generated, neither should the create method
      // for an abstract constructor
      ar->pushScope(func);
      func->outputCPPCreateImpl(cg, ar);
      ar->popScope();
    }
  }

  outputCPPGlobalTableWrappersImpl(cg, ar);
}

void ClassScope::outputCPPStaticInitializerDecl(CodeGenerator &cg) {
  if (needStaticInitializer()) {
    cg_printf("void %s%s();\n", Option::ClassStaticInitializerPrefix,
              getId(cg).c_str());
  }
}

void ClassScope::outputCPPStaticMethodWrappers(CodeGenerator &cg,
                                               AnalysisResultPtr ar,
                                               set<string> &done,
                                               const char *cls) {
  const StringToFunctionScopePtrVecMap &fmap = getFunctions();
  for (StringToFunctionScopePtrVecMap::const_iterator it = fmap.begin();
       it != fmap.end(); ++it) {
    if (done.find(it->first) != done.end()) continue;
    MethodStatementPtr m =
      dynamic_pointer_cast<MethodStatement>(it->second[0]->getStmt());
    if (!m) continue; // system classes
    m->outputCPPStaticMethodWrapper(cg, ar, cls);
    done.insert(it->first);
  }
  if (derivesFromRedeclaring() != DirectFromRedeclared) {
    ClassScopePtr par = getParentScope(ar);
    if (par) par->outputCPPStaticMethodWrappers(cg, ar, done, cls);
  }
}

void ClassScope::outputCPPGlobalTableWrappersDecl(CodeGenerator &cg,
                                                  AnalysisResultPtr ar) {
  string id = getId(cg);
  cg_printf("extern struct ObjectStaticCallbacks %s%s;\n",
            Option::ClassWrapperFunctionPrefix, id.c_str());
}

void ClassScope::outputCPPGlobalTableWrappersImpl(CodeGenerator &cg,
                                                  AnalysisResultPtr ar) {
  string id = getId(cg);
  cg_indentBegin("struct ObjectStaticCallbacks %s%s = {\n",
                 Option::ClassWrapperFunctionPrefix, id.c_str());
  // This order must match the one in object_data.h
  cg_printf("%s%s::%sgetInit,\n", Option::ClassPrefix, id.c_str(),
            Option::ObjectStaticPrefix);
  cg_printf("%s%s::%sget,\n", Option::ClassPrefix, id.c_str(),
            Option::ObjectStaticPrefix);
  cg_printf("%s%s::%slval,\n", Option::ClassPrefix, id.c_str(),
            Option::ObjectStaticPrefix);
  cg_printf("%s%s::%sinvoke,\n", Option::ClassPrefix, id.c_str(),
            Option::ObjectStaticPrefix);
  cg_printf("%s%s::%sconstant,\n", Option::ClassPrefix, id.c_str(),
            Option::ObjectStaticPrefix);
  cg_printf("%s%s::%sget_call_info\n", Option::ClassPrefix, id.c_str(),
            Option::ObjectStaticPrefix);
  cg_indentEnd("};\n");
}

bool ClassScope::addFunction(AnalysisResultPtr ar,
                             FunctionScopePtr funcScope) {
  FunctionScopePtrVec &funcs = m_functions[funcScope->getName()];
  if (funcs.size() == 1) {
    funcs[0]->setRedeclaring(0);
    ar->getCodeError()->record(CodeError::DeclaredFunctionTwice,
                               funcScope->getStmt(),
                               funcs[0]->getStmt());
  }
  if (funcs.size() > 0) {
    funcScope->setRedeclaring(funcs.size());
  }
  funcs.push_back(funcScope);
  m_functionsVec.push_back(funcScope);
  return true;
}

void ClassScope::findJumpTableMethods(CodeGenerator &cg, AnalysisResultPtr ar,
                                      bool staticOnly,
                                      vector<const char *> &funcs) {
  bool systemcpp = cg.getOutput() == CodeGenerator::SystemCPP;
  // output invoke support methods
  for (StringToFunctionScopePtrVecMap::const_iterator iter =
         m_functions.begin(); iter != m_functions.end(); ++iter) {
    if (!iter->second[0]->isRedeclaring()) {
      FunctionScopePtr func = iter->second[0];
      if (func->isAbstract() ||
          (staticOnly && !func->isStatic()) ||
          !(systemcpp || func->isDynamic() || func->isVirtual())) continue;
      const char *name = iter->first.c_str();
      funcs.push_back(name);
    }
  }
}

void ClassScope::outputCPPMethodInvokeTableSupport(CodeGenerator &cg,
    AnalysisResultPtr ar, const vector<const char*> &keys,
    const StringToFunctionScopePtrVecMap &funcScopes, bool fewArgs) {
  string id = getId(cg);
  ClassScopePtr self = dynamic_pointer_cast<ClassScope>(shared_from_this());
  for (vector<const char*>::const_iterator it = keys.begin();
      it != keys.end(); ++it) {
    const char *name = *it;
    string lname = cg.formatLabel(name);
    StringToFunctionScopePtrVecMap::const_iterator iterFuncs;
    iterFuncs = funcScopes.find(name);
    ASSERT(iterFuncs != funcScopes.end());
    FunctionScopePtr func = iterFuncs->second[0];
    const char *extra = NULL;
    string prefix;
    const char *instance = NULL;
    if (func->isStatic()) {
      prefix += Option::ClassPrefix;
      prefix += id;
      prefix += "::";
      prefix += Option::MethodImplPrefix;
      extra = "c";
    } else {
      instance = "self->";
      prefix += Option::MethodPrefix;
    }
    cg_indentBegin("Variant %s%s::%s%s(MethodCallPackage &mcp, ",
        Option::ClassPrefix, id.c_str(),
        fewArgs ? Option::InvokeFewArgsPrefix : Option::InvokePrefix,
        lname.c_str());

    if (fewArgs) {
      cg_printf("int count, INVOKE_FEW_ARGS_IMPL_ARGS");
    } else {
      cg_printf("CArrRef params");
    }
    cg_printf(") {\n");
    if (!fewArgs) FunctionScope::OutputCPPDynamicInvokeCount(cg);
    if (!func->isStatic()) {
      cg_printf("%s%s *self = NULL;\n", Option::ClassPrefix, id.c_str());
      cg_printf("%s%s pobj;\n", Option::SmartPtrPrefix, id.c_str());
      // Instance method called as such
      cg_indentBegin("if (mcp.obj) {\n");
      cg_printf("self = static_cast<%s%s*>(mcp.obj);\n",
          Option::ClassPrefix, id.c_str());
      cg_indentEnd("");
      // Instance method called statically
      cg_indentBegin("} else {\n");
      cg_printf("pobj = (NEW(%s%s)());\n", Option::ClassPrefix, id.c_str());
      cg_printf("pobj->init();\n");
      cg_printf("pobj->setDummy();\n");
      cg_printf("self = pobj.get();\n");
      cg_indentEnd("}\n");
    } else {
      // If rootObj is an object, was a static method invoked instance style.
      // Use rootObj's class name as invoking class
      cg_printf("const char *c;\n");
      cg_indentBegin("if (mcp.rootObj.is(KindOfObject)) {\n");
      cg_printf("c = mcp.rootObj.getObjectData()->o_getClassName();\n");
      cg_indentEnd("");
      cg_indentBegin("} else {\n");
      cg_printf("c = mcp.rootObj.getCStr();\n");
      cg_indentEnd("}\n");
    }
    func->outputCPPDynamicInvoke(cg, ar, prefix.c_str(),
        lname.c_str(), false, fewArgs, true, extra,
        func->isConstructor(self), instance);
    cg_indentEnd("}\n");
  }
}

void ClassScope::
outputCPPMethodInvokeTable(CodeGenerator &cg, AnalysisResultPtr ar,
    const vector<const char*> &keys,
    const StringToFunctionScopePtrVecMap &funcScopes,
    bool fewArgs, bool staticOnly, ClassScope::TableType type) {
  ClassScopePtr self = dynamic_pointer_cast<ClassScope>(shared_from_this());

  bool useMethodIndex = Option::UseMethodIndex && type == CallInfo;
  shared_ptr<JumpTableBase> jt(useMethodIndex ?
      (JumpTableBase*) new JumpTableMethodIndex (cg, ar, keys) :
      (JumpTableBase*) new JumpTable(cg, keys, true, true, type == CallInfo));

  for (; jt->ready(); jt->next()) {
    const char *name = jt->key();
    string lname = cg.formatLabel(name);
    StringToFunctionScopePtrVecMap::const_iterator iterFuncs;
    iterFuncs = funcScopes.find(name);
    ASSERT(iterFuncs != funcScopes.end());
    FunctionScopePtr func = iterFuncs->second[0];
    string id = func->getClass()->getId(cg);
    if (fewArgs &&
        func->getMinParamCount() > Option::InvokeFewArgsCount)
      continue;
    string origName = func->getOriginalName();
    if (useMethodIndex) {
      const MethodSlot* ms = ar->getMethodSlot(name);
      cg_indentBegin("if (mi.m_overloadIndex == 0x%x) { \n",
          ms->getOverloadIndex());
    } else  {
      int index = -1;
      if (cg.checkLiteralString(origName, index, ar) >= 0) {
        cg_indentBegin("HASH_GUARD_LITSTR(0x%016llXLL, ", hash_string_i(name));
        cg_printString(origName, ar);
        cg_printf(") {\n");
      } else {
        cg_indentBegin("HASH_GUARD(0x%016llXLL, %s) {\n",
            hash_string_i(name), name);
      }
    }
    switch (type) {
      case Invoke:
        {
          cg_printf("MethodCallPackage mcp;\n");
          if (staticOnly) {
            cg_printf("mcp.staticMethodCall(c, s);\n");
          } else {
            cg_printf("mcp.methodCallEx(this, s);\n");
            cg_printf("mcp.obj = this;\n");
          }
          cg_printf("return %s%s::%s%s(mcp, ",
              Option::ClassPrefix, id.c_str(),
              fewArgs ? Option::InvokeFewArgsPrefix : Option::InvokePrefix,
              lname.c_str());
          if (fewArgs) {
            cg_printf("count, INVOKE_FEW_ARGS_PASS_ARGS);\n");
          } else {
            cg_printf("params);\n");
          }
        }
        break;
      case Eval:
        {
          const char *extra = NULL;
          const char *prefix = Option::MethodPrefix;
          if (func->isStatic()) {
            prefix = Option::MethodImplPrefix;
            if (staticOnly) {
              extra = "c";
            } else {
              extra = "o_getClassName()";
            }
          }
          func->outputCPPEvalInvoke(cg, ar, prefix, lname.c_str(),
              extra, true, func->isConstructor(self));
        }
        break;
      case CallInfo:
        cg_printf("mcp.ci = &%s%s::%s%s;\n", Option::ClassPrefix,
            id.c_str(), Option::CallInfoPrefix, lname.c_str());
        if (!staticOnly) {
          cg_printf("mcp.obj = this;\n");
        }
        cg_printf("return true;\n");
        break;
      default: ASSERT(false);
    }
    cg_indentEnd("}\n");
  }
}

void ClassScope::outputCPPJumpTableDecl(CodeGenerator &cg,
    AnalysisResultPtr ar) {
  for (StringToFunctionScopePtrVecMap::const_iterator iter =
         m_functions.begin(); iter != m_functions.end(); ++iter) {
    FunctionScopePtr func = iter->second[0];
    string id = cg.formatLabel(func->getName());
    cg_printf("DECLARE_METHOD_INVOKE_HELPERS(%s);\n", id.c_str());
  }
}

void ClassScope::outputCPPJumpTable(CodeGenerator &cg,
    AnalysisResultPtr ar, bool staticOnly, bool dynamicObject /* = false */,
    ClassScope::TableType type /* = Invoke */) {
  if (type == Invoke) return;
  string id = getId(cg);
  const char *clsName = id.c_str();

  string scope;
  scope += Option::ClassPrefix;
  scope += id;
  scope += "::";
  string parentExpr, parent, parentName;
  if (m_parent.empty()) {
    parentName = "ObjectData";
    parent = "ObjectData";
  } else {
    parentName = m_parent;
    ClassScopePtr cls = ar->findClass(m_parent);
    if (cls) {
      parent = cls->getId(cg);
    } else {
      parent = parentName;
    }
  }
  bool system = cg.getOutput() == CodeGenerator::SystemCPP;
  bool needGlobals = false;
  if (dynamicObject) {
    if (staticOnly) {
      needGlobals = true;
      parentExpr = string("g->") + Option::ClassStaticsObjectPrefix +
        parentName + "->";
    } else {
      parentExpr = string("parent->");
    }
  } else {
    parentExpr = string(Option::ClassPrefix) + parent + "::";
  }
  string invokeName;
  invokeName += staticOnly ? Option::ObjectStaticPrefix : Option::ObjectPrefix;
  switch (type) {
    case Invoke: invokeName += "invoke"; break;
    case Eval: invokeName += "invoke_from_eval"; break;
    case CallInfo:
               invokeName += "get_call_info";
               if (Option::UseMethodIndex) {
                 invokeName += "_with_index";
               }
               break;
    default: ASSERT(false);
  }

  parentExpr += invokeName;
  StringToFunctionScopePtrVecMap flatScopes;
  bool flatten = type == Invoke && Option::FlattenInvoke;
  if (flatten) {
    StringToFunctionScopePtrMap fss;
    collectMethods(ar, fss, true, true);
    for (StringToFunctionScopePtrMap::const_iterator it = fss.begin();
         it != fss.end(); ++it) {
      flatScopes[it->first].push_back(it->second);
    }
  }

  vector<const char *> funcs;
  findJumpTableMethods(cg, ar, type == CallInfo ? false : staticOnly, funcs);

  if (flatten) {
    funcs.clear();
    for (StringToFunctionScopePtrVecMap::const_iterator iter =
           flatScopes.begin(); iter != flatScopes.end(); ++iter) {
      FunctionScopePtr func = iter->second[0];
      if (func->isAbstract() || func->inPseudoMain() ||
          (staticOnly && !func->isStatic()) ||
          !(system || func->isDynamic() || func->isVirtual())) continue;
      funcs.push_back(iter->first.c_str());
    }
  }

  StringToFunctionScopePtrVecMap &funcScopes = flatten ? flatScopes :
    m_functions;

  switch (type) {
    case Invoke:
      // output invoke()
      if (staticOnly) { // os_invoke
        if (funcs.empty()) {
          m_emptyJumpTables.insert(JumpTableStaticInvoke);
        }
        cg.ifdefBegin(false, "OMIT_JUMP_TABLE_CLASS_STATIC_INVOKE_%s", clsName);
        cg_indentBegin("Variant %s%s"
            "(const char *c, const char *s, CArrRef params,"
            " int64 hash, bool fatal) {\n", scope.c_str(),
            invokeName.c_str());
      } else {
        if (funcs.empty()) {
          m_emptyJumpTables.insert(JumpTableInvoke);
        }
        cg.ifdefBegin(false, "OMIT_JUMP_TABLE_CLASS_INVOKE_%s", clsName);
        cg_indentBegin("Variant %s%s"
            "(const char *s, CArrRef params,"
            " int64 hash, bool fatal) {\n", scope.c_str(),
            invokeName.c_str());
      }
      FunctionScope::OutputCPPDynamicInvokeCount(cg);
      break;
    case Eval:
      if (staticOnly) { // os_invoke
        cg_indentBegin("Variant %s%s"
            "(const char *c, const char *s, "
            "Eval::VariableEnvironment &env, "
            "const Eval::FunctionCallExpression *caller, "
            "int64 hash, bool fatal) {\n", scope.c_str(),
            invokeName.c_str());
      } else {
        cg_indentBegin("Variant %s%s"
            "(const char *s, "
            "Eval::VariableEnvironment &env, "
            "const Eval::FunctionCallExpression *caller, "
            "int64 hash, bool fatal) {\n", scope.c_str(),
            invokeName.c_str());
      }
      break;
    case CallInfo:
      cg_indentBegin("bool %s%s(MethodCallPackage &mcp, %sint64 hash) {\n",
          scope.c_str(), invokeName.c_str(),
          Option::UseMethodIndex ? "MethodIndex mi, " : "");
      cg_printf("CStrRef s __attribute__((__unused__)) (mcp.name);\n");
      break;
    default: ASSERT(false);
  }
  if (needGlobals) cg.printDeclareGlobals();
  outputCPPMethodInvokeTable(cg, ar, funcs, funcScopes, false, staticOnly,
                             type);

  string base = parentExpr;
  if (flatten && !needsInvokeParent(ar, false)) {
    base = m_derivesFromRedeclaring ? "c_DynamicObjectData" : "c_ObjectData";
    base += "::" + invokeName;
  }
  switch (type) {
    case Invoke:
      if (staticOnly) {
        cg_printf("return %s(c, s, params, hash, fatal);\n", base.c_str());
        cg_indentEnd("}\n");
        cg.ifdefEnd("OMIT_JUMP_TABLE_CLASS_STATIC_INVOKE_%s", clsName);
      } else {
        cg_printf("return %s(s, params, hash, fatal);\n", base.c_str());
        cg_indentEnd("}\n");
        cg.ifdefEnd("OMIT_JUMP_TABLE_CLASS_INVOKE_%s", clsName);
      }
      break;
    case Eval:
      if (staticOnly) {
        cg_printf("return %s(c, s, env, caller, hash, fatal);\n",
                  parentExpr.c_str());
      } else {
        cg_printf("return %s(s, env, caller, hash, fatal);\n",
                  parentExpr.c_str());
      }
      cg_indentEnd("}\n");
      break;
    case CallInfo:
      cg_printf("return %s(mcp, hash);\n", parentExpr.c_str());
      cg_indentEnd("}\n");
      break;
    default: ASSERT(false);
  }

  if (!staticOnly && type == Invoke) {
    cg.ifdefBegin(false, "OMIT_JUMP_TABLE_CLASS_INVOKE_%s", clsName);
    cg_indentBegin("Variant %s%s_few_args(const char *s, int64 hash, "
                   "int count", scope.c_str(), invokeName.c_str());
    for (int i = 0; i < Option::InvokeFewArgsCount; i++) {
      cg_printf(", CVarRef a%d", i);
    }
    cg_printf(") {\n");
    if (needGlobals) cg.printDeclareGlobals();
    outputCPPMethodInvokeTable(cg, ar, funcs, funcScopes, true, staticOnly,
                               Invoke);
    cg_printf("return %s_few_args(s, hash, count, "
        "INVOKE_FEW_ARGS_PASS_ARGS);\n", base.c_str());
    cg_indentEnd("}\n");
    cg.ifdefEnd("OMIT_JUMP_TABLE_CLASS_INVOKE_%s", clsName);
  }
}

void ClassScope::outputVolatileCheckBegin(CodeGenerator &cg,
                                          AnalysisResultPtr ar,
                                          const std::string &name) {
  if (isVolatile()) {
    OutputVolatileCheckBegin(cg, ar, name);
  }
}
void ClassScope::outputVolatileCheckEnd(CodeGenerator &cg) {
  if (isVolatile()) {
    OutputVolatileCheckEnd(cg);
  }
}

void ClassScope::OutputVolatileCheckBegin(CodeGenerator &cg,
                                          AnalysisResultPtr ar,
                                          const string &origName) {
  cg_printf("((");
  OutputVolatileCheck(cg, ar, origName, false);
  cg_printf("), (");
}

void ClassScope::OutputVolatileCheckEnd(CodeGenerator &cg) {
  cg_printf("))");
}
void ClassScope::OutputVolatileCheck(CodeGenerator &cg, AnalysisResultPtr ar,
    const string &origName, bool noThrow) {
  string lwrName(Util::toLower(origName));
  cg_printf("checkClassExists(");
  cg_printString(origName, ar);
  if (ar->findClass(lwrName)) {
    cg_printf(", &%s->CDEC(%s)",
              cg.getGlobals(ar), cg.formatLabel(lwrName).c_str());
  } else {
    cg_printf(", (bool*)0");
  }
  cg_printf(", %s->FVF(__autoload)%s)", cg.getGlobals(ar),
      noThrow ? ", true" : "");
}

void ClassScope::outputMethodWrappers(CodeGenerator &cg,
                                      AnalysisResultPtr ar) {
  if (!isInterface()) {
    string name = getId(cg);

    FunctionScopePtr constructor = findConstructor(ar, true);
    if (constructor) {
      if (!constructor->isAbstract()) {
        constructor->outputMethodWrapper(cg, ar, name.c_str());
        cg_printf("\n");
      }
    } else {
      cg_indentBegin("static %s%s Create() {\n", Option::SmartPtrPrefix,
                     name.c_str());
      cg_printf("return NEW(%s%s)();\n", Option::ClassPrefix, name.c_str());
      cg_indentEnd("}\n");
      cg_printf("\n");
    }

    ClassScopePtr self = static_pointer_cast<ClassScope>(shared_from_this());
    for (unsigned int i = 0; i < m_functionsVec.size(); i++) {
      FunctionScopePtr func = m_functionsVec[i];
      if (func->isPublic() && !func->isConstructor(self) &&
          !func->isMagic() && !func->isAbstract()) {
        func->outputMethodWrapper(cg, ar, NULL);
      }
    }
  }
}
