#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
#
# SPDX-License-Identifier: GPL-2.0-only

"""Test production error propagation independently of the mmap correction."""
import importlib.util
from pathlib import Path

path = Path(__file__).with_name('test-merge-resources.py')
spec = importlib.util.spec_from_file_location('merge_resources', path)
harness = importlib.util.module_from_spec(spec)
spec.loader.exec_module(harness)

assert harness.source.count('fwrite(') == 1, (
    'route writes through errno capture')
helpers = r'''
static int results[4], result_index;
static int fake_merge_file(const char *name, uint32_t mode)
{ (void)name; (void)mode; return results[result_index++]; }
#define aot_merge_file fake_merge_file
'''
helpers += harness.function('int aot_merge_all')
helpers += '\n#undef aot_merge_file\n'
setup = harness.main[:harness.main.index('int main(void)')]
test_main = setup + r'''
static void sequence(int first, int second, int expected)
{
    results[0] = first; results[1] = second; result_index = 0;
    assert(aot_merge_all("cache", 0) == expected);
    assert(result_index == (first > 0 ? 2 : 1));
}
int main(void)
{
    reset(); fail_write = true;
    assert(aot_merge_file("cache", 0) == -2);
    assert(live_maps == 0 && unmaps == 2);
    assert(access("cache", F_OK) == 0 && access("cache-add1", F_OK) == 0);
    assert(access("cache-tmp", F_OK) != 0);
    for (int i = 0; i < 2; i++) {
        reset(); fail_input = i;
        assert(aot_merge_file("cache", 0) == -2);
        assert(live_maps == 0);
    }
    reset(); busy = 1;
    assert(aot_merge_file("cache", 0) == -1);
    reset(); assert(unlink("cache-add1") == 0);
    assert(aot_merge_file("cache", 0) == 0);

    reset(); fail_write = true;
    AOTMergeContext ctx = { 0 };
    assert(!aot_merge_write(&ctx, "x", 1));
    assert(ctx.write_err && ctx.write_errno == ENOSPC);
    errno = EINVAL; /* later cleanup must not replace the captured error */
    assert(ctx.write_errno == ENOSPC);
    sequence(0, 0, 0);
    sequence(1, 0, 1);
    sequence(-1, 0, -1);
    sequence(-2, 0, -2);
    sequence(1, -1, -1);
    sequence(1, -2, -2);
    return 0;
}
'''
result = harness.run(helpers, test_main)
assert 'write ./cache-tmp failed:' in result.stderr
assert 'errno=28' in result.stderr
assert 'Merge failed: 1 files merged for cache' in result.stderr
assert '[AOT][Merge] Deferred:' in result.stderr
print('PASS: ENOSPC is reported and failures remain distinct '
      'after partial progress')
