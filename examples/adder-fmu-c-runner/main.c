#define _POSIX_C_SOURCE 200809L

#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <wasmtime.h>

static const char *WORLD_COSIM = "fmi:fmi3/co-simulation@3.0.0";
static const char *TARGET_PLATFORM = "wasm32-wasip2";

typedef struct {
    char model_identifier[256];
    uint32_t vr_time;
    uint32_t vr_input_a;
    uint32_t vr_input_b;
    uint32_t vr_sum;
} fmu_metadata_t;

static void print_usage(const char *argv0) {
    fprintf(stderr, "Usage: %s <path/to/adder-fmu.fmu>\n", argv0);
}

static void print_wasmtime_error(const char *context, wasmtime_error_t *error) {
    wasm_name_t msg;
    wasm_name_new_empty(&msg);
    wasmtime_error_message(error, &msg);
    fprintf(stderr, "%s: %.*s\n", context, (int)msg.size, msg.data);
    wasm_byte_vec_delete(&msg);
    wasmtime_error_delete(error);
}

static bool run_command(char *const argv[]) {
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return false;
    }
    if (pid == 0) {
        execvp(argv[0], argv);
        perror("execvp");
        _exit(127);
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        perror("waitpid");
        return false;
    }

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "command failed: %s (status=%d)\n", argv[0], status);
        return false;
    }

    return true;
}

static bool read_text_file(const char *path, char **data_out) {
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        perror("fopen");
        return false;
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return false;
    }

    long len = ftell(f);
    if (len < 0) {
        fclose(f);
        return false;
    }

    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return false;
    }

    char *buf = (char *)malloc((size_t)len + 1);
    if (buf == NULL) {
        fclose(f);
        return false;
    }

    if (fread(buf, 1, (size_t)len, f) != (size_t)len) {
        fclose(f);
        free(buf);
        return false;
    }

    buf[len] = '\0';
    fclose(f);
    *data_out = buf;
    return true;
}

static const char *find_tag_end(const char *tag_start) {
    return strchr(tag_start, '>');
}

static bool parse_xml_attribute(const char *tag_start,
                                const char *tag_end,
                                const char *attr_name,
                                char *value_out,
                                size_t value_out_len) {
    size_t attr_len = strlen(attr_name);
    const char *attr = strstr(tag_start, attr_name);
    if (attr == NULL || attr >= tag_end) {
        return false;
    }

    const char *scan = attr + attr_len;
    while (scan < tag_end && (*scan == ' ' || *scan == '\t' || *scan == '\r' || *scan == '\n')) {
        ++scan;
    }
    if (scan >= tag_end || *scan != '=') {
        return false;
    }

    ++scan;
    while (scan < tag_end && (*scan == ' ' || *scan == '\t' || *scan == '\r' || *scan == '\n')) {
        ++scan;
    }
    if (scan >= tag_end || (*scan != '"' && *scan != '\'')) {
        return false;
    }

    char quote = *scan++;
    const char *value_start = scan;
    const char *value_end = memchr(value_start, quote, (size_t)(tag_end - value_start));
    if (value_end == NULL) {
        return false;
    }

    size_t value_len = (size_t)(value_end - value_start);
    if (value_len + 1 > value_out_len) {
        return false;
    }

    memcpy(value_out, value_start, value_len);
    value_out[value_len] = '\0';
    return true;
}

static bool parse_u32_attribute(const char *tag_start,
                                const char *tag_end,
                                const char *attr_name,
                                uint32_t *value_out) {
    char buffer[32];
    if (!parse_xml_attribute(tag_start, tag_end, attr_name, buffer, sizeof(buffer))) {
        return false;
    }

    char *end = NULL;
    unsigned long value = strtoul(buffer, &end, 10);
    if (end == buffer || *end != '\0' || value > UINT32_MAX) {
        return false;
    }

    *value_out = (uint32_t)value;
    return true;
}

