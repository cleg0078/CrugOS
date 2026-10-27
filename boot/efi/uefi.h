/* Hand-written UEFI definitions for the CrugOS loader (x86_64, MS ABI). */
#ifndef CRUG_LOADER_UEFI_H
#define CRUG_LOADER_UEFI_H

#include <crug/types.h>
#include <crug/compiler.h>

#define EFIAPI CRUG_MSABI

typedef u64 efi_status;
typedef void *efi_handle;
typedef u64 efi_physical_address;
typedef u16 char16;

#define EFI_SUCCESS 0ull
#define EFI_ERROR(status) (((i64)(status)) < 0)

typedef struct efi_guid {
    u32 data1;
    u16 data2;
    u16 data3;
    u8 data4[8];
} efi_guid;

typedef struct efi_table_header {
    u64 signature;
    u32 revision;
    u32 header_size;
    u32 crc32;
    u32 reserved;
} efi_table_header;

/* ------------------------------- console ------------------------------- */

typedef struct efi_simple_text_output efi_simple_text_output;
typedef struct efi_simple_text_input efi_simple_text_input;
typedef struct efi_runtime_services efi_runtime_services;

struct efi_simple_text_output {
    efi_status(EFIAPI *reset)(efi_simple_text_output *self, u8 extended);
    efi_status(EFIAPI *output_string)(efi_simple_text_output *self, char16 *string);
    efi_status(EFIAPI *test_string)(efi_simple_text_output *self, char16 *string);
    efi_status(EFIAPI *query_mode)(efi_simple_text_output *self, usize mode,
                                   usize *columns, usize *rows);
    efi_status(EFIAPI *set_mode)(efi_simple_text_output *self, usize mode);
    efi_status(EFIAPI *set_attribute)(efi_simple_text_output *self, usize attribute);
    efi_status(EFIAPI *clear_screen)(efi_simple_text_output *self);
    efi_status(EFIAPI *set_cursor_position)(efi_simple_text_output *self, usize column,
                                            usize row);
    efi_status(EFIAPI *enable_cursor)(efi_simple_text_output *self, u8 visible);
    void *mode;
};

/* ------------------------------- memory -------------------------------- */

typedef struct efi_memory_descriptor {
    u32 type;
    u32 pad;
    efi_physical_address physical_start;
    u64 virtual_start;
    u64 number_of_pages;
    u64 attribute;
} efi_memory_descriptor;

typedef enum efi_allocate_type {
    EFI_ALLOCATE_ANY_PAGES = 0,
    EFI_ALLOCATE_MAX_ADDRESS = 1,
    EFI_ALLOCATE_ADDRESS = 2,
} efi_allocate_type;

typedef enum efi_memory_type {
    EFI_RESERVED_MEMORY_TYPE = 0,
    EFI_LOADER_CODE = 1,
    EFI_LOADER_DATA = 2,
    EFI_BOOT_SERVICES_CODE = 3,
    EFI_BOOT_SERVICES_DATA = 4,
    EFI_RUNTIME_SERVICES_CODE = 5,
    EFI_RUNTIME_SERVICES_DATA = 6,
    EFI_CONVENTIONAL_MEMORY = 7,
    EFI_UNUSABLE_MEMORY = 8,
    EFI_ACPI_RECLAIM_MEMORY = 9,
    EFI_ACPI_MEMORY_NVS = 10,
    EFI_MEMORY_MAPPED_IO = 11,
    EFI_MEMORY_MAPPED_IO_PORT_SPACE = 12,
    EFI_PAL_CODE = 13,
    EFI_PERSISTENT_MEMORY = 14,
} efi_memory_type;

/* ---------------------------- boot services ---------------------------- */

