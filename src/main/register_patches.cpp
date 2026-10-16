#include "ovl_patches.hpp"

#ifdef GOEMON_REGION_JP
#include "../../RecompiledPatchesJP/patches_bin.h"
#include "../../RecompiledPatchesJP/recomp_overlays.inl"
#else
#include "../../RecompiledPatches/patches_bin.h"
#include "../../RecompiledPatches/recomp_overlays.inl"
#endif

#include "librecomp/overlays.hpp"
#include "librecomp/game.hpp"

void goemon64::register_patches() {
    recomp::overlays::register_patches(mm_patches_bin, sizeof(mm_patches_bin), section_table, ARRLEN(section_table));
    recomp::overlays::register_base_exports(export_table);
    recomp::overlays::register_base_events(event_names);
    recomp::overlays::register_manual_patch_symbols(manual_patch_symbols);
}
