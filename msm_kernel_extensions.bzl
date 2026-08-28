load(
    "//vendor/meizu/opensource/kernel-modules:kernel_extensions.bzl",
    _define_combined_vm_image = "define_combined_vm_image",
    _define_extras = "define_extras",
    _define_top_level_rules = "define_top_level_rules",
    _get_build_config_fragments = "get_build_config_fragments",
    _get_dtb_list = "get_dtb_list",
    _get_dtbo_list = "get_dtbo_list",
    _get_dtstree = "get_dtstree",
    _get_gki_ramdisk_prebuilt_binary = "get_gki_ramdisk_prebuilt_binary",
    _get_vendor_ramdisk_binaries = "get_vendor_ramdisk_binaries",
)

define_combined_vm_image = _define_combined_vm_image
define_extras = _define_extras
define_top_level_rules = _define_top_level_rules
get_build_config_fragments = _get_build_config_fragments
get_dtb_list = _get_dtb_list
get_dtbo_list = _get_dtbo_list
get_dtstree = _get_dtstree
get_gki_ramdisk_prebuilt_binary = _get_gki_ramdisk_prebuilt_binary
get_vendor_ramdisk_binaries = _get_vendor_ramdisk_binaries
