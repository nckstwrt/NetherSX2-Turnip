/*
 * vulkan_shim.c - GOT patch approach
 *
 * Compile:
 *   aarch64-linux-android26-clang -shared -fPIC -O2 -o libvulkad.so \
 *       vulkan_shim.c -ldl -llog
 */

#include <dlfcn.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <elf.h>
#include <android/log.h>
#include <sys/stat.h>
#include <sys/system_properties.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>

#define TAG "VulkanShim"

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */
static void  *g_sys_vulkan   = NULL;  /* handle to system libvulkan.so */
static char   g_lib_dir[512];         /* our lib directory             */
static char   g_turnip_path[512];     /* full path to Turnip ICD       */
static void *g_turnip = NULL;

// Log file code
static FILE *g_logfile = NULL;

static void init_logfile(void) {
    if (g_logfile) return;
    
    /* Extract package name from our lib path */
    char *pkg_start = strstr(g_lib_dir, "/data/app/");
    if (!pkg_start) return;
    
    char *after_hash = strchr(pkg_start + 10, '/');
    if (!after_hash) return;
    after_hash++;
    
    char *dash = strstr(after_hash, "-");
    if (!dash) return;
    
    char pkg[256] = {0};
    strncpy(pkg, after_hash, dash - after_hash);
    
    /* Build path: /sdcard/Android/data/<pkg>/files/vulkan_shim.log */
    char path[512];
    snprintf(path, sizeof(path), "/sdcard/Android/data/%s/files", pkg);
    mkdir(path, 0755);  /* ensure it exists */
    
    strncat(path, "/vulkan_shim.log", sizeof(path) - strlen(path) - 1);
    
    g_logfile = fopen(path, "a");
    if (g_logfile) {
        setvbuf(g_logfile, NULL, _IOLBF, 0);  /* line-buffered for safety */
        time_t now = time(NULL);
        fprintf(g_logfile, "\n=== shim started at %s", ctime(&now));
    }
}

static void log_to_file(const char *level, const char *fmt, va_list args) {
    if (!g_logfile) init_logfile();
    if (!g_logfile) return;
    
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    localtime_r(&ts.tv_sec, &tm);
    
    fprintf(g_logfile, "%02d:%02d:%02d.%03ld [%s] ",
            tm.tm_hour, tm.tm_min, tm.tm_sec, ts.tv_nsec / 1000000,
            level);
    vfprintf(g_logfile, fmt, args);
    fputc('\n', g_logfile);
}

static void shim_logi(const char *fmt, ...) {
    va_list args, args2;
    va_start(args, fmt);
    va_copy(args2, args);
    __android_log_vprint(ANDROID_LOG_INFO, TAG, fmt, args);
    log_to_file("I", fmt, args2);
    va_end(args);
    va_end(args2);
}

static void shim_loge(const char *fmt, ...) {
    va_list args, args2;
    va_start(args, fmt);
    va_copy(args2, args);
    __android_log_vprint(ANDROID_LOG_ERROR, TAG, fmt, args);
    log_to_file("E", fmt, args2);
    va_end(args);
    va_end(args2);
}

#define LOGI(...) shim_logi(__VA_ARGS__)
#define LOGE(...) shim_loge(__VA_ARGS__)

typedef void (*PFN_vkVoidFunction)(void);
typedef void*    VkInstance;
typedef void*    VkDevice;
typedef uint32_t VkResult;
typedef void*    VkCommandBuffer;

/* ------------------------------------------------------------------ */
/* Vulkan types needed for the readback barrier hook                    */
/* ------------------------------------------------------------------ */
typedef uint32_t VkFlags;
typedef VkFlags  VkPipelineStageFlags;
typedef VkFlags  VkAccessFlags;
typedef VkFlags  VkDependencyFlags;

typedef struct VkMemoryBarrier {
    uint32_t       sType;       /* VK_STRUCTURE_TYPE_MEMORY_BARRIER = 46 */
    const void    *pNext;
    VkAccessFlags  srcAccessMask;
    VkAccessFlags  dstAccessMask;
} VkMemoryBarrier;

/* Vulkan enum values we need */
#define VK_STRUCTURE_TYPE_MEMORY_BARRIER          46
#define VK_PIPELINE_STAGE_ALL_COMMANDS_BIT        0x00010000
#define VK_PIPELINE_STAGE_TRANSFER_BIT            0x00001000
#define VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT      0x00000100
#define VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT 0x00000400
#define VK_ACCESS_SHADER_WRITE_BIT                0x00000040
#define VK_ACCESS_TRANSFER_READ_BIT               0x00000800
#define VK_ACCESS_TRANSFER_WRITE_BIT              0x00001000
#define VK_ACCESS_MEMORY_WRITE_BIT                0x00010000
#define VK_ACCESS_MEMORY_READ_BIT                 0x00008000

/* ------------------------------------------------------------------ */
/* Readback barrier hook state                                         */
/* ------------------------------------------------------------------ */
typedef void (*PFN_vkCmdPipelineBarrier)(
    VkCommandBuffer, VkPipelineStageFlags, VkPipelineStageFlags,
    VkDependencyFlags, uint32_t, const VkMemoryBarrier *,
    uint32_t, const void *, uint32_t, const void *);

typedef void (*PFN_vkCmdCopyImageToBuffer2)(VkCommandBuffer, const void *);

