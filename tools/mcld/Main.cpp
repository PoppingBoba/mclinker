//===- Main.cpp -----------------------------------------------------------===//
//
//                     The MCLinker Project
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
#include <mcld/Environment.h>
#include <mcld/IRBuilder.h>
#include <mcld/Linker.h>
#include <mcld/LinkerConfig.h>
#include <mcld/LinkerScript.h>
#include <mcld/Module.h>
#include <mcld/ADT/StringEntry.h>
#include <mcld/MC/InputAction.h>
#include <mcld/MC/CommandAction.h>
#include <mcld/MC/FileAction.h>
#include <mcld/MC/ZOption.h>
#include <mcld/Support/raw_ostream.h>
#include <mcld/Support/MsgHandling.h>
#include <mcld/Support/Path.h>
#include <mcld/Support/SystemUtils.h>
#include <mcld/Support/TargetRegistry.h>

#include <llvm/ADT/ArrayRef.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/ADT/STLExtras.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/ADT/StringSwitch.h>
#include <llvm/Option/Arg.h>
#include <llvm/Option/ArgList.h>
#include <llvm/Option/OptTable.h>
#include <llvm/Option/Option.h>
#include <llvm/Support/ManagedStatic.h>
#include <llvm/Support/Process.h>
#include <llvm/Support/Signals.h>

#include <cassert>
#include <cstdlib>
#include <string>

#if defined(HAVE_UNISTD_H)
#include <unistd.h>
#endif

#if defined(_MSC_VER) || defined(__MINGW32__)
#include <io.h>
#ifndef STDIN_FILENO
#define STDIN_FILENO 0
#endif
#ifndef STDOUT_FILENO
#define STDOUT_FILENO 1
#endif
#ifndef STDERR_FILENO
#define STDERR_FILENO 2
#endif
#endif

namespace {

enum OptionID {
  // This is not an option.
  OPT_INVALID = 0,
#define OPTION(PREFIX, NAME, ID, KIND, ...) \
  OPT_ ## ID,
#include "Options.inc"  // NOLINT
#undef OPTION
  OPT_LastOption
};

class Driver {
 private:
  class OptTable : public llvm::opt::PrecomputedOptTable {
   private:
#define PREFIX(NAME, VALUE) \
    static const char* const NAME[];
#include "Options.inc"  // NOLINT
#undef PREFIX
    static const llvm::opt::OptTable::Info InfoTable[];

   public:
    OptTable();
  };

 private:
  explicit Driver(const char* prog_name)
      : prog_name_(prog_name),
        module_(script_),
        ir_builder_(module_, config_) {
    return;
  }

 public:
  static std::unique_ptr<Driver> Create(llvm::ArrayRef<const char*> argv);

  bool Run();

 private:
  bool TranslateArguments(llvm::opt::InputArgList& args);

 private:
  const char* prog_name_;

  mcld::LinkerScript script_;

  mcld::LinkerConfig config_;

  mcld::Module module_;

  mcld::IRBuilder ir_builder_;

  mcld::Linker linker_;

 private:
  DISALLOW_COPY_AND_ASSIGN(Driver);
};

// #define PREFIX(NAME, VALUE) \
//     const char* const Driver::OptTable::NAME[] = VALUE;
// #include "Options.inc"  // NOLINT
// #undef PREFIX

// @TODO Please check this value again
#define OPTTABLE_STR_TABLE_CODE
#include "Options.inc"
#undef OPTTABLE_STR_TABLE_CODE

#define OPTTABLE_VALUES_CODE
#include "Options.inc"
#undef OPTTABLE_VALUES_CODE

#define OPTTABLE_PREFIXES_TABLE_CODE
#include "Options.inc"
#undef OPTTABLE_PREFIXES_TABLE_CODE

#define OPTTABLE_PREFIXES_UNION_CODE
#include "Options.inc"
#undef OPTTABLE_PREFIXES_UNION_CODE

static constexpr llvm::opt::OptTable::Info OptInfoTable[] = {
#define OPTION(...) LLVM_CONSTRUCT_OPT_INFO(__VA_ARGS__),
#include "Options.inc"
#undef OPTION
};


Driver::OptTable::OptTable()
    : llvm::opt::PrecomputedOptTable(OptionStrTable, OptionPrefixesTable, OptInfoTable, OptionPrefixesTable) { }

inline bool ShouldColorize() {
  const char* term = getenv("TERM");
  
  return term && (0 != strcmp(term, "dumb"));
}

/// ParseProgName - Parse program name
/// This function simplifies cross-compiling by reading triple from the program
/// name. For example, if the program name is `arm-linux-eabi-ld.mcld', we can
/// get the triple is arm-linux-eabi by the program name.
inline std::string ParseProgName(const char* prog_name) {
  static const char* suffixes[] = {"ld", "ld.mcld"};

  std::string name(mcld::sys::fs::Path(prog_name).stem().native());

  for (size_t i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]); ++i) {
    if (name == suffixes[i])
      return std::string();
  }

  llvm::StringRef prog_name_ref(prog_name);
  llvm::StringRef prefix;

  for (size_t i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]); ++i) {
    if (!prog_name_ref.ends_with(suffixes[i]))
      continue;

    llvm::StringRef::size_type last_component =
        prog_name_ref.rfind('-', prog_name_ref.size() - strlen(suffixes[i]));
    if (last_component == llvm::StringRef::npos)
      continue;
    llvm::StringRef prefix = prog_name_ref.slice(0, last_component);
    std::string ignored_error;
    if (!mcld::TargetRegistry::lookupTarget(prefix.str(), ignored_error))
      continue;
    return prefix.str();
  }
  return std::string();
}

