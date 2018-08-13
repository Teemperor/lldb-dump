//===-- ClangExpressionParser.cpp -------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

// C Includes
// C++ Includes
// Other libraries and framework includes
#include "clang/AST/ASTContext.h"
#include "clang/AST/ASTDiagnostic.h"
#include "clang/AST/ExternalASTSource.h"
#include "clang/Basic/DiagnosticIDs.h"
#include "clang/Basic/FileManager.h"
#include "clang/Basic/SourceLocation.h"
#include "clang/Basic/TargetInfo.h"
#include "clang/Basic/Version.h"
#include "clang/CodeGen/CodeGenAction.h"
#include "clang/CodeGen/ModuleBuilder.h"
#include "clang/Edit/Commit.h"
#include "clang/Edit/EditedSource.h"
#include "clang/Edit/EditsReceiver.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/CompilerInvocation.h"
#include "clang/Frontend/FrontendActions.h"
#include "clang/Frontend/FrontendDiagnostic.h"
#include "clang/Frontend/FrontendPluginRegistry.h"
#include "clang/Frontend/TextDiagnosticBuffer.h"
#include "clang/Frontend/TextDiagnosticPrinter.h"
#include "clang/Lex/Preprocessor.h"
#include "clang/Parse/ParseAST.h"
#include "clang/Rewrite/Core/Rewriter.h"
#include "clang/Rewrite/Frontend/FrontendActions.h"
#include "clang/Sema/Sema.h"
#include "clang/Sema/Lookup.h"
#include "clang/Sema/MultiplexExternalSemaSource.h"
#include "clang/Sema/SemaConsumer.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/ExecutionEngine/ExecutionEngine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/CrashRecoveryContext.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/TargetSelect.h"

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wglobal-constructors"
#include "llvm/ExecutionEngine/MCJIT.h"
#pragma clang diagnostic pop

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/DynamicLibrary.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/Host.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Signals.h"

// Project includes
#include "ClangDiagnostic.h"
#include "ClangExpressionParser.h"

#include "ClangASTSource.h"
#include "ClangExpressionDeclMap.h"
#include "ClangExpressionHelper.h"
#include "ClangModulesDeclVendor.h"
#include "ClangPersistentVariables.h"
#include "IRForTarget.h"

#include "lldb/Core/Debugger.h"
#include "lldb/Core/Disassembler.h"
#include "lldb/Core/Module.h"
#include "lldb/Core/StreamFile.h"
#include "lldb/Expression/IRDynamicChecks.h"
#include "lldb/Expression/IRExecutionUnit.h"
#include "lldb/Expression/IRInterpreter.h"
#include "lldb/Host/File.h"
#include "lldb/Host/HostInfo.h"
#include "lldb/Symbol/ClangASTContext.h"
#include "lldb/Symbol/SymbolVendor.h"
#include "lldb/Target/ExecutionContext.h"
#include "lldb/Target/Language.h"
#include "lldb/Target/ObjCLanguageRuntime.h"
#include "lldb/Target/Process.h"
#include "lldb/Target/Target.h"
#include "lldb/Target/ThreadPlanCallFunction.h"
#include "lldb/Utility/DataBufferHeap.h"
#include "lldb/Utility/LLDBAssert.h"
#include "lldb/Utility/Log.h"
#include "lldb/Utility/Stream.h"
#include "lldb/Utility/StreamString.h"
#include "lldb/Utility/StringList.h"

using namespace clang;
using namespace llvm;
using namespace lldb_private;

//===----------------------------------------------------------------------===//
// Utility Methods for Clang
//===----------------------------------------------------------------------===//

class ClangExpressionParser::LLDBPreprocessorCallbacks : public PPCallbacks {
  ClangModulesDeclVendor &m_decl_vendor;
  ClangPersistentVariables &m_persistent_vars;
  StreamString m_error_stream;
  bool m_has_errors = false;

public:
  LLDBPreprocessorCallbacks(ClangModulesDeclVendor &decl_vendor,
                            ClangPersistentVariables &persistent_vars)
      : m_decl_vendor(decl_vendor), m_persistent_vars(persistent_vars) {}

  void moduleImport(SourceLocation import_location, clang::ModuleIdPath path,
                    const clang::Module * /*null*/) override {
    std::vector<ConstString> string_path;

    for (const std::pair<IdentifierInfo *, SourceLocation> &component : path) {
      string_path.push_back(ConstString(component.first->getName()));
    }

    StreamString error_stream;

    ClangModulesDeclVendor::ModuleVector exported_modules;

    if (!m_decl_vendor.AddModule(string_path, &exported_modules,
                                 m_error_stream)) {
      m_has_errors = true;
    }

    for (ClangModulesDeclVendor::ModuleID module : exported_modules) {
      m_persistent_vars.AddHandLoadedClangModule(module);
    }
  }

  bool hasErrors() { return m_has_errors; }

  llvm::StringRef getErrorString() { return m_error_stream.GetString(); }
};

class ClangDiagnosticManagerAdapter : public clang::DiagnosticConsumer {
public:
  ClangDiagnosticManagerAdapter()
      : m_passthrough(new clang::TextDiagnosticBuffer) {}

  ClangDiagnosticManagerAdapter(
      const std::shared_ptr<clang::TextDiagnosticBuffer> &passthrough)
      : m_passthrough(passthrough) {}

  void ResetManager(DiagnosticManager *manager = nullptr) {
    m_manager = manager;
  }

  void HandleDiagnostic(DiagnosticsEngine::Level DiagLevel,
                        const clang::Diagnostic &Info) {
    if (m_manager) {
      llvm::SmallVector<char, 32> diag_str;
      Info.FormatDiagnostic(diag_str);
      diag_str.push_back('\0');
      const char *data = diag_str.data();

      lldb_private::DiagnosticSeverity severity;
      bool make_new_diagnostic = true;

      switch (DiagLevel) {
      case DiagnosticsEngine::Level::Fatal:
      case DiagnosticsEngine::Level::Error:
        severity = eDiagnosticSeverityError;
        break;
      case DiagnosticsEngine::Level::Warning:
        severity = eDiagnosticSeverityWarning;
        break;
      case DiagnosticsEngine::Level::Remark:
      case DiagnosticsEngine::Level::Ignored:
        severity = eDiagnosticSeverityRemark;
        break;
      case DiagnosticsEngine::Level::Note:
        m_manager->AppendMessageToDiagnostic(data);
        make_new_diagnostic = false;
      }
      if (make_new_diagnostic) {
        ClangDiagnostic *new_diagnostic =
            new ClangDiagnostic(data, severity, Info.getID());
        m_manager->AddDiagnostic(new_diagnostic);

        // Don't store away warning fixits, since the compiler doesn't have
        // enough context in an expression for the warning to be useful.
        // FIXME: Should we try to filter out FixIts that apply to our generated
        // code, and not the user's expression?
        if (severity == eDiagnosticSeverityError) {
          size_t num_fixit_hints = Info.getNumFixItHints();
          for (size_t i = 0; i < num_fixit_hints; i++) {
            const clang::FixItHint &fixit = Info.getFixItHint(i);
            if (!fixit.isNull())
              new_diagnostic->AddFixitHint(fixit);
          }
        }
      }
    }

    m_passthrough->HandleDiagnostic(DiagLevel, Info);
  }

  void FlushDiagnostics(DiagnosticsEngine &Diags) {
    m_passthrough->FlushDiagnostics(Diags);
  }

  DiagnosticConsumer *clone(DiagnosticsEngine &Diags) const {
    return new ClangDiagnosticManagerAdapter(m_passthrough);
  }

  clang::TextDiagnosticBuffer *GetPassthrough() { return m_passthrough.get(); }

private:
  DiagnosticManager *m_manager = nullptr;
  std::shared_ptr<clang::TextDiagnosticBuffer> m_passthrough;
};


/// \brief wraps an ExternalASTSource in an ExternalSemaSource. No functional
/// difference between the original source and this wrapper intended.
class ExternalASTSourceWrapper : public ExternalSemaSource {
  ExternalASTSource* m_Source;

public:
  ExternalASTSourceWrapper(ExternalASTSource* Source) : m_Source(Source) {
    assert(m_Source && "Can't wrap nullptr ExternalASTSource");
  }

  virtual Decl* GetExternalDecl(uint32_t ID) override {
    return m_Source->GetExternalDecl(ID);
  }

  virtual Selector GetExternalSelector(uint32_t ID) override {
    return m_Source->GetExternalSelector(ID);
  }

