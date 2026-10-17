#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "recomp.h"
#include "librecomp/helpers.hpp"

// Diagnostic logging, called from a function hook placed at the start of the game's task delete function
// (see [[patches.hook]] in mnsg_jp.toml). Prints the identity and position of every deleted task that owns an object.
extern "C" void goemon_diag_task_delete(uint8_t* rdram, recomp_context* ctx) {
    gpr task = ctx->r4;
    if (task == 0) {
        return;
    }

    uint32_t task_addr = (uint32_t)task;
    uint32_t obj = (uint32_t)MEM_W(0x18, task);
    uint16_t file_id = (uint16_t)MEM_HU(0x28, task);
    uint32_t func = (uint32_t)MEM_W(0xC, task);
    uint32_t ovl_vram = (uint32_t)MEM_W(0x2C, task);

    if (file_id == 0 || obj < 0x80000000u || obj >= 0x80800000u) {
        return;
    }

    gpr objp = (gpr)(int32_t)obj;
    float pos[3];
    for (int i = 0; i < 3; i++) {
        int32_t raw = MEM_W(0x8 + i * 4, objp);
        std::memcpy(&pos[i], &raw, sizeof(float));
    }

    uint32_t gfx1 = (uint32_t)MEM_W(0x2C, objp);
    uint32_t gfx2 = (uint32_t)MEM_W(0x30, objp);

    std::printf("[diag] task_delete task=%08X file=%u func=%08X ovl_vram=%08X pos=(%.1f, %.1f, %.1f) gfx=%08X/%08X models=",
        task_addr, file_id, func, ovl_vram, pos[0], pos[1], pos[2], gfx1, gfx2);
    for (int i = 0; i < 6; i++) {
        std::printf("%u:%08X ", (uint16_t)MEM_HU(0x34 + i * 8, objp), (uint32_t)MEM_W(0x34 + i * 8 + 4, objp));
    }
    std::printf("\n");
    std::fflush(stdout);

    // GOEMON_DIAG_DUMP_MODEL=<file id>: snapshot the first 8 MB of RDRAM once when an object using that model file is deleted.
    static bool dumped = false;
    const char* want = std::getenv("GOEMON_DIAG_DUMP_MODEL");
    if (!dumped && want != nullptr && (uint16_t)MEM_HU(0x34, objp) == (uint16_t)std::atoi(want)) {
        dumped = true;
        if (FILE* f = std::fopen("goemon_diag_rdram.bin", "wb")) {
            std::fwrite(rdram, 1, 0x800000, f);
            std::fclose(f);
            std::printf("[diag] wrote goemon_diag_rdram.bin (obj=%08X)\n", obj);
            std::fflush(stdout);
        }
    }
}