inline void ParseEmulation(llvm::Triple& triple, const char* emulation) {
  llvm::Triple emu_triple =
      llvm::StringSwitch<llvm::Triple>(emulation)
          .Case("aarch64linux", llvm::Triple("aarch64", "", "linux", "gnu"))
          .Case("armelf_linux_eabi", llvm::Triple("arm", "", "linux", "gnu"))
          .Case("elf_i386", llvm::Triple("i386", "", "", "gnu"))
          .Case("elf_x86_64", llvm::Triple("x86_64", "", "", "gnu"))
          .Case("elf32_x86_64", llvm::Triple("x86_64", "", "", "gnux32"))
          .Case("elf_i386_fbsd", llvm::Triple("i386", "", "freebsd", "gnu"))
          .Case("elf_x86_64_fbsd", llvm::Triple("x86_64", "", "freebsd", "gnu"))
          .Case("elf32ltsmip", llvm::Triple("mipsel", "", "", "gnu"))
          .Case("elf64ltsmip", llvm::Triple("mips64el", "", "", "gnu"))
          .Default(llvm::Triple());

  if (emu_triple.getArch() == llvm::Triple::UnknownArch &&
      emu_triple.getOS() == llvm::Triple::UnknownOS &&
      emu_triple.getEnvironment() == llvm::Triple::UnknownEnvironment)
    mcld::error(mcld::diag::err_invalid_emulation) << emulation << "\n";

  if (emu_triple.getArch() != llvm::Triple::UnknownArch)
    triple.setArch(emu_triple.getArch());

  if (emu_triple.getOS() != llvm::Triple::UnknownOS)
    triple.setOS(emu_triple.getOS());

  if (emu_triple.getEnvironment() != llvm::Triple::UnknownEnvironment)
    triple.setEnvironment(emu_triple.getEnvironment());
}

/// Configure the output filename.
inline bool ConfigureOutputName(llvm::StringRef output_name,
                                mcld::Module& module,
                                mcld::LinkerConfig& config) {
  std::string output(output_name.str());
  if (output.empty()) {
    if (config.targets().triple().getOS() == llvm::Triple::Win32) {
      output.assign("_out");
      switch (config.codeGenType()) {
        case mcld::LinkerConfig::Object: {
          output += ".obj";
          break;
        }
        case mcld::LinkerConfig::DynObj: {
          output += ".dll";
          break;
        }
        case mcld::LinkerConfig::Exec: {
          output += ".exe";
          break;
        }
        case mcld::LinkerConfig::External:
          break;
        default: {
          return false;
          break;
        }
      }  // switch (config.codeGenType())
    } else {
      output.assign("a.out");
    }
  }  // if (output.empty())

  module.setName(output);
  return true;
}

bool InitializeInputs(mcld::IRBuilder& ir_builder,
    std::vector<std::unique_ptr<mcld::InputAction>>& input_actions) {
  for (auto& action : input_actions) {
    assert(action != nullptr);
    action->activate(ir_builder.getInputBuilder());
  }

  if (ir_builder.getInputBuilder().isInGroup()) {
    mcld::fatal(mcld::diag::fatal_forbid_nest_group);
    return false;
  }

  return true;
}