  virtual uint32_t GetNumExternalSelectors() override {
    return m_Source->GetNumExternalSelectors();
  }

  virtual Stmt* GetExternalDeclStmt(uint64_t Offset) override {
    return m_Source->GetExternalDeclStmt(Offset);
  }

  virtual CXXCtorInitializer**
  GetExternalCXXCtorInitializers(uint64_t Offset) override {
    return m_Source->GetExternalCXXCtorInitializers(Offset);
  }

  virtual CXXBaseSpecifier*
  GetExternalCXXBaseSpecifiers(uint64_t Offset) override {
    return m_Source->GetExternalCXXBaseSpecifiers(Offset);
  }

  virtual void updateOutOfDateIdentifier(IdentifierInfo& II) override {
    m_Source->updateOutOfDateIdentifier(II);
  }

  virtual bool FindExternalVisibleDeclsByName(const DeclContext* DC,
                                              DeclarationName Name) override {
    return m_Source->FindExternalVisibleDeclsByName(DC, Name);
  }

  virtual void completeVisibleDeclsMap(const DeclContext* DC) override {
    m_Source->completeVisibleDeclsMap(DC);
  }

  virtual clang::Module* getModule(unsigned ID) override {
    return m_Source->getModule(ID);
  }

  virtual llvm::Optional<ASTSourceDescriptor>
  getSourceDescriptor(unsigned ID) override {
    return m_Source->getSourceDescriptor(ID);
  }

  virtual ExtKind hasExternalDefinitions(const Decl* D) override {
    return m_Source->hasExternalDefinitions(D);
  }

  virtual void
  FindExternalLexicalDecls(const DeclContext* DC,
                           llvm::function_ref<bool(Decl::Kind)> IsKindWeWant,
                           SmallVectorImpl<Decl*>& Result) override {
    m_Source->FindExternalLexicalDecls(DC, IsKindWeWant, Result);
  }

  virtual void FindFileRegionDecls(FileID File, unsigned Offset,
                                   unsigned Length,
                                   SmallVectorImpl<Decl*>& Decls) override {
    m_Source->FindFileRegionDecls(File, Offset, Length, Decls);
  }

  virtual void CompleteRedeclChain(const Decl* D) override {
    m_Source->CompleteRedeclChain(D);
  }

  virtual void CompleteType(TagDecl* Tag) override {
    m_Source->CompleteType(Tag);
  }

  virtual void CompleteType(ObjCInterfaceDecl* Class) override {
    m_Source->CompleteType(Class);
  }

  virtual void ReadComments() override { m_Source->ReadComments(); }

  virtual void StartedDeserializing() override {
    m_Source->StartedDeserializing();
  }

  virtual void FinishedDeserializing() override {
    m_Source->FinishedDeserializing();
  }

  virtual void StartTranslationUnit(ASTConsumer* Consumer) override {
    m_Source->StartTranslationUnit(Consumer);
  }

  virtual void PrintStats() override { m_Source->PrintStats(); }

  virtual bool layoutRecordType(
      const RecordDecl* Record, uint64_t& Size, uint64_t& Alignment,
      llvm::DenseMap<const FieldDecl*, uint64_t>& FieldOffsets,
      llvm::DenseMap<const CXXRecordDecl*, CharUnits>& BaseOffsets,
      llvm::DenseMap<const CXXRecordDecl*, CharUnits>& VirtualBaseOffsets)
      override {
    return m_Source->layoutRecordType(Record, Size, Alignment, FieldOffsets,
                                      BaseOffsets, VirtualBaseOffsets);
  }
};

//===----------------------------------------------------------------------===//
// Implementation of ClangExpressionParser
//===----------------------------------------------------------------------===//