typedef struct efi_boot_services {
    efi_table_header hdr;

    efi_status(EFIAPI *raise_tpl)(usize new_tpl);
    void(EFIAPI *restore_tpl)(usize old_tpl);
    efi_status(EFIAPI *allocate_pages)(u32 type, u32 memory_type, u64 pages,
                                       efi_physical_address *memory);
    efi_status(EFIAPI *free_pages)(efi_physical_address memory, u64 pages);
    efi_status(EFIAPI *get_memory_map)(u64 *map_size, efi_memory_descriptor *map,
                                       u64 *map_key, u64 *descriptor_size,
                                       u32 *descriptor_version);
    efi_status(EFIAPI *allocate_pool)(u32 memory_type, u64 size, void **buffer);
    efi_status(EFIAPI *free_pool)(void *buffer);
    efi_status(EFIAPI *create_event)(u32 type, usize notify_tpl, void *notify_function,
                                     void *notify_context, void **event);
    efi_status(EFIAPI *set_timer)(void *event, u32 type, u64 trigger_time);
    efi_status(EFIAPI *wait_for_event)(usize number_of_events, void **event,
                                       usize *index);
    efi_status(EFIAPI *signal_event)(void *event);
    efi_status(EFIAPI *close_event)(void *event);
    efi_status(EFIAPI *check_event)(void *event);
    efi_status(EFIAPI *install_protocol_interface)(efi_handle *handle, efi_guid *protocol,
                                                  u32 interface_type, void *interface);
    efi_status(EFIAPI *reinstall_protocol_interface)(efi_handle handle, efi_guid *protocol,
                                                    void *old_interface,
                                                    void *new_interface);
    efi_status(EFIAPI *uninstall_protocol_interface)(efi_handle handle, efi_guid *protocol,
                                                    void *interface);
    efi_status(EFIAPI *handle_protocol)(efi_handle handle, efi_guid *protocol,
                                        void **interface);
    void *reserved;
    efi_status(EFIAPI *register_protocol_notify)(efi_guid *protocol, void *event,
                                                void **registration);
    efi_status(EFIAPI *locate_handle)(u32 search_type, efi_guid *protocol,
                                      void *search_key, usize *buffer_size,
                                      efi_handle *buffer);
    efi_status(EFIAPI *locate_device_path)(efi_guid *protocol, efi_handle **device_path,
                                           efi_handle device);
    efi_status(EFIAPI *install_configuration_table)(efi_guid *guid, void *table);
    efi_status(EFIAPI *load_image)(u8 boot_policy, efi_handle parent_image_handle,
                                   void *device_path, void *source_buffer,
                                   usize source_size, efi_handle *image_handle);
    efi_status(EFIAPI *start_image)(efi_handle image_handle, usize *exit_data_size,
                                    char16 **exit_data);
    efi_status(EFIAPI *exit)(efi_handle image_handle, efi_status exit_status,
                             usize exit_data_size, char16 *exit_data);
    efi_status(EFIAPI *unload_image)(efi_handle image_handle);
    efi_status(EFIAPI *exit_boot_services)(efi_handle image_handle, usize map_key);
    efi_status(EFIAPI *get_next_monotonic_count)(u64 *count);
    efi_status(EFIAPI *stall)(usize microseconds);
    efi_status(EFIAPI *set_watchdog_timer)(usize timeout, u64 watchdog_code,
                                           usize data_size, char16 *watchdog_data);
    void *connect_controller;
    void *disconnect_controller;
    void *open_protocol;
    void *close_protocol;
    void *open_protocol_information;
    void *protocols_per_handle;
    void *locate_handle_buffer;
    efi_status(EFIAPI *locate_protocol)(efi_guid *protocol, void *registration,
                                        void **interface);
    void *install_multiple_protocol_interfaces;
    void *uninstall_multiple_protocol_interfaces;
    void *calculate_crc32;
    void(EFIAPI *copy_mem)(void *destination, void *source, usize length);
    void(EFIAPI *set_mem)(void *buffer, usize size, u8 value);
    void *create_event_ex;
} efi_boot_services;

typedef struct efi_configuration_table {
    efi_guid vendor_guid;
    void *vendor_table;
} efi_configuration_table;

typedef struct efi_system_table {
    efi_table_header hdr;
    char16 *firmware_vendor;
    u32 firmware_revision;
    u32 pad;
    efi_handle console_in_handle;
    efi_simple_text_input *con_in;
    efi_handle console_out_handle;
    efi_simple_text_output *con_out;
    efi_handle standard_error_handle;
    efi_simple_text_output *std_err;
    efi_runtime_services *runtime_services;
    efi_boot_services *boot_services;
    u64 number_of_table_entries;
    efi_configuration_table *configuration_table;
} efi_system_table;

/* -------------------------------- files -------------------------------- */

typedef struct efi_file_protocol efi_file_protocol;
typedef struct efi_simple_file_system efi_simple_file_system;