static bool parse_model_description(const char *fmu_dir, fmu_metadata_t *metadata_out) {
    char model_description_path[4096];
    snprintf(model_description_path, sizeof(model_description_path), "%s/modelDescription.xml", fmu_dir);

    char *xml = NULL;
    if (!read_text_file(model_description_path, &xml)) {
        fprintf(stderr, "Failed to read %s\n", model_description_path);
        return false;
    }

    const char *cosim_tag = strstr(xml, "<CoSimulation");
    if (cosim_tag == NULL) {
        fprintf(stderr, "Missing CoSimulation element in modelDescription.xml\n");
        free(xml);
        return false;
    }
    const char *cosim_end = find_tag_end(cosim_tag);
    if (cosim_end == NULL) {
        fprintf(stderr, "Malformed CoSimulation element in modelDescription.xml\n");
        free(xml);
        return false;
    }
    if (!parse_xml_attribute(cosim_tag, cosim_end, "modelIdentifier", metadata_out->model_identifier,
                             sizeof(metadata_out->model_identifier))) {
        fprintf(stderr, "Missing CoSimulation modelIdentifier in modelDescription.xml\n");
        free(xml);
        return false;
    }

    const char *vars_start = strstr(xml, "<ModelVariables");
    const char *vars_end = strstr(xml, "</ModelVariables>");
    if (vars_start == NULL || vars_end == NULL || vars_end <= vars_start) {
        fprintf(stderr, "Missing ModelVariables element in modelDescription.xml\n");
        free(xml);
        return false;
    }

    bool have_time = false;
    bool have_input_a = false;
    bool have_input_b = false;
    bool have_sum = false;

    const char *cursor = vars_start;
    while ((cursor = strstr(cursor, "<Float64")) != NULL && cursor < vars_end) {
        const char *tag_end = find_tag_end(cursor);
        if (tag_end == NULL || tag_end > vars_end) {
            fprintf(stderr, "Malformed Float64 variable entry in modelDescription.xml\n");
            free(xml);
            return false;
        }

        char name[128];
        uint32_t value_reference = 0;
        if (!parse_xml_attribute(cursor, tag_end, "name", name, sizeof(name)) ||
            !parse_u32_attribute(cursor, tag_end, "valueReference", &value_reference)) {
            fprintf(stderr, "Failed to parse Float64 variable entry in modelDescription.xml\n");
            free(xml);
            return false;
        }

        if (strcmp(name, "time") == 0) {
            if (have_time) {
                fprintf(stderr, "Duplicate variable 'time' in modelDescription.xml\n");
                free(xml);
                return false;
            }
            metadata_out->vr_time = value_reference;
            have_time = true;
        } else if (strcmp(name, "input_a") == 0) {
            if (have_input_a) {
                fprintf(stderr, "Duplicate variable 'input_a' in modelDescription.xml\n");
                free(xml);
                return false;
            }
            metadata_out->vr_input_a = value_reference;
            have_input_a = true;
        } else if (strcmp(name, "input_b") == 0) {
            if (have_input_b) {
                fprintf(stderr, "Duplicate variable 'input_b' in modelDescription.xml\n");
                free(xml);
                return false;
            }
            metadata_out->vr_input_b = value_reference;
            have_input_b = true;
        } else if (strcmp(name, "sum") == 0) {
            if (have_sum) {
                fprintf(stderr, "Duplicate variable 'sum' in modelDescription.xml\n");
                free(xml);
                return false;
            }
            metadata_out->vr_sum = value_reference;
            have_sum = true;
        }

        cursor = tag_end + 1;
    }

    free(xml);

    if (!have_time || !have_input_a || !have_input_b || !have_sum) {
        fprintf(stderr, "Missing one or more required variables in modelDescription.xml\n");
        return false;
    }

    return true;
}

static bool extract_fmu_to_temp(const char *fmu_path, char *temp_dir_out, size_t temp_dir_out_len) {
    const char *template_str = "/tmp/adder-fmu-c-runner-XXXXXX";
    if (strlen(template_str) + 1 > temp_dir_out_len) {
        return false;
    }

    strcpy(temp_dir_out, template_str);
    if (mkdtemp(temp_dir_out) == NULL) {
        perror("mkdtemp");
        return false;
    }

    char *const unzip_argv[] = {
        "unzip",
        "-qq",
        "-o",
        (char *)fmu_path,
        "-d",
        temp_dir_out,
        NULL,
    };

    return run_command(unzip_argv);
}