ClangExpressionParser::ClangExpressionParser(ExecutionContextScope *exe_scope,
                                             Expression &expr,
                                             bool generate_debug_info)
    : ExpressionParser(exe_scope, expr, generate_debug_info), m_compiler(),
      m_code_generator(), m_pp_callbacks(nullptr) {
  Log *log(lldb_private::GetLogIfAllCategoriesSet(LIBLLDB_LOG_EXPRESSIONS));

  // We can't compile expressions without a target.  So if the exe_scope is
  // null or doesn't have a target, then we just need to get out of here.  I'll
  // lldb_assert and not make any of the compiler objects since
  // I can't return errors directly from the constructor.  Further calls will
  // check if the compiler was made and
  // bag out if it wasn't.

  if (!exe_scope) {
    lldb_assert(exe_scope, "Can't make an expression parser with a null scope.",
                __FUNCTION__, __FILE__, __LINE__);
    return;
  }

  lldb::TargetSP target_sp;
  target_sp = exe_scope->CalculateTarget();
  if (!target_sp) {
    lldb_assert(target_sp.get(),
                "Can't make an expression parser with a null target.",
                __FUNCTION__, __FILE__, __LINE__);
    return;
  }

  // 1. Create a new compiler instance.
  m_compiler.reset(new CompilerInstance());
  lldb::LanguageType frame_lang =
      expr.Language(); // defaults to lldb::eLanguageTypeUnknown
  bool overridden_target_opts = false;
  lldb_private::LanguageRuntime *lang_rt = nullptr;

  std::string abi;
  ArchSpec target_arch;
  target_arch = target_sp->GetArchitecture();

  const auto target_machine = target_arch.GetMachine();

  // If the expression is being evaluated in the context of an existing stack
  // frame, we introspect to see if the language runtime is available.

  lldb::StackFrameSP frame_sp = exe_scope->CalculateStackFrame();
  lldb::ProcessSP process_sp = exe_scope->CalculateProcess();

  // Make sure the user hasn't provided a preferred execution language with
  // `expression --language X -- ...`
  if (frame_sp && frame_lang == lldb::eLanguageTypeUnknown)
    frame_lang = frame_sp->GetLanguage();

  if (process_sp && frame_lang != lldb::eLanguageTypeUnknown) {
    lang_rt = process_sp->GetLanguageRuntime(frame_lang);
    if (log)
      log->Printf("Frame has language of type %s",
                  Language::GetNameForLanguageType(frame_lang));
  }

  // 2. Configure the compiler with a set of default options that are
  // appropriate for most situations.
  if (target_arch.IsValid()) {
    std::string triple = target_arch.GetTriple().str();
    m_compiler->getTargetOpts().Triple = triple;
    if (log)
      log->Printf("Using %s as the target triple",
                  m_compiler->getTargetOpts().Triple.c_str());
  } else {
    // If we get here we don't have a valid target and just have to guess.
    // Sometimes this will be ok to just use the host target triple (when we
    // evaluate say "2+3", but other expressions like breakpoint conditions and
    // other things that _are_ target specific really shouldn't just be using
    // the host triple. In such a case the language runtime should expose an
    // overridden options set (3), below.
    m_compiler->getTargetOpts().Triple = llvm::sys::getDefaultTargetTriple();
    if (log)
      log->Printf("Using default target triple of %s",
                  m_compiler->getTargetOpts().Triple.c_str());
  }
  // Now add some special fixes for known architectures: Any arm32 iOS
  // environment, but not on arm64
  if (m_compiler->getTargetOpts().Triple.find("arm64") == std::string::npos &&
      m_compiler->getTargetOpts().Triple.find("arm") != std::string::npos &&
      m_compiler->getTargetOpts().Triple.find("ios") != std::string::npos) {
    m_compiler->getTargetOpts().ABI = "apcs-gnu";
  }
  // Supported subsets of x86
  if (target_machine == llvm::Triple::x86 ||
      target_machine == llvm::Triple::x86_64) {
    m_compiler->getTargetOpts().Features.push_back("+sse");
    m_compiler->getTargetOpts().Features.push_back("+sse2");
  }

  // Set the target CPU to generate code for. This will be empty for any CPU
  // that doesn't really need to make a special
  // CPU string.
  m_compiler->getTargetOpts().CPU = target_arch.GetClangTargetCPU();

  // Set the target ABI
  abi = GetClangTargetABI(target_arch);
  if (!abi.empty())
    m_compiler->getTargetOpts().ABI = abi;

  // 3. Now allow the runtime to provide custom configuration options for the
  // target. In this case, a specialized language runtime is available and we
  // can query it for extra options. For 99% of use cases, this will not be
  // needed and should be provided when basic platform detection is not enough.
  if (lang_rt)
    overridden_target_opts =
        lang_rt->GetOverrideExprOptions(m_compiler->getTargetOpts());

  if (overridden_target_opts)
    if (log && log->GetVerbose()) {
      LLDB_LOGV(
          log, "Using overridden target options for the expression evaluation");

      auto opts = m_compiler->getTargetOpts();
      LLDB_LOGV(log, "Triple: '{0}'", opts.Triple);
      LLDB_LOGV(log, "CPU: '{0}'", opts.CPU);
      LLDB_LOGV(log, "FPMath: '{0}'", opts.FPMath);
      LLDB_LOGV(log, "ABI: '{0}'", opts.ABI);
      LLDB_LOGV(log, "LinkerVersion: '{0}'", opts.LinkerVersion);
      StringList::LogDump(log, opts.FeaturesAsWritten, "FeaturesAsWritten");
      StringList::LogDump(log, opts.Features, "Features");
    }

  // 4. Create and install the target on the compiler.
  m_compiler->createDiagnostics();
  auto target_info = TargetInfo::CreateTargetInfo(
      m_compiler->getDiagnostics(), m_compiler->getInvocation().TargetOpts);
  if (log) {
    log->Printf("Using SIMD alignment: %d", target_info->getSimdDefaultAlign());
    log->Printf("Target datalayout string: '%s'",
                target_info->getDataLayout().getStringRepresentation().c_str());
    log->Printf("Target ABI: '%s'", target_info->getABI().str().c_str());
    log->Printf("Target vector alignment: %d",
                target_info->getMaxVectorAlign());
  }
  m_compiler->setTarget(target_info);

  assert(m_compiler->hasTarget());

  // 5. Set language options.
  lldb::LanguageType language = expr.Language();

  m_compiler->getLangOpts().Modules = true;
  // makes asserts very unhappy: m_compiler->getLangOpts().ModulesTS = true;
  switch (language) {
  case lldb::eLanguageTypeC:
  case lldb::eLanguageTypeC89:
  case lldb::eLanguageTypeC99:
  case lldb::eLanguageTypeC11:
    // FIXME: the following language option is a temporary workaround,
    // to "ask for C, get C++."
    // For now, the expression parser must use C++ anytime the language is a C
    // family language, because the expression parser uses features of C++ to
    // capture values.
    //m_compiler->getLangOpts().CPlusPlus = true;
    break;
  case lldb::eLanguageTypeObjC:
    //m_compiler->getLangOpts().ObjC1 = true;
    //m_compiler->getLangOpts().ObjC2 = true;
    // FIXME: the following language option is a temporary workaround,
    // to "ask for ObjC, get ObjC++" (see comment above).
    //m_compiler->getLangOpts().CPlusPlus = true;

    // Clang now sets as default C++14 as the default standard (with
    // GNU extensions), so we do the same here to avoid mismatches that
    // cause compiler error when evaluating expressions (e.g. nullptr not found
    // as it's a C++11 feature). Currently lldb evaluates C++14 as C++11 (see
    // two lines below) so we decide to be consistent with that, but this could
    // be re-evaluated in the future.
    //m_compiler->getLangOpts().CPlusPlus11 = true;
    break;
  case lldb::eLanguageTypeC_plus_plus:
  case lldb::eLanguageTypeC_plus_plus_11:
  case lldb::eLanguageTypeC_plus_plus_14:
    //m_compiler->getLangOpts().CPlusPlus11 = true;
    m_compiler->getHeaderSearchOpts().UseLibcxx = true;
    LLVM_FALLTHROUGH;
  case lldb::eLanguageTypeC_plus_plus_03:
    //m_compiler->getLangOpts().CPlusPlus = true;
    // FIXME: the following language option is a temporary workaround,
    // to "ask for C++, get ObjC++".  Apple hopes to remove this requirement on
    // non-Apple platforms, but for now it is needed.
    //m_compiler->getLangOpts().ObjC1 = true;
    break;
  case lldb::eLanguageTypeObjC_plus_plus:
  case lldb::eLanguageTypeUnknown:
  default:
    break;
  }

  m_compiler->getLangOpts().ObjC1 = true;
  m_compiler->getLangOpts().ObjC2 = true;
  m_compiler->getLangOpts().CPlusPlus = true;
  m_compiler->getLangOpts().GNUMode = true;
  m_compiler->getLangOpts().GNUKeywords = true;
  m_compiler->getLangOpts().NoBuiltin = false;
  m_compiler->getLangOpts().DoubleSquareBracketAttributes = true;
  m_compiler->getLangOpts().CPlusPlus11 = true;
  m_compiler->getHeaderSearchOpts().ModuleCachePath = "/tmp/org.llvm.lldb.cache/";
  m_compiler->getHeaderSearchOpts().UseLibcxx = true;
  m_compiler->getHeaderSearchOpts().ImplicitModuleMaps = true;

  m_compiler->getHeaderSearchOpts().ResourceDir = "/Users/teemperor/llvm/sidestuff/build/lib/clang/7.0.0";

  m_compiler->getHeaderSearchOpts().AddPath("/Users/teemperor/llvm/sidestuff/build/include/c++/v1/",
                                            clang::frontend::IncludeDirGroup::System, false, true);

  m_compiler->getHeaderSearchOpts().AddPath("/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX10.13.sdk/usr/include/",
                                            clang::frontend::IncludeDirGroup::ExternCSystem, false, true);


  m_compiler->getLangOpts().ImplicitModules = true;
  //m_compiler->getLangOpts().ModulesLocalVisibility = true;

  m_compiler->getLangOpts().Bool = true;
  m_compiler->getLangOpts().WChar = true;
  m_compiler->getLangOpts().Blocks = true;
  m_compiler->getLangOpts().DebuggerSupport =
      true; // Features specifically for debugger clients
  if (expr.DesiredResultType() == Expression::eResultTypeId)
    m_compiler->getLangOpts().DebuggerCastResultToId = true;

  m_compiler->getLangOpts().CharIsSigned =
      ArchSpec(m_compiler->getTargetOpts().Triple.c_str())
          .CharIsSignedByDefault();

  // Spell checking is a nice feature, but it ends up completing a lot of types
  // that we didn't strictly speaking need to complete. As a result, we spend a
  // long time parsing and importing debug information.
  m_compiler->getLangOpts().SpellChecking = false;

  if (process_sp && m_compiler->getLangOpts().ObjC1) {
    if (process_sp->GetObjCLanguageRuntime()) {
      if (process_sp->GetObjCLanguageRuntime()->GetRuntimeVersion() ==
          ObjCLanguageRuntime::ObjCRuntimeVersions::eAppleObjC_V2)
        m_compiler->getLangOpts().ObjCRuntime.set(ObjCRuntime::MacOSX,
                                                  VersionTuple(10, 7));
      else
        m_compiler->getLangOpts().ObjCRuntime.set(ObjCRuntime::FragileMacOSX,
                                                  VersionTuple(10, 7));

      if (process_sp->GetObjCLanguageRuntime()->HasNewLiteralsAndIndexing())
        m_compiler->getLangOpts().DebuggerObjCLiteral = true;
    }
  }

  m_compiler->getLangOpts().ThreadsafeStatics = false;
  m_compiler->getLangOpts().AccessControl =
      false; // Debuggers get universal access
  m_compiler->getLangOpts().DollarIdents =
      true; // $ indicates a persistent variable name

  // Set CodeGen options
  m_compiler->getCodeGenOpts().EmitDeclMetadata = true;
  m_compiler->getCodeGenOpts().InstrumentFunctions = false;
  m_compiler->getCodeGenOpts().DisableFPElim = true;
  m_compiler->getCodeGenOpts().OmitLeafFramePointer = false;
  if (generate_debug_info)
    m_compiler->getCodeGenOpts().setDebugInfo(codegenoptions::FullDebugInfo);
  else
    m_compiler->getCodeGenOpts().setDebugInfo(codegenoptions::NoDebugInfo);

  // Disable some warnings.
  m_compiler->getDiagnostics().setSeverityForGroup(
      clang::diag::Flavor::WarningOrError, "unused-value",
      clang::diag::Severity::Ignored, SourceLocation());
  m_compiler->getDiagnostics().setSeverityForGroup(
      clang::diag::Flavor::WarningOrError, "odr",
      clang::diag::Severity::Ignored, SourceLocation());

  // Inform the target of the language options
  //
  // FIXME: We shouldn't need to do this, the target should be immutable once
  // created. This complexity should be lifted elsewhere.
  m_compiler->getTarget().adjust(m_compiler->getLangOpts());

  // 6. Set up the diagnostic buffer for reporting errors

  m_compiler->getDiagnostics().setClient(new ClangDiagnosticManagerAdapter);

  // 7. Set up the source management objects inside the compiler

  clang::FileSystemOptions file_system_options;
  m_file_manager.reset(new clang::FileManager(file_system_options));

  if (!m_compiler->hasSourceManager())
    m_compiler->createSourceManager(*m_file_manager.get());

  m_compiler->createFileManager();
  m_compiler->createPreprocessor(TU_Complete);

  if (ClangModulesDeclVendor *decl_vendor =
          target_sp->GetClangModulesDeclVendor()) {
    ClangPersistentVariables *clang_persistent_vars =
        llvm::cast<ClangPersistentVariables>(
            target_sp->GetPersistentExpressionStateForLanguage(
                lldb::eLanguageTypeC));
    std::unique_ptr<PPCallbacks> pp_callbacks(
        new LLDBPreprocessorCallbacks(*decl_vendor, *clang_persistent_vars));
    m_pp_callbacks =
        static_cast<LLDBPreprocessorCallbacks *>(pp_callbacks.get());
    m_compiler->getPreprocessor().addPPCallbacks(std::move(pp_callbacks));
  }

  // 8. Most of this we get from the CompilerInstance, but we also want to give
  // the context an ExternalASTSource.
  m_selector_table.reset(new SelectorTable());

  std::unique_ptr<clang::ASTContext> ast_context(
      new ASTContext(m_compiler->getLangOpts(), m_compiler->getSourceManager(),
                     m_compiler->getPreprocessor().getIdentifierTable(),
                     *m_selector_table.get(), m_compiler->getPreprocessor().getBuiltinInfo()));

  ast_context->InitBuiltinTypes(m_compiler->getTarget());

  m_compiler->setASTContext(ast_context.get());

  ast_context.release();
}

