#include <onion/log.h>
#include <onion/proc_dmap.h>
#include "cheats/i_memory_backend.hpp"

#include <cerrno>
#include <cstring>
#include <sys/mman.h>

#include <ps5/kernel.h>
#include <ps5/mdbg.h>

#include "pt.h"
#include "util_platform.h"


namespace onion::cheats {
namespace {

constexpr uint64_t kPageSize = 0x4000ULL;

const char *dmap_failure_stage_name(onion_proc_dmap_failure_stage stage) {
  switch (stage) {
  case ONION_PROC_DMAP_FAILURE_ARGUMENT:
    return "argument";
  case ONION_PROC_DMAP_FAILURE_CONTEXT:
    return "process-context";
  case ONION_PROC_DMAP_FAILURE_TRANSLATION:
    return "virtual-translation";
  case ONION_PROC_DMAP_FAILURE_COPY:
    return "physical-copy";
  case ONION_PROC_DMAP_FAILURE_NONE:
  default:
    return "none";
  }
}

inline uint64_t pageAlignDown(uint64_t v) {
  return v & ~(kPageSize - 1ULL);
}
inline uint64_t pageAlignUp(uint64_t v) {
  return (v + kPageSize - 1ULL) & ~(kPageSize - 1ULL);
}

int mapCodeCaveCommon(pid_t pid, uint64_t addr, size_t len) {
  const uint64_t page_start = pageAlignDown(addr);
  const uint64_t page_end = pageAlignUp(addr + len);
  const size_t page_len = static_cast<size_t>(page_end - page_start);

  if (pt_attach_proc(pid) < 0) {
    LOG_ERROR("[Cheat] code cave attach failed pid=%d errno=%d", pid,
                 errno);
    return -1;
  }

  const intptr_t mapped =
      pt_mmap(pid, static_cast<intptr_t>(page_start), page_len,
              PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  if (mapped != static_cast<intptr_t>(page_start)) {
    LOG_ERROR("[Cheat] code cave mmap failed page=0x%llx ret=0x%llx",
                 (unsigned long long)page_start, (unsigned long long)mapped);
    pt_detach_proc(pid, 0);
    return -1;
  }

  if (kernel_mprotect(pid, page_start, page_len,
                      PROT_READ | PROT_WRITE | PROT_EXEC) < 0) {
    LOG_ERROR("[Cheat] code cave mprotect failed page=0x%llx",
                 (unsigned long long)page_start);
    pt_detach_proc(pid, 0);
    return -1;
  }

  pt_detach_proc(pid, 0);
  return 0;
}

class MdbgBackend final : public IMemoryBackend {
public:
  int read(pid_t pid, uint64_t addr, void *buf, size_t len) override {
    return mdbg_copyout(pid, static_cast<intptr_t>(addr), buf, len);
  }
  int write(pid_t pid, uint64_t addr, const void *buf, size_t len) override {
    return mdbg_copyin(pid, buf, static_cast<intptr_t>(addr), len);
  }
  int mapCodeCave(pid_t pid, uint64_t addr, size_t len) override {
    return mapCodeCaveCommon(pid, addr, len);
  }
};

class KdirectBackend final : public IMemoryBackend {
public:
  int read(pid_t pid, uint64_t addr, void *buf, size_t len) override {
    onion_proc_dmap_failure_stage failure_stage = ONION_PROC_DMAP_FAILURE_NONE;
    const int rc = onion_proc_copyout_with_failure(pid, addr, buf, len,
                                                   &failure_stage);
    if (rc < 0)
      LOG_ERROR("[Cheat] Kdirect read failed pid=%d addr=0x%llx len=%zu "
                "stage=%s rc=%d",
                (int)pid, (unsigned long long)addr, len,
                dmap_failure_stage_name(failure_stage), rc);
    return rc;
  }
  int write(pid_t pid, uint64_t addr, const void *buf, size_t len) override {
    onion_proc_dmap_failure_stage failure_stage = ONION_PROC_DMAP_FAILURE_NONE;
    const int rc = onion_proc_copyin_with_failure(pid, addr, buf, len,
                                                  &failure_stage);
    if (rc < 0)
      LOG_ERROR("[Cheat] Kdirect write failed pid=%d addr=0x%llx len=%zu "
                "stage=%s rc=%d",
                (int)pid, (unsigned long long)addr, len,
                dmap_failure_stage_name(failure_stage), rc);
    return rc;
  }
  int mapCodeCave(pid_t pid, uint64_t addr, size_t len) override {
    return mapCodeCaveCommon(pid, addr, len);
  }
};

} // namespace

std::unique_ptr<IMemoryBackend> MemoryBackendFactory::create(uint32_t fw_major) {
  if (fw_major >= 0x840) {
    return std::make_unique<KdirectBackend>();
  }
  return std::make_unique<MdbgBackend>();
}

} // namespace onion::cheats