static bool find_wasm_in_fmu(const char *fmu_dir,
                             const fmu_metadata_t *metadata,
                             char *wasm_path_out,
                             size_t wasm_path_out_len) {
    char wasm_dir[4096];
    snprintf(wasm_dir, sizeof(wasm_dir), "%s/binaries/%s", fmu_dir, TARGET_PLATFORM);

    struct stat st;
    if (stat(wasm_dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "Missing directory: %s\n", wasm_dir);
        return false;
    }

    int written = snprintf(wasm_path_out, wasm_path_out_len, "%s/%s.wasm", wasm_dir,
                           metadata->model_identifier);
    if (written < 0 || (size_t)written >= wasm_path_out_len) {
        fprintf(stderr, "Wasm path too long for modelIdentifier '%s'\n", metadata->model_identifier);
        return false;
    }

    if (access(wasm_path_out, R_OK) != 0) {
        fprintf(stderr, "Missing wasm component: %s\n", wasm_path_out);
        return false;
    }

    return true;
}

static bool read_binary_file(const char *path, uint8_t **data_out, size_t *len_out) {
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        perror("fopen");
        return false;
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return false;
    }

    long len = ftell(f);
    if (len < 0) {
        fclose(f);
        return false;
    }

    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return false;
    }

    uint8_t *buf = (uint8_t *)malloc((size_t)len);
    if (buf == NULL) {
        fclose(f);
        return false;
    }

    if (fread(buf, 1, (size_t)len, f) != (size_t)len) {
        fclose(f);
        free(buf);
        return false;
    }

    fclose(f);
    *data_out = buf;
    *len_out = (size_t)len;
    return true;
}

static bool name_equals(const wasm_name_t *name, const char *value) {
    size_t len = strlen(value);
    return name->size == len && memcmp(name->data, value, len) == 0;
}

static bool status_is_ok(const wasmtime_component_val_t *status_val) {
    return status_val->kind == WASMTIME_COMPONENT_ENUM && name_equals(&status_val->of.enumeration, "ok");
}

static bool get_component_func(const wasmtime_component_instance_t *instance,
                               wasmtime_context_t *ctx,
                               const char *iface,
                               const char *func_name,
                               wasmtime_component_func_t *out) {
    wasmtime_component_export_index_t *iface_idx =
        wasmtime_component_instance_get_export_index(instance, ctx, NULL, iface, strlen(iface));
    if (iface_idx == NULL) {
        fprintf(stderr, "Missing interface export: %s\n", iface);
        return false;
    }

    wasmtime_component_export_index_t *func_idx =
        wasmtime_component_instance_get_export_index(instance, ctx, iface_idx, func_name, strlen(func_name));
    wasmtime_component_export_index_delete(iface_idx);
    if (func_idx == NULL) {
        fprintf(stderr, "Missing function export under %s: %s\n", iface, func_name);
        return false;
    }

    bool found = wasmtime_component_instance_get_func(instance, ctx, func_idx, out);
    wasmtime_component_export_index_delete(func_idx);
    if (!found) {
        fprintf(stderr, "Export is not a function: %s.%s\n", iface, func_name);
    }
    return found;
}

static bool call_component_func(const char *label,
                                const wasmtime_component_func_t *func,
                                wasmtime_context_t *ctx,
                                wasmtime_component_val_t *args,
                                size_t nargs,
                                wasmtime_component_val_t *results,
                                size_t nresults) {
    wasmtime_error_t *error =
        wasmtime_component_func_call(func, ctx, args, nargs, results, nresults);
    if (error != NULL) {
        print_wasmtime_error(label, error);
        return false;
    }
    return true;
}

static wasmtime_component_val_t make_string_val(const char *value) {
    wasmtime_component_val_t v;
    memset(&v, 0, sizeof(v));
    v.kind = WASMTIME_COMPONENT_STRING;
    wasm_name_new(&v.of.string, strlen(value), value);
    return v;
}

static wasmtime_component_val_t make_bool_val(bool value) {
    wasmtime_component_val_t v;
    memset(&v, 0, sizeof(v));
    v.kind = WASMTIME_COMPONENT_BOOL;
    v.of.boolean = value;
    return v;
}

static wasmtime_component_val_t make_f64_val(double value) {
    wasmtime_component_val_t v;
    memset(&v, 0, sizeof(v));
    v.kind = WASMTIME_COMPONENT_F64;
    v.of.f64 = value;
    return v;
}