ClangExpressionParser::~ClangExpressionParser() {}
namespace {

  /// ASTConsumer - This is an abstract interface that should be implemented by
  /// clients that read ASTs.  This abstraction layer allows the client to be
  /// independent of the AST producer (e.g. parser vs AST dump file reader, etc).
  class ASTConsumerForwarder : public clang::ASTConsumer {
    clang::ASTConsumer *m_c;

  public:
    ASTConsumerForwarder(clang::ASTConsumer *c) : m_c(c) { }

    virtual ~ASTConsumerForwarder() {}

    virtual void Initialize(ASTContext &Context) { m_c->Initialize(Context); }

    virtual bool HandleTopLevelDecl(DeclGroupRef D) {
      return m_c->HandleTopLevelDecl(D);
    }

    virtual void HandleInlineFunctionDefinition(FunctionDecl *D) {
      m_c->HandleInlineFunctionDefinition(D);
    }

    virtual void HandleInterestingDecl(DeclGroupRef D) {
      m_c->HandleInterestingDecl(D);
    }

    virtual void HandleTranslationUnit(ASTContext &Ctx) {
      m_c->HandleTranslationUnit(Ctx);
    }

    virtual void HandleTagDeclDefinition(TagDecl *D) {
      m_c->HandleTagDeclDefinition(D);
    }

    virtual void HandleTagDeclRequiredDefinition(const TagDecl *D) {
      m_c->HandleTagDeclRequiredDefinition(D);
    }

    virtual void HandleCXXImplicitFunctionInstantiation(FunctionDecl *D) {
      m_c->HandleCXXImplicitFunctionInstantiation(D);
    }

    virtual void HandleTopLevelDeclInObjCContainer(DeclGroupRef D) {
      m_c->HandleTopLevelDeclInObjCContainer(D);
    }

    virtual void HandleImplicitImportDecl(ImportDecl *D) {
      m_c->HandleImplicitImportDecl(D);
    }

    virtual void CompleteTentativeDefinition(VarDecl *D) {
      m_c->CompleteTentativeDefinition(D);
    }

    virtual void AssignInheritanceModel(CXXRecordDecl *RD) {
      m_c->AssignInheritanceModel(RD);
    }

    virtual void HandleCXXStaticMemberVarInstantiation(VarDecl *D) {
      m_c->HandleCXXStaticMemberVarInstantiation(D);
    }

    virtual void HandleVTable(CXXRecordDecl *RD) {
      m_c->HandleVTable(RD);
    }

    virtual ASTMutationListener *GetASTMutationListener() { return m_c->GetASTMutationListener(); }

    virtual ASTDeserializationListener *GetASTDeserializationListener() {
      return m_c->GetASTDeserializationListener();
    }

    virtual void PrintStats() {
      m_c->PrintStats();
    }

    virtual bool shouldSkipFunctionBody(Decl *D) {
      return m_c->shouldSkipFunctionBody(D);
    }
  };
}

namespace {

/// An abstract interface that should be implemented by
/// external AST sources that also provide information for semantic
/// analysis.
class MyMultiplexExternalSemaSource : public ExternalSemaSource {

private:
  SmallVector<ExternalSemaSource *, 2> Sources; // doesn't own them.

public:

  ///Constructs a new multiplexing external sema source and appends the
  /// given element to it.
  ///
  ///\param[in] s1 - A non-null (old) ExternalSemaSource.
  ///\param[in] s2 - A non-null (new) ExternalSemaSource.
  ///
  MyMultiplexExternalSemaSource(ExternalSemaSource& s1, ExternalSemaSource& s2){
    Sources.push_back(&s1);
    Sources.push_back(&s2);
  }

  ~MyMultiplexExternalSemaSource() override {}

  ///Appends new source to the source list.
  ///
  ///\param[in] source - An ExternalSemaSource.
  ///
  void addSource(ExternalSemaSource &source) {
    Sources.push_back(&source);
  }

  //===--------------------------------------------------------------------===//
  // ExternalASTSource.
  //===--------------------------------------------------------------------===//

  /// Resolve a declaration ID into a declaration, potentially
  /// building a new declaration.
  Decl *GetExternalDecl(uint32_t ID) override {
    for(size_t i = 0; i < Sources.size(); ++i)
      if (Decl *Result = Sources[i]->GetExternalDecl(ID))
        return Result;
    return nullptr;
  }


  /// Complete the redeclaration chain if it's been extended since the
  /// previous generation of the AST source.
  void CompleteRedeclChain(const Decl *D) override {
    for (size_t i = 0; i < Sources.size(); ++i)
      Sources[i]->CompleteRedeclChain(D);
  }

  /// Resolve a selector ID into a selector.
  Selector GetExternalSelector(uint32_t ID) override {
    Selector Sel;
    for(size_t i = 0; i < Sources.size(); ++i) {
      Sel = Sources[i]->GetExternalSelector(ID);
      if (!Sel.isNull())
        return Sel;
    }
    return Sel;
  }

  /// Returns the number of selectors known to the external AST
  /// source.
  uint32_t GetNumExternalSelectors() override {
    uint32_t total = 0;
    for(size_t i = 0; i < Sources.size(); ++i)
      total += Sources[i]->GetNumExternalSelectors();
    return total;
  }

  /// Resolve the offset of a statement in the decl stream into
  /// a statement.
  Stmt *GetExternalDeclStmt(uint64_t Offset) override{
    for(size_t i = 0; i < Sources.size(); ++i)
      if (Stmt *Result = Sources[i]->GetExternalDeclStmt(Offset))
        return Result;
    return nullptr;
  }

  /// Resolve the offset of a set of C++ base specifiers in the decl
  /// stream into an array of specifiers.
  CXXBaseSpecifier *GetExternalCXXBaseSpecifiers(uint64_t Offset) override{
    for(size_t i = 0; i < Sources.size(); ++i)
      if (CXXBaseSpecifier *R = Sources[i]->GetExternalCXXBaseSpecifiers(Offset))
        return R;
    return nullptr;
  }

  /// Resolve a handle to a list of ctor initializers into the list of
  /// initializers themselves.
  CXXCtorInitializer **GetExternalCXXCtorInitializers(uint64_t Offset) override {
    for (auto *S : Sources)
      if (auto *R = S->GetExternalCXXCtorInitializers(Offset))
        return R;
    return nullptr;
  }

  ExtKind hasExternalDefinitions(const Decl *D) override {
    for (const auto &S : Sources)
      if (auto EK = S->hasExternalDefinitions(D))
        if (EK != EK_ReplyHazy)
          return EK;
    return EK_ReplyHazy;
  }

