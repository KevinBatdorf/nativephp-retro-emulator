// iOS links the cores statically, and a static archive only pulls objects
// something references — a self-registering core TU is referenced by nothing,
// so without this aggregator the linker would drop every core (empty
// registry). ares_create() calls retro_emulator_static_cores(), which names
// each compiled core's link anchor and forces the objects (and their
// Registrars) into the image. Android's modular build has no such problem:
// dlopen loads whole libraries.
extern "C" int retro_emulator_core_sfc_link;
extern "C" int retro_emulator_core_fc_link;
extern "C" int retro_emulator_core_gb_link;
extern "C" int retro_emulator_core_md_link;
extern "C" int retro_emulator_core_gba_link;
extern "C" int retro_emulator_core_n64_link;

extern "C" int retro_emulator_static_cores() {
    return retro_emulator_core_sfc_link
         + retro_emulator_core_fc_link
         + retro_emulator_core_gb_link
         + retro_emulator_core_md_link
         + retro_emulator_core_gba_link
         + retro_emulator_core_n64_link;
}