static wasmtime_component_val_t make_u32_list_val(const uint32_t *values, size_t count) {
    wasmtime_component_val_t v;
    memset(&v, 0, sizeof(v));
    v.kind = WASMTIME_COMPONENT_LIST;

    wasmtime_component_val_t *items = (wasmtime_component_val_t *)calloc(count, sizeof(wasmtime_component_val_t));
    for (size_t i = 0; i < count; ++i) {
        items[i].kind = WASMTIME_COMPONENT_U32;
        items[i].of.u32 = values[i];
    }
    wasmtime_component_vallist_new(&v.of.list, count, items);
    free(items);
    return v;
}

static wasmtime_component_val_t make_f64_list_val(const double *values, size_t count) {
    wasmtime_component_val_t v;
    memset(&v, 0, sizeof(v));
    v.kind = WASMTIME_COMPONENT_LIST;

    wasmtime_component_val_t *items = (wasmtime_component_val_t *)calloc(count, sizeof(wasmtime_component_val_t));
    for (size_t i = 0; i < count; ++i) {
        items[i].kind = WASMTIME_COMPONENT_F64;
        items[i].of.f64 = values[i];
    }
    wasmtime_component_vallist_new(&v.of.list, count, items);
    free(items);
    return v;
}

static wasmtime_component_val_t make_none_option_val(void) {
    wasmtime_component_val_t v;
    memset(&v, 0, sizeof(v));
    v.kind = WASMTIME_COMPONENT_OPTION;
    v.of.option = NULL;
    return v;
}

static wasmtime_component_val_t make_resource_val(const wasmtime_component_resource_any_t *resource) {
    wasmtime_component_val_t v;
    memset(&v, 0, sizeof(v));
    v.kind = WASMTIME_COMPONENT_RESOURCE;
    v.of.resource = wasmtime_component_resource_any_clone(resource);
    return v;
}

static void cleanup_vals(wasmtime_component_val_t *vals, size_t nvals) {
    for (size_t i = 0; i < nvals; ++i) {
        wasmtime_component_val_delete(&vals[i]);
    }
}

static bool parse_do_step_result(const wasmtime_component_val_t *result_val,
                                 double *last_successful_time,
                                 bool *terminate_simulation,
                                 bool *early_return) {
    if (result_val->kind != WASMTIME_COMPONENT_RESULT || !result_val->of.result.is_ok ||
        result_val->of.result.val == NULL) {
        return false;
    }

    const wasmtime_component_val_t *payload = result_val->of.result.val;
    if (payload->kind != WASMTIME_COMPONENT_RECORD) {
        return false;
    }

    bool have_last = false;
    bool have_term = false;
    bool have_early = false;

    for (size_t i = 0; i < payload->of.record.size; ++i) {
        const wasmtime_component_valrecord_entry_t *entry = &payload->of.record.data[i];
        if (name_equals(&entry->name, "last-successful-time") && entry->val.kind == WASMTIME_COMPONENT_F64) {
            *last_successful_time = entry->val.of.f64;
            have_last = true;
        } else if (name_equals(&entry->name, "terminate-simulation") &&
                   entry->val.kind == WASMTIME_COMPONENT_BOOL) {
            *terminate_simulation = entry->val.of.boolean;
            have_term = true;
        } else if (name_equals(&entry->name, "early-return") && entry->val.kind == WASMTIME_COMPONENT_BOOL) {
            *early_return = entry->val.of.boolean;
            have_early = true;
        }
    }

    return have_last && have_term && have_early;
}