  /// Find all declarations with the given name in the
  /// given context.
  bool FindExternalVisibleDeclsByName(const DeclContext *DC,
                                      DeclarationName Name) override {
    //bool AnyDeclsFound = false;
    for (size_t i = 0; i < Sources.size(); ++i)
      if (Sources[i]->FindExternalVisibleDeclsByName(DC, Name))
        return true;
    return false;
  }

  /// Ensures that the table of all visible declarations inside this
  /// context is up to date.
  void completeVisibleDeclsMap(const DeclContext *DC) override{
    for(size_t i = 0; i < Sources.size(); ++i)
      Sources[i]->completeVisibleDeclsMap(DC);
  }

  /// Finds all declarations lexically contained within the given
  /// DeclContext, after applying an optional filter predicate.
  ///
  /// \param IsKindWeWant a predicate function that returns true if the passed
  /// declaration kind is one we are looking for.
  void
  FindExternalLexicalDecls(const DeclContext *DC,
                           llvm::function_ref<bool(Decl::Kind)> IsKindWeWant,
                           SmallVectorImpl<Decl *> &Result) override {
    for(size_t i = 0; i < Sources.size(); ++i)
      Sources[i]->FindExternalLexicalDecls(DC, IsKindWeWant, Result);
  }


  /// Get the decls that are contained in a file in the Offset/Length
  /// range. \p Length can be 0 to indicate a point at \p Offset instead of
  /// a range.
  void FindFileRegionDecls(FileID File, unsigned Offset,unsigned Length,
                           SmallVectorImpl<Decl *> &Decls) override{
    for(size_t i = 0; i < Sources.size(); ++i)
      Sources[i]->FindFileRegionDecls(File, Offset, Length, Decls);
  }

  /// Gives the external AST source an opportunity to complete
  /// an incomplete type.
  void CompleteType(TagDecl *Tag) override {
    for(size_t i = 0; i < Sources.size(); ++i)
      while (!Tag->isCompleteDefinition())
        Sources[i]->CompleteType(Tag);
  }

  /// Gives the external AST source an opportunity to complete an
  /// incomplete Objective-C class.
  ///
  /// This routine will only be invoked if the "externally completed" bit is
  /// set on the ObjCInterfaceDecl via the function
  /// \c ObjCInterfaceDecl::setExternallyCompleted().
  void CompleteType(ObjCInterfaceDecl *Class) override {
    for(size_t i = 0; i < Sources.size(); ++i)
      Sources[i]->CompleteType(Class);
  }

  /// Loads comment ranges.
  void ReadComments() override {
    for(size_t i = 0; i < Sources.size(); ++i)
      Sources[i]->ReadComments();
  }

  /// Notify ExternalASTSource that we started deserialization of
  /// a decl or type so until FinishedDeserializing is called there may be
  /// decls that are initializing. Must be paired with FinishedDeserializing.
  void StartedDeserializing() override {
    for(size_t i = 0; i < Sources.size(); ++i)
      Sources[i]->StartedDeserializing();
  }

  /// Notify ExternalASTSource that we finished the deserialization of
  /// a decl or type. Must be paired with StartedDeserializing.
  void FinishedDeserializing() override {
    for(size_t i = 0; i < Sources.size(); ++i)
      Sources[i]->FinishedDeserializing();
  }

  /// Function that will be invoked when we begin parsing a new
  /// translation unit involving this external AST source.
  void StartTranslationUnit(ASTConsumer *Consumer) override {
    for(size_t i = 0; i < Sources.size(); ++i)
      Sources[i]->StartTranslationUnit(Consumer);
  }

  /// Print any statistics that have been gathered regarding
  /// the external AST source.
  void PrintStats() override {
    for(size_t i = 0; i < Sources.size(); ++i)
      Sources[i]->PrintStats();
  }

  /// Retrieve the module that corresponds to the given module ID.
  clang::Module *getModule(unsigned ID) override {
    for (size_t i = 0; i < Sources.size(); ++i)
      if (auto M = Sources[i]->getModule(ID))
        return M;
    return nullptr;
  }

  bool DeclIsFromPCHWithObjectFile(const Decl *D) override {
    for (auto *S : Sources)
      if (S->DeclIsFromPCHWithObjectFile(D))
        return true;
    return false;
  }


  /// Perform layout on the given record.
  ///
  /// This routine allows the external AST source to provide an specific
  /// layout for a record, overriding the layout that would normally be
  /// constructed. It is intended for clients who receive specific layout
  /// details rather than source code (such as LLDB). The client is expected
  /// to fill in the field offsets, base offsets, virtual base offsets, and
  /// complete object size.
  ///
  /// \param Record The record whose layout is being requested.
  ///
  /// \param Size The final size of the record, in bits.
  ///
  /// \param Alignment The final alignment of the record, in bits.
  ///
  /// \param FieldOffsets The offset of each of the fields within the record,
  /// expressed in bits. All of the fields must be provided with offsets.
  ///
  /// \param BaseOffsets The offset of each of the direct, non-virtual base
  /// classes. If any bases are not given offsets, the bases will be laid
  /// out according to the ABI.
  ///
  /// \param VirtualBaseOffsets The offset of each of the virtual base classes
  /// (either direct or not). If any bases are not given offsets, the bases will
  /// be laid out according to the ABI.
  ///
  /// \returns true if the record layout was provided, false otherwise.
  bool
  layoutRecordType(const RecordDecl *Record,
                   uint64_t &Size, uint64_t &Alignment,
                   llvm::DenseMap<const FieldDecl *, uint64_t> &FieldOffsets,
                 llvm::DenseMap<const CXXRecordDecl *, CharUnits> &BaseOffsets,
                 llvm::DenseMap<const CXXRecordDecl *,
                                CharUnits> &VirtualBaseOffsets) override{
    for(size_t i = 0; i < Sources.size(); ++i)
      if (Sources[i]->layoutRecordType(Record, Size, Alignment, FieldOffsets,
                                       BaseOffsets, VirtualBaseOffsets))
        return true;
    return false;
  }

  /// Return the amount of memory used by memory buffers, breaking down
  /// by heap-backed versus mmap'ed memory.
  void getMemoryBufferSizes(MemoryBufferSizes &sizes) const override {
    for(size_t i = 0; i < Sources.size(); ++i)
      Sources[i]->getMemoryBufferSizes(sizes);

  }

  //===--------------------------------------------------------------------===//
  // ExternalSemaSource.
  //===--------------------------------------------------------------------===//

  /// Initialize the semantic source with the Sema instance
  /// being used to perform semantic analysis on the abstract syntax
  /// tree.
  void InitializeSema(Sema &S) override {
    for(size_t i = 0; i < Sources.size(); ++i)
      Sources[i]->InitializeSema(S);
  }

  /// Inform the semantic consumer that Sema is no longer available.
  void ForgetSema() override {
    for(size_t i = 0; i < Sources.size(); ++i)
      Sources[i]->ForgetSema();
  }

  /// Load the contents of the global method pool for a given
  /// selector.
  void ReadMethodPool(Selector Sel) override {
    for(size_t i = 0; i < Sources.size(); ++i)
      Sources[i]->ReadMethodPool(Sel);
  }

  /// Load the contents of the global method pool for a given
  /// selector if necessary.
  void updateOutOfDateSelector(Selector Sel) override {
    for(size_t i = 0; i < Sources.size(); ++i)
      Sources[i]->updateOutOfDateSelector(Sel);
  }

  /// Load the set of namespaces that are known to the external source,
  /// which will be used during typo correction.
  void
  ReadKnownNamespaces(SmallVectorImpl<NamespaceDecl*> &Namespaces) override{
    for(size_t i = 0; i < Sources.size(); ++i)
      Sources[i]->ReadKnownNamespaces(Namespaces);
  }

  /// Load the set of used but not defined functions or variables with
  /// internal linkage, or used but not defined inline functions.
  void ReadUndefinedButUsed(
      llvm::MapVector<NamedDecl *, SourceLocation> &Undefined) override {
    for(size_t i = 0; i < Sources.size(); ++i)
      Sources[i]->ReadUndefinedButUsed(Undefined);
  }

  void ReadMismatchingDeleteExpressions(llvm::MapVector<
      FieldDecl *, llvm::SmallVector<std::pair<SourceLocation, bool>, 4>> &
                                            Exprs) override {
    for (auto &Source : Sources)
      Source->ReadMismatchingDeleteExpressions(Exprs);
  }