bool Driver::TranslateArguments(llvm::opt::InputArgList& args) {
  //===--------------------------------------------------------------------===//
  // Preference
  //===--------------------------------------------------------------------===//

  // --color=mode
  if (llvm::opt::Arg* arg = args.getLastArg(OPT_Color)) {
    bool res = llvm::StringSwitch<bool>(arg->getValue())
                   .Case("never", false)
                   .Case("always", true)
                   .Case("auto", ShouldColorize() &&
                                 llvm::sys::Process::FileDescriptorIsDisplayed(
                                     STDOUT_FILENO))
                   .Default(false);
    config_.options().setColor(res);
    mcld::outs().setColor(res);
    mcld::errs().setColor(res);
  }

  // --trace
  config_.options().setTrace(args.hasArg(OPT_Trace));

  // --verbose=level
  if (llvm::opt::Arg* arg = args.getLastArg(OPT_Verbose)) {
    llvm::StringRef value = arg->getValue();
    int level;
    if (value.getAsInteger(0, level)) {
      mcld::errs() << "Invalid value for" << arg->getOption().getPrefixedName()
                   << ": " << arg->getValue();
      return false;
    }
    config_.options().setVerbose(level);
  }

  // --error-limit NUMBER
  if (llvm::opt::Arg* arg = args.getLastArg(OPT_ErrorLimit)) {
    llvm::StringRef value = arg->getValue();
    int num;
    if (value.getAsInteger(0, num) || (num < 0)) {
      mcld::errs() << "Invalid value for" << arg->getOption().getPrefixedName()
                   << ": " << arg->getValue();
      return false;
    }
    config_.options().setMaxErrorNum(num);
  }

  // --warning-limit NUMBER
  if (llvm::opt::Arg* arg = args.getLastArg(OPT_WarningLimit)) {
    llvm::StringRef value = arg->getValue();
    int num;
    if (value.getAsInteger(0, num) || (num < 0)) {
      mcld::errs() << "Invalid value for" << arg->getOption().getPrefixedName()
                   << ": " << arg->getValue();
      return false;
    }
    config_.options().setMaxWarnNum(num);
  }

  // --warn-shared-textrel
  config_.options().setWarnSharedTextrel(args.hasArg(OPT_WarnSharedTextrel));

  //===--------------------------------------------------------------------===//
  // Target
  //===--------------------------------------------------------------------===//
  llvm::Triple triple;
  if (llvm::opt::Arg* arg = args.getLastArg(OPT_Triple)) {
    // 1. Use the triple from command.
    // -mtriple=value
    triple.setTriple(arg->getValue());
  } else {
    std::string prog_triple = ParseProgName(prog_name_);
    if (!prog_triple.empty()) {
      // 2. Use the triple from the program name prefix.
      triple.setTriple(prog_triple);
    } else {
      // 3. Use the default target triple.
      triple.setTriple(mcld::sys::getDefaultTargetTriple());
    }
  }

  // If a specific emulation was requested, apply it now.
  if (llvm::opt::Arg* arg = args.getLastArg(OPT_Emulation)) {
    // -m emulation
    ParseEmulation(triple, arg->getValue());
  } else if (llvm::opt::Arg* arg = args.getLastArg(OPT_Arch)) {
    // -march=value
    config_.targets().setArch(arg->getValue());
  }

  if (llvm::opt::Arg* arg = args.getLastArg(OPT_CPU)) {
    config_.targets().setTargetCPU(arg->getValue());
  }

  config_.targets().setTriple(triple);

  // --gpsize=value
  if (llvm::opt::Arg* arg = args.getLastArg(OPT_GPSize)) {
    llvm::StringRef value = arg->getValue();
    int size;
    if (value.getAsInteger(0, size) || (size< 0)) {
      mcld::errs() << "Invalid value for" << arg->getOption().getPrefixedName()
                   << ": " << arg->getValue() << "\n";
      return false;
    }
    config_.targets().setGPSize(size);
  }

  // --stub-group-size=value
  if (llvm::opt::Arg* arg = args.getLastArg(OPT_StubGroupSize)) {
    llvm::StringRef value = arg->getValue();
    int size;
    if (value.getAsInteger(0, size) || (size< 0)) {
      mcld::errs() << "Invalid value for" << arg->getOption().getPrefixedName()
                   << ": " << arg->getValue() << "\n";
      return false;
    }
    config_.targets().setStubGroupSize(size);
  }

  // --fix-cortex-a53-835769
  config_.targets().setFixCA53Erratum835769(
      args.hasArg(OPT_FixCA53Erratum835769));

  // --fix-cortex-a53-843419
  config_.targets().setFixCA53Erratum843419(
      args.hasArg(OPT_FixCA53Erratum843419));

  //===--------------------------------------------------------------------===//
  // Dynamic
  //===--------------------------------------------------------------------===//

  // --entry=entry
  if (llvm::opt::Arg* arg = args.getLastArg(OPT_Entry)) {
    script_.setEntry(arg->getValue());
  }

  // -Bsymbolic
  config_.options().setBsymbolic(args.hasArg(OPT_Bsymbolic));

  // -Bgroup
  config_.options().setBgroup(args.hasArg(OPT_Bgroup));

  // -soname=name
  if (llvm::opt::Arg* arg = args.getLastArg(OPT_SOName)) {
    config_.options().setSOName(arg->getValue());
  }

  // --no-undefined
  if (args.hasArg(OPT_NoUndef)) {
    config_.options().setNoUndefined(true);
  }

  // --allow-multiple-definition
  if (args.hasArg(OPT_AllowMulDefs)) {
    config_.options().setMulDefs(true);
  }

  // -z options
  for (llvm::opt::Arg* arg : args.filtered(OPT_Z)) {
    llvm::StringRef value = arg->getValue();
    mcld::ZOption z_opt =
        llvm::StringSwitch<mcld::ZOption>(value)
            .Case("combreloc", mcld::ZOption(mcld::ZOption::CombReloc))
            .Case("nocombreloc", mcld::ZOption(mcld::ZOption::NoCombReloc))
            .Case("defs", mcld::ZOption(mcld::ZOption::Defs))
            .Case("execstack", mcld::ZOption(mcld::ZOption::ExecStack))
            .Case("noexecstack", mcld::ZOption(mcld::ZOption::NoExecStack))
            .Case("initfirst", mcld::ZOption(mcld::ZOption::InitFirst))
            .Case("interpose", mcld::ZOption(mcld::ZOption::InterPose))
            .Case("loadfltr", mcld::ZOption(mcld::ZOption::LoadFltr))
            .Case("muldefs", mcld::ZOption(mcld::ZOption::MulDefs))
            .Case("nocopyreloc", mcld::ZOption(mcld::ZOption::NoCopyReloc))
            .Case("nodefaultlib", mcld::ZOption(mcld::ZOption::NoDefaultLib))
            .Case("nodelete", mcld::ZOption(mcld::ZOption::NoDelete))
            .Case("nodlopen", mcld::ZOption(mcld::ZOption::NoDLOpen))
            .Case("nodump", mcld::ZOption(mcld::ZOption::NoDump))
            .Case("relro", mcld::ZOption(mcld::ZOption::Relro))
            .Case("norelro", mcld::ZOption(mcld::ZOption::NoRelro))
            .Case("lazy", mcld::ZOption(mcld::ZOption::Lazy))
            .Case("now", mcld::ZOption(mcld::ZOption::Now))
            .Case("origin", mcld::ZOption(mcld::ZOption::Origin))
            .Default(mcld::ZOption());

    if (z_opt.kind() == mcld::ZOption::Unknown) {
      if (value.starts_with("common-page-size=")) {
        // -z common-page-size=value
        z_opt.setKind(mcld::ZOption::CommPageSize);
        long long unsigned size = 0;
        value.drop_front(17).getAsInteger(0, size);
        z_opt.setPageSize(static_cast<uint64_t>(size));
      } else if (value.starts_with("max-page-size=")) {
        // -z max-page-size=value
        z_opt.setKind(mcld::ZOption::MaxPageSize);
        long long unsigned size = 0;
        value.drop_front(14).getAsInteger(0, size);
        z_opt.setPageSize(static_cast<uint64_t>(size));
      }
    }
    config_.options().addZOption(z_opt);
  }

  // --dynamic-linker=file
  if (llvm::opt::Arg* arg = args.getLastArg(OPT_Dyld)) {
    config_.options().setDyld(arg->getValue());
  }

  // --enable-new-dtags
  config_.options().setNewDTags(args.hasArg(OPT_EnableNewDTags));

  // --spare-dyanmic-tags COUNT
  if (llvm::opt::Arg* arg = args.getLastArg(OPT_SpareDTags)) {
    llvm::StringRef value = arg->getValue();
    int num;
    if (value.getAsInteger(0, num) || (num < 0)) {
      mcld::errs() << "Invalid value for" << arg->getOption().getPrefixedName()
                   << ": " << arg->getValue() << "\n";
      return false;
    }
    config_.options().setNumSpareDTags(num);
  }

  //===--------------------------------------------------------------------===//
  // Output
  //===--------------------------------------------------------------------===//

  // Setup the codegen type.
  if (args.hasArg(OPT_Shared) || args.hasArg(OPT_PIE)) {
    // -shared, -pie
    config_.setCodeGenType(mcld::LinkerConfig::DynObj);
  } else if (args.hasArg(OPT_Relocatable)) {
    // -r
    config_.setCodeGenType(mcld::LinkerConfig::Object);
  } else if (llvm::opt::Arg* arg = args.getLastArg(OPT_OutputFormat)) {
    // --oformat=value
    llvm::StringRef value = arg->getValue();
    if (value == "binary") {
      config_.setCodeGenType(mcld::LinkerConfig::Binary);
    }
  } else {
    config_.setCodeGenType(mcld::LinkerConfig::Exec);
  }

  // Setup the output filename.
  llvm::StringRef output_name;
  if (llvm::opt::Arg* arg = args.getLastArg(OPT_Output)) {
    output_name = arg->getValue();
  }
  if (!ConfigureOutputName(output_name, module_, config_)) {
    mcld::unreachable(mcld::diag::unrecognized_output_file) << module_.name();
    return false;
  } else {
    if (!args.hasArg(OPT_SOName)) {
      config_.options().setSOName(module_.name());
    }
  }

  // --format=value
  if (llvm::opt::Arg* arg = args.getLastArg(OPT_InputFormat)) {
    llvm::StringRef value = arg->getValue();
    if (value == "binary") {
      config_.options().setBinaryInput();
    }
  }

  // Setup debug info stripping.
  config_.options().setStripDebug(args.hasArg(OPT_StripDebug) ||
                                  args.hasArg(OPT_StripAll));

  // Setup symbol stripping mode.
  if (args.hasArg(OPT_StripAll)) {
    config_.options().setStripSymbols(
        mcld::GeneralOptions::StripSymbolMode::StripAllSymbols);
  } else if (args.hasArg(OPT_DiscardAll)) {
    config_.options().setStripSymbols(
        mcld::GeneralOptions::StripSymbolMode::StripLocals);
  } else if (args.hasArg(OPT_DiscardLocals)) {
    config_.options().setStripSymbols(
        mcld::GeneralOptions::StripSymbolMode::StripTemporaries);
  } else {
    config_.options().setStripSymbols(
        mcld::GeneralOptions::StripSymbolMode::KeepAllSymbols);
  }

  // --eh-frame-hdr
  config_.options().setEhFrameHdr(args.hasArg(OPT_EHFrameHdr));

  // -pie
  config_.options().setPIE(args.hasArg(OPT_PIE));

  // --nmagic
  config_.options().setNMagic(args.hasArg(OPT_NMagic));

  // --omagic
  config_.options().setOMagic(args.hasArg(OPT_OMagic));

  // --hash-style=style
  if (llvm::opt::Arg* arg = args.getLastArg(OPT_HashStyle)) {
    mcld::GeneralOptions::HashStyle style =
        llvm::StringSwitch<mcld::GeneralOptions::HashStyle>(arg->getValue())
            .Case("sysv", mcld::GeneralOptions::HashStyle::SystemV)
            .Case("gnu", mcld::GeneralOptions::HashStyle::GNU)
            .Case("both", mcld::GeneralOptions::HashStyle::Both)
            .Default(mcld::GeneralOptions::HashStyle::Unknown);
    if (style != mcld::GeneralOptions::HashStyle::Unknown) {
      config_.options().setHashStyle(style);
    }
  }

  // --[no]-export-dynamic
  if (llvm::opt::Arg* arg = args.getLastArg(OPT_ExportDynamic,
                                              OPT_NoExportDynamic)) {
    if (arg->getOption().matches(OPT_ExportDynamic)) {
      config_.options().setExportDynamic(true);
    } else {
      config_.options().setExportDynamic(false);
    }
  }

  // --no-warn-mismatch
  config_.options().setWarnMismatch(!args.hasArg(OPT_NoWarnMismatch));

  // --exclude-libs
  if (llvm::opt::Arg* arg = args.getLastArg(OPT_ExcludeLibs)) {
    llvm::StringRef value = arg->getValue();
    do {
      std::pair<llvm::StringRef, llvm::StringRef> res = value.split(',');
      config_.options().excludeLIBS().insert(res.first.str());
      value = res.second;
    } while (!value.empty());
  }

  //===--------------------------------------------------------------------===//
  // Search Path
  //===--------------------------------------------------------------------===//

  // --sysroot
  if (llvm::opt::Arg* arg = args.getLastArg(OPT_Sysroot)) {
    mcld::sys::fs::Path path(arg->getValue());
    if (mcld::sys::fs::exists(path) && mcld::sys::fs::is_directory(path)) {
      script_.setSysroot(path);
    }
  }

  // -L searchdir
  for (llvm::opt::Arg* arg : args.filtered(OPT_LibraryPath)) {
    if (!script_.directories().insert(arg->getValue()))
      mcld::warning(mcld::diag::warn_cannot_open_search_dir) << arg->getValue();
  }

  // -nostdlib
  config_.options().setNoStdlib(args.hasArg(OPT_NoStdlib));

  // -rpath=path
  for (llvm::opt::Arg* arg : args.filtered(OPT_RPath)) {
    config_.options().getRpathList().push_back(arg->getValue());
  }

  //===--------------------------------------------------------------------===//
  // Symbol
  //===--------------------------------------------------------------------===//

  // -d/-dc/-dp
  config_.options().setDefineCommon(args.hasArg(OPT_DefineCommon));

  // -u symbol
  for (llvm::opt::Arg* arg : args.filtered(OPT_Undefined)) {
    config_.options().getUndefSymList().push_back(arg->getValue());
  }

  //===--------------------------------------------------------------------===//
  // Script
  //===--------------------------------------------------------------------===//

  // --wrap=symbol
  for (llvm::opt::Arg* arg : args.filtered(OPT_Wrap)) {
    bool exist = false;
    const char* symbol = arg->getValue();
    // symbol -> __wrap_symbol
    mcld::StringEntry<llvm::StringRef>* to_wrap =
        script_.renameMap().insert(symbol, exist);

    std::string to_wrap_str;
    to_wrap_str.append("__wrap_")
               .append(symbol);
    to_wrap->setValue(to_wrap_str);

    if (exist)
      mcld::warning(mcld::diag::rewrap) << symbol << to_wrap_str;

    // __real_symbol -> symbol
    std::string from_real_str;
    to_wrap_str.append("__real_")
               .append(symbol);
    mcld::StringEntry<llvm::StringRef>* from_real =
        script_.renameMap().insert(from_real_str, exist);
    from_real->setValue(symbol);

    if (exist)
      mcld::warning(mcld::diag::rewrap) << symbol << from_real_str;
  }

  // --portalbe=symbol
  for (llvm::opt::Arg* arg : args.filtered(OPT_Portable)) {
    bool exist = false;
    const char* symbol = arg->getValue();
    // symbol -> symbol_portable
    mcld::StringEntry<llvm::StringRef>* to_wrap =
        script_.renameMap().insert(symbol, exist);

    std::string to_wrap_str;
    to_wrap_str.append(symbol)
               .append("_portable");
    to_wrap->setValue(to_wrap_str);

    if (exist)
      mcld::warning(mcld::diag::rewrap) << symbol << to_wrap_str;

    // __real_symbol -> symbol
    std::string from_real_str;
    to_wrap_str.append("__real_")
               .append(symbol);
    mcld::StringEntry<llvm::StringRef>* from_real =
        script_.renameMap().insert(from_real_str, exist);
    from_real->setValue(symbol);

    if (exist)
      mcld::warning(mcld::diag::rewrap) << symbol << from_real_str;
  }

  // --section-start=section=addr
  for (llvm::opt::Arg* arg : args.filtered(OPT_SectionStart)) {
    llvm::StringRef value = arg->getValue();
    const size_t pos = value.find('=');
    uint64_t addr = 0;
    value.substr(pos + 1).getAsInteger(0, addr);
    bool exist = false;
    mcld::StringEntry<uint64_t>* mapping =
        script_.addressMap().insert(value.substr(0, pos), exist);
    mapping->setValue(addr);
  }

  // -Tbss=value
  if (llvm::opt::Arg* arg = args.getLastArg(OPT_Tbss)) {
    llvm::StringRef value = arg->getValue();
    uint64_t addr = 0;
    if (value.getAsInteger(0, addr)) {
      mcld::errs() << "Invalid value for" << arg->getOption().getPrefixedName()
                   << ": " << arg->getValue() << "\n";
      return false;
    }
    bool exist = false;
    mcld::StringEntry<uint64_t>* mapping =
        script_.addressMap().insert(".bss", exist);
    mapping->setValue(addr);
  }

  // -Tdata=value
  if (llvm::opt::Arg* arg = args.getLastArg(OPT_Tdata)) {
    llvm::StringRef value = arg->getValue();
    uint64_t addr = 0;
    if (value.getAsInteger(0, addr)) {
      mcld::errs() << "Invalid value for" << arg->getOption().getPrefixedName()
                   << ": " << arg->getValue() << "\n";
      return false;
    }
    bool exist = false;
    mcld::StringEntry<uint64_t>* mapping =
        script_.addressMap().insert(".data", exist);
    mapping->setValue(addr);
  }

  // -Ttext=value
  if (llvm::opt::Arg* arg = args.getLastArg(OPT_Ttext)) {
    llvm::StringRef value = arg->getValue();
    uint64_t addr = 0;
    if (value.getAsInteger(0, addr)) {
      mcld::errs() << "Invalid value for" << arg->getOption().getPrefixedName()
                   << ": " << arg->getValue() << "\n";
      return false;
    }
    bool exist = false;
    mcld::StringEntry<uint64_t>* mapping =
        script_.addressMap().insert(".text", exist);
    mapping->setValue(addr);
  }

  //===--------------------------------------------------------------------===//
  // Optimization
  //===--------------------------------------------------------------------===//

  // --[no-]gc-sections
  if (llvm::opt::Arg* arg = args.getLastArg(OPT_GCSections,
                                              OPT_NoGCSections)) {
    if (arg->getOption().matches(OPT_GCSections)) {
      config_.options().setGCSections(true);
    } else {
      config_.options().setGCSections(false);
    }
  }

  // --[no-]print-gc-sections
  if (llvm::opt::Arg* arg = args.getLastArg(OPT_PrintGCSections,
                                              OPT_NoPrintGCSections)) {
    if (arg->getOption().matches(OPT_PrintGCSections)) {
      config_.options().setPrintGCSections(true);
    } else {
      config_.options().setPrintGCSections(false);
    }
  }

  // --[no-]ld-generated-unwind-info
  if (llvm::opt::Arg* arg = args.getLastArg(OPT_LDGeneratedUnwindInfo,
                                              OPT_NoLDGeneratedUnwindInfo)) {
    if (arg->getOption().matches(OPT_LDGeneratedUnwindInfo)) {
      config_.options().setGenUnwindInfo(true);
    } else {
      config_.options().setGenUnwindInfo(false);
    }
  }

  // --icf=mode
  if (llvm::opt::Arg* arg = args.getLastArg(OPT_ICF)) {
    mcld::GeneralOptions::ICF mode =
        llvm::StringSwitch<mcld::GeneralOptions::ICF>(arg->getValue())
            .Case("none", mcld::GeneralOptions::ICF::None)
            .Case("all", mcld::GeneralOptions::ICF::All)
            .Case("safe", mcld::GeneralOptions::ICF::Safe)
            .Default(mcld::GeneralOptions::ICF::Unknown);
    if (mode == mcld::GeneralOptions::ICF::Unknown) {
      mcld::errs() << "Invalid value for" << arg->getOption().getPrefixedName()
                   << ": " << arg->getValue() << "\n";
      return false;
    }
    config_.options().setICFMode(mode);
  }

  // --icf-iterations
  if (llvm::opt::Arg* arg = args.getLastArg(OPT_ICFIters)) {
    llvm::StringRef value = arg->getValue();
    int num;
    if (value.getAsInteger(0, num) || (num < 0)) {
      mcld::errs() << "Invalid value for" << arg->getOption().getPrefixedName()
                   << ": " << arg->getValue() << "\n";
      return false;
    }
    config_.options().setICFIterations(num);
  }

  // --[no-]print-icf-sections
  if (llvm::opt::Arg* arg = args.getLastArg(OPT_PrintICFSections,
                                              OPT_NoPrintICFSections)) {
    if (arg->getOption().matches(OPT_PrintICFSections)) {
      config_.options().setPrintICFSections(true);
    } else {
      config_.options().setPrintICFSections(false);
    }
  }

  //===--------------------------------------------------------------------===//
  // Positional
  //===--------------------------------------------------------------------===//

  // # of regular objects, script, and namespec.
  size_t input_num = 0;
  typedef std::unique_ptr<mcld::InputAction> Action;

  std::vector<Action> actions;
  Action action;
  actions.reserve(32);

  for (llvm::opt::Arg* arg : args) {
    const unsigned index = arg->getIndex();
    printf("Test::-> %d: %s[ID %d]\n", arg->getIndex(), arg->getValue(), arg->getOption().getID());

    switch (arg->getOption().getID()) {
      // -T script
      case OPT_Script: {
        const char* value = arg->getValue();
        config_.options().getScriptList().push_back(value);

        // FIXME: Let index of script file be 0.
        action.reset(new mcld::ScriptAction(
            0x0, value, mcld::ScriptFile::LDScript, script_.directories()));
        actions.push_back(std::move(action));

        action.reset(new mcld::ContextAction(0x0));
        actions.push_back(std::move(action));

        action.reset(new mcld::MemoryAreaAction(0x0,
                                                mcld::FileHandle::ReadOnly));
        actions.push_back(std::move(action));

        ++input_num;
        break;
      }

      // --defsym=symbol=expr
      case OPT_DefSym: {
        std::string expr;
        expr.append(arg->getValue())
            .append(";");
        script_.defsyms().push_back(std::move(expr));
        action.reset(new mcld::DefSymAction(index, script_.defsyms().back()));
        actions.push_back(std::move(action));
        break;
      }

      // -l namespec
      case OPT_Namespec: {
        action.reset(new mcld::NamespecAction(
            index, arg->getValue(), script_.directories()));
        actions.push_back(std::move(action));

        action.reset(new mcld::ContextAction(index));
        actions.push_back(std::move(action));

        action.reset(new mcld::MemoryAreaAction(index,
                                                mcld::FileHandle::ReadOnly));
        actions.push_back(std::move(action));

        ++input_num;
        break;
      }

      // --whole-archive
      case OPT_WholeArchive: {
        action.reset(new mcld::WholeArchiveAction(index));
        actions.push_back(std::move(action));
        break;
      }

      // --no-whole-archive
      case OPT_NoWholeArchive: {
        action.reset(new mcld::NoWholeArchiveAction(index));
        actions.push_back(std::move(action));
        break;
      }

      // --as-needed
      case OPT_AsNeeded: {
        action.reset(new mcld::AsNeededAction(index));
        actions.push_back(std::move(action));
        break;
      }

      // --no-as-needed
      case OPT_NoAsNeeded: {
        action.reset(new mcld::NoAsNeededAction(index));
        actions.push_back(std::move(action));
        break;
      }

      // --add-needed
      // FIXME: This is deprecated. Should be --copy-dt-needed-entries.
      case OPT_AddNeeded:
      case OPT_CopyDTNeeded: {
        action.reset(new mcld::AddNeededAction(index));
        actions.push_back(std::move(action));
        break;
      }

      // --no-add-needed
      // FIXME: This is deprecated. Should be --no-copy-dt-needed-entries.
      case OPT_NoAddNeeded:
      case OPT_NoCopyDTNeeded: {
        action.reset(new mcld::AddNeededAction(index));
        actions.push_back(std::move(action));
        break;
      }

      // -Bdynamic
      case OPT_Bdynamic: {
        action.reset(new mcld::BDynamicAction(index));
        actions.push_back(std::move(action));
        break;
      }

      // -Bstatic
      case OPT_Bstatic: {
        action.reset(new mcld::BStaticAction(index));
        actions.push_back(std::move(action));
        break;
      }

      // --start-group
      case OPT_StartGroup: {
        action.reset(new mcld::StartGroupAction(index));
        actions.push_back(std::move(action));
        break;
      }

      // --end-group
      case OPT_EndGroup: {
        action.reset(new mcld::EndGroupAction(index));
        actions.push_back(std::move(action));
        break;
      }

      case OPT_INPUT: 
      case OPT_UNKNOWN: {
        action.reset(new mcld::InputFileAction(index, arg->getValue()));
        actions.push_back(std::move(action));

        action.reset(new mcld::ContextAction(index));
        actions.push_back(std::move(action));

        action.reset(new mcld::MemoryAreaAction(index,
                                                mcld::FileHandle::ReadOnly));
        actions.push_back(std::move(action));

        ++input_num;
        break;
      }

      default:
        break;
    }
  }

  if (input_num == 0) {
    mcld::fatal(mcld::diag::err_no_inputs);
    return false;
  }

  // Stable sort
  std::stable_sort(actions.begin(),
                   actions.end(),
                   [] (const Action& X, const Action& Y) {
                     return X->position() < Y->position();
                   });

  if (!InitializeInputs(ir_builder_, actions)) {
    mcld::errs() << "Failed to initialize input tree!\n";
    return false;
  }


  //===--------------------------------------------------------------------===//
  // Unknown
  //===--------------------------------------------------------------------===//
  std::vector<std::string> unknown_args = args.getAllArgValues(OPT_UNKNOWN);
  for (std::string arg : unknown_args)
    mcld::warning(mcld::diag::warn_unsupported_option) << arg;

  return true;
}