static PFN_vkCmdPipelineBarrier     g_vkCmdPipelineBarrier = NULL;
static PFN_vkCmdCopyImageToBuffer2  g_real_copy_itb2       = NULL;
static PFN_vkCmdCopyImageToBuffer2  g_real_copy_itb2_khr   = NULL;

/* ------------------------------------------------------------------ */
/* Hooked vkCmdCopyImageToBuffer2 — injects a full barrier before      */
/* the copy to ensure UBWC metadata is coherent                        */
/* ------------------------------------------------------------------ */
static void hooked_CmdCopyImageToBuffer2(
    VkCommandBuffer cmdBuf, const void *pInfo)
{
    if (g_vkCmdPipelineBarrier) {
        VkMemoryBarrier memBarrier;
        memBarrier.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        memBarrier.pNext         = NULL;
        memBarrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                   VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                                   VK_ACCESS_SHADER_WRITE_BIT |
                                   VK_ACCESS_MEMORY_WRITE_BIT;
        memBarrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT |
                                   VK_ACCESS_MEMORY_READ_BIT;

        g_vkCmdPipelineBarrier(
            cmdBuf,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            0,
            1, &memBarrier,
            0, NULL,
            0, NULL);
    }

    if (g_real_copy_itb2)
        g_real_copy_itb2(cmdBuf, pInfo);
}

/* The real android_load_sphal_library so we can call it for non-Vulkan */
static void *(*g_real_sphal_load)(const char *name, int flags) = NULL;

/* ------------------------------------------------------------------ */
/* GOT patching helpers                                                */
/* ------------------------------------------------------------------ */

/* Find the load base of a mapped library by scanning /proc/self/maps */
static uintptr_t find_lib_base(const char *libname) {
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) return 0;
    char line[512];
    uintptr_t base = 0;
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, libname) && strstr(line, "r--p")) {
            base = (uintptr_t)strtoull(line, NULL, 16);
            break;
        }
    }
    fclose(f);
    LOGI("VulkanShim: base of %s = 0x%lx", libname, base);
    return base;
}

/* Patch a GOT entry: find symbol_name in libvulkan's RELA and replace */
static int patch_got(uintptr_t base, const char *symbol_name, void *new_fn) {
    /* Walk ELF headers at base */
    Elf64_Ehdr *ehdr = (Elf64_Ehdr *)base;
    if (memcmp(ehdr->e_ident, ELFMAG, 4) != 0) {
        LOGE("VulkanShim: bad ELF magic at 0x%lx", base);
        return 0;
    }

    Elf64_Phdr *phdr = (Elf64_Phdr *)(base + ehdr->e_phoff);
    uintptr_t load_bias = 0;

    /* Find load bias: difference between actual base and p_vaddr of LOAD */
    for (int i = 0; i < ehdr->e_phnum; i++) {
        if (phdr[i].p_type == PT_LOAD && phdr[i].p_offset == 0) {
            load_bias = base - phdr[i].p_vaddr;
            break;
        }
    }
    LOGI("VulkanShim: load_bias = 0x%lx", load_bias);

    /* Find dynamic segment */
    Elf64_Dyn *dyn = NULL;
    for (int i = 0; i < ehdr->e_phnum; i++) {
        if (phdr[i].p_type == PT_DYNAMIC) {
            dyn = (Elf64_Dyn *)(load_bias + phdr[i].p_vaddr);
            break;
        }
    }
    if (!dyn) { LOGE("VulkanShim: no dynamic segment"); return 0; }

    /* Extract RELA, RELACOUNT, SYMTAB, STRTAB from dynamic section */
    Elf64_Rela  *rela      = NULL;
    Elf64_Sym   *symtab    = NULL;
    const char  *strtab    = NULL;
    size_t       relacount = 0;
    size_t       relasz    = 0;

    for (Elf64_Dyn *d = dyn; d->d_tag != DT_NULL; d++) {
        switch (d->d_tag) {
            case DT_JMPREL:   rela      = (Elf64_Rela *)(load_bias + d->d_un.d_ptr); break;
            case DT_PLTRELSZ: relasz    = d->d_un.d_val; break;
            case DT_SYMTAB:   symtab    = (Elf64_Sym  *)(load_bias + d->d_un.d_ptr); break;
            case DT_STRTAB:   strtab    = (const char *)(load_bias + d->d_un.d_ptr); break;
        }
    }

    if (!rela || !symtab || !strtab) {
        LOGE("VulkanShim: missing RELA/SYMTAB/STRTAB");
        return 0;
    }

    relacount = relasz / sizeof(Elf64_Rela);
    LOGI("VulkanShim: scanning %zu PLT entries for %s", relacount, symbol_name);

    for (size_t i = 0; i < relacount; i++) {
        uint32_t sym_idx = ELF64_R_SYM(rela[i].r_info);
        const char *name = strtab + symtab[sym_idx].st_name;
        if (strcmp(name, symbol_name) == 0) {
            uintptr_t *got_entry = (uintptr_t *)(load_bias + rela[i].r_offset);
            LOGI("VulkanShim: found %s at GOT entry %p (value=0x%lx)",
                 symbol_name, got_entry, *got_entry);

            /* Make page writable */
            uintptr_t page     = (uintptr_t)got_entry & ~(uintptr_t)(4095);
            uintptr_t page_end = ((uintptr_t)got_entry + sizeof(uintptr_t) + 4095) & ~(uintptr_t)(4095);
            if (mprotect((void *)page, page_end - page,
                         PROT_READ | PROT_WRITE) != 0) {
                LOGE("VulkanShim: mprotect failed");
                return 0;
            }

            /* Save old value and patch */
            g_real_sphal_load = (void *(*)(const char*, int))(*got_entry);
            *got_entry = (uintptr_t)new_fn;

            /* Restore to read-execute */
            mprotect((void *)page, page_end - page, PROT_READ | PROT_EXEC);

            LOGI("VulkanShim: GOT patched! old=%p new=%p",
                 g_real_sphal_load, new_fn);
            return 1;
        }
    }

    LOGE("VulkanShim: %s not found in PLT", symbol_name);
    return 0;
}