  /// Do last resort, unqualified lookup on a LookupResult that
  /// Sema cannot find.
  ///
  /// \param R a LookupResult that is being recovered.
  ///
  /// \param S the Scope of the identifier occurrence.
  ///
  /// \return true to tell Sema to recover using the LookupResult.
  bool LookupUnqualified(LookupResult &R, Scope *S) override{
    for(size_t i = 0; i < Sources.size(); ++i)
      Sources[i]->LookupUnqualified(R, S);

    return !R.empty();
  }

  /// Read the set of tentative definitions known to the external Sema
  /// source.
  ///
  /// The external source should append its own tentative definitions to the
  /// given vector of tentative definitions. Note that this routine may be
  /// invoked multiple times; the external source should take care not to
  /// introduce the same declarations repeatedly.
  void ReadTentativeDefinitions(SmallVectorImpl<VarDecl*> &Defs) override {
    for(size_t i = 0; i < Sources.size(); ++i)
      Sources[i]->ReadTentativeDefinitions(Defs);
  }

  /// Read the set of unused file-scope declarations known to the
  /// external Sema source.
  ///
  /// The external source should append its own unused, filed-scope to the
  /// given vector of declarations. Note that this routine may be
  /// invoked multiple times; the external source should take care not to
  /// introduce the same declarations repeatedly.
  void ReadUnusedFileScopedDecls(
                        SmallVectorImpl<const DeclaratorDecl*> &Decls) override {
    for(size_t i = 0; i < Sources.size(); ++i)
      Sources[i]->ReadUnusedFileScopedDecls(Decls);
  }


  /// Read the set of delegating constructors known to the
  /// external Sema source.
  ///
  /// The external source should append its own delegating constructors to the
  /// given vector of declarations. Note that this routine may be
  /// invoked multiple times; the external source should take care not to
  /// introduce the same declarations repeatedly.
  void ReadDelegatingConstructors(
                          SmallVectorImpl<CXXConstructorDecl*> &Decls) override {
    for(size_t i = 0; i < Sources.size(); ++i)
      Sources[i]->ReadDelegatingConstructors(Decls);
  }

  /// Read the set of ext_vector type declarations known to the
  /// external Sema source.
  ///
  /// The external source should append its own ext_vector type declarations to
  /// the given vector of declarations. Note that this routine may be
  /// invoked multiple times; the external source should take care not to
  /// introduce the same declarations repeatedly.
  void ReadExtVectorDecls(SmallVectorImpl<TypedefNameDecl*> &Decls) override {
    for(size_t i = 0; i < Sources.size(); ++i)
      Sources[i]->ReadExtVectorDecls(Decls);
  }

  /// Read the set of potentially unused typedefs known to the source.
  ///
  /// The external source should append its own potentially unused local
  /// typedefs to the given vector of declarations. Note that this routine may
  /// be invoked multiple times; the external source should take care not to
  /// introduce the same declarations repeatedly.
  void ReadUnusedLocalTypedefNameCandidates(
      llvm::SmallSetVector<const TypedefNameDecl *, 4> &Decls) override {
    for(size_t i = 0; i < Sources.size(); ++i)
      Sources[i]->ReadUnusedLocalTypedefNameCandidates(Decls);
  }

  /// Read the set of referenced selectors known to the
  /// external Sema source.
  ///
  /// The external source should append its own referenced selectors to the
  /// given vector of selectors. Note that this routine
  /// may be invoked multiple times; the external source should take care not
  /// to introduce the same selectors repeatedly.
  void ReadReferencedSelectors(SmallVectorImpl<std::pair<Selector,
                                              SourceLocation> > &Sels) override {
    for(size_t i = 0; i < Sources.size(); ++i)
      Sources[i]->ReadReferencedSelectors(Sels);
  }

  /// Read the set of weak, undeclared identifiers known to the
  /// external Sema source.
  ///
  /// The external source should append its own weak, undeclared identifiers to
  /// the given vector. Note that this routine may be invoked multiple times;
  /// the external source should take care not to introduce the same identifiers
  /// repeatedly.
  void ReadWeakUndeclaredIdentifiers(
           SmallVectorImpl<std::pair<IdentifierInfo*, WeakInfo> > &WI) override {
    for(size_t i = 0; i < Sources.size(); ++i)
      Sources[i]->ReadWeakUndeclaredIdentifiers(WI);
  }

  /// Read the set of used vtables known to the external Sema source.
  ///
  /// The external source should append its own used vtables to the given
  /// vector. Note that this routine may be invoked multiple times; the external
  /// source should take care not to introduce the same vtables repeatedly.
  void ReadUsedVTables(SmallVectorImpl<ExternalVTableUse> &VTables) override {
    for(size_t i = 0; i < Sources.size(); ++i)
      Sources[i]->ReadUsedVTables(VTables);
  }

  /// Read the set of pending instantiations known to the external
  /// Sema source.
  ///
  /// The external source should append its own pending instantiations to the
  /// given vector. Note that this routine may be invoked multiple times; the
  /// external source should take care not to introduce the same instantiations
  /// repeatedly.
  void ReadPendingInstantiations(
     SmallVectorImpl<std::pair<ValueDecl*, SourceLocation> >& Pending) override {
    for(size_t i = 0; i < Sources.size(); ++i)
      Sources[i]->ReadPendingInstantiations(Pending);
  }

  /// Read the set of late parsed template functions for this source.
  ///
  /// The external source should insert its own late parsed template functions
  /// into the map. Note that this routine may be invoked multiple times; the
  /// external source should take care not to introduce the same map entries
  /// repeatedly.
  void ReadLateParsedTemplates(
      llvm::MapVector<const FunctionDecl *, std::unique_ptr<LateParsedTemplate>>
          &LPTMap) override {
    for (size_t i = 0; i < Sources.size(); ++i)
      Sources[i]->ReadLateParsedTemplates(LPTMap);
  }


  /// \copydoc ExternalSemaSource::CorrectTypo
  /// \note Returns the first nonempty correction.
  TypoCorrection CorrectTypo(const DeclarationNameInfo &Typo,
                             int LookupKind, Scope *S, CXXScopeSpec *SS,
                             CorrectionCandidateCallback &CCC,
                             DeclContext *MemberContext,
                             bool EnteringContext,
                             const ObjCObjectPointerType *OPT) override {
    for (size_t I = 0, E = Sources.size(); I < E; ++I) {
      if (TypoCorrection C = Sources[I]->CorrectTypo(Typo, LookupKind, S, SS, CCC,
                                                     MemberContext,
                                                     EnteringContext, OPT))
        return C;
    }
    return TypoCorrection();
  }

  /// Produces a diagnostic note if one of the attached sources
  /// contains a complete definition for \p T. Queries the sources in list
  /// order until the first one claims that a diagnostic was produced.
  ///
  /// \param Loc the location at which a complete type was required but not
  /// provided
  ///
  /// \param T the \c QualType that should have been complete at \p Loc
  ///
  /// \return true if a diagnostic was produced, false otherwise.
  bool MaybeDiagnoseMissingCompleteType(SourceLocation Loc,
                                        QualType T) override {
    for (size_t I = 0, E = Sources.size(); I < E; ++I) {
      if (Sources[I]->MaybeDiagnoseMissingCompleteType(Loc, T))
        return true;
    }
    return false;
  }

};

}

