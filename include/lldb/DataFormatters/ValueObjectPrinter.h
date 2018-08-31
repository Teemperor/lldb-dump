//===-- ValueObjectPrinter.h ---------------------------------------*- C++
//-*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef lldb_ValueObjectPrinter_h_
#define lldb_ValueObjectPrinter_h_

// C Includes
// C++ Includes

// Other libraries and framework includes
// Project includes
#include "lldb/lldb-private.h"
#include "lldb/lldb-public.h"

#include "lldb/Utility/Flags.h"
#include "lldb/Utility/Lazy.h"

#include "lldb/DataFormatters/DumpValueObjectOptions.h"
#include "lldb/Symbol/CompilerType.h"

namespace lldb_private {

class ValueObjectPrinter {
public:
  ValueObjectPrinter(ValueObject *valobj, Stream *s);

  ValueObjectPrinter(ValueObject *valobj, Stream *s,
                     const DumpValueObjectOptions &options);

  ~ValueObjectPrinter() {}

  bool PrintValueObject();

protected:
  typedef std::set<uint64_t> InstancePointersSet;
  typedef std::shared_ptr<InstancePointersSet> InstancePointersSetSP;

  InstancePointersSetSP m_printed_instance_pointers;

  // only this class (and subclasses, if any) should ever be concerned with the
  // depth mechanism
  ValueObjectPrinter(ValueObject *valobj, Stream *s,
                     const DumpValueObjectOptions &options,
                     const DumpValueObjectOptions::PointerDepth &ptr_depth,
                     uint32_t curr_depth,
                     InstancePointersSetSP printed_instance_pointers);

  // we should actually be using delegating constructors here but some versions
  // of GCC still have trouble with those
  void Init(ValueObject *valobj, Stream *s,
            const DumpValueObjectOptions &options,
            const DumpValueObjectOptions::PointerDepth &ptr_depth,
            uint32_t curr_depth,
            InstancePointersSetSP printed_instance_pointers);

  bool GetMostSpecializedValue();

  const char *GetDescriptionForDisplay();

  const char *GetRootNameForDisplay(const char *if_fail = nullptr);

  bool ShouldPrintValueObject() { return m_should_print.get(*this); }

  bool ShouldPrintValidation();

  bool IsNil() { return m_is_nil.get(*this); }
  bool IsUninitialized() { return m_is_uninit.get(*this); }
  bool IsPtr() { return m_is_ptr.get(*this); }
  bool IsRef() { return m_is_ref.get(*this); }
  bool IsInstancePointer() { return m_is_instance_ptr.get(*this); }
  bool IsAggregate() { return m_is_aggregate.get(*this); }

  bool PrintValidationMarkerIfNeeded();

  bool PrintValidationErrorIfNeeded();

  bool PrintLocationIfNeeded();

  void PrintDecl();

  bool CheckScopeIfNeeded();

  bool ShouldPrintEmptyBrackets(bool value_printed, bool summary_printed);

  TypeSummaryImpl *GetSummaryFormatter(bool null_if_omitted = true);

  void GetValueSummaryError(std::string &value, std::string &summary,
                            std::string &error);

  bool PrintValueAndSummaryIfNeeded(bool &value_printed, bool &summary_printed);

  bool PrintObjectDescriptionIfNeeded(bool value_printed, bool summary_printed);

  bool
  ShouldPrintChildren(bool is_failed_description,
                      DumpValueObjectOptions::PointerDepth &curr_ptr_depth);

  bool ShouldExpandEmptyAggregates();

  ValueObject *GetValueObjectForChildrenGeneration();

  void PrintChildrenPreamble();

  void PrintChildrenPostamble(bool print_dotdotdot);

  lldb::ValueObjectSP GenerateChild(ValueObject *synth_valobj, size_t idx);

  void PrintChild(lldb::ValueObjectSP child_sp,
                  const DumpValueObjectOptions::PointerDepth &curr_ptr_depth);

  uint32_t GetMaxNumChildrenToPrint(bool &print_dotdotdot);

  void
  PrintChildren(bool value_printed, bool summary_printed,
                const DumpValueObjectOptions::PointerDepth &curr_ptr_depth);

  void PrintChildrenIfNeeded(bool value_printed, bool summary_printed);

  bool PrintChildrenOneLiner(bool hide_names);

private:
  ValueObject *m_orig_valobj;
  ValueObject *m_valobj;
  Stream *m_stream;
  DumpValueObjectOptions m_options;
  Flags m_type_flags;
  CompilerType m_compiler_type;
  DumpValueObjectOptions::PointerDepth m_ptr_depth;
  uint32_t m_curr_depth;

  bool UpdateShouldPrint();
  bool UpdateIsNil();
  bool UpdateIsUnit();
  bool UpdateIsPtr();
  bool UpdateIsRef();
  bool UpdateIsAggregate();
  bool UpdateIsInstancePtr();

#define LLDB_VOP ValueObjectPrinter
  LazyBoolMember<LLDB_VOP, &LLDB_VOP::UpdateShouldPrint> m_should_print;
  LazyBoolMember<LLDB_VOP, &LLDB_VOP::UpdateIsNil> m_is_nil;
  LazyBoolMember<LLDB_VOP, &LLDB_VOP::UpdateIsUnit> m_is_uninit;
  LazyBoolMember<LLDB_VOP, &LLDB_VOP::UpdateIsPtr> m_is_ptr;
  LazyBoolMember<LLDB_VOP, &LLDB_VOP::UpdateIsRef> m_is_ref;
  LazyBoolMember<LLDB_VOP, &LLDB_VOP::UpdateIsAggregate> m_is_aggregate;
  LazyBoolMember<LLDB_VOP, &LLDB_VOP::UpdateIsInstancePtr> m_is_instance_ptr;
#undef LLDB_VOP
  std::pair<TypeSummaryImpl *, bool> m_summary_formatter;
  std::string m_value;
  std::string m_summary;
  std::string m_error;
  bool m_val_summary_ok;
  std::pair<TypeValidatorResult, std::string> m_validation;

  friend struct StringSummaryFormat;

  DISALLOW_COPY_AND_ASSIGN(ValueObjectPrinter);
};

} // namespace lldb_private

#endif // lldb_ValueObjectPrinter_h_