/* Patch a GOT entry to point to a specific function (not necessarily our hook) */
static int patch_got_with_real(uintptr_t base, const char *symbol_name, void *new_fn) {
    Elf64_Ehdr *ehdr = (Elf64_Ehdr *)base;
    if (memcmp(ehdr->e_ident, ELFMAG, 4) != 0) return 0;

    Elf64_Phdr *phdr = (Elf64_Phdr *)(base + ehdr->e_phoff);
    uintptr_t load_bias = 0;
    for (int i = 0; i < ehdr->e_phnum; i++) {
        if (phdr[i].p_type == PT_LOAD && phdr[i].p_offset == 0) {
            load_bias = base - phdr[i].p_vaddr;
            break;
        }
    }

    Elf64_Dyn *dyn = NULL;
    for (int i = 0; i < ehdr->e_phnum; i++) {
        if (phdr[i].p_type == PT_DYNAMIC) {
            dyn = (Elf64_Dyn *)(load_bias + phdr[i].p_vaddr);
            break;
        }
    }
    if (!dyn) return 0;

    /* Collect ALL rela sections — both PLT (DT_JMPREL) and regular (DT_RELA) */
    Elf64_Rela *plt_rela   = NULL;  size_t plt_sz  = 0;
    Elf64_Rela *rela       = NULL;  size_t rela_sz = 0;
    Elf64_Sym  *symtab     = NULL;
    const char *strtab     = NULL;

    for (Elf64_Dyn *d = dyn; d->d_tag != DT_NULL; d++) {
        switch (d->d_tag) {
            case DT_JMPREL:   plt_rela = (Elf64_Rela *)(load_bias + d->d_un.d_ptr); break;
            case DT_PLTRELSZ: plt_sz   = d->d_un.d_val; break;
            case DT_RELA:     rela     = (Elf64_Rela *)(load_bias + d->d_un.d_ptr); break;
            case DT_RELASZ:   rela_sz  = d->d_un.d_val; break;
            case DT_SYMTAB:   symtab   = (Elf64_Sym  *)(load_bias + d->d_un.d_ptr); break;
            case DT_STRTAB:   strtab   = (const char *)(load_bias + d->d_un.d_ptr); break;
        }
    }
    if (!symtab || !strtab) return 0;

    /* Helper lambda — scan one rela table */
    #define SCAN_RELA(table, count)                                          \
    if (table) {                                                             \
        size_t n = (count) / sizeof(Elf64_Rela);                            \
        LOGI("VulkanShim: scanning %zu entries in " #table " for %s",       \
             n, symbol_name);                                                \
        for (size_t i = 0; i < n; i++) {                                    \
            uint32_t sym_idx = ELF64_R_SYM((table)[i].r_info);             \
            const char *name = strtab + symtab[sym_idx].st_name;           \
            if (strcmp(name, symbol_name) == 0) {                           \
                uintptr_t *got = (uintptr_t *)(load_bias + (table)[i].r_offset); \
                LOGI("VulkanShim: found %s at %p (val=0x%lx)",             \
                     symbol_name, got, *got);                               \
                uintptr_t pg  = (uintptr_t)got & ~(uintptr_t)4095;         \
                uintptr_t end = ((uintptr_t)got + 8 + 4095) & ~(uintptr_t)4095; \
                mprotect((void*)pg, end-pg, PROT_READ|PROT_WRITE);         \
                *got = (uintptr_t)new_fn;                                   \
                mprotect((void*)pg, end-pg, PROT_READ|PROT_EXEC);          \
                LOGI("VulkanShim: patched -> %p", new_fn);                  \
                return 1;                                                    \
            }                                                               \
        }                                                                   \
    }

    SCAN_RELA(plt_rela, plt_sz)   /* PLT entries */
    SCAN_RELA(rela,     rela_sz)  /* regular GOT entries */
    #undef SCAN_RELA

    LOGE("VulkanShim: %s not found in Turnip PLT or RELA", symbol_name);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Our hook for android_load_sphal_library                            */
/* ------------------------------------------------------------------ */
static void *sphal_hook(const char *name, int flags) {
    LOGI("VulkanShim: sphal_hook intercepted: %s (flags=%d)", name ? name : "NULL", flags);

    /* Only redirect the Vulkan ICD — identified by containing "vulkan"
       AND being a .so in a hw/ path or just named vulkan.*.so         */
    if (name) {
        int is_vulkan_icd = 0;

        /* System Adreno ICD patterns */
        if (strstr(name, "vulkan.") != NULL) is_vulkan_icd = 1;
        if (strstr(name, "/hw/vulkan") != NULL) is_vulkan_icd = 1;

        /* Explicitly NOT Vulkan — let these through */
        if (strstr(name, "gralloc") != NULL) is_vulkan_icd = 0;
        if (strstr(name, "hwcomposer") != NULL) is_vulkan_icd = 0;
        if (strstr(name, "egl") != NULL) is_vulkan_icd = 0;
        if (strstr(name, "gles") != NULL) is_vulkan_icd = 0;
        if (strstr(name, "mapper") != NULL) is_vulkan_icd = 0;

        if (is_vulkan_icd) {
            LOGI("VulkanShim: redirecting Vulkan ICD: %s -> Turnip", name);
            void *h = dlopen(g_turnip_path, flags | RTLD_GLOBAL);
            if (h) { LOGI("VulkanShim: Turnip redirect OK"); return h; }
            LOGE("VulkanShim: Turnip redirect failed: %s", dlerror());
        }
    }

    /* Pass everything else through to the real sphal loader */
    if (g_real_sphal_load) {
        LOGI("VulkanShim: passing through: %s", name ? name : "NULL");
        return g_real_sphal_load(name, flags);
    }
    return NULL;
}

/* Find a lib's path from /proc/self/maps regardless of namespace */
static int find_lib_path_in_maps(const char *libname, char *out_path, size_t out_size) {
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) return 0;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, libname) && strstr(line, "r--p")) {
            /* Extract path from end of line */
            char *path = strchr(line, '/');
            if (path) {
                path[strlen(path)-1] = '\0'; /* remove newline */
                strncpy(out_path, path, out_size-1);
                fclose(f);
                return 1;
            }
        }
    }
    fclose(f);
    return 0;
}