std::unique_ptr<Driver> Driver::Create(llvm::ArrayRef<const char*> argv) {
  // Parse command line options.
  OptTable opt_table;
  unsigned missing_arg_idx;
  unsigned missing_arg_count;
  llvm::opt::InputArgList args =
      opt_table.ParseArgs(argv.slice(1), missing_arg_idx, missing_arg_count);
  if (missing_arg_count > 0) {
    mcld::errs() << "Argument to '" << args.getArgString(missing_arg_idx)
                 << "' is missing (expected " << missing_arg_count
                 << ((missing_arg_count > 1) ? " values" : " value") << ")\n";
    return nullptr;
  }

  std::unique_ptr<Driver> result(new Driver(argv[0]));

  // Return quickly if -help is specified.
  if (args.hasArg(OPT_Help)) {
    opt_table.printHelp(mcld::outs(), argv[0], "MCLinker",
                        /* FlagsToInclude */0, /* FlagsToExclude */0);
    return nullptr;
  }

  // Print version information if requested.
  if (args.hasArg(OPT_Version)) {
    mcld::outs() << result->config_.options().getVersionString() << "\n";
  }

  // Setup instance from arguments.
  if (!result->TranslateArguments(args)) {
    return nullptr;
  }

  return result;
}

bool Driver::Run() {
  mcld::Initialize();

  if (!linker_.emulate(script_, config_)) {
    mcld::errs() << "Failed to emulate target!\n";
    return false;
  }

  if (!linker_.link(module_, ir_builder_)) {
    mcld::errs() << "Failed to link objects!\n";
    return false;
  }

  if (!linker_.emit(module_, module_.name())) {
    mcld::errs() << "Failed to emit output!\n";
    return false;
  }

  mcld::Finalize();
  return true;
}

}  // anonymous namespace

int main(int argc, char** argv) {
  std::unique_ptr<Driver> driver =
      Driver::Create(llvm::ArrayRef(argv, argc));

  if ((driver == nullptr) || !driver->Run()) {
    return EXIT_FAILURE;
  } else {
    return EXIT_SUCCESS;
  }
}
