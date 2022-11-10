// Unless explicitly stated otherwise all files in this repository are licensed
// under the Apache License Version 2.0. This product includes software
// developed at Datadog (https://www.datadoghq.com/). Copyright 2021-Present
// Datadog, Inc.

#include "ddprof_module_lib.hpp"

#include "build_id.hpp"
#include "ddres.hpp"
#include "defer.hpp"
#include "failed_assumption.hpp"
#include "logger.hpp"
#include "string_format.hpp"

#include <fcntl.h>
#include <gelf.h>
#include <libelf.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace ddprof {

static bool get_elf_offsets(int fd, const std::string &filepath,
                            Offset_t &bias_offset) {
  Elf *elf = elf_begin(fd, ELF_C_READ_MMAP, NULL);
  if (elf == NULL) {
    LG_WRN("Invalid elf %s", filepath.c_str());
    return false;
  }
  defer { elf_end(elf); };
  GElf_Ehdr ehdr_mem;
  GElf_Ehdr *ehdr = gelf_getehdr(elf, &ehdr_mem);
  if (ehdr == nullptr) {
    LG_WRN("Invalid elf %s", filepath.c_str());
    return false;
  }

  bool found_exec = false;
  switch (ehdr->e_type) {
  case ET_EXEC:
  case ET_CORE:
  case ET_DYN: {
    size_t phnum;
    if (unlikely(elf_getphdrnum(elf, &phnum) != 0)) {
      LG_WRN("Invalid elf %s", filepath.c_str());
      return false;
    }
    for (size_t i = 0; i < phnum; ++i) {
      GElf_Phdr phdr_mem;
      GElf_Phdr *ph = gelf_getphdr(elf, i, &phdr_mem);
      if (unlikely(ph == NULL)) {
        LG_WRN("Invalid elf %s", filepath.c_str());
        return false;
      }
      constexpr int rx = PF_X | PF_R;
      if (ph->p_type == PT_LOAD) {
        if ((ph->p_flags & rx) == rx) {
          if (!found_exec) {
            bias_offset = ph->p_vaddr - ph->p_offset;
            found_exec = true;
          } else {
            report_failed_assumption(ddprof::string_format(
                "Multiple exec LOAD segments: %s", filepath.c_str()));
          }
        }
      }
    }
    break;
  }
  default:
    LG_WRN("Unsupported elf type (%d) %s", ehdr->e_type, filepath.c_str());
    return false;
  }

  if (!found_exec) {
    LG_WRN("Not executable LOAD segment found in %s", filepath.c_str());
  }
  return found_exec;
}

DDRes report_module(Dwfl *dwfl, ProcessAddress_t pc, const Dso &dso,
                    const FileInfoValue &fileInfoValue, DDProfMod &ddprof_mod) {
  const std::string &filepath = fileInfoValue.get_path();
  const char *module_name = strrchr(filepath.c_str(), '/') + 1;
  if (fileInfoValue._errored) { // avoid bouncing on errors
    LG_DBG("DSO Previously errored - mod (%s)", module_name);
    return ddres_warn(DD_WHAT_MODULE);
  }

  Dwfl_Module *mod = dwfl_addrmodule(dwfl, pc);

  if (mod) {
    // There should not be a module already loaded at this address
    const char *main_name = nullptr;
    Dwarf_Addr low_addr, high_addr;
    dwfl_module_info(mod, 0, &low_addr, &high_addr, 0, 0, &main_name, 0);
    LG_NTC("Incoherent modules[PID=%d]: module %s [%lx-%lx] is already "
           "loaded at %lx(%s[ID#%d])",
           dso._pid, main_name, low_addr, high_addr, pc, filepath.c_str(),
           fileInfoValue.get_id());
    ddprof_mod._status = DDProfMod::kInconsistent;
    return ddres_warn(DD_WHAT_MODULE);
  }

  // Load the file at a matching DSO address
  dwfl_errno(); // erase previous error
  Offset_t bias_offset{};
  if (!get_elf_offsets(fileInfoValue._fd, filepath, bias_offset)) {
    fileInfoValue._errored = true;
    LG_WRN("Couldn't retrieve offsets from %s(%s)", module_name,
           fileInfoValue.get_path().c_str());
    return ddres_warn(DD_WHAT_MODULE);
  }

  Offset_t bias = dso._start - dso._pgoff - bias_offset;

  // libdwfl takes ownership (which is not 100% expected)
  int fd = dup(fileInfoValue._fd);
  if (fd < 0) {
    LG_WRN("Couldn't duplicate fd to module %s(%s)", module_name,
           fileInfoValue.get_path().c_str());
    return ddres_warn(DD_WHAT_MODULE);
  }
  ddprof_mod._mod =
      dwfl_report_elf(dwfl, module_name, filepath.c_str(), fd, bias, true);

  // Retrieve build id
  const unsigned char *bits = nullptr;
  GElf_Addr vaddr;
  if (int size = dwfl_module_build_id(ddprof_mod._mod, &bits, &vaddr);
      size > 0) {
    // ensure we called dwfl_module_getelf first (or this can fail)
    // returns the size
    ddprof_mod.set_build_id(BuildIdSpan{bits, static_cast<unsigned>(size)});
  }

  if (!ddprof_mod._mod) {
    // Ideally we would differentiate pid errors from file errors.
    // For perf reasons we will just flag the file as errored
    fileInfoValue._errored = true;
    LG_WRN("Couldn't addrmodule (%s)[0x%lx], MOD:%s (%s)", dwfl_errmsg(-1), pc,
           module_name, fileInfoValue.get_path().c_str());
    return ddres_warn(DD_WHAT_MODULE);
  } else {
    dwfl_module_info(ddprof_mod._mod, 0, &ddprof_mod._low_addr,
                     &ddprof_mod._high_addr, 0, 0, 0, 0);
    LG_DBG("Loaded mod from file (%s[ID#%d]), (%s) mod[%lx-%lx] bias[%lx], "
           "build-id: %s",
           fileInfoValue.get_path().c_str(), fileInfoValue.get_id(),
           dwfl_errmsg(-1), ddprof_mod._low_addr, ddprof_mod._high_addr, bias,
           ddprof_mod._build_id.c_str());
  }

  ddprof_mod._sym_bias = bias;
  return ddres_init();
}

} // namespace ddprof