/* 
 * From:  /data/app/~~hash==/pkg-hash==/lib/arm64/libvulkad.so
 * Get:   /data/data/pkg/cache/vulkan_deps/
 * Via:   /proc/self/maps to find package name, or derive from lib path
 */
static int setup_deps_dir(char *deps_dir, size_t size) {
    /* Our lib path looks like:
       /data/app/~~X==/xyz.aethersx2.android-Y==/lib/arm64/libvulkad.so
       The data dir is:
       /data/data/xyz.aethersx2.android/cache/vulkan_deps/            */

    /* Extract package name from our lib path */
    char *pkg_start = strstr(g_lib_dir, "/data/app/");
    if (!pkg_start) return 0;

    /* Skip past /data/app/~~hash==/ */
    char *after_hash = strchr(pkg_start + 10, '/');
    if (!after_hash) return 0;
    after_hash++; /* skip the / */

    /* Package name ends at the next - followed by hash */
    char pkg[256] = {0};
    char *dash = strstr(after_hash, "-");
    if (!dash) return 0;
    strncpy(pkg, after_hash, dash - after_hash);

    snprintf(deps_dir, size, "/data/data/%s/cache/vulkan_deps", pkg);
    LOGI("VulkanShim: deps dir = %s", deps_dir);
    return 1;
}

static void copy_file(const char *src, const char *dst) {
    /* Check if already copied */
    FILE *test = fopen(dst, "r");
    if (test) { fclose(test); LOGI("VulkanShim: already exists: %s", dst); return; }

    FILE *in = fopen(src, "rb");
    if (!in) { LOGE("VulkanShim: cannot open src: %s", src); return; }

    FILE *out = fopen(dst, "wb");
    if (!out) { 
        fclose(in); 
        LOGE("VulkanShim: cannot open dst: %s", dst); 
        return; 
    }

    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
        fwrite(buf, 1, n, out);

    fclose(in);
    fclose(out);
    chmod(dst, 0755);
    LOGI("VulkanShim: copied %s -> %s", src, dst);
}

/* Some libs have different filenames in maps vs their soname */
static int find_lib_path_in_maps_multi(const char **names, char *out_path, size_t out_size) {
    for (int i = 0; names[i]; i++) {
        if (find_lib_path_in_maps(names[i], out_path, out_size))
            return 1;
    }
    return 0;
}

static int find_and_copy_deps(const char *deps_dir) {
    const char *needed[] = {
        "libc++.so",
        "libbase.so",
        "libsync.so",
        "libcutils.so",
        "libvndksupport.so",
        "libhardware.so",
        NULL
    };

    /* Copy standard libs */
    for (int i = 0; needed[i]; i++) {
        char dst[512];
        snprintf(dst, sizeof(dst), "%s/%s", deps_dir, needed[i]);
        FILE *f = fopen(dst, "r");
        if (f) { fclose(f); LOGI("VulkanShim: cached: %s", needed[i]); continue; }
        char src[512] = {0};
        if (!find_lib_path_in_maps(needed[i], src, sizeof(src))) {
            LOGE("VulkanShim: not in maps: %s", needed[i]); continue;
        }
        copy_file(src, dst);
    }

    /* libdl_android.so needs special handling - copy it AND
       create a copy named ld-android.so since that's its SONAME dependency */
    char ldandroid_dst[512], libdlandroid_dst[512];
    snprintf(ldandroid_dst,    sizeof(ldandroid_dst),    "%s/ld-android.so",    deps_dir);
    snprintf(libdlandroid_dst, sizeof(libdlandroid_dst), "%s/libdl_android.so", deps_dir);

    int ld_cached    = (fopen(ldandroid_dst,    "r") != NULL);
    int libdl_cached = (fopen(libdlandroid_dst, "r") != NULL);

    if (!ld_cached || !libdl_cached) {
        char src[512] = {0};
        if (find_lib_path_in_maps("libdl_android.so", src, sizeof(src))) {
            if (!libdl_cached) copy_file(src, libdlandroid_dst);
            if (!ld_cached)    copy_file(src, ldandroid_dst);
            LOGI("VulkanShim: copied libdl_android.so as both names");
        } else {
            /* Try finding via linker64 path - derive libdl_android path */
            char linker_path[512] = {0};
            if (find_lib_path_in_maps("linker64", linker_path, sizeof(linker_path))) {
                /* Replace 'bin/linker64' with 'lib64/bionic/libdl_android.so' */
                char *bin = strstr(linker_path, "/bin/linker64");
                if (bin) {
                    char derived[512];
                    strncpy(derived, linker_path, bin - linker_path);
                    derived[bin - linker_path] = '\0';
                    strncat(derived, "/lib64/bionic/libdl_android.so",
                            sizeof(derived) - strlen(derived) - 1);
                    LOGI("VulkanShim: trying derived path: %s", derived);
                    FILE *tf = fopen(derived, "r");
                    if (tf) {
                        fclose(tf);
                        if (!libdl_cached) copy_file(derived, libdlandroid_dst);
                        if (!ld_cached)    copy_file(derived, ldandroid_dst);
                        LOGI("VulkanShim: copied libdl_android via derived path");
                    } else {
                        LOGE("VulkanShim: derived path not accessible: %s", derived);
                    }
                }
            }
        }
    } else {
        LOGI("VulkanShim: cached: ld-android.so + libdl_android.so");
    }

    return 1;
}