struct efi_file_protocol {
    u64 revision;
    efi_status(EFIAPI *open)(efi_file_protocol *self, efi_file_protocol **new_handle,
                              char16 *file_name, u64 open_mode, u64 attributes);
    efi_status(EFIAPI *close)(efi_file_protocol *self);
    efi_status(EFIAPI *delete)(efi_file_protocol *self);
    efi_status(EFIAPI *read)(efi_file_protocol *self, u64 *buffer_size, void *buffer);
    efi_status(EFIAPI *write)(efi_file_protocol *self, u64 *buffer_size, void *buffer);
    efi_status(EFIAPI *get_position)(efi_file_protocol *self, u64 *position);
    efi_status(EFIAPI *set_position)(efi_file_protocol *self, u64 position);
    efi_status(EFIAPI *get_info)(efi_file_protocol *self, efi_guid *information_type,
                                 u64 *buffer_size, void *buffer);
    efi_status(EFIAPI *set_info)(efi_file_protocol *self, efi_guid *information_type,
                                 u64 buffer_size, void *buffer);
    efi_status(EFIAPI *flush)(efi_file_protocol *self);
};

struct efi_simple_file_system {
    u64 revision;
    efi_status(EFIAPI *open_volume)(efi_simple_file_system *self,
                                     efi_file_protocol **root);
};

typedef struct efi_time {
    u16 year;
    u16 month;
    u16 day;
    u16 hour;
    u16 minute;
    u16 second;
    u8 pad1;
    u32 nanosecond;
    i16 timezone;
    u8 daylight;
    u8 pad2;
} efi_time;

typedef struct efi_file_info {
    u64 size;
    u64 file_size;
    u64 physical_size;
    efi_time create_time;
    efi_time last_access_time;
    efi_time modification_time;
    u64 attribute;
    char16 file_name[1];
} efi_file_info;

typedef struct efi_loaded_image_protocol {
    u32 revision;
    efi_handle parent_handle;
    efi_system_table *system_table;
    efi_handle device_handle;
    void *file_path;
    void *reserved;
    u32 load_options_size;
    void *load_options;
    void *image_base;
    u64 image_size;
    u32 image_code_type;
    u32 image_data_type;
    void *unload;
} efi_loaded_image_protocol;

/* ----------------------------- graphics/RNG ---------------------------- */

typedef struct efi_gop_mode_info {
    u32 version;
    u32 horizontal_resolution;
    u32 vertical_resolution;
    u32 pixel_format;
    u8 pixel_information[16];
    u32 pixels_per_scan_line;
} efi_gop_mode_info;

typedef struct efi_gop_mode {
    u32 max_mode;
    u32 mode;
    efi_gop_mode_info *info;
    u64 size_of_info;
    efi_physical_address framebuffer_base;
    u64 framebuffer_size;
} efi_gop_mode;

struct efi_graphics_output_protocol;
struct efi_rng_protocol;

typedef struct efi_graphics_output_protocol {
    efi_status(EFIAPI *query_mode)(struct efi_graphics_output_protocol *self, u32 mode_number,
                                   u64 *size_of_info, efi_gop_mode_info **info);
    efi_status(EFIAPI *set_mode)(struct efi_graphics_output_protocol *self, u32 mode_number);
    efi_status(EFIAPI *blt)(struct efi_graphics_output_protocol *self, void *blt_buffer,
                            u32 blt_operation, usize source_x, usize source_y,
                            usize destination_x, usize destination_y, usize width,
                            usize height, usize delta);
    efi_gop_mode *mode;
} efi_graphics_output_protocol;

typedef struct efi_rng_protocol {
    u64 revision;
    efi_status(EFIAPI *get_info)(struct efi_rng_protocol *self, u64 *algorithm_list_size,
                                 efi_guid *algorithm_list);
    efi_status(EFIAPI *get_rng)(struct efi_rng_protocol *self, efi_guid *algorithm,
                                u64 value_length, u8 *value);
} efi_rng_protocol;

/* ------------------------------- protocol GUIDs ------------------------ */

extern const efi_guid g_guid_loaded_image;
extern const efi_guid g_guid_simple_file_system;
extern const efi_guid g_guid_file_info;
extern const efi_guid g_guid_graphics_output;
extern const efi_guid g_guid_rng;
extern const efi_guid g_guid_acpi2_table;
extern const efi_guid g_guid_acpi1_table;

#endif /* CRUG_LOADER_UEFI_H */
