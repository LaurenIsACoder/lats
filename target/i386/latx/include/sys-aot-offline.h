/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SYS_AOT_OFFLINE_H
#define SYS_AOT_OFFLINE_H

#include <stdbool.h>

struct CPUState;

bool aot_offline_mode(int argc, char **argv);
int aot_offline_main(int argc, char **argv);
int aot_runtime_info(void);
bool aot_write_runtime_info_file(const char *output_path, bool skip_binary_hash);
bool aot_offline_run_batch_cow(struct CPUState *cpu, const char *jobs_json_path,
                               unsigned workers);
extern bool aot_offline_div_exception;
extern unsigned aot_offline_div_exception_translated;

#endif