/* android_get_exported_namespace — needed by bundled libvndksupport  */
__attribute__((visibility("default")))
void* android_get_exported_namespace(const char *name) {
    LOGI("VulkanShim: stub android_get_exported_namespace(%s)", name ? name : "NULL");
    /* Will be GOT-patched to real version before Turnip calls this    */
    return NULL;
}

/* android_load_sphal_library — needed by bundled libhardware         */
__attribute__((visibility("default")))
void* android_load_sphal_library(const char *name, int flags) {
    LOGI("VulkanShim: stub android_load_sphal_library(%s)", name ? name : "NULL");
    /* Will be GOT-patched — for libvulkan we redirect to Turnip,
       for everything else we call the real system function            */
    if (g_real_sphal_load) {
        return g_real_sphal_load(name, flags);
    }
    return NULL;
}

int get_adreno_model(char *value) {
    /* Android 12+ standardized property */
    if (__system_property_get("ro.soc.model", value) > 0) {
        /* SM8750 = Snapdragon 8 Elite (Adreno 830) */
		/* Elite-class / A8xx GPUs */
        if (strstr(value, "SM8850")) return 840;  /* 8 Elite Gen 5 / Elite 2 */
        if (strstr(value, "SM8845")) return 830;  /* 8 Gen 5 (flagship variant) */
        if (strstr(value, "SM8750")) return 830;  /* 8 Elite Gen 4 */
        if (strstr(value, "SM8735")) return 825;  /* 8s Gen 4 */
		
        /* extend as needed */
		if (value[0] != 0) return 0;		// If we got a value just return the string
    }

    /* Fallback: some OEMs use this instead */
    if (__system_property_get("ro.hardware.chipname", value) > 0) {
		if (strstr(value, "sm8850") || strstr(value, "SM8850")) return 840;
        if (strstr(value, "sm8750") || strstr(value, "SM8750")) return 830;
		
		if (value[0] != 0) return 0;		// If we got a value just return the string
    }

    /* Another fallback */
    if (__system_property_get("ro.board.platform", value) > 0) {
		if (strcmp(value, "pineapple") == 0) return 830;  /* SM8750 */
        if (strcmp(value, "sun") == 0)       return 840;  /* SM8850 (rumoured) */
    }

    return 0;
}

void setup_turnip_env(int noubwc, int nolrz, int flushall) {
    char buf[256] = {0};
    char *p = buf;

    if (noubwc)  p += sprintf(p, "noubwc,");
    if (nolrz)   p += sprintf(p, "nolrz,");
    if (flushall) p += sprintf(p, "flushall,");

    // Strip trailing comma
    if (p > buf) *(p - 1) = '\0';

    if (buf[0]) 
	{
		LOGI("VulkanShim: TU_DEBUG: %s", buf);
		setenv("TU_DEBUG", buf, 1);
	}
}