unsigned ClangExpressionParser::Parse(DiagnosticManager &diagnostic_manager) {
  ClangDiagnosticManagerAdapter *adapter =
      static_cast<ClangDiagnosticManagerAdapter *>(
          m_compiler->getDiagnostics().getClient());
  clang::TextDiagnosticBuffer *diag_buf = adapter->GetPassthrough();
  diag_buf->FlushDiagnostics(m_compiler->getDiagnostics());

  adapter->ResetManager(&diagnostic_manager);

  ClangExpressionHelper *type_system_helper =
      dyn_cast<ClangExpressionHelper>(m_expr.GetTypeSystemHelper());
  ClangExpressionDeclMap *decl_map = type_system_helper->DeclMap();

  clang::ASTContext *ast_context = &m_compiler->getASTContext();

  const char *expr_text = m_expr.Text();

  clang::SourceManager &source_mgr = m_compiler->getSourceManager();
  bool created_main_file = false;
  if (m_compiler->getCodeGenOpts().getDebugInfo() ==
      codegenoptions::FullDebugInfo) {
    int temp_fd = -1;
    llvm::SmallString<PATH_MAX> result_path;
    if (FileSpec tmpdir_file_spec = HostInfo::GetProcessTempDir()) {
      tmpdir_file_spec.AppendPathComponent("lldb-%%%%%%.expr");
      std::string temp_source_path = tmpdir_file_spec.GetPath();
      llvm::sys::fs::createUniqueFile(temp_source_path, temp_fd, result_path);
    } else {
      llvm::sys::fs::createTemporaryFile("lldb", "expr", temp_fd, result_path);
    }

    if (temp_fd != -1) {
      lldb_private::File file(temp_fd, true);
      const size_t expr_text_len = strlen(expr_text);
      size_t bytes_written = expr_text_len;
      if (file.Write(expr_text, bytes_written).Success()) {
        if (bytes_written == expr_text_len) {
          file.Close();
          source_mgr.setMainFileID(
              source_mgr.createFileID(m_file_manager->getFile(result_path),
                                      SourceLocation(), SrcMgr::C_User));
          created_main_file = true;
        }
      }
    }
  }

  if (!created_main_file) {
    std::unique_ptr<MemoryBuffer> memory_buffer =
        MemoryBuffer::getMemBufferCopy(expr_text, __FUNCTION__);
    source_mgr.setMainFileID(source_mgr.createFileID(std::move(memory_buffer)));
  }

  diag_buf->BeginSourceFile(m_compiler->getLangOpts(),
                            &m_compiler->getPreprocessor());
  auto &PP = m_compiler->getPreprocessor();

  PP.getBuiltinInfo().initializeBuiltins(PP.getIdentifierTable(),
                                         PP.getLangOpts());

  if (ClangExpressionDeclMap *decl_map = type_system_helper->DeclMap())
    decl_map->InstallCodeGenerator(m_code_generator);


  std::string module_name("$__lldb_module");
  m_llvm_context.reset(new LLVMContext());
  m_code_generator = CreateLLVMCodeGen(
      m_compiler->getDiagnostics(), module_name,
      m_compiler->getHeaderSearchOpts(), m_compiler->getPreprocessorOpts(),
      m_compiler->getCodeGenOpts(), *m_llvm_context);

  ASTConsumer *ast_transformer =
      type_system_helper->ASTTransformer(m_code_generator);

  std::unique_ptr<ASTConsumer> Consumer;
  if (ast_transformer) {
    std::unique_ptr<ASTConsumer> ast_transformer_proxy;
    ast_transformer_proxy.reset(new ASTConsumerForwarder(ast_transformer));
    Consumer = std::move(ast_transformer_proxy);
  } else {
    Consumer.reset(m_code_generator);
  }
  Consumer->Initialize(*ast_context);

  m_compiler->setSema(new Sema(m_compiler->getPreprocessor(), m_compiler->getASTContext(),
                               *Consumer, TU_Complete, nullptr));
  m_compiler->setASTConsumer(std::move(Consumer));

  m_compiler->createModuleManager();

  if (decl_map) {
    ExternalASTSourceWrapper *wrapper = new ExternalASTSourceWrapper(ast_context->getExternalSource());

    ast_context->TheSema = &m_compiler->getSema();

    clang::ExternalASTSource* ast_source(decl_map->CreateProxy());

    ExternalASTSourceWrapper *wrapper2 = new ExternalASTSourceWrapper(ast_source);

    decl_map->InstallASTContext(*ast_context, m_compiler->getFileManager());
    MyMultiplexExternalSemaSource *multiplexer = new MyMultiplexExternalSemaSource(*wrapper, *wrapper2);
    ast_context->setExternalSource(multiplexer);
  }

  m_ast_context.reset(
      new ClangASTContext(m_compiler->getTargetOpts().Triple.c_str()));
  m_ast_context->setASTContext(ast_context);
  m_ast_context.release();

  assert(m_compiler->getASTContext().getExternalSource() && "Sema doesn't know about the ASTReader for modules?");
  assert(m_compiler->getSema().getExternalSource() && "Sema doesn't know about the ASTReader for modules?");

  {
    llvm::CrashRecoveryContextCleanupRegistrar<Sema> CleanupSema(&m_compiler->getSema());
    ParseAST(m_compiler->getSema(), false, false);
    m_compiler->setSema(nullptr);
  }

  diag_buf->EndSourceFile();

  unsigned num_errors = diag_buf->getNumErrors();

  if (m_pp_callbacks && m_pp_callbacks->hasErrors()) {
    num_errors++;
    diagnostic_manager.PutString(eDiagnosticSeverityError,
                                 "while importing modules:");
    diagnostic_manager.AppendMessageToDiagnostic(
        m_pp_callbacks->getErrorString());
  }

  if (!num_errors) {
    if (type_system_helper->DeclMap() &&
        !type_system_helper->DeclMap()->ResolveUnknownTypes()) {
      diagnostic_manager.Printf(eDiagnosticSeverityError,
                                "Couldn't infer the type of a variable");
      num_errors++;
    }
  }

  if (!num_errors) {
    type_system_helper->CommitPersistentDecls();
  }

  adapter->ResetManager();

  return num_errors;
}

std::string
ClangExpressionParser::GetClangTargetABI(const ArchSpec &target_arch) {
  std::string abi;

  if (target_arch.IsMIPS()) {
    switch (target_arch.GetFlags() & ArchSpec::eMIPSABI_mask) {
    case ArchSpec::eMIPSABI_N64:
      abi = "n64";
      break;
    case ArchSpec::eMIPSABI_N32:
      abi = "n32";
      break;
    case ArchSpec::eMIPSABI_O32:
      abi = "o32";
      break;
    default:
      break;
    }
  }
  return abi;
}

bool ClangExpressionParser::RewriteExpression(
    DiagnosticManager &diagnostic_manager) {
  clang::SourceManager &source_manager = m_compiler->getSourceManager();
  clang::edit::EditedSource editor(source_manager, m_compiler->getLangOpts(),
                                   nullptr);
  clang::edit::Commit commit(editor);
  clang::Rewriter rewriter(source_manager, m_compiler->getLangOpts());

  class RewritesReceiver : public edit::EditsReceiver {
    Rewriter &rewrite;

  public:
    RewritesReceiver(Rewriter &in_rewrite) : rewrite(in_rewrite) {}

    void insert(SourceLocation loc, StringRef text) override {
      rewrite.InsertText(loc, text);
    }
    void replace(CharSourceRange range, StringRef text) override {
      rewrite.ReplaceText(range.getBegin(), rewrite.getRangeSize(range), text);
    }
  };

  RewritesReceiver rewrites_receiver(rewriter);

  const DiagnosticList &diagnostics = diagnostic_manager.Diagnostics();
  size_t num_diags = diagnostics.size();
  if (num_diags == 0)
    return false;

  for (const Diagnostic *diag : diagnostic_manager.Diagnostics()) {
    const ClangDiagnostic *diagnostic = llvm::dyn_cast<ClangDiagnostic>(diag);
    if (diagnostic && diagnostic->HasFixIts()) {
      for (const FixItHint &fixit : diagnostic->FixIts()) {
        // This is cobbed from clang::Rewrite::FixItRewriter.
        if (fixit.CodeToInsert.empty()) {
          if (fixit.InsertFromRange.isValid()) {
            commit.insertFromRange(fixit.RemoveRange.getBegin(),
                                   fixit.InsertFromRange, /*afterToken=*/false,
                                   fixit.BeforePreviousInsertions);
          } else
            commit.remove(fixit.RemoveRange);
        } else {
          if (fixit.RemoveRange.isTokenRange() ||
              fixit.RemoveRange.getBegin() != fixit.RemoveRange.getEnd())
            commit.replace(fixit.RemoveRange, fixit.CodeToInsert);
          else
            commit.insert(fixit.RemoveRange.getBegin(), fixit.CodeToInsert,
                          /*afterToken=*/false, fixit.BeforePreviousInsertions);
        }
      }
    }
  }

  // FIXME - do we want to try to propagate specific errors here?
  if (!commit.isCommitable())
    return false;
  else if (!editor.commit(commit))
    return false;

  // Now play all the edits, and stash the result in the diagnostic manager.
  editor.applyRewrites(rewrites_receiver);
  RewriteBuffer &main_file_buffer =
      rewriter.getEditBuffer(source_manager.getMainFileID());

  std::string fixed_expression;
  llvm::raw_string_ostream out_stream(fixed_expression);

  main_file_buffer.write(out_stream);
  out_stream.flush();
  diagnostic_manager.SetFixedExpression(fixed_expression);

  return true;
}

