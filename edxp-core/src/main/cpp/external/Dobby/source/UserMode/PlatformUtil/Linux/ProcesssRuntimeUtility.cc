#include "PlatformUtil/ProcessRuntimeUtility.h"

#include <elf.h>
#include <jni.h>
#include <string>
#include <dlfcn.h>
#include <link.h>
#include <sys/mman.h>

#include <vector>

#define LINE_MAX 2048

// ================================================================
// GetProcessMemoryLayout

static bool memory_region_comparator(MemoryRegion a, MemoryRegion b) {
  return ((addr_t)a.address < (addr_t)b.address);
}

std::vector<MemoryRegion> ProcessRuntimeUtility::GetProcessMemoryLayout() {
  std::vector<MemoryRegion> ProcessMemoryLayout;

  FILE *fp = fopen("/proc/self/maps", "r");
  if (fp == nullptr)
    return ProcessMemoryLayout;

  while (!feof(fp)) {
    char line_buffer[LINE_MAX + 1];
    fgets(line_buffer, LINE_MAX, fp);

    // ignore the rest of characters
    if (strlen(line_buffer) == LINE_MAX && line_buffer[LINE_MAX] != '\n') {
      // Entry not describing executable data. Skip to end of line to set up
      // reading the next entry.
      int c;
      do {
        c = getc(fp);
      } while ((c != EOF) && (c != '\n'));
      if (c == EOF)
        break;
    }

    addr_t  region_start;
    addr_t  region_end;
    addr_t  region_offset;
    char    permissions[5] = {'\0'}; // Ensure NUL-terminated string.
    uint8_t dev_major      = 0;
    uint8_t dev_minor      = 0;
    long    inode          = 0;
    int     path_index     = 0;

    // Sample format from man 5 proc:
    //
    // address           perms offset  dev   inode   pathname
    // 08048000-08056000 r-xp 00000000 03:0c 64593   /usr/sbin/gpm
    //
    // The final %n term captures the offset in the input string, which is used
    // to determine the path name. It *does not* increment the return value.
    // Refer to man 3 sscanf for details.
    if (sscanf(line_buffer,
               "%" PRIxPTR "-%" PRIxPTR " %4c "
               "%" PRIxPTR " %hhx:%hhx %ld %n",
               &region_start, &region_end, permissions, &region_offset, &dev_major, &dev_minor, &inode,
               &path_index) < 7) {
      FATAL("/proc/self/maps parse failed!");
      fclose(fp);
      return ProcessMemoryLayout;
    }

    MemoryPermission permission;
    if (permissions[0] == 'r' && permissions[1] == 'w') {
      permission = MemoryPermission::kReadWrite;
    } else if (permissions[0] == 'r' && permissions[2] == 'x') {
      permission = MemoryPermission::kReadExecute;
    } else if (permissions[0] == 'r' && permissions[1] == 'w' && permissions[2] == 'x') {
      permission = MemoryPermission::kReadWriteExecute;
    } else {
      permission = MemoryPermission::kNoAccess;
    }

#if 0
      DLOG(0, "%p --- %p", region_start, region_end);
#endif

    ProcessMemoryLayout.push_back(MemoryRegion{(void *)region_start, region_end - region_start, permission});
  }
  std::sort(ProcessMemoryLayout.begin(), ProcessMemoryLayout.end(), memory_region_comparator);

  fclose(fp);
  return ProcessMemoryLayout;
}

// ================================================================
// GetProcessModuleMap

static std::vector<RuntimeModule> get_process_map_with_proc_maps() {
  std::vector<RuntimeModule> ProcessModuleMap;

  FILE *fp = fopen("/proc/self/maps", "r");
  if (fp == nullptr)
    return ProcessModuleMap;

  while (!feof(fp)) {
    char line_buffer[LINE_MAX + 1];
    fgets(line_buffer, LINE_MAX, fp);

    // ignore the rest of characters
    if (strlen(line_buffer) == LINE_MAX && line_buffer[LINE_MAX] != '\n') {
      // Entry not describing executable data. Skip to end of line to set up
      // reading the next entry.
      int c;
      do {
        c = getc(fp);
      } while ((c != EOF) && (c != '\n'));
      if (c == EOF)
        break;
    }

    addr_t  region_start;
    addr_t  region_end;
    addr_t  region_offset;
    char    permissions[5] = {'\0'}; // Ensure NUL-terminated string.
    uint8_t dev_major      = 0;
    uint8_t dev_minor      = 0;
    long    inode          = 0;
    int     path_index     = 0;

    // Sample format from man 5 proc:
    //
    // address           perms offset  dev   inode   pathname
    // 08048000-08056000 r-xp 00000000 03:0c 64593   /usr/sbin/gpm
    //
    // The final %n term captures the offset in the input string, which is used
    // to determine the path name. It *does not* increment the return value.
    // Refer to man 3 sscanf for details.
    if (sscanf(line_buffer,
               "%" PRIxPTR "-%" PRIxPTR " %4c "
               "%" PRIxPTR " %hhx:%hhx %ld %n",
               &region_start, &region_end, permissions, &region_offset, &dev_major, &dev_minor, &inode,
               &path_index) < 7) {
      FATAL("/proc/self/maps parse failed!");
      fclose(fp);
      return ProcessModuleMap;
    }

    // check header section permission
    if (strcmp(permissions, "r--p") != 0 && strcmp(permissions, "r-xp") != 0)
      continue;

    // check elf magic number
    ElfW(Ehdr) *header = (ElfW(Ehdr) *)region_start;
    if (memcmp(header->e_ident, ELFMAG, SELFMAG) != 0) {
      continue;
    }

    RuntimeModule module;
    strncpy(module.path, line_buffer + path_index, 1024 - 1);
    if (module.path[strlen(module.path) - 1] == '\n') {
      module.path[strlen(module.path) - 1] = 0;
    }
    module.load_address = (void *)region_start;
    ProcessModuleMap.push_back(module);
  }

  fclose(fp);
  return ProcessModuleMap;
}

#if defined(__ANDROID__) && defined(__LP64__)
static std::vector<RuntimeModule> get_process_map_with_linker_iterator() {
  std::vector<RuntimeModule> ProcessModuleMap;

  dl_iterate_phdr(
      [](dl_phdr_info *info, size_t size, void *data) {
        RuntimeModule module = {0};
        if (info->dlpi_name && info->dlpi_name[0] == '/')
          strcpy(module.path, info->dlpi_name);
        module.load_address = (void *)info->dlpi_addr;

        // push to vector
        auto ProcessModuleMap = reinterpret_cast<std::vector<RuntimeModule> *>(data);
        ProcessModuleMap->push_back(module);
        return 0;
      },
      (void *)&ProcessModuleMap);

  return ProcessModuleMap;
}
#endif

std::vector<RuntimeModule> ProcessRuntimeUtility::GetProcessModuleMap() {
#if defined(__LP64__) && 0
  return get_process_map_with_linker_iterator();
#else
  return get_process_map_with_proc_maps();
#endif
}

RuntimeModule ProcessRuntimeUtility::GetProcessModule(const char *name) {
  std::vector<RuntimeModule> ProcessModuleMap = GetProcessModuleMap();
  for (auto module : ProcessModuleMap) {
    if (strstr(module.path, name) != 0) {
      return module;
    }
  }
  return RuntimeModule{0};
}