/* ------------------------------------------------------------------ */
/* Constructor                                                         */
/* ------------------------------------------------------------------ */
__attribute__((constructor))
static void shim_init(void) {
	LOGI("VulkanShim: shim_init");
	
    char value[PROP_VALUE_MAX] = {0};
	int adreno_model = get_adreno_model(value);
		
	//setup_turnip_env(1, 1, 1);
	// No TU_DEBUG flags needed — readback barrier hook ensures UBWC
	// metadata coherency before image-to-buffer copies, avoiding
	// the KGSL/SMMU fault without the performance cost of noubwc/flushall
    //setenv("TU_DEBUG", "noubwc,nolrz,flushall", 1);
    //putenv("TU_DEBUG=noubwc,nolrz,flushall");
    
    //LOGI("VulkanShim: TU_DEBUG verify: %s", getenv("TU_DEBUG"));
	
    /* 1. Find our lib dir */
    Dl_info info;
    if (!dladdr((void*)shim_init, &info) || !info.dli_fname) {
        LOGE("VulkanShim: dladdr failed"); return;
    }
    strncpy(g_lib_dir, info.dli_fname, sizeof(g_lib_dir) - 1);
    char *slash = strrchr(g_lib_dir, '/');
    if (!slash) return;
    *(slash + 1) = '\0';
    LOGI("VulkanShim: lib dir = %s", g_lib_dir);
	LOGI("VulkanShim: adreno_model = %d (%s)\n", adreno_model, value);
	
	// Check for override
	const char *override_path = "/data/local/tmp/libvulkan_freedreno.so";
	if (access(override_path, F_OK) == 0) {
		strcpy(g_turnip_path, override_path);
		LOGI("VulkanShim: Using Override");
	}
	else {
		if (strcmp(value, "SM8250") == 0 || strcasecmp(value, "kona") == 0) {
			snprintf(g_turnip_path, sizeof(g_turnip_path), "%slibvulkan_freedreno_v24.1.0_R18.a6xx-Patched.so", g_lib_dir);		// For SD865 - Use a patched Turnip. Change Hardware mode to Disable Readbacks if crashing.
			LOGI("VulkanShim: Using Kona specific driver");
		} else {
			if (adreno_model != 0) {
				snprintf(g_turnip_path, sizeof(g_turnip_path), "%slibvulkan_freedreno_Gen8_v28.so", g_lib_dir);		// Use this for Adreno 8
				LOGI("VulkanShim: Using Snapdragon ELITE driver");
			} else {
				snprintf(g_turnip_path, sizeof(g_turnip_path), "%slibvulkan_freedreno_v26.2.0_R1.so", g_lib_dir);		// Use this for Adreno 7
				LOGI("VulkanShim: Using *everything else* driver");
			}
		}
	}
			 
    LOGI("VulkanShim: Turnip path = %s", g_turnip_path);

	/* Make our own symbols globally visible so bundled libs can 
	   resolve android_get_exported_namespace and android_load_sphal_library
	   from our stubs */
	char self_path[512];
	snprintf(self_path, sizeof(self_path), "%slibvulkad.so", g_lib_dir);
	void *self = dlopen(self_path, RTLD_NOW | RTLD_GLOBAL | RTLD_NODELETE);
	if (self) LOGI("VulkanShim: self promoted to RTLD_GLOBAL");
	else      LOGE("VulkanShim: self promotion failed: %s", dlerror());


	/* 2. Promote libs already accessible in our namespace */
	const char *promote[] = { "liblog.so", "libsync.so", NULL };
	for (int i = 0; promote[i]; i++) {
		void *h = dlopen(promote[i], RTLD_NOLOAD | RTLD_GLOBAL);
		if (h) LOGI("VulkanShim: promoted: %s", promote[i]);
		else   LOGE("VulkanShim: not promotable: %s", promote[i]);
	}

	/* 3. Load bundled libs in dependency order */
	const char *bundled[] = {
		"libc++.so",         /* no extra deps beyond libc/libm/libdl      */
		"libbase.so",        /* needs liblog (promoted) + libc++           */
		"libcutils.so",      /* needs liblog + libbase + libc++            */
		"libvndksupport.so", /* patchelf'd: libdl_android removed          */
		"libhardware.so",    /* patchelf'd: libvndksupport removed         */
		NULL
	};
	for (int i = 0; bundled[i]; i++) {
		char path[512];
		snprintf(path, sizeof(path), "%s%s", g_lib_dir, bundled[i]);
		void *h = dlopen(path, RTLD_NOW | RTLD_GLOBAL | RTLD_NODELETE);
		if (h) LOGI("VulkanShim: loaded bundled: %s", bundled[i]);
		else   LOGE("VulkanShim: bundled load failed %s: %s", bundled[i], dlerror());
	}

    /* 4. Pre-load Turnip while all deps are now satisfied */
    g_turnip = dlopen(g_turnip_path, RTLD_NOW | RTLD_GLOBAL | RTLD_NODELETE);
    if (!g_turnip) {
        LOGE("VulkanShim: Turnip pre-load failed: %s", dlerror()); return;
    }
    LOGI("VulkanShim: Turnip pre-loaded OK");

    /* 5. Load system libvulkan.so — we will GOT patch it */
    g_sys_vulkan = dlopen("libvulkan.so", RTLD_NOW | RTLD_GLOBAL | RTLD_NODELETE);
    if (!g_sys_vulkan) {
        LOGE("VulkanShim: failed to load system libvulkan.so: %s", dlerror()); return;
    }
    LOGI("VulkanShim: system libvulkan.so loaded OK");

    /* 6. Save the real android_load_sphal_library before we patch it away */
    g_real_sphal_load = (void *(*)(const char*, int))
        dlsym(RTLD_DEFAULT, "android_load_sphal_library");
    if (!g_real_sphal_load) {
        LOGE("VulkanShim: cannot find real android_load_sphal_library"); return;
    }
    LOGI("VulkanShim: real sphal = %p", g_real_sphal_load);

    /* 7. Patch libvulkan.so GOT to intercept ICD loading */
    uintptr_t vk_base = find_lib_base("libvulkan.so");
    if (!vk_base) { LOGE("VulkanShim: libvulkan base not found"); return; }
    if (patch_got(vk_base, "android_load_sphal_library", (void*)sphal_hook)) {
        LOGI("VulkanShim: libvulkan GOT patched OK");
    } else {
        LOGE("VulkanShim: libvulkan GOT patch FAILED"); return;
    }

    /* 8. Patch bundled libhardware.so GOT to use the REAL system sphal
          so Turnip's internal gralloc calls go through the proper
          vendor namespace rather than our bundled libvndksupport */
    uintptr_t libhardware_base = find_lib_base("libhardware.so");
    if (libhardware_base) {
        if (patch_got_with_real(libhardware_base,
                                "android_load_sphal_library",
                                (void*)g_real_sphal_load)) {
            LOGI("VulkanShim: libhardware GOT patched OK");
        } else {
            LOGE("VulkanShim: libhardware GOT patch failed");
        }
    }

    /* 9. Patch bundled libvndksupport.so GOT for android_get_exported_namespace
          in case it survived patchelf and still needs the real version */
    uintptr_t vndksupport_base = find_lib_base("libvndksupport.so");
    if (vndksupport_base) {
        void *real_get_ns = dlsym(RTLD_DEFAULT, "android_get_exported_namespace");
        if (real_get_ns) {
            if (patch_got_with_real(vndksupport_base,
                                    "android_get_exported_namespace",
                                    real_get_ns)) {
                LOGI("VulkanShim: libvndksupport namespace fn patched");
            }
        }
    }

    LOGI("VulkanShim: init complete");
}