static bool FindFunctionInModule(ConstString &mangled_name,
                                 llvm::Module *module, const char *orig_name) {
  for (const auto &func : module->getFunctionList()) {
    const StringRef &name = func.getName();
    if (name.find(orig_name) != StringRef::npos) {
      mangled_name.SetString(name);
      return true;
    }
  }

  return false;
}

lldb_private::Status ClangExpressionParser::PrepareForExecution(
    lldb::addr_t &func_addr, lldb::addr_t &func_end,
    lldb::IRExecutionUnitSP &execution_unit_sp, ExecutionContext &exe_ctx,
    bool &can_interpret, ExecutionPolicy execution_policy) {
  func_addr = LLDB_INVALID_ADDRESS;
  func_end = LLDB_INVALID_ADDRESS;
  Log *log(lldb_private::GetLogIfAllCategoriesSet(LIBLLDB_LOG_EXPRESSIONS));

  lldb_private::Status err;

  std::unique_ptr<llvm::Module> llvm_module_ap(
      m_code_generator->ReleaseModule());

  if (!llvm_module_ap.get()) {
    err.SetErrorToGenericError();
    err.SetErrorString("IR doesn't contain a module");
    return err;
  }

  ConstString function_name;

  if (execution_policy != eExecutionPolicyTopLevel) {
    // Find the actual name of the function (it's often mangled somehow)

    if (!FindFunctionInModule(function_name, llvm_module_ap.get(),
                              m_expr.FunctionName())) {
      err.SetErrorToGenericError();
      err.SetErrorStringWithFormat("Couldn't find %s() in the module",
                                   m_expr.FunctionName());
      return err;
    } else {
      if (log)
        log->Printf("Found function %s for %s", function_name.AsCString(),
                    m_expr.FunctionName());
    }
  }

  SymbolContext sc;

  if (lldb::StackFrameSP frame_sp = exe_ctx.GetFrameSP()) {
    sc = frame_sp->GetSymbolContext(lldb::eSymbolContextEverything);
  } else if (lldb::TargetSP target_sp = exe_ctx.GetTargetSP()) {
    sc.target_sp = target_sp;
  }

  LLVMUserExpression::IRPasses custom_passes;
  {
    auto lang = m_expr.Language();
    if (log)
      log->Printf("%s - Current expression language is %s\n", __FUNCTION__,
                  Language::GetNameForLanguageType(lang));
    lldb::ProcessSP process_sp = exe_ctx.GetProcessSP();
    if (process_sp && lang != lldb::eLanguageTypeUnknown) {
      auto runtime = process_sp->GetLanguageRuntime(lang);
      if (runtime)
        runtime->GetIRPasses(custom_passes);
    }
  }

  if (custom_passes.EarlyPasses) {
    if (log)
      log->Printf("%s - Running Early IR Passes from LanguageRuntime on "
                  "expression module '%s'",
                  __FUNCTION__, m_expr.FunctionName());

    custom_passes.EarlyPasses->run(*llvm_module_ap);
  }

  execution_unit_sp.reset(
      new IRExecutionUnit(m_llvm_context, // handed off here
                          llvm_module_ap, // handed off here
                          function_name, exe_ctx.GetTargetSP(), sc,
                          m_compiler->getTargetOpts().Features));

  ClangExpressionHelper *type_system_helper =
      dyn_cast<ClangExpressionHelper>(m_expr.GetTypeSystemHelper());
  ClangExpressionDeclMap *decl_map =
      type_system_helper->DeclMap(); // result can be NULL

  if (decl_map) {
    Stream *error_stream = NULL;
    Target *target = exe_ctx.GetTargetPtr();
    error_stream = target->GetDebugger().GetErrorFile().get();

    IRForTarget ir_for_target(decl_map, m_expr.NeedsVariableResolution(),
                              *execution_unit_sp, *error_stream,
                              function_name.AsCString());

    bool ir_can_run =
        ir_for_target.runOnModule(*execution_unit_sp->GetModule());

    if (!ir_can_run) {
      err.SetErrorString(
          "The expression could not be prepared to run in the target");
      return err;
    }

    Process *process = exe_ctx.GetProcessPtr();

    if (execution_policy != eExecutionPolicyAlways &&
        execution_policy != eExecutionPolicyTopLevel) {
      lldb_private::Status interpret_error;

      bool interpret_function_calls =
          !process ? false : process->CanInterpretFunctionCalls();
      can_interpret = IRInterpreter::CanInterpret(
          *execution_unit_sp->GetModule(), *execution_unit_sp->GetFunction(),
          interpret_error, interpret_function_calls);

      if (!can_interpret && execution_policy == eExecutionPolicyNever) {
        err.SetErrorStringWithFormat("Can't run the expression locally: %s",
                                     interpret_error.AsCString());
        return err;
      }
    }

    if (!process && execution_policy == eExecutionPolicyAlways) {
      err.SetErrorString("Expression needed to run in the target, but the "
                         "target can't be run");
      return err;
    }

    if (!process && execution_policy == eExecutionPolicyTopLevel) {
      err.SetErrorString("Top-level code needs to be inserted into a runnable "
                         "target, but the target can't be run");
      return err;
    }

    if (execution_policy == eExecutionPolicyAlways ||
        (execution_policy != eExecutionPolicyTopLevel && !can_interpret)) {
      if (m_expr.NeedsValidation() && process) {
        if (!process->GetDynamicCheckers()) {
          DynamicCheckerFunctions *dynamic_checkers =
              new DynamicCheckerFunctions();

          DiagnosticManager install_diagnostics;

          if (!dynamic_checkers->Install(install_diagnostics, exe_ctx)) {
            if (install_diagnostics.Diagnostics().size())
              err.SetErrorString("couldn't install checkers, unknown error");
            else
              err.SetErrorString(install_diagnostics.GetString().c_str());

            return err;
          }

          process->SetDynamicCheckers(dynamic_checkers);

          if (log)
            log->Printf("== [ClangUserExpression::Evaluate] Finished "
                        "installing dynamic checkers ==");
        }

        IRDynamicChecks ir_dynamic_checks(*process->GetDynamicCheckers(),
                                          function_name.AsCString());

        llvm::Module *module = execution_unit_sp->GetModule();
        if (!module || !ir_dynamic_checks.runOnModule(*module)) {
          err.SetErrorToGenericError();
          err.SetErrorString("Couldn't add dynamic checks to the expression");
          return err;
        }

        if (custom_passes.LatePasses) {
          if (log)
            log->Printf("%s - Running Late IR Passes from LanguageRuntime on "
                        "expression module '%s'",
                        __FUNCTION__, m_expr.FunctionName());

          custom_passes.LatePasses->run(*module);
        }
      }
    }

    if (execution_policy == eExecutionPolicyAlways ||
        execution_policy == eExecutionPolicyTopLevel || !can_interpret) {
      execution_unit_sp->GetRunnableInfo(err, func_addr, func_end);
    }
  } else {
    execution_unit_sp->GetRunnableInfo(err, func_addr, func_end);
  }

  return err;
}

lldb_private::Status ClangExpressionParser::RunStaticInitializers(
    lldb::IRExecutionUnitSP &execution_unit_sp, ExecutionContext &exe_ctx) {
  lldb_private::Status err;

  lldbassert(execution_unit_sp.get());
  lldbassert(exe_ctx.HasThreadScope());

  if (!execution_unit_sp.get()) {
    err.SetErrorString(
        "can't run static initializers for a NULL execution unit");
    return err;
  }

  if (!exe_ctx.HasThreadScope()) {
    err.SetErrorString("can't run static initializers without a thread");
    return err;
  }

  std::vector<lldb::addr_t> static_initializers;

  execution_unit_sp->GetStaticInitializers(static_initializers);

  for (lldb::addr_t static_initializer : static_initializers) {
    EvaluateExpressionOptions options;

    lldb::ThreadPlanSP call_static_initializer(new ThreadPlanCallFunction(
        exe_ctx.GetThreadRef(), Address(static_initializer), CompilerType(),
        llvm::ArrayRef<lldb::addr_t>(), options));

    DiagnosticManager execution_errors;
    lldb::ExpressionResults results =
        exe_ctx.GetThreadRef().GetProcess()->RunThreadPlan(
            exe_ctx, call_static_initializer, options, execution_errors);

    if (results != lldb::eExpressionCompleted) {
      err.SetErrorStringWithFormat("couldn't run static initializer: %s",
                                   execution_errors.GetString().c_str());
      return err;
    }
  }

  return err;
}