static bool parse_get_float64_result(const wasmtime_component_val_t *result_val,
                                     double *time_out,
                                     double *sum_out) {
    if (result_val->kind != WASMTIME_COMPONENT_RESULT || !result_val->of.result.is_ok ||
        result_val->of.result.val == NULL) {
        return false;
    }

    const wasmtime_component_val_t *payload = result_val->of.result.val;
    if (payload->kind != WASMTIME_COMPONENT_LIST || payload->of.list.size != 2) {
        return false;
    }

    if (payload->of.list.data[0].kind != WASMTIME_COMPONENT_F64 ||
        payload->of.list.data[1].kind != WASMTIME_COMPONENT_F64) {
        return false;
    }

    *time_out = payload->of.list.data[0].of.f64;
    *sum_out = payload->of.list.data[1].of.f64;
    return true;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        print_usage(argv[0]);
        return 2;
    }

    const char *fmu_path = argv[1];
    char tmp_dir[128];
    char wasm_path[4096];
    fmu_metadata_t metadata;
    memset(&metadata, 0, sizeof(metadata));

    if (!extract_fmu_to_temp(fmu_path, tmp_dir, sizeof(tmp_dir))) {
        fprintf(stderr, "Failed to extract FMU archive: %s\n", fmu_path);
        return 1;
    }

    if (!parse_model_description(tmp_dir, &metadata)) {
        fprintf(stderr, "Failed to parse modelDescription.xml from %s\n", fmu_path);
        return 1;
    }

    if (!find_wasm_in_fmu(tmp_dir, &metadata, wasm_path, sizeof(wasm_path))) {
        fprintf(stderr, "No wasm binary found under %s/binaries/%s\n", tmp_dir, TARGET_PLATFORM);
        return 1;
    }

    printf("Extracted FMU to '%s'\n", tmp_dir);
    printf("Loading wasm component: '%s'\n", wasm_path);

    uint8_t *wasm_bytes = NULL;
    size_t wasm_len = 0;
    if (!read_binary_file(wasm_path, &wasm_bytes, &wasm_len)) {
        fprintf(stderr, "Failed to read wasm component bytes\n");
        return 1;
    }

    wasm_config_t *cfg = wasm_config_new();
    wasmtime_config_wasm_component_model_set(cfg, true);

    wasm_engine_t *engine = wasm_engine_new_with_config(cfg);
    wasmtime_store_t *store = wasmtime_store_new(engine, NULL, NULL);
    wasmtime_context_t *ctx = wasmtime_store_context(store);

    wasi_config_t *wasi = wasi_config_new();
    wasi_config_inherit_stdout(wasi);
    wasi_config_inherit_stderr(wasi);
    wasi_config_inherit_stdin(wasi);

    wasmtime_error_t *error = wasmtime_context_set_wasi(ctx, wasi);
    if (error != NULL) {
        print_wasmtime_error("Failed to configure WASI", error);
        free(wasm_bytes);
        wasmtime_store_delete(store);
        wasm_engine_delete(engine);
        return 1;
    }

    wasmtime_component_t *component = NULL;
    error = wasmtime_component_new(engine, wasm_bytes, wasm_len, &component);
    if (error != NULL) {
        print_wasmtime_error("Failed to compile component", error);
        free(wasm_bytes);
        wasmtime_store_delete(store);
        wasm_engine_delete(engine);
        return 1;
    }

    free(wasm_bytes);

    wasmtime_component_linker_t *linker = wasmtime_component_linker_new(engine);
    error = wasmtime_component_linker_add_wasip2(linker);
    if (error != NULL) {
        print_wasmtime_error("Failed to add WASIp2 imports", error);
        wasmtime_component_linker_delete(linker);
        wasmtime_component_delete(component);
        wasmtime_store_delete(store);
        wasm_engine_delete(engine);
        return 1;
    }

    wasmtime_component_instance_t instance;
    error = wasmtime_component_linker_instantiate(linker, ctx, component, &instance);
    if (error != NULL) {
        print_wasmtime_error("Failed to instantiate component", error);
        wasmtime_component_linker_delete(linker);
        wasmtime_component_delete(component);
        wasmtime_store_delete(store);
        wasm_engine_delete(engine);
        return 1;
    }

    wasmtime_component_func_t instantiate_func;
    wasmtime_component_func_t enter_init_func;
    wasmtime_component_func_t set_float64_func;
    wasmtime_component_func_t exit_init_func;
    wasmtime_component_func_t do_step_func;
    wasmtime_component_func_t get_float64_func;
    wasmtime_component_func_t terminate_func;

    if (!get_component_func(&instance, ctx, WORLD_COSIM,
                            "[static]co-simulation-instance.instantiate-co-simulation",
                            &instantiate_func) ||
        !get_component_func(&instance, ctx, WORLD_COSIM,
                            "[method]co-simulation-instance.enter-initialization-mode",
                            &enter_init_func) ||
        !get_component_func(&instance, ctx, WORLD_COSIM,
                            "[method]co-simulation-instance.set-float64",
                            &set_float64_func) ||
        !get_component_func(&instance, ctx, WORLD_COSIM,
                            "[method]co-simulation-instance.exit-initialization-mode",
                            &exit_init_func) ||
        !get_component_func(&instance, ctx, WORLD_COSIM,
                            "[method]co-simulation-instance.do-step",
                            &do_step_func) ||
        !get_component_func(&instance, ctx, WORLD_COSIM,
                            "[method]co-simulation-instance.get-float64",
                            &get_float64_func) ||
        !get_component_func(&instance, ctx, WORLD_COSIM,
                            "[method]co-simulation-instance.terminate",
                            &terminate_func)) {
        wasmtime_component_linker_delete(linker);
        wasmtime_component_delete(component);
        wasmtime_store_delete(store);
        wasm_engine_delete(engine);
        return 1;
    }

    const double step_size = 0.1;
    const size_t n_steps = 50;
    const double tolerance = 1e-10;

    printf("Running adder-fmu from t=0.0 to t=%.1f s in %.1f s steps...\n", n_steps * step_size,
           step_size);

    wasmtime_component_val_t instantiate_args[8];
    instantiate_args[0] = make_string_val("adder-1");
    instantiate_args[1] = make_string_val("");
    instantiate_args[2] = make_string_val("");
    instantiate_args[3] = make_bool_val(false);
    instantiate_args[4] = make_bool_val(false);
    instantiate_args[5] = make_bool_val(false);
    instantiate_args[6] = make_bool_val(false);
    instantiate_args[7] = make_u32_list_val(NULL, 0);

    wasmtime_component_val_t instantiate_results[1];
    memset(instantiate_results, 0, sizeof(instantiate_results));
    if (!call_component_func("instantiate-co-simulation", &instantiate_func, ctx,
                             instantiate_args, 8, instantiate_results, 1)) {
        cleanup_vals(instantiate_args, 8);
        wasmtime_component_linker_delete(linker);
        wasmtime_component_delete(component);
        wasmtime_store_delete(store);
        wasm_engine_delete(engine);
        return 1;
    }
    cleanup_vals(instantiate_args, 8);

    wasmtime_component_resource_any_t *cs_instance = NULL;
    if (instantiate_results[0].kind == WASMTIME_COMPONENT_OPTION &&
        instantiate_results[0].of.option != NULL &&
        instantiate_results[0].of.option->kind == WASMTIME_COMPONENT_RESOURCE) {
        cs_instance =
            wasmtime_component_resource_any_clone(instantiate_results[0].of.option->of.resource);
    }
    cleanup_vals(instantiate_results, 1);

    if (cs_instance == NULL) {
        fprintf(stderr, "instantiate-co-simulation returned no instance\n");
        wasmtime_component_linker_delete(linker);
        wasmtime_component_delete(component);
        wasmtime_store_delete(store);
        wasm_engine_delete(engine);
        return 1;
    }

    wasmtime_component_val_t enter_init_args[4];
    enter_init_args[0] = make_resource_val(cs_instance);
    enter_init_args[1] = make_none_option_val();
    enter_init_args[2] = make_f64_val(0.0);
    enter_init_args[3] = make_none_option_val();

    wasmtime_component_val_t enter_init_results[1];
    memset(enter_init_results, 0, sizeof(enter_init_results));
    if (!call_component_func("enter-initialization-mode", &enter_init_func, ctx,
                             enter_init_args, 4, enter_init_results, 1) ||
        !status_is_ok(&enter_init_results[0])) {
        fprintf(stderr, "enter-initialization-mode failed\n");
        cleanup_vals(enter_init_args, 4);
        cleanup_vals(enter_init_results, 1);
        wasmtime_component_resource_any_drop(ctx, cs_instance);
        wasmtime_component_resource_any_delete(cs_instance);
        wasmtime_component_linker_delete(linker);
        wasmtime_component_delete(component);
        wasmtime_store_delete(store);
        wasm_engine_delete(engine);
        return 1;
    }
    cleanup_vals(enter_init_args, 4);
    cleanup_vals(enter_init_results, 1);

    {
        const uint32_t vrs[] = {metadata.vr_input_a, metadata.vr_input_b};
        const double values[] = {0.0, 2.0};
        wasmtime_component_val_t set_args[3];
        set_args[0] = make_resource_val(cs_instance);
        set_args[1] = make_u32_list_val(vrs, 2);
        set_args[2] = make_f64_list_val(values, 2);

        wasmtime_component_val_t set_results[1];
        memset(set_results, 0, sizeof(set_results));
        if (!call_component_func("set-float64(init)", &set_float64_func, ctx,
                                 set_args, 3, set_results, 1) ||
            !status_is_ok(&set_results[0])) {
            fprintf(stderr, "set-float64 during initialization failed\n");
            cleanup_vals(set_args, 3);
            cleanup_vals(set_results, 1);
            wasmtime_component_resource_any_drop(ctx, cs_instance);
            wasmtime_component_resource_any_delete(cs_instance);
            wasmtime_component_linker_delete(linker);
            wasmtime_component_delete(component);
            wasmtime_store_delete(store);
            wasm_engine_delete(engine);
            return 1;
        }
        cleanup_vals(set_args, 3);
        cleanup_vals(set_results, 1);
    }

    {
        wasmtime_component_val_t exit_args[1];
        exit_args[0] = make_resource_val(cs_instance);

        wasmtime_component_val_t exit_results[1];
        memset(exit_results, 0, sizeof(exit_results));
        if (!call_component_func("exit-initialization-mode", &exit_init_func, ctx,
                                 exit_args, 1, exit_results, 1) ||
            !status_is_ok(&exit_results[0])) {
            fprintf(stderr, "exit-initialization-mode failed\n");
            cleanup_vals(exit_args, 1);
            cleanup_vals(exit_results, 1);
            wasmtime_component_resource_any_drop(ctx, cs_instance);
            wasmtime_component_resource_any_delete(cs_instance);
            wasmtime_component_linker_delete(linker);
            wasmtime_component_delete(component);
            wasmtime_store_delete(store);
            wasm_engine_delete(engine);
            return 1;
        }
        cleanup_vals(exit_args, 1);
        cleanup_vals(exit_results, 1);
    }

    for (size_t i = 0; i < n_steps; ++i) {
        double t = (double)i * step_size;
        double input_a = t;
        double input_b = 2.0 + 2.0 * t;

        {
            const uint32_t vrs[] = {metadata.vr_input_a, metadata.vr_input_b};
            const double values[] = {input_a, input_b};
            wasmtime_component_val_t set_args[3];
            set_args[0] = make_resource_val(cs_instance);
            set_args[1] = make_u32_list_val(vrs, 2);
            set_args[2] = make_f64_list_val(values, 2);

            wasmtime_component_val_t set_results[1];
            memset(set_results, 0, sizeof(set_results));
            bool ok = call_component_func("set-float64(step)", &set_float64_func, ctx,
                                          set_args, 3, set_results, 1) &&
                      status_is_ok(&set_results[0]);
            cleanup_vals(set_args, 3);
            cleanup_vals(set_results, 1);
            if (!ok) {
                fprintf(stderr, "set-float64 failed at step %zu\n", i);
                wasmtime_component_resource_any_drop(ctx, cs_instance);
                wasmtime_component_resource_any_delete(cs_instance);
                wasmtime_component_linker_delete(linker);
                wasmtime_component_delete(component);
                wasmtime_store_delete(store);
                wasm_engine_delete(engine);
                return 1;
            }
        }

        {
            wasmtime_component_val_t step_args[4];
            step_args[0] = make_resource_val(cs_instance);
            step_args[1] = make_f64_val(t);
            step_args[2] = make_f64_val(step_size);
            step_args[3] = make_bool_val(false);

            wasmtime_component_val_t step_results[1];
            memset(step_results, 0, sizeof(step_results));
            bool ok = call_component_func("do-step", &do_step_func, ctx,
                                          step_args, 4, step_results, 1);

            double last_successful_time = 0.0;
            bool terminate_simulation = false;
            bool early_return = false;
            if (ok) {
                ok = parse_do_step_result(&step_results[0],
                                          &last_successful_time,
                                          &terminate_simulation,
                                          &early_return);
            }

            cleanup_vals(step_args, 4);
            cleanup_vals(step_results, 1);

            if (!ok) {
                fprintf(stderr, "do-step parse failed at step %zu\n", i);
                wasmtime_component_resource_any_drop(ctx, cs_instance);
                wasmtime_component_resource_any_delete(cs_instance);
                wasmtime_component_linker_delete(linker);
                wasmtime_component_delete(component);
                wasmtime_store_delete(store);
                wasm_engine_delete(engine);
                return 1;
            }

            double expected_time = t + step_size;
            if (fabs(last_successful_time - expected_time) > tolerance || terminate_simulation || early_return) {
                fprintf(stderr,
                        "do-step validation failed at step %zu: last=%.17g expected=%.17g terminate=%d early=%d\n",
                        i, last_successful_time, expected_time, (int)terminate_simulation,
                        (int)early_return);
                wasmtime_component_resource_any_drop(ctx, cs_instance);
                wasmtime_component_resource_any_delete(cs_instance);
                wasmtime_component_linker_delete(linker);
                wasmtime_component_delete(component);
                wasmtime_store_delete(store);
                wasm_engine_delete(engine);
                return 1;
            }
        }

        {
            const uint32_t vrs[] = {metadata.vr_time, metadata.vr_sum};
            wasmtime_component_val_t get_args[2];
            get_args[0] = make_resource_val(cs_instance);
            get_args[1] = make_u32_list_val(vrs, 2);

            wasmtime_component_val_t get_results[1];
            memset(get_results, 0, sizeof(get_results));

            bool ok = call_component_func("get-float64", &get_float64_func, ctx,
                                          get_args, 2, get_results, 1);
            double got_time = 0.0;
            double got_sum = 0.0;
            if (ok) {
                ok = parse_get_float64_result(&get_results[0], &got_time, &got_sum);
            }

            cleanup_vals(get_args, 2);
            cleanup_vals(get_results, 1);

            if (!ok) {
                fprintf(stderr, "get-float64 parse failed at step %zu\n", i);
                wasmtime_component_resource_any_drop(ctx, cs_instance);
                wasmtime_component_resource_any_delete(cs_instance);
                wasmtime_component_linker_delete(linker);
                wasmtime_component_delete(component);
                wasmtime_store_delete(store);
                wasm_engine_delete(engine);
                return 1;
            }

            double expected_time = t + step_size;
            double expected_sum = input_a + input_b;
            if (fabs(got_time - expected_time) > tolerance || fabs(got_sum - expected_sum) > tolerance) {
                fprintf(stderr,
                        "value check failed at step %zu: time=%.17g expected=%.17g sum=%.17g expected=%.17g\n",
                        i, got_time, expected_time, got_sum, expected_sum);
                wasmtime_component_resource_any_drop(ctx, cs_instance);
                wasmtime_component_resource_any_delete(cs_instance);
                wasmtime_component_linker_delete(linker);
                wasmtime_component_delete(component);
                wasmtime_store_delete(store);
                wasm_engine_delete(engine);
                return 1;
            }
        }
    }

    {
        wasmtime_component_val_t term_args[1];
        term_args[0] = make_resource_val(cs_instance);
        wasmtime_component_val_t term_results[1];
        memset(term_results, 0, sizeof(term_results));

        bool ok = call_component_func("terminate", &terminate_func, ctx,
                                      term_args, 1, term_results, 1) &&
                  status_is_ok(&term_results[0]);
        cleanup_vals(term_args, 1);
        cleanup_vals(term_results, 1);
        if (!ok) {
            fprintf(stderr, "terminate failed\n");
            wasmtime_component_resource_any_drop(ctx, cs_instance);
            wasmtime_component_resource_any_delete(cs_instance);
            wasmtime_component_linker_delete(linker);
            wasmtime_component_delete(component);
            wasmtime_store_delete(store);
            wasm_engine_delete(engine);
            return 1;
        }
    }

    wasmtime_error_t *drop_error = wasmtime_component_resource_any_drop(ctx, cs_instance);
    if (drop_error != NULL) {
        print_wasmtime_error("resource drop failed", drop_error);
        wasmtime_component_resource_any_delete(cs_instance);
        wasmtime_component_linker_delete(linker);
        wasmtime_component_delete(component);
        wasmtime_store_delete(store);
        wasm_engine_delete(engine);
        return 1;
    }
    wasmtime_component_resource_any_delete(cs_instance);

    printf("OK - all %zu steps passed: adder FMU correctly computes sum = input_a + input_b.\n", n_steps);

    wasmtime_component_linker_delete(linker);
    wasmtime_component_delete(component);
    wasmtime_store_delete(store);
    wasm_engine_delete(engine);
    return 0;
}