/* ------------------------------------------------------------------ */
/* Forward all vk* calls to the (now patched) system libvulkan.so     */
/* ------------------------------------------------------------------ */
#define FORWARD(ret, name, args_decl, args_call)          \
ret name args_decl {                                       \
    static ret (*fn) args_decl = NULL;                    \
    if (!fn && g_sys_vulkan)                              \
        fn = (ret (*) args_decl)dlsym(g_sys_vulkan, #name); \
    if (!fn) { LOGE("VulkanShim: " #name " not resolved"); \
               return (ret)0; }                           \
    return fn args_call;                                  \
}

#define FORWARD_VOID(name, args_decl, args_call)          \
void name args_decl {                                     \
    LOGI("VulkanShim: " #name " called");                 \
    static void (*fn) args_decl = NULL;                   \
    if (!fn && g_sys_vulkan)                              \
        fn = (void (*) args_decl)dlsym(g_sys_vulkan, #name); \
    if (fn) fn args_call;                                 \
}

/* For VkResult returning functions */
#define FORWARD_VK(name, args_decl, args_call)            \
VkResult name args_decl {                                 \
    LOGI("VulkanShim: " #name " called");                 \
    static VkResult (*fn) args_decl = NULL;               \
    if (!fn && g_sys_vulkan)                              \
        fn = (VkResult (*) args_decl)dlsym(g_sys_vulkan, #name); \
    if (!fn) { LOGE("VulkanShim: " #name " not resolved"); \
               return -3; } /* VK_ERROR_INITIALIZATION_FAILED */ \
    return fn args_call;                                  \
}

/* For PFN_vkVoidFunction returning functions */
#define FORWARD_PFN(name, args_decl, args_call)           \
PFN_vkVoidFunction name args_decl {                       \
    LOGI("VulkanShim: " #name " called");                 \
    static PFN_vkVoidFunction (*fn) args_decl = NULL;     \
    if (!fn && g_sys_vulkan)                              \
        fn = (PFN_vkVoidFunction (*) args_decl)dlsym(g_sys_vulkan, #name); \
    if (!fn) { LOGE("VulkanShim: " #name " not resolved"); \
               return NULL; }                             \
    return fn args_call;                                  \
}

FORWARD_VK(vkCreateInstance,
    (const void *pCI, const void *pAlloc, VkInstance *pInst),
    (pCI, pAlloc, pInst))

/* Forward declaration — defined below vkGetInstanceProcAddr */
PFN_vkVoidFunction vkGetDeviceProcAddr(VkDevice dev, const char *name);

/* ------------------------------------------------------------------ */
/* Custom vkGetInstanceProcAddr — also intercepts readback commands     */
/* ------------------------------------------------------------------ */
/*
PFN_vkVoidFunction vkGetInstanceProcAddr(VkInstance inst, const char *name) {
    LOGI("VulkanShim: vkGetInstanceProcAddr called");

    static PFN_vkVoidFunction (*real_gipa)(VkInstance, const char*) = NULL;
    if (!real_gipa && g_sys_vulkan)
        real_gipa = (PFN_vkVoidFunction (*)(VkInstance, const char*))
            dlsym(g_sys_vulkan, "vkGetInstanceProcAddr");
    if (!real_gipa) {
        LOGE("VulkanShim: vkGetInstanceProcAddr not resolved");
        return NULL;
    }

    PFN_vkVoidFunction fn = real_gipa(inst, name);

    if (name) {
        if (strcmp(name, "vkCmdPipelineBarrier") == 0 && fn) {
            g_vkCmdPipelineBarrier = (PFN_vkCmdPipelineBarrier)fn;
            LOGI("VulkanShim: captured vkCmdPipelineBarrier (via GIPA) = %p", fn);
        }

        if (strcmp(name, "vkCmdCopyImageToBuffer2") == 0 && fn) {
            g_real_copy_itb2 = (PFN_vkCmdCopyImageToBuffer2)fn;
            LOGI("VulkanShim: intercepting vkCmdCopyImageToBuffer2 (via GIPA) = %p", fn);
            return (PFN_vkVoidFunction)hooked_CmdCopyImageToBuffer2;
        }

        if (strcmp(name, "vkCmdCopyImageToBuffer2KHR") == 0 && fn) {
            if (!g_real_copy_itb2)
                g_real_copy_itb2 = (PFN_vkCmdCopyImageToBuffer2)fn;
            LOGI("VulkanShim: intercepting vkCmdCopyImageToBuffer2KHR (via GIPA) = %p", fn);
            return (PFN_vkVoidFunction)hooked_CmdCopyImageToBuffer2;
        }

        // Return our own vkGetDeviceProcAddr so the interception chain works 
        if (strcmp(name, "vkGetDeviceProcAddr") == 0) {
            LOGI("VulkanShim: returning hooked vkGetDeviceProcAddr");
            return (PFN_vkVoidFunction)vkGetDeviceProcAddr;
        }
    }

    return fn;
}
*/
typedef void (*PFN_vkCmdCopyImageToBuffer)(
    VkCommandBuffer, uint64_t /*VkImage*/, uint32_t /*VkImageLayout*/,
    uint64_t /*VkBuffer*/, uint32_t /*regionCount*/, const void* /*pRegions*/);

static PFN_vkCmdCopyImageToBuffer g_real_copy_itb_v1 = NULL;

static void hooked_CmdCopyImageToBuffer_v1(
    VkCommandBuffer cmdBuf, uint64_t srcImage, uint32_t srcLayout,
    uint64_t dstBuffer, uint32_t regionCount, const void *pRegions)
{
    if (g_vkCmdPipelineBarrier) {
        VkMemoryBarrier memBarrier;
        memBarrier.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        memBarrier.pNext         = NULL;
        memBarrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                   VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                                   VK_ACCESS_SHADER_WRITE_BIT |
                                   VK_ACCESS_MEMORY_WRITE_BIT;
        memBarrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT |
                                   VK_ACCESS_MEMORY_READ_BIT;

        g_vkCmdPipelineBarrier(
            cmdBuf,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 1, &memBarrier, 0, NULL, 0, NULL);
        
        LOGI("VulkanShim: barrier injected before v1 readback");
    }

    g_real_copy_itb_v1(cmdBuf, srcImage, srcLayout, dstBuffer, regionCount, pRegions);
}

/* ------------------------------------------------------------------ */
/* Custom vkGetDeviceProcAddr — intercepts readback commands            */
/* ------------------------------------------------------------------ */
/*
PFN_vkVoidFunction vkGetDeviceProcAddr(VkDevice dev, const char *name) {
    LOGI("VulkanShim: vkGetDeviceProcAddr called for: %s", name ? name : "NULL");

    static PFN_vkVoidFunction (*real_gdpa)(VkDevice, const char*) = NULL;
    if (!real_gdpa && g_sys_vulkan)
        real_gdpa = (PFN_vkVoidFunction (*)(VkDevice, const char*))
            dlsym(g_sys_vulkan, "vkGetDeviceProcAddr");
    if (!real_gdpa) {
        LOGE("VulkanShim: vkGetDeviceProcAddr not resolved");
        return NULL;
    }

    PFN_vkVoidFunction fn = real_gdpa(dev, name);

    if (name) {
        // Capture vkCmdPipelineBarrier when first resolved 
        if (strcmp(name, "vkCmdPipelineBarrier") == 0) {
            g_vkCmdPipelineBarrier = (PFN_vkCmdPipelineBarrier)fn;
            LOGI("VulkanShim: captured vkCmdPipelineBarrier = %p", fn);
        }

        // Capture and intercept vkCmdCopyImageToBuffer2
        if (strcmp(name, "vkCmdCopyImageToBuffer2") == 0 && fn) {
            g_real_copy_itb2 = (PFN_vkCmdCopyImageToBuffer2)fn;
            LOGI("VulkanShim: intercepting vkCmdCopyImageToBuffer2 = %p", fn);
            return (PFN_vkVoidFunction)hooked_CmdCopyImageToBuffer2;
        }

        // Also intercept the KHR variant 
        if (strcmp(name, "vkCmdCopyImageToBuffer2KHR") == 0 && fn) {
            if (!g_real_copy_itb2)
                g_real_copy_itb2 = (PFN_vkCmdCopyImageToBuffer2)fn;
            LOGI("VulkanShim: intercepting vkCmdCopyImageToBuffer2KHR = %p", fn);
            return (PFN_vkVoidFunction)hooked_CmdCopyImageToBuffer2;
        }

        // Also intercept the non-2 variant (older API) 
        if (strcmp(name, "vkCmdCopyImageToBuffer") == 0 && fn) {
            g_real_copy_itb_v1 = (PFN_vkCmdCopyImageToBuffer)fn;
            LOGI("VulkanShim: intercepting vkCmdCopyImageToBuffer (v1) = %p", fn);
            return (PFN_vkVoidFunction)hooked_CmdCopyImageToBuffer_v1;
        }
    }

    return fn;
}
*/
FORWARD_PFN(vkGetInstanceProcAddr,
    (VkInstance inst, const char *name),
    (inst, name))

FORWARD_PFN(vkGetDeviceProcAddr,
    (VkDevice dev, const char *name),
    (dev, name))

FORWARD_VOID(vkDestroyInstance,
    (VkInstance inst, const void *pAlloc),
    (inst, pAlloc))

FORWARD_VK(vkEnumerateInstanceExtensionProperties,
    (const char *pLayer, uint32_t *pCount, void *pProps),
    (pLayer, pCount, pProps))

FORWARD_VK(vkEnumerateInstanceLayerProperties,
    (uint32_t *pCount, void *pProps),
    (pCount, pProps))

FORWARD_VK(vkEnumerateInstanceVersion,
    (uint32_t *pVer),
    (pVer))
	
